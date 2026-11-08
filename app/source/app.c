#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../sysmod/source/shared.h"
#include "gallery.h"
#include "i18n.h"
#include "player.h"
#include "prepare.h"
#include "qrcodegen.h"
#include "server.h"
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
/* hint row, right side: the language flag (help is a tile now) */
static Rect flag_btn(void) { return (Rect){281, 213, 27, 19}; }
/* Common */
static const Rect BACK_BTN = {12, 10, 22, 22};
/* First run */
static const Rect FR_LATER = {12, 200, 96, 28};
static const Rect FR_START = {116, 200, 192, 28};
/* Settings: 8 rows under a short navbar */
enum { ROW_FPS, ROW_SCREENS, ROW_SOUND, ROW_LED, ROW_LANG, ROW_HOTKEY, ROW_UNINSTALL, SET_ROWS };
static Rect set_row(int i) { return (Rect){10, 33 + i * 25.5f, 300, 23}; }
static const Rect SET_BACK = {10, 6, 21, 21};
/* Hotkey screen buttons */
static const Rect HK_BTN[3] = {{12, 200, 92, 28}, {114, 200, 92, 28}, {216, 200, 92, 28}};
/* Uninstall dialog */
static const Rect UN_CHECK = {24, 128, 272, 24};
static const Rect UN_BTN[2] = {{24, 168, 132, 28}, {164, 168, 132, 28}};
/* Help topics */
#define HELP_TOPICS 7
static Rect help_row(int i) { return (Rect){12, 40 + i * 28.0f, 296, 25}; }   /* 7 topics: down to y 233 */

#define MB_PER_MIN_BOTH_30 128u /* measured on O3DS (v10, 2026-11-03): 2.1 MB/s, both screens, 30 fps */

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

static void refresh(App *a, bool files)
{
    recorder_status(&a->st);
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
    if (a->msgImportant && (a->screen == SCR_LANG || a->screen == SCR_GUIDE))
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
                                        "download", "uninstall", "uninstalled", "guide", "lang"};
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

