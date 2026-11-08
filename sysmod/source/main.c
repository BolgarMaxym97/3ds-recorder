/*
 * rec3ds — 3DS Recorder sysmodule (phase 1 MVP, plan 3.5 / roadmap 1).
 *
 * Lives from launch (by the app) until power-off and records the whole
 * console: games, HOME Menu, applets, switching between them.
 *   Idle:      polls the hotkey every 50 ms, holds ~0.3 MB.
 *   Recording: allocates ~6 MB of SYSTEM memory, polls the LCD registers every
 *              2 ms, DMAs new frames of both screens (30 fps cap), encodes XR32
 *              straight into a ring buffer, a writer thread streams 256 KB
 *              blocks into a file that was allocated ahead of time.
 *   Stop:      the file is trimmed and renamed to recordings/YYYYMMDD_HHMMSS.r3d;
 *              buffers are freed; the next file is prepared in the background.
 * Hotkey (default R+Down+Select; L+R opens the Camera in HOME Menu) toggles recording. Notification LED: red pulse
 * while recording, short green blink when a file is saved, 3 red blinks on an
 * error. The app talks to the module through the shared page (shared.h).
 *
 * Lessons from the spike (plan 3.5): makerom adds 32 to the rsf priority; never
 * keep an application's process handle (it blocks its teardown); FCRAM app
 * framebuffers are DMA'd from the app at its linear mapping; no DMA from
 * another process' IO mapping; the default stdio device is not sdmc:.
 */
#include <3ds.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "codec.h"
#include "convert.h"
#include "r3d_format.h"
#include "shared.h"

/* Luma custom SVCs (csvc.s) */
Result svcControlMemoryUnsafe(u32 *out, u32 addr0, u32 size, MemOp op, MemPerm perm);
Result svcControlService(u32 op, ...);
Result svcMapProcessMemoryEx(Handle dstProcess, u32 vaDst, Handle srcProcess, u32 vaSrc, u32 size, u32 flags);   /* Luma 0xA0 */
Result svcUnmapProcessMemoryEx(Handle process, u32 va, u32 size);                                              /* Luma 0xA1 */

/* ------------------------------------------------------------------ config */

#define KB 1024u
#define MB (1024u * 1024u)
#define DIR_FS         "/3ds/recorder"
#define REC_DIR_FS     "/3ds/recorder/recordings"
#define NEXT_FS        REC_DIR_FS "/.next.tmp"
#define INFO_FS        REC_DIR_FS "/.next.info"   /* a running recording: start time + a safe length */
#define INFO_MAGIC     0x49443352u                /* "R3DI" */
#define INFO_EVERY_US  5000000u
#define STOPPED_FS     DIR_FS "/.system_stop"     /* the recording the system's power-off / reboot stopped (the app tells) */
#define DIR_SD         "sdmc:/3ds/recorder"
#define CONFIG_SD      DIR_SD "/config.ini"
#define LOG_SD         DIR_SD "/recorder.log"
#define LOG_FS         DIR_FS "/recorder.log"
#define LOG_OLD_FS     DIR_FS "/recorder.old.log"   /* the log moves here past LOG_MAX at start-up */
#define LOG_MAX        (256u * 1024u)

#define POLL_REC_US    2000u
#define POLL_IDLE_US   50000u
#define STATIC_GAP_US  100000u        /* screen not swapping: still 10 fps */
#define DMA_TIMEOUT_US 100000u
#define KEYFRAME_EVERY 30u
/* Converter / writer priority (core 1). Low by default: at 0x30 the converter, busy in HOME,
   starved the system modules that launch apps, and HOME waited for the recording to stop
   before it could start the Homebrew Launcher or this recorder's app (O3DS, 2026-11-03/05; at
   0x3F both opened in ~5 s). Boosted to 0x30 only while the same application has been running
   for PRIO_BOOST_AFTER_US: in games the low priority lost frames (v8: 192, v10 at 0x30: 4). */
#define CONVERTER_PRIO       0x3F
#define CONVERTER_PRIO_BOOST 0x30
#define PRIO_BOOST_AFTER_US  8000000u
/* While boosted, the converter sleeps this long after each frame: HOME Menu runs on core 1 below
   it and handles the HOME button; never getting the core, it left a game unable to return to
   HOME until the recording stopped (Wario Land, O3DS 2026-11-07). */
#define BOOST_YIELD_US       2000u
/* No new application frame (FCRAM) on the top screen for this long: the application is not in
   front (HOME, an applet) or stopped drawing; the boost ends at once. */
#define BOOST_APP_IDLE_US    300000u
#define THROTTLE_PCT   70u
#define BLOCK          (256u * KB)
static u32 g_block = BLOCK;           /* this recording's write block (BLOCK_LITE in the light profile) */
#define FIRST_ALLOC    (128ull * MB)  /* file made at record start when none is prepared */
#define PREP_ALLOC     (1024ull * MB) /* prepared in idle time: growing a file on a nearly full card
                                         stalled writes ~1-1.5 s per 32 MB (test 2026-11-03) */
#define CHUNK_ALLOC    (32ull * MB)   /* growth step after the prepared size (~1-1.5 s stall: the 8 MB ring covers it) */
#ifndef REC_MAX_FILE_MB
#define REC_MAX_FILE_MB 3900u         /* FAT32 limit is 4 GB - 1; a test build sets less */
#endif
#define MAX_FILE       ((u64)g_cfg.maxFileMb * MB)   /* then the recording goes on in a new file */
#define MIN_SD_FREE_MB 64u
#define KEEP_FREE      (4u * MB)      /* never leave less SYSTEM memory than this */
#define KEEP_FREE_LITE (1u * MB)      /* the same in an extended-memory game (Old 3DS): no applets open there */
#define LITE_BELOW     (12u * MB)     /* SYSTEM free under this at start: the light profile (normally ~23 MB) */
/* Light profile ring: HOME Menu takes ~6 MB of the ~7.4 MB an extended-memory game leaves, as soon as
   0.15 s after the button (O3DS 2026-11-08). The capture buffers go back at once on HOME; the ring
   only after the writer's current block, so it must be small enough for HOME without it. */
#define LITE_RING_MAX  (1u * MB)
#define BLOCK_LITE     (128u * KB)    /* the writer's block there: the ring goes back sooner */
#define MAX_SLOTS      6u
#define BIG_VA         0x0C000000u
#define REG32(a)       (*(volatile u32 *)(a))
#define PDC_VA(screen) (0x1EF00400u + 0x100u * (screen))
#define PAD_VA         0x1EC46000u
#define VRAM_VA        0x1F000000u
#define VRAM_PA        0x18000000u
#define VRAM_SIZE      0x00600000u
/* DSP shared memory (plan 3.7): two 0x8000-byte regions the DSP and the game
   use as a double buffer; layout from azahar audio_core/hle/shared_memory.h */
#define DSP_VA          0x1FF00000u
#define DSP_REGION(i)   (DSP_VA + 0x50000u + 0x20000u * (i))
#define DSP_REGION_SIZE 0x8000u
#define DSP_FINAL_OFF   0xA80u        /* Final Mix Samples: what the speakers play */
#define DSP_COUNTER_OFF 0x7FFEu       /* frame counter, written by the game */
#define AUDIO_FRAME     160u          /* stereo samples per DSP frame (~4.9 ms) */
#define AUDIO_BYTES     (AUDIO_FRAME * 4u)
#define AUDIO_QUEUE     512u          /* frames between the audio thread and the converter (~2.5 s; raw mode ~0.3 s) */
#define AUDIO_QUEUE_LITE 256u
#define AUDIO_POLL_US   1000u         /* a region holds a frame ~9.8 ms; two equal reads needed */
#define AUDIO_TRAILER   16u           /* R3DA payload after the samples, see r3d_format.h */
#define AUDIO_RESERVE   (256u * KB)   /* ring space video frames leave for audio (~1.6 s) */
#define DSP_SNAP_US     30000000u     /* audio_debug only: full DSP region snapshot every 30 s (ring < 50%) */
#define PDN_DSP_CNT_VA  0x1EC41230u   /* bit 0: DSP out of reset, bit 1: DSP clock on */
/* DSP memory is only read while the DSP runs (not during app switches / sleep).
   It is mapped cached (MemoryMapping): without invalidating first, reads return
   stale 32-byte cache lines in random order (raw capture, 2026-11-03), which
   showed up as "partial" and out-of-order audio frames. Games do the same
   (DSP_InvalidateDataCache before reading the shared regions). */
#define DSP_RUNNING()   ((*(const volatile u8 *)PDN_DSP_CNT_VA & 3u) == 3u)

typedef struct {
    u32 hotkey;      /* KEY_* mask */
    u32 fps;         /* 30 / 20 / 15 */
    bool bottom;     /* record the bottom screen too */
    u32 led;         /* 0 off, 1 solid, 2 pulse */
    bool audio;      /* record the DSP output */
    bool audioDebug; /* diagnostics: no video, raw DSP output polls every 250 us */
    u32 maxFileMb;   /* a recording goes on in a new file past this (tests set less: max_file_mb) */
} Config;

static Config g_cfg = { KEY_R | KEY_SELECT, 30, true, 2, true, false, REC_MAX_FILE_MB };

/* ------------------------------------------------------------------ shared page */

volatile RecShared g_shared __attribute__((aligned(0x1000), used)) = {
    REC_SHARED_MAGIC, REC_SHARED_VERSION, REC_STATE_STARTING, REC_CMD_NONE,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, { 0 }, 0, { 0 },
};

/* ------------------------------------------------------------------ init */

#define HEAP_SIZE (256u * KB)
extern char *fake_heap_start, *fake_heap_end;
extern u32 __ctru_heap, __ctru_heap_size, __ctru_linear_heap, __ctru_linear_heap_size;
void __sync_init(void);
void __sync_fini(void);
void __system_initSyscalls(void);
void __libc_init_array(void);
void __libc_fini_array(void);

static Result g_mcuRc = -1;

