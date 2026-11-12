/*
 * .r3d -> MP4 (H.264 + AAC-LC) with resume points, as a two-thread pipeline. See prep.h.
 *
 * Who owns what: the producer reads the recording and owns the screens, the YUV picture, the
 * audio chain and the AAC encoder; the consumer owns the H.264 encoder and the three output
 * files. They meet in a single-producer / single-consumer ring of items (a picture in one of
 * PIC_SLOTS buffers, an AAC frame, the end). A resume point needs both halves' state: the
 * producer takes it when the consumer has handled everything queued (then nothing moves).
 */
#include "prep.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_eq.h"
#include "audio_place.h"
#include "codec.h"
#include "huff.h"
#include "r3d_format.h"

#include "cmnMemory.h"
#include "voAAC.h"

#ifdef PREP_PLAIN_C   /* host builds: the same plain C path the console runs (no NEON / SSE) */
#undef __ARM_NEON
#undef __aarch64__
#undef __SSE2__
#endif
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

/* The video's bit rate and quantizer range. 1500 kbit/s with QP 20-34 gave 32.1 dB against the
   recording's own pictures in a Mario Kart 7 race and looked soft on a phone; 3500 with QP 18-32
   gives 34.6 dB for 1.6x the bytes and ~11% more encoder time (host, 2026-11-11). */
#ifndef PREP_VIDEO_KBPS
#define PREP_VIDEO_KBPS 3500
#define PREP_QP_MIN 18
#define PREP_QP_MAX 32
#endif
/*
 * The video's clock: milliseconds, and a frame's time is when the recorder captured it. Before,
 * frames sat on a grid of the recording's rate (30 a second), but the console captures every
 * second refresh of 59.83 Hz (29.92 a second) and the two screens at different moments, so on the
 * grid each screen repeated a frame every few seconds: the bottom screen looked short of frames
 * in the MP4 though the recording had them all (O3DS 0.12.1, 2026-11-11). A picture goes out
 * once COALESCE_MS have passed since the first screen changed, so both screens' frames of one
 * refresh pair share a picture (one video frame for the two, as before).
 */
#define GRID_HZ 1000u
#define COALESCE_MS(fps) (600u / (uint32_t)(fps))   /* 20 ms at 30 frames a second */
#define TOP_N (400 * 240)
#define BOT_N (320 * 240)
#define AAC_FRAME 1024              /* samples per channel in one AAC frame */
#define AAC_WARM 2                  /* frames fed again into a new encoder on resume */
#define AAC_OUT_MAX 6144            /* bytes of one stereo AAC frame at most (6144 bits per channel) */
#define OUT_RATE 32000              /* AAC sample rate (index 5) */
#define AAC_DELAY 1600              /* vo-aacenc: 1024 + 576 block-switch look-ahead (measured on host) */
#define SLOT_MAGIC 0x50455250u      /* "PREP" */
#define SLOT_VERSION 4u              /* 4: the video's clock in ms (GRID_HZ), State.dirtySince */
#define FTYP_SIZE 32u
#define MDAT_START (FTYP_SIZE + 8u) /* first sample byte */
#define TBL_AUDIO 0x80000000u
#define TBL_KEY 0x40000000u
#define TBL_DUR 0x3FFFFFFFu
#define RING 64                     /* queued items (a picture is one; ~1.3 AAC frames per picture) */
#define PIC_SLOTS 3                 /* pictures in flight */
#define SRC_BUF (256u << 10)        /* stdio buffers: the SD card wants few, large requests */
#define OUT_BUF (256u << 10)        /* the MP4's bytes go out in pieces this large */
#define TBL_BUF (32u << 10)

/* Everything a resume point holds besides the two screens' pixels. Plain data only.
   [P] producer's, [C] consumer's; read together only when the queue is empty. */
typedef struct {
    uint32_t magic, version, seq, crc;
    uint64_t srcSize, srcOffset;        /* [P] srcOffset: the record to read next */
    uint64_t emitted;                   /* [P] clock ticks (GRID_HZ) done */
    uint64_t dirtySince;                /* [P] the tick the picture first changed at since the last frame */
    uint64_t pendTick;                  /* [C] tick of the last video frame (its length is not known yet) */
    uint32_t partSize, tblCount;        /* [C] */
    uint32_t pendOffset, pendSize, pendKey, havePending; /* [C] */
    uint32_t videoFrames, audioFrames, dirty, lastUs;    /* [P] (videoFrames: pictures queued) */
    AudioPlacer ap;                     /* [P] audio chain from here on */
    AudioEq eq;
    uint32_t haveLast;
    int16_t lastPcm[320];               /* previous placed audio frame (joins, conceal) */
    double rsPos;                       /* resampler: next output, in input samples after rsHist[1] */
    float rsHist[4][2];
    uint32_t aacFill, aacHistFill;
    int16_t aacBuf[AAC_FRAME * 2];
    int16_t aacHist[AAC_WARM * AAC_FRAME * 2];
    uint32_t spsLen, ppsLen;            /* [C] */
    uint8_t sps[64], pps[64];
    uint8_t seen[2], needKey[2], pad[4]; /* [P] */
} State;

/* a slot: State, the two screens' byte counts, then both screens as XR32 keyframes (the
   recorder's own codec: 345 KB of pixels are ~100-200 KB, and the SD card writes slowly) */
#define SLOT_BYTES ((sizeof(State) + 8 + codec_xr32_max_size(TOP_N) + codec_xr32_max_size(BOT_N) + 4095) & ~(size_t)4095)

enum { IT_PIC, IT_AAC, IT_END };
typedef struct {
    uint8_t type;
    uint8_t slot;       /* IT_PIC: picture buffer */
    uint32_t len;       /* IT_AAC: bytes in data */
    uint64_t tick;      /* IT_PIC: grid tick; IT_END: the end tick */
    uint8_t *data;      /* IT_AAC: malloc()ed, freed by the consumer */
} Item;

struct PrepJob {
    PrepOptions opt;
    char base[256], err[128];
    FILE *src, *part, *tbl, *prep;
    char *srcBuf, *tblBuf;
    R3dFileHeader fh;
    int both, bottomOnly, H, fps, audio, skipCk, finished;
    double inRate, rsStep;
    uint32_t endUs, haveEnd;
    State s;
    /* producer */
    uint16_t *px[2];
    uint8_t *payload, *yuv;             /* yuv: the picture as it is now (updated a screen at a time) */
    VO_AUDIO_CODECAPI aapi;
    VO_HANDLE ah;
    VO_MEM_OPERATOR mop;
    uint32_t aacDiscard;
    uint8_t aacOut[AAC_OUT_MAX];
    /* the queue */
    Item ring[RING];
    volatile uint32_t head, tail;       /* producer writes head, consumer tail */
    volatile uint32_t produced, consumed; /* items queued / handled (handled = done with) */
    uint8_t *pic[PIC_SLOTS];
    volatile uint8_t picBusy[PIC_SLOTS];
    volatile int abort, producerDone;
    /* consumer */
    uint8_t *nal;
    size_t nalCap;
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    H264E_run_param_t run;
    uint64_t waitSince;
    /* The MP4's bytes: the consumer fills one buffer while the producer writes the other to the
       SD card (writing waits on the card, not on a core: the encoder goes on meanwhile). */
    uint8_t *obuf[2];
    uint32_t olen[2];
    volatile uint8_t ofull[2];
    int ocur;                           /* [C] being filled */
    int onext;                          /* [W] the next to write */
    volatile int producerExited;
    volatile int flushReq;              /* producer / consumer: write out everything (partial too) */
    volatile int consumerDone, writerExited;
    uint32_t finalSize;                 /* the MP4's length once the index is written */
    PrepStats st;
};

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

/* ------------------------------------------------------------------ small helpers */

void prep_path(char *out, size_t n, const char *base, const char *ext)
{
    snprintf(out, n, "%s.%s", base, ext);
}

static uint64_t now_us(const PrepJob *j)
{
    return j->opt.nowUs ? j->opt.nowUs() : 0;
}

static double ms_since(const PrepJob *j, uint64_t t0)
{
    return (now_us(j) - t0) / 1000.0;
}

static int fail(PrepJob *j, const char *what)
{
    if (!j->err[0])
        snprintf(j->err, sizeof(j->err), "%s", what);
    return PREP_ERROR;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t n)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}


