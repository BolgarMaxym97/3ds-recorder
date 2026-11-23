#include "ui.h"

#include "i18n.h"
#include "textbreak.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Drawing primitives for the control app. Taken from 3ds-ua-hub's ui.c (same author, checked on an
 * Old 3DS and in Azahar), with the font switched to Inter and the palette to the dark design.
 * romfs:/font/inter_*.bcfnt: Inter (OFL) at 500 and 700, made by `make fonts` at this em size. */
#define FONT_EM 24.0f
#define LINE_HEIGHT 1.3f
#define TEXT_NUDGE 0.5f
/* All shapes and text share one depth, so later draws cover earlier ones (citro2d tests GEQUAL). */
#define UI_DEPTH 0.5f
#define TEXT_BUF_GLYPHS 4096

/* base: Inter with the Latin and Cyrillic the app's texts need, plus the language names and the
 * first-run language screen in every language. script: the font of a language written otherwise
 * (Japanese, Chinese: Inter + Noto), loaded only while that language is on (ui_set_script). */
static C2D_Font base[2], script[2];
static Script script_on = SCRIPT_LATIN;
static bool base_only; /* the language list: every language's name in the base font */
static C2D_TextBuf text_buf;

static C2D_Font font_of(FontWeight weight) { return !base_only && script[weight] ? script[weight] : base[weight]; }

bool ui_has_glyphs(const char *str)
{
    C2D_Font f = font_of(FONT_REG);
    const FINF_s *info = C2D_FontGetInfo(f);
    for (const u8 *p = (const u8 *)str; *p;) {
        u32 cp = 0;
        const ssize_t n = decode_utf8(&cp, p);
        if (n <= 0)
            return false;
        p += n;
        if (cp >= 0x20 && info && C2D_FontGlyphIndexFromCodePoint(f, cp) == info->alterCharIndex)
            return false;
    }
    return true;
}

/* citro2d sizes a font by its cell height (C2D_Font's textScale = 30 / cellHeight), and mkbcfnt
 * makes the cell as tall as the font's tallest glyph: a font with kanji, or with a few more
 * letters, would come out smaller and sit lower. Every font is drawn as Inter's own cut was
 * (inter_*.bcfnt: cells 45 high, baseline at 34), which the design is measured with. */
#define REF_CELL 45.0f
#define REF_BASELINE 34.0f
static float font_adj(C2D_Font f, float *dy_em)
{
    const FINF_s *fi = C2D_FontGetInfo(f);
    if (!fi || !fi->tglp || !fi->tglp->cellHeight) {
        *dy_em = 0;
        return 1;
    }
    *dy_em = ((float)fi->tglp->baselinePos - REF_BASELINE) * 30.0f / REF_CELL;
    return fi->tglp->cellHeight / REF_CELL;
}

/* A character's advance at FONT_EM, cached: the font's own code point lookup can walk a long list
 * (Japanese, Chinese), and word wrap asks for every character of a paragraph each frame. */
#define WIDTH_CACHE 512
static struct {
    C2D_Font font;
    float scale;
    u32 cp[WIDTH_CACHE];
    float w[WIDTH_CACHE];
} width_cache[2];

static float char_width(void *ctx, uint32_t cp)
{
    const FontWeight weight = (FontWeight)(uintptr_t)ctx;
    C2D_Font f = font_of(weight);
    typeof(&width_cache[0]) c = &width_cache[weight];
    if (c->font != f) {
        memset(c->cp, 0xFF, sizeof(c->cp));
        c->font = f;
        /* textScale (30 / cellHeight) times font_adj (cellHeight / REF_CELL) */
        c->scale = 30.0f / REF_CELL;
    }
    const unsigned slot = (cp * 2654435761u) >> 23; /* 9 bits */
    if (c->cp[slot] != cp) {
        const charWidthInfo_s *cwi = C2D_FontGetCharWidthInfo(f, C2D_FontGlyphIndexFromCodePoint(f, cp));
        c->cp[slot] = cp;
        c->w[slot] = cwi ? cwi->charWidth * c->scale : 0;
    }
    return c->w[slot];
}

