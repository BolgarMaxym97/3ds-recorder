/*
 * Where a line of text may end (ui.c's word wrap), without the font: the width of a character comes
 * from the caller. Latin and Cyrillic text breaks at spaces; Japanese and Chinese, which use none,
 * between any two characters, except that closing punctuation (、。」…) does not start a line and
 * opening brackets (「（…) do not end one. Pure C, tested on the PC (tests/host/test_textbreak).
 */
#ifndef TEXTBREAK_H
#define TEXTBREAK_H

#include <stddef.h>
#include <stdint.h>

typedef float (*CharWidthFn)(void *ctx, uint32_t cp);

/* The next UTF-8 code point at *s, moving *s past it (a broken byte reads as U+FFFD). */
uint32_t utf8_next(const char **s);

/* The first line of `s` that fits `width`: returns its length in bytes (no trailing spaces) and
   sets *next to where the next line starts (past the spaces or the '\n' that ended this one; at
   the terminating NUL when this is the last line). A word wider than `width` goes on a line of its
   own and overflows it, as before. *line_w gets the line's width when not NULL. */
size_t text_line(const char *s, float width, CharWidthFn w, void *ctx, const char **next, float *line_w);

/* The width of `s` (the widest line when it has '\n'). */
float text_width(const char *s, CharWidthFn w, void *ctx);

#endif
