/* The wire format is pinned to bytes that printed on a real X6h on
 * 2026-09-20 (the reply below is the one the printer sent back). The
 * renderer is pinned by shape -- where ink lands -- because the exact glyph
 * pixels belong to the font, not to this module. */
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/printdoc.h"

static int px(const uint8_t *row, int x) { return (row[x >> 3] >> (x & 7)) & 1; }

static int ink(const uint8_t *row, int from, int to) {
  int x, n = 0;
  for (x = from; x < to; x++) n += px(row, x);
  return n;
}

void test_printdoc_crc8_is_poly_07(void) {
  static const uint8_t st[] = { 0x00, 0x06, 0x27 };
  static const uint8_t z[] = { 0x00 };
  static const uint8_t q[] = { 0x32 };
  CHECK_EQ(printdoc_crc8(z, 1), 0x00);
  CHECK_EQ(printdoc_crc8(st, 3), 0x8B);       /* from the printer's own reply */
  CHECK_EQ(printdoc_crc8(q, 1), 0x9E);
}

void test_printdoc_packet_frames_a_command(void) {
  uint8_t out[16];
  static const uint8_t data[] = { 0x30, 0x00 };
  int n = printdoc_packet(0xA1, data, 2, out, sizeof out);
  CHECK_EQ(n, 10);
  CHECK_EQ(out[0], 0x51); CHECK_EQ(out[1], 0x78); CHECK_EQ(out[2], 0xA1);
  CHECK_EQ(out[3], 0x00); CHECK_EQ(out[4], 0x02); CHECK_EQ(out[5], 0x00);
  CHECK_EQ(out[6], 0x30); CHECK_EQ(out[7], 0x00);
  CHECK_EQ(out[8], printdoc_crc8(data, 2));
  CHECK_EQ(out[9], 0xFF);
  CHECK_EQ(printdoc_packet(0xA1, data, 2, out, 9), -1);
}

void test_printdoc_row_packet_is_56_bytes(void) {
  uint8_t row[PRINT_ROW_BYTES], out[64];
  memset(row, 0, sizeof row);
  row[0] = 0x01;                                  /* leftmost pixel */
  CHECK_EQ(printdoc_row_packet(row, out, sizeof out), 56);
  CHECK_EQ(out[2], 0xA2);
  CHECK_EQ(out[4], 48);
  CHECK_EQ(out[6], 0x01);
  CHECK_EQ(out[55], 0xFF);
}

void test_printdoc_prologue_and_epilogue_bracket_a_job(void) {
  uint8_t out[192];
  int n = printdoc_prologue(out, sizeof out);
  CHECK(n > 0);
  CHECK_EQ(out[2], 0xA3);                          /* starts by asking status */
  CHECK_EQ(out[n - 1], 0xFF);
  n = printdoc_epilogue(out, sizeof out);
  CHECK(n > 0);
  CHECK_EQ(out[2], 0xA1);                          /* feeds clear of the head */
  CHECK_EQ(out[n - 1], 0xFF);
  CHECK_EQ(printdoc_prologue(out, 8), -1);
}

void test_printdoc_status_text_reads_the_reply(void) {
  static const uint8_t ok[] = { 0x51,0x78,0xA3,0x01,0x03,0x00,0x00,0x06,0x27,0x8B,0xFF };
  static const uint8_t paper[] = { 0x51,0x78,0xA3,0x01,0x03,0x00,0x01,0x06,0x27,0x00,0xFF };
  static const uint8_t junk[] = { 0x51,0x78,0xA2,0x01,0x00 };
  CHECK(printdoc_status_text(ok, sizeof ok) != NULL);
  CHECK(!strcmp(printdoc_status_text(ok, sizeof ok), "ok"));
  CHECK(strstr(printdoc_status_text(paper, sizeof paper), "paper") != NULL);
  CHECK(printdoc_status_text(junk, sizeof junk) == NULL);
  CHECK(printdoc_status_text(NULL, 0) == NULL);
}

/* ---- renderer ---- */

static int drain(const char *text, int *rows_with_ink) {
  PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  int n = 0, inked = 0;
  printdoc_begin(&d, text);
  while (printdoc_next_row(&d, row)) {
    n++;
    if (ink(row, 0, PRINT_WIDTH)) inked++;
    CHECK(n < 4000);
    if (n >= 4000) break;
  }
  if (rows_with_ink) *rows_with_ink = inked;
  return n;
}

