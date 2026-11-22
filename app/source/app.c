#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../sysmod/source/shared.h"
#include "gallery.h"
#include "i18n.h"
#include "player.h"
#include "sdtest.h"
#include "prepare.h"
#include "qrcodegen.h"
#include "server.h"
#include "stats.h"
#include "ui.h"

/* ------------------------------------------------------------------ layout helpers */

typedef struct {
    float x, y, w, h;
} Rect;

/* blocking actions, run by the worker thread (run_pending) */
enum { ACT_NONE, ACT_START, ACT_STOP, ACT_UNINSTALL };

static bool hit(Rect r, const Input *in)
{
    return in->tapped && in->tap_x >= r.x && in->tap_x < r.x + r.w && in->tap_y >= r.y && in->tap_y < r.y + r.h;
}

static void focus_ring(Rect r, float radius) { ui_frame(r.x - 1, r.y - 1, r.w + 2, r.h + 2, radius + 1, 1.5f, COL_ACCENT); }

/* Panel tiles (bottom screen, 2 x 2) */
static const Rect TILE[4] = {{12, 12, 144, 94}, {164, 12, 144, 94}, {12, 114, 144, 94}, {164, 114, 144, 94}};
/* The first tile holds two switches, one under the other: the recorder, and its replay buffer */
static const Rect TILE_REC = {12, 12, 144, 46}, TILE_REPLAY = {12, 60, 144, 46};
/* Replay: the two files the module keeps for replay_sec (sysmod ReplayBytesFor: replay_sec x 8.5 MB/s in 16 MB
   steps, at least 128 MB, plus a 512 KB header each) and the recorder's reserve: 30 s, 700 MB, as before */
static const char *replay_len_label(int sec, char *buf, size_t cap);
static u32 replay_need_mb(int sec)
{
    u32 mb = ((u32)sec * 17u / 2u + 15u) / 16u * 16u;
    if (mb < 128u)
        mb = 128u;
    return 2u * mb + 190u;
}
/* Common */
static const Rect BACK_BTN = {12, 10, 22, 22};
/* First run */
static const Rect FR_LATER = {12, 200, 96, 28};
static const Rect FR_START = {116, 200, 192, 28};
/* Settings: the groups (SCR_SETTINGS), each one's rows (SCR_SETGROUP), the language list (SCR_LANG) */
enum { ROW_FPS, ROW_SCREENS, ROW_BOTTOM_FPS, ROW_QUALITY, ROW_SOUND, ROW_HOTKEY, ROW_LED, ROW_AUTOSTART, ROW_CONVERT,
       ROW_LID, ROW_STATS, ROW_UNINSTALL, ROW_MAIN_SCREEN, ROW_REPLAY_LEN, ROW_INPUTS, ROW_MP4_INPUTS, ROW_SDTEST, ROW_COUNT };
enum { GRP_RECORDING, GRP_RECORDER, GRP_APP, GRP_LANG, GRP_COUNT };
#define GROUP_MAX 6
static const int GROUP_ROWS[GRP_LANG][GROUP_MAX] = {
    [GRP_RECORDING] = {ROW_FPS, ROW_SCREENS, ROW_MAIN_SCREEN, ROW_BOTTOM_FPS, ROW_QUALITY, ROW_SOUND},
    [GRP_RECORDER] = {ROW_HOTKEY, ROW_REPLAY_LEN, ROW_INPUTS, ROW_LED, ROW_AUTOSTART},
    [GRP_APP] = {ROW_CONVERT, ROW_MP4_INPUTS, ROW_LID, ROW_STATS, ROW_SDTEST, ROW_UNINSTALL},
};
static const int GROUP_N[GRP_LANG] = {6, 5, 6};
static Rect grp_card(int i) { return (Rect){10, 34 + i * 50.0f, 300, 44}; }   /* down to y 228 */
static Rect set_row(int i) { return (Rect){10, 34 + i * 30.0f, 300, 26}; }      /* 6 rows: down to y 210 */
static const Rect SET_BACK = {10, 6, 21, 21};
/* the language list: first run (a Continue button under it) or from Settings. Two columns, so
   every language is on screen at once (10 today); more rows than fit scroll, with a bar. */
#define LANG_COLS 2
#define LANG_ROWS_SHOWN 5
#define LANG_ROWS ((LANG_COUNT + LANG_COLS - 1) / LANG_COLS)
static Rect lang_cell(int i, int firstRow, bool first)
{
    const int row = i / LANG_COLS - firstRow, col = i % LANG_COLS;
    return (Rect){10 + col * 153.0f, (first ? 12 : 34) + row * 35.0f, 147, 31};
}
static const Rect LANG_GO = {12, 196, 296, 32};
static int g_langFirst; /* the language list's top row shown */
static Lang g_langSaved; /* lang= in config.ini when the list opened from Settings */

static void lang_scroll_to(int lang)
{
    const int row = lang / LANG_COLS;
    g_langFirst = row > LANG_ROWS_SHOWN - 1 ? row - (LANG_ROWS_SHOWN - 1) : 0;
}

/* Hotkey screen buttons */
static const Rect HK_BTN[3] = {{12, 200, 92, 28}, {114, 200, 92, 28}, {216, 200, 92, 28}};
/* Uninstall dialog */
static const Rect UN_CHECK = {24, 128, 272, 24};
static const Rect UN_BTN[2] = {{24, 168, 132, 28}, {164, 168, 132, 28}};
/* Help topics */
#define HELP_TOPICS 10
static Rect help_row(int i) { return (Rect){12, 34 + i * 20.4f, 296, 19}; }   /* 10 topics: down to y 237 */

/* A 3D game at 30 fps fills what the SD card takes: ~5.4 MB/s while writing, both screens or the top
   one alone (Mario Kart 7, O3DS 0.10.x, 2026-11-09; 2.1 MB/s before 0.10, when the bottom screen
   paused whenever the card was behind). Menus and 2D games take much less: the estimate is the
   worst case. */
#define MB_PER_MIN_BOTH_30 320u

/* ------------------------------------------------------------------ state */

/* What the app reads from the SD card (recordings, free space, the installed module, Luma's
   option) comes from the info thread: right after the module starts or a recording is saved the
   card is busy for a second or more (the module reserves its next file), and reading it on the
   main thread froze the screens (2026-11-05: a 515 ms frame after Start). The main thread only
   copies the latest snapshot; the module's own state (shared memory) it reads itself. */
typedef struct {
    RecFiles files;
    u32 sdFreeMb;
    ModuleFile moduleFile;
    bool lumaModules;
} SdInfo;

static Thread g_info;
static LightLock g_infoLock;
static LightEvent g_infoWake;
static SdInfo g_infoSnap;
static volatile u32 g_infoSeq;
static volatile bool g_infoQuit;
static u32 g_infoSeen;

/* The module file is compared (2 x 138 KB read) only when asked: it changes only by this app's own
   start / uninstall. Every 2 s that read cost the player frames (O3DS 2026-11-05). */
static volatile bool g_infoModuleWanted;
static volatile bool g_infoHold;    /* the player reads the card: no SD reads meanwhile */

static void info_read(SdInfo *i, bool module)
{
    recorder_files(&i->files);
    i->sdFreeMb = recorder_sd_free_mb();
    if (module)
        i->moduleFile = recorder_module_file();
    i->lumaModules = recorder_luma_modules_on();
}

/* The panel's "last recording" thumbnail: decoded by the info thread when the newest file changes. */
static u16 *g_lastThumb;           /* GAL_LAST_W x GAL_LAST_H, tiled; under g_infoLock */
static char g_lastName[48];        /* which recording it shows (RecFiles.newest); empty: none */
static bool g_lastOk;
static u32 g_lastSeq, g_lastSeen;
static C3D_Tex g_lastTex;
static bool g_lastTexOk;

static void last_thumb(const char *newest, u16 *work)
{
    LightLock_Lock(&g_infoLock);
    const bool same = strcmp(newest, g_lastName) == 0;
    LightLock_Unlock(&g_infoLock);
    if (same || !work)
        return;
    const bool ok = newest[0] && gal_last_thumb(newest, work);
    LightLock_Lock(&g_infoLock);
    snprintf(g_lastName, sizeof(g_lastName), "%s", newest);
    g_lastOk = ok;
    if (ok)
        memcpy(g_lastThumb, work, GAL_LAST_W * GAL_LAST_H * 2);
    g_lastSeq++;
    LightLock_Unlock(&g_infoLock);
}

static void info_main(void *arg)
{
    (void)arg;
    u16 *work = malloc(GAL_LAST_W * GAL_LAST_H * 2);
    while (!g_infoQuit) {
        if (g_infoHold) {
            LightEvent_WaitTimeout(&g_infoWake, 2000ull * 1000 * 1000);
            continue;
        }
        const bool module = g_infoModuleWanted;
        g_infoModuleWanted = false;
        SdInfo i;
        LightLock_Lock(&g_infoLock);
        i.moduleFile = g_infoSnap.moduleFile;
        LightLock_Unlock(&g_infoLock);
        info_read(&i, module);
        LightLock_Lock(&g_infoLock);
        g_infoSnap = i;
        g_infoSeq++;
        LightLock_Unlock(&g_infoLock);
        if (g_lastThumb)
            last_thumb(i.files.newest, work);
        LightEvent_WaitTimeout(&g_infoWake, 2000ull * 1000 * 1000);
    }
    free(work);
}

static void info_apply(App *a, const SdInfo *i)
{
    a->files = i->files;
    a->sdFreeMb = i->sdFreeMb;
    a->moduleFile = i->moduleFile;
    a->lumaModules = i->lumaModules;
}

/* Module state now; with files, also asks the info thread for a fresh SD snapshot. */
static void say(App *a, const char *text);
static void say_important(App *a, const char *text);
static void format_name(char *buf, size_t cap, const char *name);
static void apply_setting(App *a, const char *key, const char *value);

/* Launch on startup: what the module made of the setting (it sets the override up, shared.h
   REC_AUTO_*). A start with the console that failed switched it off: said once, and the setting
   follows. After the switch in Settings: the module's answer, or that it comes later. */
static void autostart_tick(App *a)
{
    const bool known = a->st.running && a->st.readable;
    if (known && a->cfg.autostart && a->cfg.guideSeen && a->st.autoState == REC_AUTO_FAILED_START) {
        a->autoWait = 0;
        apply_setting(a, "autostart", "off");   /* the module forgets the failed start with it */
        say_important(a, T(S_MSG_AUTO_FAILED));
        return;
    }
    if (a->autoWait) {
        if (known && a->st.autoState != REC_AUTO_OFF) {
            a->autoWait = 0;
            if (a->st.autoState == REC_AUTO_ON) {
                say(a, T(S_MSG_AUTO_ON));
                return;
            }
            say(a, T(a->st.autoState == REC_AUTO_NO_PATCHING ? S_MSG_AUTO_PATCHING
                     : a->st.autoState == REC_AUTO_ERROR     ? S_MSG_AUTO_SD_ERROR
                                                             : S_MSG_AUTO_UNSUPPORTED));
            apply_setting(a, "autostart", "off");
        } else if (a->frame > a->autoWait) {   /* the recorder is off, or a recording runs: set up after it */
            a->autoWait = 0;
            say(a, T(S_MSG_AUTO_LATER));
        }
        return;
    }
    /* the Luma option was turned off after launch on startup had been set up */
    if (known && a->cfg.autostart && a->cfg.guideSeen && a->st.autoState == REC_AUTO_NO_PATCHING && !a->autoWarned) {
        a->autoWarned = true;
        say(a, T(S_MSG_AUTO_PATCHING));
    }
}

static void refresh(App *a, bool files)
{
    recorder_poll(&a->st);   /* also tells the module the app is on the screen */
    autostart_tick(a);
    if (!a->clipsTold && a->cfg.guideSeen && prepare_clips(NULL) > 0) {
        char msg[256];
        a->clipsTold = true;
        snprintf(msg, sizeof(msg), T(S_MSG_CLIPS), prepare_clips(NULL));
        say(a, msg);
    }
    if (a->st.replayOn && (a->st.replayError == REC_REPLAY_NO_SPACE || a->st.replayError == REC_REPLAY_WRITE_ERROR) &&
        !a->replayWarned) {
        a->replayWarned = true;
        if (a->st.replayError == REC_REPLAY_NO_SPACE) {
            char len[16], msg[192];
            replay_len_label(a->cfg.replaySec, len, sizeof(len));
            snprintf(msg, sizeof(msg), T(S_MSG_REPLAY_SPACE), len, (unsigned)replay_need_mb(a->cfg.replaySec));
            say(a, msg);
        } else
            say(a, T(S_MSG_REPLAY_WRITE));
    }
    char rec[32];
    if (a->st.running && recorder_take_recovered(rec, sizeof(rec))) {
        char when[48], msg[512];
        format_name(when, sizeof(when), rec);
        snprintf(msg, sizeof(msg), T(S_MSG_RECOVERED), when);
        say_important(a, msg);
        files = true;
    }
    if (files && g_info)
        LightEvent_Signal(&g_infoWake);
    if (g_infoSeq != g_infoSeen) {
        LightLock_Lock(&g_infoLock);
        g_infoSeen = g_infoSeq;
        info_apply(a, &g_infoSnap);
        LightLock_Unlock(&g_infoLock);
    }
}

static void say(App *a, const char *text)
{
    app_log("toast: %s", text); /* every message the user saw, errors included */
    if (a->msgImportant && a->msg[0] && a->frame <= a->msgUntil) { /* waits its turn (e.g. "started" at launch) */
        snprintf(a->msgNext, sizeof(a->msgNext), "%s", text);
        return;
    }
    snprintf(a->msg, sizeof(a->msg), "%s", text);
    a->msgImportant = false;
    a->msgUntil = a->frame + 60 * (4 + (u32)strlen(text) / 100); /* long ones stay longer */
}

/* A message the user must not miss (a recording cut off or stopped by a restart): 10 s at least,
   and the next ordinary one waits behind it (2026-11-07: "started" covered it after 0.7 s). */
static void say_important(App *a, const char *text)
{
    app_log("toast: %s", text);
    snprintf(a->msg, sizeof(a->msg), "%s", text);
    a->msgImportant = true;
    a->msgUntil = a->frame + 60 * 10;
}

static void toast_tick(App *a)
{
    /* the first run's language and guide come first: an important message waits for them */
    if (a->msgImportant && ((a->screen == SCR_LANG && a->langFirst) || a->screen == SCR_GUIDE))
        a->msgUntil = a->frame + 60 * 10;
    if (a->msgNext[0] && a->frame > a->msgUntil) {
        snprintf(a->msg, sizeof(a->msg), "%s", a->msgNext);
        a->msgNext[0] = 0;
        a->msgImportant = false;
        a->msgUntil = a->frame + 60 * (4 + (u32)strlen(a->msg) / 100);
    }
}

