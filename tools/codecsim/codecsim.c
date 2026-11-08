/*
 * codecsim — T6 on the host: runs the Lite codecs over a real bench recording
 * (rec_last.r3dtest from bench.3gx), verifies every frame round-trips, and
 * reports compressed sizes and the SD rate they would need at 30 fps.
 *
 * Usage: codecsim <recording.r3dtest> [keyframe_interval=30]
 * CPU timings printed here are host timings: only the 3DS numbers count.
 */
#include "codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Optional frame dump: CODECSIM_DUMP=<dir> CODECSIM_EVERY=<n> writes every n-th
   decoded frame of each screen as a PPM (rotated to the screen's orientation). */
static const char *g_dump_dir;
static uint32_t g_dump_every = 300;

static void dump_ppm(const uint16_t *f, uint32_t cols, uint32_t rows, int screen, uint32_t idx, uint32_t time_us)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/s%d_%05u_t%06u.ppm", g_dump_dir, screen, idx, time_us / 1000);
    FILE *o = fopen(path, "wb");
    if (!o) {
        return;
    }
    /* Stored column-major: column c = screen x, word r = bottom-to-top y. */
    fprintf(o, "P6\n%u %u\n255\n", cols, rows);
    for (uint32_t y = 0; y < rows; y++) {
        for (uint32_t x = 0; x < cols; x++) {
            const uint16_t v = f[x * rows + (rows - 1 - y)];
            const unsigned char px[3] = { (unsigned char)((v >> 11) << 3), (unsigned char)(((v >> 5) & 0x3F) << 2),
                                          (unsigned char)((v & 0x1F) << 3) };
            fwrite(px, 1, 3, o);
        }
    }
    fclose(o);
}

#define MAGIC_FILE  0x42443352u
#define MAGIC_FRAME 0x46443352u
#define MAGIC_PAD   0x50443352u
#define MAGIC_XFRAME 0x58443352u
#define MAGIC_YFRAME 0x59443352u
#define MAX_FRAMES  200000

enum { C_RAW, C_XRLE, C_TILES, C_XR32, C_COUNT };
static const char *const CODEC_NAMES[C_COUNT] = { "raw", "xor_rle", "tiles8", "xr32" };

typedef struct {
    uint32_t cols, rows, n;
    uint16_t *prev, *dec, *src, *fprev, *xprev;   /* src: decoded R3DX input frames */
    uint8_t *buf;
    uint32_t frames, dups, errors, corrupt, skipped, need_key;
    uint32_t first_us, last_us;
    uint32_t *sizes[C_COUNT];
} screen_t;

