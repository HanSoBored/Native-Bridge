#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/signal.h>
#include <stddef.h>
#include <getopt.h>
#include "common.h"
#include "input.h"

#define MAX_ARGS 64

static const char ERR_EXEC_FAILED[] = "Failed to execute command";

typedef struct {
    int sock;
    pthread_mutex_t mutex;
} ClientCtx;

typedef struct {
    ClientCtx* ctx;
    int fd;
    int is_stderr;
} StreamArg;

static int send_packet(ClientCtx* ctx, uint32_t type, const void* payload, uint32_t len) {
    PacketHeader hdr = {type, len};
    pthread_mutex_lock(&ctx->mutex);
    int res = write_all(ctx->sock, &hdr, sizeof(hdr));
    if (res == 0 && len > 0) res = write_all(ctx->sock, payload, len);
    pthread_mutex_unlock(&ctx->mutex);
    return res;
}

static void* stream_reader(void* arg) {
    StreamArg* sa = (StreamArg*)arg;
    char buffer[4096];
    ssize_t n;

    while ((n = read(sa->fd, buffer, sizeof(buffer))) > 0) {
        if (send_packet(sa->ctx, sa->is_stderr ? RESP_STREAM_ERR : RESP_STREAM_CHUNK, buffer, (uint32_t)n) < 0) {
            break;  /* write failed — client connection broken */
        }
    }
    close(sa->fd);
    return NULL;
}

/* ------------------------------------------------------------------- */
/* SHARED HELPERS                                                      */
/* ------------------------------------------------------------------- */

/* parse_argv - Parse null-terminated payload into argv array
 * @payload: Null-terminated string sequence (each token \0-delimited)
 * @len: Total payload length in bytes
 * @args: Output array for parsed tokens
 * @max_args: Maximum number of tokens
 * Returns: argc (number of tokens), or 0 if empty */
static int parse_argv(char* payload, uint32_t len, char** args, int max_args) {
    int argc = 0;
    char* ptr = payload;
    while (ptr < payload + len && argc < max_args - 1) {
        args[argc++] = ptr;
        size_t max_scan = (size_t)((payload + (ptrdiff_t)len) - ptr);
        if (max_scan == 0) break;
        size_t token_len = strnlen(ptr, max_scan);
        ptr += token_len + 1;
    }
    args[argc] = NULL;
    return argc;
}

/* Shared helper: validates non-empty payload and parses into argv array.
 * Returns argc on success, -1 on error (error packet already sent via ctx). */
static int parse_command_payload(ClientCtx* ctx, char* payload, uint32_t len,
                                  char** args, int max_args) {
    if (!payload || len == 0) {
        char* err = "Empty command.";
        size_t err_len = strlen(err);
        send_packet(ctx, RESP_ERROR, err, (uint32_t)err_len);
        return -1;
    }

    int argc = parse_argv(payload, len, args, max_args);
    if (argc == 0 || args[0] == NULL) {
        char* err = "Empty command or missing command name.";
        size_t err_len = strlen(err);
        send_packet(ctx, RESP_ERROR, err, (uint32_t)err_len);
        return -1;
    }

    return argc;
}

/* fork_exec - Fork and exec a command with redirected output
 * @args: argv array (NULL-terminated)
 * @argc: number of arguments
 * @close_fd: fd to close in child (e.g., client socket), or -1
 * @out_fd: output pipe read fd (set in parent), or NULL
 * @err_fd: stderr pipe read fd (set in parent), NULL = merge to stdout
 * Returns: child PID on success, -1 on error */
static pid_t fork_exec(char** args, int argc, int close_fd,
                       int* out_fd, int* err_fd) {
    int pipe_out[2];
    if (pipe(pipe_out) < 0) return -1;

    int pipe_err[2];
    int sep = (err_fd != NULL);
    if (sep) {
        if (pipe(pipe_err) < 0) { close(pipe_out[0]); close(pipe_out[1]); return -1; }
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_out[0]); close(pipe_out[1]);
        if (sep) { close(pipe_err[0]); close(pipe_err[1]); }
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdout and stderr to pipes */
        close(pipe_out[0]);
        if (dup2(pipe_out[1], STDOUT_FILENO) < 0) _exit(1);
        close(pipe_out[1]);
        if (sep) {
            close(pipe_err[0]);
            if (dup2(pipe_err[1], STDERR_FILENO) < 0) _exit(1);
            close(pipe_err[1]);
        } else {
            if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) _exit(1);
        }
        if (close_fd >= 0) close(close_fd);

        if (argc == 1) {
            char* sh_args[] = {"sh", "-c", args[0], NULL};
            execvp("sh", sh_args);
        } else {
            execvp(args[0], args);
        }
        perror("execvp");
        _exit(1);
    }

    /* Parent: close write ends, set read fds */
    close(pipe_out[1]);
    if (sep) close(pipe_err[1]);
    if (out_fd) *out_fd = pipe_out[0];
    if (err_fd) *err_fd = sep ? pipe_err[0] : -1;
    return pid;
}

