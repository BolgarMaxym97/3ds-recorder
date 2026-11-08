#include "homeprobe.hpp"

#include <CTRPluginFramework.hpp>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "dmaprobe.hpp"
#include "fbaddr.h"
#include "sdio.h"

using namespace CTRPluginFramework;

/* Luma custom SVC (libctrpf). op 4 = PROCESSOP_GET_PA_FROM_VA: varg2 = u32 *pa, varg3 = VA. */
extern "C" Result svcControlProcess(Handle process, u32 op, u32 varg2, u32 varg3);

namespace HomeProbe
{
namespace
{
    constexpr u32 PERIOD_MS = 200;
    constexpr u32 MAX_SAMPLES = 1500;             // 5 min, oldest overwritten
    constexpr u32 MAX_ATTEMPTS = 8;
    // PDC registers through the 0x80000000|PA alias CTRPF uses, only if the kernel reports
    // that range as mapped and readable in this process. (v0.12 first build read them by DMA
    // through the GSP process' IO mapping: kernel panic at game start, crash_dump_00000017.)
    constexpr u32 PDC_ALIAS = 0x80000000u | 0x10400468u;
    constexpr u32 PDC_BYTES = 0x120;              // up to bottom select (0x578)
    constexpr u32 START_DELAY_MS = 20000;         // nothing at game boot
    constexpr u32 PROBE_BYTES = 4096;
    constexpr u32 OP_GET_PA_FROM_VA = 4;

    struct Sample
    {
        u32 tMs, calls;
        u32 topA, topB, topFmt, topSel, botA, botB, botSel;
        u32 capTop0, capBot0;
        Result pdcRc, capRc;
    };

    struct Attempt
    {
        u32 tMs, pa;
        u32 ownVa;
        Result ownRc;
        int ownMatch;
        u32 homeVa;
        Result homeRc;
        int homeMatch;
    };

    Thread        g_thread = nullptr;
    volatile bool g_stop = false;
    Sample       *g_samples = nullptr;
    u32           g_count = 0;                      // total taken (index = count % MAX)
    Attempt       g_attempts[MAX_ATTEMPTS];
    u32           g_attemptN = 0;
    u32           g_known[8];                       // PAs that belonged to the game
    u32           g_knownN = 0;
    u32           g_maxGapMs = 0, g_longestStallMs = 0;
    Result        g_gspRc = 0, g_paSelfCheck = -1, g_pdcQueryRc = -1;
    u32           g_pdcState = 0, g_pdcPerm = 0, g_pdcBase = 0, g_pdcSize = 0;
    bool          g_pdcReadable = false;
    bool          g_paOrderOk = false;
    u32           g_pdcMatchGame = 0, g_pdcOk = 0;

