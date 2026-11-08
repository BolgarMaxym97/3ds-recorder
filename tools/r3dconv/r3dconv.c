/*
 * r3dconv — decodes a 3DS Recorder .r3d file into raw RGB24 video on stdout,
 * for ffmpeg (see tools/r3d2mp4.sh).
 *
 * Usage: r3dconv [--wav out.wav] <file.r3d> [fps]   (fps defaults to the file header, else 30)
 *        r3dconv --info [--wav out.wav] <file.r3d>  (summary only; with --wav: audio only)
 * --wav writes the recording's audio (R3DA, version 4) as a 16-bit stereo WAV
 * at the DSP rate, placed by timestamps (audio_place.h), padded with silence to
 * the length of the video. --repair first drops unfinished frames and
 * re-sequences by continuity (audio_order.h): only for recordings made before
 * the DSP cache fix (v5, 2026-11-03); on clean recordings it does harm (it
 * reordered quiet NSMB2 menu music). The WAV gets a treble cut like the
 * console's speaker path (audio_eq.h); --no-eq writes the DSP mix as recorded.
 * Output: width x height RGB24 frames at a constant rate, built from the
 * frame timestamps (a screen keeps its last picture until a new one arrives):
 *   both screens: 400 x 480, top above, bottom centred below
 *   top only:     400 x 240
 * The size line "WxH fps" is printed to stderr before the frames.
 * Reading stops at the first unknown record (an unclosed file ends in stale
 * SD data, see codec/include/r3d_format.h).
 */
#include "audio_eq.h"
#include "audio_order.h"
#include "audio_place.h"
#include "codec.h"
#include "r3d_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t cols, rows, n;
    uint16_t *px;
    int seen, needKey;
    uint32_t frames, corrupt;
} Screen;

static void wav_header(FILE *w, uint32_t rate, uint32_t dataBytes)
{
    uint8_t h[44];
    const uint32_t byteRate = rate * 4u;
    memcpy(h, "RIFF", 4);
    const uint32_t riff = 36u + dataBytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    const uint32_t fmtLen = 16;
    const uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    memcpy(h + 16, &fmtLen, 4);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byteRate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &dataBytes, 4);
    fseek(w, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), w);
}

/* Writes `n` stereo samples, through the treble cut when `eq` is set (filters `pcm` in place). */
static void wav_write(FILE *w, AudioEq *eq, int16_t *pcm, uint32_t n)
{
    if (eq)
        audio_eq_run(eq, pcm, n);
    fwrite(pcm, 4, n, w);
}

static void wav_silence(FILE *w, AudioEq *eq, uint64_t samples)
{
    int16_t buf[2048];
    while (samples)
    {
        const uint32_t n = samples > 1024 ? 1024 : (uint32_t)samples;
        memset(buf, 0, sizeof(buf));
        wav_write(w, eq, buf, n);   /* zeros too: the filter's tail continues into the silence */
        samples -= n;
    }
}

/* Left channel: step at the join / average step inside `b` (as tools analysis, 2026-11-03). */
static int harsh_join(const int16_t *a, const int16_t *b)
{
    uint64_t inner = 0;
    for (int k = 1; k < 160; k++)
        inner += (uint32_t)abs(b[2 * k] - b[2 * k - 2]);
    const double avg = inner / 159.0 + 1.0;
    return abs(b[0] - a[318]) / avg > 3.0;
}

