/* Drawing primitives. See draw.h. */

#include "kernel/ui/draw.h"
#include "kernel/console/font6x8.h"

#include <string.h>

static Rect s_clip = { 0, 0, DISPLAY_W, DISPLAY_H };

/* One scanline. 480 bytes, versus 65 KB for a full-screen buffer. */
static uint16_t s_row[DISPLAY_W];

void draw_set_clip(Rect r) {
  s_clip = rect_clip(r, DISPLAY_W, DISPLAY_H);
}

Rect draw_clip(void) { return s_clip; }

void draw_rect(Rect r, uint16_t color) {
  Rect v = rect_intersect(r, s_clip);
  int16_t y;
  int i;

  if (rect_is_empty(v)) return;
  for (i = 0; i < v.w; i++) s_row[i] = color;
  for (y = v.y; y < v.y + v.h; y++) display_blit(v.x, y, v.w, 1, s_row);
}

void draw_frame(Rect r, uint16_t color) {
  Rect t, b, l, rr;
  if (rect_is_empty(r)) return;
  t = r; t.h = 1;
  b = r; b.y = (int16_t)(r.y + r.h - 1); b.h = 1;
  l = r; l.w = 1;
  rr = r; rr.x = (int16_t)(r.x + r.w - 1); rr.w = 1;
  draw_rect(t, color);
  draw_rect(b, color);
  draw_rect(l, color);
  draw_rect(rr, color);
}

void draw_bevel(Rect r, uint16_t face, uint16_t tl, uint16_t br) {
  Rect t, b, l, rr, inner;
  if (r.w < 2 || r.h < 2) { draw_rect(r, face); return; }

  t = r; t.h = 1;
  l = r; l.w = 1;
  b = r; b.y = (int16_t)(r.y + r.h - 1); b.h = 1;
  rr = r; rr.x = (int16_t)(r.x + r.w - 1); rr.w = 1;

  draw_rect(t, tl);
  draw_rect(l, tl);
  draw_rect(b, br);
  draw_rect(rr, br);

  inner = rect_inset(r, 1);
  draw_rect(inner, face);
}

/* One glyph, clipped per pixel. Clipping whole characters instead would drop
 * any glyph a damage rectangle cuts through, leaving gaps in text. */
static void draw_glyph(int16_t x, int16_t y, char ch, uint16_t fg, uint16_t bg) {
  Rect cell, v;
  const uint8_t *glyph = NULL;
  int16_t px, py;

  cell.x = x; cell.y = y; cell.w = FONT_W; cell.h = FONT_H;
  v = rect_intersect(cell, s_clip);
  if (rect_is_empty(v)) return;

  if ((unsigned char)ch >= FONT_FIRST && (unsigned char)ch <= FONT_LAST)
    glyph = font6x8[(unsigned char)ch - FONT_FIRST];

  for (py = v.y; py < v.y + v.h; py++) {
    int row_bit = py - y;
    for (px = v.x; px < v.x + v.w; px++) {
      int col = px - x;
      uint8_t bits = glyph ? glyph[col] : 0;
      s_row[px - v.x] = ((bits >> row_bit) & 1) ? fg : bg;
    }
    display_blit(v.x, py, v.w, 1, s_row);
  }
}

void draw_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  while (*s) {
    draw_glyph(x, y, *s++, fg, bg);
    x = (int16_t)(x + FONT_W);
    if (x >= s_clip.x + s_clip.w) return;    /* nothing further can be seen */
  }
}

/* A glyph at an integer scale. Clipped per pixel like the unscaled one, so a
 * damage rectangle cutting through a large character still repaints it. */
static void draw_glyph_scaled(int16_t x, int16_t y, char ch, int scale,
                              uint16_t fg, uint16_t bg) {
  Rect cell, v;
  const uint8_t *glyph = NULL;
  int16_t px, py;

  cell.x = x; cell.y = y;
  cell.w = (int16_t)(FONT_W * scale);
  cell.h = (int16_t)(FONT_H * scale);
  v = rect_intersect(cell, s_clip);
  if (rect_is_empty(v)) return;

  if ((unsigned char)ch >= FONT_FIRST && (unsigned char)ch <= FONT_LAST)
    glyph = font6x8[(unsigned char)ch - FONT_FIRST];

  for (py = v.y; py < v.y + v.h; py++) {
    int row_bit = (py - y) / scale;
    for (px = v.x; px < v.x + v.w; px++) {
      int col = (px - x) / scale;
      uint8_t bits = glyph ? glyph[col] : 0;
      s_row[px - v.x] = ((bits >> row_bit) & 1) ? fg : bg;
    }
    display_blit(v.x, py, v.w, 1, s_row);
  }
}

void draw_text_scaled(int16_t x, int16_t y, const char *s, int scale,
                      uint16_t fg, uint16_t bg) {
  if (scale <= 1) { draw_text(x, y, s, fg, bg); return; }
  while (*s) {
    draw_glyph_scaled(x, y, *s++, scale, fg, bg);
    x = (int16_t)(x + FONT_W * scale);
    if (x >= s_clip.x + s_clip.w) return;
  }
}

int16_t draw_text_width(const char *s) {
  return (int16_t)(strlen(s) * FONT_W);
}

