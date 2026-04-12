#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <getopt.h>
#include "common.h"
#include "input.h"

typedef struct {
    int sock;
    pthread_mutex_t mutex;
} ClientCtx;

typedef struct {
    ClientCtx* ctx;
    int fd;
    int is_stderr;
} StreamArg;

void send_packet(ClientCtx* ctx, uint32_t type, const void* payload, uint32_t len) {
    PacketHeader hdr = {type, len};
    pthread_mutex_lock(&ctx->mutex);
    write_all(ctx->sock, &hdr, sizeof(hdr));
    if (len > 0) write_all(ctx->sock, payload, len);
    pthread_mutex_unlock(&ctx->mutex);
}

void* stream_reader(void* arg) {
    StreamArg* sa = (StreamArg*)arg;
    char buffer[4096];
    ssize_t n;

    while ((n = read(sa->fd, buffer, sizeof(buffer))) > 0) {
        send_packet(sa->ctx, sa->is_stderr ? RESP_STREAM_ERR : RESP_STREAM_CHUNK, buffer, n);
    }
    close(sa->fd);
    return NULL;
}

void* handle_client(void* arg) {
    int client_sock = (int)(intptr_t)arg;
    ClientCtx ctx = { .sock = client_sock };
    pthread_mutex_init(&ctx.mutex, NULL);

    PacketHeader hdr;
    if (read_all(client_sock, &hdr, sizeof(hdr)) < 0) goto cleanup_sock;

    if (hdr.len > MAX_PAYLOAD_SIZE) goto cleanup_sock;

    char* payload = NULL;
    if (hdr.len > 0) {
        payload = malloc(hdr.len + 1); // Extra byte for safety
        if (!payload) goto cleanup_sock;
        if (read_all(client_sock, payload, hdr.len) < 0) goto cleanup_mem;
        payload[hdr.len] = '\0'; // Ensure termination
    }

    if (hdr.type == CMD_PING) {
        char* msg = "Pong!";
        send_packet(&ctx, RESP_SUCCESS, msg, strlen(msg) + 1);
    }
    #ifdef DIRECT_INPUT
    else if (hdr.type == CMD_TAP && hdr.len == sizeof(PayloadTap)) {
        PayloadTap* tap = (PayloadTap*)payload;
        input_tap(tap->x, tap->y);
        send_packet(&ctx, RESP_SUCCESS, "", 0);
    }
    else if (hdr.type == CMD_SWIPE && hdr.len == sizeof(PayloadSwipe)) {
        PayloadSwipe* swipe = (PayloadSwipe*)payload;
        input_swipe(swipe->x1, swipe->y1, swipe->x2, swipe->y2, swipe->duration_ms);
        send_packet(&ctx, RESP_SUCCESS, "", 0);
    }
    #endif
    else if (hdr.type == CMD_EXEC || hdr.type == CMD_STREAM) {
        if (!payload || hdr.len == 0) {
            char* err = "Empty command.";
            send_packet(&ctx, RESP_ERROR, err, strlen(err));
            goto cleanup_mem;
        }

        char* args[64];
        int argc = 0;
        char* ptr = payload;
        while (ptr < payload + hdr.len && argc < 63) {
            args[argc++] = ptr;
            // Safe increment: don't scan past hdr.len
            size_t max_scan = (payload + hdr.len) - ptr;
            size_t token_len = strnlen(ptr, max_scan);
            ptr += token_len + 1;
        }
        args[argc] = NULL;

        if (argc == 0 || args[0] == NULL) goto cleanup_mem;

        if (strcmp(args[0], "logcat") == 0) {
            int valid = (argc == 2 && (strcmp(args[1], "-d") == 0 || strcmp(args[1], "-c") == 0));
            if (hdr.type == CMD_STREAM) valid = 1; // Allow live logcat for stream
            if (!valid) {
                char* err = "Use CMD_STREAM for live logcat. Only 'logcat -d' or '-c' allowed with CMD_EXEC.";
                send_packet(&ctx, RESP_ERROR, err, strlen(err));
                goto cleanup_mem;
            }
        }

        if (hdr.type == CMD_EXEC) {
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                char* err = "Failed to create pipe";
                send_packet(&ctx, RESP_ERROR, err, strlen(err));
                goto cleanup_mem;
            }
            pid_t pid = fork();
            if (pid == 0) {
                dup2(pipefd[1], STDOUT_FILENO); 
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[0]); 
                close(pipefd[1]);
                // Close the client socket in the child to prevent leaks
                close(client_sock);

                if (argc == 1) {
                    char* sh_args[] = {"sh", "-c", args[0], NULL};
                    execvp("sh", sh_args);
                } else {
                    execvp(args[0], args);
                }
                
                // If execvp fails, the error message from perror will go to the pipe
                perror("execvp");
                exit(1);
            }
            close(pipefd[1]);

            char out[8192] = {0};
            int total = 0, n;
            while(total < sizeof(out)-1 && (n = read(pipefd[0], out + total, sizeof(out) - total - 1)) > 0) total += n;
            close(pipefd[0]);

            int status; waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                send_packet(&ctx, RESP_SUCCESS, out, total);
            else
                send_packet(&ctx, RESP_ERROR, out, total);

        } else if (hdr.type == CMD_STREAM) {
            int pipe_out[2], pipe_err[2];
            if (pipe(pipe_out) < 0 || pipe(pipe_err) < 0) {
                char* err = "Failed to create streaming pipes";
                send_packet(&ctx, RESP_ERROR, err, strlen(err));
                goto cleanup_mem;
            }
            pid_t pid = fork();
            if (pid == 0) {
                dup2(pipe_out[1], STDOUT_FILENO); 
                dup2(pipe_err[1], STDERR_FILENO);
                close(pipe_out[0]); close(pipe_out[1]); 
                close(pipe_err[0]); close(pipe_err[1]);
                close(client_sock);
                
                if (argc == 1) {
                    char* sh_args[] = {"sh", "-c", args[0], NULL};
                    execvp("sh", sh_args);
                } else {
                    execvp(args[0], args);
                }

                perror("execvp (stream)");
                exit(1);
            }
            close(pipe_out[1]); close(pipe_err[1]);

            pthread_t t1, t2;
            StreamArg arg_out = {&ctx, pipe_out[0], 0};
            StreamArg arg_err = {&ctx, pipe_err[0], 1};

            pthread_create(&t1, NULL, stream_reader, &arg_out);
            pthread_create(&t2, NULL, stream_reader, &arg_err);
            pthread_join(t1, NULL); pthread_join(t2, NULL);

            waitpid(pid, NULL, 0);
            send_packet(&ctx, RESP_STREAM_END, "", 0);
        }
    }

