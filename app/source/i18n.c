#include "i18n.h"

#include <stdio.h>

static const char *const EN[S_COUNT] = {
#define X(id, en, uk) [id] = en,
    STRINGS(X)
#undef X
};
static const char *const UK[S_COUNT] = {
#define X(id, en, uk) [id] = uk,
    STRINGS(X)
#undef X
};

static bool g_uk;

void i18n_set_ukrainian(bool on) { g_uk = on; }
bool i18n_ukrainian(void) { return g_uk; }

const char *T(Str id) { return (unsigned)id < S_COUNT ? (g_uk ? UK : EN)[id] : ""; }

const char *i18n_recordings(unsigned n, char *buf, unsigned cap)
{
    if (!g_uk)
        snprintf(buf, cap, n == 1 ? "%u recording" : "%u recordings", n);
    else {
        const unsigned d = n % 10, dd = n % 100;
        const char *w = d == 1 && dd != 11 ? "запис" : d >= 2 && d <= 4 && (dd < 12 || dd > 14) ? "записи" : "записів";
        snprintf(buf, cap, "%u %s", n, w);
    }
    return buf;
}