    u32 NowMs(void) { return (u32)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000)); }

    bool Known(u32 pa)
    {
        for (u32 i = 0; i < g_knownN; i++)
            if (g_known[i] == pa)
                return true;
        return false;
    }

    void AddKnown(u32 pa)
    {
        if (!pa || Known(pa))
            return;
        g_known[g_knownN % 8] = pa;
        if (g_knownN < 8)
            g_knownN++;
    }

    Result Dma(Handle srcProcess, u32 srcVa, u8 *dst, u32 size)
    {
        DmaConfig cfg;
        dmaConfigInitDefault(&cfg);
        cfg.flags = 0;
        Handle h = 0;
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, size);
        Result rc = svcStartInterProcessDma(&h, CUR_PROCESS_HANDLE, (u32)dst, srcProcess, srcVa, size, &cfg);
        if (R_FAILED(rc))
            return rc;
        rc = (Result)0xDEAD0010;   // timeout
        for (int i = 0; i < 100; i++)
        {
            DmaState st = DMASTATE_STARTING;
            if (R_FAILED(svcGetDmaState(&st, h)))
                break;
            if (st == DMASTATE_DONE)
            {
                rc = 0;
                break;
            }
            svcSleepThread(200 * 1000);
        }
        if (rc)
            svcStopDma(h);
        svcCloseHandle(h);
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, size);
        return rc;
    }

    /* GSPGPU::ImportDisplayCaptureInfo (0x18) on our own session. */
    Result CaptureInfo(Handle gsp, GSPGPU_CaptureInfo *info)
    {
        u32 *cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(0x18, 0, 0);
        Result rc = svcSendSyncRequest(gsp);
        if (R_FAILED(rc))
            return rc;
        memcpy(info, &cmd[2], sizeof(*info));
        return (Result)cmd[1];
    }

    /* Luma's GET_PA_FROM_VA argument order, checked on our own buffer (harmless either way). */
    void SelfCheckPaOrder(u8 *buf)
    {
        u32 pa = 0;
        g_paSelfCheck = svcControlProcess(CUR_PROCESS_HANDLE, OP_GET_PA_FROM_VA, (u32)&pa, (u32)buf);
        g_paOrderOk = R_SUCCEEDED(g_paSelfCheck) && pa != 0 && pa == svcConvertVAToPA(buf, false);
    }

    u32 HomeVaFor(Handle home, u32 pa)
    {
        if (!home || !g_paOrderOk)
            return 0;
        u32 cands[3] = { 0, 0, 0 };
        if (fb_pa_in_vram(pa))
            cands[0] = 0x1F000000 + (pa - 0x18000000);
        if (fb_pa_in_fcram(pa))
        {
            cands[1] = 0x30000000 + (pa - 0x20000000);
            cands[2] = 0x14000000 + (pa - 0x20000000);
        }
        for (u32 va : cands)
        {
            u32 got = 0;
            if (va && R_SUCCEEDED(svcControlProcess(home, OP_GET_PA_FROM_VA, (u32)&got, va & ~0xFFFu)) &&
                got == (pa & ~0xFFFu))
                return va;
        }
        return 0;
    }

    void TryForeign(u32 tMs, u32 pa, Handle home, u8 *buf)
    {
        if (g_attemptN >= MAX_ATTEMPTS)
            return;
        for (u32 i = 0; i < g_attemptN; i++)
            if (g_attempts[i].pa == pa)
                return;
        Attempt &a = g_attempts[g_attemptN++];
        memset(&a, 0, sizeof(a));
        a.tMs = tMs;
        a.pa = pa;
        a.ownMatch = a.homeMatch = -1;
        a.ownRc = a.homeRc = -1;
        if (!fb_pa_in_vram(pa) && !fb_pa_in_fcram(pa))
            return;
        // Reference: CPU read through the uncached PA alias (what CTRPF draws through),
        // only where the kernel reports the alias mapped and readable.
        static u8 ref[PROBE_BYTES];
        const u32 alias = 0x80000000u | pa;
        MemInfo mi;
        PageInfo pi;
        memset(&mi, 0, sizeof(mi));
        const bool aliasOk = R_SUCCEEDED(svcQueryMemory(&mi, &pi, alias)) && mi.state != MEMSTATE_FREE &&
                             (mi.perm & MEMPERM_READ) && mi.base_addr + mi.size >= alias + PROBE_BYTES;
        if (aliasOk)
            memcpy(ref, (const void *)alias, PROBE_BYTES);

        a.ownVa = fb_resolve_game_va(0x80000000u | pa, nullptr);
        if (a.ownVa)
        {
            a.ownRc = Dma(CUR_PROCESS_HANDLE, a.ownVa, buf, PROBE_BYTES);
            if (R_SUCCEEDED(a.ownRc))
                a.ownMatch = aliasOk ? memcmp(buf, ref, PROBE_BYTES) == 0 : 2;   // 2 = copied, not verified
        }
        a.homeVa = HomeVaFor(home, pa);
        if (a.homeVa)
        {
            a.homeRc = Dma(home, a.homeVa, buf, PROBE_BYTES);
            if (R_SUCCEEDED(a.homeRc))
                a.homeMatch = aliasOk ? memcmp(buf, ref, PROBE_BYTES) == 0 : 2;
        }
    }

    void ThreadMain(void *)
    {
        u8 *buf = (u8 *)memalign(0x1000, PROBE_BYTES);
        u8 *pdc = (u8 *)memalign(0x40, 0x200);
        if (!buf || !pdc)
            return;
        for (u32 waited = 0; waited < START_DELAY_MS && !g_stop; waited += 500)
            svcSleepThread(500ull * 1000 * 1000);
        SelfCheckPaOrder(buf);
        {
            MemInfo mi;
            PageInfo pi;
            memset(&mi, 0, sizeof(mi));
            g_pdcQueryRc = svcQueryMemory(&mi, &pi, PDC_ALIAS);
            g_pdcState = mi.state;
            g_pdcPerm = mi.perm;
            g_pdcBase = mi.base_addr;
            g_pdcSize = mi.size;
            g_pdcReadable = R_SUCCEEDED(g_pdcQueryRc) && mi.state != MEMSTATE_FREE && (mi.perm & MEMPERM_READ) &&
                            mi.base_addr <= PDC_ALIAS && mi.base_addr + mi.size >= PDC_ALIAS + PDC_BYTES;
        }
        Handle home = DmaProbeOpenProcess("menu");
        Handle gsp = 0;
        g_gspRc = srvGetServiceHandle(&gsp, "gsp::Gpu");

        u32 lastMs = NowMs(), lastCalls = DmaProbeGameCalls(), stallStart = lastMs;
        while (!g_stop)
        {
            svcSleepThread((u64)PERIOD_MS * 1000 * 1000);
            const u32 now = NowMs();
            const u32 gap = now - lastMs;
            if (gap > g_maxGapMs && g_count > 0)
                g_maxGapMs = gap;
            lastMs = now;

            Sample &s = g_samples[g_count % MAX_SAMPLES];
            memset(&s, 0, sizeof(s));
            s.tMs = now;
            s.calls = DmaProbeGameCalls();
            if (s.calls != lastCalls)
                stallStart = now;
            else if (now - stallStart > g_longestStallMs)
                g_longestStallMs = now - stallStart;
            const bool gameAlive = s.calls != lastCalls;
            lastCalls = s.calls;

            s.pdcRc = g_pdcReadable ? 0 : (Result)0xDEAD0011;
            if (g_pdcReadable)
            {
                const volatile u32 *io = (const volatile u32 *)PDC_ALIAS;
                u32 *r = (u32 *)pdc;
                for (u32 i = 0; i < PDC_BYTES / 4; i++)
                    r[i] = io[i];
                s.topA = r[0];
                s.topB = r[1];
                s.topFmt = r[2];
                s.topSel = r[4];
                s.botA = r[0x100 / 4];
                s.botB = r[0x104 / 4];
                s.botSel = r[0x110 / 4];
                g_pdcOk++;
                const u32 active = (s.topSel & 1) ? s.topB : s.topA;
                u32 fb = 0, fmt = 0, stride = 0;
                if (gameAlive && DmaProbeGameTop(&fb, &fmt, &stride))
                {
                    const u32 gamePa = fb_alias_to_pa(fb);
                    if (gamePa == s.topA || gamePa == s.topB)
                    {
                        g_pdcMatchGame++;
                        AddKnown(s.topA);
                        AddKnown(s.topB);
                    }
                }
                if (!gameAlive && g_knownN && active && !Known(active))
                    TryForeign(now, active, home, buf);
            }
            GSPGPU_CaptureInfo ci;
            memset(&ci, 0, sizeof(ci));
            s.capRc = gsp ? CaptureInfo(gsp, &ci) : g_gspRc;
            s.capTop0 = (u32)ci.screencapture[0].framebuf0_vaddr;
            s.capBot0 = (u32)ci.screencapture[1].framebuf0_vaddr;
            g_count++;
        }
        if (gsp)
            svcCloseHandle(gsp);
        if (home)
            svcCloseHandle(home);
        free(buf);
        free(pdc);
    }
} // namespace

