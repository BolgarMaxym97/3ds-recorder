/* Button presses kept with a recording, and the overlay that shows them (inputs.h). */
#include "inputs.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ the samples */

void input_track_init(InputTrack *t)
{
    memset(t, 0, sizeof(*t));
    t->now.tx = t->now.ty = R3D_TOUCH_NONE;
}

uint32_t input_track_add(InputTrack *t, const R3dRecHeader *h, const uint8_t *payload, uint32_t len)
{
    const uint32_t n = h->cols;
    if (h->magic != R3D_MAGIC_INPUT || n == 0 || n > R3D_INPUT_MAX || (uint64_t)n * sizeof(R3dInputSample) > len)
        return 0;
    for (uint32_t i = 0; i < n; i++) {
        R3dInputSample s;
        memcpy(&s, payload + i * sizeof(s), sizeof(s));
        if (t->head - t->tail >= INPUT_QUEUE)   /* full: the oldest one is the state now */
            t->now = t->q[t->tail++ % INPUT_QUEUE];
        InputState *q = &t->q[t->head++ % INPUT_QUEUE];
        q->us = h->timeUs + (uint32_t)s.dtMs * 1000u;
        q->keys = s.keys & 0x0FFFu;
        q->cx = s.cx, q->cy = s.cy;
        q->tx = s.tx < 160u ? s.tx : R3D_TOUCH_NONE;
        q->ty = s.tx < 160u && s.ty < 240u ? s.ty : R3D_TOUCH_NONE;
        if (q->ty == R3D_TOUCH_NONE)
            q->tx = R3D_TOUCH_NONE;
        q->pad[0] = q->pad[1] = 0;
        t->samples++;
    }
    return n;
}

const InputState *input_track_at(InputTrack *t, uint32_t us)
{
    while (t->tail != t->head && (int32_t)(t->q[t->tail % INPUT_QUEUE].us - us) <= 0)
        t->now = t->q[t->tail++ % INPUT_QUEUE];
    return &t->now;
}

/* ------------------------------------------------------------------ the layout */

void input_layout(InputLayout *l, const InputScreens *s)
{
    memset(l, 0, sizeof(*l));
    l->botShown = s->botX >= 0;
    l->botX = (float)s->botX, l->botY = (float)s->botY;
    if (s->botX >= 40 && s->outW - (s->botX + 320) >= 40) {
        l->strips = 1;
        l->ox = (float)s->botX, l->oy = (float)s->botY;
        return;
    }
    const int sx = s->topX >= 0 ? s->topX : s->botX, sy = s->topX >= 0 ? s->topY : s->botY;
    l->ox = (float)(sx + 6), l->oy = (float)(sy + 240 - 6 - INPUT_PANEL_H);
}

/* ------------------------------------------------------------------ the shapes */

enum { SH_BOX, SH_CROSS };
typedef struct {
    uint8_t kind;
    float cx, cy, hx, hy, r;   /* box: half sizes and corner radius; cross: arm half length, half width */
    float fill[4], stroke[4];  /* straight (not premultiplied) colours; alpha 0: none */
    float sw;                  /* stroke width, inside the edge */
} Shape;

static const float SOFT[4] = {0xC3 / 255.f, 0xC9 / 255.f, 0xD0 / 255.f, 0.92f};
static const float ACCENT[4] = {0xFF / 255.f, 0x4D / 255.f, 0x4F / 255.f, 1.f};
static const float DARK[4] = {0x0E / 255.f, 0x10 / 255.f, 0x14 / 255.f, 0.62f};
static const float HALO[4] = {0x0E / 255.f, 0x10 / 255.f, 0x14 / 255.f, 0.55f};
static const float NONE[4] = {0, 0, 0, 0};

#define MAX_SHAPES 32
typedef struct {
    Shape s[MAX_SHAPES];
    int n;
} Shapes;

static void put(Shapes *o, uint8_t kind, float cx, float cy, float hx, float hy, float r, const float *fill,
                const float *stroke, float sw)
{
    if (o->n >= MAX_SHAPES)
        return;
    Shape *s = &o->s[o->n++];
    s->kind = kind, s->cx = cx, s->cy = cy, s->hx = hx, s->hy = hy, s->r = r, s->sw = sw;
    memcpy(s->fill, fill, sizeof(s->fill));
    memcpy(s->stroke, stroke, sizeof(s->stroke));
}

/* a button: outlined, filled when held */
static void button(Shapes *o, float cx, float cy, float hx, float hy, float r, int held, float sw)
{
    put(o, SH_BOX, cx, cy, hx, hy, r, held ? ACCENT : NONE, held ? ACCENT : SOFT, sw);
}

