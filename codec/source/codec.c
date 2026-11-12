#include "codec.h"

#include <string.h>

#define XRLE_MAX_RUN 0x8000u

/* -------------------------------------------------------------------- XOR-RLE */

size_t codec_xrle_max_size(size_t n)
{
    return n * 2 + ((n + XRLE_MAX_RUN - 1) / XRLE_MAX_RUN + 1) * 2;
}

static inline uint16_t xword(const uint16_t *prev, const uint16_t *cur, size_t i)
{
    return prev ? (uint16_t)(cur[i] ^ prev[i]) : cur[i];
}

static inline void put16(uint8_t **p, uint16_t v)
{
    (*p)[0] = (uint8_t)v;
    (*p)[1] = (uint8_t)(v >> 8);
    *p += 2;
}

size_t codec_xrle_encode(const uint16_t *prev, const uint16_t *cur, size_t n, uint8_t *out)
{
    uint8_t *p = out;
    size_t i = 0;

    while (i < n) {
        if (xword(prev, cur, i) == 0) {
            size_t run = 1;
            while (i + run < n && run < XRLE_MAX_RUN && xword(prev, cur, i + run) == 0) {
                run++;
            }
            put16(&p, (uint16_t)(run - 1));
            i += run;
            continue;
        }

        /* Literal run: stop before two consecutive zero words (a zero-run token
           costs the same as one literal zero, so single zeros stay literal). */
        size_t start = i;
        size_t len = 0;
        while (i < n && len < XRLE_MAX_RUN) {
            if (xword(prev, cur, i) == 0 && i + 1 < n && xword(prev, cur, i + 1) == 0) {
                break;
            }
            i++;
            len++;
        }
        put16(&p, (uint16_t)(0x8000u | (len - 1)));
        for (size_t k = start; k < start + len; k++) {
            put16(&p, xword(prev, cur, k));
        }
    }
    return (size_t)(p - out);
}

int codec_xrle_decode(const uint8_t *in, size_t in_size, uint16_t *frame, size_t n, int keyframe)
{
    size_t pos = 0;
    size_t i = 0;

    while (pos + 2 <= in_size) {
        uint16_t t = (uint16_t)(in[pos] | (in[pos + 1] << 8));
        pos += 2;
        size_t count = (size_t)(t & 0x7FFFu) + 1;
        if (i + count > n) {
            return -1;
        }
        if (t & 0x8000u) {
            if (pos + count * 2 > in_size) {
                return -1;
            }
            for (size_t k = 0; k < count; k++, pos += 2) {
                uint16_t v = (uint16_t)(in[pos] | (in[pos + 1] << 8));
                frame[i + k] = keyframe ? v : (uint16_t)(frame[i + k] ^ v);
            }
        } else if (keyframe) {
            memset(frame + i, 0, count * 2);
        }
        i += count;
    }
    return (pos == in_size && i == n) ? 0 : -1;
}

/* ------------------------------------------------------- fused XOR-RLE encoder */

/*
 * One pass over the source: XOR, run detection, output and prev update.
 * Run state is kept across columns, so the stream equals an encode of the
 * packed frame. Literal runs use a deferred zero: a zero inside a literal is
 * written tentatively and taken back if the next word is zero too (that is
 * where codec_xrle_encode would have ended the literal).
 */
typedef struct {
    uint16_t *o;          /* output cursor (u16 tokens and literals) */
    uint16_t *lit_token;  /* token slot of the open literal run, NULL if none */
    uint32_t lit_len;     /* words in the open literal run */
    uint32_t zero_run;    /* words in the open zero run */
    int last_lit_zero;    /* last literal word written was a (tentative) zero */
} xrle_state_t;

static inline void xs_flush_zero(xrle_state_t *s)
{
    if (s->zero_run) {
        *s->o++ = (uint16_t)(s->zero_run - 1);
        s->zero_run = 0;
    }
}

static inline void xs_close_literal(xrle_state_t *s)
{
    if (s->lit_token) {
        *s->lit_token = (uint16_t)(0x8000u | (s->lit_len - 1));
        s->lit_token = NULL;
        s->lit_len = 0;
        s->last_lit_zero = 0;
    }
}

