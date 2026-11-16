/* Joining a recording the recorder paused by itself with the one it went on in (prep/source/join.c):
   the joined file holds the first's bytes as they were and the next's records later by its length
   plus the gap; recordings of another kind, cut-off ones and broken ones stay apart; a commit cut
   short at any step is finished or undone with nothing lost. */
#include "join.h"
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

static uint32_t g_rng = 1116;
static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

typedef struct {
    uint8_t *bytes;
    uint64_t size;
    uint32_t frames[2], lastUs;
} Rec;

static int is_video(uint32_t m)
{
    return m == R3D_MAGIC_FRAME || m == R3D_MAGIC_XFRAME || m == R3D_MAGIC_YFRAME || m == R3D_MAGIC_HFRAME ||
           m == R3D_MAGIC_QFRAME || m == R3D_MAGIC_CFRAME;
}

static void put(Rec *r, uint32_t magic, uint32_t len, uint32_t timeUs, uint8_t screen)
{
    const uint32_t stored = is_video(magic) && magic != R3D_MAGIC_FRAME ? (len + 15u) & ~15u : len;
    r->bytes = realloc(r->bytes, r->size + 16 + stored);
    const R3dRecHeader h = {magic, len, timeUs, screen, 2, (uint16_t)(magic == R3D_MAGIC_AUDIO ? 160 : (screen & 1) ? 320 : 400)};
    memcpy(r->bytes + r->size, &h, 16);
    for (uint32_t i = 0; i < stored; i++)
        r->bytes[r->size + 16 + i] = (uint8_t)rnd();
    r->size += 16 + stored;
    if (is_video(magic))
        r->frames[screen & 1]++;
    if (timeUs > r->lastUs)
        r->lastUs = timeUs;
}

/* A recording as the recorder closes it: frames of both screens, sound, a pad, a DSP snapshot. */
static Rec make(uint8_t mode, uint32_t seconds, uint64_t startMs, int closed)
{
    Rec r;
    memset(&r, 0, sizeof(r));
    const R3dFileHeader fh = {R3D_MAGIC_FILE, R3D_VERSION, 0, 0, mode, 30, 0, R3D_AUDIO_RATE_MHZ, startMs};
    r.bytes = malloc(32);
    memcpy(r.bytes, &fh, 32);
    r.size = 32;
    for (uint32_t t = 0; t < seconds * 1000000u; t += 33333u) {
        put(&r, t % 1000000u < 33333u ? R3D_MAGIC_CFRAME : R3D_MAGIC_YFRAME, 3000 + rnd() % 5000, t,
            (uint8_t)(t % 1000000u < 33333u ? R3D_SCREEN_KEY : 0));
        put(&r, R3D_MAGIC_CFRAME, 2000 + rnd() % 3000, t + 5000u, 1 | R3D_SCREEN_KEY);
        put(&r, R3D_MAGIC_AUDIO, 160 * 4 + 16, t + 1000u, R3D_STREAM_AUDIO);
        if (t == 300000u)
            put(&r, R3D_MAGIC_PAD, 4096, 0, 0);
        if (t == 600000u)
            put(&r, R3D_MAGIC_DSPMEM, 0x10000, t, R3D_STREAM_DSPMEM);
    }
    if (closed) {
        const struct { R3dRecHeader h; R3dEndInfo e; } end = {
            {R3D_MAGIC_END, sizeof(R3dEndInfo), seconds * 1000000u, 0, 0, 0}, {r.frames[0], r.frames[1], 7, 0}};
        r.bytes = realloc(r.bytes, r.size + 32);
        memcpy(r.bytes + r.size, &end, 32);
        r.size += 32;
    }
    return r;
}

static void save(const Rec *r, const char *path)
{
    FILE *f = fopen(path, "wb");
    fwrite(r->bytes, 1, r->size, f);
    fclose(f);
}

static uint8_t *load(const char *path, uint64_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        *size = 0;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *size = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*size ? *size : 1);
    if (fread(b, 1, *size, f) != *size)
        *size = 0;
    fclose(f);
    return b;
}

static int exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

static void touch(const char *p, const char *text)
{
    FILE *f = fopen(p, "wb");
    fputs(text, f);
    fclose(f);
}

static void clean(void)
{
    const char *names[] = {"j_a.r3d", "j_b.r3d", "j_a.jpart", "j_a.jold", "j_a.jmark"};
    for (int i = 0; i < 5; i++)
        remove(names[i]);
}

#define A "j_a.r3d"
#define B "j_b.r3d"
#define P "j_a.jpart"
#define O "j_a.jold"
#define M "j_a.jmark"

