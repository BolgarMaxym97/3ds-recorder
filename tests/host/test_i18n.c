/* app/source/i18n.c: languages, English for a missing text, plurals, dates */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "i18n.h"

#define EQ(got, want)                                                                         \
    do {                                                                                      \
        const char *g_ = (got);                                                               \
        if (strcmp(g_, want)) {                                                               \
            printf("FAIL %s:%d: \"%s\", want \"%s\"\n", __FILE__, __LINE__, g_, want);       \
            return 1;                                                                         \
        }                                                                                     \
    } while (0)

int main(void)
{
    char b[64];
    /* codes */
    assert(i18n_lang_from_code("uk") == LANG_UK && i18n_lang_from_code("UK") == LANG_UK);
    assert(i18n_lang_from_code("xx") == LANG_EN && i18n_lang_from_code("") == LANG_EN);
    for (int i = 0; i < LANG_COUNT; i++) {
        assert(i18n_lang_from_code(LANGS[i].code) == (Lang)i);
        assert(LANGS[i].name && LANGS[i].name[0]);
        assert((i == LANG_EN) == (LANGS[i].text == NULL));
    }
    /* the console's language: English for one there is no translation of */
    assert(i18n_lang_from_system(1) == LANG_EN);
    assert(i18n_lang_from_system(10) == LANG_EN); /* Russian */
    assert(i18n_lang_from_system(0) == LANG_JA && i18n_lang_from_system(2) == LANG_FR);
    assert(i18n_lang_from_system(3) == LANG_DE && i18n_lang_from_system(4) == LANG_IT);
    assert(i18n_lang_from_system(5) == LANG_ES && i18n_lang_from_system(9) == LANG_PT);
    assert(i18n_lang_from_system(6) == LANG_ZH_HANS && i18n_lang_from_system(11) == LANG_ZH_HANT);
    assert(i18n_lang_from_system(7) == LANG_EN && i18n_lang_from_system(8) == LANG_EN); /* Korean, Dutch */
    assert(i18n_lang_from_code("zh-hant") == LANG_ZH_HANT);
    assert(i18n_lang_from_system(-1) == LANG_EN && i18n_lang_from_system(99) == LANG_EN);
    for (int s = 0; s < 12; s++) /* each maps to a language that exists */
        assert((unsigned)i18n_lang_from_system(s) < LANG_COUNT);

    /* texts: every language has every key (a NULL would show English) */
    for (int i = 0; i < LANG_COUNT; i++)
        for (int k = 0; k < S_COUNT; k++)
            assert(T_in((Lang)i, (Str)k)[0] || k == S_COUNT);
    EQ(T_in(LANG_UK, S_EXIT), "Вийти");
    EQ(T_in(LANG_EN, S_EXIT), "Exit");
    EQ(T_in((Lang)99, S_EXIT), "Exit");
    EQ(T_in(LANG_EN, (Str)S_COUNT), "");

    /* plurals */
    i18n_set_lang(LANG_EN);
    EQ(i18n_recordings(0, b, sizeof(b)), "0 recordings");
    EQ(i18n_recordings(1, b, sizeof(b)), "1 recording");
    EQ(i18n_recordings(2, b, sizeof(b)), "2 recordings");
    i18n_set_lang(LANG_UK);
    EQ(i18n_recordings(1, b, sizeof(b)), "1 запис");
    EQ(i18n_recordings(3, b, sizeof(b)), "3 записи");
    EQ(i18n_recordings(5, b, sizeof(b)), "5 записів");
    EQ(i18n_recordings(11, b, sizeof(b)), "11 записів");
    EQ(i18n_recordings(12, b, sizeof(b)), "12 записів");
    EQ(i18n_recordings(21, b, sizeof(b)), "21 запис");
    EQ(i18n_recordings(22, b, sizeof(b)), "22 записи");
    EQ(i18n_recordings(111, b, sizeof(b)), "111 записів");

    /* dates */
    i18n_set_lang(LANG_EN);
    EQ(i18n_date(2026, 11, 3, true, b, sizeof(b)), "Nov 3, 2026");
    EQ(i18n_date(2026, 11, 3, false, b, sizeof(b)), "Nov 3");
    i18n_set_lang(LANG_UK);
    EQ(i18n_date(2026, 11, 3, true, b, sizeof(b)), "03.11.2026");
    EQ(i18n_date(2026, 11, 3, false, b, sizeof(b)), "03.11");
    i18n_set_lang((Lang)99); /* out of range: English */
    assert(i18n_lang() == LANG_EN);
    i18n_set_lang(LANG_EN);
    assert(i18n_decimal() == '.');
    i18n_set_lang(LANG_DE);
    assert(i18n_decimal() == ',');
    i18n_set_lang(LANG_JA);
    assert(i18n_decimal() == '.');
    puts("i18n: ok");
    return 0;
}
