/*
 * encbench: can the console itself turn a recording into a small video before sending it to a
 * phone? (research 2026-11-05: lossless can't get below ~1.5x; the size comes from a lossy codec).
 * Decodes the first FRAMES frames of the newest recording (both screens, 400 x 480, as r3dconv
 * lays them out) and encodes them with
 *   - JPEG (libjpeg-turbo, 4:2:0) at two qualities
 *   - H.264 (minih264e, plain C, CC0) at two speed settings, 1 thread and 2 (core 0 + core 1)
 * Reports ms / frame, fps and bytes for each, plus the cost of XR32 decoding and RGB -> YUV.
 * Results on screen and in sdmc:/3ds/encbench.log. A New 3DS runs at 804 MHz.
 */
#include <3ds.h>

#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <turbojpeg.h>

#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

#include "codec.h"
#include "r3d_format.h"

#define FRAMES 300
#define W 400
#define H 480
#define REC_DIR "sdmc:/3ds/recorder/recordings"
#define LOG "sdmc:/3ds/encbench.log"

static u16 g_top[400 * 240], g_bot[320 * 240];
static u8 g_rgb[W * H * 3];
static u8 g_yuv[W * H * 3 / 2];
static u8 *g_payload;
static FILE *g_log;
static char g_file[160];

static void out(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    if (g_log) {
        fprintf(g_log, "%s\n", line);
        fflush(g_log);
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static double now_ms(void) { return svcGetSystemTick() / (double)SYSCLOCK_ARM11 * 1000.0; }

static bool newest(char *outp, size_t cap)
{
    DIR *d = opendir(REC_DIR);
    if (!d)
        return false;
    char best[64] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        const size_t n = strlen(e->d_name);
        if (n > 4 && n < sizeof(best) && strcmp(e->d_name + n - 4, ".r3d") == 0 && strcmp(e->d_name, best) > 0)
            strcpy(best, e->d_name);
    }
    closedir(d);
    if (!best[0])
        return false;
    snprintf(outp, cap, REC_DIR "/%s", best);
    return true;
}

/* column-major RGB565 (word 0 = bottom row) into the 400 x 480 RGB picture */
static void put(const u16 *px, int cols, int ox, int oy)
{
    for (int y = 0; y < 240; y++) {
        u8 *row = g_rgb + ((oy + y) * W + ox) * 3;
        for (int x = 0; x < cols; x++) {
            const u16 v = px[x * 240 + 239 - y];
            row[3 * x] = (u8)(((v >> 11) & 31) * 255 / 31);
            row[3 * x + 1] = (u8)(((v >> 5) & 63) * 255 / 63);
            row[3 * x + 2] = (u8)((v & 31) * 255 / 31);
        }
    }
}

static void to_yuv(void) /* BT.601 limited range, 4:2:0 */
{
    u8 *Y = g_yuv, *U = g_yuv + W * H, *V = U + W * H / 4;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const u8 *p = g_rgb + (y * W + x) * 3;
            Y[y * W + x] = (u8)((66 * p[0] + 129 * p[1] + 25 * p[2] + 128 + 4096) >> 8);
        }
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 2) {
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                const u8 *p = g_rgb + ((y + (k >> 1)) * W + x + (k & 1)) * 3;
                r += p[0];
                g += p[1];
                b += p[2];
            }
            r /= 4; g /= 4; b /= 4;
            U[(y / 2) * (W / 2) + x / 2] = (u8)((-38 * r - 74 * g + 112 * b + 128 + 32768) >> 8);
            V[(y / 2) * (W / 2) + x / 2] = (u8)((112 * r - 94 * g - 18 * b + 128 + 32768) >> 8);
        }
}

