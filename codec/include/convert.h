#ifndef CONVERT_H
#define CONVERT_H

/*
 * Framebuffer -> RGB565 conversion on the CPU (T4, capture method "CPU copy").
 *
 * 3DS framebuffers are stored rotated: memory holds `cols` columns (400 for the
 * top screen, 320 for the bottom), each `rows` (240) pixels long, `stride`
 * bytes apart. The output keeps that layout (plan O-3: no rotation on device),
 * packed with no padding: column c, pixel r -> dst[c * out_rows + r].
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches GSPGPU_FramebufferFormat. */
enum {
    FB_RGBA8  = 0,
    FB_BGR8   = 1,
    FB_RGB565 = 2,
    FB_RGB5A1 = 3,
    FB_RGBA4  = 4,
};

/*
 * Converts a framebuffer to RGB565. step = 1 copies every pixel, step = 2 takes
 * every 2nd pixel of every 2nd column (nearest-neighbour half resolution).
 * Returns the number of bytes written to dst (cols/step * rows/step * 2),
 * or 0 for an unknown format.
 */
uint32_t fb_to_rgb565(uint16_t *dst, const uint8_t *src, uint32_t format,
                      uint32_t cols, uint32_t rows, uint32_t stride, uint32_t step);

static inline uint32_t fb_out_bytes(uint32_t cols, uint32_t rows, uint32_t step)
{
    return (cols / step) * (rows / step) * 2u;
}

#ifdef __cplusplus
}
#endif

#endif /* CONVERT_H */