static const char *const SCRIPT_FONT[] = {[SCRIPT_JA] = "ja", [SCRIPT_ZH_HANS] = "zhs", [SCRIPT_ZH_HANT] = "zht"};

bool ui_set_script(Script s)
{
    if (s == script_on)
        return true;
    if (script[FONT_BOLD] && script[FONT_BOLD] != script[FONT_REG])
        C2D_FontFree(script[FONT_BOLD]);
    if (script[FONT_REG])
        C2D_FontFree(script[FONT_REG]);
    script[FONT_REG] = script[FONT_BOLD] = NULL;
    script_on = SCRIPT_LATIN;
    if (s == SCRIPT_LATIN || (unsigned)s >= sizeof(SCRIPT_FONT) / sizeof(*SCRIPT_FONT))
        return true;
    char path[48];
    for (int w = 0; w < 2; w++) {
        snprintf(path, sizeof(path), "romfs:/font/%s_%d.bcfnt", SCRIPT_FONT[s], w == FONT_BOLD ? 700 : 500);
        script[w] = C2D_FontLoad(path);
    }
    if (!script[FONT_REG]) { /* the language's text then shows what the base font has */
        if (script[FONT_BOLD])
            C2D_FontFree(script[FONT_BOLD]);
        script[FONT_BOLD] = NULL;
        return false;
    }
    if (!script[FONT_BOLD]) /* one weight is enough to read */
        script[FONT_BOLD] = script[FONT_REG];
    script_on = s;
    return true;
}

void ui_base_font(bool on) { base_only = on; }

bool ui_init(void)
{
    base[FONT_REG] = C2D_FontLoad("romfs:/font/inter_500.bcfnt");
    base[FONT_BOLD] = C2D_FontLoad("romfs:/font/inter_700.bcfnt");
    text_buf = C2D_TextBufNew(TEXT_BUF_GLYPHS);
    return base[FONT_REG] && base[FONT_BOLD] && text_buf;
}

void ui_exit(void)
{
    ui_set_script(SCRIPT_LATIN);
    C2D_TextBufDelete(text_buf);
    C2D_FontFree(base[FONT_REG]);
    C2D_FontFree(base[FONT_BOLD]);
}

void ui_frame_begin(void)
{
    C2D_TextBufClear(text_buf);
}

/* ---------------------------------------------------------------- text */

static bool parse(C2D_Text *text, FontWeight weight, const char *str)
{
    if (!C2D_TextFontParse(text, font_of(weight), text_buf, str))
        return false;
    C2D_TextOptimize(text);
    return true;
}

/* summed from the font's advances (what citro2d lays text out with), no parsing */
float ui_text_width(float px, FontWeight weight, const char *str)
{
    return text_width(str, char_width, (void *)(uintptr_t)weight) * px / FONT_EM;
}

float ui_text(float x, float y, float px, FontWeight weight, u32 color, const char *str)
{
    C2D_Text text;
    float w = 0, h = 0, dy = 0;
    const float s = px / FONT_EM * font_adj(font_of(weight), &dy);
    if (!parse(&text, weight, str))
        return 0;
    C2D_TextGetDimensions(&text, s, s, &w, &h);
    /* one depth for everything (UI_DEPTH): text at 0.5 over shapes at 0.3 won the depth test
       against a scrim drawn later, and the white Start button showed through behind its label */
    C2D_DrawText(&text, C2D_WithColor, x, y + TEXT_NUDGE * px / 12 - dy * px / FONT_EM, UI_DEPTH, s, s, color);
    return w;
}

float ui_text_center(float cx, float y, float px, FontWeight weight, u32 color, const char *str)
{
    float w = ui_text_width(px, weight, str);
    return ui_text(cx - w / 2, y, px, weight, color, str);
}

