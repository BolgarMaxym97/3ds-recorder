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
#include <sys/stat.h>

#include "join.h"
#include "prep.h"
#include "unwrap.h"
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
static volatile u64 g_idleSince;    /* nothing to do (or held) since then, ms; 0 while working */
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
        prepare_path(path, sizeof(path), found[i].name, "join");
        if (exists(path) && !prepare_join_failed(found[i].name)) {
            found[i].state = PS_NONE;   /* waits to be joined to the recording it continues (join_one) */
            continue;
        }
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

static void delete_outputs(const char *name, int n)
{
    /* "join": its link to the recording it continues (join.h), which goes with it */
    static const char *const ext[] = {"mp4", "mp4.part", "ptbl", "prep", "join"};
    for (int i = 0; i < n; i++) {
        char path[96];
        prepare_path(path, sizeof(path), name, ext[i]);
        remove(path);
    }
}

static void delete_made(const char *name) { delete_outputs(name, 5); }

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

/* ---- replay clips: the recorder's ring files (.rpl) into ordinary recordings */

static volatile int g_clips, g_clipPermille = -1;
static volatile u32 g_clipsDone;

u32 prepare_clips_done(void) { return g_clipsDone; }
static char g_clipFailed[20];   /* not tried again this session */
static bool held(void);

int prepare_clips(int *permille)
{
    if (permille)
        *permille = g_clipPermille;
    return g_clips;
}

/* The oldest clip on the card (they become recordings in the order they were saved). */
static bool find_clip(char *name)
{
    Handle dir = 0;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, recorder_archive(), fsMakePath(PATH_ASCII, REC_FILES_FS))))
        return false;
    static FS_DirectoryEntry e[16];
    u32 got = 0;
    int n = 0;
    name[0] = 0;
    while (R_SUCCEEDED(FSDIR_Read(dir, &got, 16, e)) && got > 0)
        for (u32 i = 0; i < got; i++) {
            if ((e[i].attributes & FS_ATTRIBUTE_DIRECTORY) || strcmp(e[i].shortExt, "RPL") != 0)
                continue;
            char it[20];
            u32 k = 0;
            for (; k + 1 < sizeof(it) && e[i].name[k]; k++)
                it[k] = (char)e[i].name[k];
            it[k] = 0;
            if (k != 19 || e[i].name[k] || strcmp(it, g_clipFailed) == 0)
                continue;   /* not ours (yyyymmdd_hhmmss.rpl), or could not be read */
            n++;
            if (!name[0] || strcmp(it, name) < 0)
                snprintf(name, 20, "%s", it);
        }
    FSDIR_Close(dir);
    g_clips = n;
    return n > 0;
}

/* Waits while the work is held (the player, a transfer, a recording, the app in the background);
   a recording deleted meanwhile is forgotten from here. Stops only when the app quits. */
static int clip_progress(void *ctx, uint64_t done, uint64_t total)
{
    (void)ctx;
    g_clipPermille = total ? (int)(done * 1000u / total) : 0;
    while (!g_quit && held())
        svcSleepThread(200 * 1000 * 1000);
    take_forget(NULL);
    return g_quit;
}

/* One clip into a recording next to it; true when there was one to work on. */
static bool unwrap_clip(void)
{
    char name[20];
    if (!find_clip(name))
        return false;
    char src[96], part[96], dst[96];
    prepare_path(src, sizeof(src), name, "rpl");
    prepare_path(part, sizeof(part), name, "r3d.part");
    prepare_path(dst, sizeof(dst), name, "r3d");
    g_clipPermille = 0;
    const u64 t0 = osGetTime();
    const int rc = r3d_unwrap(src, part, clip_progress, NULL);
    if (rc == UNWRAP_OK && rename(part, dst) == 0) {
        remove(src);
        g_clipsDone++;
        app_log("clip %s: a recording now, %llu s", name, (unsigned long long)((osGetTime() - t0) / 1000));
    } else if (rc == UNWRAP_BAD) {
        remove(src);   /* no frame to keep in it: nothing to show */
        app_log("clip %s: nothing to keep in it, removed", name);
    } else if (rc != UNWRAP_STOPPED) {
        remove(part);
        snprintf(g_clipFailed, sizeof(g_clipFailed), "%s", name);   /* the card is full, or cannot be read: next time */
        app_log("clip %s: could not be made into a recording (%d)", name, rc);
    }
    g_clipPermille = -1;
    if (g_clips > 0)
        g_clips--;
    return true;
}