cleanup_mem:
    if (payload) free(payload);
cleanup_sock:
    close(client_sock);
    return NULL;
}

void print_server_help(const char* prog_name) {
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

    // Set umask to 0 so we can explicitly control permissions
    mode_t old_mask = umask(0000);
    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Failed to bind socket");
        umask(old_mask);
        return 1;
    }
    umask(old_mask);

    // Socket permissions: 0666 (rw-rw-rw-)
    //
    // WHY: The server runs as root on Android Host. Clients run as a non-root
    // user inside a chroot. The chroot's user database is invisible to the
    // Android kernel, so:
    //   - 0660 fails: the chroot user isn't root or in root's group on Android
    //   - SO_PEERCRED fails: UID namespaces differ between chroot and host
    //   - Group chown fails: the group doesn't exist on Android's /etc/group
    //
    // The chroot IS the security boundary. Any process inside it is already
    // trusted. See docs/ADR-001-socket-permissions.md for full analysis.
    if (chmod(socket_path, 0666) < 0) {
        perror("Failed to chmod socket");
    }

    listen(server_sock, 10);

    printf("Bridge Server active at: %s (Permissions: 0666)\n", socket_path);
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
        pthread_create(&tid, NULL, handle_client, (void*)(intptr_t)client_sock);
        pthread_detach(tid);
    }
    return 0;
}
