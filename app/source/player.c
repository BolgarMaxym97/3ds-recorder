#include "player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_eq.h"
#include "audio_place.h"
#include "codec.h"
#include "huff.h"
#include "r3d_format.h"
#include "recorder.h"

#define READ_BUF (512u << 10)       /* file reads: larger than any record */
#define MAX_RECORD (512u << 10)
#define SLOTS 6                      /* decoded frames per screen, decoded shortly before they are due */
#define LEAD_US 500000u              /* records are read at most this far ahead of the clock */
#define QUEUE_MAX 96                 /* video records read ahead, still compressed ... */
#define QUEUE_BYTES (4u << 20)       /* ... and their bytes */
#define DUE_US 60000u                /* a frame is decoded this long before its time */
#define LATE_US 50000u               /* a frame this long past its time is not shown */
#define THIN_STEP_US 500000u         /* still late this long after thinning began: thin the top screen too */
#define THIN_CALM_US 4000000u        /* no late frame for this long: one step back */
#define AUDIO_LOW 4                  /* sound buffers queued (of WAVE_BUFS): fewer, and the sound is read first */
#define AUDIO_PAD_LAG_US 200000u     /* a video record this far past the sound: silence up to it */
#define WAVE_BUFS 28                 /* ~0.87 s: more than LEAD_US, so writing sound never waits */
#define WAVE_SAMPLES 1024            /* stereo samples per DSP buffer (~31 ms) */
#define SEEK_EARLY_US 2100000u       /* land this much before the target (a top keyframe every ~2 s; ~1 s before 0.10) */
#define SEEK_WINDOW_US 600000u       /* ... or up to this much earlier: the scan to the target is short */
#define KEY_NEAR_US 3200000u         /* a known keyframe this close before the target: start there */
#define KEYS_MAX 16384               /* keyframes remembered per screen (top: one every ~2 s) */
#define CHAIN_MAX 128                /* frames decoded per screen to reach the target (keys: every 60) */
#define FIND_BLOCK (256u << 10)
#define HOP_BLOCK (32u << 10)

typedef struct {
    u16 *px;                         /* linear memory, PLAYER_TEX_W words per screen column */
    u32 timeUs;
    int state;                       /* 0 free, 1 decoded, 2 on screen */
} Slot;

static Thread g_thread;
static LightLock g_lock;
static LightEvent g_wake;
static volatile bool g_quit;

/* file */
static Handle g_file;
static u64 g_size;
static R3dFileHeader g_hdr;
static u8 *g_buf;
static u64 g_bufOff;                 /* file offset of g_buf[0] */
static u32 g_bufLen, g_pos;
static bool g_eof;

/* video */
static u16 *g_cur[2];                /* decoded screens, column-major as recorded */
static bool g_haveKey[2];
static Slot g_slots[2][SLOTS];
/* Video records read ahead of the clock, compressed. In the file the sound of a moment comes
   ~0.2 s after its video (the recorder writes sound in batches), so reading far enough ahead for
   the sound means holding the video meanwhile; decoded frames are far larger (O3DS 2026-11-05: a
   queue of 6 decoded frames filled, the sound behind them never got read, the clock stopped). */
typedef struct {
    R3dRecHeader h;
    u8 *data;
} Queued;
static Queued g_queue[QUEUE_MAX];
static int g_qHead, g_qCount;
static u32 g_qBytes;
static u32 g_durationMs;

/* sound: under g_lock where the main thread reads it */
static bool g_audio, g_ndspOk, g_muted;
static ndspWaveBuf g_wave[WAVE_BUFS];
static s16 *g_waveMem;
static int g_fillBuf, g_fillN;       /* the buffer being filled and its samples */
static int g_oldest, g_queued;       /* the oldest buffer handed to the DSP, how many are */
static u64 g_doneSamples;            /* played since the clock base */
static AudioPlacer g_place;
static AudioEq g_eq;
static s16 g_lastFrame[320];
static bool g_haveLast;

/* clock */
static u32 g_baseUs;                 /* clock at the last start or seek */
static u64 g_wallStart;              /* osGetTime() at that point (no sound), paused time excluded */
static u64 g_pausedAt;
static bool g_paused, g_ended;
static u32 g_thin, g_thinN[2], g_thinUs, g_framesThinned;   /* thinning level 0-2 (decode_due) */
static u32 g_framesLate;             /* frames past their time when their turn came: not shown */
static u32 g_framesShown, g_framesSkipped; /* pictures published / not (no free slot), for app.log */
static u32 g_audioStarved;
static bool g_audioFresh = true;           /* no sound queued since the start or a seek */                 /* sound buffers queued with the DSP's queue already empty (a gap heard) */
static u32 g_lastVideoUs;            /* the latest frame read: the sound is padded to it at the end */
/* The latest time among the frames queued so far. A frame that comes later in the file with an
   earlier time (the recorder stored a bottom-screen frame well after top-screen frames taken at
   the same moment: recordings up to 1.0 of games that left its converter little of the processor)
   is shown at this time, not dropped as late: the bottom screen stood still in such recordings. */
static u32 g_orderUs;
static int g_shown;                  /* screens with a picture in their texture */
static volatile s64 g_seekTo = -1;   /* requested by the UI, done by the thread */
static volatile bool g_seekBusy;     /* a seek is pending or running: the time shown is its target */
static volatile u32 g_seekShowUs;

/* ------------------------------------------------------------------ reading */

