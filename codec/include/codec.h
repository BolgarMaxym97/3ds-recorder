#ifndef CODEC_H
#define CODEC_H

/*
 * Lossless "Lite" frame codecs for RGB565 frames (T6). Portable C99: the same
 * code runs on the host (codecsim, compression ratio and round-trip checks)
 * and on the 3DS (codec cost on ARM11).
 *
 * Frames are arrays of `n` RGB565 words (any layout; the recorder stores them
 * column-major). Every codec encodes `cur` relative to `prev`; pass prev = NULL
 * for a keyframe.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * XOR-RLE: XOR with the previous frame, then a stream of u16 tokens:
 *   t < 0x8000 : t + 1 zero words (unchanged pixels)
 *   t >= 0x8000: (t & 0x7FFF) + 1 literal XOR words follow
 * Worst case: n words + one token per 32768 words.
 */
size_t codec_xrle_max_size(size_t n);
size_t codec_xrle_encode(const uint16_t *prev, const uint16_t *cur, size_t n, uint8_t *out);
/* Returns 0 on success, -1 on a malformed stream. `frame` holds prev on entry (or anything for a keyframe). */
int    codec_xrle_decode(const uint8_t *in, size_t in_size, uint16_t *frame, size_t n, int keyframe);

/*
 * Fused XOR-RLE for the recorder: reads RGB565 straight from the DMA'd
 * framebuffer and updates the packed previous frame in place.
 *   src:     src_cols columns, src_stride bytes apart, each src_rows words
 *   step:    1 = every pixel, 2 = every 2nd pixel of every 2nd column
 *   prev:    packed (src_cols/step) x (src_rows/step) words; on return it
 *            holds the current frame. Ignored as a reference when keyframe != 0.
 * Output is the same stream codec_xrle_decode reads (sizes may differ from
 * codec_xrle_encode by a few bytes at 32768-word token boundaries).
 */
size_t codec_xrle_encode_strided(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                                 uint32_t src_stride, uint32_t step, int keyframe, uint8_t *out);

/*
 * XR32 (bench v0.11): XOR-RLE over pixel pairs, built for ARM11 speed.
 * The frame is n RGB565 words (n even), read as n/2 little-endian 32-bit
 * pairs (word 2k in the low half). Stream of u16 tokens:
 *   t < 0x8000 : t + 1 unchanged pairs
 *   t >= 0x8000: (t & 0x7FFF) + 1 literal pairs follow, each as two u16
 *                (low word first) = cur ^ prev
 * A literal run ends at the first unchanged pair: breaking it costs a zero
 * token + a new literal token (2 words), the same as keeping the pair as a
 * literal, so no look-ahead is needed. A keyframe is a delta against an
 * all-zero frame.
 */
size_t codec_xr32_max_size(size_t n);
int    codec_xr32_decode(const uint8_t *in, size_t in_size, uint16_t *frame, size_t n, int keyframe);

/*
 * Fused XR32 encoder: src as in codec_xrle_encode_strided (src_rows / step
 * must be even, src 4-byte aligned, src_stride a multiple of 4). `prev` is the
 * packed previous frame (4-byte aligned); only changed pairs are written back.
 * keyframe != 0 clears prev first. `out` must be 2-byte aligned.
 */
size_t codec_xr32_encode_strided(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                                 uint32_t src_stride, uint32_t step, int keyframe, uint8_t *out);
/* Same stream; variant = memory hint at step 1: 0 none, 1 PLD, 2 touch-ahead loads (bench picks the fastest). */
#define CODEC_XR32_VARIANTS 3
size_t codec_xr32_encode_strided_v(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                                   uint32_t src_stride, uint32_t step, int keyframe, int variant, uint8_t *out);

/*
 * Dirty tiles: the frame is `cols` columns of `rows` words (rows and cols
 * multiples of 8). Output: bitmap of changed 8x8 tiles, then the raw words of
 * each changed tile in bitmap order.
 */
size_t codec_tiles_max_size(uint32_t cols, uint32_t rows);
size_t codec_tiles_encode(const uint16_t *prev, const uint16_t *cur, uint32_t cols, uint32_t rows, uint8_t *out);
int    codec_tiles_decode(const uint8_t *in, size_t in_size, uint16_t *frame, uint32_t cols, uint32_t rows);

#ifdef __cplusplus
}
#endif

#endif /* CODEC_H */