static double g_ns_fused, g_ns_xr32;

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void screen_frame(screen_t *s, const uint16_t *cur, uint32_t keyint, uint32_t time_us)
{
    const int key = (s->frames % keyint) == 0;
    const uint16_t *ref = key ? NULL : s->prev;
    uint32_t i = s->frames;

    if (s->frames == 0) {
        s->first_us = time_us;
    }
    s->last_us = time_us;
    s->sizes[C_RAW][i] = s->n * 2;

    size_t x = codec_xrle_encode(ref, cur, s->n, s->buf);
    s->sizes[C_XRLE][i] = (uint32_t)x;
    if (!key && x <= 2 * ((s->n + 0x7FFF) / 0x8000)) {
        s->dups++;
    }
    memcpy(s->dec, s->prev, s->n * 2);
    if (codec_xrle_decode(s->buf, x, s->dec, s->n, key) != 0 || memcmp(s->dec, cur, s->n * 2) != 0) {
        s->errors++;
    }

    size_t t = codec_tiles_encode(ref, cur, s->cols, s->rows, s->buf);
    s->sizes[C_TILES][i] = (uint32_t)t;
    memcpy(s->dec, s->prev, s->n * 2);
    if (codec_tiles_decode(s->buf, t, s->dec, s->cols, s->rows) != 0 || memcmp(s->dec, cur, s->n * 2) != 0) {
        s->errors++;
    }

    /* Fused encoders as the recorder calls them (packed source, step 1): host speed of old vs new. */
    double t0 = now_ns();
    codec_xrle_encode_strided(s->fprev, (const uint8_t *)cur, s->cols, s->rows, s->rows * 2, 1, key, s->buf);
    double t1 = now_ns();
    size_t x2 = codec_xr32_encode_strided(s->xprev, (const uint8_t *)cur, s->cols, s->rows, s->rows * 2, 1, key, s->buf);
    double t2 = now_ns();
    g_ns_fused += t1 - t0;
    g_ns_xr32 += t2 - t1;
    s->sizes[C_XR32][i] = (uint32_t)x2;
    memcpy(s->dec, s->prev, s->n * 2);
    if (codec_xr32_decode(s->buf, x2, s->dec, s->n, key) != 0 || memcmp(s->dec, cur, s->n * 2) != 0) {
        s->errors++;
    }

    memcpy(s->prev, cur, s->n * 2);
    s->frames++;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <recording.r3dtest> [keyframe_interval]\n", argv[0]);
        return 2;
    }
    const uint32_t keyint = argc > 2 ? (uint32_t)atoi(argv[2]) : 30;
    g_dump_dir = getenv("CODECSIM_DUMP");
    if (getenv("CODECSIM_EVERY")) {
        g_dump_every = (uint32_t)atoi(getenv("CODECSIM_EVERY"));
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    uint8_t hdr[32];
    if (fread(hdr, 1, 32, f) != 32 || rd32(hdr) != MAGIC_FILE) {
        fprintf(stderr, "not a bench recording\n");
        return 1;
    }

    screen_t scr[2];
    memset(scr, 0, sizeof(scr));
    uint8_t rec[16];
    uint8_t *payload = malloc(400 * 240 * 2 + 64);

    while (fread(rec, 1, 16, f) == 16) {
        const uint32_t magic = rd32(rec), len = rd32(rec + 4), time_us = rd32(rec + 8);
        const int xframe = magic == MAGIC_XFRAME || magic == MAGIC_YFRAME;
        const int key = (rec[12] & 0x40) != 0;
        const uint8_t screen = rec[12] & 0x3F;
        const uint32_t cols = (uint32_t)rec[14] | (uint32_t)rec[15] << 8;
        if (magic == 0) {
            break; /* preallocated tail of a recording that was not closed */
        }
        if (magic == 0x41443352u || magic == 0x4D443352u) {   /* R3DA audio / R3DM DSP snapshot */
            fseek(f, len, SEEK_CUR);
            continue;
        }
        if (magic == MAGIC_PAD) {
            fseek(f, len, SEEK_CUR);
            continue;
        }
        if ((magic != MAGIC_FRAME && !xframe) || screen > 1 || len > 400 * 240 * 2 + 64 || cols == 0) {
            fprintf(stderr, "bad record at offset %ld: end of valid data\n", ftell(f) - 16);
            break;
        }
        const uint32_t stored = xframe ? ((len + 15) & ~15u) : len;
        if (fread(payload, 1, stored, f) != stored) {
            break; /* truncated tail */
        }
        screen_t *s = &scr[screen];
        if (!s->n) {
            s->cols = cols;
            s->rows = xframe ? ((cols == 200 || cols == 160) ? 120 : 240) : len / (cols * 2);
            s->n = cols * s->rows;
            s->prev = calloc(s->n, 2);
            s->dec = calloc(s->n, 2);
            s->src = calloc(s->n, 2);
            s->fprev = calloc(s->n, 2);
            s->xprev = calloc(s->n, 2);
            s->buf = malloc(codec_xrle_max_size(s->n) > codec_tiles_max_size(cols, s->rows)
                            ? codec_xrle_max_size(s->n) : codec_tiles_max_size(cols, s->rows));
            for (int c = 0; c < C_COUNT; c++) {
                s->sizes[c] = malloc(sizeof(uint32_t) * MAX_FRAMES);
            }
        }
        const uint16_t *frame = (const uint16_t *)payload;
        if (xframe) {
            const int bad = magic == MAGIC_YFRAME ? codec_xr32_decode(payload, len, s->src, s->n, key)
                                                  : codec_xrle_decode(payload, len, s->src, s->n, key);
            if (bad != 0 || (!key && s->need_key)) {
                if (bad != 0) {
                    s->corrupt++;
                    if (s->corrupt <= 10) {
                        fprintf(stderr, "screen %u: corrupt %s frame #%u (t %u ms, fmt 0x%02X, len %u) at offset %ld\n",
                                screen, key ? "key" : "delta", s->frames, time_us / 1000, rec[13], len,
                                ftell(f) - 16 - (long)stored);
                    }
                } else {
                    s->skipped++;
                }
                s->need_key = 1;   /* resync at the next keyframe */
                continue;
            }
            s->need_key = 0;
            frame = s->src;
        }
        if (g_dump_dir && s->frames % g_dump_every == 0) {
            dump_ppm(frame, s->cols, s->rows, screen, s->frames, time_us);
        }
        if (s->frames < MAX_FRAMES) {
            screen_frame(s, frame, keyint, time_us);
        }
    }
    fclose(f);

    printf("file: %s, keyframe every %u frames\n\n", argv[1], keyint);
    printf("%-7s %-8s %7s %9s %9s %9s %9s %7s %8s\n",
           "screen", "codec", "frames", "mean_KB", "p50_KB", "p95_KB", "max_KB", "ratio", "MB/s@30");
    double total[C_COUNT] = { 0 };
    for (int si = 0; si < 2; si++) {
        screen_t *s = &scr[si];
        if (!s->frames) {
            continue;
        }
        for (int c = 0; c < C_COUNT; c++) {
            uint64_t sum = 0;
            for (uint32_t i = 0; i < s->frames; i++) {
                sum += s->sizes[c][i];
            }
            qsort(s->sizes[c], s->frames, sizeof(uint32_t), cmp_u32);
            const double mean = (double)sum / s->frames;
            const double mbps = mean * 30 / (1024.0 * 1024.0);
            total[c] += mbps;
            printf("%-7s %-8s %7u %9.1f %9.1f %9.1f %9.1f %7.2f %8.2f\n",
                   si == 0 ? "top" : "bottom", CODEC_NAMES[c], s->frames, mean / 1024,
                   s->sizes[c][s->frames / 2] / 1024.0, s->sizes[c][s->frames * 95 / 100] / 1024.0,
                   s->sizes[c][s->frames - 1] / 1024.0, (double)s->n * 2 / mean, mbps);
        }
        printf("%-7s %ux%u stored, %u unchanged frames (%.1f%%), round-trip errors: %u, corrupt input %u, skipped to resync %u\n\n",
               "", s->cols, s->rows, s->dups, 100.0 * s->dups / s->frames, s->errors, s->corrupt, s->skipped);
    }
    printf("both screens at 30 fps: raw %.2f MB/s, xor_rle %.2f MB/s, tiles8 %.2f MB/s, xr32 %.2f MB/s\n",
           total[C_RAW], total[C_XRLE], total[C_TILES], total[C_XR32]);
    printf("host encode time, fused xor_rle %.0f ms, xr32 %.0f ms (x%.2f)\n",
           g_ns_fused / 1e6, g_ns_xr32 / 1e6, g_ns_xr32 > 0 ? g_ns_fused / g_ns_xr32 : 0.0);
    return 0;
}
