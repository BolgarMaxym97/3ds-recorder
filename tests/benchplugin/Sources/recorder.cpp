#include "recorder.hpp"

#include <CTRPluginFramework.hpp>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bench.h"
#include "codec.h"
#include "convert.h"
#include "fbaddr.h"
#include "sdio.h"

using namespace CTRPluginFramework;

namespace Recorder
{
namespace
{
    /* ---------------------------------------------------------------- constants */

    constexpr u32 KB = 1024, MB = 1024 * 1024;
    // Write block: T1 picked 256 KB (96% of the 1 MB rate); 64 KB is a T8 variant.
    constexpr u64 FIRST_ALLOC = 128ull * MB;     // allocated before recording starts
    constexpr u64 CHUNK_ALLOC = 32ull * MB;
    constexpr u32 RING_HEADROOM = 512 * KB;       // left free for thread stacks and CTRPF
    // T3: the 10 MiB plugin block gives a ~9.3 MB largest malloc on O3DS.
    constexpr u32 RING_SIZES[] = { 7 * MB, 6 * MB + 512 * KB, 6 * MB, 5 * MB + 512 * KB, 5 * MB,
                                   4 * MB + 512 * KB, 4 * MB, 3 * MB, 2 * MB, 1 * MB };
    constexpr u32 TIMELINE_CAP = 6000;           // 100 s of 60 fps frames (autotest runs are 45 s)
    constexpr u32 STEADY_SKIP_US = 6000000;      // "steady" fps ignores the first 6 s of a run
    // 30 fps cap: a 60 fps game (16.7 ms) captures every 2nd frame, a 30 fps
    // game with jitter (e.g. 29 ms) still captures every frame.
    constexpr u64 CAPTURE_INTERVAL_US = 25000;
    constexpr u32 SLOW_FRAME_US = 40000;         // > 2 VBlank-ish at 30 fps
    constexpr u32 MAGIC_FILE = 0x42443352;       // "R3DB"
    constexpr u32 MAGIC_FRAME = 0x46443352;      // "R3DF"
    constexpr u32 MAGIC_PAD = 0x50443352;        // "R3DP"
    constexpr u32 MAGIC_XFRAME = 0x58443352;     // "R3DX": XOR-RLE frame, screen bit 6 = keyframe
    constexpr u32 MAGIC_YFRAME = 0x59443352;     // "R3DY": XR32 frame (pixel pairs), screen bit 6 = keyframe
    const char *const PREP_PATH = BENCH_DIR "/rec_next.tmp";
    const char *const LAST_PATH = BENCH_DIR "/rec_last.r3dtest";
    constexpr u32 KEYFRAME_INTERVAL = 30;

    /* Stream records (all 16-byte aligned; see tools/r3dtest.py). */
    struct FileHeader
    {
        u32 magic;
        u32 version;
        u64 titleId;
        u8  isNew3ds;
        u8  mode;
        u8  reserved[14];
    };
    static_assert(sizeof(FileHeader) == 32, "file header size");

    struct RecHeader
    {
        u32 magic;
        u32 len;      // payload bytes (frame) or bytes to skip (pad)
        u32 timeUs;   // since session start
        u8  screen;   // 0 top, 1 bottom
        u8  srcFormat;
        u16 cols;     // output columns; rows = len / (cols * 2)
    };
    static_assert(sizeof(RecHeader) == 16, "record header size");

    struct TimelineEntry
    {
        u32 timeUs;
        u32 intervalUs;
        u16 capTopUs;
        u16 capBotUs;
        u16 fillKb;
        u8  dropped;
        u8  pad;
    };

    /*
     * CTRPF calls the OSD callback for both screens on every buffer swap of
     * either screen (~2 calls per screen per frame in NSMB2). A new frame of a
     * screen is detected by a change of its framebuffer address (double
     * buffering); repeat calls are only counted.
     */
    struct ScreenStats
    {
        u32 fbAddr = 0, stride = 0, format = 0xFF;
        u32 calls = 0, frames = 0, captured = 0, dropped = 0, unsupported = 0;
        u64 lastCaptureUs = 0;
        bool hasLastCapture = false;
    };

    /* ---------------------------------------------------------------- state */

    Options      g_opt;
    volatile bool g_running = false;
    volatile bool g_stopping = false;     // writer: drain and exit
    volatile bool g_captureOff = true;    // OnFrame: ignore frames

    u8 *         g_ring = nullptr;
    u32          g_ringSize = 0;
    u64          g_w = 0;               // producer position (game thread)
    u64          g_r = 0;               // consumer position (writer thread)
    u32          g_maxFill = 0;

    Handle       g_file = 0;
    u64          g_fileAllocated = 0;
    u64          g_written = 0;
    Thread       g_writer = nullptr;
    int          g_writerCoreUsed = -1;

    u64          g_startTick = 0;
    u64          g_lastTopTick = 0;
    ScreenStats  g_top, g_bot;
    u16          g_pendingCapBot = 0;

    bench_hist_t g_frameHist, g_capTopHist, g_capBotHist, g_writeHist;
    TimelineEntry *g_timeline = nullptr;
    u32          g_timelineN = 0;

    Result       g_error = 0;
    u32          g_filePrioBefore = 0xFFFFFFFF;
    Result       g_filePrioRc = 0;
    u32          g_setsizeCount = 0, g_setsizeMaxUs = 0, g_firstAllocUs = 0;
    char         g_runId[20];

    const char *const MODE_NAMES[ModeCount] = {
        "count_only", "half_top", "half_both", "full_top", "full_both", "hybrid", "write_only",
    };

    void NoteError(Result rc)
    {
        if (g_error == 0 && R_FAILED(rc))
            g_error = rc;
    }

    u32 ElapsedUs(u64 from, u64 to)
    {
        u64 us = bench_ticks_to_us(to - from);
        return us > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)us;
    }

    bool CaptureTop(void) { return g_opt.mode >= ModeHalfTop && g_opt.mode <= ModeHybrid; }
    u32  Block(void) { return g_opt.blockKB * KB; }
    bool CaptureBottom(void) { return g_opt.mode == ModeHalfBoth || g_opt.mode == ModeFullBoth || g_opt.mode == ModeHybrid; }
    /* Pixel step per screen (0 top, 1 bottom): 2 = half resolution. */
    u32  StepFor(int screen)
    {
        switch (g_opt.mode)
        {
        case ModeHalfTop:
        case ModeHalfBoth: return 2;
        case ModeHybrid:   return screen == 0 ? 1 : 2;
        default:           return 1;
        }
    }

    /* ---------------------------------------------------------------- DMA state */

    /*
     * DMA capture: in the game thread only svcStartInterProcessDma is issued
     * (framebuffer -> staging slot, raw). A convert thread waits for the DMA,
     * invalidates the cache and converts/downscales into the ring. Luma's SVC
     * dispatcher has no per-process SVC ACL, so the game process may use 0x55.
     */
    /*
     * One arena per screen, split into as many slots as the real frame size
     * allows (decided on the first frame): top RGB565 = 3-4 slots, BGR8 2,
     * RGBA8 2; bottom RGB565 3, BGR8 2, RGBA8 1. More slots absorb converter
     * stalls (the game preempting core 0) instead of dropping frames.
     */
    constexpr u32 ARENA_CAP[2] = { 768 * KB, 480 * KB };
    constexpr u32 MAX_SLOTS = 4;
    constexpr u32 DMA_TIMEOUT_US = 100000;
    constexpr u32 SLOT_FREE = 0, SLOT_PENDING = 1;

