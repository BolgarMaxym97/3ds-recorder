/* Replay clips (prep/source/unwrap.c): a stream laid into a ring file as the recorder does, then
   unwrapped and compared with the records the clip must hold. Synthetic streams (time wrap, pads,
   a screen whose key frame comes late, one screen only) and, with a recording as the argument, that
   recording's own stream (the result is left next to the test for a reader to try). */
#include "unwrap.h"
#include "r3d_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
            g_fail = 1;                                                  \
        }                                                                \
    } while (0)

static uint32_t g_rng = 2026;
static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

typedef struct { uint64_t at; R3dRecHeader h; uint32_t stored; } Rec;

typedef struct {
    uint8_t *bytes;     /* the whole stream (file header first) */
    uint64_t size;
    Rec *recs;
    uint32_t n;
    R3dFileHeader fh;
} Stream;

static int is_video(uint32_t m)
{
    return m == R3D_MAGIC_FRAME || m == R3D_MAGIC_XFRAME || m == R3D_MAGIC_YFRAME || m == R3D_MAGIC_HFRAME ||
           m == R3D_MAGIC_QFRAME || m == R3D_MAGIC_CFRAME;
}

static void add(Stream *s, uint32_t magic, uint32_t len, uint32_t timeUs, uint8_t screen)
{
    const uint32_t stored = is_video(magic) ? (len + 15u) & ~15u : len;
    s->bytes = realloc(s->bytes, s->size + 16 + stored);
    s->recs = realloc(s->recs, (s->n + 1) * sizeof(Rec));
    Rec *r = &s->recs[s->n++];
    r->at = s->size;
    r->h = (R3dRecHeader){magic, len, timeUs, screen, 2, (uint16_t)(magic == R3D_MAGIC_AUDIO ? 160 : (screen & 1) ? 320 : 400)};
    r->stored = stored;
    memcpy(s->bytes + s->size, &r->h, 16);
    for (uint32_t i = 0; i < stored; i++)
        s->bytes[s->size + 16 + i] = (uint8_t)rnd();
    s->size += 16 + stored;
}

static void stream_start(Stream *s, uint8_t mode)
{
    memset(s, 0, sizeof(*s));
    s->fh = (R3dFileHeader){R3D_MAGIC_FILE, R3D_VERSION, 0, 0, mode, 30, 0, R3D_AUDIO_RATE_MHZ, 1000000000000ull};
    s->bytes = malloc(32);
    memcpy(s->bytes, &s->fh, 32);
    s->size = 32;
}

/* The stream's last ringBytes as the recorder's file holds them, with the clip header. */
static void write_ring(const Stream *s, const char *path, uint32_t dataOff, uint64_t ringBytes, uint64_t start,
                       uint32_t startUs, uint32_t endUs, uint64_t startMs)
{
    uint8_t *ring = malloc(ringBytes);
    for (uint64_t i = 0; i < ringBytes; i++)
        ring[i] = (uint8_t)rnd();   /* what the card held before */
    for (uint64_t at = 0; at < s->size; at++)
        ring[at % ringBytes] = s->bytes[at];
    R3dRingInfo in;
    memset(&in, 0, sizeof(in));
    in.magic = R3D_MAGIC_RING;
    in.version = R3D_RING_VERSION;
    in.dataOff = dataOff;
    in.startUs = startUs;
    in.ringBytes = ringBytes;
    in.start = start;
    in.end = s->size;
    in.endUs = endUs;
    in.file = s->fh;
    in.file.startTimeMs = startMs;
    FILE *f = fopen(path, "wb");
    uint8_t *gap = calloc(1, dataOff);
    memcpy(gap, &in, sizeof(in));
    fwrite(gap, 1, dataOff, f);
    fwrite(ring, 1, ringBytes, f);
    fclose(f);
    free(gap);
    free(ring);
}

