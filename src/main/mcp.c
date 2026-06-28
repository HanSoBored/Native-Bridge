#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "common.h"
#include "screenshot.h"

#define JSMN_STATIC
#include "jsmn.h"

#define DEFAULT_SOCKET_PATH "/tmp/bridge.sock"
#define MAX_PACKAGE_NAME_LEN 200
#define MAX_TIMEOUT_SECONDS 3600
#define RAW_OUTPUT_SIZE         (12 * 1024 * 1024)  /* 12 MB */

#define ESCAPED_OUTPUT_SIZE     (RAW_OUTPUT_SIZE * 2)  /* 24 MB */
#define MAX_LINE_SIZE           65536               /* 64 KB */
#define DEFAULT_SWIPE_DURATION_MS 300
#define MAX_LOGCAT_LINES        10000
#define MAX_COORDINATE          10000
#define JSON_ESCAPED_UNICODE_LEN 6   /* strlen("\\u00XX") */
#define SCREENSHOT_TARGET_WIDTH 720  /* downscale width for token savings */


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
            // Buffer full — continue counting to report total needed.
            // Guard against SIZE_MAX overflow (theoretical on 32-bit with
            // extreme inputs; in practice ESCAPED_OUTPUT_SIZE caps at ~24MB).
            if (i > SIZE_MAX / 2) { i = SIZE_MAX; break; }
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

/**
 * Parse a positive integer parameter from a string, with clamping and default fallback.
 * Returns the parsed value clamped to [min, max], or default_val on parse failure.
 */
static int parse_positive_int_param(const char* str, int default_val, int min_val, int max_val) {
    if (!str || str[0] == '\0') return default_val;
    char* endptr;
    errno = 0;
    long parsed = strtol(str, &endptr, 10);
    if (*endptr != '\0' || endptr == str || parsed <= 0) return default_val;
    if ((parsed == LONG_MAX || parsed == LONG_MIN) && errno == ERANGE) return default_val;
    if (parsed > max_val) return max_val;
    if (parsed < min_val) return min_val;
    return (int)parsed;
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

static void handle_initialize(const char* id, const char* line,
                               char* escaped_output, size_t escaped_size,
                               char* raw_output, size_t raw_output_size,
                               int is_notification) {
    (void)line; (void)escaped_output; (void)escaped_size;
    (void)raw_output; (void)raw_output_size; (void)is_notification;
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
    int (*handler)(const char*, char*, size_t, void* ctx);
} ToolDef;

// Forward declarations of all tool handlers (used in TOOLS[] below)
static int handle_device_exec(const char*, char*, size_t, void*);
static int handle_device_tap(const char*, char*, size_t, void*);
static int handle_device_swipe(const char*, char*, size_t, void*);
static int handle_device_ping(const char*, char*, size_t, void*);
static int handle_device_screenshot(const char*, char*, size_t, void*);
static int handle_device_input_text(const char*, char*, size_t, void*);
static int handle_device_logcat(const char*, char*, size_t, void*);
static int handle_device_file_read(const char*, char*, size_t, void*);
static int handle_device_app_open(const char*, char*, size_t, void*);
static int handle_device_app_close(const char*, char*, size_t, void*);
static int handle_device_app_list(const char*, char*, size_t, void*);
static int handle_device_uiautomator(const char*, char*, size_t, void*);

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
    {"device_uiautomator",
     "Dump Android UI hierarchy (via uiautomator) and filter elements. "
     "Returns matching XML node lines. Much cheaper than screenshot. "
     "If 'path' is provided: dump to that path and return raw unfiltered XML. "
     "If 'path' is omitted: dump to /tmp, filter with grep + limit.",
     "{\"type\":\"object\","
     "\"properties\":{"
       "\"query\":{\"type\":\"string\","
         "\"description\":\"Search term. Empty/'*' shows all. Ignored if path is set.\"},"
       "\"limit\":{\"type\":\"integer\","
         "\"description\":\"Max lines (default 30, max 500). Ignored if path is set.\"},"
       "\"path\":{\"type\":\"string\","
         "\"description\":\"Custom dump path. When set, returns raw unfiltered XML.\"}"
     "},"
     "\"required\":[]}",
     "text", NULL,
     handle_device_uiautomator},
 };

