/* The real Wordle app (apps/wordle/wordle.c) driven by keys and painted
 * into a framebuffer with the real fonts (test/fakescreen.h).
 *
 * Every paint must write every pixel -- there is no framebuffer on the
 * device -- and the daily puzzle must be the date's, kept on the card as it
 * is played, and counted once in the stats when it ends.
 *
 * WORDLE_DUMP=dir writes the screens there (wordle_*.ppm), to look at. */
#include <string.h>

#include "tinytest.h"
#include "fakescreen.h"

#define capp_info wordle_capp_info
#define capp_main wordle_capp_main
#include "apps/wordle/wordle.c"
#undef capp_info
#undef capp_main

static CardApi WA;

static void w_boot(int synced) {
  fakeapi_init(&WA);
  fakefs_mem(&WA);
  fakescreen_install(&WA);
  memset(&fakeapi_now, 0, sizeof fakeapi_now);
  if (synced) {
    fakeapi_now.year = 2026; fakeapi_now.month = 10; fakeapi_now.day = 16;
    fakeapi_now.hour = 9; fakeapi_now.synced = 2;
    fakeapi_epoch = 1792141200u;
  }
  wordle_capp_main(&WA, 1, 0);
  fakescreen_paint();
}

static void w_keys(const char *s) {
  for (; *s; s++) {
    if (fakescreen_ui.key(fakescreen_ui.state, (uint8_t)*s)) fakescreen_paint();
  }
}

/* A five-letter accepted word that is not `answer` and shares no letter with
 * it, so a row of it is all grey. */
static void a_miss(const char *answer, char out[6]) {
  int i, j, k;
  for (i = 0; i < WD_NWORDS; i++) {
    int clash = 0;
    wd_word(i, out);
    for (j = 0; j < 5; j++) for (k = 0; k < 5; k++) clash |= out[j] == answer[k];
    if (!clash) return;
  }
}

void test_wordle_paint_opens_on_the_daily_and_fills_the_screen(void) {
  char w[6];
  w_boot(1);
  CHECK_EQ(G.mode, MODE_DAILY);
  CHECK_EQ(G.today, wd_day(2026, 10, 16));
  wd_answer(wd_daily_index(G.today), w);
  CHECK(!strcmp(G.g.answer, w));
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(fakeapi_paint_direct != 1);
  CHECK(fakescreen_ui.wants_text(fakescreen_ui.state));
  fakescreen_dump("WORDLE_DUMP", "wordle_start");
}

void test_wordle_paint_a_refused_guess_says_why_and_stays(void) {
  w_boot(1);
  w_keys("abc\r");
  CHECK(!strcmp(G.msg, "not enough letters"));
  CHECK_EQ(G.g.n, 0);
  w_keys("de\r");
  CHECK(!strcmp(G.msg, "not in word list"));
  CHECK(!strcmp(G.g.cur, "abcde"));
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLE_DUMP", "wordle_refused");
  /* The message times out. */
  fakeapi_ticks += 5000;
  CHECK(fakescreen_ui.tick(fakescreen_ui.state, fakeapi_ticks));
  CHECK_EQ(G.msg[0], 0);
  w_keys("\x08\x08\x08\x08\x08");
  CHECK_EQ(G.g.len, 0);
}

void test_wordle_paint_colours_tiles_and_keys(void) {
  char miss[6], typed[8];
  int green;
  w_boot(1);
  a_miss(G.g.answer, miss);
  api->fmt(typed, sizeof typed, "%s\r", miss);
  w_keys(typed);
  CHECK_EQ(G.g.n, 1);
  CHECK_EQ(G.g.keys[miss[0] - 'a'], WD_ABSENT);
  /* The first row is grey tiles now. */
  CHECK(fakescreen_count(capp_rect(BX + 2, BY + 2, 3, 3), C_GREY) == 9);
  /* The answer itself: five green tiles in row two. */
  api->fmt(typed, sizeof typed, "%s\r", G.g.answer);
  w_keys(typed);
  CHECK_EQ(G.g.over, WD_WON);
  green = fakescreen_count(capp_rect(BX, BY + PITCH, 5 * PITCH, TILE), C_GREEN);
  CHECK(green > 5 * 200);
  CHECK(!fakescreen_ui.wants_text(fakescreen_ui.state));
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLE_DUMP", "wordle_won");
  /* Counted, and saved. */
  CHECK_EQ(G.st.played, 1);
  CHECK_EQ(G.st.dist[1], 1);
  CHECK(fakefs_get(STATS_PATH) != 0);
  w_keys("\t");
  CHECK_EQ(G.view, V_STATS);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLE_DUMP", "wordle_stats");
  w_keys("\x1b");
  CHECK_EQ(G.view, V_GAME);
}

void test_wordle_paint_the_daily_comes_back_as_it_was_left(void) {
  char miss[6], typed[8], answer[6];
  w_boot(1);
  memcpy(answer, G.g.answer, 6);
  a_miss(answer, miss);
  api->fmt(typed, sizeof typed, "%s\r", miss);
  w_keys(typed);
  w_keys(typed);
  CHECK_EQ(G.g.n, 2);
  /* Closed and opened again: the same two guesses. */
  memset(&G, 0, sizeof G);
  wordle_capp_main(&WA, 1, 0);
  CHECK_EQ(G.g.n, 2);
  CHECK(!strcmp(G.g.guess[1], miss));
  CHECK(!strcmp(G.g.answer, answer));
  /* Lose it: four more misses. */
  w_keys(typed); w_keys(typed); w_keys(typed); w_keys(typed);
  CHECK_EQ(G.g.over, WD_LOST);
  CHECK_EQ(G.st.played, 1);
  CHECK_EQ(G.st.won, 0);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLE_DUMP", "wordle_lost");
  /* Opened again it is finished, and not counted twice. */
  memset(&G, 0, sizeof G);
  wordle_capp_main(&WA, 1, 0);
  CHECK_EQ(G.g.over, WD_LOST);
  CHECK_EQ(G.st.played, 1);
  /* n is a practice game, which the stats do not see. */
  w_keys("n");
  CHECK_EQ(G.mode, MODE_PRACTICE);
  CHECK_EQ(G.g.over, WD_PLAYING);
  api->fmt(typed, sizeof typed, "%s\r", G.g.answer);
  w_keys(typed);
  CHECK_EQ(G.g.over, WD_WON);
  CHECK_EQ(G.st.played, 1);
  fakescreen_dump("WORDLE_DUMP", "wordle_practice");
  w_keys("d");
  CHECK_EQ(G.mode, MODE_DAILY);
  CHECK(!strcmp(G.g.answer, answer));
}

void test_wordle_paint_with_no_clock_is_practice(void) {
  w_boot(0);
  CHECK_EQ(G.mode, MODE_PRACTICE);
  CHECK(G.msg[0] != 0);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLE_DUMP", "wordle_noclock");
}
