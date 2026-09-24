/* Bitmap fonts loaded from the card: the .cfnt format tools/make_cfnt.py
 * writes. Portable -- no allocation, no I/O -- so the host suite reads the
 * same bytes the device does.
 *
 * A font is rendered on the PC at the size an app asked for, so the device
 * only copies pixels: 4 bits of coverage per pixel for the screen, blended
 * against the colour the text sits on, or 1 bit for the thermal printer.
 * The format is described at the top of tools/make_cfnt.py. The 6x8 console
 * font is not one of these; it is compiled in and stays the default.
 */
#ifndef CARDOS_CFONT_H
#define CARDOS_CFONT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const uint8_t *table;     /* count glyph records */
  const uint8_t *bits;      /* the bitmaps they index */
  size_t   bits_size;
  uint16_t first, count;
  uint8_t  bpp;             /* 1 or 4 */
  uint8_t  height;          /* one line, in pixels */
  uint8_t  ascent;          /* the baseline, in rows from the top of the line */
} CFont;

typedef struct {
  const uint8_t *bits;
  int w, h;                 /* the ink box */
  int x, y;                 /* where it sits: from the pen, from the line's top */
  int adv;                  /* how far the pen moves */
} CGlyph;

/* Read a font out of `data`, which must outlive it. Every glyph's bitmap is
 * checked against the end of the buffer here, once, so drawing never has to.
 * 0, or -1 for anything that is not a well-formed version-1 .cfnt. */
int cfont_parse(CFont *f, const uint8_t *data, size_t n);

/* The glyph for `c`. 1 if the font has it; 0 if not, with an empty glyph
 * whose advance is a quarter of the line, so a missing character leaves a
 * gap rather than running its neighbours together. */
int cfont_glyph(const CFont *f, unsigned char c, CGlyph *g);

/* Coverage at (x, y) inside the glyph's box: 0..15, and 1bpp is 0 or 15. */
int cfont_pixel(const CFont *f, const CGlyph *g, int x, int y);

/* The pen's travel over `s`, or its first `n` characters. */
int cfont_width(const CFont *f, const char *s);
int cfont_width_n(const CFont *f, const char *s, int n);

/* fg over bg at coverage 0..15, both in the panel's byte-swapped RGB565
 * (CAPP_RGB), the answer in the same form. */
uint16_t cfont_blend(uint16_t fg, uint16_t bg, int a);

#endif /* CARDOS_CFONT_H */
