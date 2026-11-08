/*
 * sdbench.3dsx — phase 0, test T1: SD write ceiling without a game running.
 *
 * Writes a large file sequentially for every variant in the matrix (block
 * size x preallocation, plus alignment / flush / API / FS priority extras)
 * and records per-write latency and worst 1-second throughput.
 *
 * Output (SD):
 *   /3ds/recorder/bench/sdbench_<model>_<sd>_<run>.csv            summary, one row per variant
 *   /3ds/recorder/bench/sdbench_<model>_<sd>_<run>_<variant>.csv  per-write timeline
 * Logs are written between variants, never during a measurement.
 */
#include <3ds.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bench.h"

#define BENCH_DIR      "/3ds/recorder/bench"
#define TMP_PATH       BENCH_DIR "/sdbench.tmp"
#define KB             1024u
#define MB             (1024u * 1024u)
#define MAX_BLOCK      (1u * MB)
#define CHUNK_PREALLOC (16u * MB)
#define FLUSH_EVERY    (8u * MB)
#define SIZE_FULL      (512u * MB)
#define SIZE_QUICK     (64u * MB)
#define MAX_EVENTS     (SIZE_FULL / (16u * KB))
#define FREE_MARGIN    (16ull * MB)

typedef enum { PREALLOC_NONE, PREALLOC_FULL, PREALLOC_CHUNK } prealloc_t;
typedef enum { FLUSH_NONE, FLUSH_EACH_WRITE, FLUSH_PERIODIC } flush_t;
typedef enum { API_FS, API_STDIO } api_t;

typedef struct {
    char name[24];
    u32 block;
    prealloc_t prealloc;
    u32 misalign;     /* buffer offset from a 0x1000 boundary */
    flush_t flush;
    api_t api;
    int fs_priority;  /* -1: leave untouched */
} variant_t;

typedef struct {
    bool done;
    Result error;     /* first failing call, 0 if none */
    u64 bytes;
    double avg_mbps;
    double min1s_mbps;
    u32 p50_us, p99_us, p999_us, max_us;
    u32 setsize_count, setsize_max_us;
    u64 setsize_total_us;
    u32 flush_count, flush_max_us;
    u32 close_us;
} result_t;

static const char *const PREALLOC_NAMES[] = { "none", "full", "chunk16M" };
static const char *const FLUSH_NAMES[] = { "none", "each", "every8M" };
static const char *const API_NAMES[] = { "fs", "stdio" };
static const char *const SD_LABELS[] = { "S1", "S2", "S3", "S4" };

#define MAX_VARIANTS 32
static variant_t variants[MAX_VARIANTS];
static result_t results[MAX_VARIANTS];
static int variant_count;

static bench_hist_t write_hist;
static bench_io_event_t io_storage[MAX_EVENTS];
static bench_io_log_t io_log;
static u8 *data_base;

/* Shared with the UI thread. */
static volatile bool abort_requested;
static volatile bool worker_finished;
static volatile int current_variant;
static volatile u64 current_bytes;

static struct {
    FS_Archive sdmc;
    u64 total;
    char model[8];
    char sd_label[4];
    char run_id[20];
    u32 cluster_bytes;
    u64 free_bytes;
    bool speedup;
    char summary_path[128];
} cfg;

/* ------------------------------------------------------------- matrix */

static void add_variant(u32 block, prealloc_t prealloc, u32 misalign, flush_t flush, api_t api, int prio, const char *tag)
{
    variant_t *v = &variants[variant_count++];
    v->block = block;
    v->prealloc = prealloc;
    v->misalign = misalign;
    v->flush = flush;
    v->api = api;
    v->fs_priority = prio;
    snprintf(v->name, sizeof(v->name), "%luK_%s%s", (unsigned long)(block / KB), PREALLOC_NAMES[prealloc], tag);
}

