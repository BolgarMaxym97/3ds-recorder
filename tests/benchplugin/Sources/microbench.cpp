#include "microbench.hpp"

#include <CTRPluginFramework.hpp>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "codec.h"
#include "convert.h"
#include "dmaprobe.hpp"
#include "fbaddr.h"
#include "sdio.h"

using namespace CTRPluginFramework;

extern "C" Result svcControlMemoryUnsafe(u32 *out, u32 addr0, u32 size, MemOp op, MemPerm perm);

namespace Microbench
{
namespace
{
    constexpr u32 KB = 1024;
    constexpr u32 L1_BYTES = 4 * KB, L1_REPS = 64;
    constexpr u32 STREAM_BYTES = 64 * KB;
    constexpr u32 FRAME_BYTES = 400 * 240 * 2;
    constexpr u32 COLS = 400, ROWS = 240;
    constexpr int REPS = 5;
    enum { MemHeap, MemBss, MemSys, MemAlias, MemCount };

    alignas(0x1000) u8 g_bss[STREAM_BYTES];
    Report g_last;

    u64 Ticks(void) { return svcGetSystemTick(); }

    u32 __attribute__((noinline)) ReadWords(const u32 *p, u32 words)
    {
        u32 a = 0, b = 0, c = 0, d = 0;
        for (u32 i = 0; i + 8 <= words; i += 8)
        {
            a += p[i] + p[i + 4];
            b += p[i + 1] + p[i + 5];
            c += p[i + 2] + p[i + 6];
            d += p[i + 3] + p[i + 7];
        }
        return a ^ b ^ c ^ d;
    }

    volatile u32 g_sink;

