/* Drawing primitives. See draw.h. */

#include "ui/draw.h"
#include "console/font6x8.h"

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

int16_t draw_text_width(const char *s) {
  return (int16_t)(strlen(s) * FONT_W);
}

void draw_text_ellipsis(int16_t x, int16_t y, int16_t max_w, const char *s,
                        uint16_t fg, uint16_t bg) {
  int16_t room = (int16_t)(max_w / FONT_W);
  int16_t n = (int16_t)strlen(s);

  if (room <= 0) return;
  if (n <= room) { draw_text(x, y, s, fg, bg); return; }

  /* Truncate and mark it, so a clipped title looks deliberate. */
  {
    int16_t keep = (int16_t)(room - 2);
    int16_t i;
    if (keep < 0) keep = 0;
    for (i = 0; i < keep; i++) {
      draw_glyph((int16_t)(x + i * FONT_W), y, s[i], fg, bg);
    }
    for (; i < room; i++) {
      draw_glyph((int16_t)(x + i * FONT_W), y, '.', fg, bg);
    }
  }
}
