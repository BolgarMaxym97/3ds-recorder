#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "recorder.h"
#include "sha256.h"
#include "stats.h"
#include "update_key.h"

#define TRIES 3                       /* a lost connection goes on from where it stopped */
#define SPACE_MARGIN (8u * 1024 * 1024)

static const u8 KEY[32] = UPDATE_PUBLIC_KEY;

static NetConn g_net;
static Thread g_thread;
static volatile bool g_enabled, g_stop, g_cancel;
static volatile UpdPhase g_phase;
static volatile UpdError g_error;
static volatile UpdCheck g_check;
static volatile u32 g_result, g_done;
static char g_self[256];
static char g_text[UPD_MANIFEST_MAX + 1];   /* the manifest as signed */
static size_t g_textLen;
static UpdManifest g_m;

static bool is3dsx(void) { return envIsHomebrew(); }
u32 update_size(void) { return is3dsx() ? g_m.x3dsx.size : g_m.cia.size; }
static const UpdFile *file(void) { return is3dsx() ? &g_m.x3dsx : &g_m.cia; }

/* ---------------------------------------------------------------- the check */

static bool ToText(const u8 *data, u32 len, void *ctx)
{
    (void)ctx;
    if (g_textLen + len > UPD_MANIFEST_MAX)
        return false;
    memcpy(g_text + g_textLen, data, len);
    g_textLen += len;
    g_text[g_textLen] = 0;
    return true;
}

static UpdCheck Check(void)
{
    char sig[160] = "";
    u32 status;
    g_textLen = 0;
    if (!net_get(&g_net, "/v1/update", 0, ToText, NULL, &status, "X-Sig", sig, sizeof(sig))) {
        if (status == 404) {
            app_log("update: no release on the server yet");
            return UPD_CHK_LATEST;
        }
        if (!g_stop)
            app_log("update: check failed (HTTP %lu, 0x%08lX)", (unsigned long)status, (unsigned long)g_result);
        return UPD_CHK_FAILED;
    }
    UpdManifest m;
    if (!upd_sig_ok(g_text, g_textLen, sig, KEY)) {
        app_log("update: the answer's signature does not check (%u bytes): ignored", (unsigned)g_textLen);
        return UPD_CHK_FAILED;
    }
    if (!upd_parse(g_text, g_textLen, &m)) {
        app_log("update: the signed manifest does not parse: ignored");
        return UPD_CHK_FAILED;
    }
    if (upd_version_cmp(m.version, APP_VERSION) <= 0) {
        app_log("update: up to date (%s, the server has %s)", APP_VERSION, m.version);
        return UPD_CHK_LATEST;
    }
    g_m = m;
    app_log("update: %s -> %s available (%s, %lu KB)", APP_VERSION, m.version, is3dsx() ? "3dsx" : "cia",
            (unsigned long)(update_size() / 1024));
    if (g_phase == UPD_NONE)
        g_phase = UPD_AVAILABLE;
    return UPD_CHK_NEWER;
}

/* arg: non-NULL when asked on the screen (at once), NULL at launch (after the start-up work) */
static void CheckThread(void *arg)
{
    for (int i = 0; !arg && i < 30 && !g_stop; i++)
        svcSleepThread(100 * 1000 * 1000);
    u32 wifi = 0;
    if (!g_stop && R_SUCCEEDED(acInit())) {
        ACU_GetWifiStatus(&wifi);
        acExit();
    }
    if (g_stop || (!arg && !g_enabled)) {
        g_check = UPD_CHK_NOT;
        return;
    }
    if (!wifi) {
        g_check = UPD_CHK_NO_WIFI;
        return;
    }
    if (R_FAILED(httpcInit(NET_HTTPC_SHM))) {
        g_check = UPD_CHK_FAILED;
        return;
    }
    g_check = Check();
    if (!g_stop)   /* the app is closing and may not have waited for this thread */
        httpcExit();
}

/* ---------------------------------------------------------------- the install */

typedef struct {
    Handle cia;          /* AM's install handle (CIA) */
    FILE *f;             /* <self>.new (3dsx) */
    u32 offset;
    Sha256 sha;
    Result failed;
} Out;

static bool ToOut(const u8 *data, u32 len, void *ctx)
{
    Out *o = ctx;
    if (o->offset + len > update_size())
        return false;
    if (o->f) {
        if (fwrite(data, 1, len, o->f) != len) {
            o->failed = -1;
            return false;
        }
    } else {
        u32 written = 0;
        const Result r = FSFILE_Write(o->cia, &written, o->offset, data, len, 0);
        if (R_FAILED(r) || written != len) {
            o->failed = R_FAILED(r) ? r : -1;
            return false;
        }
    }
    sha256_update(&o->sha, data, len);
    o->offset += len;
    g_done = o->offset;
    return true;
}

