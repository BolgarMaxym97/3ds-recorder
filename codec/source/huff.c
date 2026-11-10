/* Huffman stage over compressed frames: see huff.h. Portable C99 (host tools and the 3DS). */
#include "huff.h"

#include <string.h>

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

/* ------------------------------------------------------------------ code lengths */

/* Sorts `n` symbols by frequency, ascending (insertion sort: at most 256, mostly short runs). */
static void sort_by_freq(uint16_t *sym, const uint32_t *freq, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++)
    {
        const uint16_t s = sym[i];
        const uint32_t f = freq[s];
        uint32_t j = i;
        while (j > 0 && freq[sym[j - 1]] > f)
        {
            sym[j] = sym[j - 1];
            j--;
        }
        sym[j] = s;
    }
}

/*
 * Minimum-redundancy code lengths in place (Moffat and Katajainen, "In-place calculation of
 * minimum-redundancy codes", 1995): a[] holds the frequencies in ascending order on entry and
 * the code lengths (non-increasing) on return. n >= 2.
 */
static void min_redundancy(uint32_t *a, int n)
{
    int root = 0, leaf = 2, next;
    a[0] += a[1];
    for (next = 1; next < n - 1; next++)
    {
        if (leaf >= n || a[root] < a[leaf])
        {
            a[next] = a[root];
            a[root++] = (uint32_t)next;
        }
        else
            a[next] = a[leaf++];
        if (leaf >= n || (root < next && a[root] < a[leaf]))
        {
            a[next] += a[root];
            a[root++] = (uint32_t)next;
        }
        else
            a[next] += a[leaf++];
    }
    a[n - 2] = 0;
    for (next = n - 3; next >= 0; next--)
        a[next] = a[a[next]] + 1;
    int avbl = 1, used = 0, dpth = 0;
    root = n - 2;
    next = n - 1;
    while (avbl > 0)
    {
        while (root >= 0 && (int)a[root] == dpth)
        {
            used++;
            root--;
        }
        while (avbl > used)
        {
            a[next--] = (uint32_t)dpth;
            avbl--;
        }
        avbl = 2 * used;
        dpth++;
        used = 0;
    }
}

/* Code lengths (<= HUFF_MAX_BITS) for one table: len[s] = 0 for an unused symbol. */
static void build_lengths(const uint32_t *freq, uint8_t *len)
{
    static uint16_t sym[256];
    static uint32_t a[256];
    uint32_t n = 0;
    for (uint32_t s = 0; s < 256; s++)
    {
        len[s] = 0;
        if (freq[s])
            sym[n++] = (uint16_t)s;
    }
    if (n == 0)
        return;
    if (n == 1)
    {
        len[sym[0]] = 1;
        return;
    }
    sort_by_freq(sym, freq, n);
    for (uint32_t i = 0; i < n; i++)
        a[i] = freq[sym[i]];
    min_redundancy(a, (int)n);
    /* lengths per bit count, the longest moved down to HUFF_MAX_BITS; then, while the code is
       over-full (Kraft sum > 1), a leaf at the longest length below the limit goes one deeper (the
       smallest step down, on one of the rarest symbols) */
    uint32_t count[HUFF_MAX_BITS + 1] = { 0 };
    uint32_t kraft = 0;   /* in units of 2^-HUFF_MAX_BITS */
    for (uint32_t i = 0; i < n; i++)
    {
        const uint32_t l = a[i] > HUFF_MAX_BITS ? HUFF_MAX_BITS : a[i];
        count[l]++;
        kraft += 1u << (HUFF_MAX_BITS - l);
    }
    while (kraft > (1u << HUFF_MAX_BITS))
    {
        uint32_t bits = HUFF_MAX_BITS - 1;
        while (count[bits] == 0)
            bits--;
        count[bits]--;
        count[bits + 1]++;
        kraft -= 1u << (HUFF_MAX_BITS - bits - 1);
    }
    /* the most frequent symbols (end of sym[]) get the shortest codes */
    uint32_t i = n;
    for (uint32_t bits = 1; bits <= HUFF_MAX_BITS; bits++)
        for (uint32_t k = 0; k < count[bits]; k++)
            len[sym[--i]] = (uint8_t)bits;
}

/* Canonical codes, bit-reversed for least-significant-bit-first output: code[s] = bits | len << 16. */
static void build_codes(const uint8_t *len, uint32_t *code)
{
    uint32_t count[HUFF_MAX_BITS + 1] = { 0 }, next[HUFF_MAX_BITS + 2];
    for (uint32_t s = 0; s < 256; s++)
        count[len[s]]++;
    count[0] = 0;
    uint32_t c = 0;
    for (uint32_t b = 1; b <= HUFF_MAX_BITS; b++)
    {
        c = (c + count[b - 1]) << 1;
        next[b] = c;
    }
    for (uint32_t s = 0; s < 256; s++)
    {
        const uint32_t l = len[s];
        code[s] = 0;
        if (!l)
            continue;
        uint32_t v = next[l]++, r = 0;
        for (uint32_t k = 0; k < l; k++, v >>= 1)
            r = (r << 1) | (v & 1u);
        code[s] = r | (l << 16);
    }
}