    void Clean(const void *p, u32 size)
    {
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, size);
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, size);
    }

    /* Ticks per word with the block resident in L1 (if the memory is cacheable at all). */
    double L1Cost(const u8 *p)
    {
        g_sink = ReadWords((const u32 *)p, L1_BYTES / 4);   // warm
        const u64 t0 = Ticks();
        u32 acc = 0;
        for (u32 r = 0; r < L1_REPS; r++)
            acc += ReadWords((const u32 *)p, L1_BYTES / 4);
        g_sink = acc;
        return (double)(Ticks() - t0) / (L1_REPS * L1_BYTES / 4);
    }

    /* Ticks per word streaming `bytes` from memory (cache cleaned first where possible). */
    double StreamCost(const u8 *p, u32 bytes, bool canMaintain)
    {
        if (canMaintain)
            Clean(p, bytes);
        const u64 t0 = Ticks();
        g_sink = ReadWords((const u32 *)p, bytes / 4);
        return (double)(Ticks() - t0) / (bytes / 4);
    }

    /* Copies the game's top framebuffer into dst by DMA; false if not possible. */
    bool DmaFrame(u8 *dst, u32 fb, u32 size)
    {
        const u32 src = fb_resolve_game_va(fb, nullptr);
        if (!src)
            return false;
        DmaConfig cfg;
        dmaConfigInitDefault(&cfg);
        cfg.flags = 0;
        Handle h = 0;
        if (R_FAILED(svcStartInterProcessDma(&h, CUR_PROCESS_HANDLE, (u32)dst, CUR_PROCESS_HANDLE, src, size, &cfg)))
            return false;
        bool ok = false;
        for (int i = 0; i < 200; i++)
        {
            DmaState st = DMASTATE_STARTING;
            if (R_FAILED(svcGetDmaState(&st, h)))
                break;
            if (st == DMASTATE_DONE)
            {
                ok = true;
                break;
            }
            svcSleepThread(250 * 1000);
        }
        if (!ok)
            svcStopDma(h);
        svcCloseHandle(h);
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, size);
        return ok;
    }

    struct Ctx
    {
        Report r;
        u8 *sys = nullptr;
    };

    void Measure(Ctx &c)
    {
        Report &r = c.r;
        u8 *heap = (u8 *)memalign(0x1000, STREAM_BYTES);
        u32 fb = 0, fmt = 0, stride = 0;
        const bool haveFb = DmaProbeGameTop(&fb, &fmt, &stride);

        u8 *mem[MemCount] = { heap, g_bss, c.sys, haveFb ? (u8 *)fb : nullptr };
        for (int m = 0; m < MemCount; m++)
        {
            if (!mem[m])
                continue;
            if (m != MemAlias)
                memset(mem[m], 0x5A, STREAM_BYTES);
            const bool maintain = m != MemAlias;
            r.l1[m] = L1Cost(mem[m]);
            r.stream[m] = StreamCost(mem[m], m == MemAlias ? 16 * KB : STREAM_BYTES, maintain);
        }

        // Frames: two consecutive game frames by DMA, else synthetic.
        const bool fbUsable = haveFb && (fmt & 7u) == FB_RGB565 && stride >= ROWS * 2 && stride <= 1024;
        const u32 srcStride = fbUsable ? stride : ROWS * 2;
        const u32 srcBytes = COLS * srcStride;
        u8 *a = (u8 *)memalign(0x1000, srcBytes);
        u8 *b = (u8 *)memalign(0x1000, srcBytes);
        u16 *packedA = (u16 *)memalign(0x40, FRAME_BYTES);
        u16 *prev = (u16 *)memalign(0x40, FRAME_BYTES);
        u8 *out = (u8 *)memalign(0x40, codec_xr32_max_size(COLS * ROWS) + 64);
        u8 *big = (u8 *)memalign(0x1000, FRAME_BYTES);
        if (!a || !b || !packedA || !prev || !out || !big)
        {
            free(a); free(b); free(packedA); free(prev); free(out); free(big); free(heap);
            return;
        }

        r.realFrames = fbUsable;
        if (r.realFrames)
        {
            r.realFrames = DmaFrame(a, fb, srcBytes);
            svcSleepThread(70 * 1000 * 1000);   // a few game frames later (the thread sleeps: the game runs)
            DmaProbeGameTop(&fb, &fmt, &stride);
            r.realFrames = r.realFrames && DmaFrame(b, fb, srcBytes);
        }
        if (!r.realFrames)
        {
            u32 x = 0x12345678;
            for (u32 i = 0; i < srcBytes; i += 2)
            {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                *(u16 *)(a + i) = (u16)x;
            }
            memcpy(b, a, srcBytes);
            for (u32 col = 0; col < COLS; col++)   // 60% of the columns change, the rest stays
                if (col % 5 < 3)
                    for (u32 row = 0; row < ROWS; row++)
                        *(u16 *)(b + col * srcStride + row * 2) ^= (u16)(row * 31 + col);
        }

        // Packed A = prev after a keyframe of A.
        codec_xr32_encode_strided(packedA, a, COLS, ROWS, srcStride, 1, 1, out);

        // memcpy, cache maintenance
        Clean(packedA, FRAME_BYTES);
        Clean(big, FRAME_BYTES);
        u64 t0 = Ticks();
        memcpy(big, packedA, FRAME_BYTES);
        r.memcpyTpb = (double)(Ticks() - t0) / FRAME_BYTES;
        t0 = Ticks();
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)big, FRAME_BYTES);
        r.invalidateTicks = (u32)(Ticks() - t0);
        t0 = Ticks();
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)big, FRAME_BYTES);
        r.flushTicks = (u32)(Ticks() - t0);

        // Encoders, cold cache, best of REPS.
        size_t len = 0;
        for (int v = 0; v <= CODEC_XR32_VARIANTS; v++)   // v == VARIANTS: old XOR-RLE
        {
            u64 best = ~0ull;
            for (int rep = 0; rep < REPS; rep++)
            {
                memcpy(prev, packedA, FRAME_BYTES);
                Clean(prev, FRAME_BYTES);
                Clean(b, srcBytes);
                t0 = Ticks();
                if (v < CODEC_XR32_VARIANTS)
                    len = codec_xr32_encode_strided_v(prev, b, COLS, ROWS, srcStride, 1, 0, v, out);
                else
                    len = codec_xrle_encode_strided(prev, b, COLS, ROWS, srcStride, 1, 0, out);
                const u64 dt = Ticks() - t0;
                if (dt < best)
                    best = dt;
            }
            const double cpp = (double)best / (COLS * ROWS);
            if (v < CODEC_XR32_VARIANTS)
            {
                r.encXr32[v] = cpp;
                if (v == 0)
                    r.encRatio = len ? (double)FRAME_BYTES / len : 0;
            }
            else
                r.encXrle = cpp;
        }
        r.bestVariant = 0;
        for (int v = 1; v < CODEC_XR32_VARIANTS; v++)
            if (r.encXr32[v] < r.encXr32[r.bestVariant] * 0.97)   // needs a clear win
                r.bestVariant = v;

        r.heapLooksUncached = r.l1[MemHeap] > 4.0;
        r.sysLooksCached = c.sys && r.l1[MemSys] < 2.0;
        r.valid = true;

        free(a); free(b); free(packedA); free(prev); free(out); free(big); free(heap);
    }

    void ThreadMain(void *arg)
    {
        Measure(*(Ctx *)arg);
    }

    void WriteCsv(const Report &r)
    {
        static sdio_text_t t;
        char path[96];
        const char *model = System::IsNew3DS() ? "N3DS" : "O3DS";
        snprintf(path, sizeof(path), BENCH_DIR "/mem12_%s_%016llX.csv", model, Process::GetTitleID());
        bool created = false;
        if (!sdio_text_open(&t, path, true, &created))
            return;
        if (created)
            sdio_text_printf(&t, "model,tid,real_frames,l1_heap,l1_bss,l1_sys,l1_alias,str_heap,str_bss,str_sys,str_alias,"
                                 "memcpy_tpb,inval_192k,flush_192k,enc_c,enc_pld,enc_touch,enc_xrle,ratio,best,"
                                 "heap_uncached,sys_cached\n");
        sdio_text_printf(&t, "%s,%016llX,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%lu,%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d\n",
                         model, Process::GetTitleID(), r.realFrames, r.l1[0], r.l1[1], r.l1[2], r.l1[3],
                         r.stream[0], r.stream[1], r.stream[2], r.stream[3], r.memcpyTpb, r.invalidateTicks,
                         r.flushTicks, r.encXr32[0], r.encXr32[1], r.encXr32[2], r.encXrle, r.encRatio,
                         r.bestVariant, r.heapLooksUncached, r.sysLooksCached);
        sdio_text_close(&t);
    }
} // namespace

