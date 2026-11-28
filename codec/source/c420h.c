/*
 * C420H: a C420 frame (codec.h) made smaller without changing a bit of it. Each of its numbers is
 * predicted from its neighbours (MED, as JPEG-LS: below, left, below-left), and the difference is
 * written in a Huffman code from fixed tables (trained on O3DS recordings of five games,
 * 2026-11-28: 41.7% of C420's bytes). No table is made per frame and no pass counts anything: one
 * pass over the C420 words, the tables (32 and 64 entries) stay in the cache.
 */
#include "codec.h"

#include <string.h>

/* code lengths: Y differences (mod 32) and Co / Cg differences (mod 64) */
static const uint8_t LEN_Y[32] = {1, 2, 5, 7, 7, 8, 9, 9, 9, 10, 10, 11, 11, 11, 11, 11,
                                  11, 11, 11, 11, 11, 10, 10, 10, 9, 9, 8, 8, 7, 7, 5, 3};
static const uint8_t LEN_C[64] = {1, 3, 5, 7, 7, 8, 9, 9, 12, 12, 12, 12, 12, 12, 12, 12,
                                  12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
                                  12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
                                  12, 12, 12, 12, 12, 12, 12, 12, 9, 9, 8, 8, 7, 7, 5, 2};
#define MAXLEN 12u

/* code << 8 | length, canonical (shorter codes first, then by symbol) */
static uint32_t g_codeY[32], g_codeC[64];
/* decoding: the next 12 bits -> symbol << 8 | length */
static uint16_t g_lutY[1u << MAXLEN], g_lutC[1u << MAXLEN];
static int g_ready;

static void canonical(const uint8_t *len, uint32_t n, uint32_t *code, uint16_t *lut)
{
    uint32_t next = 0;
    for (uint32_t l = 1; l <= MAXLEN; l++) {
        for (uint32_t s = 0; s < n; s++)
            if (len[s] == l) {
                code[s] = next << 8 | l;
                const uint32_t first = next << (MAXLEN - l), count = 1u << (MAXLEN - l);
                for (uint32_t k = 0; k < count; k++)
                    lut[first + k] = (uint16_t)(s << 8 | l);
                next++;
            }
        next <<= 1;
    }
}

static void tables(void)
{
    canonical(LEN_Y, 32, g_codeY, g_lutY);
    canonical(LEN_C, 64, g_codeC, g_lutC);
    g_ready = 1;
}

static inline uint32_t med(uint32_t a, uint32_t b, uint32_t c)
{
    const uint32_t mx = a > b ? a : b, mn = a > b ? b : a;
    return c >= mx ? mn : c <= mn ? mx : a + b - c;
}

/* The neighbours of the planes (column-major, row 0 the bottom one; outside the frame 0):
     Y:      cols x rows, pixel (c, r); left = (c - 1, r), below = (c, r - 1)
     Co, Cg: one value per 2 x 2 block, (c / 2, r / 2)
   lastY holds column c - 1 (the odd column of the pair before), lastCo / lastCg the pair before;
   the new values go to the other buffers, swapped after each column pair. */
typedef struct {
    uint8_t y[2][256], co[2][128], cg[2][128];
} Edges;

