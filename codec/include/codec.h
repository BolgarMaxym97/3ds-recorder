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
/*
 * The same stream from a framebuffer in another format (BGR8, RGBA8...): each column is converted
 * into a 240-pixel buffer that stays in the cache and encoded from there, instead of converting the
 * whole frame first (a pass over the frame through main memory). conv(dst, column, rows, fmt)
 * converts one column (convert.h fb_column_to_rgb565). rows <= 256, even.
 */
size_t codec_xr32_encode_conv(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                              uint32_t src_stride, uint32_t fmt,
                              int (*conv)(uint16_t *dst, const uint8_t *src, uint32_t fmt, uint32_t rows),
                              int keyframe, uint8_t *out);
/* Same stream; variant = memory hint at step 1: 0 none, 1 PLD, 2 touch-ahead loads (bench picks the fastest). */
#define CODEC_XR32_VARIANTS 3
size_t codec_xr32_encode_strided_v(uint16_t *prev, const uint8_t *src, uint32_t src_cols, uint32_t src_rows,
                                   uint32_t src_stride, uint32_t step, int keyframe, int variant, uint8_t *out);

/*
 * Q565 (R3DQ records): one frame on its own, no reference, for pictures that change all over
 * (3D with a moving camera), where XR32 against the last frame saved almost nothing: Mario Kart
 * 7's top screen took 69% of its XR32 bytes with Q565 used for frames where over 40% of the
 * pixels changed, Smash 77%, menus 52% (O3DS recordings, 2026-11-09). A byte format after the
 * public QOI idea, made for RGB565; pixels in stored order (column by column, `rows` each).
 * Ops, by the first byte:
 *   00iiiiii           INDEX  the colour in slot i of the 64 recently seen
 *   01rrggbb           DIFF   r, g, b differ from the previous pixel by -2..1 (each + 2)
 *   10gggggg rrrrbbbb  LUMA   g differs by -32..31 (+ 32); r and b by floor(dg / 2) + (-8..7) (+ 8)
 *   11rrrrrr           RUN    the previous pixel repeated r + 1 times (r 0..61)
 *   11111110 lo hi     RAW    the pixel itself
 * Differences wrap within a channel (5 bits r and b, 6 bits g). The previous pixel starts as 0
 * and the table as all 0; a pixel from DIFF, LUMA or RAW goes into slot
 * (pixel * 0x9E3779B1) >> 26 of the table (32-bit), INDEX and RUN leave it.
 */
/* Encodes cols x rows RGB565 pixels (column c at src + c * stride bytes, 2-byte aligned) into
   `out`; 0 when it would not fit in `cap` bytes. */
size_t codec_q565_encode(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint8_t *out,
                         size_t cap);
#ifdef __3DS__
/* The same in ARM assembly for the ARM11 (codec/source/q565_arm.s): the same bytes. */
size_t codec_q565_encode_arm(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint8_t *out,
                             size_t cap);
#endif
/* Decodes into `frame` (n words, packed). Returns 0, or -1 on a malformed stream. */
int codec_q565_decode(const uint8_t *in, size_t in_size, uint16_t *frame, size_t n);

/* Percent of sampled pixels that differ from `prev` (packed, cols x rows): one in 8 rows of one
   in 4 columns. The recorder codes a frame as Q565 above Q565_CHANGED_PCT. */
#define Q565_CHANGED_PCT 40u
uint32_t codec_changed_pct(const uint16_t *prev, const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride);

/*
 * C420 (R3DC records): one frame on its own at a fixed half of its RGB565 size, for pictures that
 * change all over (3D with a moving camera), where the lossless codecs cannot keep up with the SD
 * card on an Old 3DS: XR32 kept 98% of Mario Kart 7's top screen in a race, and the codecs that pack
 * such frames (Q565, Huffman) cost more processor time than the game leaves (O3DS, 2026-11-09).
 * Lossy the way video is (4:2:0): brightness for every pixel, one colour for a 2x2 block. No
 * reference frame, no tables, no branches on the data.
 *
 * Per block of 2 columns x 2 rows (columns c, c + 1; rows r, r + 1), one little-endian 32-bit word,
 * column pairs in order, row pairs in order within them:
 *   bits  0-4   y of (c, r)        bits  5-9   y of (c, r + 1)
 *   bits 10-14  y of (c + 1, r)    bits 15-19  y of (c + 1, r + 1)
 *   bits 20-25  co + 32            bits 26-31  cg + 32
 * With r5, g6, b5 the pixel's channels and s = r5 + g6 + b5:
 *   y  = (s + o) >> 2, o = 0, 2, 2, 1 for the four pixels in the order above (ordered dither; y <= 31)
 *   co = (sum of (r5 - b5) over the block + 2) >> 2          (-31..31)
 *   cg = (sum of (2 * g6 - s) over the block + 3) >> 3        (-31..31)
 * Decoding: s' = 4 * y + 2 - o, t = s' - 2 * cg,
 *   r5 = clamp((t + 2 * co + 2) >> 2), b5 = clamp((t - 2 * co + 2) >> 2), g6 = clamp((s' + 2 * cg + 1) >> 1).
 * cols and rows are even. Size: cols * rows bytes.
 */
static inline size_t codec_c420_size(uint32_t cols, uint32_t rows) { return (size_t)cols * rows; }
/* RGB565 source: column c at src + c * stride bytes (4-byte aligned, stride a multiple of 4);
   out 4-byte aligned. Returns the size. */
size_t codec_c420_encode(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint8_t *out);
/* Another source format: columns converted two at a time into the cache (conv as in
   codec_xr32_encode_conv). rows <= 256. Returns the size, 0 if conv fails. */
size_t codec_c420_encode_conv(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint32_t fmt,
                              int (*conv)(uint16_t *dst, const uint8_t *src, uint32_t fmt, uint32_t rows),
                              uint8_t *out);
/* A source of 8 bits a channel, straight from its bytes (no RGB565 on the way: faster, and the
   brightness and colour come from all 8 bits): bpp 3 = BGR8 (bytes B, G, R), bpp 4 = RGBA8 (bytes
   A, B, G, R). HOME Menu and many games draw in BGR8. Returns the size. */
size_t codec_c420_encode_888(const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride, uint32_t bpp,
                             uint8_t *out);
/* Decodes into `frame` (cols x rows words, packed column by column). 0, or -1 on a wrong size. */
int codec_c420_decode(const uint8_t *in, size_t in_size, uint16_t *frame, uint32_t cols, uint32_t rows);
/* The same with column c at frame + c * stride words (the player's textures: no copy). */
int codec_c420_decode_strided(const uint8_t *in, size_t in_size, uint16_t *frame, uint32_t cols, uint32_t rows,
                              uint32_t stride);

/*
 * How much of the picture changed since the last call, in percent, from a sample (one pixel in 8
 * rows of one column in 4, `bpp` bytes each, any format): the recorder's choice between XR32 and
 * C420 without a reference frame. `samples` holds CODEC_SAMPLES(cols, rows) words between calls
 * and is updated. 100 when `first` is set.
 */
#define CODEC_SAMPLES(cols, rows) ((((cols) + 3u) / 4u) * (((rows) + 4u) / 8u))
uint32_t codec_sample_changed(uint32_t *samples, const uint8_t *src, uint32_t cols, uint32_t rows, uint32_t stride,
                              uint32_t bpp, int first);

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
