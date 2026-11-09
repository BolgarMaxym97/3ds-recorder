#ifndef R3D_AUDIO_PLACE_H
#define R3D_AUDIO_PLACE_H
/*
 * Places R3DA audio frames (already in playback order) on a continuous sample
 * timeline, and fills frames the recorder lost.
 *
 * The recorder stores a DSP frame when it sees it change, so frames of one
 * stretch of sound are contiguous audio while their timestamps run a few ms
 * late (never early); silence (and a paused DSP) is not stored at all. A
 * "run" is a stretch of frames with no timestamp gap over AUDIO_BREAK_US
 * between neighbours.
 *   - inside a run frames are appended back to back (timestamps are not used
 *     frame by frame: the first version did, and put hundreds of short
 *     silences and cuts into continuous sound, O3DS test 2026-11-03);
 *   - a run starts at its timestamp: silence fills up to it, or, when the audio
 *     so far is already later than that, frames are dropped until it is back
 *     within AUDIO_AHEAD_FRAMES (cuts land right after a pause);
 *   - inside a run a lost frame shows up as the audio falling behind the
 *     timestamps and, usually, as a click at that join. Whole frames are
 *     filled in (audio_conceal) at a join that is a click while the audio is
 *     more than AUDIO_BEHIND_HARSH_PCT % of a frame behind, and at any join
 *     once it is more than AUDIO_BEHIND_FRAMES behind. On O3DS (v7, 115 s)
 *     this put 31 frames back, 27 of them at a click, and kept the audio
 *     within -10..+11 ms of the timestamps (the old 30-frame tolerance let it
 *     drift 90 ms early; behind-only filling hit a click half the time);
 *   - audio more than AUDIO_AHEAD_FRAMES later than the timestamps drops the
 *     frame (did not happen on v7).
 */
#include <stdint.h>

#define AUDIO_BREAK_US      30000u   /* a gap this long between frames is a pause */
#define AUDIO_BEHIND_FRAMES 2u       /* ~10 ms: timestamps jitter less than that */
#define AUDIO_BEHIND_HARSH_PCT 70u   /* at a click, a smaller lag is enough */
#define AUDIO_AHEAD_FRAMES  8u       /* ~39 ms */
#define AUDIO_CONCEAL_MAX   8u       /* frames filled in at one join; more is left as it is */

typedef struct {
    uint64_t out;          /* samples placed so far */
    uint32_t rateMilliHz;  /* sample rate in mHz */
    uint32_t lastUs;       /* timestamp of the previous frame */
    int      started;
    int      catchingUp;   /* dropping frames at a run start */
    uint32_t conceal;      /* out: frames to fill in before this frame (inside a run) */
    int64_t  minLagSamples, maxLagSamples;   /* audio start minus timestamp, over placed frames */
} AudioPlacer;

static inline void audio_placer_init(AudioPlacer *p, uint32_t rateMilliHz)
{
    const AudioPlacer z = { 0, rateMilliHz, 0, 0, 0, 0, INT64_MAX, INT64_MIN };
    *p = z;
}

/* Samples of silence to write before this frame, or -1 to drop the frame.
   p->conceal is set to the frames to fill in before it (audio_conceal).
   harshJoin: the frame does not continue the previous one (audio_join_harsh). */
static inline int64_t audio_place(AudioPlacer *p, uint32_t timeUs, uint32_t frameSamples, int harshJoin)
{
    const uint64_t due = (uint64_t)timeUs * p->rateMilliHz / 1000000000ull;
    const uint64_t ahead = (uint64_t)frameSamples * AUDIO_AHEAD_FRAMES;
    const int runStart = !p->started || (int64_t)timeUs - (int64_t)p->lastUs > (int64_t)AUDIO_BREAK_US;
    p->started = 1;
    p->lastUs = timeUs;
    p->conceal = 0;
    int64_t silence = 0;
    if (runStart || p->catchingUp)
    {
        if (due > p->out)
            silence = (int64_t)(due - p->out);
        else if (p->out > due + ahead)
        {
            p->catchingUp = 1;
            return -1;
        }
        p->catchingUp = 0;
    }
    else if (p->out > due + ahead)
        return -1;
    else
    {
        const uint64_t n = frameSamples;
        while (p->conceal < AUDIO_CONCEAL_MAX &&
               (due > p->out + n * (AUDIO_BEHIND_FRAMES + p->conceal) ||
                (harshJoin && due * 100 > (p->out + n * p->conceal) * 100 + n * AUDIO_BEHIND_HARSH_PCT)))
            p->conceal++;
    }
    p->out += (uint64_t)silence + (uint64_t)frameSamples * p->conceal;
    const int64_t lag = (int64_t)p->out - (int64_t)due;
    if (lag < p->minLagSamples)
        p->minLagSamples = lag;
    if (lag > p->maxLagSamples)
        p->maxLagSamples = lag;
    p->out += frameSamples;
    return silence;
}