static int put(PrepJob *j, FILE *f, const void *data, size_t n)
{
    return fwrite(data, 1, n, f) == n ? 0 : fail(j, "write failed (SD card full?)");
}

static int stopped(const PrepJob *j)
{
    return j->abort || j->err[0];
}

/* ------------------------------------------------------------------ the queue */

static int flush_full(PrepJob *j);
static int request_flush(PrepJob *j);

/* [P] Adds an item, waiting while the ring is full. */
static int push(PrepJob *j, const Item *it)
{
    const uint64_t t0 = now_us(j);
    while (LOAD(j->head) - LOAD(j->tail) >= RING) {
        if (stopped(j))
            return PREP_ERROR;
        j->opt.sleepUs(1000);
    }
    j->st.produceWaitMs += ms_since(j, t0);
    const uint32_t h = j->head;
    j->ring[h % RING] = *it;
    STORE(j->head, h + 1);
    j->produced++;
    return 0;
}

/* [P] Waits until the consumer has handled everything queued. */
static int wait_drained(PrepJob *j)
{
    const uint64_t t0 = now_us(j);
    while (LOAD(j->consumed) != j->produced) {
        if (stopped(j))
            return PREP_ERROR;
        j->opt.sleepUs(1000);
    }
    j->st.produceWaitMs += ms_since(j, t0);
    return 0;
}

/* [P] A free picture buffer. */
static int free_slot(PrepJob *j)
{
    const uint64_t t0 = now_us(j);
    for (;;) {
        for (int i = 0; i < PIC_SLOTS; i++)
            if (!LOAD(j->picBusy[i])) {
                j->st.produceWaitMs += ms_since(j, t0);
                return i;
            }
        if (stopped(j))
            return -1;
        j->opt.sleepUs(1000);
    }
}

/* ------------------------------------------------------------------ picture -> YUV 4:2:0 */

/* BT.601 limited range, as tests/encbench; RGB565 -> 8 bit as r3dconv (x * 255 / 31). */
static int32_t g_yr[32], g_yg[64], g_yb[32], g_ur[32], g_ug[64], g_ub[32], g_vr[32], g_vg[64], g_vb[32];

static void yuv_tables(void)
{
    for (int i = 0; i < 64; i++) {
        const int g = i * 255 / 63;
        g_yg[i] = 129 * g, g_ug[i] = -74 * g, g_vg[i] = -94 * g;
        if (i < 32) {
            const int c = i * 255 / 31;
            g_yr[i] = 66 * c, g_ur[i] = -38 * c, g_vr[i] = 112 * c;
            g_yb[i] = 25 * c, g_ub[i] = 112 * c, g_vb[i] = -18 * c;
        }
    }
}

#define Y_OF(p) ((uint8_t)((g_yr[(p) >> 11] + g_yg[((p) >> 5) & 63] + g_yb[(p) & 31] + 128 + 4096) >> 8))

/* the screens in the video: both (400 x 480), the top or the bottom one (400 x 240) */
#define SCREEN_SHOWN(j, sc) ((sc) == 0 ? !(j)->bottomOnly : ((j)->both || (j)->bottomOnly))

/* One screen (column-major, word 0 = bottom row, 240 rows) into the picture at (ox, oy). */
static void yuv_screen(const PrepJob *j, int sc)
{
    const int W = 400, cols = sc ? 320 : 400, ox = sc ? 40 : 0, oy = sc && !j->bottomOnly ? 240 : 0;
    uint8_t *Y = j->yuv, *U = Y + W * j->H, *V = U + W * j->H / 4;
    const uint16_t *px = j->px[sc];
    for (int y = 0; y < 240; y += 2) {
        uint8_t *y0 = Y + (oy + y) * W + ox, *y1 = y0 + W;
        uint8_t *u = U + ((oy + y) / 2) * (W / 2) + ox / 2, *v = V + ((oy + y) / 2) * (W / 2) + ox / 2;
        const uint16_t *c = px + (239 - y);   /* column x: c[0] = row y, c[-1] = row y + 1 */
        for (int x = 0; x < cols; x += 2, c += 480) {
            const uint32_t a = c[0], b = c[-1], d = c[240], e = c[239];
            y0[x] = Y_OF(a), y0[x + 1] = Y_OF(d), y1[x] = Y_OF(b), y1[x + 1] = Y_OF(e);
            const uint32_t r0 = a >> 11, r1 = b >> 11, r2 = d >> 11, r3 = e >> 11;
            const uint32_t g0 = (a >> 5) & 63, g1 = (b >> 5) & 63, g2 = (d >> 5) & 63, g3 = (e >> 5) & 63;
            const uint32_t b0 = a & 31, b1 = b & 31, b2 = d & 31, b3 = e & 31;
            const int32_t su = g_ur[r0] + g_ur[r1] + g_ur[r2] + g_ur[r3] + g_ug[g0] + g_ug[g1] + g_ug[g2] +
                               g_ug[g3] + g_ub[b0] + g_ub[b1] + g_ub[b2] + g_ub[b3];
            const int32_t sv = g_vr[r0] + g_vr[r1] + g_vr[r2] + g_vr[r3] + g_vg[g0] + g_vg[g1] + g_vg[g2] +
                               g_vg[g3] + g_vb[b0] + g_vb[b1] + g_vb[b2] + g_vb[b3];
            u[x / 2] = (uint8_t)(((su >> 2) + 128 + 32768) >> 8);
            v[x / 2] = (uint8_t)(((sv >> 2) + 128 + 32768) >> 8);
        }
    }
}

/* ------------------------------------------------------------------ [C] output */

static int tbl_add(PrepJob *j, uint32_t offset, uint32_t size, uint32_t info)
{
    const uint32_t e[3] = {offset, size, info};
    if (put(j, j->tbl, e, sizeof(e)))
        return PREP_ERROR;
    j->s.tblCount++;
    return 0;
}

/* [W] Writes the full output buffers to the file, in order. */
static int flush_full(PrepJob *j)
{
    while (LOAD(j->ofull[j->onext])) {
        const uint64_t t0 = now_us(j);
        const int i = j->onext;
        if (put(j, j->part, j->obuf[i], j->olen[i]))
            return PREP_ERROR;
        j->olen[i] = 0;
        STORE(j->ofull[i], 0);
        j->onext ^= 1;
        j->st.diskMs += ms_since(j, t0);
    }
    return 0;
}

/* [W] with nothing being appended: everything so far into the file. */
static int flush_all(PrepJob *j)
{
    if (flush_full(j))
        return PREP_ERROR;
    const int i = j->ocur;   /* == onext now */
    if (j->olen[i] && put(j, j->part, j->obuf[i], j->olen[i]))
        return PREP_ERROR;
    j->olen[i] = 0;
    return fflush(j->part) ? fail(j, "write failed (SD card full?)") : 0;
}

