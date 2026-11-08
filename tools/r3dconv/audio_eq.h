#ifndef R3D_AUDIO_EQ_H
#define R3D_AUDIO_EQ_H
/*
 * Treble cut for the recorded audio (r3dconv, on by default; --no-eq).
 *
 * The recorder takes the DSP's final mix, before the console's speaker /
 * headphone path. Played back on a PC that mix sounds harsher than on the
 * console: HOME menu clicks (5 ms bursts at ~5-8 kHz) stood out although
 * they are ~10 dB below game audio in the file (O3DS v7, 2026-11-03). A
 * -8 dB high shelf at 4 kHz was the version the user picked (same as
 * ffmpeg `treble=g=-8:f=4000`, RBJ cookbook shelf, slope 1;
 * matches it to the sample, test 2026-11-03).
 */
#include <math.h>
#include <stdint.h>

#define AUDIO_EQ_FREQ_HZ 4000.0
#define AUDIO_EQ_GAIN_DB (-8.0)
#define AUDIO_EQ_Q       0.70710678   /* shelf slope 1: ffmpeg treble default */

typedef struct {
    double b0, b1, b2, a1, a2;   /* normalised by a0 */
    double z1[2], z2[2];         /* transposed direct form II state, per channel */
} AudioEq;

static inline void audio_eq_init(AudioEq *e, double rateHz, double freqHz, double gainDb, double q)
{
    const double A = pow(10.0, gainDb / 40.0), w0 = 2.0 * M_PI * freqHz / rateHz;
    const double c = cos(w0), alpha = sin(w0) / (2.0 * q), sa = 2.0 * sqrt(A) * alpha;
    const double a0 = (A + 1) - (A - 1) * c + sa;
    e->b0 = A * ((A + 1) + (A - 1) * c + sa) / a0;
    e->b1 = -2.0 * A * ((A - 1) + (A + 1) * c) / a0;
    e->b2 = A * ((A + 1) + (A - 1) * c - sa) / a0;
    e->a1 = 2.0 * ((A - 1) - (A + 1) * c) / a0;
    e->a2 = ((A + 1) - (A - 1) * c - sa) / a0;
    e->z1[0] = e->z1[1] = e->z2[0] = e->z2[1] = 0.0;
}

/* Filters `n` stereo s16 samples in place. */
static inline void audio_eq_run(AudioEq *e, int16_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t ch = 0; ch < 2; ch++)
        {
            const double x = s[2 * i + ch];
            double y = e->b0 * x + e->z1[ch];
            e->z1[ch] = e->b1 * x - e->a1 * y + e->z2[ch];
            e->z2[ch] = e->b2 * x - e->a2 * y;
            y = y > 32767.0 ? 32767.0 : y < -32768.0 ? -32768.0 : y;
            s[2 * i + ch] = (int16_t)lrint(y);
        }
}

#endif
