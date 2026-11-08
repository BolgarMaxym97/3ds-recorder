#include "dmaprobe.hpp"

#include <CTRPluginFramework.hpp>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "bench.h"
#include "fbaddr.h"
#include "sdio.h"

using namespace CTRPluginFramework;

/* Luma custom SVC (implemented in libctrpf). */
extern "C" Result svcControlMemoryUnsafe(u32 *out, u32 addr0, u32 size, MemOp op, MemPerm perm);

namespace
{
    constexpr u32 PROBE_SIZE = 64 * 1024;
    constexpr u32 VERIFY_BYTES = 4096;
    constexpr u32 WAIT_US = 50000;

    enum Src { SrcAlias, SrcGameVa, SrcVramHome, SrcHeap, SrcCount };
    enum Dst { DstHeap, DstSystem, DstCount };
    const char *const SRC_NAMES[SrcCount] = { "fb_alias", "game_va", "vram_home", "heap" };
    const char *const DST_NAMES[DstCount] = { "heap", "sys_alloc" };

    struct Outcome
    {
        u32 srcAddr = 0, dstAddr = 0;
        Result startRc = 0;
        int finalState = -1;    // DmaState, -1 if never started
        u32 latencyUs = 0;
        int match = -1;         // 1 equal, 0 differ, -1 not checked
    };

    /* HOME menu process (name "menu"): NTR-HR reads VRAM through it. */
    Handle OpenHomeProcess(void)
    {
        u32 pids[64];
        s32 n = 0;
        if (R_FAILED(svcGetProcessList(&n, pids, 64)))
            return 0;
        for (s32 i = 0; i < n; i++)
        {
            Handle h = 0;
            if (R_FAILED(svcOpenProcess(&h, pids[i])))
                continue;
            char name[9] = { 0 };
            s64 raw = 0;
            if (R_SUCCEEDED(svcGetProcessInfo(&raw, h, 0x10000)))
                memcpy(name, &raw, 8);
            if (strcmp(name, "menu") == 0)
                return h;
            svcCloseHandle(h);
        }
        return 0;
    }

    Outcome Try(Handle srcProcess, u32 src, u8 *dst, const u8 *expected)
    {
        Outcome o;
        o.srcAddr = src;
        o.dstAddr = (u32)dst;
        if (!src || !dst)
        {
            o.startRc = -1;
            return o;
        }
        memset(dst, 0xA5, VERIFY_BYTES);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, PROBE_SIZE);

        DmaConfig cfg;
        dmaConfigInitDefault(&cfg);
        cfg.flags = 0;
        Handle dma = 0;
        const u64 t0 = bench_ticks();
        o.startRc = svcStartInterProcessDma(&dma, CUR_PROCESS_HANDLE, (u32)dst, srcProcess, src, PROBE_SIZE, &cfg);
        if (R_FAILED(o.startRc))
            return o;

        DmaState st = DMASTATE_STARTING;
        while (true)
        {
            if (R_FAILED(svcGetDmaState(&st, dma)) || st == DMASTATE_DONE)
                break;
            if (bench_ticks_to_us(bench_ticks() - t0) > WAIT_US)
            {
                svcStopDma(dma);
                break;
            }
            svcSleepThread(100 * 1000);
        }
        o.latencyUs = (u32)bench_ticks_to_us(bench_ticks() - t0);
        o.finalState = (int)st;
        svcCloseHandle(dma);

        if (st == DMASTATE_DONE && expected)
        {
            svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, PROBE_SIZE);
            o.match = memcmp(dst, expected, VERIFY_BYTES) == 0 ? 1 : 0;
        }
        return o;
    }
}

namespace
{
    volatile u32 g_gameTopFb = 0, g_gameTopFormat = 0, g_gameTopStride = 0, g_gameCalls = 0;
}

bool DmaProbeTrackFramebuffer(const Screen &screen)
{
    g_gameCalls++;
    if (screen.IsTop && (screen.LeftFramebuffer & 0x80000000u))
    {
        g_gameTopFb = screen.LeftFramebuffer;
        g_gameTopFormat = (u32)screen.Format;
        g_gameTopStride = screen.Stride;
    }
    return false;
}

bool DmaProbeGameTop(u32 *fb, u32 *format, u32 *stride)
{
    *fb = g_gameTopFb;
    *format = g_gameTopFormat;
    *stride = g_gameTopStride;
    return *fb != 0;
}

u32 DmaProbeGameCalls(void)
{
    return g_gameCalls;
}

Handle DmaProbeOpenProcess(const char *wanted)
{
    u32 pids[64];
    s32 n = 0;
    if (R_FAILED(svcGetProcessList(&n, pids, 64)))
        return 0;
    for (s32 i = 0; i < n; i++)
    {
        Handle h = 0;
        if (R_FAILED(svcOpenProcess(&h, pids[i])))
            continue;
        char name[9] = { 0 };
        s64 raw = 0;
        if (R_SUCCEEDED(svcGetProcessInfo(&raw, h, 0x10000)))
            memcpy(name, &raw, 8);
        if (strcmp(name, wanted) == 0)
            return h;
        svcCloseHandle(h);
    }
    return 0;
}

