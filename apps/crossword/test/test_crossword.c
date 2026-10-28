/* Crossword's puzzle model: apps/crossword/xwcore.h.
 *
 * Two small fixtures carry most of it. FIX is a 5x5 word square with a black
 * square in two corners, ten clues and a circled square -- every square is in
 * two words:
 *
 *      0 1 2 3 4         across: 1 HEAR  5 EMBER  7 ABUSE  8 RESIN  9 REND
 *   0  H E A R .         down:   1 HEAR  2 EMBER  3 ABUSE  4 RESIN  6 REND
 *   1  E M B E R         (clue indexes 0-4 are the across, 5-9 the down)
 *   2  A B u S E         the u is circled
 *   3  R E S I N
 *   4  . R E N D
 *
 * FRAME is a ring of four words round a black middle, so the squares inside
 * the ring are in one word only and an arrow has black squares to jump:
 *
 *   0  S L A T E         across: 1 SLATE  3 SEALS    (clues 0, 1)
 *   1  T . . . A         down:   1 STARS  2 EARLS    (clues 2, 3)
 *   2  A . . . R
 *   3  R . . . L
 *   4  S E A L S
 */
#include <string.h>

#include "tinytest.h"
#include "apps/crossword/xwcore.h"
#include "apps/crossword/xwstats.h"

#define FIX_HEAD   "xw 1\ntitle Mini Heart\nby A. Setter\ndate 1993-11-21\nnote Circled letter: U\n"
#define FIX_ROWS   "size 5 5\nrow HEAR.\nrow EMBER\nrow ABuSE\nrow RESIN\nrow .REND\n"
#define FIX_ACROSS "A 1 Catch with the ears\nA 5 Smoldering bit\nA 7 Mistreat\nA 8 Pine product\nA 9 Tear, as a seam\n"
#define FIX_DOWN   "D 1 Listen up\nD 2 Fire remnant\nD 3 Misuse\nD 4 Sticky stuff\nD 6 Rip apart\n"
static const char FIX[] = FIX_HEAD FIX_ROWS FIX_ACROSS FIX_DOWN "end\n";

#define FRAME_BODY  "size 5 5\nrow SLATE\nrow T...A\nrow A...R\nrow R...L\nrow SEALS\n"
#define FRAME_CLUES "A 1 Roof material\nA 3 Barkers at the zoo\nD 1 Night lights\nD 2 Nobles\n"
#define FRAME_TEXT  "xw 1\ntitle Frame\n" FRAME_BODY FRAME_CLUES "end\n"
static const char FRAME[] = FRAME_TEXT;

static Xw P1, P2;
static char BIG[16384];
static char SAVE[XW_SAVE_MAX];

static void load_fix(Xw *p)   { CHECK_EQ(xw_parse(p, FIX), XW_OK); }
static void load_frame(Xw *p) { CHECK_EQ(xw_parse(p, FRAME), XW_OK); }

/* Cursor onto a square and a letter into it. */
static int put(Xw *p, int cell, int ch) {
  xw_goto(p, cell);
  return xw_type(p, ch);
}

/* The answer into every white square in turn but `skip` (-1 for none); what
 * the last put answered. */
static int solve(Xw *p, int skip) {
  int i, r = 0;
  for (i = 0; i < p->w * p->h; i++)
    if (p->cell[i].sol && i != skip) r = put(p, i, p->cell[i].sol);
  return r;
}

static int app(char *out, int at, const char *s) {
  while (*s) out[at++] = *s++;
  out[at] = 0;
  return at;
}

/* A 21x21 grid with no black square: 21 across, 21 down, numbered 1-21 along
 * the top and 22-41 down the left side, every clue `clue_chars` x's long. */
static int build_open_grid(char *out, int clue_chars) {
  char tmp[24], txt[300];
  int r, c, k, at = 0;
  for (k = 0; k < clue_chars; k++) txt[k] = 'x';
  txt[clue_chars] = 0;
  at = app(out, at, "xw 1\nsize 21 21\n");
  for (r = 0; r < 21; r++) {
    at = app(out, at, "row ");
    for (c = 0; c < 21; c++) out[at++] = 'A';
    out[at++] = '\n';
    out[at] = 0;
  }
  for (r = 0; r < 21; r++) {
    sprintf(tmp, "A %d ", r ? 21 + r : 1);
    at = app(out, at, tmp);
    at = app(out, at, txt);
    at = app(out, at, "\n");
  }
  for (c = 0; c < 21; c++) {
    sprintf(tmp, "D %d ", c + 1);
    at = app(out, at, tmp);
    at = app(out, at, txt);
    at = app(out, at, "\n");
  }
  return app(out, at, "end\n");
}

/* ---- loading ---------------------------------------------------------------- */

void test_crossword_parses_the_grid_and_numbers_it(void) {
  static const int want[25] = { 1, 2, 3, 4, 0,
                                5, 0, 0, 0, 6,
                                7, 0, 0, 0, 0,
                                8, 0, 0, 0, 0,
                                0, 9, 0, 0, 0 };
  static const int num[10]   = { 1, 5, 7, 8, 9,  1, 2, 3, 4, 6 };
  static const int start[10] = { 0, 5, 10, 15, 21,  0, 1, 2, 3, 9 };
  static const int len[10]   = { 4, 5, 5, 5, 4,  4, 5, 5, 5, 4 };
  int i, bad = 0;
  load_fix(&P1);
  CHECK(P1.ready);
  CHECK_EQ(P1.w, 5); CHECK_EQ(P1.h, 5);
  CHECK_EQ(P1.nclues, 10); CHECK_EQ(P1.nacross, 5);
  CHECK(!strcmp(P1.title, "Mini Heart"));
  CHECK(!strcmp(P1.by, "A. Setter"));
  CHECK(!strcmp(P1.note, "Circled letter: U"));
  CHECK_EQ(P1.year, 1993); CHECK_EQ(P1.month, 11); CHECK_EQ(P1.day, 21);
  CHECK_EQ(xw_puzzle_weekday(&P1), 6);
  CHECK(!strcmp(xw_day_name(6), "Sun"));

  CHECK_EQ(P1.cell[0].sol, 'H');
  CHECK_EQ(P1.cell[4].sol, 0);                    /* black */
  CHECK_EQ(P1.cell[20].sol, 0);
  CHECK_EQ(P1.cell[24].sol, 'D');
  CHECK_EQ(P1.cell[12].sol, 'U');                 /* the circled one is upper-cased */
  CHECK(P1.cell[12].fl & XW_F_CIRCLE);
  CHECK_EQ(P1.cell[11].fl, 0);
  CHECK_EQ(P1.cell[0].ent, 0);

  for (i = 0; i < 25; i++) if (xw_number_at(&P1, i) != want[i]) bad++;
  CHECK_EQ(bad, 0);

  bad = 0;
  for (i = 0; i < 10; i++)
    if (P1.clue[i].num != num[i] || P1.clue[i].cell != start[i] || P1.clue[i].len != len[i] ||
        P1.clue[i].dir != (i < 5 ? XW_ACROSS : XW_DOWN)) bad++;
  CHECK_EQ(bad, 0);

  CHECK(!strcmp(xw_clue_text(&P1, 0), "Catch with the ears"));
  CHECK(!strcmp(xw_clue_text(&P1, 3), "Pine product"));
  CHECK(!strcmp(xw_clue_text(&P1, 4), "Tear, as a seam"));
  CHECK(!strcmp(xw_clue_text(&P1, 5), "Listen up"));
  CHECK(!strcmp(xw_clue_text(&P1, 9), "Rip apart"));

  /* Each square knows its two words. */
  CHECK_EQ(xw_clue_at(&P1, 12, XW_ACROSS), 2);    /* 7-Across */
  CHECK_EQ(xw_clue_at(&P1, 12, XW_DOWN), 7);      /* 3-Down */
  CHECK_EQ(xw_clue_at(&P1, 9, XW_ACROSS), 1);     /* 5-Across */
  CHECK_EQ(xw_clue_at(&P1, 9, XW_DOWN), 9);       /* 6-Down */
  CHECK_EQ(xw_clue_at(&P1, 21, XW_ACROSS), 4);
  CHECK_EQ(xw_clue_at(&P1, 21, XW_DOWN), 6);
  CHECK_EQ(xw_clue_cell(&P1, 9, 0), 9);
  CHECK_EQ(xw_clue_cell(&P1, 9, 3), 24);
  CHECK_EQ(xw_clue_pos(&P1, 9, 14), 1);
  CHECK_EQ(xw_clue_pos(&P1, 9, 4), -1);           /* above the word */
  CHECK_EQ(xw_clue_pos(&P1, 9, 10), -1);          /* same row start, other column */
  CHECK_EQ(xw_clue_pos(&P1, 4, 20), -1);          /* before 9-Across */
  CHECK_EQ(xw_clue_pos(&P1, 4, 24), 3);

  /* The cursor starts on 1-Across. */
  CHECK_EQ(P1.cur, 0); CHECK_EQ(P1.dir, XW_ACROSS);
  CHECK_EQ(xw_cur_clue(&P1), 0);
  CHECK(!P1.solved);
}