void __system_allocateHeaps(void)
{
    u32 out = 0;
    __ctru_heap = 0x08000000;
    __ctru_heap_size = HEAP_SIZE;
    __ctru_linear_heap = 0;
    __ctru_linear_heap_size = 0;
    svcControlMemoryUnsafe(&out, __ctru_heap, HEAP_SIZE, (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM),
                           (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
    fake_heap_start = (char *)__ctru_heap;
    fake_heap_end = fake_heap_start + HEAP_SIZE;
}

void __appInit(void)
{
    Result rc;
    for (rc = 0xD88007FA; rc == (Result)0xD88007FA; svcSleepThread(500 * 1000LL))
        rc = srvInit();
    srvSetBlockingPolicy(true);   /* a full service port fails instead of blocking forever */
    fsInit();
    archiveMountSdmc();
}

void __appExit(void)
{
    mcuHwcExit();
    archiveUnmountAll();
    fsExit();
    srvExit();
}

void initSystem(void (*retAddr)(void))
{
    (void)retAddr;
    g_shared.initStage = 1;
    __sync_init();
    __system_initSyscalls();
    __system_allocateHeaps();
    g_shared.initStage = 2;
    __appInit();
    __libc_init_array();
    g_shared.initStage = 3;
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

/* The console clock (ms since 1900-01-01, local time) as a civil date and second of the day
   (Howard Hinnant's algorithm, epoch shifted). */
static void CivilTime(u64 msSince1900, u32 *y, u32 *m, u32 *d, u32 *sod)
{
    const u64 secs = msSince1900 / 1000u;
    s64 days = (s64)(secs / 86400u) - 25567;   /* -> days since 1970-01-01 */
    *sod = (u32)(secs % 86400u);
    days += 719468;
    const s64 era = (days >= 0 ? days : days - 146096) / 146097;
    const u32 doe = (u32)(days - era * 146097);
    const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const u32 mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (u32)(yoe + era * 400 + (*m <= 2));
}

static void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char *fmt, ...)
{
    static u32 lines;
    if (++lines > 2000)   /* a runaway loop must not fill the SD */
        return;
    FILE *f = fopen(LOG_SD, "a");
    if (!f)
        return;
    const u64 ms = osGetTime();
    u32 y, m, d, sod;
    CivilTime(ms, &y, &m, &d, &sod);
    fprintf(f, "[%04lu-%02lu-%02lu %02lu:%02lu:%02lu.%03lu] ", y, m, d, sod / 3600, sod / 60 % 60, sod % 60,
            (u32)(ms % 1000u));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static FS_Archive g_sd;

static void InitFs(void)
{
    if (R_FAILED(FSUSER_OpenArchive(&g_sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        g_sd = 0;
    if (!g_sd)
        return;
    FSUSER_CreateDirectory(g_sd, fsMakePath(PATH_ASCII, "/3ds"), 0);
    FSUSER_CreateDirectory(g_sd, fsMakePath(PATH_ASCII, DIR_FS), 0);
    FSUSER_CreateDirectory(g_sd, fsMakePath(PATH_ASCII, REC_DIR_FS), 0);
    /* a log that only grows would fill the card over months: the previous part is kept once */
    Handle f = 0;
    u64 size = 0;
    if (R_SUCCEEDED(FSUSER_OpenFile(&f, g_sd, fsMakePath(PATH_ASCII, LOG_FS), FS_OPEN_READ, 0))) {
        FSFILE_GetSize(f, &size);
        FSFILE_Close(f);
    }
    if (size > LOG_MAX) {
        FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, LOG_OLD_FS));
        FSUSER_RenameFile(g_sd, fsMakePath(PATH_ASCII, LOG_FS), g_sd, fsMakePath(PATH_ASCII, LOG_OLD_FS));
    }
}

static u32 SdFreeMb(void)
{
    FS_ArchiveResource r;
    if (R_FAILED(FSUSER_GetArchiveResource(&r, SYSTEM_MEDIATYPE_SD)))
        return 0xFFFFFFFFu;
    return (u32)((u64)r.freeClusters * r.clusterSize / MB);
}

/* ------------------------------------------------------------------ LED */

static void InitLed(void)
{
    g_mcuRc = mcuHwcInit();
    if (R_FAILED(g_mcuRc) && R_SUCCEEDED(svcControlService(0 /* steal */, mcuHwcGetSessionHandle(), "mcu::HWC")))
        g_mcuRc = 0;
}

static void LedPattern(const u8 *r, const u8 *g, const u8 *b, u8 delay, u8 smoothing, u8 loopDelay)
{
    if (R_FAILED(g_mcuRc))
        return;
    InfoLedPattern p;
    memset(&p, 0, sizeof(p));
    p.delay = delay;
    p.smoothing = smoothing;
    p.loopDelay = loopDelay;
    memcpy(p.redPattern, r, 32);
    memcpy(p.greenPattern, g, 32);
    memcpy(p.bluePattern, b, 32);
    MCUHWC_SetInfoLedPattern(&p);
}

static const u8 ZERO32[32];

/*
 * MCU pattern behaviour measured on O3DS (app LED test, 2026-11-03):
 *   - one-shot patterns (loop delay 0xFF) play far too fast to see, whatever
 *     the delay, so they are not used;
 *   - smoothing 0x20 with 1/16 s steps flattens a breathing pattern into a
 *     steady light; delay 8 + smoothing 0x40 gives the wanted slow breath.
 * So: breathing = looping pattern; every short signal = a steady looping
 * colour that the module itself switches after the step time (LedTick).
 */

static void LedSolid(u8 rr, u8 gg, u8 bb)
{
    u8 r[32], g[32], b[32];
    memset(r, rr, 32);
    memset(g, gg, 32);
    memset(b, bb, 32);
    LedPattern(r, g, b, 0x10, 0, 0);
}

static void LedOff(void) { LedSolid(0, 0, 0); }

typedef struct { u8 r, g, b; u16 ms; } LedStep;

static LedStep g_ledSeq[8];
static u32     g_ledSeqLen, g_ledSeqPos;
static u64     g_ledStepEnd;   /* osGetTime() ms */

static void LedStepStart(void)
{
    const LedStep *st = &g_ledSeq[g_ledSeqPos];
    LedSolid(st->r, st->g, st->b);
    g_ledStepEnd = osGetTime() + st->ms;
}

/* Plays a short sequence of steady colours, then turns the LED off. */
static void LedSequence(const LedStep *steps, u32 n)
{
    if (n > 8)
        n = 8;
    memcpy(g_ledSeq, steps, n * sizeof(*steps));
    g_ledSeqLen = n;
    g_ledSeqPos = 0;
    LedStepStart();
}

/* Called from the main loop (every 2-50 ms). */
static void LedTick(void)
{
    if (!g_ledSeqLen || osGetTime() < g_ledStepEnd)
        return;
    if (++g_ledSeqPos < g_ledSeqLen)
    {
        LedStepStart();
        return;
    }
    g_ledSeqLen = 0;
    LedOff();
}

/* Recording: slow red breathing like a camera's REC light (LED test variant 7). */
static void LedRecording(void)
{
    u8 r[32];
    g_ledSeqLen = 0;   /* cancels a running signal */
    if (g_cfg.led == 0)
    {
        LedOff();
        return;
    }
    if (g_cfg.led == 1)
    {
        LedSolid(0xFF, 0, 0);
        return;
    }
    for (u32 i = 0; i < 32; i++)
    {
        const u32 t = i <= 16 ? i : 32 - i;   /* 0..16..0 */
        r[i] = (u8)(8 + t * 247 / 16);
    }
    LedPattern(r, ZERO32, ZERO32, 0x08, 0x40, 0);
}

static const LedStep LED_READY[] = { { 0, 0, 0xFF, 1500 } };
static const LedStep LED_SAVED[] = { { 0, 0xFF, 0, 600 }, { 0, 0, 0, 300 }, { 0, 0xFF, 0, 600 } };
static const LedStep LED_ERROR[] = { { 0xFF, 0, 0, 300 }, { 0, 0, 0, 200 }, { 0xFF, 0, 0, 300 },
                                     { 0, 0, 0, 200 }, { 0xFF, 0, 0, 300 } };
#define LED_SIGNAL(x) LedSequence(x, sizeof(x) / sizeof(x[0]))

/* ------------------------------------------------------------------ config file */

static u32 KeyByName(const char *n)
{
    static const struct { const char *name; u32 key; } keys[] = {
        { "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R },
        { "START", KEY_START }, { "SELECT", KEY_SELECT }, { "UP", KEY_DUP }, { "DOWN", KEY_DDOWN },
        { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT },
    };
    for (u32 i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        if (strcasecmp(n, keys[i].name) == 0)
            return keys[i].key;
    return 0;
}

static void LoadConfig(void)
{
    FILE *f = fopen(CONFIG_SD, "r");
    if (!f)
    {
        f = fopen(CONFIG_SD, "w");
        if (f)
        {
            fprintf(f, "; 3DS Recorder settings (read when the recorder starts)\n"
                       "; hotkey: buttons joined by +  (A B X Y L R START SELECT UP DOWN LEFT RIGHT)\n"
                       "hotkey=R+SELECT\n"
                       "; fps: 30, 20 or 15\n"
                       "fps=30\n"
                       "; screens: both or top\n"
                       "screens=both\n"
                       "; led: pulse, solid or off\n"
                       "led=pulse\n"
                       "; audio: on or off\n"
                       "audio=on\n");
            fclose(f);
        }
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == ';' || *p == '#' || !*p)
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *v = eq + 1;
        v[strcspn(v, "\r\n ;#")] = 0;
        if (strcasecmp(p, "hotkey") == 0)
        {
            u32 mask = 0;
            for (char *tok = strtok(v, "+"); tok; tok = strtok(NULL, "+"))
                mask |= KeyByName(tok);
            if (mask)
                g_cfg.hotkey = mask;
        }
        else if (strcasecmp(p, "fps") == 0)
        {
            const u32 fps = (u32)atoi(v);
            if (fps == 30 || fps == 20 || fps == 15)
                g_cfg.fps = fps;
        }
        else if (strcasecmp(p, "screens") == 0)
            g_cfg.bottom = strcasecmp(v, "top") != 0;
        else if (strcasecmp(p, "led") == 0)
            g_cfg.led = strcasecmp(v, "off") == 0 ? 0 : strcasecmp(v, "solid") == 0 ? 1 : 2;
        else if (strcasecmp(p, "audio") == 0)
            g_cfg.audio = strcasecmp(v, "off") != 0;
        else if (strcasecmp(p, "audio_debug") == 0)
            g_cfg.audioDebug = strcasecmp(v, "on") == 0;
        else if (strcasecmp(p, "max_file_mb") == 0)   /* not in the app: a test of the 4 GB split */
        {
            const u32 mb = (u32)atoi(v);
            g_cfg.maxFileMb = mb >= 64 && mb <= REC_MAX_FILE_MB ? mb : REC_MAX_FILE_MB;
        }

    }
    fclose(f);
}

/* ------------------------------------------------------------------ big buffers */

/* Bump allocator over SYSTEM memory, freed as a whole when a recording ends. */
static u32 g_bigTop = BIG_VA;
static u32 g_keepFree = KEEP_FREE;
static bool g_lite;   /* this recording runs with the light memory profile */

/* The launcher plugin's memory (extended-memory games, Old 3DS): Rosalina takes it from the game's
   own region (sysmod WriteGameOrder: mode3 strategy), the recorder maps it here and keeps every
   recording buffer in it. SYSTEM memory then stays as the game left it: HOME Menu takes ~6 MB of
   the ~7.4 MB there, and with recording buffers in SYSTEM it crashed now and then even when they
   had all been given back before it asked (ErrDisp 0xD8C3FBF3, O3DS 2026-11-08). */
#define PLG_HDR_VA     0x0D000000u
#define PLG_MEM_VA     0x0E000000u
#define PLG_MAGIC      0x24584733u    /* Rosalina PluginHeader.magic, "3GX$" */
static u32 g_plgSize;                 /* bytes mapped at PLG_MEM_VA, 0: none */
static u32 g_plgTop;                  /* bump offset in it */
static bool g_inPlg;                  /* this recording's buffers live there */

static void *BigAlloc(u32 size)
{
    size = (size + 0xFFFu) & ~0xFFFu;
    if (g_inPlg)
    {
        if (g_plgTop + size > g_plgSize)
            return NULL;
        void *p = (void *)(PLG_MEM_VA + g_plgTop);
        g_plgTop += size;
        return p;
    }
    if (osGetMemRegionFree(MEMREGION_SYSTEM) < size + g_keepFree)
        return NULL;
    u32 out = 0;
    if (R_FAILED(svcControlMemoryUnsafe(&out, g_bigTop, size, (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM),
                                        (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        return NULL;
    g_bigTop += size;
    return (void *)out;
}

static u32 g_bigLow = BIG_VA;   /* below it already given back (ReleaseEncoderMemory) */

static void BigFreeAll(void)
{
    g_plgTop = 0;   /* the plugin's memory stays mapped until the game closes */
    u32 out = 0;
    if (g_bigTop > g_bigLow)
        svcControlMemoryUnsafe(&out, g_bigLow, g_bigTop - g_bigLow, MEMOP_FREE, (MemPerm)0);
    g_bigTop = g_bigLow = BIG_VA;
}

/* ------------------------------------------------------------------ recording state */

typedef struct {
    u8 *buf;
    u32 cap;
    volatile u32 state;   /* 0 free, 1 DMA pending */
    Handle dma;
    u8 screen, isStatic;
    u32 fmt, stride, size, timeUs;
    u64 startTick;
} Slot;

/* 6 frames per screen in BGR8 (3 bytes/pixel, what SM3DL and NSMB2 use): rides out converter
   stalls up to ~200 ms (v9: 0.13 s in HOME; with 768 / 480 KB only 2 BGR8 frames fitted) */
static const u32 ARENA_CAP[2] = { 6u * 400u * 240u * 3u, 6u * 320u * 240u * 3u };
/* Light profile: an extended-memory game on an Old 3DS leaves ~5.3 MB of SYSTEM memory (the
   launcher plugin's 2 MB included, O3DS 2026-11-08). Two frames per screen even at 4 bytes a pixel,
   a 2 MB ring, half the audio queue: ~3.7 MB. */
static const u32 ARENA_CAP_LITE[2] = { 2u * 400u * 240u * 4u, 2u * 320u * 240u * 4u };
static u32 g_arenaCap[2] = { 6u * 400u * 240u * 3u, 6u * 320u * 240u * 3u };
/* In the launcher plugin's 2 MB (an extended-memory game, Old 3DS): two frames per screen at 2 bytes
   a pixel (one at 3 or 4), a 256-frame audio queue, the ring gets the rest (~768 KB). The plugin
   cannot be bigger: Pokemon Ultra Moon did not start with 5 MB taken from it (75 MB left; 78 MB
   worked; Smash ran on 75), O3DS 2026-11-08. */
static const u32 ARENA_CAP_PLG[2] = { 2u * 400u * 240u * 2u, 2u * 320u * 240u * 2u };
#define AUDIO_QUEUE_PLG 256u   /* 128 overflowed in Smash (queue full 617 of 13792, O3DS 2026-11-08) */
static u32 g_aqCap = AUDIO_QUEUE;

static Slot   g_slots[2 * MAX_SLOTS];
static u8    *g_arena[2];
static u32    g_slotCount[2], g_slotSize[2];
static u16   *g_prev[2];
static u32    g_codecFrames[2];
static volatile u32 g_codecWords[2];

static u8    *g_ring;
static u32 g_audioReserveBytes = AUDIO_RESERVE;   /* half in the plugin's small ring (~1 s of audio) */
static volatile bool g_audioReserve;   /* audio on: video leaves AUDIO_RESERVE of ring free */
static u32    g_ringSize;
static volatile u64 g_w, g_r;
static volatile bool g_writerActive;   /* a file is open and the writer drains the ring */
static volatile bool g_writerBusy;     /* writer inside an iteration (set before it checks g_writerActive) */
static Handle g_file;
static u64    g_written, g_allocated;
static volatile Result g_writeRc;
static u64    g_recStartTick, g_recStartMs;
/* Stream offsets of recent video records (the converter writes, the main loop reads): a record
   start at or below what is on the card goes into INFO_FS, so a cut-off recording can be read
   from there on (records span the writer's blocks). */
#define BOUNDS 128u
static volatile u64 g_bound[BOUNDS];
static volatile u32 g_boundN;
static Handle g_infoFile;
static volatile bool g_recording;      /* capture on */
static u32    g_dropped, g_throttled, g_ringMax;

/* Lost video frames by reason (diagnostics, 2026-11-03: v7 dropped 170 frames
   in HOME with the ring nearly empty). DROP_RING .. DROP_DMA_ERROR are the ones
   counted in g_dropped; the last two were not counted before. */
enum { DROP_RING, DROP_NO_SLOT, DROP_DMA_TIMEOUT, DROP_DMA_ERROR, DROP_DMA_START, DROP_SLOTS_BUSY, DROP_KINDS };
static const char *const DROP_NAMES[DROP_KINDS] = { "ring full", "no free slot", "DMA timeout", "DMA error",
                                                    "DMA start failed", "slots busy (resize)" };
#define DROP_BURSTS   24u
#define DROP_JOIN_US  500000u   /* drops of one kind and screen this close are one burst */
typedef struct { u32 startUs, endUs, count; Result rc; u8 kind, screen; } DropBurst;
static u32       g_drops[DROP_KINDS];
static DropBurst g_dropBursts[DROP_BURSTS];
static u32       g_dropBurstCount, g_dropBurstsLost;
static Result    g_dmaStartRc;
static LightLock g_dropLock;
static u32       g_encodeMaxUs[2], g_encodeSlow[2];   /* converter time per frame; slow = over one 30 fps frame */
static u64       g_encodeSumUs[2];                    /* for the average (logged) */

static void Drop(int kind, int screen, Result rc)
{
    const u32 nowUs = TicksToUs(Ticks() - g_recStartTick);
    LightLock_Lock(&g_dropLock);
    g_drops[kind]++;
    if (kind <= DROP_DMA_ERROR)
        g_dropped++;
    DropBurst *b = NULL;
    for (u32 i = g_dropBurstCount; i-- > 0 && !b;)
        if (g_dropBursts[i].kind == kind && g_dropBursts[i].screen == screen && nowUs - g_dropBursts[i].endUs < DROP_JOIN_US)
            b = &g_dropBursts[i];
    if (!b && g_dropBurstCount < DROP_BURSTS)
    {
        b = &g_dropBursts[g_dropBurstCount++];
        b->startUs = nowUs;
        b->count = 0;
        b->kind = (u8)kind;
        b->screen = (u8)screen;
    }
    if (b)
    {
        b->endUs = nowUs;
        b->count++;
        b->rc = rc;
    }
    else
        g_dropBurstsLost++;
    LightLock_Unlock(&g_dropLock);
}
static LightEvent g_convEvent;

/* prepared file */
static Thread g_prepThread;
static volatile Handle g_prepFile;
static u64    g_prepSize;              /* size the prepared file should get */
static volatile u64 g_prepReached;     /* size it has so far */
static volatile bool g_prepStop;       /* a recording starts: stop growing it */
static u32    g_growCount, g_growMaxMs;
static u64    g_writeTicks;            /* time inside block writes (the card's real speed, logged) */
static u32    g_writeMaxMs;
static volatile bool g_prepDone;

/* application owning FCRAM framebuffers: pid only, never a handle */
static u32 g_appPid, g_appBase, g_ownPid;

/* ------------------------------------------------------------------ prepared file */

static void PrepMain(void *arg)
{
    (void)arg;
    const FS_Path p = fsMakePath(PATH_ASCII, NEXT_FS);
    FSUSER_DeleteFile(g_sd, p);
    Handle h = 0;
    g_prepReached = 0;
    if (R_SUCCEEDED(FSUSER_OpenFile(&h, g_sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
    {
        /* in 64 MB steps: a recording that starts meanwhile waits for one step only */
        while (g_prepReached < g_prepSize && !g_prepStop)
        {
            const u64 next = g_prepReached + 64ull * MB < g_prepSize ? g_prepReached + 64ull * MB : g_prepSize;
            if (R_FAILED(FSFILE_SetSize(h, next)))
                break;
            g_prepReached = next;
        }
        if (g_prepReached < FIRST_ALLOC / 2)
        {
            FSFILE_Close(h);
            FSUSER_DeleteFile(g_sd, p);
            h = 0;
        }
    }
    g_prepFile = h;
    g_prepDone = true;
}

static bool g_shutdownSeen;        /* the system is powering off / rebooting */

static void PrepareNext(void)
{
    if (g_prepThread || g_prepFile || !g_sd || g_shutdownSeen)
        return;
    const u32 freeMb = SdFreeMb();
    if (freeMb < (u32)(FIRST_ALLOC / MB) + 4 * MIN_SD_FREE_MB)
        return;   /* nearly full card: do not reserve space in advance */
    u64 size = ((u64)(freeMb - 4 * MIN_SD_FREE_MB) * MB) & ~(32ull * MB - 1);   /* leave 256 MB free */
    g_prepSize = size < PREP_ALLOC ? size : PREP_ALLOC;
    g_prepDone = false;
    g_prepStop = false;
    g_prepThread = threadCreate(PrepMain, NULL, 8 * KB, 0x3F, 1, false);
}

static Handle TakePrepared(void)
{
    if (g_prepThread)
    {
        g_prepStop = true;
        threadJoin(g_prepThread, U64_MAX);
        threadFree(g_prepThread);
        g_prepThread = NULL;
    }
    const Handle h = g_prepFile;
    g_prepFile = 0;
    return h;
}

/* ------------------------------------------------------------------ writer */

static void WriteInfo(void);

static void WriterMain(void *arg)
{
    (void)arg;
    u64 lastInfo = 0;
    for (;;)
    {
        g_writerBusy = true;
        if (!g_writerActive)
        {
            g_writerBusy = false;
            svcSleepThread(20 * 1000 * 1000);
            continue;
        }
        const u64 w = __atomic_load_n(&g_w, __ATOMIC_ACQUIRE);
        const u64 avail = w - g_r;
        if (avail < g_block)
        {
            g_writerBusy = false;
            svcSleepThread(4 * 1000 * 1000);
            continue;
        }
        if (R_SUCCEEDED(g_writeRc))
        {
            if (g_written + g_block > g_allocated)
            {
                const u64 t0 = osGetTime();
                g_allocated += CHUNK_ALLOC;
                FSFILE_SetSize(g_file, g_allocated);
                const u32 ms = (u32)(osGetTime() - t0);
                g_growCount++;
                if (ms > g_growMaxMs)
                    g_growMaxMs = ms;
            }
            u32 done = 0;
            const u64 w0 = Ticks();
            g_writeRc = FSFILE_Write(g_file, &done, g_written, g_ring + (g_r % g_ringSize), g_block, FS_WRITE_FLUSH);
            g_written += done;
            const u64 now = Ticks();
            g_writeTicks += now - w0;
            const u32 wms = TicksToUs(now - w0) / 1000u;
            if (wms > g_writeMaxMs)
                g_writeMaxMs = wms;
            if (now - lastInfo > (u64)INFO_EVERY_US * (SYSCLOCK_ARM11 / 1000000u)) {
                lastInfo = now;
                WriteInfo();
            }
        }
        __atomic_store_n(&g_r, g_r + g_block, __ATOMIC_RELEASE);
        g_writerBusy = false;
    }
}

/* Final partial block, from the main thread once capture and converter are idle. */
static void WriterFlushTail(void)
{
    const u64 avail = g_w - g_r;
    if (avail && R_SUCCEEDED(g_writeRc))
    {
        u32 done = 0;
        g_writeRc = FSFILE_Write(g_file, &done, g_written, g_ring + (g_r % g_ringSize), (u32)avail, FS_WRITE_FLUSH);
        g_written += done;
    }
    g_r = g_w;
}

/* ------------------------------------------------------------------ converter */

static u32 RingFill(void) { return (u32)(g_w - g_r); }

/* Encodes one DMA'd frame straight into the ring (single producer). */
static void EncodeSlot(Slot *s)
{
    const int sc = s->screen;
    const u32 cols = sc == 0 ? 400 : 320, rows = 240, words = cols * rows;
    const u32 maxLen = ((u32)codec_xr32_max_size(words) + 15u) & ~15u;
    const u32 need = sizeof(R3dRecHeader) + maxLen;
    u32 pos = (u32)(g_w % g_ringSize);
    const u32 tail = g_ringSize - pos;
    const u32 padBytes = tail < need ? tail : 0;
    if (g_ringSize - RingFill() < padBytes + need + (g_audioReserve ? g_audioReserveBytes : 0u))
    {
        /* SD behind: video gives way, audio keeps AUDIO_RESERVE. No keyframe needed after
           a skipped frame: the encoder's reference (g_prev) is the last frame it stored. */
        Drop(DROP_RING, sc, 0);
        return;
    }
    const u8 *src = s->buf;
    u32 stride = s->stride;
    if ((s->fmt & 7u) != FB_RGB565)
    {
        /* BGR8 / RGBA8 / ...: pack to RGB565 in place (output never overtakes input) */
        if (fb_to_rgb565((u16 *)s->buf, s->buf, s->fmt, cols, rows, s->stride, 1) != words * 2)
            return;
        stride = rows * 2;
    }
    if (padBytes)
    {
        R3dRecHeader *pad = (R3dRecHeader *)(g_ring + pos);
        pad->magic = R3D_MAGIC_PAD;
        pad->len = padBytes - sizeof(R3dRecHeader);
        pad->timeUs = 0;
        pad->screen = 0xFF;
        pad->srcFormat = 0;
        pad->cols = 0;
        pos = 0;
    }
    const bool key = g_codecWords[sc] != words || g_codecFrames[sc] % KEYFRAME_EVERY == 0;
    u8 *payload = g_ring + pos + sizeof(R3dRecHeader);
    const u32 len = (u32)codec_xr32_encode_strided_v(g_prev[sc], src, cols, rows, stride, 1, key, 1, payload);
    const u32 stored = (len + 15u) & ~15u;
    memset(payload + len, 0, stored - len);
    R3dRecHeader *h = (R3dRecHeader *)(g_ring + pos);
    h->magic = R3D_MAGIC_YFRAME;
    h->len = len;
    h->timeUs = s->timeUs;
    h->screen = (u8)(sc | (key ? R3D_SCREEN_KEY : 0));
    h->srcFormat = (u8)s->fmt;
    h->cols = (u16)cols;
    g_codecWords[sc] = words;
    g_codecFrames[sc]++;
    g_bound[g_boundN % BOUNDS] = g_w + padBytes;
    __atomic_store_n(&g_boundN, g_boundN + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_w, g_w + padBytes + sizeof(R3dRecHeader) + stored, __ATOMIC_RELEASE);
    const u32 fill = RingFill();
    if (fill > g_ringMax)
        g_ringMax = fill;
    if (sc == 0)
        g_shared.framesTop++;
    else
        g_shared.framesBot++;
}

/* ------------------------------------------------------------------ audio */

typedef struct {
    u32 timeUs;
    u32 region;
    u16 counter[2];         /* both regions' frame counters when the frame was taken */
    u16 pollGapUs;          /* longest gap between audio polls since the previous frame */
    u16 flags;              /* R3D_AUDIO_* */
    u32 pcm[AUDIO_FRAME];   /* L/R s16 pairs */
} AudioItem;

static AudioItem   *volatile g_aq;         /* NULL: no audio in this recording */
static volatile bool g_convInAudio;        /* converter inside DrainAudio (stop handshake) */
static volatile u32 g_aqHead, g_aqTail;    /* main thread produces, converter consumes */
static u32          g_audioLast[2][AUDIO_FRAME];   /* last frame queued, per region */
static u32          g_audioCur[2][AUDIO_FRAME];
static u32          g_audioPrev[2][AUDIO_FRAME];   /* previous poll's read */
static bool         g_audioActive[2];              /* region being written (frame not complete yet) */
static u64          g_audioStartTick[2];
static u32          g_audioQueueFull;
static u32          g_audioFrames, g_audioDropped, g_dspSnaps, g_dspOffPolls;
static u32          g_audioUnstable, g_audioBoth, g_audioMaxGapUs, g_audioGapsOver4ms;
static u64          g_audioLastPollTick;
static u32          g_audioGapSinceFrame;
static volatile bool g_audioInPoll;        /* audio thread inside PollAudio (stop handshake) */
static u32          g_lastSnapUs;

static Result g_dspCacheRc;   /* first failure of the cache invalidation, for the log */

static void DspInvalidate(u32 va, u32 size)
{
    const Result rc = svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, va & ~31u, ((va & 31u) + size + 31u) & ~31u);
    if (R_FAILED(rc) && !g_dspCacheRc)
        g_dspCacheRc = rc;
}

/* Fresh copy of region r's final mix and both frame counters. */
static void DspReadFinal(u32 r, u32 *dst)
{
    DspInvalidate(DSP_REGION(r) + DSP_FINAL_OFF, AUDIO_BYTES);
    const volatile u32 *src = (const volatile u32 *)(DSP_REGION(r) + DSP_FINAL_OFF);
    for (u32 i = 0; i < AUDIO_FRAME; i++)
        dst[i] = src[i];
}

static void DspReadCounters(u16 cnt[2])
{
    for (u32 r = 0; r < 2; r++)
    {
        DspInvalidate(DSP_REGION(r) + DSP_COUNTER_OFF, 2);
        cnt[r] = *(const volatile u16 *)(DSP_REGION(r) + DSP_COUNTER_OFF);
    }
}

/* Reserves a record with `stored` payload bytes (multiple of 16), wrapping with a
   pad record; NULL when the ring is full. Converter thread only (single producer). */
static R3dRecHeader *RingReserve(u32 stored, u32 *advance)
{
    const u32 need = sizeof(R3dRecHeader) + stored;
    u32 pos = (u32)(g_w % g_ringSize);
    const u32 tail = g_ringSize - pos;
    const u32 padBytes = tail < need ? tail : 0;
    if (g_ringSize - RingFill() < padBytes + need)
        return NULL;
    if (padBytes)
    {
        R3dRecHeader *pad = (R3dRecHeader *)(g_ring + pos);
        pad->magic = R3D_MAGIC_PAD;
        pad->len = padBytes - sizeof(R3dRecHeader);
        pad->timeUs = 0;
        pad->screen = 0xFF;
        pad->srcFormat = 0;
        pad->cols = 0;
        pos = 0;
    }
    *advance = padBytes + need;
    return (R3dRecHeader *)(g_ring + pos);
}

static void RingCommit(u32 advance)
{
    __atomic_store_n(&g_w, g_w + advance, __ATOMIC_RELEASE);
    const u32 fill = RingFill();
    if (fill > g_ringMax)
        g_ringMax = fill;
}

/*
 * Audio thread, every ~1 ms. The DSP writes each ~4.9 ms output frame into one
 * of the two regions, alternating (sometimes the same one twice). A changed
 * block is taken once it reads the same on two polls in a row (a read during a
 * write is torn). In busy scenes the DSP writes a frame in parts with pauses
 * longer than a poll, so an unfinished version can be taken too; the finished
 * one follows from the same region and the converter drops the unfinished one
 * (tools/r3dconv/audio_order.h). The frame's time is when its block first
 * changed. A block equal to the last one taken from that region (silence,
 * paused DSP) is not stored. Tests: plan 3.7.
 */
static void PollAudio(void)
{
    if (!g_aq)
        return;
    const u64 tick = Ticks();
    if (g_audioLastPollTick)
    {
        const u32 gap = TicksToUs(tick - g_audioLastPollTick);
        if (gap > g_audioMaxGapUs)
            g_audioMaxGapUs = gap;
        if (gap > 4000)
            g_audioGapsOver4ms++;
        if (gap > g_audioGapSinceFrame)
            g_audioGapSinceFrame = gap;
    }
    g_audioLastPollTick = tick;
    if (!DSP_RUNNING())
    {
        g_dspOffPolls++;
        return;
    }
    bool done[2];
    for (u32 r = 0; r < 2; r++)
    {
        DspReadFinal(r, g_audioCur[r]);
        done[r] = false;
        if (memcmp(g_audioCur[r], g_audioLast[r], AUDIO_BYTES) == 0)
        {
            g_audioActive[r] = false;   /* back to the frame already taken */
            continue;
        }
        if (!g_audioActive[r])
        {
            g_audioActive[r] = true;
            g_audioStartTick[r] = tick;   /* first change: the frame's time */
        }
        else if (memcmp(g_audioCur[r], g_audioPrev[r], AUDIO_BYTES) == 0)
        {
            done[r] = true;   /* same on two polls */
            continue;
        }
        else
            g_audioUnstable++;   /* still being written */
        memcpy(g_audioPrev[r], g_audioCur[r], AUDIO_BYTES);
    }
    u16 cnt[2];
    DspReadCounters(cnt);
    const bool both = done[0] && done[1];
    if (both)
        g_audioBoth++;
    const u32 first = both && g_audioStartTick[1] < g_audioStartTick[0] ? 1 : 0;
    for (u32 k = 0; k < 2; k++)
    {
        const u32 r = k == 0 ? first : 1 - first;
        if (!done[r])
            continue;
        g_audioActive[r] = false;
        memcpy(g_audioLast[r], g_audioCur[r], AUDIO_BYTES);
        const u32 head = g_aqHead;
        if (head - __atomic_load_n(&g_aqTail, __ATOMIC_ACQUIRE) >= g_aqCap)
        {
            g_audioQueueFull++;
            continue;
        }
        AudioItem *it = &g_aq[head % g_aqCap];
        it->timeUs = TicksToUs(g_audioStartTick[r] - g_recStartTick);
        it->region = r;
        it->counter[0] = cnt[0];
        it->counter[1] = cnt[1];
        it->pollGapUs = (u16)(g_audioGapSinceFrame > 0xFFFF ? 0xFFFF : g_audioGapSinceFrame);
        it->flags = both ? R3D_AUDIO_BOTH_CHANGED : 0;
        g_audioGapSinceFrame = 0;
        memcpy(it->pcm, g_audioCur[r], AUDIO_BYTES);
        __atomic_store_n(&g_aqHead, head + 1, __ATOMIC_RELEASE);
    }
    if (g_aqHead - g_aqTail >= 16)
        LightEvent_Signal(&g_convEvent);
}

/* audio_debug=on: every poll where either region's block changed, both blocks as
   read (two R3DA records with R3D_AUDIO_RAW, same time), to study how the DSP
   writes them. */
static void PollAudioRaw(void)
{
    if (!g_aq || !DSP_RUNNING())
        return;
    const u64 tick = Ticks();
    bool changed = false;
    for (u32 r = 0; r < 2; r++)
    {
        DspReadFinal(r, g_audioCur[r]);
        changed |= memcmp(g_audioCur[r], g_audioPrev[r], AUDIO_BYTES) != 0;
    }
    if (!changed)
        return;
    u16 cnt[2];
    DspReadCounters(cnt);
    const u32 head = g_aqHead;
    if (head + 2 - __atomic_load_n(&g_aqTail, __ATOMIC_ACQUIRE) > g_aqCap)
    {
        g_audioQueueFull++;
        return;   /* g_audioPrev not updated: the change is seen again */
    }
    for (u32 r = 0; r < 2; r++)
    {
        memcpy(g_audioPrev[r], g_audioCur[r], AUDIO_BYTES);
        AudioItem *it = &g_aq[(head + r) % g_aqCap];
        it->timeUs = TicksToUs(tick - g_recStartTick);
        it->region = r;
        it->counter[0] = cnt[0];
        it->counter[1] = cnt[1];
        it->pollGapUs = 0;
        it->flags = R3D_AUDIO_RAW;
        memcpy(it->pcm, g_audioCur[r], AUDIO_BYTES);
    }
    __atomic_store_n(&g_aqHead, head + 2, __ATOMIC_RELEASE);
    if (g_aqHead - g_aqTail >= 32)
        LightEvent_Signal(&g_convEvent);
}

/* Audio thread: polls the DSP output every 1 ms while recording (the main loop's
   2 ms period stretches when it starts frame DMAs, and frames were missed). */
static void AudioMain(void *arg)
{
    (void)arg;
    for (;;)
    {
        g_audioInPoll = true;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        const bool active = g_recording && g_aq;
        if (active)
        {
            if (g_cfg.audioDebug)
                PollAudioRaw();
            else
                PollAudio();
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        g_audioInPoll = false;
        svcSleepThread(!active ? 20ull * 1000 * 1000 : g_cfg.audioDebug ? 250ull * 1000 : AUDIO_POLL_US * 1000ull);
    }
}

/* Converter thread: queued audio frames and the periodic DSP snapshot into the ring. */
static void DrainAudioImpl(void);

static void DrainAudio(void)
{
    g_convInAudio = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (g_aq)
        DrainAudioImpl();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_convInAudio = false;
}

static void DrainAudioImpl(void)
{
    while (g_aqTail != __atomic_load_n(&g_aqHead, __ATOMIC_ACQUIRE))
    {
        const AudioItem *it = &g_aq[g_aqTail % g_aqCap];
        u32 adv = 0;
        R3dRecHeader *h = RingReserve(AUDIO_BYTES + AUDIO_TRAILER, &adv);
        if (h)
        {
            memcpy(h + 1, it->pcm, AUDIO_BYTES);
            R3dAudioTrailer *tr = (R3dAudioTrailer *)((u8 *)(h + 1) + AUDIO_BYTES);
            memset(tr, 0, sizeof(*tr));
            tr->counter[0] = it->counter[0];
            tr->counter[1] = it->counter[1];
            tr->pollGapUs = it->pollGapUs;
            tr->flags = it->flags;
            h->magic = R3D_MAGIC_AUDIO;
            h->len = AUDIO_BYTES + AUDIO_TRAILER;
            h->timeUs = it->timeUs;
            h->screen = R3D_STREAM_AUDIO;
            h->srcFormat = (u8)it->region;
            h->cols = AUDIO_FRAME;
            RingCommit(adv);
            g_audioFrames++;
        }
        else
            g_audioDropped++;
        __atomic_store_n(&g_aqTail, g_aqTail + 1, __ATOMIC_RELEASE);
    }
    const u32 nowUs = TicksToUs(Ticks() - g_recStartTick);
    if (!g_cfg.audioDebug || !g_recording || (g_dspSnaps && nowUs - g_lastSnapUs < DSP_SNAP_US) || !DSP_RUNNING() ||
        RingFill() > g_ringSize / 2)
        return;   /* a 64 KB snapshot must not take the space audio needs */
    u32 adv = 0;
    R3dRecHeader *h = RingReserve(2 * DSP_REGION_SIZE, &adv);
    if (!h)
        return;
    u32 *dst = (u32 *)(h + 1);
    for (u32 r = 0; r < 2; r++)
    {
        DspInvalidate(DSP_REGION(r), DSP_REGION_SIZE);
        const volatile u32 *src = (const volatile u32 *)DSP_REGION(r);
        for (u32 i = 0; i < DSP_REGION_SIZE / 4; i++)
            *dst++ = src[i];
    }
    h->magic = R3D_MAGIC_DSPMEM;
    h->len = 2 * DSP_REGION_SIZE;
    h->timeUs = nowUs;
    h->screen = R3D_STREAM_DSPMEM;
    h->srcFormat = 0;
    h->cols = 0;
    RingCommit(adv);
    g_lastSnapUs = nowUs;
    g_dspSnaps++;
}

static volatile bool g_boosted;
static volatile u64 g_appSwapUs;   /* recording time of the last new FCRAM frame on the top screen */
static volatile bool g_appSwapSeen; /* g_appSwapUs is set (in this recording) */

static void ConverterMain(void *arg)
{
    (void)arg;
    for (;;)
    {
        DrainAudio();
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
            LightEvent_WaitTimeout(&g_convEvent, 100ull * 1000 * 1000);
            continue;
        }
        DmaState st = DMASTATE_STARTING;
        const Result rc = svcGetDmaState(&st, next->dma);
        if (R_SUCCEEDED(rc) && st == DMASTATE_DONE)
        {
            svcCloseHandle(next->dma);
            next->dma = 0;
            svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)next->buf, next->size);
            const u64 t0 = Ticks();
            EncodeSlot(next);
            const u32 us = TicksToUs(Ticks() - t0);
            if (us > g_encodeMaxUs[next->screen])
                g_encodeMaxUs[next->screen] = us;
            g_encodeSlow[next->screen] += us > 1000000u / 30u;
            g_encodeSumUs[next->screen] += us;
            __atomic_store_n(&next->state, 0, __ATOMIC_RELEASE);
            if (g_boosted)
                svcSleepThread(BOOST_YIELD_US * 1000ull);
            continue;
        }
        if (R_FAILED(rc) || TicksToUs(Ticks() - next->startTick) > DMA_TIMEOUT_US)
        {
            svcStopDma(next->dma);
            svcCloseHandle(next->dma);
            next->dma = 0;
            Drop(R_FAILED(rc) ? DROP_DMA_ERROR : DROP_DMA_TIMEOUT, next->screen, rc);   /* not encoded: no keyframe needed */
            __atomic_store_n(&next->state, 0, __ATOMIC_RELEASE);
            continue;
        }
        svcSleepThread(300 * 1000);
    }
}

/* ------------------------------------------------------------------ capture */

static bool SlotsIdle(void)
{
    for (u32 i = 0; i < 2 * MAX_SLOTS; i++)
        if (g_slots[i].state)
            return false;
    return true;
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

/* Programs that run in the APPLICATION memory region and draw into it: games (00040000), system
   applications such as System Settings, Camera, eShop, Mii Maker (00040010) and demos (00040002). */
static bool IsAppTitle(u64 tid)
{
    const u32 hi = (u32)(tid >> 32);
    return hi == 0x00040000u || hi == 0x00040010u || hi == 0x00040002u;
}

static bool FindApp(void)
{
    g_appPid = 0;
    g_appBase = 0;
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
        const bool isApp = R_SUCCEEDED(svcGetProcessInfo(&tid, h, 0x10001)) && IsAppTitle((u64)tid);
        svcCloseHandle(h);
        if (isApp)
        {
            g_appPid = pids[i];
            static u32 lastPid;
            if (g_recording && pids[i] != lastPid)   /* diagnostics: which process the FCRAM frames come from */
                Log("app for FCRAM frames: pid %lu title %016llX", pids[i], (u64)tid);
            lastPid = pids[i];
            return true;
        }
    }
    return false;
}

/* VRAM framebuffers (HOME, applets, some games): our own VRAM mapping. FCRAM
   framebuffers: the running application at its linear mapping. The process
   handle is opened per DMA and closed at once (never keep an app alive). */
/* Every page of [va, va + size) is mapped in `proc` and readable. A DMA from memory that is not
   (a framebuffer address that belongs to another process, a range running past the end of a
   buffer) is refused by the kernel at best (D04007F0, Wario Land) and took the kernel down with
   a panic at worst (O3DS 2026-11-07, rec3ds on core 1, a 288000-byte top-screen DMA). */
static bool RangeReadable(Handle proc, u32 va, u32 size)
{
    const u32 end = va + size;
    if (end < va)
        return false;
    for (u32 at = va, i = 0; at < end; i++)
    {
        MemInfo mi;
        PageInfo pi;
        if (i >= 8 || R_FAILED(svcQueryProcessMemory(&mi, &pi, proc, at)))
            return false;
        if (mi.state == MEMSTATE_FREE || mi.state == MEMSTATE_RESERVED || !(mi.perm & MEMPERM_READ) ||
            mi.base_addr + mi.size <= at)
            return false;
        at = mi.base_addr + mi.size;
    }
    return true;
}

static Result StartFrameDma(Handle *dma, u8 *dst, u32 pa, u32 size)
{
    DmaConfig cfg;
    dmaConfigInitDefault(&cfg);
    cfg.flags = 0;
    if (pa >= VRAM_PA && pa + size <= VRAM_PA + VRAM_SIZE)
        return svcStartInterProcessDma(dma, CUR_PROCESS_HANDLE, (u32)dst, CUR_PROCESS_HANDLE, VRAM_VA + (pa - VRAM_PA),
                                       size, &cfg);
    if (pa < 0x20000000u || pa >= 0x30000000u)
        return (Result)0xDEAD0020;
    /* only the application's own FCRAM (APPLICATION region: kernel config page APPMEMALLOC); an
       applet's or HOME Menu's framebuffer there is not the application's to read */
    const u32 appEnd = 0x20000000u + *(const volatile u32 *)0x1FF80040;
    if (pa + size > appEnd || pa + size < pa)
    {
        static u32 lastMb;
        if (g_recording && (pa >> 20) != lastMb)
            Log("FCRAM frame at pa %08lX size %lu outside the application's memory (ends %08lX): skipped", pa, size,
                appEnd);
        lastMb = pa >> 20;
        return (Result)0xDEAD0023;
    }
    static u64 backoffUntil;   /* after a refused DMA: the application's memory is left alone a moment */
    if (Ticks() < backoffUntil)
        return (Result)0xDEAD0024;
    if (!g_appPid && !FindApp())
    {
        static bool logged;
        if (g_recording && !logged)
            Log("FCRAM frame at pa %08lX but no application process found", pa);
        logged = true;
        return (Result)0xDEAD0021;
    }
    Handle app = 0;
    if (R_FAILED(svcOpenProcess(&app, g_appPid)) && (!FindApp() || R_FAILED(svcOpenProcess(&app, g_appPid))))
        return (Result)0xDEAD0021;
    static const u32 bases[2] = { 0x14000000u, 0x30000000u };
    Result rc = (Result)0xDEAD0022;
    for (u32 i = 0; i < 2; i++)
    {
        const u32 base = g_appBase ? g_appBase : bases[i];
        const u32 va = base + (pa - 0x20000000u);
        rc = RangeReadable(app, va, size)
                 ? svcStartInterProcessDma(dma, CUR_PROCESS_HANDLE, (u32)dst, app, va, size, &cfg)
                 : (Result)0xDEAD0025;   /* not mapped there: not this base, or not the app's buffer */
        if (R_SUCCEEDED(rc))
        {
            g_appBase = base;
            break;
        }
        if (g_appBase)
            break;
    }
    svcCloseHandle(app);
    if (rc == (Result)0xDEAD0025)
    {
        /* this buffer is not mapped where the application's frames are: skipped, nothing reset
           (Wario Land's bottom screen, O3DS 2026-11-07); logged once per MB of address */
        static u32 lastMb;
        if (g_recording && (pa >> 20) != lastMb)
            Log("FCRAM frame at pa %08lX size %lu not mapped in pid %lu at base %08lX: skipped", pa, size, g_appPid,
                g_appBase);
        lastMb = pa >> 20;
        return rc;
    }
    static u32 lastPid, lastBase;
    static Result lastRc;
    if (g_recording && (g_appPid != lastPid || g_appBase != lastBase || (R_FAILED(rc) && rc != lastRc)))
        Log("FCRAM frame DMA: pid %lu base %08lX pa %08lX size %lu rc %08lX", g_appPid, g_appBase, pa, size, (u32)rc);
    lastPid = g_appPid;
    lastBase = g_appBase;
    lastRc = rc;
    if (R_FAILED(rc))
    {
        g_appPid = g_appBase = 0;
        backoffUntil = Ticks() + SYSCLOCK_ARM11 / 2;   /* 0.5 s */
    }
    return rc;
}

static u32 Bpp(u32 fmt)
{
    switch (fmt & 7u) { case 0: return 4; case 1: return 3; default: return 2; }
}

static void Capture(int sc, u32 pa, u32 fmt, u32 stride, u32 timeUs, bool isStatic)
{
    /* the stride bounded first: a garbage register value times 400 could wrap to a small size */
    if (stride < 240 * Bpp(fmt) || stride > 240u * 4u * 2u)
        return;
    const u32 size = (sc == 0 ? 400u : 320u) * stride;
    if (size > g_arenaCap[sc])
        return;
    if (!EnsureSlots(sc, size))
    {
        Drop(DROP_SLOTS_BUSY, sc, 0);
        return;
    }
    Slot *slot = NULL;
    for (u32 i = 0; i < g_slotCount[sc] && !slot; i++)
        if (!g_slots[sc * MAX_SLOTS + i].state)
            slot = &g_slots[sc * MAX_SLOTS + i];
    if (!slot)
    {
        Drop(DROP_NO_SLOT, sc, 0);   /* not encoded: no keyframe needed */
        return;
    }
    Handle h = 0;
    const Result rc = StartFrameDma(&h, slot->buf, pa, size);
    if (R_FAILED(rc))
    {
        g_dmaStartRc = rc;
        Drop(DROP_DMA_START, sc, rc);
        return;
    }
    slot->dma = h;
    slot->screen = (u8)sc;
    slot->isStatic = isStatic;
    slot->fmt = fmt;
    slot->stride = stride;
    slot->size = size;
    slot->timeUs = timeUs;
    slot->startTick = Ticks();
    __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);
    LightEvent_Signal(&g_convEvent);
}

typedef struct { u32 lastAddr, lastSel, lastFmt, lastStride; u64 lastNewUs, lastCapUs; bool hasCap; } Track;
static Track g_track[2];

static bool IsVram(u32 pa) { return pa >= VRAM_PA && pa < VRAM_PA + VRAM_SIZE; }
static bool g_throttleFlip;

static void PollScreens(void)
{
    const u64 nowUs = TicksToUs(Ticks() - g_recStartTick);
    const u32 gapUs = 1000000u / g_cfg.fps - 8000u;   /* 30 fps -> 25 ms: tolerates 60 Hz jitter */
    const u32 fill = RingFill();
    for (int sc = 0; sc < (g_cfg.bottom ? 2 : 1); sc++)
    {
        const u32 base = PDC_VA(sc);
        const u32 a = REG32(base + 0x68), b = REG32(base + 0x6C);
        const u32 fmt = REG32(base + 0x70), sel = REG32(base + 0x78) & 1u, stride = REG32(base + 0x90);
        const u32 cur = sel ? b : a;
        Track *t = &g_track[sc];
        if (fmt != t->lastFmt || stride != t->lastStride)   /* a mode change (New 3DS 800 px, 3D, a program's own) */
        {
            t->lastFmt = fmt;
            t->lastStride = stride;
            Log("%s screen: format %03lX stride %lu at %s pa %08lX, %lu s", sc ? "bottom" : "top", fmt, stride,
                IsVram(cur) ? "VRAM" : "FCRAM", cur, (u32)(nowUs / 1000000u));
        }
        const bool isNew = cur != t->lastAddr || sel != t->lastSel;
        if (isNew)
        {
            t->lastAddr = cur;
            t->lastSel = sel;
            t->lastNewUs = nowUs;
            if (sc == 0 && !IsVram(cur))
            {
                g_appSwapUs = nowUs;
                g_appSwapSeen = true;
            }
        }
        const bool due = !t->hasCap || nowUs - t->lastCapUs >= gapUs;
        /* A screen that does not swap is re-captured every STATIC_GAP_US, but only from VRAM:
           re-reading an application's FCRAM framebuffer kept a closing app alive, and the next
           launch waited until the recording stopped (Homebrew Launcher -> 3DS Recorder, O3DS
           2026-11-03). A still FCRAM picture just stays the last frame in the video. */
        const bool isStatic = !isNew && IsVram(cur) && (!t->hasCap || nowUs - t->lastCapUs >= STATIC_GAP_US) &&
                              nowUs - t->lastNewUs >= STATIC_GAP_US;
        if (!(due && (isNew || isStatic)))
            continue;
        t->lastCapUs = nowUs;
        t->hasCap = true;
        if (fill > g_ringSize / 100u * THROTTLE_PCT)
        {
            /* SD behind: top at half rate, bottom paused (no random losses, no forced keyframes) */
            if (sc == 0)
                g_throttleFlip = !g_throttleFlip;
            if (sc == 1 || g_throttleFlip)
            {
                g_throttled++;
                continue;
            }
        }
        if (sc == 1 && BusySlots(0) >= 2)
            continue;   /* top screen first */
        Capture(sc, cur, fmt, stride, (u32)nowUs, isStatic);
    }
}

/* ------------------------------------------------------------------ start / stop */

static void FormatName(char *out, u32 size, u64 msSince1900)
{
    u32 y, m, d, sod;
    CivilTime(msSince1900, &y, &m, &d, &sod);
    snprintf(out, size, "%04lu%02lu%02lu_%02lu%02lu%02lu.r3d", y, m, d, sod / 3600, sod / 60 % 60, sod % 60);
}

/* Memory regions and the application memory mode (kernel config page, APPMEMTYPE at 0x1FF80030):
   an app that needs another mode (the Homebrew Launcher, New 3DS titles) waits for SYSTEM memory. */
static void LogMemory(const char *when)
{
    /* signed: with the launcher plugin's 2 MB taken from it, the game's region can show "below 0" */
    Log("memory (%s): app mode %u, APPLICATION free %ld KB, SYSTEM free %lu KB, BASE free %lu KB%s", when,
        *(const volatile u8 *)0x1FF80030, (s32)osGetMemRegionFree(MEMREGION_APPLICATION) / (s32)KB,
        osGetMemRegionFree(MEMREGION_SYSTEM) / KB, osGetMemRegionFree(MEMREGION_BASE) / KB,
        "");
}

/* ------------------------------------------------------------------ priority boost */

static Thread g_writerThread, g_convThread;

static void SetBoost(bool on, const char *why)
{
    if (on == g_boosted || !g_writerThread || !g_convThread)
        return;
    g_boosted = on;
    svcSetThreadPriority(threadGetHandle(g_convThread), on ? CONVERTER_PRIO_BOOST : CONVERTER_PRIO);
    svcSetThreadPriority(threadGetHandle(g_writerThread), on ? CONVERTER_PRIO_BOOST - 1 : CONVERTER_PRIO - 1);
    Log("priority %s (converter %02X): %s", on ? "boosted" : "normal", on ? CONVERTER_PRIO_BOOST : CONVERTER_PRIO, why);
}

/* The running application's pid, 0 when none (HOME only, or an app closing / starting). Does
   not touch g_appPid, which belongs to the frame DMA. */
static u32 ApplicationPid(void)
{
    u32 pids[64];
    s32 n = 0;
    if (R_FAILED(svcGetProcessList(&n, pids, 64)))
        return 0;
    for (s32 i = n - 1; i >= 0; i--)
    {
        if (pids[i] == g_ownPid)
            continue;
        Handle h = 0;
        if (R_FAILED(svcOpenProcess(&h, pids[i])))
            continue;
        s64 tid = 0;
        const bool isApp = R_SUCCEEDED(svcGetProcessInfo(&tid, h, 0x10001)) && IsAppTitle((u64)tid);
        svcCloseHandle(h);
        if (isApp)
            return pids[i];
    }
    return 0;
}

/* Twice a second while recording: boost once the same application has run PRIO_BOOST_AFTER_US,
   drop back as soon as it is gone or another one starts (launches happen at low priority). */
static const char *CpuState(void);

static void UpdateBoost(void)
{
    static u64 lastCheck, since;
    static u32 pid;
    const u64 now = Ticks();
    const u64 recUs = TicksToUs(now - g_recStartTick);
    const bool appDrawing = g_appSwapSeen && recUs - g_appSwapUs < BOOST_APP_IDLE_US;
    if (g_boosted && !appDrawing)
    {
        SetBoost(false, "the application stopped drawing (HOME, an applet, a pause)");
        return;
    }
    if (TicksToUs(now - lastCheck) < 500000u)
        return;
    lastCheck = now;
    const u32 p = ApplicationPid();
    if (p != pid)
    {
        /* which program is in front during the recording (drops and speed depend on it) */
        s64 tid = 0;
        Handle h = 0;
        if (p && R_SUCCEEDED(svcOpenProcess(&h, p)))
        {
            svcGetProcessInfo(&tid, h, 0x10001);
            svcCloseHandle(h);
        }
        if (p)
            Log("program in front: pid %lu title %016llX at %lu s (%s)", p, (u64)tid,
                TicksToUs(now - g_recStartTick) / 1000000u, CpuState());
        else
            Log("program in front: none (HOME) at %lu s", TicksToUs(now - g_recStartTick) / 1000000u);
        pid = p;
        since = now;
        SetBoost(false, "another application or none");
        return;
    }
    if (p && appDrawing && now - since >= (u64)PRIO_BOOST_AFTER_US * (SYSCLOCK_ARM11 / 1000000u)) /* 64-bit: apps run for hours */
        SetBoost(true, "an application has been running 8 s and draws");
}

/* ------------------------------------------------------------------ extended-memory games (Old 3DS) */

/* A game that wants 80 / 96 MB on an Old 3DS (Smash, Pokemon Sun / Moon, Monster Hunter...) makes
   NS reboot the console into that memory mode first: pm ends every process (0x100), the recorder
   too (O3DS 2026-11-08). Right before that reboot the recorder leaves Rosalina's plugin loader a
   one-time order, /luma/plugins/user_param.bin: load our launcher plugin (plugin/, 2 MB, no
   screen flash) into that game. Rosalina reads the order when it starts after the reboot; the
   plugin starts the recorder again from the game's first instruction, and the recorder deletes
   the order. Nothing happens unless the recorder runs when the game starts, and nothing changes for
   other games. The plugin loader must be on; if it is off the recorder turns it on for that one
   game (Rosalina saves its settings itself) and turns it off again afterwards.
   New 3DS: pm only changes the game's memory limit there, no reboot, nothing to do. */
#define LAUNCH_3GX_FS   DIR_FS "/rec3ds-launch.3gx"
#define USER_PARAM_FS   "/luma/plugins/user_param.bin"
#define PLG_BY_US_FS    DIR_FS "/.plugin_loader_on"   /* the recorder turned the plugin loader on */
#define USER_PARAM_MAGIC 0x53524550u                    /* "PERS", Rosalina plgloader.c */
#define ORDER_RESUME     0x52455331u                    /* config[0]: a recording was running ("RES1") */

/* Rosalina's PluginLoadParameters (sysmodules/rosalina/include/plugin/plgldr.h) */
typedef struct {
    u8 noFlash;
    u8 memoryStrategy;   /* 0 swap (to SD while HOME is open), 1 mode3, 2 none */
    u8 persistent;
    u8 pad;
    u32 lowTitleId;      /* 0: any title */
    char path[256];
    u32 config[32];
} PluginLoadParams;
_Static_assert(sizeof(PluginLoadParams) == 392, "PluginLoadParameters layout");

static bool g_powerOffSeen;        /* 0x108 came: a power-off, not a game's reboot */

static const char *MemModeName(u32 mode)
{
    switch (mode)
    {
    case 0: return "64 MB";
    case 2: return "96 MB";
    case 3: return "80 MB";
    case 4: return "72 MB";
    case 5: return "32 MB";
    case 6: return "New 3DS 124 MB";
    case 7: return "New 3DS 178 MB";
    default: return "?";
    }
}

static Handle StealSession(const char *name)
{
    Handle h = 0;
    return R_SUCCEEDED(svcControlService(0 /* steal client session */, &h, name)) ? h : 0;
}

/* Rosalina's plg:ldr: one session, which loader holds only while it loads a plugin; a stolen copy
   as the fallback (that failed on O3DS 2026-11-08: no named session). -1 unknown, 0 off, 1 on */
static int PluginLoader(int set)
{
    Handle h = 0;
    srvSetBlockingPolicy(true);   /* the port is full while loader holds its session: no waiting */
    const Result rc = srvGetServiceHandle(&h, "plg:ldr");
    srvSetBlockingPolicy(false);
    if (R_FAILED(rc))
        h = StealSession("plg:ldr");
    if (!h)
        Log("plg:ldr: no session (srv rc %08lX)", rc);
    if (!h)
        return -1;
    u32 *cmd = getThreadCommandBuffer();
    int state = -1;
    if (set < 0)
    {
        cmd[0] = IPC_MakeHeader(2, 0, 0);   /* IsEnabled */
        if (R_SUCCEEDED(svcSendSyncRequest(h)) && R_SUCCEEDED((Result)cmd[1]))
            state = cmd[2] ? 1 : 0;
    }
    else
    {
        cmd[0] = IPC_MakeHeader(3, 1, 0);   /* SetState: Rosalina also saves its settings */
        cmd[1] = (u32)set;
        if (R_SUCCEEDED(svcSendSyncRequest(h)) && R_SUCCEEDED((Result)cmd[1]))
            state = set;
    }
    svcCloseHandle(h);
    return state;
}

static bool SdFileExists(const char *path)
{
    Handle f = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, g_sd, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return false;
    FSFILE_Close(f);
    return true;
}

/* user_param.bin: 1 ours (*tidLow, *resume set), 0 none, -1 someone else's */
static int ReadUserParam(u32 *tidLow, bool *resume)
{
    Handle f = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, g_sd, fsMakePath(PATH_ASCII, USER_PARAM_FS), FS_OPEN_READ, 0)))
        return 0;
    struct { u32 magic; PluginLoadParams p; } buf;
    u32 got = 0;
    const bool ok = R_SUCCEEDED(FSFILE_Read(f, &got, 0, &buf, sizeof(buf))) && got == sizeof(buf) &&
                    buf.magic == USER_PARAM_MAGIC && strcmp(buf.p.path, LAUNCH_3GX_FS) == 0;
    FSFILE_Close(f);
    if (ok && tidLow)
        *tidLow = buf.p.lowTitleId;
    if (ok && resume)
        *resume = buf.p.config[0] == ORDER_RESUME;
    return ok ? 1 : -1;
}

/* At start: in which memory mode, whether the launcher plugin started us; removes our order.
   True: a recording ran when the game rebooted the console, so it goes on. */
static bool AfterReboot(void)
{
    const u32 mode = OS_KernelConfig->app_memtype;
    u32 tidLow = 0;
    bool resume = false;
    const int order = ReadUserParam(&tidLow, &resume);
    if (order > 0)
        FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, USER_PARAM_FS));
    const bool weTurnedOn = SdFileExists(PLG_BY_US_FS);
    if (weTurnedOn)
    {
        FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, PLG_BY_US_FS));
        PluginLoader(0);
    }
    Log("memory mode %lu (%s): APPLICATION %lu KB, SYSTEM free %lu KB of %lu KB, BASE free %lu KB; launcher order %s%s",
        mode, MemModeName(mode), OS_KernelConfig->memregion_sz[0] / KB, osGetMemRegionFree(MEMREGION_SYSTEM) / KB,
        OS_KernelConfig->memregion_sz[1] / KB, osGetMemRegionFree(MEMREGION_BASE) / KB,
        order > 0 ? "found (started by the plugin?), removed" : order < 0 ? "of another program, left" : "none",
        weTurnedOn ? "; plugin loader turned off again" : "");
    if (order > 0)
        Log("launcher order was for title %08lX%s", tidLow, resume ? ": the recording goes on in 2 s" : "");
    /* only right after that game's reboot (an old order must not start a recording in another one) */
    resume = resume && order > 0 && mode != 0;
    if (resume)   /* the recording that game's start stopped goes on: nothing to explain in the app */
        FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, STOPPED_FS));
    return resume;
}