void Start(void)
{
    if (g_thread)
        return;
    if (!g_samples)
        g_samples = (Sample *)malloc(sizeof(Sample) * MAX_SAMPLES);
    if (!g_samples)
        return;
    g_stop = false;
    // Lowest application priority on the game's core: runs only in idle time (and while the game is suspended).
    g_thread = threadCreate(ThreadMain, nullptr, 8 * 1024, 0x3F, 0, false);
}

void Stop(void)
{
    if (!g_thread)
        return;
    g_stop = true;
    threadJoin(g_thread, 2ull * 1000 * 1000 * 1000);
    threadFree(g_thread);
    g_thread = nullptr;
}

void Report(char *summary, u32 summarySize)
{
    const u32 n = g_count < MAX_SAMPLES ? g_count : MAX_SAMPLES;
    static sdio_text_t t;
    char path[96];
    const char *model = System::IsNew3DS() ? "N3DS" : "O3DS";
    snprintf(path, sizeof(path), BENCH_DIR "/home12_%s_%016llX.csv", model, Process::GetTitleID());
    if (sdio_text_open(&t, path, false, nullptr))
    {
        sdio_text_printf(&t, "# gsp_rc=0x%08lX pa_selfcheck=0x%08lX pa_order_ok=%d pdc_ok=%lu pdc_match_game=%lu "
                             "max_gap_ms=%lu longest_stall_ms=%lu known=%lu pdc_query_rc=0x%08lX state=%lu perm=%lu "
                             "base=0x%08lX size=0x%lX\n",
                         (u32)g_gspRc, (u32)g_paSelfCheck, g_paOrderOk, g_pdcOk, g_pdcMatchGame, g_maxGapMs,
                         g_longestStallMs, g_knownN, (u32)g_pdcQueryRc, g_pdcState, g_pdcPerm, g_pdcBase, g_pdcSize);
        sdio_text_printf(&t, "kind,t_ms,calls,top_a,top_b,top_fmt,top_sel,bot_a,bot_b,bot_sel,cap_top0,cap_bot0,pdc_rc,cap_rc\n");
        const u32 first = g_count - n;
        for (u32 i = first; i < g_count; i++)
        {
            const Sample &s = g_samples[i % MAX_SAMPLES];
            sdio_text_printf(&t, "s,%lu,%lu,0x%08lX,0x%08lX,0x%lX,%lu,0x%08lX,0x%08lX,%lu,0x%08lX,0x%08lX,0x%08lX,0x%08lX\n",
                             s.tMs, s.calls, s.topA, s.topB, s.topFmt, s.topSel, s.botA, s.botB, s.botSel,
                             s.capTop0, s.capBot0, (u32)s.pdcRc, (u32)s.capRc);
        }
        sdio_text_printf(&t, "kind,t_ms,pa,own_va,own_rc,own_match,home_va,home_rc,home_match\n");
        for (u32 i = 0; i < g_attemptN; i++)
        {
            const Attempt &a = g_attempts[i];
            sdio_text_printf(&t, "a,%lu,0x%08lX,0x%08lX,0x%08lX,%d,0x%08lX,0x%08lX,%d\n", a.tMs, a.pa, a.ownVa,
                             (u32)a.ownRc, a.ownMatch, a.homeVa, (u32)a.homeRc, a.homeMatch);
        }
        sdio_text_close(&t);
    }

    int ownOk = 0, homeOk = 0;
    for (u32 i = 0; i < g_attemptN; i++)
    {
        ownOk += g_attempts[i].ownMatch >= 1;
        homeOk += g_attempts[i].homeMatch >= 1;
    }
    const Sample *last = n ? &g_samples[(g_count - 1) % MAX_SAMPLES] : nullptr;
    snprintf(summary, summarySize,
             "Samples %lu, max gap %lu ms, game paused max %lu s\n"
             "PDC alias: query %08lX st %lu, ok %lu, = game fb %lu\n"
             "Capture info rc %08lX, top %08lX\n"
             "Foreign fbs %lu: DMA ok own %d, via HOME %d\n"
             "PA order ok %d; game PAs known %lu",
             g_count, g_maxGapMs, g_longestStallMs / 1000, (u32)g_pdcQueryRc, g_pdcState, g_pdcOk, g_pdcMatchGame,
             last ? (u32)last->capRc : 0, last ? last->capTop0 : 0,
             g_attemptN, ownOk, homeOk, g_paOrderOk, g_knownN);
}
} // namespace HomeProbe