/* The next piece of the file is read by a thread of its own while this one decodes. With both in
   one thread a recording at 29 + 28 pictures a second (4.7 MB/s) had 1258 of its pictures shown in
   31 s, 87 late and 406 thinned out: the card took about half of the thread's time (O3DS 0.12.6,
   2026-11-13). g_preState: 0 nothing, 1 being read, 2 read (g_preLen bytes from g_preOff). Only
   fill() asks for a piece and only it takes one; a seek's own reads wait for the thread first. */
static Thread g_preThread;
static LightEvent g_preWake;
static u8 *g_pre;
static u64 g_preOff;
static volatile u32 g_preLen, g_preState;

static void pre_main(void *arg)
{
    (void)arg;
    while (!g_quit) {
        if (__atomic_load_n(&g_preState, __ATOMIC_ACQUIRE) != 1) {
            LightEvent_WaitTimeout(&g_preWake, 50 * 1000 * 1000);
            continue;
        }
        u32 got = 0;
        const u64 left = g_size - g_preOff;
        if (R_FAILED(FSFILE_Read(g_file, &got, g_preOff, g_pre, (u32)(left < READ_BUF ? left : READ_BUF))))
            got = 0;
        g_preLen = got;
        __atomic_store_n(&g_preState, 2, __ATOMIC_RELEASE);
    }
}

/* Waits for a read under way; true when a piece is there. */
static bool pre_wait(void)
{
    while (__atomic_load_n(&g_preState, __ATOMIC_ACQUIRE) == 1 && g_preThread)
        svcSleepThread(500 * 1000);
    return __atomic_load_n(&g_preState, __ATOMIC_ACQUIRE) == 2;
}

static void pre_drop(void)
{
    pre_wait();
    __atomic_store_n(&g_preState, 0, __ATOMIC_RELEASE);
}

static bool fill(u32 need)
{
    if (g_bufLen - g_pos >= need)
        return true;
    memmove(g_buf, g_buf + g_pos, g_bufLen - g_pos);
    g_bufOff += g_pos;
    g_bufLen -= g_pos;
    g_pos = 0;
    while (g_bufLen < need && g_bufOff + g_bufLen < g_size) {
        const u64 end = g_bufOff + g_bufLen;
        const u64 left = g_size - end;
        const u32 want = (u32)(left < READ_BUF - g_bufLen ? left : READ_BUF - g_bufLen);
        if (g_preState && g_preOff == end && pre_wait() && g_preLen) {
            /* the piece read meanwhile; what does not fit stays for the next time */
            const u32 n = g_preLen < want ? g_preLen : want;
            memcpy(g_buf + g_bufLen, g_pre, n);
            g_bufLen += n;
            if (n < g_preLen) {
                memmove(g_pre, g_pre + n, g_preLen - n);
                g_preOff += n;
                g_preLen -= n;
            } else
                __atomic_store_n(&g_preState, 0, __ATOMIC_RELEASE);
            continue;
        }
        pre_drop();   /* another place in the file, or nothing read */
        u32 got = 0;
        if (R_FAILED(FSFILE_Read(g_file, &got, end, g_buf + g_bufLen, want)) || !got)
            break;
        g_bufLen += got;
    }
    if (!g_preState && g_preThread && g_bufOff + g_bufLen < g_size) {
        g_preOff = g_bufOff + g_bufLen;
        __atomic_store_n(&g_preState, 1, __ATOMIC_RELEASE);
        LightEvent_Signal(&g_preWake);
    }
    return g_bufLen - g_pos >= need;
}

static void reposition(u64 off)
{
    pre_drop();   /* a seek reads the file itself */
    g_bufOff = off;
    g_bufLen = g_pos = 0;
    g_eof = false;
}

static void skip(u32 n)
{
    if (g_bufLen - g_pos >= n)
        g_pos += n;
    else
        reposition(g_bufOff + g_pos + n);
}

static bool plausible(const R3dRecHeader *h)
{
    const u32 m = h->magic;
    if (g_durationMs && h->timeUs > g_durationMs * 1000u + 10000000u)
        return false; /* bytes inside a frame that happen to look like a header */
    if (m == R3D_MAGIC_PAD || m == R3D_MAGIC_DSPMEM)
        return h->len <= (1u << 20);
    if (m == R3D_MAGIC_AUDIO)
        return h->len <= 4096 && h->cols == 160;
    return (m == R3D_MAGIC_YFRAME || m == R3D_MAGIC_HFRAME || m == R3D_MAGIC_QFRAME || m == R3D_MAGIC_CFRAME || m == R3D_MAGIC_XFRAME ||
            m == R3D_MAGIC_FRAME) &&
           h->len <= MAX_RECORD &&
           (h->screen & 0x3F) <= 1 && (h->cols == 400 || h->cols == 320);
}

static u32 stored_len(const R3dRecHeader *h)
{
    return h->magic == R3D_MAGIC_YFRAME || h->magic == R3D_MAGIC_HFRAME || h->magic == R3D_MAGIC_QFRAME ||
                       h->magic == R3D_MAGIC_CFRAME ||
                       h->magic == R3D_MAGIC_XFRAME
               ? (h->len + 15) & ~15u
               : h->len;
}

/* ------------------------------------------------------------------ clock and sound */

static u32 rate_mhz(void) { return g_hdr.audioRate ? g_hdr.audioRate : R3D_AUDIO_RATE_MHZ; }

/* Under g_lock: moves finished DSP buffers into g_doneSamples. */
static void audio_reap(void)
{
    while (g_queued && g_wave[g_oldest].status == NDSP_WBUF_DONE) {
        g_doneSamples += g_wave[g_oldest].nsamples;
        g_wave[g_oldest].status = NDSP_WBUF_FREE;
        g_oldest = (g_oldest + 1) % WAVE_BUFS;
        g_queued--;
    }
}