static inline void xs_push(xrle_state_t *s, uint16_t x)
{
    if (x == 0) {
        if (s->lit_token) {
            if (s->last_lit_zero) {
                /* Two zeros in a row: take the tentative zero back, start a zero run of 2. */
                s->o--;
                s->lit_len--;
                if (s->lit_len == 0) {
                    s->o--;          /* drop the empty literal token itself */
                    s->lit_token = NULL;
                    s->last_lit_zero = 0;
                } else {
                    xs_close_literal(s);
                }
                s->zero_run = 2;
                return;
            }
            if (s->lit_len == XRLE_MAX_RUN) {
                xs_close_literal(s);
                s->zero_run = 1;
                return;
            }
            *s->o++ = 0;
            s->lit_len++;
            s->last_lit_zero = 1;
            return;
        }
        if (s->zero_run == XRLE_MAX_RUN) {
            xs_flush_zero(s);
        }
        s->zero_run++;
        return;
    }

    if (s->zero_run) {
        xs_flush_zero(s);
    }
    if (!s->lit_token || s->lit_len == XRLE_MAX_RUN) {
        xs_close_literal(s);
        s->lit_token = s->o++;
    }
    *s->o++ = x;
    s->lit_len++;
    s->last_lit_zero = 0;
}

size_t codec_xrle_encode_strided(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                                 uint32_t src_stride, uint32_t step, int keyframe, uint8_t *out)
{
    xrle_state_t s = { (uint16_t *)out, NULL, 0, 0, 0 };
    const uint32_t out_rows = src_rows / step;
    uint16_t *p = prev;

    for (uint32_t c = 0; c + step <= src_cols; c += step) {
        const uint16_t *col = (const uint16_t *)(src + (size_t)c * src_stride);

        if (step == 1 && !keyframe) {
            uint32_t r = 0;
            while (r < out_rows) {
                /* Fast path: inside a zero run, compare two pixels as one 32-bit word. */
                if (s.zero_run && !s.lit_token && r + 2 <= out_rows && s.zero_run + 2 <= XRLE_MAX_RUN &&
                    (((uintptr_t)(col + r) | (uintptr_t)(p + r)) & 3) == 0 &&
                    *(const uint32_t *)(col + r) == *(const uint32_t *)(p + r)) {
                    s.zero_run += 2;
                    r += 2;
                    continue;
                }
                const uint16_t v = col[r];
                xs_push(&s, (uint16_t)(v ^ p[r]));
                p[r] = v;
                r++;
            }
        } else {
            for (uint32_t r = 0; r < out_rows; r++) {
                const uint16_t v = col[r * step];
                xs_push(&s, keyframe ? v : (uint16_t)(v ^ p[r]));
                p[r] = v;
            }
        }
        p += out_rows;
    }

    xs_flush_zero(&s);
    xs_close_literal(&s);
    return (size_t)((uint8_t *)s.o - out);
}

/* ------------------------------------------------------------------- XR32 */

size_t codec_xr32_max_size(size_t n)
{
    /* All literal: n words + one token per 32768 pairs. */
    return n * 2 + ((n / 2 + XRLE_MAX_RUN - 1) / XRLE_MAX_RUN + 1) * 2;
}

int codec_xr32_decode(const uint8_t *in, size_t in_size, uint16_t *frame, size_t n, int keyframe)
{
    size_t pos = 0;
    size_t i = 0;   /* in words */

    if (n & 1) {
        return -1;
    }
    while (pos + 2 <= in_size) {
        const uint16_t t = (uint16_t)(in[pos] | (in[pos + 1] << 8));
        pos += 2;
        const size_t words = ((size_t)(t & 0x7FFFu) + 1) * 2;
        if (i + words > n) {
            return -1;
        }
        if (t & 0x8000u) {
            if (pos + words * 2 > in_size) {
                return -1;
            }
            for (size_t k = 0; k < words; k++, pos += 2) {
                const uint16_t v = (uint16_t)(in[pos] | (in[pos + 1] << 8));
                frame[i + k] = keyframe ? v : (uint16_t)(frame[i + k] ^ v);
            }
        } else if (keyframe) {
            memset(frame + i, 0, words * 2);
        }
        i += words;
    }
    return (pos == in_size && i == n) ? 0 : -1;
}

typedef struct {
    uint16_t *o;         /* output cursor */
    uint16_t *lit;       /* token slot of the open literal run, NULL in zero mode */
    uint32_t lit_len;    /* pairs in the open literal run */
    uint32_t zrun;       /* pairs in the pending zero run */
} xr32_state_t;

