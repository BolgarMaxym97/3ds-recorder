/*
 * recprobe — phase S spike (plan 3.5): can a sysmodule record the whole console?
 *
 * Launched once by spike/launcher (PMAPP_LaunchTitle), loaded by Luma from
 * /luma/sysmodules/0004013000F1D302.cxi. Runs for RUN_SECONDS, then writes its
 * logs and exits. No UI: the notification LED shows the state.
 *   - 15 s phases, alternating: detect only (baseline fps) / detect + record.
 *   - Source of frames: LCD registers (PDC) through our own GPU IO mapping.
 *   - Frame owner: VRAM -> our VRAM mapping; FCRAM -> the running application
 *     (TitleID 00040000...), DMA from its linear mapping (see StartFrameDma).
 *   - Encode XR32 (codec/), ring buffer, writer thread: 256 KB blocks into a
 *     file allocated ahead of time. Same stream format as the bench (R3DY),
 *     so tools/r3dtest.py decodes it.
 *   - L+R+Down stops early.
 * Output: sdmc:/3ds/recorder/spike/ (rec.r3dtest, sec.csv, cap.csv, info.txt).
 */
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <string.h>

#include "codec.h"
#include "convert.h"
#include "csvc.h"
#include "recprobe_diag.h"

/* ------------------------------------------------------------------ diagnostics */

/*
 * Progress page, read by the launcher from this process' memory
 * (svcMapProcessMemoryEx): shows where start-up stops even if neither the SD
 * nor the LED work. Its own page; address exported by the Makefile (nm).
 */
volatile RecprobeDiag g_diag __attribute__((aligned(0x1000), used)) = { RECPROBE_DIAG_MAGIC, 0, 0, { 0 }, 0, 0, 0, 0, 0, 0, 0 };

#define STAGE_INIT RECPROBE_STAGE_INIT
#define STAGE_SYSCALLS RECPROBE_STAGE_SYSCALLS
#define STAGE_HEAP RECPROBE_STAGE_HEAP
#define STAGE_SRV RECPROBE_STAGE_SRV
#define STAGE_FS RECPROBE_STAGE_FS
#define STAGE_SDMC RECPROBE_STAGE_SDMC
#define STAGE_LIBC RECPROBE_STAGE_LIBC
#define STAGE_MAIN RECPROBE_STAGE_MAIN
#define STAGE_DIRS RECPROBE_STAGE_DIRS
#define STAGE_INFO RECPROBE_STAGE_INFO
#define STAGE_LED RECPROBE_STAGE_LED
#define STAGE_SETUP RECPROBE_STAGE_SETUP
#define STAGE_FILE RECPROBE_STAGE_FILE
#define STAGE_THREADS RECPROBE_STAGE_THREADS
#define STAGE_POLL RECPROBE_STAGE_POLL
#define STAGE_DRAIN RECPROBE_STAGE_DRAIN
#define STAGE_DONE RECPROBE_STAGE_DONE
#define STAGE_FAIL RECPROBE_STAGE_FAIL
#define STAGE(x) (g_diag.stage = (x))

/* ------------------------------------------------------------------ config */

#define KB 1024u
#define MB (1024u * 1024u)
#define RUN_SECONDS      330u
#define PHASE_SECONDS    15u      /* even phases: detect only, odd: record */
#define POLL_US          2000u
#define CAPTURE_GAP_US   25000u   /* 30 fps cap per screen */
#define STATIC_GAP_US    100000u  /* screen not swapping: still capture at 10 fps */
#define DMA_TIMEOUT_US   100000u
#define KEYFRAME_EVERY   30u
#define THROTTLE_PCT     70u
#define BLOCK            (256u * KB)
#define FIRST_ALLOC      (128ull * MB)
#define CHUNK_ALLOC      (32ull * MB)
#define MAX_SLOTS        4u
#define CAP_LOG_MAX      12000u
#define SEC_LOG_MAX      (RUN_SECONDS + 16u)
/* Explicit device: in a sysmodule the default stdio device is not sdmc:
   (v0.2 wrote nothing because "/3ds/..." went nowhere). */
#define OUT_DIR          "sdmc:/3ds/recorder/spike"
#define OUT_DIR_FS       "/3ds/recorder/spike"     /* FSUSER paths (archive-relative) */

/* IO / memory VAs from our exheader mappings */
#define PDC_VA(screen)   (0x1EF00400u + 0x100u * (screen))   /* PA 0x10400400 / 0x10400500 */
#define PAD_VA           0x1EC46000u                         /* PA 0x10146000, active low */
#define VRAM_VA          0x1F000000u
#define VRAM_PA          0x18000000u
#define VRAM_SIZE        0x00600000u
#define BIG_VA           0x0C000000u                         /* our bump allocator area */

#define REG32(a) (*(volatile u32 *)(a))

#define MAGIC_FILE   0x42443352u   /* "R3DB" */
#define MAGIC_PAD    0x50443352u   /* "R3DP" */
#define MAGIC_YFRAME 0x59443352u   /* "R3DY" */

/* ------------------------------------------------------------------ heap / init */

#define HEAP_SIZE (256u * KB)

extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
void __sync_init(void);
void __sync_fini(void);
void __system_initSyscalls(void);
void __libc_init_array(void);
void __libc_fini_array(void);

static Result g_heapRc, g_srvRc, g_fsRc, g_sdmcRc, g_mcuRc = -1, g_mcuStealRc = -1;

