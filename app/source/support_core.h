/* Help's support links as the Worker sends them (GET /v1/support, from .github/FUNDING.yml):
 * one "<label> <url>" a line. Pure: tests/host/test_support.c. */
#ifndef SUPPORT_CORE_H
#define SUPPORT_CORE_H

#include <stddef.h>

#define SUPPORT_MAX 3

typedef struct {
    char label[24];
    char url[96];
} SupportLink;

/* The links in `text` (at most `max`), how many. A line that is not "<label> https://..." in
 * printable ASCII is skipped. */
int support_parse(const char *text, size_t len, SupportLink *links, int max);

#endif
