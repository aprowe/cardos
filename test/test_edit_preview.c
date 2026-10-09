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
#include "fakeapi.h"

#define capp_info edit_capp_info
#define capp_main edit_capp_main
#include "apps/edit.c"
#undef capp_info
#undef capp_main

static CardApi FAKE;
static int FONTS_ON;

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

/* Every row the layout makes, as text, and how wide it is. Wider than a
 * line: a row of narrow letters can hold more than MAXCOL of them. */
static char ROW[64][256];
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
  fakeapi_init(&FAKE);
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
#define PAGE capp_rect(0, 0, 240, 400)
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
  md_render(capp_rect(0, 0, 240, 100), 0);
  CHECK_EQ(0, DRAWN_BELOW);
  CHECK_EQ(1, E.pmore);
  CHECK_EQ(30, NROWS);                            /* all laid out, some drawn */
}

void test_preview_the_last_screen_says_there_is_no_more(void) {
  setup(1);
  add_line("one");
  add_line("two");
  BOTTOM = 100;
  md_render(capp_rect(0, 0, 240, 100), 0);
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

/* ---- a card in memory: what load and save see ----------------------------
 *
 * A few files by name. The card's rename does not replace a file that is
 * there, so neither does this one -- which is what safefile works around. */

#define FS_FILES 4
static struct { char name[96]; char data[8192]; int len, used; } FS[FS_FILES];
static int FD_FILE, FD_POS;

static int fs_find(const char *p) {
  int i;
  for (i = 0; i < FS_FILES; i++) if (FS[i].used && strcmp(FS[i].name, p) == 0) return i;
  return -1;
}
static int f_open(const char *path, int flags) {
  int i = fs_find(path);
  if (flags & CAPP_O_WRITE) {
    if (i < 0) for (i = 0; i < FS_FILES && FS[i].used; i++) { }
    if (i >= FS_FILES) return -1;
    FS[i].used = 1;
    snprintf(FS[i].name, sizeof FS[i].name, "%s", path);
    FS[i].len = 0;
  } else if (i < 0) return -1;
  FD_FILE = i;
  FD_POS = 0;
  return 3;
}
static int f_read(int fd, void *b, size_t n) {
  int left = FS[FD_FILE].len - FD_POS;
  (void)fd;
  if ((int)n > left) n = (size_t)left;
  memcpy(b, FS[FD_FILE].data + FD_POS, n);
  FD_POS += (int)n;
  return (int)n;
}
static int f_write(int fd, const void *b, size_t n) {
  (void)fd;
  memcpy(FS[FD_FILE].data + FS[FD_FILE].len, b, n);
  FS[FD_FILE].len += (int)n;
  return (int)n;
}
static void f_close(int fd) { (void)fd; }
static int f_remove(const char *p) {
  int i = fs_find(p);
  if (i < 0) return -1;
  FS[i].used = 0;
  return 0;
}
static int f_rename(const char *a, const char *b) {
  int i = fs_find(a);
  if (i < 0 || fs_find(b) >= 0) return -1;
  snprintf(FS[i].name, sizeof FS[i].name, "%s", b);
  return 0;
}
static int f_stat(const char *p, CappStat *st) {
  if (fs_find(p) < 0) return -1;
  if (st) memset(st, 0, sizeof *st);
  return 0;
}

static int f_mkdir(const char *p) { (void)p; return 0; }
static void f_now(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->synced = 2; t->year = 2026; t->month = 9; t->day = 24;
  t->hour = 14; t->min = 32; t->sec = 7;
}

static void with_card(const char *path, const char *text) {
  memset(FS, 0, sizeof FS);
  FAKE.open = f_open; FAKE.read = f_read; FAKE.write = f_write;
  FAKE.close = f_close; FAKE.remove = f_remove; FAKE.rename = f_rename;
  FAKE.stat = f_stat;
  if (text) {
    FS[0].used = 1;
    snprintf(FS[0].name, sizeof FS[0].name, "%s", path);
    FS[0].len = (int)strlen(text);
    memcpy(FS[0].data, text, (size_t)FS[0].len);
  }
}

static const char *card_file(const char *path) {
  static char out[8192];
  int i = fs_find(path);
  if (i < 0) return NULL;
  memcpy(out, FS[i].data, (size_t)FS[i].len);
  out[FS[i].len] = 0;
  return out;
}

/* A paragraph written on the dashboard as one line, 180 characters. */
static const char PARA[] =
  "Notes written in a browser come as one line a paragraph, and this one "
  "is long enough that the old editor kept the first sixty-four characters "
  "and dropped the rest without a word.";

/* The complaint: words cut off mid-sentence. It was load, not the wrap --
 * everything past column 64 was dropped, and gone at the next save. */
static int total_chars(void) {
  int i, n = 0;
  for (i = 0; i < E.nlines; i++) n += E.len[i];
  return n;
}

void test_edit_a_long_line_loads_whole_and_saves_back_as_one_line(void) {
  char file[512];
  int i, n = 0;
  setup(1);
  snprintf(file, sizeof file, "# Title\n%s\nlast\n", PARA);
  with_card("/home/notes/a.md", file);
  load("/home/notes/a.md");
  CHECK_EQ(0, E.truncated);
  CHECK(E.nlines > 4);                          /* the paragraph took several */
  for (i = 0; i < E.nlines; i++) CHECK(E.len[i] <= MAXCOL);
  /* Joined back, the paragraph is all there. */
  for (i = 1; E.cont[i]; i++) n += E.len[i];
  n += E.len[i];
  CHECK_EQ((int)strlen(PARA), n);
  save();
  CHECK(card_file("/home/notes/a.md") != NULL);
  /* Byte for byte, the trailing newline included: it loads as an empty
   * last line, and save puts a newline between lines. */
  CHECK(strcmp(card_file("/home/notes/a.md"), file) == 0);
  CHECK(card_file("/home/notes/a.md.tmp") == NULL);
}

void test_edit_a_long_line_breaks_between_words(void) {
  int i;
  setup(1);
  with_card("/n.md", PARA);
  load("/n.md");
  /* Every line but the last ends in a space: the break is after a word. */
  for (i = 0; i + 1 < E.nlines; i++) CHECK(E.line[i][E.len[i] - 1] == ' ');
}

void test_edit_the_preview_lays_out_a_continued_line_as_one(void) {
  char joined[1024];
  setup(1);
  with_card("/n.md", PARA);
  load("/n.md");
  md_render(PAGE, 0);
  join_rows(joined, sizeof joined);
  CHECK(strcmp(joined, PARA) == 0);
}

void test_edit_typing_into_a_full_line_carries_on(void) {
  int i;
  setup(1);
  blank();
  for (i = 0; i < MAXCOL + 10; i++) insert_char(i % 8 == 7 ? ' ' : 'a');
  CHECK_EQ(2, E.nlines);
  CHECK_EQ(1, E.cont[0]);
  CHECK_EQ(MAXCOL + 10, E.len[0] + E.len[1]);
  CHECK_EQ(1, E.cy);
  CHECK_EQ(E.len[1], E.cx);
}

void test_edit_backspace_across_a_continuation_deletes_a_character(void) {
  setup(1);
  with_card("/n.md", PARA);
  load("/n.md");
  E.cy = 1; E.cx = 0;
  {
    int before = total_chars();
    char last = E.line[0][E.len[0] - 1];
    CHECK(last == ' ');
    backspace();
    CHECK_EQ(before - 1, total_chars());        /* one character, not zero */
    CHECK(E.line[0][E.len[0] - 1] != ' ');
  }
}

void test_edit_enter_at_a_continuation_makes_a_real_line(void) {
  setup(1);
  with_card("/n.md", PARA);
  load("/n.md");
  E.cy = 0; E.cx = 5;
  split_line();
  CHECK_EQ(0, E.cont[0]);
  CHECK_EQ(1, E.cont[1]);                       /* the rest still runs on */
}

void test_edit_a_save_cut_short_is_put_back_on_open(void) {
  setup(1);
  with_card("/n.md.tmp", "saved text\n");      /* only the temp survived */
  load("/n.md");
  CHECK(strncmp(E.line[0], "saved text", 10) == 0);
  CHECK(card_file("/n.md") != NULL);
}

/* ---- wrap ---------------------------------------------------------------- */

void test_edit_wrap_rows_break_after_a_space_and_lose_nothing(void) {
  int k, line, from, to, total = 0;
  setup(1);
  add_line("The quick brown fox jumps over the lazy dog while a small");
  E.cont[0] = 0;
  E.top = 0;
  for (k = 0; wrap_row(k, 20, &line, &from, &to); k++) {
    CHECK(to - from <= 20);
    if (to < E.len[0]) CHECK(E.line[0][to - 1] == ' ');
    total += to - from;
  }
  CHECK(k >= 3);
  CHECK_EQ(E.len[0], total);
}

void test_edit_wrap_the_caret_at_the_end_of_a_full_row_gets_a_row(void) {
  int line, from, to;
  setup(1);
  add_line("aaaaaaaaaaaaaaaaaaaa");                /* exactly 20, no space */
  E.top = 0;
  CHECK_EQ(2, wrap_rows(0, 20, -1));
  CHECK_EQ(1, wrap_rows(0, 20, 20));             /* the caret at the end */
  CHECK(wrap_row(1, 20, &line, &from, &to));
  CHECK_EQ(20, from);
  CHECK_EQ(20, to);
}

void test_edit_wrap_scrolls_by_rows_to_keep_the_caret_on_screen(void) {
  int i;
  setup(1);
  for (i = 0; i < 10; i++) add_line("one two three four five six seven eight");
  E.wrap = 1;
  E.top = 0;
  E.cy = 9; E.cx = 0;
  scroll_wrapped(6, 20);
  /* Each line folds into three rows at 20, so six rows hold the caret's
   * line and the one before it at most. */
  CHECK(E.top >= 7);
  CHECK(E.top <= 9);
}

/* ---- keys in the preview ------------------------------------------------- */

void test_edit_escape_in_the_preview_goes_back_to_editing(void) {
  setup(1);
  add_line("text");
  E.view = VIEW_PREVIEW;
  CHECK_EQ(1, key_preview(CAPP_KEY_ESC));
  CHECK_EQ(VIEW_EDIT, E.view);
}

void test_edit_ctrl_p_toggles_from_either_view(void) {
  setup(1);
  add_line("text");
  E.view = VIEW_EDIT;
  do_action(ACT_PREVIEW);
  CHECK_EQ(VIEW_PREVIEW, E.view);
  do_action(ACT_PREVIEW);
  CHECK_EQ(VIEW_EDIT, E.view);
}

void test_edit_the_note_command_writes_markdown_where_notes_looks(void) {
  char out[128];
  const char *argv1[1];
  setup(1);
  with_card("/x", NULL);
  FAKE.now = f_now;
  FAKE.mkdir = f_mkdir;
  argv1[0] = "buy milk";
  CHECK_EQ(0, app_command(NULL, ACT_NOTE, 1, argv1, out, sizeof out));
  CHECK(strcmp(out, "saved " CAPP_HOME "/notes/0924-143207.md") == 0);
  CHECK(card_file(CAPP_HOME "/notes/0924-143207.md") != NULL);
  CHECK(strcmp(card_file(CAPP_HOME "/notes/0924-143207.md"), "buy milk\n") == 0);
}
