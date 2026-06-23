#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "common.h"

#define JSMN_STATIC
#include "jsmn.h"

#define DEFAULT_SOCKET_PATH "/tmp/bridge.sock"
#define MAX_PACKAGE_NAME_LEN 200
#define MAX_TIMEOUT_SECONDS 3600
#define RAW_OUTPUT_SIZE         (5 * 1024 * 1024)  /* 5 MB */

#define ESCAPED_OUTPUT_SIZE     (RAW_OUTPUT_SIZE * 2)  /* 10 MB */
#define MAX_LINE_SIZE           65536               /* 64 KB */
#define DEFAULT_SWIPE_DURATION_MS 300
#define MAX_LOGCAT_LINES        10000
#define MAX_COORDINATE          10000
#define JSON_ESCAPED_UNICODE_LEN 6   /* strlen("\\u00XX") */


// -------------------------------------------------------------------
// ROBUST JSON EXTRACTOR (JSMN)
// -------------------------------------------------------------------
static int json_eq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, (size_t)(tok->end - tok->start)) == 0) {
        return 0;
    }
    return -1;
}

// Extracts a string value from a JSON object by key name.
// Handles standard JSON escapes (\", \\, \n, \t, etc.).
// Returns 1 on success, 0 if key not found / error, -2 if value was
// truncated (output buffer too small).
// LIMITATION: Does not decode \uXXXX Unicode escape sequences — they are
// passed through literally. This is acceptable for the current use case
// (Android package names and shell commands are ASCII-only). If
// internationalized input is needed in the future, add UTF-8 decoding.
static int extract_json_string(const char* json, const char* key, char* out, size_t max) {
    jsmn_parser p;
    jsmntok_t t[128]; // Enough for MCP JSON-RPC
    jsmn_init(&p);
    int r = jsmn_parse(&p, json, strlen(json), t, sizeof(t) / sizeof(t[0]));
    if (r < 0) return 0;
    if (r < 1 || t[0].type != JSMN_OBJECT) return 0;

    for (int i = 1; i < r; i++) {
        if (json_eq(json, &t[i], key) == 0) {
            if (i + 1 < r) {
                int start = t[i + 1].start;
                int end = t[i + 1].end;
                size_t out_idx = 0;
                int j;
                
                // Parse and unescape JSON string
                // Unescape switch is inverse of json_escape_char() —
                // keep in sync if adding new escape sequences.
                for (j = start; j < end && out_idx < max - 1; j++) {
                    if (json[j] == '\\' && j + 1 < end) {
                        j++;
                        switch (json[j]) {
                            case '"': out[out_idx++] = '"'; break;
                            case '\\': out[out_idx++] = '\\'; break;
                            case '/': out[out_idx++] = '/'; break;
                            case 'b': out[out_idx++] = '\b'; break;
                            case 'f': out[out_idx++] = '\f'; break;
                            case 'n': out[out_idx++] = '\n'; break;
                            case 'r': out[out_idx++] = '\r'; break;
                            case 't': out[out_idx++] = '\t'; break;
                            default: out[out_idx++] = json[j]; break;
                        }
                    } else {
                        out[out_idx++] = json[j];
                    }
                }
                out[out_idx] = '\0';
                if (j < end) return -2;  // signal truncation
                return 1;
            }
        }
    }
    return 0;
}

// -------------------------------------------------------------------
// JSON ESCAPING (FULL SUPPORT)
// Handle control characters to make JSON valid
// -------------------------------------------------------------------

// Single source of truth for JSON escape character mappings.
// Returns the escape character (e.g., 'n' for '\n'), 'u' for \uXXXX,
// or 0 if no escaping is needed.
static char json_escape_char(unsigned char c) {
    switch (c) {
        case '"':  return '"';
        case '\\': return '\\';
        case '\b': return 'b';
        case '\f': return 'f';
        case '\n': return 'n';
        case '\r': return 'r';
        case '\t': return 't';
        default:   return (c < 32 || c == 0x7F) ? 'u' : 0;
    }
}

static size_t escape_json_string(const char* src, char* dest, size_t max_dest) {
    size_t i = 0;
    while (*src) {
        unsigned char c = *src;
        char esc = json_escape_char(c);
        // Determine how many bytes this character needs when escaped
        size_t needed;
        if (esc == 0) {
            needed = 1;
        } else if (esc == 'u') {
            needed = JSON_ESCAPED_UNICODE_LEN;  /* 6 */
        } else {
            needed = 2;
        }

        if (i + needed < max_dest) {
            // Buffer space available — write the escaped character
            if (esc == 'u') {
                int n = snprintf(dest + i, max_dest - i, "\\u%04x", c);
                if (n > 0) i += (size_t)n;
            } else if (esc) {
                dest[i++] = '\\';
                dest[i++] = esc;
            } else {
                dest[i++] = c;
            }
        } else {
            // Buffer full — continue counting to report total needed
            i += needed;
        }
        src++;
    }
    // Null-terminate within bounds
    if (i < max_dest) {
        dest[i] = '\0';
    } else {
        dest[max_dest - 1] = '\0';
    }
    return i;
}

// -------------------------------------------------------------------
// BRIDGE COMMUNICATION LAYER (SPLIT FOR SRP)
// -------------------------------------------------------------------