static u32 clock_us(void)
{
    LightLock_Lock(&g_lock);
    u32 t;
    if (g_audio) {
        audio_reap();
        const u64 s = g_doneSamples + (g_queued ? ndspChnGetSamplePos(0) : 0);
        t = g_baseUs + (u32)(s * 1000000000ull / rate_mhz());
    } else {
        const u64 now = g_paused ? g_pausedAt : osGetTime();
        t = g_baseUs + (u32)((now - g_wallStart) * 1000);
    }
    LightLock_Unlock(&g_lock);
    return t;
}

/* Appends samples to the DSP queue; false when there is no free buffer (try again later). */
static bool audio_room(void)
{
    LightLock_Lock(&g_lock);
    audio_reap();
    const bool room = g_queued < WAVE_BUFS - 1;
    LightLock_Unlock(&g_lock);
    return room;
}

static int audio_queued(void)
{
    LightLock_Lock(&g_lock);
    audio_reap();
    const int n = g_queued;
    LightLock_Unlock(&g_lock);
    return n;
}

static void audio_flush(void)
{
    if (!g_fillN)
        return;
    ndspWaveBuf *w = &g_wave[g_fillBuf];
    w->nsamples = (u32)g_fillN;
    DSP_FlushDataCache(w->data_pcm16, (u32)g_fillN * 4);
    LightLock_Lock(&g_lock);
    audio_reap();
    if (g_queued == 0 && !g_audioFresh)
        g_audioStarved++;
    g_audioFresh = false;
    ndspChnWaveBufAdd(0, w);
    g_queued++;
    LightLock_Unlock(&g_lock);
    g_fillBuf = (g_fillBuf + 1) % WAVE_BUFS;
    g_fillN = 0;
}

/* Writes n stereo samples (NULL: silence) through the treble cut. */
static void audio_put(const s16 *pcm, u64 n)
{
    while (n) {
        s16 *dst = g_waveMem + (size_t)g_fillBuf * WAVE_SAMPLES * 2 + (size_t)g_fillN * 2;
        const u32 k = (u32)(n < (u64)(WAVE_SAMPLES - g_fillN) ? n : (u64)(WAVE_SAMPLES - g_fillN));
        if (pcm) {
            memcpy(dst, pcm, k * 4);
            pcm += k * 2;
        } else
            memset(dst, 0, k * 4);
        audio_eq_run(&g_eq, dst, k);
        g_fillN += (int)k;
        n -= k;
        if (g_fillN == WAVE_SAMPLES) {
            while (!g_quit && g_seekTo < 0 && !audio_room())
                svcSleepThread(4 * 1000 * 1000);
            if (g_seekTo >= 0 || g_quit)
                return; /* a seek resets the sound anyway */
            audio_flush();
        }
    }
}

/* One R3DA frame, as r3dconv writes it (clean recordings: no reordering). */
static void audio_frame(const R3dRecHeader *h, const s16 *pcm)
{
    if (h->cols != 160)
        return;
    const int harsh = g_haveLast && audio_join_harsh(g_lastFrame, pcm, 160);
    const s64 silence = audio_place(&g_place, h->timeUs, 160, harsh);
    if (silence < 0)
        return;
    if (silence > 0 && g_haveLast) { /* soft edges around silence (audio_place.h) */
        static s16 tail[320];
        const u32 t = audio_tail(g_lastFrame, 160, (u64)silence, tail);
        audio_put(tail, t);
        audio_put(NULL, (u64)silence - t);
    } else
        audio_put(NULL, (u64)silence);
    if (g_place.conceal) {
        static s16 fillPcm[AUDIO_CONCEAL_MAX * 320];
        audio_conceal(g_haveLast ? g_lastFrame : pcm, pcm, 160, g_place.conceal, fillPcm);
        audio_put(fillPcm, (u64)g_place.conceal * 160);
    }
    if (silence > 0) {
        static s16 faded[320];
        memcpy(faded, pcm, sizeof(faded));
        audio_fade_in(faded, 160);
        audio_put(faded, 160);
    } else
        audio_put(pcm, 160);
    memcpy(g_lastFrame, pcm, sizeof(g_lastFrame));
    g_haveLast = true;
}

/* Keeps the sound running through stretches the recording has no sound in (it is the clock). */
static void audio_pad(u32 timeUs)
{
    if (timeUs < AUDIO_PAD_LAG_US)
        return;
    const u64 target = (u64)(timeUs - AUDIO_PAD_LAG_US) * rate_mhz() / 1000000000ull;
    if (g_place.out < target) {
        const u64 n = target - g_place.out;
        g_place.out = target;
        audio_put(NULL, n);
    }
}

static void audio_reset(u32 atUs)
{
    LightLock_Lock(&g_lock);
    if (g_ndspOk)
        ndspChnWaveBufClear(0);
    for (int i = 0; i < WAVE_BUFS; i++)
        g_wave[i].status = NDSP_WBUF_FREE;
    g_fillBuf = g_fillN = g_oldest = g_queued = 0;
    g_audioFresh = true;   /* the next buffer starts the sound again: not a gap */
    g_doneSamples = 0;
    g_baseUs = atUs;
    g_wallStart = osGetTime();
    g_pausedAt = g_wallStart;
    LightLock_Unlock(&g_lock);
    audio_placer_init(&g_place, rate_mhz());
    g_place.out = (u64)atUs * rate_mhz() / 1000000000ull;
    audio_eq_init(&g_eq, rate_mhz() / 1000.0, AUDIO_EQ_FREQ_HZ, AUDIO_EQ_GAIN_DB, AUDIO_EQ_Q);
    g_haveLast = false;
}