static void go(App *a, Screen s, int focus)
{
    static const char *const names[] = {"panel", "firstrun", "help", "settings", "hotkey", "gallery", "player",
                                        "download", "uninstall", "uninstalled", "guide", "lang", "settings group"};
    if (s != a->screen || !a->frame)
        app_log("screen: %s", (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "?");
    a->screen = s;
    a->focus = focus;
}

/* The console turned off / restarted mid-recording (the module left a note): say so once. */
static void tell_system_stop(App *a)
{
    char stopped[48];
    bool game = false;
    if (recorder_take_system_stop(stopped, sizeof(stopped), &game)) {
        char when[48], msg[512];
        format_name(when, sizeof(when), stopped);
        snprintf(msg, sizeof(msg), T(game ? S_MSG_GAME_STOP : S_MSG_SYSTEM_STOP), when);
        app_log("system stop: %s%s", stopped, game ? " (an extended-memory game closed)" : "");
        say_important(a, msg);
    }
}

static void apply_language(App *a);

void app_init(App *a, const char *selfPath)
{
    memset(a, 0, sizeof(*a));
    snprintf(a->selfPath, sizeof(a->selfPath), "%s", selfPath);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    a->new3ds = n3ds;
    config_load(&a->cfg);
    apply_language(a);
    a->lumaVersion = recorder_luma_version();
    SdInfo first; /* the first screen needs it: read once here, then by the info thread */
    info_read(&first, true);
    g_infoSnap = first;
    info_apply(a, &first);
    recorder_status(&a->st);
    LightLock_Init(&g_infoLock);
    LightEvent_Init(&g_infoWake, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_lastThumb = malloc(GAL_LAST_W * GAL_LAST_H * 2);
    g_lastTexOk = C3D_TexInit(&g_lastTex, GAL_LAST_W, GAL_LAST_H, GPU_RGB565);
    if (g_lastTexOk)
        C3D_TexSetFilter(&g_lastTex, GPU_LINEAR, GPU_LINEAR);
    g_info = threadCreate(info_main, NULL, 16 * 1024, prio + 2, -2, false);
    prepare_set_inputs(a->cfg.mp4Inputs);   /* Buttons In Video (the console's MP4s, and the phone page's) */
    server_set_inputs(a->cfg.mp4Inputs);
    prepare_start(n3ds, n3ds && a->cfg.mp4Slices, a->cfg.convertAuto); /* recordings become MP4s for the phone in the background */
#ifndef UI_TOUR /* the screen tour runs in Azahar: its logs are no console's */
    stats_start(a->cfg.stats); /* new log lines to the developer, quietly (stats.h) */
#endif
    /* The recorder starts with the app. Only when Luma cannot load it does the first-run
       screen come up, to say what to change. */
    /* After an app update the running module is the older one: it is restarted with the new one
       (only when idle: a recording is never cut for it). */
    const bool outdated = a->st.running && a->moduleFile == MODULE_DIFFERENT &&
                          (a->st.state == REC_STATE_IDLE || a->st.state == REC_STATE_REPLAY);
    const bool autoStart = (!a->st.running || outdated) && a->lumaModules;
    if (a->cfg.guideSeen) /* else after the first run's language and guide (in its language) */
        tell_system_stop(a);
    app_log("app: %s, lang %s, guide %s, module %s (state %lu, file %s), Luma modules %s%s, SD free %lu MB, "
            "%lu recordings (%llu MB)%s",
            n3ds ? "New 3DS" : "Old 3DS", LANGS[a->cfg.lang].code, a->cfg.guideSeen ? "seen" : "first run",
            a->st.running ? "running" : "not running", (unsigned long)a->st.state,
            a->moduleFile == MODULE_SAME ? "same" : a->moduleFile == MODULE_DIFFERENT ? "different" : "missing",
            a->lumaModules ? "on" : "OFF", recorder_luma_sd_mode() ? "" : ", Luma STARTED FROM CTRNAND",
            (unsigned long)a->sdFreeMb, (unsigned long)a->files.count,
            (unsigned long long)(a->files.bytes >> 20), outdated ? ", outdated module: restarting it" : "");
    if (autoStart) {
        /* The recorder was not running: the console has been turned on (or restarted) since it last
           did. The replay buffer starts each such session switched off: it is turned on for a play
           session, not left writing to the card for good. An update of a running recorder keeps it. */
        if (!a->st.running && a->cfg.replay) {
            app_log("setting: replay=off (the recorder starts after a power-on)");
            config_set("replay", "off");
            config_load(&a->cfg);
        }
        a->pending = ACT_START;
        a->busy = T(S_STATE_STARTING);
    }
    const Screen next = a->st.running || autoStart ? SCR_PANEL : SCR_FIRSTRUN;
    if (!a->cfg.guideSeen) { /* the very first run: the language, how to use it, then on */
        a->afterGuide = next;
        a->guidePage = 0;
        a->langFirst = !a->cfg.langSet;
        if (a->langFirst) { /* the console's own language in focus (its font loads when it is chosen) */
            a->cfg.lang = i18n_lang_from_system(app_console()->sysLangId);
            i18n_set_lang(a->cfg.lang);
        }
        lang_scroll_to(a->cfg.lang);
        go(a, a->cfg.langSet ? SCR_GUIDE : SCR_LANG, a->cfg.langSet ? 0 : a->cfg.lang);
    } else
        go(a, next, 0);
}

void app_exit(App *a)
{
    if (a->screen == SCR_PHONE)
        server_stop();
    if (a->screen == SCR_PHONE)
        gal_stop();
    if (a->screen == SCR_PLAYER)
        player_stop();
    if (a->screen == SCR_GALLERY || a->screen == SCR_PLAYER)
        gal_stop();
    if (g_info) {
        g_infoQuit = true;
        LightEvent_Signal(&g_infoWake);
        threadJoin(g_info, U64_MAX);
        threadFree(g_info);
        g_info = NULL;
    }
    stats_stop();
    prepare_stop(); /* the job keeps its resume point */
}


/* Blocking actions (install + start the module, stop it: up to 15 s while a recording is saved,
   uninstall) run on a worker thread, so the screens keep drawing and the spinner turns. The UI
   only waits for it; it reads no module state meanwhile. */
static Thread g_worker;
static volatile bool g_workDone;
static int g_workAct;
static Result g_workRc;
static const char *g_workStep;
static bool g_workOk;
static const App *g_workApp;

static void worker_main(void *arg)
{
    (void)arg;
    if (g_workAct == ACT_START)
        g_workRc = recorder_install_and_start(&g_workStep);
    else if (g_workAct == ACT_STOP)
        g_workOk = recorder_stop();
    else if (g_workAct == ACT_UNINSTALL) {
        g_workOk = recorder_uninstall(g_workApp->deleteRecordings, g_workApp->selfPath);
    }
    g_workDone = true;
}

/* Returns true while an action is running (the caller then skips input). */
static bool run_pending(App *a)
{
    if (!g_worker) {
        /* uninstall: the conversion and info threads stop first, here on the UI thread (APT's hook
           list is changed by it): their open .part / .jpart files and their log lines (app.log
           made again) kept /3ds/recorder from being removed */
        if (a->pending == ACT_UNINSTALL)
            app_exit(a);
        g_workAct = a->pending;
        g_workApp = a;
        g_workDone = false;
        g_workStep = "";
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        g_worker = threadCreate(worker_main, NULL, 16 * 1024, prio + 1, -2, false);
        app_log("action %d: worker %s (main thread priority %02lX)", g_workAct, g_worker ? "started" : "NOT created",
                (unsigned long)prio);
        if (!g_worker) { /* no thread: run it here, the spinner stops for that time */
            worker_main(NULL);
            g_workDone = true;
        } else
            return true;
    }
    if (!g_workDone)
        return true;
    if (g_worker) {
        threadJoin(g_worker, U64_MAX);
        threadFree(g_worker);
        g_worker = NULL;
    }
    const int act = a->pending;
    a->pending = ACT_NONE;
    a->busy = NULL;
    char buf[512];   /* the longest message (S_MSG_START_NAND_LUMA, Ukrainian) is ~370 bytes */
    app_log("action %d done: rc %08lX, ok %d, step '%s'", act, (unsigned long)g_workRc, g_workOk, g_workStep);
    if (act == ACT_START) {
        g_infoModuleWanted = true; /* the module file may have been installed */
        refresh(a, true);
        if (R_SUCCEEDED(g_workRc)) {
            say(a, T(S_MSG_STARTED));
            if (a->screen == SCR_FIRSTRUN)
                go(a, SCR_PANEL, 0);
        } else {
            /* C8804478 (FS: not found) from pm when it launched the module. Luma started from
               CTRNAND looks for the module there, not on the SD card (recorder_luma_sd_mode); else
               seen on Luma 13.3.3 and gone after an update (field logs 2026-11-20/21, issue #4) */
            const bool notFound = g_workRc == (Result)0xC8804478;
            snprintf(buf, sizeof(buf),
                     T(!notFound ? S_MSG_START_FAILED : !recorder_luma_sd_mode() ? S_MSG_START_NAND_LUMA : S_MSG_START_OLD_LUMA),
                     g_workStep, (unsigned long)g_workRc);
            say(a, buf);
        }
    } else if (act == ACT_STOP) {
        say(a, g_workOk ? T(S_MSG_STOPPED) : T(S_MSG_STOP_FAILED));
        refresh(a, true);
    } else if (act == ACT_UNINSTALL) {
        a->uninstallOk = g_workOk;
        go(a, SCR_UNINSTALLED, 0);
    }
    return false;
}

/* the texts and the font of a->cfg.lang (Japanese, Chinese: their own font, loaded here) */
static void apply_language(App *a)
{
    i18n_set_lang(a->cfg.lang);
    const u64 t0 = osGetTime();
    if (!ui_set_script(LANGS[a->cfg.lang].script))
        app_log("font for %s: not loaded", LANGS[a->cfg.lang].code);
    else if (LANGS[a->cfg.lang].script != SCRIPT_LATIN)
        app_log("font for %s: %llu ms", LANGS[a->cfg.lang].code, osGetTime() - t0);
}

static void set_language(App *a, int lang)
{
    if ((unsigned)lang >= LANG_COUNT || a->cfg.lang == (Lang)lang)
        return;
    a->cfg.lang = (Lang)lang;
    apply_language(a);
    config_set("lang", LANGS[lang].code);
}

/* X on the first screens: the next language, without the list */
static void next_language(App *a) { set_language(a, (a->cfg.lang + 1) % LANG_COUNT); }

static void open_languages(App *a, Screen back, int backFocus)
{
    a->langFirst = false;
    g_langSaved = a->cfg.lang;
    lang_scroll_to(a->cfg.lang);
    a->langBack = back;
    a->langBackFocus = backFocus;
    go(a, SCR_LANG, a->cfg.lang);
}

/* ------------------------------------------------------------------ hotkey */

/* Buttons a hotkey may use, in the order config.ini lists them (R+SELECT). */
static const struct {
    u32 key;
    const char *name, *label;
} HK_KEYS[] = {
    {KEY_L, "L", "L"}, {KEY_R, "R", "R"}, {KEY_DUP, "UP", "↑"}, {KEY_DDOWN, "DOWN", "↓"},
    {KEY_DLEFT, "LEFT", "←"}, {KEY_DRIGHT, "RIGHT", "→"}, {KEY_A, "A", "A"}, {KEY_B, "B", "B"},
    {KEY_X, "X", "X"}, {KEY_Y, "Y", "Y"}, {KEY_SELECT, "SELECT", "SELECT"},
};
#define HK_COUNT (int)(sizeof(HK_KEYS) / sizeof(HK_KEYS[0]))
#define HK_ALLOWED (KEY_L | KEY_R | KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT | KEY_A | KEY_B | KEY_X | KEY_Y | KEY_SELECT)
#define HK_DEFAULT "R+SELECT"
#define HK_REPLAY_DEFAULT "X+SELECT"

static u32 hk_mask(const char *text)
{
    u32 m = 0;
    char buf[48];
    snprintf(buf, sizeof(buf), "%s", text);
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+"))
        for (int i = 0; i < HK_COUNT; i++)
            if (strcasecmp(t, HK_KEYS[i].name) == 0)
                m |= HK_KEYS[i].key;
    return m;
}

static void hk_text(u32 m, char *buf, size_t cap) /* config form: R+SELECT */
{
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; i < HK_COUNT; i++)
        if (m & HK_KEYS[i].key)
            o += (size_t)snprintf(buf + o, cap - o, "%s%s", o ? "+" : "", HK_KEYS[i].name);
}

static void hk_label(u32 m, char *buf, size_t cap) /* for people: R + SELECT */
{
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; i < HK_COUNT; i++)
        if (m & HK_KEYS[i].key)
            o += (size_t)snprintf(buf + o, cap - o, "%s%s", o ? " + " : "", HK_KEYS[i].label);
}

static int hk_count(u32 m)
{
    int n = 0;
    for (int i = 0; i < HK_COUNT; i++)
        n += (m & HK_KEYS[i].key) != 0;
    return n;
}

/* S_COUNT if the combination may be used, else the reason it may not. */
static Str hk_problem(u32 m)
{
    if ((m & KEY_L) && (m & KEY_R))
        return S_HK_ERR_LR;
    if (!(m & ~(KEY_L | KEY_DDOWN | KEY_SELECT)))
        return S_HK_ERR_ROSALINA; /* part of Rosalina's L + Down + SELECT */
    if (!(m & ~(KEY_A | KEY_B | KEY_X | KEY_Y)))
        return S_HK_ERR_FACE;
    return S_COUNT;
}

/* Two hotkeys that would fire together: one holds every button of the other. */
static bool hk_overlap(u32 a, u32 b)
{
    return a && b && ((a & b) == a || (a & b) == b);
}

/* The replay hotkey's default, or the next one that does not overlap the recording hotkey. */
static u32 hk_replay_default(u32 rec)
{
    static const char *const tries[] = {HK_REPLAY_DEFAULT, "Y+SELECT", "L+X", "R+Y"};
    for (int i = 0; i < 4; i++)
        if (!hk_overlap(hk_mask(tries[i]), rec))
            return hk_mask(tries[i]);
    return hk_mask(HK_REPLAY_DEFAULT);
}

static float hk_chips_width(u32 m)
{
    float w = 0;
    bool first = true;
    for (int i = 0; i < HK_COUNT; i++)
        if (m & HK_KEYS[i].key) {
            if (!first)
                w += ui_text_width(9, FONT_REG, "+") + 3;
            w += ui_text_width(8, FONT_BOLD, HK_KEYS[i].label) + 8 + 3;
            first = false;
        }
    return w;
}

/* Draws key chips for a mask; returns the width. */
static float hk_chips(float x, float y, u32 m, u32 color)
{
    const float x0 = x;
    bool first = true;
    for (int i = 0; i < HK_COUNT; i++)
        if (m & HK_KEYS[i].key) {
            if (!first)
                x += ui_text(x, y + 1, 9, FONT_REG, color, "+") + 3;
            x += ui_key_chip(x, y, HK_KEYS[i].label) + 3;
            first = false;
        }
    return x - x0;
}

/* ------------------------------------------------------------------ settings */

static void apply_setting(App *a, const char *key, const char *value)
{
    app_log("setting: %s=%s", key, value);
    config_set(key, value);
    config_load(&a->cfg);
    recorder_command(REC_CMD_RELOAD); /* the module reads it now, or after a running recording */
}

static const char *const FPS_LABEL[3] = {"15", "20", "30 fps"};
static const int FPS_VALUE[3] = {15, 20, 30};

static int fps_index(const App *a) { return a->cfg.fps == 15 ? 0 : a->cfg.fps == 20 ? 1 : 2; }

/* Segmented control on the right of a settings row: option rects, right-aligned. */
static void seg_layout(Rect row, const char *const *labels, int n, Rect *out)
{
    float x = row.x + row.w - 3;
    for (int i = n - 1; i >= 0; i--) {
        const float w = ui_text_width(9, FONT_BOLD, labels[i]) + 14;
        x -= w;
        out[i] = (Rect){x, row.y + 3, w, row.h - 6};
        x -= 2;
    }
}

typedef struct {
    const char *labels[3];
    int n, sel, disabled; /* disabled: option index, -1 none */
} Seg;

/* The bottom screen's own rate (bottom_fps, #18): 0 = the same as the frame rate. More values than a
   segmented control holds: the row shows one and ◀ ▶ step through them. */
static const int BOTTOM_FPS[5] = {0, 15, 10, 5, 1};

static int bottom_fps_index(const App *a)
{
    for (int i = 0; i < 5; i++)
        if (BOTTOM_FPS[i] == a->cfg.bottomFps)
            return i;
    return 0;
}

static const char *bottom_fps_label(const App *a, char *buf, size_t cap)
{
    const int v = a->cfg.bottomFps;
    if (!v || v >= a->cfg.fps)   /* (with Main Screen → Bottom the row is the top screen's) */
        return T(a->cfg.mainBottom ? S_TOP_SAME : S_BOTTOM_SAME);
    snprintf(buf, cap, "%d fps", v);
    return buf;
}

static bool bottom_fps_on(const App *a) { return a->cfg.screens == SCREENS_BOTH; }

/* Replay Length (replay_sec): more values than a segmented control holds, a stepper like Bottom Screen FPS */
static const int REPLAY_SEC[5] = {15, 30, 60, 120, 300};

static int replay_len_index(const App *a)
{
    for (int i = 0; i < 5; i++)
        if (REPLAY_SEC[i] == a->cfg.replaySec)
            return i;
    return 1;
}

/* "30 s", "2 min" */
static const char *replay_len_label(int sec, char *buf, size_t cap)
{
    if (sec >= 60)
        snprintf(buf, cap, T(S_UNIT_MIN), sec / 60);
    else
        snprintf(buf, cap, T(S_UNIT_SEC), sec);
    return buf;
}

static void step_replay_len(App *a, int dir, bool wrap);

static bool row_seg(const App *a, int row, Seg *g)
{
    switch (row) {
    case ROW_FPS:
        *g = (Seg){{FPS_LABEL[0], FPS_LABEL[1], FPS_LABEL[2]}, 3, fps_index(a), -1};
        return true;
    case ROW_SCREENS:
        *g = (Seg){{T(S_BOTH), T(S_TOP), T(S_BOTTOM)}, 3, a->cfg.screens, -1};
        return true;
    case ROW_QUALITY:
        *g = (Seg){{T(S_Q_AUTO), T(S_Q_LOSSLESS), NULL}, 2, a->cfg.lossless ? 1 : 0, -1};
        return true;
    case ROW_LED:
        *g = (Seg){{T(S_LED_PULSE), T(S_LED_SOLID), T(S_LED_OFF)}, 3, a->cfg.led == 2 ? 0 : a->cfg.led == 1 ? 1 : 2, -1};
        return true;
    case ROW_CONVERT:
        *g = (Seg){{T(S_CONVERT_AUTO), T(S_CONVERT_MANUAL), NULL}, 2, a->cfg.convertAuto ? 0 : 1, -1};
        return true;
    case ROW_MAIN_SCREEN:
        *g = (Seg){{T(S_TOP), T(S_BOTTOM), NULL}, 2, a->cfg.mainBottom ? 1 : 0, -1};
        return true;
    }
    return false;
}

static void choose(App *a, int row, int opt)
{
    switch (row) {
    case ROW_FPS:
        if (FPS_VALUE[opt] != a->cfg.fps) {
            char v[8];
            snprintf(v, sizeof(v), "%d", FPS_VALUE[opt]);
            apply_setting(a, "fps", v);
        }
        break;
    case ROW_SCREENS:
        if (opt != a->cfg.screens) {
            static const char *const V[3] = {"both", "top", "bottom"};
            apply_setting(a, "screens", V[opt]);
        }
        break;
    case ROW_QUALITY:
        if ((opt == 1) != a->cfg.lossless)
            apply_setting(a, "quality", opt == 1 ? "lossless" : "auto");
        break;
    case ROW_LED: {
        static const char *const V[3] = {"pulse", "solid", "off"};
        static const int L[3] = {2, 1, 0};
        if (L[opt] != a->cfg.led)
            apply_setting(a, "led", V[opt]);
        break;
    }
    case ROW_CONVERT:
        if ((opt == 0) != a->cfg.convertAuto) {
            apply_setting(a, "convert", opt == 0 ? "auto" : "manual");
            prepare_set_auto(a->cfg.convertAuto);
        }
        break;
    case ROW_MAIN_SCREEN:
        if (!bottom_fps_on(a))
            say(a, T(S_MSG_MAIN_BOTH));
        else if ((opt == 1) != a->cfg.mainBottom)
            apply_setting(a, "main_screen", opt == 1 ? "bottom" : "top");
        break;
    }
}

static void step_replay_len(App *a, int dir, bool wrap)
{
    int i = replay_len_index(a) + dir;
    if (wrap)
        i = (i + 5) % 5;
    if (i < 0 || i > 4)
        return;
    char v[8];
    snprintf(v, sizeof(v), "%d", REPLAY_SEC[i]);
    apply_setting(a, "replay_sec", v);
}

static void set_mp4_inputs(App *a, bool on)
{
    if (on == a->cfg.mp4Inputs)
        return;
    apply_setting(a, "mp4_inputs", on ? "on" : "off");
    prepare_set_inputs(on);
    server_set_inputs(on);
}

static void open_sdtest(App *a);

/* ◀ ▶ on Bottom Screen FPS (dir -1 / +1; A goes round) */
static void step_bottom_fps(App *a, int dir, bool wrap)
{
    if (!bottom_fps_on(a)) {
        say(a, T(S_MSG_BOTTOM_BOTH));
        return;
    }
    int i = bottom_fps_index(a);
    do { /* a rate at or above the frame rate is the same as Same: stepped over */
        i += dir;
        if (wrap)
            i = (i + 5) % 5;
        if (i < 0 || i > 4)
            return;
    } while (BOTTOM_FPS[i] && BOTTOM_FPS[i] >= a->cfg.fps);
    char v[8];
    snprintf(v, sizeof(v), "%d", BOTTOM_FPS[i]);
    apply_setting(a, "bottom_fps", v);
}

static void open_hotkey(App *a)
{
    a->hkStep = 0;
    a->hkHeld = a->hkCand = 0;
    a->hkReview = false;
    a->hkWaitRelease = true; /* the A that opened this screen is still down */
    go(a, SCR_HOTKEY, 0);
}

/* Launch On Startup: the module sets it up when it reads the setting; its answer comes through
   the status (autostart_tick). Not switched on without Luma's game patching: nothing would start. */
static void set_autostart(App *a, bool on)
{
    if (on == a->cfg.autostart)
        return;
    if (on && !recorder_luma_patching_on()) {
        say(a, T(S_MSG_AUTO_PATCHING));
        return;
    }
    apply_setting(a, "autostart", on ? "on" : "off");
    a->autoWait = on ? a->frame + 60 * 6 : 0;
    a->autoWarned = false;
    if (!on)
        say(a, T(S_MSG_AUTO_OFF));
}

static void set_stats(App *a, bool on)
{
    if (on == a->cfg.stats)
        return;
    apply_setting(a, "stats", on ? "on" : "off");
    stats_set_enabled(on); /* on again: from the next launch */
}

static void settings_activate(App *a, int row)
{
    Seg g;
    if (row_seg(a, row, &g)) {
        choose(a, row, (g.sel + 1) % g.n);
    } else if (row == ROW_BOTTOM_FPS)
        step_bottom_fps(a, 1, true);
    else if (row == ROW_REPLAY_LEN)
        step_replay_len(a, 1, true);
    else if (row == ROW_INPUTS)
        apply_setting(a, "inputs", a->cfg.inputs ? "off" : "on");
    else if (row == ROW_MP4_INPUTS)
        set_mp4_inputs(a, !a->cfg.mp4Inputs);
    else if (row == ROW_SDTEST)
        open_sdtest(a);
    else if (row == ROW_SOUND)
        apply_setting(a, "audio", a->cfg.audio ? "off" : "on");
    else if (row == ROW_AUTOSTART)
        set_autostart(a, !a->cfg.autostart);
    else if (row == ROW_LID)
        apply_setting(a, "lid_work", a->cfg.lidWork ? "off" : "on");
    else if (row == ROW_STATS)
        set_stats(a, !a->cfg.stats);
    else if (row == ROW_HOTKEY)
        open_hotkey(a);
    else if (row == ROW_UNINSTALL) {
        a->deleteRecordings = false;
        go(a, SCR_UNINSTALL, 1);
    }
}

/* the row in focus in the open group (ROW_*), -1 on the groups' list */
static int set_focus_row(const App *a)
{
    if (a->screen != SCR_SETGROUP || a->setGroup >= GRP_LANG || a->focus < 0 || a->focus >= GROUP_N[a->setGroup])
        return -1;
    return GROUP_ROWS[a->setGroup][a->focus];
}

/* back into a group at one of its rows (from the hotkey and uninstall screens) */
static void open_group_at(App *a, int group, int row)
{
    a->setGroup = group;
    int f = 0;
    for (int i = 0; i < GROUP_N[group]; i++)
        if (GROUP_ROWS[group][i] == row)
            f = i;
    go(a, SCR_SETGROUP, f);
}

/* ------------------------------------------------------------------ update */

static void open_gallery(App *a, const char *select);
static void open_guide(App *a, Screen after);
static void open_help(App *a, Screen back);

static void panel_activate(App *a, int tile)
{
    if (tile == 0) {
        if (a->st.running && a->st.state == REC_STATE_RECORDING)
            recorder_command(REC_CMD_STOP);
        else if (a->st.running) {
            a->pending = ACT_STOP;
            a->busy = T(S_STATE_STOPPING);
        } else {
            a->pending = ACT_START;
            a->busy = T(S_STATE_STARTING);
        }
    } else if (tile == 1)
        open_gallery(a, NULL);
    else if (tile == 2) /* downloading is in the gallery (Y on a recording) */
        open_help(a, SCR_PANEL);
    else if (tile == 3)
        go(a, SCR_SETTINGS, 0);
}

static void open_help(App *a, Screen back)
{
    a->back = back;
    a->backFocus = a->focus;
    a->helpTopic = 0;
    go(a, SCR_HELP, 0);
}

/* Panel focus: 0-3 the tiles (2 x 2; 0 is the recorder's switch, the first tile's upper half), 5 the
   replay switch under it in the same tile (the language is in Settings).
   While a recording runs the first tile is one button (Stop): 5 counts as 0 then. */
