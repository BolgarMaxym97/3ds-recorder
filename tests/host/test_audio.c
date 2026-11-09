/* Host tests for the audio timeline and frame order (tools/r3dconv/audio_place.h, audio_order.h). */
#include "../../tools/r3dconv/audio_eq.h"
#include "../../tools/r3dconv/audio_order.h"
#include "../../tools/r3dconv/audio_place.h"
#include "r3d_format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                  \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int16_t *make_frames(uint32_t count);

/* microseconds at which sample `n` is due */
static uint32_t us_of(uint64_t n) { return (uint32_t)(n * 1000000000ull / R3D_AUDIO_RATE_MHZ); }

static void test_contiguous_with_jitter(void)
{
    AudioPlacer p;
    audio_placer_init(&p, R3D_AUDIO_RATE_MHZ);
    /* the first frame starts at its timestamp (2 ms) */
    CHECK(audio_place(&p, 2000, 160, 0) == 65);
    for (uint32_t i = 1; i < 1000; i++)
    {
        /* seen 0..2 ms after the frame is due */
        const uint32_t jitter = (i * 7u) % 2000u;
        CHECK(audio_place(&p, us_of((uint64_t)i * 160) + jitter, 160, 0) == 0);
    }
    CHECK(p.out == 160000 + 65);
}

static void test_gap_is_silence(void)
{
    AudioPlacer p;
    audio_placer_init(&p, R3D_AUDIO_RATE_MHZ);
    for (uint32_t i = 0; i < 10; i++)
        CHECK(audio_place(&p, us_of((uint64_t)i * 160) + 1000, 160, 0) == (i == 0 ? 32 : 0));
    /* nothing for 1 s (silence is not stored), then audio again */
    const uint64_t resume = 1600 + 32728;
    const int64_t silence = audio_place(&p, us_of(resume), 160, 0);
    CHECK(silence > 0);
    CHECK(p.out - 160 == resume || p.out - 160 + 1 == resume);   /* frame starts at its timestamp */
}

static void test_first_frame_late_start(void)
{
    AudioPlacer p;
    audio_placer_init(&p, R3D_AUDIO_RATE_MHZ);
    /* audio starts 3 s into the recording: 3 s of silence first */
    const int64_t silence = audio_place(&p, 3000000, 160, 0);
    CHECK(silence >= 98184 && silence <= 98186);
}

static void test_ahead_is_dropped(void)
{
    AudioPlacer p;
    audio_placer_init(&p, R3D_AUDIO_RATE_MHZ);
    /* 40 frames all stamped at t = 0 (one run): appended until 8 frames ahead, then dropped */
    int dropped = 0;
    for (int i = 0; i < 40; i++)
        dropped += audio_place(&p, 0, 160, 0) < 0;
    CHECK(dropped == 31);
    CHECK(p.out == 9 * 160);
}

static void test_run_is_contiguous(void)
{
    AudioPlacer p;
    audio_placer_init(&p, R3D_AUDIO_RATE_MHZ);
    /* ~2.3 % fewer frames than time inside one run (timestamps 5 ms apart, frames 4.89 ms):
       no silence inside the run; once 2 frames behind, single frames are filled in */
    uint32_t filled = 0;
    for (uint32_t i = 0; i < 2000; i++)
    {
        const int64_t s = audio_place(&p, i * 5000u, 160, 0);
        CHECK(s == 0);
        CHECK(p.conceal <= 1);
        filled += p.conceal;
    }
    /* 2000 * 5 ms = 10 s = 2045.4 frames: 45 short, less the 2-frame tolerance */
    CHECK(filled >= 42 && filled <= 44);
    CHECK(p.minLagSamples >= -3 * 160 && p.maxLagSamples <= 0);
    /* a pause (> 30 ms) re-aligns to the timestamp */
    const int64_t s = audio_place(&p, 12000000u, 160, 0);
    CHECK(s > 0);
    CHECK(p.out - 160 == 12000000ull * R3D_AUDIO_RATE_MHZ / 1000000000ull);
}

