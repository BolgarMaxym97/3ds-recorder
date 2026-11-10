/*
 * r3dhuff — rewrites a recording's XR32 frames (R3DY) the way a newer recorder stores them, for
 * tests (the result must decode to the same pictures: tools/r3dconv, tests/web/compare.mjs):
 *   r3dhuff <in.r3d> <out.r3d>           Huffman stage (R3DH), the recorder's rule (sysmod PackStage)
 *   r3dhuff --hybrid <in.r3d> <out.r3d>  Q565 for frames that changed all over (R3DQ), XR32 for the
 *                                        rest, keyframes as the recorder makes them (sysmod EncodeFrame)
 */
#include "codec.h"
#include "huff.h"
#include "r3d_format.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYFRAME_EVERY 60u

int main(int argc, char **argv)
{
    const int hybrid = argc == 4 && strcmp(argv[1], "--hybrid") == 0;
    if (argc != 3 && !hybrid) {
        fprintf(stderr, "usage: r3dhuff [--hybrid] <in.r3d> <out.r3d>\n");
        return 2;
    }
    FILE *f = fopen(argv[argc - 2], "rb"), *o = fopen(argv[argc - 1], "wb");
    if (!f || !o) {
        fprintf(stderr, "cannot open files\n");
        return 1;
    }
    R3dFileHeader fh;
    if (fread(&fh, 1, sizeof(fh), f) != sizeof(fh) || fh.magic != R3D_MAGIC_FILE) {
        fprintf(stderr, "not a recording\n");
        return 1;
    }
    fh.version = R3D_VERSION;
    fwrite(&fh, 1, sizeof(fh), o);
    static uint8_t in[1 << 21], out[1 << 21];
    static uint16_t pic[2][400 * 240], ref[2][400 * 240];   /* decoded picture; the new encoder's reference */
    static uint32_t frames[2];
    static const uint8_t zero[16];
    uint64_t rawBytes = 0, outBytes = 0;
    uint32_t intra = 0, total = 0;
    R3dRecHeader h;
    while (fread(&h, 1, sizeof(h), f) == sizeof(h)) {
        const int delta = h.magic == R3D_MAGIC_XFRAME || h.magic == R3D_MAGIC_YFRAME || h.magic == R3D_MAGIC_HFRAME ||
                          h.magic == R3D_MAGIC_QFRAME;
        const uint32_t stored = delta ? (h.len + 15u) & ~15u : h.len;
        if (stored > sizeof(in) || fread(in, 1, stored, f) != stored)
            break;
        size_t e = 0;
        if (h.magic == R3D_MAGIC_YFRAME && hybrid) {
            const int sc = h.screen & 1, key = (h.screen & R3D_SCREEN_KEY) != 0;
            const uint32_t cols = h.cols, n = cols * 240u;
            if (n > 400u * 240u || codec_xr32_decode(in, h.len, pic[sc], n, key) != 0)
                break;
            rawBytes += h.len;
            total++;
            const uint32_t cap = (uint32_t)codec_xr32_max_size(n);
            const int first = frames[sc] == 0;
            if (!first && codec_changed_pct(ref[sc], (const uint8_t *)pic[sc], cols, 240, 480) > Q565_CHANGED_PCT &&
                (e = codec_q565_encode((const uint8_t *)pic[sc], cols, 240, 480, out, cap)) != 0) {
                h.magic = R3D_MAGIC_QFRAME;
                h.screen = (uint8_t)(sc | R3D_SCREEN_KEY);
                memcpy(ref[sc], pic[sc], n * 2);
                frames[sc] = 1;
                intra++;
            } else {
                const int k = frames[sc] % KEYFRAME_EVERY == 0;
                e = codec_xr32_encode_strided_v(ref[sc], (const uint8_t *)pic[sc], cols, 240, 480, 1, k, 1, out);
                h.magic = R3D_MAGIC_YFRAME;
                h.screen = (uint8_t)(sc | (k ? R3D_SCREEN_KEY : 0));
                frames[sc]++;
            }
            outBytes += e;
        } else if (h.magic == R3D_MAGIC_YFRAME && h.len >= 4096u) {
            e = huff_encode(in, h.len, out, h.len - h.len / 32u);
            rawBytes += h.len;
            outBytes += e ? e : h.len;
            if (e)
                h.magic = R3D_MAGIC_HFRAME;
        }
        if (e) {
            h.len = (uint32_t)e;
            fwrite(&h, 1, sizeof(h), o);
            fwrite(out, 1, e, o);
            fwrite(zero, 1, ((e + 15u) & ~15u) - e, o);
            continue;
        }
        fwrite(&h, 1, sizeof(h), o);
        fwrite(in, 1, stored, o);
        if (h.magic == R3D_MAGIC_END)
            break;
    }
    fclose(f);
    fclose(o);
    fprintf(stderr, "video frames rewritten: %.1f -> %.1f MB (%.0f%%)%s\n", rawBytes / 1e6, outBytes / 1e6,
            rawBytes ? 100.0 * outBytes / rawBytes : 0, hybrid ? "" : " (XR32 frames of 4 KB+)");
    if (hybrid)
        fprintf(stderr, "Q565: %u of %u frames\n", intra, total);
    return 0;
}
