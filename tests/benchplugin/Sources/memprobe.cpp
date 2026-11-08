#include "memprobe.hpp"

#include <CTRPluginFramework.hpp>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include "sdio.h"

using namespace CTRPluginFramework;

/* Luma custom SVC (implemented in libctrpf). */
extern "C" u32 svcConvertVAToPA(const void *VA, bool writeCheck);

namespace
{
    constexpr u32 KB = 1024;
    constexpr u32 PROBE_CHUNK = 64 * KB;
    constexpr u32 MAX_CHUNKS = 256; // 16 MB, more than any plugin block

    /* Largest single malloc that succeeds, by binary search (64 KB granularity). */
    u32 LargestBlock(u32 upper)
    {
        u32 lo = 0, hi = upper / PROBE_CHUNK;
        while (lo < hi)
        {
            u32 mid = (lo + hi + 1) / 2;
            void *p = malloc(mid * PROBE_CHUNK);
            if (p)
            {
                free(p);
                lo = mid;
            }
            else
                hi = mid - 1;
        }
        return lo * PROBE_CHUNK;
    }

    /* Total memory obtainable in 64 KB pieces. */
    u32 TotalInChunks(void)
    {
        static void *chunks[MAX_CHUNKS];
        u32 n = 0;
        while (n < MAX_CHUNKS && (chunks[n] = malloc(PROBE_CHUNK)) != nullptr)
            n++;
        for (u32 i = 0; i < n; i++)
            free(chunks[i]);
        return n * PROBE_CHUNK;
    }

    /* True if every page of [p, p+size) maps to consecutive physical pages. */
    bool IsContiguous(const u8 *p, u32 size, u32 *firstPa)
    {
        const u32 base = svcConvertVAToPA(p, false);
        *firstPa = base;
        if (base == 0)
            return false;
        for (u32 off = 0x1000; off < size; off += 0x1000)
            if (svcConvertVAToPA(p + off, false) != base + off)
                return false;
        return true;
    }
}

void RunMemProbe(char *summary, u32 summarySize)
{
    const PluginHeader *hdr = (const PluginHeader *)0x07000000;
    const u32 heapSize = hdr->heapSize;
    const u32 exeSize = hdr->exeSize;
    const u32 blockSize = heapSize + exeSize;

    const u32 largest = LargestBlock(heapSize);
    const u32 total = TotalInChunks();

    u32 pa = 0;
    bool contiguous = false;
    const u32 testSize = largest > 4 * 1024 * KB ? 4 * 1024 * KB : largest;
    if (testSize >= 0x1000)
    {
        u8 *p = (u8 *)memalign(0x1000, testSize);
        if (p)
        {
            contiguous = IsContiguous(p, testSize, &pa);
            free(p);
        }
    }

    const s64 appSize = osGetMemRegionSize(MEMREGION_APPLICATION);
    const s64 appFree = osGetMemRegionFree(MEMREGION_APPLICATION);
    const s64 sysSize = osGetMemRegionSize(MEMREGION_SYSTEM);
    const s64 sysFree = osGetMemRegionFree(MEMREGION_SYSTEM);
    const s64 baseFree = osGetMemRegionFree(MEMREGION_BASE);

    static sdio_text_t t;
    char path[96];
    const char *model = System::IsNew3DS() ? "N3DS" : "O3DS";
    snprintf(path, sizeof(path), BENCH_DIR "/mem_%s.csv", model);
    bool created = false;
    if (sdio_text_open(&t, path, true, &created))
    {
        if (created)
            sdio_text_printf(&t, "model,tid,block_kb,exe_kb,heap_kb,largest_malloc_kb,total_malloc_kb,"
                                 "contig_test_kb,contiguous,first_pa,app_kb,app_free_kb,sys_kb,sys_free_kb,base_free_kb\n");
        sdio_text_printf(&t, "%s,%016llX,%lu,%lu,%lu,%lu,%lu,%lu,%d,0x%08lX,%lld,%lld,%lld,%lld,%lld\n",
                         model, Process::GetTitleID(), blockSize / KB, exeSize / KB, heapSize / KB,
                         largest / KB, total / KB, testSize / KB, contiguous, pa,
                         appSize / KB, appFree / KB, sysSize / KB, sysFree / KB, baseFree / KB);
        sdio_text_close(&t);
    }

    snprintf(summary, summarySize,
             "Plugin block: %lu KB (exe %lu, heap %lu)\n"
             "Largest malloc: %lu KB\n"
             "Total malloc: %lu KB\n"
             "Physically contiguous (%lu KB): %s\n"
             "PA 0x%08lX\n"
             "APP free %lld / %lld KB\n"
             "SYS free %lld / %lld KB",
             blockSize / KB, exeSize / KB, heapSize / KB, largest / KB, total / KB,
             testSize / KB, contiguous ? "yes" : "NO", pa,
             appFree / KB, appSize / KB, sysFree / KB, sysSize / KB);
}
