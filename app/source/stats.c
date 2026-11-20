#include "stats.h"
#include "i18n.h"

#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config.h"
#include "recorder.h"
#include "stats_core.h"

#define REC_LOG "sdmc:/3ds/recorder/recorder.log"
#define REC_LOG_OLD "sdmc:/3ds/recorder/recorder.old.log"
#define DUMPS_DIR "sdmc:/luma/dumps/arm11"
#define APP_CAP (16 * 1024)     /* app.log bytes per launch */
#define REC_CAP (32 * 1024)     /* recorder.log bytes per launch */
#define DUMP_MAX (48 * 1024)    /* a bigger dump is skipped (the Worker takes 48 KB) */
#define WAIT_NS (15ull * 1000 * 1000 * 1000)

static LightLock g_lock;        /* g_state and the stats.ini file; the context while it is open */
static StatsState g_state;
static char g_shown[STATS_ID_LEN + 2];
static Thread g_thread;
static volatile bool g_enabled, g_stop;
static bool g_noState;           /* stats.ini could not be read */
static httpcContext g_ctx;
static bool g_ctxOpen;

/* Static: the app's heap is busy with the MP4 work, and these live for one send per launch. */
static char g_appBuf[APP_CAP + 1], g_recBuf[REC_CAP + 1], g_config[4096];
static char g_body[96 * 1024] __attribute__((aligned(4))); /* httpcAddPostDataRaw takes u32 * */
static u8 g_dump[DUMP_MAX] __attribute__((aligned(4)));

/* Through a temporary file: a power-off mid-write must not lose the marks (the lines would go again). */
static void SaveState(void)
{
    char text[160];
    const size_t n = stats_state_format(&g_state, text, sizeof(text));
    FILE *f = fopen(STATS_INI ".tmp", "w");
    if (!f)
        return;
    const bool ok = fwrite(text, 1, n, f) == n;
    if (fclose(f) == 0 && ok) {
        remove(STATS_INI);
        rename(STATS_INI ".tmp", STATS_INI);
    }
}

/* Loads stats.ini; makes the id on first use: 40 bits from the console's random source, or
 * from timers should ps:ps refuse. */
static void LoadState(void)
{
    char text[160] = {0};
    struct stat st;
    if (stat(STATS_INI, &st) != 0 && stat(STATS_INI ".tmp", &st) == 0) /* a save cut between remove and rename */
        rename(STATS_INI ".tmp", STATS_INI);
    FILE *f = fopen(STATS_INI, "r");
    if (f) {
        text[fread(text, 1, sizeof(text) - 1, f)] = 0;
        fclose(f);
    } else if (stat(STATS_INI, &st) == 0) { /* there but unreadable: no new id, nothing sent this time */
        g_noState = true;
        return;
    }
    stats_state_parse(text, &g_state);
    if (!g_state.id[0]) {
        u8 bytes[5];
        const bool ok = R_SUCCEEDED(psInit()) && R_SUCCEEDED(PS_GenerateRandomBytes(bytes, sizeof(bytes)));
        psExit();
        if (!ok) {
            const u64 t = svcGetSystemTick() ^ (osGetTime() << 20);
            for (int i = 0; i < 5; i++)
                bytes[i] = (u8)(t >> (i * 8) ^ t >> (i * 8 + 40));
        }
        stats_id_encode(bytes, g_state.id);
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/recorder", 0777);
        SaveState();
    }
    snprintf(g_shown, sizeof(g_shown), "%.4s-%.4s", g_state.id, g_state.id + 4);
}

static void Info(const char *path, StatsFile *f)
{
    memset(f, 0, sizeof(*f));
    FILE *in = fopen(path, "rb");
    if (!in)
        return;
    if (fseek(in, 0, SEEK_END) == 0) {
        const long size = ftell(in);
        f->size = size > 0 ? (u32)size : 0;
    }
    rewind(in);
    f->headLen = (u32)fread(f->head, 1, f->size < STATS_HEAD ? f->size : STATS_HEAD, in);
    if (f->headLen < (f->size < STATS_HEAD ? f->size : STATS_HEAD))
        f->size = f->headLen;
    fclose(in);
}