/* [C] appends to the MP4 (through the output buffers). */
static int sample_write(PrepJob *j, const void *data, size_t n, uint32_t *offset)
{
    *offset = j->s.partSize;
    j->s.partSize += (uint32_t)n;
    const uint8_t *p = data;
    while (n) {
        while (LOAD(j->ofull[j->ocur])) { /* both full: the card is behind */
            if (stopped(j) || LOAD(j->writerExited))
                return fail(j, "stopped");
            j->opt.sleepUs(1000);
        }
        const int c = j->ocur;
        const size_t k = n < OUT_BUF - j->olen[c] ? n : OUT_BUF - j->olen[c];
        memcpy(j->obuf[c] + j->olen[c], p, k);
        j->olen[c] += (uint32_t)k;
        p += k;
        n -= k;
        if (j->olen[c] == OUT_BUF) {
            STORE(j->ofull[c], 1);
            j->ocur ^= 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ [P] audio */

static int aac_open(PrepJob *j)
{
    voGetAACEncAPI(&j->aapi);
    j->mop.Alloc = cmnMemAlloc;
    j->mop.Copy = cmnMemCopy;
    j->mop.Free = cmnMemFree;
    j->mop.Set = cmnMemSet;
    j->mop.Check = cmnMemCheck;
    VO_CODEC_INIT_USERDATA ud;
    memset(&ud, 0, sizeof(ud));
    ud.memflag = VO_IMF_USERMEMOPERATOR;
    ud.memData = &j->mop;
    if (j->aapi.Init(&j->ah, VO_AUDIO_CodingAAC, &ud) != VO_ERR_NONE) {
        j->ah = NULL;
        return fail(j, "AAC encoder init failed");
    }
    AACENC_PARAM p;
    memset(&p, 0, sizeof(p));
    p.sampleRate = OUT_RATE;
    p.bitRate = (j->opt.audioKbps ? j->opt.audioKbps : 96) * 1000;
    p.nChannels = 2;
    p.adtsUsed = 0;
    if (j->aapi.SetParam(j->ah, VO_PID_AAC_ENCPARAM, &p) != VO_ERR_NONE)
        return fail(j, "AAC encoder settings refused");
    return 0;
}

/* One AAC frame of input (1024 stereo samples) through the encoder into the queue. */
static int aac_encode(PrepJob *j, int16_t *pcm)
{
    const uint64_t t0 = now_us(j);
    VO_CODECBUFFER in = {(VO_PBYTE)pcm, AAC_FRAME * 4, 0};
    if (j->aapi.SetInputData(j->ah, &in) != VO_ERR_NONE)
        return fail(j, "AAC input refused");
    for (;;) {
        VO_CODECBUFFER out = {j->aacOut, sizeof(j->aacOut), 0};
        VO_AUDIO_OUTPUTINFO info;
        memset(&info, 0, sizeof(info));
        const VO_U32 rc = j->aapi.GetOutputData(j->ah, &out, &info);
        if (rc == VO_ERR_INPUT_BUFFER_SMALL)
            break;
        if (rc != VO_ERR_NONE)
            return fail(j, "AAC encoding failed");
        if (!out.Length)
            continue;
        if (j->aacDiscard) {   /* warm-up after a resume: already in the file */
            j->aacDiscard--;
            continue;
        }
        Item it = {IT_AAC, 0, out.Length, 0, malloc(out.Length)};
        if (!it.data)
            return fail(j, "out of memory (sound)");
        memcpy(it.data, j->aacOut, out.Length);
        if (push(j, &it)) {
            free(it.data);
            return PREP_ERROR;
        }
        j->s.audioFrames++;
        j->st.audioFrames++;
    }
    j->st.audioMs += ms_since(j, t0);
    return 0;
}

static int aac_emit(PrepJob *j, float l, float r)
{
    State *s = &j->s;
    l = l > 32767.f ? 32767.f : l < -32768.f ? -32768.f : l;
    r = r > 32767.f ? 32767.f : r < -32768.f ? -32768.f : r;
    s->aacBuf[2 * s->aacFill] = (int16_t)(l < 0 ? l - 0.5f : l + 0.5f);
    s->aacBuf[2 * s->aacFill + 1] = (int16_t)(r < 0 ? r - 0.5f : r + 0.5f);
    if (++s->aacFill < AAC_FRAME)
        return 0;
    s->aacFill = 0;
    /* keep the last AAC_WARM frames for a resume */
    memmove(s->aacHist, s->aacHist + AAC_FRAME * 2, sizeof(s->aacHist) - AAC_FRAME * 4);
    memcpy(s->aacHist + (AAC_WARM - 1) * AAC_FRAME * 2, s->aacBuf, AAC_FRAME * 4);
    if (s->aacHistFill < AAC_WARM)
        s->aacHistFill++;
    int16_t frame[AAC_FRAME * 2];
    memcpy(frame, s->aacBuf, sizeof(frame));
    return aac_encode(j, frame);
}

/* Catmull-Rom resampler, recording rate (~32728 Hz) -> 32000 Hz. */
static int resample_in(PrepJob *j, float l, float r)
{
    State *s = &j->s;
    memmove(s->rsHist[0], s->rsHist[1], sizeof(s->rsHist[0]) * 3);
    s->rsHist[3][0] = l;
    s->rsHist[3][1] = r;
    while (s->rsPos < 1.0) {
        const float t = (float)s->rsPos;
        float o[2];
        for (int ch = 0; ch < 2; ch++) {
            const float p0 = s->rsHist[0][ch], p1 = s->rsHist[1][ch], p2 = s->rsHist[2][ch], p3 = s->rsHist[3][ch];
            o[ch] = p1 + 0.5f * t * (p2 - p0 + t * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3 + t * (3.f * (p1 - p2) + p3 - p0)));
        }
        if (aac_emit(j, o[0], o[1]))
            return PREP_ERROR;
        s->rsPos += j->rsStep;
    }
    s->rsPos -= 1.0;
    return 0;
}

/* `n` stereo samples at the recording rate (NULL: silence): EQ (as the player / r3dconv), then
   on. The filter is audio_eq's, run in floats (the ARM11's VFP does them several times faster
   than doubles; 24 bits are plenty for 16-bit sound) and without its per-sample rounding. */
static int audio_push(PrepJob *j, const int16_t *pcm, uint32_t n)
{
    AudioEq *e = &j->s.eq;
    const uint64_t t0 = now_us(j);
    const float b0 = (float)e->b0, b1 = (float)e->b1, b2 = (float)e->b2, a1 = (float)e->a1, a2 = (float)e->a2;
    float z1[2] = {(float)e->z1[0], (float)e->z1[1]}, z2[2] = {(float)e->z2[0], (float)e->z2[1]};
    int r = 0;
    for (uint32_t i = 0; i < n && !r; i++) {
        float o[2];
        for (int ch = 0; ch < 2; ch++) {
            const float x = pcm ? pcm[2 * i + ch] : 0.f;   /* silence goes through the filter too (its tail) */
            const float y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            o[ch] = y;
        }
        r = resample_in(j, o[0], o[1]);
    }
    e->z1[0] = z1[0], e->z1[1] = z1[1], e->z2[0] = z2[0], e->z2[1] = z2[1];
    j->st.pcmMs += ms_since(j, t0);
    return r ? PREP_ERROR : 0;
}

/* One R3DA frame (160 samples), placed on the timeline as r3dconv does. */
static int audio_frame(PrepJob *j, uint32_t timeUs, const int16_t *frame)
{
    State *s = &j->s;
    const int harsh = s->haveLast && audio_join_harsh(s->lastPcm, frame, 160);
    const int64_t silence = audio_place(&s->ap, timeUs, 160, harsh);
    if (silence < 0)
        return 0;
    if (silence > 0) { /* soft edges around silence (audio_place.h) */
        uint32_t t = 0;
        if (s->haveLast) {
            int16_t tail[320];
            t = audio_tail(s->lastPcm, 160, (uint64_t)silence, tail);
            if (audio_push(j, tail, t))
                return PREP_ERROR;
        }
        if ((uint64_t)silence > t && audio_push(j, NULL, (uint32_t)silence - t))
            return PREP_ERROR;
    }
    if (s->ap.conceal) {
        int16_t fill[AUDIO_CONCEAL_MAX * 320];
        audio_conceal(s->haveLast ? s->lastPcm : frame, frame, 160, s->ap.conceal, fill);
        if (audio_push(j, fill, s->ap.conceal * 160))
            return PREP_ERROR;
    }
    if (silence > 0) {
        int16_t faded[320];
        memcpy(faded, frame, sizeof(faded));
        audio_fade_in(faded, 160);
        if (audio_push(j, faded, 160))
            return PREP_ERROR;
    } else if (audio_push(j, frame, 160))
        return PREP_ERROR;
    memcpy(s->lastPcm, frame, sizeof(s->lastPcm));
    s->haveLast = 1;
    return 0;
}

/* ------------------------------------------------------------------ resume points */

static uint32_t slot_crc(const State *s, const uint32_t *lens, const uint8_t *data)
{
    uint32_t c = crc32_update(0, (const uint8_t *)s + 16, sizeof(*s) - 16);
    c = crc32_update(c, lens, 8);
    return crc32_update(c, data, (size_t)lens[0] + lens[1]);
}

/* [P] with the queue empty: both halves stand still. */
static int checkpoint(PrepJob *j, uint64_t recStart)
{
    const uint64_t t0 = now_us(j);
    State *s = &j->s;
    if (request_flush(j) || fflush(j->tbl))
        return fail(j, "write failed (SD card full?)");
    uint16_t *scratch = malloc(TOP_N * 2);
    uint8_t *data = malloc(codec_xr32_max_size(TOP_N) + codec_xr32_max_size(BOT_N));
    if (!scratch || !data) {
        free(scratch);
        free(data);
        return fail(j, "out of memory (resume point)");
    }
    uint32_t lens[2];
    lens[0] = (uint32_t)codec_xr32_encode_strided(scratch, (const uint8_t *)j->px[0], 400, 240, 480, 1, 1, data);
    lens[1] = (uint32_t)codec_xr32_encode_strided(scratch, (const uint8_t *)j->px[1], 320, 240, 480, 1, 1, data + lens[0]);
    s->srcOffset = recStart;
    s->seq++;
    s->magic = SLOT_MAGIC;
    s->version = SLOT_VERSION;
    s->crc = slot_crc(s, lens, data);
    int bad = fseek(j->prep, (long)((s->seq & 1) * SLOT_BYTES), SEEK_SET) || put(j, j->prep, s, sizeof(*s)) ||
              put(j, j->prep, lens, 8) || put(j, j->prep, data, (size_t)lens[0] + lens[1]) || fflush(j->prep);
    free(scratch);
    free(data);
    if (bad)
        return fail(j, "write failed (SD card full?)");
    j->st.checkpointMs += ms_since(j, t0);
    return 0;
}

/* The newest valid slot of base.prep into s / px (px may be NULL: state only). */
static int load_slot(FILE *f, State *s, uint16_t *top, uint16_t *bot)
{
    int best = -1;
    uint32_t bestSeq = 0;
    State t;
    const size_t cap = codec_xr32_max_size(TOP_N) + codec_xr32_max_size(BOT_N);
    uint8_t *data = malloc(cap);
    if (!data)
        return -1;
    for (int i = 0; i < 2; i++) {
        uint32_t lens[2];
        if (fseek(f, (long)(i * SLOT_BYTES), SEEK_SET) || fread(&t, 1, sizeof(t), f) != sizeof(t) ||
            t.magic != SLOT_MAGIC || t.version != SLOT_VERSION || fread(lens, 1, 8, f) != 8 ||
            (size_t)lens[0] + lens[1] > cap || fread(data, 1, (size_t)lens[0] + lens[1], f) != (size_t)lens[0] + lens[1] ||
            slot_crc(&t, lens, data) != t.crc)
            continue;
        if (best >= 0 && t.seq <= bestSeq)
            continue;
        if ((top && codec_xr32_decode(data, lens[0], top, TOP_N, 1)) ||
            (bot && codec_xr32_decode(data + lens[0], lens[1], bot, BOT_N, 1)))
            continue;
        best = i;
        bestSeq = t.seq;
        *s = t;
    }
    free(data);
    return best;
}

int prep_saved_permille(const char *base)
{
    char path[300];
    prep_path(path, sizeof(path), base, "prep");
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    State *s = malloc(sizeof(State));
    int r = -1;
    if (s && load_slot(f, s, NULL, NULL) >= 0 && s->srcSize)
        r = (int)(s->srcOffset * 1000 / s->srcSize);
    free(s);
    fclose(f);
    return r;
}

/* ------------------------------------------------------------------ [P] video */

/* The picture as it is now becomes the video frame of grid tick `tick`. */
static int produce_picture(PrepJob *j, uint64_t tick, uint64_t recStart)
{
    State *s = &j->s;
    if (s->videoFrames % PREP_GOP == 0 && s->videoFrames && !j->skipCk)
        if (wait_drained(j) || checkpoint(j, recStart))
            return PREP_ERROR;
    j->skipCk = 0;
    const uint64_t t0 = now_us(j);
    for (int sc = 0; sc < 2; sc++)
        if (s->dirty & (1u << sc) && SCREEN_SHOWN(j, sc))
            yuv_screen(j, sc);
    s->dirty = 0;
    j->st.yuvMs += ms_since(j, t0);
    const int slot = free_slot(j);
    if (slot < 0)
        return PREP_ERROR;
    memcpy(j->pic[slot], j->yuv, 400 * (size_t)j->H * 3 / 2);
    STORE(j->picBusy[slot], 1);
    const Item it = {IT_PIC, (uint8_t)slot, 0, tick, NULL};
    if (push(j, &it))
        return PREP_ERROR;
    s->videoFrames++;
    return 0;
}

/* Grid ticks before `timeUs` show the picture as it is: a frame where it changed. */
static int video_until(PrepJob *j, uint32_t timeUs, uint64_t recStart)
{
    State *s = &j->s;
    while (s->emitted * (1000000ull / GRID_HZ) < timeUs) {
        if (s->dirty && s->emitted >= s->dirtySince + COALESCE_MS(j->fps) && produce_picture(j, s->dirtySince, recStart))
            return PREP_ERROR;
        s->emitted++;
    }
    return 0;
}

/* [P] the end of the recording: the last pictures, the sound's tail, then IT_END. */
static int produce_end(PrepJob *j)
{
    State *s = &j->s;
    if (!j->haveEnd)
        j->endUs = s->lastUs;
    if (video_until(j, j->endUs, s->srcOffset) || (s->dirty && produce_picture(j, s->dirtySince, s->srcOffset)))
        return PREP_ERROR;
    if (j->audio) {
        if (audio_push(j, NULL, (uint32_t)audio_pad_to(&s->ap, j->endUs)))
            return PREP_ERROR;
        /* the last partial frame, then one of silence for the encoder's delay */
        const uint32_t pad = (AAC_FRAME - s->aacFill) % AAC_FRAME + AAC_FRAME;
        for (uint32_t i = 0; i < pad; i++)
            if (aac_emit(j, 0.f, 0.f))
                return PREP_ERROR;
    }
    const Item it = {IT_END, 0, 0, ((uint64_t)j->endUs * GRID_HZ + 999999) / 1000000, NULL};
    if (push(j, &it))
        return PREP_ERROR;
    j->producerDone = 1;
    return PREP_DONE;
}

static int produce(PrepJob *j);

int prep_produce(PrepJob *j)
{
    const int r = produce(j);
    STORE(j->producerExited, 1);
    return r;
}

/* Asks the writer to put everything appended so far into the file, and waits for it. */
static int request_flush(PrepJob *j)
{
    STORE(j->flushReq, 1);
    while (LOAD(j->flushReq)) {
        if (j->abort || LOAD(j->writerExited))
            return fail(j, "stopped");
        j->opt.sleepUs(1000);
    }
    return j->err[0] ? PREP_ERROR : 0;
}

int prep_write(PrepJob *j)
{
    int r = 0;
    while (!r) {
        r = flush_full(j);
        if (!r && LOAD(j->flushReq)) {
            r = flush_all(j);
            STORE(j->flushReq, 0);
        }
        if (j->abort || j->err[0] || LOAD(j->consumerDone))
            break;
        j->opt.sleepUs(2000);
    }
    STORE(j->writerExited, 1);
    return r ? PREP_ERROR : 0;
}

static int produce(PrepJob *j)
{
    State *s = &j->s;
    while (!stopped(j)) {
        const uint64_t recStart = s->srcOffset;
        uint64_t t0 = now_us(j);
        R3dRecHeader rh;
        if (fread(&rh, 1, sizeof(rh), j->src) != sizeof(rh))
            return produce_end(j);
        if (rh.magic == R3D_MAGIC_PAD || rh.magic == R3D_MAGIC_DSPMEM) {
            if (fseek(j->src, (long)rh.len, SEEK_CUR))
                return produce_end(j);
            s->srcOffset += sizeof(rh) + rh.len;
            j->st.readMs += ms_since(j, t0);
            continue;
        }
        if (rh.magic == R3D_MAGIC_END) {
            j->endUs = rh.timeUs;
            j->haveEnd = 1;
            return produce_end(j);
        }
        if (rh.magic == R3D_MAGIC_AUDIO) {
            const uint32_t bytes = (uint32_t)rh.cols * 4u;
            if (rh.len < bytes || rh.len > 4096 || fread(j->payload, 1, rh.len, j->src) != rh.len)
                return produce_end(j);
            s->srcOffset += sizeof(rh) + rh.len;
            j->st.readMs += ms_since(j, t0);
            if (rh.len >= bytes + sizeof(R3dAudioTrailer)) {
                const R3dAudioTrailer *tr = (const R3dAudioTrailer *)(j->payload + bytes);
                if (tr->flags & R3D_AUDIO_RAW)
                    continue;
            }
            s->lastUs = rh.timeUs > s->lastUs ? rh.timeUs : s->lastUs;
            if (rh.cols != 160 || !j->audio)
                continue;
            int16_t frame[320];
            memcpy(frame, j->payload, sizeof(frame));
            if (audio_frame(j, rh.timeUs, frame))
                return PREP_ERROR;
            continue;
        }
        const int delta = rh.magic == R3D_MAGIC_XFRAME || rh.magic == R3D_MAGIC_YFRAME || rh.magic == R3D_MAGIC_HFRAME ||
                          rh.magic == R3D_MAGIC_QFRAME || rh.magic == R3D_MAGIC_CFRAME;
        const uint32_t sc = rh.screen & 0x3F;
        if ((rh.magic != R3D_MAGIC_FRAME && !delta) || sc > 1 || rh.len > codec_xr32_max_size(TOP_N) + 16)
            return produce_end(j);   /* end of valid data */
        const uint32_t stored = delta ? ((rh.len + 15) & ~15u) : rh.len;
        if (fread(j->payload, 1, stored, j->src) != stored)
            return produce_end(j);
        j->st.readMs += ms_since(j, t0);
        /* frames of the grid before this record show the pictures so far */
        if (video_until(j, rh.timeUs, recStart))
            return PREP_ERROR;
        s->srcOffset += sizeof(rh) + stored;
        s->lastUs = rh.timeUs > s->lastUs ? rh.timeUs : s->lastUs;
        const uint32_t cols = sc ? 320 : 400, n = sc ? BOT_N : TOP_N;
        if (rh.cols != cols || !SCREEN_SHOWN(j, sc))
            continue;
        const int key = (rh.screen & R3D_SCREEN_KEY) != 0;
        t0 = now_us(j);
        int bad = 0;
        if (rh.magic == R3D_MAGIC_FRAME)
            memcpy(j->px[sc], j->payload, n * 2);
        else if (!key && s->needKey[sc])
            continue;
        else if (rh.magic == R3D_MAGIC_YFRAME)
            bad = codec_xr32_decode(j->payload, rh.len, j->px[sc], n, key);
        else if (rh.magic == R3D_MAGIC_QFRAME)
            bad = codec_q565_decode(j->payload, rh.len, j->px[sc], n);
        else if (rh.magic == R3D_MAGIC_CFRAME)
            bad = codec_c420_decode(j->payload, rh.len, j->px[sc], cols, 240);
        else if (rh.magic == R3D_MAGIC_HFRAME) {
            static HuffTables tables;   /* one conversion at a time */
            bad = huff_xr32_decode(j->payload, rh.len, j->px[sc], n, key, &tables);
        }
        else
            bad = codec_xrle_decode(j->payload, rh.len, j->px[sc], n, key);
        j->st.decodeMs += ms_since(j, t0);
        if (bad) {
            s->needKey[sc] = 1;
            continue;
        }
        s->needKey[sc] = 0;
        s->seen[sc] = 1;
        if (!s->dirty)
            s->dirtySince = s->emitted;
        s->dirty |= 1u << sc;
    }
    return PREP_ERROR;
}

void prep_abort(PrepJob *j)
{
    j->abort = 1;
}

/* ------------------------------------------------------------------ [C] video */

static int video_open(PrepJob *j)
{
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.width = 400;
    cp.height = j->H;
    cp.gop = PREP_GOP;
    cp.const_input_flag = 1;   /* the picture buffer goes back to the producer as it was */
    cp.max_threads = 1;        /* the other core makes the pictures */
    int sp = 0, ss = 0;
    if (H264E_sizeof(&cp, &sp, &ss))
        return fail(j, "H.264 encoder: bad size");
    j->enc = malloc((size_t)sp);
    j->scratch = malloc((size_t)ss);
    if (!j->enc || !j->scratch)
        return fail(j, "out of memory (H.264 encoder)");
    if (H264E_init(j->enc, &cp))
        return fail(j, "H.264 encoder init failed");
    memset(&j->run, 0, sizeof(j->run));
    j->run.encode_speed = 10;  /* 9 without the deblocking filter: 17% faster, SSIM -0.003 (Mac, 2026-11-06) */
    j->run.qp_min = PREP_QP_MIN;
    j->run.qp_max = PREP_QP_MAX;
    j->run.desired_frame_bytes = (j->opt.videoKbps ? j->opt.videoKbps : PREP_VIDEO_KBPS) * 1000 / 8 / j->fps;
    return 0;
}

/* The NALs of one encoded frame (Annex B) -> an MP4 sample (4-byte lengths); keeps SPS / PPS. */
static int video_sample(PrepJob *j, const uint8_t *in, size_t n, size_t *outLen, int *key)
{
    State *s = &j->s;
    if (j->nalCap < n + 64) {
        free(j->nal);
        j->nalCap = n + 65536;
        j->nal = malloc(j->nalCap);
        if (!j->nal)
            return fail(j, "out of memory (sample)");
    }
    size_t o = 0, i = 0;
    *key = 0;
    while (i + 3 <= n) {
        if (!(in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1)) {   /* find a start code */
            i++;
            continue;
        }
        const size_t start = i + 3;
        size_t end = start;
        while (end + 3 <= n && !(in[end] == 0 && in[end + 1] == 0 && (in[end + 2] == 1 || (in[end + 2] == 0 && end + 3 < n && in[end + 3] == 1))))
            end++;
        if (end + 3 > n)
            end = n;
        const size_t len = end - start;
        i = end;
        if (!len)
            continue;
        const int type = in[start] & 0x1F;
        if (type == 7 || type == 8) {
            uint8_t *dst = type == 7 ? s->sps : s->pps;
            uint32_t *dl = type == 7 ? &s->spsLen : &s->ppsLen;
            if (len <= sizeof(s->sps)) {
                memcpy(dst, in + start, len);
                *dl = (uint32_t)len;
            }
            continue;
        }
        if (type == 5)
            *key = 1;
        j->nal[o] = (uint8_t)(len >> 24), j->nal[o + 1] = (uint8_t)(len >> 16);
        j->nal[o + 2] = (uint8_t)(len >> 8), j->nal[o + 3] = (uint8_t)len;
        memcpy(j->nal + o + 4, in + start, len);
        o += 4 + len;
    }
    *outLen = o;
    return 0;
}

static int flush_pending(PrepJob *j, uint64_t tick)
{
    State *s = &j->s;
    if (!s->havePending)
        return 0;
    uint64_t dur = tick > s->pendTick ? tick - s->pendTick : 1;
    if (dur > TBL_DUR)
        dur = TBL_DUR;
    s->havePending = 0;
    return tbl_add(j, s->pendOffset, s->pendSize, (s->pendKey ? TBL_KEY : 0) | (uint32_t)dur);
}

static int consume_picture(PrepJob *j, const Item *it)
{
    State *s = &j->s;
    uint8_t *yuv = j->pic[it->slot];
    H264E_io_yuv_t io = {{yuv, yuv + 400 * j->H, yuv + 400 * j->H + 400 * j->H / 4}, {400, 200, 200}};
    unsigned char *coded = NULL;
    int size = 0;
    const uint64_t t0 = now_us(j);
    const int bad = H264E_encode(j->enc, j->scratch, &j->run, &io, &coded, &size);
    STORE(j->picBusy[it->slot], 0);
    j->st.videoMs += ms_since(j, t0);
    if (bad || size <= 0)
        return fail(j, "H.264 encoding failed");
    size_t len;
    int key;
    const uint64_t t1 = now_us(j);
    if (video_sample(j, coded, (size_t)size, &len, &key) || flush_pending(j, it->tick))
        return PREP_ERROR;
    uint32_t off;
    if (sample_write(j, j->nal, len, &off))
        return PREP_ERROR;
    j->st.writeMs += ms_since(j, t1);
    s->pendOffset = off;
    s->pendSize = (uint32_t)len;
    s->pendKey = (uint32_t)key;
    s->pendTick = it->tick;
    s->havePending = 1;
    j->st.frames++;
    return 0;
}

static int consume_audio(PrepJob *j, Item *it)
{
    const uint64_t t0 = now_us(j);
    uint32_t off;
    const int r = sample_write(j, it->data, it->len, &off) || tbl_add(j, off, it->len, TBL_AUDIO);
    free(it->data);
    it->data = NULL;
    j->st.writeMs += ms_since(j, t0);
    return r ? PREP_ERROR : 0;
}

/* ------------------------------------------------------------------ MP4 index (moov) */

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int bad;
} Buf;

static void b_need(Buf *b, size_t k)
{
    if (b->bad || b->n + k <= b->cap)
        return;
    size_t cap = b->cap ? b->cap * 2 : 65536;
    while (cap < b->n + k)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p) {
        b->bad = 1;
        return;
    }
    b->p = p;
    b->cap = cap;
}

