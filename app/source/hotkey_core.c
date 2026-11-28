#include "hotkey_core.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

/* in the order config.ini lists them (R+SELECT) */
const HkKey HK_KEYS[] = {
    {HK_ZL, "ZL", "ZL"}, {HK_ZR, "ZR", "ZR"}, {HK_L, "L", "L"}, {HK_R, "R", "R"}, {HK_UP, "UP", "↑"},
    {HK_DOWN, "DOWN", "↓"}, {HK_LEFT, "LEFT", "←"}, {HK_RIGHT, "RIGHT", "→"}, {HK_A, "A", "A"},
    {HK_B, "B", "B"}, {HK_X, "X", "X"}, {HK_Y, "Y", "Y"}, {HK_SELECT, "SELECT", "SELECT"},
};
const int HK_COUNT = (int)(sizeof(HK_KEYS) / sizeof(HK_KEYS[0]));

uint32_t hk_allowed(bool zlzr)
{
    const uint32_t m = HK_L | HK_R | HK_UP | HK_DOWN | HK_LEFT | HK_RIGHT | HK_A | HK_B | HK_X | HK_Y | HK_SELECT;
    return zlzr ? m | HK_ZLZR : m;
}

uint32_t hk_mask(const char *text)
{
    uint32_t m = 0;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    char *save = NULL;
    for (char *t = strtok_r(buf, "+ \t", &save); t; t = strtok_r(NULL, "+ \t", &save))
        for (int i = 0; i < HK_COUNT; i++)
            if (strcasecmp(t, HK_KEYS[i].name) == 0)
                m |= HK_KEYS[i].key;
    return m;
}

static void join(uint32_t m, char *buf, size_t cap, const char *sep, bool label)
{
    size_t o = 0;
    if (cap)
        buf[0] = 0;
    for (int i = 0; i < HK_COUNT && o < cap; i++)
        if (m & HK_KEYS[i].key)
            o += (size_t)snprintf(buf + o, cap - o, "%s%s", o ? sep : "", label ? HK_KEYS[i].label : HK_KEYS[i].name);
}

void hk_text(uint32_t m, char *buf, size_t cap) { join(m, buf, cap, "+", false); }

void hk_label(uint32_t m, char *buf, size_t cap) { join(m, buf, cap, " + ", true); }

int hk_count(uint32_t m)
{
    int n = 0;
    for (int i = 0; i < HK_COUNT; i++)
        n += (m & HK_KEYS[i].key) != 0;
    return n;
}

bool hk_overlap(uint32_t a, uint32_t b) { return a && b && ((a & b) == a || (a & b) == b); }

HkProblem hk_check(uint32_t m, uint32_t other)
{
    const int n = hk_count(m);
    if (n < ((m & HK_ZLZR) ? 1 : 2) || n > 3)
        return HK_ERR_COUNT;
    if ((m & HK_L) && (m & HK_R))
        return HK_ERR_LR;
    if (!(m & ~(uint32_t)(HK_L | HK_DOWN | HK_SELECT)))
        return HK_ERR_ROSALINA;
    if (!(m & ~(uint32_t)(HK_A | HK_B | HK_X | HK_Y)))
        return HK_ERR_FACE;
    if (hk_overlap(m, other))
        return HK_ERR_OTHER;
    return HK_OK;
}

uint32_t hk_replay_default(uint32_t rec)
{
    static const char *const tries[] = {HK_REPLAY_DEFAULT, "Y+SELECT", "L+X", "R+Y"};
    for (size_t i = 0; i < sizeof(tries) / sizeof(*tries); i++)
        if (!hk_overlap(hk_mask(tries[i]), rec))
            return hk_mask(tries[i]);
    return hk_mask(HK_REPLAY_DEFAULT);
}
