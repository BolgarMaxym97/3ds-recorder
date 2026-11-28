/* codec/include/ledcfg.h: LED Studio's settings, and that nothing changes for who set led= in 1.7. */
#include "ledcfg.h"

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
    LedEvent ev[LED_EV_COUNT], e;
    char buf[64];
    uint8_t r[32], g[32], b[32], delay, sm;
    LedcfgStep st[6];

    /* led=pulse (the default): 1.7's lights exactly */
    ledcfg_defaults(ev, 2);
    CHECK(ev[LED_EV_REC].on && ev[LED_EV_REC].pattern == LED_PAT_BREATHE);
    ledcfg_lasting_pattern(&ev[LED_EV_REC], r, g, b, &delay, &sm);
    for (int i = 0; i < 32; i++) { /* the breath 1.7 wrote: 8 + t * 247 / 16 */
        const unsigned t = i <= 16 ? (unsigned)i : 32u - (unsigned)i;
        CHECK(r[i] == (uint8_t)(8 + t * 247 / 16) && g[i] == 0 && b[i] == 0);
    }
    CHECK(delay == 0x08 && sm == 0x40 && ledcfg_cycle_ms(delay) == 16000);
    ledcfg_lasting_pattern(&ev[LED_EV_REPLAY], r, g, b, &delay, &sm);
    CHECK(b[0] == 0x28 && b[31] == 0x28 && r[0] == 0 && delay == 0x10 && sm == 0); /* faint steady blue */
    CHECK(ledcfg_signal(&ev[LED_EV_READY], st) == 1 && st[0].b == 0xFF && st[0].ms == 1500);
    CHECK(ledcfg_signal(&ev[LED_EV_SAVED], st) == 3 && st[0].g == 0xFF && st[0].ms == 600 && st[1].g == 0 &&
          st[1].ms == 300);
    CHECK(ledcfg_signal(&ev[LED_EV_ERROR], st) == 5 && st[4].r == 0xFF && st[4].ms == 300 && st[3].ms == 200);

    /* led=solid: solid red; led=off: no recording or replay light, the signals as before */
    ledcfg_defaults(ev, 1);
    CHECK(ev[LED_EV_REC].on && ev[LED_EV_REC].pattern == LED_PAT_SOLID);
    ledcfg_lasting_pattern(&ev[LED_EV_REC], r, g, b, &delay, &sm);
    CHECK(r[0] == 0xFF && r[16] == 0xFF && sm == 0);
    ledcfg_defaults(ev, 0);
    CHECK(!ev[LED_EV_REC].on && !ev[LED_EV_REPLAY].on && ev[LED_EV_SAVED].on && ev[LED_EV_ERROR].on && ev[LED_EV_READY].on);
    ledcfg_lasting_pattern(&ev[LED_EV_REC], r, g, b, &delay, &sm);
    CHECK(r[5] == 0 && g[5] == 0 && b[5] == 0);

    /* the key's form, both ways */
    CHECK(ledcfg_event("led_rec") == LED_EV_REC && ledcfg_event("LED_ERROR") == LED_EV_ERROR && ledcfg_event("led") < 0);
    CHECK(ledcfg_parse(LED_EV_REC, "on,breathe,ff3b3b,3,slow", &e));
    CHECK(e.on && e.pattern == LED_PAT_BREATHE && e.r == 0xFF && e.g == 0x3B && e.b == 0x3B && e.bright == 3 &&
          e.speed == LED_SPEED_SLOW);
    ledcfg_format(&e, buf, sizeof(buf));
    CHECK(strcmp(buf, "on,breathe,ff3b3b,3,slow") == 0);
    CHECK(ledcfg_parse(LED_EV_SAVED, "off, blink2, 00ff00, 5, normal", &e) && !e.on);
    CHECK(!ledcfg_parse(LED_EV_REC, "on,blink2,ff0000,5,normal", &e));   /* a signal's pattern for a lasting light */
    CHECK(!ledcfg_parse(LED_EV_READY, "on,breathe,0000ff,5,normal", &e)); /* and the other way */
    CHECK(!ledcfg_parse(LED_EV_REC, "on,solid,red,5,normal", &e));
    CHECK(!ledcfg_parse(LED_EV_REC, "on,solid,ff0000,6,normal", &e));
    CHECK(!ledcfg_parse(LED_EV_REC, "maybe,solid,ff0000,5,normal", &e));
    CHECK(!ledcfg_parse(LED_EV_REC, "", &e) && !ledcfg_parse(LED_EV_REC, NULL, &e));

    /* speed: slow / fast keep a whole breath in what the module writes again */
    e = (LedEvent){true, LED_PAT_BREATHE, 0xFF, 0, 0, 5, LED_SPEED_SLOW};
    ledcfg_lasting_pattern(&e, r, g, b, &delay, &sm);
    CHECK(ledcfg_cycle_ms(delay) == 32000);
    e.speed = LED_SPEED_FAST;
    ledcfg_lasting_pattern(&e, r, g, b, &delay, &sm);
    CHECK(ledcfg_cycle_ms(delay) == 8000);
    /* brightness scales the colour */
    e = (LedEvent){true, LED_PAT_SOLID, 0xFF, 0x80, 0, 3, LED_SPEED_NORMAL};
    ledcfg_lasting_pattern(&e, r, g, b, &delay, &sm);
    CHECK(r[0] == 0x82 && g[0] == 0x80 * 0x82 / 255);
    /* off: no steps */
    e.on = false;
    CHECK(ledcfg_signal(&e, st) == 0);

    if (fails) {
        printf("test_ledcfg: %d failed\n", fails);
        return 1;
    }
    printf("test_ledcfg: ok\n");
    return 0;
}