void test_crossword_a_square_in_one_word_has_no_other(void) {
  load_frame(&P1);
  CHECK_EQ(P1.nclues, 4); CHECK_EQ(P1.nacross, 2);
  CHECK_EQ(xw_number_at(&P1, 0), 1);
  CHECK_EQ(xw_number_at(&P1, 4), 2);
  CHECK_EQ(xw_number_at(&P1, 20), 3);
  CHECK_EQ(xw_number_at(&P1, 1), 0);
  CHECK_EQ(xw_number_at(&P1, 5), 0);
  CHECK_EQ(xw_clue_at(&P1, 5, XW_ACROSS), -1);    /* inside the ring: down only */
  CHECK_EQ(xw_clue_at(&P1, 5, XW_DOWN), 2);
  CHECK_EQ(xw_clue_at(&P1, 1, XW_ACROSS), 0);     /* on the top: across only */
  CHECK_EQ(xw_clue_at(&P1, 1, XW_DOWN), -1);
  CHECK_EQ(xw_clue_at(&P1, 21, XW_ACROSS), 1);
  CHECK_EQ(xw_clue_at(&P1, 21, XW_DOWN), -1);
  CHECK_EQ(xw_clue_at(&P1, 0, XW_ACROSS), 0);
  CHECK_EQ(xw_clue_at(&P1, 0, XW_DOWN), 2);
  CHECK(!strcmp(xw_clue_text(&P1, 1), "Barkers at the zoo"));
  CHECK(!strcmp(xw_clue_text(&P1, 2), "Night lights"));
  CHECK(!strcmp(xw_clue_text(&P1, 3), "Nobles"));
  CHECK_EQ(P1.year, 0);
  CHECK_EQ(xw_puzzle_weekday(&P1), -1);
}

void test_crossword_loading_forgives_what_a_converter_may_do(void) {
  /* Windows line ends. */
  int n = 0;
  const char *s;
  for (s = FIX; *s; s++) {
    if (*s == '\n') BIG[n++] = '\r';
    BIG[n++] = *s;
  }
  BIG[n] = 0;
  CHECK_EQ(xw_parse(&P1, BIG), XW_OK);
  CHECK(!strcmp(P1.title, "Mini Heart"));
  CHECK(!strcmp(xw_clue_text(&P1, 9), "Rip apart"));
  CHECK_EQ(P1.nclues, 10);

  /* Blank lines, lines of spaces, and keys from a later version. */
  CHECK_EQ(xw_parse(&P1, "xw 1\n\n   \nfuture 7\ntag x y\n" FRAME_BODY "\n" FRAME_CLUES "end\n"), XW_OK);
  CHECK_EQ(P1.nclues, 4);

  /* Lines after `end` are never read. */
  CHECK_EQ(xw_parse(&P1, FRAME_TEXT "A 99 junk\nrow nonsense\n"), XW_OK);
  CHECK_EQ(P1.nclues, 4);

  /* Clues in any mix of across and down, as long as each kind is in order. */
  CHECK_EQ(xw_parse(&P1, "xw 1\n" FIX_ROWS
                         "A 1 a1\nD 1 d1\nA 5 a5\nD 2 d2\nA 7 a7\nD 3 d3\nA 8 a8\nD 4 d4\nA 9 a9\nD 6 d6\nend\n"),
           XW_OK);
  CHECK(!strcmp(xw_clue_text(&P1, 0), "a1"));
  CHECK(!strcmp(xw_clue_text(&P1, 1), "a5"));
  CHECK(!strcmp(xw_clue_text(&P1, 4), "a9"));
  CHECK(!strcmp(xw_clue_text(&P1, 5), "d1"));
  CHECK(!strcmp(xw_clue_text(&P1, 6), "d2"));
  CHECK(!strcmp(xw_clue_text(&P1, 9), "d6"));

  /* An empty clue is allowed; it is just empty. */
  CHECK_EQ(xw_parse(&P1, "xw 1\nsize 2 1\nrow AB\nA 1\nend\n"), XW_OK);
  CHECK(!strcmp(xw_clue_text(&P1, 0), ""));
}

void test_crossword_text_is_made_safe_to_draw_and_cut_to_fit(void) {
  int at;
  /* A tab becomes a space, a byte past ASCII a '?'. */
  CHECK_EQ(xw_parse(&P1, "xw 1\n" FRAME_BODY "A 1 Caf\xe9" "\tau lait\n" "A 3 Seals\nD 1 Stars\nD 2 Nobles\nend\n"), XW_OK);
  CHECK(!strcmp(xw_clue_text(&P1, 0), "Caf? au lait"));
  CHECK(!strcmp(xw_clue_text(&P1, 1), "Seals"));

  /* Titles, authors, notes and clues are cut, and the clues after are not hurt. */
  at = app(BIG, 0, "xw 1\ntitle ");
  { int k; for (k = 0; k < 100; k++) BIG[at++] = 't'; }
  at = app(BIG, at, "\nby ");
  { int k; for (k = 0; k < 100; k++) BIG[at++] = 'b'; }
  at = app(BIG, at, "\nnote ");
  { int k; for (k = 0; k < 300; k++) BIG[at++] = 'n'; }
  at = app(BIG, at, "\n" FRAME_BODY "A 1 ");
  { int k; for (k = 0; k < 300; k++) BIG[at++] = 'y'; }
  app(BIG, at, "\nA 3 s\nD 1 t\nD 2 u\nend\n");
  CHECK_EQ(xw_parse(&P1, BIG), XW_OK);
  CHECK_EQ((int)strlen(P1.title), XW_TITLE_MAX);
  CHECK_EQ((int)strlen(P1.by), XW_BY_MAX);
  CHECK_EQ((int)strlen(P1.note), XW_NOTE_MAX);
  CHECK_EQ((int)strlen(xw_clue_text(&P1, 0)), XW_CLUE_MAX);
  CHECK(!strcmp(xw_clue_text(&P1, 1), "s"));
  CHECK(!strcmp(xw_clue_text(&P1, 2), "t"));
  CHECK(!strcmp(xw_clue_text(&P1, 3), "u"));
}

void test_crossword_dates_are_checked(void) {
  CHECK_EQ(xw_parse(&P1, "xw 1\ndate 2000-02-29\n" FRAME_BODY FRAME_CLUES "end\n"), XW_OK);
  CHECK_EQ(P1.year, 2000); CHECK_EQ(P1.month, 2); CHECK_EQ(P1.day, 29);
  CHECK_EQ(xw_puzzle_weekday(&P1), 1);            /* a Tuesday */
  /* A bad date does not spoil the puzzle; there is just no date. */
  CHECK_EQ(xw_parse(&P1, "xw 1\ndate 1900-02-29\n" FRAME_BODY FRAME_CLUES "end\n"), XW_OK);
  CHECK_EQ(P1.year, 0); CHECK_EQ(xw_puzzle_weekday(&P1), -1);
  CHECK_EQ(xw_parse(&P1, "xw 1\ndate 2012-02-30\n" FRAME_BODY FRAME_CLUES "end\n"), XW_OK);
  CHECK_EQ(P1.year, 0);
  CHECK_EQ(xw_parse(&P1, "xw 1\ndate soon\n" FRAME_BODY FRAME_CLUES "end\n"), XW_OK);
  CHECK_EQ(P1.year, 0);
  CHECK_EQ(xw_parse(&P1, "xw 1\ndate 1942-02-15\n" FRAME_BODY FRAME_CLUES "end\n"), XW_OK);
  CHECK_EQ(xw_puzzle_weekday(&P1), 6);            /* the first NYT crossword, a Sunday */

  CHECK_EQ(xw_days(1970, 1, 1), 0);
  CHECK_EQ(xw_days(1970, 1, 2), 1);
  CHECK_EQ(xw_days(2026, 1, 1), 20454);
  CHECK_EQ(xw_weekday(1970, 1, 1), 3);            /* Thursday */
  CHECK_EQ(xw_weekday(2026, 10, 27), 1);
  CHECK_EQ(xw_weekday(1993, 11, 21), 6);
  CHECK_EQ(xw_weekday(2000, 1, 1), 5);
  CHECK(xw_valid_date(2000, 2, 29));
  CHECK(!xw_valid_date(1900, 2, 29));
  CHECK(!xw_valid_date(2012, 4, 31));
  CHECK(!xw_valid_date(2012, 13, 1));
  CHECK(!xw_valid_date(2012, 0, 1));
  CHECK(!xw_valid_date(0, 1, 1));
  CHECK(!strcmp(xw_day_name(0), "Mon"));
  CHECK(!strcmp(xw_day_name(9), "???"));
}