float ui_text_right(float rx, float y, float px, FontWeight weight, u32 color, const char *str)
{
    float w = ui_text_width(px, weight, str);
    return ui_text(rx - w, y, px, weight, color, str);
}

/* A last line that the text goes on after: cut to fit with "…". */
static void draw_cut(float x, float y, float width, float px, FontWeight weight, u32 color, const char *line)
{
    char buf[260];
    size_t n = strlen(line);
    for (;;) {
        while (n > 0 && line[n - 1] == ' ')
            n--;
        memcpy(buf, line, n);
        strcpy(buf + n, "…");
        if (!n || ui_text_width(px, weight, buf) <= width)
            break;
        do
            n--;
        while (n > 0 && ((u8)line[n] & 0xC0) == 0x80); /* whole letters only */
    }
    ui_text(x, y, px, weight, color, buf);
}

/* Greedy word wrap (textbreak.c: spaces, '\n', and between Japanese or Chinese characters),
 * measured with the font's advances; draws only if `draw`. At most `max_lines` lines (0: any), the
 * last cut with "…" when there is more. */
static float wrap(float x, float y, float width, float px, FontWeight weight, u32 color, const char *str, bool draw,
                  int max_lines)
{
    char line[256];
    const float line_h = px * LINE_HEIGHT, em_width = width * FONT_EM / px;
    float cy = y;
    int lines = 0;
    const char *p = str;
    while (*p) {
        const char *next;
        size_t n = text_line(p, em_width, char_width, (void *)(uintptr_t)weight, &next, NULL);
        if (n >= sizeof(line))
            n = sizeof(line) - 1;
        const bool more = *next != 0;
        if (draw) {
            memcpy(line, p, n);
            line[n] = 0;
            if (max_lines && lines + 1 == max_lines && more)
                draw_cut(x, cy, width, px, weight, color, line);
            else
                ui_text(x, cy, px, weight, color, line);
        }
        cy += line_h;
        lines++;
        if (max_lines && lines == max_lines)
            break;
        p = next;
    }
    return cy - y;
}

float ui_text_fit(float x, float y, float width, float px, FontWeight weight, u32 color, const char *str)
{
    if (ui_text_width(px, weight, str) <= width)
        return ui_text(x, y, px, weight, color, str);
    draw_cut(x, y, width, px, weight, color, str);
    return width;
}

float ui_text_wrap(float x, float y, float width, float px, FontWeight weight, u32 color, const char *str)
{
    return wrap(x, y, width, px, weight, color, str, true, 0);
}

float ui_text_wrap_lines(float x, float y, float width, float px, FontWeight weight, u32 color, const char *str,
                         int max_lines)
{
    return wrap(x, y, width, px, weight, color, str, true, max_lines);
}

float ui_text_wrap_height(float width, float px, FontWeight weight, const char *str)
{
    return wrap(0, 0, width, px, weight, 0, str, false, 0);
}

/* ---------------------------------------------------------------- shapes */

void ui_rect(float x, float y, float w, float h, u32 color)
{
    C2D_DrawRectSolid(x, y, UI_DEPTH, w, h, color);
}

/* A fan of triangles, not C2D_DrawCircleSolid: Azahar (2126.2, Vulkan and OpenGL alike) draws
 * no citro2d circles at all, and the screens are checked there before a console. */
void ui_circle(float cx, float cy, float r, u32 color)
{
    int segments = r < 4 ? 12 : r < 12 ? 20 : 32;
    float px = cx + r, py = cy;
    for (int i = 1; i <= segments; i++) {
        float a = 2 * (float)M_PI * i / segments;
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        C2D_DrawTriangle(cx, cy, color, px, py, color, x, y, color, UI_DEPTH);
        px = x;
        py = y;
    }
}

