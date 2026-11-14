#ifndef UNWRAP_H
#define UNWRAP_H
/*
 * A replay clip (.rpl: the recorder's stream in a ring file, r3d_format.h R3dRingInfo) made into
 * an ordinary .r3d that every reader takes. Portable C99: the app runs it before a clip is played
 * or prepared for the phone, tests/host/test_unwrap.c runs it on a PC.
 *
 * The clip starts where every recorded screen has a key frame: time 0 is the later of the screens'
 * first key frames; the other screen's frames from its last key frame before that are kept at time
 * 0 (its picture then), what comes earlier is left out. Times are counted from there (the
 * recorder's 32-bit timeUs may wrap inside a clip). Padding and diagnostics records are left out;
 * an end record (R3DE) is added.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    UNWRAP_OK = 0,
    UNWRAP_STOPPED = 1,   /* the progress callback asked to stop: dst is removed */
    UNWRAP_BAD = -1,      /* not a clip, or no frame to keep in it */
    UNWRAP_IO = -2,       /* reading src or writing dst failed (dst is removed) */
};

/* done / total: bytes of the clip handled so far; returns nonzero to stop. May be NULL. */
typedef int (*UnwrapProgress)(void *ctx, uint64_t done, uint64_t total);

int r3d_unwrap(const char *src, const char *dst, UnwrapProgress progress, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