void __system_allocateHeaps(void)
{
    u32 out = 0;
    __ctru_heap = 0x08000000;
    __ctru_heap_size = HEAP_SIZE;
    __ctru_linear_heap = 0;
    __ctru_linear_heap_size = 0;
    /* SYSTEM region straight from the kernel (bypasses the resource limit). */
    g_heapRc = svcControlMemoryUnsafe(&out, __ctru_heap, HEAP_SIZE,
                                      (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM), (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + HEAP_SIZE;
}

void __appInit(void)
{
    for (g_srvRc = 0xD88007FA; g_srvRc == (Result)0xD88007FA; svcSleepThread(500 * 1000LL))
    {
        g_diag.srvTries++;
        g_srvRc = srvInit();
    }
    g_diag.rc[RECPROBE_RC_SRV] = g_srvRc;
    STAGE(STAGE_SRV);
    /* v0.1 hung here forever: GetServiceHandle blocks while a service port is full
       (most likely mcu::HWC). Non-blocking: fail with an error instead, like Rosalina. */
    srvSetBlockingPolicy(true);
    g_fsRc = fsInit();
    g_diag.rc[RECPROBE_RC_FS] = g_fsRc;
    STAGE(STAGE_FS);
    g_sdmcRc = archiveMountSdmc();
    g_diag.rc[RECPROBE_RC_SDMC] = g_sdmcRc;
    STAGE(STAGE_SDMC);
}

/* LED service: own session if the port has room, else borrow an existing client
   session through Luma (same trick the launcher uses for pm:app). Called after
   the log files exist, so it can never block the start again. */
static void InitLed(void)
{
    g_mcuRc = mcuHwcInit();
    g_diag.rc[RECPROBE_RC_MCU] = g_mcuRc;
    if (R_SUCCEEDED(g_mcuRc))
        return;
    g_mcuStealRc = svcControlService(SERVICEOP_STEAL_CLIENT_SESSION, mcuHwcGetSessionHandle(), "mcu::HWC");
    g_diag.rc[RECPROBE_RC_MCU_STEAL] = g_mcuStealRc;
    if (R_SUCCEEDED(g_mcuStealRc))
        g_mcuRc = 0;
}

void __appExit(void)
{
    archiveUnmountAll();
    fsExit();
    srvExit();
}

void initSystem(void (*retAddr)(void))
{
    (void)retAddr;
    STAGE(STAGE_INIT);
    __sync_init();
    __system_initSyscalls();
    STAGE(STAGE_SYSCALLS);
    __system_allocateHeaps();
    g_diag.rc[RECPROBE_RC_HEAP] = g_heapRc;
    STAGE(STAGE_HEAP);
    __appInit();
    __libc_init_array();
    STAGE(STAGE_LIBC);
}

void __ctru_exit(int rc)
{
    (void)rc;
    __libc_fini_array();
    __appExit();
    __sync_fini();
    svcExitProcess();
}

/* ------------------------------------------------------------------ helpers */

static u64 Ticks(void) { return svcGetSystemTick(); }
static u32 TicksToUs(u64 t) { return (u32)(t / (SYSCLOCK_ARM11 / 1000000u)); }

static void InitLed(void);

static void Led(u8 r, u8 g, u8 b)
{
    if (R_FAILED(g_mcuRc))
        return;
    InfoLedPattern p;
    memset(&p, 0, sizeof(p));
    p.delay = 0x10;
    p.loopDelay = 0;
    memset(p.redPattern, r, sizeof(p.redPattern));
    memset(p.greenPattern, g, sizeof(p.greenPattern));
    memset(p.bluePattern, b, sizeof(p.bluePattern));
    g_diag.rc[RECPROBE_RC_LED] = MCUHWC_SetInfoLedPattern(&p);
}

/* Bump allocator over SYSTEM memory (svcControlMemoryUnsafe), page granular. */
static u32 g_bigTop = BIG_VA;

static void *BigAlloc(u32 size)
{
    size = (size + 0xFFFu) & ~0xFFFu;
    u32 out = 0;
    if (R_FAILED(svcControlMemoryUnsafe(&out, g_bigTop, size, (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM),
                                        (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        return NULL;
    g_bigTop += size;
    return (void *)out;
}

#define KEEP_FREE (4u * MB)   /* never leave the system with less SYSTEM memory than this */

/* How much SYSTEM memory can we take right now? 256 KB chunks, freed again;
   stops KEEP_FREE short of the region's free size. */
static u32 ProbeMaxAlloc(void)
{
    const u32 chunk = 256u * KB;
    const u32 sysFree = osGetMemRegionFree(MEMREGION_SYSTEM);
    u32 maxChunks = sysFree > KEEP_FREE ? (sysFree - KEEP_FREE) / chunk : 0;
    if (maxChunks > 64)
        maxChunks = 64;
    u32 n = 0, out = 0;
    for (; n < maxChunks; n++)
        if (R_FAILED(svcControlMemoryUnsafe(&out, g_bigTop + n * chunk, chunk, (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM),
                                            (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
            break;
    for (u32 i = 0; i < n; i++)
        svcControlMemoryUnsafe(&out, g_bigTop + i * chunk, chunk, MEMOP_FREE, (MemPerm)0);
    return n * chunk;
}

/* ------------------------------------------------------------------ state */

typedef struct {
    u8 *buf;
    u32 cap;
    volatile u32 state;   /* 0 free, 1 dma pending */
    Handle dma;
    u8 screen, isStatic;
    u32 fmt, stride, size, pa, tMs, pid;
    u64 startTick;
} Slot;

typedef struct {
    u32 tMs;
    u8 screen, fmt, owner, flags;   /* owner: 0 VRAM, 1 FCRAM process; flags: 1 static, 2 key, 4 dma timeout */
    u16 pid, ringKb;
    u32 encUs, dmaUs, len;
} CapLog;

typedef struct {
    u16 det[2], cap[2], busy[2], dmaFail[2], noOwner[2];
    u16 botSkip, throttle, staticCaps, ownerLookups;
    u16 maxPollGapMs, ringMaxKb;
    u8 phase, fmtTop, fmtBot, ownerTop;
    u32 paTop, paBot, pidTop;
} SecLog;


static Slot   g_slots[2 * MAX_SLOTS];
static u8    *g_arena[2];
static u32    g_arenaCap[2] = { 768u * KB, 480u * KB };
static u32    g_slotCount[2], g_slotSize[2];
static u16   *g_prev[2];
static u8    *g_scratch;
static u32    g_codecFrames[2], g_codecWords[2];

static u8    *g_ring;
static u32    g_ringSize;
static volatile u64 g_w, g_r;
static volatile bool g_stopWriter, g_stopConv;
static Handle g_file;
static u64    g_written, g_allocated;
static Result g_fileRc, g_writeRc;
static u32    g_writeMaxUs;

static CapLog *g_cap;
static u32    g_capN;
static SecLog *g_sec;
static u32    g_secN;
static LightLock g_logLock;


static u32    g_memFree[3], g_probeMax, g_allocFail;
static u64    g_startTick;
static u32    g_stopReason;   /* 0 time, 1 hotkey, 2 setup error */
static u32    g_ownPid;

/* ------------------------------------------------------------------ ring */

static u32 RingFill(void) { return (u32)(g_w - g_r); }

/* Single producer (converter). Returns false if there is no room. */
static bool RingPut(const void *hdr, u32 hdrLen, const void *data, u32 len)
{
    const u32 stored = (len + 15u) & ~15u;
    const u32 need = hdrLen + stored;
    u32 pos = (u32)(g_w % g_ringSize);
    u32 tail = g_ringSize - pos;
    const u32 total = tail < need ? tail + need : need;
    if (g_ringSize - RingFill() < total)
        return false;
    if (tail < need)
    {
        u32 pad[4] = { MAGIC_PAD, tail - 16u, 0, 0x000000FFu };
        memcpy(g_ring + pos, pad, 16);
        pos = 0;
    }
    memcpy(g_ring + pos, hdr, hdrLen);
    memcpy(g_ring + pos + hdrLen, data, len);
    memset(g_ring + pos + hdrLen + len, 0, stored - len);
    __atomic_store_n(&g_w, g_w + total, __ATOMIC_RELEASE);
    return true;
}

static void WriterMain(void *arg)
{
    (void)arg;
    for (;;)
    {
        const u64 w = __atomic_load_n(&g_w, __ATOMIC_ACQUIRE);
        const u64 avail = w - g_r;
        const bool stop = g_stopWriter;
        if (avail >= BLOCK || (stop && avail))
        {
            const u32 n = avail >= BLOCK ? BLOCK : (u32)avail;
            if (g_file && R_SUCCEEDED(g_writeRc))
            {
                if (g_written + n > g_allocated)
                {
                    g_allocated += CHUNK_ALLOC;
                    FSFILE_SetSize(g_file, g_allocated);
                }
                u32 done = 0;
                const u64 t0 = Ticks();
                g_writeRc = FSFILE_Write(g_file, &done, g_written, g_ring + (g_r % g_ringSize), n, FS_WRITE_FLUSH);
                const u32 us = TicksToUs(Ticks() - t0);
                if (us > g_writeMaxUs)
                    g_writeMaxUs = us;
                g_written += done;
            }
            __atomic_store_n(&g_r, g_r + n, __ATOMIC_RELEASE);
            continue;
        }
        if (stop)
            break;
        svcSleepThread(4 * 1000 * 1000);
    }
}

/* ------------------------------------------------------------------ trace */

/* Rare events, written to SD immediately (append + close): if the module dies,
   the last line shows the last risky step. */
static u32 g_traceLines;

static void Trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Trace(const char *fmt, ...)
{
    if (g_traceLines >= 400)
        return;
    FILE *f = fopen(OUT_DIR "/trace.txt", "a");
    if (!f)
        return;
    g_traceLines++;
    fprintf(f, "%6lu ", g_startTick ? (u32)(TicksToUs(Ticks() - g_startTick) / 1000u) : 0u);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ------------------------------------------------------------------ owners */

/*
 * v0.4 crashed (crash_dump_00000018) on the first FCRAM framebuffer, in the
 * owner search that walked every process with GET_PA_FROM_VA. v0.5 follows the
 * proven ChirunoMod approach instead: VRAM framebuffers (HOME, applets, some
 * games) through our own VRAM mapping; FCRAM framebuffers only from the running
 * application (TitleID 00040000...), at its linear mapping 0x30000000 + offset,
 * falling back to 0x14000000 + offset; the kernel validates the address when
 * the DMA starts. No cross-process address translation.
 */
/*
 * Only the pid is kept, never a process handle: v0.5 held the app's handle, so
 * after the game was closed its process (and 64 MB of APPLICATION memory) could
 * not be destroyed and the next game failed to load (loader out of memory,
 * crash_dump_00000019). The handle is opened per DMA and closed right after
 * the DMA has started (the kernel keeps its own reference), as ChirunoMod does.
 */
static u32 g_appPid, g_appBase;   /* base: 0 = unknown, else 0x30000000 / 0x14000000 */

static void DropApp(void)
{
    g_appPid = 0;
    g_appBase = 0;
}

static bool FindApp(u16 *lookups)
{
    (*lookups)++;
    DropApp();
    u32 pids[64];
    s32 n = 0;
    if (R_FAILED(svcGetProcessList(&n, pids, 64)))
        return false;
    for (s32 i = n - 1; i >= 0; i--)
    {
        if (pids[i] == g_ownPid)
            continue;
        Handle h = 0;
        if (R_FAILED(svcOpenProcess(&h, pids[i])))
            continue;
        s64 tid = 0;
        const bool isApp = R_SUCCEEDED(svcGetProcessInfo(&tid, h, 0x10001)) && ((u64)tid >> 32) == 0x00040000ull;
        svcCloseHandle(h);
        if (isApp)
        {
            g_appPid = pids[i];
            Trace("app found: pid %lu tid %016llX", g_appPid, (u64)tid);
            return true;
        }
    }
    Trace("app not found (%ld processes)", n);
    return false;
}

/* Starts the DMA of the framebuffer at `pa`; fills *pid (0 = our VRAM mapping). */
static Result StartFrameDma(Handle *dma, u8 *dst, u32 pa, u32 size, const DmaConfig *cfg, u32 *pid, u16 *lookups)
{
    if (pa >= VRAM_PA && pa + size <= VRAM_PA + VRAM_SIZE)
    {
        *pid = 0;
        return svcStartInterProcessDma(dma, CUR_PROCESS_HANDLE, (u32)dst, CUR_PROCESS_HANDLE,
                                       VRAM_VA + (pa - VRAM_PA), size, cfg);
    }
    if (pa < 0x20000000u || pa >= 0x30000000u)
        return (Result)0xDEAD0020;
    if (!g_appPid && !FindApp(lookups))
        return (Result)0xDEAD0021;
    Handle app = 0;
    if (R_FAILED(svcOpenProcess(&app, g_appPid)))
    {
        Trace("app %lu gone", g_appPid);
        DropApp();
        if (!FindApp(lookups) || R_FAILED(svcOpenProcess(&app, g_appPid)))
            return (Result)0xDEAD0021;
    }
    const u32 off = pa - 0x20000000u;
    static const u32 bases[2] = { 0x30000000u, 0x14000000u };
    Result rc = (Result)0xDEAD0022;
    for (u32 i = 0; i < 2; i++)
    {
        const u32 base = g_appBase ? g_appBase : bases[i];
        rc = svcStartInterProcessDma(dma, CUR_PROCESS_HANDLE, (u32)dst, app, base + off, size, cfg);
        if (R_SUCCEEDED(rc))
        {
            if (!g_appBase)
                Trace("app %lu: DMA ok via base %08lX (pa %08lX)", g_appPid, base, pa);
            g_appBase = base;
            *pid = g_appPid;
            break;
        }
        Trace("app %lu: DMA base %08lX pa %08lX failed %08lX", g_appPid, base, pa, (u32)rc);
        if (g_appBase)
            break;   /* a known-good base failed: the app is gone or changed */
    }
    svcCloseHandle(app);   /* never keep the app alive */
    if (R_FAILED(rc))
        DropApp();
    return rc;
}

/* ------------------------------------------------------------------ slots */

static u32 Bpp(u32 fmt)
{
    switch (fmt & 7u) { case 0: return 4; case 1: return 3; default: return 2; }
}

static bool EnsureSlots(int sc, u32 size)
{
    if (g_slotCount[sc] && size <= g_slotSize[sc])
        return true;
    for (u32 i = 0; i < g_slotCount[sc]; i++)
        if (g_slots[sc * MAX_SLOTS + i].state)
            return false;
    const u32 slotSize = (size + 63u) & ~63u;
    u32 n = slotSize ? g_arenaCap[sc] / slotSize : 0;
    if (n > MAX_SLOTS)
        n = MAX_SLOTS;
    if (!n)
        return false;
    for (u32 i = 0; i < MAX_SLOTS; i++)
    {
        Slot *s = &g_slots[sc * MAX_SLOTS + i];
        s->buf = i < n ? g_arena[sc] + i * slotSize : NULL;
        s->cap = i < n ? slotSize : 0;
    }
    g_slotSize[sc] = slotSize;
    g_slotCount[sc] = n;
    return true;
}

static u32 BusySlots(int sc)
{
    u32 n = 0;
    for (u32 i = 0; i < g_slotCount[sc]; i++)
        n += g_slots[sc * MAX_SLOTS + i].state != 0;
    return n;
}

/* ------------------------------------------------------------------ converter */

static void LogCap(const Slot *s, u8 owner, u8 flags, u32 encUs, u32 dmaUs, u32 len)
{
    LightLock_Lock(&g_logLock);
    if (g_capN < CAP_LOG_MAX)
    {
        CapLog *c = &g_cap[g_capN++];
        c->tMs = s->tMs;
        c->screen = s->screen;
        c->fmt = (u8)s->fmt;
        c->owner = owner;
        c->flags = flags | (s->isStatic ? 1 : 0);
        c->pid = (u16)s->pid;
        c->ringKb = (u16)(RingFill() / KB);
        c->encUs = encUs;
        c->dmaUs = dmaUs;
        c->len = len;
    }
    LightLock_Unlock(&g_logLock);
}

static void Encode(Slot *s)
{
    const int sc = s->screen;
    const u32 cols = sc == 0 ? 400 : 320, rows = 240, words = cols * rows;
    const bool key = g_codecWords[sc] != words || g_codecFrames[sc] % KEYFRAME_EVERY == 0;
    const u64 t0 = Ticks();
    const u8 *src = s->buf;
    u32 stride = s->stride;
    if ((s->fmt & 7u) != FB_RGB565)
    {
        if (fb_to_rgb565((u16 *)s->buf, s->buf, s->fmt, cols, rows, s->stride, 1) != words * 2)
        {
            LogCap(s, s->pid ? 1 : 0, 0, 0, 0, 0);
            return;
        }
        stride = rows * 2;
    }
    const u32 len = (u32)codec_xr32_encode_strided_v(g_prev[sc], src, cols, rows, stride, 1, key, 1, g_scratch);
    const u32 encUs = TicksToUs(Ticks() - t0);
    g_codecWords[sc] = words;
    g_codecFrames[sc]++;

    u32 hdr[4];
    hdr[0] = MAGIC_YFRAME;
    hdr[1] = len;
    hdr[2] = s->tMs * 1000u;
    hdr[3] = (u32)(sc | (key ? 0x40 : 0)) | ((s->fmt & 0xFFu) << 8) | (cols << 16);
    if (!RingPut(hdr, 16, g_scratch, len))
        g_codecWords[sc] = 0;   /* lost: next frame must be a keyframe */
    LogCap(s, s->pid ? 1 : 0, key ? 2 : 0, encUs, TicksToUs(Ticks() - s->startTick), len);
}

static void ConverterMain(void *arg)
{
    (void)arg;
    while (!g_stopConv)
    {
        Slot *next = NULL;
        for (u32 i = 0; i < 2 * MAX_SLOTS; i++)
        {
            Slot *s = &g_slots[i];
            if (s->state == 1 && (!next || s->screen < next->screen ||
                                  (s->screen == next->screen && s->startTick < next->startTick)))
                next = s;
        }
        if (!next)
        {
            svcSleepThread(1000 * 1000);
            continue;
        }
        DmaState st = DMASTATE_STARTING;
        const Result rc = svcGetDmaState(&st, next->dma);
        if (R_SUCCEEDED(rc) && st == DMASTATE_DONE)
        {
            svcCloseHandle(next->dma);
            next->dma = 0;
            svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)next->buf, next->size);
            Encode(next);
            __atomic_store_n(&next->state, 0, __ATOMIC_RELEASE);
            continue;
        }
        if (R_FAILED(rc) || TicksToUs(Ticks() - next->startTick) > DMA_TIMEOUT_US)
        {
            svcStopDma(next->dma);
            svcCloseHandle(next->dma);
            next->dma = 0;
            g_codecWords[next->screen] = 0;
            LogCap(next, next->pid ? 1 : 0, 4, 0, 0, 0);
            __atomic_store_n(&next->state, 0, __ATOMIC_RELEASE);
            continue;
        }
        svcSleepThread(300 * 1000);
    }
}

/* ------------------------------------------------------------------ poller */

typedef struct { u32 lastAddr, lastSel; u64 lastNewUs, lastCapUs; bool hasCap; } ScreenTrack;

static void StartCapture(int sc, u32 pa, u32 fmt, u32 stride, u32 nowMs, bool isStatic, SecLog *sec)
{
    const u32 cols = sc == 0 ? 400 : 320;
    const u32 size = cols * stride;
    if (!stride || stride < 240 * Bpp(fmt) || size > g_arenaCap[sc])
        return;
    if (!EnsureSlots(sc, size))
    {
        sec->busy[sc]++;
        return;
    }
    Slot *slot = NULL;
    for (u32 i = 0; i < g_slotCount[sc] && !slot; i++)
        if (!g_slots[sc * MAX_SLOTS + i].state)
            slot = &g_slots[sc * MAX_SLOTS + i];
    if (!slot)
    {
        sec->busy[sc]++;
        g_codecWords[sc] = 0;
        return;
    }
    DmaConfig cfg;
    dmaConfigInitDefault(&cfg);
    cfg.flags = 0;
    Handle h = 0;
    u32 pid = 0;
    const Result rc = StartFrameDma(&h, slot->buf, pa, size, &cfg, &pid, &sec->ownerLookups);
    if (R_FAILED(rc))
    {
        if (rc == (Result)0xDEAD0021 || rc == (Result)0xDEAD0020)
            sec->noOwner[sc]++;
        else
            sec->dmaFail[sc]++;
        return;
    }
    slot->dma = h;
    slot->screen = (u8)sc;
    slot->isStatic = isStatic;
    slot->fmt = fmt;
    slot->stride = stride;
    slot->size = size;
    slot->pa = pa;
    slot->pid = pid;
    slot->tMs = nowMs;
    slot->startTick = Ticks();
    __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);
    sec->cap[sc]++;
    if (isStatic)
        sec->staticCaps++;
    if (sc == 0)
    {
        sec->ownerTop = pid ? 1 : 0;
        sec->pidTop = pid;
    }
}

static void Poll(void)
{
    ScreenTrack tr[2];
    memset(tr, 0, sizeof(tr));
    bool throttleFlip = false, comboHeld = true;   /* ignore a combo still held at start */
    u64 lastPollUs = 0;
    u32 curSec = 0xFFFFFFFFu;
    SecLog *sec = NULL;
    u32 lastPhase = 0xFF;

    for (;;)
    {
        const u64 nowUs = TicksToUs(Ticks() - g_startTick);
        const u32 nowMs = (u32)(nowUs / 1000u);
        const u32 s = (u32)(nowUs / 1000000u);
        if (s >= RUN_SECONDS)
            break;
        if (s != curSec)
        {
            curSec = s;
            g_diag.seconds = s;
            g_diag.captures = g_capN;
            g_diag.writtenKb = (u32)(g_written / KB);
            if (g_secN < SEC_LOG_MAX)
            {
                sec = &g_sec[g_secN++];
                memset(sec, 0, sizeof(*sec));
            }
        }
        const u32 phase = (s / PHASE_SECONDS) & 1u;
        sec->phase = (u8)phase;
        g_diag.phase = phase;
        if (phase != lastPhase)
        {
            lastPhase = phase;
            Trace("phase %s", phase ? "REC" : "detect");
            if (phase)
                Led(0xFF, 0, 0);       /* red: recording */
            else
                Led(0, 0x30, 0);       /* dim green: detect only */
        }
        if (lastPollUs)
        {
            const u32 gapMs = (u32)((nowUs - lastPollUs) / 1000u);
            if (gapMs > sec->maxPollGapMs)
                sec->maxPollGapMs = (u16)(gapMs > 0xFFFF ? 0xFFFF : gapMs);
        }
        lastPollUs = nowUs;

        const u32 pad = ~REG32(PAD_VA) & 0xFFFu;
        const u32 combo = KEY_L | KEY_R | KEY_DDOWN;
        if ((pad & combo) == combo)
        {
            if (!comboHeld)
            {
                g_stopReason = 1;
                break;
            }
        }
        else
            comboHeld = false;

        const u32 fill = RingFill();
        if (fill / KB > sec->ringMaxKb)
            sec->ringMaxKb = (u16)(fill / KB);

        for (int sc = 0; sc < 2; sc++)
        {
            const u32 base = PDC_VA(sc);
            const u32 a = REG32(base + 0x68), b = REG32(base + 0x6C);
            const u32 fmt = REG32(base + 0x70);
            const u32 sel = REG32(base + 0x78) & 1u;
            const u32 stride = REG32(base + 0x90);
            const u32 cur = sel ? b : a;
            ScreenTrack *t = &tr[sc];
            const bool isNew = cur != t->lastAddr || sel != t->lastSel;
            if (isNew)
            {
                sec->det[sc]++;
                t->lastAddr = cur;
                t->lastSel = sel;
                t->lastNewUs = nowUs;
            }
            if (sc == 0) { sec->paTop = cur; sec->fmtTop = (u8)fmt; }
            else         { sec->paBot = cur; sec->fmtBot = (u8)fmt; }
            {
                static u32 lastFmt[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu }, lastStride[2], lastRegion[2] = { 9, 9 };
                const u32 region = cur >= VRAM_PA && cur < VRAM_PA + VRAM_SIZE ? 0 : (cur >= 0x20000000u && cur < 0x30000000u ? 1 : 2);
                if (fmt != lastFmt[sc] || stride != lastStride[sc] || region != lastRegion[sc])
                {
                    Trace("screen %d: pa %08lX (%s) fmt %08lX stride %lu", sc, cur,
                          region == 0 ? "VRAM" : region == 1 ? "FCRAM" : "other", fmt, stride);
                    lastFmt[sc] = fmt;
                    lastStride[sc] = stride;
                    lastRegion[sc] = region;
                }
            }

            if (!phase || !g_ring)
                continue;
            const bool due = !t->hasCap || nowUs - t->lastCapUs >= CAPTURE_GAP_US;
            const bool isStatic = !isNew && (!t->hasCap || nowUs - t->lastCapUs >= STATIC_GAP_US) &&
                                  nowUs - t->lastNewUs >= STATIC_GAP_US;
            if (!(due && (isNew || isStatic)))
                continue;
            t->lastCapUs = nowUs;
            t->hasCap = true;

            if (fill > g_ringSize / 100u * THROTTLE_PCT)
            {
                if (sc == 0)
                    throttleFlip = !throttleFlip;
                if (sc == 1 || throttleFlip)
                {
                    if (sc == 0) sec->throttle++; else sec->botSkip++;
                    continue;
                }
            }
            if (sc == 1 && BusySlots(0) >= 2)
            {
                sec->botSkip++;
                continue;
            }
            StartCapture(sc, cur, fmt, stride, nowMs, isStatic, sec);
        }
        svcSleepThread(POLL_US * 1000ull);
    }
}

/* ------------------------------------------------------------------ output */

static void WriteLogs(void)
{
    FILE *f = fopen(OUT_DIR "/sec.csv", "w");
    if (f)
    {
        fprintf(f, "sec,phase,det_top,det_bot,cap_top,cap_bot,busy_top,busy_bot,dmafail_top,dmafail_bot,"
                   "noowner_top,noowner_bot,bot_skip,throttle,static_caps,owner_lookups,max_poll_gap_ms,ring_max_kb,"
                   "fmt_top,fmt_bot,pa_top,pa_bot,owner_top,pid_top\n");
        for (u32 i = 0; i < g_secN; i++)
        {
            const SecLog *s = &g_sec[i];
            fprintf(f, "%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,0x%X,0x%X,0x%08lX,0x%08lX,%u,%lu\n",
                    i, s->phase, s->det[0], s->det[1], s->cap[0], s->cap[1], s->busy[0], s->busy[1],
                    s->dmaFail[0], s->dmaFail[1], s->noOwner[0], s->noOwner[1], s->botSkip, s->throttle,
                    s->staticCaps, s->ownerLookups, s->maxPollGapMs, s->ringMaxKb, s->fmtTop, s->fmtBot,
                    s->paTop, s->paBot, s->ownerTop, s->pidTop);
        }
        fclose(f);
    }
    f = fopen(OUT_DIR "/cap.csv", "w");
    if (f)
    {
        fprintf(f, "t_ms,screen,fmt,owner,flags,pid,ring_kb,enc_us,dma_us,len\n");
        for (u32 i = 0; i < g_capN; i++)
        {
            const CapLog *c = &g_cap[i];
            fprintf(f, "%lu,%u,0x%X,%u,%u,%u,%u,%lu,%lu,%lu\n", c->tMs, c->screen, c->fmt, c->owner, c->flags,
                    c->pid, c->ringKb, c->encUs, c->dmaUs, c->len);
        }
        fclose(f);
    }
}

static void WriteInfo(const char *phase)
{
    FILE *f = fopen(OUT_DIR "/info.txt", "w");
    if (!f)
        return;
    s64 isN3ds = 0;
    fprintf(f, "recprobe spike v0.6, stage: %s\n", phase);
    fprintf(f, "model: %s\n", svcGetSystemInfo(&isN3ds, 0x10001, 0) == 0 ? "N3DS" : "O3DS");
    fprintf(f, "recprobe v0.2: init rc: heap 0x%08lX srv 0x%08lX fs 0x%08lX sdmc 0x%08lX mcu 0x%08lX (steal 0x%08lX)\n",
            (u32)g_heapRc, (u32)g_srvRc, (u32)g_fsRc, (u32)g_sdmcRc, (u32)g_mcuRc, (u32)g_mcuStealRc);
    fprintf(f, "free at start: application %lu KB, system %lu KB, base %lu KB\n",
            g_memFree[0] / KB, g_memFree[1] / KB, g_memFree[2] / KB);
    fprintf(f, "probe: max SYSTEM alloc %lu KB (256 KB chunks, freed)\n", g_probeMax / KB);
    fprintf(f, "allocated: ring %lu KB, arenas %lu + %lu KB, alloc failures %lu, big top 0x%08lX\n",
            g_ringSize / KB, g_arenaCap[0] / KB, g_arenaCap[1] / KB, g_allocFail, g_bigTop);
    fprintf(f, "file rc 0x%08lX, write rc 0x%08lX, written %llu bytes, write max %lu us\n",
            (u32)g_fileRc, (u32)g_writeRc, g_written, g_writeMaxUs);
    fprintf(f, "slots top %lu x %lu B, bottom %lu x %lu B\n", g_slotCount[0], g_slotSize[0], g_slotCount[1], g_slotSize[1]);
    fprintf(f, "captures logged %lu, seconds %lu, stop reason %lu (0 time, 1 hotkey, 2 setup)\n",
            g_capN, g_secN, g_stopReason);
    fprintf(f, "free now: application %lu KB, system %lu KB, base %lu KB\n",
            osGetMemRegionFree(MEMREGION_APPLICATION) / KB, osGetMemRegionFree(MEMREGION_SYSTEM) / KB,
            osGetMemRegionFree(MEMREGION_BASE) / KB);
    fclose(f);
}

/* ------------------------------------------------------------------ main */

static bool Setup(void)
{
    g_memFree[0] = osGetMemRegionFree(MEMREGION_APPLICATION);
    g_memFree[1] = osGetMemRegionFree(MEMREGION_SYSTEM);
    g_memFree[2] = osGetMemRegionFree(MEMREGION_BASE);
    g_probeMax = ProbeMaxAlloc();

    if (osGetMemRegionFree(MEMREGION_SYSTEM) < 2u * MB + KEEP_FREE)
        return false;   /* not even the fixed buffers fit with a safe margin */
    g_cap = BigAlloc(CAP_LOG_MAX * sizeof(CapLog));
    g_sec = BigAlloc(SEC_LOG_MAX * sizeof(SecLog));
    for (int sc = 0; sc < 2; sc++)
    {
        g_arena[sc] = BigAlloc(g_arenaCap[sc]);
        g_prev[sc] = BigAlloc((sc == 0 ? 400u : 320u) * 240u * 2u);
    }
    g_scratch = BigAlloc((u32)codec_xr32_max_size(400 * 240) + 64);
    if (!g_cap || !g_sec || !g_arena[0] || !g_arena[1] || !g_prev[0] || !g_prev[1] || !g_scratch)
    {
        g_allocFail++;
        return false;
    }
    static const u32 ringSizes[] = { 4u * MB, 3u * MB, 2u * MB, 1536u * KB, 1u * MB, 512u * KB };
    for (u32 i = 0; i < sizeof(ringSizes) / sizeof(ringSizes[0]) && !g_ring; i++)
    {
        if (osGetMemRegionFree(MEMREGION_SYSTEM) < ringSizes[i] + KEEP_FREE)
            continue;
        g_ring = BigAlloc(ringSizes[i]);
        if (g_ring)
            g_ringSize = ringSizes[i];
        else
            g_allocFail++;
    }
    return g_ring != NULL;
}

static void OpenRecording(void)
{
    FS_Archive sd;
    g_fileRc = FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if (R_FAILED(g_fileRc))
        return;
    const FS_Path p = fsMakePath(PATH_ASCII, OUT_DIR_FS "/rec.r3dtest");
    FSUSER_DeleteFile(sd, p);
    g_fileRc = FSUSER_OpenFile(&g_file, sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    FSUSER_CloseArchive(sd);
    if (R_FAILED(g_fileRc))
    {
        g_file = 0;
        return;
    }
    g_allocated = FIRST_ALLOC;
    FSFILE_SetSize(g_file, g_allocated);

    /* File header (32 bytes, as the bench): magic, version 2, tid 0, is_new, mode 4 = full_both. */
    u8 hdr[32];
    memset(hdr, 0, sizeof(hdr));
    const u32 magic = MAGIC_FILE, version = 2;
    memcpy(hdr, &magic, 4);
    memcpy(hdr + 4, &version, 4);
    s64 isN3ds = 0;
    hdr[16] = svcGetSystemInfo(&isN3ds, 0x10001, 0) == 0;
    hdr[17] = 4;
    memcpy(g_ring, hdr, 32);
    __atomic_store_n(&g_w, 32, __ATOMIC_RELEASE);
}

int main(void)
{
    STAGE(STAGE_MAIN);
    LightLock_Init(&g_logLock);
    svcGetProcessId(&g_ownPid, CUR_PROCESS_HANDLE);
    {
        FS_Archive sd;
        Result rc = FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
        if (R_SUCCEEDED(rc))
        {
            FSUSER_CreateDirectory(sd, fsMakePath(PATH_ASCII, "/3ds"), 0);
            FSUSER_CreateDirectory(sd, fsMakePath(PATH_ASCII, "/3ds/recorder"), 0);
            rc = FSUSER_CreateDirectory(sd, fsMakePath(PATH_ASCII, OUT_DIR_FS), 0);
            if (rc == (Result)0xC82044BE)   /* already exists */
                rc = 0;
            FSUSER_DeleteFile(sd, fsMakePath(PATH_ASCII, OUT_DIR_FS "/trace.txt"));
            FSUSER_CloseArchive(sd);
        }
        g_diag.rc[RECPROBE_RC_MKDIR] = rc;
    }
    STAGE(STAGE_DIRS);
    Trace("recprobe v0.6 start");
    WriteInfo("init");
    STAGE(STAGE_INFO);
    InitLed();
    STAGE(STAGE_LED);
    if (R_FAILED(g_fsRc) || R_FAILED(g_sdmcRc))
    {
        Led(0, 0, 0xFF);   /* blue: no SD access, nothing can be logged */
        svcSleepThread(60ull * 1000 * 1000 * 1000);
        Led(0, 0, 0);
        return 0;
    }
    Led(0x30, 0, 0x30);   /* magenta: starting */

    g_diag.sysFreeKb = osGetMemRegionFree(MEMREGION_SYSTEM) / KB;
    const bool setupOk = Setup();
    g_diag.probeMaxKb = g_probeMax / KB;
    g_diag.ringKb = g_ringSize / KB;
    g_diag.rc[RECPROBE_RC_SETUP] = setupOk ? 0 : -1;
    STAGE(STAGE_SETUP);
    if (!setupOk)
    {
        STAGE(STAGE_FAIL);
        g_stopReason = 2;
        WriteInfo("setup failed");
        Led(0, 0, 0);
        return 0;
    }
    OpenRecording();
    g_diag.rc[RECPROBE_RC_FILE] = g_fileRc;
    STAGE(STAGE_FILE);
    WriteInfo("running");

    Thread writer = threadCreate(WriterMain, NULL, 8 * KB, 0x30, 1, false);
    Thread conv = threadCreate(ConverterMain, NULL, 8 * KB, 0x30, 1, false);
    STAGE(STAGE_THREADS);
    if (!writer || !conv)
    {
        STAGE(STAGE_FAIL);
        g_stopReason = 2;
        WriteInfo("threadCreate failed");
        Led(0, 0, 0);
        return 0;
    }

    g_startTick = Ticks();
    STAGE(STAGE_POLL);
    Poll();
    STAGE(STAGE_DRAIN);

    /* Drain: let pending DMAs finish, then the converter, then the writer. */
    Led(0x30, 0x30, 0);   /* yellow: saving */
    svcSleepThread(200 * 1000 * 1000);
    g_stopConv = true;
    threadJoin(conv, 3ull * 1000 * 1000 * 1000);
    g_stopWriter = true;
    threadJoin(writer, 10ull * 1000 * 1000 * 1000);
    if (g_file)
    {
        FSFILE_SetSize(g_file, g_written);
        FSFILE_Close(g_file);
    }
    DropApp();

    WriteLogs();
    WriteInfo("done");
    Led(0, 0, 0);
    STAGE(STAGE_DONE);
    return 0;
}
