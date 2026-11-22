/*
 * Settings → App → Test SD Card: writes SDTEST_MB to the card the way the recorder does (512 KB
 * blocks into a file made at its size, each block flushed), times them, deletes the file. A thread
 * of its own; the app shows the progress and the result.
 */
#ifndef SDTEST_H
#define SDTEST_H

#include <3ds.h>
#include <stdbool.h>

#define SDTEST_MB 96u

typedef enum { SDT_IDLE, SDT_RUNNING, SDT_DONE, SDT_FAILED } SdTestState;

typedef struct {
    SdTestState state;
    int permille;
    u32 kbPerSec;     /* the whole write */
    u32 slowestMs;    /* the slowest block */
    Result rc;        /* SDT_FAILED */
} SdTestResult;

/* Starts the test (false: one is running, or no thread). */
bool sdtest_start(void);
/* The state now; a finished test's thread is joined here. */
SdTestResult sdtest_poll(void);

/* What the speed is enough for: 2 both screens at 30 fps in 3D, 1 with the bottom one at a lower
   rate, 0 slow. (An Old 3DS records a 3D game at ~5.4 MB/s with both screens, statistics 2026-11-09.) */
static inline int sdtest_grade(u32 kbPerSec) { return kbPerSec >= 6144u ? 2 : kbPerSec >= 4096u ? 1 : 0; }

#endif