static inline void xr32_flush_zero(xr32_state_t *s)
{
    uint32_t z = s->zrun;
    while (z > XRLE_MAX_RUN) {
        *s->o++ = (uint16_t)(XRLE_MAX_RUN - 1);
        z -= XRLE_MAX_RUN;
    }
    if (z) {
        *s->o++ = (uint16_t)(z - 1);
    }
    s->zrun = 0;
}

static inline void xr32_close_literal(xr32_state_t *s)
{
    *s->lit = (uint16_t)(0x8000u | (s->lit_len - 1));
    s->lit = NULL;
    s->lit_len = 0;
}

/* Pair r of a column: step 1 reads one aligned word, step 2 packs pixels 4r and 4r+2. */
static inline uint32_t xr32_load(const uint8_t *col, uint32_t r, uint32_t step)
{
    if (step == 1) {
        return ((const uint32_t *)(const void *)col)[r];
    }
    const uint16_t *p = (const uint16_t *)(const void *)col;
    return (uint32_t)p[4 * r] | (uint32_t)p[4 * r + 2] << 16;
}

/*
 * One column of `pairs` pairs. Zero mode scans 4 pairs per iteration with
 * no stores; literal mode writes the XOR and the new pair. step is a
 * compile-time constant after inlining.
 */
/*
 * Memory hints (variant): 0 none, 1 PLD 128 bytes ahead on both streams,
 * 2 "touch": plain loads 128 bytes ahead into a sink (works even where PLD
 * is a no-op; ARM11 does not stall until the loaded register is used).
 */
#define XR32_AHEAD 32u   /* pairs = 128 bytes */

static inline __attribute__((always_inline)) void xr32_hint(const uint32_t *c, const uint32_t *p, uint32_t r,
                                                            uint32_t pairs, uint32_t hint, uint32_t *sink)
{
    if (hint == 0 || r + XR32_AHEAD >= pairs) {
        return;
    }
    if (hint == 1) {
        __builtin_prefetch(c + r + XR32_AHEAD);
        __builtin_prefetch(p + r + XR32_AHEAD);
    } else {
        *sink += ((const volatile uint32_t *)c)[r + XR32_AHEAD] + ((const volatile uint32_t *)p)[r + XR32_AHEAD];
    }
}

static inline __attribute__((always_inline)) void xr32_column(xr32_state_t *s, const uint8_t *col,
                                                              uint32_t *p, uint32_t pairs, uint32_t step,
                                                              uint32_t hint, uint32_t *sink)
{
    uint32_t r = 0;
    while (r < pairs) {
        if (!s->lit) {
            const uint32_t r0 = r;
            if (step == 1) {
                const uint32_t *c = (const uint32_t *)(const void *)col;
                if (hint) {
                    /* one hint per 32-byte cache line (8 pairs) */
                    while (r + 8 <= pairs) {
                        xr32_hint(c, p, r, pairs, hint, sink);
                        if (((c[r] ^ p[r]) | (c[r + 1] ^ p[r + 1]) | (c[r + 2] ^ p[r + 2]) | (c[r + 3] ^ p[r + 3]) |
                             (c[r + 4] ^ p[r + 4]) | (c[r + 5] ^ p[r + 5]) | (c[r + 6] ^ p[r + 6]) |
                             (c[r + 7] ^ p[r + 7])) != 0) {
                            break;
                        }
                        r += 8;
                    }
                }
                while (r + 4 <= pairs &&
                       ((c[r] ^ p[r]) | (c[r + 1] ^ p[r + 1]) | (c[r + 2] ^ p[r + 2]) | (c[r + 3] ^ p[r + 3])) == 0) {
                    r += 4;
                }
            }
            while (r < pairs && xr32_load(col, r, step) == p[r]) {
                r++;
            }
            s->zrun += r - r0;
            if (r >= pairs) {
                break;
            }
            xr32_flush_zero(s);
            s->lit = s->o++;
        }
        uint16_t *o = s->o;
        uint32_t len = s->lit_len;
        while (r < pairs) {
            if (step == 1 && (r & 7) == 0) {
                xr32_hint((const uint32_t *)(const void *)col, p, r, pairs, hint, sink);
            }
            const uint32_t v = xr32_load(col, r, step);
            const uint32_t x = v ^ p[r];
            if (x == 0) {
                break;
            }
            p[r] = v;
            o[0] = (uint16_t)x;
            o[1] = (uint16_t)(x >> 16);
            o += 2;
            r++;
            if (++len == XRLE_MAX_RUN) {
                break;
            }
        }
        s->o = o;
        s->lit_len = len;
        /* Close on an unchanged pair or a full run; a literal reaching the end
           of the column stays open and continues in the next column. */
        if (r < pairs || len == XRLE_MAX_RUN) {
            xr32_close_literal(s);
        }
    }
}

