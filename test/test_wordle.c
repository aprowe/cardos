/* Wordle's rules, word list, daily puzzle and stats: apps/wordle/wordle.h. */
#include <string.h>

#include "tinytest.h"
#include "apps/wordle/wordle.h"

/* The colours as a string, to compare in one go: g green, y yellow, - grey. */
static const char *sc(const char *guess, const char *answer) {
  static char out[6];
  uint8_t s[5];
  int i;
  wd_score(guess, answer, s);
  for (i = 0; i < 5; i++)
    out[i] = s[i] == WD_CORRECT ? 'g' : s[i] == WD_PRESENT ? 'y' : s[i] == WD_ABSENT ? '-' : '?';
  out[5] = 0;
  return out;
}

void test_wordle_scores_greens_yellows_and_greys(void) {
  CHECK(!strcmp(sc("crane", "crane"), "ggggg"));
  CHECK(!strcmp(sc("bumpy", "crane"), "-----"));
  CHECK(!strcmp(sc("nacre", "crane"), "yyyyg"));
  CHECK(!strcmp(sc("slate", "plate"), "-gggg"));
}

/* The duplicate-letter rules: greens first; then, left to right, a letter is
 * yellow only while the answer still has one of it not already matched. Each
 * of these was worked by hand. */
void test_wordle_duplicate_letters_follow_the_standard_rules(void) {
  /* Two Es guessed, the answer's one E already green: both others grey. */
  CHECK(!strcmp(sc("speed", "abled"), "---gg"));
  CHECK(!strcmp(sc("geese", "those"), "---gg"));
  CHECK(!strcmp(sc("eerie", "crane"), "--y-g"));
  /* One B left after the green: the first spare B is yellow, the next grey. */
  CHECK(!strcmp(sc("bobby", "abbey"), "y-g-g"));
  CHECK(!strcmp(sc("babes", "abbey"), "yygg-"));
  CHECK(!strcmp(sc("kebab", "abbey"), "-ygyy"));
  /* Two Os in both, one in place. */
  CHECK(!strcmp(sc("loops", "spool"), "yygyy"));
  /* Three Rs guessed, two in the answer, one of them green. */
  CHECK(!strcmp(sc("error", "rerun"), "yyg--"));
  /* Two of a letter in the answer, guessed once in the wrong place. */
  CHECK(!strcmp(sc("eagle", "geese"), "y-y-g"));
}

void test_wordle_the_keyboard_keeps_the_best_state_of_each_letter(void) {
  uint8_t keys[26], s[5];
  memset(keys, 0, sizeof keys);
  wd_score("bobby", "abbey", s);
  wd_keys(keys, "bobby", s);
  CHECK_EQ(keys['b' - 'a'], WD_CORRECT);      /* green beats the yellow and grey Bs */
  CHECK_EQ(keys['o' - 'a'], WD_ABSENT);
  CHECK_EQ(keys['y' - 'a'], WD_CORRECT);
  wd_score("abbey", "abbey", s);
  wd_keys(keys, "abbey", s);
  CHECK_EQ(keys['a' - 'a'], WD_CORRECT);
  wd_score("yeast", "abbey", s);             /* a later yellow never downgrades */
  wd_keys(keys, "yeast", s);
  CHECK_EQ(keys['y' - 'a'], WD_CORRECT);
  CHECK_EQ(keys['z' - 'a'], 0);
}

void test_wordle_knows_its_words(void) {
  char w[6];
  int i, n = 0;
  CHECK(wd_valid("crane"));
  CHECK(wd_valid("aahed"));                  /* the first word */
  CHECK(wd_valid("zymic") || wd_valid("zowie")); /* near the end */
  CHECK(!wd_valid("xqzzv"));
  CHECK(!wd_valid("cran"));
  CHECK(!wd_valid("cranes"));
  CHECK(!wd_valid("CRANE"));                 /* the list is lowercase; the app lowers */
  CHECK(!wd_valid("whore"));                 /* blocked */
  CHECK(wd_is_answer("crane"));
  CHECK(!wd_is_answer("aahed"));
  /* Every index names a word that looks itself up again, in order. */
  for (i = 0; i < WD_NWORDS; i++) {
    char prev[6];
    if (i) memcpy(prev, w, 6);
    wd_word(i, w);
    if (wd_find(w) != i) { n++; continue; }
    if (i && strcmp(prev, w) >= 0) n++;
  }
  CHECK_EQ(n, 0);
}