void test_printdoc_empty_document_prints_nothing(void) {
  CHECK_EQ(drain("", NULL), 0);
  CHECK_EQ(drain(NULL, NULL), 0);
}

void test_printdoc_glyph_scales_and_stays_inside_margins(void) {
  uint8_t block[PRINTDOC_LINE_H_MAX][PRINT_ROW_BYTES];
  /* '!' is column 2 = 0x5F: bits 0-4 and 6. At 2x from x=10 that is a
   * 2-wide bar at x 14,15 on rows 0..9 and 12,13, nothing on 10,11. */
  memset(block, 0, sizeof block);
  printdoc_glyph(block, PRINTDOC_LINE_H_MAX, 10, 0, '!', 2, 0);
  CHECK_EQ(ink(block[2], 0, PRINT_WIDTH), 2);
  CHECK(px(block[2], 14) && px(block[2], 15));
  CHECK(!px(block[2], 13) && !px(block[2], 16));
  CHECK_EQ(ink(block[0], 0, PRINT_WIDTH), 2);
  CHECK_EQ(ink(block[10], 0, PRINT_WIDTH), 0);
  CHECK_EQ(ink(block[12], 0, PRINT_WIDTH), 2);
  CHECK_EQ(ink(block[14], 0, PRINT_WIDTH), 0);
  /* bold smears one pixel right */
  memset(block, 0, sizeof block);
  printdoc_glyph(block, PRINTDOC_LINE_H_MAX, 10, 0, '!', 2, 1);
  CHECK_EQ(ink(block[0], 0, PRINT_WIDTH), 3);
  /* a glyph past the right edge is clipped, not wrapped into the next row */
  memset(block, 0, sizeof block);
  printdoc_glyph(block, PRINTDOC_LINE_H_MAX, PRINT_WIDTH - 4, 0, '!', 2, 0);
  CHECK_EQ(ink(block[0], 0, PRINT_WIDTH), 0);
  /* rows past the block are dropped */
  memset(block, 0, sizeof block);
  printdoc_glyph(block, 2, 0, 1, '!', 2, 0);
  CHECK_EQ(ink(block[1], 0, PRINT_WIDTH), 2);
  CHECK_EQ(ink(block[2], 0, PRINT_WIDTH), 0);
}

void test_printdoc_text_line_is_2x_and_respects_margin(void) {
  PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  int n = 0, first_ink = -1;
  printdoc_begin(&d, "Hello");
  while (printdoc_next_row(&d, row)) {
    int x;
    for (x = 0; x < PRINT_WIDTH; x++)
      if (px(row, x)) { if (first_ink < 0 || x < first_ink) first_ink = x; }
    CHECK_EQ(ink(row, 0, PRINT_MARGIN), 0);
    CHECK_EQ(ink(row, PRINT_WIDTH - PRINT_MARGIN, PRINT_WIDTH), 0);
    n++;
  }
  CHECK(n >= 16 && n <= 24);        /* one 2x line and its gap */
  CHECK(first_ink >= PRINT_MARGIN && first_ink < PRINT_MARGIN + 4);
}

static int full_rows(const char *text) {
  PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  int full = 0;
  printdoc_begin(&d, text);
  while (printdoc_next_row(&d, row))
    if (ink(row, PRINT_MARGIN, PRINT_WIDTH - PRINT_MARGIN) == PRINT_WIDTH - 2 * PRINT_MARGIN)
      full++;
  return full;
}

void test_printdoc_heading_is_taller_and_ruled(void) {
  int text_rows = drain("Hello", NULL);
  int head_rows = drain("# Hello", NULL);
  int sub_rows = drain("## Hello", NULL);
  CHECK(head_rows > text_rows + 8);
  CHECK(sub_rows >= text_rows);
  /* the rule is a full-width inked row that a text line never has */
  CHECK_EQ(full_rows("Hello"), 0);
  {
    int f = full_rows("# Hello");
    CHECK(f >= 1 && f <= 3);
  }
}

