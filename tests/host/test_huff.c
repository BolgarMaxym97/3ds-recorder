/* Huffman stage (codec/source/huff.c): round trips, edge cases, malformed streams.
   With a recording as the argument: every XR32 frame round-trips, and the sizes are reported. */
#include "huff.h"
#include "codec.h"
#include "r3d_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

static HuffTables g_tables;
static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

static size_t round_trip(const uint8_t *in, size_t n)
{
    const size_t cap = HUFF_HEADER + n * 2 + 16;
    uint8_t *enc = malloc(cap), *dec = malloc(n + 1);
    const size_t e = huff_encode(in, n, enc, cap);
    CHECK(e != 0 && e % 4 == 0);
    CHECK(huff_raw_size(enc, e) == n);
    CHECK(huff_decode(enc, e, dec, n, &g_tables) == 0);
    CHECK(memcmp(in, dec, n) == 0);
    /* a size limit below the result: refused */
    CHECK(huff_encode(in, n, enc, e - 4) == 0);
    free(enc);
    free(dec);
    return e;
}

static void test_edges(void)
{
    uint8_t b[4096];
    round_trip(b, 0);
    b[0] = 7;
    round_trip(b, 1);
    memset(b, 0xAA, sizeof(b));
    CHECK(round_trip(b, sizeof(b)) < HUFF_HEADER + sizeof(b) / 8 + 8);   /* one symbol: 1 bit each */
    for (size_t i = 0; i < sizeof(b); i++)
        b[i] = (uint8_t)i;
    round_trip(b, sizeof(b));
    round_trip(b, 4095);   /* odd */
    /* skewed: lengths past HUFF_MAX_BITS before the limit (Fibonacci-like frequencies) */
    size_t n = 0;
    uint32_t f = 1, g = 1;
    for (int s = 0; s < 20 && n < sizeof(b); s++)
    {
        for (uint32_t k = 0; k < f && n < sizeof(b); k++)
            b[n++] = (uint8_t)s;
        const uint32_t h = f + g;
        g = f;
        f = h;
    }
    round_trip(b, n);
}

/* 256 symbols with a long tail (many codes past HUFF_MAX_BITS before the limit, as in real
   frames; the first limit fix left the code over-full there) */
static void test_long_tail(void)
{
    static uint8_t b[300000];
    size_t n = 0;
    for (uint32_t s = 0; s < 256 && n < sizeof(b); s++)
    {
        const uint32_t c = 1u + (150000u >> (s / 6u));
        for (uint32_t k = 0; k < c && n < sizeof(b); k++)
            b[n++] = (uint8_t)s;
    }
    for (size_t i = n; i-- > 1;)   /* shuffled */
    {
        const size_t j = rnd() % (i + 1);
        const uint8_t t = b[i];
        b[i] = b[j];
        b[j] = t;
    }
    round_trip(b, n);
}

static void test_random(void)
{
    static uint8_t b[200000];
    for (int r = 0; r < 50; r++)
    {
        const size_t n = rnd() % sizeof(b);
        const uint32_t spread = 1 + rnd() % 256;
        for (size_t i = 0; i < n; i++)
            b[i] = (uint8_t)((rnd() % spread) * (i & 1 ? 3 : 1));
        round_trip(b, n);
    }
}

static void test_malformed(void)
{
    static uint8_t b[20000], enc[30000], dec[20000];
    for (size_t i = 0; i < sizeof(b); i++)
        b[i] = (uint8_t)(rnd() % 40);
    const size_t e = huff_encode(b, sizeof(b), enc, sizeof(enc));
    CHECK(e);
    CHECK(huff_decode(enc, e - 4, dec, sizeof(dec), &g_tables) != 0);   /* cut short */
    CHECK(huff_decode(enc, HUFF_HEADER - 1, dec, sizeof(dec), &g_tables) != 0);
    CHECK(huff_decode(enc, e, dec, sizeof(dec) - 1, &g_tables) != 0);   /* too big for the buffer */
    uint8_t bad[HUFF_HEADER + 64];
    memcpy(bad, enc, HUFF_HEADER);
    memset(bad + 4, 0x11, 256);   /* every symbol 1 bit: not a code */
    CHECK(huff_decode(bad, sizeof(bad), dec, sizeof(dec), &g_tables) != 0);
    /* random damage never crashes or writes past the buffer */
    for (int r = 0; r < 2000; r++)
    {
        static uint8_t d[30000];
        memcpy(d, enc, e);
        for (int k = 0; k < 1 + (int)(rnd() % 8); k++)
            d[rnd() % e] ^= (uint8_t)(1 + rnd() % 255);
        (void)huff_decode(d, e, dec, sizeof(dec), &g_tables);
    }
}