static volatile uint32_t xr32_sink;

static inline __attribute__((always_inline)) size_t xr32_frame(uint32_t *prev, const uint8_t *src, uint32_t src_cols,
                                                               uint32_t src_rows, uint32_t src_stride, uint32_t step,
                                                               uint32_t hint, uint8_t *out)
{
    xr32_state_t s = { (uint16_t *)(void *)out, NULL, 0, 0 };
    const uint32_t pairs = src_rows / step / 2;
    uint32_t *p = prev;
    uint32_t sink = 0;

    for (uint32_t c = 0; c + step <= src_cols; c += step) {
        xr32_column(&s, src + (size_t)c * src_stride, p, pairs, step, hint, &sink);
        p += pairs;
    }
    if (hint == 2) {
        xr32_sink = sink;
    }
    xr32_flush_zero(&s);
    if (s.lit) {
        xr32_close_literal(&s);
    }
    return (size_t)((uint8_t *)s.o - out);
}

size_t codec_xr32_encode_strided_v(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                                   uint32_t src_stride, uint32_t step, int keyframe, int variant, uint8_t *out)
{
    uint32_t *p = (uint32_t *)(void *)prev;
    if (keyframe) {
        memset(p, 0, (size_t)(src_cols / step) * (src_rows / step) * 2);
    }
    if (step != 1) {
        return xr32_frame(p, src, src_cols, src_rows, src_stride, 2, 0, out);
    }
    switch (variant) {
    case 1:  return xr32_frame(p, src, src_cols, src_rows, src_stride, 1, 1, out);
    case 2:  return xr32_frame(p, src, src_cols, src_rows, src_stride, 1, 2, out);
    default: return xr32_frame(p, src, src_cols, src_rows, src_stride, 1, 0, out);
    }
}

size_t codec_xr32_encode_conv(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                              uint32_t src_stride, uint32_t fmt,
                              int (*conv)(uint16_t *dst, const uint8_t *src, uint32_t fmt, uint32_t rows),
                              int keyframe, uint8_t *out)
{
    uint32_t tmp[128];   /* one column: up to 256 pixels */
    uint32_t *p = (uint32_t *)(void *)prev;
    const uint32_t pairs = src_rows / 2;
    if (src_rows > 256)
        return 0;
    if (keyframe) {
        memset(p, 0, (size_t)src_cols * src_rows * 2);
    }
    xr32_state_t s = { (uint16_t *)(void *)out, NULL, 0, 0 };
    uint32_t sink = 0;
    for (uint32_t c = 0; c < src_cols; c++) {
        if (!conv((uint16_t *)(void *)tmp, src + (size_t)c * src_stride, fmt, src_rows))
            return 0;
        xr32_column(&s, (const uint8_t *)tmp, p, pairs, 1, 1, &sink);
        p += pairs;
    }
    xr32_flush_zero(&s);
    if (s.lit) {
        xr32_close_literal(&s);
    }
    return (size_t)((uint8_t *)s.o - out);
}

size_t codec_xr32_encode_strided(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                                 uint32_t src_stride, uint32_t step, int keyframe, uint8_t *out)
{
    return codec_xr32_encode_strided_v(prev, src, src_cols, src_rows, src_stride, step, keyframe, 0, out);
}

/* --------------------------------------------------------------- dirty tiles */

size_t codec_tiles_max_size(uint32_t cols, uint32_t rows)
{
    size_t tiles = (size_t)(cols / 8) * (rows / 8);
    return (tiles + 7) / 8 + (size_t)cols * rows * 2;
}

static int tile_changed(const uint16_t *prev, const uint16_t *cur, uint32_t rows, uint32_t c0, uint32_t r0)
{
    for (uint32_t c = c0; c < c0 + 8; c++) {
        const uint16_t *a = prev + (size_t)c * rows + r0;
        const uint16_t *b = cur + (size_t)c * rows + r0;
        if (memcmp(a, b, 16) != 0) {
            return 1;
        }
    }
    return 0;
}