struct Bad { const char *text; int err; };

void test_crossword_refuses_a_puzzle_that_is_not_one(void) {
  static const struct Bad BAD[] = {
    /* not our file, or cut short */
    { "size 5 5\n",                                   XW_ERR_HEADER },
    { "xw 2\n",                                       XW_ERR_HEADER },
    { "xw\n",                                         XW_ERR_HEADER },
    { "",                                             XW_ERR_TRUNC },
    { "xw 1\n",                                       XW_ERR_TRUNC },
    { "xw 1\nsize 2 1\nrow AB\nA 1 x\n",              XW_ERR_TRUNC },
    /* the size */
    { "xw 1\nsize 22 5\n",                            XW_ERR_SIZE },
    { "xw 1\nsize 5 22\n",                            XW_ERR_SIZE },
    { "xw 1\nsize 0 5\n",                             XW_ERR_SIZE },
    { "xw 1\nsize 5\n",                               XW_ERR_SIZE },
    { "xw 1\nsize 2 1\nsize 2 1\n",                   XW_ERR_SIZE },
    { "xw 1\nrow AB\n",                               XW_ERR_SIZE },
    { "xw 1\nA 1 x\n",                                XW_ERR_SIZE },
    /* the rows */
    { "xw 1\nsize 2 1\nrow ABC\n",                    XW_ERR_GRID },
    { "xw 1\nsize 2 1\nrow \n",                       XW_ERR_GRID },
    { "xw 1\nsize 2 1\nrow A4\n",                     XW_ERR_GRID },
    { "xw 1\nsize 2 1\nrow AB\nrow AB\n",             XW_ERR_GRID },
    { "xw 1\nsize 2 2\nrow AB\nA 1 x\n",              XW_ERR_GRID },
    { "xw 1\nsize 2 2\nrow AB\nend\n",                XW_ERR_GRID },
    { "xw 1\nsize 2 1\nrow AB\nA 1 x\nrow AB\n",      XW_ERR_GRID },
    /* grids that are not crosswords */
    { "xw 1\nsize 3 1\nrow A.B\nend\n",               XW_ERR_ORPHAN },
    { "xw 1\nsize 2 1\nrow ..\nend\n",                XW_ERR_EMPTY },
    /* clues that do not belong to the grid */
    { "xw 1\nsize 2 1\nrow AB\nend\n",                XW_ERR_MISSING },
    { "xw 1\nsize 2 1\nrow AB\nA 2 x\nend\n",         XW_ERR_CLUE },
    { "xw 1\nsize 2 1\nrow AB\nA 1 x\nA 2 y\nend\n",  XW_ERR_CLUE },
    { "xw 1\nsize 2 1\nrow AB\nD 1 x\nend\n",         XW_ERR_CLUE },
    { "xw 1\nsize 2 1\nrow AB\nA x\nend\n",           XW_ERR_CLUE },
    { "xw 1\nsize 2 1\nrow AB\nA 1x y\nend\n",        XW_ERR_CLUE },
    { "xw 1\n" FIX_ROWS "A 1 a\nA 5 b\nA 7 c\nA 8 d\n" FIX_DOWN "end\n",      XW_ERR_MISSING },
    { "xw 1\n" FIX_ROWS "A 1 a\nA 6 b\n",                                      XW_ERR_CLUE },
    { "xw 1\n" FIX_ROWS FIX_ACROSS "A 10 extra\n",                             XW_ERR_CLUE },
    { "xw 1\n" FIX_ROWS FIX_ACROSS "D 1 a\nD 2 b\nD 3 c\nD 5 d\nD 6 e\nend\n", XW_ERR_CLUE },
    { "xw 1\n" FIX_ROWS FIX_ACROSS "D 1 a\nD 2 b\nD 3 c\nD 4 d\nend\n",        XW_ERR_MISSING }
  };
  int i;
  for (i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++) {
    int got = xw_parse(&P1, BAD[i].text);
    if (got != BAD[i].err) printf("  bad case %d: got %d, wanted %d\n", i, got, BAD[i].err);
    CHECK_EQ(got, BAD[i].err);
    CHECK(!P1.ready);
  }
  /* The same puzzle with its last line cut off. */
  {
    int n = (int)(strstr(FIX, "end\n") - FIX);
    memcpy(BIG, FIX, (size_t)n);
    BIG[n] = 0;
    CHECK_EQ(xw_parse(&P1, BIG), XW_ERR_TRUNC);
  }
  CHECK(!strcmp(xw_err_text(XW_ERR_TRUNC), "puzzle cut short"));
  CHECK(!strcmp(xw_err_text(XW_OK), "ok"));
  CHECK(!strcmp(xw_err_text(-99), "error"));
}

void test_crossword_loads_a_line_at_a_time_and_an_error_sticks(void) {
  const char *s = FIX;
  int r = XW_OK, lines = 0;
  xw_init(&P1);
  while (*s) {
    char line[128];
    int n = 0;
    while (*s && *s != '\n') line[n++] = *s++;
    line[n] = 0;
    if (*s) s++;
    r = xw_load_line(&P1, line);
    lines++;
    if (r != XW_OK) break;
  }
  CHECK_EQ(r, XW_DONE);                           /* the `end` line, the 22nd */
  CHECK_EQ(lines, 22);
  CHECK(P1.ready);
  CHECK_EQ(xw_load_end(&P1), XW_OK);
  CHECK_EQ(xw_load_line(&P1, "A 99 junk"), XW_DONE);
  CHECK_EQ(P1.nclues, 10);

  xw_init(&P2);
  CHECK_EQ(xw_load_line(&P2, "xw 1"), XW_OK);
  CHECK_EQ(xw_load_end(&P2), XW_ERR_TRUNC);       /* nothing says it is whole */
  CHECK(!P2.ready);

  xw_init(&P2);
  CHECK_EQ(xw_load_line(&P2, "nope"), XW_ERR_HEADER);
  CHECK_EQ(xw_load_line(&P2, "xw 1"), XW_ERR_HEADER);   /* the first error is kept */
  CHECK_EQ(xw_load_end(&P2), XW_ERR_HEADER);
}

void test_crossword_takes_the_biggest_grid_and_refuses_what_does_not_fit(void) {
  int r, c, at;
  CHECK(build_open_grid(BIG, 8) > 0);
  CHECK_EQ(xw_parse(&P1, BIG), XW_OK);
  CHECK_EQ(P1.w, 21); CHECK_EQ(P1.h, 21);
  CHECK_EQ(P1.nclues, 42); CHECK_EQ(P1.nacross, 21);
  CHECK_EQ(xw_number_at(&P1, 0), 1);
  CHECK_EQ(xw_number_at(&P1, 20), 21);
  CHECK_EQ(xw_number_at(&P1, 5 * 21), 26);
  CHECK_EQ(xw_number_at(&P1, 5 * 21 + 1), 0);
  CHECK_EQ(P1.clue[1].num, 22);
  CHECK_EQ(P1.clue[20].num, 41);
  CHECK_EQ(P1.clue[21].num, 1);
  CHECK(!strcmp(xw_clue_text(&P1, 41), "xxxxxxxx"));
  CHECK_EQ(solve(&P1, -1) & XW_R_WON, XW_R_WON);   /* all 441 squares, and it knows */

  /* Too much clue text: 42 clues of 200 characters is 8442 bytes. */
  CHECK(build_open_grid(BIG, 200) > 0);
  CHECK_EQ(xw_parse(&P1, BIG), XW_ERR_BIG);
  CHECK(!P1.ready);

  /* More entries than clue slots: two-square words in every row and column,
   * each row shifted along from the one above -- 140 across and 140 down. */
  at = app(BIG, 0, "xw 1\nsize 21 21\n");
  for (r = 0; r < 21; r++) {
    at = app(BIG, at, "row ");
    for (c = 0; c < 21; c++) BIG[at++] = ((r + c) % 3 == 2) ? '.' : 'A';
    BIG[at++] = '\n';
    BIG[at] = 0;
  }
  app(BIG, at, "end\n");
  CHECK_EQ(xw_parse(&P1, BIG), XW_ERR_BIG);
}

