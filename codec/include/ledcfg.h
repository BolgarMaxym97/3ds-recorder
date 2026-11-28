/*
 * The notification LED's settings (LED Studio, redesign 2.0), shared by the module (sysmod), which
 * lights it, and the app, which edits it: one config.ini key per event,
 *     led_rec=on,breathe,ff0000,5,normal
 * (on|off, the pattern, the colour, brightness 1-5, speed slow|normal|fast). An event without its
 * key behaves as 1.7 did, from the old key led=pulse|solid|off (ledcfg_defaults): nobody who set
 * that sees anything change. Header only, no libctru (tests/host/test_ledcfg.c).
 */
#ifndef LEDCFG_H
#define LEDCFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

enum { LED_EV_READY, LED_EV_REC, LED_EV_REPLAY, LED_EV_SAVED, LED_EV_ERROR, LED_EV_COUNT };
/* Solid and Breathe last (the recording, the replay buffer); Glow and the blinks are short signals
   (ready, saved, error) the module plays step by step. ledcfg_pattern_ok says which go where. */
enum { LED_PAT_SOLID, LED_PAT_BREATHE, LED_PAT_GLOW, LED_PAT_BLINK2, LED_PAT_BLINK3, LED_PAT_COUNT };
enum { LED_SPEED_SLOW, LED_SPEED_NORMAL, LED_SPEED_FAST };

typedef struct {
    bool on;
    uint8_t pattern, r, g, b, bright, speed;
} LedEvent;

static const char *const LEDCFG_KEY[LED_EV_COUNT] = {"led_ready", "led_rec", "led_replay", "led_saved", "led_error"};
static const char *const LEDCFG_PAT[LED_PAT_COUNT] = {"solid", "breathe", "glow", "blink2", "blink3"};
static const char *const LEDCFG_SPEED[3] = {"slow", "normal", "fast"};
/* brightness 1-5 as the MCU's 0-255; 1 is the replay buffer's faint blue of 1.7 (0x28) */
static const uint8_t LEDCFG_LEVEL[6] = {0, 0x28, 0x50, 0x82, 0xBE, 0xFF};

/* the recording and the replay buffer stay lit; the others are signals */
static inline bool ledcfg_lasting(int ev) { return ev == LED_EV_REC || ev == LED_EV_REPLAY; }

static inline bool ledcfg_pattern_ok(int ev, int pattern)
{
    return ledcfg_lasting(ev) ? pattern == LED_PAT_SOLID || pattern == LED_PAT_BREATHE : pattern != LED_PAT_BREATHE;
}

/* What 1.7 showed, from its led= (0 off, 1 solid, 2 pulse): blue for 1.5 s when ready, the red
   breath (solid red, nothing) while recording, faint steady blue for the replay buffer (nothing
   with led=off), green twice when saved, red three times on an error. */
static inline void ledcfg_defaults(LedEvent ev[LED_EV_COUNT], int legacyLed)
{
    const LedEvent d[LED_EV_COUNT] = {
        [LED_EV_READY] = {true, LED_PAT_GLOW, 0x00, 0x00, 0xFF, 5, LED_SPEED_NORMAL},
        [LED_EV_REC] = {legacyLed != 0, legacyLed == 1 ? LED_PAT_SOLID : LED_PAT_BREATHE, 0xFF, 0x00, 0x00, 5, LED_SPEED_NORMAL},
        [LED_EV_REPLAY] = {legacyLed != 0, LED_PAT_SOLID, 0x00, 0x00, 0xFF, 1, LED_SPEED_NORMAL},
        [LED_EV_SAVED] = {true, LED_PAT_BLINK2, 0x00, 0xFF, 0x00, 5, LED_SPEED_NORMAL},
        [LED_EV_ERROR] = {true, LED_PAT_BLINK3, 0xFF, 0x00, 0x00, 5, LED_SPEED_NORMAL},
    };
    memcpy(ev, d, sizeof(d));
}

static inline int ledcfg_event(const char *key)
{
    for (int i = 0; i < LED_EV_COUNT; i++)
        if (strcasecmp(key, LEDCFG_KEY[i]) == 0)
            return i;
    return -1;
}

