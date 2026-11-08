#ifndef DMAPROBE_HPP
#define DMAPROBE_HPP

#include <3ds.h>

/*
 * T4 DMA probe: tries svcStartInterProcessDma for every combination of
 * source (framebuffer alias, VRAM in the game process, VRAM via the HOME menu
 * process, plugin heap) and destination (plugin heap, kernel-allocated SYSTEM
 * memory). Records start result, final DMA state and whether the copied bytes
 * match. Appends to BENCH_DIR/dma_probe_<model>.csv and fills `summary`.
 * Run it from the menu (game paused).
 */
void RunDmaProbe(char *summary, u32 summarySize);

/*
 * Must run for the whole plugin lifetime (OSD::Run at startup): remembers the
 * game's own top framebuffer. While the CTRPF menu is open the OSD screen
 * points at CTRPF's buffer in the plugin heap (0x06000000), so the probe has
 * to use the last address seen during gameplay (game fbs come as 0x80000000|PA).
 */
namespace CTRPluginFramework { class Screen; }
bool DmaProbeTrackFramebuffer(const CTRPluginFramework::Screen &screen);
/* Last game top framebuffer seen during gameplay (0x80000000|PA alias), its format and stride. */
bool DmaProbeGameTop(u32 *fb, u32 *format, u32 *stride);
/* OSD callback count: stops growing while the game does not present frames (HOME menu, sleep). */
u32  DmaProbeGameCalls(void);
/* Opens a process by its 8-char name ("menu", "gsp", "rosalina"), 0 if not found. */
Handle DmaProbeOpenProcess(const char *name);

#endif