/* At 0x100 (not a power-off), before anything else: if a game that reboots the console into its
   memory mode is about to start, leave Rosalina the order to load the launcher plugin into it.
   Quick and without log lines: pm kills the recorder ~1.5 s after 0x100, and once a recording had
   been saved first the order was never written (O3DS 2026-11-08). No exheader read either: an Old
   3DS reboots before an application only when its memory mode differs from the current one, so an
   application title in the reboot parameters is such a game. LogGameOrder() tells afterwards. */
static struct {
    const char *skip;   /* why no order, NULL: written */
    Result rc;
    u64 tid;
    u32 done;
} g_order;

static void WriteGameOrder(bool wasRecording)
{
    memset(&g_order, 0, sizeof(g_order));
    s64 n3ds = 0;
    svcGetSystemInfo(&n3ds, 0x10000, 0x201);
    if (n3ds)
    {
        g_order.skip = "New 3DS: no reboot for such games";
        return;
    }
    Handle pm = StealSession("pm:app");
    if (!pm)
    {
        g_order.skip = "no pm:app session";
        return;
    }
    static u8 params[0x1000];
    *pmAppGetSessionHandle() = pm;
    g_order.rc = PMAPP_GetFIRMLaunchParams(params, sizeof(params));
    *pmAppGetSessionHandle() = 0;
    svcCloseHandle(pm);
    /* the title NS starts after the reboot: +0x508 on O3DS 2026-11-08; the first application id */
    for (u32 o = 0x500; R_SUCCEEDED(g_order.rc) && o < 0x540 && !g_order.tid; o += 8)
    {
        u64 v;
        memcpy(&v, params + o, 8);
        if ((u32)(v >> 32) == 0x00040000u && (u32)v != 0)
            g_order.tid = v;
    }
    if (!g_order.tid)
    {
        g_order.skip = "not into a game";
        return;
    }
    if (!SdFileExists(LAUNCH_3GX_FS))
    {
        g_order.skip = "no launcher plugin " LAUNCH_3GX_FS;
        return;
    }
    if (ReadUserParam(NULL, NULL) < 0)
    {
        g_order.skip = USER_PARAM_FS " belongs to another program";
        return;
    }
    struct { u32 magic; PluginLoadParams p; } buf;
    memset(&buf, 0, sizeof(buf));
    buf.magic = USER_PARAM_MAGIC;
    buf.p.noFlash = 1;
    /* PLG_STRATEGY_MODE3: the plugin's 2 MB come from the game's own region, not from SYSTEM.
       With the default (swap: SYSTEM, written to SD while HOME is open) Rosalina could not take them
       back when the game was closed from HOME and asked for a reboot (0xD86007F3, O3DS 2026-11-08). */
    buf.p.memoryStrategy = 1;
    buf.p.persistent = 0;
    buf.p.lowTitleId = (u32)g_order.tid;
    buf.p.config[0] = wasRecording ? ORDER_RESUME : 0;
    snprintf(buf.p.path, sizeof(buf.p.path), "%s", LAUNCH_3GX_FS);
    FSUSER_CreateDirectory(g_sd, fsMakePath(PATH_ASCII, "/luma/plugins"), 0);
    const FS_Path p = fsMakePath(PATH_ASCII, USER_PARAM_FS);
    FSUSER_DeleteFile(g_sd, p);
    Handle f = 0;
    g_order.rc = FSUSER_OpenFile(&f, g_sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_SUCCEEDED(g_order.rc))
    {
        g_order.rc = FSFILE_Write(f, &g_order.done, 0, &buf, sizeof(buf), FS_WRITE_FLUSH);
        FSFILE_Close(f);
    }
    if (R_FAILED(g_order.rc) || g_order.done != sizeof(buf))
        g_order.skip = "writing failed";
}