/* A quarter of a disc, as a fan: the corners of a rounded rectangle. */
static void quarter(float cx, float cy, float r, float from, u32 color)
{
    int segments = r < 4 ? 3 : r < 12 ? 5 : 8;
    float px = cx + cosf(from) * r, py = cy + sinf(from) * r;
    for (int i = 1; i <= segments; i++) {
        float a = from + (float)M_PI / 2 * i / segments;
        float x = cx + cosf(a) * r, y = cy + sinf(a) * r;
        C2D_DrawTriangle(cx, cy, color, px, py, color, x, y, color, UI_DEPTH);
        px = x;
        py = y;
    }
}

/* Built from pieces that never overlap, so a translucent colour comes out even (the title
 * plate on a banner): full circles at the corners would darken them. */
void ui_rrect(float x, float y, float w, float h, float r, u32 color)
{
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    ui_rect(x + r, y, w - 2 * r, h, color);
    ui_rect(x, y + r, r, h - 2 * r, color);
    ui_rect(x + w - r, y + r, r, h - 2 * r, color);
    quarter(x + r, y + r, r, (float)M_PI, color);
    quarter(x + w - r, y + r, r, (float)M_PI * 1.5f, color);
    quarter(x + w - r, y + h - r, r, 0, color);
    quarter(x + r, y + h - r, r, (float)M_PI / 2, color);
}

/* A surface with a one-pixel lighter edge: dark-on-dark cards need it on the TN panels. */
void ui_card(float x, float y, float w, float h, float r, u32 color)
{
    ui_rrect(x - 0.5f, y - 0.5f, w + 1, h + 1, r + 0.5f, COL_EDGE);
    ui_rrect(x, y, w, h, r, color);
}

void ui_ring(float cx, float cy, float r_out, float r_in, float fraction, u32 color, u32 track)
{
    const int segments = 72;
    for (int pass = 0; pass < 2; pass++) {
        float upto = pass == 0 ? 1.0f : fraction;
        u32 c = pass == 0 ? track : color;
        int n = (int)ceilf(segments * upto);
        for (int i = 0; i < n; i++) {
            float a0 = -M_PI / 2 + 2 * M_PI * i / segments;
            float a1 = -M_PI / 2 + 2 * M_PI * fminf((i + 1.0f) / segments, upto);
            float x0o = cx + cosf(a0) * r_out, y0o = cy + sinf(a0) * r_out;
            float x1o = cx + cosf(a1) * r_out, y1o = cy + sinf(a1) * r_out;
            float x0i = cx + cosf(a0) * r_in, y0i = cy + sinf(a0) * r_in;
            float x1i = cx + cosf(a1) * r_in, y1i = cy + sinf(a1) * r_in;
            C2D_DrawTriangle(x0o, y0o, c, x1o, y1o, c, x0i, y0i, c, UI_DEPTH);
            C2D_DrawTriangle(x1o, y1o, c, x1i, y1i, c, x0i, y0i, c, UI_DEPTH);
        }
    }
}

/* One corner of a frame: a quarter ring from `from` radians, centred on (cx, cy). */
static void corner(float cx, float cy, float r_out, float r_in, float from, u32 color)
{
    const int segments = 8;
    for (int i = 0; i < segments; i++) {
        float a0 = from + (float)M_PI / 2 * i / segments, a1 = from + (float)M_PI / 2 * (i + 1) / segments;
        float x0o = cx + cosf(a0) * r_out, y0o = cy + sinf(a0) * r_out;
        float x1o = cx + cosf(a1) * r_out, y1o = cy + sinf(a1) * r_out;
        float x0i = cx + cosf(a0) * r_in, y0i = cy + sinf(a0) * r_in;
        float x1i = cx + cosf(a1) * r_in, y1i = cy + sinf(a1) * r_in;
        C2D_DrawTriangle(x0o, y0o, color, x1o, y1o, color, x0i, y0i, color, UI_DEPTH);
        C2D_DrawTriangle(x1o, y1o, color, x1i, y1i, color, x0i, y0i, color, UI_DEPTH);
    }
}