size_t codec_tiles_encode(const uint16_t *prev, const uint16_t *cur, uint32_t cols, uint32_t rows, uint8_t *out)
{
    const uint32_t tc = cols / 8, tr = rows / 8;
    const size_t bitmap = ((size_t)tc * tr + 7) / 8;
    uint8_t *p = out + bitmap;
    memset(out, 0, bitmap);

    size_t t = 0;
    for (uint32_t x = 0; x < tc; x++) {
        for (uint32_t y = 0; y < tr; y++, t++) {
            const uint32_t c0 = x * 8, r0 = y * 8;
            if (prev && !tile_changed(prev, cur, rows, c0, r0)) {
                continue;
            }
            out[t / 8] |= (uint8_t)(1u << (t % 8));
            for (uint32_t c = c0; c < c0 + 8; c++) {
                memcpy(p, cur + (size_t)c * rows + r0, 16);
                p += 16;
            }
        }
    }
    return (size_t)(p - out);
}

int codec_tiles_decode(const uint8_t *in, size_t in_size, uint16_t *frame, uint32_t cols, uint32_t rows)
{
    const uint32_t tc = cols / 8, tr = rows / 8;
    const size_t bitmap = ((size_t)tc * tr + 7) / 8;
    if (in_size < bitmap) {
        return -1;
    }
    const uint8_t *p = in + bitmap;
    const uint8_t *end = in + in_size;

    size_t t = 0;
    for (uint32_t x = 0; x < tc; x++) {
        for (uint32_t y = 0; y < tr; y++, t++) {
            if (!(in[t / 8] & (1u << (t % 8)))) {
                continue;
            }
            if (p + 128 > end) {
                return -1;
            }
            for (uint32_t c = x * 8; c < x * 8 + 8; c++) {
                memcpy(frame + (size_t)c * rows + y * 8, p, 16);
                p += 16;
            }
        }
    }
    return p == end ? 0 : -1;
}

/* -------------------------------------------------------------------- Q565 */

#define Q565_RAW 0xFEu

static inline uint32_t q565_hash(uint32_t px) { return (px * 0x9E3779B1u) >> 26; }

size_t codec_q565_encode(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint8_t *out,
                         size_t cap)
{
    uint16_t index[64];
    memset(index, 0, sizeof(index));
    uint8_t *o = out;
    uint8_t *const limit = out + (cap >= 4 ? cap - 4 : 0);   /* room for a run flush and the largest op */
    uint32_t prev = 0, run = 0;
    if (cap < 4)
        return 0;
    for (uint32_t c = 0; c < cols; c++) {
        const uint16_t *col = (const uint16_t *)(src + (size_t)c * stride);
        for (uint32_t r = 0; r < rows; r++) {
            const uint32_t v = col[r];
            if (v == prev) {
                if (++run == 62) {
                    if (o >= out + cap)
                        return 0;
                    *o++ = (uint8_t)(0xC0u | 61u);
                    run = 0;
                }
                continue;
            }
            if (o > limit)
                return 0;
            if (run) {
                *o++ = (uint8_t)(0xC0u | (run - 1));
                run = 0;
            }
            const uint32_t h = q565_hash(v);
            if (index[h] == v) {
                *o++ = (uint8_t)h;
            } else {
                index[h] = (uint16_t)v;
                /* channel differences, wrapped (the decoder adds them back modulo 32 / 64) */
                const uint32_t dr = ((v >> 11) - (prev >> 11)) & 31u, dg = (((v >> 5) & 63u) - ((prev >> 5) & 63u)) & 63u,
                               db = ((v & 31u) - (prev & 31u)) & 31u;
                const uint32_t ur = (dr + 2u) & 31u, ug = (dg + 2u) & 63u, ub = (db + 2u) & 31u;
                const uint32_t gb = (dg + 32u) & 63u;    /* LUMA's g field: dg + 32 */
                const uint32_t half = (gb >> 1) - 16u;   /* floor(dg / 2), wrapped */
                const uint32_t lr = (dr - half + 8u) & 31u, lb = (db - half + 8u) & 31u;
                if ((ur | ub) < 4u && ug < 4u)
                    *o++ = (uint8_t)(0x40u | ur << 4 | ug << 2 | ub);
                else if ((lr | lb) < 16u) {
                    o[0] = (uint8_t)(0x80u | gb);
                    o[1] = (uint8_t)(lr << 4 | lb);
                    o += 2;
                } else {
                    o[0] = Q565_RAW;
                    o[1] = (uint8_t)v;
                    o[2] = (uint8_t)(v >> 8);
                    o += 3;
                }
            }
            prev = v;
        }
    }
    if (run) {
        if (o >= out + cap)
            return 0;
        *o++ = (uint8_t)(0xC0u | (run - 1));
    }
    return (size_t)(o - out);
}