void test_wordle_answers_are_counted_and_each_is_a_word(void) {
  char w[6];
  int k, bad = 0;
  for (k = 0; k < WD_NANSWERS; k++) {
    if (wd_answer(k, w) != 0) { bad++; continue; }
    if (!wd_valid(w) || !wd_is_answer(w)) bad++;
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(wd_answer(WD_NANSWERS, w), -1);
  CHECK_EQ(wd_answer(-1, w), -1);
}

void test_wordle_daily_is_one_word_a_date_and_none_repeat_for_a_cycle(void) {
  static uint8_t seen[WD_NANSWERS];
  int d, dup = 0;
  CHECK_EQ(wd_day(2026, 1, 1), 0);
  CHECK_EQ(wd_day(2026, 1, 2), 1);
  CHECK_EQ(wd_day(2026, 3, 1), 59);
  CHECK_EQ(wd_day(2027, 1, 1), 365);
  CHECK_EQ(wd_day(2028, 3, 1), 365 + 365 + 31 + 29);   /* 2028 is a leap year */
  CHECK_EQ(wd_day(2025, 12, 31), -1);
  CHECK_EQ(wd_daily_index(10), wd_daily_index(10));
  CHECK(wd_daily_index(0) != wd_daily_index(1));
  memset(seen, 0, sizeof seen);
  for (d = 0; d < WD_NANSWERS; d++) {
    int k = wd_daily_index(d);
    if (k < 0 || k >= WD_NANSWERS || seen[k]) dup++;
    else seen[k] = 1;
  }
  CHECK_EQ(dup, 0);
  CHECK_EQ(wd_daily_index(WD_NANSWERS + 7), wd_daily_index(7));
  /* Not the list in alphabetical order. */
  CHECK(wd_daily_index(1) != wd_daily_index(0) + 1);
}

void test_wordle_a_game_takes_letters_and_refuses_bad_guesses(void) {
  WdGame g;
  wd_start(&g, "crane");
  CHECK_EQ(wd_submit(&g), WD_SHORT);
  CHECK(wd_type(&g, 'S'));                   /* lowered */
  CHECK(wd_type(&g, 'l'));
  CHECK(!wd_type(&g, '1'));
  CHECK(wd_type(&g, 'a'));
  CHECK(wd_type(&g, 't'));
  CHECK(wd_type(&g, 'e'));
  CHECK(!wd_type(&g, 'x'));                  /* full */
  CHECK(!strcmp(g.cur, "slate"));
  wd_back(&g);
  CHECK(!strcmp(g.cur, "slat"));
  wd_type(&g, 'x');
  CHECK_EQ(wd_submit(&g), WD_UNKNOWN);       /* "slatx" */
  CHECK_EQ(g.n, 0);
  CHECK(!strcmp(g.cur, "slatx"));            /* kept, to be fixed */
  wd_back(&g);
  wd_type(&g, 'e');
  CHECK_EQ(wd_submit(&g), WD_OK);
  CHECK_EQ(g.n, 1);
  CHECK_EQ(g.len, 0);
  CHECK_EQ(g.over, WD_PLAYING);
  CHECK_EQ(g.keys['s' - 'a'], WD_ABSENT);
  CHECK_EQ(g.keys['e' - 'a'], WD_CORRECT);
  CHECK(wd_guess(&g, "crane") == WD_OK);
  CHECK_EQ(g.over, WD_WON);
  CHECK_EQ(g.n, 2);
  CHECK(!wd_type(&g, 'a'));
  CHECK_EQ(wd_submit(&g), WD_OVER);
}

void test_wordle_six_misses_lose(void) {
  WdGame g;
  int i;
  const char *tries[6] = { "slate", "pious", "dumpy", "bight", "flock", "wheez" };
  wd_start(&g, "crane");
  for (i = 0; i < 5; i++) CHECK_EQ(wd_guess(&g, tries[i]), WD_OK);
  CHECK_EQ(g.over, WD_PLAYING);
  CHECK_EQ(wd_guess(&g, tries[5]), WD_UNKNOWN);
  CHECK_EQ(wd_guess(&g, "zebra"), WD_OK);
  CHECK_EQ(g.over, WD_LOST);
  CHECK_EQ(g.n, 6);
}

void test_wordle_stats_count_dailies_and_streaks(void) {
  WdStats s;
  wd_stats_init(&s);
  wd_stats_finish(&s, 10, 1, 3);
  CHECK_EQ(s.played, 1); CHECK_EQ(s.won, 1); CHECK_EQ(s.streak, 1); CHECK_EQ(s.best, 1);
  CHECK_EQ(s.dist[2], 1);
  wd_stats_finish(&s, 10, 1, 3);             /* the same day twice counts once */
  CHECK_EQ(s.played, 1);
  wd_stats_finish(&s, 11, 1, 6);
  CHECK_EQ(s.streak, 2); CHECK_EQ(s.best, 2); CHECK_EQ(s.dist[5], 1);
  CHECK_EQ(wd_stats_streak(&s, 11), 2);
  CHECK_EQ(wd_stats_streak(&s, 12), 2);      /* today not played yet */
  CHECK_EQ(wd_stats_streak(&s, 13), 0);      /* a day missed */
  wd_stats_finish(&s, 13, 1, 1);             /* after a gap: starts again */
  CHECK_EQ(s.streak, 1); CHECK_EQ(s.best, 2);
  wd_stats_finish(&s, 14, 0, 6);
  CHECK_EQ(s.streak, 0); CHECK_EQ(s.played, 4); CHECK_EQ(s.won, 3);
  CHECK_EQ(wd_stats_streak(&s, 14), 0);
}

void test_wordle_stats_round_trip_through_text(void) {
  WdStats s, t;
  char buf[512];
  int n;
  wd_stats_init(&s);
  wd_stats_finish(&s, 287, 1, 4);
  wd_stats_finish(&s, 288, 1, 2);
  s.day = 289;
  s.nguess = 2;
  memcpy(s.guess[0], "slate", 6);
  memcpy(s.guess[1], "crony", 6);
  n = wd_stats_format(&s, buf, sizeof buf);
  CHECK(n > 0 && n < (int)sizeof buf);
  CHECK((int)strlen(buf) == n);
  wd_stats_parse(&t, buf);
  CHECK_EQ(t.played, 2); CHECK_EQ(t.won, 2); CHECK_EQ(t.streak, 2); CHECK_EQ(t.best, 2);
  CHECK_EQ(t.dist[1], 1); CHECK_EQ(t.dist[3], 1); CHECK_EQ(t.dist[0], 0);
  CHECK_EQ(t.last_win, 288); CHECK_EQ(t.last_done, 288);
  CHECK_EQ(t.day, 289); CHECK_EQ(t.nguess, 2);
  CHECK(!strcmp(t.guess[1], "crony"));
  /* Nothing, or rubbish, is a fresh start, not a crash. */
  wd_stats_parse(&t, "");
  CHECK_EQ(t.played, 0); CHECK_EQ(t.day, WD_NO_DAY);
  wd_stats_parse(&t, "played x\ndist 1 2\nday 5 crane toolong zz\nwon 3\r\n");
  CHECK_EQ(t.won, 3); CHECK_EQ(t.dist[1], 2); CHECK_EQ(t.nguess, 1);
  /* A buffer too small says so. */
  CHECK_EQ(wd_stats_format(&s, buf, 10), -1);
}