/* The joined file: A's bytes, then B's records with times later by A's length + the gap, one end. */
static void joined_ok(void)
{
    clean();
    Rec a = make(R3D_MODE_FULL_BOTH, 4, 1000, 1), b = make(R3D_MODE_FULL_BOTH, 3, 9000, 1);
    save(&a, A);
    save(&b, B);
    CHECK(r3d_join(A, B, P, NULL, NULL) == JOIN_OK);
    uint64_t n = 0;
    uint8_t *j = load(P, &n);
    CHECK(n == a.size - 32 + b.size - 64 + 32);
    CHECK(j && memcmp(j, a.bytes, a.size - 32) == 0);   /* the first, as it was, header included */
    /* B's records: the same but for the time, JOIN_GAP_US after A's last record */
    const uint32_t off = a.lastUs + JOIN_GAP_US;
    uint64_t at = a.size - 32, bt = 32;
    int ok = 1;
    while (bt < b.size - 32) {
        R3dRecHeader hb, hj;
        memcpy(&hb, b.bytes + bt, 16);
        memcpy(&hj, j + at, 16);
        const uint32_t stored = is_video(hb.magic) && hb.magic != R3D_MAGIC_FRAME ? (hb.len + 15u) & ~15u : hb.len;
        ok &= hj.magic == hb.magic && hj.len == hb.len && hj.screen == hb.screen && hj.cols == hb.cols;
        ok &= hj.timeUs == (hb.magic == R3D_MAGIC_PAD ? hb.timeUs : hb.timeUs + off);
        ok &= memcmp(j + at + 16, b.bytes + bt + 16, stored) == 0;
        at += 16 + stored;
        bt += 16 + stored;
    }
    CHECK(ok);
    R3dRecHeader eh;
    R3dEndInfo ei;
    memcpy(&eh, j + n - 32, 16);
    memcpy(&ei, j + n - 16, 16);
    CHECK(at == n - 32 && eh.magic == R3D_MAGIC_END && eh.timeUs == off + 3000000u);
    CHECK(ei.framesTop == a.frames[0] + b.frames[0] && ei.framesBottom == a.frames[1] + b.frames[1] && ei.dropped == 14);
    /* commit: A is the joined one, B gone, no scratch left */
    CHECK(r3d_join_commit(A, B, P, O, M) == 0);
    uint64_t n2 = 0;
    uint8_t *j2 = load(A, &n2);
    CHECK(n2 == n && memcmp(j, j2, n) == 0);
    CHECK(!exists(B) && !exists(P) && !exists(O) && !exists(M));
    /* and a third file joins on to it the same way (a session paused twice) */
    Rec c = make(R3D_MODE_FULL_BOTH, 2, 20000, 1);
    save(&c, B);
    CHECK(r3d_join(A, B, P, NULL, NULL) == JOIN_OK && r3d_join_commit(A, B, P, O, M) == 0);
    uint64_t n3 = 0;
    uint8_t *j3 = load(A, &n3);
    memcpy(&eh, j3 + n3 - 32, 16);
    CHECK(n3 == n - 32 + c.size - 64 + 32 && eh.timeUs == off + b.lastUs + JOIN_GAP_US + 2000000u);
    free(j);
    free(j2);
    free(j3);
    free(a.bytes);
    free(b.bytes);
    free(c.bytes);
}

/* Kept apart, nothing written: another kind, a cut-off one, a broken one, too long. */
static void kept_apart(void)
{
    Rec a = make(R3D_MODE_FULL_BOTH, 2, 1000, 1);
    struct { Rec b; const char *what; } cases[4];
    cases[0].b = make(R3D_MODE_FULL_TOP, 2, 5000, 1);   cases[0].what = "another mode";
    cases[1].b = make(R3D_MODE_FULL_BOTH, 2, 5000, 0);  cases[1].what = "cut off (no end record)";
    cases[2].b = make(R3D_MODE_FULL_BOTH, 2, 5000, 1);  cases[2].what = "a broken record";
    memset(cases[2].b.bytes + 32, 0x5A, 4);   /* its first record's magic: bytes that are no record */
    cases[3].b = make(R3D_MODE_FULL_BOTH, 2, 5000, 1);  cases[3].what = "too long together";
    {
        R3dRecHeader eh;
        memcpy(&eh, cases[3].b.bytes + cases[3].b.size - 32, 16);
        eh.timeUs = JOIN_MAX_US - 1000000u;
        memcpy(cases[3].b.bytes + cases[3].b.size - 32, &eh, 16);
    }
    for (int i = 0; i < 4; i++) {
        clean();
        save(&a, A);
        save(&cases[i].b, B);
        const int rc = r3d_join(A, B, P, NULL, NULL);
        if (rc != JOIN_DIFFERENT)
            fprintf(stderr, "  %s: rc %d\n", cases[i].what, rc);
        CHECK(rc == JOIN_DIFFERENT);
        CHECK(!exists(P));
        uint64_t na = 0, nb = 0;
        uint8_t *la = load(A, &na), *lb = load(B, &nb);
        CHECK(na == a.size && memcmp(la, a.bytes, na) == 0 && nb == cases[i].b.size);
        free(la);
        free(lb);
        free(cases[i].b.bytes);
    }
    /* a clip is never joined */
    clean();
    Rec b = make(R3D_MODE_FULL_BOTH, 2, 5000, 1);
    b.bytes[19] |= R3D_FILE_CLIP;
    save(&a, A);
    save(&b, B);
    CHECK(r3d_join(A, B, P, NULL, NULL) == JOIN_DIFFERENT && !exists(P));
    /* the first missing (deleted meanwhile) */
    remove(A);
    CHECK(r3d_join(A, B, P, NULL, NULL) == JOIN_IO && !exists(P) && exists(B));
    free(a.bytes);
    free(b.bytes);
}

