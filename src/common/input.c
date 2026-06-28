#include "input.h"
#include "common.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <stdatomic.h>

/* Touch-down duration: long enough for kernel/display driver to reliably
 * register the touch-down event before touch-up, short enough to feel
 * instant. Values below ~20ms can be dropped by some Android drivers. */
#define TOUCH_DOWN_MS 50

/* Atomic sequence counter for touch tracking-IDs. Incremented on every
 * tap/swipe to ensure unique ABS_MT_TRACKING_ID per touch session.
 * Wraparound from UINT_MAX back to 0 is theoretically possible after
 * ~4 billion taps (~13 years at 10/sec), making reuse astronomically
 * unlikely in practice. The touch-up path always uses tracking_id=-1
 * (kernel "finger removed" sentinel), so stale IDs are harmless. */
static _Atomic unsigned int tap_seq = 0;
static char current_touch_device[256] = "";

void input_set_device(const char* device_path) {
    strncpy(current_touch_device, device_path, sizeof(current_touch_device) - 1);
    current_touch_device[sizeof(current_touch_device) - 1] = '\0';
    fprintf(stderr, "[INPUT] Touch device set to: %s\n", device_path);
}

static int write_event(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    if (type < 0 || type > 0xFFFF) return -1;
    if (code < 0 || code > 0xFFFF) return -1;
    ev.type = (__u16)type;
    ev.code = (__u16)code;
    /* value fits in __s32: callers pass coordinates (< 10000), tracking IDs
     * (atomic counter, ~4B wraps after 13 yrs at 10Hz), or touch_state (0/1). */
    ev.value = (__s32)value;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        fprintf(stderr, "[INPUT] write_event failed: type=%d code=%d value=%d fd=%d\n",
                type, code, value, fd);
        return -1;
    }
    return 0;
}

/* Unified touch event: touch_state=1 for down, 0 for up. */
static int send_touch_event(int fd, int tracking_id, int x, int y, int touch_state) {
    /* Bounds: 0-10000 covers standard Android touch input. Some high-DPI
     * digitizers report up to 32767, but this limit is consistent across
     * the codebase (see MAX_COORDINATE in mcp.c). Expand if needed. */
    if (x < 0 || x > 10000 || y < 0 || y > 10000) {
        fprintf(stderr, "[INPUT] send_touch_event: coordinates (%d,%d) out of range (0-10000)\n", x, y);
        return -1;
    }
    if (write_event(fd, EV_ABS, ABS_MT_TRACKING_ID, tracking_id) < 0) return -1;
    if (write_event(fd, EV_ABS, ABS_MT_POSITION_X, x) < 0) return -1;
    if (write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y) < 0) return -1;
    if (write_event(fd, EV_KEY, BTN_TOUCH, touch_state) < 0) return -1;
    if (write_event(fd, EV_SYN, SYN_REPORT, 0) < 0) return -1;
    return 0;
}

static int open_touch_device(const char* caller) {
    if (current_touch_device[0] == '\0') {
        fprintf(stderr, "[INPUT] %s FAILED: no touch device set (use -d flag)\n", caller);
        return -1;
    }
    int fd = open(current_touch_device, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[INPUT] %s FAILED: cannot open '%s': %s\n",
                caller, current_touch_device, strerror(errno));
        return -1;
    }
    return fd;
}

/* EINTR-safe nanosleep with iteration guard to prevent infinite loops on
 * persistent non-EINTR errors (e.g., EINVAL). Retry up to 100 times if
 * interrupted by a signal; beyond that, continue normally (best-effort). */
static void reliable_nanosleep(struct timespec ts) {
    struct timespec rem;
    int attempts = 0;
    while (nanosleep(&ts, &rem) < 0 && errno == EINTR && attempts < 100) {
        ts = rem;
        attempts++;
    }
}

int input_tap(int x, int y) {
    int fd = open_touch_device("tap(x,y)");
    if (fd < 0) return -1;

    fprintf(stderr, "[INPUT] tap(%d,%d) on %s\n", x, y, current_touch_device);

    unsigned int my_seq = atomic_fetch_add(&tap_seq, 1);
    if (send_touch_event(fd, (int)my_seq, x, y, 1) < 0) {
        fprintf(stderr, "[INPUT] tap(%d,%d) FAILED: write error during touch down\n", x, y);
        close(fd);
        return -1;
    }

    /* Touch-down delay: see TOUCH_DOWN_MS definition for rationale. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = (long)(TOUCH_DOWN_MS * 1000000L) };
    reliable_nanosleep(ts);

    if (send_touch_event(fd, -1, x, y, 0) < 0) {
        fprintf(stderr, "[INPUT] tap(%d,%d) FAILED: write error during touch up\n", x, y);
        close(fd);
        return -1;
    }

    close(fd);
    fprintf(stderr, "[INPUT] tap(%d,%d) OK\n", x, y);
    return 0;
}

int input_swipe(int x1, int y1, int x2, int y2, uint64_t duration_ms) {
    int fd = open_touch_device("swipe(x1,y1->x2,y2)");
    if (fd < 0) return -1;

    fprintf(stderr, "[INPUT] swipe(%d,%d->%d,%d) dur=%lu ms on %s\n",
            x1, y1, x2, y2, (unsigned long)duration_ms, current_touch_device);

    /* Cap duration to prevent int overflow in steps calculation */
    if (duration_ms > 30000) duration_ms = 30000;

    int step_delay = 10;
    int steps = (int)(duration_ms / (uint64_t)step_delay);
    if (steps < 1) steps = 1;

    float dx = (float)(x2 - x1) / (float)steps;
    float dy = (float)(y2 - y1) / (float)steps;

    unsigned int my_seq = atomic_fetch_add(&tap_seq, 1);
    if (send_touch_event(fd, (int)my_seq, x1, y1, 1) < 0) {
        fprintf(stderr, "[INPUT] swipe FAILED: write error during touch down\n");
        close(fd);
        return -1;
    }

    float cx = (float)x1, cy = (float)y1;
    for (int i = 0; i < steps; i++) {
        cx += dx;
        cy += dy;
        if (write_event(fd, EV_ABS, ABS_MT_POSITION_X, (int)cx) < 0) goto cleanup;
        if (write_event(fd, EV_ABS, ABS_MT_POSITION_Y, (int)cy) < 0) goto cleanup;
        if (write_event(fd, EV_SYN, SYN_REPORT, 0) < 0) goto cleanup;
        struct timespec step_ts = { .tv_sec = 0, .tv_nsec = (long)(step_delay * 1000000) };
        reliable_nanosleep(step_ts);
    }

    if (send_touch_event(fd, -1, x2, y2, 0) < 0) {
        fprintf(stderr, "[INPUT] swipe FAILED: write error during touch up\n");
        close(fd);
        return -1;
    }

    close(fd);
    fprintf(stderr, "[INPUT] swipe(%d,%d->%d,%d) OK\n", x1, y1, x2, y2);
    return 0;

cleanup:
    close(fd);
    fprintf(stderr, "[INPUT] swipe(%d,%d->%d,%d) FAILED (broken fd in loop)\n",
            x1, y1, x2, y2);
    return -1;
}