static void put_screen(uint8_t *rgb, uint32_t outW, const Screen *s, uint32_t ox, uint32_t oy)
{
    if (!s->seen) {
        return;
    }
    for (uint32_t y = 0; y < s->rows; y++) {
        uint8_t *row = rgb + ((size_t)(oy + y) * outW + ox) * 3;
        for (uint32_t x = 0; x < s->cols; x++) {
            /* column-major, word 0 = bottom row */
            const uint16_t v = s->px[x * s->rows + (s->rows - 1 - y)];
            row[x * 3 + 0] = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
            row[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
            row[x * 3 + 2] = (uint8_t)((v & 0x1F) * 255 / 31);
        }
    }
}

int main(int argc, char **argv)
{
    int info = 0, argi = 1;
    const char *wavPath = NULL;
    int repair = 0;   /* --repair: drop partial frames and re-sequence (recordings before the cache fix) */
    int useEq = 1;    /* --no-eq: no treble cut */
    for (;;) {
        if (argc > argi && strcmp(argv[argi], "--info") == 0) {
            info = 1;
            argi++;
        } else if (argc > argi && strcmp(argv[argi], "--no-eq") == 0) {
            useEq = 0;
            argi++;
        } else if (argc > argi && strcmp(argv[argi], "--repair") == 0) {
            repair = 1;
            argi++;
        } else if (argc > argi + 1 && strcmp(argv[argi], "--wav") == 0) {
            wavPath = argv[argi + 1];
            argi += 2;
        } else {
            break;
        }
    }
    if (argc <= argi) {
        fprintf(stderr, "usage: %s [--info] [--repair] [--no-eq] [--wav out.wav] <file.r3d> [fps]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[argi], "rb");
    if (!f) {
        perror(argv[argi]);
        return 1;
    }
    R3dFileHeader fh;
    if (fread(&fh, 1, sizeof(fh), f) != sizeof(fh) || fh.magic != R3D_MAGIC_FILE) {
        fprintf(stderr, "not a .r3d file\n");
        return 1;
    }
    uint32_t fps = argc > argi + 1 ? (uint32_t)atoi(argv[argi + 1]) : (fh.version >= 3 && fh.fps ? fh.fps : 30);
    if (fps == 0 || fps > 60) {
        fps = 30;
    }
    const int both = fh.mode != R3D_MODE_FULL_TOP;
    const uint32_t outW = 400, outH = both ? 480 : 240;
    fprintf(stderr, "%ux%u %u\n", outW, outH, fps);

    Screen scr[2] = { { 400, 240, 400 * 240, NULL, 0, 1, 0, 0 }, { 320, 240, 320 * 240, NULL, 0, 1, 0, 0 } };
    for (int i = 0; i < 2; i++) {
        scr[i].px = calloc(scr[i].n, 2);
    }
    uint8_t *rgb = calloc((size_t)outW * outH, 3);
    uint8_t *payload = malloc(codec_xr32_max_size(400 * 240) + 64);
    uint64_t emitted = 0;
    uint32_t lastUs = 0;
    R3dRecHeader rh;

    const uint32_t audioRate = fh.version >= 4 ? fh.audioRate : 0;
    AudioPlacer ap;
    audio_placer_init(&ap, audioRate ? audioRate : R3D_AUDIO_RATE_MHZ);
    uint32_t audioFrames = 0, audioDropped = 0, audioGaps = 0, audioConcealed = 0, dspSnaps = 0, regionSwitches = 0, lastRegion = 0xFF;
    uint64_t audioSilence = 0;
    FILE *wav = NULL;
    int16_t *aPcm = NULL;          /* all audio frames (160 stereo samples each), recording order */
    uint32_t *aTime = NULL, aCount = 0, aCap = 0;
    uint8_t *aRegion = NULL;
    if (wavPath) {
        wav = fopen(wavPath, "wb");
        if (!wav) {
            perror(wavPath);
            return 1;
        }
        wav_header(wav, (ap.rateMilliHz + 500u) / 1000u, 0);
    }
    AudioEq eqState;
    audio_eq_init(&eqState, ap.rateMilliHz / 1000.0, AUDIO_EQ_FREQ_HZ, AUDIO_EQ_GAIN_DB, AUDIO_EQ_Q);
    AudioEq *eq = useEq ? &eqState : NULL;

    while (fread(&rh, 1, sizeof(rh), f) == sizeof(rh)) {
        if (rh.magic == R3D_MAGIC_PAD) {
            fseek(f, rh.len, SEEK_CUR);
            continue;
        }
        if (rh.magic == R3D_MAGIC_END) {   /* the last record of a closed recording */
            R3dEndInfo end;
            if (rh.len == sizeof(end) && fread(&end, 1, sizeof(end), f) == sizeof(end))
                fprintf(stderr, "end: %.1f s, frames %u + %u, dropped %u\n", rh.timeUs / 1e6,
                        (unsigned)end.framesTop, (unsigned)end.framesBottom, (unsigned)end.dropped);
            break;
        }
        if (rh.magic == R3D_MAGIC_DSPMEM) {
            dspSnaps++;
            fseek(f, rh.len, SEEK_CUR);
            continue;
        }
        if (rh.magic == R3D_MAGIC_AUDIO) {
            const uint32_t bytes = (uint32_t)rh.cols * 4u;
            if (rh.len < bytes || rh.len > 4096 || fread(payload, 1, rh.len, f) != rh.len)
                break;   /* samples, then an optional R3dAudioTrailer */
            if (rh.len >= bytes + sizeof(R3dAudioTrailer)) {
                const R3dAudioTrailer *tr = (const R3dAudioTrailer *)(payload + bytes);
                if (tr->flags & R3D_AUDIO_RAW)
                    continue;   /* audio_debug recording: raw reads, see tools/dspraw.py */
            }
            audioFrames++;
            if (lastRegion != 0xFF && rh.srcFormat == lastRegion)
                regionSwitches++;   /* counts frames from the same region twice in a row */
            lastRegion = rh.srcFormat;
            if (rh.cols != 160)
                continue;
            if (aCount == aCap) {
                aCap = aCap ? aCap * 2 : 4096;
                aPcm = realloc(aPcm, (size_t)aCap * 160 * 2 * sizeof(int16_t));
                aTime = realloc(aTime, (size_t)aCap * sizeof(uint32_t));
                aRegion = realloc(aRegion, aCap);
                if (!aPcm || !aTime || !aRegion) {
                    fprintf(stderr, "out of memory\n");
                    return 1;
                }
            }
            memcpy(aPcm + (size_t)aCount * 320, payload, 640);
            aRegion[aCount] = rh.srcFormat;
            aTime[aCount++] = rh.timeUs;
            continue;
        }
        const int delta = rh.magic == R3D_MAGIC_XFRAME || rh.magic == R3D_MAGIC_YFRAME;
        const uint32_t sc = rh.screen & 0x3F;
        if ((rh.magic != R3D_MAGIC_FRAME && !delta) || sc > 1 || rh.len > codec_xr32_max_size(400 * 240) + 16) {
            break;   /* end of valid data */
        }
        const uint32_t stored = delta ? ((rh.len + 15) & ~15u) : rh.len;
        if (fread(payload, 1, stored, f) != stored) {
            break;
        }
        Screen *s = &scr[sc];
        if (rh.cols != s->cols) {
            continue;   /* half-res bench recordings are not supported here */
        }

        /* emit output frames up to this record's time with the pictures so far */
        if (!info) {
            while ((uint64_t)emitted * 1000000u / fps < rh.timeUs) {
                memset(rgb, 0, (size_t)outW * outH * 3);
                put_screen(rgb, outW, &scr[0], 0, 0);
                if (both) {
                    put_screen(rgb, outW, &scr[1], 40, 240);
                }
                fwrite(rgb, 1, (size_t)outW * outH * 3, stdout);
                emitted++;
            }
        }
        lastUs = rh.timeUs;

        const int key = (rh.screen & R3D_SCREEN_KEY) != 0;
        int bad = 0;
        if (rh.magic == R3D_MAGIC_FRAME) {
            memcpy(s->px, payload, s->n * 2);
        } else if (!key && s->needKey) {
            continue;   /* waiting for a keyframe after a damaged frame */
        } else if (rh.magic == R3D_MAGIC_YFRAME) {
            bad = codec_xr32_decode(payload, rh.len, s->px, s->n, key);
        } else {
            bad = codec_xrle_decode(payload, rh.len, s->px, s->n, key);
        }
        if (bad) {
            s->corrupt++;
            s->needKey = 1;
            continue;
        }
        s->needKey = 0;
        s->seen = 1;
        s->frames++;
    }
    fclose(f);

    /* audio: drop unfinished frames, playback order, then the timeline */
    uint32_t partial = 0;
    if (aCount && repair) {
        uint8_t *keep = malloc(aCount);
        partial = audio_mark_partial(aPcm, aRegion, aCount, 160, keep);
        uint32_t o = 0;
        for (uint32_t i = 0; i < aCount; i++) {
            if (!keep[i])
                continue;
            memmove(aPcm + (size_t)o * 320, aPcm + (size_t)i * 320, 640);
            aTime[o] = aTime[i];
            aRegion[o] = aRegion[i];
            o++;
        }
        aCount = o;
        free(keep);
    }
    uint32_t *order = aCount ? malloc((size_t)aCount * sizeof(uint32_t)) : NULL;
    uint32_t placed = aCount;
    if (repair && aCount)
        placed = audio_order(aPcm, aCount, 160, order);
    else
        for (uint32_t i = 0; i < aCount; i++)
            order[i] = i;
    uint32_t reordered = 0, joins = 0, harshIn = 0, harshOut = 0;
    for (uint32_t i = 1; i < aCount; i++) {
        if (aTime[i] - aTime[i - 1] < 20000) {
            joins++;
            harshIn += harsh_join(aPcm + (size_t)(i - 1) * 320, aPcm + (size_t)i * 320);
        }
    }
    for (uint32_t i = 1; i < placed; i++) {
        const uint32_t a = order[i - 1], b = order[i];
        if ((int64_t)aTime[b] - (int64_t)aTime[a] < 20000)
            harshOut += harsh_join(aPcm + (size_t)a * 320, aPcm + (size_t)b * 320);
    }
    const int trace = getenv("R3DCONV_TRACE") != NULL;   /* placement events on stderr */
    int16_t fill[AUDIO_CONCEAL_MAX * 320];
    int64_t lastWritten = -1;
    for (uint32_t i = 0; i < placed; i++) {
        if (i && order[i] < order[i - 1]) {
            reordered++;
            if (trace)
                fprintf(stderr, "trace %.3f reorder\n", aTime[order[i]] / 1e6);
        }
        const int harsh = lastWritten >= 0 &&
                          audio_join_harsh(aPcm + (size_t)lastWritten * 320, aPcm + (size_t)order[i] * 320, 160);
        const int64_t silence = audio_place(&ap, aTime[order[i]], 160, harsh);
        if (silence < 0) {
            audioDropped++;
            if (trace)
                fprintf(stderr, "trace %.3f drop\n", aTime[order[i]] / 1e6);
            continue;
        }
        if (silence > 0) {
            if (trace)
                fprintf(stderr, "trace %.3f silence %.1f ms\n", aTime[order[i]] / 1e6, silence * 1e6 / ap.rateMilliHz);
            audioGaps++;
            audioSilence += (uint64_t)silence;
        }
        if (ap.conceal) {
            if (trace)
                fprintf(stderr, "trace %.3f conceal %u\n", aTime[order[i]] / 1e6, ap.conceal);
            audioConcealed += ap.conceal;
        }
        if (wav) {
            wav_silence(wav, eq, (uint64_t)silence);
            if (ap.conceal) {
                const int16_t *next = aPcm + (size_t)order[i] * 320;
                audio_conceal(lastWritten >= 0 ? aPcm + (size_t)lastWritten * 320 : next, next, 160, ap.conceal, fill);
                wav_write(wav, eq, fill, ap.conceal * 160);
            }
            int16_t frame[320];   /* a copy: aPcm stays unfiltered for the next join and fill */
            memcpy(frame, aPcm + (size_t)order[i] * 320, sizeof(frame));
            wav_write(wav, eq, frame, 160);
        }
        lastWritten = order[i];
    }

    fprintf(stderr, "%.1f s, top %u frames, bottom %u frames, damaged %u/%u, output %llu frames at %u fps\n",
            lastUs / 1e6, scr[0].frames, scr[1].frames, scr[0].corrupt, scr[1].corrupt,
            (unsigned long long)emitted, fps);
    if (audioFrames || dspSnaps || audioRate) {
        fprintf(stderr, "audio: %u frames (%.1f s), %u partial, %u reordered, %u duplicates, %u silent gaps (%.1f s), %u dropped, "
                        "%u same-region repeats, %u lost frames filled in, %u DSP snapshots\n",
                audioFrames, audioFrames * 160.0 * 1000.0 / ap.rateMilliHz, partial, reordered, aCount - placed, audioGaps,
                audioSilence * 1000.0 / ap.rateMilliHz, audioDropped, regionSwitches, audioConcealed, dspSnaps);
        if (ap.minLagSamples <= ap.maxLagSamples)
            fprintf(stderr, "audio vs timestamps: %+.1f .. %+.1f ms (negative = audio earlier)\n",
                    ap.minLagSamples * 1e6 / ap.rateMilliHz, ap.maxLagSamples * 1e6 / ap.rateMilliHz);
        if (joins && repair)
            fprintf(stderr, "audio joins: harsh %.1f%% as recorded, %.1f%% after reordering (inside frames ~5%%)\n",
                    100.0 * harshIn / joins, 100.0 * harshOut / joins);
        else if (joins)
            fprintf(stderr, "audio joins: harsh %.1f%% as recorded (inside frames ~5%%)\n", 100.0 * harshIn / joins);
    }
    if (wav) {
        wav_silence(wav, eq, audio_pad_to(&ap, lastUs));
        const long end = ftell(wav);
        wav_header(wav, (ap.rateMilliHz + 500u) / 1000u, (uint32_t)(end - 44));
        fclose(wav);
    }
    free(order);
    free(aPcm);
    free(aTime);
    free(aRegion);
    return 0;
}