void test_printdoc_rule_and_blank_lines(void) {
  int full = full_rows("---");
  int n = drain("---", NULL);
  CHECK(full >= 1 && full <= 3);
  CHECK(n > full);                                  /* padded */
  n = drain("\n\n", &full);
  CHECK(n > 0 && n < 32);
  CHECK_EQ(full, 0);                                /* blank lines are blank */
}

void test_printdoc_checkbox_is_drawn_not_typed(void) {
  PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  int box_rows = 0, ticked_ink = 0, open_ink = 0, text_ink = 0;
  printdoc_begin(&d, "[ ] Milk");
  while (printdoc_next_row(&d, row)) {
    /* the box lives in the first 24 columns past the margin */
    int b = ink(row, PRINT_MARGIN, PRINT_MARGIN + 24);
    if (b) box_rows++;
    open_ink += b;
  }
  CHECK(box_rows >= 12);                             /* a box, not a bracket */
  printdoc_begin(&d, "[x] Milk");
  while (printdoc_next_row(&d, row)) ticked_ink += ink(row, PRINT_MARGIN, PRINT_MARGIN + 24);
  CHECK(ticked_ink > open_ink);                     /* the tick adds ink */
  /* text starts after the box */
  printdoc_begin(&d, "[ ] I");
  while (printdoc_next_row(&d, row)) text_ink += ink(row, PRINT_MARGIN + 24, PRINT_WIDTH);
  CHECK(text_ink > 0);
}

