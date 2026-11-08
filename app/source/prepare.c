/*
 * Background "prepare for the phone" (see prepare.h), at the lowest priority. prep/ is a
 * pipeline of two halves: the producer (reading, decoding, pictures, sound) runs on core 1
 * (Old 3DS: the app's share of that core is raised first, a thread there cannot be made
 * before) or core 2 (New 3DS); the H.264 encoder and the file writes run on the app's core.
 */
#include "prepare.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prep.h"
#include "recorder.h"
#include "../../sysmod/source/shared.h"

#define MAX_ENTRIES 512
#define CORE1_LIMIT 80   /* % of core 1 for the app while encoding (tests/encbench) */

typedef struct {
    char name[20];        /* yyyymmdd_hhmmss.r3d */
    u8 state;             /* PrepareState */
    s16 permille;
} Entry;

static Thread g_thread, g_helper;
static LightLock g_lock;
static Entry g_e[MAX_ENTRIES];      /* under g_lock */
static int g_n;
static char g_first[20];            /* under g_lock */
static char g_forget[20];           /* under g_lock */
static LightEvent g_forgotten;
static volatile u32 g_hold;
static volatile bool g_quit, g_suspended;
static bool g_new3ds;
static aptHookCookie g_hook;
static u32 g_oldLimit;
static bool g_limitSet;
static int g_shareGot;
static volatile bool g_busy;        /* a job is open and not held */
#define CORE1_IDLE 5     /* % of core 1 with no job: the least APT takes (0 is refused, E0E01BF4) */
#define CORE1_HELD 5     /* a job open but held: the producer must still reach its wait (a thread
                            frozen inside malloc or a file call would block the others) */
static volatile bool g_jobOpen;

/* the producer's and the writer's threads */
static LightEvent g_go, g_done, g_wgo, g_wdone;
static Thread g_writer;
static volatile bool g_helperQuit;

void prepare_path(char *out, size_t n, const char *name, const char *ext)
{
    snprintf(out, n, "sdmc:" REC_FILES_FS "/%.15s.%s", name, ext);
}

static void base_of(char *out, size_t n, const char *name)
{
    snprintf(out, n, "sdmc:" REC_FILES_FS "/%.15s", name);
}

static bool exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static Entry *find(const char *name)
{
    for (int i = 0; i < g_n; i++)
        if (strcmp(g_e[i].name, name) == 0)
            return &g_e[i];
    return NULL;
}

static void set_state(const char *name, PrepareState st, int permille)
{
    LightLock_Lock(&g_lock);
    Entry *e = find(name);
    if (e) {
        e->state = (u8)st;
        e->permille = (s16)permille;
    }
    LightLock_Unlock(&g_lock);
}

static int cmp_newest(const void *a, const void *b)
{
    return strcmp(((const Entry *)b)->name, ((const Entry *)a)->name);
}

/* Adds recordings that are new on the card (and drops ones that are gone). */
static void scan(void)
{
    static Entry found[MAX_ENTRIES];
    int n = 0;
    Handle dir = 0;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, recorder_archive(), fsMakePath(PATH_ASCII, REC_FILES_FS))))
        return;
    static FS_DirectoryEntry e[16];
    u32 got = 0;
    while (n < MAX_ENTRIES && R_SUCCEEDED(FSDIR_Read(dir, &got, 16, e)) && got > 0)
        for (u32 i = 0; i < got && n < MAX_ENTRIES; i++) {
            if ((e[i].attributes & FS_ATTRIBUTE_DIRECTORY) || strcmp(e[i].shortExt, "R3D") != 0)
                continue;
            Entry *it = &found[n];
            u32 k = 0;
            for (; k + 1 < sizeof(it->name) && e[i].name[k]; k++)
                it->name[k] = (char)e[i].name[k];
            it->name[k] = 0;
            if (k != 19)
                continue;   /* not ours */
            it->state = PS_QUEUED;
            it->permille = -1;
            n++;
        }
    FSDIR_Close(dir);
    qsort(found, n, sizeof(Entry), cmp_newest);
    /* keep what is known; look at the card only for new names */
    for (int i = 0; i < n; i++) {
        LightLock_Lock(&g_lock);
        const Entry *old = find(found[i].name);
        if (old)
            found[i] = *old;
        LightLock_Unlock(&g_lock);
        if (old)
            continue;
        char path[96];
        prepare_path(path, sizeof(path), found[i].name, "mp4");
        if (exists(path)) {
            found[i].state = PS_READY;
            found[i].permille = 1000;
        } else {
            char base[96];
            base_of(base, sizeof(base), found[i].name);
            found[i].permille = (s16)prep_saved_permille(base);
        }
    }
    LightLock_Lock(&g_lock);
    memcpy(g_e, found, (size_t)n * sizeof(Entry));
    g_n = n;
    LightLock_Unlock(&g_lock);
}