/* ---- continuations: a recording the recorder paused by itself and went on with in a new file
   (an applet, the lid, HOME in an extended-memory game) is joined to the session's first file
   (join.h). The module leaves "<next>.join" naming that file. */

/* links not tried again this session (no room, the card, a link it cannot read): the gallery
   shows those continuations as recordings of their own */
#define JOIN_FAILED_MAX 32
static char g_joinFailed[JOIN_FAILED_MAX][20];   /* under g_lock */
static int g_joinFailedN;
static volatile int g_joins, g_joinPermille = -1;

static void join_fail(const char *next)
{
    LightLock_Lock(&g_lock);
    if (g_joinFailedN < JOIN_FAILED_MAX)
        snprintf(g_joinFailed[g_joinFailedN++], 20, "%.19s", next);
    LightLock_Unlock(&g_lock);
}

bool prepare_join_failed(const char *next)
{
    bool found = false;
    if (!g_thread)
        return false;
    LightLock_Lock(&g_lock);
    for (int i = 0; i < g_joinFailedN && !found; i++)
        found = strcmp(g_joinFailed[i], next) == 0;
    LightLock_Unlock(&g_lock);
    return found;
}

int prepare_joins(int *permille)
{
    if (permille)
        *permille = g_joinPermille;
    return g_joins;
}

static bool is_rec_name(const char *n)
{
    if (strlen(n) != 19 || strcmp(n + 15, ".r3d") != 0 || n[8] != '_')
        return false;
    for (int i = 0; i < 15; i++)
        if (i != 8 && (n[i] < '0' || n[i] > '9'))
            return false;
    return true;
}

/* The oldest link on the card: the continuation's name and the first file's. */
static bool find_join(char *next, char *first)
{
    Handle dir = 0;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, recorder_archive(), fsMakePath(PATH_ASCII, REC_FILES_FS))))
        return false;
    static FS_DirectoryEntry e[16];
    u32 got = 0;
    int count = 0;
    char best[20] = "";
    while (R_SUCCEEDED(FSDIR_Read(dir, &got, 16, e)) && got > 0)
        for (u32 i = 0; i < got; i++) {
            if ((e[i].attributes & FS_ATTRIBUTE_DIRECTORY) || strcmp(e[i].shortExt, "JOI") != 0)
                continue;
            char it[24];
            u32 k = 0;
            for (; k + 1 < sizeof(it) && e[i].name[k]; k++)
                it[k] = (char)e[i].name[k];
            it[k] = 0;
            if (k != 20 || strcmp(it + 15, ".join") != 0)
                continue;   /* not ours (yyyymmdd_hhmmss.join) */
            memcpy(it + 15, ".r3d", 5);
            if (prepare_join_failed(it))
                continue;
            count++;
            if (!best[0] || strcmp(it, best) < 0)
                snprintf(best, sizeof(best), "%.19s", it);
        }
    FSDIR_Close(dir);
    g_joins = count;
    if (!best[0])
        return false;
    snprintf(next, 20, "%s", best);
    char link[96];
    prepare_path(link, sizeof(link), best, "join");
    FILE *f = fopen(link, "rb");
    char buf[32] = {0};
    const size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f)
        fclose(f);
    buf[n < 19 ? n : 19] = 0;
    snprintf(first, 20, "%s", buf);
    return true;
}

static char g_joinFirst[20], g_joinNext[20];   /* the pair being joined */

/* The pair is one the gallery wants deleted: the join stops first, then the request is answered. */
static bool join_forgotten(void)
{
    LightLock_Lock(&g_lock);
    const bool ours = g_forget[0] && (strcmp(g_forget, g_joinFirst) == 0 || strcmp(g_forget, g_joinNext) == 0);
    LightLock_Unlock(&g_lock);
    return ours;
}

static int join_progress(void *ctx, uint64_t done, uint64_t total)
{
    (void)ctx;
    g_joinPermille = total ? (int)(done * 1000u / total) : 0;
    while (!g_quit && held() && !join_forgotten())
        svcSleepThread(200 * 1000 * 1000);
    if (g_quit || join_forgotten())
        return 1;   /* its files are closed before the request is answered */
    take_forget(NULL);
    return 0;
}

static u64 file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (u64)st.st_size : 0;
}

