/* inputs.c: the R3DI samples in time, the overlay's layout and its pixels. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inputs.h"

static R3dRecHeader rec(uint32_t timeUs, uint16_t n)
{
    R3dRecHeader h = {R3D_MAGIC_INPUT, (uint32_t)((n * 8u + 15u) & ~15u), timeUs, R3D_STREAM_INPUT, 0, n};
    return h;
}

static void test_track(void)
{
    InputTrack *t = malloc(sizeof(*t));
    input_track_init(t);
    assert(input_track_at(t, 0)->keys == 0 && input_track_at(t, 0)->tx == R3D_TOUCH_NONE);
    R3dInputSample s[3] = {
        {0, R3D_KEY_A, 10, -20, R3D_TOUCH_NONE, R3D_TOUCH_NONE},
        {16, R3D_KEY_A | R3D_KEY_L | 0xF000u, 0, 0, 80, 120},   /* high bits dropped */
        {40, 0, 127, -127, 200, 5},                            /* tx out of range: not touched */
    };
    R3dRecHeader h = rec(1000000, 3);
    assert(input_track_add(t, &h, (const uint8_t *)s, 32) == 3);
    assert(input_track_at(t, 999999)->keys == 0);
    const InputState *a = input_track_at(t, 1000000);
    assert(a->keys == R3D_KEY_A && a->cx == 10 && a->cy == -20 && a->tx == R3D_TOUCH_NONE);
    a = input_track_at(t, 1016000);
    assert(a->keys == (R3D_KEY_A | R3D_KEY_L) && a->tx == 80 && a->ty == 120);
    a = input_track_at(t, 1039999);
    assert(a->keys == (R3D_KEY_A | R3D_KEY_L));
    a = input_track_at(t, 5000000);
    assert(a->keys == 0 && a->cx == 127 && a->tx == R3D_TOUCH_NONE && a->ty == R3D_TOUCH_NONE);
    /* not an input record, or too short */
    R3dRecHeader bad = rec(0, 3);
    bad.magic = R3D_MAGIC_AUDIO;
    assert(input_track_add(t, &bad, (const uint8_t *)s, 32) == 0);
    h = rec(0, 3);
    assert(input_track_add(t, &h, (const uint8_t *)s, 16) == 0);
    /* a full queue: the oldest becomes the state, nothing is lost out of order */
    input_track_init(t);
    for (uint32_t i = 0; i < INPUT_QUEUE + 10; i++) {
        R3dInputSample one = {0, (uint16_t)(i & 0xFFF), 0, 0, R3D_TOUCH_NONE, R3D_TOUCH_NONE};
        R3dRecHeader hh = rec(i * 1000u, 1);
        assert(input_track_add(t, &hh, (const uint8_t *)&one, 16) == 1);
    }
    assert(t->now.keys == 9);
    assert(input_track_at(t, (INPUT_QUEUE + 9) * 1000u)->keys == ((INPUT_QUEUE + 9) & 0xFFF));
    free(t);
}

static void test_layout(void)
{
    InputLayout l;
    InputScreens both = {400, 480, 0, 0, 40, 240};
    input_layout(&l, &both);
    assert(l.strips && l.ox == 40 && l.oy == 240 && l.botShown);
    InputScreens bottomOnly = {400, 240, -1, -1, 40, 0};
    input_layout(&l, &bottomOnly);
    assert(l.strips && l.oy == 0);
    InputScreens top = {400, 240, 0, 0, -1, -1};
    input_layout(&l, &top);
    assert(!l.strips && !l.botShown && l.ox == 6 && l.oy == 240 - 6 - INPUT_PANEL_H);
    InputScreens side = {720, 240, 0, 0, 400, 0};
    input_layout(&l, &side);
    assert(!l.strips && l.botShown && l.ox == 6);
    InputScreens bottom = {320, 240, -1, -1, 0, 0};
    input_layout(&l, &bottom);
    assert(!l.strips && l.ox == 6);

    InputState st = {0, 0, 0, 0, R3D_TOUCH_NONE, R3D_TOUCH_NONE, {0, 0}};
    InputRect r[3];
    input_layout(&l, &both);
    assert(input_rects(&l, &st, 400, 480, r) == 2);
    assert(r[0].x == 0 && r[0].y == 240 && r[0].w == 40 && r[0].h == 240 && r[1].x == 360);
    st.tx = 0, st.ty = 0;   /* touch in the corner: its box clipped to the picture */
    assert(input_rects(&l, &st, 400, 480, r) == 3);
    assert(r[2].x >= 0 && r[2].y >= 240 - 12 && r[2].w > 0);
}

