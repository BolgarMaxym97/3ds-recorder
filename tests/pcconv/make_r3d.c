/*
 * Writes a synthetic .r3d for tests/pcconv/run.sh: both screens, 4 s at 30 fps, every frame kind
 * the recorder writes (R3DF, R3DY key/delta, R3DQ, R3DC), stereo audio, and an R3DE end record.
 * Pictures carry markers whose position and colour run.sh checks after conversion:
 *   top:    red 8x8 at screen top-left, blue 8x8 at bottom-right, a white square moving right
 *   bottom: green 8x8 at screen top-left
 * Usage: make_r3d out.r3d
 */
#include "codec.h"
#include "r3d_format.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 240

static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

/* screen (x, y), y from the top -> column-major, word 0 = bottom row */
static void put(uint16_t *fb, int x, int y, uint16_t c) { fb[x * ROWS + (ROWS - 1 - y)] = c; }

static void draw(uint16_t *fb, int cols, int t, int isTop)
{
    for (int x = 0; x < cols; x++)
        for (int y = 0; y < ROWS; y++)
            put(fb, x, y, rgb565(x * 255 / cols, y * 255 / ROWS, 96));
    if (isTop) {
        int sx = (t * 4) % (cols - 40);
        for (int x = sx; x < sx + 40; x++)
            for (int y = 100; y < 140; y++)
                put(fb, x, y, rgb565(255, 255, 255));
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                put(fb, x, y, rgb565(255, 0, 0));
                put(fb, cols - 1 - x, ROWS - 1 - y, rgb565(0, 0, 255));
            }
    } else {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++)
                put(fb, x, y, rgb565(0, 255, 0));
    }
}

/* aligned_alloc wants a size that is a multiple of the alignment (macOS returns NULL otherwise) */
static void *alloc16(size_t n)
{
    void *p = aligned_alloc(16, (n + 15) & ~(size_t)15);
    if (!p)
        exit(1);
    return p;
}

static void rec(FILE *f, uint32_t magic, const void *payload, uint32_t len, uint32_t timeUs, uint8_t screen, uint16_t cols)
{
    R3dRecHeader h = { magic, len, timeUs, screen, 0x02, cols };
    fwrite(&h, 1, sizeof(h), f);
    fwrite(payload, 1, len, f);
    static const uint8_t zero[16];
    fwrite(zero, 1, (16 - (len & 15)) & 15, f);   /* records are 16-byte aligned */
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    FILE *f = fopen(argv[1], "wb");
    if (!f)
        return 1;
    R3dFileHeader fh = { R3D_MAGIC_FILE, R3D_VERSION, 0, 0, R3D_MODE_FULL_BOTH, 30, R3D_FILE_INPUT, R3D_AUDIO_RATE_MHZ, 0 };
    fwrite(&fh, 1, sizeof(fh), f);

    uint16_t *top = malloc(400 * ROWS * 2), *bot = malloc(320 * ROWS * 2);
    uint16_t *prev = alloc16(400 * ROWS * 2);
    uint8_t *out = alloc16(codec_xr32_max_size(400 * ROWS) + 64);
    const uint32_t frames = 120, us = 1000000 / 30;
    uint32_t audioSample = 0;
    for (uint32_t i = 0; i < frames; i++) {
        const uint32_t t = i * us + 500;
        draw(top, 400, (int)i, 1);
        size_t n;
        if (i % 40 == 20) {          /* a picture on its own: Q565 */
            n = codec_q565_encode((const uint8_t *)top, 400, ROWS, ROWS * 2, out, codec_xr32_max_size(400 * ROWS));
            rec(f, R3D_MAGIC_QFRAME, out, (uint32_t)n, t, 0 | R3D_SCREEN_KEY, 400);
            memcpy(prev, top, 400 * ROWS * 2);
        } else if (i % 40 == 30) {   /* lossy C420 */
            n = codec_c420_encode((const uint8_t *)top, 400, ROWS, ROWS * 2, out);
            rec(f, R3D_MAGIC_CFRAME, out, (uint32_t)n, t, 0 | R3D_SCREEN_KEY, 400);
            codec_c420_decode(out, n, prev, 400, ROWS);   /* the decoder's picture is the next reference */
        } else {                     /* XR32 against the last picture, a keyframe every 60 */
            const int key = i % 60 == 0;
            n = codec_xr32_encode_strided(prev, (const uint8_t *)top, 400, ROWS, ROWS * 2, 1, key, out);
            rec(f, R3D_MAGIC_YFRAME, out, (uint32_t)n, t, (uint8_t)(0 | (key ? R3D_SCREEN_KEY : 0)), 400);
        }
        if (i % 15 == 0) {           /* the buttons: a different one every half second, a touch in odd seconds */
            R3dInputSample in[2] = { { 0, (uint16_t)(1u << ((i / 15) % 12)), (int8_t)((i * 4) % 200 - 100), 50,
                                  (uint8_t)((i / 30) % 2 ? 80 : R3D_TOUCH_NONE), (uint8_t)((i / 30) % 2 ? 120 : R3D_TOUCH_NONE) } };
            rec(f, R3D_MAGIC_INPUT, in, sizeof(in), t, R3D_STREAM_INPUT, 1);   /* len: a multiple of 16 */
        }
        if (i % 30 == 10) {          /* a record of a type the readers do not know: stepped over */
            static const uint8_t future[32] = { 1, 2, 3 };
            rec(f, 0x5A443352u /* "R3DZ" */, future, sizeof(future), t, 9, 0);
        }
        if (i % 30 == 0) {           /* bottom screen: raw, once a second */
            draw(bot, 320, (int)i, 0);
            rec(f, R3D_MAGIC_FRAME, bot, 320 * ROWS * 2, t + 100, 1, 320);
        }
        /* audio: 440 Hz, DSP frames of 160 stereo samples, placed by time */
        while ((uint64_t)audioSample * 1000000000ull / R3D_AUDIO_RATE_MHZ < (uint64_t)t + us) {
            int16_t pcm[320];
            for (int k = 0; k < 160; k++) {
                int16_t v = (int16_t)(8000 * sin(2 * M_PI * 440 * (audioSample + k) * 1000.0 / R3D_AUDIO_RATE_MHZ));
                pcm[2 * k] = pcm[2 * k + 1] = v;
            }
            rec(f, R3D_MAGIC_AUDIO, pcm, sizeof(pcm),
                (uint32_t)((uint64_t)audioSample * 1000000000ull / R3D_AUDIO_RATE_MHZ), R3D_STREAM_AUDIO, 160);
            audioSample += 160;
        }
    }
    R3dEndInfo end = { frames, frames / 30, 0, 0 };
    rec(f, R3D_MAGIC_END, &end, sizeof(end), frames * us, 0, 0);
    fclose(f);
    return 0;
}