static void dpad(Shapes *o, float cx, float cy, float u, unsigned keys, float sw)
{
    const float h = u * 0.5f;
    if (keys & R3D_KEY_UP)
        put(o, SH_BOX, cx, cy - u, h, h, 1.f, ACCENT, NONE, 0);
    if (keys & R3D_KEY_DOWN)
        put(o, SH_BOX, cx, cy + u, h, h, 1.f, ACCENT, NONE, 0);
    if (keys & R3D_KEY_LEFT)
        put(o, SH_BOX, cx - u, cy, h, h, 1.f, ACCENT, NONE, 0);
    if (keys & R3D_KEY_RIGHT)
        put(o, SH_BOX, cx + u, cy, h, h, 1.f, ACCENT, NONE, 0);
    put(o, SH_CROSS, cx, cy, u * 1.5f, h, 1.5f, NONE, SOFT, sw);
}

static void stick(Shapes *o, float cx, float cy, float ring, float dot, float travel, const InputState *st, float sw)
{
    put(o, SH_BOX, cx, cy, ring, ring, ring, NONE, SOFT, sw);
    const float dx = st->cx * travel / 127.f, dy = -st->cy * travel / 127.f;
    put(o, SH_BOX, cx + dx, cy + dy, dot, dot, dot, SOFT, NONE, 0);
}

static void abxy(Shapes *o, float cx, float cy, float d, float r, unsigned keys, float sw)
{
    button(o, cx, cy - d, r, r, r, (keys & R3D_KEY_X) != 0, sw);
    button(o, cx - d, cy, r, r, r, (keys & R3D_KEY_Y) != 0, sw);
    button(o, cx + d, cy, r, r, r, (keys & R3D_KEY_A) != 0, sw);
    button(o, cx, cy + d, r, r, r, (keys & R3D_KEY_B) != 0, sw);
}

static void shapes(const InputLayout *l, const InputState *st, Shapes *o)
{
    const unsigned k = st->keys;
    const int touched = st->tx != R3D_TOUCH_NONE;
    o->n = 0;
    if (l->strips) {
        const float sw = 1.3f, y0 = l->oy, lx = l->ox - 20.f, rx = l->ox + 340.f;
        button(o, lx, y0 + 14.f, 14.f, 4.f, 4.f, (k & R3D_KEY_L) != 0, sw);
        stick(o, lx, y0 + 62.f, 13.f, 5.f, 8.f, st, sw);   /* as on the console: the stick above the D-pad */
        dpad(o, lx, y0 + 118.f, 9.f, k, sw);
        button(o, lx, y0 + 170.f, 10.f, 3.5f, 3.5f, (k & R3D_KEY_SELECT) != 0, sw);
        button(o, rx, y0 + 14.f, 14.f, 4.f, 4.f, (k & R3D_KEY_R) != 0, sw);
        abxy(o, rx, y0 + 70.f, 11.f, 5.5f, k, sw);
        button(o, rx, y0 + 170.f, 10.f, 3.5f, 3.5f, (k & R3D_KEY_START) != 0, sw);
    } else {
        const float sw = 1.1f, x0 = l->ox, y0 = l->oy;
        put(o, SH_BOX, x0 + INPUT_PANEL_W / 2.f, y0 + INPUT_PANEL_H / 2.f, INPUT_PANEL_W / 2.f, INPUT_PANEL_H / 2.f, 8.f,
            DARK, NONE, 0);
        button(o, x0 + 19.f, y0 + 7.f, 13.f, 3.f, 3.f, (k & R3D_KEY_L) != 0, sw);
        button(o, x0 + 97.f, y0 + 7.f, 13.f, 3.f, 3.f, (k & R3D_KEY_R) != 0, sw);
        stick(o, x0 + 19.f, y0 + 25.f, 8.f, 3.2f, 5.f, st, sw);
        dpad(o, x0 + 19.f, y0 + 47.f, 6.f, k, sw);
        if (!l->botShown) {
            put(o, SH_BOX, x0 + 58.f, y0 + 27.f, 16.f, 12.f, 2.f, NONE, SOFT, sw);
            if (touched)
                put(o, SH_BOX, x0 + 42.f + (st->tx * 2 + 1) / 10.f, y0 + 15.f + st->ty / 10.f, 2.f, 2.f, 2.f, ACCENT, NONE,
                    0);
        }
        button(o, x0 + 51.f, y0 + 48.f, 6.f, 2.2f, 2.2f, (k & R3D_KEY_SELECT) != 0, sw);
        button(o, x0 + 65.f, y0 + 48.f, 6.f, 2.2f, 2.2f, (k & R3D_KEY_START) != 0, sw);
        abxy(o, x0 + 97.f, y0 + 31.f, 9.f, 4.f, k, sw);
    }
    if (l->botShown && touched) {
        const float x = l->botX + st->tx * 2 + 1.f, y = l->botY + st->ty + 0.5f;
        put(o, SH_BOX, x, y, 8.5f, 8.5f, 8.5f, NONE, HALO, 3.f);
        put(o, SH_BOX, x, y, 7.f, 7.f, 7.f, NONE, ACCENT, 2.f);
        put(o, SH_BOX, x, y, 2.f, 2.f, 2.f, ACCENT, NONE, 0);
    }
}