static void b_u8(Buf *b, uint32_t v)
{
    b_need(b, 1);
    if (!b->bad)
        b->p[b->n++] = (uint8_t)v;
}
static void b_u16(Buf *b, uint32_t v) { b_u8(b, v >> 8); b_u8(b, v); }
static void b_u32(Buf *b, uint32_t v) { b_u16(b, v >> 16); b_u16(b, v); }
static void b_bytes(Buf *b, const void *d, size_t k)
{
    b_need(b, k);
    if (!b->bad) {
        memcpy(b->p + b->n, d, k);
        b->n += k;
    }
}
static void b_zero(Buf *b, size_t k) { while (k--) b_u8(b, 0); }
static size_t box(Buf *b, const char *type)
{
    const size_t at = b->n;
    b_u32(b, 0);
    b_bytes(b, type, 4);
    return at;
}
static size_t fullbox(Buf *b, const char *type, uint32_t verFlags)
{
    const size_t at = box(b, type);
    b_u32(b, verFlags);
    return at;
}
static void box_end(Buf *b, size_t at)
{
    if (b->bad)
        return;
    const uint32_t size = (uint32_t)(b->n - at);
    b->p[at] = (uint8_t)(size >> 24), b->p[at + 1] = (uint8_t)(size >> 16);
    b->p[at + 2] = (uint8_t)(size >> 8), b->p[at + 3] = (uint8_t)size;
}
static void b_matrix(Buf *b)
{
    static const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
    for (int i = 0; i < 9; i++)
        b_u32(b, m[i]);
}