// Appends a payload chunk to the output buffer and checks response type.
// Returns 1 if the caller should break the read loop, 0 to continue.
// Sets *is_error if the response is an error type.
static int append_response_chunk(const PacketHeader* hdr, const char* payload,
                                 char* output_buf, size_t* current_len,
                                 size_t max_out, int* is_error) {
    if (hdr->type == RESP_STREAM_CHUNK ||
        hdr->type == RESP_STREAM_ERR ||
        hdr->type == RESP_SUCCESS ||
        hdr->type == RESP_ERROR) {
        size_t needed = hdr->len;
        if (*current_len < max_out - 1) {
            size_t space = (max_out - 1) - *current_len;
            size_t to_copy = (needed <= space) ? needed : space;
            memcpy(output_buf + *current_len, payload, to_copy);
        }
        *current_len += needed;
        // Safe null termination: *current_len may exceed max_out when tracking truncation
        size_t end = (*current_len >= max_out - 1) ? (max_out - 1) : *current_len;
        output_buf[end] = '\0';

        if (hdr->type == RESP_ERROR) {
            *is_error = 1;
            return 1;
        }
        if (hdr->type == RESP_SUCCESS) return 1;
        return 0;
    } else if (hdr->type == RESP_STREAM_END) {
        return 1;
    }
    return 0;
}

// Socket creation and connection. Returns socket fd on success, or -1
// on failure (with error in output_buf).
static int bridge_connect(const char* socket_path, char* output_buf, size_t max_out) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        snprintf(output_buf, max_out, "Error: Failed to create socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    int n = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
    if (n < 0 || (size_t)n >= sizeof(addr.sun_path)) {
        snprintf(output_buf, max_out, "Error: Socket path too long.");
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        snprintf(output_buf, max_out,
            "Error: Could not connect to bridge at %s. Is the server running?",
            socket_path);
        close(sockfd);
        return -1;
    }
    return sockfd;
}

// Sends a bridge protocol request (header + payload). Returns 0 on success.
static int bridge_send_request(int sockfd, CommandType type,
                               const char* payload_data, size_t payload_len) {
    if (payload_len > UINT32_MAX) return -1;
    PacketHeader hdr = { .type = type, .len = (uint32_t)payload_len };
    if (write_all(sockfd, &hdr, sizeof(hdr)) < 0) return -1;
    if (payload_len > 0) {
        if (write_all(sockfd, payload_data, payload_len) < 0) return -1;
    }
    return 0;
}

// Reads the full bridge response (stream-aware loop). Returns 0 for success,
// 1 for remote error, -1 for connection/read error.
static int bridge_read_response(int sockfd, char* output_buf, size_t max_out) {
    size_t current_len = 0;
    output_buf[0] = '\0';
    int is_error = 0;

    while (1) {
        PacketHeader res_hdr;
        int read_result = read_all(sockfd, &res_hdr, sizeof(res_hdr));
        if (read_result < 0) {
            snprintf(output_buf, max_out, "Error: Connection lost while reading response.");
            return -1;
        }

        // Payload size protection
        if (res_hdr.len > MAX_PAYLOAD_SIZE) {
            snprintf(output_buf, max_out, "Error: Response payload too large.");
            return -1;
        }

        char res_payload[MAX_PAYLOAD_SIZE + 1];
        if (res_hdr.len > 0) {
            if (read_all(sockfd, res_payload, res_hdr.len) < 0) {
                snprintf(output_buf, max_out, "Error: Failed to read response payload.");
                return -1;
            }
            res_payload[res_hdr.len] = '\0';
        } else {
            res_payload[0] = '\0';
        }

        if (append_response_chunk(&res_hdr, res_payload, output_buf,
                                  &current_len, max_out, &is_error)) {
            break;
        }
    }
    if (current_len > max_out - 1) {
        // Buffer was truncated (current_len tracks total that would have been written).
        // Preserve accumulated partial data with a truncation marker in place
        // of the last bytes of content.
        const char* trunc_msg = " [TRUNCATED]";
        size_t trunc_len = strlen(trunc_msg);
        size_t start = (max_out - 1 > trunc_len) ? (max_out - 1 - trunc_len) : 0;
        memcpy(output_buf + start, trunc_msg, trunc_len);
        output_buf[max_out - 1] = '\0';
        return -1;
    }
    return is_error;
}

// Orchestrates a complete bridge command lifecycle: connect → send → read → close.
static int run_bridge_command(CommandType type, const char* payload_data,
                              size_t payload_len, char* output_buf, size_t max_out) {
    const char* socket_path = getenv("BRIDGE_SOCKET");
    if (!socket_path) socket_path = DEFAULT_SOCKET_PATH;

    int sockfd = bridge_connect(socket_path, output_buf, max_out);
    if (sockfd < 0) return -1;

    if (bridge_send_request(sockfd, type, payload_data, payload_len) < 0) {
        close(sockfd);
        snprintf(output_buf, max_out, "Error: Failed to send request to bridge.");
        return -1;
    }

    int result = bridge_read_response(sockfd, output_buf, max_out);
    close(sockfd);
    return result;
}

// Executes a shell command on the Android device via the bridge daemon.
// The payload length includes the null terminator (strlen + 1) so the
// receiver can treat the payload as a C string.
// Implemented as static inline (not macro) to avoid double evaluation
// of arguments (CERT PRE31-C).
static inline int exec_stream_cmd(const char *cmd_buf, char *out_buf, size_t out_size) {
    return run_bridge_command(CMD_STREAM, cmd_buf,
                              strlen(cmd_buf) + 1, out_buf, out_size);
}