/* ------------------------------------------------------------------ video */

static Slot *free_slot(int sc)
{
    for (int i = 0; i < SLOTS; i++)
        if (g_slots[sc][i].state == 0)
            return &g_slots[sc][i];
    return NULL;
}

static bool has_free_slot(int sc)
{
    LightLock_Lock(&g_lock);
    const bool ok = free_slot(sc) != NULL;
    LightLock_Unlock(&g_lock);
    return ok;
}

/* Decodes a frame record into g_cur; publish: also queue it for the screen. */
static void video_frame(const R3dRecHeader *h, const u8 *payload, bool publish)
{
    const int sc = h->screen & 0x3F;
    const u32 cols = sc ? 320 : 400, words = cols * 240;
    if (h->cols != cols)
        return;
    const bool key = h->magic == R3D_MAGIC_FRAME || (h->screen & R3D_SCREEN_KEY);
    if (!key && !g_haveKey[sc])
        return; /* after a seek: wait for this screen's keyframe */
    if (h->magic == R3D_MAGIC_CFRAME && publish) {
        /* A picture on its own: straight into the texture's memory, not into g_cur and from there
           (a copy of the whole frame less, 60 times a second with both screens at 30). g_cur no
           longer holds the picture: the next frame must be a keyframe, and after C420 it is. */
        LightLock_Lock(&g_lock);
        Slot *s = free_slot(sc);
        LightLock_Unlock(&g_lock);
        if (!s) {
            g_framesSkipped++;
            return;
        }
        g_haveKey[sc] = false;
        if (codec_c420_decode_strided(payload, h->len, s->px, cols, 240, PLAYER_TEX_W) != 0)
            return;
        g_framesShown++;
        GSPGPU_FlushDataCache(s->px, PLAYER_TEX_W * cols * 2);
        LightLock_Lock(&g_lock);
        s->timeUs = h->timeUs;
        s->state = 1;
        LightLock_Unlock(&g_lock);
        return;
    }
    int bad;
    if (h->magic == R3D_MAGIC_FRAME) {
        bad = h->len < words * 2;
        if (!bad)
            memcpy(g_cur[sc], payload, words * 2);
    } else if (h->magic == R3D_MAGIC_YFRAME)
        bad = codec_xr32_decode(payload, h->len, g_cur[sc], words, key);
    else if (h->magic == R3D_MAGIC_QFRAME)
        bad = codec_q565_decode(payload, h->len, g_cur[sc], words);
    else if (h->magic == R3D_MAGIC_CFRAME)
        bad = codec_c420_decode(payload, h->len, g_cur[sc], cols, 240);
    else if (h->magic == R3D_MAGIC_HFRAME) {
        static HuffTables tables;   /* the player's thread only */
        bad = huff_xr32_decode(payload, h->len, g_cur[sc], words, key, &tables);
    }
    else
        bad = codec_xrle_decode(payload, h->len, g_cur[sc], words, key);
    if (bad) {
        g_haveKey[sc] = false; /* damaged: wait for the next keyframe */
        return;
    }
    g_haveKey[sc] = true;
    if (!publish)
        return;
    LightLock_Lock(&g_lock);
    Slot *s = free_slot(sc);
    LightLock_Unlock(&g_lock);
    if (!s) {
        g_framesSkipped++; /* the screen is behind: this picture is not shown */
        return;
    }
    g_framesShown++;
    for (u32 x = 0; x < cols; x++)
        memcpy(s->px + x * PLAYER_TEX_W, g_cur[sc] + x * 240, 240 * 2);
    GSPGPU_FlushDataCache(s->px, PLAYER_TEX_W * cols * 2);
    LightLock_Lock(&g_lock);
    s->timeUs = h->timeUs;
    s->state = 1;
    LightLock_Unlock(&g_lock);
}

static void clear_slots(void)
{
    LightLock_Lock(&g_lock);
    for (int sc = 0; sc < 2; sc++)
        for (int i = 0; i < SLOTS; i++)
            if (g_slots[sc][i].state == 1)
                g_slots[sc][i].state = 0;
    LightLock_Unlock(&g_lock);
}

static bool queue_push(const R3dRecHeader *h, const u8 *payload)
{
    if (g_qCount == QUEUE_MAX || g_qBytes + h->len > QUEUE_BYTES)
        return false;
    u8 *copy = malloc(h->len ? h->len : 1);
    if (!copy)
        return false;
    memcpy(copy, payload, h->len);
    g_queue[(g_qHead + g_qCount) % QUEUE_MAX] = (Queued){*h, copy};
    g_qCount++;
    g_qBytes += h->len;
    return true;
}

static void queue_pop(void)
{
    Queued *q = &g_queue[g_qHead];
    g_qBytes -= q->h.len;
    free(q->data);
    q->data = NULL;
    g_qHead = (g_qHead + 1) % QUEUE_MAX;
    g_qCount--;
}

static void queue_clear(void)
{
    g_orderUs = 0;   /* a seek, or another recording: times start over */
    while (g_qCount)
        queue_pop();
    g_qHead = 0;
}

static bool queue_full(u32 len) { return g_qCount == QUEUE_MAX || g_qBytes + len > QUEUE_BYTES; }

/* ------------------------------------------------------------------ seeking */

/* Keyframes seen so far (read ahead or scanned while seeking), per screen, in time order: a seek
   back, or a short one forward, lands right on one instead of guessing. */
typedef struct {
    u32 t;
    u64 off;
} Key;
static Key *g_keys[2];
static int g_nKeys[2];

