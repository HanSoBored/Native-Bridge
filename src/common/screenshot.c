#include "screenshot.h"
#include "lodepng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const unsigned char B64DEC[256] = {
    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
    ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
};

/* Separate validity table — avoids ambiguity between 'A' (decodes to 0)
 * and invalid characters (also decode to 0 in B64DEC). */
static const unsigned char B64VALID[256] = {
    ['A']=1,['B']=1,['C']=1,['D']=1,['E']=1,['F']=1,['G']=1,['H']=1,
    ['I']=1,['J']=1,['K']=1,['L']=1,['M']=1,['N']=1,['O']=1,['P']=1,
    ['Q']=1,['R']=1,['S']=1,['T']=1,['U']=1,['V']=1,['W']=1,['X']=1,
    ['Y']=1,['Z']=1,['a']=1,['b']=1,['c']=1,['d']=1,['e']=1,['f']=1,
    ['g']=1,['h']=1,['i']=1,['j']=1,['k']=1,['l']=1,['m']=1,['n']=1,
    ['o']=1,['p']=1,['q']=1,['r']=1,['s']=1,['t']=1,['u']=1,['v']=1,
    ['w']=1,['x']=1,['y']=1,['z']=1,['0']=1,['1']=1,['2']=1,['3']=1,
    ['4']=1,['5']=1,['6']=1,['7']=1,['8']=1,['9']=1,['+']=1,['/']=1,
    ['=']=1
};