/* A continuation stays a recording of its own: prepared like any other, shown again. */
static void kept_apart(const char *next)
{
    LightLock_Lock(&g_lock);
    Entry *e = find(next);
    if (e && e->state == PS_NONE) {
        e->state = PS_QUEUED;
        e->permille = -1;
    }
    LightLock_Unlock(&g_lock);
    g_clipsDone++;   /* the gallery reads its list again */
}

/* The joined recording is in place: what was made from either is stale, the gallery reads again. */
static void joined(const char *first, const char *next, const char *link)
{
    remove(link);
    delete_made(first);
    delete_made(next);
    LightLock_Lock(&g_lock);
    Entry *e = find(first);
    if (e) {
        e->state = PS_QUEUED;
        e->permille = -1;
    }
    e = find(next);
    if (e) {
        e->state = PS_NONE;
        e->permille = -1;
    }
    LightLock_Unlock(&g_lock);
    g_clipsDone++;   /* the gallery reads its list again */
}

/* The newest recording named after `first` and before `next` (out: its name), if any: a part of
   the same session whose own join failed. A later part's link still names the session's first file,
   and joining that one to it left the failed part out, in the wrong order. */
static bool part_between(const char *first, const char *next, char *out)
{
    Handle dir = 0;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, recorder_archive(), fsMakePath(PATH_ASCII, REC_FILES_FS))))
        return false;
    static FS_DirectoryEntry e[16];
    u32 got = 0;
    out[0] = 0;
    while (R_SUCCEEDED(FSDIR_Read(dir, &got, 16, e)) && got > 0)
        for (u32 i = 0; i < got; i++) {
            if ((e[i].attributes & FS_ATTRIBUTE_DIRECTORY) || strcmp(e[i].shortExt, "R3D") != 0)
                continue;
            char it[24];
            u32 k = 0;
            for (; k + 1 < sizeof(it) && e[i].name[k]; k++)
                it[k] = (char)e[i].name[k];
            it[k] = 0;
            if (k != 19 || !is_rec_name(it))
                continue;
            if (strcmp(it, first) > 0 && strcmp(it, next) < 0 && strcmp(it, out) > 0)
                memcpy(out, it, 20);   /* k == 19: the name and its 0 */
        }
    FSDIR_Close(dir);
    return out[0] != 0;
}

/* One link handled; true when there was one. Nothing is changed in place (join.h): both
   recordings stay as they are until the joined one is whole and checked. */
