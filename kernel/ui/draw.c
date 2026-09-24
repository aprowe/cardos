/* Drawing primitives. See draw.h. */

#include "kernel/ui/draw.h"
#include "kernel/console/font6x8.h"

#include <string.h>

static Rect s_clip = { 0, 0, DISPLAY_W, DISPLAY_H };

/* One scanline. 480 bytes, versus 65 KB for a full-screen buffer. */
static uint16_t s_row[DISPLAY_W];

/* Staging for anything sent as more than one row: a run of text, and the
 * bands of a fill. Shared between the two deliberately -- display_blit does
 * not return until the panel has the pixels, so there is never more than one
 * of them in flight. (It did once, and a fill overwriting a line of text
 * mid-DMA is what forced display_blit to be synchronous; see display.c.)
 *
 * One line of text, composed before any of it is sent.
 *
 * Every draw_bitmap is a window-set -- CASET, RASET, RAMWR -- and then the
 * pixels. Drawing a glyph a row at a time meant four transactions carrying
 * twelve bytes, eight times per character: a forty-character line cost about
 * 1,280 transactions to move 1,920 bytes, and the ceremony dwarfed the
 * payload. Composed here and sent once, the same line is one transaction.
 *
 * 3,840 bytes of .bss, which on a machine with 120 KB free is a good trade
 * for the thing the screen spends most of its time doing. */
static uint16_t s_text[DISPLAY_W * FONT_H];

void draw_set_clip(Rect r) {
  s_clip = rect_clip(r, DISPLAY_W, DISPLAY_H);
}

Rect draw_clip(void) { return s_clip; }

