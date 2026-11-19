#include "textbreak.h"

uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int n;
    if (p[0] < 0x80) {
        *s += 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0)
        cp = p[0] & 0x1F, n = 1;
    else if ((p[0] & 0xF0) == 0xE0)
        cp = p[0] & 0x0F, n = 2;
    else if ((p[0] & 0xF8) == 0xF0)
        cp = p[0] & 0x07, n = 3;
    else {
        *s += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            *s += i;
            return 0xFFFD;
        }
        cp = cp << 6 | (p[i] & 0x3F);
    }
    *s += n + 1;
    return cp;
}

/* Japanese, Chinese and their punctuation: a line may break before or after each one */
static int wide(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x2FFFF);
}

/* does not start a line (closing punctuation, small kana, the long vowel mark) */
static int no_start(uint32_t cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0xFF01: case 0xFF1F: case 0xFF1A: case 0xFF1B:
    case 0xFF09: case 0x300D: case 0x300F: case 0x3011: case 0x3015: case 0x3009: case 0x300B: case 0x201D:
    case 0x2019: case 0x2026: case 0x30FC: case 0x30FB: case 0x3005: case 0xFF05: case 0x2025:
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083: case 0x3085:
    case 0x3087: case 0x308E: case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3:
    case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE: case 0x30F5: case 0x30F6:
    case ',': case '.': case '!': case '?': case ':': case ';': case ')': case ']': case '%':
        return 1;
    }
    return 0;
}

/* does not end a line (opening brackets) */
static int no_end(uint32_t cp)
{
    switch (cp) {
    case 0xFF08: case 0x300C: case 0x300E: case 0x3010: case 0x3014: case 0x3008: case 0x300A: case 0x201C:
    case 0x2018: case '(': case '[':
        return 1;
    }
    return 0;
}

size_t text_line(const char *s, float width, CharWidthFn w, void *ctx, const char **next, float *line_w)
{
    const char *p = s, *cut = NULL, *cut_next = NULL;
    float x = 0, cut_x = 0;
    uint32_t prev = 0;
    while (*p) {
        const char *at = p;
        const uint32_t cp = utf8_next(&p);
        if (cp == '\n') {
            *next = p;
            if (line_w)
                *line_w = x;
            return (size_t)(at - s);
        }
        if (cp == ' ') {
            if (at > s && prev != ' ') { /* a line may end before the spaces, the next starts after */
                cut = at;
                cut_x = x;
            }
            const char *q = p;
            while (*q == ' ')
                q++;
            cut_next = q;
        } else if (at > s && prev != ' ' && (wide(cp) || wide(prev)) && !no_start(cp) && !no_end(prev)) {
            cut = at;
            cut_next = at;
            cut_x = x;
        }
        const float cw = w(ctx, cp);
        if (x + cw > width && cut && cp != ' ') {
            *next = cut_next;
            if (line_w)
                *line_w = cut_x;
            return (size_t)(cut - s);
        }
        x += cw;
        prev = cp;
    }
    /* the last line: trailing spaces off */
    const char *end = p;
    while (end > s && end[-1] == ' ')
        end--;
    if (end < p && cut == end)
        x = cut_x;
    *next = p;
    if (line_w)
        *line_w = x;
    return (size_t)(end - s);
}

float text_width(const char *s, CharWidthFn w, void *ctx)
{
    float x = 0, max = 0;
    const char *p = s;
    while (*p) {
        const uint32_t cp = utf8_next(&p);
        if (cp == '\n') {
            if (x > max)
                max = x;
            x = 0;
        } else
            x += w(ctx, cp);
    }
    return x > max ? x : max;
}
