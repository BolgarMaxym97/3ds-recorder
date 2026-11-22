#ifndef INPUTS_H
#define INPUTS_H
/*
 * Button presses kept with a recording (R3DI records, r3d_format.h) and the overlay that shows
 * them on a video. Portable C99, shared by the console's MP4 maker (prep/) and r3dconv; web/inputs.js
 * is the same drawing in JavaScript (tests/web/compare.mjs checks they agree).
 *
 * The overlay is drawn from shapes measured by signed distance (rounded boxes, circles, the D-pad's
 * cross), so it is smooth at any scale:
 *   - a bottom screen with 40 px free on both sides (both screens, 400 x 480; the bottom screen
 *     alone, 400 x 240): the buttons in those strips, nothing of the game covered;
 *   - otherwise a small panel in the bottom-left corner of the top screen (or of the bottom one);
 *   - a touch is a ring on the bottom screen where it was, or a dot on the panel's small screen.
 */
#include <stdint.h>

#include "r3d_format.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t us;           /* recording time it starts at */
    uint16_t keys;         /* R3D_KEY_* */
    int8_t   cx, cy;
    uint8_t  tx, ty;       /* R3D_TOUCH_NONE: not touched */
    uint8_t  pad[2];
} InputState;

/* A reader's stream of samples, read in recording order: samples wait in a queue until the
   picture's time passes them. */
#define INPUT_QUEUE 512u
typedef struct {
    InputState q[INPUT_QUEUE];
    uint32_t head, tail;   /* queued: [tail, head) */
    InputState now;        /* the state at the last time asked */
    uint32_t samples;      /* all samples read */
} InputTrack;

void input_track_init(InputTrack *t);
/* An R3DI record's payload; returns the samples taken (0: not a valid record). */
uint32_t input_track_add(InputTrack *t, const R3dRecHeader *h, const uint8_t *payload, uint32_t len);
/* The state at time `us` (times asked must not go back). */
const InputState *input_track_at(InputTrack *t, uint32_t us);

/* Where the screens are in the output picture (1x pixels); x < 0: the screen is not shown. */
typedef struct {
    int outW, outH;
    int topX, topY;
    int botX, botY;        /* the bottom screen is 320 x 240 */
} InputScreens;

typedef struct {
    int strips;            /* 1: the strips beside the bottom screen; 0: the corner panel */
    float ox, oy;          /* strips: the bottom screen's corner; panel: its corner */
    int botShown;
    float botX, botY;
} InputLayout;

#define INPUT_PANEL_W 116
#define INPUT_PANEL_H 60

void input_layout(InputLayout *l, const InputScreens *s);

/* The areas (1x pixels, clipped to the picture) the overlay can draw on for this state: up to 3. */
typedef struct { int x, y, w, h; } InputRect;
int input_rects(const InputLayout *l, const InputState *st, int outW, int outH, InputRect out[3]);

/* The overlay's colour at a point (1x units; px = the size of an output pixel in them, 1 / scale):
   rgba premultiplied, 0..1. */
void input_shade(const InputLayout *l, const InputState *st, float x, float y, float px, float rgba[4]);

/* Drawn over an RGB24 picture (r3dconv, scale 1). */
void input_draw_rgb24(uint8_t *rgb, int w, int h, const InputLayout *l, const InputState *st);

/* Drawn over a YUV 4:2:0 picture (BT.601 limited range, as prep makes it; w and h even). The shading
   is cached per state: `cache` holds INPUT_CACHE_BYTES. */
#define INPUT_TOUCH_BOX 24   /* the touch ring's square */
#define INPUT_CACHE_BYTES (4u * (2u * 40u * 240u + INPUT_TOUCH_BOX * INPUT_TOUCH_BOX) + 64u)
typedef struct {
    InputState key;
    int valid, n;
    InputRect r[3];
    uint8_t *rgba[3];      /* premultiplied, 0..255, per rect */
    uint8_t *mem;
} InputCache;
void input_cache_init(InputCache *c, void *mem);
void input_draw_yuv420(uint8_t *y, uint8_t *u, uint8_t *v, int w, int h, const InputLayout *l, const InputState *st,
                       InputCache *c);

#ifdef __cplusplus
}
#endif
#endif
