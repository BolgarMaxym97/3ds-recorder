#include "gallery.h"
#include "prepare.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
#include "huff.h"
#include "r3d_format.h"
#include "recorder.h"

#define MAX_ITEMS 1000
#define SCAN_LIMIT (8u << 20)      /* bytes read from the start looking for the first keyframes */
#define TAIL_SCAN (256u << 10)     /* bytes read from the end to estimate a cut-off file's length */
#define MAX_RECORD (512u << 10)    /* larger than any frame record: anything bigger is not one */

static Thread g_thread;
static LightLock g_lock;
static LightEvent g_wake;
static volatile bool g_quit, g_hold;

/* shared with the UI, under g_lock */
static GalList g_list;
static GalPreview g_preview;
static char g_want[32];
static char g_deleteName[32];
static bool g_deleteDone, g_deleteOk;

/* the thread's own */
static u16 *g_frame;               /* one decoded screen, column-major (400 x 240 words) */
static u16 *g_work[2];             /* tiled top / bottom being filled before a swap */

/* ------------------------------------------------------------------ pixels */

/* Index of pixel (x, y from the top) in a GPU tiled texture `w` wide (8 x 8 tiles, Morton order). */
static inline u32 tiled(u32 x, u32 y, u32 w)
{
    return (((y >> 3) * (w >> 3) + (x >> 3)) << 6) |
           ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
}

/* A screen as the recorder stores it (column-major, word 0 = bottom row) into a tiled texture. */
static void to_texture(const u16 *frame, u32 cols, u16 *tex)
{
    for (u32 x = 0; x < cols; x++) {
        const u16 *col = frame + x * 240;
        for (u32 y = 0; y < 240; y++)
            tex[tiled(x, y, GAL_TEX_W)] = col[239 - y];
    }
}

/* The top screen shrunk `k` x (400 x 240 -> 400/k x 240/k), box-averaged, into a tiled texture
   `texW` x `texH`: 8 x for the list (50 x 30), 4 x for the panel's last recording (100 x 60). */
static void to_thumb(const u16 *frame, u16 *thumb, u32 texW, u32 texH, u32 k)
{
    memset(thumb, 0, texW * texH * 2);
    const u32 n = k * k;
    for (u32 ty = 0; ty < 240 / k; ty++)
        for (u32 tx = 0; tx < 400 / k; tx++) {
            u32 r = 0, g = 0, b = 0;
            for (u32 dx = 0; dx < k; dx++)
                for (u32 dy = 0; dy < k; dy++) {
                    const u16 v = frame[(tx * k + dx) * 240 + 239 - (ty * k + dy)];
                    r += v >> 11;
                    g += (v >> 5) & 63;
                    b += v & 31;
                }
            thumb[tiled(tx, ty, texW)] = (u16)((r / n) << 11 | (g / n) << 5 | (b / n));
        }
}

/* ------------------------------------------------------------------ files */