/* What the clip starting at record `first` must hold, by the rules in unwrap.h; compared with `path`. */
static void check_clip(const Stream *s, uint32_t first, uint32_t startUs, uint32_t endUs, uint64_t startMs, const char *path)
{
    const int want[2] = {s->fh.mode != R3D_MODE_FULL_BOTTOM, s->fh.mode != R3D_MODE_FULL_TOP};
    int have[2] = {0, 0}, all = 0;
    uint32_t keyRec[2] = {0, 0}, firstRec[2] = {0, 0};
    for (uint32_t i = first; i < s->n && !all; i++) {
        const Rec *r = &s->recs[i];
        if (!is_video(r->h.magic))
            continue;
        const uint32_t sc = r->h.screen & 1u, rel = r->h.timeUs - startUs;
        if (rel > 15000000u && (have[0] || have[1]))
            break;
        if (r->h.screen & R3D_SCREEN_KEY) {
            if (!have[sc])
                firstRec[sc] = i;
            have[sc] = 1;
            keyRec[sc] = i;
            all = (have[0] || !want[0]) && (have[1] || !want[1]);
        }
    }
    uint32_t zeroRec;
    if (all && have[0] && have[1])
        zeroRec = keyRec[0] > keyRec[1] ? keyRec[0] : keyRec[1];
    else {
        const int sc = have[0] ? 0 : 1;
        keyRec[sc] = firstRec[sc];
        zeroRec = firstRec[sc];
    }
    const uint32_t base = s->recs[zeroRec].h.timeUs;

    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    R3dFileHeader fh;
    CHECK(fread(&fh, 1, 32, f) == 32);
    CHECK(fh.magic == R3D_MAGIC_FILE && fh.mode == s->fh.mode && fh.audioRate == s->fh.audioRate);
    CHECK(fh.flags & R3D_FILE_CLIP);
    CHECK(fh.startTimeMs == startMs + (base - startUs) / 1000u);
    uint32_t frames[2] = {0, 0}, lastUs = 0, kept = 0;
    uint8_t *got = malloc(512u * 1024u);
    for (uint32_t i = first; i < s->n; i++) {
        const Rec *r = &s->recs[i];
        if (r->h.magic == R3D_MAGIC_PAD || r->h.magic == R3D_MAGIC_DSPMEM)
            continue;
        const int32_t t = (int32_t)(r->h.timeUs - base);
        if (is_video(r->h.magic)) {
            const uint32_t sc = r->h.screen & 1u;
            if (!have[sc]) {
                if (!(r->h.screen & R3D_SCREEN_KEY))
                    continue;
                have[sc] = 1;
            } else if (i < keyRec[sc])
                continue;
            frames[sc]++;
        } else if (t < 0)
            continue;
        R3dRecHeader h;
        if (fread(&h, 1, 16, f) != 16 || fread(got, 1, r->stored, f) != r->stored) {
            CHECK(!"clip shorter than expected");
            break;
        }
        const uint32_t tu = t < 0 ? 0u : (uint32_t)t;
        CHECK(h.magic == r->h.magic && h.len == r->h.len && h.screen == r->h.screen && h.cols == r->h.cols);
        CHECK(h.timeUs == tu);
        CHECK(memcmp(got, s->bytes + r->at + 16, r->stored) == 0);
        lastUs = tu > lastUs ? tu : lastUs;
        kept++;
    }
    struct { R3dRecHeader h; R3dEndInfo e; } end;
    CHECK(fread(&end, 1, sizeof(end), f) == sizeof(end));
    CHECK(end.h.magic == R3D_MAGIC_END && end.h.len == sizeof(R3dEndInfo));
    CHECK(end.e.framesTop == frames[0] && end.e.framesBottom == frames[1]);
    const int32_t endT = (int32_t)(endUs - base);
    CHECK(end.h.timeUs == (endT > 0 && (uint32_t)endT > lastUs ? (uint32_t)endT : lastUs));
    CHECK(fgetc(f) == EOF);
    CHECK(kept > 0);
    fclose(f);
    free(got);
}

static uint32_t g_calls;
static int count_progress(void *ctx, uint64_t done, uint64_t total)
{
    (void)ctx;
    CHECK(done <= total);
    g_calls++;
    return 0;
}
static int stop_progress(void *ctx, uint64_t done, uint64_t total)
{
    (void)ctx; (void)done; (void)total;
    return 1;
}

/* A stream like a recording's: both screens at ~30 fps with key frames now and then, sound every
   ~5 ms, a pad record here and there; times start `t0` (so they can wrap inside the clip). */
