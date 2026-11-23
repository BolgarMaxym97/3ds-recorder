/* Help's support links as the Worker sends them (app/source/support_core.c). */
#include "support_core.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            g_fail++;                                                  \
        }                                                              \
    } while (0)

int main(void)
{
    SupportLink l[SUPPORT_MAX];
    const char two[] = "Ko-fi https://ko-fi.com/maksymbolhar\nMonobank https://send.monobank.ua/jar/9jJDGho7H8\n";
    CHECK(support_parse(two, sizeof(two) - 1, l, SUPPORT_MAX) == 2);
    CHECK(!strcmp(l[0].label, "Ko-fi") && !strcmp(l[0].url, "https://ko-fi.com/maksymbolhar"));
    CHECK(!strcmp(l[1].label, "Monobank") && !strcmp(l[1].url, "https://send.monobank.ua/jar/9jJDGho7H8"));

    CHECK(support_parse("", 0, l, SUPPORT_MAX) == 0);
    const char crlf[] = "Ko-fi https://ko-fi.com/a\r\n";
    CHECK(support_parse(crlf, sizeof(crlf) - 1, l, SUPPORT_MAX) == 1 && !strcmp(l[0].url, "https://ko-fi.com/a"));
    const char nonl[] = "Ko-fi https://ko-fi.com/a";
    CHECK(support_parse(nonl, sizeof(nonl) - 1, l, SUPPORT_MAX) == 1);

    /* skipped: not https, no url, a space in it, not ASCII, too long */
    const char bad[] = "A http://x.com/a\nB\nC https://\nD https://x.com/a b\nЕ https://x.com/a\n"
                       "F https://x.com/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                       "G https://ok.com/g\n";
    CHECK(support_parse(bad, sizeof(bad) - 1, l, SUPPORT_MAX) == 1 && !strcmp(l[0].label, "G"));

    const char four[] = "A https://a.com\nB https://b.com\nC https://c.com\nD https://d.com\n";
    CHECK(support_parse(four, sizeof(four) - 1, l, SUPPORT_MAX) == 3 && !strcmp(l[2].label, "C"));
    CHECK(support_parse(four, sizeof(four) - 1, l, 1) == 1);

    if (g_fail)
        return 1;
    printf("test_support: ok\n");
    return 0;
}