static void w_tkhd(Buf *b, uint32_t id, uint32_t durMs, int audio, uint32_t w, uint32_t h)
{
    const size_t at = fullbox(b, "tkhd", 3);
    b_u32(b, 0), b_u32(b, 0), b_u32(b, id), b_u32(b, 0), b_u32(b, durMs);
    b_zero(b, 8);
    b_u16(b, 0), b_u16(b, audio ? 1 : 0), b_u16(b, audio ? 0x100 : 0), b_u16(b, 0);
    b_matrix(b);
    b_u32(b, w << 16), b_u32(b, h << 16);
    box_end(b, at);
}

static void w_mdhd_hdlr(Buf *b, uint32_t timescale, uint32_t dur, int audio)
{
    size_t at = fullbox(b, "mdhd", 0);
    b_u32(b, 0), b_u32(b, 0), b_u32(b, timescale), b_u32(b, dur), b_u16(b, 0x55C4), b_u16(b, 0);
    box_end(b, at);
    at = fullbox(b, "hdlr", 0);
    b_u32(b, 0);
    b_bytes(b, audio ? "soun" : "vide", 4);
    b_zero(b, 12);
    const char *name = audio ? "SoundHandler" : "VideoHandler";
    b_bytes(b, name, strlen(name) + 1);
    box_end(b, at);
}

