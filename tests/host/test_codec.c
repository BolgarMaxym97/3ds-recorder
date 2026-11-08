/*
 * Host unit tests for the Lite codecs: round trips and size bounds.
 */
#include "codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define COLS 40
#define ROWS 24
#define N (COLS * ROWS)

static uint32_t rng = 12345;
static uint16_t rnd16(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (uint16_t)rng;
}

static uint16_t prev[N], cur[N], frame[N];
static uint8_t buf[N * 2 + 64];

static void xrle_roundtrip(const uint16_t *p, const uint16_t *c, const char *name)
{
    size_t sz = codec_xrle_encode(p, c, N, buf);
    CHECK(sz <= codec_xrle_max_size(N));
    if (p) {
        memcpy(frame, p, sizeof(frame));
    } else {
        memset(frame, 0xEE, sizeof(frame));
    }
    int rc = codec_xrle_decode(buf, sz, frame, N, p == NULL);
    if (rc != 0 || memcmp(frame, c, sizeof(frame)) != 0) {
        fprintf(stderr, "FAIL xrle round trip: %s\n", name);
        failures++;
    }
}

static void tiles_roundtrip(const uint16_t *p, const uint16_t *c, const char *name)
{
    size_t sz = codec_tiles_encode(p, c, COLS, ROWS, buf);
    CHECK(sz <= codec_tiles_max_size(COLS, ROWS));
    if (p) {
        memcpy(frame, p, sizeof(frame));
    } else {
        memset(frame, 0xEE, sizeof(frame));
    }
    int rc = codec_tiles_decode(buf, sz, frame, COLS, ROWS);
    if (rc != 0 || memcmp(frame, c, sizeof(frame)) != 0) {
        fprintf(stderr, "FAIL tiles round trip: %s\n", name);
        failures++;
    }
}

/* Straightforward XR32 reference: pairs over the packed frame, literal ends at the first zero pair. */
static size_t xr32_reference(const uint16_t *p, const uint16_t *c, size_t n, uint8_t *out)
{
    uint8_t *o = out;
    size_t i = 0;
    const size_t pairs = n / 2;
#define PAIRX(k) ((uint32_t)(c[2 * (k)] ^ (p ? p[2 * (k)] : 0)) | (uint32_t)(c[2 * (k) + 1] ^ (p ? p[2 * (k) + 1] : 0)) << 16)
    while (i < pairs) {
        size_t run = 0;
        if (PAIRX(i) == 0) {
            while (i + run < pairs && run < 0x8000 && PAIRX(i + run) == 0) {
                run++;
            }
            o[0] = (uint8_t)(run - 1);
            o[1] = (uint8_t)((run - 1) >> 8);
            o += 2;
        } else {
            while (i + run < pairs && run < 0x8000 && PAIRX(i + run) != 0) {
                run++;
            }
            o[0] = (uint8_t)(run - 1);
            o[1] = (uint8_t)(0x80 | ((run - 1) >> 8));
            o += 2;
            for (size_t k = i; k < i + run; k++) {
                const uint32_t x = PAIRX(k);
                o[0] = (uint8_t)x;
                o[1] = (uint8_t)(x >> 8);
                o[2] = (uint8_t)(x >> 16);
                o[3] = (uint8_t)(x >> 24);
                o += 4;
            }
        }
        i += run;
    }
#undef PAIRX
    return (size_t)(o - out);
}

static void both(const uint16_t *p, const uint16_t *c, const char *name)
{
    xrle_roundtrip(p, c, name);
    tiles_roundtrip(p, c, name);
}