static void build_matrix(void)
{
    static const u32 blocks[] = { 16 * KB, 32 * KB, 64 * KB, 128 * KB, 256 * KB, 512 * KB, 1 * MB };
    variant_count = 0;

    /* A: block size x preallocation. */
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        for (int p = PREALLOC_NONE; p <= PREALLOC_CHUNK; p++) {
            add_variant(blocks[b], (prealloc_t)p, 0, FLUSH_NONE, API_FS, -1, "");
        }
    }

    /* B: one factor at a time against the 256K / no-prealloc baseline. */
    add_variant(256 * KB, PREALLOC_NONE, 4, FLUSH_NONE, API_FS, -1, "_mis4");
    add_variant(256 * KB, PREALLOC_NONE, 0x80, FLUSH_NONE, API_FS, -1, "_mis80");
    add_variant(256 * KB, PREALLOC_NONE, 0, FLUSH_EACH_WRITE, API_FS, -1, "_flushE");
    add_variant(256 * KB, PREALLOC_NONE, 0, FLUSH_PERIODIC, API_FS, -1, "_flush8M");
    add_variant(256 * KB, PREALLOC_NONE, 0, FLUSH_NONE, API_STDIO, -1, "_stdio");
    add_variant(256 * KB, PREALLOC_NONE, 0, FLUSH_NONE, API_FS, 1, "_prio1");
}

/* ------------------------------------------------------------- helpers */

static u32 elapsed_us(u64 start, u64 end)
{
    u64 us = bench_ticks_to_us(end - start);
    return us > UINT32_MAX ? UINT32_MAX : (u32)us;
}

static void fill_pattern(u8 *buf, u32 size)
{
    u32 x = 0x12345678u;
    u32 *w = (u32 *)buf;
    for (u32 i = 0; i < size / 4; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w[i] = x;
    }
}

static void delete_tmp(void)
{
    FSUSER_DeleteFile(cfg.sdmc, fsMakePath(PATH_ASCII, TMP_PATH));
}

static void note_error(result_t *r, Result rc)
{
    if (r->error == 0 && R_FAILED(rc)) {
        r->error = rc;
    }
}

static void timed_set_size(Handle h, u64 size, result_t *r)
{
    u64 t0 = bench_ticks();
    Result rc = FSFILE_SetSize(h, size);
    u32 us = elapsed_us(t0, bench_ticks());
    note_error(r, rc);
    r->setsize_count++;
    r->setsize_total_us += us;
    if (us > r->setsize_max_us) {
        r->setsize_max_us = us;
    }
}

static void timed_flush(Handle h, result_t *r)
{
    u64 t0 = bench_ticks();
    Result rc = FSFILE_Flush(h);
    u32 us = elapsed_us(t0, bench_ticks());
    note_error(r, rc);
    r->flush_count++;
    if (us > r->flush_max_us) {
        r->flush_max_us = us;
    }
}

/* ------------------------------------------------------------- one variant */

