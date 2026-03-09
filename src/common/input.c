#include "input.h"
#include "common.h"
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static char current_touch_device[256] = "";

void input_set_device(const char* device_path) {
    strncpy(current_touch_device, device_path, sizeof(current_touch_device) - 1);
    current_touch_device[sizeof(current_touch_device) - 1] = '\0';
}

void write_event(int fd, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof(ev)) < 0) {
        // Just log or ignore, but checking the return value silences the warning
    }
}


void input_tap(int x, int y) {
    if (current_touch_device[0] == '\0') return;
    int fd = open(current_touch_device, O_WRONLY);
    if (fd < 0) return;

    write_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
    write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    write_event(fd, EV_KEY, BTN_TOUCH, 1);
    write_event(fd, EV_SYN, SYN_REPORT, 0);

    write_event(fd, EV_ABS, ABS_MT_POSITION_X, x);
    write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y);
    write_event(fd, EV_KEY, BTN_TOUCH, 0);
    write_event(fd, EV_SYN, SYN_REPORT, 0);

    usleep(50000);
    close(fd);
}

void input_swipe(int x1, int y1, int x2, int y2, uint64_t duration_ms) {
    if (current_touch_device[0] == '\0') return;
    int fd = open(current_touch_device, O_WRONLY);
    if (fd < 0) return;

    int step_delay = 10;
    int steps = duration_ms / step_delay;
    if (steps < 1) steps = 1;

    float dx = (float)(x2 - x1) / steps;
    float dy = (float)(y2 - y1) / steps;

    write_event(fd, EV_ABS, ABS_MT_POSITION_X, x1);
    write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y1);
    write_event(fd, EV_KEY, BTN_TOUCH, 1);
    write_event(fd, EV_SYN, SYN_REPORT, 0);

    float cx = x1, cy = y1;
    for (int i = 0; i < steps; i++) {
        cx += dx;
        cy += dy;
        write_event(fd, EV_ABS, ABS_MT_POSITION_X, (int)cx);
        write_event(fd, EV_ABS, ABS_MT_POSITION_Y, (int)cy);
        write_event(fd, EV_SYN, SYN_REPORT, 0);
        usleep(step_delay * 1000);
    }

    write_event(fd, EV_ABS, ABS_MT_POSITION_X, x2);
    write_event(fd, EV_ABS, ABS_MT_POSITION_Y, y2);
    write_event(fd, EV_KEY, BTN_TOUCH, 0);
    write_event(fd, EV_SYN, SYN_REPORT, 0);

    close(fd);
}