/* ------------------------------------------------------------------- */
/* CMD HANDLERS (SRP — each handles one command type)                  */
/* ------------------------------------------------------------------- */

static int handle_client_ping(ClientCtx* ctx) {
    char* msg = "Pong!";
    size_t msg_len = strlen(msg) + 1;
    /* "Pong!" is 6 bytes, trivially safe */
    return send_packet(ctx, RESP_SUCCESS, msg, (uint32_t)msg_len);
}

#ifdef DIRECT_INPUT
static int handle_client_tap(ClientCtx* ctx, PayloadTap* tap) {
    fprintf(stderr, "[SERVER] CMD_TAP (%d, %d)\n", tap->x, tap->y);
    if (input_tap(tap->x, tap->y) == 0) {
        return send_packet(ctx, RESP_SUCCESS, "", 0);
    } else {
        char* err = "Tap failed (check server stderr for details)";
        size_t err_len = strlen(err);
        /* error messages are short */
        return send_packet(ctx, RESP_ERROR, err, (uint32_t)err_len);
    }
}

static int handle_client_swipe(ClientCtx* ctx, PayloadSwipe* swipe) {
    fprintf(stderr, "[SERVER] CMD_SWIPE (%d,%d)->(%d,%d) dur=%lu\n",
            swipe->x1, swipe->y1, swipe->x2, swipe->y2, (unsigned long)swipe->duration_ms);
    if (input_swipe(swipe->x1, swipe->y1, swipe->x2, swipe->y2, swipe->duration_ms) == 0) {
        return send_packet(ctx, RESP_SUCCESS, "", 0);
    } else {
        char* err = "Swipe failed (check server stderr for details)";
        size_t err_len = strlen(err);
        /* error messages are short */
        return send_packet(ctx, RESP_ERROR, err, (uint32_t)err_len);
    }
}
#endif

/* Extract first whitespace-delimited token from a command string.
 * Returns 1 if a token was found, 0 if empty. */
static int get_first_token(const char* cmd, char* token, size_t token_size) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    size_t i = 0;
    while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t' && i < token_size - 1) {
        token[i] = cmd[i];
        i++;
    }
    token[i] = '\0';
    return (i > 0) ? 1 : 0;
}

/* Validates logcat-specific arguments for CMD_EXEC.
 * Returns an error string if invalid, NULL if valid. */
static const char* validate_logcat_exec_args(char** args, int argc) {
    /* args[0] is a shell command string; extract first word */
    char first_token[64];
    if (!get_first_token(args[0], first_token, sizeof(first_token)))
        return NULL;  /* empty command, let caller handle */
    if (strcmp(first_token, "logcat") == 0) {
        int valid = (argc == 2 && (strcmp(args[1], "-d") == 0 || strcmp(args[1], "-c") == 0));
        if (!valid) {
            return "Use CMD_STREAM for live logcat. Only 'logcat -d' or '-c' allowed with CMD_EXEC.";
        }
    }
    return NULL;
}

/* Reads data from a pipe fd into a buffer until EOF or buffer full.
 * Returns total bytes read, or -1 on read error. */
static ssize_t read_pipe_to_buffer(int fd, char* buf, size_t bufsize) {
    ssize_t total = 0;
    ssize_t n;
    while (total < (ssize_t)bufsize - 1 &&
           (n = read(fd, buf + total, bufsize - (size_t)total - 1)) > 0)
        total += n;
    return (n < 0) ? -1 : total;
}