static void add_key(int sc, u32 t, u64 off)
{
    const int n = g_nKeys[sc];
    if (g_keys[sc] && n < KEYS_MAX && (!n || t > g_keys[sc][n - 1].t))
        g_keys[sc][g_nKeys[sc]++] = (Key){t, off};
}

/* The last known keyframe at or before t, or NULL. */
static const Key *key_before(int sc, u32 t)
{
    int lo = 0, hi = g_nKeys[sc] - 1, best = -1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (g_keys[sc][mid].t <= t) {
            best = mid;
            lo = mid + 1;
        } else
            hi = mid - 1;
    }
    return best >= 0 ? &g_keys[sc][best] : NULL;
}

static bool seek_aborted(void) { return g_quit || g_seekTo >= 0; }

/* The first record at or after `off` that starts a chain of valid records (3, or 2 when the third
   would lie past the block read); its time in *timeUs. One read of FIND_BLOCK, searched in memory. */
static bool find_record(u64 off, u64 *found, u32 *timeUs)
{
    off = sizeof(R3dFileHeader) + ((off - sizeof(R3dFileHeader)) & ~15ull);
    u32 len = 0;
    if (off >= g_size || R_FAILED(FSFILE_Read(g_file, &len, off, g_buf, FIND_BLOCK)) || len < 64)
        return false;
    for (u32 p = 0; p + sizeof(R3dRecHeader) <= len; p += 16) {
        u32 q = p;
        int ok = 0;
        u32 prevT = 0;
        for (; ok < 3 && q + sizeof(R3dRecHeader) <= len; ok++) {
            const R3dRecHeader *h = (const R3dRecHeader *)(g_buf + q);
            if (!plausible(h))
                break;
            /* neighbours in the file are close in time (audio lags video by ~0.2 s) */
            if (ok && (h->timeUs > prevT + 2000000u || h->timeUs + 2000000u < prevT))
                break;
            prevT = h->timeUs;
            q += sizeof(R3dRecHeader) + stored_len(h);
        }
        if (ok == 3 || (ok == 2 && q + sizeof(R3dRecHeader) > len)) {
            *found = off + p;
            *timeUs = ((const R3dRecHeader *)(g_buf + p))->timeUs;
            return true;
        }
    }
    return false;
}

/* Where to start scanning for a seek to targetUs: a known top keyframe shortly before it, else a
   guess from the byte rate (from the nearest known point), corrected by the time found there. */
static u64 seek_landing(u32 targetUs)
{
    /* The scan forward from the landing reaches the target; landing after it would leave the
       player waiting in silence until the clock got there (O3DS 2026-11-05: 5-15 s frozen after
       seeks in a row). So the result is never later than the target: `lo` is the best point
       known at or before it, and each guess only replaces it when it is too. */
    const u32 aimUs = targetUs > SEEK_EARLY_US ? targetUs - SEEK_EARLY_US : 0;
    if (!aimUs)
        return sizeof(R3dFileHeader);
    const Key *k = key_before(0, targetUs);
    if (k && targetUs - k->t <= KEY_NEAR_US)
        return k->off;
    k = key_before(0, aimUs); /* the best known point before the aim, to guess from */
    u64 lo = sizeof(R3dFileHeader);
    u32 loT = 0;
    if (k) {
        lo = k->off;
        loT = k->t;
    }
    if (!g_durationMs)
        return lo;
    const double perUs = (double)(g_size - sizeof(R3dFileHeader)) / ((double)g_durationMs * 1000.0);
    double guess = (double)lo + perUs * ((double)aimUs - (double)loT);
    for (int i = 0; i < 4 && !seek_aborted(); i++) {
        if (guess < (double)lo)
            guess = (double)lo;
        if (guess > g_size - 64)
            guess = (double)(g_size - 64);
        u64 at;
        u32 t;
        if (!find_record((u64)guess, &at, &t))
            break;
        if (t <= targetUs && t >= loT) {
            lo = at;
            loT = t;
            if (aimUs - (t < aimUs ? t : aimUs) < SEEK_WINDOW_US)
                break; /* close before the aim: scan forward from here */
            guess = (double)at + perUs * ((double)aimUs - (double)t);
        } else if (t > targetUs) /* overshot: back by the overshoot and a second more */
            guess = (double)at - perUs * ((double)(t - aimUs) + 1000000.0);
        else
            break;
    }
    return lo;
}

/* Record headers while seeking, through a small read cache (sound records lie close together). */
static u64 g_hopOff;
static u32 g_hopLen;

static bool header_at(u64 off, R3dRecHeader *h)
{
    if (off < g_hopOff || off + sizeof(*h) > g_hopOff + g_hopLen) {
        g_hopOff = off;
        g_hopLen = 0;
        if (R_FAILED(FSFILE_Read(g_file, &g_hopLen, off, g_buf, HOP_BLOCK)) || g_hopLen < sizeof(*h))
            return false;
    }
    memcpy(h, g_buf + (off - g_hopOff), sizeof(*h));
    return true;
}