/* ------------------------------------------------------------------ encoder */

size_t huff_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap)
{
    static uint32_t freq[2][256], code[2][256];
    static uint8_t len[2][256];
    if (cap < HUFF_HEADER + 4 || n > 0xFFFFFFFFu)
        return 0;
    memset(freq, 0, sizeof(freq));
    const size_t pairs = n / 2;
    for (size_t i = 0; i < pairs; i++)
    {
        freq[0][in[2 * i]]++;
        freq[1][in[2 * i + 1]]++;
    }
    if (n & 1)
        freq[0][in[n - 1]]++;
    uint64_t bits = 0;
    for (int t = 0; t < 2; t++)
    {
        build_lengths(freq[t], len[t]);
        build_codes(len[t], code[t]);
        for (uint32_t s = 0; s < 256; s++)
            bits += (uint64_t)freq[t][s] * len[t][s];
    }
    const uint64_t size = HUFF_HEADER + (((bits + 31) / 32) * 4);
    if (size > cap)
        return 0;

    out[0] = (uint8_t)n;
    out[1] = (uint8_t)(n >> 8);
    out[2] = (uint8_t)(n >> 16);
    out[3] = (uint8_t)(n >> 24);
    for (int t = 0; t < 2; t++)
        for (uint32_t k = 0; k < 128; k++)
            out[4 + 128 * t + k] = (uint8_t)(len[t][2 * k] | (len[t][2 * k + 1] << 4));

    uint8_t *o = out + HUFF_HEADER;
    uint32_t acc = 0, nb = 0;   /* nb < 16 between symbols: one 12-bit code always fits */
#define HUFF_PUT(c)                                     \
    do {                                                \
        acc |= ((c) & 0xFFFFu) << nb;                   \
        nb += (c) >> 16;                                \
        if (nb >= 16)                                   \
        {                                               \
            o[0] = (uint8_t)acc;                        \
            o[1] = (uint8_t)(acc >> 8);                 \
            o += 2;                                     \
            acc >>= 16;                                 \
            nb -= 16;                                   \
        }                                               \
    } while (0)
    const uint32_t *c0 = code[0], *c1 = code[1];
    for (size_t i = 0; i < pairs; i++)
    {
        const uint32_t a = c0[in[2 * i]], b = c1[in[2 * i + 1]];
        HUFF_PUT(a);
        HUFF_PUT(b);
    }
    if (n & 1)
        HUFF_PUT(c0[in[n - 1]]);
#undef HUFF_PUT
    if (nb)
    {
        o[0] = (uint8_t)acc;
        o[1] = (uint8_t)(acc >> 8);
        o += 2;
    }
    while ((size_t)(o - out) < size)
        *o++ = 0;
    return (size_t)size;
}

/* ------------------------------------------------------------------ decoder */

size_t huff_raw_size(const uint8_t *in, size_t in_size)
{
    return in_size < HUFF_HEADER ? 0 : rd32(in);
}

/* Fills one decoding table from the code lengths; false if they do not form a valid code. */
static int build_table(const uint8_t *packed, uint16_t *table)
{
    uint8_t len[256];
    uint32_t code[256];
    uint32_t kraft = 0;   /* in units of 2^-HUFF_MAX_BITS */
    for (uint32_t k = 0; k < 128; k++)
    {
        len[2 * k] = packed[k] & 15u;
        len[2 * k + 1] = packed[k] >> 4;
    }
    for (uint32_t s = 0; s < 256; s++)
    {
        if (len[s] > HUFF_MAX_BITS)
            return 0;
        if (len[s])
            kraft += 1u << (HUFF_MAX_BITS - len[s]);
    }
    if (kraft > (1u << HUFF_MAX_BITS))
        return 0;
    memset(table, 0, sizeof(uint16_t) << HUFF_MAX_BITS);
    build_codes(len, code);
    for (uint32_t s = 0; s < 256; s++)
    {
        const uint32_t l = len[s];
        if (!l)
            continue;
        const uint16_t e = (uint16_t)(s << 4 | l);
        for (uint32_t v = code[s] & 0xFFFFu; v < (1u << HUFF_MAX_BITS); v += 1u << l)
            table[v] = e;
    }
    return 1;
}

