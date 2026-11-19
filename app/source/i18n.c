#include "i18n.h"

#include <stdio.h>
#include <strings.h>

const LangInfo LANGS[LANG_COUNT] = {
    [LANG_EN] = {"en", "English"},
    [LANG_UK] = {"uk", "Українська"},
};

static const char *const TEXT[S_COUNT][LANG_COUNT] = {
#define X(id, en, uk) [id] = {[LANG_EN] = en, [LANG_UK] = uk},
    STRINGS(X)
#undef X
};

static Lang g_lang = LANG_EN;

void i18n_set_lang(Lang lang) { g_lang = (unsigned)lang < LANG_COUNT ? lang : LANG_EN; }
Lang i18n_lang(void) { return g_lang; }

Lang i18n_lang_from_code(const char *code)
{
    for (int i = 0; i < LANG_COUNT; i++)
        if (strcasecmp(code, LANGS[i].code) == 0)
            return (Lang)i;
    return LANG_EN;
}

const char *T(Str id) { return (unsigned)id < S_COUNT ? TEXT[id][g_lang] : ""; }

const char *i18n_recordings(unsigned n, char *buf, unsigned cap)
{
    if (g_lang != LANG_UK)
        snprintf(buf, cap, n == 1 ? "%u recording" : "%u recordings", n);
    else {
        const unsigned d = n % 10, dd = n % 100;
        const char *w = d == 1 && dd != 11 ? "запис" : d >= 2 && d <= 4 && (dd < 12 || dd > 14) ? "записи" : "записів";
        snprintf(buf, cap, "%u %s", n, w);
    }
    return buf;
}