static void do_seek(u32 targetUs)
{
    pre_drop();   /* the reads below are this thread's own */
    clear_slots();
    queue_clear();
    audio_reset(targetUs); /* the old sound stops; the clock holds at the target meanwhile */
    g_haveKey[0] = g_haveKey[1] = false;
    const u64 from = seek_landing(targetUs);
    /* 1. walk the record headers up to the target: per screen, the frames since its last keyframe */
    static u64 chain[2][CHAIN_MAX];
    int n[2] = {0, 0};
    bool broken[2] = {false, false};
    g_hopLen = 0;
    u64 p = from;
    R3dRecHeader h;
    while (!seek_aborted() && p + sizeof(h) <= g_size && header_at(p, &h) && plausible(&h) && h.timeUs < targetUs) {
        if (h.magic != R3D_MAGIC_AUDIO && h.magic != R3D_MAGIC_PAD && h.magic != R3D_MAGIC_DSPMEM) {
            const int sc = h.screen & 0x3F;
            if (h.magic == R3D_MAGIC_FRAME || (h.screen & R3D_SCREEN_KEY)) {
                n[sc] = 0;
                broken[sc] = false;
                add_key(sc, h.timeUs, p);
            }
            if (n[sc] < CHAIN_MAX)
                chain[sc][n[sc]++] = p;
            else
                broken[sc] = true;
        }
        p += sizeof(h) + stored_len(&h);
    }
    if (seek_aborted())
        return;
    /* 2. decode each screen's frames from its keyframe; the last one is shown at once */
    for (int sc = 0; sc < 2 && !seek_aborted(); sc++) {
        if (broken[sc] || !n[sc])
            continue; /* no keyframe in range: this screen waits for its next one */
        for (int i = 0; i < n[sc] && !seek_aborted(); i++) {
            u32 got = 0;
            if (R_FAILED(FSFILE_Read(g_file, &got, chain[sc][i], g_buf, sizeof(h))) || got != sizeof(h))
                break;
            memcpy(&h, g_buf, sizeof(h));
            const u32 len = stored_len(&h);
            if (len > READ_BUF || R_FAILED(FSFILE_Read(g_file, &got, chain[sc][i] + sizeof(h), g_buf, len)) || got != len)
                break;
            video_frame(&h, g_buf, i == n[sc] - 1);
        }
    }
    if (seek_aborted())
        return;
    reposition(p);
    audio_reset(targetUs);
    LightLock_Lock(&g_lock);
    g_ended = false;
    LightLock_Unlock(&g_lock);
    g_seekBusy = g_seekTo >= 0;
}

/* ------------------------------------------------------------------ thread */

static void finish(void)
{
    if (g_audio && !g_eof) {
        audio_pad(g_lastVideoUs + AUDIO_PAD_LAG_US + 100000u); /* the last frames get their time */
        audio_flush();
    }
    g_eof = true;
}

/* The next due video record: decoded and queued for the screen. */
static bool decode_due(u32 now)
{
    if (!g_qCount)
        return false;
    Queued *q = &g_queue[g_qHead];
    if (q->h.timeUs > now + DUE_US)
        return false;
    const int qsc = q->h.screen & 0x3F;
    if (q->h.magic == R3D_MAGIC_CFRAME && qsc <= 1) {
        /* Behind: C420 frames are thinned evenly (every second one of the bottom screen, then of
           the top one too) instead of dropped in runs as they fall late. Back to all of them
           after THIN_CALM_US without a late frame. */
        if (g_thin && now > g_thinUs + THIN_CALM_US) {
            g_thin--;
            g_thinUs = now;
        }
        if ((g_thin >= 1 && qsc == 1) || g_thin >= 2) {
            if (++g_thinN[qsc] & 1u) {
                g_framesThinned++;
                queue_pop();
                return true;
            }
        }
    }
    if (now > q->h.timeUs + LATE_US) {
        if (g_thin < 2 && (!g_thin || now > g_thinUs + THIN_STEP_US))
            g_thin++;
        g_thinUs = now;
        /* The player is behind (an Old 3DS decoding both screens at 30 frames a second): a frame
           whose time has passed is not shown. A C420 frame is a picture on its own and the next
           XR32 frame after it is a keyframe, so it is not decoded either; any other frame is
           decoded for the ones after it. The sound went dry 355 times in a 40 s recording with
           every late frame decoded and shown (O3DS 0.12.0, 2026-11-11). */
        if (q->h.magic != R3D_MAGIC_CFRAME)
            video_frame(&q->h, q->data, false);
        g_framesLate++;
        queue_pop();
        return true;
    }
    if (!has_free_slot(q->h.screen & 0x3F))
        return false;
    video_frame(&q->h, q->data, true);
    queue_pop();
    return true;
}

/* Reads one record if it is within LEAD_US: sound to the DSP, video into the queue. */
static bool read_ahead(u32 now)
{
    if (g_eof)
        return false;
    if (!fill(sizeof(R3dRecHeader))) {
        finish();
        return false;
    }
    R3dRecHeader h;
    memcpy(&h, g_buf + g_pos, sizeof(h));
    if (!plausible(&h)) { /* R3DE, or the end of what a cut-off file holds */
        finish();
        return false;
    }
    const u32 len = stored_len(&h);
    if (h.magic == R3D_MAGIC_PAD || h.magic == R3D_MAGIC_DSPMEM) {
        skip(sizeof(h) + len);
        return true;
    }
    const bool video = h.magic != R3D_MAGIC_AUDIO;
    if (h.timeUs > now + LEAD_US || (video && queue_full(h.len))) {
        /* The sound is the clock: across a stretch with no records at all (the recorder skips
           unchanged frames) it must keep playing, or the clock never reaches the next record. */
        if (g_audio && h.timeUs > now + LEAD_US) {
            audio_pad(h.timeUs);
            if (audio_queued() < 2)
                audio_flush();
        }
        return false;
    }
    if (!fill(sizeof(h) + len)) {
        finish();
        return false;
    }
    const u8 *payload = g_buf + g_pos + sizeof(h);
    if (!video) {
        if (g_audio)
            audio_frame(&h, (const s16 *)payload);
    } else {
        if (g_audio)
            audio_pad(h.timeUs);
        if (h.timeUs > g_lastVideoUs)
            g_lastVideoUs = h.timeUs;
        if (h.magic == R3D_MAGIC_FRAME || (h.screen & R3D_SCREEN_KEY))
            add_key(h.screen & 0x3F, h.timeUs, g_bufOff + g_pos);
        if (h.timeUs < g_orderUs)
            h.timeUs = g_orderUs;   /* out of order in the file: due with the frames around it */
        else
            g_orderUs = h.timeUs;
        queue_push(&h, payload);
    }
    skip(sizeof(h) + len);
    return true;
}