/* After the recording is saved: the log line, and the plugin loader on if it is off. */
static void LogGameOrder(bool wasRecording)
{
    if (g_order.skip)
    {
        Log("reboot: no launcher order (%s; title %016llX, rc %08lX)", g_order.skip, g_order.tid, g_order.rc);
        return;
    }
    Log("reboot into game %016llX: launcher order written%s", g_order.tid,
        wasRecording ? ", the recording goes on there" : "");
    const int loader = PluginLoader(-1);
    if (loader == 0)
    {
        const int now = PluginLoader(1);
        if (now == 1)
        {
            Handle f = 0;
            if (R_SUCCEEDED(FSUSER_OpenFile(&f, g_sd, fsMakePath(PATH_ASCII, PLG_BY_US_FS), FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
                FSFILE_Close(f);
        }
        Log("reboot: plugin loader was off, turned on: %s", now == 1 ? "ok" : "failed");
    }
    else
        Log("reboot: plugin loader %s", loader == 1 ? "on" : "state unknown");
}

/* ------------------------------------------------------------------ cut-off recordings */

typedef struct {
    u32 magic, version;
    u64 startMs;         /* the file header's startTimeMs */
    u64 safe;            /* a record starts here and everything before it is on the card */
} RecInfo;

/* INFO_FS during a recording: the newest record start that the writer has put on the card.
   From the writer thread, right after a block (the main loop polls the screens: an SD write
   there could cost frames). It never moves back: with the SD behind, all of the recent record
   starts can be past what is written. */
static u64 g_infoSafe;

static void WriteInfo(void)
{
    if (!g_infoFile)
        return;
    const u64 written = g_written;
    u64 safe = g_infoSafe > sizeof(R3dFileHeader) ? g_infoSafe : sizeof(R3dFileHeader);
    const u32 n = __atomic_load_n(&g_boundN, __ATOMIC_ACQUIRE);
    for (u32 i = n > BOUNDS ? n - BOUNDS : 0; i < n; i++) {
        const u64 b = g_bound[i % BOUNDS];
        if (b + sizeof(R3dRecHeader) <= written && b > safe)
            safe = b;
    }
    g_infoSafe = safe;
    const RecInfo info = {INFO_MAGIC, 1, g_recStartMs, safe};
    u32 done = 0;
    FSFILE_Write(g_infoFile, &done, 0, &info, sizeof(info), FS_WRITE_FLUSH);
}

static void OpenInfo(void)
{
    const FS_Path p = fsMakePath(PATH_ASCII, INFO_FS);
    FSUSER_DeleteFile(g_sd, p);
    g_infoFile = 0;
    if (R_FAILED(FSUSER_OpenFile(&g_infoFile, g_sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        g_infoFile = 0;
    g_boundN = 0;
    g_infoSafe = 0;
    WriteInfo();
}

static void CloseInfo(void)
{
    if (g_infoFile)
        FSFILE_Close(g_infoFile);
    g_infoFile = 0;
    FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, INFO_FS));
}

/* A record that can follow `prevUs` in a real recording (the card past the end holds whatever
   it held before: maybe an older recording's records, whose times do not fit). */
static bool RecordFits(const R3dRecHeader *h, u32 prevUs, bool haveTime, u64 at, u64 size)
{
    const u32 m = h->magic;
    if (m == R3D_MAGIC_PAD || m == R3D_MAGIC_DSPMEM)
        return h->len < 64u * MB && at + sizeof(*h) + h->len <= size;
    if (m != R3D_MAGIC_FRAME && m != R3D_MAGIC_XFRAME && m != R3D_MAGIC_YFRAME && m != R3D_MAGIC_AUDIO &&
        m != R3D_MAGIC_END)
        return false;
    if (h->len > 2u * MB || at + sizeof(*h) + h->len > size)
        return false;
    if (m != R3D_MAGIC_AUDIO && m != R3D_MAGIC_END && (h->screen & 0x3F) > 1)
        return false;
    return !haveTime || (h->timeUs + 2000000u >= prevUs && h->timeUs <= prevUs + 60000000u);
}

/* At start-up, before the next file is prepared (that deletes NEXT_FS): a recording cut off by
   power loss or a crash becomes a normal .r3d (without its R3DE end record; readers estimate). */
static void RecoverCutOff(void)
{
    const FS_Path pi = fsMakePath(PATH_ASCII, INFO_FS), pn = fsMakePath(PATH_ASCII, NEXT_FS);
    Handle fi = 0, fn = 0;
    RecInfo info;
    u32 got = 0;
    if (R_FAILED(FSUSER_OpenFile(&fi, g_sd, pi, FS_OPEN_READ, 0)))
        return;
    const bool okInfo = R_SUCCEEDED(FSFILE_Read(fi, &got, 0, &info, sizeof(info))) && got == sizeof(info) &&
                        info.magic == INFO_MAGIC;
    FSFILE_Close(fi);
    FSUSER_DeleteFile(g_sd, pi);
    if (!okInfo || R_FAILED(FSUSER_OpenFile(&fn, g_sd, pn, FS_OPEN_READ | FS_OPEN_WRITE, 0)))
        return;
    R3dFileHeader fh;
    u64 size = 0;
    FSFILE_GetSize(fn, &size);
    if (R_FAILED(FSFILE_Read(fn, &got, 0, &fh, sizeof(fh))) || got != sizeof(fh) || fh.magic != R3D_MAGIC_FILE ||
        fh.startTimeMs != info.startMs) {
        FSFILE_Close(fn);
        Log("cut-off recording: %s does not match %s, left alone", NEXT_FS, INFO_FS);
        return;
    }
    /* from the safe record on, as long as records look like this recording's */
    u64 at = info.safe >= sizeof(fh) && info.safe < size ? info.safe : sizeof(fh), end = at, last = at;
    u32 prevUs = 0, records = 0;
    bool haveTime = false, closed = false;   /* the first timed record (at the safe point) sets the time */
    for (;;) {
        R3dRecHeader h;
        if (R_FAILED(FSFILE_Read(fn, &got, at, &h, sizeof(h))) || got != sizeof(h))
            break;
        if (!RecordFits(&h, prevUs, haveTime, at, size))
            break;
        const bool delta = h.magic == R3D_MAGIC_XFRAME || h.magic == R3D_MAGIC_YFRAME;
        last = at;
        at += sizeof(h) + (delta ? ((h.len + 15u) & ~15u) : h.len);
        end = at;
        records++;
        if (h.magic != R3D_MAGIC_PAD && h.magic != R3D_MAGIC_DSPMEM) {
            prevUs = h.timeUs;
            haveTime = true;
        }
        if (h.magic == R3D_MAGIC_END) {
            closed = true;
            break;
        }
    }
    /* the record at the cut may be only partly on the card (its tail is what the card held
       before): the last one is dropped, as nothing after it proves it complete */
    if (!closed && records) {
        end = last;
        records--;
    }
    if (end <= sizeof(fh)) {   /* cut off before its first frame reached the card: nothing to keep */
        FSFILE_Close(fn);
        FSUSER_DeleteFile(g_sd, pn);
        Log("cut-off recording: no frame on the card, deleted");
        return;
    }
    FSFILE_SetSize(fn, end);
    FSFILE_Close(fn);
    char name[32], path[96];
    FormatName(name, sizeof(name), info.startMs);
    snprintf(path, sizeof(path), REC_DIR_FS "/%s", name);
    const Result rn = FSUSER_RenameFile(g_sd, pn, g_sd, fsMakePath(PATH_ASCII, path));
    Log("cut-off recording recovered: %s, %llu KB (%lu records past the safe point, %lu s, %s), rename rc %08lX",
        name, end / KB, records, prevUs / 1000000u, closed ? "closed" : "no end record", (u32)rn);
    if (R_SUCCEEDED(rn)) {
        g_shared.recovered++;
        snprintf((char *)g_shared.recoveredFile, sizeof(g_shared.recoveredFile), "%s", name);
    }
}

/* Maps the launcher plugin's heap (from its header at 0x07000000 in the game) once per game. */
static bool MapPluginMemory(void)
{
    if (g_plgSize)
        return true;
    const u32 mode = OS_KernelConfig->app_memtype;
    if (mode == 0 || mode >= 6 || (*(const volatile u32 *)0x1FF800F0 & 0xFFFFu) == 0)
        return false;   /* not an extended-memory game on an Old 3DS, or no plugin in it */
    const u32 pid = ApplicationPid();
    Handle game = 0;
    if (!pid || R_FAILED(svcOpenProcess(&game, pid)))
        return false;
    u32 magic = 0, heapVa = 0, heapSize = 0;
    Result rc = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_HDR_VA, game, 0x07000000u, 0x1000u, 0);
    if (R_SUCCEEDED(rc))
    {
        const volatile u32 *h = (const volatile u32 *)PLG_HDR_VA;
        magic = h[0];
        heapVa = h[2];
        heapSize = h[3];
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_HDR_VA, 0x1000u);
        if (magic != PLG_MAGIC || heapSize < MB || heapSize > 16u * MB || (heapVa & 0xFFFu))
            rc = -1;
        else
            /* MAPEXFLAGS_PRIVATE: mapped like our own heap (0xBB05), so FS writes and DMA accept it */
            rc = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA, game, heapVa, heapSize & ~0xFFFu, 1);
    }
    svcCloseHandle(game);
    if (R_SUCCEEDED(rc))
    {
        /* the SD must take writes straight from it (the writer does that), else SYSTEM as before */
        const FS_Path p = fsMakePath(PATH_ASCII, DIR_FS "/.plugin_memory_test");
        Handle f = 0;
        u32 done = 0;
        rc = FSUSER_OpenFile(&f, g_sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
        if (R_SUCCEEDED(rc))
        {
            rc = FSFILE_Write(f, &done, 0, (const void *)PLG_MEM_VA, 0x1000u, FS_WRITE_FLUSH);
            FSFILE_Close(f);
        }
        FSUSER_DeleteFile(g_sd, p);
        if (R_SUCCEEDED(rc) && done == 0x1000u)
            g_plgSize = heapSize & ~0xFFFu;
        else
        {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA, heapSize & ~0xFFFu);
            if (R_SUCCEEDED(rc))
                rc = -2;
        }
    }
    Log("launcher plugin memory: %s (rc %08lX, magic %08lX, heap %08lX, %lu KB)", R_SUCCEEDED(rc) ? "mapped" : "not used",
        rc, magic, heapVa, heapSize / KB);
    return R_SUCCEEDED(rc);
}

static void UnmapPluginMemory(void)
{
    if (!g_plgSize)
        return;
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA, g_plgSize);
    g_plgSize = 0;
    g_inPlg = false;
    Log("launcher plugin memory unmapped (the game closes)");
}

