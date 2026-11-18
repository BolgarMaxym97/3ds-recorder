/*
 * codec_c420_encode_bgr8_arm (codec/source/c420_arm.s) against codec_c420_encode_888 (C, bpp 3),
 * byte for byte, on synthetic BGR8 frames: noise, black and white (the saturated fields), smooth
 * pictures, padded strides, any even or odd column count, rows from 4 to 256. Runs on an ARM CPU:
 *   tests/arm/run.sh   (Docker, linux/arm/v7 under emulation)
 */
#include "codec.h"

#include <stdio.h>
#include <string.h>

size_t codec_c420_encode_bgr8_arm(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint8_t *out);

static int g_fail;
static uint32_t g_rng = 11;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

#define MAX_COLS 400u
#define MAX_ROWS 256u
#define MAX_STRIDE (MAX_ROWS * 3u + 64u)
static uint8_t g_src[MAX_COLS * MAX_STRIDE + 64] __attribute__((aligned(32)));
static uint32_t g_a[MAX_COLS * MAX_ROWS / 4 + 16], g_b[MAX_COLS * MAX_ROWS / 4 + 16];

static void fill(int kind, uint32_t cols, uint32_t stride)
{
    for (uint32_t i = 0; i < cols * stride; i++) {
        const uint32_t r = rnd();
        switch (kind) {
        case 0: g_src[i] = (uint8_t)r; break;                            /* noise */
        case 1: g_src[i] = r & 1 ? 255 : 0; break;                       /* black and white channels */
        case 2: g_src[i] = 255; break;                                   /* white: every field saturated */
        case 3: g_src[i] = r % 3 ? 255 : (uint8_t)r; break;              /* mostly bright */
        case 4: g_src[i] = r % 3 ? 0 : (uint8_t)r; break;                /* mostly dark */
        case 5: g_src[i] = (uint8_t)(i / stride + (i % stride) / 3 + (r & 3)); break;   /* smooth + noise */
        default: {                                                        /* RGB565 expanded, as games draw */
            const uint32_t ch = (i % stride) % 3, v = (i / stride * 3 + (i % stride) / 3) & 0xFF;
            g_src[i] = (uint8_t)(ch == 1 ? v & 0xFC : v & 0xF8);
        }
        }
    }
}

static void same(uint32_t cols, uint32_t rows, uint32_t stride, const char *what)
{
    memset(g_a, 0xAA, sizeof(g_a));
    memset(g_b, 0x55, sizeof(g_b));
    const size_t a = codec_c420_encode_888(g_src, cols, rows, stride, 3, (uint8_t *)g_a);
    const size_t b = codec_c420_encode_bgr8_arm(g_src, cols, rows, stride, (uint8_t *)g_b);
    if (a != b || memcmp(g_a, g_b, a) != 0) {
        if (g_fail++ < 10)
            fprintf(stderr, "%s: cols %u rows %u stride %u: C %zu bytes, ARM %zu bytes\n", what, cols, rows, stride, a, b);
        return;
    }
    /* nothing written past the frame */
    for (size_t i = b; i < b + 16; i++)
        if (((const uint8_t *)g_b)[i] != 0x55 && g_fail++ < 10)
            fprintf(stderr, "%s: ARM wrote past %zu bytes\n", what, b);
}

int main(void)
{
    static const char *const kinds[] = { "noise", "black and white", "white", "bright", "dark", "smooth", "565" };
    for (int t = 0; t < 700; t++) {
        const int kind = t % 7;
        const uint32_t rows = t % 3 == 0 ? 4u * (1u + rnd() % (MAX_ROWS / 4u)) : 240u;
        const uint32_t cols = t % 5 == 0 ? rnd() % (MAX_COLS + 1u) : t & 1 ? 400u : 320u;
        const uint32_t stride = t % 4 == 0 ? rows * 3u + 4u * (rnd() % 16u) : rows * 3u;
        fill(kind, cols, stride);
        same(cols, rows, stride, kinds[kind]);
    }
    if (g_fail) {
        printf("test_c420_arm: %d mismatches\n", g_fail);
        return 1;
    }
    printf("test_c420_arm: ok\n");
    return 0;
}
