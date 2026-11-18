/* See join.h. */
#include "join.h"

#include "r3d_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
_Static_assert(sizeof(off_t) == 8, "joins up to 3900 MB need a 64-bit off_t");

#define BUF    (1024u * 1024u)   /* 1 MB at a time: reading and writing take turns on one card */
#define REPORT (1024u * 1024u)

typedef struct {
    R3dRecHeader h;
    R3dEndInfo e;
} EndRec;

static int is_video(uint32_t m)
{
    return m == R3D_MAGIC_FRAME || m == R3D_MAGIC_XFRAME || m == R3D_MAGIC_YFRAME || m == R3D_MAGIC_HFRAME ||
           m == R3D_MAGIC_QFRAME || m == R3D_MAGIC_CFRAME;
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* A closed recording: its header, size, and the end record in its last 32 bytes. */
static int open_rec(const char *path, FILE **f, R3dFileHeader *fh, uint64_t *size, EndRec *end)
{
    *f = fopen(path, "rb");
    if (!*f)
        return JOIN_IO;
    /* read in large pieces: the records are small (a frame ~10-50 KB), and stdio's own buffer is
       tiny (139 MB took 123 s, ~1.1 MB/s, on an Old 3DS, 2026-11-16) */
    setvbuf(*f, NULL, _IOFBF, BUF);
    if (fread(fh, 1, sizeof(*fh), *f) != sizeof(*fh) || fseeko(*f, 0, SEEK_END))
        return JOIN_IO;
    const off_t n = ftello(*f);
    if (n < 0)
        return JOIN_IO;
    *size = (uint64_t)n;
    if (fh->magic != R3D_MAGIC_FILE || *size < sizeof(*fh) + sizeof(*end) || (*size & 15u))
        return JOIN_DIFFERENT;
    if (fseeko(*f, (off_t)(*size - sizeof(*end)), SEEK_SET) || fread(end, 1, sizeof(*end), *f) != sizeof(*end))
        return JOIN_IO;
    if (end->h.magic != R3D_MAGIC_END || end->h.len != sizeof(R3dEndInfo))
        return JOIN_DIFFERENT;   /* not closed properly (a recovered recording): kept apart */
    return fseeko(*f, (off_t)sizeof(*fh), SEEK_SET) ? JOIN_IO : JOIN_OK;
}

static int same_kind(const R3dFileHeader *a, const R3dFileHeader *b)
{
    return a->version == b->version && a->isNew3ds == b->isNew3ds && a->mode == b->mode && a->fps == b->fps &&
           a->audioRate == b->audioRate && !(a->flags & R3D_FILE_CLIP) && !(b->flags & R3D_FILE_CLIP);
}

/* `n` bytes from `in` to `out` through `buf`. */
static int copy(FILE *in, FILE *out, uint64_t n, uint8_t *buf, uint64_t *done, uint64_t total, uint64_t *reported,
                JoinProgress progress, void *ctx)
{
    while (n) {
        const uint32_t take = n < BUF ? (uint32_t)n : BUF;
        if (fread(buf, 1, take, in) != take || fwrite(buf, 1, take, out) != take)
            return JOIN_IO;
        n -= take;
        *done += take;
        if (progress && *done >= *reported + REPORT) {
            *reported = *done;
            if (progress(ctx, *done, total))
                return JOIN_STOPPED;
        }
    }
    return JOIN_OK;
}

/* The records of `in` from the file header to its end record (at `endAt`) into `out`, `shift`
   later; *last: the latest time among them. The walk must end exactly at the end record. */
static int copy_records(FILE *in, FILE *out, uint64_t endAt, uint32_t shift, uint32_t *last, uint8_t *buf,
                        uint64_t *done, uint64_t total, uint64_t *reported, JoinProgress progress, void *ctx)
{
    uint64_t at = sizeof(R3dFileHeader);
    if (fseeko(in, (off_t)at, SEEK_SET))
        return JOIN_IO;
    *last = 0;
    while (at < endAt) {
        R3dRecHeader h;
        if (at + sizeof(h) > endAt)
            return JOIN_DIFFERENT;
        if (fread(&h, 1, sizeof(h), in) != sizeof(h))
            return JOIN_IO;
        const uint32_t m = h.magic;
        if (m != R3D_MAGIC_PAD && m != R3D_MAGIC_DSPMEM && m != R3D_MAGIC_AUDIO && !is_video(m))
            return JOIN_DIFFERENT;
        if (is_video(m) && (h.screen & 0x3Fu) > 1u)
            return JOIN_DIFFERENT;
        const uint64_t stored = is_video(m) && m != R3D_MAGIC_FRAME ? ((uint64_t)h.len + 15u) & ~15ull : h.len;
        if (at + sizeof(h) + stored > endAt)
            return JOIN_DIFFERENT;
        if (m != R3D_MAGIC_PAD) {
            if (h.timeUs > *last)
                *last = h.timeUs;
            h.timeUs += shift;
        }
        if (fwrite(&h, 1, sizeof(h), out) != sizeof(h))
            return JOIN_IO;
        *done += sizeof(h);
        const int rc = copy(in, out, stored, buf, done, total, reported, progress, ctx);
        if (rc != JOIN_OK)
            return rc;
        at += sizeof(h) + stored;
    }
    return JOIN_OK;
}

static int finish(FILE *a, FILE *b, FILE *out, uint8_t *buf, const char *part, int rc)
{
    if (a)
        fclose(a);
    if (b)
        fclose(b);
    if (out && fclose(out) && rc == JOIN_OK)
        rc = JOIN_IO;
    free(buf);
    if (rc != JOIN_OK && out)
        remove(part);
    return rc;
}

int r3d_join(const char *first, const char *next, const char *part, JoinProgress progress, void *ctx)
{
    FILE *a = NULL, *b = NULL, *out = NULL;
    R3dFileHeader ha, hb;
    EndRec ea, eb;
    uint64_t sa = 0, sb = 0;
    int rc = open_rec(first, &a, &ha, &sa, &ea);
    if (rc == JOIN_OK)
        rc = open_rec(next, &b, &hb, &sb, &eb);
    if (rc != JOIN_OK)
        return finish(a, b, NULL, NULL, part, rc);
    const uint64_t bodyA = sa - sizeof(ea), bodyB = sb - sizeof(hb) - sizeof(eb), total = bodyA + bodyB + sizeof(ea);
    /* the longest it can be: the first's end record time (its last record comes no later) */
    if (!same_kind(&ha, &hb) || (uint64_t)ea.h.timeUs + JOIN_GAP_US + eb.h.timeUs > JOIN_MAX_US ||
        total > JOIN_MAX_BYTES)
        return finish(a, b, NULL, NULL, part, JOIN_DIFFERENT);
    uint8_t *buf = malloc(BUF);
    if (!buf)
        return finish(a, b, NULL, NULL, part, JOIN_IO);
    out = fopen(part, "wb");
    if (!out)
        return finish(a, b, NULL, buf, part, JOIN_IO);
    setvbuf(out, NULL, _IOFBF, BUF);
    uint64_t done = 0, reported = 0;

    /* the first recording's header and records as they are (checked on the way) */
    if (fwrite(&ha, 1, sizeof(ha), out) != sizeof(ha))
        return finish(a, b, out, buf, part, JOIN_IO);
    done = sizeof(ha);
    uint32_t lastA = 0, lastB = 0;
    rc = copy_records(a, out, sa - sizeof(ea), 0, &lastA, buf, &done, total, &reported, progress, ctx);
    if (rc != JOIN_OK)
        return finish(a, b, out, buf, part, rc);
    /* the next one's go on JOIN_GAP_US after the first's last record: the pause shows as that much
       of its last picture (its end record may lie later: the tail of a recording that gave its
       memory back at once is not on the card) */
    const uint32_t offset = lastA + JOIN_GAP_US;
    rc = copy_records(b, out, sb - sizeof(eb), offset, &lastB, buf, &done, total, &reported, progress, ctx);
    if (rc != JOIN_OK)
        return finish(a, b, out, buf, part, rc);

    /* one end record for both: the whole length, the frames of both */
    EndRec end = ea;
    end.h.timeUs = offset + eb.h.timeUs;
    end.e.framesTop = ea.e.framesTop + eb.e.framesTop;
    end.e.framesBottom = ea.e.framesBottom + eb.e.framesBottom;
    end.e.dropped = ea.e.dropped + eb.e.dropped;
    end.e.reserved = 0;
    if (fwrite(&end, 1, sizeof(end), out) != sizeof(end) || fflush(out))
        return finish(a, b, out, buf, part, JOIN_IO);
    done += sizeof(end);
    const off_t written = ftello(out);
    if (written < 0 || (uint64_t)written != total || done != total)
        return finish(a, b, out, buf, part, JOIN_IO);
    if (progress)
        progress(ctx, total, total);
    return finish(a, b, out, buf, part, JOIN_OK);
}

static int write_mark(const char *mark, const char *next)
{
    FILE *f = fopen(mark, "wb");
    if (!f)
        return -1;
    const size_t n = strlen(next);
    const int ok = fwrite(next, 1, n, f) == n;
    return fclose(f) == 0 && ok ? 0 : -1;
}

int r3d_join_commit(const char *first, const char *next, const char *part, const char *old, const char *mark)
{
    if (write_mark(mark, next)) {
        remove(mark);
        return -1;
    }
    if (rename(first, old)) {
        remove(mark);   /* nothing changed */
        return -1;
    }
    if (rename(part, first)) {
        if (rename(old, first) == 0)
            remove(mark);   /* back as it was */
        return -1;
    }
    remove(old);
    remove(next);
    remove(mark);
    return 0;
}

int r3d_join_recover(const char *first, const char *next, const char *part, const char *old, const char *mark)
{
    if (!exists(mark)) {
        if (exists(part))
            remove(part);   /* a join cut short: it is made again */
        return JOIN_NOTHING;
    }
    /* the mark is written once `part` is whole; then `first` -> `old`, `part` -> `first` */
    int a = exists(first), p = exists(part), o = exists(old);
    if (a && p && !o) {   /* nothing renamed yet: undone, joined again later */
        remove(part);
        remove(mark);
        return JOIN_UNDONE;
    }
    if (!a && p && o && rename(part, first) == 0) {
        a = 1;
        p = 0;
    }
    if (a && !p) {   /* the joined recording is in place */
        remove(old);
        remove(next);
        remove(mark);
        return JOIN_FINISHED;
    }
    if (!a && o && rename(old, first) == 0) {   /* the first recording back as it was */
        remove(part);
        remove(mark);
        return JOIN_UNDONE;
    }
    return JOIN_STUCK;
}
