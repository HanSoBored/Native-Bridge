#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#define MAX_PAYLOAD_SIZE 8192

typedef enum { CMD_EXEC = 1, CMD_STREAM, CMD_PING, CMD_TAP, CMD_SWIPE } CommandType;
typedef enum { RESP_SUCCESS = 1, RESP_ERROR, RESP_STREAM_CHUNK, RESP_STREAM_END, RESP_STREAM_ERR } ResponseType;

typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t len;
} PacketHeader;

typedef struct __attribute__((packed)) { int32_t x; int32_t y; } PayloadTap;
typedef struct __attribute__((packed)) { int32_t x1; int32_t y1; int32_t x2; int32_t y2; uint64_t duration_ms; } PayloadSwipe;

static inline int write_all(int fd, const void *buf, size_t count) {
    size_t bytes_written = 0;
    const uint8_t *ptr = (const uint8_t*)buf;
    while (bytes_written < count) {
        ssize_t res = write(fd, ptr + bytes_written, count - bytes_written);
        if (res <= 0) { if (errno == EINTR) continue; return -1; }
        bytes_written += (size_t)res;
    }
    return 0;
}

static inline int read_all(int fd, void *buf, size_t count) {
    size_t bytes_read = 0;
    uint8_t *ptr = (uint8_t*)buf;
    while (bytes_read < count) {
        ssize_t res = read(fd, ptr + bytes_read, count - bytes_read);
        if (res <= 0) { if (errno == EINTR) continue; return -1; }
        bytes_read += (size_t)res;
    }
    return 0;
}

#endif // COMMON_H