int codec_q565_decode(const uint8_t *in, size_t in_size, uint16_t *frame, size_t n)
{
    uint16_t index[64];
    memset(index, 0, sizeof(index));
    uint32_t prev = 0;
    size_t pos = 0, i = 0;
    while (i < n) {
        if (pos >= in_size)
            return -1;
        const uint32_t b = in[pos++];
        if (b == Q565_RAW) {
            if (pos + 2 > in_size)
                return -1;
            prev = in[pos] | (uint32_t)in[pos + 1] << 8;
            pos += 2;
            index[q565_hash(prev)] = (uint16_t)prev;
        } else if ((b & 0xC0u) == 0xC0u) {
            const uint32_t k = (b & 0x3Fu) + 1;
            if (i + k > n)
                return -1;
            for (uint32_t j = 0; j < k; j++)
                frame[i + j] = (uint16_t)prev;
            i += k;
            continue;
        } else if ((b & 0xC0u) == 0x00u) {
            prev = index[b];
        } else {
            int sr, sg, sb;
            if ((b & 0xC0u) == 0x40u) {
                sr = (int)((b >> 4) & 3u) - 2;
                sg = (int)((b >> 2) & 3u) - 2;
                sb = (int)(b & 3u) - 2;
            } else {
                if (pos >= in_size)
                    return -1;
                const uint32_t b2 = in[pos++];
                sg = (int)(b & 0x3Fu) - 32;
                const int half = (sg + 32) / 2 - 16;
                sr = (int)(b2 >> 4) - 8 + half;
                sb = (int)(b2 & 15u) - 8 + half;
            }
            const uint32_t r = ((prev >> 11) + (uint32_t)sr) & 31u, g = (((prev >> 5) & 63u) + (uint32_t)sg) & 63u,
                           bl = ((prev & 31u) + (uint32_t)sb) & 31u;
            prev = r << 11 | g << 5 | bl;
            index[q565_hash(prev)] = (uint16_t)prev;
        }
        frame[i++] = (uint16_t)prev;
    }
    return pos == in_size ? 0 : -1;
}

uint32_t codec_changed_pct(const uint16_t *prev, const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride)
{
    uint32_t seen = 0, changed = 0;
    for (uint32_t c = 0; c < cols; c += 4) {
        const uint16_t *col = (const uint16_t *)(src + (size_t)c * stride), *p = prev + (size_t)c * rows;
        for (uint32_t r = 3; r < rows; r += 8, seen++)
            changed += col[r] != p[r];
    }
    return seen ? changed * 100u / seen : 0;
}

/* -------------------------------------------------------------------- C420 */

/*
 * Two pixels at a time in one register (16-bit lanes): a = rows r and r + 1 of column c,
 * b = the same of column c + 1. The sums stay within their lanes (s <= 125, r5 + 31 - b5 <= 62).
 */
#define C420_M5 0x001F001Fu
#define C420_M6 0x003F003Fu
static inline __attribute__((always_inline)) uint32_t c420_block(uint32_t a, uint32_t b)
{
    const uint32_t ra = (a >> 11) & C420_M5, ga = (a >> 5) & C420_M6, ba = a & C420_M5;
    const uint32_t rb = (b >> 11) & C420_M5, gb = (b >> 5) & C420_M6, bb = b & C420_M5;
    const uint32_t sa = ra + ga + ba, sb = rb + gb + bb;
    const uint32_t ya = ((sa + 0x00020000u) >> 2) & C420_M5;   /* o = 0, 2 */
    const uint32_t yb = ((sb + 0x00010002u) >> 2) & C420_M5;   /* o = 2, 1 */
    /* per lane (two pixels), biased to stay positive: r - b + 62, and 2 * g - s + 128 */
    const uint32_t cl = ra + rb + 0x003E003Eu - ba - bb;
    const uint32_t gl = ((ga + gb) << 1) + 0x00800080u - sa - sb;
    /* both lanes added (in the top half): the block's sums + 124 and + 256; the rounding and the
       fields' + 32 are in the constants: (so + 2 + 128) >> 2 and (sg + 3 + 256) >> 3 */
    const uint32_t co = ((cl + (cl << 16)) + (6u << 16)) >> 18, cg = ((gl + (gl << 16)) + (3u << 16)) >> 19;
    /* a lane pair of y into 10 bits: the top lane lands right above the bottom one */
    return ((ya | ya >> 11) & 0x3FFu) | ((yb | yb >> 11) & 0x3FFu) << 10 | co << 20 | cg << 26;
}