static void check_end(void)
{
    LightLock_Lock(&g_lock);
    if (g_audio)
        audio_reap();
    bool left = g_qCount || (g_audio && g_queued);
    for (int sc = 0; sc < 2; sc++)
        for (int i = 0; i < SLOTS; i++)
            left = left || g_slots[sc][i].state == 1;
    if (!left)
        g_ended = true;
    LightLock_Unlock(&g_lock);
}

static void thread_main(void *arg)
{
    (void)arg;
    while (!g_quit) {
        const s64 seek = g_seekTo;
        if (seek >= 0) {
            g_seekTo = -1;
            do_seek((u32)seek);
            continue;
        }
        if (g_paused) {
            LightEvent_WaitTimeout(&g_wake, 20 * 1000 * 1000);
            continue;
        }
        const u32 now = clock_us();
        /* the sound first when little of it is queued: it is the clock, and a gap in it is heard */
        const bool hungry = g_audio && audio_queued() < AUDIO_LOW;
        if ((hungry && read_ahead(now)) || decode_due(now) || read_ahead(now))
            continue;
        if (g_eof)
            check_end();
        LightEvent_WaitTimeout(&g_wake, 4 * 1000 * 1000); /* a frame shown frees a slot: wakes early */
    }
}

/* ------------------------------------------------------------------ API */

bool player_start(const char *name, u32 durationMs)
{
    char path[96];
    snprintf(path, sizeof(path), REC_FILES_FS "/%s", name);
    if (R_FAILED(FSUSER_OpenFile(&g_file, recorder_archive(), fsMakePath(PATH_ASCII, path), FS_OPEN_READ, 0)))
        return false;
    FSFILE_GetSize(g_file, &g_size);
    u32 got = 0;
    if (R_FAILED(FSFILE_Read(g_file, &got, 0, &g_hdr, sizeof(g_hdr))) || got != sizeof(g_hdr) ||
        g_hdr.magic != R3D_MAGIC_FILE) {
        FSFILE_Close(g_file);
        g_file = 0;
        return false;
    }
    g_durationMs = durationMs;
    LightLock_Init(&g_lock);
    LightEvent_Init(&g_wake, RESET_ONESHOT);
    g_quit = g_paused = g_ended = g_eof = false;
    g_shown = 0;
    g_lastVideoUs = 0;
    g_orderUs = 0;
    g_seekTo = -1;
    g_buf = malloc(READ_BUF);
    g_pre = malloc(READ_BUF);
    g_preState = 0;
    LightEvent_Init(&g_preWake, RESET_ONESHOT);
    for (int sc = 0; sc < 2; sc++) {
        g_keys[sc] = malloc(KEYS_MAX * sizeof(Key));
        g_nKeys[sc] = 0;
    }
    g_seekBusy = false;
    for (int sc = 0; sc < 2; sc++) {
        g_cur[sc] = malloc(400 * 240 * 2);
        g_haveKey[sc] = false;
        for (int i = 0; i < SLOTS; i++)
            g_slots[sc][i] = (Slot){linearAlloc(PLAYER_TEX_W * 400 * 2), 0, 0};
    }
    bool ok = g_buf && g_pre && g_cur[0] && g_cur[1];
    for (int sc = 0; sc < 2; sc++)
        for (int i = 0; i < SLOTS; i++)
            ok = ok && g_slots[sc][i].px;
    if (!ok) {
        player_stop();
        return false;
    }

    /* sound: needs the DSP firmware (sdmc:/3ds/dspfirm.cdc, from DSP1); without it, video only */
    g_audio = false;
    g_framesShown = g_framesSkipped = g_framesLate = g_audioStarved = 0;
    g_thin = g_thinN[0] = g_thinN[1] = g_thinUs = g_framesThinned = 0;
    Result ndspRc = 0;
    g_ndspOk = g_hdr.audioRate && R_SUCCEEDED(ndspRc = ndspInit());
    if (g_hdr.audioRate && !g_ndspOk) /* most often no sdmc:/3ds/dspfirm.cdc */
        app_log("player: no sound, ndspInit %08lX", (unsigned long)ndspRc);
    if (g_ndspOk) {
        g_waveMem = linearAlloc(WAVE_BUFS * WAVE_SAMPLES * 4);
        if (g_waveMem) {
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            ndspChnReset(0);
            ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
            ndspChnSetRate(0, g_hdr.audioRate / 1000.0f);
            ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
            float mix[12] = {1.0f, 1.0f};
            ndspChnSetMix(0, mix);
            memset(g_wave, 0, sizeof(g_wave));
            for (int i = 0; i < WAVE_BUFS; i++)
                g_wave[i].data_pcm16 = g_waveMem + (size_t)i * WAVE_SAMPLES * 2;
            g_audio = true;
        }
    }
    g_muted = false;
    reposition(sizeof(R3dFileHeader));
    audio_reset(0);

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    /* above the decoder: it only asks the card for the next piece and sleeps until it is there */
    g_preThread = threadCreate(pre_main, NULL, 8 * 1024, prio, -2, false);
    g_thread = threadCreate(thread_main, NULL, 32 * 1024, prio + 1, -2, false);
    if (!g_thread) {
        player_stop();
        return false;
    }
    return true;
}