// -------------------------------------------------------------------
// SECURITY BLACKLIST (MCP ONLY)
// -------------------------------------------------------------------
const char* BLACKLISTED_COMMANDS[] = {
    "rm", "mv", "cp", "sh", "su", "chmod", "chown", "kill", "reboot", "recovery", "bootloader", "dd"
};

static int is_command_blocked(const char* cmd) {
    if (!cmd) return 1;
    for (size_t i = 0; i < sizeof(BLACKLISTED_COMMANDS) / sizeof(char*); i++) {
        if (strcmp(cmd, BLACKLISTED_COMMANDS[i]) == 0) return 1;
        const char* last_slash = strrchr(cmd, '/');
        if (last_slash && strcmp(last_slash + 1, BLACKLISTED_COMMANDS[i]) == 0) return 1;
    }
    return 0;
}

// -------------------------------------------------------------------
// VALIDATION HELPERS
// -------------------------------------------------------------------
/** Check if input contains any character from the given set.
 *  Generic helper parameterized by character set — eliminates loop duplication
 *  while preserving separate security contexts via static inline wrappers. */
static int contains_any_char(const char* input, const char* chars) {
    for (const char* c = input; *c != '\0'; c++) {
        if (strchr(chars, *c)) return 1;
    }
    return 0;
}

// Block shell metacharacters for DOUBLE-QUOTED contexts (input_text, file_read).
// Per POSIX.1-2017 §2.2.3, single quotes (') are literal inside double quotes.
static inline int contains_shell_metacharacters(const char* i) {
    return contains_any_char(i, "\"$`\\\n\r!");
}

// Block shell metacharacters for UNQUOTED contexts (device_exec).
// Based on lightNVR's dangerous character set (opensensor/lightNVR).
static inline int contains_shell_dangerous(const char* i) {
    return contains_any_char(i, ";|&`$><\n\r\\\"'!{}()[]~*?");
}

// Check if input contains only alphanumeric characters
static int is_alphanumeric_only(const char* input) {
    for (size_t i = 0; input[i]; i++) {
        if (!isalnum((unsigned char)input[i])) {
            return 0;
        }
    }
    return 1;
}

// Validates Android package name format (e.g., "com.example.app").
// Rules: alphanumeric + dots/underscores/hyphens, no consecutive dots,
// must contain at least one dot, cannot start/end with dot or hyphen.
// Returns 0 on success, -1 on failure (with error message in error_buf).
static int validate_package_name(const char* package, char* error_buf, size_t error_buf_size) {
    if (strlen(package) == 0) {
        snprintf(error_buf, error_buf_size, "Error: package name is required.");
        return -1;
    }
    if (strlen(package) > MAX_PACKAGE_NAME_LEN) {
        snprintf(error_buf, error_buf_size, "Error: package name too long.");
        return -1;
    }
    for (size_t i = 0; package[i]; i++) {
        char c = (unsigned char)package[i];
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') {
            snprintf(error_buf, error_buf_size, "Error: invalid chars. Use [a-zA-Z0-9._-].");
            return -1;
        }
    }
    // Must not start/end with dot or hyphen
    size_t len = strlen(package);
    if (package[0] == '.' || package[0] == '-' ||
        package[len - 1] == '.' || package[len - 1] == '-') {
        snprintf(error_buf, error_buf_size, "Error: no leading/trailing '.' or '-'.");
        return -1;
    }
    // No consecutive dots
    if (strstr(package, "..") != NULL) {
        snprintf(error_buf, error_buf_size, "Error: package name must not contain '..'.");
        return -1;
    }
    if (strchr(package, '.') == NULL) {
        snprintf(error_buf, error_buf_size, "Error: need at least one '.' (e.g. 'com.example')");
        return -1;
    }
    return 0;
}

// -------------------------------------------------------------------
// STRING UTILITY HELPERS
// -------------------------------------------------------------------
/** Trim trailing whitespace (spaces, tabs, newlines, carriage returns)
 *  in-place. Returns the new length of the string, allowing callers to
 *  avoid a second strlen() call on large buffers. */
static size_t strtrim_trailing(char* s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = '\0';
    return len;
}

// -------------------------------------------------------------------
// JSON-RPC METHOD HANDLERS
// -------------------------------------------------------------------

// Outputs a JSON-RPC id field value with correct JSON type.
// If id is "null" → print bare null (JSON-RPC notification).
// Always quote non-null IDs — this is valid JSON-RPC 2.0 for all inputs
// (spec permits string IDs even if the original was a number).
// The isdigit() heuristic was removed because it produced invalid JSON
// for string IDs starting with a digit (e.g., "123abc").
static void print_json_id(const char* id) {
    if (strcmp(id, "null") == 0) {
        printf("null");
    } else {
        char escaped_id[256];
        escape_json_string(id, escaped_id, sizeof(escaped_id));
        printf("\"%s\"", escaped_id);
    }
}

// Prints the shared JSON-RPC 2.0 response envelope prefix.
// All response types start with {"jsonrpc":"2.0","id":...}.
static void print_mcp_response_header(const char* id) {
    printf("{\"jsonrpc\":\"2.0\",\"id\":");
    print_json_id(id);
}

static void handle_initialize(const char* id) {
    print_mcp_response_header(id);
    printf(",\"result\":{\"protocolVersion\":\"2024-11-05\","
        "\"capabilities\":{\"tools\":{}},"
        "\"serverInfo\":{\"name\":\"native-bridge-mcp\","
        "\"version\":\"1.1.0\"}}}\n");
}