/* The recording to work on: the one asked for first, else the newest not done. */
static bool pick(char *name)
{
    bool ok = false;
    LightLock_Lock(&g_lock);
    Entry *e = g_first[0] ? find(g_first) : NULL;
    if (!e || e->state == PS_READY || e->state == PS_FAILED)
        for (int i = 0; i < g_n && (!e || e->state == PS_READY || e->state == PS_FAILED); i++)
            e = &g_e[i];
    if (e && (e->state == PS_QUEUED || e->state == PS_WORKING)) {
        snprintf(name, 20, "%s", e->name);
        ok = true;
    }
    LightLock_Unlock(&g_lock);
    return ok;
}

static bool recording(void)
{
    static u64 at;
    static bool was;
    const u64 now = osGetTime();
    if (now - at >= 2000) {
        at = now;
        RecStatus st;
        recorder_status(&st);
        was = st.running && (st.state == REC_STATE_RECORDING || st.state == REC_STATE_SAVING);
    }
    return was;
}

static void delete_made(const char *name)
{
    static const char *const ext[] = {"mp4", "mp4.part", "ptbl", "prep"};
    for (int i = 0; i < 4; i++) {
        char path[96];
        prepare_path(path, sizeof(path), name, ext[i]);
        remove(path);
    }
}

/* A request to forget `name` (gallery delete): answered when its job is not open. */
static bool take_forget(const char *open)
{
    LightLock_Lock(&g_lock);
    char name[20];
    snprintf(name, sizeof(name), "%s", g_forget);
    LightLock_Unlock(&g_lock);
    if (!name[0] || (open && strcmp(open, name) == 0))
        return name[0] != 0;
    delete_made(name);
    LightLock_Lock(&g_lock);
    Entry *e = find(name);
    if (e) {
        e->state = PS_NONE;
        e->permille = -1;
    }
    g_forget[0] = 0;
    LightLock_Unlock(&g_lock);
    LightEvent_Signal(&g_forgotten);
    return false;
}

static u64 now_us(void)
{
    return svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000000);
}

static void sleep_us(u32 us)
{
    svcSleepThread((s64)us * 1000);
}

/* The producer's thread (prep_produce: reading, decoding, pictures, sound) on core 1 (Old 3DS)
   or 2 (New 3DS); the encoder runs on this core in thread_main. */
static PrepJob *volatile g_prodJob;

static void helper_main(void *arg)
{
    (void)arg;
    for (;;) {
        LightEvent_Wait(&g_go);
        if (g_helperQuit)
            return;
        prep_produce(g_prodJob);
        LightEvent_Signal(&g_done);
    }
}

/* The writer's thread (prep_write: the MP4's buffers to the card): on this core, above the
   encoder; it mostly waits on the card. */
static void writer_main(void *arg)
{
    (void)arg;
    for (;;) {
        LightEvent_Wait(&g_wgo);
        if (g_helperQuit)
            return;
        prep_write(g_prodJob);
        LightEvent_Signal(&g_wdone);
    }
}

static void job_start(PrepJob *job)
{
    LightEvent_Clear(&g_done);
    LightEvent_Clear(&g_wdone);
    g_prodJob = job;
    g_jobOpen = true;
    LightEvent_Signal(&g_go);
    LightEvent_Signal(&g_wgo);
}