/* ---- moving ----------------------------------------------------------------- */

void test_crossword_space_turns_the_direction_where_there_is_a_word(void) {
  load_fix(&P1);
  CHECK_EQ(xw_toggle(&P1), XW_R_DIR);
  CHECK_EQ(P1.dir, XW_DOWN);
  CHECK_EQ(P1.cur, 0);
  CHECK_EQ(xw_cur_clue(&P1), 5);                  /* 1-Down */
  CHECK_EQ(xw_toggle(&P1), XW_R_DIR);
  CHECK_EQ(P1.dir, XW_ACROSS);

  load_frame(&P1);
  xw_goto(&P1, 1);                                /* across only */
  CHECK_EQ(xw_toggle(&P1), 0);
  CHECK_EQ(P1.dir, XW_ACROSS);
  xw_goto(&P1, 5);                                /* down only: the cursor turns itself */
  CHECK_EQ(P1.dir, XW_DOWN);
  CHECK_EQ(xw_toggle(&P1), 0);
  CHECK_EQ(P1.dir, XW_DOWN);
}

void test_crossword_arrows_move_without_turning_and_stop_at_black_squares(void) {
  load_fix(&P1);
  CHECK_EQ(xw_arrow(&P1, 0, 1), XW_R_MOVE);
  CHECK_EQ(P1.cur, 1);
  xw_arrow(&P1, 0, 1);
  xw_arrow(&P1, 0, 1);
  CHECK_EQ(P1.cur, 3);
  CHECK_EQ(xw_arrow(&P1, 0, 1), 0);               /* (0,4) is black, then the edge */
  CHECK_EQ(P1.cur, 3);
  CHECK_EQ(xw_arrow(&P1, 1, 0), XW_R_MOVE);       /* across the direction: it moves, no turn */
  CHECK_EQ(P1.cur, 8);
  CHECK_EQ(P1.dir, XW_ACROSS);                    /* (1,3) has an across word: still across */
  CHECK_EQ(xw_arrow(&P1, -1, 0), XW_R_MOVE);
  CHECK_EQ(P1.cur, 3);
  CHECK_EQ(xw_arrow(&P1, -1, 0), 0);              /* the top edge */
  CHECK_EQ(P1.cur, 3);

  /* Black corners block the way. */
  xw_goto(&P1, 9);                                /* (1,4), down */
  CHECK_EQ(xw_arrow(&P1, -1, 0), 0);              /* (0,4) is black */
  CHECK_EQ(P1.cur, 9);
  xw_goto(&P1, 21);                               /* (4,1) */
  CHECK_EQ(xw_arrow(&P1, 0, -1), 0);              /* (4,0) is black: nothing, not a turn */
  CHECK_EQ(P1.cur, 21);

  /* Not an arrow. */
  CHECK_EQ(xw_arrow(&P1, 0, 0), 0);
  CHECK_EQ(xw_arrow(&P1, 1, 1), 0);
  CHECK_EQ(xw_step(&P1, 0, 0), 0);
}

void test_crossword_arrows_jump_over_black_squares(void) {
  load_frame(&P1);
  CHECK_EQ(xw_arrow(&P1, 0, 1), XW_R_MOVE);       /* (0,0) -> (0,1) */
  CHECK_EQ(P1.cur, 1);
  /* Down from (0,1): (1,1) to (3,1) are black, (4,1) is the next square. */
  CHECK_EQ(xw_arrow(&P1, 1, 0), XW_R_MOVE);
  CHECK_EQ(P1.cur, 21);
  CHECK_EQ(P1.dir, XW_ACROSS);                    /* it has no down word, so it stays across */
  CHECK_EQ(xw_arrow(&P1, -1, 0), XW_R_MOVE);
  CHECK_EQ(P1.cur, 1);
  CHECK_EQ(xw_arrow(&P1, 0, -1), XW_R_MOVE);
  CHECK_EQ(P1.cur, 0);
  CHECK_EQ(xw_arrow(&P1, 0, -1), 0);
  /* At (0,0), which has both, down goes at once; (1,0) has only a down
   * word, so there the direction follows. */
  CHECK_EQ(xw_arrow(&P1, 1, 0), XW_R_MOVE | XW_R_DIR);
  CHECK_EQ(P1.cur, 5);
  CHECK_EQ(P1.dir, XW_DOWN);
  /* (1,0) has no across word: right does not turn, it jumps the black middle. */
  CHECK_EQ(xw_arrow(&P1, 0, 1), XW_R_MOVE);
  CHECK_EQ(P1.cur, 9);
  CHECK_EQ(P1.dir, XW_DOWN);
  CHECK_EQ(xw_arrow(&P1, 0, -1), XW_R_MOVE);
  CHECK_EQ(P1.cur, 5);
}

void test_crossword_tab_walks_the_clues_across_then_down(void) {
  static const int cur[10] = { 5, 10, 15, 21, 0, 1, 2, 3, 9, 0 };
  static const int dir[10] = { XW_ACROSS, XW_ACROSS, XW_ACROSS, XW_ACROSS, XW_DOWN,
                               XW_DOWN, XW_DOWN, XW_DOWN, XW_DOWN, XW_ACROSS };
  int i, bad = 0;
  load_fix(&P1);
  CHECK_EQ(xw_next_word(&P1, 1), XW_R_MOVE);      /* 1-Across to 5-Across */
  CHECK_EQ(P1.cur, 5);
  load_fix(&P1);
  for (i = 0; i < 10; i++) {
    xw_next_word(&P1, 1);
    if (P1.cur != cur[i] || P1.dir != dir[i]) bad++;
  }
  CHECK_EQ(bad, 0);                               /* ten steps round, back at 1-Across */

  load_fix(&P1);
  CHECK_EQ(xw_next_word(&P1, 0), XW_R_MOVE | XW_R_DIR);   /* backwards from the first: the last */
  CHECK_EQ(P1.cur, 9); CHECK_EQ(P1.dir, XW_DOWN);
  xw_next_word(&P1, 0);
  CHECK_EQ(P1.cur, 3);                            /* 4-Down */
  CHECK_EQ(xw_cur_clue(&P1), 8);

  /* From 9-Across to 1-Down both change. */
  xw_goto_clue(&P1, 4);
  CHECK_EQ(xw_next_word(&P1, 1), XW_R_MOVE | XW_R_DIR);
  CHECK_EQ(P1.cur, 0); CHECK_EQ(P1.dir, XW_DOWN);

  /* In a grid with no across words the first clue is a down one. */
  CHECK_EQ(xw_parse(&P2, "xw 1\nsize 1 3\nrow A\nrow B\nrow C\nD 1 Letters\nend\n"), XW_OK);
  CHECK_EQ(P2.nacross, 0); CHECK_EQ(P2.nclues, 1);
  CHECK_EQ(P2.dir, XW_DOWN); CHECK_EQ(P2.cur, 0);
  CHECK_EQ(xw_next_word(&P2, 1), 0);              /* only one place to go */
}

void test_crossword_a_word_is_entered_at_its_first_empty_square(void) {
  load_fix(&P1);
  xw_goto(&P1, 5);
  xw_type(&P1, 'e');
  xw_type(&P1, 'm');                              /* 5-Across: E M so far, the cursor on 7 */
  CHECK_EQ(P1.cur, 7);
  xw_goto(&P1, 0);
  xw_next_word(&P1, 1);
  CHECK_EQ(P1.cur, 7);                            /* not 5: the first that is empty */
  xw_type(&P1, 'b'); xw_type(&P1, 'e'); xw_type(&P1, 'r');
  xw_goto(&P1, 0);
  xw_next_word(&P1, 1);
  CHECK_EQ(P1.cur, 5);                            /* all full: the first square */

  load_fix(&P1);
  CHECK_EQ(xw_goto_clue(&P1, 7), XW_R_MOVE | XW_R_DIR);   /* 3-Down */
  CHECK_EQ(P1.cur, 2); CHECK_EQ(P1.dir, XW_DOWN);
  CHECK_EQ(xw_goto_clue(&P1, 7), 0);
  CHECK_EQ(xw_goto_clue(&P1, 10), 0);
  CHECK_EQ(xw_goto_clue(&P1, -1), 0);
}