int main(void)
{
    for (int i = 0; i < N; i++) {
        prev[i] = rnd16();
    }

    /* identical frame: XOR-RLE is one token per 32768 words, tiles = bitmap only */
    memcpy(cur, prev, sizeof(cur));
    both(prev, cur, "identical");
    CHECK(codec_xrle_encode(prev, cur, N, buf) == 2);
    CHECK(codec_tiles_encode(prev, cur, COLS, ROWS, buf) == (size_t)((COLS / 8) * (ROWS / 8) + 7) / 8);

    /* keyframes: random, all zero, all non-zero */
    for (int i = 0; i < N; i++) {
        cur[i] = rnd16();
    }
    both(NULL, cur, "keyframe random");
    memset(cur, 0, sizeof(cur));
    both(NULL, cur, "keyframe black");
    for (int i = 0; i < N; i++) {
        cur[i] = 0xFFFF;
    }
    both(NULL, cur, "keyframe white");

    /* sparse changes, including single changed words between zeros and at both ends */
    memcpy(cur, prev, sizeof(cur));
    cur[0] ^= 1;
    cur[5] ^= 0x8000;
    cur[7] ^= 3;
    cur[N - 1] ^= 0x1234;
    for (int i = 300; i < 340; i++) {
        cur[i] ^= rnd16() | 1;
    }
    both(prev, cur, "sparse");

    /* alternating zero / non-zero XOR words (single zeros stay literal) */
    memcpy(cur, prev, sizeof(cur));
    for (int i = 0; i < N; i += 2) {
        cur[i] ^= 0x0F0F;
    }
    both(prev, cur, "alternating");

    /* everything changes: worst case */
    for (int i = 0; i < N; i++) {
        cur[i] = (uint16_t)(prev[i] ^ (rnd16() | 1));
    }
    both(prev, cur, "all changed");

    /* long runs beyond the 32768-word token limit */
    {
        enum { BIG = 70000 };
        static uint16_t a[BIG], b[BIG], f[BIG];
        static uint8_t out[BIG * 2 + 64];
        memset(a, 0, sizeof(a));
        memcpy(b, a, sizeof(b));
        b[BIG - 1] = 7;
        size_t sz = codec_xrle_encode(a, b, BIG, out);
        memcpy(f, a, sizeof(f));
        CHECK(codec_xrle_decode(out, sz, f, BIG, 0) == 0);
        CHECK(memcmp(f, b, sizeof(f)) == 0);
        for (int i = 0; i < BIG; i++) {
            b[i] = (uint16_t)(i | 1);
        }
        sz = codec_xrle_encode(NULL, b, BIG, out);
        CHECK(sz <= codec_xrle_max_size(BIG));
        CHECK(codec_xrle_decode(out, sz, f, BIG, 1) == 0);
        CHECK(memcmp(f, b, sizeof(f)) == 0);
    }

    /* fused strided encoder vs the reference, on real frame sizes */
    {
        enum { SC = 400, SR = 240, STRIDE = 512 };          /* top screen, 32 bytes of padding per column */
        static uint8_t src[SC * STRIDE];
        static uint16_t packed_prev[SC * SR], packed_cur[SC * SR], fprev[SC * SR], dec[SC * SR];
        static uint8_t out_ref[SC * SR * 2 + 64], out_fused[SC * SR * 2 + 64];
        static uint16_t xprev[SC * SR] __attribute__((aligned(4)));
        static uint8_t out_x32[SC * SR * 2 + 64], out_x32ref[SC * SR * 2 + 64];
        const int patterns = 6;
        for (int step = 1; step <= 2; step++) {
            const uint32_t oc = SC / step, orow = SR / step, n = oc * orow;
            memset(fprev, 0, sizeof(fprev));
            memset(packed_prev, 0, sizeof(packed_prev));
            memset(xprev, 0x5A, sizeof(xprev));   /* garbage: the first frame is a keyframe */
            for (int fr = 0; fr < patterns * 2; fr++) {
                const int key = fr % patterns == 0;
                /* build the source frame (with garbage in the stride padding) */
                for (uint32_t c = 0; c < SC; c++) {
                    uint16_t *col = (uint16_t *)(src + c * STRIDE);
                    for (uint32_t r = 0; r < STRIDE / 2; r++) {
                        uint16_t v;
                        switch (fr % patterns) {
                        case 0: v = rnd16(); break;                                   /* noise keyframe */
                        case 1: v = col[r]; break;                                    /* identical */
                        case 2: v = (r % 50 == 0 && c % 3 == 0) ? (uint16_t)(col[r] ^ 1) : col[r]; break; /* sparse */
                        case 3: v = (uint16_t)(col[r] ^ ((r & 1) ? 0 : 0x0F0F)); break;  /* alternating */
                        case 4: v = c < 100 ? 0 : col[r]; break;                       /* big black area */
                        default: v = (uint16_t)(col[r] ^ (rnd16() | 1)); break;        /* all changed */
                        }
                        col[r] = r < SR ? v : (uint16_t)0xDEAD;
                    }
                }
                for (uint32_t c = 0; c < oc; c++) {
                    const uint16_t *col = (const uint16_t *)(src + (size_t)c * step * STRIDE);
                    for (uint32_t r = 0; r < orow; r++) {
                        packed_cur[c * orow + r] = col[r * step];
                    }
                }
                size_t ref = codec_xrle_encode(key ? NULL : packed_prev, packed_cur, n, out_ref);
                size_t fsz = codec_xrle_encode_strided(fprev, src, SC, SR, STRIDE, (uint32_t)step, key, out_fused);
                CHECK(fsz <= codec_xrle_max_size(n));
                memcpy(dec, packed_prev, n * 2);
                if (codec_xrle_decode(out_fused, fsz, dec, n, key) != 0 || memcmp(dec, packed_cur, n * 2) != 0) {
                    fprintf(stderr, "FAIL fused decode: step %d frame %d\n", step, fr);
                    failures++;
                }
                if (memcmp(fprev, packed_cur, n * 2) != 0) {
                    fprintf(stderr, "FAIL fused prev update: step %d frame %d\n", step, fr);
                    failures++;
                }
                if (fsz > ref + 8 || fsz + 8 < ref) {
                    fprintf(stderr, "FAIL fused size %zu vs reference %zu: step %d frame %d\n", fsz, ref, step, fr);
                    failures++;
                }

                /* XR32 memory-hint variants: same stream as variant 0 */
                for (int v = 1; v < CODEC_XR32_VARIANTS; v++) {
                    static uint16_t vprev[SC * SR] __attribute__((aligned(4)));
                    static uint8_t vout[SC * SR * 2 + 64];
                    memcpy(vprev, xprev, sizeof(vprev));
                    size_t a = codec_xr32_encode_strided_v(vprev, src, SC, SR, STRIDE, (uint32_t)step, key, v, vout);
                    static uint16_t v0prev[SC * SR] __attribute__((aligned(4)));
                    memcpy(v0prev, xprev, sizeof(v0prev));
                    size_t b = codec_xr32_encode_strided_v(v0prev, src, SC, SR, STRIDE, (uint32_t)step, key, 0, out_x32);
                    if (a != b || memcmp(vout, out_x32, a) != 0 || memcmp(vprev, v0prev, n * 2) != 0) {
                        fprintf(stderr, "FAIL xr32 variant %d differs: step %d frame %d\n", v, step, fr);
                        failures++;
                    }
                }
                /* XR32: must equal the reference byte for byte and keep prev in sync */
                size_t xsz = codec_xr32_encode_strided(xprev, src, SC, SR, STRIDE, (uint32_t)step, key, out_x32);
                size_t xref = xr32_reference(key ? NULL : packed_prev, packed_cur, n, out_x32ref);
                CHECK(xsz <= codec_xr32_max_size(n));
                if (xsz != xref || memcmp(out_x32, out_x32ref, xsz) != 0) {
                    fprintf(stderr, "FAIL xr32 stream %zu vs reference %zu: step %d frame %d\n", xsz, xref, step, fr);
                    failures++;
                }
                if (key) {
                    memset(dec, 0xEE, n * 2);
                } else {
                    memcpy(dec, packed_prev, n * 2);
                }
                if (codec_xr32_decode(out_x32, xsz, dec, n, key) != 0 || memcmp(dec, packed_cur, n * 2) != 0) {
                    fprintf(stderr, "FAIL xr32 decode: step %d frame %d\n", step, fr);
                    failures++;
                }
                if (memcmp(xprev, packed_cur, n * 2) != 0) {
                    fprintf(stderr, "FAIL xr32 prev update: step %d frame %d\n", step, fr);
                    failures++;
                }
                memcpy(packed_prev, packed_cur, n * 2);
            }
        }
    }

    /* malformed streams are rejected */
    memcpy(cur, prev, sizeof(cur));
    cur[10] ^= 1;
    size_t sz = codec_xrle_encode(prev, cur, N, buf);
    CHECK(codec_xrle_decode(buf, sz - 1, frame, N, 0) == -1);
    sz = codec_xr32_encode_strided(frame, (const uint8_t *)cur, COLS, ROWS, ROWS * 2, 1, 1, buf);
    CHECK(codec_xr32_decode(buf, sz - 2, frame, N, 1) == -1);
    sz = codec_tiles_encode(prev, cur, COLS, ROWS, buf);
    CHECK(codec_tiles_decode(buf, sz - 2, frame, COLS, ROWS) == -1);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("codec host tests: all passed\n");
    return EXIT_SUCCESS;
}