    struct Slot
    {
        u8 *buf = nullptr;
        u32 cap = 0;
        u32 state = SLOT_FREE;   // accessed atomically
        Handle dma = 0;
        u32 seq = 0;
        u8  screen = 0;
        u32 format = 0, stride = 0, size = 0, timeUs = 0;
        u64 startTick = 0;
    };

    Slot          g_slots[2 * MAX_SLOTS];   // [0..3] top, [4..7] bottom
    u8           *g_arena[2] = { nullptr, nullptr };
    u32           g_slotCount[2] = { 0, 0 }, g_slotSize[2] = { 0, 0 };
    u32           g_botSkipped = 0;          // bottom frames skipped on purpose to let the top catch up
    u32           g_topThrottled = 0;        // top frames skipped on purpose while the SD is behind
    bool          g_throttleFlip = false;
    constexpr u32 THROTTLE_PCT = 70;         // ring fill above which the top drops to every 2nd frame
    u32           g_slotSeq = 0;
    volatile bool g_convStop = false;
    int           g_convCoreUsed = -1;
    int           g_dmaSrcMode = -1;              // 0: address as given, 1: VRAM via 0x1F000000
    Result        g_dmaFirstRc = 0;
    u32           g_dmaErrors = 0, g_dmaBusy = 0, g_dmaTimeouts = 0, g_ringDrops = 0;
    bench_hist_t  g_dmaLatHist, g_convHist, g_encHist;

    /* Delta codec state per screen: the previous frame (packed RGB565). */
    constexpr u32 FRAME_TOP_WORDS = 400 * 240, FRAME_BOT_WORDS = 320 * 240;
    u16          *g_codecPrev[2] = { nullptr, nullptr };
    u32           g_codecFrames[2] = { 0, 0 };
    u32           g_codecWords[2] = { 0, 0 };
    u64           g_rawBytes = 0, g_encBytes = 0;
    u32           g_fusedFrames = 0;
    s64           g_cpuLimitMax = -1, g_cpuLimitCur = -1;   // RESLIMIT_CPUTIME of the game (sys core share)

    void ReadCpuTimeLimit(void)
    {
        Handle rl = 0;
        g_cpuLimitMax = g_cpuLimitCur = -1;
        if (R_FAILED(svcGetResourceLimit(&rl, CUR_PROCESS_HANDLE)))
            return;
        ResourceLimitType name = RESLIMIT_CPUTIME;
        svcGetResourceLimitLimitValues(&g_cpuLimitMax, rl, &name, 1);
        svcGetResourceLimitCurrentValues(&g_cpuLimitCur, rl, &name, 1);
        svcCloseHandle(rl);
    }

    /* ---------------------------------------------------------------- ring */

    /*
     * Reserve `need` contiguous bytes. If the tail of the ring is too short,
     * a pad record fills it and the data starts at offset 0. Returns nullptr
     * (frame dropped) if the ring does not have enough free space.
     */
    u8 *RingReserve(u32 need, u32 *total)
    {
        const u64 r = __atomic_load_n(&g_r, __ATOMIC_ACQUIRE);
        const u32 pos = (u32)(g_w % g_ringSize);
        const u32 tail = g_ringSize - pos;
        *total = need;
        if (tail < need)
            *total = tail + need;
        if (g_ringSize - (u32)(g_w - r) < *total)
            return nullptr;
        if (tail < need)
        {
            RecHeader *pad = (RecHeader *)(g_ring + pos);
            pad->magic = MAGIC_PAD;
            pad->len = tail - sizeof(RecHeader);
            pad->timeUs = 0;
            pad->screen = 0xFF;
            pad->srcFormat = 0;
            pad->cols = 0;
            return g_ring;
        }
        return g_ring + pos;
    }

    void RingCommit(u32 total)
    {
        const u64 w = g_w + total;
        __atomic_store_n(&g_w, w, __ATOMIC_RELEASE);
        const u32 fill = (u32)(w - __atomic_load_n(&g_r, __ATOMIC_ACQUIRE));
        if (fill > g_maxFill)
            g_maxFill = fill;
    }

    /* ---------------------------------------------------------------- capture */

    /* Updates per-screen info; returns true if this call shows a new frame. */
    bool IsNewFrame(const Screen &screen, ScreenStats &st)
    {
        st.calls++;
        st.stride = screen.Stride;
        st.format = (u32)screen.Format;
        if (screen.LeftFramebuffer == st.fbAddr)
            return false;
        st.fbAddr = screen.LeftFramebuffer;
        st.frames++;
        return true;
    }

    bool CaptureScreen(const Screen &screen, ScreenStats &st, u64 nowUs, u16 *capUs, bool *dropped)
    {
        const u32 cols = screen.IsTop ? 400 : 320;
        const u32 rows = 240;
        const u32 step = StepFor(screen.IsTop ? 0 : 1);

        if (st.hasLastCapture && nowUs - st.lastCaptureUs < CAPTURE_INTERVAL_US)
            return false;

        const u64 t0 = bench_ticks();
        const u32 payload = fb_out_bytes(cols, rows, step);
        u32 total = 0;
        u8 *dst = RingReserve(sizeof(RecHeader) + payload, &total);
        if (!dst)
        {
            st.dropped++;
            *dropped = true;
            return false;
        }

        RecHeader *h = (RecHeader *)dst;
        u32 n = fb_to_rgb565((u16 *)(dst + sizeof(RecHeader)), (const u8 *)screen.LeftFramebuffer,
                             st.format, cols, rows, st.stride, step);
        if (n != payload)
        {
            st.unsupported++;   // unknown pixel format: keep the stream valid, skip the frame
            return false;
        }
        h->magic = MAGIC_FRAME;
        h->len = n;
        h->timeUs = (u32)nowUs;
        h->screen = screen.IsTop ? 0 : 1;
        h->srcFormat = (u8)st.format;
        h->cols = (u16)(cols / step);
        RingCommit(total);

        u32 us = ElapsedUs(t0, bench_ticks());
        *capUs = us > 0xFFFF ? 0xFFFF : (u16)us;
        st.captured++;
        st.lastCaptureUs = nowUs;
        st.hasLastCapture = true;
        return true;
    }

    bool AllocCodecBuffers(void)
    {
        const u32 words[2] = { FRAME_TOP_WORDS, FRAME_BOT_WORDS };
        for (int i = 0; i < 2; i++)
        {
            if (!g_codecPrev[i])
                g_codecPrev[i] = (u16 *)memalign(0x40, words[i] * 2);
            if (!g_codecPrev[i])
                return false;
            g_codecFrames[i] = 0;
            g_codecWords[i] = 0;
        }
        return true;
    }

