/* See unwrap.h. */
#include "unwrap.h"

#include "r3d_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 1 MB at a time each way: reading and writing take turns on one card (clips took 1.6-2 MB/s with
   256 KB pieces on an Old 3DS, 2026-11-14) */
#define READ_BUF   (1024u * 1024u)
#define WRITE_BUF  (1024u * 1024u)
#define MAX_RECORD (512u * 1024u)   /* the largest record is a raw or XR32 top-screen frame, ~200 KB */
#define SCAN_US    15000000u   /* a screen with no key frame this far into the clip is not waited for */
#define REPORT     (1024u * 1024u)

typedef struct {
    FILE *f;
    R3dRingInfo in;
    uint8_t *buf;
    uint64_t pos;   /* stream offset of buf[0] */
    uint32_t len;
} Src;

/* `n` bytes of the stream at `at` (inside [start, end)). */
static int src_read(Src *s, uint64_t at, void *out, uint32_t n)
{
    uint8_t *o = out;
    while (n) {
        if (at < s->pos || at >= s->pos + s->len) {
            const uint64_t in = at % s->in.ringBytes;
            uint64_t chunk = s->in.ringBytes - in;   /* up to the ring's end in the file */
            if (chunk > s->in.end - at)
                chunk = s->in.end - at;
            if (chunk > READ_BUF)
                chunk = READ_BUF;
            if (!chunk || fseek(s->f, (long)(s->in.dataOff + in), SEEK_SET) || fread(s->buf, 1, (size_t)chunk, s->f) != chunk)
                return -1;
            s->pos = at;
            s->len = (uint32_t)chunk;
        }
        const uint32_t have = (uint32_t)(s->pos + s->len - at), take = n < have ? n : have;
        memcpy(o, s->buf + (at - s->pos), take);
        o += take;
        at += take;
        n -= take;
    }
    return 0;
}

static int is_video(uint32_t m)
{
    return m == R3D_MAGIC_FRAME || m == R3D_MAGIC_XFRAME || m == R3D_MAGIC_YFRAME || m == R3D_MAGIC_HFRAME ||
           m == R3D_MAGIC_QFRAME || m == R3D_MAGIC_CFRAME;
}

/* The record at `at`: its header and the bytes it takes after the header. 0: a record of the
   stream; 1: the stream ends here (its end, or bytes that are no record); -1: read error. */
static int record_at(Src *s, uint64_t at, R3dRecHeader *h, uint32_t *stored)
{
    if (at + sizeof(*h) > s->in.end)
        return 1;
    if (src_read(s, at, h, sizeof(*h)))
        return -1;
    const uint32_t m = h->magic;
    if (m != R3D_MAGIC_PAD && m != R3D_MAGIC_DSPMEM && m != R3D_MAGIC_AUDIO && !is_video(m))
        return 1;
    if (h->len > MAX_RECORD || (is_video(m) && (h->screen & 0x3Fu) > 1u))
        return 1;
    *stored = is_video(m) && m != R3D_MAGIC_FRAME ? (h->len + 15u) & ~15u : h->len;
    return at + sizeof(*h) + *stored > s->in.end ? 1 : 0;
}

static int finish(Src *s, FILE *out, uint8_t *copy, const char *dst, int rc)
{
    if (out && fclose(out) && rc == UNWRAP_OK)
        rc = UNWRAP_IO;
    if (s->f)
        fclose(s->f);
    free(s->buf);
    free(copy);
    if (rc != UNWRAP_OK)
        remove(dst);
    return rc;
}