static void w_dinf(Buf *b)
{
    const size_t at = box(b, "dinf"), dref = fullbox(b, "dref", 0);
    b_u32(b, 1);
    const size_t url = fullbox(b, "url ", 1);
    box_end(b, url);
    box_end(b, dref);
    box_end(b, at);
}

/* entries of one track: e[i] = {offset, size, info} */
static void w_tables(Buf *b, const uint32_t (*e)[3], uint32_t n, int audio)
{
    size_t at = fullbox(b, "stts", 0);
    if (audio) {
        b_u32(b, 1), b_u32(b, n), b_u32(b, AAC_FRAME);
    } else {
        const size_t cnt = b->n;
        b_u32(b, 0);
        uint32_t runs = 0;
        for (uint32_t i = 0; i < n;) {
            uint32_t k = i + 1;
            while (k < n && (e[k][2] & TBL_DUR) == (e[i][2] & TBL_DUR))
                k++;
            b_u32(b, k - i), b_u32(b, e[i][2] & TBL_DUR);
            runs++;
            i = k;
        }
        if (!b->bad)
            b->p[cnt] = (uint8_t)(runs >> 24), b->p[cnt + 1] = (uint8_t)(runs >> 16),
            b->p[cnt + 2] = (uint8_t)(runs >> 8), b->p[cnt + 3] = (uint8_t)runs;
    }
    box_end(b, at);
    if (!audio) {
        at = fullbox(b, "stss", 0);
        uint32_t keys = 0;
        for (uint32_t i = 0; i < n; i++)
            keys += (e[i][2] & TBL_KEY) != 0;
        b_u32(b, keys);
        for (uint32_t i = 0; i < n; i++)
            if (e[i][2] & TBL_KEY)
                b_u32(b, i + 1);
        box_end(b, at);
    }
    at = fullbox(b, "stsc", 0);
    b_u32(b, 1), b_u32(b, 1), b_u32(b, 1), b_u32(b, 1);
    box_end(b, at);
    at = fullbox(b, "stsz", 0);
    b_u32(b, 0), b_u32(b, n);
    for (uint32_t i = 0; i < n; i++)
        b_u32(b, e[i][1]);
    box_end(b, at);
    at = fullbox(b, "stco", 0);
    b_u32(b, n);
    for (uint32_t i = 0; i < n; i++)
        b_u32(b, e[i][0]);
    box_end(b, at);
}