static const char B64ENC[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static unsigned char* base64_decode(const char* src, size_t* out_size) {
    size_t len = strlen(src);
    if (len == 0) return NULL;

    // Validate input length: must be at least 4 chars and multiple of 4
    if (len < 4 || len % 4 != 0) return NULL;

    size_t padding = 0;
    if (len >= 2 && src[len-1] == '=') padding++;
    if (len >= 2 && src[len-2] == '=') padding++;

    /* padding is always 0-2, len >= 4, so no underflow possible */
    size_t out_len = len / 4 * 3 - padding;
    unsigned char* out = malloc(out_len + 1);
    if (!out) return NULL;

    size_t out_idx = 0;
    for (size_t i = 0; i < len; i += 4) {
        unsigned b = 0;
        int valid = 0;
        if (i + 3 < len) {
            unsigned c0 = (unsigned char)src[i];
            unsigned c1 = (unsigned char)src[i+1];
            unsigned c2 = (unsigned char)src[i+2];
            unsigned c3 = (unsigned char)src[i+3];
            /* cN is already in [0,255] via unsigned char cast — no range check needed */
            unsigned v0 = B64DEC[c0];
            unsigned v1 = B64DEC[c1];
            unsigned v2 = (c2 == '=') ? 0 : B64DEC[c2];
            unsigned v3 = (c3 == '=') ? 0 : B64DEC[c3];
            /* Validate ALL 4 characters using dedicated validity table.
             * B64VALID avoids the 'A'=0 ambiguity in B64DEC. */
            if (B64VALID[c0] && B64VALID[c1] &&
                (c2 == '=' || B64VALID[c2]) &&
                (c3 == '=' || B64VALID[c3])) { valid = 1; }
            b = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;
        }
        if (!valid && out_len > 0) { free(out); return NULL; }
        if (out_idx < out_len) out[out_idx++] = (unsigned char)(b >> 16);
        if (out_idx < out_len && src[i+2] != '=') out[out_idx++] = (unsigned char)(b >> 8);
        if (out_idx < out_len && src[i+3] != '=') out[out_idx++] = (unsigned char)b;
    }

    *out_size = out_len;
    return out;
}

static size_t base64_encode(const unsigned char* src, size_t src_size, char* out, size_t out_size) {
    size_t needed = (src_size + 2) / 3 * 4;
    if (needed + 1 > out_size) return 0;

    size_t out_idx = 0;
    for (size_t i = 0; i < src_size; i += 3) {
        unsigned b = ((unsigned)src[i] << 16);
        if (i + 1 < src_size) b |= ((unsigned)src[i+1] << 8);
        if (i + 2 < src_size) b |= src[i+2];

        out[out_idx++] = B64ENC[(b >> 18) & 0x3F];
        out[out_idx++] = B64ENC[(b >> 12) & 0x3F];
        out[out_idx++] = (i + 1 < src_size) ? B64ENC[(b >> 6) & 0x3F] : '=';
        out[out_idx++] = (i + 2 < src_size) ? B64ENC[b & 0x3F] : '=';
    }
    out[out_idx] = '\0';
    return out_idx;
}

/* Pure box-filter (area-average) downscale — no guards, no allocation.
 * Each output pixel averages all contributing input pixels.
 * Eliminates nearest-neighbor aliasing on UI text while being simpler
 * than Mitchell/Catmull-Rom filters. */
static void box_filter_rgba(const unsigned char* input, unsigned w, unsigned h,
                            unsigned char* output, unsigned dw, unsigned dh) {
    for (unsigned ry = 0; ry < dh; ry++) {
        unsigned sy_start = (ry * h) / dh;
        unsigned sy_end = ((ry + 1) * h + dh - 1) / dh;
        if (sy_end > h) sy_end = h;
        for (unsigned rx = 0; rx < dw; rx++) {
            unsigned sx_start = (rx * w) / dw;
            unsigned sx_end = ((rx + 1) * w + dw - 1) / dw;
            if (sx_end > w) sx_end = w;
            uint64_t r = 0, g = 0, b = 0, a = 0, count = 0;
            for (unsigned y = sy_start; y < sy_end; y++) {
                for (unsigned x = sx_start; x < sx_end; x++) {
                    size_t si = ((size_t)y * w + x) * 4;
                    r += input[si+0]; g += input[si+1];
                    b += input[si+2]; a += input[si+3];
                    count++;
                }
            }
            size_t di = ((size_t)ry * dw + rx) * 4;
            /* Guard against division by zero (defensive — count >= 1 in
             * current caller paths, but protects against future refactors). */
            if (count == 0) {
                output[di+0] = 0; output[di+1] = 0;
                output[di+2] = 0; output[di+3] = 0;
                continue;
            }
            /* Integer division truncates (discards fractional part). For a
             * box (area-average) filter this is standard and acceptable —
             * the averaged pixel value is bounded, and truncation error
             * (< 1 per channel) is visually negligible at target sizes. */
            output[di+0] = (unsigned char)(r / count);
            output[di+1] = (unsigned char)(g / count);
            output[di+2] = (unsigned char)(b / count);
            output[di+3] = (unsigned char)(a / count);
        }
    }
}

static void resize_rgba(const unsigned char* input, unsigned w, unsigned h,
                        unsigned char** output, unsigned* out_w, unsigned* out_h,
                        unsigned target_width) {
    if (w == 0 || h == 0) { *output = NULL; return; }

    // Fast path: image already small enough — copy unchanged
    if (target_width >= w) {
        *out_w = w;
        *out_h = h;
        size_t size = (size_t)w * (size_t)h * 4;
        *output = malloc(size);
        if (*output) memcpy(*output, input, size);
        return;
    }

    unsigned dw = target_width;
    unsigned dh = (unsigned)((unsigned long long)h * dw / w);
    if (dh < 1) dh = 1;

    size_t out_size = (size_t)dw * (size_t)dh * 4;
    unsigned char* out = malloc(out_size);
    if (!out) { *output = NULL; return; }

    // Delegate to box-filter helper
    box_filter_rgba(input, w, h, out, dw, dh);

    *output = out;
    *out_w = dw;
    *out_h = dh;
}

int downscale_screenshot_base64(const char* input_b64, char* output, size_t out_size, unsigned target_width, unsigned* orig_w, unsigned* orig_h) {
    size_t png_size;
    unsigned char* png_bytes = base64_decode(input_b64, &png_size);
    if (!png_bytes) {
        fprintf(stderr, "[SCREENSHOT] base64 decode failed\n");
        return -1;
    }

    unsigned char* rgba = NULL;
    unsigned w, h;
    unsigned err = lodepng_decode32(&rgba, &w, &h, png_bytes, png_size);
    free(png_bytes);
    if (err) {
        fprintf(stderr, "[SCREENSHOT] PNG decode failed: %u\n", err);
        return -1;
    }

    // Extract original dimensions from validated lodepng output
    if (orig_w && orig_h) {
        *orig_w = w;
        *orig_h = h;
    }

    unsigned char* downscaled = NULL;
    unsigned dw, dh;
    resize_rgba(rgba, w, h, &downscaled, &dw, &dh, target_width);
    free(rgba);
    if (!downscaled) {
        fprintf(stderr, "[SCREENSHOT] downscale failed\n");
        return -1;
    }

    unsigned char* out_png = NULL;
    size_t out_png_size;
    err = lodepng_encode32(&out_png, &out_png_size, downscaled, dw, dh);
    free(downscaled);
    if (err) {
        fprintf(stderr, "[SCREENSHOT] PNG encode failed: %u\n", err);
        return -1;
    }

    size_t written = base64_encode(out_png, out_png_size, output, out_size);
    free(out_png);
    if (written == 0) {
        fprintf(stderr, "[SCREENSHOT] base64 encode failed (buffer too small)\n");
        return -1;
    }

    fprintf(stderr, "[SCREENSHOT] %ux%u -> %ux%u (%zu bytes PNG base64)\n",
            w, h, dw, dh, written);
    return 0;
}