/* signed distance to a rounded box (negative inside) */
static float sd_box(float px, float py, float hx, float hy, float r)
{
    const float qx = fabsf(px) - hx + r, qy = fabsf(py) - hy + r;
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    const float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

static float sd(const Shape *s, float x, float y)
{
    const float px = x - s->cx, py = y - s->cy;
    if (s->kind == SH_CROSS) {
        const float a = sd_box(px, py, s->hy, s->hx, s->r), b = sd_box(px, py, s->hx, s->hy, s->r);
        return a < b ? a : b;
    }
    return sd_box(px, py, s->hx, s->hy, s->r);
}

static float cover(float d, float px)
{
    const float c = 0.5f - d / px;
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

static void over(float o[4], const float c[4], float a)
{
    /* source-over: c straight, coverage a */
    const float k = c[3] * a;
    if (k <= 0)
        return;
    o[0] = c[0] * k + o[0] * (1 - k);
    o[1] = c[1] * k + o[1] * (1 - k);
    o[2] = c[2] * k + o[2] * (1 - k);
    o[3] = k + o[3] * (1 - k);
}

static void shade(const Shapes *sh, float x, float y, float px, float rgba[4])
{
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    for (int i = 0; i < sh->n; i++) {
        const Shape *s = &sh->s[i];
        const float ex = (s->kind == SH_CROSS ? s->hx : s->hx) + px + 1, ey = (s->kind == SH_CROSS ? s->hx : s->hy) + px + 1;
        if (fabsf(x - s->cx) > ex || fabsf(y - s->cy) > ey)
            continue;
        const float d = sd(s, x, y);
        if (s->fill[3] > 0)
            over(rgba, s->fill, cover(d, px));
        if (s->stroke[3] > 0 && s->sw > 0) {
            const float h = s->sw * 0.5f;
            over(rgba, s->stroke, cover(fabsf(d + h) - h, px));
        }
    }
}

void input_shade(const InputLayout *l, const InputState *st, float x, float y, float px, float rgba[4])
{
    Shapes sh;
    shapes(l, st, &sh);
    shade(&sh, x, y, px, rgba);
}

static int clip(InputRect *r, int w, int h)
{
    if (r->x < 0)
        r->w += r->x, r->x = 0;
    if (r->y < 0)
        r->h += r->y, r->y = 0;
    if (r->x + r->w > w)
        r->w = w - r->x;
    if (r->y + r->h > h)
        r->h = h - r->y;
    return r->w > 0 && r->h > 0;
}

int input_rects(const InputLayout *l, const InputState *st, int outW, int outH, InputRect out[3])
{
    InputRect r[3];
    int n = 0;
    if (l->strips) {
        r[n++] = (InputRect){(int)l->ox - 40, (int)l->oy, 40, 240};
        r[n++] = (InputRect){(int)l->ox + 320, (int)l->oy, 40, 240};
    } else {
        r[n++] = (InputRect){(int)l->ox, (int)l->oy, INPUT_PANEL_W, INPUT_PANEL_H};
    }
    if (l->botShown && st->tx != R3D_TOUCH_NONE) {
        const int x = (int)l->botX + st->tx * 2 + 1, y = (int)l->botY + st->ty;
        r[n++] = (InputRect){x - INPUT_TOUCH_BOX / 2, y - INPUT_TOUCH_BOX / 2, INPUT_TOUCH_BOX, INPUT_TOUCH_BOX};
    }
    int m = 0;
    for (int i = 0; i < n; i++)
        if (clip(&r[i], outW, outH))
            out[m++] = r[i];
    return m;
}

static uint8_t u8(float v)
{
    const float c = v * 255.f + 0.5f;
    return (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
}

void input_draw_rgb24(uint8_t *rgb, int w, int h, const InputLayout *l, const InputState *st)
{
    Shapes sh;
    shapes(l, st, &sh);
    InputRect r[3];
    const int n = input_rects(l, st, w, h, r);
    for (int i = 0; i < n; i++)
        for (int y = r[i].y; y < r[i].y + r[i].h; y++)
            for (int x = r[i].x; x < r[i].x + r[i].w; x++) {
                float c[4];
                shade(&sh, x + 0.5f, y + 0.5f, 1.f, c);
                if (c[3] <= 0)
                    continue;
                uint8_t *p = rgb + ((size_t)y * w + x) * 3;
                for (int k = 0; k < 3; k++)
                    p[k] = u8(c[k] + p[k] / 255.f * (1 - c[3]));
            }
}

void input_cache_init(InputCache *c, void *mem)
{
    memset(c, 0, sizeof(*c));
    c->mem = (uint8_t *)mem;
}

static int same(const InputState *a, const InputState *b)
{
    return a->keys == b->keys && a->cx == b->cx && a->cy == b->cy && a->tx == b->tx && a->ty == b->ty;
}

static void cache_fill(InputCache *c, const InputLayout *l, const InputState *st, int w, int h)
{
    Shapes sh;
    shapes(l, st, &sh);
    c->n = input_rects(l, st, w, h, c->r);
    uint8_t *m = c->mem;
    for (int i = 0; i < c->n; i++) {
        c->rgba[i] = m;
        for (int y = 0; y < c->r[i].h; y++)
            for (int x = 0; x < c->r[i].w; x++, m += 4) {
                float v[4];
                shade(&sh, c->r[i].x + x + 0.5f, c->r[i].y + y + 0.5f, 1.f, v);
                m[0] = u8(v[0]), m[1] = u8(v[1]), m[2] = u8(v[2]), m[3] = u8(v[3]);
            }
    }
    c->key = *st;
    c->valid = 1;
}

/* BT.601 limited range of a premultiplied colour: the value to add once the picture's is scaled by 1 - a */
static int y_of(int r, int g, int b, int a) { return ((66 * r + 129 * g + 25 * b + 128) >> 8) + (16 * a + 127) / 255; }
static int u_of(int r, int g, int b, int a) { return ((-38 * r - 74 * g + 112 * b + 128) >> 8) + (128 * a + 127) / 255; }
static int v_of(int r, int g, int b, int a) { return ((112 * r - 94 * g - 18 * b + 128) >> 8) + (128 * a + 127) / 255; }

static uint8_t mix(int pic, int add, int a)
{
    const int v = (pic * (255 - a) + 127) / 255 + add;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void input_draw_yuv420(uint8_t *Y, uint8_t *U, uint8_t *V, int w, int h, const InputLayout *l, const InputState *st,
                       InputCache *c)
{
    if (!c->valid || !same(&c->key, st))
        cache_fill(c, l, st, w, h);
    for (int i = 0; i < c->n; i++) {
        const InputRect *r = &c->r[i];
        const uint8_t *m = c->rgba[i];
        for (int y = 0; y < r->h; y++)
            for (int x = 0; x < r->w; x++) {
                const uint8_t *p = m + (y * r->w + x) * 4;
                if (!p[3])
                    continue;
                uint8_t *d = Y + (size_t)(r->y + y) * w + r->x + x;
                *d = mix(*d, y_of(p[0], p[1], p[2], p[3]), p[3]);
            }
        /* chroma: each 2 x 2 block inside the rect, the four pixels averaged */
        const int x0 = r->x & ~1, y0 = r->y & ~1;
        for (int by = y0; by < r->y + r->h; by += 2)
            for (int bx = x0; bx < r->x + r->w; bx += 2) {
                int sr = 0, sg = 0, sb = 0, sa = 0;
                for (int k = 0; k < 4; k++) {
                    const int px = bx + (k & 1) - r->x, py = by + (k >> 1) - r->y;
                    if (px < 0 || py < 0 || px >= r->w || py >= r->h)
                        continue;
                    const uint8_t *p = m + (py * r->w + px) * 4;
                    sr += p[0], sg += p[1], sb += p[2], sa += p[3];
                }
                if (!sa || bx + 1 >= w || by + 1 >= h)
                    continue;
                sr /= 4, sg /= 4, sb /= 4, sa /= 4;
                const size_t ci = (size_t)(by / 2) * (w / 2) + bx / 2;
                U[ci] = mix(U[ci], u_of(sr, sg, sb, sa), sa);
                V[ci] = mix(V[ci], v_of(sr, sg, sb, sa), sa);
            }
    }
}