/* Frames in recording order: calls each(index) once per top-screen frame, g_rgb ready. */
typedef void (*FrameFn)(int index, void *ctx);
static double decode_all(FrameFn each, void *ctx, double *convMs)
{
    FILE *f = fopen(g_file, "rb");
    if (!f)
        return -1;
    setvbuf(f, NULL, _IOFBF, 256 * 1024);
    fseek(f, sizeof(R3dFileHeader), SEEK_SET);
    memset(g_top, 0, sizeof(g_top));
    memset(g_bot, 0, sizeof(g_bot));
    memset(g_rgb, 0, sizeof(g_rgb));
    double dec = 0;
    *convMs = 0;
    int n = 0;
    R3dRecHeader h;
    while (n < FRAMES && fread(&h, 1, sizeof(h), f) == sizeof(h)) {
        const bool delta = h.magic == R3D_MAGIC_YFRAME || h.magic == R3D_MAGIC_XFRAME;
        const u32 stored = delta ? (h.len + 15) & ~15u : h.len;
        if (h.magic == R3D_MAGIC_AUDIO || h.magic == R3D_MAGIC_PAD || h.magic == R3D_MAGIC_DSPMEM) {
            fseek(f, stored, SEEK_CUR);
            continue;
        }
        if (h.magic != R3D_MAGIC_YFRAME || (h.screen & 0x3F) > 1 || stored > 400 * 240 * 2 + 4096)
            break;
        if (fread(g_payload, 1, stored, f) != stored)
            break;
        const int sc = h.screen & 0x3F;
        const double t0 = now_ms();
        codec_xr32_decode(g_payload, h.len, sc ? g_bot : g_top, sc ? 320 * 240 : 400 * 240, (h.screen & R3D_SCREEN_KEY) != 0);
        const double t1 = now_ms();
        put(sc ? g_bot : g_top, sc ? 320 : 400, sc ? 40 : 0, sc ? 240 : 0);
        dec += t1 - t0;
        *convMs += now_ms() - t1;
        if (sc == 0) /* one output frame per top frame */
            each(n++, ctx);
    }
    fclose(f);
    return n ? dec / n : -1;
}

/* ------------------------------------------------------------ JPEG */

typedef struct {
    tjhandle tj;
    int quality;
    double ms;
    u64 bytes;
} JpegCtx;

static void jpeg_frame(int index, void *p)
{
    (void)index;
    JpegCtx *c = p;
    unsigned char *jpg = NULL;
    unsigned long size = 0;
    const double t0 = now_ms();
    tjCompress2(c->tj, g_rgb, W, 0, H, TJPF_RGB, &jpg, &size, TJSAMP_420, c->quality, TJFLAG_FASTDCT);
    c->ms += now_ms() - t0;
    c->bytes += size;
    tjFree(jpg);
}

/* ------------------------------------------------------------ H.264 */

typedef struct {
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    H264E_run_param_t run;
    double ms, yuvMs;
    u64 bytes;
} H264Ctx;

static void h264_frame(int index, void *p)
{
    (void)index;
    H264Ctx *c = p;
    const double t0 = now_ms();
    to_yuv();
    const double t1 = now_ms();
    H264E_io_yuv_t io = {{g_yuv, g_yuv + W * H, g_yuv + W * H + W * H / 4}, {W, W / 2, W / 2}};
    unsigned char *coded = NULL;
    int size = 0;
    H264E_encode(c->enc, c->scratch, &c->run, &io, &coded, &size);
    c->ms += now_ms() - t1;
    c->yuvMs += t1 - t0;
    c->bytes += (u64)size;
}

/* the encoder's jobs on 2 threads: this one and one on core 1 */
static Thread g_helper;
static LightEvent g_go, g_done;
static void (*g_job)(void *);
static void *g_jobData;
static volatile bool g_helperQuit;

static void helper_main(void *arg)
{
    (void)arg;
    for (;;) {
        LightEvent_Wait(&g_go);
        if (g_helperQuit)
            return;
        g_job(g_jobData);
        LightEvent_Signal(&g_done);
    }
}

static void run_jobs(void *token, void (*job)(void *), void *data[], int njobs)
{
    (void)token;
    int i = 0;
    if (g_helper && njobs > 1) {
        g_job = job;
        g_jobData = data[0];
        LightEvent_Signal(&g_go);
        i = 1;
    }
    for (; i < njobs; i++)
        job(data[i]);
    if (g_helper && njobs > 1)
        LightEvent_Wait(&g_done);
}