    bool AllocSlots(void)
    {
        for (int sc = 0; sc < 2; sc++)
        {
            if (!g_arena[sc])
                g_arena[sc] = (u8 *)memalign(0x1000, ARENA_CAP[sc]);
            if (!g_arena[sc])
                return false;
            g_slotCount[sc] = g_slotSize[sc] = 0;   // split on the first frame
        }
        for (Slot &s : g_slots)
        {
            s.buf = nullptr;
            s.cap = 0;
            s.state = SLOT_FREE;
            s.dma = 0;
        }
        return true;
    }

    /* Splits the screen's arena for frames of `size` bytes. Only while all its slots are free. */
    bool EnsureSlots(int sc, u32 size)
    {
        if (g_slotCount[sc] && size <= g_slotSize[sc])
            return true;
        for (u32 i = 0; i < g_slotCount[sc]; i++)
            if (__atomic_load_n(&g_slots[sc * MAX_SLOTS + i].state, __ATOMIC_ACQUIRE) != SLOT_FREE)
                return false;
        const u32 slotSize = (size + 63) & ~63u;
        u32 n = slotSize ? ARENA_CAP[sc] / slotSize : 0;
        if (n > MAX_SLOTS)
            n = MAX_SLOTS;
        if (n == 0)
            return false;
        for (u32 i = 0; i < MAX_SLOTS; i++)
        {
            Slot &s = g_slots[sc * MAX_SLOTS + i];
            s.buf = i < n ? g_arena[sc] + i * slotSize : nullptr;
            s.cap = i < n ? slotSize : 0;
        }
        g_slotSize[sc] = slotSize;
        g_slotCount[sc] = n;
        return true;
    }

    /*
     * CTRPF reports framebuffers as 0x80000000 | PA, which DMA rejects
     * (0xE0E01BF5). Translate to the game's own VA (VRAM or linear FCRAM,
     * verified with VA->PA). Games alternate between two buffers, so cache
     * the translation per buffer address.
     */
    u32 g_vaCacheFb[4], g_vaCacheVa[4];
    u32 g_vaCacheNext = 0;

    u32 SourceVa(u32 fb)
    {
        for (u32 i = 0; i < 4; i++)
            if (g_vaCacheFb[i] == fb)
                return g_vaCacheVa[i];
        int kind = 0;
        const u32 va = fb_resolve_game_va(fb, &kind);
        if (va)
            g_dmaSrcMode = kind;   // 1 vram, 2 linear 0x30000000, 3 linear 0x14000000
        g_vaCacheFb[g_vaCacheNext] = fb;
        g_vaCacheVa[g_vaCacheNext] = va;
        g_vaCacheNext = (g_vaCacheNext + 1) % 4;
        return va;
    }

    Result StartDma(Handle *h, u8 *dst, u32 fb, u32 size)
    {
        DmaConfig cfg;
        dmaConfigInitDefault(&cfg);
        cfg.flags = 0;   // never block the game thread waiting for a free channel

        const u32 src = SourceVa(fb);
        if (!src)
            return (Result)0xE0E01BF5;   // same code the kernel gives for the untranslated alias
        Result rc = svcStartInterProcessDma(h, CUR_PROCESS_HANDLE, (u32)dst, CUR_PROCESS_HANDLE, src, size, &cfg);
        if (R_FAILED(rc) && g_dmaFirstRc == 0)
            g_dmaFirstRc = rc;
        return rc;
    }

    bool CaptureScreenDma(const Screen &screen, ScreenStats &st, u64 nowUs, u16 *capUs, bool *dropped)
    {
        if (st.hasLastCapture && nowUs - st.lastCaptureUs < CAPTURE_INTERVAL_US)
            return false;

        const u64 t0 = bench_ticks();
        const int sc = screen.IsTop ? 0 : 1;
        const u32 size = (screen.IsTop ? 400 : 320) * st.stride;
        if (size == 0 || size > ARENA_CAP[sc])
        {
            st.unsupported++;
            return false;
        }

        // SD behind (ring filling): halve the frame rate in a controlled way instead of
        // losing frames at random when the ring is full (a lost frame also forces a keyframe).
        const u32 fill = (u32)(g_w - __atomic_load_n(&g_r, __ATOMIC_ACQUIRE));
        if (g_ringSize && fill > g_ringSize / 100 * THROTTLE_PCT)
        {
            if (sc == 0)
                g_throttleFlip = !g_throttleFlip;   // top: every 2nd frame; bottom: paused
            if (sc == 1 || g_throttleFlip)
            {
                if (sc == 0)
                    g_topThrottled++;
                else
                    g_botSkipped++;
                st.lastCaptureUs = nowUs;
                st.hasLastCapture = true;
                return false;
            }
        }

        // The top screen has priority: while its converter backlog is 2+ frames,
        // skip bottom frames (they cost converter time the top needs).
        if (sc == 1 && CaptureTop())
        {
            u32 busyTop = 0;
            for (u32 i = 0; i < g_slotCount[0]; i++)
                if (__atomic_load_n(&g_slots[i].state, __ATOMIC_ACQUIRE) != SLOT_FREE)
                    busyTop++;
            if (busyTop >= 2)
            {
                g_botSkipped++;
                st.lastCaptureUs = nowUs;   // keeps the 30 fps cadence
                st.hasLastCapture = true;
                return false;
            }
        }

        Slot *slot = nullptr;
        if (EnsureSlots(sc, size))
            for (u32 i = 0; i < g_slotCount[sc] && !slot; i++)
                if (__atomic_load_n(&g_slots[sc * MAX_SLOTS + i].state, __ATOMIC_ACQUIRE) == SLOT_FREE)
                    slot = &g_slots[sc * MAX_SLOTS + i];
        if (!slot)
        {
            g_dmaBusy++;
            st.dropped++;
            *dropped = true;
            return false;
        }

        Handle h = 0;
        if (R_FAILED(StartDma(&h, slot->buf, screen.LeftFramebuffer, size)))
        {
            g_dmaErrors++;
            st.dropped++;
            *dropped = true;
            return false;
        }
        slot->dma = h;
        slot->seq = ++g_slotSeq;
        slot->screen = screen.IsTop ? 0 : 1;
        slot->format = st.format;
        slot->stride = st.stride;
        slot->size = size;
        slot->timeUs = (u32)nowUs;
        slot->startTick = t0;
        __atomic_store_n(&slot->state, SLOT_PENDING, __ATOMIC_RELEASE);

        const u32 us = ElapsedUs(t0, bench_ticks());
        *capUs = us > 0xFFFF ? 0xFFFF : (u16)us;
        st.captured++;
        st.lastCaptureUs = nowUs;
        st.hasLastCapture = true;
        return true;
    }

    /*
     * Converters (1 or 2 threads, cores 0 and/or 1). Each claims the oldest
     * pending slot of a screen nobody else is converting (XOR-RLE needs frames
     * of one screen in order), encodes into its own scratch buffer, then
     * copies the record into the ring under g_convLock.
     */
    constexpr u32 SLOT_CONVERTING = 2;
    constexpr int MAX_CONVERTERS = 2;
    constexpr u32 SCRATCH_SIZE = 400 * 240 * 2 + 64;   // >= codec_xrle_max_size(96000) and a raw top frame
    LightLock     g_convLock;
    bool          g_screenBusy[2] = { false, false };
    u8           *g_scratch[MAX_CONVERTERS] = { nullptr, nullptr };
    u32           g_convFrames[MAX_CONVERTERS] = { 0, 0 };
    Thread        g_converters[MAX_CONVERTERS] = { nullptr, nullptr };
    int           g_converterCores[MAX_CONVERTERS] = { -1, -1 };
    int           g_converterCount = 0;

