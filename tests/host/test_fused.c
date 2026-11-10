/* codec_xr32_encode_conv (a column converted at a time) against converting the whole frame first
   (fb_to_rgb565) and encoding that: the same bytes and the same reference frame afterwards. */
#include "codec.h"
#include "convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
static uint32_t g_rng = 7;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

int main(void)
{
    enum { COLS = 400, ROWS = 240 };
    static const uint32_t fmts[5] = { FB_RGBA8, FB_BGR8, FB_RGB565, FB_RGB5A1, FB_RGBA4 };
    static const uint32_t bpp[5] = { 4, 3, 2, 2, 2 };
    static uint8_t fb[COLS * 256 * 4];
    static uint16_t conv[COLS * ROWS], prevA[COLS * ROWS], prevB[COLS * ROWS];
    static uint8_t outA[COLS * ROWS * 2 + 64], outB[COLS * ROWS * 2 + 64];
    for (int f = 0; f < 5; f++) {
        const uint32_t stride = ROWS * bpp[f] + (f == 2 ? 32 : 0);   /* RGB565 with padded columns too */
        memset(fb, 0, sizeof(fb));
        for (int t = 0; t < 40; t++) {
            /* change a random part of the frame (none, some, all) */
            const uint32_t span = t % 4 == 0 ? 0 : t % 4 == 1 ? 50 : t % 4 == 2 ? 4000 : sizeof(fb);
            const uint32_t at = rnd() % sizeof(fb);
            for (uint32_t i = 0; i < span; i++)
                fb[(at + i) % sizeof(fb)] = (uint8_t)rnd();
            const int key = t % 10 == 0;
            if (!fb_to_rgb565(conv, fb, fmts[f] | 0x80300u, COLS, ROWS, stride, 1)) {
                g_fail++;
                continue;
            }
            const size_t a = codec_xr32_encode_strided_v(prevA, (const uint8_t *)conv, COLS, ROWS, ROWS * 2, 1, key, 1, outA);
            const size_t b = codec_xr32_encode_conv(prevB, fb, COLS, ROWS, stride, fmts[f] | 0x80300u,
                                                    fb_column_to_rgb565, key, outB);
            if (a != b || memcmp(outA, outB, a) != 0 || memcmp(prevA, prevB, sizeof(prevA)) != 0) {
                fprintf(stderr, "format %u frame %d: whole frame %zu bytes, per column %zu\n", fmts[f], t, a, b);
                g_fail++;
            }
        }
    }
    if (g_fail) {
        printf("test_fused: %d failures\n", g_fail);
        return 1;
    }
    printf("test_fused: ok\n");
    return 0;
}
