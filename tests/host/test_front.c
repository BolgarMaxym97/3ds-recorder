/* A program that has just started is left alone (sysmod/source/front.h): when the replay buffer
   may start and the processor's speed may be changed or put back. */
#include "front.h"

#include <stdio.h>

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

#define Q 15u   /* the quiet time, in the tests' unit */

int main(void)
{
    /* a program that was there when the module first looked: age unknown, nothing held back */
    {
        FrontApp f = { 0 };
        CHECK(!front_quiet(&f, 0, Q));
        CHECK(!front_note(&f, 44, 100));
        CHECK(!front_quiet(&f, 100, Q));
        CHECK(!front_note(&f, 44, 102));
        CHECK(!front_quiet(&f, 102, Q));
    }
    /* HOME alone first, then a game starts: quiet for Q, then not */
    {
        FrontApp f = { 0 };
        CHECK(!front_note(&f, 0, 100));
        CHECK(!front_quiet(&f, 100, Q));
        CHECK(front_note(&f, 45, 110));
        CHECK(front_quiet(&f, 110, Q));
        CHECK(front_quiet(&f, 124, Q));
        CHECK(!front_quiet(&f, 125, Q));
        CHECK(!front_quiet(&f, 100000, Q));
    }
    /* the first program was old; it closes and the next one starts: that one is new */
    {
        FrontApp f = { 0 };
        front_note(&f, 44, 100);
        CHECK(!front_note(&f, 0, 200));
        CHECK(!front_quiet(&f, 200, Q));
        CHECK(front_note(&f, 46, 210));
        CHECK(front_quiet(&f, 211, Q));
        CHECK(!front_quiet(&f, 226, Q));
    }
    /* the process list fails for a moment (pid 0), the same program is back: not a new start */
    {
        FrontApp f = { 0 };
        front_note(&f, 0, 0);
        front_note(&f, 45, 10);
        CHECK(!front_note(&f, 0, 500));
        CHECK(!front_note(&f, 45, 502));
        CHECK(!front_quiet(&f, 502, Q));
    }
    /* one program straight after another, never "none" in between */
    {
        FrontApp f = { 0 };
        front_note(&f, 44, 0);
        CHECK(front_note(&f, 45, 300));
        CHECK(front_quiet(&f, 305, Q));
    }
    /* pm's 0x10C alone (the module does not look at pids when idle with Replay off) */
    {
        FrontApp f = { 0 };
        CHECK(!front_quiet(&f, 1000, Q));
        front_started(&f, 1000);
        CHECK(front_quiet(&f, 1000, Q));
        CHECK(front_quiet(&f, 1014, Q));
        CHECK(!front_quiet(&f, 1015, Q));
        /* and with the pid seen a little later: the later of the two counts */
        front_note(&f, 0, 1001);
        front_note(&f, 47, 1003);
        CHECK(front_quiet(&f, 1016, Q));
        CHECK(!front_quiet(&f, 1018, Q));
    }
    /* the processor's speed goes back only for the same program, and only from the recorder's state */
    CHECK(cpu_restore_ok(44, 44, 3));
    CHECK(cpu_restore_ok(0, 0, 3));     /* boosted in HOME alone, still HOME alone */
    CHECK(!cpu_restore_ok(44, 45, 3));  /* another program now */
    CHECK(!cpu_restore_ok(44, 0, 3));   /* the program closed */
    CHECK(!cpu_restore_ok(0, 44, 3));   /* a program started since */
    CHECK(!cpu_restore_ok(44, 44, 0));  /* NS changed the speed itself */
    CHECK(!cpu_restore_ok(44, 44, 1));
    CHECK(!cpu_restore_ok(44, 44, 2));

    /* FCRAM frames held while a program starts (launch_hold); VRAM ones never */
    CHECK(front_hold_fcram(true, true, false));
    CHECK(!front_hold_fcram(true, true, true));    /* HOME Menu's VRAM frames go on */
    CHECK(!front_hold_fcram(true, false, false));  /* loaded: frames again */
    CHECK(!front_hold_fcram(false, true, false));  /* off (the default) */
    /* a game started from HOME during a recording: held for exactly the quiet time */
    {
        FrontApp f = { 0 };
        front_note(&f, 0, 100);
        front_note(&f, 47, 107);
        CHECK(front_hold_fcram(true, front_quiet(&f, 108, Q), false));
        CHECK(front_hold_fcram(true, front_quiet(&f, 121, Q), false));
        CHECK(!front_hold_fcram(true, front_quiet(&f, 122, Q), false));
    }

    /* both screens at one FCRAM address: unsettled; VRAM or different addresses: fine */
    CHECK(screens_unsettled(0x203A0C80u, false, 0x203A0C80u, false));
    CHECK(!screens_unsettled(0x20339730u, false, 0x203A0C80u, false));
    CHECK(!screens_unsettled(0x18273000u, true, 0x18273000u, true));
    CHECK(!screens_unsettled(0x20339730u, false, 0x1802CA50u, true));

    /* 804 MHz + L2 only with a program in front that has loaded: never in HOME Menu */
    CHECK(!cpu_boost_now(0, false));
    CHECK(!cpu_boost_now(47, true));
    CHECK(cpu_boost_now(47, false));

    /* the APPLICATION region's end: the region on a New 3DS, the limit on an Old 3DS */
    CHECK(app_region_end(6, 0x04000000u) == 0x27C00000u);   /* CTGP-7: 64 MB limit, frames at 26D26E00 */
    CHECK(app_region_end(6, 0x07C00000u) == 0x27C00000u);
    CHECK(app_region_end(7, 0x05000000u) == 0x2B200000u);
    CHECK(app_region_end(0, 0x04000000u) == 0x24000000u);   /* Old 3DS 64 MB */
    CHECK(app_region_end(3, 0x05000000u) == 0x25000000u);   /* Old 3DS 80 MB */
    /* pm's limit left under the New 3DS mode's size */
    CHECK(app_limit_stuck(6, 0x04000000u));
    CHECK(app_limit_stuck(6, 0x05000000u));
    CHECK(!app_limit_stuck(6, 0x07C00000u));
    CHECK(!app_limit_stuck(0, 0x04000000u));
    CHECK(!app_limit_stuck(2, 0x06000000u));
    /* what AppMemFix puts back: the mode's own size */
    CHECK(app_region_end(6, 0) - 0x20000000u == 0x07C00000u);
    CHECK(app_region_end(7, 0) - 0x20000000u == 0x0B200000u);
    CHECK(!app_limit_stuck(6, app_region_end(6, 0) - 0x20000000u));

    /* a picture is taken: new swap, still VRAM refresh, late; a pending swap once it has stayed */
    CHECK(capture_trigger(true, false, false, false, 0, 100000u));
    CHECK(capture_trigger(false, true, false, false, 0, 100000u));
    CHECK(capture_trigger(false, false, true, true, 0, 100000u));
    CHECK(!capture_trigger(false, false, false, false, 500000u, 100000u));   /* nothing pending */
    CHECK(!capture_trigger(false, false, false, true, 99999u, 100000u));     /* pending, not yet still */
    CHECK(capture_trigger(false, false, false, true, 100000u, 100000u));     /* the game stopped drawing */

    /* a CTRPluginFramework menu: RGB565, 480-byte rows, the top screen in 2D, whole screens */
    CHECK(ctrpf_display_layout(0, 0x42, 480, 400 * 480));
    CHECK(ctrpf_display_layout(1, 0x02, 480, 320 * 480));
    CHECK(!ctrpf_display_layout(0, 0x02, 480, 400 * 480));   /* top not in the 2D mode */
    CHECK(!ctrpf_display_layout(0, 0x41, 720, 400 * 720));   /* BGR8: a game's own */
    CHECK(!ctrpf_display_layout(0, 0x42, 480, 400 * 480 - 1));
    CHECK(!ctrpf_display_layout(2, 0x02, 480, 320 * 480));
    CHECK(ctrpf_display_buffer(0, 0) == 0x06000000u && ctrpf_display_buffer(0, 1) == 0x0602EE00u);
    CHECK(ctrpf_display_buffer(1, 0) == 0x0605DC00u && ctrpf_display_buffer(1, 1) == 0x06083400u);

    /* a refused buffer is left alone a moment, the others not */
    {
        PaBackoff b = { { 0 }, { 0 } };
        CHECK(!pa_backoff_active(&b, 0x20205490u, 0));
        pa_backoff_set(&b, 0x20205490u, 500);
        CHECK(pa_backoff_active(&b, 0x20205490u, 499));
        CHECK(!pa_backoff_active(&b, 0x20205490u, 500));
        CHECK(!pa_backoff_active(&b, 0x2024B9A0u, 100));   /* the game's other buffer goes on */
        pa_backoff_set(&b, 0x20205490u, 900);              /* refused again: the same entry, later */
        CHECK(pa_backoff_active(&b, 0x20205490u, 800));
        for (uint32_t i = 1; i <= 4; i++)
            pa_backoff_set(&b, 0x21000000u + i, 1000 + i);  /* more buffers than entries: the first to end goes */
        CHECK(pa_backoff_active(&b, 0x21000004u, 1000));
        CHECK(!pa_backoff_active(&b, 0x20205490u, 800));
    }

    if (!g_fail)
        printf("test_front: ok\n");
    return g_fail;
}