    Slot *ClaimSlot(void)
    {
        LightLock_Lock(&g_convLock);
        // Oldest pending frame of the top screen first, then the bottom.
        Slot *next = nullptr;
        for (Slot &s : g_slots)
            if (__atomic_load_n(&s.state, __ATOMIC_ACQUIRE) == SLOT_PENDING && !g_screenBusy[s.screen] &&
                (!next || s.screen < next->screen || (s.screen == next->screen && s.seq < next->seq)))
                next = &s;
        if (next)
        {
            __atomic_store_n(&next->state, SLOT_CONVERTING, __ATOMIC_RELEASE);
            g_screenBusy[next->screen] = true;
        }
        LightLock_Unlock(&g_convLock);
        return next;
    }

    void ReleaseSlot(Slot &s)
    {
        LightLock_Lock(&g_convLock);
        g_screenBusy[s.screen] = false;
        __atomic_store_n(&s.state, SLOT_FREE, __ATOMIC_RELEASE);
        LightLock_Unlock(&g_convLock);
    }

    struct Encoded
    {
        bool ok = false, fused = false;
        u32 magic = 0, len = 0, raw = 0;
        u8  screenByte = 0;
        u16 cols = 0;
    };

    /* Encodes one DMA'd frame into `scratch`. Touches only per-screen codec state (screen is claimed). */
    Encoded EncodeSlot(const Slot &s, u8 *scratch)
    {
        Encoded e;
        const int sc = s.screen;
        const u32 cols = sc == 0 ? 400 : 320;
        const u32 step = StepFor(sc);
        const u32 words = (cols / step) * (240 / step);
        e.cols = (u16)(cols / step);
        e.raw = words * 2;

        if (g_opt.codec == 0)
        {
            if (fb_to_rgb565((u16 *)scratch, s.buf, s.format, cols, 240, s.stride, step) != words * 2)
                return e;
            e.ok = true;
            e.magic = MAGIC_FRAME;
            e.len = words * 2;
            e.screenByte = (u8)sc;
            return e;
        }

        const bool key = g_codecWords[sc] != words || g_codecFrames[sc] % KEYFRAME_INTERVAL == 0;
        // RGB565 sources: one pass straight from the DMA slot. Other formats are first
        // packed to RGB565 in place (output never overtakes input), then encoded at step 1.
        const u8 *src = s.buf;
        u32 srcCols = cols, srcRows = 240, srcStride = s.stride, srcStep = step;
        e.fused = (s.format & 7u) == FB_RGB565;
        if (!e.fused)
        {
            if (fb_to_rgb565((u16 *)s.buf, s.buf, s.format, cols, 240, s.stride, step) != words * 2)
                return e;
            srcCols = cols / step;
            srcRows = 240 / step;
            srcStride = srcRows * 2;
            srcStep = 1;
        }
        if (g_opt.codec == 2)
            e.len = (u32)codec_xr32_encode_strided_v(g_codecPrev[sc], src, srcCols, srcRows, srcStride, srcStep, key,
                                                     g_opt.encVariant, scratch);
        else
            e.len = (u32)codec_xrle_encode_strided(g_codecPrev[sc], src, srcCols, srcRows, srcStride, srcStep, key, scratch);
        g_codecWords[sc] = words;
        g_codecFrames[sc]++;
        e.ok = true;
        e.magic = g_opt.codec == 2 ? MAGIC_YFRAME : MAGIC_XFRAME;
        e.screenByte = (u8)(sc | (key ? 0x40 : 0));
        return e;
    }

