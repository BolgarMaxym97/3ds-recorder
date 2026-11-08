/*
 * Host unit tests for the framebuffer -> RGB565 converters.
 */
#include "convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define COLS 4
#define ROWS 4

/* Pure colours in each format, expected RGB565 result. */
static const uint16_t RED565 = 0xF800, GREEN565 = 0x07E0, BLUE565 = 0x001F, WHITE565 = 0xFFFF;

static void test_rgba8(void)
{
    /* stride with 8 bytes of padding per column */
    const uint32_t stride = ROWS * 4 + 8;
    uint8_t src[COLS * (ROWS * 4 + 8)];
    memset(src, 0, sizeof(src));
    uint32_t *col0 = (uint32_t *)(src);
    uint32_t *col1 = (uint32_t *)(src + stride);
    col0[0] = 0xFF0000FFu; /* red */
    col0[1] = 0x00FF00FFu; /* green */
    col0[2] = 0x0000FFFFu; /* blue */
    col0[3] = 0xFFFFFFFFu; /* white */
    col1[0] = 0x0000FFFFu;

    uint16_t dst[COLS * ROWS];
    uint32_t n = fb_to_rgb565(dst, src, FB_RGBA8, COLS, ROWS, stride, 1);
    CHECK(n == COLS * ROWS * 2);
    CHECK(dst[0] == RED565);
    CHECK(dst[1] == GREEN565);
    CHECK(dst[2] == BLUE565);
    CHECK(dst[3] == WHITE565);
    CHECK(dst[4] == BLUE565); /* column 1 starts at stride, packed in output */
}

static void test_bgr8_half(void)
{
    const uint32_t stride = ROWS * 3;
    uint8_t src[COLS * ROWS * 3];
    memset(src, 0, sizeof(src));
    /* column 0, pixel 0 = red; column 0, pixel 2 = green; column 2, pixel 0 = blue */
    src[2] = 0xFF;
    src[2 * 3 + 1] = 0xFF;
    src[2 * stride + 0] = 0xFF;

    uint16_t dst[4];
    uint32_t n = fb_to_rgb565(dst, src, FB_BGR8, COLS, ROWS, stride, 2);
    CHECK(n == 2 * 2 * 2);
    CHECK(dst[0] == RED565);
    CHECK(dst[1] == GREEN565);
    CHECK(dst[2] == BLUE565);
    CHECK(dst[3] == 0);
}

static void test_16bit_formats(void)
{
    const uint32_t stride = ROWS * 2;
    uint16_t src[COLS * ROWS];
    uint16_t dst[COLS * ROWS];

    for (int i = 0; i < COLS * ROWS; i++) {
        src[i] = (uint16_t)(i * 2654435761u);
    }
    CHECK(fb_to_rgb565(dst, (const uint8_t *)src, FB_RGB565, COLS, ROWS, stride, 1) == sizeof(dst));
    CHECK(memcmp(dst, src, sizeof(dst)) == 0);

    src[0] = 0xF801;          /* RGB5A1 red, alpha 1 */
    src[1] = 0x07C1;          /* RGB5A1 green */
    src[2] = 0x003F;          /* RGB5A1 blue */
    fb_to_rgb565(dst, (const uint8_t *)src, FB_RGB5A1, COLS, ROWS, stride, 1);
    CHECK(dst[0] == RED565);
    CHECK(dst[1] == GREEN565);
    CHECK(dst[2] == BLUE565);

    src[0] = 0xF00F;          /* RGBA4 red */
    src[1] = 0x0F0F;          /* RGBA4 green */
    src[2] = 0x00FF;          /* RGBA4 blue */
    src[3] = 0xFFFF;          /* RGBA4 white */
    fb_to_rgb565(dst, (const uint8_t *)src, FB_RGBA4, COLS, ROWS, stride, 1);
    CHECK(dst[0] == RED565);
    CHECK(dst[1] == GREEN565);
    CHECK(dst[2] == BLUE565);
    CHECK(dst[3] == WHITE565);
}

static void test_unknown_format(void)
{
    uint16_t dst[4];
    uint8_t src[64] = { 0 };
    CHECK(fb_to_rgb565(dst, src, 7, 2, 2, 8, 1) == 0);
}

/* Real O3DS value: top screen RGB565 with the stereo-3D flag (0x20) set. */
static void test_format_flags_masked(void)
{
    uint16_t src[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    uint16_t dst[4] = { 0 };
    CHECK(fb_to_rgb565(dst, (const uint8_t *)src, 0x22, 1, 4, 8, 1) == 8);
    CHECK(dst[0] == RED565);
    CHECK(dst[3] == WHITE565);
}

static void test_real_sizes(void)
{
    CHECK(fb_out_bytes(400, 240, 1) == 192000);
    CHECK(fb_out_bytes(400, 240, 2) == 48000);
    CHECK(fb_out_bytes(320, 240, 2) == 38400);
}

int main(void)
{
    test_rgba8();
    test_bgr8_half();
    test_16bit_formats();
    test_unknown_format();
    test_format_flags_masked();
    test_real_sizes();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("convert host tests: all passed\n");
    return EXIT_SUCCESS;
}