// Handles the "tools/list" JSON-RPC method. Returns the full catalog of
// supported tools with their names, descriptions, and input schemas.
// Uses a data-driven tool table (Redis command table pattern) so adding
// a new tool requires only one table entry — no JSON string editing.

typedef struct {
    const char* name;
    const char* description;
    const char* input_schema;  /* JSON object string */
    const char* content_type;  /* "text" or "image" (NULL means "text") */
    const char* mime_type;     /* e.g. "image/png" (NULL for text) */
    int (*handler)(const char*, char*, size_t);
} ToolDef;

// Forward declarations of all tool handlers (used in TOOLS[] below)
static int handle_device_exec(const char*, char*, size_t);
static int handle_device_tap(const char*, char*, size_t);
static int handle_device_swipe(const char*, char*, size_t);
static int handle_device_ping(const char*, char*, size_t);
static int handle_device_screenshot(const char*, char*, size_t);
static int handle_device_input_text(const char*, char*, size_t);
static int handle_device_logcat(const char*, char*, size_t);
static int handle_device_file_read(const char*, char*, size_t);
static int handle_device_app_open(const char*, char*, size_t);
static int handle_device_app_close(const char*, char*, size_t);
static int handle_device_app_list(const char*, char*, size_t);

static const ToolDef TOOLS[] = {
    {"device_exec",
     "Execute shell command on Android Host. "
     "IMPORTANT: POSIX shell metacharacters (;|&`$(){}[]~*? etc.) "
     "are BLOCKED for security in MCP mode. Use single-word commands "
     "with arguments only. Always use single quotes ('') for "
     "regex/complex arguments instead of double quotes to avoid JSON "
     "escape corruption (e.g. grep -E 'A|B').",
     "{\"type\":\"object\","
     "\"properties\":{\"cmd\":{\"type\":\"string\"},"
     "\"timeout\":{\"type\":\"integer\","
     "\"description\":\"Timeout in seconds (optional)\"}},"
     "\"required\":[\"cmd\"]}",
     "text", NULL,
     handle_device_exec},
    {"device_tap",
     "Tap screen at (x, y).",
     "{\"type\":\"object\","
     "\"properties\":{\"x\":{\"type\":\"integer\"},"
     "\"y\":{\"type\":\"integer\"}},"
     "\"required\":[\"x\",\"y\"]}",
     "text", NULL,
     handle_device_tap},
    {"device_swipe",
     "Swipe screen (x1,y1) to (x2,y2).",
     "{\"type\":\"object\","
     "\"properties\":{\"x1\":{\"type\":\"integer\"},"
     "\"y1\":{\"type\":\"integer\"},"
     "\"x2\":{\"type\":\"integer\"},"
     "\"y2\":{\"type\":\"integer\"}},"
     "\"required\":[\"x1\",\"y1\",\"x2\",\"y2\"]}",
     "text", NULL,
     handle_device_swipe},
    {"device_ping",
     "Check daemon connection.",
     "{\"type\":\"object\",\"properties\":{}}",
     "text", NULL,
     handle_device_ping},
    {"device_screenshot",
     "Take a screenshot. Returns an image the AI can view.",
     "{\"type\":\"object\",\"properties\":{}}",
     "image", "image/png",
     handle_device_screenshot},
    {"device_input_text",
     "Inject text input.",
     "{\"type\":\"object\","
     "\"properties\":{\"text\":{\"type\":\"string\"}},"
     "\"required\":[\"text\"]}",
     "text", NULL,
     handle_device_input_text},
    {"device_logcat",
     "Fetch recent logcat.",
     "{\"type\":\"object\","
     "\"properties\":{\"lines\":{\"type\":\"integer\"},"
     "\"filter\":{\"type\":\"string\"}},"
     "\"required\":[]}",
     "text", NULL,
     handle_device_logcat},
    {"device_file_read",
     "Read file directly.",
     "{\"type\":\"object\","
     "\"properties\":{\"path\":{\"type\":\"string\"}},"
     "\"required\":[\"path\"]}",
     "text", NULL,
     handle_device_file_read},
    {"device_app_open",
     "Open an Android app by package name.",
     "{\"type\":\"object\","
     "\"properties\":{\"package\":{\"type\":\"string\"}},"
     "\"required\":[\"package\"]}",
     "text", NULL,
     handle_device_app_open},
    {"device_app_close",
     "Force-stop an Android app by package name.",
     "{\"type\":\"object\","
     "\"properties\":{\"package\":{\"type\":\"string\"}},"
     "\"required\":[\"package\"]}",
     "text", NULL,
     handle_device_app_close},
    {"device_app_list",
     "List installed Android apps. Optionally filter by keyword.",
     "{\"type\":\"object\","
     "\"properties\":{\"filter\":{\"type\":\"string\","
     "\"description\":\"Optional keyword to filter packages "
     "(e.g. 'chrome')\"}}}",
     "text", NULL,
     handle_device_app_list},
};

static void handle_tools_list(const char* id) {
    size_t tool_count = sizeof(TOOLS) / sizeof(TOOLS[0]);
    print_mcp_response_header(id);
    printf(",\"result\":{\"tools\":[");
    for (size_t i = 0; i < tool_count; i++) {
        if (i > 0) printf(",");
        printf("{\"name\":\"%s\",\"description\":\"%s\",\"inputSchema\":%s}",
               TOOLS[i].name, TOOLS[i].description, TOOLS[i].input_schema);
    }
    printf("]}}\n");
}

