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

#define TOP_W 400
#define BOTTOM_W 320
#define SCREEN_H 240

/* Packed like C2D_Color32(), but usable in constant initialisers. */
#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)
/* Design tokens; the surfaces are a step lighter than the mock-up's (#1F232A on #14161A):
 * on the TN panels of the console the two are hard to tell apart (3ds-ua-hub, same lesson). */
#define COL_BG          RGBA(0x14, 0x16, 0x1A, 0xFF)
#define COL_SURFACE     RGBA(0x24, 0x29, 0x31, 0xFF)
#define COL_SURFACE2    RGBA(0x31, 0x37, 0x40, 0xFF)
#define COL_EDGE        RGBA(0x30, 0x36, 0x3F, 0xFF)
#define COL_TRACK       RGBA(0x3A, 0x3F, 0x47, 0xFF)
#define COL_TEXT        RGBA(0xE8, 0xEA, 0xED, 0xFF)
#define COL_SOFT        RGBA(0xC3, 0xC9, 0xD0, 0xFF)
#define COL_MUTED       RGBA(0x9A, 0xA3, 0xAD, 0xFF)
#define COL_FAINT       RGBA(0x5A, 0x61, 0x6B, 0xFF)
#define COL_ACCENT      RGBA(0xFF, 0x4D, 0x4F, 0xFF)
#define COL_DANGER      RGBA(0xC6, 0x2F, 0x31, 0xFF)
#define COL_ERR         RGBA(0xFF, 0x8A, 0x8C, 0xFF)
#define COL_ERR_BG      RGBA(0x3A, 0x1D, 0x1F, 0xFF)
#define COL_WARN        RGBA(0xFF, 0xB1, 0x5C, 0xFF)
#define COL_WARN_BG     RGBA(0x3A, 0x2A, 0x14, 0xFF)
#define COL_INFO        RGBA(0x7A, 0xB8, 0xFF, 0xFF)
#define COL_INFO_BG     RGBA(0x1B, 0x2A, 0x3D, 0xFF)
#define COL_OK          RGBA(0x5F, 0xD8, 0x96, 0xFF)
#define COL_OK_BG       RGBA(0x24, 0x37, 0x2C, 0xFF)
#define COL_LED_BLUE    RGBA(0x4D, 0x9F, 0xFF, 0xFF)
#define COL_TAG_BG      RGBA(0x1D, 0x2B, 0x40, 0xFF)   /* behind a blue tag (a replay clip in the gallery) */
#define COL_LED_BLUE_FAINT RGBA(0x2B, 0x52, 0x86, 0xFF)   /* the replay buffer's faint steady blue */
#define COL_LED_GREEN   RGBA(0x2F, 0xBF, 0x71, 0xFF)
#define COL_WHITE       RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_SCRIM       RGBA(0x0B, 0x0C, 0x0E, 0xE0)
#define COL_FLAG_BLUE   RGBA(0x00, 0x57, 0xB7, 0xFF)
#define COL_FLAG_YELLOW RGBA(0xFF, 0xD7, 0x00, 0xFF)
#define COL_US_RED      RGBA(0xB2, 0x22, 0x34, 0xFF)
#define COL_US_BLUE     RGBA(0x3C, 0x3B, 0x6E, 0xFF)

typedef enum { FONT_REG, FONT_BOLD } FontWeight;

typedef enum {
    ICON_CHECK, ICON_CROSS, ICON_INFO, ICON_WARN, ICON_GRID, ICON_SLIDERS, ICON_PHONE,
    ICON_BACK, ICON_NEXT, ICON_PLAY, ICON_TRASH, ICON_PAUSE, ICON_RW, ICON_FF, ICON_SOUND, ICON_MUTE,
} Icon;

bool ui_init(void);
void ui_exit(void);
void ui_frame_begin(void); /* clears the per-frame text buffer */

/* Text. `px` is the em size in screen pixels. Returns the drawn width. */
float ui_text(float x, float y, float px, FontWeight weight, u32 color, const char *str);
float ui_text_center(float cx, float y, float px, FontWeight weight, u32 color, const char *str);
float ui_text_right(float rx, float y, float px, FontWeight weight, u32 color, const char *str);
/* Word-wrapped to `width`; returns the height used. */
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

/* A round key cap with a letter (A, B, Y) or a pill (START), then the label. */
float ui_key_hint(float x, float y, const char *key, const char *label);
float ui_key_hint_width(const char *key, const char *label);
/* A key chip (R, ↓, SELECT) for the hotkey; returns its width. */
float ui_key_chip(float x, float y, const char *key);
/* Language switch flag, 21 x 13. */
void ui_flag(float x, float y, bool ukraine);
void ui_flag_sized(float x, float y, float w, bool ukraine); /* w wide, 21:13 */
/* Toggle 32 x 18 at (x, y). */
void ui_toggle(float x, float y, bool on);
/* Status pill, right edge at x, 19 high. */
void ui_pill(float x, float y, const char *label, u32 fg, u32 bg, bool dot_filled);

#endif