enum { FOCUS_REPLAY = 5 };

static void toggle_replay(App *a)
{
    char len[16];
    replay_len_label(a->cfg.replaySec, len, sizeof(len));
    if (!a->cfg.replay && a->sdFreeMb < replay_need_mb(a->cfg.replaySec)) {
        char msg[192];
        snprintf(msg, sizeof(msg), T(S_MSG_REPLAY_SPACE), len, (unsigned)replay_need_mb(a->cfg.replaySec));
        say(a, msg);
        return;
    }
    apply_setting(a, "replay", a->cfg.replay ? "off" : "on");
    a->replayWarned = false;
    if (a->cfg.replay) {
        char hk[48], msg[256];
        hk_label(hk_mask(a->cfg.replayHotkey), hk, sizeof(hk));
        snprintf(msg, sizeof(msg), T(S_MSG_REPLAY_ON), hk, len);
        say(a, msg);
    } else
        say(a, T(S_MSG_REPLAY_OFF));
}

static void update_panel(App *a, const Input *in)
{
    const u32 d = in->down;
    const bool split = !(a->st.running && a->st.state == REC_STATE_RECORDING);   /* the first tile shows both switches */
    int f = a->focus;
    if (f == FOCUS_REPLAY && !split)
        f = 0;
    if (d & KEY_DLEFT)
        f = f < 4 && f % 2 == 1 ? f - 1 : f;
    if (d & KEY_DRIGHT)
        f = f == FOCUS_REPLAY ? 1 : f < 4 && f % 2 == 0 ? f + 1 : f;
    if (d & KEY_DUP)
        f = f == FOCUS_REPLAY ? 0 : f == 2 && split ? FOCUS_REPLAY : f >= 2 ? f - 2 : f;
    if (d & KEY_DDOWN)
        f = f == 0 && split ? FOCUS_REPLAY : f == FOCUS_REPLAY ? 2 : f < 2 ? f + 2 : f;
    a->focus = f;
    if (d & KEY_A) {
        if (f == FOCUS_REPLAY)
            toggle_replay(a);
        else
            panel_activate(a, f);
    }
    if ((d & KEY_Y) && a->files.newest[0]) { /* the last recording: straight into the player */
        open_gallery(a, a->files.newest);
        a->galAutoPlay = true;
    }
    if (d & KEY_SELECT)
        open_help(a, SCR_PANEL);
    if (d & KEY_START)
        a->quit = true;
    for (int i = 0; i < 4; i++)
        if (hit(i == 0 && split ? TILE_REC : TILE[i], in)) {
            a->focus = i;
            panel_activate(a, i);
        }
    if (split && hit(TILE_REPLAY, in)) {
        a->focus = FOCUS_REPLAY;
        toggle_replay(a);
    }
}

static void update_firstrun(App *a, const Input *in)
{
    const u32 d = in->down;
    const bool canStart = a->lumaModules;
    if (d & KEY_DLEFT)
        a->focus = 0;
    if (d & KEY_DRIGHT && canStart)
        a->focus = 1;
    if (!canStart)
        a->focus = 0;
    int act = -1;
    if (d & KEY_A)
        act = a->focus;
    if (d & KEY_B)
        act = 0;
    if (hit(FR_LATER, in))
        act = 0;
    if (hit(FR_START, in) && canStart)
        act = 1;
    if (d & KEY_START)
        a->quit = true;
    if (d & KEY_X)
        next_language(a);
    if (act == 0)
        go(a, SCR_PANEL, 0);
    else if (act == 1) {
        a->pending = ACT_START;
        a->busy = T(S_STATE_STARTING);
    }
}

static void update_help(App *a, const Input *in)
{
    const u32 d = in->down;
    if (d & KEY_DUP && a->helpTopic > 0)
        a->helpTopic--;
    if (d & KEY_DDOWN && a->helpTopic < HELP_TOPICS - 1)
        a->helpTopic++;
    for (int i = 0; i < HELP_TOPICS; i++)
        if (hit(help_row(i), in))
            a->helpTopic = i;
    if ((d & (KEY_B | KEY_SELECT)) || hit(BACK_BTN, in))
        go(a, a->back, a->backFocus);
    if (d & KEY_Y)
        open_guide(a, SCR_HELP);
}

/* ------------------------------------------------------------------ language list */


static void update_lang(App *a, const Input *in)
{
    const u32 d = in->down;
    const bool first = a->langFirst;
    int f = a->focus;
    if ((d & KEY_DUP) && f >= LANG_COLS)
        f -= LANG_COLS;
    if ((d & KEY_DDOWN) && f + LANG_COLS < LANG_COUNT)
        f += LANG_COLS;
    if ((d & KEY_DLEFT) && f % LANG_COLS)
        f--;
    if ((d & KEY_DRIGHT) && f % LANG_COLS < LANG_COLS - 1 && f + 1 < LANG_COUNT)
        f++;
    if (f / LANG_COLS < g_langFirst)
        g_langFirst = f / LANG_COLS;
    if (f / LANG_COLS > g_langFirst + LANG_ROWS_SHOWN - 1)
        g_langFirst = f / LANG_COLS - (LANG_ROWS_SHOWN - 1);
    for (int i = g_langFirst * LANG_COLS; i < LANG_COUNT && i < (g_langFirst + LANG_ROWS_SHOWN) * LANG_COLS; i++)
        if (hit(lang_cell(i, g_langFirst, first), in))
            f = i;
    if (f != a->focus) { /* the screens switch at once, so the choice shows itself; saved on leaving */
        a->focus = f;
        a->cfg.lang = (Lang)f;
        i18n_set_lang(a->cfg.lang);
    }
    if (first) {
        if ((d & KEY_A) || hit(LANG_GO, in)) {
            a->cfg.langSet = true;
            config_set("lang", LANGS[a->cfg.lang].code);
            apply_language(a);
            go(a, SCR_GUIDE, 0);
        }
        return;
    }
    if ((d & (KEY_A | KEY_B)) || hit(SET_BACK, in)) {
        if (a->cfg.lang != g_langSaved)
            config_set("lang", LANGS[a->cfg.lang].code);
        apply_language(a);
        go(a, a->langBack, a->langBackFocus);
    }
}

/* ------------------------------------------------------------------ guide (first run) */

#define GUIDE_PAGES 4
static const Rect GD_SKIP = {12, 198, 110, 30};
static const Rect GD_NEXT = {130, 198, 178, 30};

static void open_guide(App *a, Screen after)
{
    a->afterGuide = after;
    a->guidePage = 0;
    go(a, SCR_GUIDE, 0);
}

static void guide_done(App *a)
{
    if (!a->cfg.guideSeen) {
        a->cfg.guideSeen = true;
        config_set("guide", "done");
        tell_system_stop(a); /* held back until the language was chosen */
    }
    go(a, a->afterGuide, 0);
}

static void update_guide(App *a, const Input *in)
{
    const u32 d = in->down;
    if ((d & (KEY_A | KEY_DRIGHT)) || hit(GD_NEXT, in)) {
        if (a->guidePage + 1 < GUIDE_PAGES)
            a->guidePage++;
        else if (!(d & KEY_DRIGHT))
            guide_done(a);
    }
    if ((d & KEY_DLEFT) && a->guidePage > 0)
        a->guidePage--;
    if ((d & KEY_B) || hit(GD_SKIP, in))
        guide_done(a);
    if (d & KEY_X)
        next_language(a);
}

static void update_settings(App *a, const Input *in)
{
    const u32 d = in->down;
    if (d & KEY_DUP && a->focus > 0)
        a->focus--;
    if (d & KEY_DDOWN && a->focus < GRP_COUNT - 1)
        a->focus++;
    int open = (d & KEY_A) ? a->focus : -1;
    for (int i = 0; i < GRP_COUNT; i++)
        if (hit(grp_card(i), in))
            open = a->focus = i;
    if (open == GRP_LANG)
        open_languages(a, SCR_SETTINGS, GRP_LANG);
    else if (open >= 0) {
        a->setGroup = open;
        go(a, SCR_SETGROUP, 0);
    }
    if ((d & KEY_B) || hit(SET_BACK, in))
        go(a, SCR_PANEL, 3);
}

static void update_setgroup(App *a, const Input *in)
{
    const u32 d = in->down;
    const int n = GROUP_N[a->setGroup];
    if (d & KEY_DUP && a->focus > 0)
        a->focus--;
    if (d & KEY_DDOWN && a->focus < n - 1)
        a->focus++;
    const int row = GROUP_ROWS[a->setGroup][a->focus];
    const int dir = (d & KEY_DRIGHT) ? 1 : (d & KEY_DLEFT) ? -1 : 0;
    Seg g;
    if (dir && row_seg(a, row, &g)) {
        const int o = g.sel + dir;
        if (o >= 0 && o < g.n)
            choose(a, row, o);
    } else if (dir && row == ROW_BOTTOM_FPS)
        step_bottom_fps(a, dir, false);
    else if (dir && row == ROW_REPLAY_LEN)
        step_replay_len(a, dir, false);
    else if (dir) {
        const bool on = dir > 0;
        if (row == ROW_SOUND && on != a->cfg.audio)
            apply_setting(a, "audio", on ? "on" : "off");
        else if (row == ROW_INPUTS && on != a->cfg.inputs)
            apply_setting(a, "inputs", on ? "on" : "off");
        else if (row == ROW_MP4_INPUTS)
            set_mp4_inputs(a, on);
        else if (row == ROW_AUTOSTART)
            set_autostart(a, on);
        else if (row == ROW_LID && on != a->cfg.lidWork)
            apply_setting(a, "lid_work", on ? "on" : "off");
        else if (row == ROW_STATS)
            set_stats(a, on);
    }
    if (d & KEY_A)
        settings_activate(a, row);
    for (int i = 0; i < n && in->tapped; i++) {
        const int r = GROUP_ROWS[a->setGroup][i];
        const Rect rr = set_row(i);
        if (row_seg(a, r, &g)) {
            Rect opt[3] = {{0}};
            seg_layout(rr, g.labels, g.n, opt);
            for (int k = 0; k < g.n; k++)
                if (hit(opt[k], in)) {
                    a->focus = i;
                    choose(a, r, k);
                }
        } else if ((r == ROW_BOTTOM_FPS || r == ROW_REPLAY_LEN) && hit(rr, in)) { /* ◀ on the left of the value, ▶ on the right */
            a->focus = i;
            const float vx = rr.x + rr.w - 40; /* the middle of the value */
            const int dir = in->tap_x >= rr.x + rr.w - 90 && in->tap_x < vx ? -1 : 1;
            if (r == ROW_BOTTOM_FPS)
                step_bottom_fps(a, dir, in->tap_x < rr.x + rr.w - 90);
            else
                step_replay_len(a, dir, in->tap_x < rr.x + rr.w - 90);
        } else if (hit(rr, in)) {
            a->focus = i;
            settings_activate(a, r);
        }
    }
    if ((d & KEY_B) || hit(SET_BACK, in))
        go(a, SCR_SETTINGS, a->setGroup);
}

/* The hotkey screen's two steps: the recording hotkey, then the replay one. */
static u32 hk_step_now(const App *a) { return hk_mask(a->hkStep ? a->cfg.replayHotkey : a->cfg.hotkey); }

static Str hk_step_problem(const App *a, u32 m)
{
    const Str p = hk_problem(m);
    return p == S_COUNT && a->hkStep == 1 && hk_overlap(m, hk_mask(a->cfg.hotkey)) ? S_HK_ERR_OTHER : p;
}

static void hotkey_set(App *a, u32 m, bool tell)
{
    char cfg[48], label[64], msg[160];
    hk_text(m, cfg, sizeof(cfg));
    hk_label(m, label, sizeof(label));
    apply_setting(a, a->hkStep ? "replay_hotkey" : "hotkey", cfg);
    if (tell) {
        snprintf(msg, sizeof(msg), T(a->hkStep ? S_HK_SAVED2 : S_HK_SAVED), label);
        say(a, msg);
    }
}

/* This step is done (saved or kept): on to the replay hotkey, then back to Settings. */
static void hotkey_next(App *a)
{
    if (a->hkStep == 0) {
        a->hkStep = 1;
        a->hkHeld = a->hkCand = 0;
        a->hkReview = false;
        a->hkWaitRelease = true;
        return;
    }
    /* kept as it was, but the recording hotkey chosen a step ago now holds it (or the other way) */
    if (hk_overlap(hk_step_now(a), hk_mask(a->cfg.hotkey)))
        hotkey_set(a, hk_replay_default(hk_mask(a->cfg.hotkey)), true);
    open_group_at(a, GRP_RECORDER, ROW_HOTKEY);
}

static void hotkey_save(App *a, u32 m)
{
    hotkey_set(a, m, true);
    hotkey_next(a);
}

/* Capture: the held buttons are the candidate; 1 s steady with 2-3 buttons sets it. Every button
   can be part of a hotkey, so until they are released nothing else reacts; START cancels (it
   cannot be in a hotkey). Review: A saves, B captures again. The touch buttons always work. */
static void update_hotkey(App *a, const Input *in)
{
    const u32 held = in->held & HK_ALLOWED;
    if ((in->down & KEY_START) || hit(HK_BTN[1], in)) {   /* keep this one */
        hotkey_next(a);
        return;
    }
    if (hit(HK_BTN[0], in)) {
        hotkey_save(a, a->hkStep ? hk_replay_default(hk_mask(a->cfg.hotkey)) : hk_mask(HK_DEFAULT));
        return;
    }
    if (hit(HK_BTN[2], in) && a->hkReview && hk_step_problem(a, a->hkCand) == S_COUNT) {
        hotkey_save(a, a->hkCand);
        return;
    }
    if (a->hkWaitRelease) {
        if (!held)
            a->hkWaitRelease = false;
        return;
    }
    if (a->hkReview) {
        if ((in->down & KEY_A) && hk_step_problem(a, a->hkCand) == S_COUNT)
            hotkey_save(a, a->hkCand);
        else if (in->down & KEY_B) {
            a->hkReview = false;
            a->hkCand = a->hkHeld = 0;
            a->hkWaitRelease = true;
        }
        return;
    }
    if (held != a->hkHeld) {
        a->hkHeld = held;
        a->hkSince = a->frame;
    }
    const int n = hk_count(held);
    if (n >= 2 && n <= 3 && a->frame - a->hkSince >= 60) {
        a->hkCand = held;
        a->hkReview = true;
        a->hkWaitRelease = true;
    }
}

/* ------------------------------------------------------------------ SD card test */

static const Rect SDT_BTN[2] = {{12, 200, 144, 28}, {164, 200, 144, 28}};   /* Back, Start / Test again */
static SdTestResult g_sdt;

static void sdtest_hold(bool on)   /* the other readers of the card wait meanwhile */
{
    gal_hold(on);
    g_infoHold = on;
    prepare_hold(PREP_HOLD_SDTEST, on);
}

static void open_sdtest(App *a)
{
    memset(&g_sdt, 0, sizeof(g_sdt));
    go(a, SCR_SDTEST, 1);
}

static void update_sdtest(App *a, const Input *in)
{
    const u32 d = in->down;
    if (g_sdt.state == SDT_RUNNING) {
#ifdef UI_TOUR
        return;   /* the tour's pretend states stay as they are */
#endif
        g_sdt = sdtest_poll();
        if (g_sdt.state != SDT_RUNNING)
            sdtest_hold(false);
        return;   /* nothing else until the test is done (it writes to the card) */
    }
    if (d & KEY_DLEFT)
        a->focus = 0;
    if (d & KEY_DRIGHT)
        a->focus = 1;
    int act = -1;
    if (d & KEY_A)
        act = a->focus;
    if (hit(SDT_BTN[0], in) || (d & KEY_B))
        act = 0;
    if (hit(SDT_BTN[1], in))
        act = 1;
    if (act == 0)
        open_group_at(a, GRP_APP, ROW_SDTEST);
    else if (act == 1) {
        if (a->st.running && a->st.state == REC_STATE_RECORDING) {
            say(a, T(S_SDT_BUSY));
            return;
        }
        sdtest_hold(true);
        if (sdtest_start())
            g_sdt.state = SDT_RUNNING, g_sdt.permille = 0;
        else
            sdtest_hold(false);
    }
}

static void update_uninstall(App *a, const Input *in)
{
    const u32 d = in->down;
    if (d & KEY_DUP)
        a->focus = 0;
    if (d & KEY_DDOWN && a->focus == 0)
        a->focus = 1;
    if (d & KEY_DLEFT && a->focus == 2)
        a->focus = 1;
    if (d & KEY_DRIGHT && a->focus == 1)
        a->focus = 2;
    int act = -1;
    if (d & KEY_A)
        act = a->focus;
    if (hit(UN_CHECK, in))
        act = 0;
    if (hit(UN_BTN[0], in))
        act = 1;
    if (hit(UN_BTN[1], in))
        act = 2;
    if (d & KEY_B)
        act = 1;
    if (act == 0) {
        a->focus = 0;
        a->deleteRecordings = !a->deleteRecordings;
    } else if (act == 1)
        open_group_at(a, GRP_APP, ROW_UNINSTALL);
    else if (act == 2) {
        a->pending = ACT_UNINSTALL;
        a->busy = T(S_UN_WORKING);
    }
}

/* ------------------------------------------------------------------ gallery */

#define GAL_ROWS 5
static Rect gal_row(int i) { return (Rect){10, 36 + i * 40, 300, 36}; }
static const Rect DEL_BTN[2] = {{30, 150, 124, 30}, {166, 150, 124, 30}}; /* cancel, delete */

static C3D_Tex g_galTex[2];                 /* the selected recording: top, bottom */
static C3D_Tex g_thumbTex[GAL_ROWS];
static const u16 *g_thumbSrc[GAL_ROWS];     /* which thumbnail each row's texture holds */
static bool g_galTexOk;

static void open_player(App *a, const GalItem *it);
static void open_phone(App *a, const char *name);

static void open_gallery(App *a, const char *select)
{
    a->galDeleting = false; /* a deletion left running when the gallery closed has ended with it */
    g_galTexOk = C3D_TexInit(&g_galTex[0], GAL_TEX_W, GAL_TEX_H, GPU_RGB565) &&
                 C3D_TexInit(&g_galTex[1], GAL_TEX_W, GAL_TEX_H, GPU_RGB565);
    for (int i = 0; i < GAL_ROWS && g_galTexOk; i++) {
        g_galTexOk = C3D_TexInit(&g_thumbTex[i], GAL_THUMB_W, GAL_THUMB_H, GPU_RGB565);
        g_thumbSrc[i] = NULL;
    }
    for (int i = 0; i < 2; i++)
        C3D_TexSetFilter(&g_galTex[i], GPU_LINEAR, GPU_LINEAR);
    a->galSel = a->galTop = 0;
    a->galVersion = a->galPreviewVersion = 0;
    a->galConfirm = false;
    snprintf(a->galOpen, sizeof(a->galOpen), "%s", select ? select : "");
    a->galAutoPlay = false;
    a->galClipsDone = prepare_clips_done();
    gal_start();
    go(a, SCR_GALLERY, 0);
}

static void close_gallery(App *a)
{
    gal_stop();
    for (int i = 0; i < 2; i++)
        C3D_TexDelete(&g_galTex[i]);
    for (int i = 0; i < GAL_ROWS; i++)
        C3D_TexDelete(&g_thumbTex[i]);
    g_galTexOk = false;
    refresh(a, true);
    go(a, SCR_PANEL, 1);
}

/* Returns true when the delete was confirmed. */
static bool update_gallery_confirm(App *a, const Input *in)
{
    const u32 d = in->down;
    if (d & (KEY_DLEFT | KEY_DRIGHT))
        a->galConfirmFocus ^= 1;
    bool del = (d & KEY_A) && a->galConfirmFocus == 1;
    bool cancel = (d & KEY_B) || ((d & KEY_A) && a->galConfirmFocus == 0) || hit(DEL_BTN[0], in);
    del = del || hit(DEL_BTN[1], in);
    if (del || cancel)
        a->galConfirm = false;
    return del;
}

