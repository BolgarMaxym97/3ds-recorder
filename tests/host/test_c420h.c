/* C420H (codec.h): every C420 frame comes back to the bit. Synthetic frames always; with .r3d
   paths, every C420 frame of those recordings, and the size against C420. */
#include "codec.h"
#include "r3d_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int roundtrip(const uint8_t *c420, uint32_t cols, size_t *size, double *encMs)
{
    static uint8_t enc[400 * 240 * 3], back[400 * 240];
    const double t0 = now_ms();
    const size_t n = codec_c420h_encode(c420, cols, 240, enc, sizeof(enc));
    *encMs += now_ms() - t0;
    *size = n;
    if (!n)
        return 0;
    memset(back, 0xAA, sizeof(back));
    return codec_c420h_decode(enc, n, back, cols, 240) == 0 && memcmp(back, c420, (size_t)cols * 240) == 0;
}

int main(int argc, char **argv)
{
    static uint16_t px[400 * 240];
    static uint8_t c420[400 * 240];
    size_t n;
    double ms = 0;
    /* a gradient, noise, flat */
    for (int kind = 0; kind < 3; kind++) {
        srand(7);
        for (int i = 0; i < 400 * 240; i++)
            px[i] = kind == 0 ? (uint16_t)((i / 240) * 65535 / 400) : kind == 1 ? (uint16_t)rand() : 0x7BEF;
        codec_c420_encode((const uint8_t *)px, 400, 240, 480, c420);
        CHECK(roundtrip(c420, 400, &n, &ms));
        printf("synthetic %d: %zu bytes (%.0f%% of C420)\n", kind, n, 100.0 * n / sizeof(c420));
    }
    /* too small an output: 0, nothing written past it */
    {
        static uint8_t small[1000 + 16];
        memset(small, 0x55, sizeof(small));
        CHECK(codec_c420h_encode(c420, 400, 240, small, 1000) == 0);
        CHECK(small[1000 + 8] == 0x55);
    }
    for (int a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f)
            continue;
        R3dFileHeader fh;
        if (fread(&fh, 1, sizeof(fh), f) != sizeof(fh)) {
            fclose(f);
            continue;
        }
        size_t raw[2] = {0, 0}, packed[2] = {0, 0}, frames[2] = {0, 0}, bad = 0;
        double t[2] = {0, 0};
        static uint8_t buf[1 << 20];
        R3dRecHeader rh;
        while (fread(&rh, 1, sizeof(rh), f) == sizeof(rh) && (rh.magic & 0xFFFFFFu) == 0x443352u) {
            const uint32_t stored = rh.magic == R3D_MAGIC_PAD ? rh.len : (rh.len + 15u) & ~15u;
            if (rh.magic == R3D_MAGIC_CFRAME && rh.len == (uint32_t)rh.cols * 240u && rh.len <= sizeof(buf)) {
                if (fread(buf, 1, stored, f) != stored)
                    break;
                const int sc = rh.screen & 1;
                size_t s;
                if (!roundtrip(buf, rh.cols, &s, &t[sc]))
                    bad++;
                raw[sc] += rh.len, packed[sc] += s ? s : rh.len, frames[sc]++;
            } else if (fseek(f, stored, SEEK_CUR))
                break;
            if (rh.magic == R3D_MAGIC_END)
                break;
        }
        fclose(f);
        CHECK(bad == 0);
        printf("%s: top %zu frames %.1f%% (%.2f ms each here), bottom %zu frames %.1f%%, %zu not the same back\n", argv[a],
               frames[0], raw[0] ? 100.0 * packed[0] / raw[0] : 0, frames[0] ? t[0] / frames[0] : 0, frames[1],
               raw[1] ? 100.0 * packed[1] / raw[1] : 0, bad);
    }
    printf(g_fail ? "test_c420h: FAIL\n" : "test_c420h: ok\n");
    return g_fail;
}