static void h264_test(const char *name, int speed, int threads)
{
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.width = W;
    cp.height = H;
    cp.gop = 60;
    cp.const_input_flag = 1;
    cp.max_threads = threads;
    cp.run_func_in_thread = threads > 1 ? run_jobs : NULL;
    int sp = 0, ss = 0;
    if (H264E_sizeof(&cp, &sp, &ss)) {
        out("%s: H264E_sizeof failed", name);
        return;
    }
    H264Ctx c;
    memset(&c, 0, sizeof(c));
    c.enc = memalign(64, sp);
    c.scratch = memalign(64, ss);
    if (!c.enc || !c.scratch || H264E_init(c.enc, &cp)) {
        out("%s: init failed (%d + %d KB)", name, sp / 1024, ss / 1024);
        free(c.enc);
        free(c.scratch);
        return;
    }
    c.run.encode_speed = speed;
    c.run.qp_min = 20;
    c.run.qp_max = 38;
    c.run.desired_frame_bytes = 1500000 / 8 / 30; /* ~1.5 Mbit/s: r3d2mp4-like quality */
    double conv;
    const double dec = decode_all(h264_frame, &c, &conv);
    (void)dec;
    const double ms = c.ms / FRAMES;
    out("%-22s %6.1f ms/frame %5.1f fps  %6.2f MB/300f (yuv %.1f ms)", name, ms, 1000.0 / ms,
        c.bytes / 1048576.0, c.yuvMs / FRAMES);
    free(c.enc);
    free(c.scratch);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    osSetSpeedupEnable(true);
    g_log = fopen(LOG, "a");
    g_payload = malloc(400 * 240 * 2 + 8192);
    out("encbench: %s, %d frames, %dx%d", n3ds ? "New 3DS (804 MHz)" : "Old 3DS (268 MHz)", FRAMES, W, H);
    if (!newest(g_file, sizeof(g_file)) || !g_payload) {
        out("no recording in " REC_DIR);
    } else {
        out("file: %s", strrchr(g_file, '/') + 1);
        /* decoding alone */
        JpegCtx j = {tjInitCompress(), 75, 0, 0};
        double conv;
        double dec = decode_all(jpeg_frame, &j, &conv);
        out("XR32 decode %.1f ms/screen-frame, to RGB %.1f ms", dec, conv / FRAMES);
        out("%-22s %6.1f ms/frame %5.1f fps  %6.2f MB/300f", "JPEG q75 (4:2:0)", j.ms / FRAMES, 1000.0 * FRAMES / j.ms,
            j.bytes / 1048576.0);
        j.quality = 85;
        j.ms = 0;
        j.bytes = 0;
        decode_all(jpeg_frame, &j, &conv);
        out("%-22s %6.1f ms/frame %5.1f fps  %6.2f MB/300f", "JPEG q85 (4:2:0)", j.ms / FRAMES, 1000.0 * FRAMES / j.ms,
            j.bytes / 1048576.0);
        tjDestroy(j.tj);

        h264_test("H.264 speed 9, 1 thr", 9, 1);
        h264_test("H.264 speed 4, 1 thr", 4, 1);
        /* core 1: an application may use part of it (30% by default, asked for 80) */
        APT_SetAppCpuTimeLimit(80);
        LightEvent_Init(&g_go, RESET_ONESHOT);
        LightEvent_Init(&g_done, RESET_ONESHOT);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        g_helper = threadCreate(helper_main, NULL, 64 * 1024, prio, 1, false);
        if (!g_helper && n3ds)
            g_helper = threadCreate(helper_main, NULL, 64 * 1024, prio, 2, false);
        if (g_helper) {
            h264_test("H.264 speed 9, 2 thr", 9, 2);
            g_helperQuit = true;
            LightEvent_Signal(&g_go);
            threadJoin(g_helper, U64_MAX);
            threadFree(g_helper);
        } else
            out("no second thread (core 1)");
        /* the same speed numbers for a 1.5 min recording */
        out("1.5 min = 2700 frames: multiply ms/frame by 2.7 for seconds");
    }
    out("done. START: exit");
    if (g_log)
        fclose(g_log);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