static size_t ReadRange(const char *path, u32 from, u32 to, char *dst)
{
    if (to <= from)
        return 0;
    FILE *in = fopen(path, "rb");
    if (!in)
        return 0;
    size_t n = 0;
    if (fseek(in, (long)from, SEEK_SET) == 0)
        n = fread(dst, 1, to - from, in);
    fclose(in);
    return n;
}

typedef struct {
    const char *text; /* the new whole lines, IP addresses cleaned */
    size_t len;
    StatsMark next;   /* the mark once they are sent */
} Piece;

/* The byte before `at` in the file: '\n' when a cut there starts a whole line. */
static bool LineStart(const char *path, u32 at)
{
    char c = '\n';
    return at == 0 || (ReadRange(path, at - 1, at, &c) == 1 && c == '\n');
}

static bool SameHead(const StatsFile *a, const StatsFile *b)
{
    return b->size >= a->size && b->headLen >= a->headLen && memcmp(a->head, b->head, a->headLen) == 0;
}

static Piece Collect(const char *path, const char *oldPath, const StatsMark *mark, u32 cap, char *buf)
{
    Piece none = {.text = buf, .len = 0, .next = *mark};
    StatsFile cur, old;
    Info(path, &cur);
    Info(oldPath, &old);
    StatsPick k = stats_pick(mark, &cur, &old, cap);
    if (k.trimmed) /* a cut exactly at a line start keeps that line */
        k.trimmed = k.oldTo > k.oldFrom ? !LineStart(oldPath, k.oldFrom) : !LineStart(path, k.curFrom);
    const size_t oldLen = ReadRange(oldPath, k.oldFrom, k.oldTo, buf);
    const size_t curLen = ReadRange(path, k.curFrom, k.curTo, buf + oldLen);
    /* rotated or cut short while being read (the module rotates its log when it starts): next time */
    StatsFile cur2, old2;
    Info(path, &cur2);
    Info(oldPath, &old2);
    if (oldLen != k.oldTo - k.oldFrom || curLen != k.curTo - k.curFrom || !SameHead(&cur, &cur2) ||
        (k.oldTo > k.oldFrom && !SameHead(&old, &old2)))
        return none;
    size_t start, end, used;
    stats_lines(buf, oldLen + curLen, oldLen, k.trimmed, &start, &end, &used);
    Piece p = {.text = buf + start, .next = stats_next_mark(mark, &cur, &old, &k, end, oldLen, used)};
    p.len = stats_scrub(buf + start, end - start);
    return p;
}

/* One POST; the HTTP status, 0 when it did not get an answer. */
static u32 Post(const char *path, const char *type, const void *data, size_t len, const char *dumpName)
{
    u32 status = 0;
    LightLock_Lock(&g_lock);
    if (g_stop || !g_enabled || R_FAILED(httpcOpenContext(&g_ctx, HTTPC_METHOD_POST, path, 0))) {
        LightLock_Unlock(&g_lock);
        return 0;
    }
    g_ctxOpen = true;
    LightLock_Unlock(&g_lock);
    bool ok = R_SUCCEEDED(httpcAddRequestHeaderField(&g_ctx, "User-Agent", "3ds-recorder/" APP_VERSION)) &&
              R_SUCCEEDED(httpcAddRequestHeaderField(&g_ctx, "Content-Type", type));
    if (ok && dumpName)
        ok = R_SUCCEEDED(httpcAddRequestHeaderField(&g_ctx, "X-Id", g_state.id)) &&
             R_SUCCEEDED(httpcAddRequestHeaderField(&g_ctx, "X-Name", dumpName));
    ok = ok && R_SUCCEEDED(httpcAddPostDataRaw(&g_ctx, (const u32 *)data, len)) && !g_stop; /* the app is closing */
    if (ok && R_SUCCEEDED(httpcBeginRequest(&g_ctx)) && R_FAILED(httpcGetResponseStatusCodeTimeout(&g_ctx, &status, WAIT_NS)))
        status = 0;
    LightLock_Lock(&g_lock);
    g_ctxOpen = false;
    httpcCloseContext(&g_ctx);
    LightLock_Unlock(&g_lock);
    return status;
}