static void synth(Stream *s, uint8_t mode, uint32_t t0, uint32_t seconds, uint32_t botKeyEvery)
{
    stream_start(s, mode);
    uint32_t n[2] = {0, 0};
    for (uint32_t ms = 0; ms < seconds * 1000u; ms += 5) {
        const uint32_t t = t0 + ms * 1000u;
        add(s, R3D_MAGIC_AUDIO, 656, t - 3000u, R3D_STREAM_AUDIO);
        if (ms % 35 == 0 && mode != R3D_MODE_FULL_BOTTOM) {
            const int key = n[0]++ % 20 == 0;
            add(s, key && (n[0] & 1) ? R3D_MAGIC_CFRAME : R3D_MAGIC_YFRAME, 2000 + rnd() % 9000, t, (uint8_t)(key ? R3D_SCREEN_KEY : 0));
        }
        if (ms % 35 == 15 && mode != R3D_MODE_FULL_TOP) {
            const int key = n[1]++ % botKeyEvery == 7 % botKeyEvery;
            add(s, R3D_MAGIC_YFRAME, 1500 + rnd() % 5000, t + 400u, (uint8_t)(1 | (key ? R3D_SCREEN_KEY : 0)));
        }
        if (ms % 1000 == 995)
            add(s, R3D_MAGIC_PAD, 16 * (1 + rnd() % 40), 0, 0xFF);
    }
}

/* The first top-screen (or, without one, bottom-screen) key frame at or after stream offset `low`. */
static uint32_t first_key(const Stream *s, uint64_t low)
{
    for (uint32_t i = 0; i < s->n; i++)
        if (s->recs[i].at >= low && is_video(s->recs[i].h.magic) && (s->recs[i].h.screen & R3D_SCREEN_KEY))
            return i;
    return s->n;
}

static void run_case(const char *what, Stream *s, uint64_t ringBytes, uint32_t dataOff)
{
    const uint64_t low = s->size > ringBytes ? s->size - ringBytes : 32;
    const uint32_t first = first_key(s, low);
    CHECK(first < s->n);
    if (first >= s->n)
        return;
    const uint32_t startUs = s->recs[first].h.timeUs, endUs = s->recs[s->n - 1].h.timeUs + 700000u;
    write_ring(s, "unwrap_test.rpl", dataOff, ringBytes, s->recs[first].at, startUs, endUs, 5000000000000ull);
    g_calls = 0;
    const int rc = r3d_unwrap("unwrap_test.rpl", "unwrap_test.r3d", count_progress, NULL);
    CHECK(rc == UNWRAP_OK);
    if (rc == UNWRAP_OK)
        check_clip(s, first, startUs, endUs, 5000000000000ull, "unwrap_test.r3d");
    printf("  %s: stream %llu KB, ring %llu KB, clip from %llu KB: %s\n", what, (unsigned long long)s->size / 1024,
           (unsigned long long)ringBytes / 1024, (unsigned long long)s->recs[first].at / 1024, rc == UNWRAP_OK ? "ok" : "FAILED");
    free(s->bytes);
    free(s->recs);
}

static void test_synthetic(void)
{
    Stream s;
    synth(&s, R3D_MODE_FULL_BOTH, 1000000u, 40, 25);
    run_case("both screens, stream longer than the ring", &s, 1024u * 1024u, 512u * 1024u);
    synth(&s, R3D_MODE_FULL_BOTH, 0xFFFFFFFFu - 9000000u, 30, 25);
    run_case("time wraps inside the clip", &s, 2u * 1024u * 1024u, 4096);
    synth(&s, R3D_MODE_FULL_BOTH, 5000u, 6, 25);
    run_case("stream shorter than the ring", &s, 4u * 1024u * 1024u, 512u * 1024u);
    synth(&s, R3D_MODE_FULL_TOP, 77000000u, 20, 25);
    run_case("top screen only", &s, 512u * 1024u, 512u * 1024u);
    synth(&s, R3D_MODE_FULL_BOTTOM, 77000000u, 20, 9);
    run_case("bottom screen only", &s, 512u * 1024u, 512u * 1024u);
    synth(&s, R3D_MODE_FULL_BOTH, 123456u, 30, 100000);   /* the bottom screen never gets a key frame */
    run_case("a screen without a key frame", &s, 1024u * 1024u, 512u * 1024u);
}