void player_stop(void)
{
    if (g_thread)
        app_log("player: %lu pictures shown, %lu skipped (screen behind), %lu late (player behind), %lu thinned, sound %s, "
                "sound ran dry %lu times",
                (unsigned long)g_framesShown, (unsigned long)g_framesSkipped, (unsigned long)g_framesLate,
                (unsigned long)g_framesThinned,
                g_ndspOk ? "on" : "off", (unsigned long)g_audioStarved);
    if (g_thread) {
        g_quit = true;
        LightEvent_Signal(&g_wake);
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
    }
    if (g_preThread) {
        g_quit = true;
        LightEvent_Signal(&g_preWake);
        threadJoin(g_preThread, U64_MAX);
        threadFree(g_preThread);
        g_preThread = NULL;
    }
    queue_clear();
    if (g_ndspOk) {
        ndspChnWaveBufClear(0);
        ndspExit();
        g_ndspOk = false;
    }
    g_audio = false;
    if (g_waveMem)
        linearFree(g_waveMem);
    g_waveMem = NULL;
    for (int sc = 0; sc < 2; sc++) {
        free(g_cur[sc]);
        g_cur[sc] = NULL;
        for (int i = 0; i < SLOTS; i++) {
            if (g_slots[sc][i].px)
                linearFree(g_slots[sc][i].px);
            g_slots[sc][i].px = NULL;
        }
    }
    free(g_buf);
    g_buf = NULL;
    free(g_pre);
    g_pre = NULL;
    for (int sc = 0; sc < 2; sc++) {
        free(g_keys[sc]);
        g_keys[sc] = NULL;
        g_nKeys[sc] = 0;
    }
    if (g_file)
        FSFILE_Close(g_file);
    g_file = 0;
}

void player_toggle_pause(void)
{
    if (g_ended) { /* at the end: play again from the start */
        player_seek(0);
        return;
    }
    LightLock_Lock(&g_lock);
    g_paused = !g_paused;
    if (g_ndspOk)
        ndspChnSetPaused(0, g_paused);
    if (g_paused)
        g_pausedAt = osGetTime();
    else
        g_wallStart += osGetTime() - g_pausedAt; /* the paused time does not count */
    LightLock_Unlock(&g_lock);
    LightEvent_Signal(&g_wake);
}

void player_seek(s64 targetMs)
{
    const s64 end = g_durationMs ? (s64)g_durationMs : 0;
    if (end && targetMs > end - 1000)
        targetMs = end - 1000;
    if (targetMs < 0)
        targetMs = 0;
    g_seekShowUs = (u32)(targetMs * 1000);
    g_seekBusy = true;
    g_seekTo = targetMs * 1000;
    LightLock_Lock(&g_lock);
    g_ended = false;
    LightLock_Unlock(&g_lock);
    g_eof = false;
    LightEvent_Signal(&g_wake);
}

void player_set_mute(bool mute)
{
    g_muted = mute;
    if (g_ndspOk) {
        float mix[12] = {mute ? 0.0f : 1.0f, mute ? 0.0f : 1.0f};
        ndspChnSetMix(0, mix);
    }
}

bool player_paused(void) { return g_paused; }
bool player_ended(void) { return g_ended; }
bool player_muted(void) { return g_muted; }
bool player_has_audio(void) { return g_audio; }
bool player_file_audio(void) { return g_hdr.audioRate != 0; }
bool player_both(void) { return g_hdr.mode == R3D_MODE_FULL_BOTH; }
bool player_bottom_only(void) { return g_hdr.mode == R3D_MODE_FULL_BOTTOM; }
u32 player_duration_ms(void) { return g_durationMs; }

u32 player_time_ms(void)
{
    /* while seeking: its target (so presses in a row add up); at the end: the last frame's time */
    const u32 t = (g_seekBusy ? g_seekShowUs : g_ended ? g_lastVideoUs : clock_us()) / 1000;
    return g_durationMs && t > g_durationMs ? g_durationMs : t;
}

int player_present(C3D_Tex *top, C3D_Tex *bottom)
{
    if (g_seekTo >= 0)
        return g_shown;
    const u32 now = clock_us();
    C3D_Tex *tex[2] = {top, bottom};
    LightLock_Lock(&g_lock);
    for (int sc = 0; sc < 2; sc++) {
        Slot *best = NULL;
        for (int i = 0; i < SLOTS; i++) {
            Slot *s = &g_slots[sc][i];
            if (s->state == 1 && s->timeUs <= now + 8000 && (!best || s->timeUs > best->timeUs))
                best = s;
        }
        if (!best)
            continue;
        for (int i = 0; i < SLOTS; i++) { /* older ones and the one on screen are done with */
            Slot *s = &g_slots[sc][i];
            if (s != best && (s->state == 2 || (s->state == 1 && s->timeUs < best->timeUs)))
                s->state = 0;
        }
        best->state = 2;
        const u32 cols = sc ? 320 : 400;
        C3D_SyncDisplayTransfer((u32 *)best->px, GX_BUFFER_DIM(PLAYER_TEX_W, cols), (u32 *)tex[sc]->data,
                                GX_BUFFER_DIM(PLAYER_TEX_W, cols),
                                GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |
                                    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) |
                                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
        g_shown |= 1 << sc;
    }
    LightLock_Unlock(&g_lock);
    LightEvent_Signal(&g_wake); /* a slot may be free now */
    return g_shown;
}