void test_crossword_clicking_a_square_moves_or_turns(void) {
  load_fix(&P1);
  CHECK_EQ(xw_click(&P1, 0), XW_R_DIR);           /* the one the cursor is on: turn */
  CHECK_EQ(P1.dir, XW_DOWN);
  CHECK_EQ(xw_click(&P1, 12), XW_R_MOVE);         /* another: go there, same direction */
  CHECK_EQ(P1.cur, 12); CHECK_EQ(P1.dir, XW_DOWN);
  CHECK_EQ(xw_click(&P1, 4), 0);                  /* black */
  CHECK_EQ(xw_click(&P1, 99), 0);
  CHECK_EQ(xw_click(&P1, -1), 0);
  CHECK_EQ(P1.cur, 12);
  CHECK_EQ(xw_click(&P1, 12), XW_R_DIR);
  CHECK_EQ(P1.dir, XW_ACROSS);

  /* A square with no across word turns the cursor to down by itself. */
  load_frame(&P1);
  CHECK_EQ(xw_click(&P1, 5), XW_R_MOVE | XW_R_DIR);
  CHECK_EQ(P1.cur, 5); CHECK_EQ(P1.dir, XW_DOWN);
}

void test_crossword_the_current_word_is_known_to_the_screen(void) {
  load_fix(&P1);
  CHECK(xw_in_cur_word(&P1, 0));
  CHECK(xw_in_cur_word(&P1, 3));
  CHECK(!xw_in_cur_word(&P1, 4));                 /* black */
  CHECK(!xw_in_cur_word(&P1, 5));
  xw_toggle(&P1);
  CHECK(xw_in_cur_word(&P1, 15));                 /* 1-Down is HEAR: 0, 5, 10, 15 */
  CHECK(!xw_in_cur_word(&P1, 20));
  CHECK(!xw_in_cur_word(&P1, 1));
}

/* ---- typing ----------------------------------------------------------------- */

void test_crossword_typing_fills_the_word_and_stops_at_its_end(void) {
  load_fix(&P1);
  CHECK_EQ(xw_type(&P1, 'h'), XW_R_CELL | XW_R_MOVE);
  CHECK_EQ(P1.cell[0].ent, 'H');                  /* either case, kept upper */
  CHECK_EQ(P1.cur, 1);
  xw_type(&P1, 'E');
  xw_type(&P1, 'a');
  CHECK_EQ(P1.cur, 3);
  CHECK_EQ(xw_type(&P1, 'x'), XW_R_CELL);         /* the last square: the cursor stays */
  CHECK_EQ(P1.cur, 3); CHECK_EQ(P1.cell[3].ent, 'X');
  CHECK_EQ(xw_type(&P1, 'x'), 0);                 /* the same letter again changes nothing */
  CHECK_EQ(xw_type(&P1, '1'), 0);
  CHECK_EQ(xw_type(&P1, ' '), 0);
  CHECK_EQ(xw_type(&P1, '.'), 0);
  CHECK_EQ(xw_type(&P1, 0), 0);
  CHECK_EQ(P1.cell[3].ent, 'X');
  CHECK(!P1.solved);

  /* Typing over a letter that is already right only moves on. */
  xw_goto(&P1, 0);
  CHECK_EQ(xw_type(&P1, 'h'), XW_R_MOVE);
  CHECK_EQ(P1.cur, 1);

  /* Down a word the cursor goes down. */
  xw_goto(&P1, 3);
  xw_toggle(&P1);
  CHECK_EQ(xw_type(&P1, 'r'), XW_R_CELL | XW_R_MOVE);
  CHECK_EQ(P1.cell[3].ent, 'R');
  CHECK_EQ(P1.cur, 8);

  /* Nothing to type into before a puzzle is loaded. */
  xw_init(&P2);
  CHECK_EQ(xw_type(&P2, 'a'), 0);
}

void test_crossword_backspace_and_delete_empty_squares(void) {
  load_fix(&P1);
  xw_type(&P1, 'h'); xw_type(&P1, 'e');           /* H E, the cursor on 2 */
  CHECK_EQ(P1.cur, 2);
  /* Square 2 is empty: back to 1, and that one is emptied. */
  CHECK_EQ(xw_backspace(&P1), XW_R_MOVE | XW_R_CELL);
  CHECK_EQ(P1.cur, 1); CHECK_EQ(P1.cell[1].ent, 0); CHECK_EQ(P1.cell[0].ent, 'H');
  /* Square 1 is empty too: back to 0 and empty H. */
  CHECK_EQ(xw_backspace(&P1), XW_R_MOVE | XW_R_CELL);
  CHECK_EQ(P1.cur, 0); CHECK_EQ(P1.cell[0].ent, 0);
  /* The start of the word with nothing in it: nowhere to go. */
  CHECK_EQ(xw_backspace(&P1), 0);
  CHECK_EQ(P1.cur, 0);

  /* On a letter it empties in place. */
  xw_type(&P1, 'h');
  xw_goto(&P1, 0);
  CHECK_EQ(xw_backspace(&P1), XW_R_CELL);
  CHECK_EQ(P1.cur, 0); CHECK_EQ(P1.cell[0].ent, 0);

  /* Delete empties in place and never steps back. */
  xw_type(&P1, 'h'); xw_type(&P1, 'e');
  xw_goto(&P1, 1);
  CHECK_EQ(xw_clear(&P1), XW_R_CELL);
  CHECK_EQ(P1.cur, 1); CHECK_EQ(P1.cell[1].ent, 0);
  CHECK_EQ(xw_clear(&P1), 0);
  CHECK_EQ(P1.cur, 1); CHECK_EQ(P1.cell[0].ent, 'H');
}

void test_crossword_check_marks_wrong_letters_until_they_are_changed(void) {
  load_fix(&P1);
  put(&P1, 0, 'q');                               /* wrong: it is an H */
  put(&P1, 6, 'm');                               /* right */
  put(&P1, 12, 'z');                              /* wrong: U */
  put(&P1, 24, 'd');                              /* right, and the cursor stays on it */
  CHECK_EQ(P1.cur, 24);
  CHECK_EQ(xw_check(&P1, XW_SCOPE_CELL), 0);
  xw_goto(&P1, 0);
  CHECK_EQ(xw_check(&P1, XW_SCOPE_CELL), 1);
  CHECK(P1.cell[0].fl & XW_F_WRONG);
  CHECK_EQ(xw_wrongs(&P1), 1);
  CHECK_EQ(xw_check(&P1, XW_SCOPE_WORD), 1);      /* HEAR: only the first is in it */
  CHECK_EQ(xw_wrongs(&P1), 1);
  CHECK_EQ(xw_check(&P1, XW_SCOPE_ALL), 2);       /* and the Z in ABUSE */
  CHECK_EQ(xw_wrongs(&P1), 2);
  CHECK(P1.cell[12].fl & XW_F_WRONG);
  CHECK(!(P1.cell[6].fl & XW_F_WRONG));
  CHECK(P1.cell[12].fl & XW_F_CIRCLE);            /* the circle is not disturbed */
  /* Empty squares are not wrong, just not done. */
  CHECK(!(P1.cell[1].fl & XW_F_WRONG));

  put(&P1, 12, 'u');                              /* mending takes the mark off */
  CHECK(!(P1.cell[12].fl & XW_F_WRONG));
  CHECK_EQ(xw_wrongs(&P1), 1);
  xw_goto(&P1, 0);
  CHECK_EQ(xw_clear(&P1), XW_R_CELL);             /* emptying takes it off as well */
  CHECK(!(P1.cell[0].fl & XW_F_WRONG));
  CHECK_EQ(xw_wrongs(&P1), 0);

  put(&P1, 0, 'x');
  xw_check(&P1, XW_SCOPE_ALL);
  CHECK_EQ(xw_unmark(&P1), 1);
  CHECK_EQ(xw_wrongs(&P1), 0);
  CHECK_EQ(P1.cell[0].ent, 'X');                  /* the letter stays; only the mark goes */
  CHECK_EQ(xw_unmark(&P1), 0);
}