static void run_variant(const variant_t *v, result_t *r)
{
    memset(r, 0, sizeof(*r));
    bench_hist_reset(&write_hist);
    bench_io_log_init(&io_log, io_storage, MAX_EVENTS);
    current_bytes = 0;
    delete_tmp();

    const u8 *buf = data_base + v->misalign;
    Handle h = 0;
    FILE *f = NULL;
    u32 old_priority = 0;
    Result rc;

    if (v->fs_priority >= 0) {
        FSUSER_GetPriority(&old_priority);
        note_error(r, FSUSER_SetPriority((u32)v->fs_priority));
    }

    if (v->api == API_FS) {
        rc = FSUSER_OpenFile(&h, cfg.sdmc, fsMakePath(PATH_ASCII, TMP_PATH), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
        note_error(r, rc);
        if (R_FAILED(rc)) {
            goto out;
        }
    } else {
        f = fopen("sdmc:" TMP_PATH, "wb");
        if (!f) {
            r->error = -1;
            goto out;
        }
        setvbuf(f, NULL, _IONBF, 0);
    }

    u64 allocated = 0;
    if (v->api == API_FS && v->prealloc == PREALLOC_FULL) {
        timed_set_size(h, cfg.total, r);
        allocated = cfg.total;
    }

    u64 next_flush = FLUSH_EVERY;
    u64 offset = 0;
    while (offset < cfg.total && !abort_requested && r->error == 0) {
        if (v->api == API_FS && v->prealloc == PREALLOC_CHUNK && offset + v->block > allocated) {
            allocated += CHUNK_PREALLOC;
            if (allocated > cfg.total) {
                allocated = cfg.total;
            }
            timed_set_size(h, allocated, r);
        }

        u64 t0 = bench_ticks();
        if (v->api == API_FS) {
            u32 written = 0;
            u32 flags = v->flush == FLUSH_EACH_WRITE ? FS_WRITE_FLUSH : 0;
            rc = FSFILE_Write(h, &written, offset, buf, v->block, flags);
            if (R_SUCCEEDED(rc) && written != v->block) {
                rc = -2;
            }
        } else {
            rc = fwrite(buf, 1, v->block, f) == v->block ? 0 : -3;
        }
        u64 t1 = bench_ticks();
        note_error(r, rc);
        if (R_FAILED(rc)) {
            break;
        }

        bench_hist_add_us(&write_hist, elapsed_us(t0, t1));
        bench_io_log_add(&io_log, t0, t1, v->block);
        offset += v->block;
        current_bytes = offset;

        if (v->api == API_FS && v->flush == FLUSH_PERIODIC && offset >= next_flush) {
            timed_flush(h, r);
            next_flush += FLUSH_EVERY;
        }
    }

    u64 tc = bench_ticks();
    if (v->api == API_FS) {
        FSFILE_Close(h);
    } else {
        fclose(f);
    }
    r->close_us = elapsed_us(tc, bench_ticks());
    r->bytes = offset;

    if (io_log.n > 0) {
        const bench_io_event_t *first = &io_log.ev[0];
        const bench_io_event_t *last = &io_log.ev[io_log.n - 1];
        double secs = (double)(last->end_tick - first->start_tick) / (double)BENCH_TICKS_PER_SEC;
        r->avg_mbps = secs > 0 ? (double)offset / MB / secs : 0;
        double min_rate = bench_io_min_window_rate(io_log.ev, io_log.n, BENCH_TICKS_PER_SEC);
        r->min1s_mbps = min_rate >= 0 ? min_rate / MB : -1;
    }
    r->p50_us = bench_hist_percentile_us(&write_hist, 50);
    r->p99_us = bench_hist_percentile_us(&write_hist, 99);
    r->p999_us = bench_hist_percentile_us(&write_hist, 99.9);
    r->max_us = write_hist.max_us;
    r->done = !abort_requested && r->error == 0;

out:
    if (v->fs_priority >= 0) {
        FSUSER_SetPriority(old_priority);
    }
    delete_tmp();
}

/* ------------------------------------------------------------- CSV output */

static void write_summary_header(void)
{
    FILE *f = fopen(cfg.summary_path, "w");
    if (!f) {
        return;
    }
    fputs("model,sd,cluster_bytes,free_mb,speedup,variant,block_kb,prealloc,misalign,flush,api,fs_priority,"
          "total_mb,avg_mbps,min1s_mbps,write_p50_us,write_p99_us,write_p999_us,write_max_us,"
          "setsize_count,setsize_total_us,setsize_max_us,flush_count,flush_max_us,close_us,result,complete\n", f);
    fclose(f);
}

static void append_summary(const variant_t *v, const result_t *r)
{
    FILE *f = fopen(cfg.summary_path, "a");
    if (!f) {
        return;
    }
    fprintf(f, "%s,%s,%lu,%llu,%d,%s,%lu,%s,%lu,%s,%s,%d,"
               "%.1f,%.3f,%.3f,%lu,%lu,%lu,%lu,"
               "%lu,%llu,%lu,%lu,%lu,%lu,0x%08lX,%d\n",
            cfg.model, cfg.sd_label, (unsigned long)cfg.cluster_bytes, cfg.free_bytes / MB, cfg.speedup,
            v->name, (unsigned long)(v->block / KB), PREALLOC_NAMES[v->prealloc], (unsigned long)v->misalign,
            FLUSH_NAMES[v->flush], API_NAMES[v->api], v->fs_priority,
            (double)r->bytes / MB, r->avg_mbps, r->min1s_mbps,
            (unsigned long)r->p50_us, (unsigned long)r->p99_us, (unsigned long)r->p999_us, (unsigned long)r->max_us,
            (unsigned long)r->setsize_count, r->setsize_total_us, (unsigned long)r->setsize_max_us,
            (unsigned long)r->flush_count, (unsigned long)r->flush_max_us, (unsigned long)r->close_us,
            (unsigned long)r->error, r->done);
    fclose(f);
}

static void write_timeline(const variant_t *v)
{
    /* summary_path is ".../sdbench_<model>_<sd>_<run>.csv"; insert "_<variant>". */
    char path[sizeof(cfg.summary_path) + sizeof(v->name) + 2];
    int stem = (int)strlen(cfg.summary_path) - 4;
    snprintf(path, sizeof(path), "%.*s_%.23s.csv", stem, cfg.summary_path, v->name);
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fputs("idx,start_us,dur_us,bytes\n", f);
    u64 origin = io_log.n ? io_log.ev[0].start_tick : 0;
    for (u32 i = 0; i < io_log.n; i++) {
        const bench_io_event_t *e = &io_log.ev[i];
        fprintf(f, "%lu,%llu,%lu,%lu\n", (unsigned long)i, bench_ticks_to_us(e->start_tick - origin),
                (unsigned long)elapsed_us(e->start_tick, e->end_tick), (unsigned long)e->bytes);
    }
    fclose(f);
}

/* ------------------------------------------------------------- worker */

static void worker(void *arg)
{
    (void)arg;
    write_summary_header();
    for (int i = 0; i < variant_count && !abort_requested; i++) {
        current_variant = i;
        run_variant(&variants[i], &results[i]);
        if (results[i].bytes > 0) {
            write_timeline(&variants[i]);
        }
        append_summary(&variants[i], &results[i]);
    }
    worker_finished = true;
}

/* ------------------------------------------------------------- UI */

static PrintConsole top, bottom;

static void ensure_dirs(void)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/recorder", 0777);
    mkdir("sdmc:" BENCH_DIR, 0777);
}