void RunDmaProbe(char *summary, u32 summarySize)
{
    const u32 fb = g_gameTopFb;
    if (!fb)
    {
        snprintf(summary, summarySize, "No game framebuffer seen yet.\nClose the menu, play a few seconds, retry.");
        return;
    }
    const u32 pa = fb_alias_to_pa(fb);
    int vaKind = 0;
    const u32 gameVa = fb_resolve_game_va(fb, &vaKind);   // VRAM or FCRAM mapping verified via VA->PA
    const u32 vramVa = fb_pa_in_vram(pa) ? 0x1F000000 + (pa - 0x18000000) : 0;

    // Reference bytes: CPU read through the alias that the CPU-copy path uses.
    static u8 expectedFb[VERIFY_BYTES];
    memcpy(expectedFb, (const void *)fb, VERIFY_BYTES);

    u8 *heapSrc = (u8 *)memalign(0x1000, PROBE_SIZE);
    u8 *heapDst = (u8 *)memalign(0x1000, PROBE_SIZE);
    if (heapSrc)
    {
        for (u32 i = 0; i < PROBE_SIZE; i++)
            heapSrc[i] = (u8)(i * 7 + 3);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)heapSrc, PROBE_SIZE);
    }

    // Plain kernel-allocated memory mapped in this process (not the plugin block).
    u32 sysAddr = Process::GetFreeMemoryRegion(PROBE_SIZE, 0x0E000000);
    u32 sysOut = 0;
    const Result sysRc = sysAddr
        ? svcControlMemoryUnsafe(&sysOut, sysAddr, PROBE_SIZE,
                                 (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM | MEMOP_LINEAR_FLAG),
                                 (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))
        : -1;
    u8 *sysDst = R_SUCCEEDED(sysRc) ? (u8 *)sysOut : nullptr;

    const Handle home = OpenHomeProcess();

    Outcome out[SrcCount][DstCount];
    for (int d = 0; d < DstCount; d++)
    {
        u8 *dst = d == DstHeap ? heapDst : sysDst;
        out[SrcAlias][d] = Try(CUR_PROCESS_HANDLE, fb, dst, expectedFb);
        out[SrcGameVa][d] = Try(CUR_PROCESS_HANDLE, gameVa, dst, expectedFb);
        out[SrcVramHome][d] = home ? Try(home, vramVa, dst, expectedFb) : Outcome();
        if (!home)
            out[SrcVramHome][d].startRc = -2;
        out[SrcHeap][d] = Try(CUR_PROCESS_HANDLE, (u32)heapSrc, dst, heapSrc);
    }

    if (home)
        svcCloseHandle(home);
    if (sysDst)
        svcControlMemoryUnsafe(&sysOut, (u32)sysDst, PROBE_SIZE, MEMOP_FREE, (MemPerm)0);
    free(heapSrc);
    free(heapDst);

    static sdio_text_t t;
    char path[96];
    const char *model = System::IsNew3DS() ? "N3DS" : "O3DS";
    snprintf(path, sizeof(path), BENCH_DIR "/dma_probe_%s.csv", model);
    bool created = false;
    if (sdio_text_open(&t, path, true, &created))
    {
        if (created)
            sdio_text_printf(&t, "model,tid,fb,fb_format,game_va,game_va_kind,sys_alloc_addr,sys_alloc_rc,home_found,src,dst,"
                                 "src_addr,dst_addr,start_rc,final_state,latency_us,match\n");
        for (int s = 0; s < SrcCount; s++)
            for (int d = 0; d < DstCount; d++)
            {
                const Outcome &o = out[s][d];
                sdio_text_printf(&t, "%s,%016llX,0x%08lX,%lu,0x%08lX,%d,0x%08lX,0x%08lX,%d,%s,%s,0x%08lX,0x%08lX,0x%08lX,%d,%lu,%d\n",
                                 model, Process::GetTitleID(), fb, g_gameTopFormat, gameVa, vaKind, sysAddr, (u32)sysRc, home != 0,
                                 SRC_NAMES[s], DST_NAMES[d], o.srcAddr, o.dstAddr, (u32)o.startRc,
                                 o.finalState, o.latencyUs, o.match);
            }
        sdio_text_close(&t);
    }

    static const char *const KIND[] = { "none", "vram", "lin30", "lin14" };
    int n = snprintf(summary, summarySize, "fb 0x%08lX -> va 0x%08lX (%s), home %s\n", fb, gameVa, KIND[vaKind], home ? "yes" : "no");
    for (int s = 0; s < SrcCount && n > 0 && (u32)n < summarySize; s++)
        for (int d = 0; d < DstCount && (u32)n < summarySize; d++)
        {
            const Outcome &o = out[s][d];
            n += snprintf(summary + n, summarySize - n, "%s>%s %08lX st%d %s\n", SRC_NAMES[s], DST_NAMES[d],
                          (u32)o.startRc, o.finalState,
                          o.match == 1 ? "OK" : o.match == 0 ? "DIFF" : "-");
        }
}
