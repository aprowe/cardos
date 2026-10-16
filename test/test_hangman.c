/* Hangman's rules: apps/hangman/hangman.h. */
#include <string.h>

#include "tinytest.h"
#include "apps/hangman/hangman.h"

void test_hangman_cleans_a_secret(void) {
  char out[HM_MAX + 1];
  CHECK_EQ(hm_clean("  big  red bus ", out), 9);
  CHECK(!strcmp(out, "BIG RED BUS"));
  CHECK_EQ(hm_clean("rock'n'roll", out), 9);       /* not letters: a gap */
  CHECK(!strcmp(out, "ROCK N ROLL"));
  CHECK_EQ(hm_clean("   ", out), 0);
  CHECK_EQ(hm_clean("abcdefghijklmnopqrstuvwxyzabc", out), HM_LONG);   /* 29 */
  CHECK_EQ(hm_clean("abcdefghijklmnopqrstuvwxyzab", out), 28);
}

void test_hangman_wraps_on_word_boundaries(void) {
  int s[3], l[3];
  const char *w = "THE QUICK BROWN FOX";
  CHECK_EQ(hm_wrap(w, (int)strlen(w), 14, s, l, 2), 2);
  CHECK_EQ(s[0], 0); CHECK_EQ(l[0], 9);              /* THE QUICK */
  CHECK_EQ(s[1], 10); CHECK_EQ(l[1], 9);             /* BROWN FOX */
  w = "ABCDEFGHIJKLMN";                                /* exactly one line */
  CHECK_EQ(hm_wrap(w, 14, 14, s, l, 2), 1);
  w = "ABCDEFGHIJKLMNO";                               /* one word too long */
  CHECK_EQ(hm_wrap(w, 15, 14, s, l, 2), -1);
  w = "AAAA BBBB CCCC DDDD EEEE FFFF";                 /* two full lines */
  CHECK_EQ(hm_wrap(w, (int)strlen(w), 14, s, l, 2), 2);
  CHECK_EQ(l[0], 14); CHECK_EQ(s[1], 15);
  w = "AAAA BBBB CCCCC DDDD EEEE FF";                  /* three lines */
  CHECK_EQ(hm_wrap(w, (int)strlen(w), 14, s, l, 2), -1);
  CHECK_EQ(hm_wrap(w, (int)strlen(w), 14, s, l, 3), 3);
}

void test_hangman_refuses_what_cannot_be_played(void) {
  Hangman h;
  CHECK_EQ(hm_start(&h, ""), HM_EMPTY);
  CHECK_EQ(hm_start(&h, "1234"), HM_EMPTY);
  CHECK_EQ(hm_start(&h, "supercalifragilistic"), HM_LONG);   /* a word over 14 */
  CHECK_EQ(hm_start(&h, "aaaa bbbb cccc dddd eeee ffff"), HM_LONG);
  CHECK_EQ(hm_start(&h, "Cat nap"), 0);
  CHECK(!strcmp(h.word, "CAT NAP"));
  CHECK_EQ(h.len, 7);
}

void test_hangman_guesses_fill_letters_and_count_misses(void) {
  Hangman h;
  char shown[HM_MAX + 1], wrong[27];
  hm_start(&h, "cat nap");
  hm_shown(&h, shown, 0);
  CHECK(!strcmp(shown, "___ ___"));
  CHECK_EQ(hm_guess(&h, 'a'), 2);
  hm_shown(&h, shown, 0);
  CHECK(!strcmp(shown, "_A_ _A_"));
  CHECK_EQ(hm_guess(&h, 'A'), HM_AGAIN);
  CHECK_EQ(hm_guess(&h, '7'), HM_BAD);
  CHECK_EQ(hm_guess(&h, ' '), HM_BAD);
  CHECK_EQ(hm_guess(&h, 'z'), HM_MISS);
  CHECK_EQ(hm_guess(&h, 'q'), HM_MISS);
  CHECK_EQ(h.misses, 2);
  CHECK_EQ(hm_wrong(&h, wrong), 2);
  CHECK(!strcmp(wrong, "ZQ"));
  CHECK(!strcmp(h.order, "AZQ"));
  hm_shown(&h, shown, 1);
  CHECK(!strcmp(shown, "CAT NAP"));
}

void test_hangman_finding_every_letter_wins(void) {
  Hangman h;
  hm_start(&h, "cat nap");
  hm_guess(&h, 'c'); hm_guess(&h, 'a'); hm_guess(&h, 't'); hm_guess(&h, 'n');
  CHECK_EQ(h.state, HM_PLAYING);
  CHECK_EQ(hm_guess(&h, 'p'), 1);
  CHECK_EQ(h.state, HM_WON);
  CHECK_EQ(hm_guess(&h, 'x'), HM_BAD);              /* over */
}

void test_hangman_six_misses_lose(void) {
  Hangman h;
  const char *m = "bdefgh";
  int i;
  hm_start(&h, "cat");
  for (i = 0; i < 5; i++) CHECK_EQ(hm_guess(&h, m[i]), HM_MISS);
  CHECK_EQ(h.state, HM_PLAYING);
  CHECK_EQ(hm_guess(&h, 'a'), 1);                   /* a hit costs nothing */
  CHECK_EQ(h.misses, 5);
  CHECK_EQ(hm_guess(&h, m[5]), HM_MISS);
  CHECK_EQ(h.misses, HM_MISSES);
  CHECK_EQ(h.state, HM_LOST);
}

void test_hangman_the_match_scores_and_swaps_the_setter(void) {
  HmMatch m;
  Hangman h;
  hm_match_init(&m);
  CHECK(!strcmp(m.name[0], "P1"));
  CHECK_EQ(m.setter, 0);
  hm_start(&h, "ox");
  hm_match_round(&m, &h);                           /* not over: nothing */
  CHECK_EQ(m.round, 1);
  hm_guess(&h, 'o'); hm_guess(&h, 'x');
  hm_match_round(&m, &h);                           /* P2 guessed it */
  CHECK_EQ(m.score[1], 1); CHECK_EQ(m.score[0], 0);
  CHECK_EQ(m.round, 2); CHECK_EQ(m.setter, 1);
  hm_start(&h, "ox");
  { const char *w = "abcdef"; while (*w) hm_guess(&h, *w++); }
  hm_match_round(&m, &h);                           /* P1 failed: P2, the setter, scores */
  CHECK_EQ(m.score[1], 2); CHECK_EQ(m.score[0], 0);
  CHECK_EQ(m.setter, 0);
}
