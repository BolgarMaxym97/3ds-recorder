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
#include "huff.h"
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
#define KEYFRAME_EVERY 60u   /* ~2 s: keyframes were 4-18% of the bytes at 1 s (O3DS recordings, 2026-11-09) */
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
#define BLOCK          (512u * KB)   /* 256 KB wrote 7% slower (MK7, O3DS 2026-11-09: 4968 vs 5338 KB/s) */
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
/* Replay buffer (config replay=on): the stream goes round in a file of a fixed size instead of a
   growing one, so the card always holds the last half a minute and more; a hotkey makes a clip of
   the last REPLAY_CLIP_US (r3d_format.h: R3dRingInfo). Two such files: a saved clip takes one away
   (renamed, nothing copied), the buffer goes on in the other while a new one is made.
   256 MB: 30 s of both screens at up to 8.5 MB/s (a card takes ~5.4 on an Old 3DS). */
#define REPLAY_BYTES    (256ull * MB)
#define REPLAY_DATA_OFF BLOCK          /* the ring's data starts a write block into the file */
#define REPLAY_CLIP_US  30000000ull
#define REPLAY_STEP     (16ull * MB)   /* a new file grows in steps this big (32 MB stalled the writer 1-1.5 s) */
#define REPLAY_RETRY_US 3000000ull     /* between tries to start the buffer */
#define REPLAY_BEAT_US  4000000ull     /* the app's beat older than this: the app is off the screen */
#define MAX_REC_US      3600000000ull  /* a recording goes on in a new file after an hour: timeUs (32 bits) wraps at 71.6 min */
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
    bool top;        /* record the top screen */
    bool bottom;     /* record the bottom screen (screens=top: off; screens=bottom: the only one) */
    u32 led;         /* 0 off, 1 solid, 2 pulse */
    bool audio;      /* record the DSP output */
    bool audioDebug; /* diagnostics: no video, raw DSP output polls every 250 us */
    u32 maxFileMb;   /* a recording goes on in a new file past this (tests set less: max_file_mb) */
    /* Huffman stage (huffman=on, not in the app). Off: on an Old 3DS neither core had the time for
       it in Mario Kart 7 (game's core: 1 frame in 3 coded; packer on the system core: 64-76 ms a
       frame, the converter waited and ~1060 frames were lost, O3DS 2026-11-09). Left for New 3DS
       tests; every reader decodes R3DH. */
    bool huff;
    /* Q565 frames (intra=on, not in the app). Off: in C the converter took ~54 ms a frame for them
       on an Old 3DS (XR32 ~11 ms) and lost ~550 frames in 30 s of Mario Kart 7, though they took a
       third of the bytes (O3DS 2026-11-09). On again once the encoder is fast enough. */
    bool intra;
    /* quality=lossless (the app's Quality setting): XR32 only, every frame exact, as before 0.12.
       auto: frames of a picture that changes all over are stored as C420 (codec.h). */
    bool lossless;
    s8 c420;         /* c420=on / off (not in the app): overrides the console's default, -1: default */
    /* n3ds_test=on (a test build has it on: make REC_TESTBUILD=1), New 3DS only: benches at start-up
       (per core, at both processor speeds), an idle probe of every core around each recording, and
       each recording takes the next of TEST_VARS (codec, cores). One session of a tester's gives
       what the New 3DS defaults need. */
    bool n3dsTest;
    u32 sdLimitKb;   /* sd_limit_kb=N (not in the app, a test): the card takes at most N KB/s, as a slow one would */
    bool bench;      /* codec_bench=on: CodecBench at start-up (~0.6 s on an Old 3DS), not in the app */
    bool abTest;     /* ab_test=on: each recording takes the next AB_* variant (not in the app) */
    bool replay;     /* replay=on: the replay buffer runs whenever nothing else is recorded */
    u32 saveKey;     /* replay_hotkey: KEY_* mask that saves the buffer's last 30 s as a clip */
} Config;

static Config g_cfg = { .hotkey = KEY_R | KEY_SELECT, .fps = 30, .top = true, .bottom = true, .led = 2, .audio = true,
                        .audioDebug = false, .maxFileMb = REC_MAX_FILE_MB, .huff = false, .intra = false, .lossless = false, .c420 = -1, .bench = false, .abTest = false,
                        .replay = false, .saveKey = KEY_X | KEY_SELECT,
#ifdef REC_TESTBUILD
                        .n3dsTest = true,
#endif
                      };

static const char *ScreensName(void)
{
    return g_cfg.top && g_cfg.bottom ? "both" : g_cfg.top ? "top" : "bottom";
}

/* ------------------------------------------------------------------ shared page */