static Handle open_rec(const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), REC_FILES_FS "/%s", name);
    Handle f = 0;
    if (R_FAILED(FSUSER_OpenFile(&f, recorder_archive(), fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return 0;
    return f;
}

static bool read_at(Handle f, u64 off, void *buf, u32 len)
{
    u32 got = 0;
    return R_SUCCEEDED(FSFILE_Read(f, &got, off, buf, len)) && got == len;
}

static bool plausible(const R3dRecHeader *h)
{
    const u32 m = h->magic;
    return (m == R3D_MAGIC_YFRAME || m == R3D_MAGIC_HFRAME || m == R3D_MAGIC_QFRAME || m == R3D_MAGIC_CFRAME || m == R3D_MAGIC_XFRAME ||
            m == R3D_MAGIC_FRAME || m == R3D_MAGIC_AUDIO) &&
           h->len < MAX_RECORD && (h->screen & 0x3F) <= R3D_STREAM_AUDIO && h->timeUs < 6u * 3600u * 1000000u;
}

/* Header and end record; a file without R3DE: its length estimated from the last records found
   in its tail (records are 16-byte aligned, so a record header starts on one of those). */
static void read_info(GalItem *it)
{
    Handle f = open_rec(it->name);
    if (!f)
        return;
    R3dFileHeader fh;
    if (read_at(f, 0, &fh, sizeof(fh)) && fh.magic == R3D_MAGIC_FILE) {
        it->readable = true;
        it->fps = fh.fps;
        it->both = fh.mode == R3D_MODE_FULL_BOTH;
        it->bottomOnly = fh.mode == R3D_MODE_FULL_BOTTOM;
        it->audio = fh.audioRate != 0;
        it->clip = (fh.flags & R3D_FILE_CLIP) != 0;
    }
    struct {
        R3dRecHeader h;
        R3dEndInfo e;
    } end;
    if (it->readable && it->bytes >= sizeof(fh) + sizeof(end) &&
        read_at(f, it->bytes - sizeof(end), &end, sizeof(end)) && end.h.magic == R3D_MAGIC_END &&
        end.h.len == sizeof(R3dEndInfo)) {
        it->ended = true;
        it->durationMs = end.h.timeUs / 1000;
        it->dropped = end.e.dropped;
    } else if (it->readable && it->bytes > sizeof(fh) + 16) {
        u64 from = it->bytes > sizeof(fh) + TAIL_SCAN ? it->bytes - TAIL_SCAN : sizeof(fh);
        from = sizeof(fh) + ((from - sizeof(fh)) & ~15ull);
        const u32 len = (u32)((it->bytes - from) & ~15ull);
        u8 *buf = malloc(len);
        if (buf && read_at(f, from, buf, len)) {
            u32 last = 0;
            for (u32 p = 0; p + sizeof(R3dRecHeader) <= len; p += 16) {
                const R3dRecHeader *h = (const R3dRecHeader *)(buf + p);
                if (plausible(h) && h->timeUs > last)
                    last = h->timeUs;
            }
            it->durationMs = last / 1000;
        }
        free(buf);
    }
    FSFILE_Close(f);
}

/* Finds the first keyframe of the screens in `want` (bit 0 top, bit 1 bottom), decodes each into
   `frame` (column-major) and hands it to `got`. Returns the screens found. */
static int scan_keys(const char *name, int want, u16 *frame, void (*got)(int screen, const u16 *frame, void *ctx),
                     void *ctx)
{
    Handle f = open_rec(name);
    if (!f)
        return 0;
    int found = 0;
    u8 *payload = NULL;
    u64 off = sizeof(R3dFileHeader);
    while (found != want && off < SCAN_LIMIT) {
        R3dRecHeader h;
        if (!read_at(f, off, &h, sizeof(h)))
            break;
        off += sizeof(h);
        const u32 m = h.magic;
        if (m == R3D_MAGIC_PAD || m == R3D_MAGIC_AUDIO || m == R3D_MAGIC_DSPMEM) {
            off += h.len;
            continue;
        }
        const bool delta = m == R3D_MAGIC_YFRAME || m == R3D_MAGIC_HFRAME || m == R3D_MAGIC_QFRAME || m == R3D_MAGIC_CFRAME || m == R3D_MAGIC_XFRAME;
        const u32 sc = h.screen & 0x3F;
        if ((m != R3D_MAGIC_FRAME && !delta) || sc > 1 || h.len > MAX_RECORD)
            break;
        const u32 stored = delta ? (h.len + 15) & ~15u : h.len;
        const u32 cols = sc == 0 ? 400 : 320, words = cols * 240;
        const bool key = m == R3D_MAGIC_FRAME || (h.screen & R3D_SCREEN_KEY);
        if (key && (want & (1 << sc)) && !(found & (1 << sc)) && h.cols == cols) {
            if (!payload && !(payload = malloc(MAX_RECORD)))
                break;
            int bad = !read_at(f, off, payload, h.len);
            if (!bad) {
                if (m == R3D_MAGIC_FRAME) {
                    bad = h.len < words * 2;
                    if (!bad)
                        memcpy(frame, payload, words * 2);
                } else if (m == R3D_MAGIC_YFRAME)
                    bad = codec_xr32_decode(payload, h.len, frame, words, 1);
                else if (m == R3D_MAGIC_QFRAME)
                    bad = codec_q565_decode(payload, h.len, frame, words);
                else if (m == R3D_MAGIC_CFRAME)
                    bad = codec_c420_decode(payload, h.len, frame, cols, 240);
                else if (m == R3D_MAGIC_HFRAME) {
                    static HuffTables tables;   /* the gallery's thread only */
                    bad = huff_xr32_decode(payload, h.len, frame, words, 1, &tables);
                }
                else
                    bad = codec_xrle_decode(payload, h.len, frame, words, 1);
            }
            if (!bad) {
                got((int)sc, frame, ctx);
                found |= 1 << sc;
            }
        }
        off += stored;
    }
    free(payload);
    FSFILE_Close(f);
    return found;
}

static int g_thumbScreen;   /* the screen the list thumbnail shows (1: a bottom-screen-only recording) */

static void got_preview(int screen, const u16 *frame, void *thumb)
{
    to_texture(frame, screen == 0 ? 400 : 320, g_work[screen]);
    if (screen == g_thumbScreen && thumb)
        to_thumb(frame, thumb, GAL_THUMB_W, GAL_THUMB_H, 8);
}

static void got_thumb(int screen, const u16 *frame, void *thumb)
{
    (void)screen;
    to_thumb(frame, thumb, GAL_THUMB_W, GAL_THUMB_H, 8);
}

/* The selected recording: g_work[0] / [1] (tiled) and its list thumbnail. */
static int read_frames(const char *name, int want, u16 *thumb)
{
    g_thumbScreen = want == 2 ? 1 : 0;
    return scan_keys(name, want, g_frame, got_preview, thumb);
}

static void got_last(int screen, const u16 *frame, void *thumb)
{
    (void)screen;
    to_thumb(frame, thumb, GAL_LAST_W, GAL_LAST_H, 4);
}

bool gal_last_thumb(const char *name, u16 *thumb)
{
    u16 *frame = malloc(400 * 240 * 2);
    const bool ok = frame && (scan_keys(name, 1, frame, got_last, thumb) == 1 ||
                              scan_keys(name, 2, frame, got_last, thumb) == 2);   /* bottom screen only */
    free(frame);
    return ok;
}

static int cmp_newest(const void *a, const void *b)
{
    return strcmp(((const GalItem *)b)->name, ((const GalItem *)a)->name);
}

static void read_list(void)
{
    GalItem *items = calloc(MAX_ITEMS, sizeof(GalItem));
    int n = 0;
    u64 bytes = 0;
    Handle dir = 0;
    if (items && R_SUCCEEDED(FSUSER_OpenDirectory(&dir, recorder_archive(), fsMakePath(PATH_ASCII, REC_FILES_FS)))) {
        static FS_DirectoryEntry e[16];
        u32 got = 0;
        while (n < MAX_ITEMS && R_SUCCEEDED(FSDIR_Read(dir, &got, 16, e)) && got > 0)
            for (u32 i = 0; i < got && n < MAX_ITEMS; i++) {
                if ((e[i].attributes & FS_ATTRIBUTE_DIRECTORY) || strcmp(e[i].shortExt, "R3D") != 0)
                    continue;
                GalItem *it = &items[n++];
                u32 k = 0;
                for (; k + 1 < sizeof(it->name) && e[i].name[k]; k++)
                    it->name[k] = (char)e[i].name[k]; /* ASCII: yyyymmdd_hhmmss.r3d */
                it->name[k] = 0;
                it->bytes = e[i].fileSize;
                bytes += it->bytes;
            }
        FSDIR_Close(dir);
    }
    if (n)
        qsort(items, n, sizeof(GalItem), cmp_newest);
    LightLock_Lock(&g_lock);
    g_list.items = items;
    g_list.count = n;
    g_list.bytes = bytes;
    g_list.loaded = true;
    g_list.version++;
    LightLock_Unlock(&g_lock);
}

static GalItem *find(const char *name)
{
    for (int i = 0; i < g_list.count; i++)
        if (strcmp(g_list.items[i].name, name) == 0)
            return &g_list.items[i];
    return NULL;
}

/* ------------------------------------------------------------------ thread */

static bool do_delete(void)
{
    char name[32];
    LightLock_Lock(&g_lock);
    snprintf(name, sizeof(name), "%s", g_deleteName);
    g_deleteName[0] = 0;
    LightLock_Unlock(&g_lock);
    if (!name[0])
        return false;
    prepare_forget(name); /* the MP4 made from it (and a half-made one) goes too */
    char path[96];
    snprintf(path, sizeof(path), REC_FILES_FS "/%s", name);
    Handle probe = 0;
    bool ok = R_SUCCEEDED(FSUSER_DeleteFile(recorder_archive(), fsMakePath(PATH_ASCII, path)));
    if (!ok && R_FAILED(FSUSER_OpenFile(&probe, recorder_archive(), fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        ok = true; /* already gone (deleted twice): that is what was asked */
    else if (probe)
        FSFILE_Close(probe);
    LightLock_Lock(&g_lock);
    GalItem *it = ok ? find(name) : NULL;
    if (it) {
        free(it->thumb);
        g_list.bytes -= it->bytes;
        const int i = (int)(it - g_list.items);
        memmove(it, it + 1, (size_t)(g_list.count - i - 1) * sizeof(GalItem));
        g_list.count--;
        g_list.version++;
    }
    g_deleteDone = true;
    g_deleteOk = ok;
    LightLock_Unlock(&g_lock);
    return true;
}

static bool do_preview(void)
{
    char name[32];
    int want = 1;   /* screens to read: bit 0 top, bit 1 bottom */
    LightLock_Lock(&g_lock);
    const bool need = g_want[0] && strcmp(g_want, g_preview.name) != 0;
    snprintf(name, sizeof(name), "%s", g_want);
    GalItem *it = need ? find(name) : NULL;
    if (it)
        want = !it->info || it->both ? 3 : it->bottomOnly ? 2 : 1;
    LightLock_Unlock(&g_lock);
    if (!need)
        return false;
    u16 *thumb = malloc(GAL_THUMB_W * GAL_THUMB_H * 2);
    const int got = it ? read_frames(name, want, thumb) : 0;
    const int primary = want == 2 ? 2 : 1;   /* the screen the thumbnail comes from */
    LightLock_Lock(&g_lock);
    snprintf(g_preview.name, sizeof(g_preview.name), "%s", name);
    g_preview.ok = (got & primary) != 0;
    g_preview.hasBottom = (got & 2) != 0;
    for (int s = 0; s < 2; s++) { /* the decoded buffers become the shown ones */
        u16 *shown = s ? g_preview.bottom : g_preview.top;
        if (s)
            g_preview.bottom = g_work[1];
        else
            g_preview.top = g_work[0];
        g_work[s] = shown;
    }
    g_preview.version++;
    it = find(name);
    if (it && (got & primary) && !it->thumb) {
        it->thumb = thumb;
        thumb = NULL;
        g_list.version++;
    }
    LightLock_Unlock(&g_lock);
    free(thumb);
    return true;
}

/* One item still missing its details, then its thumbnail, from the top of the list. */
static bool do_next_item(void)
{
    char name[32] = "";
    bool needInfo = false;
    LightLock_Lock(&g_lock);
    for (int i = 0; i < g_list.count && !name[0]; i++)
        if (!g_list.items[i].info) {
            snprintf(name, sizeof(name), "%s", g_list.items[i].name);
            needInfo = true;
        }
    for (int i = 0; i < g_list.count && !name[0]; i++) {
        const GalItem *it = &g_list.items[i];
        if (!it->thumb && !it->thumbFailed && it->readable)
            snprintf(name, sizeof(name), "%s", it->name);
    }
    GalItem copy = {0};
    if (needInfo)
        copy = *find(name);
    LightLock_Unlock(&g_lock);
    if (!name[0])
        return false;
    if (needInfo) {
        read_info(&copy);
        LightLock_Lock(&g_lock);
        GalItem *it = find(name);
        if (it) {
            it->readable = copy.readable;
            it->ended = copy.ended;
            it->audio = copy.audio;
            it->both = copy.both;
            it->bottomOnly = copy.bottomOnly;
            it->clip = copy.clip;
            it->fps = copy.fps;
            it->durationMs = copy.durationMs;
            it->dropped = copy.dropped;
            it->info = true;
            g_list.version++;
        }
        LightLock_Unlock(&g_lock);
        return true;
    }
    u16 *thumb = malloc(GAL_THUMB_W * GAL_THUMB_H * 2);
    const int got = thumb ? scan_keys(name, 1, g_frame, got_thumb, thumb) : 0;
    LightLock_Lock(&g_lock);
    GalItem *it = find(name);
    if (it) {
        if (got & 1) {
            it->thumb = thumb;
            thumb = NULL;
        } else
            it->thumbFailed = true;
        g_list.version++;
    }
    LightLock_Unlock(&g_lock);
    free(thumb);
    return true;
}

static void thread_main(void *arg)
{
    (void)arg;
    read_list();
    while (!g_quit) {
        /* deleting and the selected preview go first, between the background items */
        if (!g_hold && (do_delete() || do_preview() || do_next_item()))
            continue;
        LightEvent_Wait(&g_wake);
    }
}

/* ------------------------------------------------------------------ API */

void gal_start(void)
{
    if (g_thread)
        return;
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    memset(&g_list, 0, sizeof(g_list));
    memset(&g_preview, 0, sizeof(g_preview));
    g_want[0] = g_deleteName[0] = 0;
    g_deleteDone = false;
    g_quit = g_hold = false;
    g_frame = malloc(400 * 240 * 2);
    g_work[0] = malloc(GAL_TEX_W * GAL_TEX_H * 2);
    g_work[1] = malloc(GAL_TEX_W * GAL_TEX_H * 2);
    g_preview.top = malloc(GAL_TEX_W * GAL_TEX_H * 2);
    g_preview.bottom = malloc(GAL_TEX_W * GAL_TEX_H * 2);
    if (!g_frame || !g_work[0] || !g_work[1] || !g_preview.top || !g_preview.bottom) {
        gal_stop();
        g_list.loaded = true; /* shows an empty list instead of waiting forever */
        return;
    }
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(thread_main, NULL, 16 * 1024, prio + 1, -2, false);
    if (!g_thread)
        read_list(); /* no thread: at least the list, without details */
}

void gal_stop(void)
{
    if (g_thread) {
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
    }
    for (int i = 0; i < g_list.count; i++)
        free(g_list.items[i].thumb);
    free(g_list.items);
    g_list.items = NULL;
    g_list.count = 0;
    free(g_frame);
    free(g_work[0]);
    free(g_work[1]);
    free(g_preview.top);
    free(g_preview.bottom);
    g_frame = g_work[0] = g_work[1] = g_preview.top = g_preview.bottom = NULL;
}

void gal_lock(void) { LightLock_Lock(&g_lock); }
void gal_unlock(void) { LightLock_Unlock(&g_lock); }
const GalList *gal_list(void) { return &g_list; }
const GalPreview *gal_preview(void) { return &g_preview; }

void gal_want_preview(const char *name)
{
    LightLock_Lock(&g_lock);
    const bool changed = strcmp(g_want, name) != 0;
    if (changed)
        snprintf(g_want, sizeof(g_want), "%s", name);
    LightLock_Unlock(&g_lock);
    if (changed)
        LightEvent_Signal(&g_wake);
}

void gal_delete(const char *name)
{
    LightLock_Lock(&g_lock);
    snprintf(g_deleteName, sizeof(g_deleteName), "%s", name);
    g_deleteDone = false;
    LightLock_Unlock(&g_lock);
    LightEvent_Signal(&g_wake);
}

bool gal_take_deleted(bool *ok)
{
    LightLock_Lock(&g_lock);
    const bool done = g_deleteDone;
    if (done) {
        *ok = g_deleteOk;
        g_deleteDone = false;
    }
    LightLock_Unlock(&g_lock);
    return done;
}

void gal_hold(bool hold)
{
    g_hold = hold;
    if (!hold)
        LightEvent_Signal(&g_wake);
}

void gal_read_info(GalItem *it) { read_info(it); }