static inline __attribute__((always_inline)) uint32_t *c420_columns(const uint32_t *a, const uint32_t *b,
                                                                    uint32_t pairs, uint32_t *o, int hint)
{
    for (uint32_t r = 0; r < pairs; r++) {
        if (hint && (r & 7u) == 0) {   /* one hint per 32-byte cache line */
            __builtin_prefetch(a + r + 32);
            __builtin_prefetch(b + r + 32);
        }
        *o++ = c420_block(a[r], b[r]);
    }
    return o;
}

size_t codec_c420_encode(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint8_t *out)
{
    uint32_t *o = (uint32_t *)(void *)out;
    for (uint32_t c = 0; c + 2 <= cols; c += 2) {
        const uint8_t *col = src + (size_t)c * stride;
        o = c420_columns((const uint32_t *)(const void *)col, (const uint32_t *)(const void *)(col + stride),
                         rows / 2, o, 1);
    }
    return (size_t)((uint8_t *)o - out);
}

size_t codec_c420_encode_conv(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint32_t fmt,
                              int (*conv)(uint16_t *dst, const uint8_t *src, uint32_t fmt, uint32_t rows),
                              uint8_t *out)
{
    uint32_t ta[128], tb[128];   /* two columns: up to 256 pixels each */
    uint32_t *o = (uint32_t *)(void *)out;
    if (rows > 256)
        return 0;
    for (uint32_t c = 0; c + 2 <= cols; c += 2) {
        const uint8_t *col = src + (size_t)c * stride;
        if (!conv((uint16_t *)(void *)ta, col, fmt, rows) || !conv((uint16_t *)(void *)tb, col + stride, fmt, rows))
            return 0;
        o = c420_columns(ta, tb, rows / 2, o, 0);
    }
    return (size_t)((uint8_t *)o - out);
}

/*
 * Decoding with the clamps in tables: with A = 2 * co - 2 * cg + 2, B = -2 * co - 2 * cg + 2,
 * C = 2 * cg + 1 for the block, a pixel is r5 = clamp((s' + A) >> 2), b5 = clamp((s' + B) >> 2),
 * g6 = clamp((s' + C) >> 1). (s' + A) >> 2 and (s' + B) >> 2 are within -32..63, (s' + C) >> 1
 * within -32..94: the tables are indexed from -32 and hold the channel in its place in the pixel.
 * The player decodes 60 of these frames a second on an Old 3DS (both screens at 30).
 */
/*
 * From 8-bit channels: with L = R + 2 * G + B (8 * s of the RGB565 pixel, with the bits RGB565
 * drops), y = (L + 8 * o) >> 5, co = (sum of (R - B) + 16) >> 5, cg = (sum of (4 * G - L) + 32) >> 6;
 * each can come out one above its field's range (white, pure red or green), so they are clamped.
 */
static inline __attribute__((always_inline)) uint32_t c420_min(uint32_t v, uint32_t max) { return v > max ? max : v; }

size_t codec_c420_encode_888(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint32_t bpp,
                             uint8_t *out)
{
    uint32_t *o = (uint32_t *)(void *)out;
    const uint32_t first = bpp == 4 ? 1u : 0u;   /* RGBA8: the alpha byte comes first */
    for (uint32_t c = 0; c + 2 <= cols; c += 2) {
        const uint8_t *a = src + (size_t)c * stride + first, *b = a + stride;
        for (uint32_t r = 0; r + 2 <= rows; r += 2, a += 2 * bpp, b += 2 * bpp) {
            if ((r & 15u) == 0) {
                __builtin_prefetch(a + 96);
                __builtin_prefetch(b + 96);
            }
            const uint32_t b0 = a[0], g0 = a[1], r0 = a[2], b1 = a[bpp], g1 = a[bpp + 1], r1 = a[bpp + 2];
            const uint32_t b2 = b[0], g2 = b[1], r2 = b[2], b3 = b[bpp], g3 = b[bpp + 1], r3 = b[bpp + 2];
            const uint32_t l0 = r0 + 2 * g0 + b0, l1 = r1 + 2 * g1 + b1, l2 = r2 + 2 * g2 + b2, l3 = r3 + 2 * g3 + b3;
            /* biased to stay positive: + 1024 and + 2048 (32 after the shifts: the fields' offset) */
            const uint32_t so = r0 + r1 + r2 + r3 + 1024u + 16u - b0 - b1 - b2 - b3;
            const uint32_t sg = 4 * (g0 + g1 + g2 + g3) + 2048u + 32u - l0 - l1 - l2 - l3;
            *o++ = c420_min(l0 >> 5, 31) | c420_min((l1 + 16) >> 5, 31) << 5 | c420_min((l2 + 16) >> 5, 31) << 10 |
                   c420_min((l3 + 8) >> 5, 31) << 15 | c420_min(so >> 5, 63) << 20 | c420_min(sg >> 6, 63) << 26;
        }
    }
    return (size_t)((uint8_t *)o - out);
}

