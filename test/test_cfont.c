/* .cfnt fonts: the reading, which is the only part that can be wrong in a way
 * that crashes -- a glyph whose bitmap runs past the end of the file would
 * send the renderer reading whatever follows it in the heap. The fonts here
 * are built byte by byte, so these tests pin the format itself, not
 * whatever tools/make_cfnt.py happens to write.
 */

#include <string.h>

#include "tinytest.h"
#include "kernel/ui/cfont.h"

static uint8_t g_buf[512];

static void put16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, unsigned v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A two-character 4bpp font, 'A' and 'C' ('B' absent): A is a 3x2 box of
 * coverage 0..5, C a 1x1 dot at full coverage. Line 10, baseline 8. */
static size_t make_font(int bpp) {
  uint8_t *p = g_buf, *t;
  memset(g_buf, 0, sizeof g_buf);
  memcpy(p, "CFNT", 4);
  p[4] = 1; p[5] = (uint8_t)bpp; p[6] = 10; p[7] = 8;
  put16(p + 8, 'A'); put16(p + 10, 3);
  t = p + 16;
  /* A: offset 0, 3x2, x 1, y 2, advance 5, present */
  put32(t, 0); t[4] = 3; t[5] = 2; t[6] = 1; t[7] = 2; t[8] = 5; t[9] = 1;
  t += 10;                                  /* B: absent, all zero */
  t += 10;
  if (bpp == 4) {
    /* C: after A's 3 bytes */
    put32(t, 3); t[4] = 1; t[5] = 1; t[6] = 0; t[7] = 7; t[8] = 2; t[9] = 1;
    t += 10;
    t[0] = 0x01; t[1] = 0x23; t[2] = 0x45;  /* A: 0 1 2 3 4 5 */
    t[3] = 0xF0;                            /* C: 15 */
    return (size_t)(t + 4 - g_buf);
  }
  /* 1bpp: A is 101 / 010 -> bits 101010 = 0xA8; C one set bit */
  put32(t, 1); t[4] = 1; t[5] = 1; t[6] = 0; t[7] = 7; t[8] = 2; t[9] = 1;
  t += 10;
  t[0] = 0xA8;
  t[1] = 0x80;
  return (size_t)(t + 2 - g_buf);
}

void test_cfont_parses_the_header(void) {
  CFont f;
  size_t n = make_font(4);
  CHECK_EQ(cfont_parse(&f, g_buf, n), 0);
  CHECK_EQ(f.bpp, 4);
  CHECK_EQ(f.height, 10);
  CHECK_EQ(f.ascent, 8);
  CHECK_EQ(f.first, 'A');
  CHECK_EQ(f.count, 3);
}

void test_cfont_reads_4bpp_coverage(void) {
  CFont f;
  CGlyph g;
  cfont_parse(&f, g_buf, make_font(4));
  CHECK_EQ(cfont_glyph(&f, 'A', &g), 1);
  CHECK_EQ(g.w, 3); CHECK_EQ(g.h, 2); CHECK_EQ(g.x, 1); CHECK_EQ(g.y, 2); CHECK_EQ(g.adv, 5);
  CHECK_EQ(cfont_pixel(&f, &g, 0, 0), 0);
  CHECK_EQ(cfont_pixel(&f, &g, 1, 0), 1);
  CHECK_EQ(cfont_pixel(&f, &g, 2, 0), 2);
  CHECK_EQ(cfont_pixel(&f, &g, 0, 1), 3);
  CHECK_EQ(cfont_pixel(&f, &g, 2, 1), 5);
  CHECK_EQ(cfont_glyph(&f, 'C', &g), 1);
  CHECK_EQ(cfont_pixel(&f, &g, 0, 0), 15);
}