static int handle_client_exec(ClientCtx* ctx, char* payload, uint32_t len) {
    char* args[MAX_ARGS];
    int argc = parse_command_payload(ctx, payload, len, args, MAX_ARGS);
    if (argc < 0) return -1;

    /* Validate logcat-specific args */
    const char* logcat_err = validate_logcat_exec_args(args, argc);
    if (logcat_err) {
        size_t err_len = strlen(logcat_err);
        return send_packet(ctx, RESP_ERROR, logcat_err, (uint32_t)err_len);
    }

    int out_fd;
    pid_t pid = fork_exec(args, argc, ctx->sock, &out_fd, NULL);
    if (pid < 0) {
        const char* err = ERR_EXEC_FAILED;
        size_t err_len = strlen(err);
        return send_packet(ctx, RESP_ERROR, err, (uint32_t)err_len);
    }

    char* out = calloc(1, 8192);
    if (!out) {
        char* err = "Out of memory reading command output";
        size_t err_len = strlen(err);
        close(out_fd);
        return send_packet(ctx, RESP_ERROR, err, (uint32_t)err_len);
    }
    ssize_t total = read_pipe_to_buffer(out_fd, out, 8192);
    close(out_fd);
    if (total < 0) total = 0;
    /* Warn if output may have been truncated (buffer full at 8191 bytes) */
    if (total == 8191) {
        fprintf(stderr, "Warning: command output possibly truncated "
                        "(%zd bytes, 8KB buffer limit)\n", total);
    }

    int status;
    waitpid(pid, &status, 0);
    /* total is always <= 8192: read_pipe_to_buffer clamps at bufsize-1 */
    assert(total >= 0 && total < 8192);
    int ret;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        ret = send_packet(ctx, RESP_SUCCESS, out, (uint32_t)total);
    } else if (WIFSIGNALED(status)) {
        char sig_msg[64];
        snprintf(sig_msg, sizeof(sig_msg), "Killed by signal %d", WTERMSIG(status));
        ret = send_packet(ctx, RESP_ERROR, sig_msg, (uint32_t)strlen(sig_msg) + 1);
    } else {
        ret = send_packet(ctx, RESP_ERROR, out, (uint32_t)total);
    }
    free(out);
    return ret;
}

static int handle_client_stream(ClientCtx* ctx, char* payload, uint32_t len) {
    char* args[MAX_ARGS];
    int argc = parse_command_payload(ctx, payload, len, args, MAX_ARGS);
    if (argc < 0) return -1;

    int out_fd, err_fd;
    pid_t pid = fork_exec(args, argc, ctx->sock, &out_fd, &err_fd);
    if (pid < 0) {
        const char* err = ERR_EXEC_FAILED;
        size_t err_len = strlen(err);
        return send_packet(ctx, RESP_ERROR, err, (uint32_t)err_len);
    }

    pthread_t t1, t2;
    StreamArg arg_out = {ctx, out_fd, 0};
    StreamArg arg_err = {ctx, err_fd, 1};

    if (pthread_create(&t1, NULL, stream_reader, &arg_out) != 0) {
        close(out_fd); close(err_fd); return -1;
    }
    if (pthread_create(&t2, NULL, stream_reader, &arg_err) != 0) {
        close(out_fd); pthread_join(t1, NULL);
        close(err_fd); return -1;
    }
    pthread_join(t1, NULL); pthread_join(t2, NULL);

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        return send_packet(ctx, RESP_ERROR, "Command failed", 15);
    return send_packet(ctx, RESP_STREAM_END, "", 0);
}

static int handle_client_unknown(ClientCtx* ctx) {
    char* err = "Unknown command or invalid payload";
    size_t err_len = strlen(err);
    return send_packet(ctx, RESP_ERROR, err, (uint32_t)err_len);
}

/* ------------------------------------------------------------------- */
/* DISPATCH FUNCTION (thin — header reading + dispatch + cleanup)      */
/* ------------------------------------------------------------------- */
static void* handle_client(void* arg) {
    int client_sock = (int)(intptr_t)arg;
    ClientCtx ctx = { .sock = client_sock };
    pthread_mutex_init(&ctx.mutex, NULL);

    PacketHeader hdr;
    if (read_all(client_sock, &hdr, sizeof(hdr)) < 0) goto cleanup_sock;
    if (hdr.len > MAX_PAYLOAD_SIZE) goto cleanup_sock;

    char* payload = NULL;
    if (hdr.len > 0) {
        payload = malloc(hdr.len + 1);
        if (!payload) goto cleanup_sock;
        if (read_all(client_sock, payload, hdr.len) < 0) goto cleanup_mem;
        payload[hdr.len] = '\0';
    }

    {
        int ret = 0;
        if (hdr.type == CMD_PING) {
            ret = handle_client_ping(&ctx);
        }
        #ifdef DIRECT_INPUT
        else if (hdr.type == CMD_TAP && hdr.len == sizeof(PayloadTap)) {
            ret = handle_client_tap(&ctx, (PayloadTap*)payload);
        }
        else if (hdr.type == CMD_SWIPE && hdr.len == sizeof(PayloadSwipe)) {
            ret = handle_client_swipe(&ctx, (PayloadSwipe*)payload);
        }
        #endif
        else if (hdr.type == CMD_EXEC) {
            ret = handle_client_exec(&ctx, payload, hdr.len);
        }
        else if (hdr.type == CMD_STREAM) {
            ret = handle_client_stream(&ctx, payload, hdr.len);
        }
        else {
            ret = handle_client_unknown(&ctx);
        }
        if (ret < 0) {
            /* send_packet failed — client connection is broken,
             * fall through to cleanup */
        }
    }

cleanup_mem:
    if (payload) free(payload);
cleanup_sock:
    pthread_mutex_destroy(&ctx.mutex);
    close(client_sock);
    return NULL;
}