static int write_moov(PrepJob *j)
{
    State *s = &j->s;
    if (fflush(j->tbl) || fseek(j->tbl, 0, SEEK_SET))
        return fail(j, "cannot read the sample table");
    uint32_t (*all)[3] = malloc((size_t)s->tblCount * 12 + 12);
    if (!all || fread(all, 12, s->tblCount, j->tbl) != s->tblCount) {
        free(all);
        return fail(j, "cannot read the sample table");
    }
    /* split into the two tracks, in file order */
    uint32_t nv = 0, na = 0;
    for (uint32_t i = 0; i < s->tblCount; i++)
        if (all[i][2] & TBL_AUDIO)
            na++;
        else
            nv++;
    uint32_t (*v)[3] = malloc((size_t)nv * 12 + 12), (*a)[3] = malloc((size_t)na * 12 + 12);
    if (!v || !a) {
        free(all), free(v), free(a);
        return fail(j, "out of memory (index)");
    }
    uint32_t iv = 0, ia = 0, vTicks = 0;
    for (uint32_t i = 0; i < s->tblCount; i++) {
        if (all[i][2] & TBL_AUDIO) {
            memcpy(a[ia++], all[i], 12);
        } else {
            memcpy(v[iv++], all[i], 12);
            vTicks += all[i][2] & TBL_DUR;
        }
    }
    free(all);
    const uint32_t durMs = (uint32_t)((uint64_t)vTicks * 1000 / GRID_HZ);
    const uint32_t aDur = (uint32_t)((uint64_t)j->endUs * OUT_RATE / 1000000);
    Buf b = {0};
    const size_t moov = box(&b, "moov");
    size_t at = fullbox(&b, "mvhd", 0);
    b_u32(&b, 0), b_u32(&b, 0), b_u32(&b, 1000), b_u32(&b, durMs), b_u32(&b, 0x10000), b_u16(&b, 0x100);
    b_zero(&b, 10);
    b_matrix(&b);
    b_zero(&b, 24);
    b_u32(&b, na ? 3 : 2);
    box_end(&b, at);

    /* video */
    const size_t trak = box(&b, "trak");
    w_tkhd(&b, 1, durMs, 0, 400, (uint32_t)j->H);
    size_t mdia = box(&b, "mdia");
    w_mdhd_hdlr(&b, GRID_HZ, vTicks, 0);
    size_t minf = box(&b, "minf");
    at = fullbox(&b, "vmhd", 1);
    b_zero(&b, 8);
    box_end(&b, at);
    w_dinf(&b);
    size_t stbl = box(&b, "stbl");
    size_t stsd = fullbox(&b, "stsd", 0);
    b_u32(&b, 1);
    const size_t avc1 = box(&b, "avc1");
    b_zero(&b, 6), b_u16(&b, 1), b_zero(&b, 16), b_u16(&b, 400), b_u16(&b, (uint32_t)j->H);
    b_u32(&b, 0x480000), b_u32(&b, 0x480000), b_u32(&b, 0), b_u16(&b, 1);
    b_zero(&b, 32), b_u16(&b, 0x18), b_u16(&b, 0xFFFF);
    const size_t avcC = box(&b, "avcC");
    b_u8(&b, 1), b_u8(&b, s->sps[1]), b_u8(&b, s->sps[2]), b_u8(&b, s->sps[3]), b_u8(&b, 0xFF), b_u8(&b, 0xE1);
    b_u16(&b, s->spsLen), b_bytes(&b, s->sps, s->spsLen), b_u8(&b, 1), b_u16(&b, s->ppsLen), b_bytes(&b, s->pps, s->ppsLen);
    box_end(&b, avcC);
    box_end(&b, avc1);
    box_end(&b, stsd);
    w_tables(&b, (const uint32_t (*)[3])v, nv, 0);
    box_end(&b, stbl);
    box_end(&b, minf);
    box_end(&b, mdia);
    box_end(&b, trak);

    if (na) {
        const size_t trakA = box(&b, "trak");
        w_tkhd(&b, 2, (uint32_t)((uint64_t)aDur * 1000 / OUT_RATE), 1, 0, 0);
        const size_t edts = box(&b, "edts");
        at = fullbox(&b, "elst", 0);   /* skip the encoder's priming */
        b_u32(&b, 1), b_u32(&b, (uint32_t)((uint64_t)aDur * 1000 / OUT_RATE)), b_u32(&b, AAC_DELAY), b_u32(&b, 0x10000);
        box_end(&b, at);
        box_end(&b, edts);
        mdia = box(&b, "mdia");
        w_mdhd_hdlr(&b, OUT_RATE, na * AAC_FRAME, 1);
        minf = box(&b, "minf");
        at = fullbox(&b, "smhd", 0);
        b_u32(&b, 0);
        box_end(&b, at);
        w_dinf(&b);
        stbl = box(&b, "stbl");
        stsd = fullbox(&b, "stsd", 0);
        b_u32(&b, 1);
        const size_t mp4a = box(&b, "mp4a");
        b_zero(&b, 6), b_u16(&b, 1), b_zero(&b, 8), b_u16(&b, 2), b_u16(&b, 16), b_u32(&b, 0), b_u32(&b, OUT_RATE << 16);
        const size_t esds = fullbox(&b, "esds", 0);
        const uint32_t br = (j->opt.audioKbps ? j->opt.audioKbps : 96) * 1000;
        b_u8(&b, 3), b_u8(&b, 25), b_u16(&b, 2), b_u8(&b, 0);              /* ES_Descriptor */
        b_u8(&b, 4), b_u8(&b, 17), b_u8(&b, 0x40), b_u8(&b, 0x15);         /* DecoderConfig: AAC, audio */
        b_u8(&b, 0), b_u16(&b, AAC_OUT_MAX), b_u32(&b, br), b_u32(&b, br);
        b_u8(&b, 5), b_u8(&b, 2), b_u8(&b, 0x12), b_u8(&b, 0x90);          /* AAC-LC, 32 kHz, stereo */
        b_u8(&b, 6), b_u8(&b, 1), b_u8(&b, 2);                             /* SLConfig */
        box_end(&b, esds);
        box_end(&b, mp4a);
        box_end(&b, stsd);
        w_tables(&b, (const uint32_t (*)[3])a, na, 1);
        box_end(&b, stbl);
        box_end(&b, minf);
        box_end(&b, mdia);
        box_end(&b, trakA);
    }
    box_end(&b, moov);
    free(v);
    free(a);
    if (b.bad) {
        free(b.p);
        return fail(j, "out of memory (index)");
    }
    const uint32_t mdat = s->partSize - FTYP_SIZE;
    const uint8_t sz[4] = {(uint8_t)(mdat >> 24), (uint8_t)(mdat >> 16), (uint8_t)(mdat >> 8), (uint8_t)mdat};
    int r = 0;
    j->finalSize = s->partSize + (uint32_t)b.n;
    if (fseek(j->part, (long)s->partSize, SEEK_SET) || put(j, j->part, b.p, b.n) ||
        fseek(j->part, FTYP_SIZE, SEEK_SET) || put(j, j->part, sz, 4) || fflush(j->part))
        r = fail(j, "write failed (SD card full?)");
    free(b.p);
    return r;
}

/* [C] IT_END: the last frame's length, the index, the rename. */
static int consume_end(PrepJob *j, uint64_t endTick)
{
    State *s = &j->s;
    if (endTick <= s->pendTick)
        endTick = s->pendTick + 1;
    if (flush_pending(j, endTick))
        return PREP_ERROR;
    if (request_flush(j))   /* every byte out; then the file is the consumer's */
        return PREP_ERROR;
    STORE(j->consumerDone, 1);
    while (!LOAD(j->writerExited))
        j->opt.sleepUs(1000);
    if (!s->spsLen || !s->ppsLen)
        return fail(j, "no video in the recording");
    if (write_moov(j))
        return PREP_ERROR;
    if (fflush(j->part) || ftruncate(fileno(j->part), j->finalSize)) /* the space reserved but not used */
        return fail(j, "cannot finish the file");
    fclose(j->part);
    fclose(j->tbl);
    fclose(j->prep);
    j->part = j->tbl = j->prep = NULL;
    char part[300], mp4[300], tbl[300], prep[300];
    prep_path(part, sizeof(part), j->base, "mp4.part");
    prep_path(mp4, sizeof(mp4), j->base, "mp4");
    prep_path(tbl, sizeof(tbl), j->base, "ptbl");
    prep_path(prep, sizeof(prep), j->base, "prep");
    remove(mp4);
    if (rename(part, mp4))
        return fail(j, "cannot rename the result");
    remove(tbl);
    remove(prep);
    j->finished = 1;
    return PREP_DONE;
}

int prep_consume(PrepJob *j)
{
    if (j->finished)
        return PREP_DONE;
    if (j->err[0])
        return PREP_ERROR;
    for (int n = 0; n < 4; n++) {
        const uint32_t t = j->tail;
        if (t == LOAD(j->head)) {   /* empty: the producer is behind */
            if (!j->waitSince)
                j->waitSince = now_us(j) + 1;
            return n ? PREP_MORE : PREP_WAIT;
        }
        if (j->waitSince) {
            j->st.consumeWaitMs += ms_since(j, j->waitSince - 1);
            j->waitSince = 0;
        }
        Item it = j->ring[t % RING];
        STORE(j->tail, t + 1);
        int r = 0;
        if (it.type == IT_PIC)
            r = consume_picture(j, &it);
        else if (it.type == IT_AAC)
            r = consume_audio(j, &it);
        else
            r = consume_end(j, it.tick);
        __atomic_add_fetch(&j->consumed, 1, __ATOMIC_RELEASE);
        if (r)
            return r;   /* PREP_ERROR or PREP_DONE */
    }
    return PREP_MORE;
}

/* ------------------------------------------------------------------ open / close */

static void state_init(PrepJob *j)
{
    State *s = &j->s;
    memset(s, 0, sizeof(*s));
    audio_placer_init(&s->ap, j->fh.audioRate ? j->fh.audioRate : R3D_AUDIO_RATE_MHZ);
    audio_eq_init(&s->eq, s->ap.rateMilliHz / 1000.0, AUDIO_EQ_FREQ_HZ, AUDIO_EQ_GAIN_DB, AUDIO_EQ_Q);
    s->needKey[0] = s->needKey[1] = 1;
    s->dirty = 3;   /* tick 0 is a frame (black until the first pictures) */
    s->partSize = MDAT_START;
}

static FILE *open_file(PrepJob *j, const char *ext, const char *mode)
{
    char path[300];
    prep_path(path, sizeof(path), j->base, ext);
    return fopen(path, mode);
}

