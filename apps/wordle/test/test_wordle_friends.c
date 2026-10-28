/* apps/wordle/wdfriends.h: what a finished daily publishes for friends --
 * the colours, never the letters -- and reading a friend's back. */

#include <string.h>

#include "tinytest.h"
#include "apps/wordle/wdfriends.h"

static void play(WdGame *g, const char *answer, const char *const *guesses) {
  wd_start(g, answer);
  for (; *guesses; guesses++) wd_guess(g, *guesses);
}

void test_wordle_friends_a_win_is_its_colours_and_no_letters(void) {
  static const char *const G3[] = { "crane", "slate", "plate", 0 };
  WdGame g;
  WdFriend f;
  char buf[160];
  play(&g, "plate", G3);
  CHECK_EQ(g.over, WD_WON);
  CHECK(wdf_format(&g, 299, 5, buf, sizeof buf) > 0);
  CHECK(!strcmp(buf, "day 299\nresult 3\nrows ..g.g .gggg ggggg\nstreak 5\n"));
  CHECK(!strstr(buf, "plate") && !strstr(buf, "crane"));
  CHECK(wdf_parse(buf, (int)strlen(buf), &f));
  CHECK_EQ(f.day, 299);
  CHECK_EQ(f.result, 3);
  CHECK_EQ(f.nrows, 3);
  CHECK_EQ(f.rows[0][2], WD_CORRECT);
  CHECK_EQ(f.rows[0][3], WD_ABSENT);
  CHECK_EQ(f.rows[2][4], WD_CORRECT);
  CHECK_EQ(f.streak, 5);
}

void test_wordle_friends_six_misses_are_an_x(void) {
  static const char *const G6[] = { "crane", "crane", "crane", "crane", "crane", "crane", 0 };
  WdGame g;
  WdFriend f;
  char buf[160], sc[4];
  play(&g, "plumb", G6);
  CHECK_EQ(g.over, WD_LOST);
  wdf_format(&g, 7, 0, buf, sizeof buf);
  CHECK(wdf_parse(buf, (int)strlen(buf), &f));
  CHECK_EQ(f.result, WDF_X);
  CHECK_EQ(f.nrows, 6);
  wdf_score(&f, sc);
  CHECK(!strcmp(sc, "X/6"));
}

void test_wordle_friends_refuses_a_game_still_going_and_junk(void) {
  static const char *const G1[] = { "crane", 0 };
  WdGame g;
  WdFriend f;
  char buf[160];
  play(&g, "plate", G1);
  CHECK_EQ(wdf_format(&g, 1, 0, buf, sizeof buf), -1);
  CHECK(!wdf_parse("hello\nworld", 11, &f));
  CHECK(!wdf_parse("day 3\n", 6, &f));                 /* no result */
  CHECK(wdf_parse("day 3\nresult 2\nrows gg.gg ggggg zzz\n", 36, &f));
  CHECK_EQ(f.nrows, 2);                                  /* stops at a bad row */
  CHECK(wdf_parse("day 3\nresult 4", 15, &f));          /* no newline at the end */
  CHECK_EQ(f.result, 4);
}
