/*
 * Hotkeys: their buttons, how config.ini writes them (R+SELECT), and which combinations may be
 * used. No libctru here (tests/host/test_hotkey.c); the button bits are the HID ones (libctru
 * KEY_*, checked in app.c). ZL / ZR exist on New 3DS only and are offered there only.
 */
#ifndef HOTKEY_CORE_H
#define HOTKEY_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HK_A = 1u << 0, HK_B = 1u << 1, HK_SELECT = 1u << 2, HK_START = 1u << 3, HK_RIGHT = 1u << 4,
    HK_LEFT = 1u << 5, HK_UP = 1u << 6, HK_DOWN = 1u << 7, HK_R = 1u << 8, HK_L = 1u << 9,
    HK_X = 1u << 10, HK_Y = 1u << 11, HK_ZL = 1u << 14, HK_ZR = 1u << 15,
};

#define HK_DEFAULT "R+SELECT"
#define HK_REPLAY_DEFAULT "X+SELECT"
#define HK_ZLZR (HK_ZL | HK_ZR)

/* why a combination may not be a hotkey */
typedef enum {
    HK_OK,
    HK_ERR_LR,       /* L + R: the Camera in HOME */
    HK_ERR_ROSALINA, /* L + ↓ + SELECT or a part of it: the Rosalina menu */
    HK_ERR_FACE,     /* only A / B / X / Y: pressed in games all the time */
    HK_ERR_OTHER,    /* holds every button of the other hotkey (or the other way): both would fire */
    HK_ERR_COUNT,    /* not 2-3 buttons (1-3 with ZL or ZR) */
} HkProblem;

typedef struct {
    uint32_t key;
    const char *name, *label; /* config.ini; on the screen */
} HkKey;

extern const HkKey HK_KEYS[];
extern const int HK_COUNT;

/* the buttons a hotkey may hold: ZL / ZR only when the console has them */
uint32_t hk_allowed(bool zlzr);
uint32_t hk_mask(const char *text);                 /* "R+SELECT" -> bits; unknown names left out */
void hk_text(uint32_t m, char *buf, size_t cap);    /* bits -> "R+SELECT" */
void hk_label(uint32_t m, char *buf, size_t cap);   /* bits -> "R + SELECT" */
int hk_count(uint32_t m);
bool hk_overlap(uint32_t a, uint32_t b);
/* a combination as a whole: HK_OK or the first rule it breaks (`other`: the other hotkey, 0: none) */
HkProblem hk_check(uint32_t m, uint32_t other);
/* the replay hotkey's default, or the next one that does not overlap the recording hotkey */
uint32_t hk_replay_default(uint32_t rec);

#endif