static void update_gallery(App *a, const Input *in)
{
    bool ok;
    if (gal_take_deleted(&ok)) {
        a->galDeleting = false;
        say(a, T(ok ? S_DELETED : S_DEL_FAILED));
        refresh(a, true);
    }
    /* a replay clip became a recording: the list is read again, the selection stays on its recording */
    if (prepare_clips_done() != a->galClipsDone && !a->galConfirm && !a->galDeleting) {
        a->galClipsDone = prepare_clips_done();
        gal_lock();
        const GalList *old = gal_list();
        if (old->loaded && a->galSel < old->count)
            snprintf(a->galOpen, sizeof(a->galOpen), "%s", old->items[a->galSel].name);
        gal_unlock();
        gal_stop();
        for (int i = 0; i < GAL_ROWS; i++)
            g_thumbSrc[i] = NULL;
        a->galVersion = a->galPreviewVersion = 0;
        gal_start();
        refresh(a, true);
    }
    gal_lock();
    const GalList *l = gal_list();
    if (l->version != a->galVersion) {
        a->galVersion = l->version;
        for (int i = 0; l->loaded && a->galOpen[0] && i < l->count; i++)
            if (strcmp(l->items[i].name, a->galOpen) == 0)
                a->galSel = i;
        if (l->loaded)
            a->galOpen[0] = 0;
    }
    const int n = l->count;
    if (a->galSel >= n)
        a->galSel = n ? n - 1 : 0;
    const GalItem *it = n ? &l->items[a->galSel] : NULL;
    const u32 d = in->down;
    char del[32] = "", want[32] = "";
    if (a->galConfirm && it) {
        if (update_gallery_confirm(a, in))
            snprintf(del, sizeof(del), "%s", it->name);
    } else {
        a->galConfirm = false;
        int sel = a->galSel;
        if (d & KEY_DUP)
            sel--;
        if (d & KEY_DDOWN)
            sel++;
        if (d & KEY_L)
            sel -= GAL_ROWS;
        if (d & KEY_R)
            sel += GAL_ROWS;
        bool play = (d & KEY_A) != 0;
        for (int i = 0; i < GAL_ROWS && a->galTop + i < n; i++)
            if (hit(gal_row(i), in)) {
                play = play || a->galTop + i == a->galSel; /* a second tap opens it */
                sel = a->galTop + i;
            }
        a->galSel = sel < 0 ? 0 : sel >= n ? (n ? n - 1 : 0) : sel;
        if (it && it->info && a->galAutoPlay && !a->galOpen[0]) { /* Y on the panel */
            a->galAutoPlay = false;
            play = it->readable;
        }
        if (play && it && it->info && it->readable) {
            GalItem copy = *it;
            gal_unlock();
            open_player(a, &copy);
            return;
        }
        if ((d & KEY_X) && it && !a->galDeleting) { /* one deletion at a time: it can take seconds */
            a->galConfirm = true;
            a->galConfirmFocus = 0;
        }
        if ((d & KEY_Y) && it && it->info && it->readable) {
            char name[32];
            snprintf(name, sizeof(name), "%s", it->name);
            gal_unlock();
            open_phone(a, name);
            return;
        }
        if ((d & KEY_B) || hit(BACK_BTN, in)) {
            gal_unlock();
            close_gallery(a);
            return;
        }
    }
    if (a->galSel < a->galTop)
        a->galTop = a->galSel;
    if (a->galSel >= a->galTop + GAL_ROWS)
        a->galTop = a->galSel - GAL_ROWS + 1;
    if (n)
        snprintf(want, sizeof(want), "%s", l->items[a->galSel].name);
    gal_unlock();
    /* both take the gallery lock themselves */
    if (want[0])
        gal_want_preview(want);
    if (del[0]) {
        app_log("delete: %s", del);
        a->galDeleting = true;
        say(a, T(S_DELETING)); /* stopping its conversion first can take a few seconds */
        gal_delete(del);
    }
}

/* ------------------------------------------------------------------ player */

#define PL_SHOW_FRAMES (60 * 3)            /* controls stay up 3 s after the last input */
static const Rect PL_BAR = {16, 154, 288, 16};     /* the progress bar's touch area */
static const Rect PL_SOUND = {14, 182, 40, 28};
static const Rect PL_RW = {94, 180, 32, 32}, PL_PLAY = {141, 177, 38, 38}, PL_FF = {194, 180, 32, 32};
static C3D_Tex g_plTex[2];
static bool g_plTexOk;
static bool g_plInputs;   /* X in the player: the buttons pressed shown (kept for the session) */

/* The buttons pressed in the player (codec/source/inputs.c's corner panel, drawn with the UI's shapes):
   the panel in the top screen's bottom-left corner, a touch as a ring on the bottom screen. */
static void pl_button(float x, float y, float w, float h, bool held)
{
    if (held)
        ui_rrect(x, y, w, h, h / 2, COL_ACCENT);
    else
        ui_frame(x, y, w, h, h / 2, 1.2f, COL_SOFT);
}

static void draw_inputs_top(const InputState *st)
{
    const float x0 = 6, y0 = 240 - 6 - INPUT_PANEL_H;
    const unsigned k = st->keys;
    ui_rrect(x0, y0, INPUT_PANEL_W, INPUT_PANEL_H, 8, RGBA(0x0E, 0x10, 0x14, 0x9E));
    pl_button(x0 + 6, y0 + 4, 26, 6, k & R3D_KEY_L);
    pl_button(x0 + 84, y0 + 4, 26, 6, k & R3D_KEY_R);
    ui_frame(x0 + 11, y0 + 17, 16, 16, 8, 1.2f, COL_SOFT);   /* the stick */
    ui_circle(x0 + 19 + st->cx * 5.0f / 127, y0 + 25 - st->cy * 5.0f / 127, 3.2f, COL_SOFT);
    const float dx = x0 + 19, dy = y0 + 47, u = 6;   /* the D-pad */
    ui_rrect(dx - u * 1.5f, dy - u / 2, u * 3, u, 1.5f, COL_SURFACE2);
    ui_rrect(dx - u / 2, dy - u * 1.5f, u, u * 3, 1.5f, COL_SURFACE2);
    if (k & R3D_KEY_UP)
        ui_rrect(dx - u / 2, dy - u * 1.5f, u, u, 1, COL_ACCENT);
    if (k & R3D_KEY_DOWN)
        ui_rrect(dx - u / 2, dy + u / 2, u, u, 1, COL_ACCENT);
    if (k & R3D_KEY_LEFT)
        ui_rrect(dx - u * 1.5f, dy - u / 2, u, u, 1, COL_ACCENT);
    if (k & R3D_KEY_RIGHT)
        ui_rrect(dx + u / 2, dy - u / 2, u, u, 1, COL_ACCENT);
    pl_button(x0 + 45, y0 + 46, 12, 4.4f, k & R3D_KEY_SELECT);
    pl_button(x0 + 59, y0 + 46, 12, 4.4f, k & R3D_KEY_START);
    const float ax = x0 + 97, ay = y0 + 31, d = 9, r = 4;   /* X top, Y left, A right, B bottom */
    const struct { float x, y; unsigned key; } abxy[4] = {
        {ax, ay - d, R3D_KEY_X}, {ax - d, ay, R3D_KEY_Y}, {ax + d, ay, R3D_KEY_A}, {ax, ay + d, R3D_KEY_B}};
    for (int i = 0; i < 4; i++) {
        if (k & abxy[i].key)
            ui_circle(abxy[i].x, abxy[i].y, r, COL_ACCENT);
        else
            ui_frame(abxy[i].x - r, abxy[i].y - r, 2 * r, 2 * r, r, 1.2f, COL_SOFT);
    }
}

static void draw_inputs_bottom(const InputState *st)
{
    if (st->tx == R3D_TOUCH_NONE)
        return;
    const float x = st->tx * 2 + 1.0f, y = st->ty + 0.5f;
    ui_frame(x - 8.5f, y - 8.5f, 17, 17, 8.5f, 3, RGBA(0x0E, 0x10, 0x14, 0x8C));
    ui_frame(x - 7, y - 7, 14, 14, 7, 2, COL_ACCENT);
    ui_circle(x, y, 2, COL_ACCENT);
}

static void open_player(App *a, const GalItem *it)
{
    gal_hold(true);
    g_infoHold = true;
    prepare_hold(PREP_HOLD_PLAYER, true); /* the player needs the CPU and the card */
    g_plTexOk = C3D_TexInit(&g_plTex[0], PLAYER_TEX_W, PLAYER_TEX_H, GPU_RGB565) &&
                C3D_TexInit(&g_plTex[1], PLAYER_TEX_W, PLAYER_TEX_H, GPU_RGB565);
    app_log("play: %s (%llu KB, %lu ms, info %d)", it->name, (unsigned long long)(it->bytes >> 10),
            (unsigned long)it->durationMs, it->info);
    if (!g_plTexOk || !player_start(it->name, it->info ? it->durationMs : 0)) {
        if (g_plTexOk) {
            C3D_TexDelete(&g_plTex[0]);
            C3D_TexDelete(&g_plTex[1]);
        }
        g_plTexOk = false;
        gal_hold(false);
        g_infoHold = false;
        prepare_hold(PREP_HOLD_PLAYER, false);
        say(a, T(S_PLAY_FAILED));
        return;
    }
    /* the file has sound and the DSP cannot play it: most often no DSP firmware dump (DSP1); said
       once a session (12 players without sound in the field logs, 2026-11-19) */
    static bool noDspSaid;
    if (player_file_audio() && !player_has_audio() && !noDspSaid) {
        noDspSaid = true;
        say(a, T(S_MSG_NO_DSP));
    }
    for (int i = 0; i < 2; i++)
        C3D_TexSetFilter(&g_plTex[i], GPU_LINEAR, GPU_LINEAR);
    a->playerShowUntil = a->frame + PL_SHOW_FRAMES;
    go(a, SCR_PLAYER, 0);
}

static void close_player(App *a)
{
    player_stop();
    C3D_TexDelete(&g_plTex[0]);
    C3D_TexDelete(&g_plTex[1]);
    g_plTexOk = false;
    gal_hold(false);
    g_infoHold = false;
    prepare_hold(PREP_HOLD_PLAYER, false);
    go(a, SCR_GALLERY, 0);
}

static void update_player(App *a, const Input *in)
{
    const u32 d = in->down;
    if (d || in->tapped)
        a->playerShowUntil = a->frame + PL_SHOW_FRAMES;
    const s64 t = player_time_ms();
    if ((d & KEY_A) || hit(PL_PLAY, in))
        player_toggle_pause();
    if ((d & KEY_DLEFT) || hit(PL_RW, in))
        player_seek(t - 10000);
    if ((d & KEY_DRIGHT) || hit(PL_FF, in))
        player_seek(t + 10000);
    if ((d & KEY_Y) || hit(PL_SOUND, in))
        player_set_mute(!player_muted());
    if (d & KEY_X && player_has_inputs())   /* the buttons pressed, over the video */
        g_plInputs = !g_plInputs;
    if (hit(PL_BAR, in) && player_duration_ms())
        player_seek((s64)(in->tap_x - PL_BAR.x) * player_duration_ms() / PL_BAR.w);
    if (d & KEY_B)
        close_player(a);
}

/* ------------------------------------------------------------------ to phone */

static const Rect PH_STOP = {100, 200, 120, 30};
static u8 g_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
static bool g_qrOk;
static u64 g_prepSeenAt;   /* the preparation's progress at the first look, for the time left */
static int g_prepSeen;

static void phone_start(App *a)
{
    const ServerState st = server_start(a->phoneName, LANGS[a->cfg.lang].code, APP_VERSION, a->new3ds);
    g_qrOk = false;
    ServerStats ss;
    server_stats(&ss);
    static const char *const states[] = {"off", "no Wi-Fi", "failed", "running"};
    app_log("download: %s, server %s %s, prepare %d", a->phoneName, (unsigned)st < 4 ? states[st] : "?", ss.url,
            (int)prepare_state(a->phoneName, NULL));
    if (st == SRV_RUNNING) {
        u8 tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
        g_qrOk = qrcodegen_encodeText(ss.url, tmp, g_qr, qrcodegen_Ecc_MEDIUM, 1, 6, qrcodegen_Mask_AUTO, true);
    }
}

/* From the gallery: offer this recording (the gallery stays open underneath). */
static void open_phone(App *a, const char *name)
{
    snprintf(a->phoneName, sizeof(a->phoneName), "%.31s", name);
    prepare_first(name); /* the MP4 for the phone: this recording next */
    g_prepSeen = -1;
    gal_hold(true); /* the SD card is the server's meanwhile */
    go(a, SCR_PHONE, 0);
    phone_start(a);
}

static void close_phone(App *a)
{
    prepare_hold(PREP_HOLD_SEND, false);
    server_stop();
    gal_hold(false);
    go(a, SCR_GALLERY, 0);
}

static void update_phone(App *a, const Input *in)
{
    ServerStats ss;
    server_stats(&ss);
    /* while the MP4 goes to the phone, other recordings wait: the Wi-Fi needs the CPU */
    const bool sending = ss.connections > 0 && prepare_state(a->phoneName, NULL) == PS_READY;
    prepare_hold(PREP_HOLD_SEND, sending);
    if ((in->down & KEY_A) && ss.state != SRV_RUNNING) {
        server_stop();
        phone_start(a);
    }
    if ((in->down & KEY_B) || hit(BACK_BTN, in) || (ss.state == SRV_RUNNING && hit(PH_STOP, in)))
        close_phone(a);
}

void app_update(App *a, const Input *in)
{
    a->frame++;
    toast_tick(a);
    if (a->pending) { /* the worker runs; screens keep drawing with the spinner */
        run_pending(a);
        return;
    }
    if (a->frame % 20 == 0)
        refresh(a, a->frame % 120 == 0);
    switch (a->screen) {
    case SCR_PANEL: update_panel(a, in); break;
    case SCR_FIRSTRUN: update_firstrun(a, in); break;
    case SCR_HELP: update_help(a, in); break;
    case SCR_GUIDE: update_guide(a, in); break;
    case SCR_LANG: update_lang(a, in); break;
    case SCR_SETTINGS: update_settings(a, in); break;
    case SCR_SETGROUP: update_setgroup(a, in); break;
    case SCR_HOTKEY: update_hotkey(a, in); break;
    case SCR_GALLERY: update_gallery(a, in); break;
    case SCR_PLAYER: update_player(a, in); break;
    case SCR_PHONE: update_phone(a, in); break;
    case SCR_UNINSTALL: update_uninstall(a, in); break;
    case SCR_SDTEST: update_sdtest(a, in); break;
    case SCR_UNINSTALLED:
        if ((in->down & (KEY_A | KEY_B | KEY_START)) || in->tapped)
            a->quit = true;
        break;
    }
}

/* ------------------------------------------------------------------ drawing helpers */

static void stat_card(float x, float y, float w, const char *label, const char *value, const char *sub)
{
    ui_card(x, y, w, 50, 8, COL_SURFACE);
    ui_text(x + 8, y + 6, 9, FONT_REG, COL_MUTED, label);
    ui_text(x + 8, y + 17, 13, FONT_BOLD, COL_TEXT, value);
    if (sub)
        ui_text(x + 8, y + 34, 9, FONT_REG, COL_MUTED, sub);
}

static void button(Rect r, const char *label, int style, bool focused) /* 0 secondary, 1 primary, 2 danger, 3 disabled */
{
    const u32 bg = style == 1 ? COL_TEXT : style == 2 ? COL_DANGER : COL_SURFACE;
    const u32 fg = style == 1 ? COL_BG : style == 2 ? COL_WHITE : style == 3 ? COL_FAINT : COL_TEXT;
    ui_rrect(r.x, r.y, r.w, r.h, 7, bg);
    ui_text_center(r.x + r.w / 2, r.y + r.h / 2 - 7, 10, FONT_BOLD, fg, label);
    if (focused)
        focus_ring(r, 7);
}

static void check_row(float y, bool ok, bool bad, const char *label, const char *value)
{
    ui_card(14, y, 372, 26, 7, bad ? COL_ERR_BG : COL_SURFACE);
    ui_icon(ok ? ICON_CHECK : bad ? ICON_CROSS : ICON_INFO, 28, y + 13, 14, ok ? COL_OK : bad ? COL_ERR : COL_INFO);
    ui_text(42, y + 6, 10, FONT_REG, COL_TEXT, label);
    ui_text_right(378, y + 7, 9, FONT_REG, bad ? COL_ERR : COL_MUTED, value);
}

static void navbar(const char *title)
{
    ui_rrect(BACK_BTN.x, BACK_BTN.y, BACK_BTN.w, BACK_BTN.h, 6, COL_SURFACE);
    ui_icon(ICON_BACK, BACK_BTN.x + 11, BACK_BTN.y + 11, 13, COL_TEXT);
    ui_text(42, 13, 11, FONT_BOLD, COL_TEXT, title);
}

static void format_gb(char *buf, size_t cap, u32 mb)
{
    if (mb >= 1024)
        snprintf(buf, cap, "%lu%c%lu %s", (unsigned long)(mb / 1024), i18n_decimal(), (unsigned long)(mb % 1024 * 10 / 1024), T(S_GB));
    else
        snprintf(buf, cap, "%lu %s", (unsigned long)mb, T(S_MB));
}

