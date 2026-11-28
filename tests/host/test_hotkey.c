/* app/source/hotkey_core.c: the rules a hotkey follows (the same as 1.7, plus ZL / ZR on New 3DS). */
#include "hotkey_core.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            fails++;                                                                                                   \
        }                                                                                                              \
    } while (0)

int main(void)
{
    char buf[64];

    /* config.ini form and back, as 1.7 wrote it */
    CHECK(hk_mask("R+SELECT") == (HK_R | HK_SELECT));
    CHECK(hk_mask("select+r") == (HK_R | HK_SELECT));
    CHECK(hk_mask("L+DOWN+SELECT") == (HK_L | HK_DOWN | HK_SELECT));
    CHECK(hk_mask("R+BOGUS") == HK_R);
    CHECK(hk_mask("") == 0 && hk_mask(NULL) == 0);
    CHECK(hk_mask("R + SELECT") == (HK_R | HK_SELECT));   /* hand-written, as the module reads it */
    hk_text(HK_SELECT | HK_R, buf, sizeof(buf));
    CHECK(strcmp(buf, "R+SELECT") == 0);
    hk_label(HK_SELECT | HK_X, buf, sizeof(buf));
    CHECK(strcmp(buf, "X + SELECT") == 0);
    hk_text(HK_ZL | HK_A, buf, sizeof(buf));
    CHECK(strcmp(buf, "ZL+A") == 0 && hk_mask(buf) == (HK_ZL | HK_A));

    /* the rules */
    CHECK(hk_check(hk_mask(HK_DEFAULT), 0) == HK_OK);
    CHECK(hk_check(hk_mask(HK_REPLAY_DEFAULT), hk_mask(HK_DEFAULT)) == HK_OK);
    CHECK(hk_check(HK_L | HK_R, 0) == HK_ERR_LR);
    CHECK(hk_check(HK_L | HK_R | HK_A, 0) == HK_ERR_LR);
    CHECK(hk_check(HK_L | HK_DOWN | HK_SELECT, 0) == HK_ERR_ROSALINA);
    CHECK(hk_check(HK_L | HK_SELECT, 0) == HK_ERR_ROSALINA);
    CHECK(hk_check(HK_DOWN | HK_SELECT, 0) == HK_ERR_ROSALINA);
    CHECK(hk_check(HK_A | HK_B, 0) == HK_ERR_FACE);
    CHECK(hk_check(HK_A | HK_B | HK_X, 0) == HK_ERR_FACE);
    CHECK(hk_check(HK_L | HK_Y, 0) == HK_OK);
    CHECK(hk_check(HK_R, 0) == HK_ERR_COUNT);                         /* one button: too easy */
    CHECK(hk_check(HK_L | HK_UP | HK_A | HK_SELECT, 0) == HK_ERR_COUNT); /* four */
    CHECK(hk_check(HK_X | HK_SELECT, HK_X | HK_SELECT | HK_R) == HK_ERR_OTHER);
    CHECK(hk_check(HK_R | HK_SELECT, HK_R | HK_SELECT) == HK_ERR_OTHER);
    CHECK(hk_check(HK_X | HK_SELECT, HK_R | HK_SELECT) == HK_OK);    /* sharing SELECT is fine */

    /* ZL / ZR: one button is enough with them */
    CHECK(hk_check(HK_ZL, 0) == HK_OK);
    CHECK(hk_check(HK_ZR, HK_ZL) == HK_OK);
    CHECK(hk_check(HK_ZL, HK_ZL | HK_A) == HK_ERR_OTHER);
    CHECK(hk_check(HK_ZL | HK_ZR | HK_A | HK_B, 0) == HK_ERR_COUNT);
    CHECK((hk_allowed(false) & HK_ZLZR) == 0 && (hk_allowed(true) & HK_ZLZR) == HK_ZLZR);
    CHECK((hk_allowed(true) & HK_START) == 0);

    /* the replay default steps aside from the recording hotkey */
    CHECK(hk_replay_default(hk_mask("R+SELECT")) == hk_mask("X+SELECT"));
    CHECK(hk_replay_default(hk_mask("X+SELECT")) == hk_mask("Y+SELECT"));
    CHECK(hk_replay_default(hk_mask("SELECT+X+Y")) == hk_mask("L+X"));

    if (fails) {
        printf("test_hotkey: %d failed\n", fails);
        return 1;
    }
    printf("test_hotkey: ok\n");
    return 0;
}