static void print_server_help(const char* prog_name) {
    printf("Native-Bridge Server Daemon\n");
    printf("Usage: %s -s <socket_path> [-d <touch_device>]\n\n", prog_name);
    printf("Options:\n");
    printf("  -s <path>    Path for the Unix socket (required)\n");
    printf("  -d <dev>     Kernel touch device path (optional)\n");
    printf("  -h           Show this help\n");
}

int main(int argc, char *argv[]) {
    char socket_path[256] = "";
    char touch_device[256] = "";
    int touch_enabled = 0;

    int opt;
    while ((opt = getopt(argc, argv, "s:d:h")) != -1) {
        switch (opt) {
            case 's':
                snprintf(socket_path, sizeof(socket_path), "%s", optarg);
                break;
            case 'd':
                snprintf(touch_device, sizeof(touch_device), "%s", optarg);
                touch_enabled = 1;
                break;
            case 'h':
            default:
                print_server_help(argv[0]);
                return 1;
        }
    }

    if (socket_path[0] == '\0') {
        fprintf(stderr, "Error: -s (socket path) is required.\n\n");
        print_server_help(argv[0]);
        return 1;
    }

    if (touch_enabled) {
        input_set_device(touch_device);
    }

    signal(SIGPIPE, SIG_IGN);

    struct sockaddr_un addr;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "FATAL: Socket path is too long! (Max %lu chars)\n", sizeof(addr.sun_path) - 1);
        return 1;
    }

    unlink(socket_path);
    int server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t path_len = strlen(socket_path);
    if (path_len >= sizeof(addr.sun_path)) {
        fprintf(stderr, "FATAL: Socket path is too long!\n");
        return 1;
    }
    memcpy(addr.sun_path, socket_path, path_len + 1);

    /* Set umask to 0 so we can explicitly control permissions */
    mode_t old_mask = umask(0000);
    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Failed to bind socket");
        umask(old_mask);
        return 1;
    }
    umask(old_mask);

    /* Socket permissions: 0666 (rw-rw-rw-)
     *
     * WHY: The server runs as root on Android Host. Clients run as a non-root
     * user inside a chroot. The chroot's user database is invisible to the
     * Android kernel, so:
     *   - 0660 fails: the chroot user isn't root or in root's group on Android
     *   - SO_PEERCRED fails: UID namespaces differ between chroot and host
     *   - Group chown fails: the group doesn't exist on Android's /etc/group
     *
     * The chroot IS the security boundary. Any process inside it is already
     * trusted. See docs/ADR-001-socket-permissions.md for full analysis. */
    if (chmod(socket_path, 0666) < 0) {
        perror("Failed to chmod socket");
    }

    listen(server_sock, 10);

    printf("Bridge Server active at: %s (Permissions: 0666)\n", socket_path);
    /* Feature: DIRECT_INPUT — touch device status */
    #ifdef DIRECT_INPUT
    if (touch_enabled) {
        printf("[Feature Enabled] Direct Kernel Input Module Loaded\n");
        printf("  -> Using Touch Device: %s\n", touch_device);
    } else {
        printf("[Feature Disabled] Touch input not configured (use -d to enable)\n");
    }
    #endif

    while (1) {
        int client_sock = accept(server_sock, NULL, NULL);
        if (client_sock < 0) continue;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, (void*)(intptr_t)client_sock) != 0) {
            close(client_sock);
            continue;
        }
        pthread_detach(tid);
    }
    return 0;
}