volatile RecShared g_shared __attribute__((aligned(0x1000), used)) = {
    REC_SHARED_MAGIC, REC_SHARED_VERSION, REC_STATE_STARTING, REC_CMD_NONE,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, { 0 }, 0, { 0 }, 0, 0, 0, 0, { 0 },
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
/* Times since a recording's start: 32 bits of microseconds wrap after 71.6 minutes, and the replay
   buffer runs for hours. Records keep the low 32 bits (timeUs); everything compared is 64 bits. */
static u64 TicksToUs64(u64 t) { return t / (SYSCLOCK_ARM11 / 1000000u); }

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
/*
 * The log. While frames are captured a line stays in memory: the main thread, which polls the
 * screens every 2 ms, used to open and write the log file itself, behind the writer's 512 KB
 * blocks on the same card (lines come right when a game loads or HOME opens: screen modes,
 * the program in front, priorities; frames went missing there uncounted). The writer's thread
 * puts the kept lines on the card every couple of seconds, between its blocks; the first line
 * after the capture stops writes them out too. Straight FS calls: any thread's stack will do.
 */
#define LOG_BUF (16u * KB)
static FS_Archive g_sd;
static volatile bool g_recording;      /* capture on */
static char g_logBuf[LOG_BUF], g_logOut[LOG_BUF], g_logLine[1536];
static volatile u32 g_logLen;
static LightLock g_logLock;
static bool g_logLockOk;

static void LogWrite(const char *text, u32 len)
{
    Handle f = 0;
    u64 size = 0;
    u32 done = 0;
    if (!g_sd || R_FAILED(FSUSER_OpenFile(&f, g_sd, fsMakePath(PATH_ASCII, LOG_FS), FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        return;
    FSFILE_GetSize(f, &size);
    FSFILE_Write(f, &done, size, text, len, FS_WRITE_FLUSH);
    FSFILE_Close(f);
}

/* The lines kept in memory, onto the card. */
static void LogFlush(void)
{
    if (!g_logLockOk || !g_logLen)
        return;
    /* copied out first: the main thread's next line must not wait for the card behind this lock */
    LightLock_Lock(&g_logLock);
    const u32 n = g_logLen;
    memcpy(g_logOut, g_logBuf, n);
    g_logLen = 0;
    LightLock_Unlock(&g_logLock);
    if (n)
        LogWrite(g_logOut, n);
}

static void Log(const char *fmt, ...)
{
    static u32 lines;
    if (++lines > 4000)   /* a runaway loop must not fill the SD */
        return;
    if (g_logLockOk)
        LightLock_Lock(&g_logLock);
    const u64 ms = osGetTime();
    u32 y, m, d, sod;
    CivilTime(ms, &y, &m, &d, &sod);
    int n = snprintf(g_logLine, sizeof(g_logLine), "[%04lu-%02lu-%02lu %02lu:%02lu:%02lu.%03lu] ", y, m, d, sod / 3600,
                     sod / 60 % 60, sod % 60, (u32)(ms % 1000u));
    const int room = (int)sizeof(g_logLine) - n - 1;   /* the line's end keeps a byte */
    va_list ap;
    va_start(ap, fmt);
    int t = vsnprintf(g_logLine + n, (size_t)room, fmt, ap);
    va_end(ap);
    n += t < 0 ? 0 : t >= room ? room - 1 : t;
    g_logLine[n++] = '\n';
    if (g_recording && g_logLockOk && g_logLen + (u32)n <= LOG_BUF)
    {
        memcpy(g_logBuf + g_logLen, g_logLine, (size_t)n);
        g_logLen += (u32)n;
    }
    else
    {
        if (g_logLen)
            LogWrite(g_logBuf, g_logLen);
        g_logLen = 0;
        LogWrite(g_logLine, (u32)n);
    }
    if (g_logLockOk)
        LightLock_Unlock(&g_logLock);
}

/* The recorder's FS session at a low priority: the ring's constant writes must not get ahead of
   the game's own SD reads (streamed music, levels). Luma3DS-Flashback measured no cost to the
   writes at its lowest priority (8, which it uses); the values before and after are logged. */
#define FS_PRIORITY 8u
static u32 g_fsPrioOld = 0xFFFFFFFFu, g_fsPrioNew = 0xFFFFFFFFu;

static void InitFs(void)
{
    FSUSER_GetPriority(&g_fsPrioOld);
    if (R_SUCCEEDED(FSUSER_SetPriority(FS_PRIORITY)))
        FSUSER_GetPriority(&g_fsPrioNew);
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

/* The battery, for the log: recordings and the replay buffer say what they started and ended
   with, and a line every ten minutes says it whatever the recorder does, so a play session with
   the buffer on can be put next to one with it off. Percent and voltage from the MCU (the
   voltage register counts 20 mV steps and moves sooner than the percent); the charger from ptm:u
   (a session on the charger says nothing about the drain). */
static bool g_ptmu;
typedef struct { u8 pct; u32 mv; bool charger; } Battery;

static Battery BatteryNow(void)
{
    Battery b = { 0, 0, false };
    u8 v = 0;
    if (R_SUCCEEDED(g_mcuRc))
    {
        MCUHWC_GetBatteryLevel(&b.pct);
        if (R_SUCCEEDED(MCUHWC_GetBatteryVoltage(&v)))
            b.mv = v * 20u;
    }
    if (g_ptmu)
        PTMU_GetAdapterState(&b.charger);
    return b;
}
static Battery g_batStart;   /* when this recording (or the buffer) started */

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

static bool g_ledReplay;   /* the replay buffer runs: what the LED goes back to after a signal */
static void LedReplay(void);

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
    if (g_ledReplay)
        LedReplay();
    else
        LedOff();
}

/* Recording: slow red breathing like a camera's REC light (LED test variant 7). */
static void LedRecording(void)
{
    u8 r[32];
    g_ledSeqLen = 0;   /* cancels a running signal */
    g_ledReplay = false;
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

/* Replay buffer running: a faint steady blue (the recording's red breath means a file is being made). */
static void LedReplay(void)
{
    g_ledReplay = true;
    if (g_ledSeqLen)
        return;   /* a signal is playing (clip saved): LedTick sets it after that */
    if (g_cfg.led == 0)
        LedOff();
    else
        LedSolid(0, 0, 0x28);
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
                       "; screens: both, top or bottom\n"
                       "screens=both\n"
                       "; quality: auto or lossless\n"
                       "quality=auto\n"
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
        if (strcasecmp(p, "hotkey") == 0 || strcasecmp(p, "replay_hotkey") == 0)
        {
            const bool save = strcasecmp(p, "replay_hotkey") == 0;
            u32 mask = 0;
            for (char *tok = strtok(v, "+"); tok; tok = strtok(NULL, "+"))
                mask |= KeyByName(tok);
            if (mask && save)
                g_cfg.saveKey = mask;
            else if (mask)
                g_cfg.hotkey = mask;
        }
        else if (strcasecmp(p, "replay") == 0)
            g_cfg.replay = strcasecmp(v, "on") == 0;
        else if (strcasecmp(p, "fps") == 0)
        {
            const u32 fps = (u32)atoi(v);
            if (fps == 30 || fps == 20 || fps == 15)
                g_cfg.fps = fps;
        }
        else if (strcasecmp(p, "screens") == 0)
        {
            g_cfg.top = strcasecmp(v, "bottom") != 0;
            g_cfg.bottom = strcasecmp(v, "top") != 0;
        }
        else if (strcasecmp(p, "led") == 0)
            g_cfg.led = strcasecmp(v, "off") == 0 ? 0 : strcasecmp(v, "solid") == 0 ? 1 : 2;
        else if (strcasecmp(p, "audio") == 0)
            g_cfg.audio = strcasecmp(v, "off") != 0;
        else if (strcasecmp(p, "huffman") == 0)
            g_cfg.huff = strcasecmp(v, "on") == 0;
        else if (strcasecmp(p, "intra") == 0)
            g_cfg.intra = strcasecmp(v, "on") == 0;
        else if (strcasecmp(p, "quality") == 0)
            g_cfg.lossless = strcasecmp(v, "lossless") == 0;
        else if (strcasecmp(p, "c420") == 0)
            g_cfg.c420 = strcasecmp(v, "on") == 0 ? 1 : strcasecmp(v, "off") == 0 ? 0 : -1;
        else if (strcasecmp(p, "n3ds_test") == 0)
            g_cfg.n3dsTest = strcasecmp(v, "on") == 0;
        else if (strcasecmp(p, "sd_limit_kb") == 0)
            g_cfg.sdLimitKb = (u32)atoi(v);
        else if (strcasecmp(p, "codec_bench") == 0)
            g_cfg.bench = strcasecmp(v, "on") == 0;
        else if (strcasecmp(p, "ab_test") == 0)
            g_cfg.abTest = strcasecmp(v, "on") == 0;
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
static u32 g_plgHeapVa;               /* the plugin heap's address in the game */
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

/* A big buffer in 4 MB pieces at consecutive addresses: a New 3DS's kernel turned down single
   requests of ~14 MB and more even with that much free (Luma3DS-Flashback's notes). Leaves at
   least `margin` of SYSTEM memory free. Not for the plugin's memory. */
static void *BigAllocChunked(u32 size, u32 margin)
{
    const u32 piece = 4u * MB;
    if (g_inPlg || osGetMemRegionFree(MEMREGION_SYSTEM) < size + margin)
        return NULL;
    const u32 start = g_bigTop;
    for (u32 done = 0; done < size; done += piece)
    {
        const u32 n = size - done < piece ? size - done : piece;
        u32 out = 0;
        if (R_FAILED(svcControlMemoryUnsafe(&out, g_bigTop, (n + 0xFFFu) & ~0xFFFu,
                                            (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM),
                                            (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        {
            if (g_bigTop > start)
                svcControlMemoryUnsafe(&out, start, g_bigTop - start, MEMOP_FREE, (MemPerm)0);
            g_bigTop = start;
            return NULL;
        }
        g_bigTop += (n + 0xFFFu) & ~0xFFFu;
    }
    return (void *)start;
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
    volatile u32 state;   /* 0 free, 1 DMA running (its handle open), 2 DMA done (handle closed), to encode */
    Handle dma;
    u8 screen, isStatic, wide;   /* wide: the top screen's 800 px mode, 800 columns DMA'd */
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
/* With every frame C420 (no reference frames there): three top-screen frames (two in BGR8), and
   2.5 s of audio instead of 1.25: Smash's loading starved the converter for up to 3.9 s and 554 of
   5370 audio frames found the queue full (O3DS 0.12.1, 2026-11-11). */
static const u32 ARENA_CAP_PLG_C420[2] = { 3u * 400u * 240u * 2u, 2u * 320u * 240u * 2u };
#define AUDIO_QUEUE_PLG_C420 512u
#define AUDIO_QUEUE_PLG 256u   /* 128 overflowed in Smash (queue full 617 of 13792, O3DS 2026-11-08) */
static u32 g_aqCap = AUDIO_QUEUE;

static Slot   g_slots[2 * MAX_SLOTS];
static u8    *g_arena[2];
static u32    g_slotCount[2], g_slotSize[2];
static u16   *g_prev[2];
static u32    g_codecFrames[2];
static volatile u32 g_codecWords[2];

static u8    *g_ring;
/* Room past the ring's end for one whole record (normal profile, New 3DS): a record that does not
   fit before the end runs on into it and that part is copied to the ring's start (RingCommit).
   Without it the rest of the ring was padding written to the SD: 1-2% of the bytes with the 8 MB
   ring, 9-14% with a 768 KB-2 MB one (O3DS recordings, 2026-11-09). 0: pad records. */
static u32    g_ringOver;
/* the plugin's ring (extended-memory games, Old 3DS): its first g_ringOver bytes are mapped a second
   time right after its end, so a record past the end is already at the start (no copy, no extra
   memory; the 768 KB ring there wasted ~9% of the bytes on pad records, Smash O3DS 2026-11-08) */
static bool   g_ringMirror;
/* Huffman stage (codec/include/huff.h): see the packer (Stage, PackStage) */
#define HUFF_MIN_BYTES 4096u
static bool   g_intraOn;                 /* Q565 frames in this recording (EncodeFrame) */
static u32    g_intraFrames[2], g_intraMiss, g_intraBusy;
static bool   g_q565ArmBad;              /* the start-up check found the ARM encoder wrong: C is used */
static u64    g_intraBytes;   /* smaller frames: the code tables cost more than they save */
static u32    g_huffFrames, g_huffKept, g_huffBusy;   /* coded / not smaller / skipped (converter behind) */
static u64    g_huffIn, g_huffOut;
/* A/B test (config ab_test=on): each new recording takes the next variant (logged), so one
   series of recordings in one place compares them */
enum { AB_DEFAULT, AB_LOSSLESS, AB_STEPS, AB_LOSSLESS_STEPS, AB_VARIANTS };
static const char *const AB_NAMES[AB_VARIANTS] = { "default", "lossless", "throttle steps", "lossless, throttle steps (as 0.11.4)" };
/*
 * C420 frames (codec.h) in this recording: a screen whose picture changes all over (3D with a moving
 * camera) is stored at a fixed half of its size, the rest stays XR32 (exact). In a Mario Kart 7
 * race XR32 kept 98% of the top screen's bytes, so 30 + 30 frames a second were 9 MB/s for a card
 * that takes 5.4 (O3DS 2026-11-09); with C420 they are ~5.2 MB/s. The choice needs no reference
 * frame: a sample of the picture (codec_sample_changed) against the last one.
 *   into C420: C420_ENTER_PCT of the sample changed in two frames running (one busy frame, a scene
 *              cut, stays XR32);
 *   back to XR32: C420_CALM_FRAMES frames running under C420_LEAVE_PCT. The first XR32 frame after
 *              C420 is a keyframe (C420 frames are not exact, XR32 needs the exact last picture).
 */
#define C420_ENTER_PCT   40u
#define C420_LEAVE_PCT   25u
#define C420_CALM_FRAMES 8u
static bool   g_c420On;
static bool   g_c420Always;              /* every frame C420, no reference frames (g_prev is NULL) */
static bool   g_c420Mode[2];             /* this screen's frames go out as C420 now */
static u32    g_c420Hot[2], g_c420Calm[2];
static bool   g_sampled[2];              /* g_samples holds the last frame's sample */
static u32    g_samples[2][CODEC_SAMPLES(400u, 240u)];
static u32    g_c420Frames[2], g_c420Switches;
/* encode time by codec: [0] XR32 (and Q565), [1] C420; per screen */
static u64    g_codecUs[2][2];
static u32    g_codecN[2][2];
static u32    g_lastCodec;               /* the codec of the frame EncodeFrame just made */
static u64    g_waitUs[2];               /* capture start to encode start, summed: the converter's lag */
static u32    g_waitN[2];                /* frames in g_waitUs */
static u32    g_waitMaxUs[2];
static bool   g_paced;                   /* the pacer (PaceUpdate) instead of the throttle steps */
static u32    g_abNext, g_abVariant;
/* throttled bottom screen: 2 captures in every 3 (both screens; 1: 1 in 2). In Mario Kart 7 (O3DS
   2026-11-09) keeping 1 in 3 left the bottom at 7-8 fps; 1 in 2 ~10 fps; 2 in 3 ~11 fps for ~1 fps
   of the top (~22 instead of ~23.5), the one that looked best. */
static u32 g_bottomKeep = 2u;
static u32    g_writeFlags = FS_WRITE_FLUSH;
static u32    g_torn[2];   /* frames whose screen swapped again while their DMA ran (may be torn) */
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
/* Replay buffer: this recording goes round in a replay file (REPLAY_BYTES) and is never a file of
   its own unless a clip is saved. */
static bool   g_replay;
/* Where a clip can start: key frames of the last minute or so (stream offset, time, screen), at most
   one per screen in KEYMARK_GAP_US. Written by the ring's producer, read by the main thread once
   the producer is idle. */
typedef struct { u64 at, us; u32 timeUs; u8 screen; } KeyMark;
#define KEYMARKS        256u
#define KEYMARK_GAP_US  500000ull
/* In the replay buffer a screen gets a key frame at least this often, by time: XR32's come every
   KEYFRAME_EVERY frames, minutes apart on a screen that changes now and then, and a screen that
   does not change at all gets no frame (a clip must start at a key frame of each screen). */
#define REPLAY_KEY_US   4000000ull
static KeyMark g_keys[KEYMARKS];
static u32    g_keyN;
static u64    g_keyMarkUs[2];            /* time of the screen's last mark */
static bool   g_keyMarked[2];
static u64    g_keyUs[2], g_storedUs[2]; /* time of the screen's last key frame / last frame in the ring */
static bool   g_hasFrame[2];             /* the screen has a frame in the ring (g_storedUs is set) */
static bool   g_lastC420[2];             /* its last frame was C420: g_prev does not hold it, g_c420Keep does */
static u8    *g_c420Keep[2];             /* the last C420 frame's bytes (replay: stored again when the screen is still) */
static u8    *g_againBuf;                /* a copy of a reference frame while it is stored again (replay) */
static u8     g_lastFmt[2];              /* srcFormat of the screen's last record */
static u32    g_replayAgain;             /* key frames stored again for a still screen */
static u32    g_dropped, g_throttled, g_ringMax;

/* Lost video frames by reason (diagnostics, 2026-11-03: v7 dropped 170 frames
   in HOME with the ring nearly empty). DROP_RING .. DROP_DMA_ERROR are the ones
   counted in g_dropped; the last two were not counted before. */
enum { DROP_RING, DROP_NO_SLOT, DROP_DMA_TIMEOUT, DROP_DMA_ERROR, DROP_DMA_START, DROP_SLOTS_BUSY, DROP_DMA_LIMIT,
       DROP_KINDS };
static const char *const DROP_NAMES[DROP_KINDS] = { "ring full", "no free slot", "DMA timeout", "DMA error",
                                                    "DMA start failed", "slots busy (resize)", "DMA limit" };
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
    if (kind <= DROP_DMA_ERROR || kind == DROP_DMA_LIMIT)
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

/* ------------------------------------------------------------------ replay buffer's files */

#define REPLAY_FILE_BYTES (REPLAY_DATA_OFF + REPLAY_BYTES)
static const char *const REPLAY_FS[2] = { REC_DIR_FS "/.replay0.tmp", REC_DIR_FS "/.replay1.tmp" };
static volatile bool g_rpReady[2];     /* the file is there at its full size */
static u32    g_rpCur;                 /* the one the buffer uses (next) */
static Thread g_rpThread;
static volatile bool g_rpStop, g_rpDone;
static volatile bool g_rpNoSpace;      /* the card has no room for them */
static u64    g_rpRetryTick;

static u32 RingFill(void);

static bool ReplayFileWhole(u32 i)
{
    Handle h = 0;
    u64 size = 0;
    if (R_FAILED(FSUSER_OpenFile(&h, g_sd, fsMakePath(PATH_ASCII, REPLAY_FS[i]), FS_OPEN_READ, 0)))
        return false;
    FSFILE_GetSize(h, &size);
    FSFILE_Close(h);
    return size == REPLAY_FILE_BYTES;
}

/* Megabytes the replay files still need on the card (0: both are there, or the buffer is off). */
static u32 ReplayMissingMb(void)
{
    u32 mb = 0;
    for (u32 i = 0; i < 2 && g_cfg.replay; i++)
        if (!g_rpReady[i])
            mb += (u32)(REPLAY_FILE_BYTES / MB) + 1u;
    return mb;
}

/* Makes the missing replay files, lowest priority. One may be made while the buffer records into
   the other (after a clip took a file away): in small steps, and only while the ring in memory is
   nearly empty, as each step keeps the card busy. */
static void ReplayPrepMain(void *arg)
{
    (void)arg;
    for (u32 i = 0; i < 2 && !g_rpStop; i++)
    {
        if (g_rpReady[i])
            continue;
        if (ReplayFileWhole(i))
        {
            g_rpReady[i] = true;
            continue;
        }
        const FS_Path p = fsMakePath(PATH_ASCII, REPLAY_FS[i]);
        FSUSER_DeleteFile(g_sd, p);
        if (SdFreeMb() < (u32)(REPLAY_FILE_BYTES / MB) + 2 * MIN_SD_FREE_MB)
        {
            g_rpNoSpace = true;
            break;
        }
        Handle h = 0;
        if (R_FAILED(FSUSER_OpenFile(&h, g_sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
            break;
        u64 size = 0;
        while (size < REPLAY_FILE_BYTES && !g_rpStop)
        {
            if (g_recording && g_ringSize && RingFill() > g_ringSize / 4u)
            {
                svcSleepThread(50 * 1000 * 1000);
                continue;
            }
            const u64 next = size + REPLAY_STEP < REPLAY_FILE_BYTES ? size + REPLAY_STEP : REPLAY_FILE_BYTES;
            if (R_FAILED(FSFILE_SetSize(h, next)))
            {
                g_rpNoSpace = true;
                break;
            }
            size = next;
            svcSleepThread(20 * 1000 * 1000);
        }
        FSFILE_Close(h);
        if (size == REPLAY_FILE_BYTES)
            g_rpReady[i] = true;
        else
            FSUSER_DeleteFile(g_sd, p);
    }
    g_rpDone = true;
}

/* Main loop: starts the thread above when a file is missing; reaps it when it is done. */
static void ReplayPrepare(void)
{
    if (g_rpThread)
    {
        if (!g_rpDone)
            return;
        threadJoin(g_rpThread, U64_MAX);
        threadFree(g_rpThread);
        g_rpThread = NULL;
    }
    if (!g_cfg.replay || !g_sd || g_shutdownSeen || (g_rpReady[0] && g_rpReady[1]))
        return;
    if (g_rpNoSpace && Ticks() - g_rpRetryTick < 30ull * SYSCLOCK_ARM11)
        return;   /* a full card: looked at again now and then (recordings get deleted) */
    g_rpRetryTick = Ticks();
    g_rpNoSpace = false;
    g_rpDone = false;
    g_rpStop = false;
    g_rpThread = threadCreate(ReplayPrepMain, NULL, 8 * KB, 0x3F, 1, false);
}

/* The buffer was turned off (or the recorder is removed): its files go, the card gets 512 MB back. */
static void ReplayRemoveFiles(void)
{
    if (g_rpThread)
    {
        g_rpStop = true;
        threadJoin(g_rpThread, U64_MAX);
        threadFree(g_rpThread);
        g_rpThread = NULL;
    }
    for (u32 i = 0; i < 2; i++)
    {
        FSUSER_DeleteFile(g_sd, fsMakePath(PATH_ASCII, REPLAY_FS[i]));
        g_rpReady[i] = false;
    }
    g_rpNoSpace = false;
}

static void PrepareNext(void)
{
    if (g_prepThread || g_prepFile || !g_sd || g_shutdownSeen)
        return;
    const u32 freeMb = SdFreeMb();
    const u32 keep = 4 * MIN_SD_FREE_MB + ReplayMissingMb();   /* and room for the replay buffer's files */
    if (freeMb < (u32)(FIRST_ALLOC / MB) + keep)
        return;   /* nearly full card: do not reserve space in advance */
    u64 size = ((u64)(freeMb - keep) * MB) & ~(32ull * MB - 1);   /* leave 256 MB free */
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

/* Where the stream's byte `at` goes in the file. The replay buffer's file is a ring of REPLAY_BYTES
   (a multiple of every write block, and writes start at multiples of the block: none crosses its end). */
static u64 FileOffset(u64 at)
{
    return g_replay ? REPLAY_DATA_OFF + at % REPLAY_BYTES : at;
}

static void WriterMain(void *arg)
{
    (void)arg;
    u64 lastInfo = 0, lastLog = 0;
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
            if (!g_replay && g_written + g_block > g_allocated)
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
            g_writeRc = FSFILE_Write(g_file, &done, FileOffset(g_written), g_ring + (g_r % g_ringSize), g_block, g_writeFlags);
            g_written += done;
            if (g_cfg.sdLimitKb)
            {
                /* test: a slow card (the block takes at least as long as at sdLimitKb KB/s) */
                const u32 wantUs = (u32)((u64)g_block * 1000000u / ((u64)g_cfg.sdLimitKb * KB));
                const u32 tookUs = TicksToUs(Ticks() - w0);
                if (tookUs < wantUs)
                    svcSleepThread((s64)(wantUs - tookUs) * 1000);
            }
            const u64 now = Ticks();
            g_writeTicks += now - w0;
            const u32 wms = TicksToUs(now - w0) / 1000u;
            if (wms > g_writeMaxMs)
                g_writeMaxMs = wms;
            if (g_logLen && now - lastLog > 2ull * SYSCLOCK_ARM11) {   /* lines kept while capturing (Log) */
                lastLog = now;
                LogFlush();
            }
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
    /* in pieces that end where the ring in memory does (and the replay file's ring): after a card
       that stood still for long the tail can be more than a block, and one write of it read past
       the ring's end */
    while (g_w != g_r && R_SUCCEEDED(g_writeRc))
    {
        const u32 pos = (u32)(g_r % g_ringSize);
        u64 n = g_w - g_r;
        if (n > g_ringSize - pos)
            n = g_ringSize - pos;
        if (g_replay && n > REPLAY_BYTES - g_written % REPLAY_BYTES)
            n = REPLAY_BYTES - g_written % REPLAY_BYTES;
        u32 done = 0;
        g_writeRc = FSFILE_Write(g_file, &done, FileOffset(g_written), g_ring + pos, (u32)n, FS_WRITE_FLUSH);
        g_written += done;
        if (done != n && R_SUCCEEDED(g_writeRc))
            g_writeRc = (Result)0xE0000006;   /* a short write: the card is full */
        g_r += n;
    }
    g_r = g_w;
}

/* ------------------------------------------------------------------ converter */

static u32 RingFill(void) { return (u32)(g_w - g_r); }

static void RingCommit(u32 advance);

/*
 * Two stages since 0.10.1 (with the Huffman stage on): the converter (Old 3DS: on the game's core,
 * in the time the game leaves idle) packs a frame to XR32 into a staging buffer; the packer, on the
 * system core, Huffman-codes it into the ring and is the ring's only producer (audio included).
 * With both on the game's core, Huffman was left out of 2 frames in 3 in Mario Kart 7 (~35 ms a
 * frame there, O3DS 2026-11-09); the system core has time to spare too (Flashback's notes: about a
 * third of it in Mario Kart 7). Without the Huffman stage the converter writes the ring itself.
 */
typedef struct {
    u8 *buf;               /* XR32 stream */
    R3dRecHeader h;        /* as it goes into the ring (magic R3DY, len = XR32 bytes) */
    u32 reserve;           /* ring bytes held for it (record + a possible pad) */
} Stage;
#define STAGES_MAX 3u
static Stage g_stage[STAGES_MAX];
static u32 g_stageN;                     /* 0: no packer in this recording */
static volatile u32 g_stHead, g_stTail;  /* converter produces, packer consumes */
static volatile u32 g_reserved;          /* ring bytes held for staged frames */
static volatile bool g_convWaiting;      /* a frame waits for a staging buffer: Huffman gives way */
static LightEvent g_packEvent;
static u64 g_packTicks;                  /* time in the packer per frame (Huffman + copy) */

static bool PackerOn(void) { return g_stageN != 0; }
static bool StagesIdle(void) { return g_stHead == g_stTail; }

/* Ring bytes free for a new record: what the writer has not reached, less what staged frames hold. */
static u32 RingFree(void) { return g_ringSize - RingFill() - g_reserved; }

/* Writes the pad record that fills the ring's tail. */
static void PutPad(u32 pos, u32 padBytes)
{
    R3dRecHeader *pad = (R3dRecHeader *)(g_ring + pos);
    pad->magic = R3D_MAGIC_PAD;
    pad->len = padBytes - sizeof(R3dRecHeader);
    pad->timeUs = 0;
    pad->screen = 0xFF;
    pad->srcFormat = 0;
    pad->cols = 0;
}

/* Bookkeeping once a video record is in the ring at stream offset `at`. */
static void VideoStored(int sc, u64 at, const R3dRecHeader *h)
{
    g_bound[g_boundN % BOUNDS] = at;
    __atomic_store_n(&g_boundN, g_boundN + 1, __ATOMIC_RELEASE);
    /* the record's time in 64 bits: it is not later than now, and less than a wrap earlier */
    const u64 now = TicksToUs64(Ticks() - g_recStartTick);
    const u64 us = now - (u32)((u32)now - h->timeUs);
    g_storedUs[sc] = us;
    g_hasFrame[sc] = true;
    g_lastFmt[sc] = h->srcFormat;
    g_lastC420[sc] = h->magic == R3D_MAGIC_CFRAME;
    if (h->screen & R3D_SCREEN_KEY)
    {
        g_keyUs[sc] = us;
        if (g_replay && (!g_keyMarked[sc] || us - g_keyMarkUs[sc] >= KEYMARK_GAP_US))
        {
            const KeyMark k = { at, us, h->timeUs, (u8)sc };
            g_keys[g_keyN % KEYMARKS] = k;
            g_keyN++;
            g_keyMarkUs[sc] = us;
            g_keyMarked[sc] = true;
        }
    }
    if (sc == 0)
        g_shared.framesTop++;
    else
        g_shared.framesBot++;
}

/* One frame, coded into `dst` (room for an XR32 frame of its size): Q565 when most of the picture
   changed (3D with a moving camera: XR32 against the last frame saved almost nothing there), else
   XR32 against the last frame. A Q565 frame is a keyframe; the next keyframe comes KEYFRAME_EVERY
   frames after it. Converter thread. */
/* The top screen's 800 px mode: one recorded column from two of the framebuffer's, channels averaged
   (rounding down). fmt carries WIDE_FMT; the second column is g_wideStride bytes on. */
#define WIDE_FMT 0x80000000u
static u32 g_wideStride;

static int WideColumn(u16 *dst, const u8 *src, u32 fmt, u32 rows)
{
    u16 b[256];
    if (rows > 256 || !fb_column_to_rgb565(dst, src, fmt & 7u, rows) ||
        !fb_column_to_rgb565(b, src + g_wideStride, fmt & 7u, rows))
        return 0;
    for (u32 r = 0; r < rows; r++)
    {
        const u32 x = dst[r], y = b[r];
        /* per channel (r 5, g 6, b 5 bits): (x & y) + ((x ^ y) >> 1) without carries between them */
        dst[r] = (u16)((x & y) + (((x ^ y) & 0xF7DEu) >> 1));
    }
    return 1;
}

static u32 EncodeFrame(int sc, const u8 *src, u32 fmt, u32 cols, u32 stride, u8 *dst, u32 cap, u32 *magic,
                       bool *key, bool busy)
{
    const u32 rows = 240, words = cols * rows;
    const bool sameSize = g_codecWords[sc] == words;
    /* busy: more frames wait, and Q565 costs more time than XR32: XR32 for this one */
    if (g_intraOn && busy && sameSize)
        g_intraBusy++;
    else if (g_intraOn && sameSize && g_codecFrames[sc] && !(fmt & WIDE_FMT) && (fmt & 7u) == FB_RGB565 &&
             codec_changed_pct(g_prev[sc], src, cols, rows, stride) > Q565_CHANGED_PCT)
    {
        const u32 len = g_q565ArmBad ? (u32)codec_q565_encode(src, cols, rows, stride, dst, cap)
                                     : (u32)codec_q565_encode_arm(src, cols, rows, stride, dst, cap);
        if (len)
        {
            /* the next XR32 frame's reference */
            if (stride == rows * 2)
                memcpy(g_prev[sc], src, words * 2);
            else
                for (u32 c = 0; c < cols; c++)
                    memcpy(g_prev[sc] + c * rows, src + c * stride, rows * 2);
            g_codecFrames[sc] = 1;
            g_intraFrames[sc]++;
            g_intraBytes += len;
            *magic = R3D_MAGIC_QFRAME;
            *key = true;
            return len;
        }
        g_intraMiss++;   /* bigger than XR32 could be: XR32 */
    }
    if (g_c420On)
    {
        const u32 f = fmt & 7u, bpp = f == FB_RGBA8 ? 4u : f == FB_BGR8 ? 3u : 2u;
        const u32 colStride = fmt & WIDE_FMT ? 2u * stride : stride;
        const bool known = sameSize && g_sampled[sc];
        const u32 pct = g_c420Always ? 100u : codec_sample_changed(g_samples[sc], src, cols, rows, colStride, bpp, !known);
        g_sampled[sc] = true;
        if (g_c420Always)
            g_c420Mode[sc] = true;
        else if (!known)
        {
            g_c420Mode[sc] = false;
            g_c420Hot[sc] = g_c420Calm[sc] = 0;
        }
        else if (!g_c420Mode[sc])
        {
            g_c420Hot[sc] = pct > C420_ENTER_PCT ? g_c420Hot[sc] + 1 : 0;
            if (g_c420Hot[sc] >= 2)
            {
                g_c420Mode[sc] = true;
                g_c420Calm[sc] = 0;
                g_c420Switches++;
            }
        }
        else
        {
            g_c420Calm[sc] = pct < C420_LEAVE_PCT ? g_c420Calm[sc] + 1 : 0;
            if (g_c420Calm[sc] >= C420_CALM_FRAMES)
            {
                g_c420Mode[sc] = false;
                g_c420Hot[sc] = 0;
                g_c420Switches++;
            }
        }
        if (g_c420Mode[sc])
        {
            const u32 len = fmt & WIDE_FMT ? (u32)codec_c420_encode_conv(src, cols, rows, 2u * stride, fmt, WideColumn, dst)
                            : f == FB_RGB565 ? (u32)codec_c420_encode(src, cols, rows, stride, dst)
                            : f == FB_BGR8 || f == FB_RGBA8 ? (u32)codec_c420_encode_888(src, cols, rows, stride, bpp, dst)
                            : (u32)codec_c420_encode_conv(src, cols, rows, stride, fmt, fb_column_to_rgb565, dst);
            if (len)
            {
                if (g_c420Keep[sc] && len <= cols * rows)
                    memcpy(g_c420Keep[sc], dst, len);
                g_codecFrames[sc] = 0;   /* the next XR32 frame is a keyframe */
                g_codecWords[sc] = words;
                g_c420Frames[sc]++;
                g_lastCodec = 1;
                *magic = R3D_MAGIC_CFRAME;
                *key = true;
                return len;
            }
        }
    }
    if (!g_prev[sc])
    {
        /* no reference frame (g_c420Always) and C420 could not take this frame: an empty record,
           which readers skip as damaged */
        g_lastCodec = 2;
        *magic = R3D_MAGIC_CFRAME;
        *key = true;
        return 0;
    }
    g_lastCodec = 0;
    *key = !sameSize || g_codecFrames[sc] % KEYFRAME_EVERY == 0 ||
           (g_replay && TicksToUs64(Ticks() - g_recStartTick) - g_keyUs[sc] >= REPLAY_KEY_US);
    if (*key)
        g_codecFrames[sc] = 0;   /* the next one comes KEYFRAME_EVERY frames from here */
    /* another format (BGR8: HOME Menu and many games): converted a column at a time into the cache
       and encoded from there, not a pass over the whole frame through main memory first */
    u32 len;
    if (fmt & WIDE_FMT)
        len = (u32)codec_xr32_encode_conv(g_prev[sc], src, cols, rows, 2u * stride, fmt, WideColumn, *key, dst);
    else if ((fmt & 7u) == FB_RGB565)
        len = (u32)codec_xr32_encode_strided_v(g_prev[sc], src, cols, rows, stride, 1, *key, 1, dst);
    else
        len = (u32)codec_xr32_encode_conv(g_prev[sc], src, cols, rows, stride, fmt, fb_column_to_rgb565, *key, dst);
    g_codecFrames[sc]++;
    g_codecWords[sc] = words;
    *magic = R3D_MAGIC_YFRAME;
    return len;
}

/* Encodes one DMA'd frame: into a staging buffer (packer on), else straight into the ring
   (single producer). */
static void EncodeSlot(Slot *s, bool busy)
{
    const int sc = s->screen;
    const u32 cols = sc == 0 ? 400 : 320, rows = 240, words = cols * rows;
    const u32 maxLen = ((u32)codec_xr32_max_size(words) + 15u) & ~15u;
    const u32 need = sizeof(R3dRecHeader) + maxLen;
    const u32 audio = g_audioReserve ? g_audioReserveBytes : 0u;
    u32 pos = (u32)(g_w % g_ringSize);
    const u32 tail = g_ringSize - pos;
    /* with the packer the place in the ring is not known yet: room for the worst pad too */
    const u32 padBytes = !PackerOn() && tail < need && !g_ringOver ? tail : 0;
    const u32 room = PackerOn() ? (g_ringOver ? need : 2u * need) : padBytes + need;
    if (RingFree() < room + audio)
    {
        /* SD behind: video gives way, audio keeps AUDIO_RESERVE. No keyframe needed after
           a skipped frame: the encoder's reference (g_prev) is the last frame it stored. */
        Drop(DROP_RING, sc, 0);
        return;
    }
    const u8 *src = s->buf;
    u32 stride = s->stride;
    u32 fmt = s->fmt;   /* the frame's format as EncodeFrame reads it */
    if (s->wide)
    {
        /* 800 columns: XR32 from pairs of columns averaged into one (EncodeFrame), no Q565 */
        g_wideStride = stride;
        fmt |= WIDE_FMT;
    }
    else if ((fmt & 7u) != FB_RGB565 && g_intraOn)
    {
        /* BGR8 / RGBA8 / ...: pack to RGB565 in place for Q565 (output never overtakes input).
           Without Q565, XR32 converts a column at a time (EncodeFrame). */
        if (fb_to_rgb565((u16 *)s->buf, s->buf, fmt, cols, rows, s->stride, 1) != words * 2)
            return;
        stride = rows * 2;
        fmt = FB_RGB565;
    }
    R3dRecHeader hd = { R3D_MAGIC_YFRAME, 0, s->timeUs, (u8)sc, (u8)s->fmt, (u16)cols };
    u32 magic = 0;
    bool key = false;
    if (PackerOn())
    {
        Stage *st = &g_stage[g_stHead % g_stageN];
        hd.len = EncodeFrame(sc, src, fmt, cols, stride, st->buf, maxLen, &magic, &key, busy);
        hd.magic = magic;
        hd.screen = (u8)(sc | (key ? R3D_SCREEN_KEY : 0));
        st->h = hd;
        const u32 rec = sizeof(R3dRecHeader) + ((hd.len + 15u) & ~15u);
        st->reserve = g_ringOver ? rec : 2u * rec;
        __atomic_add_fetch(&g_reserved, st->reserve, __ATOMIC_SEQ_CST);
        __atomic_store_n(&g_stHead, g_stHead + 1, __ATOMIC_RELEASE);
        LightEvent_Signal(&g_packEvent);
        return;
    }
    if (padBytes)
    {
        PutPad(pos, padBytes);
        pos = 0;
    }
    u8 *payload = g_ring + pos + sizeof(R3dRecHeader);
    hd.len = EncodeFrame(sc, src, fmt, cols, stride, payload, maxLen, &magic, &key, busy);
    hd.magic = magic;
    hd.screen = (u8)(sc | (key ? R3D_SCREEN_KEY : 0));
    const u32 stored = (hd.len + 15u) & ~15u;
    memset(payload + hd.len, 0, stored - hd.len);
    *(R3dRecHeader *)(g_ring + pos) = hd;
    const u64 at = g_w + padBytes;
    if (padBytes)
        __atomic_store_n(&g_w, g_w + padBytes, __ATOMIC_RELEASE);
    RingCommit(sizeof(R3dRecHeader) + stored);
    VideoStored(sc, at, &hd);
}

/* Replay buffer, converter thread with nothing to encode: a screen whose picture has not changed
   for REPLAY_KEY_US gets its last picture again as a key frame (a still FCRAM framebuffer is never
   captured again: PollScreens), so a clip finds every screen's picture near its start. From the
   reference frame (XR32), or the kept bytes when the last frame was C420. */
static void StoreAgain(int sc)
{
    const u32 cols = sc == 0 ? 400 : 320, rows = 240, words = cols * rows;
    if (g_codecWords[sc] != words || (g_lastC420[sc] ? !g_c420Keep[sc] : !g_prev[sc] || !g_againBuf))
        return;
    const u32 maxLen = ((u32)codec_xr32_max_size(words) + 15u) & ~15u;
    const u32 need = sizeof(R3dRecHeader) + maxLen;
    u32 pos = (u32)(g_w % g_ringSize);
    const u32 tail = g_ringSize - pos;
    const u32 padBytes = tail < need && !g_ringOver ? tail : 0;
    if (RingFree() < padBytes + need + (g_audioReserve ? g_audioReserveBytes : 0u))
        return;   /* the card is behind: later */
    if (padBytes)
    {
        PutPad(pos, padBytes);
        pos = 0;
    }
    u8 *payload = g_ring + pos + sizeof(R3dRecHeader);
    R3dRecHeader hd = { R3D_MAGIC_YFRAME, 0, TicksToUs(Ticks() - g_recStartTick), (u8)(sc | R3D_SCREEN_KEY), g_lastFmt[sc],
                        (u16)cols };
    if (g_lastC420[sc])
    {
        hd.magic = R3D_MAGIC_CFRAME;
        hd.len = (u32)codec_c420_size(cols, rows);
        memcpy(payload, g_c420Keep[sc], hd.len);
    }
    else
    {
        /* a key frame of the reference's picture; from a copy: the encoder clears the reference
           first and fills it again from what it encodes */
        memcpy(g_againBuf, g_prev[sc], words * 2u);
        hd.len = (u32)codec_xr32_encode_strided_v(g_prev[sc], g_againBuf, cols, rows, rows * 2u, 1, 1, 1, payload);
        g_codecFrames[sc] = 1;
    }
    const u32 stored = (hd.len + 15u) & ~15u;
    memset(payload + hd.len, 0, stored - hd.len);
    *(R3dRecHeader *)(g_ring + pos) = hd;
    const u64 at = g_w + padBytes;
    if (padBytes)
        __atomic_store_n(&g_w, g_w + padBytes, __ATOMIC_RELEASE);
    RingCommit(sizeof(R3dRecHeader) + stored);
    VideoStored(sc, at, &hd);
    g_replayAgain++;
}

/* Packer: the oldest staged frame into the ring, Huffman-coded when that is smaller and the
   converter is not waiting for a staging buffer. */
static void PackStage(void)
{
    Stage *st = &g_stage[g_stTail % g_stageN];
    const u64 t0 = Ticks();
    const u32 raw = st->h.len, rawStored = (raw + 15u) & ~15u;
    const int sc = st->h.screen & 1;
    u32 pos = (u32)(g_w % g_ringSize);
    const u32 tail = g_ringSize - pos;
    const u32 padBytes = tail < sizeof(R3dRecHeader) + rawStored && !g_ringOver ? tail : 0;
    if (padBytes)
    {
        PutPad(pos, padBytes);
        pos = 0;
    }
    u8 *payload = g_ring + pos + sizeof(R3dRecHeader);
    R3dRecHeader hd = st->h;
    size_t coded = 0;
    const bool xr32 = hd.magic == R3D_MAGIC_YFRAME;   /* Q565 frames go in as they are */
    if (xr32 && raw >= HUFF_MIN_BYTES && !g_convWaiting)
    {
        coded = huff_encode(st->buf, raw, payload, raw - raw / 32u);   /* at least ~3% smaller */
        g_huffFrames++;
        g_huffIn += raw;
        g_huffOut += coded ? coded : raw;
        g_huffKept += coded == 0;
    }
    else if (xr32 && raw >= HUFF_MIN_BYTES)
        g_huffBusy++;
    if (coded)
    {
        hd.magic = R3D_MAGIC_HFRAME;
        hd.len = (u32)coded;
    }
    else
        memcpy(payload, st->buf, raw);
    const u32 stored = (hd.len + 15u) & ~15u;
    memset(payload + hd.len, 0, stored - hd.len);
    *(R3dRecHeader *)(g_ring + pos) = hd;
    const u64 at = g_w + padBytes;
    if (padBytes)
        __atomic_store_n(&g_w, g_w + padBytes, __ATOMIC_RELEASE);
    RingCommit(sizeof(R3dRecHeader) + stored);
    __atomic_sub_fetch(&g_reserved, st->reserve, __ATOMIC_SEQ_CST);
    VideoStored(sc, at, &hd);
    g_packTicks += Ticks() - t0;
    __atomic_store_n(&g_stTail, g_stTail + 1, __ATOMIC_RELEASE);
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
    const u32 padBytes = tail < need && !g_ringOver ? tail : 0;
    if (RingFree() < padBytes + need)
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
    /* a record that ran past the ring's end into the overhang: that part goes to the ring's start
       (free: the space check covered the whole record), before the writer may see it */
    const u32 pos = (u32)(g_w % g_ringSize);
    if (g_ringOver && !g_ringMirror && pos + advance > g_ringSize)
        memcpy(g_ring, g_ring + g_ringSize, pos + advance - g_ringSize);
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
        LightEvent_Signal(PackerOn() ? &g_packEvent : &g_convEvent);
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
        LightEvent_Signal(PackerOn() ? &g_packEvent : &g_convEvent);
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
/* The converter and the packer can each have more than one thread, on different cores (New 3DS
   test: cores 2 and 3); only the active one works, the others sleep. Switched between recordings. */
#define CONV_MAX 3u
#define PACK_MAX 2u
static Thread g_convThreads[CONV_MAX], g_packThreads[PACK_MAX];
static s32    g_convCores[CONV_MAX], g_packCores[PACK_MAX];
static u32    g_convCount, g_packCount;
static volatile u32 g_convActive, g_packActive;
static volatile u64 g_appSwapUs;   /* recording time of the last new FCRAM frame on the top screen */
static volatile bool g_appSwapSeen; /* g_appSwapUs is set (in this recording) */

/* Converter thread state for StopRecording: g_convHold makes it leave the slots alone; g_convInSlot
   is set while it handles one. */
static volatile bool g_convHold, g_convInSlot;

/* Main thread, every loop: every running DMA that finished gets its handle closed at once (each
   holds one of the ARM11's few DMA channels until then); one that failed or took too long is
   stopped and dropped. The main thread starts the DMAs and closes them; the converter only takes
   finished ones (state 2). When the converter closed them, between frames, finished DMAs held
   their channels through each encode and through a starved converter, and the DMA limit dropped
   frames in bursts mid-race (Mario Kart 7, O3DS 0.11.3, 2026-11-09). */
static void ReapDmas(void)
{
    for (u32 i = 0; i < 2 * MAX_SLOTS; i++)
    {
        Slot *s = &g_slots[i];
        if (s->state != 1)
            continue;
        DmaState st = DMASTATE_STARTING;
        const Result rc = svcGetDmaState(&st, s->dma);
        if (R_SUCCEEDED(rc) && st == DMASTATE_DONE)
        {
            svcCloseHandle(s->dma);
            s->dma = 0;
            __atomic_store_n(&s->state, 2, __ATOMIC_RELEASE);
            LightEvent_Signal(&g_convEvent);
        }
        else if (R_FAILED(rc) || TicksToUs(Ticks() - s->startTick) > DMA_TIMEOUT_US)
        {
            svcStopDma(s->dma);
            svcCloseHandle(s->dma);
            s->dma = 0;
            Drop(R_FAILED(rc) ? DROP_DMA_ERROR : DROP_DMA_TIMEOUT, s->screen, rc);   /* not encoded: no keyframe needed */
            __atomic_store_n(&s->state, 0, __ATOMIC_RELEASE);
        }
    }
}

/* One round of the converter: a wait, or one frame encoded. */
static void ConverterStep(void)
{
    if (g_convHold)
    {
        svcSleepThread(2 * 1000 * 1000);
        return;
    }
    if (!PackerOn())
        DrainAudio();
    g_convInSlot = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (g_convHold)
    {
        g_convInSlot = false;
        return;
    }
    Slot *next = NULL;
    bool running = false;
    for (u32 i = 0; i < 2 * MAX_SLOTS; i++)
    {
        Slot *s = &g_slots[i];
        running |= s->state == 1;
        /* The oldest frame first, whichever screen it is of. With the top screen's always first, a
           converter that gets little of the processor (Yo-kai Watch, O3DS 2026-11-14) left bottom
           frames waiting 0.6-2 s: they went into the file that far behind top frames of the same
           time, so the console's player took them for late and never showed them, and in the MP4
           the bottom screen ran behind the top one. The top screen keeps its lead where frames
           are captured (PollScreens: the bottom one waits behind a busy top screen). */
        if (s->state == 2 && (!next || s->startTick < next->startTick ||
                              (s->startTick == next->startTick && s->screen < next->screen)))
            next = s;
    }
    if (!next)
    {
        if (g_replay && g_recording && !PackerOn())
        {
            const u64 now = TicksToUs64(Ticks() - g_recStartTick);
            for (int sc = 0; sc < 2; sc++)
                if (g_hasFrame[sc] && now - g_storedUs[sc] >= REPLAY_KEY_US)
                    StoreAgain(sc);
        }
        g_convInSlot = false;
        if (running)
            svcSleepThread(300 * 1000);
        else
            LightEvent_WaitTimeout(&g_convEvent, 100ull * 1000 * 1000);
        return;
    }
    if (PackerOn() && g_stHead - g_stTail >= g_stageN)
    {
        /* every staging buffer full: wait for the packer (it leaves Huffman out meanwhile) */
        g_convInSlot = false;
        g_convWaiting = true;
        LightEvent_Signal(&g_packEvent);
        svcSleepThread(1000 * 1000);
        return;
    }
    g_convWaiting = false;
    /* on this core, where the frame is read: the DMA wrote around the cache */
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)next->buf, next->size);
    u32 waiting = 0;   /* other frames DMA'd or on their way */
    for (u32 i = 0; i < 2 * MAX_SLOTS; i++)
        waiting += g_slots[i].state != 0 && &g_slots[i] != next;
    const u64 t0 = Ticks();
    const u32 waited = TicksToUs(t0 - next->startTick);
    g_waitUs[next->screen] += waited;
    g_waitN[next->screen]++;
    if (waited > g_waitMaxUs[next->screen])
        g_waitMaxUs[next->screen] = waited;
    g_lastCodec = 2;   /* nothing encoded (frame dropped: ring full) */
    EncodeSlot(next, waiting >= 2);
    const u32 us = TicksToUs(Ticks() - t0);
    if (g_lastCodec < 2)
    {
        g_codecUs[g_lastCodec][next->screen] += us;
        g_codecN[g_lastCodec][next->screen]++;
    }
    if (us > g_encodeMaxUs[next->screen])
        g_encodeMaxUs[next->screen] = us;
    g_encodeSlow[next->screen] += us > 1000000u / 30u;
    g_encodeSumUs[next->screen] += us;
    __atomic_store_n(&next->state, 0, __ATOMIC_RELEASE);
    g_convInSlot = false;
    if (g_boosted)
        svcSleepThread(BOOST_YIELD_US * 1000ull);
}

/* Only one converter thread works at a time (g_convOwner): the active one can change while a
   frame is being encoded (a New 3DS program that keeps core 2 busy: main loop). */
static volatile u32 g_convOwner;

static void ConverterMain(void *arg)
{
    const u32 id = (u32)arg;
    for (;;)
    {
        if (g_convActive != id)
        {
            svcSleepThread(20 * 1000 * 1000);
            continue;
        }
        u32 nobody = 0;
        if (!__atomic_compare_exchange_n(&g_convOwner, &nobody, id + 1u, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        {
            svcSleepThread(1000 * 1000);   /* the other thread finishes its round */
            continue;
        }
        if (g_convActive == id)
            ConverterStep();
        __atomic_store_n(&g_convOwner, 0u, __ATOMIC_SEQ_CST);
    }
}

/* Packer thread (system core, lowest priority): staged frames and queued audio into the ring. */
static void PackerMain(void *arg)
{
    const u32 id = (u32)arg;
    for (;;)
    {
        if (g_packActive != id)
        {
            svcSleepThread(20 * 1000 * 1000);
            continue;
        }
        if (!PackerOn())
        {
            LightEvent_WaitTimeout(&g_packEvent, 100ull * 1000 * 1000);
            continue;
        }
        DrainAudio();
        if (g_stTail != __atomic_load_n(&g_stHead, __ATOMIC_ACQUIRE))
        {
            PackStage();
            continue;
        }
        LightEvent_WaitTimeout(&g_packEvent, 10ull * 1000 * 1000);   /* audio comes every ~5 ms: 16 queued wake it */
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
    const u32 slotSize = (size + 0xFFFu) & ~0xFFFu;   /* page-aligned DMA destinations */
    u32 n = slotSize ? g_arenaCap[sc] / slotSize : 0;
    if (n > MAX_SLOTS)
        n = MAX_SLOTS;
    const bool fits = g_slotCount[sc] && size <= g_slotSize[sc];
    /* Frames got smaller (a recording started in HOME Menu, BGR8, then a game in RGB565): the
       slots are laid out again for more of them once none is in use; until then the larger ones
       do (5 instead of 6 of the top screen's stayed for the whole recording before). */
    if (fits && n <= g_slotCount[sc])
        return true;
    for (u32 i = 0; i < g_slotCount[sc]; i++)
        if (g_slots[sc * MAX_SLOTS + i].state)
            return fits;
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

/* DMAs running at once, both screens (handles open: each holds a DMA channel; the ARM11 has 8).
   With the converter starved (the game's core busy while a game loads), both screens filled every
   slot with a DMA still open, and the kernel panicked in svcStartInterProcessDma starting the 6th
   of one screen (Mario Kart 7 launched from HOME while recording, O3DS 0.11.0-0.11.1, 2026-11-09;
   0.10 had let the bottom screen capture behind a busy top one). */
#define DMA_MAX_RUNNING 4u
static u32 g_dmaMaxRunning;

static u32 DmasRunning(void)
{
    u32 n = 0;
    for (u32 i = 0; i < 2 * MAX_SLOTS; i++)
        n += g_slots[i].state == 1;
    return n;
}

static void Capture(int sc, u32 pa, u32 fmt, u32 stride, u32 timeUs, bool isStatic)
{
    if ((fmt & 7u) > FB_RGBA4)
        return;   /* not a pixel format: a register read while the game set its screens up */
    /* the stride bounded first: a garbage register value times 400 could wrap to a small size */
    if (stride < 240 * Bpp(fmt) || stride > 240u * 4u * 2u)
        return;
    /* the top screen's 800 px mode: neither "normal 2D" (bit 6) nor 3D (bit 5) in its format
       register (as Rosalina's screenshots read it); 800 columns, recorded as 400 (pairs averaged) */
    const bool wide = sc == 0 && (fmt & 0x60u) == 0;
    const u32 size = (sc == 0 ? (wide ? 800u : 400u) : 320u) * stride;
    if (size > g_arenaCap[sc])
        return;
    if (!EnsureSlots(sc, size))
    {
        Drop(DROP_SLOTS_BUSY, sc, 0);
        return;
    }
    const u32 running = DmasRunning();
    if (running >= DMA_MAX_RUNNING)
    {
        Drop(DROP_DMA_LIMIT, sc, 0);   /* not encoded: no keyframe needed */
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
    slot->wide = wide;
    slot->size = size;
    slot->timeUs = timeUs;
    slot->startTick = Ticks();
    __atomic_store_n(&slot->state, 1, __ATOMIC_RELEASE);
    if (running + 1 > g_dmaMaxRunning)
        g_dmaMaxRunning = running + 1;
    LightEvent_Signal(&g_convEvent);
}

typedef struct {
    u32 lastAddr, lastSel, lastFmt, lastStride;
    u64 lastNewUs, lastCapUs;   /* last swap seen; last capture slot (taken or throttled) */
    u64 lastRealUs;             /* last frame actually captured */
    u64 modeUs;                 /* when its mode (format, stride, VRAM / FCRAM) last changed */
    bool vram;
    bool hasCap;
    u8 throttleN;               /* capture slots while throttled */
    bool pend;                  /* a swap since the last capture slot that was not captured */
    bool precise;               /* that swap was seen within a poll of happening (lastNewUs is its time) */
    u64 swapGapUs;              /* time between the last two swaps */
} Track;
static Track g_track[2];

static bool IsVram(u32 pa) { return pa >= VRAM_PA && pa < VRAM_PA + VRAM_SIZE; }
#define THROTTLE_HARD_PCT 85u        /* ring this full: the bottom screen pauses */
#define BOTTOM_MIN_GAP_US 100000u     /* behind a busy top screen the bottom still gets 10 fps */
#define TORN_US           4000u       /* a swap this soon after a capture started: its DMA may read a frame being drawn */
#define POLL_EARLY_US     3000u       /* wake this long before the next capture is due */
/* No DMA from a screen this long after its mode changed: the game is setting its screens up then.
   A kernel panic (rec3ds, core 1) hit right as Mario Kart 7 moved its top screen to a new VRAM
   buffer with a new stride after a launch from HOME while recording (O3DS 0.11.0, 2026-11-09). */
#define MODE_SETTLE_US    200000u
#define VBLANK_US         16713u      /* a screen refresh (59.83 Hz) */
#define LATE_AFTER_US     3000u       /* a late capture: this long after a refresh (a swap there has been seen by then) */
#define LATE_BEFORE_US    6000u       /* and this long before the next (the DMA's time and a poll) */
#define FAST_SWAP_US      30000u      /* swaps closer than this: the game draws faster than the recording */
static u64 g_lastPollUs;

/*
 * The pacer: when the SD card is behind, each screen keeps an even share of its frames, set from
 * how full the ring is and which way it moves, instead of the steps at 70% and 85% (THROTTLE_PCT:
 * all of the bottom screen's frames, then 2 in 3, then none, and the top at half rate, back and
 * forth; the picture's pace jumped between ~30 and ~20 frames a second). g_paceCut 0 = every frame;
 * a cut takes two thirds from the bottom screen and one third from the top, down to 3/8 and 1/2 of
 * their frames. Frames are dropped by an error-diffusion counter, so what stays is evenly spaced.
 */
#define PACE_FULL     256u
#define PACE_EVERY_US 100000u
#define PACE_CUT_MAX  384u
static u32 g_paceCut, g_paceAcc[2], g_paceFill, g_paceCutMax;
static u32 g_paceWaiting, g_paceByBacklog;   /* frames waiting for the converter at the last step; steps they decided */
static u64 g_paceUs, g_paceCutSum;
static u32 g_paceTicks;

static u32 PaceRate(int sc)
{
    const bool both = g_cfg.top && g_cfg.bottom;
    const u32 cut = !both ? g_paceCut / 2u : sc == 1 ? g_paceCut * 2u / 3u : g_paceCut / 3u;
    const u32 most = !both || sc == 0 ? PACE_FULL / 2u : PACE_FULL * 5u / 8u;
    return PACE_FULL - (cut < most ? cut : most);
}

static void PaceUpdate(u64 nowUs, u32 fill)
{
    /* a small ring fills sooner: the plugin's 768 KB one in ~150 ms, and 179 frames found it full
       with a step every 100 ms (Smash, O3DS 0.12.2, 2026-11-12) */
    const u32 every = g_ringSize >= 8u * MB ? PACE_EVERY_US : PACE_EVERY_US / 100u * (g_ringSize / (8u * MB / 100u) + 1u);
    if (nowUs - g_paceUs < (every < 10000u ? 10000u : every))
        return;
    g_paceUs = nowUs;
    u32 pct = fill / (g_ringSize / 100u);
    bool rising = fill > g_paceFill + g_ringSize / 64u;
    g_paceFill = fill;
    if (!g_lite)
    {
        /* The converter behind (a game that leaves it little of the processor) counts like a card
           that is behind: frames waiting to be encoded are thinned out evenly, the bottom screen's
           first, where they used to be lost in runs once every slot was taken ("no free slot": 169
           in 37 s of Yo-kai Watch with an empty ring, O3DS 2026-11-14). Eight waiting frames are as
           bad as a full ring. Not in the small-memory profiles (two slots a screen: measured there
           before it is changed). */
        u32 waiting = 0;
        for (u32 i = 0; i < 2 * MAX_SLOTS; i++)
            waiting += g_slots[i].state == 2;
        const u32 backlog = waiting >= 8u ? 100u : waiting * 100u / 8u;
        if (backlog > pct)
        {
            pct = backlog;
            rising = waiting > g_paceWaiting;
            g_paceByBacklog++;
        }
        g_paceWaiting = waiting;
    }
    const s32 step = pct > 80u ? 48 : pct > 65u ? 24 : pct > 45u && rising ? 12 : pct < 30u ? -12 : pct < 50u && !rising ? -4 : 0;
    const s32 cut = (s32)g_paceCut + step;
    g_paceCut = cut < 0 ? 0u : cut > (s32)PACE_CUT_MAX ? PACE_CUT_MAX : (u32)cut;
    if (g_paceCut > g_paceCutMax)
        g_paceCutMax = g_paceCut;
    g_paceCutSum += g_paceCut;
    g_paceTicks++;
}

/* Whether this capture of the screen is kept. */
static bool PaceTake(int sc)
{
    g_paceAcc[sc] += PaceRate(sc);
    if (g_paceAcc[sc] < PACE_FULL)
        return false;
    g_paceAcc[sc] -= PACE_FULL;
    return true;
}

/* Captures the screens that swapped (or a still VRAM one now and then). Returns how long the main
   loop may sleep: until shortly before the next capture is due, polling every POLL_REC_US from
   then on so a swap is caught within ~2 ms (the frame stays put until the game's next swap). */
static u32 PollScreens(void)
{
    const u64 nowUs = TicksToUs64(Ticks() - g_recStartTick);
    const u32 gapUs = 1000000u / g_cfg.fps - 8000u;   /* 30 fps -> 25 ms: tolerates 60 Hz jitter */
    const u32 fill = RingFill();
    u64 wakeUs = nowUs + 20000u;
    if (g_paced)
        PaceUpdate(nowUs, fill);
    for (int sc = 0; sc < 2; sc++)
    {
        if (sc == 0 ? !g_cfg.top : !g_cfg.bottom)
            continue;
        const u32 base = PDC_VA(sc);
        const u32 a = REG32(base + 0x68), b = REG32(base + 0x6C);
        const u32 fmt = REG32(base + 0x70), sel = REG32(base + 0x78) & 1u, stride = REG32(base + 0x90);
        const u32 cur = sel ? b : a;
        Track *t = &g_track[sc];
        if (fmt != t->lastFmt || stride != t->lastStride || IsVram(cur) != t->vram)   /* a mode change (800 px, 3D, a program's own) */
        {
            t->lastFmt = fmt;
            t->lastStride = stride;
            t->vram = IsVram(cur);
            t->modeUs = nowUs;
            if (!g_replay)   /* the buffer runs for hours: the log's lines are for recordings */
                Log("%s screen: format %03lX stride %lu at %s pa %08lX, %lu s", sc ? "bottom" : "top", fmt, stride,
                    IsVram(cur) ? "VRAM" : "FCRAM", cur, (u32)(nowUs / 1000000u));
        }
        const bool isNew = cur != t->lastAddr || sel != t->lastSel;
        if (isNew)
        {
            if (t->hasCap && nowUs - t->lastRealUs < TORN_US)
                g_torn[sc]++;
            t->lastAddr = cur;
            t->lastSel = sel;
            t->swapGapUs = nowUs - t->lastNewUs;
            t->lastNewUs = nowUs;
            t->pend = true;
            t->precise = nowUs - g_lastPollUs <= POLL_REC_US * 3u / 2u;
            if (sc == 0 && !IsVram(cur))
            {
                g_appSwapUs = nowUs;
                g_appSwapSeen = true;
            }
        }
        const u64 dueUs = t->hasCap ? t->lastCapUs + gapUs : nowUs;
        if (dueUs < wakeUs + POLL_EARLY_US)
            wakeUs = dueUs > POLL_EARLY_US ? dueUs - POLL_EARLY_US : 0;
        const bool due = !t->hasCap || nowUs >= dueUs;
        /* A screen that does not swap is re-captured every STATIC_GAP_US, but only from VRAM:
           re-reading an application's FCRAM framebuffer kept a closing app alive, and the next
           launch waited until the recording stopped (Homebrew Launcher -> 3DS Recorder, O3DS
           2026-11-03). A still FCRAM picture just stays the last frame in the video. */
        const bool isStatic = !isNew && IsVram(cur) && (!t->hasCap || nowUs - t->lastCapUs >= STATIC_GAP_US) &&
                              nowUs - t->lastNewUs >= STATIC_GAP_US;
        /* A game drawing faster than the recording but slower than 60 (Monster Hunter 4 on a New
           3DS, ~45 a second): every other swap came under gapUs after a capture and was let go, and
           the one after it was waited for, so the recording had 22-27 pictures a second with nothing
           dropped (New 3DS 0.12.5, 2026-11-12). Such a swap's picture is still on the screen: it is
           taken once a whole frame time has passed since the last capture. Games swap at a screen
           refresh, so it is taken only in the middle between two refreshes (counted from the swap):
           a DMA started just before the next swap would read a buffer the game draws into again. */
        const u32 phase = (u32)((nowUs - t->lastNewUs) % VBLANK_US);
        const bool late = !isNew && t->pend && t->precise && t->hasCap && nowUs - t->lastCapUs >= 1000000u / g_cfg.fps &&
                          phase >= LATE_AFTER_US && phase <= VBLANK_US - LATE_BEFORE_US;
        if (!(due && (isNew || isStatic || late)))
            continue;
        if (t->hasCap && nowUs - t->modeUs < MODE_SETTLE_US)
            continue;   /* the screen's mode just changed: let it settle */
        t->lastCapUs = nowUs;
        t->hasCap = true;
        t->pend = false;
        if (g_paced)
        {
            if (!PaceTake(sc))
            {
                g_throttled++;
                continue;
            }
        }
        else if (fill > g_ringSize / 100u * THROTTLE_PCT)
        {
            /* SD behind (no random losses, no forced keyframes): with both screens the bottom drops
               to g_bottomKeep of its rate first and the top stays at full rate; the top halves only
               when the ring is nearly full, and the bottom pauses then. Before: the bottom paused at
               once (games played on it, New Love Plus+, stuttered: Reddit 2026-11-09), then both at
               half rate (the top lost ~5 fps in Mario Kart 7); the top went from ~19 to ~25 fps this
               way (O3DS 2026-11-09). A single recorded screen runs at half rate. */
            const bool hard = fill > g_ringSize / 100u * THROTTLE_HARD_PCT;
            const bool both = g_cfg.top && g_cfg.bottom;
            const bool skip = !both               ? ++t->throttleN % 2u != 0u
                              : sc == 1           ? hard || (g_bottomKeep == 1u ? ++t->throttleN % 2u != 0u
                                                                        : ++t->throttleN % 3u == 0u)
                              : hard && ++t->throttleN % 2u != 0u;
            if (skip)
            {
                g_throttled++;
                continue;
            }
        }
        if (sc == 1 && g_cfg.top && BusySlots(0) >= 2 && nowUs - t->lastRealUs < BOTTOM_MIN_GAP_US)
            continue;   /* top screen first, but the bottom keeps 10 fps */
        t->lastRealUs = nowUs;
        Capture(sc, cur, fmt, stride, (u32)nowUs, isStatic);
    }
    g_lastPollUs = nowUs;
    /* a game faster than the recording: every poll, so its swaps' times are known (late captures) */
    for (int sc = 0; sc < 2; sc++)
        if ((sc == 0 ? g_cfg.top : g_cfg.bottom) && g_track[sc].hasCap && g_track[sc].swapGapUs < FAST_SWAP_US)
            return POLL_REC_US;
    const u64 after = TicksToUs64(Ticks() - g_recStartTick);
    return wakeUs > after + POLL_REC_US ? (u32)(wakeUs - after) : POLL_REC_US;
}

/* Once at start-up (HOME, nothing recording): the encoders' own speed on this console, without a
   game taking the processor, on a synthetic 3D-like frame (smooth shading with noise) against
   itself panned by one column. Checks the ARM Q565 encoder against the C one. config codec_bench=on.
   O3DS 2026-11-09 (at boot, the system core busy too): XR32 35 ms, Q565 C 90 ms, ARM 76 ms. */
static const char *CpuState(void);
static const char *g_benchWhat = "start-up";

static void CodecBench(void)
{
    const u32 cols = 400, rows = 240, words = cols * rows;
    const u32 cap = (u32)codec_xr32_max_size(words) + 16u;
    u16 *src = BigAlloc(words * 2), *prev = BigAlloc(words * 2), *ref = BigAlloc(words * 2);
    u8 *outA = BigAlloc(cap), *outB = BigAlloc(cap);
    if (!src || !prev || !ref || !outA || !outB)
    {
        BigFreeAll();
        Log("codec bench: no memory");
        return;
    }
    u32 seed = 1;
    for (u32 c = 0; c <= cols; c++)
        for (u32 r = 0; r < rows; r++)
        {
            seed = seed * 1103515245u + 12345u;
            const u32 n = (seed >> 16) % 8u < 3u ? (seed >> 20) & 3u : 0u;
            const u16 v = (u16)((((c + r * 3u) >> 3) + n) % 32u << 11 | ((c * 2u + r) >> 2) % 64u << 5 | ((r >> 3) + n) % 32u);
            if (c < cols)
                src[c * rows + r] = v;
            if (c > 0)
                ref[(c - 1) * rows + r] = v;
        }
    u32 best[6] = { ~0u, ~0u, ~0u, ~0u, ~0u, ~0u };
    u32 lenX = 0, lenC = 0, lenA = 0, pct = 0;
    for (u32 k = 0; k < 3; k++)
    {
        memcpy(prev, ref, words * 2);
        u64 t0 = Ticks();
        lenX = (u32)codec_xr32_encode_strided_v(prev, (const u8 *)src, cols, rows, rows * 2, 1, 0, 1, outA);
        u64 t1 = Ticks();
        if (TicksToUs(t1 - t0) < best[0]) best[0] = TicksToUs(t1 - t0);
        t0 = Ticks();
        codec_c420_encode((const u8 *)src, cols, rows, rows * 2, outA);
        t1 = Ticks();
        if (TicksToUs(t1 - t0) < best[4]) best[4] = TicksToUs(t1 - t0);
        t0 = Ticks();
        codec_c420_encode_888((const u8 *)src, 255, rows, rows * 3, 3, outA);   /* the same memory read as BGR8 (255 columns of it) */
        t1 = Ticks();
        if (TicksToUs(t1 - t0) * 400u / 255u < best[5]) best[5] = TicksToUs(t1 - t0) * 400u / 255u;
        t0 = Ticks();
        lenC = (u32)codec_q565_encode((const u8 *)src, cols, rows, rows * 2, outA, cap);
        t1 = Ticks();
        if (TicksToUs(t1 - t0) < best[1]) best[1] = TicksToUs(t1 - t0);
        t0 = Ticks();
        lenA = (u32)codec_q565_encode_arm((const u8 *)src, cols, rows, rows * 2, outB, cap);
        t1 = Ticks();
        if (TicksToUs(t1 - t0) < best[2]) best[2] = TicksToUs(t1 - t0);
        t0 = Ticks();
        pct = codec_changed_pct(ref, (const u8 *)src, cols, rows, rows * 2);
        t1 = Ticks();
        if (TicksToUs(t1 - t0) < best[3]) best[3] = TicksToUs(t1 - t0);
    }
    const bool same = lenA == lenC && memcmp(outA, outB, lenA) == 0;
    {
        /* the Huffman stage over this frame's XR32 bytes (the packer's work) */
        memcpy(prev, ref, words * 2);
        const u32 lx = (u32)codec_xr32_encode_strided_v(prev, (const u8 *)src, cols, rows, rows * 2, 1, 0, 1, outA);
        const u64 t0 = Ticks();
        const u32 lh = (u32)huff_encode(outA, lx, (u8 *)ref, words * 2);
        Log("codec bench, %s, %s: Huffman over XR32 %lu us, %lu -> %lu KB", g_benchWhat, CpuState(), TicksToUs(Ticks() - t0),
            lx / KB, lh / KB);
    }
    Log("codec bench (no game, synthetic panned frame, %lu%% changed): XR32 %lu us %lu KB; Q565 C %lu us, ARM %lu us, "
        "%lu KB (%s); C420 %lu us, from BGR8 %lu us (scaled to 400 columns); change check %lu us", pct, best[0], lenX / KB, best[1], best[2], lenA / KB,
        same ? "ARM = C" : "ARM DIFFERS FROM C", best[4], best[5], best[3]);
    if (!same)
        g_q565ArmBad = true;
    BigFreeAll();
}

/* ------------------------------------------------------------------ New 3DS test */

typedef struct {
    const char *name;
    u8 intra, c420, huff, convExtra, packExtra;
} TestVar;
/* the most telling first: a tester may make fewer recordings than there are variants */
static const TestVar TEST_VARS[] = {
    { "Q565, converter on the system core (the default so far)", 1, 0, 0, 0, 0 },
    { "Q565, converter on an extra core", 1, 0, 0, 1, 0 },
    { "Q565 + Huffman, converter and packer on extra cores", 1, 0, 1, 1, 1 },
    { "C420, converter on the system core", 0, 1, 0, 0, 0 },
    { "C420, converter on an extra core", 0, 1, 0, 1, 0 },
    { "XR32 only, converter on the system core", 0, 0, 0, 0, 0 },
    { "Q565 + Huffman, converter and packer on the system core", 1, 0, 1, 0, 0 },
};
#define TEST_VAR_COUNT (sizeof(TEST_VARS) / sizeof(TEST_VARS[0]))
static u32 g_testNext;
static const TestVar *g_testVar;   /* this recording's, NULL: not a test */
static bool g_isNew3ds;
/* A step of the test that may take the console down (threads on cores 2 and 3, the benches) leaves
   a marker file while it runs; a marker found at start-up means that step did so last time, and it
   is left out from then on: the console starts normally again after one bad start. Deleting the
   markers (or the whole /3ds/recorder folder's dot files) lets the steps run again. */
#define TEST_THREADS_FS DIR_FS "/.n3ds_test_threads"
#define TEST_BENCH_FS   DIR_FS "/.n3ds_test_bench"
static bool g_testExtraCores;   /* threads may be made on cores 2 and 3 */
static bool g_core2Behind;      /* New 3DS: frames were lost with the converter on core 2: the system core from then on */

static bool TestMarkerFound(const char *path)
{
    Handle f = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, g_sd, fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return false;
    FSFILE_Close(f);
    return true;
}

static void TestMarker(const char *path, bool on)
{
    const FS_Path p = fsMakePath(PATH_ASCII, path);
    FSUSER_DeleteFile(g_sd, p);
    Handle f = 0;
    if (!on || R_FAILED(FSUSER_OpenFile(&f, g_sd, p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0)))
        return;
    u32 done = 0;
    FSFILE_Write(f, &done, 0, "x", 1, FS_WRITE_FLUSH);
    FSFILE_Close(f);
}

/* How much of each core is free: a thread at the lowest priority on every core counts loops for
   PROBE_MS; the counts (raw, with the processor's state) are compared between a quiet console and
   a running game. "-": no thread on that core. */
#define PROBE_MS 100u
static volatile bool g_probeStop;
static volatile u32 g_probeCount[4];

static void ProbeMain(void *arg)
{
    const u32 core = (u32)arg;
    u32 n = 0;
    while (!g_probeStop)
    {
        for (volatile u32 i = 0; i < 2000u; i++)
            ;
        n++;
    }
    g_probeCount[core] = n;
}

static void IdleProbe(const char *when)
{
    Thread t[4] = { NULL, NULL, NULL, NULL };
    const u32 cores = g_isNew3ds && g_testExtraCores ? 4u : 2u;
    g_probeStop = false;
    for (u32 c = 0; c < cores; c++)
    {
        g_probeCount[c] = 0;
        t[c] = threadCreate(ProbeMain, (void *)c, 2 * KB, 0x3F, (s32)c, false);
    }
    svcSleepThread((s64)PROBE_MS * 1000 * 1000);
    g_probeStop = true;
    char line[96];
    u32 n = 0;
    for (u32 c = 0; c < cores; c++)
    {
        if (t[c])
        {
            threadJoin(t[c], 2000ull * 1000 * 1000);
            threadFree(t[c]);
            n += (u32)snprintf(line + n, sizeof(line) - n, " core %lu: %lu,", c, g_probeCount[c]);
        }
        else
            n += (u32)snprintf(line + n, sizeof(line) - n, " core %lu: -,", c);
    }
    Log("idle probe (%s, %u ms, loops at the lowest priority):%s %s", when, PROBE_MS, line, CpuState());
}

static void BenchThread(void *arg)
{
    (void)arg;
    CodecBench();
}

/* The codec bench on one core (a thread there), or a line saying no thread could be made. */
static void BenchOnCore(s32 core, const char *what)
{
    g_benchWhat = what;
    Thread t = threadCreate(BenchThread, NULL, 16 * KB, 0x30, core, false);
    if (!t)
    {
        Log("codec bench, %s: no thread on core %ld", what, core);
        return;
    }
    threadJoin(t, 20000ull * 1000 * 1000);
    threadFree(t);
}

static bool g_ptmSysm;

/* Start-up, New 3DS test: every core as the console boots (HOME Menu's processor state), then at
   804 MHz with the L2 cache, as recordings run (CpuBoost). */
static void TestBenches(void)
{
    IdleProbe("start-up");
    BenchOnCore(1, "core 1");
    if (g_testExtraCores)
    {
        BenchOnCore(2, "core 2");
        BenchOnCore(3, "core 3");
    }
    s64 mhz = 0, l2 = 0;
    if (!g_ptmSysm || R_FAILED(svcGetSystemInfo(&mhz, 0x10001, 0)) || R_FAILED(svcGetSystemInfo(&l2, 0x10001, 2)))
    {
        Log("test benches: the processor's speed cannot be changed (ptm:sysm %s)", g_ptmSysm ? "ok" : "missing");
        return;
    }
    const u8 before = (u8)((mhz > 300 ? 1 : 0) | (l2 ? 2 : 0));
    const Result rc = PTMSYSM_ConfigureNew3DSCPU(3);
    Log("test benches: 804 MHz + L2 asked for (rc %08lX): %s", (u32)rc, CpuState());
    IdleProbe("start-up, fast");
    BenchOnCore(1, "core 1, fast");
    if (g_testExtraCores)
    {
        BenchOnCore(2, "core 2, fast");
        BenchOnCore(3, "core 3, fast");
    }
    PTMSYSM_ConfigureNew3DSCPU(before);
    Log("test benches: the processor is back: %s", CpuState());
    g_benchWhat = "start-up";
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

static Thread g_writerThread, g_convThread, g_packThread;
static bool g_convOnGameCore;   /* Old 3DS: the converter runs on core 0 at the lowest priority */

static void SetBoost(bool on, const char *why)
{
    if (on == g_boosted || !g_writerThread || !g_convThread)
        return;
    g_boosted = on;
    /* on the game's core a boost would take time from the game itself: the converter stays lowest */
    if (!g_convOnGameCore)
    {
        svcSetThreadPriority(threadGetHandle(g_convThread), on ? CONVERTER_PRIO_BOOST : CONVERTER_PRIO);
        if (g_packThread)   /* New 3DS: both on the system core */
            svcSetThreadPriority(threadGetHandle(g_packThread), on ? CONVERTER_PRIO_BOOST : CONVERTER_PRIO);
    }
    svcSetThreadPriority(threadGetHandle(g_writerThread), on ? CONVERTER_PRIO_BOOST - 1 : CONVERTER_PRIO - 1);
    if (!g_replay)   /* the buffer runs for hours: the log's lines are for recordings */
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
static void CpuBoost(bool on);

static u32 g_frontPid;   /* the application that runs now, 0: none (HOME alone); UpdateBoost while recording, the main loop when idle */

static void UpdateBoost(void)
{
    static u64 lastCheck, since;
    static u32 pid;
    const u64 now = Ticks();
    const u64 recUs = TicksToUs64(now - g_recStartTick);
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
    g_frontPid = p;
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
        if (g_replay)
            ;
        else if (p)
            Log("program in front: pid %lu title %016llX at %lu s (%s)", p, (u64)tid,
                (u32)(TicksToUs64(now - g_recStartTick) / 1000000u), CpuState());
        else
            Log("program in front: none (HOME) at %lu s", (u32)(TicksToUs64(now - g_recStartTick) / 1000000u));
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
    if (m != R3D_MAGIC_FRAME && m != R3D_MAGIC_XFRAME && m != R3D_MAGIC_YFRAME && m != R3D_MAGIC_HFRAME &&
        m != R3D_MAGIC_QFRAME && m != R3D_MAGIC_CFRAME &&
        m != R3D_MAGIC_AUDIO && m != R3D_MAGIC_END)
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
        const bool delta = h.magic == R3D_MAGIC_XFRAME || h.magic == R3D_MAGIC_YFRAME || h.magic == R3D_MAGIC_HFRAME ||
                           h.magic == R3D_MAGIC_QFRAME || h.magic == R3D_MAGIC_CFRAME;
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
        {
            g_plgSize = heapSize & ~0xFFFu;
            g_plgHeapVa = heapVa;
        }
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

/* Maps the plugin ring's first `over` bytes again right after the plugin's memory (where the ring
   ends). False: not mapped (pad records as before). */
static u32 g_mirrorSize, g_mirrorOff;   /* a mirror is mapped: its size, and the ring's offset it shows */

static bool MirrorRing(u32 over)
{
    const u32 ringOff = (u32)g_ring - PLG_MEM_VA;
    if (!g_ring || !over || over > g_ringSize || ringOff + g_ringSize != g_plgSize || !g_plgHeapVa)
        return false;
    if (g_mirrorSize)
    {
        /* The last recording's mirror is still mapped (StopRecording leaves it: mapping one again
           failed with E0A01BF5 in every recording after the first in one run of Smash, O3DS
           0.12.1-0.12.2, and those went on with pad records). The same place: it serves again. */
        volatile u32 *a = (volatile u32 *)g_ring, *b = (volatile u32 *)(PLG_MEM_VA + g_plgSize);
        const u32 keep = a[0];
        a[0] = 0x4D525253u;
        const bool same = g_mirrorSize == over && g_mirrorOff == ringOff && b[0] == 0x4D525253u;
        a[0] = keep;
        if (same)
            return true;
        const Result un = svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA + g_plgSize, g_mirrorSize);
        Log("plugin ring mirror: another place now (%lu KB at %lu KB, was %lu KB at %lu KB), unmap rc %08lX", over / KB,
            ringOff / KB, g_mirrorSize / KB, g_mirrorOff / KB, (u32)un);
        g_mirrorSize = 0;
    }
    const u32 pid = ApplicationPid();
    Handle game = 0;
    if (!pid || R_FAILED(svcOpenProcess(&game, pid)))
        return false;
    Result rc = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA + g_plgSize, game, g_plgHeapVa + ringOff,
                                      over, 1);
    if (R_FAILED(rc))
    {
        /* The second recording in one run of Smash got E0A01BF5 here (O3DS 0.12.1, 2026-11-11): in
           case the last recording's mirror is still mapped there, unmap and map once more. */
        const Result first = rc;
        const Result un = svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA + g_plgSize, over);
        rc = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA + g_plgSize, game, g_plgHeapVa + ringOff, over, 1);
        Log("plugin ring mirror: map rc %08lX, unmap rc %08lX, second map rc %08lX", (u32)first, (u32)un, (u32)rc);
    }
    svcCloseHandle(game);
    if (R_FAILED(rc))
    {
        Log("plugin ring mirror not mapped (rc %08lX): pad records", (u32)rc);
        return false;
    }
    /* the mirror must show the ring's own bytes */
    volatile u32 *a = (volatile u32 *)g_ring, *b = (volatile u32 *)(PLG_MEM_VA + g_plgSize);
    const u32 keep = a[0];
    a[0] = 0x4D525252u;
    const bool same = b[0] == 0x4D525252u;
    a[0] = keep;
    if (!same)
    {
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA + g_plgSize, over);
        Log("plugin ring mirror does not show the ring: pad records");
        return false;
    }
    g_mirrorSize = over;
    g_mirrorOff = ringOff;
    return true;
}

static void UnmirrorRing(void)
{
    if (g_mirrorSize)
    {
        const Result rc = svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA + g_plgSize, g_mirrorSize);
        if (R_FAILED(rc))
            Log("plugin ring mirror unmap rc %08lX", (u32)rc);
    }
    g_mirrorSize = 0;
    g_ringMirror = false;
}

static void UnmapPluginMemory(void)
{
    if (!g_plgSize)
        return;
    UnmirrorRing();
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, PLG_MEM_VA, g_plgSize);
    g_plgSize = 0;
    g_inPlg = false;
    Log("launcher plugin memory unmapped (the game closes)");
}

static R3dFileHeader g_fileHdr;   /* this recording's file header (a clip's .r3d gets it too) */

/* replay: the replay buffer (the stream goes round in a replay file), else a recording of its own. */
static bool StartRecordingAs(bool replay)
{
    if (g_shutdownSeen)   /* powering off / rebooting: nothing new */
        return false;
    const u32 sdFree = SdFreeMb();
    if (replay)
    {
        if (!g_rpReady[g_rpCur])
            g_rpCur ^= 1u;
        if (!g_sd || !g_rpReady[g_rpCur])
            return false;   /* its files are being made (ReplayPrepare), or the card has no room */
    }
    else
    {
        g_shared.lastError = 0;
        if (!g_sd || sdFree < MIN_SD_FREE_MB + 16)
        {
            g_shared.lastError = (s32)0xE0000001;   /* SD missing or full */
            Log("start refused: SD free %lu MB", sdFree);
            return false;
        }
    }
    const u32 memMode = OS_KernelConfig->app_memtype;
    if (replay && ((memMode != 0 && memMode < 6) || osGetMemRegionFree(MEMREGION_SYSTEM) < LITE_BELOW))
    {
        /* A game that needs extra memory (Old 3DS): a ring of 1 MB or less, and HOME Menu wants the
           memory back the moment HOME is pressed. A recording there is the user's choice; the
           buffer, on all the time, stays out (and does not touch the launcher plugin's memory). */
        g_shared.replayError = REC_REPLAY_NO_MEMORY;
        return false;
    }
    g_inPlg = !replay && MapPluginMemory();
    g_plgTop = 0;
    const bool lite = g_inPlg || osGetMemRegionFree(MEMREGION_SYSTEM) < LITE_BELOW;
    g_lite = lite;
    const u32 ringMax = lite ? LITE_RING_MAX : 8u * MB;
    g_keepFree = lite ? KEEP_FREE_LITE : KEEP_FREE;
    g_aqCap = g_inPlg ? AUDIO_QUEUE_PLG : lite ? AUDIO_QUEUE_LITE : AUDIO_QUEUE;
    /* A/B test: this recording's variant (the writer is idle: it is not recording) */
    const u32 ab = g_cfg.abTest ? g_abNext++ % AB_VARIANTS : AB_DEFAULT;
    g_abVariant = ab;
    g_block = lite ? BLOCK_LITE : BLOCK;   /* 1 MB blocks: +2%, within the noise (MK7, O3DS 2026-11-09) */
    /* Q565 (g_intraOn, set above): off on an Old 3DS (intra=on for tests): in ARM assembly it still
       took ~2.2x XR32's time (start-up bench: 76 vs 35 ms without a game; ~1 frame in 3 got it in
       Mario Kart 7, the rest fell back to XR32, and the bottom screen was jerkier, O3DS 2026-11-09). */
    g_bottomKeep = 2u;
    g_paced = ab == AB_DEFAULT || ab == AB_LOSSLESS;
    g_paceCut = g_paceCutMax = g_paceFill = g_paceTicks = 0;
    g_paceWaiting = g_paceByBacklog = 0;
    g_paceAcc[0] = g_paceAcc[1] = PACE_FULL - 1u;   /* the first capture is kept */
    g_paceUs = g_paceCutSum = 0;
    g_intraFrames[0] = g_intraFrames[1] = g_intraMiss = g_intraBusy = 0;
    g_intraBytes = 0;
    g_writeFlags = FS_WRITE_FLUSH;   /* without it: +2%, within the noise (MK7, O3DS 2026-11-09) */
    s64 isN3ds = 0;
    svcGetSystemInfo(&isN3ds, 0x10000, 0x201);
    /* C420: on an Old 3DS, where the card cannot take a 3D game's frames whole; a New 3DS has the
       processor for the exact codecs (Q565), so it stays as it was until measured there. */
    g_c420On = !g_cfg.lossless && ab != AB_LOSSLESS && ab != AB_LOSSLESS_STEPS && (g_cfg.c420 >= 0 ? g_cfg.c420 != 0 : !isN3ds);
    g_intraOn = (g_cfg.intra || isN3ds) && !g_c420On;   /* New 3DS: the processor has the time (804 MHz, L2: CpuBoost) */
    /* In the launcher plugin's 2 MB every frame is C420: no reference frames (346 KB), which go to
       a third top-screen frame and a longer audio queue (ARENA_CAP_PLG_C420, AUDIO_QUEUE_PLG_C420). */
    /* New 3DS test: this recording's variant decides the codecs and the cores */
    g_testVar = NULL;
    if (g_cfg.n3dsTest && isN3ds)
    {
        for (u32 i = 0; i < TEST_VAR_COUNT && !g_testVar; i++)
        {
            const TestVar *v = &TEST_VARS[g_testNext++ % TEST_VAR_COUNT];
            if ((!v->convExtra || g_convCount > 1) && (!v->packExtra || g_packCount > 1 || g_convCount > 1))
                g_testVar = v;   /* a variant for extra cores is left out where there are none */
        }
    }
    /* New 3DS: the converter on core 2 when there is one, unless it fell behind there before */
    g_convActive = g_convCount > 1 && !g_core2Behind ? 1u : 0u;
    g_packActive = 0;
    if (g_testVar)
    {
        g_intraOn = g_testVar->intra;
        g_c420On = g_testVar->c420;
        g_paced = true;
        g_convActive = g_testVar->convExtra ? 1u : 0u;
        g_packActive = g_testVar->packExtra && g_packCount > 1 ? 1u : 0u;
    }
    g_convThread = g_convThreads[g_convActive];
    g_packThread = g_packThreads[g_packActive];
    g_c420Always = g_inPlg && g_c420On;
    if (g_c420Always)
        g_aqCap = AUDIO_QUEUE_PLG_C420;
    for (int sc = 0; sc < 2; sc++)
    {
        g_c420Mode[sc] = g_sampled[sc] = false;
        g_c420Hot[sc] = g_c420Calm[sc] = g_c420Frames[sc] = 0;
        g_waitUs[sc] = 0;
        g_waitN[sc] = 0;
        g_waitMaxUs[sc] = 0;
        g_codecUs[0][sc] = g_codecUs[1][sc] = 0;
        g_codecN[0][sc] = g_codecN[1][sc] = 0;
    }
    g_c420Switches = 0;
    /* Half in the plugin's small ring: with the full 256 KB Smash lost as much sound (queue full 376
       vs 364: the converter's stalls, not the ring) and 2.4x more video frames (ring full 851 vs 361),
       O3DS 2026-11-08. */
    g_audioReserveBytes = g_inPlg ? AUDIO_RESERVE / 2u : AUDIO_RESERVE;
    for (int sc = 0; sc < 2; sc++)
    {
        g_arenaCap[sc] = g_c420Always ? ARENA_CAP_PLG_C420[sc] : g_inPlg ? ARENA_CAP_PLG[sc] : lite ? ARENA_CAP_LITE[sc] : ARENA_CAP[sc];
        g_arena[sc] = BigAlloc(g_arenaCap[sc]);
        g_prev[sc] = g_c420Always ? NULL : BigAlloc((sc == 0 ? 400u : 320u) * 240u * 2u);
        g_c420Keep[sc] = replay && g_c420On ? BigAlloc((u32)codec_c420_size(sc == 0 ? 400u : 320u, 240u)) : NULL;
        g_slotCount[sc] = g_slotSize[sc] = 0;
        g_codecFrames[sc] = 0;
        g_codecWords[sc] = 0;
    }
    g_againBuf = replay ? BigAlloc(400u * 240u * 2u) : NULL;
    const bool prevOk = g_c420Always || (g_prev[0] && g_prev[1]);
    /* before the ring: given back with the encoder's memory (ReleaseEncoderMemory) */
    const u32 recMax = ((sizeof(R3dRecHeader) + (u32)codec_xr32_max_size(400u * 240u) + 15u) & ~15u);
    /* staging buffers for the packer (Huffman on): three in the normal profile, one in the light
       ones (no memory to spare there: the ring gets less) */
    /* not in the replay buffer: the converter is the ring's producer there (StoreAgain) */
    u32 stages = !replay && (g_testVar ? g_testVar->huff != 0 : g_cfg.huff) ? (lite ? 1u : STAGES_MAX) : 0u;
    for (u32 i = 0; i < stages; i++)
        if (!(g_stage[i].buf = BigAlloc(recMax)))
            stages = i;
    g_stHead = g_stTail = 0;
    g_reserved = 0;
    g_convWaiting = false;
    g_packTicks = 0;
    g_huffFrames = g_huffKept = g_huffBusy = 0;
    g_huffIn = g_huffOut = 0;
    g_torn[0] = g_torn[1] = 0;
    /* the overhang only in the normal profile: the light ones have no memory to spare */
    const u32 over = lite && !g_inPlg ? 0u : (recMax + 0xFFFu) & ~0xFFFu;   /* the plugin's: mirrored, no memory */
    /* 8 MB rides out ~3 s of SD stall at 2.5 MB/s */
    static const u32 ringSizes[] = { 8u * MB, 6u * MB, 4u * MB, 3u * MB, 2u * MB, 1u * MB };
    g_ring = NULL;
    void *plgAq = NULL;
    if (g_inPlg && g_arena[0] && g_arena[1] && prevOk)
    {
        /* the audio queue first, then the ring in whole write blocks up to the end of the plugin's
           memory, where its mirror goes (MirrorRing) */
        plgAq = g_cfg.audio ? BigAlloc(g_aqCap * sizeof(AudioItem)) : NULL;
        const u32 left = g_plgSize - g_plgTop;
        const u32 size = (left / g_block) * g_block;
        if (size >= 4u * g_block && (!g_cfg.audio || plgAq))
        {
            g_plgTop = g_plgSize - size;
            g_ring = BigAlloc(size);
            g_ringSize = size;
            g_ringMirror = MirrorRing(over);
        }
    }
    /* New 3DS: ~60 MB of SYSTEM memory free with a game running (tester's logs, 2026-11-08); a
       bigger ring rides out a slow card's long spells (Flashback's notes: ~220 frames a minute lost
       on a SanDisk Ultra with ~3 MB, none with ~25 MB). 24 MB stays free for HOME and applets. */
    static const u32 bigRings[] = { 24u * MB, 16u * MB };
    for (u32 i = 0; i < 2 && isN3ds && !lite && !g_ring && g_arena[0] && g_arena[1] && prevOk; i++)
        if ((g_ring = BigAllocChunked(bigRings[i] + over, 24u * MB)) != NULL)
            g_ringSize = bigRings[i];
    for (u32 i = 0; i < sizeof(ringSizes) / sizeof(ringSizes[0]) && !g_ring && g_arena[0] && g_arena[1] && prevOk; i++)
        if (ringSizes[i] <= ringMax && (g_ring = BigAlloc(ringSizes[i] + over)) != NULL)
            g_ringSize = ringSizes[i];
    g_ringOver = !g_ring ? 0u : g_ringMirror ? over : !g_inPlg ? over : 0u;
    if (!g_ring)
    {
        BigFreeAll();
        if (replay)
            g_shared.replayError = REC_REPLAY_NO_MEMORY;
        else
            g_shared.lastError = (s32)0xE0000002;   /* not enough SYSTEM memory */
        if (!replay)   /* the buffer tries again every few seconds: no line each time */
            Log("start refused: SYSTEM free %lu KB", osGetMemRegionFree(MEMREGION_SYSTEM) / KB);
        return false;
    }
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_track, 0, sizeof(g_track));
    g_lastPollUs = 0;
    g_aq = !g_cfg.audio ? NULL : g_inPlg ? plgAq : BigAlloc(g_aqCap * sizeof(AudioItem));
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

    g_file = replay ? 0 : TakePrepared();
    const bool prepared = g_file != 0;
    if (replay)
    {
        if (R_FAILED(FSUSER_OpenFile(&g_file, g_sd, fsMakePath(PATH_ASCII, REPLAY_FS[g_rpCur]), FS_OPEN_WRITE, 0)))
        {
            g_file = 0;
            g_rpReady[g_rpCur] = false;   /* gone (the card was changed, the file deleted): made again */
            BigFreeAll();
            return false;
        }
    }
    else if (!g_file)
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
    g_allocated = replay ? REPLAY_FILE_BYTES : prepared ? g_prepReached : FIRST_ALLOC;
    g_replay = replay;
    g_keyN = g_replayAgain = 0;
    for (int sc = 0; sc < 2; sc++)
    {
        g_keyMarked[sc] = g_hasFrame[sc] = g_lastC420[sc] = false;
        g_keyUs[sc] = g_storedUs[sc] = 0;
    }
    g_growCount = g_growMaxMs = 0;
    g_writeTicks = 0;
    g_writeMaxMs = 0;
    g_written = 0;
    g_writeRc = 0;
    g_w = g_r = 0;
    g_dropped = g_throttled = g_ringMax = 0;
    memset(g_drops, 0, sizeof(g_drops));
    g_dropBurstCount = g_dropBurstsLost = 0;
    g_dmaMaxRunning = 0;
    g_convHold = false;
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
    fh->isNew3ds = isN3ds != 0;
    fh->mode = !g_cfg.bottom ? R3D_MODE_FULL_TOP : !g_cfg.top ? R3D_MODE_FULL_BOTTOM : R3D_MODE_FULL_BOTH;
    fh->fps = (u8)g_cfg.fps;
    fh->audioRate = g_aq ? R3D_AUDIO_RATE_MHZ : 0;
    g_recStartMs = osGetTime();
    fh->startTimeMs = g_recStartMs;
    g_fileHdr = *fh;
    g_w = sizeof(*fh);
    if (!replay)   /* a buffer cut off by power loss is not a recording to recover */
        OpenInfo();

    CpuBoost(true);
    g_batStart = BatteryNow();
    g_recStartTick = Ticks();
    g_stageN = stages;   /* the packer takes over the ring (audio too) */
    g_writerActive = true;
    g_recording = true;
    g_shared.ringKb = g_ringSize / KB;
    g_shared.sysFreeKb = osGetMemRegionFree(MEMREGION_SYSTEM) / KB;
    g_shared.state = replay ? REC_STATE_REPLAY : REC_STATE_RECORDING;
    if (replay)
    {
        g_shared.replayError = REC_REPLAY_OK;
        LedReplay();
    }
    else
        LedRecording();
    Log("%s started (%sfile %llu MB%s, PDN_DSP_CNT %02X, fps %lu, %s, audio %s, ring %lu KB, %s memory profile%s, "
        "SYSTEM free %lu KB, SD free %lu MB, %s)",
        replay ? "replay buffer" : "recording", g_cfg.audioDebug ? "AUDIO DEBUG: no video, raw DSP polls; " : "", g_allocated / MB, prepared ? " prepared" : "",
        *(const volatile u8 *)PDN_DSP_CNT_VA, g_cfg.fps, ScreensName(),
        g_aq ? "on" : g_cfg.audio ? "off (no memory)" : "off", g_ringSize / KB, lite ? "light" : "normal",
        g_inPlg ? " in the launcher plugin's memory" : "",
        g_shared.sysFreeKb, sdFree,
        CpuState());
    if (!replay)
        LogMemory("recording started");
    if (g_testVar)
    {
        Log("test variant %lu of %u: %s; converter on core %ld, packer on core %ld", (g_testNext - 1) % TEST_VAR_COUNT + 1,
            (unsigned)TEST_VAR_COUNT, g_testVar->name, g_convCores[g_convActive], g_packCores[g_packActive]);
        IdleProbe("recording starts");
    }
    return true;
}

static bool StartRecording(void) { return StartRecordingAs(false); }

/* New 3DS: the clock and L2 the running program chose (the converter's speed depends on them). */
/* New 3DS: 804 MHz and the L2 cache while recording (Rosalina's New 3DS menu changes them the same
   way while a game runs); what was set before comes back when the recording stops. A game in
   legacy mode runs at 268 MHz without L2, and the converter shares the system core there.
   ptm:sysm (rec3ds.rsf). */
static s32  g_cpuBefore = -1;   /* PTMSYSM_ConfigureNew3DSCPU value to restore, -1: nothing changed */

static void CpuBoost(bool on)
{
    if (!g_ptmSysm)
        return;
    if (on && g_cpuBefore < 0)
    {
        s64 mhz = 0, l2 = 0;
        if (R_FAILED(svcGetSystemInfo(&mhz, 0x10001, 0)) || R_FAILED(svcGetSystemInfo(&l2, 0x10001, 2)))
            return;
        const s32 before = (mhz > 300 ? 1 : 0) | (l2 ? 2 : 0);
        if (before == 3)
            return;   /* the game runs that way already */
        if (R_SUCCEEDED(PTMSYSM_ConfigureNew3DSCPU(3)))
            g_cpuBefore = before;
    }
    else if (!on && g_cpuBefore >= 0)
    {
        PTMSYSM_ConfigureNew3DSCPU((u8)g_cpuBefore);
        g_cpuBefore = -1;
    }
}

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
        "slots busy %lu, DMA limit %lu (most running at once %lu); encode avg %lu / %lu us, max %lu / %lu us, over 33 ms %lu / %lu (top / bottom); %s",
        g_drops[DROP_RING], g_drops[DROP_NO_SLOT], g_drops[DROP_DMA_TIMEOUT], g_drops[DROP_DMA_ERROR],
        g_drops[DROP_DMA_START], (u32)g_dmaStartRc, g_drops[DROP_SLOTS_BUSY], g_drops[DROP_DMA_LIMIT], g_dmaMaxRunning,
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

/* Where a clip of the buffer's last REPLAY_CLIP_US starts: for each screen its last key frame not
   later than that (else its first one the file still holds), and the earlier of the two in the
   stream, so each screen's picture is known from the clip's start (r3d_unwrap drops what comes
   before a screen's key frame). false: no key frame in the file yet. */
static bool ReplayClipStart(u64 end, u64 endUs, KeyMark *out)
{
    const u64 low = end > REPLAY_BYTES ? end - REPLAY_BYTES : sizeof(R3dFileHeader);
    const u64 target = endUs > REPLAY_CLIP_US ? endUs - REPLAY_CLIP_US : 0;
    const u32 n = g_keyN < KEYMARKS ? g_keyN : KEYMARKS;
    bool found = false;
    for (u32 sc = 0; sc < 2; sc++)
    {
        const KeyMark *best = NULL;
        for (u32 i = 0; i < n; i++)
        {
            const KeyMark *k = &g_keys[i];
            if (k->screen != sc || k->at < low || k->at >= end)
                continue;
            if (!best || (k->us <= target ? best->us > target || k->us > best->us : best->us > target && k->us < best->us))
                best = k;
        }
        if (best && (!found || best->at < out->at))
        {
            *out = *best;
            found = true;
        }
    }
    return found;
}

static bool g_saveClip;      /* this stop of the replay buffer saves a clip (else the buffer is just left) */
static bool g_replayFailed;  /* the card refused the buffer's writes: off until the settings are read again */

/* The replay buffer stops (its tail is on the card). A clip: the ring's header goes to the file's
   start and the file gets a recording's name with .rpl (the app makes the .r3d: r3d_unwrap); the
   buffer goes on in the other file. Otherwise the file stays as it is for the next round. */
static bool CloseReplay(bool save, u64 endUs, char *name, u32 nameSize)
{
    bool ok = false;
    KeyMark k;
    if (save && R_SUCCEEDED(g_writeRc) && g_w == g_r && ReplayClipStart(g_written, endUs, &k))
    {
        R3dRingInfo info;
        memset(&info, 0, sizeof(info));
        info.magic = R3D_MAGIC_RING;
        info.version = R3D_RING_VERSION;
        info.dataOff = REPLAY_DATA_OFF;
        info.startUs = k.timeUs;
        info.ringBytes = REPLAY_BYTES;
        info.start = k.at;
        info.end = g_written;
        info.endUs = (u32)endUs;
        info.file = g_fileHdr;
        info.file.startTimeMs = g_recStartMs + k.us / 1000u;
        u32 done = 0;
        ok = R_SUCCEEDED(FSFILE_Write(g_file, &done, 0, &info, sizeof(info), FS_WRITE_FLUSH)) && done == sizeof(info);
        if (ok)
        {
            FormatName(name, nameSize, info.file.startTimeMs);
            const size_t len = strlen(name);
            if (len > 3)
                memcpy(name + len - 3, "rpl", 3);   /* .r3d -> .rpl: the app's lists of recordings leave it out */
        }
    }
    FSFILE_Close(g_file);
    g_file = 0;
    if (ok)
    {
        char path[96];
        snprintf(path, sizeof(path), REC_DIR_FS "/%s", name);
        ok = R_SUCCEEDED(FSUSER_RenameFile(g_sd, fsMakePath(PATH_ASCII, REPLAY_FS[g_rpCur]), g_sd, fsMakePath(PATH_ASCII, path)));
        if (ok)
        {
            g_rpReady[g_rpCur] = false;   /* ReplayPrepare makes a new one */
            g_rpCur ^= 1u;
        }
    }
    return ok;
}

static void StopRecording(const char *why, bool releaseFirst)
{
    const bool replay = g_replay, saveClip = g_replay && g_saveClip;
    const u64 endUs = TicksToUs64(Ticks() - g_recStartTick);
    g_saveClip = false;
    g_shared.state = REC_STATE_SAVING;
    g_recording = false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* pending DMAs finish or time out, the converter empties the slots, the packer the stages */
    for (u32 i = 0; i < 400 && (!SlotsIdle() || !StagesIdle()); i++)
    {
        ReapDmas();   /* this thread closes finished DMAs */
        LightEvent_Signal(&g_packEvent);
        svcSleepThread(5 * 1000 * 1000);
    }
    if (!SlotsIdle())
    {
        /* the converter is starved (the game's core busy): it leaves the slots alone from here, and
           DMAs still running are stopped before their buffers are given back (they used to be freed
           with the DMA still writing into them, the handles left open) */
        g_convHold = true;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for (u32 i = 0; i < 2000 && g_convInSlot; i++)
            svcSleepThread(5 * 1000 * 1000);
        u32 stopped = 0;
        for (u32 i = 0; i < 2 * MAX_SLOTS; i++)
        {
            Slot *sl = &g_slots[i];
            if (sl->state == 1 && sl->dma)
            {
                svcStopDma(sl->dma);
                svcCloseHandle(sl->dma);
                sl->dma = 0;
                stopped++;
            }
            sl->state = 0;
        }
        Log("stop: converter starved, %lu DMAs stopped, slots cleared%s", stopped,
            g_convInSlot ? " (converter still inside a frame)" : "");
    }
    /* the replay buffer's converter may be storing a still screen's picture again (StoreAgain) */
    for (u32 i = 0; i < 2000 && replay && g_convInSlot; i++)
        svcSleepThread(5 * 1000 * 1000);
    while (g_audioInPoll)   /* the audio thread sees g_recording == false from its next poll */
        svcSleepThread(1 * 1000 * 1000);
    for (u32 i = 0; i < 100 && g_aq && g_aqTail != g_aqHead; i++)   /* queued audio into the ring */
    {
        LightEvent_Signal(&g_convEvent);
        LightEvent_Signal(&g_packEvent);
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
    if (releaseFirst && SlotsIdle() && StagesIdle())
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
            g_ringOver = 0;
            ringDropped = true;
        }
        Log("HOME: recording memory given back at once (ring %s), SYSTEM free %lu KB",
            ringDropped ? "dropped" : "still being written", osGetMemRegionFree(MEMREGION_SYSTEM) / KB);
    }
    if (replay && !saveClip)
    {
        /* the buffer is just left: what is still in memory need not reach the card */
        g_writerActive = false;
        while (g_writerBusy)
            svcSleepThread(2 * 1000 * 1000);
        g_r = g_w;
    }
    /* writer: whole blocks first, then the tail from here */
    for (u32 i = 0; i < 2000 && !ringDropped && g_w - g_r >= g_block; i++)
        svcSleepThread(5 * 1000 * 1000);
    g_writerActive = false;
    while (g_writerBusy)   /* a block write may take ~0.3 s */
        svcSleepThread(5 * 1000 * 1000);
    if (!ringDropped)
        WriterFlushTail();
    char name[32] = "";
    Result rn = 0;
    const u32 seconds = (u32)(endUs / 1000000u);
    const Battery batEnd = BatteryNow();
    if (replay)
    {
        const bool clip = CloseReplay(saveClip, endUs, name, sizeof(name));
        Log("replay buffer stopped: %lu s, %llu KB, frames %lu + %lu (%lu stored again), dropped %lu (ring full %lu, "
            "no free slot %lu, DMA limit %lu, DMA timeout %lu), throttled %lu, ring max %lu KB, slowest block %lu ms, "
            "encode avg %lu / %lu us, pacer cut %lu on average (most %lu, %lu of %lu steps by the converter's backlog), "
            "battery %u%% -> %u%% (%lu -> %lu mV%s), write rc %08lX, %s%s, reason: %s",
            seconds, g_written / KB, g_shared.framesTop, g_shared.framesBot, g_replayAgain, g_dropped, g_drops[DROP_RING],
            g_drops[DROP_NO_SLOT], g_drops[DROP_DMA_LIMIT], g_drops[DROP_DMA_TIMEOUT], g_throttled,
            g_ringMax / KB, g_writeMaxMs,
            g_shared.framesTop ? (u32)(g_encodeSumUs[0] / g_shared.framesTop) : 0u,
            g_shared.framesBot ? (u32)(g_encodeSumUs[1] / g_shared.framesBot) : 0u,
            g_paceTicks ? (u32)(g_paceCutSum / g_paceTicks) : 0u, g_paceCutMax, g_paceByBacklog, g_paceTicks,
            g_batStart.pct, batEnd.pct, g_batStart.mv, batEnd.mv, g_batStart.charger || batEnd.charger ? ", ON THE CHARGER" : "",
            (u32)g_writeRc, clip ? "clip saved: " : saveClip ? "CLIP NOT SAVED" : "no clip",
            clip ? name : "", why);
        if (clip)
        {
            snprintf((char *)g_shared.lastClip, sizeof(g_shared.lastClip), "%s", name);
            g_shared.clips++;
        }
        if (R_FAILED(g_writeRc))
        {
            g_replayFailed = true;
            g_shared.replayError = REC_REPLAY_WRITE_ERROR;
        }
        g_ledReplay = false;
        if (saveClip || R_FAILED(g_writeRc))
        {
            if (clip)
                LED_SIGNAL(LED_SAVED);
            else
                LED_SIGNAL(LED_ERROR);
        }
        else if (!g_ledSeqLen)
            LedOff();
        CpuBoost(false);
        SetBoost(false, "replay buffer stopped");
        g_replay = false;
        g_ring = NULL;
        g_stageN = 0;
        g_ringOver = 0;
        g_ringMirror = false;
        BigFreeAll();
        g_appPid = g_appBase = 0;
        g_convHold = false;
        g_shared.state = REC_STATE_IDLE;
        return;
    }
    WriteEnd();

    FormatName(name, sizeof(name), g_recStartMs);
    char path[96];
    snprintf(path, sizeof(path), REC_DIR_FS "/%s", name);
    FSFILE_SetSize(g_file, g_written);
    FSFILE_Close(g_file);
    g_file = 0;
    rn = FSUSER_RenameFile(g_sd, fsMakePath(PATH_ASCII, NEXT_FS), g_sd, fsMakePath(PATH_ASCII, path));
    CloseInfo();   /* saved: nothing to recover */
    if (g_testVar)
        IdleProbe("recording ends");
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
    {
        const u32 packed = g_huffFrames + g_huffBusy, packUs = TicksToUs(g_packTicks);
        Log("compression: Q565 %lu + %lu frames (%llu KB, %lu not smaller, %lu left to XR32: converter behind)",
            g_intraFrames[0], g_intraFrames[1], g_intraBytes / KB, g_intraMiss, g_intraBusy);
        Log("compression: %s, %lu staging buffers, Huffman %lu frames (%lu kept as XR32, %lu skipped: converter "
            "waiting) %llu -> %llu KB (%lu%%), packer %lu us a frame; ring %lu KB + overhang %lu KB, write block %lu KB%s; "
            "maybe-torn frames %lu / %lu",
            AB_NAMES[g_abVariant], g_stageN, g_huffFrames, g_huffKept, g_huffBusy, g_huffIn / KB, g_huffOut / KB,
            g_huffIn ? (u32)(g_huffOut * 100u / g_huffIn) : 0u, packed ? packUs / packed : 0u, g_ringSize / KB,
            g_ringOver / KB, g_block / KB, g_writeFlags ? "" : " (no flush)", g_torn[0], g_torn[1]);
#define AVG(sum, n) ((n) ? (u32)((sum) / (n)) : 0u)
        Log("codecs: C420 %s, %lu + %lu frames (top / bottom), %lu switches; encode XR32 %lu / %lu us (%lu + %lu frames), "
            "C420 %lu / %lu us; capture to encode %lu / %lu ms on average, longest %lu / %lu ms",
            g_c420On ? "on" : "off", g_c420Frames[0], g_c420Frames[1], g_c420Switches,
            AVG(g_codecUs[0][0], g_codecN[0][0]), AVG(g_codecUs[0][1], g_codecN[0][1]), g_codecN[0][0], g_codecN[0][1],
            AVG(g_codecUs[1][0], g_codecN[1][0]), AVG(g_codecUs[1][1], g_codecN[1][1]),
            AVG(g_waitUs[0], g_waitN[0]) / 1000u, AVG(g_waitUs[1], g_waitN[1]) / 1000u,
            g_waitMaxUs[0] / 1000u, g_waitMaxUs[1] / 1000u);
        if (g_paced)
            Log("pacer: frames kept at the end %lu%% / %lu%% (top / bottom), cut %lu on average, most %lu of %u; "
                "%lu of %lu steps by the converter's backlog",
                PaceRate(0) * 100u / PACE_FULL, PaceRate(1) * 100u / PACE_FULL, AVG(g_paceCutSum, g_paceTicks), g_paceCutMax,
                PACE_CUT_MAX, g_paceByBacklog, g_paceTicks);
        else
            Log("pacer: off (throttle steps at %u%% and %u%%)", THROTTLE_PCT, THROTTLE_HARD_PCT);
#undef AVG
    }
    LogDrops();
    Log("battery %u%% -> %u%% (%lu -> %lu mV%s) in %lu s", g_batStart.pct, batEnd.pct, g_batStart.mv, batEnd.mv,
        g_batStart.charger || batEnd.charger ? ", ON THE CHARGER" : "", seconds);
    LogMemory("recording stopped");
    CpuBoost(false);
    SetBoost(false, "recording stopped");
    snprintf((char *)g_shared.lastFile, sizeof(g_shared.lastFile), "%s", name);
    g_shared.files++;
    if (R_FAILED(g_writeRc) || R_FAILED(rn))
        g_shared.lastError = R_FAILED(g_writeRc) ? g_writeRc : rn;

    g_ring = NULL;
    g_stageN = 0;   /* the stages are empty (waited for above) */
    g_ringOver = 0;
    g_ringMirror = false;   /* the mapping stays for the next recording (MirrorRing); UnmapPluginMemory removes it */
    BigFreeAll();
    g_appPid = g_appBase = 0;
    g_convHold = false;
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

/* The app is on the screen: it adds to g_shared.appBeat every second or so while it runs in front.
   The replay buffer pauses then (the app's player, its MP4 conversion and its Wi-Fi transfer need
   the card and the processor, and nobody wants a clip of the app). An app that was closed, sent to
   the background or crashed stops adding. Called every round of the main loop. */
static bool AppOnScreen(void)
{
    static u32 last;
    static u64 at;
    static bool seen;
    const u32 beat = g_shared.appBeat;
    const u64 now = Ticks();
    if (beat != last)
    {
        last = beat;
        at = now;
        seen = true;
    }
    return seen && now - at < REPLAY_BEAT_US * (SYSCLOCK_ARM11 / 1000000u);
}

int main(void)
{
    svcGetProcessId(&g_ownPid, CUR_PROCESS_HANDLE);
    LightEvent_Init(&g_convEvent, RESET_ONESHOT);
    LightEvent_Init(&g_packEvent, RESET_ONESHOT);
    LightLock_Init(&g_dropLock);
    LightLock_Init(&g_logLock);
    g_logLockOk = true;
    InitFs();
    LoadConfig();
    InitLed();
    g_shared.initStage = 4;

    /* the writer sits just above the converter: it mostly waits on the SD, and a starved writer
       fills the ring */
    Thread writer = threadCreate(WriterMain, NULL, 8 * KB, CONVERTER_PRIO - 1, 1, false);
    /* Old 3DS: the system core has little time to spare while a game runs (Smash: ~22 + 11 ms of
       encoding per pair of frames there, "no free slot" drops). On the game's core, at the lowest
       priority, the converter takes only the time the game's own threads leave idle while they
       wait for the GPU or the next screen refresh. (The idea comes from Luma3DS-Flashback's notes;
       no code from it.) The DMA, the writer and the audio stay on the system core. New 3DS: the
       system core keeps up; a game can also leave its core with little idle time there. */
    s64 isNew3ds = 0;
    svcGetSystemInfo(&isNew3ds, 0x10000, 0x201);
    Thread conv = NULL;
    g_isNew3ds = isNew3ds != 0;
    if (!isNew3ds && (conv = threadCreate(ConverterMain, (void *)0, 8 * KB, CONVERTER_PRIO, 0, false)) != NULL)
        g_convOnGameCore = true;
    if (!conv)
        conv = threadCreate(ConverterMain, (void *)0, 8 * KB, CONVERTER_PRIO, 1, false);
    g_writerThread = writer;
    g_convThread = conv;
    g_convThreads[0] = conv;
    g_convCores[0] = g_convOnGameCore ? 0 : 1;
    g_convCount = 1;
    /* the packer on the system core at the converter's priority (the writer stays above it) */
    Thread pack = threadCreate(PackerMain, (void *)0, 8 * KB, CONVERTER_PRIO, 1, false);
    g_packThread = pack;
    g_packThreads[0] = pack;
    g_packCores[0] = 1;
    g_packCount = 1;
    char extra[96] = "";
    const bool threadsFailed = isNew3ds && TestMarkerFound(TEST_THREADS_FS);
    if (threadsFailed)
        snprintf(extra, sizeof(extra), " left out: the console went down making them last time (" TEST_THREADS_FS ")");
    if (isNew3ds && !threadsFailed)
    {
        TestMarker(TEST_THREADS_FS, true);
        /* New 3DS: a second converter on core 2, which games leave idle (rsf CanAccessCore2); recordings
           use it (StartRecording). There Q565 took 4.7-5.9 ms a top-screen frame against 7.7-8.7 ms on
           the system core, which a game's own system calls keep ~40% busy (New 3DS XL 0.12.5,
           2026-11-12: 21 recordings in three programs, nothing dropped on either core). The test
           (n3ds_test) also tries core 3, and a second packer on core 3 (else 2). */
        u32 n = 0;
        for (s32 core = 2; core <= (g_cfg.n3dsTest ? 3 : 2) && g_convCount < 2; core++)
        {
            Thread t = threadCreate(ConverterMain, (void *)1, 8 * KB, CONVERTER_PRIO, core, false);
            n += (u32)snprintf(extra + n, sizeof(extra) - n, " converter core %ld %s;", core, t ? "ok" : "refused");
            if (t)
            {
                g_convThreads[1] = t;
                g_convCores[1] = core;
                g_convCount = 2;
            }
        }
        for (s32 core = 3; g_cfg.n3dsTest && core >= 2 && g_packCount < 2; core--)
        {
            Thread t = threadCreate(PackerMain, (void *)1, 8 * KB, CONVERTER_PRIO, core, false);
            n += (u32)snprintf(extra + n, sizeof(extra) - n, " packer core %ld %s;", core, t ? "ok" : "refused");
            if (t)
            {
                g_packThreads[1] = t;
                g_packCores[1] = core;
                g_packCount = 2;
            }
        }
        svcSleepThread(100 * 1000 * 1000);   /* the new threads have run by now */
        TestMarker(TEST_THREADS_FS, false);
        g_testExtraCores = true;
        g_convActive = g_convCount > 1 ? 1u : 0u;   /* from the start: no switch with a frame on its way */
    }
    /* highest non-privileged priority: a poll late by more than ~6 ms loses a frame
       (v5: ~1 per second at 0x20); it only reads 1.3 KB per ms */
    Thread audio = threadCreate(AudioMain, NULL, 4 * KB, 0x18, 1, false);
    if (!writer || !conv || !audio || !pack)
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
        "fps %lu, screens %s, audio %s, file limit %lu MB, SYSTEM free %lu KB, SD free %lu MB, converter on core %d, "
        "FS priority %ld -> %ld%s%s)",
        n3ds ? "New 3DS" : "Old 3DS", (u32)(luma >> 24) & 0xFF, (u32)(luma >> 16) & 0xFF, (u32)(luma >> 8) & 0xFF,
        GET_VERSION_MAJOR(kv), GET_VERSION_MINOR(kv), GET_VERSION_REVISION(kv), g_cfg.hotkey, g_cfg.fps,
        ScreensName(), g_cfg.audio ? "on" : "off", g_cfg.maxFileMb, osGetMemRegionFree(MEMREGION_SYSTEM) / KB,
        SdFreeMb(), (int)g_convCores[g_convActive], (s32)g_fsPrioOld, (s32)g_fsPrioNew, g_cfg.huff ? "" : ", Huffman off",
        g_cfg.abTest ? ", A/B test" : "");
    if (n3ds)
        g_ptmSysm = R_SUCCEEDED(ptmSysmInit());
    if (n3ds && g_cfg.n3dsTest)
    {
        Log("New 3DS test build: extra threads:%s", extra[0] ? extra : " none");
        if (TestMarkerFound(TEST_BENCH_FS))
            Log("test benches left out: the console went down in them last time (" TEST_BENCH_FS ")");
        else
        {
            TestMarker(TEST_BENCH_FS, true);
            TestBenches();
            TestMarker(TEST_BENCH_FS, false);
        }
    }
    else if (g_cfg.bench)
        CodecBench();
    const bool resumeInGame = AfterReboot();
    RecoverCutOff();   /* before PrepareNext: it deletes NEXT_FS */
    for (u32 i = 0; i < 2 && g_cfg.replay && g_sd; i++)   /* before PrepareNext: it leaves room for missing ones */
        g_rpReady[i] = ReplayFileWhole(i);
    if (!g_cfg.replay && g_sd)   /* off (the app turns it off after a power-on): the card gets its 512 MB back */
        ReplayRemoveFiles();
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
    g_ptmu = ptmu;
    bool lidClosed = false, lidResume = false;
    u64 resumeAt = 0, lastLidPoll = 0;
    if (resumeInGame)   /* the game draws its first frames by then; same path as after the lid */
    {
        lidResume = true;
        resumeAt = Ticks() + SYSCLOCK_ARM11 * 2;
    }

    bool comboHeld = true;   /* a combo still held from the launch must not start a recording */
    bool saveHeld = true;
    u64 replayAt = 0;        /* no try to start the replay buffer before this tick */
    u64 lastBattery = 0, lastAppCheck = 0, noAppSince = 0;
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
        const bool appOnScreen = AppOnScreen();
        if (Ticks() - lastBattery > 600ull * SYSCLOCK_ARM11)   /* every ten minutes, whatever runs */
        {
            lastBattery = Ticks();
            const Battery b = BatteryNow();
            Log("battery %u%%, %lu mV%s; %s, replay buffer %s", b.pct, b.mv, b.charger ? ", ON THE CHARGER" : "",
                !g_recording ? "not recording" : g_replay ? "the replay buffer runs" : "a recording runs",
                g_cfg.replay ? "on" : "off");
        }
        if (!g_recording && g_cfg.replay && Ticks() - lastAppCheck > 2ull * SYSCLOCK_ARM11)
        {
            lastAppCheck = Ticks();
            g_frontPid = ApplicationPid();
        }
        ReplayPrepare();
        g_shared.replayOn = g_cfg.replay;
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
                const bool buffer = g_replay;   /* starts again by itself once the lid is open */
                StopRecording("lid closed (sleep)", false);
                lidResume = !buffer;
            }
        }
        else if (nid == 0x213 && lidClosed)
        {
            lidClosed = false;
            if (lidResume)   /* the screens wake up a moment after the lid */
                resumeAt = Ticks() + SYSCLOCK_ARM11 * 3 / 2;
            replayAt = Ticks() + SYSCLOCK_ARM11 * 3 / 2;
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
            if (g_recording && g_replay)
                StopRecording("a recording goes on", false);
            if (!g_recording && !StartRecording())
                LED_SIGNAL(LED_ERROR);
        }
        if (nid == PTMNOTIFID_SHUTDOWN || nid == 0x100)
        {
            Log("system notification %03lX: %s", nid, nid == 0x100 ? "the recorder must end" : "power-off or reboot");
            g_shutdownSeen = true;   /* no new recording and no file prepared from now on */
            const bool wasRecording = g_recording && !g_replay;
            const bool gameReboot = nid == 0x100 && !g_powerOffSeen;   /* a game's memory mode, System Settings... */
            if (gameReboot)
                WriteGameOrder(wasRecording);
            if (g_recording && g_replay)
                StopRecording(nid == 0x100 ? "the system ends the recorder" : "power-off or reboot", false);
            else if (g_recording)
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
            g_replayFailed = false;
            if (!g_cfg.replay)
            {
                ReplayRemoveFiles();
                g_shared.replayError = REC_REPLAY_OK;
            }
            Log("settings reloaded (hotkey mask %03lX, fps %lu, screens %s, audio %s, led %lu)", g_cfg.hotkey, g_cfg.fps,
                ScreensName(), g_cfg.audio ? "on" : "off", g_cfg.led);
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
        /* The replay hotkey: the buffer's last 30 s become a clip. At once, as the recording hotkey
           (with a 0.2 s hold first, short presses did nothing: O3DS 2026-11-14). */
        bool save = false;
        if ((pad & g_cfg.saveKey) == g_cfg.saveKey)
        {
            save = !saveHeld;
            saveHeld = true;
        }
        else
            saveHeld = false;

        if (g_recording)
        {
            u32 sleepUs = POLL_REC_US;
            ReapDmas();
            if (!g_cfg.audioDebug)
                sleepUs = PollScreens();
            if (DmasRunning())
                sleepUs = POLL_REC_US;   /* close them soon after they finish */
            UpdateBoost();
            /* New 3DS: a program that keeps core 2 busy itself starves the converter there (lowest
               priority) and frames find no free slot: back to the system core, as before 0.12.6,
               for the rest of this recording and the next ones */
            if (g_convActive == 1 && !g_testVar && g_drops[DROP_NO_SLOT] + g_drops[DROP_DMA_LIMIT] >= 8u)
            {
                g_core2Behind = true;
                g_convActive = 0;
                g_convThread = g_convThreads[0];
                svcSetThreadPriority(threadGetHandle(g_convThread), g_boosted ? CONVERTER_PRIO_BOOST : CONVERTER_PRIO);
                Log("converter on core %ld fell behind: the system core from now on", g_convCores[1]);
            }
            const u64 now = Ticks();
            g_shared.recSeconds = (u32)(TicksToUs64(now - g_recStartTick) / 1000000u);
            g_shared.recKb = (u32)(g_written / KB);
            g_shared.dropped = g_dropped;
            g_shared.throttled = g_throttled;
            if (g_replay)
            {
                if (toggle || cmd == REC_CMD_START)
                {
                    StopRecording("a recording starts", false);
                    if (!StartRecording())
                        LED_SIGNAL(LED_ERROR);
                }
                else if (save || cmd == REC_CMD_SAVE_CLIP)
                {
                    g_saveClip = true;
                    StopRecording("clip", false);   /* the buffer starts again from the idle part below */
                }
                else if (cmd == REC_CMD_EXIT)
                {
                    StopRecording("recorder exit", false);
                    break;
                }
                else if (R_FAILED(g_writeRc))
                    StopRecording("SD write error", false);
                else if (reloadPending)
                    StopRecording("settings changed", false);
                else if (appOnScreen)
                    StopRecording("the app is on the screen", false);
                else if (!g_frontPid)
                {
                    /* HOME Menu alone: nothing to keep (its banners move all the time, and the card and
                       the battery would pay for it). A moment's grace: a program closing and the next
                       one starting, and a clip of a game's last seconds saved right after it closed. */
                    if (!noAppSince)
                        noAppSince = now;
                    else if (now - noAppSince > 15ull * SYSCLOCK_ARM11)
                        StopRecording("no program is running (HOME Menu alone)", false);
                }
                else
                    noAppSince = 0;
                svcSleepThread((u64)(g_recording ? sleepUs : POLL_REC_US) * 1000ull);
                continue;
            }
            const char *why = NULL;
            bool goOn = false;   /* the file is full: save it and go on in a new one */
            if (toggle || cmd == REC_CMD_STOP)
                why = "user";
            else if (cmd == REC_CMD_EXIT)
                why = "recorder exit";
            else if (g_written + 8ull * MB >= MAX_FILE || TicksToUs64(now - g_recStartTick) >= MAX_REC_US) {
                why = "file size (FAT32) or length limit: going on in a new file";
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
            /* the pad, notifications and the lid are read here too: at most 20 ms late */
            svcSleepThread((u64)(g_recording ? sleepUs : POLL_REC_US) * 1000ull);
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
        /* the replay buffer: on whenever the setting is and nothing else records. Not while a
           recording is about to go on (lid, HOME), nor while the next recording's file is still
           being made (its steps keep the card busy for seconds). */
        if (g_cfg.replay && !g_replayFailed && !g_shutdownSeen && !lidResume && !homeResume && !appOnScreen && g_frontPid &&
            Ticks() >= replayAt && (!g_prepThread || g_prepDone))
        {
            noAppSince = 0;
            if (StartRecordingAs(true))
                continue;
            if (g_rpNoSpace)
                g_shared.replayError = REC_REPLAY_NO_SPACE;
            replayAt = Ticks() + REPLAY_RETRY_US * (SYSCLOCK_ARM11 / 1000000u);
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
    if (!systemEnd)   /* a power-off keeps them for the next start; an uninstall or update does not */
        ReplayRemoveFiles();
    LedOff();
    Log(systemEnd ? "recorder ended by the system (power-off or reboot)" : "recorder exited");
    g_shared.state = REC_STATE_EXITED;
    /* the writer and converter threads are idle; the process exit ends them */
    if (g_sd)
        FSUSER_CloseArchive(g_sd);
    svcExitProcess();
    return 0;
}