void ui_frame(float x, float y, float w, float h, float r, float t, u32 color)
{
    if (r * 2 > w)
        r = w / 2;
    if (r * 2 > h)
        r = h / 2;
    if (r < t)
        r = t;
    ui_rect(x + r, y, w - 2 * r, t, color);
    ui_rect(x + r, y + h - t, w - 2 * r, t, color);
    ui_rect(x, y + r, t, h - 2 * r, color);
    ui_rect(x + w - t, y + r, t, h - 2 * r, color);
    corner(x + r, y + r, r, r - t, (float)M_PI, color);
    corner(x + w - r, y + r, r, r - t, (float)M_PI * 1.5f, color);
    corner(x + w - r, y + h - r, r, r - t, 0, color);
    corner(x + r, y + h - r, r, r - t, (float)M_PI / 2, color);
}

/* ---------------------------------------------------------------- icons */

/* Icons are drawn on a 24-unit grid, like the mock-up's stroke SVGs. */
typedef struct {
    float x, y, k, thick;
    u32 color;
} Pen;

static void line(const Pen *p, float x0, float y0, float x1, float y1)
{
    C2D_DrawLine(p->x + x0 * p->k, p->y + y0 * p->k, p->color, p->x + x1 * p->k, p->y + y1 * p->k, p->color, p->thick, UI_DEPTH);
    ui_circle(p->x + x0 * p->k, p->y + y0 * p->k, p->thick / 2, p->color);
    ui_circle(p->x + x1 * p->k, p->y + y1 * p->k, p->thick / 2, p->color);
}

static void poly(const Pen *p, const float *pts, int n, bool closed)
{
    for (int i = 0; i + 1 < n; i++)
        line(p, pts[2 * i], pts[2 * i + 1], pts[2 * i + 2], pts[2 * i + 3]);
    if (closed)
        line(p, pts[2 * n - 2], pts[2 * n - 1], pts[0], pts[1]);
}

static void arc(const Pen *p, float cx, float cy, float r, float from_deg, float to_deg)
{
    const int steps = 18;
    for (int i = 0; i < steps; i++) {
        float a0 = (from_deg + (to_deg - from_deg) * i / steps) * (float)M_PI / 180;
        float a1 = (from_deg + (to_deg - from_deg) * (i + 1) / steps) * (float)M_PI / 180;
        line(p, cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r);
    }
}

