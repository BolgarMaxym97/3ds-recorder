/*
 * fb_column_bgr8_arm (codec/source/convert_arm.s) against fb_column_to_rgb565 (C, FB_BGR8), pixel for
 * pixel, and XR32 frames encoded through either (codec_xr32_encode_conv, as the recorder does), on
 * synthetic BGR8 columns: noise, black and white, every byte value in every channel. Runs on an ARM CPU:
 *   tests/arm/run.sh   (Docker, linux/arm/v7 under emulation)
 */
#include "codec.h"
#include "convert.h"

#include <stdio.h>
#include <string.h>

void fb_column_bgr8_arm(uint16_t *dst, const uint8_t *src, uint32_t rows);

static int g_fail;
static uint32_t g_rng = 5;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

#define MAX_ROWS 256u
static uint8_t g_src[400u * MAX_ROWS * 3u + 64u] __attribute__((aligned(32)));
static uint16_t g_a[MAX_ROWS + 16], g_b[MAX_ROWS + 16];

static int conv_arm(uint16_t *dst, const uint8_t *src, uint32_t fmt, uint32_t rows)
{
    (void)fmt;
    fb_column_bgr8_arm(dst, src, rows);
    return 1;
}

static void columns(void)
{
    for (int t = 0; t < 4000; t++) {
        const uint32_t rows = t % 3 == 0 ? 4u * (rnd() % (MAX_ROWS / 4u + 1u)) : 240u;
        const int kind = t % 4;
        for (uint32_t i = 0; i < rows * 3u; i++)
            g_src[i] = kind == 0 ? (uint8_t)rnd() : kind == 1 ? (rnd() & 1 ? 255 : 0) : kind == 2 ? (uint8_t)(i + t) : 255;
        memset(g_a, 0xAA, sizeof(g_a));
        memset(g_b, 0x55, sizeof(g_b));
        fb_column_to_rgb565(g_a, g_src, FB_BGR8, rows);
        fb_column_bgr8_arm(g_b, g_src, rows);
        if (memcmp(g_a, g_b, rows * 2u) != 0 && g_fail++ < 10)
            fprintf(stderr, "column of %u rows (kind %d): ARM differs from C\n", rows, kind);
        for (uint32_t i = rows; i < rows + 8u; i++)
            if (g_b[i] != 0x5555u && g_fail++ < 10)
                fprintf(stderr, "column of %u rows: ARM wrote past it\n", rows);
    }
}

/* whole frames through the XR32 encoder, as the recorder codes BGR8 pictures */
static void frames(void)
{
    static uint16_t prevA[400 * 240], prevB[400 * 240];
    static uint8_t outA[400 * 240 * 2 + 1024], outB[400 * 240 * 2 + 1024];
    const uint32_t cols = 400, rows = 240, stride = rows * 3u;
    for (int f = 0; f < 40; f++) {
        for (uint32_t i = 0; i < cols * stride; i++)
            g_src[i] = f % 4 == 0 ? (uint8_t)rnd() : (rnd() & 15) == 0 ? (uint8_t)rnd() : g_src[i];   /* a few changes */
        const int key = f % 10 == 0;
        const size_t a = codec_xr32_encode_conv(prevA, g_src, cols, rows, stride, FB_BGR8, fb_column_to_rgb565, key, outA);
        const size_t b = codec_xr32_encode_conv(prevB, g_src, cols, rows, stride, FB_BGR8, conv_arm, key, outB);
        if ((a != b || memcmp(outA, outB, a) != 0 || memcmp(prevA, prevB, sizeof(prevA)) != 0) && g_fail++ < 10)
            fprintf(stderr, "XR32 frame %d: through ARM %zu bytes, through C %zu\n", f, b, a);
    }
}

int main(void)
{
    columns();
    frames();
    if (g_fail) {
        printf("test_convert_arm: %d mismatches\n", g_fail);
        return 1;
    }
    printf("test_convert_arm: ok\n");
    return 0;
}