int r3d_unwrap(const char *src, const char *dst, UnwrapProgress progress, void *ctx)
{
    Src s;
    memset(&s, 0, sizeof(s));
    s.f = fopen(src, "rb");
    if (!s.f)
        return UNWRAP_IO;
    if (fread(&s.in, 1, sizeof(s.in), s.f) != sizeof(s.in)) {
        fclose(s.f);
        return UNWRAP_BAD;
    }
    const R3dRingInfo *in = &s.in;
    if (in->magic != R3D_MAGIC_RING || in->version != R3D_RING_VERSION || in->file.magic != R3D_MAGIC_FILE ||
        in->dataOff < sizeof(*in) || !in->ringBytes || in->ringBytes > 0x7FFFFFFFu - in->dataOff || in->start >= in->end ||
        in->end - in->start > in->ringBytes) {
        fclose(s.f);
        return UNWRAP_BAD;
    }
    s.buf = malloc(READ_BUF);
    uint8_t *copy = malloc(MAX_RECORD);
    if (!s.buf || !copy)
        return finish(&s, NULL, copy, dst, UNWRAP_IO);

    /* which screens the recording has */
    const int want[2] = {in->file.mode != R3D_MODE_FULL_BOTTOM, in->file.mode != R3D_MODE_FULL_TOP};
    /* pass 1, the clip's first seconds: each screen's key frames up to the point where every
       screen has had one */
    int have[2] = {0, 0}, all = 0;
    uint64_t keyAt[2] = {0, 0}, firstAt[2] = {0, 0};
    uint32_t keyUs[2] = {0, 0}, firstUs[2] = {0, 0};   /* counted from in->startUs */
    for (uint64_t at = in->start; !all;) {
        R3dRecHeader h;
        uint32_t stored = 0;
        const int r = record_at(&s, at, &h, &stored);
        if (r < 0)
            return finish(&s, NULL, copy, dst, UNWRAP_IO);
        if (r)
            break;
        if (is_video(h.magic)) {
            const uint32_t sc = h.screen & 0x3Fu, rel = h.timeUs - in->startUs;
            if (rel > SCAN_US && rel < 0x80000000u && (have[0] || have[1]))
                break;
            if ((h.screen & R3D_SCREEN_KEY) || h.magic == R3D_MAGIC_FRAME) {
                if (!have[sc]) {
                    firstAt[sc] = at;
                    firstUs[sc] = rel < 0x80000000u ? rel : 0u;
                }
                have[sc] = 1;
                keyAt[sc] = at;
                keyUs[sc] = rel < 0x80000000u ? rel : 0u;
                all = (have[0] || !want[0]) && (have[1] || !want[1]);
            }
        }
        at += sizeof(h) + stored;
    }
    if (!have[0] && !have[1])
        return finish(&s, NULL, copy, dst, UNWRAP_BAD);
    /* Every screen has a key frame: time 0 is the last one found (the later screen's first), and
       the other screen starts at its last key frame before it. A screen still without one is not
       waited for: the clip starts at the other's first key frame. */
    uint32_t zero;
    if (all && have[0] && have[1])
        zero = keyUs[0] > keyUs[1] ? keyUs[0] : keyUs[1];
    else {
        const int sc = have[0] ? 0 : 1;
        keyAt[sc] = firstAt[sc];
        zero = firstUs[sc];
    }
    const uint32_t base = in->startUs + zero;

    FILE *out = fopen(dst, "wb");
    if (!out)
        return finish(&s, NULL, copy, dst, UNWRAP_IO);
    setvbuf(out, NULL, _IOFBF, WRITE_BUF);
    R3dFileHeader fh = in->file;
    fh.startTimeMs += zero / 1000u;
    fh.flags |= R3D_FILE_CLIP;
    if (fwrite(&fh, 1, sizeof(fh), out) != sizeof(fh))
        return finish(&s, out, copy, dst, UNWRAP_IO);

    /* pass 2: the records, from the earlier of the screens' key frames */
    uint64_t at = have[0] && (!have[1] || keyAt[0] < keyAt[1]) ? keyAt[0] : keyAt[1];
    const uint64_t from = at, total = in->end - from;
    uint64_t reported = 0;
    uint32_t frames[2] = {0, 0}, lastUs = 0;
    for (;;) {
        R3dRecHeader h;
        uint32_t stored = 0;
        const int r = record_at(&s, at, &h, &stored);
        if (r < 0)
            return finish(&s, out, copy, dst, UNWRAP_IO);
        if (r)
            break;
        const uint64_t recAt = at;
        at += sizeof(h) + stored;
        if (progress && at - from >= reported + REPORT) {
            reported = at - from;
            if (progress(ctx, reported, total))
                return finish(&s, out, copy, dst, UNWRAP_STOPPED);
        }
        if (h.magic == R3D_MAGIC_PAD || h.magic == R3D_MAGIC_DSPMEM)
            continue;
        const int32_t t = (int32_t)(h.timeUs - base);
        if (is_video(h.magic)) {
            const uint32_t sc = h.screen & 0x3Fu;
            if (!have[sc]) {   /* a screen whose first key frame comes late in the clip */
                if (!(h.screen & R3D_SCREEN_KEY) && h.magic != R3D_MAGIC_FRAME)
                    continue;
                have[sc] = 1;
            } else if (recAt < keyAt[sc])
                continue;
            frames[sc]++;
        } else if (t < 0)
            continue;   /* sound from before the clip's start */
        h.timeUs = t < 0 ? 0u : (uint32_t)t;
        if (h.timeUs > lastUs)
            lastUs = h.timeUs;
        if (src_read(&s, recAt + sizeof(h), copy, stored))
            return finish(&s, out, copy, dst, UNWRAP_IO);
        if (fwrite(&h, 1, sizeof(h), out) != sizeof(h) || fwrite(copy, 1, stored, out) != stored)
            return finish(&s, out, copy, dst, UNWRAP_IO);
    }
    if (!frames[0] && !frames[1])
        return finish(&s, out, copy, dst, UNWRAP_BAD);
    /* the clip's length: up to when it was saved (a still picture stays to the end) */
    const int32_t endT = (int32_t)(in->endUs - base);
    const struct {
        R3dRecHeader h;
        R3dEndInfo e;
    } end = {
        {R3D_MAGIC_END, sizeof(R3dEndInfo), endT > 0 && (uint32_t)endT > lastUs ? (uint32_t)endT : lastUs, 0, 0, 0},
        {frames[0], frames[1], 0, 0},
    };
    if (fwrite(&end, 1, sizeof(end), out) != sizeof(end) || fflush(out))
        return finish(&s, out, copy, dst, UNWRAP_IO);
    if (progress)
        progress(ctx, total, total);
    return finish(&s, out, copy, dst, UNWRAP_OK);
}