void app_init(App *a, const char *selfPath)
{
    memset(a, 0, sizeof(*a));
    snprintf(a->selfPath, sizeof(a->selfPath), "%s", selfPath);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    a->new3ds = n3ds;
    config_load(&a->cfg);
    i18n_set_ukrainian(a->cfg.ukrainian);
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
    prepare_start(n3ds); /* recordings become MP4s for the phone in the background */
    /* The recorder starts with the app. Only when Luma cannot load it does the first-run
       screen come up, to say what to change. */
    /* After an app update the running module is the older one: it is restarted with the new one
       (only when idle: a recording is never cut for it). */
    const bool outdated = a->st.running && a->moduleFile == MODULE_DIFFERENT && a->st.state == REC_STATE_IDLE;
    const bool autoStart = (!a->st.running || outdated) && a->lumaModules;
    if (a->cfg.guideSeen) /* else after the first run's language and guide (in its language) */
        tell_system_stop(a);
    app_log("app: %s, lang %s, guide %s, module %s (state %lu, file %s), Luma modules %s, SD free %lu MB, "
            "%lu recordings (%llu MB)%s",
            n3ds ? "New 3DS" : "Old 3DS", a->cfg.ukrainian ? "uk" : "en", a->cfg.guideSeen ? "seen" : "first run",
            a->st.running ? "running" : "not running", (unsigned long)a->st.state,
            a->moduleFile == MODULE_SAME ? "same" : a->moduleFile == MODULE_DIFFERENT ? "different" : "missing",
            a->lumaModules ? "on" : "OFF", (unsigned long)a->sdFreeMb, (unsigned long)a->files.count,
            (unsigned long long)(a->files.bytes >> 20), outdated ? ", outdated module: restarting it" : "");
    if (autoStart) {
        a->pending = ACT_START;
        a->busy = T(S_STATE_STARTING);
    }
    const Screen next = a->st.running || autoStart ? SCR_PANEL : SCR_FIRSTRUN;
    if (!a->cfg.guideSeen) { /* the very first run: the language, how to use it, then on */
        a->afterGuide = next;
        a->guidePage = 0;
        go(a, a->cfg.langSet ? SCR_GUIDE : SCR_LANG, a->cfg.ukrainian ? 1 : 0);
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
    else if (g_workAct == ACT_UNINSTALL)
        g_workOk = recorder_uninstall(g_workApp->deleteRecordings, g_workApp->selfPath);
    g_workDone = true;
}

/* Returns true while an action is running (the caller then skips input). */
static bool run_pending(App *a)
{
    if (!g_worker) {
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
    char buf[160];
    app_log("action %d done: rc %08lX, ok %d, step '%s'", act, (unsigned long)g_workRc, g_workOk, g_workStep);
    if (act == ACT_START) {
        g_infoModuleWanted = true; /* the module file may have been installed */
        refresh(a, true);
        if (R_SUCCEEDED(g_workRc)) {
            say(a, T(S_MSG_STARTED));
            if (a->screen == SCR_FIRSTRUN)
                go(a, SCR_PANEL, 0);
        } else {
            snprintf(buf, sizeof(buf), T(S_MSG_START_FAILED), g_workStep, (unsigned long)g_workRc);
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

static void set_language(App *a, bool uk)
{
    if (a->cfg.ukrainian == uk)
        return;
    a->cfg.ukrainian = uk;
    i18n_set_ukrainian(uk);
    config_set("lang", uk ? "uk" : "en");
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

static bool row_seg(const App *a, int row, Seg *g)
{
    switch (row) {
    case ROW_FPS:
        *g = (Seg){{FPS_LABEL[0], FPS_LABEL[1], FPS_LABEL[2]}, 3, fps_index(a), -1};
        return true;
    case ROW_SCREENS:
        *g = (Seg){{T(S_BOTH), T(S_TOP), NULL}, 2, a->cfg.bothScreens ? 0 : 1, -1};
        return true;
    case ROW_LED:
        *g = (Seg){{T(S_LED_PULSE), T(S_LED_SOLID), T(S_LED_OFF)}, 3, a->cfg.led == 2 ? 0 : a->cfg.led == 1 ? 1 : 2, -1};
        return true;
    case ROW_LANG:
        *g = (Seg){{"English", "Українська", NULL}, 2, a->cfg.ukrainian ? 1 : 0, -1};
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
        if ((opt == 0) != a->cfg.bothScreens)
            apply_setting(a, "screens", opt == 0 ? "both" : "top");
        break;
    case ROW_LED: {
        static const char *const V[3] = {"pulse", "solid", "off"};
        static const int L[3] = {2, 1, 0};
        if (L[opt] != a->cfg.led)
            apply_setting(a, "led", V[opt]);
        break;
    }
    case ROW_LANG:
        set_language(a, opt == 1);
        break;
    }
}

static void open_hotkey(App *a)
{
    a->hkHeld = a->hkCand = 0;
    a->hkReview = false;
    a->hkWaitRelease = true; /* the A that opened this screen is still down */
    go(a, SCR_HOTKEY, 0);
}

static void settings_activate(App *a, int row)
{
    Seg g;
    if (row_seg(a, row, &g)) {
        choose(a, row, (g.sel + 1) % g.n);
    } else if (row == ROW_SOUND)
        apply_setting(a, "audio", a->cfg.audio ? "off" : "on");
    else if (row == ROW_HOTKEY)
        open_hotkey(a);
    else if (row == ROW_UNINSTALL) {
        a->deleteRecordings = false;
        go(a, SCR_UNINSTALL, 1);
    }
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

/* Panel focus: 0-3 the tiles (2 x 2), 4 the language flag (the hint row under the tiles). */
enum { FOCUS_FLAG = 4 };

static void update_panel(App *a, const Input *in)
{
    const u32 d = in->down;
    int f = a->focus;
    if (d & KEY_DLEFT)
        f = f < 4 && f % 2 == 1 ? f - 1 : f;
    if (d & KEY_DRIGHT)
        f = f < 4 && f % 2 == 0 ? f + 1 : f;
    if (d & KEY_DUP)
        f = f == FOCUS_FLAG ? 3 : f >= 2 ? f - 2 : f;
    if (d & KEY_DDOWN)
        f = f < 2 ? f + 2 : FOCUS_FLAG;
    a->focus = f;
    if (d & KEY_A) {
        if (f == FOCUS_FLAG)
            set_language(a, !a->cfg.ukrainian);
        else
            panel_activate(a, f);
    }
    if (d & KEY_X)
        set_language(a, !a->cfg.ukrainian);
    if ((d & KEY_Y) && a->files.newest[0]) { /* the last recording: straight into the player */
        open_gallery(a, a->files.newest);
        a->galAutoPlay = true;
    }
    if (d & KEY_SELECT)
        open_help(a, SCR_PANEL);
    if (d & KEY_START)
        a->quit = true;
    for (int i = 0; i < 4; i++)
        if (hit(TILE[i], in)) {
            a->focus = i;
            panel_activate(a, i);
        }
    if (hit(flag_btn(), in)) {
        a->focus = FOCUS_FLAG;
        set_language(a, !a->cfg.ukrainian);
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
        set_language(a, !a->cfg.ukrainian);
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

/* ------------------------------------------------------------------ language (first run) */

static const Rect LANG_CARD[2] = {{12, 40, 144, 132}, {164, 40, 144, 132}};
static const Rect LANG_GO = {12, 196, 296, 32};

static void update_lang(App *a, const Input *in)
{
    const u32 d = in->down;
    int f = a->focus;
    if (d & KEY_DLEFT)
        f = 0;
    if (d & KEY_DRIGHT)
        f = 1;
    for (int i = 0; i < 2; i++)
        if (hit(LANG_CARD[i], in))
            f = i;
    if (f != a->focus) { /* the screens switch at once, so the choice shows itself */
        a->focus = f;
        a->cfg.ukrainian = f == 1;
        i18n_set_ukrainian(f == 1);
    }
    if ((d & KEY_A) || hit(LANG_GO, in)) {
        a->cfg.langSet = true;
        config_set("lang", a->cfg.ukrainian ? "uk" : "en");
        go(a, SCR_GUIDE, 0);
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
        set_language(a, !a->cfg.ukrainian);
}

static void update_settings(App *a, const Input *in)
{
    const u32 d = in->down;
    if (d & KEY_DUP && a->focus > 0)
        a->focus--;
    if (d & KEY_DDOWN && a->focus < SET_ROWS - 1)
        a->focus++;
    Seg g;
    if (row_seg(a, a->focus, &g) && (d & (KEY_DLEFT | KEY_DRIGHT))) {
        int o = g.sel + ((d & KEY_DRIGHT) ? 1 : -1);
        if (o >= 0 && o < g.n)
            choose(a, a->focus, o);
    }
    if (a->focus == ROW_SOUND && (d & (KEY_DLEFT | KEY_DRIGHT)) && ((d & KEY_DRIGHT) != 0) != a->cfg.audio)
        apply_setting(a, "audio", (d & KEY_DRIGHT) ? "on" : "off");
    if (d & KEY_A)
        settings_activate(a, a->focus);
    for (int r = 0; r < SET_ROWS && in->tapped; r++) {
        const Rect row = set_row(r);
        if (row_seg(a, r, &g)) {
            Rect opt[3] = {{0}};
            seg_layout(row, g.labels, g.n, opt);
            for (int i = 0; i < g.n; i++)
                if (hit(opt[i], in)) {
                    a->focus = r;
                    choose(a, r, i);
                }
        } else if (hit(row, in)) {
            a->focus = r;
            settings_activate(a, r);
        }
    }
    if ((d & KEY_B) || hit(SET_BACK, in))
        go(a, SCR_PANEL, 3);
}

static void hotkey_save(App *a, u32 m)
{
    char cfg[48], label[64], msg[128];
    hk_text(m, cfg, sizeof(cfg));
    hk_label(m, label, sizeof(label));
    apply_setting(a, "hotkey", cfg);
    snprintf(msg, sizeof(msg), T(S_HK_SAVED), label);
    say(a, msg);
    go(a, SCR_SETTINGS, ROW_HOTKEY);
}

/* Capture: the held buttons are the candidate; 1 s steady with 2-3 buttons sets it. Every button
   can be part of a hotkey, so until they are released nothing else reacts; START cancels (it
   cannot be in a hotkey). Review: A saves, B captures again. The touch buttons always work. */
static void update_hotkey(App *a, const Input *in)
{
    const u32 held = in->held & HK_ALLOWED;
    if ((in->down & KEY_START) || hit(HK_BTN[1], in)) {
        go(a, SCR_SETTINGS, ROW_HOTKEY);
        return;
    }
    if (hit(HK_BTN[0], in)) {
        hotkey_save(a, hk_mask(HK_DEFAULT));
        return;
    }
    if (hit(HK_BTN[2], in) && a->hkReview && hk_problem(a->hkCand) == S_COUNT) {
        hotkey_save(a, a->hkCand);
        return;
    }
    if (a->hkWaitRelease) {
        if (!held)
            a->hkWaitRelease = false;
        return;
    }
    if (a->hkReview) {
        if ((in->down & KEY_A) && hk_problem(a->hkCand) == S_COUNT)
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
        go(a, SCR_SETTINGS, ROW_UNINSTALL);
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
    const ServerState st = server_start(a->phoneName, a->cfg.ukrainian ? "uk" : "en", APP_VERSION, a->new3ds);
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
    case SCR_HOTKEY: update_hotkey(a, in); break;
    case SCR_GALLERY: update_gallery(a, in); break;
    case SCR_PLAYER: update_player(a, in); break;
    case SCR_PHONE: update_phone(a, in); break;
    case SCR_UNINSTALL: update_uninstall(a, in); break;
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
        snprintf(buf, cap, "%lu.%lu GB", (unsigned long)(mb / 1024), (unsigned long)(mb % 1024 * 10 / 1024));
    else
        snprintf(buf, cap, "%lu MB", (unsigned long)mb);
}

/* yyyymmdd_hhmmss -> "Nov 3, 2026 · 17:26" / "03.11.2026 · 17:26" */
static void format_name(char *buf, size_t cap, const char *name)
{
    static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (sscanf(name, "%4d%2d%2d_%2d%2d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12) {
        snprintf(buf, cap, "%s", name);
        return;
    }
    if (i18n_ukrainian())
        snprintf(buf, cap, "%02d.%02d.%04d · %02d:%02d", d, mo, y, h, mi);
    else
        snprintf(buf, cap, "%s %d, %04d · %02d:%02d", MON[mo - 1], d, y, h, mi);
}

static u32 minutes_left(const App *a)
{
    u32 perMin = MB_PER_MIN_BOTH_30 * (u32)a->cfg.fps / 30u;
    if (!a->cfg.bothScreens)
        perMin = perMin * 6 / 10;
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
        snprintf(mb, sizeof(mb), "%lu MB", (unsigned long)(a->st.recKb / 1024));
        const u32 kbs = a->st.recSeconds ? a->st.recKb / a->st.recSeconds : 0;
        snprintf(rate, sizeof(rate), "≈ %lu.%lu MB/s", (unsigned long)(kbs / 1024), (unsigned long)(kbs % 1024 * 10 / 1024));
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
        stat_card(14 + 2 * (w + 7), y, w, T(S_SCREENS), T(a->cfg.bothScreens ? S_BOTH : S_TOP),
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
            snprintf(buf, sizeof(buf), "%llu MB · %s", (unsigned long long)(a->files.newestBytes >> 20),
                     i18n_recordings(a->files.count, cnt, sizeof(cnt)));
            ui_text(136, 172, 9, FONT_REG, COL_MUTED, buf);
            int pm = -1; /* its MP4 for the phone */
            const PrepareState ps = prepare_state(a->files.newest, &pm);
            if (ps == PS_READY)
                ui_text_right(376, 140, 9, FONT_REG, COL_OK, T(S_GAL_PREP_READY));
            else if (ps == PS_WORKING || (ps == PS_QUEUED && pm > 0)) {
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

static const Str HELP_TITLE[HELP_TOPICS] = {S_H1, S_H2, S_H3, S_H4, S_H5, S_H6, S_H7};
static const Str HELP_BODY[HELP_TOPICS] = {S_H1_BODY, S_H2_BODY, S_H3_BODY, S_H4_BODY, S_H5_BODY, S_H6_BODY, S_H7_BODY};

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
        static const u32 lamp[4] = {COL_LED_BLUE, COL_ACCENT, COL_LED_GREEN, COL_ACCENT};
        const char *p = T(S_H2_BODY);
        float y = 54;
        for (int i = 0; i < 5 && *p; i++) {
            const char *e = strchr(p, '\n');
            char line[200];
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n >= sizeof(line))
                n = sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = 0;
            if (i < 4)
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
    hk_label(hk_mask(a->cfg.hotkey), hk, sizeof(hk));
    snprintf(body, sizeof(body), T(HELP_BODY[a->helpTopic]), hk, hk);
    ui_text_wrap(14, 54, 372, 10, FONT_REG, COL_SOFT, body);
    ui_key_hint(14, 218, "Y", T(S_GUIDE_OPEN));
    help_channel();
}

static void draw_lang_top(App *a)
{
    (void)a;
    ui_circle(200, 62, 22, COL_SURFACE);
    ui_circle(200, 62, 7, COL_ACCENT);
    ui_text_center(200, 98, 9, FONT_REG, COL_MUTED, "3DS Recorder");
    ui_text_center(200, 128, 19, FONT_BOLD, COL_TEXT, "Choose your language");
    ui_text_center(200, 158, 13, FONT_REG, COL_SOFT, "Оберіть мову");
}

static void draw_lang_bottom(App *a)
{
    static const char *const name[2] = {"English", "Українська"};
    for (int i = 0; i < 2; i++) {
        const Rect r = LANG_CARD[i];
        ui_card(r.x, r.y, r.w, r.h, 12, a->focus == i ? COL_SURFACE2 : COL_SURFACE);
        ui_flag_sized(r.x + (r.w - 72) / 2, r.y + 24, 72, i == 1);
        ui_text_center(r.x + r.w / 2, r.y + 88, 12, FONT_BOLD, COL_TEXT, name[i]);
        if (a->focus == i)
            focus_ring(r, 12);
    }
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

static u32 mb_per_min(const App *a)
{
    const u32 perMin = MB_PER_MIN_BOTH_30 * (u32)a->cfg.fps / 30u;
    return a->cfg.bothScreens ? perMin : perMin * 6 / 10;
}

static void draw_settings_top(App *a)
{
    char v[48], note[220], s1[32], s2[32], s3[32];
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, T(S_FRAME_IN_FILE));
    ui_frame(14, 28, 104, a->cfg.bothScreens ? 122 : 64, 6, 1, COL_FAINT);
    ui_rrect(19, 33, 94, 54, 4, COL_INFO_BG);
    ui_text_center(66, 53, 8, FONT_BOLD, COL_SOFT, T(S_TOP_BOX));
    if (a->cfg.bothScreens) {
        ui_rrect(28, 91, 76, 54, 4, COL_INFO_BG);
        ui_text_center(66, 111, 8, FONT_BOLD, COL_SOFT, T(S_BOTTOM_BOX));
    }
    snprintf(v, sizeof(v), "%d fps · %s", a->cfg.fps, T(a->cfg.audio ? S_SOUND_ON : S_SOUND_OFF));
    ui_text_center(66, a->cfg.bothScreens ? 156 : 98, 9, FONT_BOLD, COL_TEXT, v);
    ui_text(132, 13, 15, FONT_BOLD, COL_TEXT, T(S_FILE_TITLE));
    const u32 perMin = mb_per_min(a);
    const float w = (254 - 6) / 2.0f;
    snprintf(s1, sizeof(s1), "≈ %lu.%lu MB/s", (unsigned long)(perMin / 60), (unsigned long)(perMin % 60 * 10 / 60));
    snprintf(s2, sizeof(s2), "≈ %lu MB", (unsigned long)perMin);
    snprintf(s3, sizeof(s3), T(S_MINUTES), (unsigned)(perMin ? a->sdFreeMb / perMin : 0));
    stat_card(132, 40, w, T(S_CODEC), T(S_CODEC_VAL), NULL);
    stat_card(138 + w, 40, w, T(S_STREAM), s1, NULL);
    stat_card(132, 96, w, T(S_PER_MIN), s2, NULL);
    stat_card(138 + w, 96, w, T(S_FITS), s3, NULL);
    snprintf(note, sizeof(note), T(S_SETTINGS_NOTE), (unsigned)(perMin ? 4096 / perMin : 0));
    const float h = ui_text_wrap_height(234, 9, FONT_REG, note) + 14;
    /* right under the stat cards: the toast takes the bottom of the screen */
    ui_card(132, 154, 254, h, 8, COL_SURFACE);
    ui_text_wrap(142, 161, 234, 9, FONT_REG, COL_SOFT, note);
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

static void draw_uninstalled_top(App *a)
{
    ui_text(14, 13, 9, FONT_REG, COL_MUTED, "3DS Recorder");
    ui_text(14, 25, 17, FONT_BOLD, a->uninstallOk ? COL_TEXT : COL_WARN, T(a->uninstallOk ? S_UN_DONE : S_UN_PARTIAL));
    ui_text_wrap(14, 58, 372, 10, FONT_REG, COL_SOFT, T(a->uninstallOk ? S_UN_DONE_BODY : S_UN_PARTIAL_BODY));
}

/* yyyymmdd_hhmmss -> "03.11 · 17:26" / "Nov 3 · 17:26" (the gallery list) */
static void format_short(char *buf, size_t cap, const char *name)
{
    static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (sscanf(name, "%4d%2d%2d_%2d%2d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12)
        snprintf(buf, cap, "%s", name);
    else if (i18n_ukrainian())
        snprintf(buf, cap, "%02d.%02d · %02d:%02d", d, mo, h, mi);
    else
        snprintf(buf, cap, "%s %d · %02d:%02d", MON[mo - 1], d, h, mi);
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
    frame_box(12, 14, 150, 90, &g_galTex[0], 400, mine && p->ok, failed);
    if (!it->info || it->both)
        frame_box(27, 112, 120, 90, &g_galTex[1], 320, mine && p->hasBottom, failed || (mine && !p->hasBottom));
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
        snprintf(prof, sizeof(prof), "%s · %u", T(S_PROFILE_STD), (unsigned)it->fps);
        if (it->ended)
            snprintf(drop, sizeof(drop), "%lu", (unsigned long)it->dropped);
        else
            snprintf(drop, sizeof(drop), "—");
        stat_card(x, 62, w, T(S_DURATION), dur, NULL);
        stat_card(x + w + 6, 62, w, T(S_SIZE), v, NULL);
        stat_card(x, 118, w, T(S_PROFILE), prof, NULL);
        stat_card(x + w + 6, 118, w, T(S_DROPPED), drop, NULL);
        snprintf(buf, sizeof(buf), "%s · %s", T(it->audio ? S_WITH_SOUND : S_NO_SOUND),
                 T(it->both ? S_BOTH_SCREENS : S_TOP_SCREEN));
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
    else
        ui_spinner(200, 120, 14, COL_FAINT);
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
    if (!a->msg[0] || a->frame > a->msgUntil || (a->msgImportant && (a->screen == SCR_LANG || a->screen == SCR_GUIDE)))
        return;
    const float h = ui_text_wrap_height(350, 9, FONT_REG, a->msg) + 12;
    ui_rrect(14, 226 - h, 372, h, 7, COL_INFO_BG);
    ui_icon(ICON_INFO, 28, 226 - h + 12, 12, COL_INFO);
    ui_text_wrap(40, 226 - h + 6, 340, 9, FONT_REG, COL_TEXT, a->msg);
}

void app_draw_top(App *a)
{
    switch (a->screen) {
    case SCR_PANEL: draw_panel_top(a); break;
    case SCR_FIRSTRUN: draw_firstrun_top(a); break;
    case SCR_HELP: draw_help_top(a); break;
    case SCR_GUIDE: draw_guide_top(a); break;
    case SCR_LANG: draw_lang_top(a); break;
    case SCR_SETTINGS: draw_settings_top(a); break;
    case SCR_HOTKEY: draw_hotkey_top(a); break;
    case SCR_GALLERY: draw_gallery_top(a); break;
    case SCR_PLAYER: draw_player_top(a); break;
    case SCR_PHONE: draw_phone_top(a); break;
    case SCR_UNINSTALL: draw_uninstall_top(a); break;
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
        ui_card(t.x, t.y, t.w, t.h, 10, COL_SURFACE);
        ui_text(t.x + 10, t.y + 11, 11, FONT_BOLD, COL_TEXT, T(S_TILE_RECORDER));
        ui_toggle(t.x + t.w - 42, t.y + 10, a->st.running);
        ui_text_wrap_lines(t.x + 10, t.y + t.h - 32, t.w - 20, 9, FONT_REG, COL_MUTED,
                           T(a->st.running ? S_TILE_RECORDER_ON : S_TILE_RECORDER_OFF), 2);
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
    if (a->focus < 4)
        focus_ring(TILE[a->focus], 10);
    /* hint row: hotkey, language flag */
    const float x = 12 + ui_text(12, 217, 9, FONT_REG, COL_MUTED, T(S_START_STOP)) + 5;
    hk_chips(x, 216, hk_mask(a->cfg.hotkey), COL_MUTED);
    const Rect fb = flag_btn();
    ui_rrect(fb.x, fb.y, fb.w, fb.h, 4, COL_SURFACE2);
    ui_flag(fb.x + 3, fb.y + 3, !a->cfg.ukrainian); /* shows the other language */
    if (a->focus == FOCUS_FLAG)
        focus_ring(fb, 4);
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
        ui_rrect(r.x + 7, r.y + 4, 17, 17, 4, COL_SURFACE2);
        ui_text_center(r.x + 15.5f, r.y + 6, 8, FONT_BOLD, COL_SOFT, n);
        ui_text(r.x + 32, r.y + 5, 10, FONT_BOLD, COL_TEXT, T(HELP_TITLE[i]));
        if (i == a->helpTopic)
            focus_ring(r, 7);
    }
}

static void draw_settings_bottom(App *a)
{
    ui_rrect(SET_BACK.x, SET_BACK.y, SET_BACK.w, SET_BACK.h, 6, COL_SURFACE);
    ui_icon(ICON_BACK, SET_BACK.x + 10.5f, SET_BACK.y + 10.5f, 12, COL_TEXT);
    ui_text(38, 8, 11, FONT_BOLD, COL_TEXT, T(S_SETTINGS));
    static const Str NAME[SET_ROWS] = {S_FPS, S_SCREENS, S_SOUND, S_LED, S_LANGUAGE, S_HOTKEY, S_UNINSTALL_ROW};
    for (int r = 0; r < SET_ROWS; r++) {
        const Rect row = set_row(r);
        if (r != ROW_UNINSTALL)
            ui_card(row.x, row.y, row.w, row.h, 6, COL_SURFACE);
        ui_text(row.x + 8, row.y + 5, 9.5f, FONT_BOLD, r == ROW_UNINSTALL ? COL_ERR : COL_TEXT, T(NAME[r]));
        Seg g;
        if (row_seg(a, r, &g)) {
            Rect opt[3] = {{0}};
            seg_layout(row, g.labels, g.n, opt);
            ui_rrect(opt[0].x - 2, opt[0].y - 1, opt[g.n - 1].x + opt[g.n - 1].w - opt[0].x + 4, opt[0].h + 2, 4, COL_BG);
            for (int i = 0; i < g.n; i++) {
                const bool sel = i == g.sel, dis = i == g.disabled;
                if (sel)
                    ui_rrect(opt[i].x, opt[i].y, opt[i].w, opt[i].h, 3, COL_ACCENT);
                const float tw = ui_text_width(9, FONT_BOLD, g.labels[i]);
                ui_text_center(opt[i].x + opt[i].w / 2, opt[i].y + 2, 9, FONT_BOLD,
                               sel ? COL_WHITE : dis ? COL_FAINT : COL_SOFT, g.labels[i]);
                if (dis)
                    ui_rect(opt[i].x + opt[i].w / 2 - tw / 2, opt[i].y + opt[i].h / 2, tw, 1, COL_FAINT);
            }
        } else if (r == ROW_SOUND)
            ui_toggle(row.x + row.w - 37, row.y + 2.5f, a->cfg.audio);
        else if (r == ROW_HOTKEY) {
            const u32 m = hk_mask(a->cfg.hotkey);
            hk_chips(row.x + row.w - 20 - hk_chips_width(m), row.y + 4.5f, m, COL_MUTED);
            ui_icon(ICON_NEXT, row.x + row.w - 9, row.y + row.h / 2, 10, COL_MUTED);
        } else
            ui_icon(ICON_NEXT, row.x + row.w - 9, row.y + row.h / 2, 10, COL_MUTED);
        if (r == a->focus)
            focus_ring(row, 6);
    }
}

static void draw_hotkey_bottom(App *a)
{
    char now[64];
    ui_text(12, 10, 11, FONT_BOLD, COL_TEXT, T(S_HK_SCREEN));
    ui_card(12, 32, 296, 24, 7, COL_SURFACE);
    ui_text(22, 37, 9.5f, FONT_REG, COL_SOFT, T(S_HK_NOW));
    hk_label(hk_mask(a->cfg.hotkey), now, sizeof(now));
    ui_text_right(298, 37, 9.5f, FONT_BOLD, COL_TEXT, now);
    const bool review = a->hkReview;
    const Str problem = review ? hk_problem(a->hkCand) : S_COUNT;
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
    button(HK_BTN[1], T(S_CANCEL), 0, false);
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
    snprintf(also, sizeof(also), T(S_UN_ALSO), (unsigned long)a->files.count, gb);
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
        if (cut)
            ui_icon(ICON_WARN, r.x + 67, r.y + 26, 10, COL_WARN);
        ui_text(r.x + (cut ? 76 : 62), r.y + 20, 9, cut ? FONT_BOLD : FONT_REG, cut ? COL_WARN : COL_MUTED, buf);
        if (it->info && it->readable) {
            format_duration(buf, sizeof(buf), it);
            ui_text_right(r.x + r.w - 8, r.y + 4, 11, FONT_BOLD, COL_TEXT, buf);
        }
        int pm = -1; /* the MP4 for the phone */
        const PrepareState ps = prepare_state(it->name, &pm);
        if (ps == PS_READY)
            ui_text_right(r.x + r.w - 8, r.y + 20, 9, FONT_REG, COL_OK, T(S_GAL_PREP_READY));
        else if (ps == PS_FAILED)
            ui_text_right(r.x + r.w - 8, r.y + 20, 9, FONT_REG, COL_FAINT, T(S_GAL_PREP_FAILED));
        else if (ps == PS_WORKING || (ps == PS_QUEUED && pm > 0)) {
            snprintf(buf, sizeof(buf), T(S_GAL_PREP_PCT), pm / 10);
            ui_text_right(r.x + r.w - 8, r.y + 20, 9, FONT_REG, ps == PS_WORKING ? COL_SOFT : COL_FAINT, buf);
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
    if (player_both()) {
        /* the texture was filled on the top screen's pass (player_present) */
        ui_tex_screen(&g_plTex[1], 320, 160, 120);
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
    float hx = 64;
    hx += ui_key_hint(hx, 222, "←→", T(S_PL_SEEK)) + 10;
    hx += ui_key_hint(hx, 222, "A", T(S_PL_PAUSE)) + 10;
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
        snprintf(buf, sizeof(buf), "%.1f / %.1f MB", done / 1048576.0, ss.fileSize / 1048576.0);
        ui_text(22, 80, 9, FONT_REG, COL_MUTED, buf);
        if (ss.kbPerSec) {
            const u32 mins = (u32)((ss.fileSize - done) / 1024 / ss.kbPerSec / 60) + 1;
            snprintf(t, sizeof(t), T(S_PH_LEFT), (unsigned)mins);
            snprintf(buf, sizeof(buf), "%lu.%02lu MB/s · %s", (unsigned long)(ss.kbPerSec / 1024),
                     (unsigned long)(ss.kbPerSec % 1024 * 100 / 1024), t);
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
    switch (a->screen) {
    case SCR_PANEL: draw_panel_bottom(a); break;
    case SCR_FIRSTRUN: draw_firstrun_bottom(a); break;
    case SCR_HELP: draw_help_bottom(a); break;
    case SCR_GUIDE: draw_guide_bottom(a); break;
    case SCR_LANG: draw_lang_bottom(a); break;
    case SCR_SETTINGS: draw_settings_bottom(a); break;
    case SCR_HOTKEY: draw_hotkey_bottom(a); break;
    case SCR_GALLERY: draw_gallery_bottom(a); break;
    case SCR_PLAYER: draw_player_bottom(a); break;
    case SCR_PHONE: draw_phone_bottom(a); break;
    case SCR_UNINSTALL: draw_uninstall_bottom(a); break;
    case SCR_UNINSTALLED: draw_uninstalled_bottom(a); break;
    }
    if (a->busy) {
        ui_rect(0, 0, BOTTOM_W, SCREEN_H, COL_SCRIM);
        ui_spinner(160, 100, 14, COL_TEXT);
        ui_text_center(160, 128, 11, FONT_BOLD, COL_TEXT, a->busy);
    }
}