const Report &Run(char *summary, u32 summarySize)
{
    static Ctx ctx;
    ctx.r = Report();

    // Kernel-allocated SYSTEM memory (not the plugin block), for the cacheability comparison.
    u32 sysAddr = Process::GetFreeMemoryRegion(STREAM_BYTES, 0x0E000000);
    u32 sysOut = 0;
    ctx.sys = nullptr;
    if (sysAddr && R_SUCCEEDED(svcControlMemoryUnsafe(&sysOut, sysAddr, STREAM_BYTES,
                                   (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM | MEMOP_LINEAR_FLAG),
                                   (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
        ctx.sys = (u8 *)sysOut;

    // Core 0, priority above the game's threads: clean timings; the game pauses ~1-2 s.
    Thread t = threadCreate(ThreadMain, &ctx, 16 * KB, 0x18, 0, false);
    if (t)
    {
        threadJoin(t, U64_MAX);
        threadFree(t);
    }
    if (ctx.sys)
        svcControlMemoryUnsafe(&sysOut, (u32)ctx.sys, STREAM_BYTES, MEMOP_FREE, (MemPerm)0);

    g_last = ctx.r;
    const Report &r = g_last;
    if (!r.valid)
    {
        snprintf(summary, summarySize, "microbench: failed (memory / thread)\n");
        return g_last;
    }
    WriteCsv(r);
    snprintf(summary, summarySize,
             "mem t/w L1 h%.1f b%.1f s%.1f a%.0f, str h%.1f a%.0f\n"
             "enc c/px C%.1f P%.1f T%.1f old%.1f > v%d %s x%.1f\n",
             r.l1[0], r.l1[1], r.l1[2], r.l1[3], r.stream[0], r.stream[3],
             r.encXr32[0], r.encXr32[1], r.encXr32[2], r.encXrle, r.bestVariant,
             r.realFrames ? "real" : "synth", r.encRatio);
    return g_last;
}

const Report &Last(void)
{
    return g_last;
}
} // namespace Microbench