static void set_busy(bool on);

/* Stops the producer, then frees the job (it keeps its last resume point). */
static void job_close(PrepJob **job)
{
    if (!*job)
        return;
    prep_abort(*job);
    LightEvent_Wait(&g_done);
    LightEvent_Wait(&g_wdone);
    prep_close(*job);
    *job = NULL;
    g_jobOpen = false;
    set_busy(false); /* the producer is waiting for the next job: core 1 back to the system */
}

static bool held(void)
{
    return g_hold || g_suspended || recording();
}

/* Old 3DS: the app's share of core 1, where the producer runs next to the system (SD card,
   Wi-Fi). Large only while a job runs, and tuned by who waits: the encoder waiting for pictures
   asks for more, a full queue gives some back. */
static int g_share = 60;

static void set_busy(bool on)
{
    static int applied = -1;
    g_busy = on;
    const int want = on ? g_share : g_jobOpen ? CORE1_HELD : CORE1_IDLE;
    if (want == applied || g_new3ds || !g_limitSet)
        return;
    applied = want;
    const Result rc = APT_SetAppCpuTimeLimit((u32)want);
    static bool told;
    if (R_FAILED(rc) && !told) { /* once: 0 may not be allowed everywhere */
        told = true;
        app_log("prepare: core 1 share %d%% refused (%08lX)", want, (unsigned long)rc);
    }
}

typedef struct {
    u64 at, start;
    double produceWait, consumeWait;
    u32 frames;
} Tune;

static void tune(PrepJob *job, Tune *t, const char *name)
{
    const u64 now = osGetTime();
    if (now - t->at < 2000)
        return;
    const PrepStats *s = prep_stats(job);
    const double span = (double)(now - t->at), dp = s->produceWaitMs - t->produceWait,
                 dc = s->consumeWaitMs - t->consumeWait;
    const int old = g_share;
    if (g_new3ds) /* core 2 is all ours */
        ;
    else if (dc > 0.10 * span && g_share + 10 <= g_shareGot) /* the encoder waits for pictures */
        g_share += 10;
    else if (dp > 0.40 * span && g_share > 30) /* the producer waits for the encoder */
        g_share -= 10;
    if (g_share != old && g_busy)
        set_busy(true);
    if ((now - t->start) / 30000 != (t->at - t->start) / 30000) /* every 30 s */
        app_log("prepare %s: %lu frames in %llu s (%.0f ms each now), core 1 share %d%%; read %.0f decode %.0f yuv %.0f "
                "pcm %.0f audio %.0f ckpt %.0f pwait %.0f disk %.0f | video %.0f write %.0f cwait %.0f ms",
                name, (unsigned long)s->frames, (unsigned long long)((now - t->start) / 1000),
                s->frames > t->frames ? span / (s->frames - t->frames) : 0.0, g_share, s->readMs, s->decodeMs,
                s->yuvMs, s->pcmMs, s->audioMs, s->checkpointMs, s->produceWaitMs, s->diskMs, s->videoMs, s->writeMs,
                s->consumeWaitMs);
    t->at = now;
    t->produceWait = s->produceWaitMs;
    t->consumeWait = s->consumeWaitMs;
    t->frames = s->frames;
}

