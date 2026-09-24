/* .cfnt fonts. See cfont.h, and tools/make_cfnt.py for the format. */

#include "kernel/ui/cfont.h"

#include <string.h>

#define HEADER 16
#define GLYPH  10

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* Bytes a w x h glyph's bitmap takes: its pixels, packed, rounded up. */
static size_t glyph_bytes(const CFont *f, int w, int h) {
  size_t px = (size_t)w * (size_t)h;
  return f->bpp == 4 ? (px + 1) / 2 : (px + 7) / 8;
}

int cfont_parse(CFont *f, const uint8_t *data, size_t n) {
  size_t table_end;
  unsigned i;

  if (!f || !data || n < HEADER) return -1;
  if (memcmp(data, "CFNT", 4) != 0 || data[4] != 1) return -1;
  if (data[5] != 1 && data[5] != 4) return -1;

  memset(f, 0, sizeof *f);
  f->bpp = data[5];
  f->height = data[6];
  f->ascent = data[7];
  f->first = (uint16_t)rd16(data + 8);
  f->count = (uint16_t)rd16(data + 10);
  table_end = HEADER + (size_t)f->count * GLYPH;
  if (f->count == 0 || table_end > n) return -1;
  f->table = data + HEADER;
  f->bits = data + table_end;
  f->bits_size = n - table_end;

  /* Every glyph, once: its bitmap must end inside the buffer. After this
   * nothing that reads a pixel has to check. */
  for (i = 0; i < f->count; i++) {
    const uint8_t *g = f->table + i * GLYPH;
    uint32_t off = rd32(g);
    if (!g[9]) continue;
    if (off > f->bits_size || glyph_bytes(f, g[4], g[5]) > f->bits_size - off)
      return -1;
  }
  return 0;
}

int cfont_glyph(const CFont *f, unsigned char c, CGlyph *g) {
  const uint8_t *r;
  memset(g, 0, sizeof *g);
  g->adv = f->height / 4;
  if (c < f->first || c >= f->first + f->count) return 0;
  r = f->table + (size_t)(c - f->first) * GLYPH;
  if (!r[9]) return 0;
  g->bits = f->bits + rd32(r);
  g->w = r[4];
  g->h = r[5];
  g->x = (int8_t)r[6];
  g->y = (int8_t)r[7];
  g->adv = r[8];
  return 1;
}

int cfont_pixel(const CFont *f, const CGlyph *g, int x, int y) {
  size_t n;
  if (!g->bits || x < 0 || y < 0 || x >= g->w || y >= g->h) return 0;
  n = (size_t)y * (size_t)g->w + (size_t)x;
  if (f->bpp == 4) {
    uint8_t b = g->bits[n >> 1];
    return (n & 1) ? (b & 15) : (b >> 4);
  }
  return (g->bits[n >> 3] >> (7 - (n & 7))) & 1 ? 15 : 0;
}

int cfont_width_n(const CFont *f, const char *s, int n) {
  CGlyph g;
  int w = 0, i;
  for (i = 0; i < n && s[i]; i++) {
    cfont_glyph(f, (unsigned char)s[i], &g);
    w += g.adv;
  }
  return w;
}

int cfont_width(const CFont *f, const char *s) {
  return cfont_width_n(f, s, 0x7FFFFFFF);
}

/* Unswap, mix each channel, swap back. Coverage 15 is exactly fg and 0
 * exactly bg, so the edges of a glyph never tint the solid parts. */
uint16_t cfont_blend(uint16_t fg, uint16_t bg, int a) {
  unsigned f, b, r, gg, bl;
  if (a <= 0) return bg;
  if (a >= 15) return fg;
  f = (uint16_t)((fg >> 8) | (fg << 8));
  b = (uint16_t)((bg >> 8) | (bg << 8));
  r  = (((f >> 11) & 31) * (unsigned)a + ((b >> 11) & 31) * (unsigned)(15 - a) + 7) / 15;
  gg = (((f >> 5) & 63) * (unsigned)a + ((b >> 5) & 63) * (unsigned)(15 - a) + 7) / 15;
  bl = ((f & 31) * (unsigned)a + (b & 31) * (unsigned)(15 - a) + 7) / 15;
  f = (r << 11) | (gg << 5) | bl;
  return (uint16_t)(((f >> 8) | (f << 8)) & 0xFFFF);
}