/* big stdio buffers: a few large SD requests instead of many small ones */
static void buffers(PrepJob *j)
{
    if (j->part) /* the output buffers are big already */
        setvbuf(j->part, NULL, _IONBF, 0);
    if (j->tbl && j->tblBuf)
        setvbuf(j->tbl, j->tblBuf, _IOFBF, TBL_BUF);
}

static int start_fresh(PrepJob *j, uint64_t srcSize)
{
    state_init(j);
    j->s.srcSize = srcSize;
    j->s.srcOffset = sizeof(R3dFileHeader);
    memset(j->px[0], 0, TOP_N * 2);
    memset(j->px[1], 0, BOT_N * 2);
    j->part = open_file(j, "mp4.part", "w+b");
    j->tbl = open_file(j, "ptbl", "w+b");
    j->prep = open_file(j, "prep", "w+b");
    if (!j->part || !j->tbl || !j->prep)
        return fail(j, "cannot create files next to the recording");
    /* Reserve the MP4's space at once (~1/20 of the recording; 1/10 to be safe): written into
       a file that grows, a nearly full, fragmented card finds clusters for every piece, and
       that was 40% of the time on an Old 3DS (2026-11-06). A larger result just grows. */
    uint64_t reserve = (srcSize / 10 + (1u << 20)) & ~(uint64_t)((1u << 20) - 1);
    if (reserve < (4u << 20))
        reserve = 4u << 20;
    if (reserve > (512u << 20))
        reserve = 512u << 20;
    if (ftruncate(fileno(j->part), (off_t)reserve))
        return fail(j, "no space on the SD card");
    buffers(j);
    static const uint8_t head[MDAT_START] = {
        0, 0, 0, FTYP_SIZE, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0,
        'i', 's', 'o', 'm', 'i', 's', 'o', '2', 'a', 'v', 'c', '1', 'm', 'p', '4', '1',
        0, 0, 0, 0, 'm', 'd', 'a', 't'};
    return put(j, j->part, head, sizeof(head));
}

static int try_resume(PrepJob *j, uint64_t srcSize)
{
    FILE *f = open_file(j, "prep", "r+b");
    if (!f)
        return 0;
    if (load_slot(f, &j->s, j->px[0], j->px[1]) < 0 || j->s.srcSize != srcSize) {
        fclose(f);
        return 0;
    }
    j->prep = f;
    j->part = open_file(j, "mp4.part", "r+b");
    j->tbl = open_file(j, "ptbl", "r+b");
    /* the part keeps its reserved length: what follows partSize is written over */
    if (!j->part || !j->tbl || ftruncate(fileno(j->tbl), (off_t)j->s.tblCount * 12) ||
        fseek(j->part, (long)j->s.partSize, SEEK_SET) ||
        fseek(j->tbl, (long)j->s.tblCount * 12, SEEK_SET)) {
        if (j->part)
            fclose(j->part);
        if (j->tbl)
            fclose(j->tbl);
        fclose(f);
        j->part = j->tbl = j->prep = NULL;
        return 0;
    }
    buffers(j);
    return 1;
}

PrepJob *prep_open(const char *src, const char *base, const PrepOptions *opt, char *err, size_t errLen)
{
    PrepJob *j = calloc(1, sizeof(PrepJob));
    if (!j) {
        snprintf(err, errLen, "out of memory");
        return NULL;
    }
    j->opt = *opt;
    snprintf(j->base, sizeof(j->base), "%s", base);
    yuv_tables();
    j->srcBuf = malloc(SRC_BUF);
    j->obuf[0] = malloc(OUT_BUF);
    j->obuf[1] = malloc(OUT_BUF);
    j->tblBuf = malloc(TBL_BUF);
    j->src = fopen(src, "rb");
    if (j->src && j->srcBuf)
        setvbuf(j->src, j->srcBuf, _IOFBF, SRC_BUF);
    if (!j->src || fread(&j->fh, 1, sizeof(j->fh), j->src) != sizeof(j->fh) || j->fh.magic != R3D_MAGIC_FILE) {
        fail(j, "not a recording");
        goto error;
    }
    fseek(j->src, 0, SEEK_END);
    const uint64_t srcSize = (uint64_t)ftell(j->src);
    j->both = j->fh.mode != R3D_MODE_FULL_TOP && j->fh.mode != R3D_MODE_FULL_BOTTOM;
    j->bottomOnly = j->fh.mode == R3D_MODE_FULL_BOTTOM;   /* 400 x 240, the bottom screen centred */
    j->H = j->both ? 480 : 240;
    j->fps = j->fh.version >= 3 && j->fh.fps ? j->fh.fps : 30;
    if (j->fps > 60)
        j->fps = 30;
    j->audio = j->fh.version >= 4 && j->fh.audioRate;
    j->inRate = (j->fh.audioRate ? j->fh.audioRate : R3D_AUDIO_RATE_MHZ) / 1000.0;
    j->rsStep = j->inRate / OUT_RATE;
    j->px[0] = malloc(TOP_N * 2);
    j->px[1] = malloc(BOT_N * 2);
    j->payload = malloc(codec_xr32_max_size(TOP_N) + 64);
    const size_t picBytes = 400 * (size_t)j->H * 3 / 2;
    j->yuv = malloc(picBytes);
    for (int i = 0; i < PIC_SLOTS; i++)
        j->pic[i] = malloc(picBytes);
    if (!j->obuf[0] || !j->obuf[1] || !j->px[0] || !j->px[1] || !j->payload || !j->yuv || !j->pic[PIC_SLOTS - 1] || !j->pic[0] || !j->pic[1]) {
        fail(j, "out of memory");
        goto error;
    }
    memset(j->yuv, 16, 400 * (size_t)j->H);
    memset(j->yuv + 400 * j->H, 128, 400 * (size_t)j->H / 2);
    const int resumed = try_resume(j, srcSize);
    if (!resumed && start_fresh(j, srcSize))
        goto error;
    if (video_open(j) || (j->audio && aac_open(j)))
        goto error;
    if (resumed) {
        j->st.resumedFrom = j->s.videoFrames;
        j->skipCk = 1;   /* the resume point is this very state */
        for (int sc = 0; sc < 2; sc++)
            if (j->s.seen[sc] && SCREEN_SHOWN(j, sc))
                yuv_screen(j, sc);
        if (j->audio && j->s.aacHistFill) {   /* warm the new encoder up; its output is in the file */
            j->aacDiscard = j->s.aacHistFill;
            for (uint32_t i = AAC_WARM - j->s.aacHistFill; i < AAC_WARM; i++) {
                int16_t frame[AAC_FRAME * 2];
                memcpy(frame, j->s.aacHist + i * AAC_FRAME * 2, sizeof(frame));
                if (aac_encode(j, frame))
                    goto error;
            }
            j->aacDiscard = 0;
        }
    }
    if (fseek(j->src, (long)j->s.srcOffset, SEEK_SET)) {
        fail(j, "cannot read the recording");
        goto error;
    }
    return j;
error:
    snprintf(err, errLen, "%s", j->err[0] ? j->err : "cannot open");
    prep_close(j);
    return NULL;
}

void prep_close(PrepJob *j)
{
    if (!j)
        return;
    if (j->ah)
        j->aapi.Uninit(j->ah);
    for (uint32_t t = j->tail; t != j->head; t++)   /* sound frames still queued */
        if (j->ring[t % RING].type == IT_AAC)
            free(j->ring[t % RING].data);
    if (j->src)
        fclose(j->src);
    if (j->part)
        fclose(j->part);
    if (j->tbl)
        fclose(j->tbl);
    if (j->prep)
        fclose(j->prep);
    free(j->enc);
    free(j->scratch);
    free(j->px[0]);
    free(j->px[1]);
    free(j->payload);
    free(j->yuv);
    for (int i = 0; i < PIC_SLOTS; i++)
        free(j->pic[i]);
    free(j->nal);
    free(j->srcBuf);
    free(j->obuf[0]);
    free(j->obuf[1]);
    free(j->tblBuf);
    free(j);
}

uint32_t prep_permille(const PrepJob *j)
{
    if (j->finished)
        return 1000;
    const uint32_t p = j->s.srcSize ? (uint32_t)(j->s.srcOffset * 1000 / j->s.srcSize) : 0;
    return p > 999 ? 999 : p;
}

const PrepStats *prep_stats(const PrepJob *j)
{
    return &j->st;
}

const char *prep_error(const PrepJob *j)
{
    return j->err;
}