/* yyyymmdd_hhmmss -> "Nov 3, 2026 · 17:26" / "03.11.2026 · 17:26" (i18n_date) */
static void format_name(char *buf, size_t cap, const char *name)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    char date[24];
    if (sscanf(name, "%4d%2d%2d_%2d%2d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12)
        snprintf(buf, cap, "%s", name);
    else
        snprintf(buf, cap, "%s · %02d:%02d", i18n_date(y, mo, d, true, date, sizeof(date)), h, mi);
}

/* the top screen alone still fills the card in 3D; the bottom one alone takes about half of it */
static u32 mb_per_min(const App *a)
{
    const u32 perMin = MB_PER_MIN_BOTH_30 * (u32)a->cfg.fps / 30u;
    return a->cfg.screens == SCREENS_BOTTOM ? perMin / 2 : perMin;
}

static u32 minutes_left(const App *a)
{
    const u32 perMin = mb_per_min(a);
    return perMin ? a->sdFreeMb / perMin : 0;
}

/* ------------------------------------------------------------------ top screen */

/* The newest recording's thumbnail, uploaded here (after C3D_FrameBegin) when it changed. */
static bool draw_last_thumb(const char *newest, float x, float y)
{
    if (!g_lastTexOk || !g_lastThumb)
        return false;
    LightLock_Lock(&g_infoLock);
    const bool ok = g_lastOk && strcmp(g_lastName, newest) == 0;
    if (ok && g_lastSeq != g_lastSeen) {
        memcpy(g_lastTex.data, g_lastThumb, GAL_LAST_W * GAL_LAST_H * 2);
        C3D_TexFlush(&g_lastTex);
        g_lastSeen = g_lastSeq;
    }
    LightLock_Unlock(&g_infoLock);
    if (!ok)
        return false;
    ui_tex(&g_lastTex, 100, 60, x, y, 1);
    ui_frame(x, y, 100, 60, 2, 1, COL_FAINT);
    return true;
}

/* "Saving clip 42% · 2 more" while replay clips are made into recordings, else "Joining recordings
   42% · 1 more" while recordings paused by the recorder are joined (prepare.c); false: none. */
static bool clips_text(char *buf, size_t cap)
{
    int pm = -1;
    int n = prepare_clips(&pm);
    const bool joins = n <= 0;
    if (joins)
        n = prepare_joins(&pm);
    if (n <= 0)
        return false;
    char one[64];
    if (pm >= 0)
        snprintf(one, sizeof(one), T(joins ? S_JOIN_PCT : S_CLIP_PCT), pm / 10);
    else
        snprintf(one, sizeof(one), "%s", T(joins ? S_JOIN_WAIT : S_CLIP_WAIT));
    if (n > 1)
        snprintf(buf, cap, T(S_CLIP_MORE), one, n - 1);
    else
        snprintf(buf, cap, "%s", one);
    return true;
}

static void draw_panel_top(App *a)
{
    char buf[96], v[48], s[48];
    snprintf(buf, sizeof(buf), "3DS Recorder %s%s%s · %s", APP_VERSION, APP_CHANNEL[0] ? " " : "", APP_CHANNEL,
             a->new3ds ? "New 3DS" : "Old 3DS"); /* the version: beta reports name it */
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, buf);
    const bool recording = a->st.running && a->st.state == REC_STATE_RECORDING;
    if (!a->st.running || a->st.state == REC_STATE_EXITED)
        snprintf(buf, sizeof(buf), "%s", T(S_STATE_OFF));
    else if (recording) {
        char t[16];
        snprintf(t, sizeof(t), "%02lu:%02lu", (unsigned long)(a->st.recSeconds / 60), (unsigned long)(a->st.recSeconds % 60));
        snprintf(buf, sizeof(buf), T(S_STATE_RECORDING), t);
    } else
        snprintf(buf, sizeof(buf), "%s",
                 T(a->st.state == REC_STATE_STARTING ? S_STATE_STARTING
                   : a->st.state == REC_STATE_SAVING ? S_STATE_SAVING
                   : a->st.state == REC_STATE_ERROR  ? S_STATE_ERROR
                                                      : S_STATE_READY));
    ui_text(14, 25, 17, FONT_BOLD, COL_TEXT, buf);
    if (recording)
        ui_pill(386, 16, "REC", COL_ERR, COL_ERR_BG, true);
    else if (a->st.running)
        ui_pill(386, 16, T(S_PILL_ON), COL_OK, COL_OK_BG, true);
    else
        ui_pill(386, 16, T(S_PILL_OFF), COL_SOFT, COL_SURFACE, false);

    const float w = (372 - 3 * 7) / 4.0f, y = 60;
    format_gb(v, sizeof(v), a->sdFreeMb);
    snprintf(s, sizeof(s), T(S_MINUTES), (unsigned)minutes_left(a));
    if (recording) {
        char fr[24], sk[32], mb[24], rate[24];
        snprintf(fr, sizeof(fr), "%lu", (unsigned long)(a->st.framesTop + a->st.framesBot));
        snprintf(sk, sizeof(sk), T(S_SKIPPED), (unsigned)a->st.dropped);
        snprintf(mb, sizeof(mb), "%lu %s", (unsigned long)(a->st.recKb / 1024), T(S_MB));
        const u32 kbs = a->st.recSeconds ? a->st.recKb / a->st.recSeconds : 0;
        snprintf(rate, sizeof(rate), "≈ %lu%c%lu %s", (unsigned long)(kbs / 1024), i18n_decimal(), (unsigned long)(kbs % 1024 * 10 / 1024), T(S_MBS));
        stat_card(14, y, w, T(S_PROFILE), T(S_PROFILE_STD), "XR32");
        stat_card(14 + (w + 7), y, w, T(S_FRAMES), fr, sk);
        stat_card(14 + 2 * (w + 7), y, w, T(S_FILE), mb, rate);
        stat_card(14 + 3 * (w + 7), y, w, T(S_SD_FREE), v, s);
        ui_card(14, 120, 372, 106, 8, COL_SURFACE);
        ui_icon(ICON_INFO, 30, 136, 14, COL_INFO);
        ui_text(44, 129, 10, FONT_BOLD, COL_TEXT, T(S_WHOLE_CONSOLE));
        ui_text_wrap(44, 145, 330, 9, FONT_REG, COL_SOFT, T(S_WHOLE_CONSOLE_BODY));
    } else {
        char fps[16];
        snprintf(fps, sizeof(fps), "%d fps", a->cfg.fps);
        stat_card(14, y, w, T(S_PROFILE), T(S_PROFILE_STD), "XR32");
        stat_card(14 + (w + 7), y, w, T(S_FPS), fps, T(S_TARGET));
        stat_card(14 + 2 * (w + 7), y, w, T(S_SCREENS), T(a->cfg.screens == SCREENS_BOTH ? S_BOTH : a->cfg.screens == SCREENS_TOP ? S_TOP : S_BOTTOM),
                  T(a->cfg.audio ? S_SOUND_ON : S_SOUND_OFF));
        stat_card(14 + 3 * (w + 7), y, w, T(S_SD_FREE), v, s);
        ui_card(14, 120, 372, 106, 8, COL_SURFACE);
        if (a->files.newest[0]) {
            if (!draw_last_thumb(a->files.newest, 24, 136)) {
                ui_rrect(24, 136, 100, 60, 5, COL_SURFACE2);
                ui_icon(ICON_PLAY, 74, 166, 20, COL_FAINT);
            }
            ui_text(136, 140, 9, FONT_REG, COL_MUTED, T(S_LAST));
            format_name(buf, sizeof(buf), a->files.newest);
            ui_text(136, 154, 11, FONT_BOLD, COL_TEXT, buf);
            char cnt[40];
            snprintf(buf, sizeof(buf), "%llu %s · %s", (unsigned long long)(a->files.newestBytes >> 20), T(S_MB),
                     i18n_recordings(a->files.count, cnt, sizeof(cnt)));
            ui_text(136, 172, 9, FONT_REG, COL_MUTED, buf);
            int pm = -1; /* its MP4 for the phone */
            const PrepareState ps = prepare_state(a->files.newest, &pm);
            if (ps == PS_READY)
                ui_text_right(376, 140, 9, FONT_REG, COL_OK, T(S_GAL_PREP_READY));
            else if (ps == PS_WORKING || (ps == PS_QUEUED && pm > 0 && a->cfg.convertAuto)) { /* manual: a paused job waits for a Download */
                snprintf(buf, sizeof(buf), T(S_GAL_PREP_PCT), pm / 10);
                ui_text_right(376, 140, 9, FONT_REG, ps == PS_WORKING ? COL_SOFT : COL_FAINT, buf);
            }
            ui_key_hint(136, 200, "Y", T(S_OPEN));
        } else {
            ui_text(28, 150, 11, FONT_BOLD, COL_TEXT, T(S_NO_RECORDINGS));
            char hk[48], hint[96];
            hk_label(hk_mask(a->cfg.hotkey), hk, sizeof(hk));
            snprintf(hint, sizeof(hint), T(S_NO_RECORDINGS_HINT), hk);
            ui_text(28, 168, 9, FONT_REG, COL_MUTED, hint);
        }
        if (clips_text(buf, sizeof(buf)))   /* replay clips on their way to being recordings */
            ui_text_right(376, 204, 9, FONT_REG, COL_SOFT, buf);
    }
}

static void draw_firstrun_top(App *a)
{
    char val[64], gb[24];
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, T(S_FR_EYEBROW));
    ui_text(14, 25, 17, FONT_BOLD, COL_TEXT, T(S_FR_ALMOST));
    const u32 v = a->lumaVersion;
    if (v)
        snprintf(val, sizeof(val), T(S_FR_LUMA_OK), (unsigned long)(v >> 16), (unsigned long)((v >> 8) & 0xFF));
    check_row(58, v != 0, v == 0, T(S_FR_LUMA), v ? val : T(S_FR_LUMA_UNKNOWN));
    check_row(89, a->lumaModules, !a->lumaModules, T(S_FR_OPTION), T(a->lumaModules ? S_FR_ON : S_FR_OFF));
    check_row(120, a->moduleFile == MODULE_SAME, false, T(S_FR_MODULE),
              T(a->moduleFile == MODULE_SAME ? S_FR_MODULE_OK
                : a->moduleFile == MODULE_DIFFERENT ? S_FR_MODULE_UPDATE
                                                    : S_FR_MODULE_MISSING));
    check_row(151, a->st.running, !a->st.running, T(S_FR_RUNNING), a->st.running ? T(S_FR_ON) : T(S_FR_RUNNING_NO));
    format_gb(gb, sizeof(gb), a->sdFreeMb);
    snprintf(val, sizeof(val), T(S_FR_SD_FREE), gb);
    check_row(182, a->sdFreeMb > 256, a->sdFreeMb <= 256, T(S_FR_SD), val);
}

static const Str HELP_TITLE[HELP_TOPICS] = {S_H1, S_H2, S_H3, S_H4, S_H5, S_H6, S_H7, S_H8, S_H9, S_H10};
static const Str HELP_BODY[HELP_TOPICS] = {S_H1_BODY, S_H2_BODY, S_H3_BODY, S_H4_BODY, S_H5_BODY, S_H6_BODY, S_H7_BODY, S_H8_BODY, S_H9_BODY, S_H10_BODY};

/* The author's Telegram channel (news, questions), opposite "Y How to use". */
static void help_channel(void)
{
    ui_text_right(386, 219.5f, 9, FONT_BOLD, COL_SOFT, "Telegram · t.me/side_quest_ua");
}

static void draw_help_top(App *a)
{
    char buf[48];
    snprintf(buf, sizeof(buf), T(S_HELP_OF), a->helpTopic + 1, HELP_TOPICS);
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, buf);
    ui_text(14, 25, 15, FONT_BOLD, COL_TEXT, T(HELP_TITLE[a->helpTopic]));
    if (a->helpTopic == 1) { /* the LED: coloured lamps next to the lines */
        static const u32 lamp[5] = {COL_LED_BLUE, COL_ACCENT, COL_LED_BLUE_FAINT, COL_LED_GREEN, COL_ACCENT};
        const char *p = T(S_H2_BODY);
        float y = 50;
        for (int i = 0; i < 6 && *p; i++) {
            const char *e = strchr(p, '\n');
            char line[200];
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n >= sizeof(line))
                n = sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = 0;
            if (i < 5)
                ui_circle(22, y + 7, 5, lamp[i]);
            else
                ui_circle(22, y + 7, 5, COL_SURFACE2);
            y += ui_text_wrap(36, y, 350, 10, FONT_REG, COL_SOFT, line) + 6;
            p = e ? e + 1 : p + n;
        }
        ui_key_hint(14, 218, "Y", T(S_GUIDE_OPEN));
        help_channel();
        return;
    }
    /* the "how to record" topic names the configured hotkey */
    char hk[48], body[2048];
    /* (the replay topic: the hotkey that saves a clip) */
    hk_label(hk_mask(HELP_BODY[a->helpTopic] == S_H8_BODY ? a->cfg.replayHotkey : a->cfg.hotkey), hk, sizeof(hk));
    snprintf(body, sizeof(body), T(HELP_BODY[a->helpTopic]), hk, hk);
    /* a long topic (a long language) gets a smaller size rather than running into the footer */
    float px = 10;
    while (px > 8.5f && ui_text_wrap_height(372, px, FONT_REG, body) > 212 - 54)
        px -= 0.5f;
    ui_text_wrap(14, 54, 372, px, FONT_REG, COL_SOFT, body);
    ui_key_hint(14, 218, "Y", T(S_GUIDE_OPEN));
    help_channel();
}

/* "Мова · Language": the way back for someone who picked a language they cannot read */
static const char *language_label(void)
{
    static char buf[64];
    if (i18n_lang() == LANG_EN)
        return T(S_LANGUAGE);
    snprintf(buf, sizeof(buf), "%s · Language", T(S_LANGUAGE));
    return buf;
}

static void draw_lang_top(App *a)
{
    (void)a;
    ui_circle(200, 62, 22, COL_SURFACE);
    ui_circle(200, 62, 7, COL_ACCENT);
    ui_text_center(200, 98, 9, FONT_REG, COL_MUTED, "3DS Recorder");
    ui_text_center(200, 136, 19, FONT_BOLD, COL_TEXT, T(S_LANG_CHOOSE)); /* in the language in focus */
}

static void draw_lang_bottom(App *a)
{
    const bool first = a->langFirst;
    if (!first) {
        ui_rrect(SET_BACK.x, SET_BACK.y, SET_BACK.w, SET_BACK.h, 6, COL_SURFACE);
        ui_icon(ICON_BACK, SET_BACK.x + 10.5f, SET_BACK.y + 10.5f, 12, COL_TEXT);
        ui_text(38, 8, 11, FONT_BOLD, COL_TEXT, language_label());
    }
    for (int i = g_langFirst * LANG_COLS; i < LANG_COUNT && i < (g_langFirst + LANG_ROWS_SHOWN) * LANG_COLS; i++) {
        const Rect r = lang_cell(i, g_langFirst, first);
        const bool sel = a->cfg.lang == (Lang)i;
        ui_card(r.x, r.y, r.w, r.h, 8, sel ? COL_SURFACE2 : COL_SURFACE);
        ui_flag_sized(r.x + 9, r.y + (r.h - 24 * 13 / 21.0f) / 2, 24, i);
        ui_text_fit(r.x + 41, r.y + 9, r.w - 41 - (sel ? 24 : 8), 10.5f, FONT_BOLD, COL_TEXT, LANGS[i].name);
        if (sel)
            ui_icon(ICON_CHECK, r.x + r.w - 14, r.y + r.h / 2, 12, COL_ACCENT);
        if (a->focus == i)
            focus_ring(r, 8);
    }
    if (LANG_ROWS > LANG_ROWS_SHOWN) { /* more languages than fit: a scroll bar on the right */
        const Rect top = lang_cell(0, 0, first);
        const float h = LANG_ROWS_SHOWN * 35.0f - 4, thumb = h * LANG_ROWS_SHOWN / LANG_ROWS;
        ui_rrect(313, top.y, 3, h, 1.5f, COL_SURFACE);
        const int range = LANG_ROWS - LANG_ROWS_SHOWN > 0 ? LANG_ROWS - LANG_ROWS_SHOWN : 1;
        ui_rrect(313, top.y + (h - thumb) * g_langFirst / range, 3, thumb, 1.5f, COL_SOFT);
    }
    if (first)
        button(LANG_GO, T(S_LANG_CONTINUE), 1, false);
}

static const Str GUIDE_TITLE[GUIDE_PAGES] = {S_G1_TITLE, S_G2_TITLE, S_G3_TITLE, S_G4_TITLE};
static const Str GUIDE_BODY[GUIDE_PAGES] = {S_G1_BODY, S_G2_BODY, S_G3_BODY, S_G4_BODY};

static void draw_guide_top(App *a)
{
    char buf[64], hk[48], body[1024];
    const int pg = a->guidePage;
    snprintf(buf, sizeof(buf), T(S_GUIDE_OF), pg + 1, GUIDE_PAGES);
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, buf);
    ui_text(14, 25, 17, FONT_BOLD, COL_TEXT, T(GUIDE_TITLE[pg]));
    const u32 m = hk_mask(a->cfg.hotkey);
    hk_label(m, hk, sizeof(hk));
    snprintf(body, sizeof(body), T(GUIDE_BODY[pg]), hk);
    ui_text_wrap(14, 58, 372, 11, FONT_REG, COL_SOFT, body);
    /* the picture of the page: what to press, what the LED does */
    ui_card(14, 160, 372, 66, 10, COL_SURFACE);
    if (pg == 0 || pg == 1) {
        const float w = hk_chips_width(m);
        hk_chips(110 - w / 2, 184, m, COL_TEXT);
        ui_icon(ICON_NEXT, 200, 193, 14, COL_FAINT);
        ui_circle(290, 193, 12, pg == 0 ? COL_ACCENT : COL_LED_GREEN);
        ui_text(308, 186, 10, FONT_BOLD, COL_SOFT, pg == 0 ? "REC" : "×2");
    } else if (pg == 2) {
        float x = 40;
        x += ui_key_hint(x, 186, "A", T(S_PLAY)) + 24;
        ui_key_hint(x, 186, "Y", T(S_PH_HINT));
    } else {
        ui_circle(110, 193, 12, COL_LED_BLUE);
        ui_icon(ICON_NEXT, 200, 193, 14, COL_FAINT);
        ui_icon(ICON_CHECK, 290, 193, 20, COL_OK);
    }
}

static void draw_guide_bottom(App *a)
{
    const int pg = a->guidePage;
    ui_card(12, 12, 296, 170, 10, COL_SURFACE);
    ui_circle(160, 70, 28, COL_SURFACE2);
    const char n[2] = {(char)('1' + pg), 0};
    ui_text_center(160, 56, 22, FONT_BOLD, COL_TEXT, n);
    ui_text_center(160, 112, 12, FONT_BOLD, COL_TEXT, T(GUIDE_TITLE[pg]));
    for (int i = 0; i < GUIDE_PAGES; i++) /* page dots */
        ui_circle(160 + (i - (GUIDE_PAGES - 1) / 2.0f) * 14, 150, i == pg ? 4 : 3, i == pg ? COL_ACCENT : COL_SURFACE2);
    button(GD_SKIP, T(S_GUIDE_SKIP), 0, false);
    button(GD_NEXT, T(pg + 1 < GUIDE_PAGES ? S_GUIDE_NEXT : S_GUIDE_DONE), 1, true);
}


static void draw_settings_top(App *a)
{
    char v[48], note[640], s1[32], s2[32], s3[32];
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, T(S_FRAME_IN_FILE));
    const bool both = a->cfg.screens == SCREENS_BOTH;
    ui_frame(14, 28, 104, both ? 122 : 64, 6, 1, COL_FAINT);
    if (a->cfg.screens != SCREENS_BOTTOM) {
        ui_rrect(19, 33, 94, 54, 4, COL_INFO_BG);
        ui_text_center(66, 53, 8, FONT_BOLD, COL_SOFT, T(S_TOP_BOX));
    }
    if (a->cfg.screens != SCREENS_TOP) {
        const float y = both ? 91 : 33;
        ui_rrect(28, y, 76, 54, 4, COL_INFO_BG);
        ui_text_center(66, y + 20, 8, FONT_BOLD, COL_SOFT, T(S_BOTTOM_BOX));
    }
    snprintf(v, sizeof(v), "%d fps · %s", a->cfg.fps, T(a->cfg.audio ? S_SOUND_ON : S_SOUND_OFF));
    ui_text_center(66, both ? 156 : 98, 9, FONT_BOLD, COL_TEXT, v);
    ui_text(132, 13, 15, FONT_BOLD, COL_TEXT, T(S_FILE_TITLE));
    const u32 perMin = mb_per_min(a);
    const float w = (254 - 6) / 2.0f;
    snprintf(s1, sizeof(s1), "≈ %lu%c%lu %s", (unsigned long)(perMin / 60), i18n_decimal(), (unsigned long)(perMin % 60 * 10 / 60), T(S_MBS));
    snprintf(s2, sizeof(s2), "≈ %lu %s", (unsigned long)perMin, T(S_MB));
    snprintf(s3, sizeof(s3), T(S_MINUTES), (unsigned)(perMin ? a->sdFreeMb / perMin : 0));
    const int row = set_focus_row(a);
    if (row == ROW_AUTOSTART)   /* what the row does, while it is in focus */
        snprintf(note, sizeof(note), "%s", T(S_AUTOSTART_NOTE));
    else if (row == ROW_LID)
        snprintf(note, sizeof(note), "%s", T(S_LID_WORK_NOTE));
    else if (row == ROW_STATS)
        snprintf(note, sizeof(note), T(S_STATS_NOTE), stats_id());
    else if (row == ROW_CONVERT)
        snprintf(note, sizeof(note), "%s", T(S_CONVERT_NOTE));
    else if (row == ROW_BOTTOM_FPS)
        snprintf(note, sizeof(note), "%s", T(S_BOTTOM_FPS_NOTE));
    else if (row == ROW_MAIN_SCREEN)
        snprintf(note, sizeof(note), "%s", T(S_MAIN_SCREEN_NOTE));
    else if (row == ROW_REPLAY_LEN)
        snprintf(note, sizeof(note), "%s", T(S_REPLAY_LEN_NOTE));
    else if (row == ROW_INPUTS)
        snprintf(note, sizeof(note), "%s", T(S_INPUTS_NOTE));
    else if (row == ROW_MP4_INPUTS)
        snprintf(note, sizeof(note), "%s", T(S_MP4_INPUTS_NOTE));
    else if (row == ROW_SDTEST)
        snprintf(note, sizeof(note), "%s", T(S_SDTEST_NOTE));
    else
        snprintf(note, sizeof(note), T(S_SETTINGS_NOTE), (unsigned)(perMin ? 4096 / perMin : 0));
    /* right under the stat cards (the toast takes the bottom of the screen); a note too long for that
       goes in their place: they are about the recording, the note about the row in focus */
    const float h = ui_text_wrap_height(234, 9, FONT_REG, note) + 14;
    const bool tall = h > SCREEN_H - 8 - 154;
    if (!tall) {
        stat_card(132, 40, w, T(S_CODEC), T(a->cfg.lossless ? S_CODEC_VAL : S_CODEC_AUTO), NULL);
        stat_card(138 + w, 40, w, T(S_STREAM), s1, NULL);
        stat_card(132, 96, w, T(S_PER_MIN), s2, NULL);
        stat_card(138 + w, 96, w, T(S_FITS), s3, NULL);
    }
    const float y = tall ? 40 : 154, max_h = SCREEN_H - 8 - y;
    const int lines = (int)((max_h - 14) / (9 * 1.3f));
    ui_card(132, y, 254, h < max_h ? h : max_h, 8, COL_SURFACE);
    ui_text_wrap_lines(142, y + 7, 234, 9, FONT_REG, COL_SOFT, note, lines);
}

