#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "common.h"

#define JSMN_STATIC
#include "jsmn.h"

#define DEFAULT_SOCKET_PATH "/tmp/bridge.sock"

// -------------------------------------------------------------------
// ROBUST JSON EXTRACTOR (JSMN)
// -------------------------------------------------------------------
static int json_eq(const char *json, jsmntok_t *tok, const char *s) {
    if (tok->type == JSMN_STRING && (int)strlen(s) == tok->end - tok->start &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0) {
        return 0;
    }
    return -1;
}

int extract_json_string(const char* json, const char* key, char* out, size_t max) {
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
                
                // Parse and unescape JSON string
                for (int j = start; j < end && out_idx < max - 1; j++) {
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
void escape_json_string(const char* src, char* dest, size_t max_dest) {
    size_t i = 0;
    while (*src && i < max_dest - 2) {
        unsigned char c = *src;
        if (c == '"') { dest[i++] = '\\'; dest[i++] = '"'; }
        else if (c == '\\') { dest[i++] = '\\'; dest[i++] = '\\'; }
        else if (c == '\b') { dest[i++] = '\\'; dest[i++] = 'b'; }
        else if (c == '\f') { dest[i++] = '\\'; dest[i++] = 'f'; }
        else if (c == '\n') { dest[i++] = '\\'; dest[i++] = 'n'; }
        else if (c == '\r') { dest[i++] = '\\'; dest[i++] = 'r'; }
        else if (c == '\t') { dest[i++] = '\\'; dest[i++] = 't'; }
        else if (c < 32) { 
            // Ignore other control chars
        } 
        else { dest[i++] = c; }
        src++;
    }
    dest[i] = '\0';
}

// -------------------------------------------------------------------
// COMMUNICATION FUNCTION TO NATIVE-BRIDGE SERVER
// Use memcpy instead of strcat for buffer safety
// -------------------------------------------------------------------
int run_bridge_command(CommandType type, const char* payload_data, int payload_len, char* output_buf, size_t max_out) {
    const char* socket_path = getenv("BRIDGE_SOCKET");
    if (!socket_path) socket_path = DEFAULT_SOCKET_PATH;

    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) {
        snprintf(output_buf, max_out, "Error: Failed to create socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        snprintf(output_buf, max_out, "Error: Could not connect to bridge at %s. Is the server running?", socket_path);
        close(sockfd);
        return -1;
    }

    // Send Request
    PacketHeader hdr = { .type = type, .len = (uint32_t)payload_len };
    if (write_all(sockfd, &hdr, sizeof(hdr)) < 0) {
        close(sockfd); 
        return -1;
    }
    if (payload_len > 0) {
        write_all(sockfd, payload_data, payload_len);
    }

    // Read Response Loop
    size_t current_len = 0;
    output_buf[0] = '\0';
    int is_error = 0;

    while (1) {
        PacketHeader res_hdr;
        int read_result = read_all(sockfd, &res_hdr, sizeof(res_hdr));
        if (read_result < 0) {
            snprintf(output_buf, max_out, "Error: Connection lost while reading response.");
            is_error = -1;
            break;
        }

        // Payload size protection
        if (res_hdr.len > MAX_PAYLOAD_SIZE) {
            snprintf(output_buf, max_out, "Error: Response payload too large.");
            is_error = -1;
            break;
        }

        char res_payload[MAX_PAYLOAD_SIZE + 1];
        if (res_hdr.len > 0) {
            if (read_all(sockfd, res_payload, res_hdr.len) < 0) break;
            res_payload[res_hdr.len] = '\0';
        } else {
            res_payload[0] = '\0';
        }

        if (res_hdr.type == RESP_STREAM_CHUNK || res_hdr.type == RESP_STREAM_ERR || res_hdr.type == RESP_SUCCESS || res_hdr.type == RESP_ERROR) {
            // Append safely using memcpy
            size_t needed = res_hdr.len;
            if (current_len + needed < max_out - 1) {
                memcpy(output_buf + current_len, res_payload, needed);
                current_len += needed;
            } else if (current_len < max_out - 1) {
                // Buffer full, append remaining space then stop
                size_t space = (max_out - 1) - current_len;
                memcpy(output_buf + current_len, res_payload, space);
                current_len += space;
            }
            output_buf[current_len] = '\0';

            if (res_hdr.type == RESP_ERROR) {
                is_error = 1; // Logic error from the remote (eg command failed)
                break;
            }
            if (res_hdr.type == RESP_SUCCESS) break;
        } 
        else if (res_hdr.type == RESP_STREAM_END) {
            break;
        }
    }
    close(sockfd);
    return is_error;
}

// -------------------------------------------------------------------
// SECURITY BLACKLIST (MCP ONLY)
// -------------------------------------------------------------------
const char* BLACKLISTED_COMMANDS[] = {
    "rm", "mv", "cp", "sh", "su", "chmod", "chown", "kill", "reboot", "recovery", "bootloader", "dd"
};

int is_command_blocked(const char* cmd) {
    if (!cmd) return 1;
    for (size_t i = 0; i < sizeof(BLACKLISTED_COMMANDS) / sizeof(char*); i++) {
        if (strcmp(cmd, BLACKLISTED_COMMANDS[i]) == 0) return 1;
        char* last_slash = strrchr(cmd, '/');
        if (last_slash && strcmp(last_slash + 1, BLACKLISTED_COMMANDS[i]) == 0) return 1;
    }
    return 0;
}

// -------------------------------------------------------------------
// QUOTE-AWARE TOKENIZER
// Handles "quoted arguments" correctly for shell-like behavior
// -------------------------------------------------------------------
int tokenize_with_quotes(char* input, char* payload, int max_payload) {
    int p_len = 0;
    char* p = input;
    
    while (*p) {
        // Skip whitespace
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        
        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p++;
        }
        
        while (*p) {
            if (quote) {
                if (*p == quote) { p++; break; }
            } else {
                if (isspace((unsigned char)*p)) break;
            }
            
            if (p_len < max_payload - 1) {
                payload[p_len++] = *p++;
            } else {
                p++; // Just skip if payload full
            }
        }
        payload[p_len++] = '\0'; // Null terminate this token
    }
    return p_len;
}

// -------------------------------------------------------------------
// MAIN MCP EVENT LOOP
// -------------------------------------------------------------------
int main() {
    setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering stdout
    
    // Log to stderr to avoid interfering with JSON-RPC on stdout
    fprintf(stderr, "[MCP] Native-Bridge MCP Server Ready (Safe Mode)\n");

    char line[65536]; // Large input buffer to accommodate long JSON
    char* escaped_output = malloc(131072);
    if (!escaped_output) return 1;
    
    while (fgets(line, sizeof(line), stdin)) {
        char method[64] = {0};
        char id[64] = {0};

        // Extract ID and basic Method
        if (!extract_json_string(line, "method", method, sizeof(method))) continue;
        
        // Get ID (Default null if notification)
        if (!extract_json_string(line, "id", id, sizeof(id))) {
            strcpy(id, "null"); 
        }

        // --- HANDLERS ---

        if (strcmp(method, "initialize") == 0) {
            printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"native-bridge-mcp\",\"version\":\"1.1.0\"}}}\n", id);
        }
        else if (strcmp(method, "notifications/initialized") == 0) {
            // No response needed
        }
        else if (strcmp(method, "tools/list") == 0) {
            printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":["
                "{\"name\":\"device_exec\",\"description\":\"Execute shell command on Android Host. Supports quoted arguments.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"cmd\":{\"type\":\"string\"}},\"required\":[\"cmd\"]}},"
                "{\"name\":\"device_tap\",\"description\":\"Tap screen at (x, y).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"}},\"required\":[\"x\",\"y\"]}},"
                "{\"name\":\"device_swipe\",\"description\":\"Swipe screen (x1,y1) to (x2,y2).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x1\":{\"type\":\"integer\"},\"y1\":{\"type\":\"integer\"},\"x2\":{\"type\":\"integer\"},\"y2\":{\"type\":\"integer\"}},\"required\":[\"x1\",\"y1\",\"x2\",\"y2\"]}},"
                "{\"name\":\"device_ping\",\"description\":\"Check daemon connection.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                "{\"name\":\"device_screenshot\",\"description\":\"Take screenshot (returns base64).\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
                "{\"name\":\"device_input_text\",\"description\":\"Inject text input.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}},"
                "{\"name\":\"device_logcat\",\"description\":\"Fetch recent logcat.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"lines\":{\"type\":\"integer\"},\"filter\":{\"type\":\"string\"}},\"required\":[\"lines\"]}},"
                "{\"name\":\"device_file_read\",\"description\":\"Read file directly.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}}"
            "]}}\n", id);
        }
        else if (strcmp(method, "tools/call") == 0) {
            char tool_name[64] = {0};
            extract_json_string(line, "name", tool_name, sizeof(tool_name));

            char raw_output[65536] = {0}; 
            int exec_res = 0;

            // --- EXECUTE TOOL ---
            if (strcmp(tool_name, "device_ping") == 0) {
                exec_res = run_bridge_command(CMD_PING, "", 0, raw_output, sizeof(raw_output));
            } 
            else if (strcmp(tool_name, "device_exec") == 0) {
                char cmd_str[2048] = {0};
                extract_json_string(line, "cmd", cmd_str, sizeof(cmd_str));
                
                // Extract first token for security check
                char first_token[256] = {0};
                char* p = cmd_str;
                while (*p && isspace((unsigned char)*p)) p++;
                int i = 0;
                while (*p && !isspace((unsigned char)*p) && i < 255) first_token[i++] = *p++;
                first_token[i] = '\0';

                // Blacklist check on the first argument (command)
                if (is_command_blocked(first_token)) {
                    exec_res = -1;
                    snprintf(raw_output, sizeof(raw_output), "Security Error: Command '%s' is blacklisted in MCP mode.", first_token);
                } else {
                    // Send raw command as a single null-terminated token to trigger 'sh -c' on server
                    // Use CMD_STREAM for better handling of large outputs
                    exec_res = run_bridge_command(CMD_STREAM, cmd_str, strlen(cmd_str) + 1, raw_output, sizeof(raw_output));
                }
            }
            else if (strcmp(tool_name, "device_tap") == 0) {
                char buf[16];
                int x = 0, y = 0;
                if (extract_json_string(line, "x", buf, sizeof(buf))) x = atoi(buf);
                if (extract_json_string(line, "y", buf, sizeof(buf))) y = atoi(buf);

                PayloadTap tap = {x, y};
                exec_res = run_bridge_command(CMD_TAP, (char*)&tap, sizeof(tap), raw_output, sizeof(raw_output));
                if (exec_res == 0) strcpy(raw_output, "Tap OK");
            }
            else if (strcmp(tool_name, "device_swipe") == 0) {
                char buf[16];
                PayloadSwipe s = {0};
                s.duration_ms = 300;
                if (extract_json_string(line, "x1", buf, sizeof(buf))) s.x1 = atoi(buf);
                if (extract_json_string(line, "y1", buf, sizeof(buf))) s.y1 = atoi(buf);
                if (extract_json_string(line, "x2", buf, sizeof(buf))) s.x2 = atoi(buf);
                if (extract_json_string(line, "y2", buf, sizeof(buf))) s.y2 = atoi(buf);

                exec_res = run_bridge_command(CMD_SWIPE, (char*)&s, sizeof(s), raw_output, sizeof(raw_output));
                if (exec_res == 0) strcpy(raw_output, "Swipe OK");
            }
            else if (strcmp(tool_name, "device_screenshot") == 0) {
                char* cmd = "screencap -p | base64 -w 0";
                exec_res = run_bridge_command(CMD_STREAM, cmd, strlen(cmd) + 1, raw_output, sizeof(raw_output));
            }
            else if (strcmp(tool_name, "device_input_text") == 0) {
                char text[1024] = {0};
                extract_json_string(line, "text", text, sizeof(text));
                char cmd[1200];
                snprintf(cmd, sizeof(cmd), "input text \"%s\"", text);
                exec_res = run_bridge_command(CMD_STREAM, cmd, strlen(cmd) + 1, raw_output, sizeof(raw_output));
            }
            else if (strcmp(tool_name, "device_logcat") == 0) {
                char lines_str[16] = {0}, filter[128] = {0}, cmd[512] = {0};
                extract_json_string(line, "lines", lines_str, sizeof(lines_str));
                extract_json_string(line, "filter", filter, sizeof(filter));
                int lines = atoi(lines_str) > 0 ? atoi(lines_str) : 100;
                
                if (strlen(filter) > 0) {
                    snprintf(cmd, sizeof(cmd), "logcat -d -t %d | grep -i \"%s\"", lines, filter);
                } else {
                    snprintf(cmd, sizeof(cmd), "logcat -d -t %d", lines);
                }
                exec_res = run_bridge_command(CMD_STREAM, cmd, strlen(cmd) + 1, raw_output, sizeof(raw_output));
            }
            else if (strcmp(tool_name, "device_file_read") == 0) {
                char path[512] = {0};
                extract_json_string(line, "path", path, sizeof(path));
                char cmd[1024];
                snprintf(cmd, sizeof(cmd), "cat \"%s\"", path);
                exec_res = run_bridge_command(CMD_STREAM, cmd, strlen(cmd) + 1, raw_output, sizeof(raw_output));
            }
            else {
                exec_res = -1;
                strcpy(raw_output, "Unknown tool");
            }

            // --- SEND RESPONSE ---
            escape_json_string(raw_output, escaped_output, 131072);
            
            // Correct JSON-RPC Response Format
            if (exec_res != 0) {
                // MCP allows return text in isError true
                printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}\n", id, escaped_output);
            } else {
                printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}\n", id, escaped_output);
            }
        }
        // Ignore unknown methods
    }
    free(escaped_output);
    return 0;
}
