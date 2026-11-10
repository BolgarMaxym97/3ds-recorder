/*
 * codec_q565_encode_arm (codec/source/q565_arm.s) against codec_q565_encode (C), byte for byte,
 * on synthetic frames and on every frame of the recordings given. Runs on an ARM CPU:
 *   tests/arm/run.sh [recording.r3d ...]   (Docker, linux/arm/v7 under emulation)
 */
#include "codec.h"
#include "r3d_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t codec_q565_encode_arm(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint8_t *out,
                             size_t cap);

static int g_fail;
static uint32_t g_rng = 99;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static uint8_t g_a[400 * 240 * 3 + 64], g_b[400 * 240 * 3 + 64];

static void same(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, size_t cap, const char *what)
{
    memset(g_a, 0xAA, sizeof(g_a));
    memset(g_b, 0x55, sizeof(g_b));
    const size_t a = codec_q565_encode(src, cols, rows, stride, g_a, cap);
    const size_t b = codec_q565_encode_arm(src, cols, rows, stride, g_b, cap);
    if (a != b || memcmp(g_a, g_b, a) != 0) {
        if (g_fail++ < 10)
            fprintf(stderr, "%s: C %zu bytes, ARM %zu bytes, cap %zu\n", what, a, b, cap);
    }
    /* nothing written past the limit */
    for (size_t i = cap; i < cap + 16 && i < sizeof(g_b); i++)
        if (g_b[i] != 0x55 && g_fail++ < 10)
            fprintf(stderr, "%s: ARM wrote past cap %zu\n", what, cap);
}

static void synthetic(void)
{
    static uint16_t f[400 * 256];
    const uint32_t strides[2] = { 480, 512 };
    for (int t = 0; t < 300; t++) {
        const int kind = t % 5;
        for (uint32_t i = 0; i < 400u * 256u; i++) {
            const uint32_t r = rnd();
            f[i] = kind == 0 ? (uint16_t)r
                 : kind == 1 ? 0
                 : kind == 2 ? (uint16_t)(i / 300 * 0x0821u)
                 : kind == 3 ? (uint16_t)(0x1234u * (r % 6))
                             : (uint16_t)((r & 7) ? f[i ? i - 1 : 0] + (r >> 8) % 5 * 0x0841u : r);
        }
        const uint32_t stride = strides[t & 1], cols = t % 7 == 0 ? 1 + rnd() % 400 : 400;
        same((const uint8_t *)f, cols, 240, stride, 400 * 240 * 3 + 16, "synthetic");
        same((const uint8_t *)f, cols, 240, stride, rnd() % (400 * 240 * 2), "synthetic, small cap");
        same((const uint8_t *)f, cols, 240, stride, rnd() % 64, "synthetic, tiny cap");
    }
}

static void recording(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        g_fail++;
        return;
    }
    static uint16_t pic[2][400 * 240];
    static uint8_t in[1 << 20];
    R3dFileHeader fh;
    uint32_t frames = 0;
    if (fread(&fh, 1, sizeof(fh), f) != sizeof(fh))
        return;
    R3dRecHeader h;
    while (fread(&h, 1, sizeof(h), f) == sizeof(h)) {
        const int delta = h.magic == R3D_MAGIC_YFRAME || h.magic == R3D_MAGIC_XFRAME;
        const uint32_t stored = delta ? (h.len + 15u) & ~15u : h.len;
        if (stored > sizeof(in) || fread(in, 1, stored, f) != stored || h.magic == R3D_MAGIC_END)
            break;
        if (h.magic != R3D_MAGIC_YFRAME)
            continue;
        const int sc = h.screen & 1;
        const uint32_t n = h.cols * 240u;
        if (n > 400u * 240u || codec_xr32_decode(in, h.len, pic[sc], n, (h.screen & R3D_SCREEN_KEY) != 0))
            break;
        same((const uint8_t *)pic[sc], h.cols, 240, 480, codec_xr32_max_size(n), "recording frame");
        frames++;
    }
    fclose(f);
    printf("%s: %u frames compared\n", path, frames);
}

int main(int argc, char **argv)
{
    synthetic();
    for (int i = 1; i < argc; i++)
        recording(argv[i]);
    if (g_fail) {
        printf("test_q565_arm: %d mismatches\n", g_fail);
        return 1;
    }
    printf("test_q565_arm: ok\n");
    return 0;
}
