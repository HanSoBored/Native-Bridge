#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "common.h"

int sockfd;

static void intHandler(int dummy) {
    (void)dummy;
    printf("\nExiting...\n");
    if(sockfd > 0) close(sockfd);
    exit(0);
}

static void print_help(void) {
    printf("Native-Bridge Client\n");
    printf("Usage:\n");
    printf("  andro -e <cmd> [args...]\n");
    printf("  andro -s <cmd> [args...]\n");
    printf("  andro tap <x> <y>\n");
    printf("  andro swipe <x1> <y1> <x2> <y2> [duration_ms]\n");
    printf("  andro ping\n");
}

int main(int argc, char *argv[]) {
    signal(SIGINT, intHandler);

    if (argc < 2) { print_help(); return 1; }

    const char* socket_path = getenv("BRIDGE_SOCKET");
    if (socket_path == NULL) {
        fprintf(stderr, "Error: BRIDGE_SOCKET environment variable is not set.\n");
        fprintf(stderr, "Usage: export BRIDGE_SOCKET=/path/to/socket\n");
        return 1;
    }

    struct sockaddr_un addr;
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "FATAL: Socket path from env is too long!\n");
        return 1;
    }

    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        fprintf(stderr, "Failed to connect to %s.\n", socket_path);
        fprintf(stderr, "Hint: Is the server running?\n");
        return 1;
    }

    PacketHeader hdr = {0};
    char payload[MAX_PAYLOAD_SIZE] = {0};

    if (strcmp(argv[1], "-e") == 0 || strcmp(argv[1], "-s") == 0) {
        if (argc < 3) return 1;
        hdr.type = (strcmp(argv[1], "-e") == 0) ? CMD_EXEC : CMD_STREAM;
        for (int i = 2; i < argc; i++) {
            size_t len = strlen(argv[i]);
            if (hdr.len + len + 1 > sizeof(payload)) break;
            strcpy(payload + hdr.len, argv[i]);
            hdr.len += (uint32_t)(len + 1);
        }
    }
    else if (strcmp(argv[1], "tap") == 0) {
        if (argc != 4) return 1;
        hdr.type = CMD_TAP;
        PayloadTap tap = {atoi(argv[2]), atoi(argv[3])};
        memcpy(payload, &tap, sizeof(tap));
        hdr.len = sizeof(tap);
    }
    else if (strcmp(argv[1], "swipe") == 0) {
        if (argc < 6) return 1;
        hdr.type = CMD_SWIPE;
        PayloadSwipe swipe = {
            atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
            (uint64_t)((argc > 6) ? atoi(argv[6]) : 300)
        };
        memcpy(payload, &swipe, sizeof(swipe));
        hdr.len = sizeof(swipe);
    }
    else if (strcmp(argv[1], "ping") == 0) { hdr.type = CMD_PING; hdr.len = 0; }
    else { print_help(); return 1; }

    write_all(sockfd, &hdr, sizeof(hdr));
    if (hdr.len > 0) write_all(sockfd, payload, hdr.len);

    while (1) {
        PacketHeader res_hdr;
        if (read_all(sockfd, &res_hdr, sizeof(res_hdr)) < 0) break;

        if (res_hdr.len > MAX_PAYLOAD_SIZE) break;

        char* res_payload = NULL;
        if (res_hdr.len > 0) {
            res_payload = malloc(res_hdr.len + 1);
            if (read_all(sockfd, res_payload, res_hdr.len) < 0) {
                free(res_payload);
                break;
            }
            res_payload[res_hdr.len] = '\0';
        }

        if (res_hdr.type == RESP_STREAM_CHUNK) {
            printf("%s", res_payload);
            fflush(stdout);
        } else if (res_hdr.type == RESP_STREAM_ERR) {
            fprintf(stderr, "%s", res_payload);
            fflush(stderr);
        } else if (res_hdr.type == RESP_STREAM_END) {
            if (res_payload) free(res_payload);
            break;
        } else if (res_hdr.type == RESP_SUCCESS) {
            if (res_payload && strlen(res_payload) > 0) {
                printf("%s", res_payload);
                if (res_payload[res_hdr.len-1] != '\n') printf("\n");
            }
            if (res_payload) free(res_payload);
            break;
        } else if (res_hdr.type == RESP_ERROR) {
            fprintf(stderr, "Remote Error: %s\n", res_payload ? res_payload : "Unknown");
            if (res_payload) free(res_payload);
            break;
        }
        if (res_payload) free(res_payload);
    }

    close(sockfd);
    return 0;
}
