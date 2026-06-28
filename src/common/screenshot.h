#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <stddef.h>

int downscale_screenshot_base64(const char* input_b64, char* output, size_t out_size, unsigned target_width, unsigned* orig_w, unsigned* orig_h);

#endif