/* "on,breathe,ff0000,5,normal" into e; false (e untouched) when it is not that */
static inline bool ledcfg_parse(int ev, const char *v, LedEvent *e)
{
    char buf[64], *save = NULL, *t[5];
    snprintf(buf, sizeof(buf), "%s", v ? v : "");
    int n = 0;
    for (char *p = strtok_r(buf, ", \t", &save); p && n < 5; p = strtok_r(NULL, ", \t", &save))
        t[n++] = p;
    if (n != 5 || strlen(t[2]) != 6)
        return false;
    LedEvent x;
    if (strcasecmp(t[0], "on") == 0)
        x.on = true;
    else if (strcasecmp(t[0], "off") == 0)
        x.on = false;
    else
        return false;
    int p = -1, s = -1;
    for (int i = 0; i < LED_PAT_COUNT; i++)
        if (strcasecmp(t[1], LEDCFG_PAT[i]) == 0)
            p = i;
    for (int i = 0; i < 3; i++)
        if (strcasecmp(t[4], LEDCFG_SPEED[i]) == 0)
            s = i;
    char *end = NULL;
    const unsigned long rgb = strtoul(t[2], &end, 16);
    const int br = atoi(t[3]);
    if (p < 0 || s < 0 || !end || *end || br < 1 || br > 5 || !ledcfg_pattern_ok(ev, p))
        return false;
    x.pattern = (uint8_t)p;
    x.r = (uint8_t)(rgb >> 16);
    x.g = (uint8_t)(rgb >> 8);
    x.b = (uint8_t)rgb;
    x.bright = (uint8_t)br;
    x.speed = (uint8_t)s;
    *e = x;
    return true;
}

static inline void ledcfg_format(const LedEvent *e, char *buf, size_t cap)
{
    snprintf(buf, cap, "%s,%s,%02x%02x%02x,%u,%s", e->on ? "on" : "off", LEDCFG_PAT[e->pattern % LED_PAT_COUNT], e->r, e->g,
             e->b, (unsigned)e->bright, LEDCFG_SPEED[e->speed % 3]);
}

/* the colour at its brightness, 0-255 a channel */
static inline uint8_t ledcfg_channel(const LedEvent *e, uint8_t c) { return (uint8_t)(c * LEDCFG_LEVEL[e->bright > 5 ? 5 : e->bright] / 255); }

/* A lasting light as the MCU's 32-step pattern: steady, or a breath (dim, up, down) like 1.7's
   (8..255 of the colour). *delay is the step's length in 1/16 s: normal 8 (the pattern takes 16 s,
   which the module writes again every 16 s, LedKeep), slow 16, fast 4. */
static inline void ledcfg_lasting_pattern(const LedEvent *e, uint8_t r[32], uint8_t g[32], uint8_t b[32], uint8_t *delay,
                                          uint8_t *smoothing)
{
    for (int i = 0; i < 32; i++) {
        unsigned k = 255; /* 0-255 of the colour */
        if (e->on && e->pattern == LED_PAT_BREATHE) {
            const unsigned t = i <= 16 ? (unsigned)i : 32u - (unsigned)i; /* 0..16..0 */
            k = 8 + t * 247 / 16;
        }
        r[i] = e->on ? (uint8_t)(ledcfg_channel(e, e->r) * k / 255) : 0;
        g[i] = e->on ? (uint8_t)(ledcfg_channel(e, e->g) * k / 255) : 0;
        b[i] = e->on ? (uint8_t)(ledcfg_channel(e, e->b) * k / 255) : 0;
    }
    const bool breathe = e->on && e->pattern == LED_PAT_BREATHE;
    *delay = !breathe ? 0x10 : e->speed == LED_SPEED_SLOW ? 0x10 : e->speed == LED_SPEED_FAST ? 0x04 : 0x08;
    *smoothing = breathe ? 0x40 : 0;
}

/* how long the lasting pattern takes, ms (the module writes it again then) */
static inline uint32_t ledcfg_cycle_ms(uint8_t delay) { return 32u * delay * 1000u / 16u; }

/* A signal as steady steps the module switches itself (one-shot MCU patterns play too fast to see,
   O3DS 2026-11-03): 1.7's timings. Off steps are 0,0,0. Returns the step count (at most 6). */
typedef struct {
    uint8_t r, g, b;
    uint16_t ms;
} LedcfgStep;

static inline int ledcfg_signal(const LedEvent *e, LedcfgStep out[6])
{
    if (!e->on)
        return 0;
    const uint8_t r = ledcfg_channel(e, e->r), g = ledcfg_channel(e, e->g), b = ledcfg_channel(e, e->b);
    int n = 0;
    if (e->pattern == LED_PAT_BLINK2 || e->pattern == LED_PAT_BLINK3) {
        const bool two = e->pattern == LED_PAT_BLINK2;
        const int times = two ? 2 : 3;
        for (int i = 0; i < times; i++) {
            if (i)
                out[n++] = (LedcfgStep){0, 0, 0, (uint16_t)(two ? 300 : 200)};
            out[n++] = (LedcfgStep){r, g, b, (uint16_t)(two ? 600 : 300)};
        }
    } else /* Glow, Solid: steady for 1.5 s */
        out[n++] = (LedcfgStep){r, g, b, 1500};
    return n;
}

#endif
