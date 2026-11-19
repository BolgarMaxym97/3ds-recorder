/* app/source/textbreak.c: where lines end, with every character 1 wide (CJK 2) */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "textbreak.h"

static float cw(void *ctx, uint32_t cp)
{
    (void)ctx;
    return cp >= 0x2E80 ? 2.0f : 1.0f;
}

/* the lines of s at width, joined with '|' */
static const char *lines(const char *s, float width)
{
    static char out[512];
    size_t o = 0;
    const char *p = s;
    while (*p) {
        const char *next;
        size_t n = text_line(p, width, cw, NULL, &next, NULL);
        if (o)
            out[o++] = '|';
        memcpy(out + o, p, n);
        o += n;
        assert(next > p);
        p = next;
    }
    out[o] = 0;
    return out;
}

#define EQ(s, w, want)                                                                         \
    do {                                                                                       \
        const char *got = lines(s, w);                                                         \
        if (strcmp(got, want)) {                                                               \
            printf("FAIL %s:%d: \"%s\" at %g -> \"%s\", want \"%s\"\n", __FILE__, __LINE__, s, \
                   (double)(w), got, want);                                                    \
            return 1;                                                                          \
        }                                                                                      \
    } while (0)

int main(void)
{
    /* Latin: spaces */
    EQ("one two three", 7, "one two|three");
    EQ("one two three", 100, "one two three");
    EQ("one two three", 3, "one|two|three");
    EQ("verylongword x", 4, "verylongword|x");          /* a long word overflows on its own */
    EQ("a  b", 1, "a|b");                                /* spaces between lines go */
    EQ("trailing  ", 100, "trailing");
    EQ("line one\nline two", 100, "line one|line two");
    EQ("a\n\nb", 100, "a||b");                           /* an empty line stays */
    EQ("Привіт світе", 7, "Привіт|світе");               /* Cyrillic: words */
    /* Japanese: any two characters, 2 wide each */
    EQ("録画を開始します", 6, "録画を|開始し|ます");
    EQ("録画、開始", 4, "録|画、|開始");                   /* 、 does not start a line */
    EQ("はい「録」", 6, "はい|「録」");                /* 「 does not end one, 」 does not start one */
    EQ("SDカード", 5, "SD|カー|ド");                    /* ー: no line start... */
    EQ("ＳＤカードを入れる", 8, "ＳＤカー|ドを入れ|る");
    /* mixed */
    EQ("按 Ⓐ 开始录制", 4, "按 Ⓐ|开始|录制");
    EQ("Wi-Fi で送る", 6, "Wi-Fi|で送る");
    /* widths */
    const char *next;
    float lw;
    text_line("ab cd", 3, cw, NULL, &next, &lw);
    assert(lw == 2 && !strcmp(next, "cd"));
    assert(text_width("ab\nabcd", cw, NULL) == 4);
    assert(text_width("録画", cw, NULL) == 4);
    /* UTF-8 */
    const char *u = "\xE9\x8C\xB2";
    assert(utf8_next(&u) == 0x9332 && !*u);
    const char *bad = "\xE9\x41";
    assert(utf8_next(&bad) == 0xFFFD && *bad == 'A');
    puts("textbreak: ok");
    return 0;
}
