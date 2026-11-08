#include "bench.h"

#include <string.h>

#ifdef BENCH_HOST
#include <time.h>

uint64_t bench_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * BENCH_TICKS_PER_SEC
         + (uint64_t)ts.tv_nsec * BENCH_TICKS_PER_SEC / 1000000000ULL;
}
#else
#include <3ds.h>

uint64_t bench_ticks(void)
{
    return svcGetSystemTick();
}
#endif

/* ---------------------------------------------------------------- histogram */

static uint32_t hist_bin_index(uint32_t us)
{
    const uint32_t fine_limit = BENCH_HIST_FINE_BINS * BENCH_HIST_FINE_US;
    if (us < fine_limit) {
        return us / BENCH_HIST_FINE_US;
    }
    uint32_t coarse = (us - fine_limit) / BENCH_HIST_COARSE_US;
    if (coarse < BENCH_HIST_COARSE_BINS) {
        return BENCH_HIST_FINE_BINS + coarse;
    }
    return BENCH_HIST_BINS - 1u;
}

/* Exclusive upper edge of a bin; the overflow bin has no edge (UINT32_MAX). */
static uint32_t hist_bin_upper_us(uint32_t idx)
{
    if (idx < BENCH_HIST_FINE_BINS) {
        return (idx + 1u) * BENCH_HIST_FINE_US;
    }
    if (idx < BENCH_HIST_FINE_BINS + BENCH_HIST_COARSE_BINS) {
        return BENCH_HIST_FINE_BINS * BENCH_HIST_FINE_US
             + (idx - BENCH_HIST_FINE_BINS + 1u) * BENCH_HIST_COARSE_US;
    }
    return UINT32_MAX;
}

void bench_hist_reset(bench_hist_t *h)
{
    memset(h, 0, sizeof(*h));
    h->min_us = UINT32_MAX;
}

void bench_hist_add_us(bench_hist_t *h, uint32_t us)
{
    h->count++;
    h->sum_us += us;
    if (us < h->min_us) {
        h->min_us = us;
    }
    if (us > h->max_us) {
        h->max_us = us;
    }
    h->bins[hist_bin_index(us)]++;
}

uint32_t bench_hist_percentile_us(const bench_hist_t *h, double p)
{
    if (h->count == 0) {
        return 0;
    }
    if (p <= 0.0) {
        return h->min_us;
    }
    if (p >= 100.0) {
        return h->max_us;
    }

    /* Nearest-rank: smallest value with at least p% of samples at or below it. */
    uint64_t rank = (uint64_t)(p / 100.0 * (double)h->count + 0.999999);
    if (rank == 0) {
        rank = 1;
    }

    uint64_t seen = 0;
    for (uint32_t i = 0; i < BENCH_HIST_BINS; i++) {
        seen += h->bins[i];
        if (seen >= rank) {
            uint32_t upper = hist_bin_upper_us(i);
            return upper < h->max_us ? upper : h->max_us;
        }
    }
    return h->max_us;
}

uint32_t bench_hist_mean_us(const bench_hist_t *h)
{
    return h->count ? (uint32_t)(h->sum_us / h->count) : 0;
}

/* ------------------------------------------------------------------ I/O log */

void bench_io_log_init(bench_io_log_t *log, bench_io_event_t *storage, uint32_t cap)
{
    log->ev = storage;
    log->cap = cap;
    log->n = 0;
    log->dropped = 0;
}

bool bench_io_log_add(bench_io_log_t *log, uint64_t start_tick, uint64_t end_tick, uint32_t bytes)
{
    if (log->n >= log->cap) {
        log->dropped++;
        return false;
    }
    bench_io_event_t *e = &log->ev[log->n++];
    e->start_tick = start_tick;
    e->end_tick = end_tick;
    e->bytes = bytes;
    return true;
}

/*
 * B(x) = bytes completed by tick x (each write spread evenly over its own
 * duration). Queried with non-decreasing x, so it advances like a cursor.
 */
typedef struct {
    const bench_io_event_t *ev;
    uint32_t n;
    uint32_t i;     /* first event with end_tick > last x */
    double done;    /* bytes of events before i */
} bytes_cursor_t;

static double cursor_bytes_at(bytes_cursor_t *c, uint64_t x)
{
    while (c->i < c->n && c->ev[c->i].end_tick <= x) {
        c->done += (double)c->ev[c->i].bytes;
        c->i++;
    }
    if (c->i < c->n && c->ev[c->i].start_tick < x) {
        const bench_io_event_t *e = &c->ev[c->i];
        return c->done + (double)e->bytes * (double)(x - e->start_tick)
                         / (double)(e->end_tick - e->start_tick);
    }
    return c->done;
}

/* Breakpoint k of the run: event k/2's start (even k) or end (odd k). */
static uint64_t breakpoint(const bench_io_event_t *ev, uint32_t k)
{
    return (k & 1u) ? ev[k >> 1].end_tick : ev[k >> 1].start_tick;
}

double bench_io_min_window_rate(const bench_io_event_t *ev, uint32_t n, uint64_t window_ticks)
{
    if (n == 0 || window_ticks == 0) {
        return -1.0;
    }
    const uint64_t run_start = ev[0].start_tick;
    const uint64_t run_end = ev[n - 1].end_tick;
    if (run_end - run_start < window_ticks) {
        return -1.0;
    }

    /*
     * B(t) - B(t - W) is piecewise linear in t; its minimum lies where t or
     * t - W hits a breakpoint. Merge both sorted candidate streams (P and
     * P + W) and evaluate each candidate inside [run_start + W, run_end].
     */
    bytes_cursor_t right = { ev, n, 0, 0.0 };
    bytes_cursor_t left = { ev, n, 0, 0.0 };
    const uint32_t points = 2u * n;
    uint32_t a = 0; /* index into P      (right edge on a breakpoint) */
    uint32_t b = 0; /* index into P + W  (left edge on a breakpoint)  */
    double min_bytes = -1.0;

    while (a < points || b < points) {
        uint64_t ta = a < points ? breakpoint(ev, a) : UINT64_MAX;
        uint64_t tb = b < points ? breakpoint(ev, b) + window_ticks : UINT64_MAX;
        uint64_t t;
        if (ta <= tb) {
            t = ta;
            a++;
        } else {
            t = tb;
            b++;
        }
        if (t < run_start + window_ticks) {
            continue;
        }
        if (t > run_end) {
            break;
        }
        double bytes = cursor_bytes_at(&right, t) - cursor_bytes_at(&left, t - window_ticks);
        if (min_bytes < 0.0 || bytes < min_bytes) {
            min_bytes = bytes;
        }
    }
    return min_bytes * (double)BENCH_TICKS_PER_SEC / (double)window_ticks;
}