// -------------------------------------------------------------------
// RESPONSE FORMATTING HELPER
// -------------------------------------------------------------------
// Formats and prints the JSON-RPC response for a tool call.
// The content type and MIME type are read from the ToolDef struct,
// making this fully data-driven — no special-casing by tool name.
// Future non-text tools (audio, binary, etc.) only need the correct
// content_type/mime_type in their TOOLS[] entry.
static void send_mcp_response(const char* id, const ToolDef* tool,
                              int exec_res, const char* raw_output,
                              char* escaped_output, size_t escaped_size) {
    // Escape once before branching (DRY — identical across all paths)
    size_t escaped_len = escape_json_string(raw_output, escaped_output, escaped_size);
    if (escaped_len >= escaped_size) {
        // Truncation occurred — return error instead of corrupt data
        print_mcp_response_header(id);
        printf(",\"result\":{\"isError\":true,\"content\":[{\"type\":\"text\","
            "\"text\":\"Error: Output too large (%zu bytes after JSON escaping).\"}]}}\n",
            escaped_len);
        return;
    }

    if (strcmp(tool->content_type, "image") == 0 && exec_res == 0) {
        // Return as image content type for vision-capable AI models
        print_mcp_response_header(id);
        printf(",\"result\":{\"content\":[{\"type\":\"%s\","
            "\"mimeType\":\"%s\",\"data\":\"%s\"}]}}\n",
            tool->content_type, tool->mime_type, escaped_output);
    } else if (exec_res != 0) {
        // MCP allows return text in isError true
        print_mcp_response_header(id);
        printf(",\"result\":{\"isError\":true,\"content\":[{\"type\":\"text\","
            "\"text\":\"%s\"}]}}\n", escaped_output);
    } else {
        print_mcp_response_header(id);
        printf(",\"result\":{\"content\":[{\"type\":\"text\","
            "\"text\":\"%s\"}]}}\n", escaped_output);
    }
}

// -------------------------------------------------------------------
// TOOL HANDLER IMPLEMENTATIONS
// -------------------------------------------------------------------

static int handle_device_ping(const char* json_line, char* output, size_t out_size) {
    (void)json_line; // Unused
    return run_bridge_command(CMD_PING, "", 0, output, out_size);
}

// Wraps a command with the `timeout` utility if a valid timeout is specified.
// Uses strtol for safe parsing and caps the timeout to MAX_TIMEOUT_SECONDS
// to prevent holding the bridge connection open for excessive durations.
// Returns 0 on success, -1 if the final command was truncated.
static int wrap_with_timeout(const char* cmd, const char* timeout_str,
                              char* final_cmd, size_t size) {
    if (strlen(timeout_str) > 0) {
        char* endptr;
        long t = strtol(timeout_str, &endptr, 10);
        // Accept only fully-parsed, positive values within the cap
        if (*endptr == '\0' && t > 0 && t <= MAX_TIMEOUT_SECONDS) {
            int n = snprintf(final_cmd, size, "timeout %ld %s", t, cmd);
            if (n < 0 || (size_t)n >= size) {
                // Truncation: fall back to command without timeout wrapper
                // (better to run without timeout than a truncated command)
                fprintf(stderr, "Warning: timeout wrapper truncated, running command without timeout\n");
                n = snprintf(final_cmd, size, "%s", cmd);
                if (n < 0 || (size_t)n >= size) return -1;
            }
            return 0;
        }
        // Invalid or out-of-range timeout: ignore and run without timeout
    }
    int n = snprintf(final_cmd, size, "%s", cmd);
    if (n < 0 || (size_t)n >= size) return -1;
    return 0;
}

// Validates a shell command for safe execution in MCP mode.
// Layer 1: Scans for POSIX shell metacharacters (injection prevention).
// Layer 2: Blacklists dangerous commands (defense-in-depth).
// Returns 0 on success, -1 on failure (with error message in output).
static int validate_cmd_safe(const char* cmd_str, char* output, size_t out_size) {
    // Layer 1: Comprehensive metacharacter scan on ENTIRE command string.
    if (contains_shell_dangerous(cmd_str)) {
        snprintf(output, out_size,
            "Security Error: Command contains disallowed shell metacharacters. "
            "Shell operators (;|&$`>< etc.) are not permitted in MCP mode.");
        return -1;
    }

    // Layer 2: Blacklist check on first token (defense-in-depth).
    char first_token[256] = {0};
    const char* p = cmd_str;
    while (*p && isspace((unsigned char)*p)) p++;
    int i = 0;
    while (*p && !isspace((unsigned char)*p) && i < 255) first_token[i++] = *p++;
    first_token[i] = '\0';

    if (is_command_blocked(first_token)) {
        snprintf(output, out_size,
            "Security Error: Command '%s' is blacklisted in MCP mode.",
            first_token);
        return -1;
    }
    return 0;
}