/* Every XR32 frame of a recording: round trip and sizes. */
static void test_recording(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        fprintf(stderr, "cannot open %s\n", path);
        g_fail = 1;
        return;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz)
        g_fail = 1;
    fclose(f);
    static uint8_t enc[400000], dec[400000];
    static uint16_t picA[2][400 * 240], picB[2][400 * 240];   /* XR32 decoder vs the fused one */
    uint32_t pictures = 0;
    uint64_t raw[2] = { 0 }, out[2] = { 0 };
    uint32_t frames = 0, kept = 0;
    double encS = 0, decS = 0;
    long p = 32;
    while (p + 16 <= sz)
    {
        R3dRecHeader h;
        memcpy(&h, d + p, 16);
        if (h.magic != R3D_MAGIC_YFRAME && h.magic != R3D_MAGIC_PAD && h.magic != R3D_MAGIC_AUDIO &&
            h.magic != R3D_MAGIC_END && h.magic != R3D_MAGIC_DSPMEM && h.magic != R3D_MAGIC_XFRAME)
            break;
        const long stored = h.magic == R3D_MAGIC_PAD ? (long)h.len : (long)((h.len + 15u) & ~15u);
        if (h.magic == R3D_MAGIC_YFRAME && p + 16 + (long)h.len <= sz)
        {
            const int sc = h.screen & 1;
            const uint8_t *in = d + p + 16;
            clock_t t0 = clock();
            const size_t e = huff_encode(in, h.len, enc, h.len);
            encS += (double)(clock() - t0) / CLOCKS_PER_SEC;
            raw[sc] += h.len;
            frames++;
            const int key = (h.screen & R3D_SCREEN_KEY) != 0;
            const size_t words = (size_t)h.cols * 240u;
            if (words <= 400u * 240u && codec_xr32_decode(in, h.len, picA[sc], words, key) == 0)
            {
                const int rc = e ? huff_xr32_decode(enc, e, picB[sc], words, key, &g_tables)
                                 : codec_xr32_decode(in, h.len, picB[sc], words, key);
                CHECK(rc == 0);
                CHECK(memcmp(picA[sc], picB[sc], words * 2) == 0);
                pictures++;
            }
            if (e)
            {
                t0 = clock();
                CHECK(huff_decode(enc, e, dec, sizeof(dec), &g_tables) == 0);
                decS += (double)(clock() - t0) / CLOCKS_PER_SEC;
                CHECK(memcmp(dec, in, h.len) == 0);
                out[sc] += e;
            }
            else
            {
                out[sc] += h.len;
                kept++;
            }
        }
        p += 16 + stored;
    }
    free(d);
    CHECK(pictures > frames / 2);
    printf("%s: %u frames (%u kept as XR32), top %.1f -> %.1f MB (%.0f%%), bottom %.1f -> %.1f MB (%.0f%%), "
           "host encode %.2f ms, decode %.2f ms per frame\n",
           strrchr(path, '/') ? strrchr(path, '/') + 1 : path, frames, kept, raw[0] / 1e6, out[0] / 1e6,
           raw[0] ? 100.0 * out[0] / raw[0] : 0, raw[1] / 1e6, out[1] / 1e6, raw[1] ? 100.0 * out[1] / raw[1] : 0,
           frames ? encS * 1000 / frames : 0, frames ? decS * 1000 / frames : 0);
}

int main(int argc, char **argv)
{
    test_edges();
    test_long_tail();
    test_random();
    test_malformed();
    for (int i = 1; i < argc; i++)
        test_recording(argv[i]);
    if (g_fail)
        return 1;
    printf("test_huff: ok\n");
    return 0;
}