static bool StartRecording(void)
{
    if (g_shutdownSeen)   /* powering off / rebooting: nothing new */
        return false;
    g_shared.lastError = 0;
    const u32 sdFree = SdFreeMb();
    if (!g_sd || sdFree < MIN_SD_FREE_MB + 16)
    {
        g_shared.lastError = (s32)0xE0000001;   /* SD missing or full */
        Log("start refused: SD free %lu MB", sdFree);
        return false;
    }
    g_inPlg = MapPluginMemory();
    g_plgTop = 0;
    const bool lite = g_inPlg || osGetMemRegionFree(MEMREGION_SYSTEM) < LITE_BELOW;
    g_lite = lite;
    const u32 ringMax = lite ? LITE_RING_MAX : 8u * MB;
    g_keepFree = lite ? KEEP_FREE_LITE : KEEP_FREE;
    g_aqCap = g_inPlg ? AUDIO_QUEUE_PLG : lite ? AUDIO_QUEUE_LITE : AUDIO_QUEUE;
    g_block = lite ? BLOCK_LITE : BLOCK;   /* set while the writer is idle (not recording) */
    /* Half in the plugin's small ring: with the full 256 KB Smash lost as much sound (queue full 376
       vs 364: the converter's stalls, not the ring) and 2.4x more video frames (ring full 851 vs 361),
       O3DS 2026-11-08. */
    g_audioReserveBytes = g_inPlg ? AUDIO_RESERVE / 2u : AUDIO_RESERVE;
    for (int sc = 0; sc < 2; sc++)
    {
        g_arenaCap[sc] = g_inPlg ? ARENA_CAP_PLG[sc] : lite ? ARENA_CAP_LITE[sc] : ARENA_CAP[sc];
        g_arena[sc] = BigAlloc(g_arenaCap[sc]);
        g_prev[sc] = BigAlloc((sc == 0 ? 400u : 320u) * 240u * 2u);
        g_slotCount[sc] = g_slotSize[sc] = 0;
        g_codecFrames[sc] = 0;
        g_codecWords[sc] = 0;
    }
    /* 8 MB rides out ~3 s of SD stall at 2.5 MB/s */
    static const u32 ringSizes[] = { 8u * MB, 6u * MB, 4u * MB, 3u * MB, 2u * MB, 1u * MB };
    g_ring = NULL;
    if (g_inPlg && g_arena[0] && g_arena[1] && g_prev[0] && g_prev[1])
    {
        /* what is left after the audio queue, in whole write blocks */
        const u32 aqBytes = g_cfg.audio ? ((g_aqCap * sizeof(AudioItem) + 0xFFFu) & ~0xFFFu) : 0u;
        const u32 left = g_plgSize - g_plgTop;
        const u32 size = left > aqBytes ? ((left - aqBytes) / g_block) * g_block : 0u;
        if (size >= 4u * g_block && (g_ring = BigAlloc(size)) != NULL)
            g_ringSize = size;
    }
    for (u32 i = 0; i < sizeof(ringSizes) / sizeof(ringSizes[0]) && !g_ring && g_arena[0] && g_arena[1] && g_prev[0] && g_prev[1]; i++)
        if (ringSizes[i] <= ringMax && (g_ring = BigAlloc(ringSizes[i])) != NULL)
            g_ringSize = ringSizes[i];
    if (!g_ring)
    {
        BigFreeAll();
        g_shared.lastError = (s32)0xE0000002;   /* not enough SYSTEM memory */
        Log("start refused: SYSTEM free %lu KB", osGetMemRegionFree(MEMREGION_SYSTEM) / KB);
        return false;
    }
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_track, 0, sizeof(g_track));
    g_aq = g_cfg.audio ? BigAlloc(g_aqCap * sizeof(AudioItem)) : NULL;
    g_audioReserve = g_aq != NULL;
    g_aqHead = g_aqTail = 0;
    g_audioFrames = g_audioDropped = g_dspSnaps = g_dspOffPolls = 0;
    g_dspCacheRc = 0;
    g_lastSnapUs = 0;
    memset(g_audioLast, 0, sizeof(g_audioLast));
    memset(g_audioPrev, 0, sizeof(g_audioPrev));
    g_audioActive[0] = g_audioActive[1] = false;
    g_audioQueueFull = 0;
    g_audioUnstable = g_audioBoth = g_audioMaxGapUs = g_audioGapsOver4ms = g_audioGapSinceFrame = 0;
    g_audioLastPollTick = 0;

    g_file = TakePrepared();
    const bool prepared = g_file != 0;
    if (!g_file)
    {
        const FS_Path p = fsMakePath(PATH_ASCII, NEXT_FS);
        FSUSER_DeleteFile(g_sd, p);
        if (R_FAILED(FSUSER_OpenFile(&g_file, g_sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        {
            g_file = 0;
            BigFreeAll();
            g_shared.lastError = (s32)0xE0000003;
            Log("start refused: cannot create %s", NEXT_FS);
            return false;
        }
        FSFILE_SetSize(g_file, FIRST_ALLOC);
    }
    g_allocated = prepared ? g_prepReached : FIRST_ALLOC;
    g_growCount = g_growMaxMs = 0;
    g_writeTicks = 0;
    g_writeMaxMs = 0;
    g_written = 0;
    g_writeRc = 0;
    g_w = g_r = 0;
    g_dropped = g_throttled = g_ringMax = 0;
    memset(g_drops, 0, sizeof(g_drops));
    g_dropBurstCount = g_dropBurstsLost = 0;
    memset(g_encodeMaxUs, 0, sizeof(g_encodeMaxUs));
    memset(g_encodeSlow, 0, sizeof(g_encodeSlow));
    memset(g_encodeSumUs, 0, sizeof(g_encodeSumUs));
    g_shared.framesTop = g_shared.framesBot = 0;
    g_appSwapUs = 0;
    g_appSwapSeen = false;

    R3dFileHeader *fh = (R3dFileHeader *)g_ring;
    memset(fh, 0, sizeof(*fh));
    fh->magic = R3D_MAGIC_FILE;
    fh->version = R3D_VERSION;
    s64 isN3ds = 0;
    fh->isNew3ds = svcGetSystemInfo(&isN3ds, 0x10001, 0) == 0;
    fh->mode = g_cfg.bottom ? R3D_MODE_FULL_BOTH : R3D_MODE_FULL_TOP;
    fh->fps = (u8)g_cfg.fps;
    fh->audioRate = g_aq ? R3D_AUDIO_RATE_MHZ : 0;
    g_recStartMs = osGetTime();
    fh->startTimeMs = g_recStartMs;
    g_w = sizeof(*fh);
    OpenInfo();

    g_recStartTick = Ticks();
    g_writerActive = true;
    g_recording = true;
    g_shared.ringKb = g_ringSize / KB;
    g_shared.sysFreeKb = osGetMemRegionFree(MEMREGION_SYSTEM) / KB;
    g_shared.state = REC_STATE_RECORDING;
    LedRecording();
    Log("recording started (%sfile %llu MB%s, PDN_DSP_CNT %02X, fps %lu, %s, audio %s, ring %lu KB, %s memory profile%s, "
        "SYSTEM free %lu KB, SD free %lu MB, %s)",
        g_cfg.audioDebug ? "AUDIO DEBUG: no video, raw DSP polls; " : "", g_allocated / MB, prepared ? " prepared" : "",
        *(const volatile u8 *)PDN_DSP_CNT_VA, g_cfg.fps, g_cfg.bottom ? "both screens" : "top screen",
        g_aq ? "on" : g_cfg.audio ? "off (no memory)" : "off", g_ringSize / KB, lite ? "light" : "normal",
        g_inPlg ? " in the launcher plugin's memory" : "",
        g_shared.sysFreeKb, sdFree,
        CpuState());
    LogMemory("recording started");
    return true;
}

/* New 3DS: the clock and L2 the running program chose (the converter's speed depends on them). */
static const char *CpuState(void)
{
    static char buf[48];
    s64 mhz = 0, l2 = 0;
    if (R_SUCCEEDED(svcGetSystemInfo(&mhz, 0x10001, 0)) && R_SUCCEEDED(svcGetSystemInfo(&l2, 0x10001, 2)))
        snprintf(buf, sizeof(buf), "CPU %lld MHz, L2 %s", (long long)mhz, l2 ? "on" : "off");
    else
        snprintf(buf, sizeof(buf), "CPU 268 MHz (Old 3DS)");
    return buf;
}

/* One log line per drop kind that happened, then the bursts (time, screen, count). */
static void LogDrops(void)
{
    Log("video drops: ring full %lu, no free slot %lu, DMA timeout %lu, DMA error %lu, DMA start failed %lu (last rc %08lX), "
        "slots busy %lu; encode avg %lu / %lu us, max %lu / %lu us, over 33 ms %lu / %lu (top / bottom); %s",
        g_drops[DROP_RING], g_drops[DROP_NO_SLOT], g_drops[DROP_DMA_TIMEOUT], g_drops[DROP_DMA_ERROR],
        g_drops[DROP_DMA_START], (u32)g_dmaStartRc, g_drops[DROP_SLOTS_BUSY],
        g_shared.framesTop ? (u32)(g_encodeSumUs[0] / g_shared.framesTop) : 0,
        g_shared.framesBot ? (u32)(g_encodeSumUs[1] / g_shared.framesBot) : 0, g_encodeMaxUs[0], g_encodeMaxUs[1],
        g_encodeSlow[0], g_encodeSlow[1], CpuState());
    for (u32 i = 0; i < g_dropBurstCount; i++)
    {
        const DropBurst *b = &g_dropBursts[i];
        Log("  drop burst %lu.%03lu-%lu.%03lu s, %s, %s x%lu, rc %08lX", b->startUs / 1000000u, b->startUs / 1000u % 1000u,
            b->endUs / 1000000u, b->endUs / 1000u % 1000u, b->screen ? "bottom" : "top", DROP_NAMES[b->kind], b->count,
            (u32)b->rc);
    }
    if (g_dropBurstsLost)
        Log("  drop bursts not listed: %lu drops", g_dropBurstsLost);
}

/* The R3DE record: the gallery reads length, frames and drops from the file's last 32 bytes. */
static void WriteEnd(void)
{
    struct {
        R3dRecHeader h;
        R3dEndInfo e;
    } end = {
        {R3D_MAGIC_END, sizeof(R3dEndInfo), (u32)TicksToUs(Ticks() - g_recStartTick), 0, 0, 0},
        {g_shared.framesTop, g_shared.framesBot, g_dropped, 0},
    };
    if (R_FAILED(g_writeRc))
        return;
    u32 done = 0;
    g_writeRc = FSFILE_Write(g_file, &done, g_written, &end, sizeof(end), FS_WRITE_FLUSH);
    g_written += done;
}

/* HOME in a game that needs extra memory (Old 3DS): HOME Menu wants SYSTEM memory a moment after
   the button, sooner than the ring reaches the SD (0.5-2.6 s). The capture buffers, the previous
   frames and the audio queue go back at once (~1.9 MB, light profile); the ring follows when it is
   written. Allocation order: arenas and previous frames, then the ring, then the audio queue. */
static void ReleaseEncoderMemory(void *aq)
{
    u32 out = 0;
    const u32 ringVa = (u32)g_ring;
    if (ringVa > g_bigLow)
        svcControlMemoryUnsafe(&out, g_bigLow, ringVa - g_bigLow, MEMOP_FREE, (MemPerm)0);
    if (aq && (u32)aq < g_bigTop)
    {
        svcControlMemoryUnsafe(&out, (u32)aq, g_bigTop - (u32)aq, MEMOP_FREE, (MemPerm)0);
        g_bigTop = (u32)aq;
    }
    g_bigLow = ringVa;
    for (int sc = 0; sc < 2; sc++)
    {
        g_arena[sc] = NULL;
        g_prev[sc] = NULL;
        g_slotCount[sc] = g_slotSize[sc] = 0;
    }
    memset(g_slots, 0, sizeof(g_slots));
}

static void StopRecording(const char *why, bool releaseFirst)
{
    g_shared.state = REC_STATE_SAVING;
    g_recording = false;
    /* pending DMAs finish or time out, the converter empties the slots */
    for (u32 i = 0; i < 200 && !SlotsIdle(); i++)
        svcSleepThread(5 * 1000 * 1000);
    while (g_audioInPoll)   /* the audio thread sees g_recording == false from its next poll */
        svcSleepThread(1 * 1000 * 1000);
    for (u32 i = 0; i < 100 && g_aq && g_aqTail != g_aqHead; i++)   /* queued audio into the ring */
    {
        LightEvent_Signal(&g_convEvent);
        svcSleepThread(5 * 1000 * 1000);
    }
    void *const aq = g_aq;
    g_aq = NULL;   /* the converter stops touching the ring for audio ... */
    g_audioReserve = false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    while (g_convInAudio)   /* ... once it leaves DrainAudio */
        svcSleepThread(1 * 1000 * 1000);
    bool ringDropped = false;
    u64 dropFrom = 0;
    if (releaseFirst && SlotsIdle())
    {
        ReleaseEncoderMemory(aq);
        /* The ring too, without waiting for the SD: HOME Menu took the memory within 0.4 s while
           the ring was still being written, and crashed (O3DS 2026-11-08, 2nd HOME). The writer
           ends its current block (~50 ms); the file is cut at the last frame start written; the
           unwritten tail (under a second) is lost. */
        g_writerActive = false;
        while (g_writerBusy)
            svcSleepThread(2 * 1000 * 1000);
        const u64 r = g_r;
        u64 cut = 0;
        const u32 n = __atomic_load_n(&g_boundN, __ATOMIC_ACQUIRE);
        for (u32 i = n > BOUNDS ? n - BOUNDS : 0; i < n; i++)
            if (g_bound[i % BOUNDS] <= r && g_bound[i % BOUNDS] > cut)
                cut = g_bound[i % BOUNDS];
        if (r == g_w)
            cut = r;
        if (cut >= sizeof(R3dFileHeader) && cut <= g_written)
        {
            dropFrom = cut;
            g_written = cut;
            g_r = g_w;
            u32 out = 0;
            svcControlMemoryUnsafe(&out, g_bigLow, g_bigTop - g_bigLow, MEMOP_FREE, (MemPerm)0);
            g_bigLow = g_bigTop = BIG_VA;
            g_ring = NULL;
            ringDropped = true;
        }
        Log("HOME: recording memory given back at once (ring %s), SYSTEM free %lu KB",
            ringDropped ? "dropped" : "still being written", osGetMemRegionFree(MEMREGION_SYSTEM) / KB);
    }
    /* writer: whole blocks first, then the tail from here */
    for (u32 i = 0; i < 2000 && !ringDropped && g_w - g_r >= g_block; i++)
        svcSleepThread(5 * 1000 * 1000);
    g_writerActive = false;
    while (g_writerBusy)   /* a block write may take ~0.3 s */
        svcSleepThread(5 * 1000 * 1000);
    if (!ringDropped)
        WriterFlushTail();
    WriteEnd();

    char name[32];
    FormatName(name, sizeof(name), g_recStartMs);
    char path[96];
    snprintf(path, sizeof(path), REC_DIR_FS "/%s", name);
    FSFILE_SetSize(g_file, g_written);
    FSFILE_Close(g_file);
    g_file = 0;
    const Result rn = FSUSER_RenameFile(g_sd, fsMakePath(PATH_ASCII, NEXT_FS), g_sd, fsMakePath(PATH_ASCII, path));
    CloseInfo();   /* saved: nothing to recover */
    const u32 seconds = TicksToUs(Ticks() - g_recStartTick) / 1000000u;
    Log("recording saved: %s, %lu s, %llu KB, frames %lu + %lu, dropped %lu, throttled %lu, ring max %lu KB, "
        "file grown %lu times (max %lu ms), audio frames %lu (ring full %lu, queue full %lu, still-writing polls %lu, both-changed %lu, poll gap max %lu us, gaps > 4 ms %lu), "
        "DSP snapshots %lu, DSP off polls %lu, DSP cache rc %08lX, write rc %08lX, rename rc %08lX, reason: %s",
        name, seconds, g_written / KB, g_shared.framesTop, g_shared.framesBot, g_dropped, g_throttled,
        g_ringMax / KB, g_growCount, g_growMaxMs, g_audioFrames, g_audioDropped, g_audioQueueFull, g_audioUnstable, g_audioBoth, g_audioMaxGapUs, g_audioGapsOver4ms,
        g_dspSnaps, g_dspOffPolls, (u32)g_dspCacheRc, (u32)g_writeRc, (u32)rn, why);
    {
        /* the card's speed while it wrote (throttling means it was slower than the video) */
        const u64 us = g_writeTicks / (SYSCLOCK_ARM11 / 1000000u);
        Log("SD writes: %llu KB at %lu KB/s while writing, slowest block %lu ms; video %lu KB/s on average",
            g_written / KB, us ? (u32)(g_written * 1000000ull / us / KB) : 0, g_writeMaxMs,
            seconds ? (u32)(g_written / KB / seconds) : 0);
    }
    if (ringDropped)
        Log("  file cut at %llu KB: the part not yet on the SD was dropped for HOME", dropFrom / KB);
    LogDrops();
    LogMemory("recording stopped");
    SetBoost(false, "recording stopped");
    snprintf((char *)g_shared.lastFile, sizeof(g_shared.lastFile), "%s", name);
    g_shared.files++;
    if (R_FAILED(g_writeRc) || R_FAILED(rn))
        g_shared.lastError = R_FAILED(g_writeRc) ? g_writeRc : rn;

    g_ring = NULL;
    BigFreeAll();
    g_appPid = g_appBase = 0;
    g_shared.state = REC_STATE_IDLE;
    if (R_FAILED(g_shared.lastError))
        LED_SIGNAL(LED_ERROR);
    else
        LED_SIGNAL(LED_SAVED);
    PrepareNext();
}

/* The app says why a recording stopped when it is opened next (recorder_take_system_stop). */
static void WriteSystemStop(bool gameClosed)
{
    Handle f = 0;
    const FS_Path p = fsMakePath(PATH_ASCII, STOPPED_FS);
    FSUSER_DeleteFile(g_sd, p);
    if (R_FAILED(FSUSER_OpenFile(&f, g_sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        return;
    u32 done = 0;
    char what[64];
    /* "game": an extended-memory game closed and rebooted the Old 3DS back to HOME */
    snprintf(what, sizeof(what), "%s%s", (const char *)g_shared.lastFile, gameClosed ? "\ngame" : "");
    FSFILE_Write(f, &done, 0, what, strlen(what), FS_WRITE_FLUSH);
    FSFILE_Close(f);
}

/* ------------------------------------------------------------------ main loop */

int main(void)
{
    svcGetProcessId(&g_ownPid, CUR_PROCESS_HANDLE);
    LightEvent_Init(&g_convEvent, RESET_ONESHOT);
    LightLock_Init(&g_dropLock);
    InitFs();
    LoadConfig();
    InitLed();
    g_shared.initStage = 4;

    /* the writer sits just above the converter: it mostly waits on the SD, and a starved writer
       fills the ring */
    Thread writer = threadCreate(WriterMain, NULL, 8 * KB, CONVERTER_PRIO - 1, 1, false);
    Thread conv = threadCreate(ConverterMain, NULL, 8 * KB, CONVERTER_PRIO, 1, false);
    g_writerThread = writer;
    g_convThread = conv;
    /* highest non-privileged priority: a poll late by more than ~6 ms loses a frame
       (v5: ~1 per second at 0x20); it only reads 1.3 KB per ms */
    Thread audio = threadCreate(AudioMain, NULL, 4 * KB, 0x18, 1, false);
    if (!writer || !conv || !audio)
    {
        g_shared.state = REC_STATE_ERROR;
        g_shared.lastError = (s32)0xE0000004;
        Log("start-up failed: threadCreate");
        LED_SIGNAL(LED_ERROR);
        for (;;)
        {
            if (g_shared.cmd == REC_CMD_EXIT)
                break;
            LedTick();
            svcSleepThread(50 * 1000 * 1000);
        }
        g_shared.state = REC_STATE_EXITED;
        return 0;
    }
    s64 luma = 0, n3ds = 0;
    svcGetSystemInfo(&luma, 0x10000, 0);
    svcGetSystemInfo(&n3ds, 0x10000, 0x201);
    const u32 kv = osGetKernelVersion();
    Log("recorder started (build " __DATE__ " " __TIME__ ", %s, Luma %lu.%lu.%lu, kernel %lu.%lu-%lu, hotkey mask %03lX, "
        "fps %lu, screens %s, audio %s, file limit %lu MB, SYSTEM free %lu KB, SD free %lu MB)",
        n3ds ? "New 3DS" : "Old 3DS", (u32)(luma >> 24) & 0xFF, (u32)(luma >> 16) & 0xFF, (u32)(luma >> 8) & 0xFF,
        GET_VERSION_MAJOR(kv), GET_VERSION_MINOR(kv), GET_VERSION_REVISION(kv), g_cfg.hotkey, g_cfg.fps,
        g_cfg.bottom ? "both" : "top", g_cfg.audio ? "on" : "off", g_cfg.maxFileMb, osGetMemRegionFree(MEMREGION_SYSTEM) / KB,
        SdFreeMb());
    const bool resumeInGame = AfterReboot();
    RecoverCutOff();   /* before PrepareNext: it deletes NEXT_FS */
    g_shared.state = REC_STATE_IDLE;
    LED_SIGNAL(LED_READY);
    PrepareNext();

    /* Power-off and reboot (leaving System Settings reboots the console, O3DS 2026-11-07): ptm
       announces it (0x108), then pm asks every process to end (0x100) and kills it a moment later.
       A recording is saved properly first instead of being cut off. */
    Handle notif = 0;
    if (R_SUCCEEDED(srvEnableNotification(&notif)))
    {
        srvSubscribe(PTMNOTIFID_SHUTDOWN);
        srvSubscribe(0x100);
        srvSubscribe(0x213);   /* lid opened (NS) */
        srvSubscribe(0x214);   /* lid closed: the console goes to sleep */
        srvSubscribe(0x204);   /* HOME pressed (Rosalina publishes it too) */
    }
    else
        notif = 0;
    bool systemEnd = false;
    /* The lid: in sleep the GPU and LCD are off, and reading their registers or DMA from VRAM
       froze the whole console (New 3DS, 2026-11-07). Closing the lid saves the recording; it goes
       on in a new file a moment after the lid opens. ptm:u is a second source for the lid state. */
    const bool ptmu = R_SUCCEEDED(ptmuInit());
    bool lidClosed = false, lidResume = false;
    u64 resumeAt = 0, lastLidPoll = 0;
    if (resumeInGame)   /* the game draws its first frames by then; same path as after the lid */
    {
        lidResume = true;
        resumeAt = Ticks() + SYSCLOCK_ARM11 * 2;
    }

    bool comboHeld = true;   /* a combo still held from the launch must not start a recording */
    bool homeResume = false, homeSeen = false;   /* a recording saved for HOME goes on after it */
    u32 lastPlg = 0;
    bool reloadPending = false;
    u64 lastSdCheck = 0;
    for (;;)
    {
        u32 cmd = g_shared.cmd;
        if (cmd != REC_CMD_NONE)
            g_shared.cmd = REC_CMD_NONE;
        u32 nid = 0;
        if (notif && svcWaitSynchronization(notif, 0) == 0 && R_FAILED(srvReceiveNotification(&nid)))
            nid = 0;
        if ((g_recording || lidClosed) && ptmu && Ticks() - lastLidPoll > SYSCLOCK_ARM11 / 10)   /* every 100 ms */
        {
            lastLidPoll = Ticks();
            u8 open = 1;
            if (R_SUCCEEDED(PTMU_GetShellState(&open)) && !nid)
                nid = !open && !lidClosed ? 0x214 : open && lidClosed ? 0x213 : 0;
        }
        if (nid == 0x214 && !lidClosed)
        {
            lidClosed = true;
            Log("lid closed%s", g_recording ? ": recording saved, it goes on when the lid opens" : "");
            if (g_recording)
            {
                StopRecording("lid closed (sleep)", false);
                lidResume = true;
            }
        }
        else if (nid == 0x213 && lidClosed)
        {
            lidClosed = false;
            if (lidResume)   /* the screens wake up a moment after the lid */
                resumeAt = Ticks() + SYSCLOCK_ARM11 * 3 / 2;
            Log("lid opened%s", lidResume ? ": recording goes on in 1.5 s" : "");
        }
        /* HOME in a game that needs extra memory (Old 3DS): HOME Menu wants SYSTEM memory back when it
           opens, and with a light-profile recording in it HOME Menu crashed (ErrDisp 0xD8C3FBF3,
           O3DS 2026-11-08). The recording is saved as soon as HOME is pressed and goes on in a new
           file when the game is back: Rosalina's plugin status word in the config page says that
           (2 while our launcher plugin is swapped out for HOME, 1 again in the game). */
        if (nid == 0x204)
        {
            Log("HOME pressed: SYSTEM free %lu KB%s", osGetMemRegionFree(MEMREGION_SYSTEM) / KB,
                g_recording && g_lite && !g_inPlg ? ", the recording is saved to give HOME Menu its memory" : "");
            if (g_recording && g_lite && !g_inPlg)
            {
                StopRecording("HOME in a game that needs extra memory", true);
                homeResume = true;
                homeSeen = false;
            }
        }
        {
            const u32 plg = *(const volatile u32 *)0x1FF800F0 & 0xFFFFu;   /* PLG_CFG_*: 1 running, 2 in HOME */
            if (plg != lastPlg)
            {
                if (OS_KernelConfig->app_memtype != 0)
                    Log("launcher plugin status %lu (%s): SYSTEM free %lu KB", plg,
                        plg == 1 ? "game" : plg == 2 ? "HOME, plugin swapped out" : plg == 3 ? "game closing" : "none",
                        osGetMemRegionFree(MEMREGION_SYSTEM) / KB);
                lastPlg = plg;
                if (homeResume && plg == 2)
                    homeSeen = true;
                if (homeResume && homeSeen && plg == 1)   /* back in the game: new file in 1 s */
                {
                    homeResume = false;
                    lidResume = true;
                    resumeAt = Ticks() + SYSCLOCK_ARM11;
                }
                if (plg == 3 || plg == 0)   /* the game closes: Rosalina frees the plugin's memory after it */
                {
                    homeResume = false;
                    if (g_recording && g_inPlg)
                    {
                        StopRecording("the game closes", false);
                        WriteSystemStop(true);   /* the console restarts right after: the app explains */
                    }
                    UnmapPluginMemory();
                }
            }
        }
        if (lidResume && resumeAt && !lidClosed && Ticks() >= resumeAt)
        {
            lidResume = false;
            resumeAt = 0;
            if (!g_recording && !StartRecording())
                LED_SIGNAL(LED_ERROR);
        }
        if (nid == PTMNOTIFID_SHUTDOWN || nid == 0x100)
        {
            Log("system notification %03lX: %s", nid, nid == 0x100 ? "the recorder must end" : "power-off or reboot");
            g_shutdownSeen = true;   /* no new recording and no file prepared from now on */
            const bool wasRecording = g_recording;
            const bool gameReboot = nid == 0x100 && !g_powerOffSeen;   /* a game's memory mode, System Settings... */
            if (gameReboot)
                WriteGameOrder(wasRecording);
            if (g_recording)
            {
                StopRecording(nid == 0x100 ? "the system ends the recorder" : "power-off or reboot", false);
                WriteSystemStop(nid == 0x100 && !g_powerOffSeen && OS_KernelConfig->app_memtype != 0 &&
                                OS_KernelConfig->app_memtype < 6);
            }
            if (nid == PTMNOTIFID_SHUTDOWN)
                g_powerOffSeen = true;
            if (nid == 0x100)
            {
                if (gameReboot)
                    LogGameOrder(wasRecording);
                systemEnd = true;
                cmd = REC_CMD_EXIT;
            }
        }
        if (cmd == REC_CMD_RELOAD)
            reloadPending = true;
        if (reloadPending && !g_recording)   /* never mid-recording: fps / screens / audio stay as they started */
        {
            reloadPending = false;
            LoadConfig();
            Log("settings reloaded (hotkey mask %03lX, fps %lu, screens %s, audio %s, led %lu)", g_cfg.hotkey, g_cfg.fps,
                g_cfg.bottom ? "both" : "top", g_cfg.audio ? "on" : "off", g_cfg.led);
        }

        if (lidClosed && !g_recording)   /* asleep: no LED, no buttons, no screens until the lid opens */
        {
            if (cmd == REC_CMD_EXIT)
                break;
            svcSleepThread(POLL_IDLE_US * 1000ull);
            continue;
        }
        LedTick();
        bool toggle = false;
        const u32 pad = ~REG32(PAD_VA) & 0xFFFu;
        if ((pad & g_cfg.hotkey) == g_cfg.hotkey)
        {
            toggle = !comboHeld;
            comboHeld = true;
        }
        else
            comboHeld = false;

        if (g_recording)
        {
            if (!g_cfg.audioDebug)
                PollScreens();
            UpdateBoost();
            const u64 now = Ticks();
            g_shared.recSeconds = TicksToUs(now - g_recStartTick) / 1000000u;
            g_shared.recKb = (u32)(g_written / KB);
            g_shared.dropped = g_dropped;
            g_shared.throttled = g_throttled;
            const char *why = NULL;
            bool goOn = false;   /* the file is full: save it and go on in a new one */
            if (toggle || cmd == REC_CMD_STOP)
                why = "user";
            else if (cmd == REC_CMD_EXIT)
                why = "recorder exit";
            else if (g_written + 8ull * MB >= MAX_FILE) {
                why = "file size limit (FAT32): going on in a new file";
                goOn = true;
            }
            else if (R_FAILED(g_writeRc))
                why = "SD write error";
            else if (now - lastSdCheck > 5ull * SYSCLOCK_ARM11)
            {
                lastSdCheck = now;
                g_shared.sdFreeMb = SdFreeMb();
                if (g_written + 2 * CHUNK_ALLOC >= g_allocated && g_shared.sdFreeMb < MIN_SD_FREE_MB)
                    why = "SD card full";
            }
            if (why)
                StopRecording(why, false);
            if (goOn && !StartRecording()) /* ~1-2 s between the two files */
                LED_SIGNAL(LED_ERROR);
            if (cmd == REC_CMD_EXIT)
                break;
            svcSleepThread(POLL_REC_US * 1000ull);
            continue;
        }

        if (cmd == REC_CMD_EXIT)
            break;
        if ((toggle || cmd == REC_CMD_START) && !g_shutdownSeen && !lidClosed)
        {
            if (!StartRecording())
                LED_SIGNAL(LED_ERROR);
            continue;
        }
        svcSleepThread(POLL_IDLE_US * 1000ull);
    }

    /* exit (uninstall / update from the app): nothing may stay behind */
    if (g_prepThread || g_prepFile)
    {
        const Handle h = TakePrepared();
        if (h)
            FSFILE_Close(h);
    }
    FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, NEXT_FS));
    FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, INFO_FS));
    LedOff();
    Log(systemEnd ? "recorder ended by the system (power-off or reboot)" : "recorder exited");
    g_shared.state = REC_STATE_EXITED;
    /* the writer and converter threads are idle; the process exit ends them */
    if (g_sd)
        FSUSER_CloseArchive(g_sd);
    svcExitProcess();
    return 0;
}