static void thread_main(void *arg)
{
    (void)arg;
    const PrepOptions opt = {0, 0, now_us, sleep_us};
    PrepJob *job = NULL;
    char name[20] = "";
    u64 scanned = 0;
    Tune t = {0};
    while (!g_quit) {
        if (take_forget(job ? name : NULL)) { /* its job is open: close it first */
            job_close(&job);
            continue;
        }
        if (held()) { /* the producer stops by itself once the queue is full */
            set_busy(false);
            svcSleepThread(200 * 1000 * 1000);
            continue;
        }
        if (osGetTime() - scanned > 5000) { /* new recordings; a directory listing */
            scan();
            scanned = osGetTime();
        }
        if (!job) {
            if (!pick(name)) {
                set_busy(false);
                svcSleepThread(500 * 1000 * 1000);
                continue;
            }
            char src[96], base[96], err[128];
            prepare_path(src, sizeof(src), name, "r3d");
            base_of(base, sizeof(base), name);
            job = prep_open(src, base, &opt, err, sizeof(err));
            if (!job) {
                app_log("prepare %s: %s", name, err);
                set_state(name, PS_FAILED, -1);
                continue;
            }
            if (prep_stats(job)->resumedFrom)
                app_log("prepare %s: resumed at frame %lu", name, (unsigned long)prep_stats(job)->resumedFrom);
            set_state(name, PS_WORKING, (int)prep_permille(job));
            memset(&t, 0, sizeof(t));
            t.at = t.start = osGetTime();
            job_start(job);
        }
        set_busy(true);
        /* another recording asked for first: this one keeps its resume point */
        char want[20];
        LightLock_Lock(&g_lock);
        snprintf(want, sizeof(want), "%s", g_first);
        LightLock_Unlock(&g_lock);
        if (want[0] && strcmp(want, name) != 0) {
            LightLock_Lock(&g_lock);
            const Entry *w = find(want);
            const bool other = w && (w->state == PS_QUEUED);
            LightLock_Unlock(&g_lock);
            if (other) {
                job_close(&job);
                char base[96];
                base_of(base, sizeof(base), name);
                set_state(name, PS_QUEUED, prep_saved_permille(base));
                continue;
            }
        }
        const int r = prep_consume(job);
        if (r == PREP_MORE || r == PREP_WAIT) {
            set_state(name, PS_WORKING, (int)prep_permille(job));
            tune(job, &t, name);
            if (r == PREP_WAIT)
                sleep_us(1000);
            continue;
        }
        const PrepStats *s = prep_stats(job);
        if (r == PREP_DONE) {
            app_log("prepare %s: done in %llu s, %lu video + %lu audio frames (from %lu), core 1 %d%%; read %.0f "
                    "decode %.0f yuv %.0f pcm %.0f audio %.0f ckpt %.0f pwait %.0f disk %.0f | video %.0f write %.0f cwait %.0f ms",
                    name, (unsigned long long)((osGetTime() - t.start) / 1000), (unsigned long)s->frames,
                    (unsigned long)s->audioFrames, (unsigned long)s->resumedFrom, g_share, s->readMs, s->decodeMs,
                    s->yuvMs, s->pcmMs, s->audioMs, s->checkpointMs, s->produceWaitMs, s->diskMs, s->videoMs, s->writeMs,
                    s->consumeWaitMs);
            set_state(name, PS_READY, 1000);
        } else {
            app_log("prepare %s: %s", name, prep_error(job));
            set_state(name, PS_FAILED, -1);
        }
        job_close(&job);
        set_busy(false);
        if (r != PREP_DONE)
            delete_made(name);   /* frees the space; it is not tried again this session */
    }
    job_close(&job);
    set_busy(false);
}

static void on_apt(APT_HookType hook, void *param)
{
    (void)param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP)
        g_suspended = true;
    else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
        g_suspended = false;
}

