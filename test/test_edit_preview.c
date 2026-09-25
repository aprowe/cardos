/* Edit's markdown preview: the wrap, on the host.
 *
 * The preview used to cut a long line at the edge of the screen and draw
 * nothing of the rest. These check that every character of a line lands on
 * some row, that no row is wider than the page, that a line breaks at a space
 * when it can and inside a word only when it must, and that the inline
 * markers come out without taking the text with them. The font is a fake
 * with uneven widths, so a wrap that assumed 6 pixels a character fails.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info edit_capp_info
#define capp_main edit_capp_main
#include "apps/edit.c"
#undef capp_info
#undef capp_main

static CardApi FAKE;
static int FONTS_ON;

static int f_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
static void *f_memset(void *d, int c, size_t n)         { return memset(d, c, n); }
static void *f_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t f_strlen(const char *s)                   { return strlen(s); }
static void f_fill(CRect r, uint16_t c)                 { (void)r; (void)c; }
static void f_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  (void)x; (void)y; (void)s; (void)fg; (void)bg;
}

/* 1 is the body, 2 the bold; -1 the 6x8. */
static int f_font_load(const char *name) {
  if (!FONTS_ON) return -1;
  return strcmp(name, "ui13b") == 0 ? 2 : 1;
}
static int f_char_w(int font, char c) {
  if (font < 0) return 6;
  if (c == ' ') return 3;
  if (c == 'm' || c == 'w' || c == 'M' || c == 'W') return 10;
  if (c == 'i' || c == 'l' || c == '.' || c == ',') return 3;
  return font == 2 ? 7 : 6;
}
static int f_text_width(int font, const char *s) {
  int w = 0;
  while (*s) w += f_char_w(font, *s++);
  return w;
}
static int f_font_height(int font) { return font < 0 ? 8 : 15; }

static int DRAWN_BELOW;
static int BOTTOM;
static void f_text_font(int font, int16_t x, int16_t y, const char *s,
                        uint16_t fg, uint16_t bg) {
  (void)x; (void)s; (void)fg; (void)bg;
  if (y + f_font_height(font) > BOTTOM) DRAWN_BELOW++;
}

/* Every row the layout makes, as text, and how wide it is. */
static char ROW[64][MAXCOL + 1];
static int  ROW_W[64];
static int  NROWS;

static void grab(int line, int from, int to) {
  (void)line;
  if (NROWS >= 64) return;
  memcpy(ROW[NROWS], M.ch + from, (size_t)(to - from));
  ROW[NROWS][to - from] = 0;
  ROW_W[NROWS] = md_width(from, to, 0);
  NROWS++;
}

static void setup(int fonts) {
  memset(&FAKE, 0, sizeof FAKE);
  FAKE.fmt = f_fmt;
  FAKE.mem_set = f_memset;
  FAKE.mem_cpy = f_memcpy;
  FAKE.str_len = f_strlen;
  FAKE.fill = f_fill;
  FAKE.text = f_text;
  FAKE.font_load = f_font_load;
  FAKE.text_width = f_text_width;
  FAKE.font_height = f_font_height;
  FAKE.text_font = f_text_font;
  api = &FAKE;
  FONTS_ON = fonts;
  memset(&E, 0, sizeof E);
  memset(&M, 0, sizeof M);
  E.nlines = 0;
  NROWS = 0;
  DRAWN_BELOW = 0;
  md_row_hook = grab;
}

static void add_line(const char *s) {
  int n = (int)strlen(s);
  memcpy(E.line[E.nlines], s, (size_t)n);
  E.len[E.nlines] = (short)n;
  E.nlines++;
}

/* The page: 240 wide, the margins take 8, so a text row has 232. */
#define PAGE rect(0, 0, 240, 400)
#define AVAIL (240 - PG_LEFT * 2)

/* Joined back with single spaces, the rows must be the line again. */
static void join_rows(char *out, size_t n) {
  int i;
  out[0] = 0;
  for (i = 0; i < NROWS; i++) {
    if (i) strncat(out, " ", n - strlen(out) - 1);
    strncat(out, ROW[i], n - strlen(out) - 1);
  }
}

static const char LONG[] =
  "The quick brown fox jumps over the lazy dog while a small wombat "
  "watches.";

void test_preview_a_long_line_wraps_and_loses_nothing(void) {
  char joined[256];
  int i;
  setup(1);
  add_line("The quick brown fox jumps over the lazy dog while a small wombat");
  md_render(PAGE, 0);
  CHECK(NROWS >= 2);
  join_rows(joined, sizeof joined);
  CHECK(strcmp(joined, "The quick brown fox jumps over the lazy dog while a small wombat") == 0);
  for (i = 0; i < NROWS; i++) {
    CHECK(ROW_W[i] <= AVAIL);
    CHECK(ROW[i][0] != ' ');                    /* no row starts with the gap */
  }
}