static int stop_at_half(void *ctx, uint64_t done, uint64_t total)
{
    (void)ctx;
    return done * 2 >= total;
}

/* Stopped by the app: no part left, both as they were. */
static void stopped(void)
{
    clean();
    Rec a = make(R3D_MODE_FULL_BOTH, 6, 1000, 1), b = make(R3D_MODE_FULL_BOTH, 6, 9000, 1);
    save(&a, A);
    save(&b, B);
    CHECK(r3d_join(A, B, P, stop_at_half, NULL) == JOIN_STOPPED);
    CHECK(!exists(P) && exists(A) && exists(B));
    free(a.bytes);
    free(b.bytes);
}

/* A commit cut short at each step: finished or undone, never a recording lost. */
static void recover(void)
{
    Rec a = make(R3D_MODE_FULL_BOTH, 2, 1000, 1), b = make(R3D_MODE_FULL_BOTH, 2, 9000, 1);
    uint64_t nj = 0;
    uint8_t *joined = NULL;
    clean();
    save(&a, A);
    save(&b, B);
    CHECK(r3d_join(A, B, P, NULL, NULL) == JOIN_OK);
    joined = load(P, &nj);

    /* 0. part written, no mark (stopped before the commit): part removed, both kept */
    CHECK(r3d_join_recover(A, B, P, O, M) == JOIN_NOTHING);
    CHECK(!exists(P) && exists(A) && exists(B));

    /* 1. mark written, nothing renamed: undone, both kept */
    save(&a, A);
    save(&b, B);
    touch(P, "x");
    touch(M, B);
    CHECK(r3d_join_recover(A, B, P, O, M) == JOIN_UNDONE);
    CHECK(exists(A) && exists(B) && !exists(P) && !exists(M));
    uint64_t n = 0;
    uint8_t *l = load(A, &n);
    CHECK(n == a.size && memcmp(l, a.bytes, n) == 0);
    free(l);

    /* 2. first renamed to old, part not yet in place: part put in place, finished */
    clean();
    save(&a, O);
    save(&b, B);
    FILE *f = fopen(P, "wb");
    fwrite(joined, 1, nj, f);
    fclose(f);
    touch(M, B);
    CHECK(r3d_join_recover(A, B, P, O, M) == JOIN_FINISHED);
    l = load(A, &n);
    CHECK(n == nj && memcmp(l, joined, n) == 0 && !exists(B) && !exists(O) && !exists(P) && !exists(M));
    free(l);

    /* 3. part in place, old and next still there: finished */
    clean();
    save(&a, O);
    save(&b, B);
    f = fopen(A, "wb");
    fwrite(joined, 1, nj, f);
    fclose(f);
    touch(M, B);
    CHECK(r3d_join_recover(A, B, P, O, M) == JOIN_FINISHED);
    CHECK(exists(A) && !exists(B) && !exists(O) && !exists(M));

    /* 4. only next left to remove */
    save(&b, B);
    touch(M, B);
    CHECK(r3d_join_recover(A, B, P, O, M) == JOIN_FINISHED && !exists(B) && !exists(M));

    /* 5. first renamed away and the part lost: the first comes back, next kept */
    clean();
    save(&a, O);
    save(&b, B);
    touch(M, B);
    CHECK(r3d_join_recover(A, B, P, O, M) == JOIN_UNDONE);
    l = load(A, &n);
    CHECK(n == a.size && memcmp(l, a.bytes, n) == 0 && exists(B) && !exists(O) && !exists(M));
    free(l);

    /* 6. no mark, nothing to do */
    CHECK(r3d_join_recover(A, B, P, O, M) == JOIN_NOTHING && exists(A) && exists(B));

    /* 7. a state no step makes (all three there): nothing touched */
    save(&a, O);
    touch(P, "x");
    touch(M, B);
    CHECK(r3d_join_recover(A, B, P, O, M) == JOIN_STUCK && exists(A) && exists(B) && exists(O) && exists(P));
    clean();
    free(joined);
    free(a.bytes);
    free(b.bytes);
}

int main(void)
{
    joined_ok();
    kept_apart();
    stopped();
    recover();
    clean();
    if (!g_fail)
        printf("test_join: ok\n");
    return g_fail;
}
