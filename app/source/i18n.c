#include "i18n.h"

#include <stdio.h>
#include <strings.h>

extern const char *const TEXT_UK[S_COUNT], *const TEXT_DE[S_COUNT], *const TEXT_ES[S_COUNT], *const TEXT_FR[S_COUNT], *const TEXT_IT[S_COUNT], *const TEXT_PT[S_COUNT], *const TEXT_JA[S_COUNT], *const TEXT_ZH_HANS[S_COUNT], *const TEXT_ZH_HANT[S_COUNT];

const LangInfo LANGS[LANG_COUNT] = {
    [LANG_EN] = {"en", "English", NULL, SCRIPT_LATIN},
    [LANG_UK] = {"uk", "Українська", TEXT_UK, SCRIPT_LATIN},
    [LANG_DE] = {"de", "Deutsch", TEXT_DE, SCRIPT_LATIN},
    [LANG_ES] = {"es", "Español", TEXT_ES, SCRIPT_LATIN},
    [LANG_FR] = {"fr", "Français", TEXT_FR, SCRIPT_LATIN},
    [LANG_IT] = {"it", "Italiano", TEXT_IT, SCRIPT_LATIN},
    [LANG_PT] = {"pt", "Português", TEXT_PT, SCRIPT_LATIN},
    [LANG_JA] = {"ja", "日本語", TEXT_JA, SCRIPT_JA},
    [LANG_ZH_HANS] = {"zh-Hans", "简体中文", TEXT_ZH_HANS, SCRIPT_ZH_HANS},
    [LANG_ZH_HANT] = {"zh-Hant", "繁體中文", TEXT_ZH_HANT, SCRIPT_ZH_HANT},
};

static const char *const TEXT_EN[S_COUNT] = {
#define X(id, en) [id] = en,
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

Lang i18n_lang_from_system(int cfgLanguage)
{
    static const char *const CODE[12] = {"ja", "en", "fr", "de", "it", "es", "zh-Hans", "", "", "pt", "", "zh-Hant"};
    return cfgLanguage >= 0 && cfgLanguage < 12 && CODE[cfgLanguage][0] ? i18n_lang_from_code(CODE[cfgLanguage])
                                                                         : LANG_EN;
}

const char *T_in(Lang lang, Str id)
{
    if ((unsigned)id >= S_COUNT)
        return "";
    const char *const *t = (unsigned)lang < LANG_COUNT ? LANGS[lang].text : NULL;
    return t && t[id] ? t[id] : TEXT_EN[id];
}

const char *T(Str id) { return T_in(g_lang, id); }

/* CLDR cardinal rules, as far as these languages need them */
const char *i18n_recordings(unsigned n, char *buf, unsigned cap)
{
    const char *code = LANGS[g_lang].code;
    Str form = S_N_REC_MANY;
    if (!strcasecmp(code, "uk")) {
        const unsigned d = n % 10, dd = n % 100;
        form = d == 1 && dd != 11 ? S_N_REC_ONE : d >= 2 && d <= 4 && (dd < 12 || dd > 14) ? S_N_REC_FEW : S_N_REC_MANY;
    } else if (!strcasecmp(code, "fr") || !strcasecmp(code, "pt"))
        form = n <= 1 ? S_N_REC_ONE : S_N_REC_MANY; /* 0 and 1 are singular */
    else if (LANGS[g_lang].script == SCRIPT_LATIN)
        form = n == 1 ? S_N_REC_ONE : S_N_REC_MANY;
    snprintf(buf, cap, T(form), n);
    return buf;
}

const char *i18n_date(int y, int mo, int d, bool year, char *buf, unsigned cap)
{
    static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char *code = LANGS[g_lang].code;
    if (mo < 1 || mo > 12)
        mo = 1;
    if (g_lang == LANG_EN)
        year ? snprintf(buf, cap, "%s %d, %04d", MON[mo - 1], d, y) : snprintf(buf, cap, "%s %d", MON[mo - 1], d);
    else if (LANGS[g_lang].script != SCRIPT_LATIN) /* ja, zh: year first */
        year ? snprintf(buf, cap, "%04d/%02d/%02d", y, mo, d) : snprintf(buf, cap, "%02d/%02d", mo, d);
    else if (!strcasecmp(code, "uk") || !strcasecmp(code, "de")) /* de writes "03.11." without a year */
        year ? snprintf(buf, cap, "%02d.%02d.%04d", d, mo, y)
             : snprintf(buf, cap, *code == 'd' ? "%02d.%02d." : "%02d.%02d", d, mo);
    else /* es, fr, it, pt */
        year ? snprintf(buf, cap, "%02d/%02d/%04d", d, mo, y) : snprintf(buf, cap, "%02d/%02d", d, mo);
    return buf;
}

char i18n_decimal(void)
{
    return g_lang == LANG_EN || LANGS[g_lang].script != SCRIPT_LATIN ? '.' : ',';
}