void draw_text_ellipsis(int16_t x, int16_t y, int16_t max_w, const char *s,
                        uint16_t fg, uint16_t bg) {
  int16_t room = (int16_t)(max_w / FONT_W);
  int16_t n = (int16_t)strlen(s);

  if (room <= 0) return;
  if (n <= room) { draw_text(x, y, s, fg, bg); return; }

  /* Cut, with nothing to mark it. An ellipsis costs two of the six or seven
   * characters that fit under an icon, and "Settin.." reads worse than
   * "Setting" -- the reader can already see the label ran out of room. */
  {
    int16_t i;
    for (i = 0; i < room; i++)
      draw_glyph((int16_t)(x + i * FONT_W), y, s[i], fg, bg);
  }
}


/* ------------------------------------------------------------ bitmap ---- */

void draw_bitmap1(int16_t x, int16_t y, int16_t w, int16_t h,
                  const uint8_t *bits, uint16_t fg, uint16_t bg) {
  Rect box, v;
  int16_t px, py;
  int stride;

  if (!bits || w <= 0 || h <= 0) return;
  stride = (w + 7) / 8;

  box.x = x; box.y = y; box.w = w; box.h = h;
  v = rect_intersect(box, s_clip);
  if (rect_is_empty(v)) return;

  for (py = v.y; py < v.y + v.h; py++) {
    const uint8_t *row = bits + (size_t)(py - y) * (size_t)stride;
    for (px = v.x; px < v.x + v.w; px++) {
      int col = px - x;
      int on = (row[col >> 3] >> (7 - (col & 7))) & 1;
      s_row[px - v.x] = on ? fg : bg;
    }
    display_blit(v.x, py, v.w, 1, s_row);
  }
}

void draw_bitmap1_scaled(int16_t x, int16_t y, int16_t w, int16_t h,
                         const uint8_t *bits, int scale,
                         uint16_t fg, uint16_t bg) {
  Rect box, v;
  int16_t px, py;
  int stride;

  if (!bits || w <= 0 || h <= 0 || scale <= 0) return;
  if (scale == 1) { draw_bitmap1(x, y, w, h, bits, fg, bg); return; }
  stride = (w + 7) / 8;

  box.x = x; box.y = y;
  box.w = (int16_t)(w * scale);
  box.h = (int16_t)(h * scale);
  v = rect_intersect(box, s_clip);
  if (rect_is_empty(v)) return;

  /* One blit per output scanline, not per source pixel: at 4x a 16x16 icon is
   * 4096 pixels, and a blit each would be 4096 SPI transactions. */
  for (py = v.y; py < v.y + v.h; py++) {
    const uint8_t *row = bits + (size_t)((py - y) / scale) * (size_t)stride;
    for (px = v.x; px < v.x + v.w; px++) {
      int col = (px - x) / scale;
      s_row[px - v.x] = ((row[col >> 3] >> (7 - (col & 7))) & 1) ? fg : bg;
    }
    display_blit(v.x, py, v.w, 1, s_row);
  }
}

/* ------------------------------------------------------------ cursor ---- */

/* Bit 0 is the leftmost pixel. The classic arrow: a filled wedge with a tail.
 * The outline is derived rather than stored -- a pixel is outline if it is not
 * fill but touches fill -- which keeps one table instead of two in step. */
static const uint8_t ARROW[12] = {
  0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F,
  0x7F, 0xFF, 0x1F, 0x31, 0x60, 0x60,
};

static int arrow_fill(int col, int row) {
  if (row < 0 || row >= 12 || col < 0 || col >= 8) return 0;
  return (ARROW[row] >> col) & 1;
}

static int arrow_outline(int col, int row) {
  int dc, dr;
  if (arrow_fill(col, row)) return 0;
  for (dr = -1; dr <= 1; dr++)
    for (dc = -1; dc <= 1; dc++)
      if (arrow_fill(col + dc, row + dr)) return 1;
  return 0;
}

Rect draw_cursor_bounds(int16_t x, int16_t y) {
  Rect r;
  r.x = (int16_t)(x - 1);       /* room for the outline left of the tip */
  r.y = (int16_t)(y - 1);
  r.w = CURSOR_W;
  r.h = CURSOR_H;
  return r;
}

void draw_cursor(int16_t x, int16_t y) {
  Rect box = draw_cursor_bounds(x, y), v;
  int16_t px, py;

  v = rect_intersect(box, s_clip);
  if (rect_is_empty(v)) return;

  for (py = v.y; py < v.y + v.h; py++) {
    int row = py - y;
    int16_t run_start = -1;

    /* Build the whole scanline, then blit each solid run. Anything not part of
       the pointer has to be left alone, so transparent pixels break the run
       rather than being emitted as a background colour. */
    for (px = v.x; px <= v.x + v.w; px++) {
      int col = px - x;
      int solid = (px < v.x + v.w) &&
                  (arrow_fill(col, row) || arrow_outline(col, row));
      if (solid) {
        if (run_start < 0) run_start = px;
        s_row[px - run_start] = arrow_fill(col, row) ? C_DARK : C_WHITE;
      } else if (run_start >= 0) {
        display_blit(run_start, py, px - run_start, 1, s_row);
        run_start = -1;
      }
    }
  }
}