void test_crossword_reveal_fills_the_answer_and_remembers_it_was_given(void) {
  int k;
  load_fix(&P1);
  put(&P1, 5, 'x');
  xw_goto(&P1, 5);
  CHECK_EQ(xw_reveal(&P1, XW_SCOPE_CELL), XW_R_CELL);
  CHECK_EQ(P1.cell[5].ent, 'E');
  CHECK(P1.cell[5].fl & XW_F_REVEALED);
  CHECK_EQ(xw_reveals(&P1), 1);
  CHECK_EQ(xw_reveal(&P1, XW_SCOPE_CELL), 0);     /* already there */
  CHECK_EQ(xw_reveals(&P1), 1);

  /* A revealed square is locked: typed over, it keeps its letter (the cursor
   * still moves on), and it is neither emptied nor stepped back through. */
  xw_goto(&P1, 5);
  CHECK_EQ(xw_type(&P1, 'q'), XW_R_MOVE);
  CHECK_EQ(P1.cell[5].ent, 'E'); CHECK_EQ(P1.cur, 6);
  xw_goto(&P1, 5);
  CHECK_EQ(xw_clear(&P1), 0);
  CHECK_EQ(xw_backspace(&P1), 0);
  CHECK_EQ(P1.cell[5].ent, 'E');
  xw_goto(&P1, 6);
  CHECK_EQ(xw_backspace(&P1), XW_R_MOVE);         /* steps back onto it, and it stays */
  CHECK_EQ(P1.cur, 5); CHECK_EQ(P1.cell[5].ent, 'E');

  /* A word: 3-Down is ABUSE down column 2. */
  xw_goto_clue(&P1, 7);
  CHECK_EQ(xw_reveal(&P1, XW_SCOPE_WORD), XW_R_CELL);
  for (k = 0; k < 5; k++) CHECK_EQ(P1.cell[2 + 5 * k].ent, "ABUSE"[k]);
  CHECK_EQ(xw_reveals(&P1), 6);
  CHECK(!P1.solved);

  /* Everything else. */
  CHECK_EQ(xw_reveal(&P1, XW_SCOPE_ALL), XW_R_CELL | XW_R_WON);
  CHECK(P1.solved);
  CHECK_EQ(xw_reveals(&P1), 23);                  /* every white square was given */
  CHECK_EQ(xw_reveal(&P1, XW_SCOPE_ALL), 0);

  /* What the solver had typed right is theirs, and not counted. */
  load_fix(&P1);
  xw_type(&P1, 'h'); xw_type(&P1, 'e'); xw_type(&P1, 'a'); xw_type(&P1, 'r');
  CHECK_EQ(xw_reveal(&P1, XW_SCOPE_ALL), XW_R_CELL | XW_R_WON);
  CHECK_EQ(xw_reveals(&P1), 19);
  CHECK(!(P1.cell[0].fl & XW_F_REVEALED));
  CHECK(P1.cell[5].fl & XW_F_REVEALED);
  CHECK(P1.cell[12].fl & XW_F_CIRCLE);            /* the circle survives a reveal */
}

void test_crossword_solving_every_square_wins_exactly_once(void) {
  int f, t, r;
  load_fix(&P1);
  solve(&P1, 24);
  CHECK(!P1.solved);
  CHECK(!xw_solved(&P1));
  xw_count(&P1, &f, &t);
  CHECK_EQ(f, 22); CHECK_EQ(t, 23);
  r = put(&P1, 24, 'd');
  CHECK(r & XW_R_WON);
  CHECK(P1.solved);
  CHECK(xw_solved(&P1));
  CHECK_EQ(xw_reveals(&P1), 0);                   /* all the solver's own */
  /* A finished grid is locked. */
  CHECK_EQ(xw_type(&P1, 'z'), 0);
  CHECK_EQ(P1.cell[24].ent, 'D');
  CHECK_EQ(xw_backspace(&P1), 0);
  CHECK_EQ(xw_clear(&P1), 0);
  CHECK_EQ(xw_reveal(&P1, XW_SCOPE_ALL), 0);
  CHECK(xw_goto(&P1, 0) & XW_R_MOVE);             /* but the cursor is free */
  CHECK_EQ(xw_toggle(&P1), XW_R_DIR);

  /* The last square can be any one of them, and the order does not matter. */
  load_fix(&P1);
  solve(&P1, 0);
  CHECK(!P1.solved);
  CHECK(put(&P1, 0, 'H') & XW_R_WON);

  /* A full grid with a wrong letter is not a win, and mending it is. */
  load_fix(&P1);
  solve(&P1, 24);
  put(&P1, 24, 'x');
  xw_count(&P1, &f, &t);
  CHECK_EQ(f, t);
  CHECK(!P1.solved);
  CHECK_EQ(xw_check(&P1, XW_SCOPE_ALL), 1);
  r = put(&P1, 24, 'd');
  CHECK(r & XW_R_WON);

  /* The frame, whose squares are in one word each, wins the same way. */
  load_frame(&P1);
  CHECK(solve(&P1, -1) & XW_R_WON);
  CHECK(P1.solved);
}

void test_crossword_start_over_empties_the_grid(void) {
  load_fix(&P1);
  xw_clock_start(&P1, 0);
  xw_clock_stop(&P1, 5000);
  solve(&P1, -1);
  CHECK(P1.solved);
  CHECK_EQ(xw_elapsed_ms(&P1, 99999), 5000);
  xw_reset(&P1);
  CHECK(!P1.solved);
  CHECK_EQ(P1.cur, 0); CHECK_EQ(P1.dir, XW_ACROSS);
  CHECK_EQ(xw_elapsed_ms(&P1, 99999), 0);
  CHECK_EQ(P1.cell[3].ent, 0);
  CHECK(P1.cell[12].fl & XW_F_CIRCLE);            /* the circle is part of the puzzle */
  CHECK_EQ(xw_reveals(&P1), 0);
  put(&P1, 0, 'h');
  CHECK_EQ(P1.cell[0].ent, 'H');                  /* playable again */
}

/* ---- the clock ---------------------------------------------------------------- */

void test_crossword_the_clock_runs_only_while_started(void) {
  load_fix(&P1);
  CHECK_EQ(xw_elapsed_ms(&P1, 5000), 0);
  xw_clock_start(&P1, 5000);
  CHECK_EQ(xw_elapsed_ms(&P1, 7500), 2500);
  xw_clock_start(&P1, 6000);                      /* already running: not restarted */
  CHECK_EQ(xw_elapsed_ms(&P1, 7500), 2500);
  xw_clock_stop(&P1, 9000);
  CHECK_EQ(xw_elapsed_ms(&P1, 99999), 4000);      /* stopped: it stays */
  xw_clock_stop(&P1, 12345);                      /* stopping a stopped clock does nothing */
  CHECK_EQ(xw_elapsed_ms(&P1, 99999), 4000);
  CHECK_EQ(xw_elapsed_s(&P1, 0), 4);
  xw_clock_start(&P1, 100000);
  CHECK_EQ(xw_elapsed_ms(&P1, 100500), 4500);
  xw_clock_stop(&P1, 101000);
  CHECK_EQ(xw_elapsed_ms(&P1, 0), 5000);

  /* The millisecond counter wrapping round is only a short run. */
  xw_clock_start(&P1, 0xFFFFFF00u);
  CHECK_EQ(xw_elapsed_ms(&P1, 0x00000100u), 5512);
  xw_clock_stop(&P1, 0x00000100u);
  CHECK_EQ(xw_elapsed_ms(&P1, 0), 5512);

  /* A finished puzzle does not start counting again. */
  solve(&P1, -1);
  xw_clock_start(&P1, 1);
  CHECK(!P1.running);
}

/* ---- progress ------------------------------------------------------------------ */