static void read_sd_info(void)
{
    FS_ArchiveResource res;
    if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&res))) {
        cfg.cluster_bytes = res.clusterSize;
        cfg.free_bytes = (u64)res.freeClusters * res.clusterSize;
    }
}

static void print_menu(int sd_idx, bool is_new)
{
    consoleSelect(&bottom);
    consoleClear();
    printf("sdbench - phase 0 / T1\n\n");
    printf("SD label   < %s >   L / R\n", SD_LABELS[sd_idx]);
    printf("Size       %lu MB     X\n", (unsigned long)(cfg.total / MB));
    if (is_new) {
        printf("Speedup    %s       Y\n", cfg.speedup ? "on " : "off");
    }
    printf("\nVariants   %d\n", variant_count);
    printf("Cluster    %lu KB\n", (unsigned long)(cfg.cluster_bytes / KB));
    printf("Free       %llu MB\n", cfg.free_bytes / MB);
    printf("\nA: start   START: exit\n");
    printf("B during run: abort\n");
    printf("\nClose other apps. Keep the\nconsole on charger, lid open.\n");
}

static void print_result_line(int i)
{
    const result_t *r = &results[i];
    if (r->error) {
        printf("%-16s ERR %08lX\n", variants[i].name, (unsigned long)r->error);
    } else {
        printf("%-16s %5.2f %5.2f %5lums\n", variants[i].name, r->avg_mbps, r->min1s_mbps,
               (unsigned long)(r->max_us / 1000));
    }
}

static void print_results(int upto)
{
    consoleSelect(&top);
    consoleClear();
    printf("%s %s cl=%luK run %s\n", cfg.model, cfg.sd_label, (unsigned long)(cfg.cluster_bytes / KB), cfg.run_id);
    printf("%-16s %5s %5s %7s\n", "variant", "avg", "min1s", "max");
    int first = upto > 26 ? upto - 26 : 0;
    for (int i = first; i < upto; i++) {
        print_result_line(i);
    }
}

static void print_progress(void)
{
    int i = current_variant;
    consoleSelect(&bottom);
    consoleClear();
    printf("Running %d / %d\n", i + 1, variant_count);
    printf("%s\n\n", variants[i].name);
    printf("%3llu%%  %llu / %llu MB\n", current_bytes * 100 / cfg.total, current_bytes / MB, cfg.total / MB);
    printf("\nB: abort\n");
}