void test_preview_breaks_at_a_space_when_one_fits(void) {
  int i, j;
  setup(1);
  add_line("The quick brown fox jumps over the lazy dog while a small wombat");
  md_render(PAGE, 0);
  /* Every row but the last ends at the end of a word of the line. */
  for (i = 0; i + 1 < NROWS; i++) {
    const char *at = strstr("The quick brown fox jumps over the lazy dog while a small wombat", ROW[i]);
    CHECK(at != NULL);
    j = (int)strlen(ROW[i]);
    CHECK(at && (at[j] == ' ' || at[j] == 0));
  }
}

void test_preview_the_rows_are_as_full_as_they_can_be(void) {
  int i;
  setup(1);
  add_line("The quick brown fox jumps over the lazy dog while a small wombat");
  md_render(PAGE, 0);
  /* The next word would not have fitted on any row but the last. */
  for (i = 0; i + 1 < NROWS; i++) {
    char next[MAXCOL + 1];
    char with[2 * MAXCOL + 2];
    sscanf(ROW[i + 1], "%64s", next);
    snprintf(with, sizeof with, "%s %s", ROW[i], next);
    CHECK(f_text_width(1, with) > AVAIL);
  }
}

void test_preview_a_word_wider_than_the_page_is_broken_inside(void) {
  char word[MAXCOL + 1], joined[256];
  int i;
  setup(1);
  memset(word, 'm', 60);                        /* 600 px of m */
  word[60] = 0;
  add_line(word);
  md_render(PAGE, 0);
  CHECK(NROWS >= 3);
  join_rows(joined, sizeof joined);
  /* Joined with spaces that were never there: strip them to compare. */
  {
    char solid[256];
    int k = 0;
    for (i = 0; joined[i]; i++) if (joined[i] != ' ') solid[k++] = joined[i];
    solid[k] = 0;
    CHECK(strcmp(solid, word) == 0);
  }
  for (i = 0; i < NROWS; i++) CHECK(ROW_W[i] <= AVAIL);
}

void test_preview_markers_come_out_and_the_text_stays(void) {
  setup(1);
  md_inline("**bold** and `x = 1` and [a link](http://x.y) end", 0);
  CHECK(strcmp(M.ch, "bold and x = 1 and a link end") == 0);
  CHECK_EQ(ST_B, M.st[0]);
  CHECK_EQ(ST_N, M.st[5]);
  CHECK_EQ(ST_C, M.st[9]);                       /* the x */
  CHECK_EQ(ST_L, M.st[19]);                      /* the a of "a link" */
}

void test_preview_an_underscore_inside_a_word_is_kept(void) {
  setup(1);
  md_inline("call snake_case_name and _this_", 0);
  CHECK(strcmp(M.ch, "call snake_case_name and this") == 0);
  CHECK_EQ(ST_E, M.st[25]);
}

void test_preview_a_fenced_block_is_verbatim_and_wraps_in_6x8(void) {
  char joined[256];
  const char *code = "x = **not bold** + some_long_identifier_name(1, 2, 3)";
  setup(1);
  add_line("```");
  add_line(code);
  add_line("```");
  md_render(PAGE, 0);
  CHECK(NROWS >= 2);                              /* 53 chars of 6 px > 232 */
  join_rows(joined, sizeof joined);
  CHECK(strcmp(joined, code) == 0);
}

void test_preview_a_list_item_wraps_under_its_text(void) {
  char joined[256];
  setup(1);
  add_line("- The quick brown fox jumps over the lazy dog while a small wombat");
  md_render(PAGE, 0);
  CHECK(NROWS >= 2);
  join_rows(joined, sizeof joined);
  CHECK(strcmp(joined, "The quick brown fox jumps over the lazy dog while a small wombat") == 0);
}

void test_preview_without_fonts_still_wraps(void) {
  char joined[256];
  int i;
  setup(0);
  add_line("The quick brown fox jumps over the lazy dog while a small wombat");
  md_render(PAGE, 0);
  CHECK(NROWS >= 2);
  join_rows(joined, sizeof joined);
  CHECK(strcmp(joined, "The quick brown fox jumps over the lazy dog while a small wombat") == 0);
  for (i = 0; i < NROWS; i++) CHECK(ROW_W[i] <= AVAIL);
}

void test_preview_nothing_is_drawn_past_the_bottom(void) {
  int i;
  setup(1);
  for (i = 0; i < 30; i++) add_line("a line of text");
  BOTTOM = 100;
  md_render(rect(0, 0, 240, 100), 0);
  CHECK_EQ(0, DRAWN_BELOW);
  CHECK_EQ(1, E.pmore);
  CHECK_EQ(30, NROWS);                            /* all laid out, some drawn */
}

void test_preview_the_last_screen_says_there_is_no_more(void) {
  setup(1);
  add_line("one");
  add_line("two");
  BOTTOM = 100;
  md_render(rect(0, 0, 240, 100), 0);
  CHECK_EQ(0, E.pmore);
}

void test_preview_an_empty_heading_draws_nothing_odd(void) {
  setup(1);
  add_line("# ");
  add_line("#");
  md_render(PAGE, 0);
  CHECK(NROWS >= 1);
  CHECK(strcmp(ROW[0], "") == 0);
}
