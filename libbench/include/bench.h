#ifndef BENCH_H
#define BENCH_H

/*
 * libbench — allocation-free measurement helpers shared by the phase-0
 * benchmarks (sdbench.3dsx now, the .3gx benches later).
 *
 * Time base: the ARM11 system tick (svcGetSystemTick), 268 111 856 Hz on both
 * Old and New 3DS. Build with -DBENCH_HOST to compile the same code on a PC
 * (host unit tests), where the tick comes from a monotonic clock.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BENCH_TICKS_PER_SEC 268111856ULL

uint64_t bench_ticks(void);

static inline uint64_t bench_ticks_to_us(uint64_t ticks)
{
    return ticks * 1000000ULL / BENCH_TICKS_PER_SEC;
}

static inline uint64_t bench_us_to_ticks(uint64_t us)
{
    return us * BENCH_TICKS_PER_SEC / 1000000ULL;
}

/*
 * Duration histogram with fixed bins, so percentiles need no stored samples:
 *   0 .. 100 ms  in 0.1 ms bins (1000 bins)
 *   100 ms .. 5 s in 10 ms bins  (490 bins)
 *   >= 5 s       one overflow bin
 * min/max/sum are exact; percentiles are the upper edge of the bin that holds
 * the requested rank, clamped to the exact max.
 */
#define BENCH_HIST_FINE_BINS    1000u
#define BENCH_HIST_FINE_US      100u
#define BENCH_HIST_COARSE_BINS  490u
#define BENCH_HIST_COARSE_US    10000u
#define BENCH_HIST_BINS         (BENCH_HIST_FINE_BINS + BENCH_HIST_COARSE_BINS + 1u)

typedef struct {
    uint64_t count;
    uint64_t sum_us;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t bins[BENCH_HIST_BINS];
} bench_hist_t;

void     bench_hist_reset(bench_hist_t *h);
void     bench_hist_add_us(bench_hist_t *h, uint32_t us);
/* p in [0, 100]. Returns 0 for an empty histogram. */
uint32_t bench_hist_percentile_us(const bench_hist_t *h, double p);
uint32_t bench_hist_mean_us(const bench_hist_t *h);

/*
 * I/O event log in caller-provided storage: one entry per completed write.
 * Used afterwards to compute the worst sustained throughput.
 */
typedef struct {
    uint64_t start_tick;
    uint64_t end_tick;
    uint32_t bytes;
} bench_io_event_t;

typedef struct {
    bench_io_event_t *ev;
    uint32_t cap;
    uint32_t n;
    uint32_t dropped;
} bench_io_log_t;

void bench_io_log_init(bench_io_log_t *log, bench_io_event_t *storage, uint32_t cap);
bool bench_io_log_add(bench_io_log_t *log, uint64_t start_tick, uint64_t end_tick, uint32_t bytes);

/*
 * Minimum throughput (bytes/s) over any window of `window_ticks` that lies
 * fully inside [first event start, last event end]. Each write's bytes are
 * spread evenly over its own duration; gaps between writes (SetSize, flush,
 * anything else) count as zero bytes, so stalls lower the result.
 * Windows are evaluated ending at every event end, which is exact for the
 * right edge and a close approximation for the left one.
 * Returns -1.0 if the run is shorter than one window.
 */
double bench_io_min_window_rate(const bench_io_event_t *ev, uint32_t n, uint64_t window_ticks);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_H */