static int handle_device_exec(const char* json_line, char* output, size_t out_size) {
    char cmd_str[2048] = {0};
    char timeout_str[16] = {0};
    char final_cmd[2500] = {0};
    
    // 'cmd' is a required parameter
    int ret = extract_json_string(json_line, "cmd", cmd_str, sizeof(cmd_str));
    if (ret == 0) {
        snprintf(output, out_size, "Error: 'cmd' parameter is required.");
        return -1;
    }
    if (ret == -2) {
        snprintf(output, out_size, "Error: 'cmd' too long (max %zu characters).", sizeof(cmd_str) - 1);
        return -1;
    }
    if (cmd_str[0] == '\0') {
        snprintf(output, out_size, "Error: 'cmd' must not be empty.");
        return -1;
    }

    extract_json_string(json_line, "timeout", timeout_str, sizeof(timeout_str));

    // Validate original command before timeout wrapping (CERT STR02-C)
    if (validate_cmd_safe(cmd_str, output, out_size) != 0) return -1;

    if (wrap_with_timeout(cmd_str, timeout_str, final_cmd, sizeof(final_cmd)) != 0) {
        snprintf(output, out_size, "Error: Command too long after timeout wrapping.");
        return -1;
    }

    // Send wrapped command (with timeout if specified)
    return exec_stream_cmd(final_cmd, output, out_size);
}

// Parses a JSON coordinate parameter (key name like "x", "y1", etc.).
// Returns 0 on success, -1 on failure with error message in output.
static int parse_coordinate(const char* json_line, const char* key,
                            int* out, char* output, size_t out_size) {
    char buf[16];
    if (!extract_json_string(json_line, key, buf, sizeof(buf))) {
        snprintf(output, out_size, "Error: '%s' parameter is required.", key);
        return -1;
    }
    char* endptr;
    long val = strtol(buf, &endptr, 10);
    if (*endptr != '\0' || endptr == buf || val < 0 || val > MAX_COORDINATE) {
        snprintf(output, out_size, "Error: invalid value for '%s' (must be 0-%d).",
                 key, MAX_COORDINATE);
        return -1;
    }
    *out = (int)val;
    return 0;
}

static int handle_device_tap(const char* json_line, char* output, size_t out_size) {
    int x = 0, y = 0;
    if (parse_coordinate(json_line, "x", &x, output, out_size) != 0) return -1;
    if (parse_coordinate(json_line, "y", &y, output, out_size) != 0) return -1;

    PayloadTap tap = {x, y};
    int res = run_bridge_command(CMD_TAP, (char*)&tap, sizeof(tap), output, out_size);
    if (res == 0) snprintf(output, out_size, "Tap OK");
    return res;
}

static int handle_device_swipe(const char* json_line, char* output, size_t out_size) {
    PayloadSwipe s = {0};
    s.duration_ms = DEFAULT_SWIPE_DURATION_MS;

    // Use local variables to avoid taking address of packed struct members
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (parse_coordinate(json_line, "x1", &x1, output, out_size) != 0) return -1;
    if (parse_coordinate(json_line, "y1", &y1, output, out_size) != 0) return -1;
    if (parse_coordinate(json_line, "x2", &x2, output, out_size) != 0) return -1;
    if (parse_coordinate(json_line, "y2", &y2, output, out_size) != 0) return -1;
    s.x1 = x1; s.y1 = y1; s.x2 = x2; s.y2 = y2;

    int res = run_bridge_command(CMD_SWIPE, (char*)&s, sizeof(s), output, out_size);
    if (res == 0) snprintf(output, out_size, "Swipe OK");
    return res;
}

static int handle_device_screenshot(const char* json_line, char* output, size_t out_size) {
    (void)json_line; // Unused
    
    // Note: Pipe is safe here because the command is hardcoded, not user-supplied.
    // CMD_STREAM passes this to the bridge daemon's shell; the MCP server's
    // metacharacter validator does not apply to hardcoded commands.
    char* cmd = "screencap -p | base64 -w 0";
    int res = exec_stream_cmd(cmd, output, out_size);
    if (res != 0) {
        return res;
    }

    // Strip trailing whitespace (GNU base64 -w 0 appends \n).
    // Using strtrim_trailing to avoid a second strlen() on the ~1 MB buffer.
    size_t ss_len = strtrim_trailing(output);

    if (ss_len >= out_size - 1) {
        snprintf(output, out_size, "Error: Screenshot too large (%zu bytes).", ss_len);
        return -1;
    }

    return 0;
}

static int handle_device_input_text(const char* json_line, char* output, size_t out_size) {
    char text[1024] = {0};
    if (!extract_json_string(json_line, "text", text, sizeof(text))) {
        snprintf(output, out_size, "Error: 'text' parameter is required.");
        return -1;
    }
    if (contains_shell_metacharacters(text)) {
        snprintf(output, out_size, "Error: text contains invalid character.");
        return -1;
    }
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "input text \"%s\"", text);
    return exec_stream_cmd(cmd, output, out_size);
}

// Validates that a filter string (if non-empty) is alphanumeric-only.
// Returns 0 on success, -1 on failure (with error message in output).
static int validate_filter(const char* filter, char* output, size_t out_size) {
    if (strlen(filter) > 0 && !is_alphanumeric_only(filter)) {
        snprintf(output, out_size, "Error: invalid filter. Only alphanumeric characters allowed.");
        return -1;
    }
    return 0;
}

static int handle_device_logcat(const char* json_line, char* output, size_t out_size) {
    char lines_str[16] = {0}, filter[128] = {0}, cmd[512] = {0};
    extract_json_string(json_line, "lines", lines_str, sizeof(lines_str));
    extract_json_string(json_line, "filter", filter, sizeof(filter));

    // Safe parsing with strtol — detect non-numeric input and overflow
    char* endptr;
    long parsed = strtol(lines_str, &endptr, 10);
    // Clamp to valid range: 1..MAX_LOGCAT_LINES, default 100
    int lines;
    if (*endptr != '\0' || endptr == lines_str || parsed <= 0) {
        lines = 100;  // default for empty/invalid
    } else if (parsed > MAX_LOGCAT_LINES) {
        lines = MAX_LOGCAT_LINES;  // cap to prevent resource exhaustion
    } else {
        lines = (int)parsed;
    }
    
    if (validate_filter(filter, output, out_size) != 0) return -1;
    if (strlen(filter) > 0) {
        snprintf(cmd, sizeof(cmd), "logcat -d -t %d | grep -iF \"%s\"", lines, filter);
    } else {
        snprintf(cmd, sizeof(cmd), "logcat -d -t %d", lines);
    }
    return exec_stream_cmd(cmd, output, out_size);
}