size_t codec_c420h_encode(const uint8_t *in, uint32_t cols, uint32_t rows, uint8_t *out, size_t cap)
{
    if (!g_ready)
        tables();
    if ((cols | rows) & 1u || rows > 256 || cap < 4)
        return 0;
    Edges e;
    memset(&e, 0, sizeof(e));
    uint32_t acc = 0, nbits = 0;
    size_t o = 0;
    const size_t stop = cap - 4;   /* room for the last bits */
#define PUT(cv)                                                                \
    do {                                                                       \
        const uint32_t cv_ = (cv), l_ = cv_ & 255u;                            \
        acc = acc << l_ | cv_ >> 8;                                            \
        nbits += l_;                                                           \
        if (nbits >= 16) {                                                     \
            nbits -= 16;                                                       \
            out[o] = (uint8_t)(acc >> (nbits + 8));                            \
            out[o + 1] = (uint8_t)(acc >> nbits);                              \
            o += 2;                                                            \
        }                                                                      \
    } while (0)
    int cur = 0;
    for (uint32_t c = 0; c < cols; c += 2, cur ^= 1) {
        const uint8_t *ly = e.y[cur], *lco = e.co[cur], *lcg = e.cg[cur];
        uint8_t *ny = e.y[cur ^ 1], *nco = e.co[cur ^ 1], *ncg = e.cg[cur ^ 1];
        uint32_t b0 = 0, b1 = 0, bco = 0, bcg = 0;   /* below: (c, r - 1), (c + 1, r - 1), the block below */
        uint32_t lb = 0, lbc = 0;                    /* left-below: (c - 1, r - 1), the block left-below */
        for (uint32_t r = 0; r < rows; r += 2, in += 4) {
            if (o > stop)
                return 0;
            const uint32_t w = in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
            const uint32_t y0 = w & 31u, y1 = (w >> 5) & 31u, y2 = (w >> 10) & 31u, y3 = (w >> 15) & 31u;
            const uint32_t co = (w >> 20) & 63u, cg = w >> 26;
            const uint32_t l0 = ly[r], l1 = ly[r + 1];
            PUT(g_codeY[(y0 - med(b0, l0, lb)) & 31u]);   /* (c, r) */
            PUT(g_codeY[(y1 - med(y0, l1, l0)) & 31u]);   /* (c, r + 1) */
            PUT(g_codeY[(y2 - med(b1, y0, b0)) & 31u]);   /* (c + 1, r) */
            PUT(g_codeY[(y3 - med(y2, y1, y0)) & 31u]);   /* (c + 1, r + 1) */
            const uint32_t lc = lco[r / 2], lg = lcg[r / 2];
            PUT(g_codeC[(co - med(bco, lc, lbc & 255u)) & 63u]);
            PUT(g_codeC[(cg - med(bcg, lg, lbc >> 8)) & 63u]);
            ny[r] = (uint8_t)y2, ny[r + 1] = (uint8_t)y3;
            nco[r / 2] = (uint8_t)co, ncg[r / 2] = (uint8_t)cg;
            b0 = y1, b1 = y3, bco = co, bcg = cg;
            lb = l1, lbc = lc | lg << 8;
        }
    }
#undef PUT
    if (nbits)
        acc <<= 16 - nbits;   /* the last bits, zeros after them */
    if (nbits > 0)
        out[o++] = (uint8_t)(acc >> 8);
    if (nbits > 8)
        out[o++] = (uint8_t)acc;
    return o;
}

int codec_c420h_decode(const uint8_t *in, size_t in_size, uint8_t *frame, uint32_t cols, uint32_t rows)
{
    if (!g_ready)
        tables();
    if ((cols | rows) & 1u || rows > 256)
        return -1;
    Edges e;
    memset(&e, 0, sizeof(e));
    uint32_t acc = 0, nbits = 0;
    size_t i = 0;
#define GET(lut, sym)                                                         \
    do {                                                                      \
        while (nbits < MAXLEN) {                                              \
            acc = acc << 8 | (i < in_size ? in[i] : 0u);                      \
            i++;                                                              \
            nbits += 8;                                                       \
        }                                                                     \
        const uint32_t e_ = (lut)[(acc >> (nbits - MAXLEN)) & ((1u << MAXLEN) - 1u)]; \
        nbits -= e_ & 255u;                                                   \
        sym = e_ >> 8;                                                        \
    } while (0)
    int cur = 0;
    uint8_t *o = frame;
    for (uint32_t c = 0; c < cols; c += 2, cur ^= 1) {
        const uint8_t *ly = e.y[cur], *lco = e.co[cur], *lcg = e.cg[cur];
        uint8_t *ny = e.y[cur ^ 1], *nco = e.co[cur ^ 1], *ncg = e.cg[cur ^ 1];
        uint32_t b0 = 0, b1 = 0, bco = 0, bcg = 0, lb = 0, lbc = 0;
        for (uint32_t r = 0; r < rows; r += 2, o += 4) {
            uint32_t d;
            const uint32_t l0 = ly[r], l1 = ly[r + 1];
            GET(g_lutY, d);
            const uint32_t y0 = (d + med(b0, l0, lb)) & 31u;
            GET(g_lutY, d);
            const uint32_t y1 = (d + med(y0, l1, l0)) & 31u;
            GET(g_lutY, d);
            const uint32_t y2 = (d + med(b1, y0, b0)) & 31u;
            GET(g_lutY, d);
            const uint32_t y3 = (d + med(y2, y1, y0)) & 31u;
            const uint32_t lc = lco[r / 2], lg = lcg[r / 2];
            GET(g_lutC, d);
            const uint32_t co = (d + med(bco, lc, lbc & 255u)) & 63u;
            GET(g_lutC, d);
            const uint32_t cg = (d + med(bcg, lg, lbc >> 8)) & 63u;
            const uint32_t w = y0 | y1 << 5 | y2 << 10 | y3 << 15 | co << 20 | cg << 26;
            o[0] = (uint8_t)w, o[1] = (uint8_t)(w >> 8), o[2] = (uint8_t)(w >> 16), o[3] = (uint8_t)(w >> 24);
            ny[r] = (uint8_t)y2, ny[r + 1] = (uint8_t)y3;
            nco[r / 2] = (uint8_t)co, ncg[r / 2] = (uint8_t)cg;
            b0 = y1, b1 = y3, bco = co, bcg = cg;
            lb = l1, lbc = lc | lg << 8;
        }
    }
#undef GET
    return i > in_size + 4 ? -1 : 0;   /* read past the end: damaged */
}
