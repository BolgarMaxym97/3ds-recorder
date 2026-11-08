#ifndef R3D_AUDIO_ORDER_H
#define R3D_AUDIO_ORDER_H
/*
 * Restores the playback order of R3DA frames (r3dconv --repair).
 *
 * Only for recordings made before the DSP cache fix (2026-11-03, plan 3.7):
 * the recorder read DSP memory through a stale cache, which produced the
 * unfinished and out-of-order frames handled here. Clean recordings must not
 * go through it (it reordered quiet music wrongly).
 *
 * On O3DS the recorder sees the DSP's two output regions change in an order
 * that does not always match playback (test 2026-11-03: ~10% of frame
 * boundaries were clicks; the right frame was 1-3 frames away). Frames are
 * re-sequenced greedily: from the next `AUDIO_ORDER_WINDOW` frames (in
 * recording order) take the one whose first samples continue the last
 * samples of the frame before it best. Exact duplicates of a frame already
 * placed are skipped (silent frames are kept). Measured on that recording:
 * clicks 14.8% -> 6.6% of boundaries, against 4.9% for sample steps inside
 * frames (the natural level).
 *
 * Before that, partial frames are dropped: in busy scenes (SM3DL) the DSP
 * writes a frame in parts with pauses longer than the recorder's two polls, so
 * it also records an unfinished frame; the finished one follows a few frames
 * later from the same region (test 2026-11-03: ~11% of SM3DL frames, 3% of
 * NSMB2; new and old parts interleave, e.g. new-old-new). A frame made of
 * the next and the previous frame of its region (see audio_mark_partial) is
 * dropped; after that SM3DL gave 204-207 frames/s against the DSP's 204.6.
 *
 * The cost of a candidate is how well the two frames' slopes meet at the
 * join, relative to the candidate's own average sample step, so loud and
 * quiet passages weigh the same.
 */
#include "audio_place.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_ORDER_WINDOW 4u
#define AUDIO_ORDER_DUPS   8u   /* recent placed frames checked for exact duplicates */
#define AUDIO_PARTIAL_AHEAD 4u  /* frames of the same region are searched within 2x this distance */

/* keep[i] = 0 for frames that are an unfinished version of a later frame:
   its stereo samples equal the next frame of the same region (new part) or
   the previous one (old part), both present, except at most 1/8 (a frame can
   be caught in two unfinished states).
   frames: stereo s16, n samples each; region: DSP region of each frame. */
static inline uint32_t audio_mark_partial(const int16_t *frames, const uint8_t *region, uint32_t count, uint32_t n,
                                          uint8_t *keep)
{
    const uint32_t *w = (const uint32_t *)(const void *)frames;   /* one stereo sample per word */
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        keep[i] = 1;
        int64_t prev = -1, next = -1;
        for (int64_t j = (int64_t)i - 1; j >= 0 && j + AUDIO_PARTIAL_AHEAD * 2 >= (int64_t)i && prev < 0; j--)
            if (region[j] == region[i])
                prev = j;
        for (uint32_t j = i + 1; j < count && j <= i + AUDIO_PARTIAL_AHEAD * 2 && next < 0; j++)
            if (region[j] == region[i])
                next = j;
        if (prev < 0 || next < 0)
            continue;
        const uint32_t *x = w + (size_t)i * n, *p = w + (size_t)prev * n, *y = w + (size_t)next * n;
        uint32_t fromNext = 0, fromPrev = 0, other = 0;
        for (uint32_t k = 0; k < n; k++)
        {
            if (x[k] == y[k])
                fromNext++;
            else if (x[k] == p[k])
                fromPrev++;
            else
                other++;
        }
        if (other * 8 <= n && fromPrev && fromNext)   /* up to 1/8 from an intermediate state */
        {
            keep[i] = 0;
            dropped++;
        }
    }
    return dropped;
}

/*
 * frames: `count` frames of `n` stereo samples each, in recording order.
 * order:  out, playback order (indices); returns how many were placed
 *         (count minus the skipped duplicates).
 */
static inline uint32_t audio_order(const int16_t *frames, uint32_t count, uint32_t n, uint32_t *order)
{
    if (!count)
        return 0;
    const size_t fs = (size_t)n * 2;
    uint32_t pool[AUDIO_ORDER_WINDOW], poolLen = 0, next = 0, placed = 0;
    order[placed++] = next++;
    while (next < count && poolLen < AUDIO_ORDER_WINDOW)
        pool[poolLen++] = next++;
    while (poolLen)
    {
        const int16_t *last = frames + fs * order[placed - 1];
        uint32_t best = 0;
        double bestCost = 0;
        for (uint32_t q = 0; q < poolLen; q++)
        {
            const double c = audio_join_cost(last, frames + fs * pool[q], n);
            if (q == 0 || c < bestCost)
            {
                bestCost = c;
                best = q;
            }
        }
        const uint32_t pick = pool[best];
        memmove(&pool[best], &pool[best + 1], (poolLen - best - 1) * sizeof(pool[0]));
        poolLen--;
        if (next < count)
            pool[poolLen++] = next++;
        int dup = 0, silent = 1;
        for (size_t k = 0; k < fs && silent; k++)
            silent = frames[fs * pick + k] == 0;
        for (uint32_t k = placed > AUDIO_ORDER_DUPS ? placed - AUDIO_ORDER_DUPS : 0; k < placed && !dup && !silent; k++)
            dup = memcmp(frames + fs * order[k], frames + fs * pick, fs * sizeof(int16_t)) == 0;
        if (!dup)
            order[placed++] = pick;
    }
    return placed;
}

#endif