void test_printdoc_wraps_long_lines_on_words(void) {
  /* 29 columns at 2x; this is 39 chars, so it must become two lines */
  int one = drain("abcdefghij", NULL);
  int two = drain("aaaa bbbb cccc dddd eeee ffff gggg hhhh", NULL);
  CHECK_EQ(two, one * 2);
  /* an unbreakable word is cut rather than lost: 60 chars = 3 lines */
  CHECK_EQ(drain("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL), one * 3);
  /* the second line of a wrapped checkbox item is indented past the box */
  {
    PrintDoc d;
    uint8_t row[PRINT_ROW_BYTES];
    int r = 0, box_ink_second_line = 0;
    printdoc_begin(&d, "[ ] aaaa bbbb cccc dddd eeee ffff gggg hhhh");
    while (printdoc_next_row(&d, row)) {
      if (r >= one) box_ink_second_line += ink(row, PRINT_MARGIN, PRINT_MARGIN + 24);
      r++;
    }
    CHECK_EQ(r, one * 2);
    CHECK_EQ(box_ink_second_line, 0);
  }
}

void test_printdoc_count_rows_matches_the_stream(void) {
  const char *doc = "# Groceries\n[ ] Milk\n[x] Eggs\n---\nprinted today\n";
  CHECK_EQ(printdoc_count_rows(doc), drain(doc, NULL));
  CHECK_EQ(printdoc_count_rows(""), 0);
}

void test_printdoc_tolerates_cr_and_missing_final_newline(void) {
  CHECK_EQ(drain("a\r\nb\r\n", NULL), drain("a\nb", NULL));
}

/* ---- with fonts ----
 *
 * A made-up font whose every printable character is a solid 4x10 block,
 * one pixel in from the pen and six rows down a 20-row line, advancing 6.
 * All the glyphs share one bitmap, which the format allows. So where ink
 * lands, and where a line breaks, can be worked out by hand. */

#include "kernel/ui/cfont.h"

static uint8_t s_fbuf[16 + 95 * 10 + 8];

static void make_block_font(CFont *f, int height) {
  uint8_t *p = s_fbuf, *t;
  int i;
  memset(s_fbuf, 0, sizeof s_fbuf);
  memcpy(p, "CFNT", 4);
  p[4] = 1; p[5] = 1; p[6] = (uint8_t)height; p[7] = (uint8_t)(height - 4);
  p[8] = ' '; p[10] = 95;
  for (i = 0; i < 95; i++) {
    t = p + 16 + i * 10;
    if (i == 0) { t[8] = 6; t[9] = 1; continue; }     /* space: no ink */
    t[4] = 4; t[5] = 10; t[6] = 1; t[7] = 6; t[8] = 6; t[9] = 1;
  }
  memset(p + 16 + 95 * 10, 0xFF, 5);                   /* 40 set bits */
  CHECK_EQ(cfont_parse(f, s_fbuf, sizeof s_fbuf), 0);
}

static int drain_fonts(const char *text, const PrintFonts *pf, int *inked,
                       int *first_x, int *widest) {
  static PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  int n = 0, x;
  *inked = 0; *first_x = -1; *widest = 0;
  printdoc_begin_fonts(&d, text, pf);
  while (printdoc_next_row(&d, row)) {
    int w = ink(row, 0, PRINT_WIDTH);
    n++;
    if (w) (*inked)++;
    if (w > *widest) *widest = w;
    for (x = 0; x < PRINT_WIDTH; x++)
      if (px(row, x) && (*first_x < 0 || x < *first_x)) *first_x = x;
    CHECK_EQ(ink(row, 0, PRINT_MARGIN), 0);
    CHECK_EQ(ink(row, PRINT_WIDTH - PRINT_MARGIN, PRINT_WIDTH), 0);
    if (n > 4000) break;
  }
  return n;
}

void test_printdoc_font_draws_a_line_in_that_font(void) {
  CFont f;
  PrintFonts pf = { 0, 0, 0 };
  int inked, first_x, widest, n;
  make_block_font(&f, 20);
  pf.body = &f;
  n = drain_fonts("ab", &pf, &inked, &first_x, &widest);
  CHECK_EQ(inked, 10);                     /* the block is ten rows tall */
  CHECK_EQ(first_x, PRINT_MARGIN + 1);     /* one pixel in from the pen */
  CHECK_EQ(widest, 8);                     /* two 4-wide blocks */
  CHECK(n >= 20 && n <= 26);               /* one line and its gap */
}

/* 352 usable pixels at 6 a character is 58 whole characters. */
void test_printdoc_font_wraps_by_pixel_width(void) {
  CFont f;
  PrintFonts pf = { 0, 0, 0 };
  char text[80];
  int inked, first_x, widest;
  make_block_font(&f, 20);
  pf.body = &f;
  memset(text, 'a', 58); text[58] = 0;
  drain_fonts(text, &pf, &inked, &first_x, &widest);
  CHECK_EQ(inked, 10);                     /* fits on one line */
  memset(text, 'a', 59); text[59] = 0;
  drain_fonts(text, &pf, &inked, &first_x, &widest);
  CHECK_EQ(inked, 20);                     /* one over: two lines */
  /* and at a space when there is one: 40 a's, a space, 30 a's is two
   * lines, the first holding only the 40. */
  memset(text, 'a', 71); text[40] = ' '; text[71] = 0;
  drain_fonts(text, &pf, &inked, &first_x, &widest);
  CHECK_EQ(inked, 20);
  CHECK_EQ(widest, 40 * 4);
}

void test_printdoc_font_heading_uses_the_heading_font_and_is_ruled(void) {
  CFont body, head;
  static uint8_t hbuf[sizeof s_fbuf];
  PrintFonts pf = { 0, 0, 0 };
  int inked, first_x, widest, n;
  make_block_font(&head, 30);
  memcpy(hbuf, s_fbuf, sizeof hbuf);
  CHECK_EQ(cfont_parse(&head, hbuf, sizeof hbuf), 0);
  make_block_font(&body, 20);
  pf.body = &body; pf.head = &head;
  n = drain_fonts("# Hi", &pf, &inked, &first_x, &widest);
  CHECK(n >= 30 + 4);                      /* taller than a body line */
  CHECK(widest >= PRINT_WIDTH - 2 * PRINT_MARGIN);   /* the rule */
  CHECK_EQ(inked, 10 + 2);                 /* the glyphs and the rule */
}

void test_printdoc_font_checkbox_sits_beside_its_text(void) {
  CFont f;
  PrintFonts pf = { 0, 0, 0 };
  int inked, first_x, widest;
  make_block_font(&f, 20);
  pf.body = &f;
  drain_fonts("[ ] x", &pf, &inked, &first_x, &widest);
  CHECK_EQ(first_x, PRINT_MARGIN);         /* the box's left edge */
  CHECK(inked >= 10 && inked <= 20);       /* box and text share rows */
}

/* No fonts is the old renderer, row for row. */
void test_printdoc_no_fonts_is_the_6x8_rendering(void) {
  PrintFonts none = { 0, 0, 0 };
  static const char *doc = "# Title\n[ ] one\n[x] two\n---\nplain text";
  static PrintDoc a, b;
  uint8_t ra[PRINT_ROW_BYTES], rb[PRINT_ROW_BYTES];
  int ga, gb, same = 1, n = 0;
  printdoc_begin(&a, doc);
  printdoc_begin_fonts(&b, doc, &none);
  do {
    ga = printdoc_next_row(&a, ra);
    gb = printdoc_next_row(&b, rb);
    if (ga != gb || (ga && memcmp(ra, rb, sizeof ra))) same = 0;
    n++;
  } while (ga && gb && n < 4000);
  CHECK(same);
  CHECK_EQ(printdoc_count_with(&b, doc, &none), printdoc_count_rows(doc));
}

/* ---- bitmap lines ---- */

void test_printdoc_bits_line_lights_the_runs(void) {
  PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  /* white 10, black 3, white 100, black 2: ink at 10..12 and 113..114 */
  printdoc_begin(&d, "%%10,3,100,2");
  CHECK(printdoc_next_row(&d, row));
  CHECK_EQ(ink(row, 0, PRINT_WIDTH), 5);
  CHECK(px(row, 10) && px(row, 12) && !px(row, 9) && !px(row, 13));
  CHECK(px(row, 113) && px(row, 114) && !px(row, 115));
  CHECK(!printdoc_next_row(&d, row));           /* one row, no gap under it */
}

void test_printdoc_bits_line_repeats_and_ends_black(void) {
  int inked;
  /* 5 identical rows; a black run at the end reaches the paper's edge */
  CHECK_EQ(drain("%%5*380,4", &inked), 5);
  CHECK_EQ(inked, 5);
  /* blank rows: just a count */
  CHECK_EQ(drain("%%7*", &inked), 7);
  CHECK_EQ(inked, 0);
  /* runs past the edge are clipped, not wrapped */
  CHECK_EQ(drain("%%0,1000", &inked), 1);
  CHECK_EQ(inked, 1);
  /* consecutive bitmap lines butt together: an image is many of them */
  CHECK_EQ(drain("%%3*0,1\n%%2*\n%%0,1", &inked), 6);
  CHECK_EQ(inked, 4);
}

void test_printdoc_not_quite_bits_is_text(void) {
  int inked;
  /* a LaTeX comment, a bad repeat, a word: all print as text, 2x high */
  CHECK(drain("%% comment", &inked) >= 16);
  CHECK(inked > 0);
  CHECK(drain("%%0*1,2", NULL) >= 16);
  CHECK(drain("%%999*1,2", NULL) >= 16);
  CHECK(drain("%%1,2x", NULL) >= 16);
  CHECK_EQ(printdoc_count_rows("%%4*1,1\nhi"), 4 + printdoc_count_rows("hi"));
}

void test_printdoc_bits_line_carries_raw_pixels_after_an_equals(void) {
  PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  /* 16 white, then base64: '/' is 63 = six black, 'g' is 32 = 100000,
   * 'A' is none -- so ink at 16..21 and 22, and nothing from 23 */
  printdoc_begin(&d, "%%16=/gA");
  CHECK(printdoc_next_row(&d, row));
  CHECK_EQ(ink(row, 0, PRINT_WIDTH), 7);
  CHECK(!px(row, 15) && px(row, 16) && px(row, 21) && px(row, 22) && !px(row, 23));
  CHECK(!printdoc_next_row(&d, row));
  /* no runs before it: raw from the paper's edge, and it repeats */
  CHECK_EQ(drain("%%3*=w", NULL), 3);
  printdoc_begin(&d, "%%=w");                    /* 'w' is 48 = 110000 */
  CHECK(printdoc_next_row(&d, row));
  CHECK(px(row, 0) && px(row, 1) && !px(row, 2));
  /* past the paper's edge is dropped */
  {
    char line[100];
    int i, inked;
    memcpy(line, "%%=", 3);
    for (i = 0; i < 90; i++) line[3 + i] = '/';
    line[93] = 0;
    CHECK_EQ(drain(line, &inked), 1);
    CHECK_EQ(inked, 1);
  }
  /* only base64 after the = */
  CHECK(drain("%%=ab,c", NULL) >= 16);
  CHECK(drain("%%1=a b", NULL) >= 16);
}
