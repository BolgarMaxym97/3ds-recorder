#ifndef MEMPROBE_HPP
#define MEMPROBE_HPP

#include <3ds.h>

/*
 * T3: how much memory the plugin really gets, and whether it is physically
 * contiguous (usable as a GPU DMA target for T4's DisplayTransfer path).
 * Appends one row to BENCH_DIR/mem_<model>.csv and fills `summary`.
 */
void RunMemProbe(char *summary, u32 summarySize);

#endif