static bool Delivered(u32 status) { return status >= 200 && status < 300; }

static bool SendReport(void)
{
    const Piece app = Collect(REC_APP_LOG, REC_APP_LOG_OLD, &g_state.app, APP_CAP, g_appBuf);
    const Piece rec = Collect(REC_LOG, REC_LOG_OLD, &g_state.rec, REC_CAP, g_recBuf);
    size_t configLen = 0;
    FILE *f = fopen(CONFIG_PATH, "r");
    if (f) {
        configLen = fread(g_config, 1, sizeof(g_config) - 1, f);
        fclose(f);
    }
    char system[32] = "";
    if (R_FAILED(osGetSystemVersionDataString(NULL, NULL, system, sizeof(system))))
        system[0] = 0;
    const AppConsole *c = app_console();

    StatsJson j;
    stats_json_begin(&j, g_body, sizeof(g_body));
    stats_json_int(&j, "v", 1);
    stats_json_str(&j, "id", g_state.id, strlen(g_state.id));
    stats_json_str(&j, "app", APP_VERSION, strlen(APP_VERSION));
    stats_json_str(&j, "ch", APP_CHANNEL, strlen(APP_CHANNEL));
    stats_json_str(&j, "build", envIsHomebrew() ? "3dsx" : "cia", envIsHomebrew() ? 4 : 3);
    stats_json_str(&j, "model", c->model, strlen(c->model));
    stats_json_str(&j, "region", c->region, strlen(c->region));
    stats_json_str(&j, "system", system, strlen(system));
    stats_json_str(&j, "luma", c->luma, strlen(c->luma));
    stats_json_str(&j, "kernel", c->kernel, strlen(c->kernel));
    const char *lang = LANGS[i18n_lang()].code; /* the app's language; sys_lang: the console's */
    stats_json_str(&j, "lang", lang, strlen(lang));
    stats_json_str(&j, "sys_lang", c->sysLang, strlen(c->sysLang));
    stats_json_str(&j, "config", g_config, configLen);
    char from[96];
    stats_json_str(&j, "from", from, stats_from(&g_state, from, sizeof(from)));
    stats_json_str(&j, "app_log", app.text, app.len);
    stats_json_str(&j, "rec_log", rec.text, rec.len);
    /* A report the server will never take (too big here, or refused as malformed: 4xx but 429) is
       skipped, not tried again at every launch: the marks move on past those lines. */
    bool skip = false;
    u32 status = 0;
    if (!stats_json_end(&j)) {
        app_log("stats: report too big, its lines skipped");
        skip = true;
    } else {
        status = Post(STATS_URL "/v1/report", "application/json", g_body, j.len, NULL);
        skip = status >= 400 && status < 500 && status != 429;
        if (!Delivered(status) && !skip) {
            app_log("stats: report not sent (HTTP %lu)", (unsigned long)status);
            return false;
        }
        if (skip)
            app_log("stats: report refused (HTTP %lu), its lines skipped", (unsigned long)status);
    }
    LightLock_Lock(&g_lock);
    g_state.app = app.next;
    g_state.rec = rec.next;
    SaveState();
    LightLock_Unlock(&g_lock);
    if (skip)
        return false;
    app_log("stats: sent %u + %u bytes of logs, system %s", (unsigned)app.len, (unsigned)rec.len, system);
    return true;
}

/* The Luma dumps not sent yet, with their files' dates, into app.log before the report goes: a
   dump alone says nothing of when the crash was (the first one sent is the newest of any age), the
   date puts it next to recorder.log's lines (field logs 2026-11-19). The console's clock, local. */
