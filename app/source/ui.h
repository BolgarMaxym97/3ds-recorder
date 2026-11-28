/*
 * Drawing for the control app's screens: the palette and shapes of design/ui-studio.html
 * (variant B "Studio"), at the console's own resolution: the design is drawn at x2, so every
 * size here is half. Primitives taken from 3ds-ua-hub.
 */
#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

#include "i18n.h"

#define TOP_W 400
#define BOTTOM_W 320
#define SCREEN_H 240

/* Packed like C2D_Color32(), but usable in constant initialisers. */
#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)
/* Design tokens: Studio 2.0 (the redesign canvas, 2026-11-27). The cards keep the one-pixel
 * lighter edge of ui_card: on the TN panels of the console #1F232A on #14161A alone is hard to tell
 * apart (3ds-ua-hub, same lesson). */
#define COL_BG          RGBA(0x14, 0x16, 0x1A, 0xFF)
#define COL_SURFACE     RGBA(0x1F, 0x23, 0x2A, 0xFF)
#define COL_SURFACE2    RGBA(0x2A, 0x2F, 0x38, 0xFF)
#define COL_EDGE        RGBA(0x30, 0x36, 0x3F, 0xFF)
#define COL_TRACK       RGBA(0x3A, 0x40, 0x4A, 0xFF)
#define COL_TEXT        RGBA(0xE8, 0xEA, 0xED, 0xFF)
#define COL_SOFT        RGBA(0xC9, 0xD0, 0xD8, 0xFF)
#define COL_MUTED       RGBA(0x9A, 0xA3, 0xAD, 0xFF)
#define COL_DIM         RGBA(0x7D, 0x85, 0x8F, 0xFF)   /* eyebrows, the version under the tiles */
#define COL_FAINT       RGBA(0x5A, 0x61, 0x6B, 0xFF)   /* disabled */
#define COL_ACCENT      RGBA(0xFF, 0x4D, 0x4F, 0xFF)
#define COL_DANGER      RGBA(0xFF, 0x4D, 0x4F, 0xFF)
#define COL_ERR         RGBA(0xFF, 0x84, 0x86, 0xFF)
#define COL_ERR_BG      RGBA(0x3A, 0x1C, 0x1E, 0xFF)
#define COL_WARN        RGBA(0xFF, 0xC4, 0x57, 0xFF)
#define COL_WARN_FILL   RGBA(0xFF, 0xB0, 0x20, 0xFF)   /* a button lit amber, a warning icon */
#define COL_WARN_BG     RGBA(0x3A, 0x2F, 0x12, 0xFF)
#define COL_INFO        RGBA(0x7F, 0xD4, 0xFF, 0xFF)
#define COL_INFO_BG     RGBA(0x14, 0x30, 0x3F, 0xFF)
#define COL_OK          RGBA(0x5B, 0xE3, 0x9A, 0xFF)
#define COL_OK_FILL     RGBA(0x3D, 0xDC, 0x84, 0xFF)   /* a switch that is on in a list, the Ready dot */
#define COL_OK_BG       RGBA(0x12, 0x32, 0x1F, 0xFF)
#define COL_LED_BLUE    RGBA(0x4D, 0x9F, 0xFF, 0xFF)
#define COL_TAG_BG      RGBA(0x14, 0x30, 0x3F, 0xFF)   /* behind a blue tag (a replay clip in the gallery) */
#define COL_LED_BLUE_FAINT RGBA(0x2B, 0x52, 0x86, 0xFF)   /* the replay buffer's faint steady blue */
#define COL_LED_GREEN   RGBA(0x2F, 0xBF, 0x71, 0xFF)
#define COL_WHITE       RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_SCRIM       RGBA(0x0B, 0x0C, 0x0E, 0xE0)
#define COL_US_RED      RGBA(0xB2, 0x22, 0x34, 0xFF)
#define COL_US_BLUE     RGBA(0x3C, 0x3B, 0x6E, 0xFF)
/* Redesign 2.0: each colour has one role. Red is the recording only, teal is Replay, the focus
   frame is white (it was red, which now means "recording"). */
#define COL_REC         COL_ACCENT
#define COL_REC_BG      COL_ERR_BG
#define COL_REPLAY      RGBA(0x4C, 0xC3, 0xFF, 0xFF)
#define COL_REPLAY_BG   RGBA(0x14, 0x30, 0x3F, 0xFF)
#define COL_REPLAY_SOFT RGBA(0x7F, 0xD4, 0xFF, 0xFF)
#define COL_FOCUS       COL_TEXT

typedef enum { FONT_REG, FONT_BOLD } FontWeight;

typedef enum {
    ICON_CHECK, ICON_CROSS, ICON_INFO, ICON_WARN, ICON_GRID, ICON_SLIDERS, ICON_PHONE,
    ICON_BACK, ICON_NEXT, ICON_PLAY, ICON_TRASH, ICON_PAUSE, ICON_RW, ICON_FF, ICON_SOUND, ICON_MUTE,
    /* Redesign 2.0 */
    ICON_REC, ICON_REPLAY, ICON_GEAR, ICON_LED, ICON_DOWNLOAD, ICON_UPDATE, ICON_GLOBE, ICON_CLOCK,
    ICON_BLOCK, ICON_STOP, ICON_SD, ICON_HELP, ICON_BOLT,
} Icon;