void test_cfont_reads_1bpp_as_all_or_nothing(void) {
  CFont f;
  CGlyph g;
  CHECK_EQ(cfont_parse(&f, g_buf, make_font(1)), 0);
  cfont_glyph(&f, 'A', &g);
  CHECK_EQ(cfont_pixel(&f, &g, 0, 0), 15);
  CHECK_EQ(cfont_pixel(&f, &g, 1, 0), 0);
  CHECK_EQ(cfont_pixel(&f, &g, 2, 0), 15);
  CHECK_EQ(cfont_pixel(&f, &g, 0, 1), 0);
  CHECK_EQ(cfont_pixel(&f, &g, 1, 1), 15);
  CHECK_EQ(cfont_pixel(&f, &g, 2, 1), 0);
}

/* Absent, or outside the range: no ink, and a quarter-line advance. */
void test_cfont_missing_glyph_leaves_a_gap(void) {
  CFont f;
  CGlyph g;
  cfont_parse(&f, g_buf, make_font(4));
  CHECK_EQ(cfont_glyph(&f, 'B', &g), 0);
  CHECK_EQ(g.w, 0); CHECK_EQ(g.adv, 10 / 4);
  CHECK_EQ(cfont_glyph(&f, 'z', &g), 0);
  CHECK_EQ(g.adv, 10 / 4);
  CHECK_EQ(cfont_pixel(&f, &g, 0, 0), 0);
}

void test_cfont_width_sums_the_advances(void) {
  CFont f;
  cfont_parse(&f, g_buf, make_font(4));
  CHECK_EQ(cfont_width(&f, "AC"), 7);
  CHECK_EQ(cfont_width(&f, "ABC"), 7 + 2);
  CHECK_EQ(cfont_width_n(&f, "ACAC", 3), 12);
  CHECK_EQ(cfont_width(&f, ""), 0);
}

/* The one that matters: a file that lies about where its bitmaps are. */
void test_cfont_refuses_what_would_read_past_the_end(void) {
  CFont f;
  size_t n = make_font(4);
  CHECK_EQ(cfont_parse(&f, g_buf, n - 1), -1);         /* C's byte cut off */
  CHECK_EQ(cfont_parse(&f, g_buf, 16 + 25), -1);       /* table cut off */
  CHECK_EQ(cfont_parse(&f, g_buf, 12), -1);            /* header cut off */
  n = make_font(4);
  put32(g_buf + 16, 200);                              /* A's offset too far */
  CHECK_EQ(cfont_parse(&f, g_buf, n), -1);
  n = make_font(4);
  g_buf[16 + 4] = 200;                                 /* A too wide */
  CHECK_EQ(cfont_parse(&f, g_buf, n), -1);
}

void test_cfont_refuses_the_wrong_file(void) {
  CFont f;
  size_t n = make_font(4);
  g_buf[0] = 'X';
  CHECK_EQ(cfont_parse(&f, g_buf, n), -1);
  n = make_font(4);
  g_buf[4] = 2;                                        /* version */
  CHECK_EQ(cfont_parse(&f, g_buf, n), -1);
  n = make_font(4);
  g_buf[5] = 8;                                        /* bpp */
  CHECK_EQ(cfont_parse(&f, g_buf, n), -1);
  CHECK_EQ(cfont_parse(&f, NULL, 100), -1);
}

/* Colours are byte-swapped RGB565 (CAPP_RGB). */
#define SW(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
void test_cfont_blend_mixes_in_the_panels_byte_order(void) {
  uint16_t white = SW(0xFFFF), black = SW(0x0000);
  uint16_t red = SW(0xF800), blue = SW(0x001F);
  CHECK_EQ(cfont_blend(white, black, 15), white);
  CHECK_EQ(cfont_blend(white, black, 0), black);
  CHECK_EQ(cfont_blend(red, blue, 15), red);
  CHECK_EQ(cfont_blend(red, blue, 0), blue);
  {
    uint16_t m = SW(cfont_blend(white, black, 8));     /* back to plain 565 */
    int r = m >> 11, g = (m >> 5) & 63, b = m & 31;
    CHECK(r >= 15 && r <= 17);
    CHECK(g >= 31 && g <= 35);
    CHECK(b >= 15 && b <= 17);
  }
}