static void LogDumpDates(void)
{
    DIR *d = opendir(DUMPS_DIR);
    if (!d)
        return;
    struct dirent *e;
    u32 logged = 0;
    while ((e = readdir(d)) && logged < 8) {
        u32 n;
        if (!stats_dump_number(e->d_name, &n) || (g_state.dumpSet && n <= g_state.dump))
            continue;
        char path[300];
        snprintf(path, sizeof(path), DUMPS_DIR "/%s", e->d_name);
        u64 mtime = 0;
        if (R_FAILED(archive_getmtime(path, &mtime)))
            continue;
        const time_t t = (time_t)mtime;
        struct tm tm;
        gmtime_r(&t, &tm);
        app_log("stats: luma dump %s dated %04d-%02d-%02d %02d:%02d:%02d", e->d_name, tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        logged++;
    }
    closedir(d);
}

static void SendDump(void)
{
    static u32 nums[256];
    size_t count = 0;
    DIR *d = opendir(DUMPS_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && count < sizeof(nums) / sizeof(nums[0]))
        if (stats_dump_number(e->d_name, &nums[count]))
            count++;
    closedir(d);
    u32 n;
    if (!stats_next_dump(nums, count, g_state.dumpSet, g_state.dump, &n))
        return;
    char name[32], path[64];
    snprintf(name, sizeof(name), "crash_dump_%08lu.dmp", (unsigned long)n);
    snprintf(path, sizeof(path), DUMPS_DIR "/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    size_t len = fread(g_dump, 1, sizeof(g_dump), f);
    const bool whole = fgetc(f) == EOF;
    fclose(f);
    bool done = !whole || len == 0; /* too big or empty: skipped for good */
    if (!done) {
        const u32 status = Post(STATS_URL "/v1/dump", "application/octet-stream", g_dump, len, name);
        done = Delivered(status);
        app_log("stats: %s %s (HTTP %lu)", name, done ? "sent" : "not sent", (unsigned long)status);
    }
    if (done) {
        LightLock_Lock(&g_lock);
        g_state.dumpSet = true;
        g_state.dump = n;
        SaveState();
        LightLock_Unlock(&g_lock);
    }
}

static void Run(void *arg)
{
    (void)arg;
    /* after the start-up work (the module starting, the gallery), and quietly */
    for (int i = 0; i < 20 && !g_stop; i++)
        svcSleepThread(100 * 1000 * 1000);
    u32 wifi = 0;
    if (!g_stop && R_SUCCEEDED(acInit())) {
        ACU_GetWifiStatus(&wifi);
        acExit();
    }
    if (g_stop || !g_enabled || !wifi)
        return;
    /* POST data goes through httpc's shared memory (with 0 every POST fails: 0xD8A0A003, 3ds-ua):
       room for the whole report */
    if (R_FAILED(httpcInit(sizeof(g_body) + 0x8000)))
        return;
    LogDumpDates();
    if (SendReport())
        SendDump();
    if (!g_stop)   /* the app is closing and may not have waited for this thread: its services go with it */
        httpcExit();
}

void stats_start(bool enabled)
{
    LightLock_Init(&g_lock);
    g_enabled = enabled;
    LoadState();
    if (!enabled || g_noState)
        return;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(Run, NULL, 32 * 1024, prio + 2 > 0x3F ? 0x3F : prio + 2, -2, false);
}

void stats_set_enabled(bool enabled) { g_enabled = enabled; }

const char *stats_id(void) { return g_shown; }

void stats_stop(void)
{
    if (!g_thread)
        return;
    g_stop = true;
    LightLock_Lock(&g_lock);
    if (g_ctxOpen)
        httpcCancelConnection(&g_ctx);
    LightLock_Unlock(&g_lock);
    if (R_SUCCEEDED(threadJoin(g_thread, 3ull * 1000 * 1000 * 1000)))
        threadFree(g_thread);
    g_thread = NULL;
}
