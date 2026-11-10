#include "convert.h"

#include <string.h>

static inline uint16_t pack565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* RGBA8: little-endian word 0xRRGGBBAA (bytes in memory: A, B, G, R). */
static void col_rgba8(uint16_t *d, const uint8_t *s, uint32_t rows, uint32_t step)
{
    const uint32_t *p = (const uint32_t *)s;
    for (uint32_t r = 0; r < rows; r += step) {
        uint32_t v = p[r];
        *d++ = pack565(v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF);
    }
}

/* BGR8: bytes in memory B, G, R. */
static void col_bgr8(uint16_t *d, const uint8_t *s, uint32_t rows, uint32_t step)
{
    for (uint32_t r = 0; r < rows; r += step) {
        const uint8_t *p = s + r * 3u;
        *d++ = pack565(p[2], p[1], p[0]);
    }
}

static void col_rgb565(uint16_t *d, const uint8_t *s, uint32_t rows, uint32_t step)
{
    if (step == 1) {
        memcpy(d, s, rows * 2u);
        return;
    }
    const uint16_t *p = (const uint16_t *)s;
    for (uint32_t r = 0; r < rows; r += step) {
        *d++ = p[r];
    }
}

/* RGB5A1: rrrrrgggggbbbbba. */
static void col_rgb5a1(uint16_t *d, const uint8_t *s, uint32_t rows, uint32_t step)
{
    const uint16_t *p = (const uint16_t *)s;
    for (uint32_t r = 0; r < rows; r += step) {
        uint32_t v = p[r];
        uint32_t g5 = (v >> 6) & 0x1F;
        *d++ = (uint16_t)((v & 0xF800) | (((g5 << 1) | (g5 >> 4)) << 5) | ((v >> 1) & 0x1F));
    }
}

/* RGBA4: rrrrggggbbbbaaaa. */
static void col_rgba4(uint16_t *d, const uint8_t *s, uint32_t rows, uint32_t step)
{
    const uint16_t *p = (const uint16_t *)s;
    for (uint32_t r = 0; r < rows; r += step) {
        uint32_t v = p[r];
        uint32_t r4 = v >> 12, g4 = (v >> 8) & 0xF, b4 = (v >> 4) & 0xF;
        *d++ = (uint16_t)((((r4 << 1) | (r4 >> 3)) << 11) | (((g4 << 2) | (g4 >> 2)) << 5) | ((b4 << 1) | (b4 >> 3)));
    }
}

int fb_column_to_rgb565(uint16_t *dst, const uint8_t *src, uint32_t format, uint32_t rows)
{
    switch (format & 7u) {
    case FB_RGBA8:  col_rgba8(dst, src, rows, 1);  return 1;
    case FB_BGR8:   col_bgr8(dst, src, rows, 1);   return 1;
    case FB_RGB565: col_rgb565(dst, src, rows, 1); return 1;
    case FB_RGB5A1: col_rgb5a1(dst, src, rows, 1); return 1;
    case FB_RGBA4:  col_rgba4(dst, src, rows, 1);  return 1;
    default:        return 0;
    }
}

uint32_t fb_to_rgb565(uint16_t *dst, const uint8_t *src, uint32_t format,
                      uint32_t cols, uint32_t rows, uint32_t stride, uint32_t step)
{
    void (*col)(uint16_t *, const uint8_t *, uint32_t, uint32_t);
    /* The framebuffer format register carries flags above bit 2 (e.g. 0x20 on
       the top screen when stereo 3D is on); only bits 0-2 are the pixel format. */
    switch (format & 7u) {
    case FB_RGBA8:  col = col_rgba8;  break;
    case FB_BGR8:   col = col_bgr8;   break;
    case FB_RGB565: col = col_rgb565; break;
    case FB_RGB5A1: col = col_rgb5a1; break;
    case FB_RGBA4:  col = col_rgba4;  break;
    default:        return 0;
    }

    const uint32_t out_rows = rows / step;
    for (uint32_t c = 0; c + step <= cols; c += step) {
        col(dst, src + c * stride, rows, step);
        dst += out_rows;
    }
    return fb_out_bytes(cols, rows, step);
}