// Checks if a path starts with an allowed prefix (/sdcard, /storage, /data/local/tmp).
// Uses a table-driven approach consistent with BLACKLISTED_COMMANDS and TOOLS patterns.
static int is_allowed_path_prefix(const char* path) {
    static const char* allowed_prefixes[] = {
        "/sdcard",
        "/storage",
        "/data/local/tmp",
    };
    for (size_t i = 0; i < sizeof(allowed_prefixes) / sizeof(allowed_prefixes[0]); i++) {
        size_t plen = strlen(allowed_prefixes[i]);
        if (strncmp(path, allowed_prefixes[i], plen) == 0 &&
            (path[plen] == '/' || path[plen] == '\0')) {
            return 1;  // allowed
        }
    }
    return 0;  // not allowed
}

static int handle_device_file_read(const char* json_line, char* output, size_t out_size) {
    char path[512] = {0};
    if (!extract_json_string(json_line, "path", path, sizeof(path))) {
        snprintf(output, out_size, "Error: 'path' parameter is required.");
        return -1;
    }
    if (contains_shell_metacharacters(path)) {
        snprintf(output, out_size, "Error: path contains invalid character.");
        return -1;
    }
    if (strstr(path, "..") != NULL) {
        snprintf(output, out_size, "Error: path traversal not allowed.");
        return -1;
    }
    if (!is_allowed_path_prefix(path)) {
        snprintf(output, out_size,
            "Error: reading only allowed from /sdcard/, /storage/, /data/local/tmp/");
        return -1;
    }
    // NOTE: Path prefix check does not resolve symlinks. A symlink
    // within an allowed prefix (e.g., /sdcard/evil_link -> /data/data/...)
    // could bypass the whitelist. This is low risk on Android since
    // creating symlinks in /sdcard/ requires root, but consider adding
    // realpath() resolution if the bridge daemon supports it.
    char cmd[1024];
    // Execute cat with safe format and capture stderr for error reporting
    snprintf(cmd, sizeof(cmd), "cat \"%s\" 2>&1", path);
    return exec_stream_cmd(cmd, output, out_size);
}

// Shared helper: extracts "package" from JSON and validates as Android package name.
// Returns 0 on success, -1 on failure (with error message in output).
static int extract_and_validate_package(const char* json_line,
                                         char* package, size_t pkg_size,
                                         char* output, size_t out_size) {
    if (!extract_json_string(json_line, "package", package, pkg_size)) {
        snprintf(output, out_size, "Error: 'package' parameter is required.");
        return -1;
    }
    return validate_package_name(package, output, out_size);
}

// Parses the last non-empty line of resolve-activity output.
// Expects output to already be trimmed of trailing whitespace.
// Returns 0 on success with activity populated, -1 on error (message in error_buf).
static int parse_activity_output(const char* output, char* activity,
                                 size_t activity_size, char* error_buf, size_t error_size) {
    // Find the last line after the final newline
    const char* p = strrchr(output, '\n');
    p = p ? p + 1 : output;
    // Skip leading whitespace on the activity line
    while (*p == ' ' || *p == '\t') p++;

    if (strlen(p) == 0 || strchr(p, '/') == NULL) {
        snprintf(error_buf, error_size, "Error: Could not resolve activity.");
        return -1;
    }
    // Defense-in-depth: validate before embedding in shell command
    if (contains_shell_metacharacters(p)) {
        snprintf(error_buf, error_size, "Error: Resolved activity contains invalid characters.");
        return -1;
    }

    int n = snprintf(activity, activity_size, "%s", p);
    if (n < 0 || (size_t)n >= activity_size) {
        snprintf(error_buf, error_size, "Error: Resolved activity name too long (%zu chars).", strlen(p));
        return -1;
    }
    return 0;
}

static int handle_device_app_open(const char* json_line, char* output, size_t out_size) {
    char package[MAX_PACKAGE_NAME_LEN + 1] = {0};
    if (extract_and_validate_package(json_line, package, sizeof(package),
                                      output, out_size) != 0) return -1;

    // Step 1: Resolve the main activity for the package.
    // Two-step approach eliminates $() subshell composition, removing the
    // latent injection vector that existed when resolve was embedded inline.
    char resolve_cmd[512];
    snprintf(resolve_cmd, sizeof(resolve_cmd),
             "cmd package resolve-activity --brief \"%s\" 2>&1", package);
    int res = exec_stream_cmd(resolve_cmd, output, out_size);
    if (res != 0) return res;

    // Step 2: Parse resolved activity from output
    strtrim_trailing(output);
    char activity_buf[256] = {0};
    if (parse_activity_output(output, activity_buf, sizeof(activity_buf),
                               output, out_size) != 0) return -1;

    // Step 3: Launch with the resolved activity (no shell composition)
    char launch_cmd[512];
    snprintf(launch_cmd, sizeof(launch_cmd),
             "am start -W -n \"%s\" 2>&1", activity_buf);
    res = exec_stream_cmd(launch_cmd, output, out_size);
    if (res == 0 && strlen(output) == 0) {
        snprintf(output, out_size, "App '%s' launched successfully.", package);
    }
    return res;
}