static void test_refused(void)
{
    Stream s;
    synth(&s, R3D_MODE_FULL_BOTH, 0, 4, 25);
    const uint32_t first = first_key(&s, 32);
    write_ring(&s, "unwrap_test.rpl", 4096, 4u * 1024u * 1024u, s.recs[first].at, s.recs[first].h.timeUs, 4000000u, 1);
    CHECK(r3d_unwrap("no_such_file.rpl", "unwrap_test.r3d", NULL, NULL) == UNWRAP_IO);
    remove("unwrap_test.r3d");
    /* a recording is not a clip */
    FILE *f = fopen("unwrap_test.rpl", "wb");
    fwrite(s.bytes, 1, (size_t)s.size, f);
    fclose(f);
    CHECK(r3d_unwrap("unwrap_test.rpl", "unwrap_test.r3d", NULL, NULL) == UNWRAP_BAD);
    CHECK(fopen("unwrap_test.r3d", "rb") == NULL);
    /* the file cut short: a read error, nothing left behind */
    synth(&s, R3D_MODE_FULL_BOTH, 0, 30, 25);
    const uint32_t k = first_key(&s, s.size - 1024u * 1024u);
    write_ring(&s, "unwrap_test.rpl", 4096, 1024u * 1024u, s.recs[k].at, s.recs[k].h.timeUs, 30000000u, 1);
    f = fopen("unwrap_test.rpl", "rb");
    uint8_t *half = malloc(600000);
    const size_t got = fread(half, 1, 600000, f);
    fclose(f);
    f = fopen("unwrap_test.rpl", "wb");
    fwrite(half, 1, got, f);
    fclose(f);
    free(half);
    CHECK(r3d_unwrap("unwrap_test.rpl", "unwrap_test.r3d", NULL, NULL) == UNWRAP_IO);
    CHECK(fopen("unwrap_test.r3d", "rb") == NULL);
    /* stopped by the caller */
    synth(&s, R3D_MODE_FULL_BOTH, 0, 40, 25);
    const uint32_t k2 = first_key(&s, s.size - 3u * 1024u * 1024u);
    write_ring(&s, "unwrap_test.rpl", 4096, 3u * 1024u * 1024u, s.recs[k2].at, s.recs[k2].h.timeUs, 40000000u, 1);
    CHECK(r3d_unwrap("unwrap_test.rpl", "unwrap_test.r3d", stop_progress, NULL) == UNWRAP_STOPPED);
    CHECK(fopen("unwrap_test.r3d", "rb") == NULL);
    remove("unwrap_test.rpl");
}

/* A real recording's stream through a ring of 60% of its size. */
static void test_recording(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        g_fail = 1;
        return;
    }
    Stream s;
    memset(&s, 0, sizeof(s));
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    s.bytes = malloc((size_t)size);
    CHECK(fread(s.bytes, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    memcpy(&s.fh, s.bytes, 32);
    uint64_t at = 32;
    while (at + 16 <= (uint64_t)size) {
        R3dRecHeader h;
        memcpy(&h, s.bytes + at, 16);
        if (h.magic == R3D_MAGIC_END || (!is_video(h.magic) && h.magic != R3D_MAGIC_AUDIO && h.magic != R3D_MAGIC_PAD &&
                                         h.magic != R3D_MAGIC_DSPMEM))
            break;
        const uint32_t stored = is_video(h.magic) && h.magic != R3D_MAGIC_FRAME ? (h.len + 15u) & ~15u : h.len;
        if (at + 16 + stored > (uint64_t)size)
            break;
        s.recs = realloc(s.recs, (s.n + 1) * sizeof(Rec));
        s.recs[s.n++] = (Rec){at, h, stored};
        at += 16 + stored;
    }
    s.size = at;
    const uint64_t ring = (s.size * 6 / 10) & ~(512ull * 1024 - 1);
    CHECK(ring > 0 && s.n > 0);
    if (!ring || !s.n)
        return;
    const uint32_t first = first_key(&s, s.size - ring);
    CHECK(first < s.n);
    const uint32_t startUs = s.recs[first].h.timeUs, endUs = s.recs[s.n - 1].h.timeUs;
    write_ring(&s, "unwrap_real.rpl", 512u * 1024u, ring, s.recs[first].at, startUs, endUs, s.fh.startTimeMs + startUs / 1000u);
    const int rc = r3d_unwrap("unwrap_real.rpl", "unwrap_real.r3d", count_progress, NULL);
    CHECK(rc == UNWRAP_OK);
    if (rc == UNWRAP_OK)
        check_clip(&s, first, startUs, endUs, s.fh.startTimeMs + startUs / 1000u, "unwrap_real.r3d");
    printf("  %s: %u records, ring %llu KB, clip of the last %u s -> unwrap_real.r3d: %s\n", path, s.n,
           (unsigned long long)ring / 1024, (endUs - startUs) / 1000000u, rc == UNWRAP_OK ? "ok" : "FAILED");
    remove("unwrap_real.rpl");
}

int main(int argc, char **argv)
{
    test_synthetic();
    test_refused();
    remove("unwrap_test.r3d");
    if (argc > 1)
        test_recording(argv[1]);
    printf("test_unwrap: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail;
}