    void ConvertSlot(const Slot &s, int idx)
    {
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)s.buf, s.size);
        const u64 t0 = bench_ticks();
        const Encoded e = EncodeSlot(s, g_scratch[idx]);
        const u64 t1 = bench_ticks();
        if (!e.ok)
            return;

        LightLock_Lock(&g_convLock);
        u32 total = 0;
        const bool delta = e.magic != MAGIC_FRAME;
        const u32 stored = delta ? ((e.len + 15) & ~15u) : e.len;
        u8 *dst = RingReserve(sizeof(RecHeader) + stored, &total);
        if (!dst)
        {
            g_ringDrops++;
            // The decoder never sees this frame: the next delta must not reference it.
            if (delta)
                g_codecWords[s.screen] = 0;
        }
        else
        {
            RecHeader *h = (RecHeader *)dst;
            h->magic = e.magic;
            h->len = e.len;
            h->timeUs = s.timeUs;
            h->screen = e.screenByte;
            h->srcFormat = (u8)s.format;
            h->cols = e.cols;
            memcpy(dst + sizeof(RecHeader), g_scratch[idx], e.len);
            RingCommit(total);
            g_rawBytes += e.raw;
            g_encBytes += e.len;
            if (e.fused)
                g_fusedFrames++;
            g_convFrames[idx]++;
        }
        bench_hist_add_us(&g_encHist, ElapsedUs(t0, t1));
        bench_hist_add_us(&g_convHist, ElapsedUs(t0, bench_ticks()));
        LightLock_Unlock(&g_convLock);
    }

    void ConverterMain(void *arg)
    {
        const int idx = (int)(intptr_t)arg;
        for (;;)
        {
            Slot *slot = ClaimSlot();
            if (!slot)
            {
                if (g_convStop)
                    break;
                svcSleepThread(1000 * 1000);
                continue;
            }

            // DMA of one frame takes ~1.5 ms: wait for it while holding the claim.
            for (;;)
            {
                DmaState ds = DMASTATE_STARTING;
                const Result rc = svcGetDmaState(&ds, slot->dma);
                const u64 now = bench_ticks();
                if (R_SUCCEEDED(rc) && ds == DMASTATE_DONE)
                {
                    LightLock_Lock(&g_convLock);
                    bench_hist_add_us(&g_dmaLatHist, ElapsedUs(slot->startTick, now));
                    LightLock_Unlock(&g_convLock);
                    svcCloseHandle(slot->dma);
                    slot->dma = 0;
                    ConvertSlot(*slot, idx);
                    break;
                }
                if (R_FAILED(rc) || ElapsedUs(slot->startTick, now) > DMA_TIMEOUT_US)
                {
                    svcStopDma(slot->dma);
                    svcCloseHandle(slot->dma);
                    slot->dma = 0;
                    LightLock_Lock(&g_convLock);
                    g_dmaTimeouts++;
                    g_codecWords[slot->screen] = 0;   // frame lost: next one is a keyframe
                    LightLock_Unlock(&g_convLock);
                    break;
                }
                svcSleepThread(300 * 1000);
            }
            ReleaseSlot(*slot);
        }
    }

    bool Capture(const Screen &screen, ScreenStats &st, u64 nowUs, u16 *capUs, bool *dropped)
    {
        return g_opt.useDma ? CaptureScreenDma(screen, st, nowUs, capUs, dropped)
                            : CaptureScreen(screen, st, nowUs, capUs, dropped);
    }

    bool OnFrame(const Screen &screen)
    {
        if (!g_running || g_captureOff)
            return false;

        const u64 now = bench_ticks();
        const u64 nowUs = bench_ticks_to_us(now - g_startTick);

        if (!screen.IsTop)
        {
            if (!IsNewFrame(screen, g_bot))
                return false;
            bool dropped = false;
            u16 cap = 0;
            if (CaptureBottom() && Capture(screen, g_bot, nowUs, &cap, &dropped))
            {
                bench_hist_add_us(&g_capBotHist, cap);
                g_pendingCapBot = cap;
            }
            return false;
        }

        if (!IsNewFrame(screen, g_top))
            return false;
        u32 interval = g_lastTopTick ? ElapsedUs(g_lastTopTick, now) : 0;
        g_lastTopTick = now;
        if (interval)
            bench_hist_add_us(&g_frameHist, interval);

        u16 cap = 0;
        bool dropped = false;
        if (CaptureTop() && Capture(screen, g_top, nowUs, &cap, &dropped))
            bench_hist_add_us(&g_capTopHist, cap);

        if (g_timelineN < TIMELINE_CAP)
        {
            TimelineEntry &e = g_timeline[g_timelineN++];
            e.timeUs = (u32)nowUs;
            e.intervalUs = interval;
            e.capTopUs = cap;
            e.capBotUs = g_pendingCapBot;
            const u32 fill = (u32)(g_w - __atomic_load_n(&g_r, __ATOMIC_ACQUIRE));
            e.fillKb = (u16)(fill / KB);
            e.dropped = dropped ? 1 : 0;
            e.pad = 0;
        }
        g_pendingCapBot = 0;
        return false;
    }

    /* ------------------------------------------------------- prepared file */

    /*
     * SetSize of the first 128 MB takes 0.03-2.3 s on O3DS. A low-priority
     * thread creates and allocates the next recording file in the background
     * (plugin start, after every stop), so Start() only takes the handle.
     */
    Thread        g_prepThread = nullptr;
    Handle        g_prepFile = 0;
    u32           g_prepUs = 0;
    Result        g_prepRc = 0;
    bool          g_usedPrepared = false;

    void PrepMain(void *)
    {
        const u64 t0 = bench_ticks();
        FS_Path p = fsMakePath(PATH_ASCII, PREP_PATH);
        FSUSER_DeleteFile(sdio_archive(), p);
        Handle h = 0;
        Result rc = FSUSER_OpenFile(&h, sdio_archive(), p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
        if (R_SUCCEEDED(rc))
        {
            rc = FSFILE_SetSize(h, FIRST_ALLOC);
            if (R_FAILED(rc))
            {
                FSFILE_Close(h);
                h = 0;
            }
        }
        g_prepRc = rc;
        g_prepUs = ElapsedUs(t0, bench_ticks());
        g_prepFile = h;
    }

    /* Waits for a running preparation; returns the prepared handle (0 if none). */
    Handle TakePrepared(void)
    {
        if (g_prepThread)
        {
            threadJoin(g_prepThread, U64_MAX);
            threadFree(g_prepThread);
            g_prepThread = nullptr;
        }
        const Handle h = g_prepFile;
        g_prepFile = 0;
        return h;
    }

    /* ---------------------------------------------------------------- writer */

    void TimedSetSize(u64 size, u32 *outUs)
    {
        const u64 t0 = bench_ticks();
        NoteError(FSFILE_SetSize(g_file, size));
        const u32 us = ElapsedUs(t0, bench_ticks());
        g_setsizeCount++;
        if (us > g_setsizeMaxUs)
            g_setsizeMaxUs = us;
        if (outUs)
            *outUs = us;
    }

    void GrowFileFor(u32 n)
    {
        if (g_written + n > g_fileAllocated)
        {
            g_fileAllocated += CHUNK_ALLOC;
            TimedSetSize(g_fileAllocated, nullptr);
        }
    }

    void TimedWrite(const u8 *src, u32 n)
    {
        GrowFileFor(n);
        u32 done = 0;
        const u64 t0 = bench_ticks();
        Result rc = FSFILE_Write(g_file, &done, g_written, src, n, FS_WRITE_FLUSH);
        bench_hist_add_us(&g_writeHist, ElapsedUs(t0, bench_ticks()));
        NoteError(rc);
        g_written += done;
    }

    /* write_only: stream the ring's (arbitrary) contents at a fixed rate, no capture. */
    void WriterDummy(void)
    {
        const u32 n = Block();
        const u64 start = bench_ticks();
        while (!g_stopping && g_error == 0)
        {
            const u64 elapsedUs = bench_ticks_to_us(bench_ticks() - start);
            const u64 target = elapsedUs * g_opt.writeRateKBs * KB / 1000000ull;
            if (g_written <= target)   // behind schedule: write a block
                TimedWrite(g_ring + (g_written % g_ringSize), n);
            else
                svcSleepThread(2 * 1000 * 1000);
        }
    }

    void WriterMain(void *)
    {
        if (g_opt.mode == ModeWriteOnly)
        {
            WriterDummy();
            return;
        }
        for (;;)
        {
            const u64 w = __atomic_load_n(&g_w, __ATOMIC_ACQUIRE);
            const u64 r = g_r;
            const u64 avail = w - r;
            const bool stopping = g_stopping;

            if (avail >= Block() || (stopping && avail > 0))
            {
                const u32 n = avail >= Block() ? Block() : (u32)avail;
                if (g_opt.writeToSd && g_error == 0)
                    TimedWrite(g_ring + (r % g_ringSize), n);
                __atomic_store_n(&g_r, r + n, __ATOMIC_RELEASE);
                continue;
            }
            if (stopping)
                break;
            svcSleepThread(4 * 1000 * 1000);
        }
    }

    /* ---------------------------------------------------------------- output */

    void WriteFileHeader(void)
    {
        u32 total = 0;
        u8 *p = RingReserve(sizeof(FileHeader), &total);
        FileHeader *h = (FileHeader *)p;
        memset(h, 0, sizeof(*h));
        h->magic = MAGIC_FILE;
        h->version = 1;
        h->titleId = Process::GetTitleID();
        h->isNew3ds = System::IsNew3DS() ? 1 : 0;
        h->mode = (u8)g_opt.mode;
        RingCommit(total);
    }

    RunResult g_lastRun;

    const char *CodecName(void)
    {
        return g_opt.codec == 2 ? "xr32" : g_opt.codec == 1 ? "xor_rle" : "raw";
    }

    /* Game fps over active time: intervals > 500 ms are pauses (menu, loading, HOME). */
    double ActiveFps(u32 skipUs = 0)
    {
        u64 activeUs = 0;
        u32 frames = 0;
        for (u32 i = 0; i < g_timelineN; i++)
        {
            const u32 iv = g_timeline[i].intervalUs;
            if (iv && iv <= 500000 && g_timeline[i].timeUs >= skipUs)
            {
                activeUs += iv;
                frames++;
            }
        }
        return activeUs ? frames * 1e6 / (double)activeUs : 0.0;
    }

    void WriteCsv(double durationS)
    {
        static sdio_text_t t;
        char path[128];
        const u64 tid = Process::GetTitleID();
        const char *model = System::IsNew3DS() ? "N3DS" : "O3DS";
        bool created = false;

        snprintf(path, sizeof(path), BENCH_DIR "/game9_%s_%016llX.csv", model, tid);
        if (sdio_text_open(&t, path, true, &created))
        {
            if (created)
                sdio_text_printf(&t, "model,tid,run,mode,write_sd,writer_core_req,writer_core_used,ring_kb,"
                    "fmt_top,fmt_bot,fb_top,fb_bot,stride_top,stride_bot,duration_s,top_calls,top_frames,game_fps,"
                    "frame_p50_us,frame_p95_us,frame_p99_us,frame_max_us,frames_over_40ms,bot_calls,bot_frames,unsupported,"
                    "cap_top,cap_bot,drop_top,drop_bot,cap_top_p50_us,cap_top_p99_us,cap_top_max_us,"
                    "cap_bot_p50_us,cap_bot_p99_us,cap_bot_max_us,stream_mb,stream_mbps,ring_max_fill_pct,"
                    "write_p50_us,write_p99_us,write_max_us,setsize_count,setsize_max_us,first_alloc_us,"
                    "timeline_dropped,error,block_kb,write_rate_kbs,file_prio_req,file_prio_before,file_prio_rc,"
                    "capture,convert_core_used,dma_src_mode,dma_first_rc,dma_errors,dma_busy,dma_timeouts,ring_drops,"
                    "dma_lat_p50_us,dma_lat_p99_us,dma_lat_max_us,conv_p50_us,conv_p99_us,conv_max_us,"
                    "codec,enc_p50_us,enc_p99_us,enc_max_us,raw_mb,enc_mb,codec_ratio,fused_frames,cpu_limit_max,cpu_limit_cur,"
                    "fps_active,conv0_core,conv0_frames,conv1_core,conv1_frames,"
                    "fps_steady,bot_skipped,slots_top,slots_bot,prepared_file,prep_us,prep_rc,top_throttled,enc_variant\n");

            u64 slow = 0;
            for (u32 i = 0; i < g_timelineN; i++)
                if (g_timeline[i].intervalUs > SLOW_FRAME_US)
                    slow++;

            const double streamMb = (double)(g_opt.mode == ModeWriteOnly ? g_written : g_w) / MB;
            sdio_text_printf(&t, "%s,%016llX,%s,%s,%d,%d,%d,%lu,%lu,%lu,0x%08lX,0x%08lX,%lu,%lu,%.1f,%lu,%lu,%.2f,",
                model, tid, g_runId, MODE_NAMES[g_opt.mode], g_opt.writeToSd, g_opt.writerCore, g_writerCoreUsed,
                g_ringSize / KB, g_top.format, g_bot.format, g_top.fbAddr, g_bot.fbAddr, g_top.stride, g_bot.stride,
                durationS, g_top.calls, g_top.frames, durationS > 0 ? g_top.frames / durationS : 0.0);
            sdio_text_printf(&t, "%lu,%lu,%lu,%lu,%llu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                bench_hist_percentile_us(&g_frameHist, 50), bench_hist_percentile_us(&g_frameHist, 95),
                bench_hist_percentile_us(&g_frameHist, 99), g_frameHist.max_us, slow, g_bot.calls, g_bot.frames, g_top.unsupported + g_bot.unsupported,
                g_top.captured, g_bot.captured, g_top.dropped, g_bot.dropped);
            sdio_text_printf(&t, "%lu,%lu,%lu,%lu,%lu,%lu,%.1f,%.3f,%.1f,",
                bench_hist_percentile_us(&g_capTopHist, 50), bench_hist_percentile_us(&g_capTopHist, 99), g_capTopHist.max_us,
                bench_hist_percentile_us(&g_capBotHist, 50), bench_hist_percentile_us(&g_capBotHist, 99), g_capBotHist.max_us,
                streamMb, durationS > 0 ? streamMb / durationS : 0.0, 100.0 * g_maxFill / g_ringSize);
            sdio_text_printf(&t, "%lu,%lu,%lu,%lu,%lu,%lu,%lu,0x%08lX",
                bench_hist_percentile_us(&g_writeHist, 50), bench_hist_percentile_us(&g_writeHist, 99), g_writeHist.max_us,
                g_setsizeCount, g_setsizeMaxUs, g_firstAllocUs,
                g_timelineN >= TIMELINE_CAP ? 1ul : 0ul, (u32)g_error);
            sdio_text_printf(&t, ",%lu,%lu,%d,%ld,0x%08lX", g_opt.blockKB, g_opt.writeRateKBs,
                g_opt.filePriority, (long)(s32)g_filePrioBefore, (u32)g_filePrioRc);
            sdio_text_printf(&t, ",%s,%d,%d,0x%08lX,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,",
                g_opt.useDma ? "dma" : "cpu", g_convCoreUsed, g_dmaSrcMode, (u32)g_dmaFirstRc,
                g_dmaErrors, g_dmaBusy, g_dmaTimeouts, g_ringDrops,
                bench_hist_percentile_us(&g_dmaLatHist, 50), bench_hist_percentile_us(&g_dmaLatHist, 99), g_dmaLatHist.max_us,
                bench_hist_percentile_us(&g_convHist, 50), bench_hist_percentile_us(&g_convHist, 99), g_convHist.max_us);
            sdio_text_printf(&t, "%s,%lu,%lu,%lu,%.2f,%.2f,%.3f,", CodecName(),
                bench_hist_percentile_us(&g_encHist, 50), bench_hist_percentile_us(&g_encHist, 99), g_encHist.max_us,
                (double)g_rawBytes / MB, (double)g_encBytes / MB,
                g_encBytes ? (double)g_rawBytes / (double)g_encBytes : 0.0);
            sdio_text_printf(&t, "%lu,%lld,%lld,%.2f,%d,%lu,%d,%lu,", g_fusedFrames, g_cpuLimitMax, g_cpuLimitCur,
                ActiveFps(), g_converterCores[0], g_convFrames[0], g_converterCores[1], g_convFrames[1]);
            sdio_text_printf(&t, "%.2f,%lu,%lu,%lu,%d,%lu,0x%08lX,%lu,%d\n", ActiveFps(STEADY_SKIP_US), g_botSkipped,
                g_slotCount[0], g_slotCount[1], g_usedPrepared ? 1 : 0, g_prepUs, (u32)g_prepRc, g_topThrottled,
                g_opt.encVariant);
            sdio_text_close(&t);
        }

        snprintf(path, sizeof(path), BENCH_DIR "/game9_%s_%016llX_%s_%s.csv", model, tid, g_runId, MODE_NAMES[g_opt.mode]);
        if (sdio_text_open(&t, path, false, nullptr))
        {
            sdio_text_printf(&t, "t_us,interval_us,cap_top_us,cap_bot_us,ring_fill_kb,dropped\n");
            for (u32 i = 0; i < g_timelineN; i++)
            {
                const TimelineEntry &e = g_timeline[i];
                sdio_text_printf(&t, "%lu,%lu,%u,%u,%u,%u\n", e.timeUs, e.intervalUs, e.capTopUs, e.capBotUs, e.fillKb, e.dropped);
            }
            sdio_text_close(&t);
        }
    }

} // namespace

