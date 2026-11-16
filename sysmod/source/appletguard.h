/*
 * Applets and the recorder's memory (the module does the rest, main.c; tests/host/test_appletguard.c
 * runs these on a PC).
 *
 * Applets (Internet Browser, Game Notes, Friend List, ...) take their memory from SYSTEM, where the
 * recorder keeps its buffers (~12 MB in the normal profile). On an Old 3DS HOME Menu ends to make
 * room for the applet, and the Internet Browser stayed on its loading screen with 24.6 MB free while
 * a recording ran (it would have had ~36.6 MB; a user's report and O3DS 2026-11-16).
 *
 * So, when an applet comes (HOME Menu ends, or an applet process starts) while the recorder keeps
 * its buffers in SYSTEM memory:
 *   1. the recording is saved and its memory given back at once; the replay buffer just stops;
 *   2. once the applet has taken what it wants (AG_SETTLE_US: the browser takes its ~36 MB within
 *      0.3 s of its start, 1.3 s after HOME Menu ends), a recording goes on in a new file with the
 *      light profile (~3.7 MB, 4 MB always left free); next to the browser there is no room for it;
 *   3. if the applet then wants more (SYSTEM free under AG_LOW_KB), that recording is saved too and
 *      nothing is recorded until the applet ends;
 *   4. when the applet has ended (HOME Menu back for a moment), a recording goes on in a new file
 *      with the normal profile; the replay buffer starts again by itself.
 *
 * SYSTEM memory is NOT polled while an applet starts: asking the kernel for it (svcGetSystemInfo 0)
 * on every main-loop pass while the browser took its memory panicked the kernel (O3DS 2026-11-16,
 * the same panic routine as the DMA panics of 0.11). The caller reads it only for a light recording
 * next to a running applet (ag_wants_free), at most once a second, and passes AG_FREE_UNKNOWN
 * otherwise.
 */
#ifndef APPLETGUARD_H
#define APPLETGUARD_H

#include <stdbool.h>
#include <stdint.h>

#define AG_SETTLE_US   3000000ull   /* an applet is left alone this long after it came */
#define AG_LOW_KB      2048u        /* a light recording next to an applet stops under this */
#define AG_FREE_UNKNOWN 0xFFFFFFFFu /* SYSTEM free not read this time */
#define AG_BACK_US     1500000ull   /* the applet gone this long: back to normal */

/* HOME Menu's titles (JPN, USA, EUR, CHN, KOR, TWN) */
static inline bool ag_is_home(uint64_t tid)
{
    switch (tid) {
    case 0x0004003000008202ull: case 0x0004003000008F02ull: case 0x0004003000009802ull:
    case 0x000400300000A102ull: case 0x000400300000A902ull: case 0x000400300000B102ull:
        return true;
    default:
        return false;
    }
}

/* an applet's title (HOME Menu is one too: ag_is_home) */
static inline bool ag_is_applet(uint64_t tid)
{
    return (uint32_t)(tid >> 32) == 0x00040030u;
}

typedef enum {
    AG_NONE,       /* no applet in the way */
    AG_RELEASED,   /* memory given back: waiting for the applet to settle (or to end) */
    AG_LITE,       /* a light recording runs next to the applet */
    AG_BLOCKED,    /* the applet wanted more: nothing recorded until it ends */
} AgState;

typedef enum {
    AG_DO_NOTHING,
    AG_DO_RELEASE,       /* save the recording / stop the buffer, memory given back at once */
    AG_DO_START_LITE,    /* a recording in a new file, light profile */
    AG_DO_STOP,          /* save the light recording, memory given back at once */
    AG_DO_START_NORMAL,  /* a recording in a new file, normal profile */
    AG_DO_RESTART_NORMAL /* save the light recording, a new one with the normal profile */
} AgAction;

typedef struct {
    AgState st;
    bool resume;          /* a recording (not the buffer) was running: one goes on */
    uint64_t since;       /* the applet came */
    uint64_t gone;        /* the applet is gone since (0: it is there) */
} AppletGuard;

static inline bool ag_gone_long(AppletGuard *g, uint64_t now, uint64_t usTicks)
{
    if (!g->gone)
        g->gone = now ? now : 1u;
    return now - g->gone >= AG_BACK_US * usTicks;
}

/*
 * One look (a few times a second). applet: an applet other than HOME Menu runs, or HOME Menu has
 * ended (an applet is coming); rec: the recorder records (or the buffer runs) with its buffers in
 * SYSTEM memory now (not in the launcher plugin's); replay: that is the replay buffer; freeKb:
 * SYSTEM free, or AG_FREE_UNKNOWN (only a light recording's watch uses it: ag_wants_free);
 * usTicks: ticks per microsecond. The caller does the action and tells a failed start
 * with ag_start_failed.
 */
static inline AgAction ag_step(AppletGuard *g, bool applet, bool rec, bool replay, uint32_t freeKb, uint64_t now,
                               uint64_t usTicks)
{
    if (applet)
        g->gone = 0;
    switch (g->st) {
    case AG_NONE:
        if (!applet || !rec)
            return AG_DO_NOTHING;
        g->st = AG_RELEASED;
        g->resume = !replay;
        g->since = now;
        return AG_DO_RELEASE;
    case AG_RELEASED:
        if (rec) {   /* a recording started meanwhile (the hotkey): light, watched as one */
            g->st = AG_LITE;
            return AG_DO_NOTHING;
        }
        if (!applet) {
            if (!ag_gone_long(g, now, usTicks))
                return AG_DO_NOTHING;
            g->st = AG_NONE;
            return g->resume ? AG_DO_START_NORMAL : AG_DO_NOTHING;
        }
        if (g->resume && now - g->since >= AG_SETTLE_US * usTicks) {
            g->st = AG_LITE;
            return AG_DO_START_LITE;
        }
        return AG_DO_NOTHING;
    case AG_LITE:
        if (!rec) {   /* stopped by the user */
            g->st = applet ? AG_BLOCKED : AG_NONE;
            g->resume = false;
            return AG_DO_NOTHING;
        }
        if (applet) {
            if (freeKb == AG_FREE_UNKNOWN || freeKb >= AG_LOW_KB)
                return AG_DO_NOTHING;
            g->st = AG_BLOCKED;
            g->resume = true;
            return AG_DO_STOP;
        }
        if (!ag_gone_long(g, now, usTicks))
            return AG_DO_NOTHING;
        g->st = AG_NONE;
        return AG_DO_RESTART_NORMAL;
    case AG_BLOCKED:
        if (rec) {   /* started by the user anyway: light, watched */
            g->st = AG_LITE;
            return AG_DO_NOTHING;
        }
        if (applet || !ag_gone_long(g, now, usTicks))
            return AG_DO_NOTHING;
        g->st = AG_NONE;
        return g->resume ? AG_DO_START_NORMAL : AG_DO_NOTHING;
    }
    return AG_DO_NOTHING;
}

/* The recording the guard asked for (light, or normal after the applet) did not start. */
static inline void ag_start_failed(AppletGuard *g, bool applet)
{
    g->st = applet ? AG_BLOCKED : AG_NONE;
    g->resume = applet;   /* tried again when the applet ends */
}

/* SYSTEM free is wanted on this look: a light recording runs next to an applet. */
static inline bool ag_wants_free(const AppletGuard *g)
{
    return g->st == AG_LITE;
}

/* The recorder's profile: light while an applet is in the way. */
static inline bool ag_lite(const AppletGuard *g)
{
    return g->st != AG_NONE;
}

#endif
