#include "support_core.h"

#include <string.h>

static int printable(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] <= ' ' || s[i] > '~')
            return 0;
    return 1;
}

int support_parse(const char *text, size_t len, SupportLink *links, int max)
{
    int count = 0;
    for (size_t at = 0; at < len && count < max;) {
        const char *line = text + at;
        const char *nl = memchr(line, '\n', len - at);
        size_t n = nl ? (size_t)(nl - line) : len - at;
        at += n + 1;
        if (n && line[n - 1] == '\r')
            n--;
        const char *sp = memchr(line, ' ', n);
        if (!sp)
            continue;
        const size_t ln = (size_t)(sp - line), un = n - ln - 1;
        const char *url = sp + 1;
        if (!ln || ln >= sizeof(links->label) || un <= 8 || un >= sizeof(links->url) || memcmp(url, "https://", 8) ||
            !printable(line, ln) || !printable(url, un))
            continue;
        memcpy(links[count].label, line, ln);
        links[count].label[ln] = 0;
        memcpy(links[count].url, url, un);
        links[count].url[un] = 0;
        count++;
    }
    return count;
}
