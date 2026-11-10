#ifndef HUFF_H
#define HUFF_H
/*
 * Huffman stage over a compressed frame (R3DH records): the bytes of an XR32
 * stream, coded with two canonical Huffman codes, one for the even and one for
 * the odd bytes (the low and high bytes of its 16-bit words, which hold
 * different things: RGB565 XOR bits split 3+5 / 5+3, token counts). On O3DS
 * recordings (2026-11-09) this took XR32 frames to 60-72% of their size in 3D
 * games and ~45% in menus.
 *
 * Stream:
 *   u32 rawLen                 bytes it decodes to
 *   u8  lens[2][128]           code length per symbol, 4 bits each (symbol 2k in the
 *                              low nibble), 0 = unused, at most HUFF_MAX_BITS
 *   u16 bits[]                 codes, least significant bit first, little-endian
 * The encoded size is a multiple of 4.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUFF_MAX_BITS 12u
#define HUFF_HEADER   (4u + 256u)

/* Encodes `n` bytes into `out` (room for `cap` bytes). Returns the encoded size, or 0 when it
   would not fit in `cap` (the caller then stores the input as it is). Not reentrant: its
   tables are static (the recorder's converter stack is 8 KB). */
size_t huff_encode(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

/* Bytes an encoded stream decodes to, or 0 when it is too short to hold a header. */
size_t huff_raw_size(const uint8_t *in, size_t in_size);

/* Decoding tables (16 KB): one per decoding thread, filled for each stream. */
typedef struct {
    uint16_t t[2][1u << HUFF_MAX_BITS];   /* symbol << 4 | code length, 0 = no code */
} HuffTables;

/* Decodes into `out` (huff_raw_size bytes, at most `cap`). Returns 0, or -1 on a malformed
   stream. */
int huff_decode(const uint8_t *in, size_t in_size, uint8_t *out, size_t cap, HuffTables *tables);

/* An R3DH frame (Huffman over XR32) decoded straight into `frame` (n RGB565 words), as
   codec_xr32_decode does for R3DY: `frame` holds the previous picture unless keyframe != 0.
   Returns 0, or -1 on a malformed stream. */
int huff_xr32_decode(const uint8_t *in, size_t in_size, uint16_t *frame, size_t n, int keyframe, HuffTables *tables);

#ifdef __cplusplus
}
#endif

#endif