static void Fail(UpdError e, const char *what)
{
    g_error = e;
    g_phase = g_cancel ? UPD_AVAILABLE : UPD_FAILED;
    app_log("update: %s %s (%s, 0x%08lX)", g_cancel ? "cancelled:" : "failed:", what,
            e == UPD_ERR_NET ? "connection" : e == UPD_ERR_SERVER ? "server" : e == UPD_ERR_SPACE ? "space" :
            e == UPD_ERR_CHECK ? "file check" : "install", (unsigned long)g_result);
}

static bool EnoughSpace(void)
{
    FS_ArchiveResource res;
    if (R_FAILED(FSUSER_GetArchiveResource(&res, SYSTEM_MEDIATYPE_SD)))
        return true; /* unknown: the write itself will tell */
    const u64 free = (u64)res.freeClusters * res.clusterSize;
    return free >= (u64)update_size() + SPACE_MARGIN;
}

static void InstallThread(void *arg)
{
    (void)arg;
    const UpdFile *want = file();
    char path[64], self[264], fresh[272];
    snprintf(path, sizeof(path), "/v1/update/%s/%s", g_m.version, is3dsx() ? "3dsx" : "cia");
    app_log("update: download %s (%lu KB)", path, (unsigned long)(want->size / 1024));
    if (!EnoughSpace()) {
        Fail(UPD_ERR_SPACE, "SD card");
        return;
    }
    if (R_FAILED(httpcInit(NET_HTTPC_SHM))) {
        Fail(UPD_ERR_NET, "httpc");
        return;
    }
    const bool am = !is3dsx() && R_SUCCEEDED(amInit());
    Out out = {0};
    sha256_init(&out.sha);
    if (is3dsx()) {
        snprintf(self, sizeof(self), "%s%s", strncmp(g_self, "sdmc:", 5) ? "sdmc:" : "", g_self);
        snprintf(fresh, sizeof(fresh), "%s.new", self);
        out.f = fopen(fresh, "wb");
        if (!out.f) {
            Fail(UPD_ERR_INSTALL, "cannot write the .3dsx.new");
            goto done;
        }
    } else {
        Result r = am ? AM_StartCiaInstall(MEDIATYPE_SD, &out.cia) : -1;
        if (R_FAILED(r)) {
            g_result = (u32)r;
            Fail(UPD_ERR_INSTALL, "AM_StartCiaInstall");
            goto done;
        }
    }

    u32 status = 0;
    bool got = false;
    for (int t = 0; t < TRIES && !got && !g_cancel && !g_stop; t++) {
        const u32 from = out.offset;
        got = net_get(&g_net, path, from, ToOut, &out, &status, NULL, NULL, 0);
        if (!got && (out.failed || (status && status != 200 && status != 206)))
            break; /* the card refused, or the server said no: trying again does not help */
        if (!got && t + 1 < TRIES && !g_cancel)
            app_log("update: connection lost at %lu KB, try %d", (unsigned long)(out.offset / 1024), t + 2);
    }
    if (out.f)
        fclose(out.f);
    uint8_t digest[32];
    char sha[65];
    sha256_final(&out.sha, digest);
    upd_hex(digest, sha);
    if (!got || out.offset != want->size || strcmp(sha, want->sha256)) {
        if (!is3dsx())
            AM_CancelCIAInstall(out.cia); /* nothing half-installed stays */
        else
            remove(fresh);
        if (out.failed) {
            g_result = (u32)out.failed;
            Fail(UPD_ERR_INSTALL, "write");
        } else if (got || out.offset == want->size)
            Fail(UPD_ERR_CHECK, sha);
        else if (status && status != 200 && status != 206) {
            g_result = status;
            Fail(UPD_ERR_SERVER, path);
        } else
            Fail(UPD_ERR_NET, path);
        goto done;
    }

    g_phase = UPD_INSTALLING;
    if (is3dsx()) {
        /* the running one aside first, back again should the new one not take its name */
        char old[272];
        snprintf(old, sizeof(old), "%s.old", self);
        remove(old);
        const bool aside = rename(self, old) == 0;
        if (rename(fresh, self) != 0) {
            if (aside)
                rename(old, self);
            remove(fresh);
            Fail(UPD_ERR_INSTALL, "rename .3dsx.new");
            goto done;
        }
        remove(old);
    } else {
        const Result r = AM_FinishCiaInstall(out.cia);
        if (R_FAILED(r)) {
            g_result = (u32)r;
            Fail(UPD_ERR_INSTALL, "AM_FinishCiaInstall");
            goto done;
        }
    }
    app_log("update: %s installed (%s)", g_m.version, is3dsx() ? "3dsx" : "cia");
    g_phase = UPD_DONE;
done:
    if (am)
        amExit();
    if (!g_stop)
        httpcExit();
}

