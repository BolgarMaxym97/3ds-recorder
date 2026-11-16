/* Applets and the recorder's memory (sysmod/source/appletguard.h): when the recording gives its
   memory back, goes on light next to an applet, stops for a greedy one, and comes back normal. */
#include "appletguard.h"

#include <stdio.h>

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

#define MS 1000ull   /* the tests' clock: 1 tick = 1 us */
#define T  1ull

/* The browser case (O3DS 2026-11-16): recording in HOME, the browser comes, settles, ends. */
static void browser(void)
{
    AppletGuard g = { 0 };
    CHECK(ag_step(&g, false, true, false, 11616, 0, T) == AG_DO_NOTHING);
    /* HOME Menu ends: the recording gives its memory back at once */
    CHECK(ag_step(&g, true, true, false, 27136, 100 * MS, T) == AG_DO_RELEASE);
    CHECK(g.st == AG_RELEASED && g.resume && ag_lite(&g));
    /* the browser loads: SYSTEM free is not read; nothing starts for 3 s */
    CHECK(!ag_wants_free(&g));
    uint64_t t = 350 * MS;
    for (; t < 3100 * MS; t += 250 * MS)
        CHECK(ag_step(&g, true, false, false, AG_FREE_UNKNOWN, t, T) == AG_DO_NOTHING);
    /* 3 s past HOME Menu's end: a light recording is tried */
    CHECK(ag_step(&g, true, false, false, AG_FREE_UNKNOWN, t, T) == AG_DO_START_LITE && g.st == AG_LITE);
    CHECK(ag_wants_free(&g));
    /* light recording runs; the browser loads a page and wants more: saved, nothing more */
    CHECK(ag_step(&g, true, true, false, 6000, t, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, true, true, false, AG_FREE_UNKNOWN, t + 100 * MS, T) == AG_DO_NOTHING);   /* not read: no stop */
    CHECK(ag_step(&g, true, true, false, 1900, t + 250 * MS, T) == AG_DO_STOP);
    CHECK(g.st == AG_BLOCKED);
    /* not started again while it runs, however much is free */
    CHECK(ag_step(&g, true, false, false, 20000, t + 5000 * MS, T) == AG_DO_NOTHING);
    /* the browser ends; HOME Menu comes back: normal recording after 1.5 s */
    t += 10000 * MS;
    CHECK(ag_step(&g, false, false, false, 11000, t, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, false, false, false, 11000, t + 1000 * MS, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, false, false, false, 11000, t + 1500 * MS, T) == AG_DO_START_NORMAL);
    CHECK(g.st == AG_NONE && !ag_lite(&g));
}

/* A small applet (Friend List): light next to it, normal again after it. */
static void small_applet(void)
{
    AppletGuard g = { 0 };
    CHECK(ag_step(&g, true, true, false, 26616, 0, T) == AG_DO_RELEASE);
    AgAction a = AG_DO_NOTHING;
    uint64_t t = 250 * MS;
    for (; a == AG_DO_NOTHING && t < 5000 * MS; t += 250 * MS)
        a = ag_step(&g, true, false, false, AG_FREE_UNKNOWN, t, T);
    CHECK(a == AG_DO_START_LITE && t - 250 * MS >= 3000 * MS);
    CHECK(ag_step(&g, true, true, false, 20000, t, T) == AG_DO_NOTHING);
    /* it ends; a flicker of HOME gone again within 1.5 s keeps the light one */
    CHECK(ag_step(&g, false, true, false, 12000, t + 250 * MS, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, true, true, false, 12000, t + 500 * MS, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, false, true, false, 12000, t + 750 * MS, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, false, true, false, 12000, t + 2000 * MS, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, false, true, false, 12000, t + 2250 * MS, T) == AG_DO_RESTART_NORMAL);
    CHECK(g.st == AG_NONE);
}

/* The replay buffer: stops for the applet, nothing is started by the guard, the idle part starts it. */
static void replay(void)
{
    AppletGuard g = { 0 };
    CHECK(ag_step(&g, true, true, true, 15000, 0, T) == AG_DO_RELEASE);
    CHECK(!g.resume);
    for (uint64_t t = 250 * MS; t < 10000 * MS; t += 250 * MS)
        CHECK(ag_step(&g, true, false, false, 20000, t, T) == AG_DO_NOTHING);
    CHECK(ag_lite(&g));   /* the buffer is held off meanwhile */
    CHECK(ag_step(&g, false, false, false, 11000, 10000 * MS, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, false, false, false, 11000, 11500 * MS, T) == AG_DO_NOTHING);
    CHECK(g.st == AG_NONE && !ag_lite(&g));
}

/* The user stops the light recording; the hotkey starts one while held: watched, light. */
static void user(void)
{
    AppletGuard g = { 0 };
    CHECK(ag_step(&g, true, true, false, 20000, 0, T) == AG_DO_RELEASE);
    CHECK(ag_step(&g, true, true, false, 20000, 500 * MS, T) == AG_DO_NOTHING);   /* hotkey: started */
    CHECK(g.st == AG_LITE);
    CHECK(ag_step(&g, true, false, false, 20000, 1000 * MS, T) == AG_DO_NOTHING); /* hotkey: stopped */
    CHECK(g.st == AG_BLOCKED && !g.resume);
    CHECK(ag_step(&g, false, false, false, 11000, 2000 * MS, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, false, false, false, 11000, 4000 * MS, T) == AG_DO_NOTHING); /* not resumed */
    CHECK(g.st == AG_NONE);
}

/* Not recording, or recording in the launcher plugin's memory: nothing to do. */
static void idle(void)
{
    AppletGuard g = { 0 };
    for (uint64_t t = 0; t < 5000 * MS; t += 250 * MS)
        CHECK(ag_step(&g, true, false, false, 20000, t, T) == AG_DO_NOTHING);
    CHECK(g.st == AG_NONE && !ag_lite(&g));
}

/* A light start that fails: tried again normal when the applet ends. */
static void failed(void)
{
    AppletGuard g = { 0 };
    CHECK(ag_step(&g, true, true, false, 20000, 0, T) == AG_DO_RELEASE);
    ag_start_failed(&g, true);
    CHECK(g.st == AG_BLOCKED && g.resume);
    CHECK(ag_step(&g, false, false, false, 11000, 1000 * MS, T) == AG_DO_NOTHING);
    CHECK(ag_step(&g, false, false, false, 11000, 2500 * MS, T) == AG_DO_START_NORMAL);
    ag_start_failed(&g, false);
    CHECK(g.st == AG_NONE);
}

int main(void)
{
    CHECK(ag_is_home(0x0004003000009802ull) && ag_is_home(0x0004003000008F02ull) && ag_is_home(0x0004003000008202ull));
    CHECK(!ag_is_home(0x0004003000009D02ull));
    CHECK(ag_is_applet(0x0004003000009D02ull) && !ag_is_applet(0x0004013000003702ull) && !ag_is_applet(0x000400000B3DA300ull));
    browser();
    small_applet();
    replay();
    user();
    idle();
    failed();
    if (!g_fail)
        printf("test_appletguard: ok\n");
    return g_fail;
}