int huff_decode(const uint8_t *in, size_t in_size, uint8_t *out, size_t cap, HuffTables *tables)
{
    if (in_size < HUFF_HEADER)
        return -1;
    const size_t n = rd32(in);
    if (n > cap)
        return -1;
    if (!build_table(in + 4, tables->t[0]) || !build_table(in + 4 + 128, tables->t[1]))
        return -1;
    const uint8_t *p = in + HUFF_HEADER, *end = in + in_size;
    uint32_t acc = 0, nb = 0, zf = 0;   /* zf: zero bits fed past the end */
#define HUFF_GET(tab, dst)                                          \
    do {                                                            \
        if (nb < 16)                                                \
        {                                                           \
            if (p + 2 <= end)                                       \
            {                                                       \
                acc |= (uint32_t)(p[0] | (p[1] << 8)) << nb;        \
                p += 2;                                             \
            }                                                       \
            else                                                    \
                zf += 16;                                           \
            nb += 16;                                               \
        }                                                           \
        const uint32_t e = (tab)[acc & ((1u << HUFF_MAX_BITS) - 1)]; \
        const uint32_t l = e & 15u;                                 \
        if (!l)                                                     \
            return -1;                                              \
        acc >>= l;                                                  \
        nb -= l;                                                    \
        (dst) = (uint8_t)(e >> 4);                                  \
    } while (0)
    const uint16_t *t0 = tables->t[0], *t1 = tables->t[1];
    const size_t pairs = n / 2;
    for (size_t i = 0; i < pairs; i++)
    {
        HUFF_GET(t0, out[2 * i]);
        HUFF_GET(t1, out[2 * i + 1]);
        if (zf > 32)   /* reading far past the end: a malformed stream */
            return -1;
    }
    if (n & 1)
        HUFF_GET(t0, out[n - 1]);
#undef HUFF_GET
    /* none of the bits used may lie past the end */
    return zf <= nb ? 0 : -1;
}

/* ------------------------------------------------------------------ fused XR32 decoder */

/*
 * An R3DH frame straight into the picture: the Huffman stage and the XR32 runs in one pass, with
 * no buffer for the XR32 stream between them (the gallery's player on an Old 3DS decodes two
 * screens a frame). Same result as huff_decode + codec_xr32_decode.
 */
int huff_xr32_decode(const uint8_t *in, size_t in_size, uint16_t *frame, size_t n, int keyframe, HuffTables *tables)
{
    if (in_size < HUFF_HEADER || (n & 1))
        return -1;
    const size_t raw = rd32(in);
    if (raw & 1)
        return -1;
    if (!build_table(in + 4, tables->t[0]) || !build_table(in + 4 + 128, tables->t[1]))
        return -1;
    const uint8_t *p = in + HUFF_HEADER, *end = in + in_size;
    const uint16_t *t0 = tables->t[0], *t1 = tables->t[1];
    uint32_t acc = 0, nb = 0, zf = 0;
    size_t left = raw / 2;   /* 16-bit words still to read */
    size_t i = 0;            /* in pixels */
#define HUFF_SYM(tab, dst)                                          \
    do {                                                            \
        if (nb < 16)                                                \
        {                                                           \
            if (p + 2 <= end)                                       \
            {                                                       \
                acc |= (uint32_t)(p[0] | (p[1] << 8)) << nb;        \
                p += 2;                                             \
            }                                                       \
            else                                                    \
                zf += 16;                                           \
            nb += 16;                                               \
        }                                                           \
        const uint32_t e = (tab)[acc & ((1u << HUFF_MAX_BITS) - 1)]; \
        const uint32_t l = e & 15u;                                 \
        if (!l)                                                     \
            return -1;                                              \
        acc >>= l;                                                  \
        nb -= l;                                                    \
        (dst) = e >> 4;                                             \
    } while (0)
#define HUFF_WORD(dst)            \
    do {                          \
        uint32_t lo_, hi_;        \
        HUFF_SYM(t0, lo_);        \
        HUFF_SYM(t1, hi_);        \
        (dst) = lo_ | hi_ << 8;   \
    } while (0)
    while (left)
    {
        uint32_t t;
        HUFF_WORD(t);
        left--;
        const size_t words = ((size_t)(t & 0x7FFFu) + 1) * 2;
        if (i + words > n)
            return -1;
        if (t & 0x8000u)
        {
            if (words > left)
                return -1;
            left -= words;
            for (size_t k = 0; k < words; k++)
            {
                uint32_t v;
                HUFF_WORD(v);
                frame[i + k] = keyframe ? (uint16_t)v : (uint16_t)(frame[i + k] ^ v);
            }
            if (zf > 32)
                return -1;
        }
        else if (keyframe)
            memset(frame + i, 0, words * 2);
        i += words;
    }
#undef HUFF_WORD
#undef HUFF_SYM
    return i == n && zf <= nb ? 0 : -1;
}