void prepare_start(bool new3ds)
{
    if (g_thread)
        return;
    g_new3ds = new3ds;
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_forgotten, RESET_ONESHOT);
    LightEvent_Init(&g_go, RESET_ONESHOT);
    LightEvent_Init(&g_done, RESET_ONESHOT);
    LightEvent_Init(&g_wgo, RESET_ONESHOT);
    LightEvent_Init(&g_wdone, RESET_ONESHOT);
    g_quit = g_helperQuit = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* the producer's thread: core 2 on a New 3DS, else core 1, where an application gets no
       time until it asks for a share (svcCreateThread there fails before that) */
    if (!new3ds) {
        if (R_FAILED(APT_GetAppCpuTimeLimit(&g_oldLimit)))
            g_oldLimit = 0;
        /* the CIA allows up to 80% (cia.rsf MaxCpu); an older install or the 3dsx's host title may
           allow less (Luma pm refuses more than the title's maximum): then 30% */
        Result rc = APT_SetAppCpuTimeLimit(CORE1_LIMIT);
        g_shareGot = CORE1_LIMIT;
        if (R_FAILED(rc)) {
            app_log("prepare: core 1 share %d%% refused (%08lX)", CORE1_LIMIT, (unsigned long)rc);
            rc = APT_SetAppCpuTimeLimit(30);
            g_shareGot = 30;
        }
        g_limitSet = R_SUCCEEDED(rc);
        if (g_share > g_shareGot)
            g_share = g_shareGot;
    }
    int core = new3ds ? 2 : 1;
    g_helper = threadCreate(helper_main, NULL, 32 * 1024, prio + 3, core, false);
    if (!g_helper && new3ds)
        g_helper = threadCreate(helper_main, NULL, 32 * 1024, prio + 3, core = 1, false);
    if (!g_helper) /* no other core: both halves share this one */
        g_helper = threadCreate(helper_main, NULL, 32 * 1024, prio + 3, core = -2, false);
    if (!new3ds && g_limitSet) /* made; until a job runs, the system keeps core 1 */
        APT_SetAppCpuTimeLimit(CORE1_IDLE);
    aptHook(&g_hook, on_apt, NULL);
    g_writer = threadCreate(writer_main, NULL, 16 * 1024, prio + 2, -2, false);
    g_thread = g_helper && g_writer ? threadCreate(thread_main, NULL, 64 * 1024, prio + 3, -2, false) : NULL;
    if (new3ds)
        app_log("prepare: started, producer on core %d", core);
    else
        app_log("prepare: started, producer on core %d, core 1 share %s up to %d%%", core,
                g_limitSet ? "set" : "refused", g_shareGot);
}

void prepare_stop(void)
{
    if (!g_thread)
        return;
    g_quit = true;
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = NULL;
    aptUnhook(&g_hook);
    g_helperQuit = true;
    if (g_helper) {
        LightEvent_Signal(&g_go);
        threadJoin(g_helper, U64_MAX);
        threadFree(g_helper);
        g_helper = NULL;
    }
    if (g_writer) {
        LightEvent_Signal(&g_wgo);
        threadJoin(g_writer, U64_MAX);
        threadFree(g_writer);
        g_writer = NULL;
    }
    if (g_limitSet && g_oldLimit)
        APT_SetAppCpuTimeLimit(g_oldLimit);
    g_limitSet = false;
}

bool prepare_busy(void)
{
    return g_busy;
}

void prepare_hold(u32 bit, bool on)
{
    if (on)
        __atomic_fetch_or(&g_hold, bit, __ATOMIC_SEQ_CST);
    else
        __atomic_fetch_and(&g_hold, ~bit, __ATOMIC_SEQ_CST);
}

void prepare_first(const char *name)
{
    LightLock_Lock(&g_lock);
    snprintf(g_first, sizeof(g_first), "%.19s", name);
    LightLock_Unlock(&g_lock);
}

PrepareState prepare_state(const char *name, int *permille)
{
    PrepareState st = PS_NONE;
    int p = -1;
    if (g_thread) {
        LightLock_Lock(&g_lock);
        const Entry *e = find(name);
        if (e) {
            st = (PrepareState)e->state;
            p = e->permille;
        }
        LightLock_Unlock(&g_lock);
    }
    if (permille)
        *permille = p;
    return st;
}

void prepare_forget(const char *name)
{
    if (!g_thread) {
        delete_made(name);
        return;
    }
    LightEvent_Clear(&g_forgotten);
    LightLock_Lock(&g_lock);
    snprintf(g_forget, sizeof(g_forget), "%.19s", name);
    LightLock_Unlock(&g_lock);
    /* the thread answers within a step (one video frame) or a hold check */
    const u64 t0 = osGetTime();
    const bool answered = LightEvent_WaitTimeout(&g_forgotten, 5ull * 1000 * 1000 * 1000) == 0;
    app_log("prepare: forget %s %s after %llu ms", name, answered ? "done" : "TIMED OUT", (unsigned long long)(osGetTime() - t0));
}
