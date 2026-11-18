/*
 * Work with the lid closed (lid.h). The lid state comes from ptm:u (PTMU_GetShellState), as 3hs
 * does; sleep is refused through libctru's flag, which answers APT's sleep query.
 */
#include "lid.h"

#include <3ds.h>

#include "prepare.h"
#include "recorder.h"

#define POLL_MS        100u
#define IDLE_GRACE_MS  6000u    /* prepare.c looks for clips and joins every 5 s: no gap is "done" */
#define LOG_EVERY_MS   60000u   /* a line a minute while the lid is closed */
#define BATTERY_LOW    1u       /* PTMU_GetBatteryLevel 0..5: 1 is the red LED */

static bool g_ptmu, g_closed, g_refused;
static volatile bool g_awakeDone;   /* the work ended with the lid closed and no sleep came after */
static u64 g_lastPoll, g_closedAt, g_lastLog;
static aptHookCookie g_hook;

static void on_apt(APT_HookType hook, void *param)
{
    (void)param;
    if (hook == APTHOOK_ONSLEEP) {
        g_awakeDone = false;
        app_log("lid: the console goes to sleep");
    }
    else if (hook == APTHOOK_ONWAKEUP)
        app_log("lid: the console woke up");
}

static u8 battery(bool *charging)
{
    u8 level = 0, charge = 0;
    if (!g_ptmu || R_FAILED(PTMU_GetBatteryLevel(&level)))
        level = 0xFF;
    *charging = g_ptmu && R_SUCCEEDED(PTMU_GetBatteryChargeState(&charge)) && charge;
    return level;
}

/* Sleep is refused while there is work, unless the battery is nearly empty. */
static bool want_refuse(bool enabled, bool *lowBattery)
{
    *lowBattery = false;
    if (!enabled || prepare_idle_ms() >= IDLE_GRACE_MS)
        return false;
    bool charging = false;
    const u8 level = battery(&charging);
    *lowBattery = level <= BATTERY_LOW && !charging;
    return !*lowBattery;
}

void lid_init(void)
{
    g_ptmu = R_SUCCEEDED(ptmuInit());
    aptHook(&g_hook, on_apt, NULL);
    if (!g_ptmu)
        app_log("lid: no ptm:u: the console sleeps with the lid closed as before");
}

void lid_exit(void)
{
    aptUnhook(&g_hook);
    aptSetSleepAllowed(true);
    if (g_ptmu)
        ptmuExit();
    g_ptmu = false;
}

bool lid_step(bool enabled)
{
    if (!g_ptmu)
        return false;
    const u64 now = osGetTime();
    if (now - g_lastPoll < POLL_MS)
        return g_closed;
    g_lastPoll = now;

    bool low = false;
    const bool refuse = want_refuse(enabled, &low);
    if (refuse != g_refused) {
        g_refused = refuse;
        /* allowed again with the lid closed: libctru asks APT to sleep now (APT_SleepIfShellClosed) */
        if (!refuse && g_closed) {
            bool charging = false;
            const u8 level = battery(&charging);
            app_log("lid: %s with the lid closed after %llu s (battery %u/5%s): sleep allowed",
                    low ? "battery low" : !enabled ? "switched off" : "work done", (unsigned long long)((now - g_closedAt) / 1000), level,
                    charging ? ", charging" : "");
            g_awakeDone = true;
        }
        aptSetSleepAllowed(!refuse);
    }

    u8 shell = 1;
    if (R_FAILED(PTMU_GetShellState(&shell)))
        return g_closed;
    const bool closed = shell == 0;
    if (closed != g_closed) {
        bool charging = false;
        const u8 level = battery(&charging);
        if (closed) {
            g_closedAt = g_lastLog = now;
            g_awakeDone = false;
            app_log("lid closed: %s (battery %u/5%s)", refuse ? "the work goes on, sleep refused" : "nothing to do, sleep",
                    level, charging ? ", charging" : "");
        } else
            app_log("lid opened after %llu s%s (battery %u/5%s)", (unsigned long long)((now - g_closedAt) / 1000),
                    g_awakeDone ? ", the console stayed awake after the work was done" : "", level,
                    charging ? ", charging" : "");
        g_closed = closed;
    } else if (closed && now - g_lastLog >= LOG_EVERY_MS) {
        g_lastLog = now;
        bool charging = false;
        const u8 level = battery(&charging);
        app_log("lid closed %llu s: %s, battery %u/5%s", (unsigned long long)((now - g_closedAt) / 1000),
                refuse ? "working" : "awake with nothing to do", level, charging ? ", charging" : "");
    }
    return g_closed;
}