/* Samples of silence that bring the audio up to `timeUs` (end of the video). */
static inline uint64_t audio_pad_to(const AudioPlacer *p, uint32_t timeUs)
{
    const uint64_t due = (uint64_t)timeUs * p->rateMilliHz / 1000000000ull;
    return due > p->out ? due - p->out : 0;
}

static inline uint32_t audio_iabs(int32_t v) { return (uint32_t)(v < 0 ? -v : v); }

/* frame: `n` stereo s16 samples, L R L R ... Per channel: the candidate's first
   sample against the previous frame's slope (2 p[-1] - p[-2]) and the previous
   frame's last sample against the candidate's slope (2 c[0] - c[1]). */
static inline double audio_join_cost(const int16_t *prev, const int16_t *next, uint32_t n)
{
    uint64_t inner = 0;
    for (uint32_t k = 1; k < n; k++)
        inner += audio_iabs(next[2 * k] - next[2 * k - 2]) + audio_iabs(next[2 * k + 1] - next[2 * k - 1]);
    const double avg = (double)inner / (2.0 * (n - 1)) + 1.0;
    uint32_t err = 0;
    for (uint32_t ch = 0; ch < 2; ch++)
    {
        const int32_t p1 = prev[2 * (n - 1) + ch], p2 = prev[2 * (n - 2) + ch];
        const int32_t c0 = next[ch], c1 = next[2 + ch];
        err += audio_iabs(c0 - (2 * p1 - p2)) + audio_iabs(p1 - (2 * c0 - c1));
    }
    return err / 4.0 / avg;
}

/* the join sounds like a click: its slopes miss by over 4x the average sample step */
static inline int audio_join_harsh(const int16_t *prev, const int16_t *next, uint32_t n)
{
    return audio_join_cost(prev, next, n) > 4.0;
}

/* index into a frame of n samples played back and forth: n-1, n-2 .. 0, 0, 1 .. */
static inline uint32_t audio_pingpong(uint32_t i, uint32_t n)
{
    const uint32_t q = i % (2 * n);
    return q < n ? n - 1 - q : q - n;
}

/*
 * Fills `frames` lost frames between `prev` and `next` (n stereo s16 samples
 * each) into `out` (frames * n stereo samples): prev played backwards from its
 * end, cross-faded (raised cosine) into next played backwards towards its
 * start. Both ends meet their neighbour at the same sample, so the join has no
 * step (a plain cut there is the click of a lost frame).
 */
static inline void audio_conceal(const int16_t *prev, const int16_t *next, uint32_t n, uint32_t frames,
                                 int16_t *out)
{
    const uint32_t len = n * frames;
    for (uint32_t j = 0; j < len; j++)
    {
        const double w = 0.5 - 0.5 * __builtin_cos(3.14159265358979323846 * (j + 0.5) / len);
        const uint32_t a = audio_pingpong(j, n), b = n - 1 - audio_pingpong(len - 1 - j, n);
        for (uint32_t ch = 0; ch < 2; ch++)
        {
            const double v = (1.0 - w) * prev[2 * a + ch] + w * next[2 * b + ch];
            out[2 * j + ch] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
        }
    }
}

/*
 * Soft edges around silence (a pause, or sound the recorder lost while a game kept the system
 * core for up to 0.2 s: Mario Kart 7's race start, O3DS 2026-11-08). The sound used to stop on a
 * sample and start on one, which sounds like a stutter. Before the silence, the previous frame is
 * played backwards from its end (no step at the join) and fades out over up to one frame
 * (audio_tail fills the start of the silence with it); the frame after the silence fades in over
 * its first AUDIO_EDGE_FADE samples (audio_fade_in). The timeline does not change.
 */
#define AUDIO_EDGE_FADE 80u   /* ~2.4 ms */

/* Writes min(silence, n) samples of `prev` fading out into `out`; returns how many. */
static inline uint32_t audio_tail(const int16_t *prev, uint32_t n, uint64_t silence, int16_t *out)
{
    const uint32_t len = silence < n ? (uint32_t)silence : n;
    for (uint32_t j = 0; j < len; j++)
    {
        const double w = 0.5 + 0.5 * __builtin_cos(3.14159265358979323846 * (j + 0.5) / len);
        const uint32_t a = audio_pingpong(j, n);
        for (uint32_t ch = 0; ch < 2; ch++)
        {
            const double v = w * prev[2 * a + ch];
            out[2 * j + ch] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
        }
    }
    return len;
}

/* Fades `frame` (n stereo samples) in over its first AUDIO_EDGE_FADE samples, in place. */
static inline void audio_fade_in(int16_t *frame, uint32_t n)
{
    const uint32_t len = n < AUDIO_EDGE_FADE ? n : AUDIO_EDGE_FADE;
    for (uint32_t j = 0; j < len; j++)
    {
        const double w = 0.5 - 0.5 * __builtin_cos(3.14159265358979323846 * (j + 0.5) / len);
        for (uint32_t ch = 0; ch < 2; ch++)
        {
            const double v = w * frame[2 * j + ch];
            frame[2 * j + ch] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
        }
    }
}

#endif