void ui_icon(Icon icon, float cx, float cy, float size, u32 color)
{
    Pen p = {cx - size / 2, cy - size / 2, size / 24.0f, fmaxf(1.4f, size / 13.0f), color};
    switch (icon) {
    case ICON_CHECK: {
        const float pts[] = {5, 12.5f, 10, 17, 19, 7};
        poly(&p, pts, 3, false);
        break;
    }
    case ICON_CROSS:
        line(&p, 6, 6, 18, 18);
        line(&p, 18, 6, 6, 18);
        break;
    case ICON_INFO:
        arc(&p, 12, 12, 9, 0, 360);
        line(&p, 12, 11, 12, 17);
        ui_circle(cx, p.y + 7.5f * p.k, p.thick * 0.7f, color);
        break;
    case ICON_WARN: {
        const float pts[] = {12, 3.5f, 21, 19.5f, 3, 19.5f};
        poly(&p, pts, 3, true);
        line(&p, 12, 10, 12, 13.5f);
        ui_circle(cx, p.y + 16.5f * p.k, p.thick * 0.6f, color);
        break;
    }
    case ICON_GRID: {
        const float r[4][2] = {{3, 3}, {14, 3}, {3, 14}, {14, 14}};
        for (int i = 0; i < 4; i++) {
            const float q[] = {r[i][0], r[i][1], r[i][0] + 7, r[i][1], r[i][0] + 7, r[i][1] + 7, r[i][0], r[i][1] + 7};
            poly(&p, q, 4, true);
        }
        break;
    }
    case ICON_SLIDERS:
        line(&p, 4, 7, 13, 7);
        line(&p, 19, 7, 20, 7);
        line(&p, 4, 17, 7, 17);
        line(&p, 13, 17, 20, 17);
        arc(&p, 16, 7, 2.2f, 0, 360);
        arc(&p, 10, 17, 2.2f, 0, 360);
        break;
    case ICON_PHONE: {
        const float body[] = {7, 2.5f, 17, 2.5f, 17, 21.5f, 7, 21.5f};
        poly(&p, body, 4, true);
        line(&p, 11, 18.5f, 13, 18.5f);
        break;
    }
    case ICON_BACK: {
        const float pts[] = {15, 5, 8, 12, 15, 19};
        poly(&p, pts, 3, false);
        break;
    }
    case ICON_NEXT: {
        const float pts[] = {9, 5, 16, 12, 9, 19};
        poly(&p, pts, 3, false);
        break;
    }
    case ICON_PLAY:
        C2D_DrawTriangle(p.x + 7 * p.k, p.y + 4.5f * p.k, color, p.x + 7 * p.k, p.y + 19.5f * p.k, color,
                         p.x + 19.5f * p.k, p.y + 12 * p.k, color, UI_DEPTH);
        break;
    case ICON_PAUSE:
        ui_rrect(p.x + 6.5f * p.k, p.y + 5 * p.k, 4 * p.k, 14 * p.k, 1 * p.k, color);
        ui_rrect(p.x + 13.5f * p.k, p.y + 5 * p.k, 4 * p.k, 14 * p.k, 1 * p.k, color);
        break;
    case ICON_RW:
    case ICON_FF: { /* two triangles pointing left (rewind) or right */
        const float d = icon == ICON_FF ? 1 : -1;
        for (int i = 0; i < 2; i++) {
            const float bx = 12 + d * (i ? 0 : -8), tip = bx + d * 8;
            C2D_DrawTriangle(p.x + bx * p.k, p.y + 6 * p.k, color, p.x + bx * p.k, p.y + 18 * p.k, color,
                             p.x + tip * p.k, p.y + 12 * p.k, color, UI_DEPTH);
        }
        break;
    }
    case ICON_SOUND:
    case ICON_MUTE: { /* a speaker, then waves or a cross */
        const float body[] = {4, 9, 8, 9, 13, 5, 13, 19, 8, 15, 4, 15};
        poly(&p, body, 6, true);
        if (icon == ICON_SOUND) {
            line(&p, 16.5f, 9.5f, 17.5f, 12);
            line(&p, 17.5f, 12, 16.5f, 14.5f);
            line(&p, 19, 7, 21, 12);
            line(&p, 21, 12, 19, 17);
        } else {
            line(&p, 16, 9, 21, 15);
            line(&p, 21, 9, 16, 15);
        }
        break;
    }
    case ICON_TRASH: {
        const float can[] = {6, 7, 7, 20, 17, 20, 18, 7};
        const float lid[] = {9, 7, 9, 4, 15, 4, 15, 7};
        line(&p, 4, 7, 20, 7);
        poly(&p, lid, 4, false);
        poly(&p, can, 4, false);
        break;
    }
    }
}

void ui_spinner(float cx, float cy, float r, u32 color)
{
    const int dots = 8;
    int lead = (int)(osGetTime() / 100 % dots);
    for (int i = 0; i < dots; i++) {
        float a = 2 * (float)M_PI * i / dots - (float)M_PI / 2;
        int behind = (lead - i + dots) % dots; /* 0: the bright one, then fading */
        u32 alpha = behind < 4 ? 255 - behind * 50 : 60;
        ui_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.26f, (color & 0x00FFFFFF) | alpha << 24);
    }
}

static float key_cap_width(const char *key)
{
    return strlen(key) > 1 ? ui_text_width(7, FONT_BOLD, key) + 10 : 15;
}

float ui_key_hint_width(const char *key, const char *label)
{
    return key_cap_width(key) + 4 + ui_text_width(9, FONT_REG, label);
}

