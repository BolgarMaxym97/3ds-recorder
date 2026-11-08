/*
 * Host twin of encbench's H.264 case: raw RGB24 400x480 frames on stdin (from build/r3dconv)
 * -> the same minih264e settings as on the console -> Annex B .h264 file, to judge the quality.
 * Build: cc -O2 -I../../third_party/minih264 host_h264.c -o ../../build/host_h264
 * Usage: build/r3dconv rec.r3d 30 | build/host_h264 out.h264 [speed qp_min qp_max kbit/s gop]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* plain C, like the console (no NEON there): hide the host's SIMD from minih264e */
#undef __ARM_NEON
#undef __aarch64__
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

#define W 400
#define H 480

static unsigned char g_rgb[W * H * 3], g_yuv[W * H * 3 / 2];

static void to_yuv(void) /* BT.601 limited range, 4:2:0: same as encbench */
{
    unsigned char *Y = g_yuv, *U = g_yuv + W * H, *V = U + W * H / 4;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const unsigned char *p = g_rgb + (y * W + x) * 3;
            Y[y * W + x] = (unsigned char)((66 * p[0] + 129 * p[1] + 25 * p[2] + 128 + 4096) >> 8);
        }
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 2) {
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                const unsigned char *p = g_rgb + ((y + (k >> 1)) * W + x + (k & 1)) * 3;
                r += p[0];
                g += p[1];
                b += p[2];
            }
            r /= 4;
            g /= 4;
            b /= 4;
            U[(y / 2) * (W / 2) + x / 2] = (unsigned char)((-38 * r - 74 * g + 112 * b + 128 + 32768) >> 8);
            V[(y / 2) * (W / 2) + x / 2] = (unsigned char)((112 * r - 94 * g - 18 * b + 128 + 32768) >> 8);
        }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: r3dconv rec.r3d 30 | %s out.h264 [speed]\n", argv[0]);
        return 1;
    }
    FILE *out = fopen(argv[1], "wb");
    if (!out)
        return 1;
    H264E_create_param_t cp;
    memset(&cp, 0, sizeof(cp));
    cp.width = W;
    cp.height = H;
    cp.gop = argc > 6 ? atoi(argv[6]) : 60;
    cp.const_input_flag = 1;
    cp.max_threads = 1;
    int sp = 0, ss = 0;
    if (H264E_sizeof(&cp, &sp, &ss))
        return 1;
    H264E_persist_t *enc = malloc(sp);
    H264E_scratch_t *scratch = malloc(ss);
    if (!enc || !scratch || H264E_init(enc, &cp))
        return 1;
    H264E_run_param_t run;
    memset(&run, 0, sizeof(run));
    run.encode_speed = argc > 2 ? atoi(argv[2]) : 9;
    run.qp_min = argc > 3 ? atoi(argv[3]) : 20;
    run.qp_max = argc > 4 ? atoi(argv[4]) : 38;
    run.desired_frame_bytes = (argc > 5 ? atoi(argv[5]) : 1500) * 1000 / 8 / 30;
    long frames = 0, bytes = 0;
    while (fread(g_rgb, 1, sizeof(g_rgb), stdin) == sizeof(g_rgb)) {
        to_yuv();
        H264E_io_yuv_t io = {{g_yuv, g_yuv + W * H, g_yuv + W * H + W * H / 4}, {W, W / 2, W / 2}};
        unsigned char *coded = NULL;
        int size = 0;
        if (H264E_encode(enc, scratch, &run, &io, &coded, &size))
            return 1;
        fwrite(coded, 1, (size_t)size, out);
        frames++;
        bytes += size;
    }
    fclose(out);
    fprintf(stderr, "%ld frames, %.2f MB\n", frames, bytes / 1048576.0);
    return 0;
}