void draw_rect(Rect r, uint16_t color) {
  Rect v = rect_intersect(r, s_clip);
  int16_t y;
  int i;

  if (rect_is_empty(v)) return;

  /* Send it in bands rather than scanlines when it fits: a 240x135 clear was
   * 135 window-sets, and is now 17. Anything wider than the staging buffer
   * falls back to a row at a time, which is what it always did. */
  if (v.w <= DISPLAY_W) {
    int band = (int)(sizeof s_text / sizeof s_text[0]) / v.w;   /* rows at a time */
    if (band > 1) {
      int16_t done = 0;
      for (i = 0; i < band * v.w; i++) s_text[i] = color;
      while (done < v.h) {
        int16_t n = (int16_t)(v.h - done);
        if (n > band) n = (int16_t)band;
        display_blit(v.x, (int16_t)(v.y + done), v.w, n, s_text);
        done = (int16_t)(done + n);
      }
      return;
    }
  }

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

/* A run of text, composed into one buffer and sent in one transaction.
 *
 * The clipping is done here rather than per glyph, so the buffer holds only
 * the visible part and the blit is exactly the damaged region. Falls back to
 * the per-glyph path for anything that will not fit -- which at this screen
 * size means nothing, but the fallback costs four lines and removes a
 * silent-corruption failure mode. */
void draw_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  Rect cell, v;
  int16_t len = 0, px, py;
  const char *p;

  for (p = s; *p; p++) len++;
  if (len <= 0) return;

  cell.x = x;
  cell.y = y;
  cell.w = (int16_t)(len * FONT_W);
  cell.h = FONT_H;
  v = rect_intersect(cell, s_clip);
  if (rect_is_empty(v)) return;

  if (v.w > DISPLAY_W) {
    while (*s) {
      draw_glyph(x, y, *s++, fg, bg);
      x = (int16_t)(x + FONT_W);
      if (x >= s_clip.x + s_clip.w) return;
    }
    return;
  }

  for (py = 0; py < v.h; py++) {
    int row_bit = (v.y + py) - y;
    for (px = 0; px < v.w; px++) {
      int col_in_run = (v.x + px) - x;
      int ch = col_in_run / FONT_W;
      int col = col_in_run % FONT_W;
      unsigned char c = (unsigned char)s[ch];
      const uint8_t *glyph = (c >= FONT_FIRST && c <= FONT_LAST)
                           ? font6x8[c - FONT_FIRST] : NULL;
      uint8_t bits = glyph ? glyph[col] : 0;
      s_text[py * v.w + px] = ((bits >> row_bit) & 1) ? fg : bg;
    }
  }
  display_blit(v.x, v.y, v.w, v.h, s_text);
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

void draw_image_scaled(int16_t x, int16_t y, int16_t w, int16_t h,
                       const uint16_t *px, int scale, uint16_t transparent) {
  Rect box, v;
  int16_t py, run_start, sx;

  if (!px || w <= 0 || h <= 0 || scale <= 0) return;
  box.x = x; box.y = y;
  box.w = (int16_t)(w * scale);
  box.h = (int16_t)(h * scale);
  v = rect_intersect(box, s_clip);
  if (rect_is_empty(v)) return;

  /* Runs of opaque pixels are blitted together and transparent ones break the
   * run, the same way the cursor is drawn: there is no back buffer to read, so
   * "leave this pixel alone" has to mean "do not write it". */
  for (py = v.y; py < v.y + v.h; py++) {
    const uint16_t *row = px + (size_t)((py - y) / scale) * (size_t)w;
    run_start = -1;
    for (sx = v.x; sx <= v.x + v.w; sx++) {
      int last = (sx == v.x + v.w);
      uint16_t c = last ? transparent : row[(sx - x) / scale];
      if (!last && c != transparent) {
        if (run_start < 0) run_start = sx;
        s_row[sx - run_start] = c;
      } else if (run_start >= 0) {
        display_blit(run_start, py, sx - run_start, 1, s_row);
        run_start = -1;
      }
    }
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

/* A run of text in a .cfnt, composed in bands and sent a band at a time.
 *
 * The same shape as draw_text: only the visible part of the run is built,
 * into s_text, and each band is one blit. A band is as many rows of the
 * visible width as s_text holds -- sixteen at full screen width, more for
 * narrower runs -- so a 42-row clock face is three transactions, not 42.
 * The background is laid first and every glyph pixel blended onto it, which
 * is what lets 4-bit edges look smooth on whatever colour the app chose. */
void draw_text_cfont(const CFont *f, int16_t x, int16_t y, const char *s,
                     uint16_t fg, uint16_t bg) {
  Rect cell, v;
  int band, top, i;

  if (!f || !s || !*s) return;
  cell.x = x;
  cell.y = y;
  cell.w = (int16_t)cfont_width(f, s);
  cell.h = f->height;
  v = rect_intersect(cell, s_clip);
  if (rect_is_empty(v)) return;
  band = (int)(sizeof s_text / sizeof s_text[0]) / v.w;
  if (band < 1) return;                    /* wider than the screen: cannot be */

  for (top = v.y; top < v.y + v.h; top += band) {
    int rows = v.y + v.h - top, pen = x;
    const char *p;
    if (rows > band) rows = band;
    for (i = 0; i < rows * v.w; i++) s_text[i] = bg;

    for (p = s; *p; p++) {
      CGlyph g;
      int gx0, gx1, gy0, gy1, gx, gy;
      cfont_glyph(f, (unsigned char)*p, &g);
      /* The part of this glyph's box inside the band and the visible run. */
      gx0 = v.x - (pen + g.x);             if (gx0 < 0) gx0 = 0;
      gx1 = v.x + v.w - (pen + g.x);       if (gx1 > g.w) gx1 = g.w;
      gy0 = top - (y + g.y);               if (gy0 < 0) gy0 = 0;
      gy1 = top + rows - (y + g.y);        if (gy1 > g.h) gy1 = g.h;
      for (gy = gy0; gy < gy1; gy++) {
        uint16_t *out = s_text + (y + g.y + gy - top) * v.w + (pen + g.x - v.x);
        for (gx = gx0; gx < gx1; gx++) {
          int a = cfont_pixel(f, &g, gx, gy);
          if (a) out[gx] = cfont_blend(fg, bg, a);
        }
      }
      pen += g.adv;
      if (pen >= v.x + v.w && g.x >= 0) break;   /* the rest is off the run */
    }
    display_blit(v.x, (int16_t)top, v.w, (int16_t)rows, s_text);
  }
}
