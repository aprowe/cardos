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