static void px(const InputLayout *l, const InputState *st, float x, float y, float out[4])
{
    input_shade(l, st, x, y, 1.f, out);
}

static void test_pixels(void)
{
    InputLayout l;
    InputScreens both = {400, 480, 0, 0, 40, 240};
    input_layout(&l, &both);
    InputState st = {0, 0, 0, 0, R3D_TOUCH_NONE, R3D_TOUCH_NONE, {0, 0}};
    float c[4];
    /* A (right of the ABXY diamond: x 380 + 11, y 240 + 70): hollow when up, red when held */
    px(&l, &st, 391.5f, 310.5f, c);
    assert(c[3] < 0.05f);
    st.keys = R3D_KEY_A;
    px(&l, &st, 391.5f, 310.5f, c);
    assert(c[3] > 0.95f && c[0] > 0.95f && c[1] < 0.4f);
    /* its outline, unpressed: grey */
    st.keys = 0;
    px(&l, &st, 380.f + 11.f + 5.f, 310.5f, c);
    assert(c[3] > 0.4f && c[0] > 0.3f && c[0] < 0.95f && c[1] > 0.3f);
    /* the stick's dot follows the stick */
    px(&l, &st, 20.f, 302.f, c);   /* above the D-pad, as on the console */
    assert(c[3] > 0.9f);
    st.cx = 127;
    px(&l, &st, 28.f, 302.f, c);
    assert(c[3] > 0.9f);
    /* a touch: a red ring around the point on the bottom screen */
    st.tx = 80, st.ty = 100;
    px(&l, &st, 40.f + 161.f + 6.f, 340.5f, c);
    assert(c[3] > 0.5f && c[0] > 0.6f);
    /* the game's picture is not touched outside the overlay */
    px(&l, &st, 200.f, 120.f, c);
    assert(c[3] == 0);

    /* RGB24 and YUV: drawn where the overlay is, nothing elsewhere */
    uint8_t *rgb = calloc(400 * 480, 3);
    st.keys = R3D_KEY_B;
    input_draw_rgb24(rgb, 400, 480, &l, &st);
    const uint8_t *b = rgb + ((240 + 81) * 400 + 380) * 3;
    assert(b[0] > 200 && b[1] < 120);
    assert(rgb[(100 * 400 + 200) * 3] == 0);
    free(rgb);

    uint8_t *Y = malloc(400 * 480), *U = malloc(400 * 480 / 4), *V = malloc(400 * 480 / 4);
    memset(Y, 16, 400 * 480), memset(U, 128, 400 * 480 / 4), memset(V, 128, 400 * 480 / 4);
    void *mem = malloc(INPUT_CACHE_BYTES);
    InputCache cache;
    input_cache_init(&cache, mem);
    input_draw_yuv420(Y, U, V, 400, 480, &l, &st, &cache);
    assert(Y[(240 + 81) * 400 + 380] > 60);                 /* red over black: brighter */
    assert(V[((240 + 81) / 2) * 200 + 190] > 180);           /* and red */
    assert(Y[100 * 400 + 200] == 16 && U[50 * 200 + 100] == 128);
    /* the same state again: the cached shading, the same result */
    uint8_t y1 = Y[(240 + 81) * 400 + 380];
    memset(Y, 16, 400 * 480);
    input_draw_yuv420(Y, U, V, 400, 480, &l, &st, &cache);
    assert(Y[(240 + 81) * 400 + 380] == y1);
    /* the corner panel, top screen only */
    InputScreens top = {400, 240, 0, 0, -1, -1};
    input_layout(&l, &top);
    memset(Y, 200, 400 * 240);
    cache.valid = 0;
    input_draw_yuv420(Y, U, V, 400, 240, &l, &st, &cache);
    assert(Y[(240 - 6 - 30) * 400 + 6 + 30] < 200);          /* the dark panel */
    free(Y), free(U), free(V), free(mem);
}

int main(void)
{
    test_track();
    test_layout();
    test_pixels();
    printf("test_inputs: OK\n");
    return 0;
}
