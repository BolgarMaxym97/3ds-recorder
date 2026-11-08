#ifndef HOMEPROBE_HPP
#define HOMEPROBE_HPP

#include <3ds.h>

/*
 * Phase-7 feasibility probe (full-console recording, plan 3.4), bench v0.12.
 *
 * A low-priority background thread (core 0, 0x3F) samples every 200 ms:
 *   - the LCD controller (PDC) framebuffer registers, read through the
 *     0x80000000|PA alias only if svcQueryMemory reports it mapped (a DMA from
 *     the GSP process' IO mapping caused a kernel panic: never do that);
 *   - GSPGPU ImportDisplayCaptureInfo on a session of our own;
 *   - whether the game still presents frames (OSD callbacks).
 * Sampling starts 20 s after plugin start.
 * When the screen shows a framebuffer that is not the game's (HOME Menu), it
 * tries to DMA 4 KB of it: through our own VRAM mapping and through the HOME
 * Menu process, and compares with a CPU read of the 0x80000000|PA alias.
 *
 * Answers: does a plugin keep running while HOME Menu is open, can it see
 * which buffer is on screen, and can it copy that buffer by DMA.
 */
namespace HomeProbe
{
    void Start(void);
    void Stop(void);
    /* Writes BENCH_DIR/home12_<model>_<tid>.csv and fills a short summary. */
    void Report(char *summary, u32 summarySize);
}

#endif