static uint16_t c420_r[96], c420_g[128];
static uint8_t c420_b[96];
static int c420_ready;

static void c420_tables(void)
{
    for (int i = 0; i < 96; i++) {
        const int v = i - 32 < 0 ? 0 : i - 32 > 31 ? 31 : i - 32;
        c420_r[i] = (uint16_t)(v << 11);
        c420_b[i] = (uint8_t)v;
    }
    for (int i = 0; i < 128; i++) {
        const int v = i - 32 < 0 ? 0 : i - 32 > 63 ? 63 : i - 32;
        c420_g[i] = (uint16_t)(v << 5);
    }
    c420_ready = 1;
}

int codec_c420_decode(const uint8_t *in, size_t in_size, uint16_t *frame, uint32_t cols, uint32_t rows)
{
    return codec_c420_decode_strided(in, in_size, frame, cols, rows, rows);
}

int codec_c420_decode_strided(const uint8_t *in, size_t in_size, uint16_t *frame, uint32_t cols, uint32_t rows,
                              uint32_t stride)
{
    if ((cols | rows) & 1u || in_size != codec_c420_size(cols, rows))
        return -1;
    if (!c420_ready)
        c420_tables();
    for (uint32_t c = 0; c < cols; c += 2) {
        uint16_t *a = frame + (size_t)c * stride, *b = a + stride;
        for (uint32_t r = 0; r < rows; r += 2, in += 4) {
            const uint32_t w = in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
            const int32_t co = (int32_t)((w >> 20) & 63u) - 32, cg = (int32_t)(w >> 26) - 32;
            /* + 128 and + 64: the tables' offset of 32 after the shifts */
            const uint32_t A = (uint32_t)(2 * co - 2 * cg + 2 + 128), B = (uint32_t)(-2 * co - 2 * cg + 2 + 128),
                           C = (uint32_t)(2 * cg + 1 + 64);
#define C420_PX(y, o) \
    (s = 4u * (y) + 2u - (o), (uint16_t)(c420_r[(s + A) >> 2] | c420_g[(s + C) >> 1] | c420_b[(s + B) >> 2]))
            uint32_t s;
            a[r] = C420_PX(w & 31u, 0u);
            a[r + 1] = C420_PX((w >> 5) & 31u, 2u);
            b[r] = C420_PX((w >> 10) & 31u, 2u);
            b[r + 1] = C420_PX((w >> 15) & 31u, 1u);
#undef C420_PX
        }
    }
    return 0;
}

uint32_t codec_sample_changed(uint32_t *samples, const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride,
                              uint32_t bpp, int first)
{
    const uint32_t mask = bpp >= 4 ? 0xFFFFFFFFu : (1u << (8u * bpp)) - 1u;
    uint32_t seen = 0, changed = 0;
    for (uint32_t c = 0; c < cols; c += 4) {
        const uint8_t *col = src + (size_t)c * stride;
        for (uint32_t r = 3; r < rows; r += 8, seen++) {
            const uint8_t *p = col + (size_t)r * bpp;
            uint32_t v = p[0] | (uint32_t)p[1] << 8;
            if (bpp > 2)
                v |= (uint32_t)p[2] << 16;
            if (bpp > 3)
                v |= (uint32_t)p[3] << 24;
            v &= mask;
            changed += samples[seen] != v;
            samples[seen] = v;
        }
    }
    return first || !seen ? 100u : changed * 100u / seen;
}
