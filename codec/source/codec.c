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