static int handle_device_app_close(const char* json_line, char* output, size_t out_size) {
    char package[MAX_PACKAGE_NAME_LEN + 1] = {0};
    if (extract_and_validate_package(json_line, package, sizeof(package),
                                      output, out_size) != 0) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "am force-stop \"%s\" 2>&1", package);
    int res = exec_stream_cmd(cmd, output, out_size);
    if (res == 0 && strlen(output) == 0) {
        snprintf(output, out_size, "App '%s' stopped.", package);
    }
    return res;
}

static int handle_device_app_list(const char* json_line, char* output, size_t out_size) {
    char app_filter[256] = {0};
    // filter is optional — tolerate missing key (zero-initialized is fine)
    int ret = extract_json_string(json_line, "filter", app_filter, sizeof(app_filter));
    if (ret == -2) {
        fprintf(stderr, "Warning: app_list filter truncated (max %zu chars)\n", sizeof(app_filter) - 1);
    }
    char cmd[512];
    if (validate_filter(app_filter, output, out_size) != 0) return -1;
    if (strlen(app_filter) > 0) {
        snprintf(cmd, sizeof(cmd), "pm list packages 2>&1 | grep -iF \"%s\"", app_filter);
    } else {
        snprintf(cmd, sizeof(cmd), "pm list packages 2>&1");
    }
    return exec_stream_cmd(cmd, output, out_size);
}

// -------------------------------------------------------------------
// TOOL DISPATCH HELPER (DATA-DRIVEN)
// -------------------------------------------------------------------
// Dispatches a tool call by name using the handler function pointer stored
// in the TOOLS[] entry. Single source of truth — no separate dispatch table.
// Returns pointer to the matched ToolDef on success, NULL on failure.
// The handler result is written to *out_result.
static const ToolDef* dispatch_tool_call(const char* json_line, const char* tool_name,
                                         char* raw_output, size_t raw_output_size,
                                         int* out_result) {
    size_t tool_count = sizeof(TOOLS) / sizeof(TOOLS[0]);
    for (size_t i = 0; i < tool_count; i++) {
        if (strcmp(tool_name, TOOLS[i].name) == 0) {
            *out_result = TOOLS[i].handler(json_line, raw_output, raw_output_size);
            return &TOOLS[i];
        }
    }
    snprintf(raw_output, raw_output_size, "Unknown tool");
    *out_result = -1;
    return NULL;
}

// -------------------------------------------------------------------
// PER-REQUEST DISPATCH (EXTRACTED FROM MAIN LOOP)
// -------------------------------------------------------------------
// Handles a single JSON-RPC request line. Extracts the method and id,
// dispatches to the appropriate handler, and sends the response.
static void handle_mcp_request(const char* line, char* escaped_output, size_t escaped_size,
                                char* raw_output, size_t raw_output_size) {
    char method[64] = {0};
    char id[64] = {0};

    if (!extract_json_string(line, "method", method, sizeof(method))) return;
    int is_notification = !extract_json_string(line, "id", id, sizeof(id));
    if (is_notification) {
        snprintf(id, sizeof(id), "null");
    }

    if (strcmp(method, "initialize") == 0) {
        if (!is_notification) handle_initialize(id);
    } else if (strcmp(method, "notifications/initialized") == 0) {
        // No response needed (correct per JSON-RPC 2.0 — this is always a notification)
    } else if (strcmp(method, "tools/list") == 0) {
        if (!is_notification) handle_tools_list(id);
    } else if (strcmp(method, "tools/call") == 0) {
        if (is_notification) return;  // Notifications MUST NOT produce a response
        char tool_name[64] = {0};
        extract_json_string(line, "name", tool_name, sizeof(tool_name));
        // raw_output is heap-allocated in main() — reentrant-safe
        int exec_res;
        const ToolDef* tool = dispatch_tool_call(line, tool_name,
                                                raw_output, raw_output_size,
                                                &exec_res);
        if (tool) {
            send_mcp_response(id, tool, exec_res, raw_output,
                             escaped_output, escaped_size);
        } else {
            // Tool not found — raw_output already contains "Unknown tool"
            // Use TOOLS[0] as a text-only fallback for the response
            send_mcp_response(id, &TOOLS[0], -1, raw_output,
                             escaped_output, escaped_size);
        }
    }
}

// -------------------------------------------------------------------
// MAIN MCP EVENT LOOP
// -------------------------------------------------------------------
int main() {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    fprintf(stderr, "[MCP] Native-Bridge MCP Server Ready (Safe Mode)\n");

    char* line = malloc(MAX_LINE_SIZE);
    if (!line) return 1;
    char* escaped_output = malloc(ESCAPED_OUTPUT_SIZE);
    if (!escaped_output) { free(line); return 1; }
    char* raw_output = malloc(RAW_OUTPUT_SIZE);
    if (!raw_output) { free(escaped_output); free(line); return 1; }

    while (fgets(line, MAX_LINE_SIZE, stdin))
        handle_mcp_request(line, escaped_output, ESCAPED_OUTPUT_SIZE,
                           raw_output, RAW_OUTPUT_SIZE);

    free(raw_output);
    free(escaped_output);
    free(line);
    return 0;
}