static bool join_one(void)
{
    char first[20], next[20];
    if (!find_join(next, first))
        return false;
    char a[96], b[96], part[96], old[96], mark[96], link[96];
    prepare_path(link, sizeof(link), next, "join");
    if (!is_rec_name(first) || strcmp(first, next) >= 0) {
        /* not a link the recorder writes (it names an earlier recording), or one read while it was
           being written: left alone this session, kept apart */
        join_fail(next);
        kept_apart(next);
        app_log("join %s: the link names \"%s\": not now", next, first);
        return true;
    }
    {
        char mid[20];
        if (part_between(first, next, mid)) {
            /* the link names the part before it instead: joined in order, after that one */
            FILE *f = fopen(link, "wb");
            const bool ok = f && fwrite(mid, 1, 19, f) == 19;
            if (f)
                fclose(f);
            app_log("join %s: %s lies between it and %s: linked to it instead%s", next, mid, first, ok ? "" : " (NOT WRITTEN)");
            if (!ok) {
                join_fail(next);
                kept_apart(next);
            }
            return true;
        }
    }
    prepare_path(a, sizeof(a), first, "r3d");
    prepare_path(b, sizeof(b), next, "r3d");
    prepare_path(part, sizeof(part), first, "jpart");
    prepare_path(old, sizeof(old), first, "jold");
    prepare_path(mark, sizeof(mark), first, "jmark");
    /* a join of this pair cut short last time (the app closed, the power gone) */
    const int rec = r3d_join_recover(a, b, part, old, mark);
    if (rec == JOIN_FINISHED) {
        joined(first, next, link);
        app_log("join %s + %s: finished after a stop", first, next);
        return true;
    }
    if (rec == JOIN_STUCK) {
        join_fail(next);
        kept_apart(next);
        app_log("join %s + %s: files left in a state it cannot sort out, left alone", first, next);
        return true;
    }
    if (rec == JOIN_UNDONE)
        app_log("join %s + %s: undone after a stop, both kept; joining again", first, next);
    if (!exists(b) || !exists(a)) {
        remove(link);   /* one of them was deleted: nothing to join */
        kept_apart(next);
        app_log("join %s + %s: %s is gone, kept apart", first, next, exists(a) ? next : first);
        return true;
    }
    const u64 need = file_size(a) + file_size(b);
    const u64 freeB = (u64)recorder_sd_free_mb() << 20;
    if (freeB < need + (64ull << 20)) {
        join_fail(next);
        kept_apart(next);
        app_log("join %s + %s: %llu MB needed, %llu MB free: not now", first, next, (unsigned long long)(need >> 20),
                (unsigned long long)(freeB >> 20));
        return true;
    }
    snprintf(g_joinFirst, sizeof(g_joinFirst), "%s", first);
    snprintf(g_joinNext, sizeof(g_joinNext), "%s", next);
    const u64 t0 = osGetTime();
    g_joinPermille = 0;
    int rc = r3d_join(a, b, part, join_progress, NULL);
    if (rc == JOIN_OK) {
        /* in place only while nothing reads them (the player, a transfer) or records */
        while (!g_quit && held() && !join_forgotten())
            svcSleepThread(200 * 1000 * 1000);
        if (g_quit || join_forgotten()) {
            remove(part);
            rc = JOIN_STOPPED;
        } else if ((delete_outputs(first, 4), delete_outputs(next, 4), r3d_join_commit(a, b, part, old, mark) == 0)) {
            joined(first, next, link);
            app_log("join %s + %s: one recording now, %llu MB, %llu s", first, next, (unsigned long long)(need >> 20),
                    (unsigned long long)((osGetTime() - t0) / 1000));
        } else {
            const int r = r3d_join_recover(a, b, part, old, mark);
            if (r == JOIN_FINISHED)
                joined(first, next, link);
            else {
                join_fail(next);
                kept_apart(next);
            }
            app_log("join %s + %s: putting it in place failed, %s", first, next,
                    r == JOIN_FINISHED ? "finished" : r == JOIN_UNDONE ? "undone, both kept" : "left alone");
        }
    } else if (rc == JOIN_DIFFERENT) {
        remove(link);
        kept_apart(next);
        app_log("join %s + %s: not recordings that join (another kind, cut off, or too long): kept apart", first, next);
    } else if (rc == JOIN_IO) {
        join_fail(next);
        kept_apart(next);
        app_log("join %s + %s: reading or writing failed: kept apart this time", first, next);
    }
    g_joinFirst[0] = g_joinNext[0] = 0;
    g_joinPermille = -1;
    if (g_joins > 0)
        g_joins--;
    if (rc == JOIN_STOPPED)
        take_forget(NULL);   /* a delete of one of them waits for this */
    return true;
}

static void mark_idle(void)
{
    if (!g_idleSince)
        g_idleSince = osGetTime();
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
    u64 scanned = 0, clipLook = 0, joinLook = 0;
    Tune t = {0};
    while (!g_quit) {
        if (take_forget(job ? name : NULL)) { /* its job is open: close it first */
            job_close(&job);
            continue;
        }
        if (held()) { /* the producer stops by itself once the queue is full */
            mark_idle();
            set_busy(false);
            svcSleepThread(200 * 1000 * 1000);
            continue;
        }
        if (!job && osGetTime() - clipLook > 5000) { /* clips first: they are recordings only after this */
            if (unwrap_clip()) {
                g_idleSince = 0;
                scanned = 0;
                continue;
            }
            clipLook = osGetTime();
        }
        if (!job && osGetTime() - joinLook > 5000) { /* then continuations: one recording before its MP4 */
            if (join_one()) {
                g_idleSince = 0;
                scanned = 0;
                continue;
            }
            joinLook = osGetTime();
        }
        if (osGetTime() - scanned > 5000) { /* new recordings; a directory listing */
            scan();
            scanned = osGetTime();
        }
        if (!job) {
            if (!pick(name)) {
                mark_idle();
                set_busy(false);
                svcSleepThread(500 * 1000 * 1000);
                continue;
            }
            g_idleSince = 0;
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

u32 prepare_idle_ms(void)
{
    const u64 since = g_idleSince;
    if (!g_thread)
        return UINT32_MAX;
    return since ? (u32)(osGetTime() - since) : 0;
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