static void draw_hotkey_top(App *a)
{
    const u32 m = a->hkReview ? a->hkCand : a->hkHeld;
    ui_text_center(200, 40, 15, FONT_BOLD, COL_TEXT, T(a->hkReview ? S_HK_REVIEW_TITLE : S_HK_HOLD_TITLE));
    const char *labels[3] = {NULL, NULL, NULL};
    int n = 0;
    for (int i = 0; i < HK_COUNT && n < 3; i++)
        if (m & HK_KEYS[i].key)
            labels[n++] = HK_KEYS[i].label;
    float wbox[3], total = 0;
    for (int i = 0; i < 3; i++) {
        const float tw = labels[i] ? ui_text_width(17, FONT_BOLD, labels[i]) + 20 : 0;
        wbox[i] = tw > 44 ? tw : 44;
        total += wbox[i] + (i ? 20 : 0);
    }
    float x = 200 - total / 2;
    for (int i = 0; i < 3; i++) {
        if (i) {
            ui_text_center(x + 10, 92, 17, FONT_BOLD, COL_MUTED, "+");
            x += 20;
        }
        if (labels[i]) {
            ui_rrect(x, 80, wbox[i], 44, 9, COL_SURFACE2);
            ui_frame(x, 80, wbox[i], 44, 9, 1.5f, COL_ACCENT);
            ui_text_center(x + wbox[i] / 2, 90, 17, FONT_BOLD, COL_TEXT, labels[i]);
        } else {
            ui_frame(x, 80, wbox[i], 44, 9, 1.2f, COL_FAINT);
            ui_text_center(x + wbox[i] / 2, 90, 17, FONT_BOLD, COL_FAINT, "?");
        }
        x += wbox[i];
    }
    if (!a->hkReview && hk_count(a->hkHeld) > 3)
        ui_text_center(200, 134, 9, FONT_BOLD, COL_WARN, T(S_HK_TOO_MANY));
    float frac = a->hkReview ? 1 : 0;
    if (!a->hkReview && hk_count(a->hkHeld) >= 2 && hk_count(a->hkHeld) <= 3)
        frac = (a->frame - a->hkSince) / 60.0f;
    if (frac > 1)
        frac = 1;
    ui_rrect(95, 152, 210, 5, 2.5f, COL_SURFACE2);
    if (frac > 0)
        ui_rrect(95, 152, 210 * frac, 5, 2.5f, COL_ACCENT);
    if (!a->hkReview)
        ui_text_center(200, 164, 9, FONT_REG, COL_MUTED, T(S_HK_HOLD_HINT));
}

static void draw_uninstall_top(App *a)
{
    char cnt[64], n[32], gb[24];
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, T(S_UN_EYEBROW));
    ui_text(14, 25, 17, FONT_BOLD, COL_TEXT, T(S_UN_TITLE));
    check_row(60, false, true, T(S_UN_MODULE), T(S_UN_MODULE_SUB));
    check_row(91, false, true, T(S_UN_SETTINGS), "config.ini, recorder.log");
    check_row(122, false, true, T(S_UN_APP), envIsHomebrew() ? "3ds-recorder.3dsx" : T(S_UN_APP_CIA));
    format_gb(gb, sizeof(gb), (u32)(a->files.bytes >> 20));
    snprintf(cnt, sizeof(cnt), "%s · %s", i18n_recordings(a->files.count, n, sizeof(n)), gb);
    check_row(153, !a->deleteRecordings, a->deleteRecordings, T(S_UN_RECORDINGS),
              a->deleteRecordings ? cnt : T(S_UN_RECORDINGS_KEEP));
}

static void draw_sdtest_top(App *a)
{
    (void)a;
    char buf[96], num[16];
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, T(S_GRP_APP));
    ui_text(14, 25, 17, FONT_BOLD, COL_TEXT, T(S_SDTEST));
    const SdTestResult *r = &g_sdt;
    if (r->state == SDT_IDLE) {
        ui_text_wrap(14, 58, 372, 10, FONT_REG, COL_SOFT, T(S_SDT_INTRO));
        return;
    }
    if (r->state == SDT_RUNNING) {
        snprintf(buf, sizeof(buf), T(S_SDT_RUNNING), r->permille / 10);
        ui_text(14, 70, 11, FONT_BOLD, COL_TEXT, buf);
        ui_rrect(14, 92, 372, 8, 4, COL_SURFACE2);
        ui_rrect(14, 92, 372 * r->permille / 1000.0f, 8, 4, COL_ACCENT);
        ui_text_wrap(14, 112, 372, 9, FONT_REG, COL_MUTED, T(S_SDT_INTRO));
        return;
    }
    if (r->state == SDT_FAILED) {
        snprintf(buf, sizeof(buf), T(S_SDT_FAIL), (unsigned long)r->rc);
        ui_text_wrap(14, 70, 372, 10, FONT_REG, COL_ERR, buf);
        return;
    }
    snprintf(num, sizeof(num), "%lu%c%lu", (unsigned long)(r->kbPerSec / 1024), i18n_decimal(),
             (unsigned long)(r->kbPerSec % 1024 * 10 / 1024));
    snprintf(buf, sizeof(buf), T(S_SDT_SPEED), num);
    ui_text(14, 62, 28, FONT_BOLD, COL_TEXT, buf);
    const int grade = sdtest_grade(r->kbPerSec);
    const u32 col = grade == 2 ? COL_OK : grade == 1 ? COL_WARN : COL_ERR;
    const u32 bg = grade == 2 ? COL_OK_BG : grade == 1 ? COL_WARN_BG : COL_ERR_BG;
    const char *verdict = T(grade == 2 ? S_SDT_GOOD : grade == 1 ? S_SDT_OK : S_SDT_SLOW);
    const float th = ui_text_wrap_height(330, 10, FONT_REG, verdict), ch = th + 20;
    ui_card(14, 104, 372, ch, 8, bg);
    ui_icon(grade == 2 ? ICON_CHECK : ICON_WARN, 30, 104 + ch / 2, 14, col);
    ui_text_wrap(46, 114, 330, 10, FONT_REG, col, verdict);
    snprintf(buf, sizeof(buf), T(S_SDT_SLOWEST), (unsigned)r->slowestMs);
    ui_text(14, 104 + ch + 12, 9, FONT_REG, COL_MUTED, buf);
}

static void draw_uninstalled_top(App *a)
{
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, "3DS Recorder");
    ui_text(14, 25, 17, FONT_BOLD, a->uninstallOk ? COL_TEXT : COL_WARN, T(a->uninstallOk ? S_UN_DONE : S_UN_PARTIAL));
    ui_text_wrap(14, 58, 372, 10, FONT_REG, COL_SOFT, T(a->uninstallOk ? S_UN_DONE_BODY : S_UN_PARTIAL_BODY));
}