float ui_key_hint(float x, float y, const char *key, const char *label)
{
    float h = 15;
    float w = key_cap_width(key);
    ui_rrect(x, y, w, h, h / 2, COL_SURFACE2);
    ui_text_center(x + w / 2, y + (strlen(key) > 1 ? 3.5f : 1.5f), strlen(key) > 1 ? 7 : 9, FONT_BOLD, COL_TEXT, key);
    return w + 4 + ui_text(x + w + 4, y + 1.5f, 9, FONT_REG, COL_MUTED, label);
}

/* A key chip as in the design (R, ↓, SELECT): rounded rectangle, label inside. */
float ui_key_chip(float x, float y, const char *key)
{
    float w = ui_text_width(8, FONT_BOLD, key) + 8;
    ui_rrect(x, y, w, 14, 4, COL_SURFACE2);
    ui_text(x + 4, y + 1.5f, 8, FONT_BOLD, COL_TEXT, key);
    return w;
}

/* The languages' flags, 21 x 13 (English: US stripes + canton; Ukrainian: blue over yellow). */
void ui_flag(float x, float y, int lang) { ui_flag_sized(x, y, 21, lang); }

static void stripes(float x, float y, float w, float h, bool vertical, const u32 *colors, const float *parts, int n)
{
    float total = 0, at = 0;
    for (int i = 0; i < n; i++)
        total += parts[i];
    for (int i = 0; i < n; i++) {
        const float len = (vertical ? w : h) * parts[i] / total;
        if (vertical)
            ui_rect(x + at, y, len, h, colors[i]);
        else
            ui_rect(x, y + at, w, len, colors[i]);
        at += len;
    }
}

/* A language's flag, 21:13. Chinese has none (whose flag would be a statement): its first letter
   (简 / 繁) on a plain tile instead, from the base font, which has every language's name. */
