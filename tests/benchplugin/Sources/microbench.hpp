#ifndef MICROBENCH_HPP
#define MICROBENCH_HPP

#include <3ds.h>

/*
 * T5 microbench (bench v0.12): where does the encoder's time go?
 *
 * Runs ~1-2 s on core 0 at high priority (the game pauses meanwhile; it runs
 * before the autotest baseline). All numbers are system ticks (268 MHz: on
 * O3DS one tick = one CPU cycle).
 *   - read cost per 32-bit word, L1-resident (4 KB x 64) and streamed (64 KB,
 *     cache invalidated first), for plugin heap, plugin static data (.bss),
 *     kernel-allocated SYSTEM memory and the game framebuffer alias;
 *   - memcpy, cache invalidate / flush of 192 KB;
 *   - XR32 encoder cycles per pixel for every memory-hint variant and the old
 *     XOR-RLE, on two real consecutive game frames (DMA'd), cold cache.
 * The fastest XR32 variant is returned for the recorder. Appends a row to
 * BENCH_DIR/mem12_<model>_<tid>.csv.
 */
namespace Microbench
{
    struct Report
    {
        bool   valid = false;
        bool   realFrames = false;      ///< encoder ran on DMA'd game frames (else synthetic)
        double l1[4] = { 0, 0, 0, 0 };  ///< ticks/word: heap, bss, sys, fb alias
        double stream[4] = { 0, 0, 0, 0 };
        double memcpyTpb = 0;           ///< ticks per byte, 192 KB heap -> heap
        u32    invalidateTicks = 0, flushTicks = 0;   ///< 192 KB
        double encXr32[3] = { 0, 0, 0 };              ///< cycles/pixel, variants 0..2
        double encXrle = 0;
        double encRatio = 0;
        int    bestVariant = 0;
        bool   heapLooksUncached = false;
        bool   sysLooksCached = false;
    };

    /* Blocks the caller ~1-2 s. `summary` gets two short lines for the autotest table. */
    const Report &Run(char *summary, u32 summarySize);
    const Report &Last(void);
}

#endif