static void test_lost_frame_filled_at_click(void)
{
    AudioPlacer p;
    audio_placer_init(&p, R3D_AUDIO_RATE_MHZ);
    /* frames seen 1.5 ms late; frame 100 lost: frame 101 (a click) is 1 frame behind */
    for (uint32_t i = 0; i < 300; i++)
    {
        if (i == 100)
            continue;
        const int harsh = i == 101;
        audio_place(&p, us_of((uint64_t)i * 160) + 1500, 160, harsh);
        CHECK(p.conceal == (i == 101 ? 1u : 0u));
    }
    /* the same lag at a smooth join is left alone (timestamp jitter) */
    audio_placer_init(&p, R3D_AUDIO_RATE_MHZ);
    for (uint32_t i = 0; i < 300; i++)
    {
        if (i == 100)
            continue;
        audio_place(&p, us_of((uint64_t)i * 160) + 1500, 160, 0);
        CHECK(p.conceal == 0);
    }
}

static void test_conceal_joins_smoothly(void)
{
    int16_t *f = make_frames(3);
    int16_t fill[2 * 320];
    for (uint32_t frames = 1; frames <= 2; frames++)
    {
        /* frame 1 lost (or two frames): the fill meets frame 0's end and frame 2's start */
        audio_conceal(f, f + 2 * 320, 160, frames, fill);
        const uint32_t last = frames * 160 - 1;
        for (uint32_t ch = 0; ch < 2; ch++)
        {
            CHECK(abs(fill[ch] - f[318 + ch]) <= 1);
            CHECK(abs(fill[2 * last + ch] - f[2 * 320 + ch]) <= 1);
        }
        CHECK(!audio_join_harsh(f, fill, 160));
    }
    free(f);
}

/* gain of the treble cut for a sine at `hz` (after the filter settles) */
static double eq_gain(double hz)
{
    AudioEq e;
    audio_eq_init(&e, 32728.498, AUDIO_EQ_FREQ_HZ, AUDIO_EQ_GAIN_DB, AUDIO_EQ_Q);
    static int16_t s[2 * 16384];
    for (uint32_t i = 0; i < 16384; i++)
        s[2 * i] = s[2 * i + 1] = (int16_t)(10000 * sin(2 * M_PI * hz * i / 32728.498));
    audio_eq_run(&e, s, 16384);
    double peak = 0;
    for (uint32_t i = 8192; i < 16384; i++)
        peak = fmax(peak, fabs((double)s[2 * i]));
    return 20 * log10(peak / 10000);
}

static void test_eq_cuts_treble_only(void)
{
    CHECK(fabs(eq_gain(100)) < 0.3);        /* bass and voice untouched */
    CHECK(eq_gain(1000) > -1.0);
    CHECK(eq_gain(4000) < -2.5 && eq_gain(4000) > -5.5);   /* half the cut at the shelf */
    CHECK(eq_gain(8000) < -6.5);            /* HOME clicks: ~5-8 kHz */
    /* loud input never wraps */
    AudioEq e;
    audio_eq_init(&e, 32728.498, AUDIO_EQ_FREQ_HZ, AUDIO_EQ_GAIN_DB, AUDIO_EQ_Q);
    int16_t s[2 * 64];
    for (uint32_t i = 0; i < 64; i++)
        s[2 * i] = s[2 * i + 1] = i < 32 ? 32767 : -32768;
    audio_eq_run(&e, s, 64);
    for (uint32_t i = 0; i < 31; i++)
        CHECK(s[2 * i] > 0);
}

static void test_pad_to_end(void)
{
    AudioPlacer p;
    audio_placer_init(&p, R3D_AUDIO_RATE_MHZ);
    audio_place(&p, 0, 160, 0);
    CHECK(audio_pad_to(&p, 1000000) == 32728 - 160);
    CHECK(audio_pad_to(&p, 1000) == 0);
}

/* a two-tone signal cut into 160-sample frames */
static int16_t *make_frames(uint32_t count)
{
    int16_t *f = malloc((size_t)count * 320 * sizeof(int16_t));
    for (uint32_t i = 0; i < count * 160; i++)
    {
        const double t = i / 32728.498;
        f[2 * i] = (int16_t)(6000 * sin(2 * M_PI * 440 * t) + 2000 * sin(2 * M_PI * 1250 * t));
        f[2 * i + 1] = (int16_t)(5000 * sin(2 * M_PI * 330 * t + 1.0));
    }
    return f;
}