void ui_flag_sized(float x, float y, float w, int lang)
{
    const float h = w * 13 / 21, k = w / 21;
    const char *code = (unsigned)lang < LANG_COUNT ? LANGS[lang].code : "en";
    static const float ONE[3] = {1, 1, 1}, SPAIN[3] = {1, 2, 1};
    if (!strcmp(code, "uk")) {
        ui_rect(x, y, w, h / 2, RGBA(0x00, 0x57, 0xB7, 0xFF));
        ui_rect(x, y + h / 2, w, h / 2, RGBA(0xFF, 0xD7, 0x00, 0xFF));
    } else if (!strcmp(code, "de")) {
        const u32 c[3] = {RGBA(0x00, 0x00, 0x00, 0xFF), RGBA(0xDD, 0x00, 0x00, 0xFF), RGBA(0xFF, 0xCE, 0x00, 0xFF)};
        stripes(x, y, w, h, false, c, ONE, 3);
    } else if (!strcmp(code, "es")) {
        const u32 c[3] = {RGBA(0xAA, 0x15, 0x1B, 0xFF), RGBA(0xF1, 0xBF, 0x00, 0xFF), RGBA(0xAA, 0x15, 0x1B, 0xFF)};
        stripes(x, y, w, h, false, c, SPAIN, 3);
    } else if (!strcmp(code, "fr")) {
        const u32 c[3] = {RGBA(0x00, 0x55, 0xA4, 0xFF), COL_WHITE, RGBA(0xEF, 0x41, 0x35, 0xFF)};
        stripes(x, y, w, h, true, c, ONE, 3);
    } else if (!strcmp(code, "it")) {
        const u32 c[3] = {RGBA(0x00, 0x92, 0x46, 0xFF), COL_WHITE, RGBA(0xCE, 0x2B, 0x37, 0xFF)};
        stripes(x, y, w, h, true, c, ONE, 3);
    } else if (!strcmp(code, "pt")) { /* Portugal: green 2/5, red 3/5, the arms on the line between */
        const float cx = x + w * 0.4f, cy = y + h / 2, r = h * 0.3f;
        ui_rect(x, y, w * 0.4f, h, RGBA(0x00, 0x66, 0x00, 0xFF));
        ui_rect(x + w * 0.4f, y, w * 0.6f, h, RGBA(0xFF, 0x00, 0x00, 0xFF));
        ui_circle(cx, cy, r, RGBA(0xFF, 0xFF, 0x00, 0xFF));
        ui_rrect(cx - r * 0.5f, cy - r * 0.55f, r, r * 1.15f, r * 0.25f, COL_WHITE);
        ui_rrect(cx - r * 0.38f, cy - r * 0.43f, r * 0.76f, r * 0.9f, r * 0.2f, RGBA(0xFF, 0x00, 0x00, 0xFF));
        ui_rect(cx - r * 0.12f, cy - r * 0.2f, r * 0.24f, r * 0.4f, COL_WHITE);
    } else if (!strcmp(code, "ja")) {
        ui_rect(x, y, w, h, COL_WHITE);
        ui_circle(x + w / 2, y + h / 2, h * 0.3f, RGBA(0xBC, 0x00, 0x2D, 0xFF));
    } else if (!strncmp(code, "zh", 2)) {
        char letter[5] = "";
        const char *p = LANGS[lang].name, *q = p;
        utf8_next(&q);
        memcpy(letter, p, (size_t)(q - p) < 4 ? (size_t)(q - p) : 4);
        ui_rrect(x, y, w, h, 2.5f * k, COL_SURFACE2);
        ui_text_center(x + w / 2, y + h / 2 - h * 0.42f, h * 0.72f, FONT_BOLD, COL_TEXT, letter);
    } else { /* en: the United States */
        for (int i = 0; i < 7; i++)
            ui_rect(x, y + i * h / 7, w, h / 7, i % 2 ? COL_WHITE : COL_US_RED);
        ui_rect(x, y, w * 0.42f, h * 4 / 7, COL_US_BLUE);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++)
                ui_rect(x + (1.2f + c * 2.1f) * k, y + (1.1f + r * 2.3f) * k, 0.8f * k, 0.8f * k, COL_WHITE);
    }
}

void ui_toggle(float x, float y, bool on)
{
    ui_rrect(x, y, 32, 18, 9, on ? COL_ACCENT : COL_TRACK);
    ui_circle(on ? x + 23 : x + 9, y + 9, 7, on ? COL_WHITE : COL_SOFT);
}

void ui_pill(float x, float y, const char *label, u32 fg, u32 bg, bool dot_filled)
{
    float w = ui_text_width(9, FONT_BOLD, label) + 24;
    ui_rrect(x - w, y, w, 19, 9.5f, bg);
    if (dot_filled)
        ui_circle(x - w + 10, y + 9.5f, 3, fg);
    else {
        ui_circle(x - w + 10, y + 9.5f, 3.2f, fg);
        ui_circle(x - w + 10, y + 9.5f, 2.2f, bg);
    }
    ui_text(x - w + 17, y + 3.5f, 9, FONT_BOLD, fg, label);
}

void ui_tex(C3D_Tex *tex, float w, float h, float x, float y, float scale)
{
    const Tex3DS_SubTexture sub = {(u16)w, (u16)h, 0.0f, 1.0f, w / tex->width, 1.0f - h / tex->height};
    const C2D_Image img = {tex, &sub};
    C2D_DrawImageAt(img, x, y, UI_DEPTH, NULL, scale, scale);
}

void ui_tex_screen(C3D_Tex *tex, float cols, float cx, float cy)
{
    const Tex3DS_SubTexture sub = {240, (u16)cols, 0.0f, 1.0f, 240.0f / tex->width, 1.0f - cols / tex->height};
    const C2D_Image img = {tex, &sub};
    C2D_DrawImageAtRotated(img, cx, cy, UI_DEPTH, -(float)M_PI / 2, NULL, 1, 1);
}