void test_crossword_progress_round_trips_through_text(void) {
  int n, i, bad = 0;
  XwPeek pk;
  load_fix(&P1);
  xw_type(&P1, 'h'); xw_type(&P1, 'e'); xw_type(&P1, 'a'); xw_type(&P1, 'r');
  xw_goto(&P1, 12);
  xw_reveal(&P1, XW_SCOPE_CELL);                  /* the circled U, given */
  xw_goto(&P1, 5);
  xw_type(&P1, 'x');                              /* wrong, and the cursor on 6 */
  CHECK_EQ(xw_check(&P1, XW_SCOPE_WORD), 1);
  CHECK(P1.cell[5].fl & XW_F_WRONG);
  CHECK_EQ(P1.cur, 6);
  xw_toggle(&P1);                                 /* down */
  xw_clock_start(&P1, 1000);
  xw_clock_stop(&P1, 126500);                     /* 125.5 s */

  n = xw_save(&P1, SAVE, sizeof SAVE);
  CHECK(n > 0);
  CHECK_EQ((int)strlen(SAVE), n);
  CHECK(!strcmp(SAVE,
      "xwp 1\nstate P\ntime 125\nfill 6 23\nreveals 1\nsize 5 5\ncur 6 D\n"
      "g HEAR.\ng X----\ng --u--\ng -----\ng .----\nend\n"));

  load_fix(&P2);
  CHECK_EQ(xw_restore(&P2, SAVE), XW_OK);
  for (i = 0; i < 25; i++) {
    if (P2.cell[i].ent != P1.cell[i].ent) bad++;
    if (P2.cell[i].fl != (P1.cell[i].fl & (XW_F_CIRCLE | XW_F_REVEALED))) bad++;   /* marks are not kept */
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(P2.cur, 6); CHECK_EQ(P2.dir, XW_DOWN);
  CHECK_EQ(xw_elapsed_ms(&P2, 777), 125000);      /* whole seconds, and not running */
  CHECK(!P2.running);
  CHECK(!P2.solved);
  CHECK_EQ(xw_reveals(&P2), 1);
  CHECK_EQ(xw_wrongs(&P2), 0);

  /* The same file is restored over a game that has moved on. */
  put(&P2, 2, 'q');
  CHECK_EQ(xw_restore(&P2, SAVE), XW_OK);
  CHECK_EQ(P2.cell[2].ent, 'A');

  /* And the top of it is enough for a list. */
  CHECK_EQ(xw_peek(SAVE, &pk), XW_OK);
  CHECK(pk.ok); CHECK_EQ(pk.state, 'P'); CHECK_EQ((int)pk.secs, 125);
  CHECK_EQ(pk.filled, 6); CHECK_EQ(pk.total, 23); CHECK_EQ(pk.reveals, 1);
  {
    char cut[64];
    int k = (int)(strstr(SAVE, "fill 6") - SAVE) + 4;      /* "...time 125\nfill" */
    memcpy(cut, SAVE, (size_t)k);
    cut[k] = 0;
    CHECK_EQ(xw_peek(cut, &pk), XW_OK);
    CHECK_EQ(pk.state, 'P'); CHECK_EQ((int)pk.secs, 125);
    CHECK_EQ(pk.filled, 0);                       /* a line not finished is not read */
  }
  CHECK_EQ(xw_peek("", &pk), XW_ERR_HEADER);
  CHECK(!pk.ok);
  CHECK_EQ(xw_peek("hello\n", &pk), XW_ERR_HEADER);
  CHECK_EQ(xw_peek("xwp 1\n", &pk), XW_ERR_HEADER);        /* no state yet */
  CHECK_EQ(xw_peek("xwp 1\nstate S\n", &pk), XW_OK);
  CHECK_EQ(pk.state, 'S');

  /* A buffer too small says so. */
  CHECK_EQ(xw_save(&P1, SAVE, 20), -1);
  xw_init(&P2);
  CHECK_EQ(xw_save(&P2, SAVE, sizeof SAVE), -1);           /* nothing loaded */
}

void test_crossword_a_finished_puzzle_is_saved_as_finished(void) {
  XwPeek pk;
  load_fix(&P1);
  solve(&P1, -1);
  CHECK(xw_save(&P1, SAVE, sizeof SAVE) > 0);
  CHECK(strstr(SAVE, "state S\n") != 0);
  CHECK(strstr(SAVE, "fill 23 23\n") != 0);
  CHECK(strstr(SAVE, "reveals 0\n") != 0);
  CHECK(strstr(SAVE, "g HEAR.\ng EMBER\ng ABUSE\ng RESIN\ng .REND\n") != 0);
  load_fix(&P2);
  CHECK_EQ(xw_restore(&P2, SAVE), XW_OK);
  CHECK(P2.solved);                               /* worked out from the grid, not trusted */
  CHECK_EQ(xw_type(&P2, 'z'), 0);                 /* and locked */
  xw_peek(SAVE, &pk);
  CHECK_EQ(pk.state, 'S');

  /* Finished with help is told apart. */
  load_fix(&P1);
  xw_reveal(&P1, XW_SCOPE_ALL);
  CHECK(xw_save(&P1, SAVE, sizeof SAVE) > 0);
  CHECK(strstr(SAVE, "state H\n") != 0);
  CHECK(strstr(SAVE, "reveals 23\n") != 0);
  CHECK(strstr(SAVE, "g hear.\ng ember\ng abuse\ng resin\ng .rend\n") != 0);
  xw_peek(SAVE, &pk);
  CHECK_EQ(pk.state, 'H'); CHECK_EQ(pk.reveals, 23);
  load_fix(&P2);
  CHECK_EQ(xw_restore(&P2, SAVE), XW_OK);
  CHECK(P2.solved);
  CHECK_EQ(xw_reveals(&P2), 23);
  CHECK(P2.cell[12].fl & XW_F_CIRCLE);
  CHECK(P2.cell[12].fl & XW_F_REVEALED);
}

void test_crossword_progress_that_does_not_fit_changes_nothing(void) {
  int i;
  static const char *const BAD[] = {
    "",
    "xwp 2\nsize 5 5\n",
    "hello\n",
    /* another size */
    "xwp 1\nsize 4 5\ng ----\nend\n",
    /* no end: cut short */
    "xwp 1\nsize 5 5\ng ----.\ng -----\ng -----\ng -----\ng .----\n",
    /* rows missing */
    "xwp 1\nsize 5 5\ng ----.\nend\n",
    /* a row before the size */
    "xwp 1\ng ----.\ng -----\ng -----\ng -----\ng .----\nend\n",
    /* more rows than the puzzle has */
    "xwp 1\nsize 5 5\ng ----.\ng -----\ng -----\ng -----\ng .----\ng -----\nend\n",
    /* a row of the wrong length */
    "xwp 1\nsize 5 5\ng ----\ng -----\ng -----\ng -----\ng .----\nend\n",
    /* black where the puzzle has white, and the other way round */
    "xwp 1\nsize 5 5\ng ----.\ng -----\ng -.---\ng -----\ng .----\nend\n",
    "xwp 1\nsize 5 5\ng -----\ng -----\ng -----\ng -----\ng .----\nend\n",
    /* a character that is nothing */
    "xwp 1\nsize 5 5\ng 1---.\ng -----\ng -----\ng -----\ng .----\nend\n",
    /* a revealed letter that is not the answer */
    "xwp 1\nsize 5 5\ng z---.\ng -----\ng -----\ng -----\ng .----\nend\n"
  };
  load_fix(&P1);
  put(&P1, 0, 'q');                               /* 'Q' at 0, the cursor on 1 */
  xw_goto(&P1, 12);
  for (i = 0; i < (int)(sizeof BAD / sizeof BAD[0]); i++) {
    int got = xw_restore(&P1, BAD[i]);
    if (got >= 0) printf("  progress case %d was taken\n", i);
    CHECK(got < 0);
    CHECK_EQ(P1.cell[0].ent, 'Q');                /* untouched */
    CHECK_EQ(P1.cur, 12);
  }
  CHECK_EQ(xw_restore(&P1, ""), XW_ERR_HEADER);
  CHECK_EQ(xw_restore(&P1, BAD[3]), XW_ERR_SIZE);
  CHECK_EQ(xw_restore(&P1, BAD[4]), XW_ERR_TRUNC);
  CHECK_EQ(xw_restore(&P1, BAD[5]), XW_ERR_GRID);

  /* Another puzzle's progress: same size, other black squares. */
  load_frame(&P2);
  put(&P2, 0, 'a');
  CHECK(xw_save(&P2, SAVE, sizeof SAVE) > 0);
  CHECK_EQ(xw_restore(&P1, SAVE), XW_ERR_GRID);
  CHECK_EQ(P1.cell[0].ent, 'Q');

  /* Nothing loaded. */
  xw_init(&P2);
  CHECK_EQ(xw_restore(&P2, SAVE), XW_ERR_STATE);
}

void test_crossword_progress_written_by_hand_is_taken_as_far_as_it_goes(void) {
  /* No clock, no cursor, no counts: just the grid. */
  load_fix(&P1);
  put(&P1, 0, 'q');
  CHECK_EQ(xw_restore(&P1, "xwp 1\nsize 5 5\ng HEAR.\ng -----\ng -----\ng -----\ng .----\nend\n"), XW_OK);
  CHECK_EQ(P1.cell[0].ent, 'H'); CHECK_EQ(P1.cell[3].ent, 'R');
  CHECK_EQ(P1.cell[5].ent, 0);
  CHECK_EQ(P1.cur, 0); CHECK_EQ(P1.dir, XW_ACROSS);
  CHECK_EQ(xw_elapsed_ms(&P1, 0), 0);

  /* A cursor off the grid, or on a black square, is the start. */
  CHECK_EQ(xw_restore(&P1, "xwp 1\ncur 99 D\nsize 5 5\ng ----.\ng -----\ng -----\ng -----\ng .----\nend\n"), XW_OK);
  CHECK_EQ(P1.cur, 0); CHECK_EQ(P1.dir, XW_ACROSS);
  CHECK_EQ(xw_restore(&P1, "xwp 1\ncur 4 D\nsize 5 5\ng ----.\ng -----\ng -----\ng -----\ng .----\nend\n"), XW_OK);
  CHECK_EQ(P1.cur, 0);

  /* A direction the square has no word in is turned to one it has. */
  load_frame(&P1);
  CHECK_EQ(xw_restore(&P1, "xwp 1\ncur 1 D\nsize 5 5\ng -----\ng -...-\ng -...-\ng -...-\ng -----\nend\n"), XW_OK);
  CHECK_EQ(P1.cur, 1); CHECK_EQ(P1.dir, XW_ACROSS);

  /* Lines it does not know, a clock too large for the counter, CRLF. */
  load_fix(&P1);
  CHECK_EQ(xw_restore(&P1, "xwp 1\r\nmood happy\r\ntime 999999999\r\nsize 5 5\r\n"
                           "g ----.\r\ng -----\r\ng -----\r\ng -----\r\ng .----\r\nend\r\n"), XW_OK);
  CHECK_EQ(xw_elapsed_s(&P1, 0), XW_SECS_MAX);
}

/* ---- the statistics (apps/crossword/xwstats.h) ----------------------------------------------- */

void test_crossword_stats_keep_a_best_and_an_average_per_weekday(void) {
  XwStats s;
  xws_init(&s);
  CHECK_EQ(xws_solved_all(&s), 0);
  CHECK_EQ(xws_avg(&s, 0), 0);
  CHECK_EQ(xws_solve(&s, 0, 600, XWS_NO_DAY), XWS_FIRST);      /* the first Monday */
  CHECK_EQ(s.best[0], 600); CHECK_EQ(s.solved[0], 1);
  CHECK_EQ(xws_solve(&s, 0, 500, XWS_NO_DAY), XWS_BEST);       /* a faster one */
  CHECK_EQ(xws_solve(&s, 0, 500, XWS_NO_DAY), XWS_NOT_BEST);   /* as fast is not faster */
  CHECK_EQ(xws_solve(&s, 0, 900, XWS_NO_DAY), XWS_NOT_BEST);
  CHECK_EQ(s.best[0], 500); CHECK_EQ(s.solved[0], 4);
  CHECK_EQ(s.total[0], 600 + 500 + 500 + 900);
  CHECK_EQ(xws_avg(&s, 0), 625);
  /* Another weekday has its own. */
  CHECK_EQ(xws_solve(&s, 5, 3000, XWS_NO_DAY), XWS_FIRST);
  CHECK_EQ(s.best[5], 3000); CHECK_EQ(s.best[0], 500);
  CHECK_EQ(xws_solved_all(&s), 5);
  /* No time at all is a second, never "none". */
  CHECK_EQ(xws_solve(&s, 2, 0, XWS_NO_DAY), XWS_FIRST);
  CHECK_EQ(s.best[2], 1);
  /* A weekday that is none of them counts nowhere but the streak. */
  CHECK_EQ(xws_solve(&s, -1, 100, 10), XWS_NOT_BEST);
  CHECK_EQ(xws_solve(&s, 7, 100, 10), XWS_NOT_BEST);
  CHECK_EQ(xws_solved_all(&s), 6);
  CHECK_EQ(xws_avg(&s, -1), 0); CHECK_EQ(xws_avg(&s, 7), 0);
}

void test_crossword_stats_a_streak_is_days_in_a_row(void) {
  XwStats s;
  xws_init(&s);
  xws_solve(&s, 0, 100, 500);
  CHECK_EQ(s.streak, 1); CHECK_EQ(s.best_streak, 1);
  xws_solve(&s, 1, 100, 500);                  /* the same day again: still one */
  CHECK_EQ(s.streak, 1);
  xws_solve(&s, 2, 100, 501);
  xws_solve(&s, 3, 100, 502);
  CHECK_EQ(s.streak, 3); CHECK_EQ(s.best_streak, 3);
  CHECK_EQ(xws_streak_now(&s, 502), 3);        /* today, solved */
  CHECK_EQ(xws_streak_now(&s, 503), 3);        /* today not yet: the streak is not lost */
  CHECK_EQ(xws_streak_now(&s, 504), 0);        /* a day missed */
  CHECK_EQ(xws_streak_now(&s, XWS_NO_DAY), 3); /* no clock: as it was left */
  xws_solve(&s, 4, 100, 510);                  /* after a gap it starts again */
  CHECK_EQ(s.streak, 1); CHECK_EQ(s.best_streak, 3);
  xws_solve(&s, 5, 100, XWS_NO_DAY);           /* with no clock the streak is not touched */
  CHECK_EQ(s.streak, 1); CHECK_EQ(s.last_day, 510);
  /* A solve with help is not a clean one. */
  xws_help(&s);
  xws_help(&s);
  CHECK_EQ(s.helped, 2); CHECK_EQ(s.streak, 1); CHECK_EQ(xws_solved_all(&s), 6);
}

void test_crossword_stats_round_trip_through_text(void) {
  XwStats s, t;
  char buf[512];
  int n;
  xws_init(&s);
  xws_solve(&s, 0, 600, 20454);
  xws_solve(&s, 0, 480, 20455);
  xws_solve(&s, 6, 2400, 20456);
  xws_help(&s);
  n = xws_format(&s, buf, sizeof buf);
  CHECK(n > 0 && n < (int)sizeof buf);
  CHECK_EQ((int)strlen(buf), n);
  CHECK(!strncmp(buf, "cwstats 1\nwd 0 2 480 1080\nwd 1 0 0 0\n", 37));
  CHECK(strstr(buf, "wd 6 1 2400 2400\n") != 0);
  CHECK(strstr(buf, "helped 1\nstreak 3 3 20456\n") != 0);
  xws_parse(&t, buf);
  CHECK_EQ(t.solved[0], 2); CHECK_EQ(t.best[0], 480); CHECK_EQ(t.total[0], 1080);
  CHECK_EQ(t.solved[6], 1); CHECK_EQ(t.best[6], 2400);
  CHECK_EQ(t.helped, 1); CHECK_EQ(t.streak, 3); CHECK_EQ(t.best_streak, 3);
  CHECK_EQ(t.last_day, 20456);
  CHECK_EQ(xws_avg(&t, 0), 540);
  /* A fresh set saves its lack of a day as a day that cannot be, and reads it back. */
  xws_init(&s);
  n = xws_format(&s, buf, sizeof buf);
  CHECK(strstr(buf, "streak 0 0 0\n") != 0);
  xws_parse(&t, buf);
  CHECK_EQ(t.last_day, XWS_NO_DAY);
  /* A buffer too small says so. */
  CHECK_EQ(xws_format(&s, buf, 20), -1);
}

void test_crossword_stats_that_cannot_be_read_are_a_fresh_start(void) {
  XwStats t;
  xws_parse(&t, "");
  CHECK_EQ(xws_solved_all(&t), 0); CHECK_EQ(t.last_day, XWS_NO_DAY);
  xws_parse(&t, "nonsense\nwd x y\nwd 9 1 2 3\nwd 1 2\nhelped many\nstreak 1 2\n");
  CHECK_EQ(xws_solved_all(&t), 0); CHECK_EQ(t.helped, 0); CHECK_EQ(t.streak, 0);
  /* What is understood is kept, whatever is round it; CRLF and unknown lines are forgiven. */
  xws_parse(&t, "cwstats 1\r\nwd 3 4 100 800\r\nmood happy\r\nhelped 2\r\nstreak 2 5 99\r\n");
  CHECK_EQ(t.solved[3], 4); CHECK_EQ(t.best[3], 100); CHECK_EQ(t.total[3], 800);
  CHECK_EQ(t.helped, 2); CHECK_EQ(t.streak, 2); CHECK_EQ(t.best_streak, 5); CHECK_EQ(t.last_day, 99);
}