/* How a 3DS button glyph is drawn: as a key to press (light), greyed (a hotkey that does nothing
   now), or tinted (a hotkey that is not allowed, the buttons lit on the console picture). */
typedef enum { BTN_NORMAL, BTN_MUTED, BTN_WARN, BTN_REC, BTN_REPLAY } BtnStyle;

bool ui_init(void);
void ui_exit(void);
void ui_frame_begin(void); /* clears the per-frame text buffer */
/* The font for a language's script: Japanese and Chinese load their own (a few MB), the others
   use the base font. false when it could not be loaded (the text then shows what the base has). */
bool ui_set_script(Script s);
/* true: all text in the base font, which has every language's name and first-run language screen
   (the language list, where the hovered language previews without loading its font) */
void ui_base_font(bool on);
/* every character of `str` is in the font text is drawn with now (texts from outside the app: the
   update's notes, written after this app's fonts were made) */
bool ui_has_glyphs(const char *str);

/* Text. `px` is the em size in screen pixels. Returns the drawn width. */
float ui_text(float x, float y, float px, FontWeight weight, u32 color, const char *str);
float ui_text_center(float cx, float y, float px, FontWeight weight, u32 color, const char *str);
float ui_text_right(float rx, float y, float px, FontWeight weight, u32 color, const char *str);
/* Word-wrapped to `width`; returns the height used. */
/* One line cut with "…" to `width` when it is wider. */
float ui_text_fit(float x, float y, float width, float px, FontWeight weight, u32 color, const char *str);
float ui_text_wrap(float x, float y, float width, float px, FontWeight weight, u32 color, const char *str);
/* At most `max_lines` lines, the last cut with "…" when the text goes on. */
float ui_text_wrap_lines(float x, float y, float width, float px, FontWeight weight, u32 color, const char *str,
                         int max_lines);
float ui_text_wrap_height(float width, float px, FontWeight weight, const char *str);
float ui_text_width(float px, FontWeight weight, const char *str);

/* Shapes. */
void ui_rect(float x, float y, float w, float h, u32 color);
void ui_rrect(float x, float y, float w, float h, float r, u32 color);
void ui_card(float x, float y, float w, float h, float r, u32 color);
void ui_circle(float cx, float cy, float r, u32 color);
void ui_ring(float cx, float cy, float r_out, float r_in, float fraction, u32 color, u32 track);
/* An outline `t` px thick along a rounded rectangle, nothing inside: the D-pad focus. */
void ui_frame(float x, float y, float w, float h, float r, float t, u32 color);
void ui_icon(Icon icon, float cx, float cy, float size, u32 color);
void ui_spinner(float cx, float cy, float r, u32 color);
/* The top-left w x h pixels of a texture (rows from the top, as gallery.c fills them), scaled. */
void ui_tex(C3D_Tex *tex, float w, float h, float x, float y, float scale);
/* A recorded screen as the LCD stores it (240 rows per column, `cols` columns, in a 256 x 512
   texture), turned upright and centred on (cx, cy). */
void ui_tex_screen(C3D_Tex *tex, float cols, float cx, float cy);

/* A 3DS button in its own shape, 14 high: A/B/X/Y round, L/R and ZL/ZR shoulders (ZL/ZR in
   outline), the D-pad's arrows square, SELECT/START (and other words) a pill. Returns the width. */
float ui_btn(float x, float y, const char *key, BtnStyle style);
float ui_btn_width(const char *key);
/* A button, then the label (the key hints at the bottom of a screen). */
float ui_key_hint(float x, float y, const char *key, const char *label);
float ui_key_hint_width(const char *key, const char *label);
/* A button of a hotkey (R, ↓, SELECT); returns its width. */
float ui_key_chip(float x, float y, const char *key);
float ui_key_chip_width(const char *key);
/* A small label with a coloured background (Ready for download, Converting 12%), 13 high. */
float ui_badge(float x, float y, const char *label, u32 fg, u32 bg);
float ui_badge_width(const char *label);
/* A bar `frac` full (0..1). */
void ui_progress(float x, float y, float w, float h, float frac, u32 fg, u32 track);
/* The D-pad focus: a white frame just outside a rectangle. */
void ui_focus(float x, float y, float w, float h, float r);
/* A language's flag (i18n.h Lang), 21 x 13. */
void ui_flag(float x, float y, int lang);
void ui_flag_sized(float x, float y, float w, int lang); /* w wide, 21:13 */
/* Toggle 32 x 18 at (x, y); ui_toggle_c: on in another colour (teal for Replay). */
void ui_toggle(float x, float y, bool on);
void ui_toggle_c(float x, float y, bool on, u32 on_color);
/* Status pill, right edge at x, 19 high. */
void ui_pill(float x, float y, const char *label, u32 fg, u32 bg, bool dot_filled);

#endif