static void handle_tools_list(const char* id, const char* line,
                               char* escaped_output, size_t escaped_size,
                               char* raw_output, size_t raw_output_size,
                               int is_notification) {
    (void)line; (void)escaped_output; (void)escaped_size;
    (void)raw_output; (void)raw_output_size; (void)is_notification;
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
// METHOD DISPATCH TABLE TYPES
// -------------------------------------------------------------------
typedef void (*method_handler)(const char* id, const char* line,
                                char* escaped_output, size_t escaped_size,
                                char* raw_output, size_t raw_output_size,
                                int is_notification);

typedef struct {
    const char* method;
    int allow_notification;
    method_handler fn;
} MethodDef;

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

static int handle_device_ping(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)json_line; (void)ctx;
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

static int handle_device_exec(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
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

    int ret_timeout = extract_json_string(json_line, "timeout", timeout_str, sizeof(timeout_str));
    if (ret_timeout == -2) {
        snprintf(output, out_size, "Error: 'timeout' value too long.");
        return -1;
    }

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

static int handle_device_tap(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
    int x = 0, y = 0;
    if (parse_coordinate(json_line, "x", &x, output, out_size) != 0) return -1;
    if (parse_coordinate(json_line, "y", &y, output, out_size) != 0) return -1;

    PayloadTap tap = {x, y};
    int res = run_bridge_command(CMD_TAP, (char*)&tap, sizeof(tap), output, out_size);
    if (res == 0) snprintf(output, out_size, "Tap OK");
    return res;
}

static int handle_device_swipe(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
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

/* Screenshot dimensions type — returned from handle_device_screenshot
 * and consumed by send_screenshot_response for coordinate metadata. */
typedef struct { int ow, oh, dw, dh; } ShotDims;

static int handle_device_screenshot(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)json_line;
    ShotDims* shot = (ShotDims*)ctx;
    /* Zero dimension struct to prevent stale-value usage on early return */
    memset(shot, 0, sizeof(*shot));

    // Step 1: Capture full-res screenshot as PNG base64
    // NOTE: "screencap -p" is Android-specific; not available on
    // non-Android Linux or other platforms. Also requires the
    // "base64" utility (part of BusyBox/coreutils on Android).
    char* cmd = "screencap -p | base64 -w 0";
    int res = exec_stream_cmd(cmd, output, out_size);
    if (res != 0) return res;

    strtrim_trailing(output);

    /* Step 2: Downscale to save tokens (default 720px width).
     * Peak memory budget: ~12 MB (base64) + ~12 MB (orig copy) + ~50 MB (LodePNG RGBA)
     * = ~74 MB. strdup is required because downscale_screenshot_base64 reads from
     * the input pointer and writes to the output pointer, which cannot overlap. */
    char* orig = strdup(output);
    if (!orig) {
        snprintf(output, out_size, "Error: Out of memory processing screenshot.");
        return -1;
    }

    unsigned orig_w = 0, orig_h = 0;
    if (downscale_screenshot_base64(orig, output, out_size,
                                     SCREENSHOT_TARGET_WIDTH, &orig_w, &orig_h) != 0) {
        // Fall back to full-res — warn so operators can detect when downscale fails
        fprintf(stderr, "[MCP] Warning: screenshot downscale failed, "
                        "falling back to full-resolution (%zu bytes of base64)\n",
                        strlen(orig));
        size_t orig_len = strlen(orig);
        if (orig_len >= out_size) {
            snprintf(output, out_size, "Error: Screenshot too large (%zu bytes).", orig_len);
            free(orig);
            return -1;
        }
        memcpy(output, orig, orig_len + 1);
        free(orig);
        return 0;
    }
    free(orig);

    // Store dimensions for metadata in response
    shot->ow = (int)orig_w;
    shot->oh = (int)orig_h;
    shot->dw = (orig_w > 0 && orig_w < SCREENSHOT_TARGET_WIDTH)
                   ? (int)orig_w
                   : SCREENSHOT_TARGET_WIDTH;
    shot->dh = (orig_w > 0 && orig_h > 0)
                   ? (int)((unsigned long long)shot->dw * orig_h / orig_w)
                   : 0;

    return 0;
}

static int handle_device_input_text(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
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

static int handle_device_logcat(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
    char lines_str[16] = {0}, filter[128] = {0}, cmd[512] = {0};
    extract_json_string(json_line, "lines", lines_str, sizeof(lines_str));
    extract_json_string(json_line, "filter", filter, sizeof(filter));

    int lines = parse_positive_int_param(lines_str, 100, 1, MAX_LOGCAT_LINES);
    
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

static int handle_device_file_read(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
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

static int handle_device_app_open(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
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

static int handle_device_app_close(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
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

static int handle_device_app_list(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
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
// DEVICE_UI_AUTOMATOR HANDLER
// -------------------------------------------------------------------

// Shared safe-string validator for uiautomator inputs.
// Checks for path traversal (..) and disallowed characters.
static int is_safe_string(const char* s, const char* extra_allowed,
                          const char* err_name, char* output, size_t out_size) {
    // Check for path traversal before character validation
    if (strstr(s, "..") != NULL) {
        snprintf(output, out_size,
            "Error: %s contains '..' (path traversal not allowed).", err_name);
        return -1;
    }
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c)) continue;
        if (extra_allowed && strchr(extra_allowed, c)) continue;
        snprintf(output, out_size,
            "Error: %s contains disallowed character '%c'.", err_name, (char)c);
        return -1;
    }
    return 0;
}

// Validate query: only alphanumeric + safe punctuation (shell-safe inside single quotes)
static int is_uia_safe_query(const char* q, char* output, size_t out_size) {
    return is_safe_string(q, " _-./:@=#+()*,", "query", output, out_size);
}

// Validate path: only alphanumeric + / _ - .
static int is_uia_safe_path(const char* p, char* output, size_t out_size) {
    if (strlen(p) == 0) {
        snprintf(output, out_size, "Error: path must not be empty.");
        return -1;
    }
    return is_safe_string(p, "/_.-", "path", output, out_size);
}

static int handle_uiautomator_raw_path(const char* path, char* output, size_t out_size) {
    if (is_uia_safe_path(path, output, out_size) != 0) return -1;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "uiautomator dump '%s' 2>/dev/null && cat '%s' 2>/dev/null",
        path, path);
    fprintf(stderr, "[UIA] raw dump -> %s\n", path);
    return exec_stream_cmd(cmd, output, out_size);
}

static int handle_uiautomator_filtered(const char* query, int limit,
                                        char* output, size_t out_size) {
    if (is_uia_safe_query(query, output, out_size) != 0) return -1;

    char temp_path[] = "/tmp/nb_uia_XXXXXX.xml";
    int temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) {
        snprintf(output, out_size, "Error: failed to create temp file.");
        return -1;
    }
    close(temp_fd);
    char cmd[2048];
    int n;
    if (strlen(query) == 0 || strcmp(query, "*") == 0) {
        n = snprintf(cmd, sizeof(cmd),
            "uiautomator dump \"%s\" 2>&1 && "
            "head -%d \"%s\"; "
            "rm -f \"%s\"", temp_path, limit, temp_path, temp_path);
    } else {
        n = snprintf(cmd, sizeof(cmd),
            "uiautomator dump \"%s\" 2>&1 && "
            "grep -iF '%s' \"%s\" | head -%d; "
            "rm -f \"%s\"", temp_path, query, temp_path, limit, temp_path);
    }
    if (n < 0 || (size_t)n >= sizeof(cmd)) {
        unlink(temp_path);
        snprintf(output, out_size, "Error: full command too long.");
        return -1;
    }

    int ret = exec_stream_cmd(cmd, output, out_size);
    unlink(temp_path);  /* clean up regardless of exec result (rm in cmd is best-effort) */
    return ret;
}

static int handle_device_uiautomator(const char* json_line, char* output, size_t out_size, void* ctx) {
    (void)ctx;
    char path[512] = {0};
    char query[256] = {0};
    char limit_str[16] = {0};

    int has_path = extract_json_string(json_line, "path", path, sizeof(path));
    if (has_path && has_path != -2) {
        return handle_uiautomator_raw_path(path, output, out_size);
    }

    if (has_path == -2) {
        snprintf(output, out_size,
                 "Error: 'path' too long (max %zu characters).",
                 sizeof(path) - 1);
        return -1;
    }

    // Extract query (required unless path is set)
    int ret = extract_json_string(json_line, "query", query, sizeof(query));
    if (ret == -2) {
        snprintf(output, out_size,
                 "Error: 'query' too long (max %zu characters).",
                 sizeof(query) - 1);
        return -1;
    }
    if (ret == 0) {
        snprintf(output, out_size, "Error: 'query' parameter is required when path is not set.");
        return -1;
    }

    // Extract limit
    int has_limit = extract_json_string(json_line, "limit", limit_str, sizeof(limit_str));
    if (has_limit == -2) {
        snprintf(output, out_size,
                 "Error: 'limit' too long (max %zu characters).",
                 sizeof(limit_str) - 1);
        return -1;
    }
    int limit = parse_positive_int_param(limit_str, 30, 1, 500);

    return handle_uiautomator_filtered(query, limit, output, out_size);
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
                                         int* out_result, void* ctx) {
    size_t tool_count = sizeof(TOOLS) / sizeof(TOOLS[0]);
    for (size_t i = 0; i < tool_count; i++) {
        if (strcmp(tool_name, TOOLS[i].name) == 0) {
            *out_result = TOOLS[i].handler(json_line, raw_output, raw_output_size, ctx);
            return &TOOLS[i];
        }
    }
    snprintf(raw_output, raw_output_size, "Unknown tool");
    *out_result = -1;
    return NULL;
}

// -------------------------------------------------------------------
// FORWARD DECLARATIONS
// -------------------------------------------------------------------
static void send_screenshot_response(const char* id, int orig_w, int orig_h,
                                     const char* raw_b64, char* escaped_buf, size_t escaped_size);

// -------------------------------------------------------------------
// METHOD HANDLER EXTRACTIONS (SRP — one function per JSON-RPC method)
// -------------------------------------------------------------------

static void handle_ping(const char* id, const char* line,
                         char* escaped_output, size_t escaped_size,
                         char* raw_output, size_t raw_output_size,
                         int is_notification) {
    (void)line; (void)escaped_output; (void)escaped_size;
    (void)raw_output; (void)raw_output_size;
    if (!is_notification) {
        print_mcp_response_header(id);
        printf(",\"result\":{}}\n");
    }
}

static void handle_notifications_initialized(const char* id, const char* line,
                                              char* escaped_output, size_t escaped_size,
                                              char* raw_output, size_t raw_output_size,
                                              int is_notification) {
    (void)id; (void)line; (void)escaped_output; (void)escaped_size;
    (void)raw_output; (void)raw_output_size; (void)is_notification;
    /* No response needed per JSON-RPC 2.0 — always a notification */
}

static void handle_tools_call(const char* id, const char* line,
                               char* escaped_output, size_t escaped_size,
                               char* raw_output, size_t raw_output_size,
                               int is_notification) {
    (void)is_notification;
    /* Notifications MUST NOT produce a response — handled by caller */
    char tool_name[64] = {0};
    extract_json_string(line, "name", tool_name, sizeof(tool_name));
    ShotDims shot_dims = {0};
    int exec_res;
    const ToolDef* tool = dispatch_tool_call(line, tool_name,
                                            raw_output, raw_output_size,
                                            &exec_res, &shot_dims);
    if (tool && strcmp(tool_name, "device_screenshot") == 0 && exec_res == 0) {
        send_screenshot_response(id, shot_dims.dw, shot_dims.dh,
                                 raw_output, escaped_output, escaped_size);
    } else if (tool) {
        send_mcp_response(id, tool, exec_res, raw_output,
                         escaped_output, escaped_size);
    } else {
        send_mcp_response(id, &TOOLS[0], -1, raw_output,
                         escaped_output, escaped_size);
    }
}

static void handle_unknown_method(const char* id, const char* line,
                                   char* escaped_output, size_t escaped_size,
                                   char* raw_output, size_t raw_output_size,
                                   int is_notification) {
    (void)line; (void)escaped_output; (void)escaped_size;
    (void)raw_output; (void)raw_output_size;
    if (!is_notification) {
        print_mcp_response_header(id);
        printf(",\"result\":{\"isError\":true,\"content\":[{\"type\":\"text\","
               "\"text\":\"Unknown method\"}]}}\n");
    }
}

// -------------------------------------------------------------------
// METHOD DISPATCH TABLE
// -------------------------------------------------------------------
static const MethodDef METHODS[] = {
    {"initialize",              0, handle_initialize},
    {"notifications/initialized", 1, handle_notifications_initialized},
    {"ping",                    0, handle_ping},
    {"tools/list",              0, handle_tools_list},
    {"tools/call",              0, handle_tools_call},
};

// -------------------------------------------------------------------
// PER-REQUEST DISPATCH (ORCHESTRATION ONLY)
// -------------------------------------------------------------------
// Handles a single JSON-RPC request line. Extracts method and id,
// looks up in METHODS[] dispatch table, calls the appropriate handler.
// Unknown methods fall through to handle_unknown_method.
static void handle_mcp_request(const char* line, char* escaped_output, size_t escaped_size,
                                char* raw_output, size_t raw_output_size) {
    char method[64] = {0};
    char id[64] = {0};

    if (!extract_json_string(line, "method", method, sizeof(method))) return;
    int is_notification = !extract_json_string(line, "id", id, sizeof(id));
    if (is_notification) {
        snprintf(id, sizeof(id), "null");
    }

    size_t num_methods = sizeof(METHODS) / sizeof(METHODS[0]);
    for (size_t i = 0; i < num_methods; i++) {
        if (strcmp(method, METHODS[i].method) == 0) {
            if (is_notification && !METHODS[i].allow_notification) return;
            METHODS[i].fn(id, line, escaped_output, escaped_size,
                          raw_output, raw_output_size, is_notification);
            return;
        }
    }

    /* Unknown method */
    handle_unknown_method(id, line, escaped_output, escaped_size,
                          raw_output, raw_output_size, is_notification);
}

/* ------------------------------------------------------------------- */
/* DEVICE RESOLUTION                                                   */
/* ------------------------------------------------------------------- */
static int dev_width = 0;
static int dev_height = 0;
/* @single-threaded — queried once on first screenshot, read-only thereafter
 * in the single-threaded MCP event loop. Safe without synchronization. */

/* Resolution query is lazy: called on first screenshot request */
static void query_device_resolution(void) {
    char buf[128] = {0};
    /* NOTE: "wm size" is Android-specific — requires Android's WindowManager
     * shell command. Not available on non-Android Linux or other platforms. */
    int res = exec_stream_cmd("wm size", buf, sizeof(buf));
    if (res == 0) {
        /* Output: "Physical size: 1080x2340\n" (or "Override size: ...") */
        const char* p = strchr(buf, ':');
        if (p) {
            p++;
            while (*p == ' ') p++;
            if (sscanf(p, "%dx%d", &dev_width, &dev_height) == 2) {
                fprintf(stderr, "[MCP] Device resolution: %dx%d\n", dev_width, dev_height);
                return;
            }
        }
    }
    fprintf(stderr, "[MCP] Could not detect device resolution (wm size failed)\n");
}

/* Builds the metadata text describing screenshot dimensions and scale.
 * Handles the case where device resolution or image dimensions are unknown. */
static void build_response_meta(int dev_w, int dev_h, int orig_w, int orig_h,
                                float scale_x, float scale_y,
                                char* meta, size_t meta_size) {
    if (orig_w <= 0 || orig_h <= 0) {
        snprintf(meta, meta_size, "Screenshot (unknown size)");
    } else if (dev_w > 0) {
        snprintf(meta, meta_size,
            "Screen: %dx%d | Image: %dx%d | Scale: %.2fx %.2fy. "
            "Multiply image coords by scale to get device coords.",
            dev_w, dev_h, orig_w, orig_h,
            (double)scale_x, (double)scale_y);
    } else {
        snprintf(meta, meta_size,
            "Image: %dx%d | Device resolution unknown. "
            "Use image coordinates directly.",
            orig_w, orig_h);
    }
}

/* Formats a JSON-RPC id as a JSON value: "null" for notifications,
 * or a quoted escaped string otherwise. */
static void format_json_id(const char* id, char* out, size_t out_size) {
    if (strcmp(id, "null") == 0) {
        snprintf(out, out_size, "null");
    } else {
        char esc_id[256];
        escape_json_string(id, esc_id, sizeof(esc_id));
        snprintf(out, out_size, "\"%s\"", esc_id);
    }
}

/* Builds a JSON-RPC response with text metadata + image content for screenshot.
 * Uses three sequential printf calls (prefix, base64 data, suffix) to avoid
 * the fragile in-place memmove approach that previously dual-purposed the
 * escaped buffer. */
static void send_screenshot_response(const char* id, int orig_w, int orig_h,
                                     const char* raw_b64, char* escaped_buf,
                                     size_t escaped_size) {
    // Lazily query device resolution on first screenshot request
    if (dev_width == 0) query_device_resolution();

    // P1 guard: prevent division by zero in scaling computation
    if (orig_w <= 0 || orig_h <= 0) { orig_w = 0; orig_h = 0; }
    float scale_x = 1.0f, scale_y = 1.0f;
    if (orig_w > 0 && orig_h > 0) {
        scale_x = (dev_width > 0) ? (float)dev_width / (float)orig_w : 1.0f;
        scale_y = (dev_height > 0) ? (float)dev_height / (float)orig_h : 1.0f;
    }

    // Escape the base64 image data into the buffer
    size_t b64_escaped = escape_json_string(raw_b64, escaped_buf, escaped_size);
    if (b64_escaped >= escaped_size) {
        char id_fmt[512];
        format_json_id(id, id_fmt, sizeof(id_fmt));
        printf("{\"jsonrpc\":\"2.0\",\"id\":%s,"
               "\"result\":{\"isError\":true,\"content\":[{\"type\":\"text\","
               "\"text\":\"Error: Screenshot output too large.\"}]}}\n",
               id_fmt);
        return;
    }

    // Build and escape metadata text
    char meta[256];
    build_response_meta(dev_width, dev_height, orig_w, orig_h,
                        scale_x, scale_y, meta, sizeof(meta));
    char escaped_meta[512];
    size_t meta_escaped = escape_json_string(meta, escaped_meta, sizeof(escaped_meta));
    if (meta_escaped >= sizeof(escaped_meta)) {
        memcpy(escaped_meta, "error", 6);
    }

    // Format JSON-RPC id as a JSON value
    char id_fmt[512];
    format_json_id(id, id_fmt, sizeof(id_fmt));

    // Print JSON-RPC prefix with metadata (no in-place buffer manipulation)
    printf("{\"jsonrpc\":\"2.0\",\"id\":%s,"
           "\"result\":{\"content\":["
           "{\"type\":\"text\",\"text\":\"%s\"},"
           "{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"",
           id_fmt, escaped_meta);

    // Print escaped base64 image data
    printf("%s", escaped_buf);

    // Print JSON suffix
    printf("\"}]}}\n");
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