/* ---------------------------------------------------------------- the rest */

static void StartCheck(bool now)
{
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_check = UPD_CHK_RUNNING;
    g_thread = threadCreate(CheckThread, now ? (void *)1 : NULL, 16 * 1024, prio + 2 > 0x3F ? 0x3F : prio + 2, -2, false);
    if (!g_thread)
        g_check = UPD_CHK_FAILED;
}

void update_start(bool enabled, const char *self)
{
    net_conn_init(&g_net, &g_stop, &g_cancel, &g_result);
    snprintf(g_self, sizeof(g_self), "%s", self ? self : "/3ds/3ds-recorder.3dsx");
    g_enabled = enabled;
    if (enabled)
        StartCheck(false);
}

static void Join(void);

bool update_check_now(void)
{
    if (g_check == UPD_CHK_RUNNING || update_busy() || g_phase == UPD_DONE)
        return false;
    Join();
    StartCheck(true);
    return g_thread != NULL;
}

UpdCheck update_check(void) { return g_check; }

/* Only the next launches: an update found now stays shown (the user may have just asked for it). */
void update_set_enabled(bool enabled) { g_enabled = enabled; }

UpdPhase update_phase(void) { return g_phase; }
const UpdManifest *update_info(void) { return &g_m; }
UpdError update_error(void) { return g_error; }
u32 update_result(void) { return g_result; }
bool update_busy(void) { return g_phase == UPD_DOWNLOADING || g_phase == UPD_INSTALLING; }

float update_progress(void)
{
    const u32 size = update_size();
    return size ? (float)g_done / (float)size : 0.0f;
}

int update_notes(const char *lang, char lines[][UPD_NOTE_LEN], int max)
{
    return g_textLen ? upd_notes(g_text, g_textLen, lang, lines, max) : 0;
}

static void Join(void)
{
    if (!g_thread)
        return;
    if (R_SUCCEEDED(threadJoin(g_thread, 20ull * 1000 * 1000 * 1000)))
        threadFree(g_thread);
    g_thread = NULL;
}

bool update_begin(void)
{
    if ((g_phase != UPD_AVAILABLE && g_phase != UPD_FAILED) || !g_m.version[0])
        return false;
    Join(); /* the check's thread, long done */
    g_cancel = false;
    g_error = UPD_ERR_NONE;
    g_result = 0;
    g_done = 0;
    g_phase = UPD_DOWNLOADING;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* a step above the main loop's priority number: the UI keeps drawing the progress */
    g_thread = threadCreate(InstallThread, NULL, 24 * 1024, prio + 1 > 0x3F ? 0x3F : prio + 1, -2, false);
    if (!g_thread) {
        g_phase = UPD_FAILED;
        g_error = UPD_ERR_INSTALL;
        return false;
    }
    return true;
}

void update_cancel(void)
{
    if (g_phase != UPD_DOWNLOADING)
        return;
    g_cancel = true;
    net_abort(&g_net);
}

void update_stop(void)
{
    if (!g_thread)
        return;
    g_stop = true;
    if (g_phase == UPD_INSTALLING) /* a CIA being committed, or the 3dsx renamed: let it finish */
        Join();
    net_abort(&g_net);
    Join();
}

#ifdef UI_TOUR
/* The screen tour (make TOUR=1): a made-up newer version in a given state, nothing downloaded. */
void update_tour(UpdPhase phase, UpdCheck check, UpdError error)
{
    static const char TEXT[] =
        "version=1.7.0\n"
        "cia=8812544 0000000000000000000000000000000000000000000000000000000000000000\n"
        "3dsx=9012345 0000000000000000000000000000000000000000000000000000000000000000\n"
        "notes.en=Update the app from Settings: no more trips to GitHub|Buttons in the MP4 on the phone too|"
        "Replay starts faster after a game launches|Fixes for New 3DS\n"
        "notes.uk=Оновлення застосунку прямо з Налаштувань|Кнопки в MP4 і на телефоні|"
        "Replay швидше вмикається після запуску гри|Виправлення для New 3DS\n"
        "notes.ja=設定からソフトをアップデート|新しい機能\n";
    g_textLen = sizeof(TEXT) - 1;
    memcpy(g_text, TEXT, sizeof(TEXT));
    upd_parse(g_text, g_textLen, &g_m);
    g_phase = phase;
    g_check = check;
    g_error = error;
    g_result = error == UPD_ERR_INSTALL ? 0xC8E083FC : 0;
    g_done = update_size() / 100 * 63;
}
#endif