static void swap_frames(int16_t *f, uint32_t a, uint32_t b)
{
    int16_t tmp[320];
    memcpy(tmp, f + a * 320, sizeof(tmp));
    memcpy(f + a * 320, f + b * 320, sizeof(tmp));
    memcpy(f + b * 320, tmp, sizeof(tmp));
}

static void test_order_restores_local_swaps(void)
{
    const uint32_t n = 400;
    int16_t *f = make_frames(n);
    /* recorded order: every 7th pair swapped, some frames 2-3 places late */
    uint32_t truth[400];
    for (uint32_t i = 0; i < n; i++)
        truth[i] = i;
    for (uint32_t i = 5; i + 1 < n; i += 7)
    {
        swap_frames(f, i, i + 1);
        const uint32_t t = truth[i];
        truth[i] = truth[i + 1];
        truth[i + 1] = t;
    }
    for (uint32_t i = 40; i + 3 < n; i += 50)   /* frame i played after i+1, i+2 */
    {
        swap_frames(f, i, i + 1);
        swap_frames(f, i + 1, i + 2);
        const uint32_t t = truth[i];
        truth[i] = truth[i + 1];
        truth[i + 1] = truth[i + 2];
        truth[i + 2] = t;
    }
    uint32_t order[400];
    const uint32_t placed = audio_order(f, n, 160, order);
    CHECK(placed == n);
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < placed; i++)
        if (truth[order[i]] != i && wrong++ < 5) fprintf(stderr, "  at %u: frame %u\n", i, truth[order[i]]);
    CHECK(wrong == 0);
    free(f);
}

static void test_order_skips_duplicates_keeps_silence(void)
{
    int16_t *f = make_frames(6);
    memcpy(f + 3 * 320, f + 2 * 320, 640);   /* frame 3 = frame 2 again */
    memset(f + 4 * 320, 0, 640);             /* two silent frames */
    memset(f + 5 * 320, 0, 640);
    uint32_t order[6];
    CHECK(audio_order(f, 6, 160, order) == 5);
    free(f);
}

static void test_soft_edges_around_silence(void)
{
    int16_t *f = make_frames(2);
    int16_t tail[320];
    /* the tail starts where the previous frame ended (no step) and fades to zero */
    CHECK(audio_tail(f, 160, 5000, tail) == 160);
    for (int ch = 0; ch < 2; ch++)
    {
        CHECK(abs(tail[ch] - f[318 + ch]) <= 1);
        CHECK(abs(tail[318 + ch]) <= 1 + abs(f[ch]) / 1000);
    }
    /* a short silence: the tail fills only it */
    CHECK(audio_tail(f, 160, 37, tail) == 37);
    CHECK(audio_tail(f, 160, 0, tail) == 0);
    /* the next frame starts from zero and is untouched after AUDIO_EDGE_FADE samples */
    int16_t g[320];
    memcpy(g, f + 320, sizeof(g));
    audio_fade_in(g, 160);
    for (int ch = 0; ch < 2; ch++)
        CHECK(abs(g[ch]) <= 1 + abs(f[320 + ch]) / 1000);
    CHECK(memcmp(g + 2 * AUDIO_EDGE_FADE, f + 320 + 2 * AUDIO_EDGE_FADE, (160 - AUDIO_EDGE_FADE) * 4) == 0);
    free(f);
}

int main(void)
{
    test_contiguous_with_jitter();
    test_gap_is_silence();
    test_first_frame_late_start();
    test_ahead_is_dropped();
    test_run_is_contiguous();
    test_lost_frame_filled_at_click();
    test_conceal_joins_smoothly();
    test_soft_edges_around_silence();
    test_eq_cuts_treble_only();
    test_pad_to_end();
    test_order_restores_local_swaps();
    test_order_skips_duplicates_keeps_silence();
    if (failures)
    {
        fprintf(stderr, "audio host tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("audio host tests: all passed\n");
    return 0;
}
