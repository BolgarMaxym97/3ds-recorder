/*
 * Host unit tests for libbench. Run: make -C tests/host
 */
#include "bench.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define CHECK_NEAR(actual, expected, rel) do { \
    double a_ = (actual), e_ = (expected); \
    if (fabs(a_ - e_) > fabs(e_) * (rel) + 1e-9) { \
        fprintf(stderr, "FAIL %s:%d: %s = %f, expected %f\n", __FILE__, __LINE__, #actual, a_, e_); \
        failures++; \
    } \
} while (0)

static const uint64_t SEC = BENCH_TICKS_PER_SEC;
static const double MB = 1024.0 * 1024.0;

static bench_hist_t hist;

static void test_hist_empty(void)
{
    bench_hist_reset(&hist);
    CHECK(bench_hist_percentile_us(&hist, 50) == 0);
    CHECK(bench_hist_mean_us(&hist) == 0);
}

static void test_hist_fine_range(void)
{
    bench_hist_reset(&hist);
    for (uint32_t us = 1; us <= 1000; us++) {
        bench_hist_add_us(&hist, us);
    }
    CHECK(hist.count == 1000);
    CHECK(hist.min_us == 1);
    CHECK(hist.max_us == 1000);
    CHECK(bench_hist_mean_us(&hist) == 500);
    /* 500th value is 500 us, it sits in bin [500, 600) -> reported 600. */
    CHECK(bench_hist_percentile_us(&hist, 50) == 600);
    /* 990th value is 990 us, bin [900, 1000) -> 1000, clamped to max 1000. */
    CHECK(bench_hist_percentile_us(&hist, 99) == 1000);
    CHECK(bench_hist_percentile_us(&hist, 0) == 1);
    CHECK(bench_hist_percentile_us(&hist, 100) == 1000);
}

static void test_hist_tail(void)
{
    bench_hist_reset(&hist);
    for (int i = 0; i < 999; i++) {
        bench_hist_add_us(&hist, 2000);
    }
    bench_hist_add_us(&hist, 250000); /* one 250 ms stall in the coarse range */
    CHECK(bench_hist_percentile_us(&hist, 99.9) == 2100);
    CHECK(bench_hist_percentile_us(&hist, 99.95) == 250000);
    CHECK(hist.max_us == 250000);

    bench_hist_add_us(&hist, 10000000); /* 10 s -> overflow bin */
    CHECK(bench_hist_percentile_us(&hist, 100) == 10000000);
    CHECK(bench_hist_percentile_us(&hist, 99.99) == 10000000);
}

#define MAX_EV 256
static bench_io_event_t storage[MAX_EV];

static void test_io_log_capacity(void)
{
    bench_io_log_t log;
    bench_io_event_t small[2];
    bench_io_log_init(&log, small, 2);
    CHECK(bench_io_log_add(&log, 0, 1, 10));
    CHECK(bench_io_log_add(&log, 1, 2, 10));
    CHECK(!bench_io_log_add(&log, 2, 3, 10));
    CHECK(log.n == 2);
    CHECK(log.dropped == 1);
}

/* Back-to-back 1 MB writes, 0.1 s each, for `count` writes. */
static uint32_t fill_steady(bench_io_log_t *log, uint32_t count, uint64_t gap_after, uint64_t gap_ticks)
{
    uint64_t t = 1000;
    for (uint32_t i = 0; i < count; i++) {
        bench_io_log_add(log, t, t + SEC / 10, (uint32_t)MB);
        t += SEC / 10;
        if (i + 1 == gap_after) {
            t += gap_ticks;
        }
    }
    return log->n;
}

static void test_window_steady(void)
{
    bench_io_log_t log;
    bench_io_log_init(&log, storage, MAX_EV);
    fill_steady(&log, 50, 0, 0); /* 5 s at 10 MB/s */
    CHECK_NEAR(bench_io_min_window_rate(log.ev, log.n, SEC) / MB, 10.0, 0.001);
}

static void test_window_short_run(void)
{
    bench_io_log_t log;
    bench_io_log_init(&log, storage, MAX_EV);
    fill_steady(&log, 5, 0, 0); /* 0.5 s < 1 s window */
    CHECK(bench_io_min_window_rate(log.ev, log.n, SEC) < 0.0);
    CHECK(bench_io_min_window_rate(log.ev, 0, SEC) < 0.0);
}

static void test_window_stall_longer_than_window(void)
{
    bench_io_log_t log;
    bench_io_log_init(&log, storage, MAX_EV);
    fill_steady(&log, 50, 20, SEC * 3 / 2); /* 1.5 s stall after 2 s */
    CHECK_NEAR(bench_io_min_window_rate(log.ev, log.n, SEC), 0.0, 0.0);
}

static void test_window_short_stall(void)
{
    bench_io_log_t log;
    bench_io_log_init(&log, storage, MAX_EV);
    fill_steady(&log, 50, 20, SEC / 2); /* 0.5 s stall: worst window = 0.5 s of data */
    CHECK_NEAR(bench_io_min_window_rate(log.ev, log.n, SEC) / MB, 5.0, 0.001);
}

static void test_window_slow_write(void)
{
    bench_io_log_t log;
    bench_io_log_init(&log, storage, MAX_EV);
    uint64_t t = 0;
    for (int i = 0; i < 30; i++) {
        /* One write takes 1 s (1 MB/s), the rest 0.1 s (10 MB/s). */
        uint64_t dur = (i == 15) ? SEC : SEC / 10;
        bench_io_log_add(&log, t, t + dur, (uint32_t)MB);
        t += dur;
    }
    CHECK_NEAR(bench_io_min_window_rate(log.ev, log.n, SEC) / MB, 1.0, 0.001);
}

int main(void)
{
    test_hist_empty();
    test_hist_fine_range();
    test_hist_tail();
    test_io_log_capacity();
    test_window_steady();
    test_window_short_run();
    test_window_stall_longer_than_window();
    test_window_short_stall();
    test_window_slow_write();

    uint64_t t0 = bench_ticks();
    uint64_t t1 = bench_ticks();
    CHECK(t1 >= t0);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("libbench host tests: all passed\n");
    return EXIT_SUCCESS;
}