static bool run_all(void)
{
    if (cfg.free_bytes < cfg.total + FREE_MARGIN) {
        consoleSelect(&bottom);
        printf("\nNot enough free space.\nNeed %llu MB.\n", (cfg.total + FREE_MARGIN) / MB);
        return false;
    }

    time_t now = time(NULL);
    strftime(cfg.run_id, sizeof(cfg.run_id), "%Y%m%d_%H%M%S", localtime(&now));
    snprintf(cfg.summary_path, sizeof(cfg.summary_path), "sdmc:" BENCH_DIR "/sdbench_%s_%s_%s.csv",
             cfg.model, cfg.sd_label, cfg.run_id);
    memset(results, 0, sizeof(results));
    abort_requested = false;
    worker_finished = false;
    current_variant = 0;
    current_bytes = 0;

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = threadCreate(worker, NULL, 32 * KB, prio - 1, -2, false);
    if (!t) {
        consoleSelect(&bottom);
        printf("\nthreadCreate failed\n");
        return false;
    }

    int shown = 0;
    u32 frame = 0;
    while (!worker_finished) {
        hidScanInput();
        if (hidKeysDown() & KEY_B) {
            abort_requested = true;
        }
        if (++frame % 15 == 0) {
            int done = current_variant;
            if (done != shown) {
                print_results(done);
                shown = done;
            }
            print_progress();
        }
        gspWaitForVBlank();
    }
    threadJoin(t, U64_MAX);
    threadFree(t);

    print_results(variant_count);
    consoleSelect(&bottom);
    consoleClear();
    printf("%s\n\nSummary:\n%s\n\nA: run again  START: exit\n",
           abort_requested ? "Aborted." : "Done.", cfg.summary_path + 5);
    return true;
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, &top);
    consoleInit(GFX_BOTTOM, &bottom);

    bool is_new = false;
    APT_CheckNew3DS(&is_new);
    snprintf(cfg.model, sizeof(cfg.model), "%s", is_new ? "N3DS" : "O3DS");
    cfg.total = SIZE_FULL;
    int sd_idx = 1;
    snprintf(cfg.sd_label, sizeof(cfg.sd_label), "%s", SD_LABELS[sd_idx]);

    build_matrix();
    data_base = memalign(0x1000, MAX_BLOCK + 0x1000);
    Result rc = FSUSER_OpenArchive(&cfg.sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (!data_base || R_FAILED(rc)) {
        consoleSelect(&bottom);
        printf("Init failed: mem=%p fs=%08lX\nSTART: exit\n", data_base, (unsigned long)rc);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) {
                break;
            }
            gspWaitForVBlank();
        }
        gfxExit();
        return 0;
    }
    fill_pattern(data_base, MAX_BLOCK + 0x1000);
    ensure_dirs();
    read_sd_info();

    bool in_menu = true;
    print_menu(sd_idx, is_new);
    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        if (down & KEY_START) {
            break;
        }
        if (down & KEY_A) {
            read_sd_info();
            run_all();
            read_sd_info();
            in_menu = false;
            continue;
        }
        if (down & (KEY_L | KEY_R | KEY_X | KEY_Y)) {
            if (down & KEY_L) {
                sd_idx = (sd_idx + 3) % 4;
            }
            if (down & KEY_R) {
                sd_idx = (sd_idx + 1) % 4;
            }
            if (down & KEY_X) {
                cfg.total = cfg.total == SIZE_FULL ? SIZE_QUICK : SIZE_FULL;
            }
            if ((down & KEY_Y) && is_new) {
                cfg.speedup = !cfg.speedup;
                osSetSpeedupEnable(cfg.speedup);
            }
            snprintf(cfg.sd_label, sizeof(cfg.sd_label), "%s", SD_LABELS[sd_idx]);
            in_menu = true;
        }
        if (in_menu && down) {
            print_menu(sd_idx, is_new);
        }
        gspWaitForVBlank();
    }

    FSUSER_CloseArchive(cfg.sdmc);
    free(data_base);
    gfxExit();
    return 0;
}
