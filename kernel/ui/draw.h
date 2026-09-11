/* Drawing primitives. Device-only.
 *
 * Everything honours a clip rectangle, because the compositor hands out damage
 * rectangles and there is no back buffer to draw into and sort out later -- a
 * full-screen canvas would be 65 KB, a fifth of the heap. Painting is straight
 * to the panel, so each primitive has to confine itself.
 */
#ifndef CARDOS_DRAW_H
#define CARDOS_DRAW_H

#include <stdint.h>

#include "drv/display.h"
#include "ui/rect.h"

/* The classic beveled palette. Chosen for legibility at 240x135: a one-pixel
 * light/dark edge still reads as raised, where a subtle flat fill vanishes. */
#define C_DESKTOP  RGB565(0,   128, 128)   /* teal */
#define C_FACE     RGB565(192, 192, 192)
#define C_LIGHT    RGB565(255, 255, 255)
#define C_SHADOW   RGB565(128, 128, 128)
#define C_DARK     RGB565(0,   0,   0)
#define C_TITLE    RGB565(0,   0,   128)   /* navy, focused */
#define C_TITLE_UN RGB565(128, 128, 128)   /* unfocused */
#define C_TITLE_FG RGB565(255, 255, 255)
#define C_TEXT     RGB565(0,   0,   0)
#define C_WHITE    RGB565(255, 255, 255)

void draw_set_clip(Rect r);
Rect draw_clip(void);

void draw_rect(Rect r, uint16_t color);            /* filled */
void draw_frame(Rect r, uint16_t color);           /* 1px outline */

/* A raised box: light on the top and left, shadow on the bottom and right.
 * Pass them swapped for a sunken one. */
void draw_bevel(Rect r, uint16_t face, uint16_t tl, uint16_t br);

/* Text in the 6x8 console font. Clipped per pixel, not per character, so a
 * damage rectangle that cuts a glyph in half still draws the visible half. */
void draw_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg);

/* As draw_text but stops at `max_w` pixels, ending with ".." if it had to. */
void draw_text_ellipsis(int16_t x, int16_t y, int16_t max_w, const char *s,
                        uint16_t fg, uint16_t bg);

int16_t draw_text_width(const char *s);

/* The pointer. 8x12 plus a one-pixel outline, so it stays visible over the
 * teal desktop, a grey window and a white content well alike. */
#define CURSOR_W 9
#define CURSOR_H 13
void draw_cursor(int16_t x, int16_t y);

#endif /* CARDOS_DRAW_H */
