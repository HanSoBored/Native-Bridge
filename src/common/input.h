#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

void input_set_device(const char* device_path);
void input_tap(int x, int y);
void input_swipe(int x1, int y1, int x2, int y2, uint64_t duration_ms);

#endif