void PrepareNext(void)
{
    if (g_prepThread || g_prepFile)
        return;
    g_prepRc = 0;
    g_prepUs = 0;
    // Lowest priority on the game's core: only runs while the game idles; the work is FS IPC anyway.
    g_prepThread = threadCreate(PrepMain, nullptr, 8 * KB, 0x3F, 0, false);
}

void Shutdown(void)
{
    const Handle h = TakePrepared();
    if (h)
    {
        FSFILE_Close(h);
        FSUSER_DeleteFile(sdio_archive(), fsMakePath(PATH_ASCII, PREP_PATH));
    }
}

const RunResult &LastRun(void)
{
    return g_lastRun;
}

const char *ModeName(Mode mode)
{
    return mode < ModeCount ? MODE_NAMES[mode] : "?";
}

bool IsRunning(void)
{
    return g_running;
}

bool Start(const Options &opt, char *error, u32 errorSize)
{
    if (g_running)
        return true;

    g_opt = opt;
    g_error = 0;
    g_w = g_r = 0;
    g_maxFill = 0;
    g_written = 0;
    g_fileAllocated = 0;
    g_setsizeCount = g_setsizeMaxUs = g_firstAllocUs = 0;
    g_top = ScreenStats();
    g_bot = ScreenStats();
    g_lastTopTick = 0;
    g_pendingCapBot = 0;
    g_timelineN = 0;
    bench_hist_reset(&g_frameHist);
    bench_hist_reset(&g_capTopHist);
    bench_hist_reset(&g_capBotHist);
    bench_hist_reset(&g_writeHist);

    if (g_opt.blockKB != 64 && g_opt.blockKB != 256)
        g_opt.blockKB = 256;
    if (g_opt.mode == ModeWriteOnly)
        g_opt.writeToSd = true;
    g_filePrioBefore = 0xFFFFFFFF;
    g_filePrioRc = 0;
    g_slotSeq = 0;
    g_dmaSrcMode = -1;
    g_dmaFirstRc = 0;
    memset(g_vaCacheFb, 0, sizeof(g_vaCacheFb));
    g_dmaErrors = g_dmaBusy = g_dmaTimeouts = g_ringDrops = 0;
    g_botSkipped = 0;
    g_topThrottled = 0;
    g_throttleFlip = false;
    g_usedPrepared = false;
    g_convCoreUsed = -1;
    bench_hist_reset(&g_dmaLatHist);
    bench_hist_reset(&g_convHist);
    bench_hist_reset(&g_encHist);
    g_rawBytes = g_encBytes = 0;
    g_fusedFrames = 0;
    ReadCpuTimeLimit();
    const bool needConverter = g_opt.useDma && CaptureTop();

    // Allocated once and kept for the plugin's lifetime: freeing and
    // re-allocating fragments the heap (8 MB failed after earlier runs).
    if (!g_timeline)
        g_timeline = (TimelineEntry *)malloc(sizeof(TimelineEntry) * TIMELINE_CAP);
    // Staging slots are allocated before the ring in every mode: the ring
    // takes the largest block that fits, so slots allocated later may not fit
    // (seen on O3DS: write_only first, then DMA -> out of memory).
    if ((!AllocCodecBuffers() || !AllocSlots()) && needConverter)
    {
        snprintf(error, errorSize, "Out of memory for DMA staging slots");
        return false;
    }
    // Converter scratch buffers before the ring (the ring takes the largest block left).
    for (int i = 0; i < MAX_CONVERTERS; i++)
        if (!g_scratch[i])
            g_scratch[i] = (u8 *)memalign(0x40, SCRATCH_SIZE);
    if (needConverter && (!g_scratch[0] || !g_scratch[1]))
    {
        snprintf(error, errorSize, "Out of memory for converter buffers");
        return false;
    }
    if (!g_ring)
    {
        // Keep headroom for thread stacks and CTRPF itself while picking the ring size.
        void *headroom = malloc(RING_HEADROOM);
        for (u32 i = 0; !g_ring && i < sizeof(RING_SIZES) / sizeof(RING_SIZES[0]); i++)
        {
            g_ring = (u8 *)memalign(0x1000, RING_SIZES[i]);
            if (g_ring)
                g_ringSize = RING_SIZES[i];
        }
        free(headroom);
    }
    if (!g_ring || !g_timeline)
    {
        snprintf(error, errorSize, "Out of memory (ring %p, timeline %p)", g_ring, g_timeline);
        return false;
    }

    time_t now = time(nullptr);
    strftime(g_runId, sizeof(g_runId), "%Y%m%d_%H%M%S", localtime(&now));

    if (g_opt.writeToSd)
    {
        // Normally the file was created and allocated in the background (PrepareNext).
        g_file = TakePrepared();
        g_usedPrepared = g_file != 0;
        g_fileAllocated = FIRST_ALLOC;
        if (!g_file)
        {
            FS_Path p = fsMakePath(PATH_ASCII, PREP_PATH);
            FSUSER_DeleteFile(sdio_archive(), p);
            Result rc = FSUSER_OpenFile(&g_file, sdio_archive(), p, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
            if (R_FAILED(rc))
            {
                snprintf(error, errorSize, "Cannot open %s: %08lX", PREP_PATH, (u32)rc);
                g_file = 0;
                return false;
            }
            TimedSetSize(FIRST_ALLOC, &g_firstAllocUs);
        }
        FSFILE_GetPriority(g_file, &g_filePrioBefore);
        if (g_opt.filePriority >= 0)
            g_filePrioRc = FSFILE_SetPriority(g_file, (u32)g_opt.filePriority);
    }

    WriteFileHeader();

    g_stopping = false;
    g_writerCoreUsed = g_opt.writerCore;
    g_writer = threadCreate(WriterMain, nullptr, 16 * KB, 0x31, g_opt.writerCore, false);
    if (!g_writer && g_opt.writerCore != 0)
    {
        g_writerCoreUsed = 0;
        g_writer = threadCreate(WriterMain, nullptr, 16 * KB, 0x31, 0, false);
    }
    if (!g_writer)
    {
        snprintf(error, errorSize, "threadCreate failed");
        if (g_file)
        {
            FSFILE_Close(g_file);
            g_file = 0;
        }
        return false;
    }

    if (needConverter)
    {
        g_convStop = false;
        LightLock_Init(&g_convLock);
        g_screenBusy[0] = g_screenBusy[1] = false;
        g_convFrames[0] = g_convFrames[1] = 0;
        // 2 = both: core 1 first (free in games without a sys-core limit), core 0 picks up the rest
        // at low priority in the game's idle time (wins when the game limits core 1, e.g. NSMB2: 30%).
        const int wanted[MAX_CONVERTERS] = { g_opt.convertCore == 2 ? 1 : g_opt.convertCore, g_opt.convertCore == 2 ? 0 : -1 };
        g_converterCount = 0;
        for (int i = 0; i < MAX_CONVERTERS; i++)
        {
            g_converterCores[i] = -1;
            if (wanted[i] < 0)
                continue;
            // Core 0 is the game's core: lowest application priority (0x3F), so the converter only
            // runs in the game's idle time and can never preempt it. Core 1 is the sys core.
            const int prio = wanted[i] == 0 ? 0x3F : 0x31;
            Thread t = threadCreate(ConverterMain, (void *)(intptr_t)i, 16 * KB, prio, wanted[i], false);
            if (!t && wanted[i] != 0)
                t = threadCreate(ConverterMain, (void *)(intptr_t)i, 16 * KB, 0x3F, 0, false);
            if (t)
            {
                g_converters[i] = t;
                g_converterCores[i] = wanted[i];
                g_converterCount++;
            }
        }
        g_convCoreUsed = g_opt.convertCore;
        if (g_converterCount == 0)
        {
            snprintf(error, errorSize, "Could not start any converter thread (threadCreate)");
            g_stopping = true;
            threadJoin(g_writer, U64_MAX);
            threadFree(g_writer);
            g_writer = nullptr;
            if (g_file)
            {
                FSFILE_Close(g_file);
                g_file = 0;
            }
            return false;
        }
    }

    g_startTick = bench_ticks();
    g_running = true;
    g_captureOff = false;
    OSD::Run(OnFrame);
    return true;
}

void Stop(char *summary, u32 summarySize)
{
    if (!g_running)
        return;

    OSD::Stop(OnFrame);
    g_captureOff = true;
    const double durationS = (double)bench_ticks_to_us(bench_ticks() - g_startTick) / 1e6;
    // A callback already running in the game thread may still be capturing a
    // frame: give it a few frames before the pipeline drains.
    svcSleepThread(100ull * 1000 * 1000);

    // Converter first (it may still push frames into the ring), then the writer.
    g_convStop = true;
    for (int i = 0; i < MAX_CONVERTERS; i++)
    {
        if (!g_converters[i])
            continue;
        if (R_SUCCEEDED(threadJoin(g_converters[i], 5ull * 1000 * 1000 * 1000)))
            threadFree(g_converters[i]);
        else
            NoteError(0xDEAD0002);
        g_converters[i] = nullptr;
    }
    g_stopping = true;

    // If the writer never got CPU time (e.g. sys core denied), do not hang:
    // leak the thread and its ring instead of freeing memory it may touch.
    const bool joined = R_SUCCEEDED(threadJoin(g_writer, 5ull * 1000 * 1000 * 1000));
    if (joined)
        threadFree(g_writer);
    else
        NoteError(0xDEAD0001);
    g_writer = nullptr;
    g_running = false;

    if (g_file)
    {
        FSFILE_SetSize(g_file, g_written);
        FSFILE_Close(g_file);
        g_file = 0;
        char keep[96];
        snprintf(keep, sizeof(keep), BENCH_DIR "/rec_%016llX_%s.r3dtest", Process::GetTitleID(), MODE_NAMES[g_opt.mode]);
        FS_Path last = fsMakePath(PATH_ASCII, g_opt.keepRecording ? keep : LAST_PATH);
        FSUSER_DeleteFile(sdio_archive(), last);
        FSUSER_RenameFile(sdio_archive(), fsMakePath(PATH_ASCII, PREP_PATH), sdio_archive(), last);
    }

    WriteCsv(durationS);
    PrepareNext();

    const u32 dropped = g_top.dropped + g_bot.dropped;
    {
        RunResult &rr = g_lastRun;
        rr.mode = g_opt.mode;
        rr.fpsActive = ActiveFps();
        rr.fpsSteady = ActiveFps(STEADY_SKIP_US);
        const u32 botOffered = g_bot.captured + g_bot.dropped + g_botSkipped;
        rr.botSkipPct = botOffered ? 100.0 * g_botSkipped / botOffered : 0.0;
        const u32 topOffered = g_top.captured + g_top.dropped + g_topThrottled;
        rr.topThrottlePct = topOffered ? 100.0 * g_topThrottled / topOffered : 0.0;
        const u32 offered = g_top.captured + g_bot.captured + dropped;
        rr.dropPct = offered ? 100.0 * (dropped + g_ringDrops + g_dmaTimeouts) / offered : 0.0;
        rr.ratio = g_encBytes ? (double)g_rawBytes / (double)g_encBytes : 0.0;
        rr.mbps = durationS > 0 ? (double)(g_opt.mode == ModeWriteOnly ? g_written : g_w) / MB / durationS : 0.0;
        rr.ringMaxPct = g_ringSize ? 100.0 * g_maxFill / g_ringSize : 0.0;
        rr.convFrames[0] = g_convFrames[0];
        rr.convFrames[1] = g_convFrames[1];
        rr.cpuLimit = g_cpuLimitMax;
        rr.valid = true;
    }
    snprintf(summary, summarySize,
        "%s, %.0f s\n"
        "Game: %.1f fps, frame p99 %lu ms, max %lu ms\n"
        "Capture top p99: %lu us, bottom p99: %lu us\n"
        "Captured %lu + %lu, dropped %lu\n"
        "Stream %.2f MB/s, ring max %.0f%% of %lu KB\n"
        "SD write max %lu ms, error %08lX\n"
        "%s: DMA p99 %lu us, convert p99 %lu us, src %d, err %lu, busy %lu\n"
        "Codec %s: x%.2f, encode p99 %lu us, ring drops %lu\n"
        "Sys core limit: %lld%% (max %lld%%), convert core %d\n"
        "Active fps %.1f, converted c1 %lu / c0 %lu",
        MODE_NAMES[g_opt.mode], durationS,
        durationS > 0 ? g_top.frames / durationS : 0.0,
        bench_hist_percentile_us(&g_frameHist, 99) / 1000, g_frameHist.max_us / 1000,
        bench_hist_percentile_us(&g_capTopHist, 99), bench_hist_percentile_us(&g_capBotHist, 99),
        g_top.captured, g_bot.captured, dropped,
        durationS > 0 ? (double)(g_opt.mode == ModeWriteOnly ? g_written : g_w) / MB / durationS : 0.0,
        100.0 * g_maxFill / g_ringSize, g_ringSize / KB,
        g_writeHist.max_us / 1000, (u32)g_error,
        g_opt.useDma ? "DMA" : "CPU", bench_hist_percentile_us(&g_dmaLatHist, 99),
        bench_hist_percentile_us(&g_convHist, 99), g_dmaSrcMode, g_dmaErrors, g_dmaBusy,
        CodecName(), g_encBytes ? (double)g_rawBytes / (double)g_encBytes : 0.0,
        bench_hist_percentile_us(&g_encHist, 99), g_ringDrops, g_cpuLimitCur, g_cpuLimitMax, g_convCoreUsed,
        g_lastRun.fpsActive, g_convFrames[0], g_convFrames[1]);

    if (!joined)
    {
        // Leaked on purpose: the stuck writer may still touch the ring.
        g_ring = nullptr;
        g_ringSize = 0;
    }
}
} // namespace Recorder