/* yyyymmdd_hhmmss -> "03.11 · 17:26" / "Nov 3 · 17:26" (the gallery list) */
static void format_short(char *buf, size_t cap, const char *name)
{
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    char date[24];
    if (sscanf(name, "%4d%2d%2d_%2d%2d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12)
        snprintf(buf, cap, "%s", name);
    else
        snprintf(buf, cap, "%s · %02d:%02d", i18n_date(y, mo, d, false, date, sizeof(date)), h, mi);
}

/* 125000 ms -> "02:05", over an hour "1:02:05"; "≈ " when estimated */
static void format_duration(char *buf, size_t cap, const GalItem *it)
{
    const u32 t = it->durationMs / 1000;
    const char *about = it->ended ? "" : "≈ ";
    if (t >= 3600)
        snprintf(buf, cap, "%s%lu:%02lu:%02lu", about, (unsigned long)(t / 3600), (unsigned long)(t / 60 % 60),
                 (unsigned long)(t % 60));
    else
        snprintf(buf, cap, "%s%02lu:%02lu", about, (unsigned long)(t / 60), (unsigned long)(t % 60));
}

/* Copies a decoded preview into its texture: only after C3D_FrameBegin, when the GPU is done
   with the last frame's use of it. */
static void upload(C3D_Tex *tex, const u16 *px, size_t bytes)
{
    memcpy(tex->data, px, bytes);
    C3D_TexFlush(tex);
}

static void frame_box(float x, float y, float w, float h, C3D_Tex *tex, float srcW, bool ready, bool failed)
{
    if (ready) {
        ui_tex(tex, srcW, 240, x, y, w / srcW);
        ui_frame(x, y, w, h, 2, 1, COL_FAINT);
        return;
    }
    ui_rrect(x, y, w, h, 4, COL_SURFACE2);
    if (!failed)
        ui_spinner(x + w / 2, y + h / 2, 8, COL_FAINT);
}

static void draw_gallery_top(App *a)
{
    char buf[96], v[48];
    gal_lock();
    const GalList *l = gal_list();
    const GalPreview *p = gal_preview();
    if (g_galTexOk && p->version != a->galPreviewVersion) {
        a->galPreviewVersion = p->version;
        upload(&g_galTex[0], p->top, GAL_TEX_W * GAL_TEX_H * 2);
        upload(&g_galTex[1], p->bottom, GAL_TEX_W * GAL_TEX_H * 2);
    }
    if (!l->loaded || !l->count) {
        ui_text(14, 13, 15, FONT_BOLD, COL_TEXT, T(S_TILE_GALLERY));
        if (!l->loaded)
            ui_text(14, 40, 10, FONT_REG, COL_MUTED, T(S_GAL_LOADING));
        else {
            char hk[48], hint[96];
            hk_label(hk_mask(a->cfg.hotkey), hk, sizeof(hk));
            snprintf(hint, sizeof(hint), T(S_NO_RECORDINGS_HINT), hk);
            ui_text(14, 40, 11, FONT_BOLD, COL_TEXT, T(S_NO_RECORDINGS));
            ui_text(14, 58, 9, FONT_REG, COL_MUTED, hint);
        }
        gal_unlock();
        return;
    }
    const GalItem *it = &l->items[a->galSel];
    const bool mine = strcmp(p->name, it->name) == 0 && g_galTexOk;
    const bool failed = mine && !p->ok;
    if (it->info && it->bottomOnly)   /* the bottom screen in the top one's place */
        frame_box(27, 14, 120, 90, &g_galTex[1], 320, mine && p->hasBottom, failed || (mine && !p->hasBottom));
    else {
        frame_box(12, 14, 150, 90, &g_galTex[0], 400, mine && p->ok, failed);
        if (!it->info || it->both)
            frame_box(27, 112, 120, 90, &g_galTex[1], 320, mine && p->hasBottom, failed || (mine && !p->hasBottom));
    }
    if (failed)
        ui_text_center(87, 54, 9, FONT_REG, COL_MUTED, T(S_UNREADABLE));

    const float x = 176, w = (388 - x - 6) / 2;
    snprintf(buf, sizeof(buf), T(S_GAL_NTH), a->galSel + 1, l->count);
    ui_text(x, 13, 9, FONT_REG, COL_MUTED, buf);
    format_name(buf, sizeof(buf), it->name);
    ui_text(x, 26, 13, FONT_BOLD, COL_TEXT, buf);
    ui_text(x, 45, 8, FONT_REG, COL_FAINT, it->name);
    format_gb(v, sizeof(v), (u32)(it->bytes >> 20));
    if (it->info && it->readable) {
        char dur[24], prof[32], drop[16];
        format_duration(dur, sizeof(dur), it);
        snprintf(prof, sizeof(prof), "%s · %u", T(it->clip ? S_GAL_CLIP : S_PROFILE_STD), (unsigned)it->fps);
        if (it->ended)
            snprintf(drop, sizeof(drop), "%lu", (unsigned long)it->dropped);
        else
            snprintf(drop, sizeof(drop), "—");
        stat_card(x, 62, w, T(S_DURATION), dur, NULL);
        stat_card(x + w + 6, 62, w, T(S_SIZE), v, NULL);
        stat_card(x, 118, w, T(S_PROFILE), prof, NULL);
        stat_card(x + w + 6, 118, w, T(S_DROPPED), drop, NULL);
        snprintf(buf, sizeof(buf), "%s · %s%s%s", T(it->audio ? S_WITH_SOUND : S_NO_SOUND),
                 T(it->both ? S_BOTH_SCREENS : it->bottomOnly ? S_BOTTOM_SCREEN : S_TOP_SCREEN), it->inputs ? " · " : "",
                 it->inputs ? T(S_WITH_BUTTONS) : "");
        ui_text(x, 176, 9, FONT_REG, COL_SOFT, buf);
        if (!it->ended) { /* cut off (power, a crash): hard to miss */
            ui_card(x, 188, 212, 25, 6, COL_WARN_BG);
            ui_icon(ICON_WARN, x + 12, 200.5f, 12, COL_WARN);
            ui_text_wrap(x + 24, 191, 184, 8.5f, FONT_BOLD, COL_WARN, T(S_NOT_CLOSED));
        }
    } else if (it->info) {
        stat_card(x, 62, w, T(S_SIZE), v, NULL);
        ui_text_wrap(x, 120, 212, 9, FONT_REG, COL_WARN, T(S_UNREADABLE));
    } else {
        stat_card(x, 62, w, T(S_SIZE), v, NULL);
        ui_spinner(x + w + 6 + w / 2, 87, 8, COL_FAINT);
    }
    float hx = x;
    hx += ui_key_hint(hx, 216, "A", T(S_PLAY)) + 10;
    hx += ui_key_hint(hx, 216, "Y", T(S_PH_HINT)) + 10;
    ui_key_hint(hx, 216, "X", T(S_DELETE));
    gal_unlock();
}

static void format_clock(char *buf, size_t cap, u32 ms)
{
    const u32 t = ms / 1000;
    if (t >= 3600)
        snprintf(buf, cap, "%lu:%02lu:%02lu", (unsigned long)(t / 3600), (unsigned long)(t / 60 % 60), (unsigned long)(t % 60));
    else
        snprintf(buf, cap, "%02lu:%02lu", (unsigned long)(t / 60), (unsigned long)(t % 60));
}

static bool player_controls(const App *a)
{
    return player_paused() || player_ended() || a->frame < a->playerShowUntil;
}

static void draw_player_top(App *a)
{
    const int shown = g_plTexOk ? player_present(&g_plTex[0], &g_plTex[1]) : 0;
    ui_rect(0, 0, TOP_W, SCREEN_H, RGBA(0, 0, 0, 0xFF));
    if (shown & 1)
        ui_tex_screen(&g_plTex[0], 400, 200, 120);
    else if (player_bottom_only())
        ui_text_center(200, 110, 10, FONT_REG, COL_FAINT, T(S_PL_BOTTOM_ONLY));
    else
        ui_spinner(200, 120, 14, COL_FAINT);
    InputState in;
    if (g_plInputs && (shown & 1) && player_input(&in))
        draw_inputs_top(&in);
    if (player_controls(a) && player_file_audio() && !player_has_audio())
        ui_text(12, 14, 9, FONT_BOLD, COL_WARN, T(S_PL_NO_DSP));
    if (player_controls(a)) {
        char now[16], total[16], buf[40];
        format_clock(now, sizeof(now), player_time_ms());
        format_clock(total, sizeof(total), player_duration_ms());
        snprintf(buf, sizeof(buf), "%s / %s", now, total);
        const float w = ui_text_width(10, FONT_BOLD, buf) + 20;
        ui_rrect(386 - w, 10, w, 22, 11, RGBA(0x0A, 0x0C, 0x10, 0xC8));
        ui_text(386 - w + 10, 14, 10, FONT_BOLD, COL_TEXT, buf);
    }
}

static void draw_phone_top(App *a)
{
    (void)a;
    ServerStats ss;
    server_stats(&ss);
    if (ss.state != SRV_RUNNING) {
        const bool wifi = ss.state == SRV_NO_WIFI;
        ui_icon(ICON_WARN, 30, 34, 22, COL_WARN);
        ui_text(48, 24, 15, FONT_BOLD, COL_TEXT, T(wifi ? S_PH_NO_WIFI : S_PH_FAILED));
        ui_text_wrap(16, 56, 368, 10, FONT_REG, COL_SOFT, T(wifi ? S_PH_NO_WIFI_BODY : S_PH_FAILED_BODY));
        ui_text_wrap(16, 140, 368, 9, FONT_REG, COL_MUTED, T(S_PH_SD));
        ui_key_hint(16, 216, "A", T(S_PH_RETRY));
        return;
    }
    /* the code: white card, dark modules, quiet zone of 2 modules */
    const float box = 172, bx = 14, by = 34;
    ui_rrect(bx, by, box, box, 10, COL_WHITE);
    if (g_qrOk) {
        const int n = qrcodegen_getSize(g_qr);
        const float m = (float)(int)((box - 16) / n), q = bx + (box - m * n) / 2, qy = by + (box - m * n) / 2;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (qrcodegen_getModule(g_qr, x, y))
                    ui_rect(q + x * m, qy + y * m, m, m, COL_BG);
    }
    const float x = 200, w = 188;
    ui_text(x, 34, 9, FONT_REG, COL_MUTED, T(S_PH_EYEBROW));
    const float h = ui_text_wrap_height(w, 13, FONT_BOLD, T(S_PH_TITLE));
    ui_text_wrap(x, 48, w, 13, FONT_BOLD, COL_TEXT, T(S_PH_TITLE));
    ui_text_wrap(x, 48 + h + 8, w, 9, FONT_REG, COL_SOFT, T(S_PH_BODY));
}

static void draw_toast(App *a)
{
    if (!a->msg[0] || a->frame > a->msgUntil || (a->msgImportant && ((a->screen == SCR_LANG && a->langFirst) || a->screen == SCR_GUIDE)))
        return;
    const float h = ui_text_wrap_height(350, 9, FONT_REG, a->msg) + 12;
    ui_rrect(14, 226 - h, 372, h, 7, COL_INFO_BG);
    ui_icon(ICON_INFO, 28, 226 - h + 12, 12, COL_INFO);
    ui_text_wrap(40, 226 - h + 6, 340, 9, FONT_REG, COL_TEXT, a->msg);
}

void app_draw_top(App *a)
{
    ui_base_font(a->screen == SCR_LANG); /* every language's name and preview there, no font to load */
    switch (a->screen) {
    case SCR_PANEL: draw_panel_top(a); break;
    case SCR_FIRSTRUN: draw_firstrun_top(a); break;
    case SCR_HELP: draw_help_top(a); break;
    case SCR_GUIDE: draw_guide_top(a); break;
    case SCR_LANG: draw_lang_top(a); break;
    case SCR_SETTINGS:
    case SCR_SETGROUP: draw_settings_top(a); break;
    case SCR_HOTKEY: draw_hotkey_top(a); break;
    case SCR_GALLERY: draw_gallery_top(a); break;
    case SCR_PLAYER: draw_player_top(a); break;
    case SCR_PHONE: draw_phone_top(a); break;
    case SCR_UNINSTALL: draw_uninstall_top(a); break;
    case SCR_SDTEST: draw_sdtest_top(a); break;
    case SCR_UNINSTALLED: draw_uninstalled_top(a); break;
    }
    draw_toast(a);
}

/* ------------------------------------------------------------------ bottom screen */

static void draw_panel_bottom(App *a)
{
    char buf[48];
    const bool recording = a->st.running && a->st.state == REC_STATE_RECORDING;
    /* tile 0: recorder on / off, or stop recording */
    Rect t = TILE[0];
    if (recording) {
        ui_card(t.x, t.y, t.w, t.h, 10, COL_ERR_BG);
        ui_text_wrap(t.x + 10, t.y + 10, 110, 11, FONT_BOLD, COL_ERR, T(S_TILE_STOP));
        ui_circle(t.x + t.w - 16, t.y + 16, 4, COL_ACCENT);
        ui_text_wrap_lines(t.x + 10, t.y + t.h - 32, t.w - 20, 9, FONT_REG, COL_MUTED, T(S_TILE_STOP_SUB), 2);
    } else {
        /* two switches in one card: the recorder, and under a hairline its replay buffer */
        ui_card(t.x, t.y, t.w, t.h, 10, COL_SURFACE);
        ui_text(t.x + 10, t.y + 9, 11, FONT_BOLD, COL_TEXT, T(S_TILE_RECORDER));
        ui_toggle(t.x + t.w - 42, t.y + 8, a->st.running);
        ui_text(t.x + 10, t.y + 27, 9, FONT_REG, COL_MUTED,
                T(a->st.running ? S_TILE_RECORDER_ON_SHORT : S_TILE_RECORDER_OFF_SHORT));
        ui_rect(t.x + 10, t.y + 46.5f, t.w - 20, 1, COL_SURFACE2);
        const Rect r = TILE_REPLAY;
        char hk[48];
        hk_label(hk_mask(a->cfg.replayHotkey), hk, sizeof(hk));
        char len[16], title[48], off[64];
        replay_len_label(a->cfg.replaySec, len, sizeof(len));
        snprintf(title, sizeof(title), T(S_REPLAY), len);
        snprintf(off, sizeof(off), T(S_TILE_REPLAY_OFF), len);
        snprintf(buf, sizeof(buf), T(S_TILE_REPLAY_ON), hk);
        ui_text(r.x + 10, r.y + 9, 11, FONT_BOLD, COL_TEXT, title);
        ui_toggle(r.x + r.w - 42, r.y + 8, a->cfg.replay);
        ui_text(r.x + 10, r.y + 27, 9, FONT_REG, COL_MUTED, a->cfg.replay ? buf : off);
    }
    static const Icon icon[4] = {ICON_CHECK, ICON_GRID, ICON_INFO, ICON_SLIDERS};
    const Str title[4] = {S_TILE_RECORDER, S_TILE_GALLERY, S_TILE_HELP, S_TILE_SETTINGS};
    for (int i = 1; i < 4; i++) {
        t = TILE[i];
        ui_card(t.x, t.y, t.w, t.h, 10, COL_SURFACE);
        if (i == 2) { /* help: "?" in a circle, the same size as the icons */
            ui_circle(t.x + 20, t.y + 21, 10, COL_SURFACE2);
            ui_text_center(t.x + 20, t.y + 13.5f, 11, FONT_BOLD, COL_TEXT, "?");
        } else
            ui_icon(icon[i], t.x + 20, t.y + 21, 20, COL_TEXT);
        ui_text(t.x + 10, t.y + t.h - 34, 11, FONT_BOLD, COL_TEXT, T(title[i]));
        const char *sub = i == 1   ? (a->files.count ? i18n_recordings(a->files.count, buf, sizeof(buf)) : T(S_NO_RECORDINGS))
                          : i == 2 ? T(S_TILE_HELP_SUB)
                                   : T(S_TILE_SETTINGS_SUB);
        ui_text(t.x + 10, t.y + t.h - 18, 9, FONT_REG, COL_MUTED, sub);
    }
    if (!recording && (a->focus == 0 || a->focus == FOCUS_REPLAY))
        focus_ring(a->focus == 0 ? TILE_REC : TILE_REPLAY, 10);
    else if (a->focus < 4 || a->focus == FOCUS_REPLAY)
        focus_ring(TILE[a->focus < 4 ? a->focus : 0], 10);
    /* hint row: the hotkey */
    const float x = 12 + ui_text(12, 217, 9, FONT_REG, COL_MUTED, T(S_START_STOP)) + 5;
    hk_chips(x, 216, hk_mask(a->cfg.hotkey), COL_MUTED);
}

static void draw_firstrun_bottom(App *a)
{
    if (!a->lumaModules) {
        ui_text(12, 12, 11, FONT_BOLD, COL_TEXT, T(S_FR_LUMA_TITLE));
        ui_text_wrap(12, 32, 296, 10, FONT_REG, COL_SOFT, T(S_FR_LUMA_BODY));
        button(FR_LATER, T(S_LATER), 0, a->focus == 0);
        button(FR_START, T(S_START), 3, false);
        return;
    }
    ui_text(12, 12, 11, FONT_BOLD, COL_TEXT, T(S_FR_START_TITLE));
    const float h = ui_text_wrap(12, 32, 296, 10, FONT_REG, COL_SOFT, T(S_FR_START_BODY));
    ui_circle(20, 32 + h + 14, 5, COL_LED_BLUE);
    ui_text(32, 32 + h + 7, 9, FONT_REG, COL_MUTED, T(S_FR_LED));
    button(FR_LATER, T(S_LATER), 0, a->focus == 0);
    button(FR_START, T(S_START), 1, a->focus == 1);
}

static void draw_help_bottom(App *a)
{
    navbar(T(S_HELP_TITLE));
    for (int i = 0; i < HELP_TOPICS; i++) {
        Rect r = help_row(i);
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        if (i == a->helpTopic)
            ui_rrect(r.x, r.y, r.w, r.h, 7, COL_SURFACE);
        ui_rrect(r.x + 7, r.y + 1.5f, 17, 17, 4, COL_SURFACE2);
        ui_text_center(r.x + 15.5f, r.y + 3.5f, 8, FONT_BOLD, COL_SOFT, n);
        ui_text(r.x + 32, r.y + 2.5f, 10, FONT_BOLD, COL_TEXT, T(HELP_TITLE[i]));
        if (i == a->helpTopic)
            focus_ring(r, 7);
    }
}

static void settings_header(const char *title)
{
    ui_rrect(SET_BACK.x, SET_BACK.y, SET_BACK.w, SET_BACK.h, 6, COL_SURFACE);
    ui_icon(ICON_BACK, SET_BACK.x + 10.5f, SET_BACK.y + 10.5f, 12, COL_TEXT);
    ui_text(38, 8, 11, FONT_BOLD, COL_TEXT, title);
}

static const Str GROUP_NAME[GRP_COUNT] = {S_GRP_RECORDING, S_GRP_RECORDER, S_GRP_APP, S_LANGUAGE};

static const char *group_name(int group)
{
    return group == GRP_LANG ? language_label() : T(GROUP_NAME[group]);
}

/* what a group holds now, one line under its name */
static void group_summary(const App *a, int grp, char *buf, size_t cap)
{
    static const Str SCREENS[3] = {S_BOTH, S_TOP, S_BOTTOM};
    static const Str LED[3] = {S_LED_OFF, S_LED_SOLID, S_LED_PULSE};
    char hk[48];
    switch (grp) {
    case GRP_RECORDING:
        snprintf(buf, cap, "%d fps · %s · %s · %s", a->cfg.fps, T(SCREENS[a->cfg.screens]),
                 T(a->cfg.lossless ? S_Q_LOSSLESS : S_Q_AUTO), T(a->cfg.audio ? S_SOUND_ON : S_SOUND_OFF));
        break;
    case GRP_RECORDER: {
        char len[16], rp[48];
        hk_label(hk_mask(a->cfg.hotkey), hk, sizeof(hk));
        snprintf(rp, sizeof(rp), T(S_REPLAY), replay_len_label(a->cfg.replaySec, len, sizeof(len)));
        snprintf(buf, cap, "%s · %s · %s %s%s%s", hk, rp, T(S_LED), T(LED[a->cfg.led]), a->cfg.autostart ? " · " : "",
                 a->cfg.autostart ? T(S_AUTOSTART) : "");
        break;
    }
    case GRP_APP:
        snprintf(buf, cap, "%s: %s", T(S_CONVERT), T(a->cfg.convertAuto ? S_CONVERT_AUTO : S_CONVERT_MANUAL));
        break;
    default:
        snprintf(buf, cap, "%s", LANGS[a->cfg.lang].name);
    }
}

static void draw_settings_bottom(App *a)
{
    char sum[128];
    settings_header(T(S_SETTINGS));
    for (int i = 0; i < GRP_COUNT; i++) {
        const Rect c = grp_card(i);
        ui_card(c.x, c.y, c.w, c.h, 8, COL_SURFACE);
        float x = c.x + 12;
        if (i == GRP_LANG) {
            ui_flag_sized(x, c.y + (c.h - 26 * 13 / 21.0f) / 2, 26, a->cfg.lang);
            x += 38;
        }
        ui_text(x, c.y + 7, 10.5f, FONT_BOLD, COL_TEXT, group_name(i));
        group_summary(a, i, sum, sizeof(sum));
        ui_text(x, c.y + 25, 9, FONT_REG, COL_MUTED, sum);
        ui_icon(ICON_NEXT, c.x + c.w - 12, c.y + c.h / 2, 11, COL_MUTED);
        if (a->focus == i)
            focus_ring(c, 8);
    }
}

static void draw_setgroup_bottom(App *a)
{
    static const Str NAME[ROW_COUNT] = {
        [ROW_FPS] = S_FPS,           [ROW_SCREENS] = S_SCREENS, [ROW_BOTTOM_FPS] = S_BOTTOM_FPS, [ROW_QUALITY] = S_QUALITY,
        [ROW_SOUND] = S_SOUND,       [ROW_HOTKEY] = S_HOTKEY,   [ROW_LED] = S_LED,               [ROW_AUTOSTART] = S_AUTOSTART,
        [ROW_CONVERT] = S_CONVERT,   [ROW_LID] = S_LID_WORK,    [ROW_STATS] = S_STATS,           [ROW_UNINSTALL] = S_UNINSTALL_ROW,
        [ROW_MAIN_SCREEN] = S_MAIN_SCREEN, [ROW_REPLAY_LEN] = S_REPLAY_LEN, [ROW_INPUTS] = S_INPUTS,
        [ROW_MP4_INPUTS] = S_MP4_INPUTS,   [ROW_SDTEST] = S_SDTEST,
    };
    settings_header(group_name(a->setGroup));
    for (int i = 0; i < GROUP_N[a->setGroup]; i++) {
        const int r = GROUP_ROWS[a->setGroup][i];
        const Rect row = set_row(i);
        const bool off = (r == ROW_BOTTOM_FPS || r == ROW_MAIN_SCREEN) && !bottom_fps_on(a);
        if (r != ROW_UNINSTALL)
            ui_card(row.x, row.y, row.w, row.h, 6, COL_SURFACE);
        const Str name = r == ROW_BOTTOM_FPS && a->cfg.mainBottom ? S_TOP_FPS : NAME[r];   /* the second screen's rate */
        ui_text(row.x + 9, row.y + 7, 9.5f, FONT_BOLD, r == ROW_UNINSTALL ? COL_ERR : off ? COL_FAINT : COL_TEXT, T(name));
        Seg g;
        if (row_seg(a, r, &g)) {
            Rect opt[3] = {{0}};
            seg_layout(row, g.labels, g.n, opt);
            ui_rrect(opt[0].x - 2, opt[0].y - 1, opt[g.n - 1].x + opt[g.n - 1].w - opt[0].x + 4, opt[0].h + 2, 4, COL_BG);
            for (int k = 0; k < g.n; k++) {
                const bool sel = k == g.sel;
                if (sel)
                    ui_rrect(opt[k].x, opt[k].y, opt[k].w, opt[k].h, 3, off ? COL_SURFACE2 : COL_ACCENT);
                ui_text_center(opt[k].x + opt[k].w / 2, opt[k].y + 4, 9, FONT_BOLD,
                               off ? COL_FAINT : sel ? COL_WHITE : COL_SOFT, g.labels[k]);
            }
        } else if (r == ROW_BOTTOM_FPS || r == ROW_REPLAY_LEN) {
            char v[16];
            const char *label = r == ROW_BOTTOM_FPS ? bottom_fps_label(a, v, sizeof(v)) : replay_len_label(a->cfg.replaySec, v, sizeof(v));
            const u32 col = off ? COL_FAINT : COL_TEXT;
            const float rx = row.x + row.w - 22;
            ui_text_right(rx, row.y + 7, 9.5f, FONT_BOLD, col, label);
            ui_icon(ICON_BACK, rx - ui_text_width(9.5f, FONT_BOLD, label) - 10, row.y + row.h / 2, 9, off ? COL_FAINT : COL_MUTED);
            ui_icon(ICON_NEXT, row.x + row.w - 11, row.y + row.h / 2, 9, off ? COL_FAINT : COL_MUTED);
        } else if (r == ROW_SOUND || r == ROW_AUTOSTART || r == ROW_LID || r == ROW_STATS || r == ROW_INPUTS ||
                   r == ROW_MP4_INPUTS)
            ui_toggle(row.x + row.w - 38, row.y + 4,
                      r == ROW_SOUND ? a->cfg.audio : r == ROW_AUTOSTART ? a->cfg.autostart
                                                     : r == ROW_LID      ? a->cfg.lidWork
                                                     : r == ROW_INPUTS   ? a->cfg.inputs
                                                     : r == ROW_MP4_INPUTS ? a->cfg.mp4Inputs
                                                                         : a->cfg.stats);
        else if (r == ROW_HOTKEY) {
            /* the recording hotkey, then the replay one */
            const u32 m = hk_mask(a->cfg.hotkey), m2 = hk_mask(a->cfg.replayHotkey);
            const float sep = ui_text_width(9, FONT_REG, "·") + 8;
            float x = row.x + row.w - 20 - hk_chips_width(m) - sep - hk_chips_width(m2);
            x += hk_chips(x, row.y + 6, m, COL_MUTED) + 1;
            x += ui_text(x, row.y + 7, 9, FONT_REG, COL_FAINT, "·") + 4;
            hk_chips(x, row.y + 6, m2, COL_MUTED);
            ui_icon(ICON_NEXT, row.x + row.w - 9, row.y + row.h / 2, 10, COL_MUTED);
        } else
            ui_icon(ICON_NEXT, row.x + row.w - 9, row.y + row.h / 2, 10, COL_MUTED);
        if (i == a->focus)
            focus_ring(row, 6);
    }
}

static void draw_hotkey_bottom(App *a)
{
    char now[64];
    ui_text(12, 10, 11, FONT_BOLD, COL_TEXT, T(a->hkStep ? S_HK_SCREEN2 : S_HK_SCREEN));
    ui_card(12, 32, 296, 24, 7, COL_SURFACE);
    ui_text(22, 37, 9.5f, FONT_REG, COL_SOFT, T(S_HK_NOW));
    hk_label(hk_step_now(a), now, sizeof(now));
    ui_text_right(298, 37, 9.5f, FONT_BOLD, COL_TEXT, now);
    const bool review = a->hkReview;
    const Str problem = review ? hk_step_problem(a, a->hkCand) : S_COUNT;
    float y = 64;
    if (review) {
        const bool ok = problem == S_COUNT;
        const char *msg = T(ok ? S_HK_OK : problem);
        const float h = ui_text_wrap_height(250, 9.5f, FONT_REG, msg) + 14;
        ui_rrect(12, y, 296, h, 7, ok ? COL_OK_BG : COL_ERR_BG);
        ui_icon(ok ? ICON_CHECK : ICON_WARN, 27, y + 13, 13, ok ? COL_OK : COL_ERR);
        ui_text_wrap(42, y + 7, 256, 9.5f, FONT_REG, ok ? COL_TEXT : COL_ERR, msg);
        y += h + 8;
    }
    ui_text_wrap(12, y, 296, 9, FONT_REG, COL_MUTED, T(S_HK_RULES));
    ui_text(12, 182, 9, FONT_REG, COL_MUTED, T(!review ? S_HK_KEYS_CAPTURE : problem == S_COUNT ? S_HK_KEYS_REVIEW : S_HK_KEYS_RETRY));
    button(HK_BTN[0], T(S_RESET), 0, false);
    button(HK_BTN[1], T(S_HK_KEEP), 0, false);
    button(HK_BTN[2], T(S_SAVE), review && problem == S_COUNT ? 1 : 3, false);
}

static void draw_uninstall_bottom(App *a)
{
    char also[96], gb[24];
    ui_rect(0, 0, BOTTOM_W, SCREEN_H, COL_SCRIM);
    ui_card(12, 30, 296, 178, 10, COL_SURFACE);
    ui_text(24, 42, 12, FONT_BOLD, COL_TEXT, T(S_UN_ASK));
    ui_text_wrap(24, 62, 272, 9, FONT_REG, COL_SOFT, T(S_UN_ASK_BODY));
    format_gb(gb, sizeof(gb), (u32)(a->files.bytes >> 20));
    char n[40];
    snprintf(also, sizeof(also), T(S_UN_ALSO), i18n_recordings(a->files.count, n, sizeof(n)), gb);
    Rect c = UN_CHECK;
    ui_rrect(c.x + 2, c.y + 4, 16, 16, 3, a->deleteRecordings ? COL_ACCENT : COL_TRACK);
    if (a->deleteRecordings)
        ui_icon(ICON_CHECK, c.x + 10, c.y + 12, 12, COL_WHITE);
    ui_text(c.x + 26, c.y + 5, 9, FONT_REG, COL_TEXT, also);
    if (a->focus == 0)
        focus_ring(c, 5);
    button(UN_BTN[0], T(S_CANCEL), 0, a->focus == 1);
    button(UN_BTN[1], T(S_UNINSTALL), 2, a->focus == 2);
}

/* an SD card: its body, the cut corner, the contacts */
static void draw_sd_card(float cx, float cy, u32 col)
{
    const float w = 44, h = 58, x = cx - w / 2, y = cy - h / 2;
    ui_rrect(x, y, w, h, 5, col);
    ui_rect(x + w - 12, y - 1, 13, 11, COL_BG);                      /* the corner cut off */
    for (int i = 0; i < 4; i++)
        ui_rrect(x + 6 + i * 7, y + 6, 4, 10, 1, COL_BG);           /* the contacts */
}

static void draw_sdtest_bottom(App *a)
{
    draw_sd_card(160, 90, g_sdt.state == SDT_RUNNING ? COL_ACCENT : COL_SURFACE2);
    if (g_sdt.state == SDT_RUNNING) {
        ui_spinner(160, 160, 10, COL_MUTED);
        return;
    }
    button(SDT_BTN[0], T(g_sdt.state == SDT_IDLE ? S_CANCEL : S_BACK), 0, a->focus == 0);
    button(SDT_BTN[1], T(g_sdt.state == SDT_IDLE ? S_START : S_SDT_AGAIN), 1, a->focus == 1);
}

static void draw_uninstalled_bottom(App *a)
{
    (void)a;
    button((Rect){12, 200, 296, 28}, T(S_EXIT), 1, true);
}

static void draw_gallery_bottom(App *a)
{
    char buf[192], gb[24]; /* UTF-8 Ukrainian: two bytes a letter */
    gal_lock();
    const GalList *l = gal_list();
    navbar(T(S_TILE_GALLERY));
    if (l->loaded) {
        format_gb(gb, sizeof(gb), (u32)(l->bytes >> 20));
        char cnt[40];
        if (!clips_text(buf, sizeof(buf)))   /* while clips are being saved: that, instead of the totals */
            snprintf(buf, sizeof(buf), "%s · %s", i18n_recordings(l->count, cnt, sizeof(cnt)), gb);
        ui_text_right(308, 15, 9, FONT_REG, COL_MUTED, buf);
    } else
        ui_spinner(296, 21, 7, COL_FAINT);
    for (int i = 0; i < GAL_ROWS && a->galTop + i < l->count; i++) {
        const int idx = a->galTop + i;
        const GalItem *it = &l->items[idx];
        const Rect r = gal_row(i);
        ui_card(r.x, r.y, r.w, r.h, 7, idx == a->galSel ? COL_SURFACE2 : COL_SURFACE);
        if (it->thumb && g_galTexOk) {
            if (g_thumbSrc[i] != it->thumb) {
                upload(&g_thumbTex[i], it->thumb, GAL_THUMB_W * GAL_THUMB_H * 2);
                g_thumbSrc[i] = it->thumb;
            }
            ui_tex(&g_thumbTex[i], GAL_THUMB_DRAWN_W, GAL_THUMB_DRAWN_H, r.x + 4, r.y + 3, 1);
        } else
            ui_rrect(r.x + 4, r.y + 3, GAL_THUMB_DRAWN_W, GAL_THUMB_DRAWN_H, 3, COL_BG);
        format_short(buf, sizeof(buf), it->name);
        ui_text(r.x + 62, r.y + 4, 11, FONT_BOLD, COL_TEXT, buf);
        format_gb(gb, sizeof(gb), (u32)(it->bytes >> 20));
        const bool cut = it->info && it->readable && !it->ended;
        if (it->info && !it->readable)
            snprintf(buf, sizeof(buf), "%s · %s", T(S_UNREADABLE), gb);
        else
            snprintf(buf, sizeof(buf), "%s · %s", T(cut ? S_GAL_CUT : S_PROFILE_STD), gb);
        /* the MP4 for the phone, on the right; the line on the left gets what is left of the row */
        int pm = -1;
        const PrepareState ps = prepare_state(it->name, &pm);
        char st[48] = "";
        u32 stCol = COL_SOFT;
        if (ps == PS_READY)
            snprintf(st, sizeof(st), "%s", T(S_GAL_PREP_READY)), stCol = COL_OK;
        else if (ps == PS_FAILED)
            snprintf(st, sizeof(st), "%s", T(S_GAL_PREP_FAILED)), stCol = COL_FAINT;
        else if (ps == PS_WORKING || (ps == PS_QUEUED && pm > 0 && a->cfg.convertAuto)) { /* manual: a paused job waits for a Download */
            snprintf(st, sizeof(st), T(S_GAL_PREP_PCT), pm / 10);
            stCol = ps == PS_WORKING ? COL_SOFT : COL_FAINT;
        }
        const float right = r.x + r.w - 8, stW = st[0] ? ui_text_width(9, FONT_REG, st) + 6 : 0;
        if (st[0])
            ui_text_right(right, r.y + 20, 9, FONT_REG, stCol, st);
        if (cut)
            ui_icon(ICON_WARN, r.x + 67, r.y + 26, 10, COL_WARN);
        if (it->info && it->readable && it->clip && !cut) {
            /* a replay clip: a tag in the replay LED's blue instead of the profile's name */
            const float tw = ui_text_width(8, FONT_BOLD, T(S_GAL_CLIP));
            ui_rrect(r.x + 62, r.y + 19.5f, tw + 10, 13, 4, COL_TAG_BG);
            ui_text(r.x + 67, r.y + 21, 8, FONT_BOLD, COL_LED_BLUE, T(S_GAL_CLIP));
            ui_text_fit(r.x + 62 + tw + 15, r.y + 20, right - stW - (r.x + 62 + tw + 15), 9, FONT_REG, COL_MUTED, gb);
        } else {
            const float lx = r.x + (cut ? 76 : 62);
            ui_text_fit(lx, r.y + 20, right - stW - lx, 9, cut ? FONT_BOLD : FONT_REG, cut ? COL_WARN : COL_MUTED, buf);
        }
        if (it->info && it->readable) {
            format_duration(buf, sizeof(buf), it);
            ui_text_right(right, r.y + 4, 11, FONT_BOLD, COL_TEXT, buf);
        }
        if (idx == a->galSel)
            focus_ring(r, 7);
    }
    if (a->galConfirm && l->count) {
        const GalItem *it = &l->items[a->galSel];
        char when[48];
        format_name(when, sizeof(when), it->name);
        format_gb(gb, sizeof(gb), (u32)(it->bytes >> 20));
        snprintf(buf, sizeof(buf), T(S_DEL_BODY), when, gb);
        ui_rect(0, 0, BOTTOM_W, SCREEN_H, COL_SCRIM);
        ui_card(20, 60, 280, 132, 10, COL_SURFACE);
        ui_text(34, 72, 12, FONT_BOLD, COL_TEXT, T(S_DEL_TITLE));
        ui_text_wrap(34, 94, 252, 9, FONT_REG, COL_SOFT, buf);
        button(DEL_BTN[0], T(S_CANCEL), 0, a->galConfirmFocus == 0);
        button(DEL_BTN[1], T(S_DEL_BTN), 2, a->galConfirmFocus == 1);
    }
    gal_unlock();
}

static void draw_player_bottom(App *a)
{
    ui_rect(0, 0, BOTTOM_W, SCREEN_H, RGBA(0, 0, 0, 0xFF));
    if (player_both() || player_bottom_only()) {
        /* the texture was filled on the top screen's pass (player_present) */
        ui_tex_screen(&g_plTex[1], 320, 160, 120);
        InputState in;
        if (g_plInputs && player_input(&in))
            draw_inputs_bottom(&in);
    } else
        ui_text_center(160, 60, 10, FONT_REG, COL_FAINT, T(S_PL_TOP_ONLY));
    if (!player_controls(a))
        return;
    ui_rect(0, 146, BOTTOM_W, SCREEN_H - 146, RGBA(0x14, 0x16, 0x1A, 0xF0));
    const u32 dur = player_duration_ms(), now = player_time_ms();
    const float frac = dur ? (float)now / dur : 0;
    const float bx = PL_BAR.x, by = PL_BAR.y + 6, bw = PL_BAR.w;
    ui_rrect(bx, by, bw, 4, 2, COL_SURFACE2);
    ui_rrect(bx, by, bw * frac, 4, 2, COL_ACCENT);
    ui_circle(bx + bw * frac, by + 2, 6, COL_WHITE);
    char t[16];
    format_clock(t, sizeof(t), now);
    ui_text(bx, 168, 8, FONT_BOLD, COL_MUTED, t);
    format_clock(t, sizeof(t), dur);
    ui_text_right(bx + bw, 168, 8, FONT_BOLD, COL_MUTED, t);
    ui_rrect(PL_SOUND.x, PL_SOUND.y, PL_SOUND.w, PL_SOUND.h, 8, COL_SURFACE);
    ui_icon(player_muted() || !player_has_audio() ? ICON_MUTE : ICON_SOUND, PL_SOUND.x + 20, PL_SOUND.y + 14, 16,
            player_has_audio() ? COL_TEXT : COL_FAINT);
    ui_icon(ICON_RW, PL_RW.x + 16, PL_RW.y + 16, 18, COL_TEXT);
    ui_circle(PL_PLAY.x + 19, PL_PLAY.y + 19, 19, COL_WHITE);
    ui_icon(player_paused() || player_ended() ? ICON_PLAY : ICON_PAUSE, PL_PLAY.x + 19 + (player_paused() ? 1 : 0),
            PL_PLAY.y + 19, 20, COL_BG);
    ui_icon(ICON_FF, PL_FF.x + 16, PL_FF.y + 16, 18, COL_TEXT);
    /* the hints, centred (X: the buttons, when the recording kept them) */
    const bool keys = player_has_inputs();
    float hw = ui_key_hint_width("←→", T(S_PL_SEEK)) + ui_key_hint_width("A", T(S_PL_PAUSE)) +
               ui_key_hint_width("B", T(S_PL_BACK)) + 20;
    if (keys)
        hw += ui_key_hint_width("X", T(S_PL_INPUTS)) + 10;
    float hx = (BOTTOM_W - hw) / 2;
    hx += ui_key_hint(hx, 222, "←→", T(S_PL_SEEK)) + 10;
    hx += ui_key_hint(hx, 222, "A", T(S_PL_PAUSE)) + 10;
    if (keys)
        hx += ui_key_hint(hx, 222, "X", T(S_PL_INPUTS)) + 10;
    ui_key_hint(hx, 222, "B", T(S_PL_BACK));
}

static void draw_phone_bottom(App *a)
{
    ServerStats ss;
    server_stats(&ss);
    navbar(T(S_TILE_PHONE));
    if (ss.state != SRV_RUNNING) {
        ui_text_wrap(16, 60, 288, 10, FONT_REG, COL_SOFT, T(S_PH_SD));
        return;
    }
    const bool live = ss.connections > 0 || (u32)osGetTime() - ss.lastActivityMs < 5000;
    ui_pill(308, 12, T(live ? S_PH_CONNECTED : S_PH_WAITING), live ? COL_OK : COL_SOFT, live ? COL_OK_BG : COL_SURFACE,
            live);
    char buf[96], t[48];
    ui_card(12, 40, 296, 62, 8, COL_SURFACE);
    format_short(t, sizeof(t), a->phoneName);
    ui_text(22, 48, 11, FONT_BOLD, COL_TEXT, t);
    int pm = -1;
    const PrepareState ps = prepare_state(a->phoneName, &pm);
    if (ps == PS_NONE || ps == PS_QUEUED || ps == PS_WORKING) { /* the console makes the MP4 first */
        ui_text_right(298, 49, 9, FONT_REG, COL_MUTED, T(S_PH_PREPARING));
        const float frac = pm > 0 ? pm / 1000.0f : 0;
        ui_rrect(22, 68, 276, 5, 2.5f, COL_SURFACE2);
        ui_rrect(22, 68, 276 * frac, 5, 2.5f, COL_ACCENT);
        snprintf(buf, sizeof(buf), "%d%%", pm > 0 ? pm / 10 : 0);
        ui_text(22, 80, 9, FONT_REG, COL_MUTED, buf);
        const u64 now = osGetTime();
        if (g_prepSeen < 0 && pm >= 0) {
            g_prepSeen = pm;
            g_prepSeenAt = now;
        } else if (g_prepSeen >= 0 && pm > g_prepSeen + 10 && now > g_prepSeenAt) {
            const float perMs = (float)(pm - g_prepSeen) / (float)(now - g_prepSeenAt);
            const u32 mins = (u32)((1000 - pm) / perMs / 60000.0f) + 1;
            snprintf(t, sizeof(t), T(S_PH_LEFT), (unsigned)mins);
            ui_text_right(298, 80, 9, FONT_REG, COL_MUTED, t);
        }
    } else if (ss.file[0] && ss.fileSize) {
        const bool all = ss.fileDone >= ss.fileSize;
        ui_text_right(298, 49, 9, FONT_REG, all ? COL_OK : COL_MUTED, T(all ? S_PH_READY : S_PH_SENDING));
        const u64 done = ss.fileDone > ss.fileSize ? ss.fileSize : ss.fileDone;
        const float frac = (float)done / ss.fileSize;
        ui_rrect(22, 68, 276, 5, 2.5f, COL_SURFACE2);
        ui_rrect(22, 68, 276 * frac, 5, 2.5f, all ? COL_OK : COL_ACCENT);
        snprintf(buf, sizeof(buf), "%.1f / %.1f %s", done / 1048576.0, ss.fileSize / 1048576.0, T(S_MB));
        for (char *c = buf; *c; c++) /* the language's decimal separator */
            if (*c == '.')
                *c = i18n_decimal();
        ui_text(22, 80, 9, FONT_REG, COL_MUTED, buf);
        if (ss.kbPerSec) {
            const u32 mins = (u32)((ss.fileSize - done) / 1024 / ss.kbPerSec / 60) + 1;
            snprintf(t, sizeof(t), T(S_PH_LEFT), (unsigned)mins);
            snprintf(buf, sizeof(buf), "%lu%c%02lu %s · %s", (unsigned long)(ss.kbPerSec / 1024), i18n_decimal(),
                     (unsigned long)(ss.kbPerSec % 1024 * 100 / 1024), T(S_MBS), t);
            ui_text_right(298, 80, 9, FONT_REG, COL_MUTED, buf);
        }
    } else
        ui_text_wrap(22, 68, 276, 9, FONT_REG, COL_MUTED, T(ps == PS_FAILED ? S_PH_PREP_FAILED : S_PH_IDLE));
    float y = 110;
    const bool preparing = ps == PS_NONE || ps == PS_QUEUED || ps == PS_WORKING;
    const char *note = T(preparing ? S_PH_PREP_BODY : S_PH_SLOW);
    const float h1 = ui_text_wrap_height(270, 9, FONT_REG, note) + 12;
    ui_card(12, y, 296, h1, 7, preparing ? COL_INFO_BG : COL_WARN_BG);
    ui_text_wrap(22, y + 6, 276, 9, FONT_REG, preparing ? COL_TEXT : COL_WARN, note);
    y += h1 + 6;
    const float h2 = ui_text_wrap_height(270, 9, FONT_REG, T(S_PH_SD)) + 12;
    ui_card(12, y, 296, h2, 7, COL_INFO_BG);
    ui_text_wrap(22, y + 6, 276, 9, FONT_REG, COL_TEXT, T(S_PH_SD));
    button(PH_STOP, T(S_PH_STOP), 2, false);
}

void app_draw_bottom(App *a)
{
    ui_base_font(a->screen == SCR_LANG); /* every language's name and preview there, no font to load */
    switch (a->screen) {
    case SCR_PANEL: draw_panel_bottom(a); break;
    case SCR_FIRSTRUN: draw_firstrun_bottom(a); break;
    case SCR_HELP: draw_help_bottom(a); break;
    case SCR_GUIDE: draw_guide_bottom(a); break;
    case SCR_LANG: draw_lang_bottom(a); break;
    case SCR_SETTINGS: draw_settings_bottom(a); break;
    case SCR_SETGROUP: draw_setgroup_bottom(a); break;
    case SCR_HOTKEY: draw_hotkey_bottom(a); break;
    case SCR_GALLERY: draw_gallery_bottom(a); break;
    case SCR_PLAYER: draw_player_bottom(a); break;
    case SCR_PHONE: draw_phone_bottom(a); break;
    case SCR_UNINSTALL: draw_uninstall_bottom(a); break;
    case SCR_SDTEST: draw_sdtest_bottom(a); break;
    case SCR_UNINSTALLED: draw_uninstalled_bottom(a); break;
    }
    if (a->busy) {
        ui_rect(0, 0, BOTTOM_W, SCREEN_H, COL_SCRIM);
        ui_spinner(160, 100, 14, COL_TEXT);
        ui_text_center(160, 128, 11, FONT_BOLD, COL_TEXT, a->busy);
    }
}

#ifdef UI_TOUR
/* ------------------------------------------------------------------ the screen tour (make TOUR=1) */

/* the toasts, each shown once on the panel */
static const Str TOUR_MSG[] = {
    S_MSG_STARTED, S_MSG_STOPPED, S_MSG_STOP_FAILED, S_MSG_START_FAILED, S_MSG_SYSTEM_STOP, S_MSG_GAME_STOP,
    S_MSG_RECOVERED, S_MSG_SOON, S_MSG_AUTO_ON, S_MSG_AUTO_OFF, S_MSG_AUTO_PATCHING, S_MSG_AUTO_SD_ERROR,
    S_MSG_AUTO_UNSUPPORTED, S_MSG_AUTO_FAILED, S_MSG_AUTO_LATER, S_MSG_REPLAY_ON, S_MSG_REPLAY_OFF,
    S_MSG_REPLAY_WRITE, S_MSG_REPLAY_SPACE, S_MSG_CLIPS, S_MSG_BOTTOM_BOTH, S_MSG_MAIN_BOTH, S_SDT_BUSY, S_DELETED, S_DEL_FAILED,
};
#define TOUR_N_MSG ((int)(sizeof(TOUR_MSG) / sizeof(*TOUR_MSG)))
#define TOUR_ROWS (GROUP_MAX * GRP_LANG)   /* every row of every group in focus (its note on the top screen) */
enum { TS_SDT_IDLE, TS_SDT_RUN, TS_SDT_GOOD, TS_SDT_OK, TS_SDT_SLOW, TS_SDT_FAILED, TS_SDT_N };
enum {
    TS_LANG_FIRST, TS_GUIDE, TS_PANEL = TS_GUIDE + GUIDE_PAGES, TS_PANEL_REPLAY, TS_FIRSTRUN, TS_SETTINGS, TS_GROUP,
    TS_LANG = TS_GROUP + TOUR_ROWS, TS_HELP, TS_HOTKEY = TS_HELP + HELP_TOPICS, TS_HOTKEY2, TS_GALLERY, TS_GAL_DELETE,
    TS_PLAYER, TS_SDTEST, TS_UNINSTALL = TS_SDTEST + TS_SDT_N, TS_UNINSTALLED, TS_UN_PARTIAL, TS_MSG,
    TS_PER_LANG = TS_MSG + TOUR_N_MSG
};

/* Puts the app in tour step `step` (of the language `lang`) and names it; returns the frames to
   draw before the picture is taken, 0 when the tour is over. */
int app_tour(App *a, int lang, int step, char *name, size_t cap)
{
    if (step >= TS_PER_LANG)
        return 0;
    if (a->cfg.lang != (Lang)lang || !step) {
        a->cfg.lang = (Lang)lang;
        apply_language(a);
    }
    a->msg[0] = 0;
    a->msgNext[0] = 0;
    a->galConfirm = false;
    int wait = 3;
    if (step == TS_LANG_FIRST) {
        a->langFirst = true;
        lang_scroll_to(lang);
        go(a, SCR_LANG, lang);
        snprintf(name, cap, "lang-first");
    } else if (step < TS_PANEL) {
        a->guidePage = step - TS_GUIDE;
        go(a, SCR_GUIDE, 0);
        snprintf(name, cap, "guide-%d", a->guidePage + 1);
    } else if (step == TS_PANEL || step == TS_PANEL_REPLAY) {
        if (step == TS_PANEL_REPLAY) {   /* Replay on, 2 min */
            a->cfg.replay = true;
            a->cfg.replaySec = 120;
        }
        go(a, SCR_PANEL, step == TS_PANEL_REPLAY ? FOCUS_REPLAY : 0);
        snprintf(name, cap, step == TS_PANEL ? "panel" : "panel-replay");
    } else if (step == TS_FIRSTRUN) {
        go(a, SCR_FIRSTRUN, 0);
        snprintf(name, cap, "firstrun");
    } else if (step == TS_SETTINGS) {
        go(a, SCR_SETTINGS, 0);
        snprintf(name, cap, "settings");
    } else if (step < TS_LANG) {
        a->cfg.replay = false;
        a->cfg.replaySec = 30;
        const int g = (step - TS_GROUP) / GROUP_MAX, row = (step - TS_GROUP) % GROUP_MAX;
        if (row >= GROUP_N[g])
            return 1;   /* no such row: a frame, no picture worth keeping (named as a repeat) */
        a->setGroup = g;
        go(a, SCR_SETGROUP, row);
        snprintf(name, cap, "group-%d-%d", g + 1, row + 1);
    } else if (step == TS_LANG) {
        open_languages(a, SCR_SETTINGS, GRP_LANG);
        snprintf(name, cap, "lang");
    } else if (step < TS_HOTKEY) {
        a->helpTopic = step - TS_HELP;
        a->back = SCR_PANEL;
        go(a, SCR_HELP, 0);
        snprintf(name, cap, "help-%02d", a->helpTopic + 1);
    } else if (step == TS_HOTKEY || step == TS_HOTKEY2) {
        open_hotkey(a);
        a->hkStep = step == TS_HOTKEY2;
        snprintf(name, cap, step == TS_HOTKEY ? "hotkey" : "hotkey-2");
    } else if (step == TS_GALLERY) {
        open_gallery(a, NULL);
        wait = 90; /* the list is read in the background */
        snprintf(name, cap, "gallery");
    } else if (step == TS_GAL_DELETE) {
        if (a->screen != SCR_GALLERY)
            open_gallery(a, NULL);
        a->galConfirm = true;
        snprintf(name, cap, "gallery-delete");
    } else if (step == TS_PLAYER) {   /* the newest recording, the buttons shown (tools/azahar: a synthetic one) */
        if (a->screen != SCR_GALLERY)
            open_gallery(a, NULL);
        gal_lock();
        const GalList *l = gal_list();
        GalItem it;
        const bool have = l->loaded && l->count > 0;
        if (have)
            it = l->items[0];
        gal_unlock();
        if (have) {
            g_plInputs = true;
            open_player(a, &it);
            player_seek(1500);
            a->playerShowUntil = a->frame + 600;
        }
        wait = 150;
        snprintf(name, cap, "player-buttons");
    } else if (step < TS_UNINSTALL) {
        if (a->screen == SCR_PLAYER)
            close_player(a);
        const int k = step - TS_SDTEST;
        memset(&g_sdt, 0, sizeof(g_sdt));
        g_sdt.state = k == TS_SDT_IDLE ? SDT_IDLE : k == TS_SDT_RUN ? SDT_RUNNING : k == TS_SDT_FAILED ? SDT_FAILED : SDT_DONE;
        g_sdt.permille = 420;
        g_sdt.kbPerSec = k == TS_SDT_GOOD ? 9318 : k == TS_SDT_OK ? 5120 : 2867;
        g_sdt.slowestMs = k == TS_SDT_GOOD ? 41 : k == TS_SDT_OK ? 118 : 352;
        g_sdt.rc = (Result)0xC86044D2;
        go(a, SCR_SDTEST, 1);
        static const char *const N[TS_SDT_N] = {"sdtest", "sdtest-run", "sdtest-good", "sdtest-ok", "sdtest-slow", "sdtest-failed"};
        snprintf(name, cap, "%s", N[k]);
    } else if (step == TS_UNINSTALL) {
        go(a, SCR_UNINSTALL, 1);
        snprintf(name, cap, "uninstall");
    } else if (step == TS_UNINSTALLED || step == TS_UN_PARTIAL) {
        a->uninstallOk = step == TS_UNINSTALLED;
        go(a, SCR_UNINSTALLED, 0);
        snprintf(name, cap, "uninstalled%s", a->uninstallOk ? "" : "-partial");
    } else {
        const Str m = TOUR_MSG[step - TS_MSG];
        if (m == S_MSG_START_FAILED)
            snprintf(a->msg, sizeof(a->msg), T(m), "pm:app", 0xD8E06406ul);
        else if (m == S_MSG_CLIPS)
            snprintf(a->msg, sizeof(a->msg), T(m), 3);
        else if (m == S_MSG_REPLAY_ON)
            snprintf(a->msg, sizeof(a->msg), T(m), "X+SELECT", "30 s");
        else if (m == S_MSG_REPLAY_SPACE)
            snprintf(a->msg, sizeof(a->msg), T(m), "2 min", 2238u);
        else
            snprintf(a->msg, sizeof(a->msg), T(m), "Mario Kart 7");
        a->msgImportant = false;
        a->msgUntil = a->frame + 600;
        go(a, SCR_PANEL, 0);
        snprintf(name, cap, "msg-%02d", step - TS_MSG + 1);
    }
    return wait;
}
#endif
