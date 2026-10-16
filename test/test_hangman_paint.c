/* The real Hangman app (apps/hangman/hangman.c) played through two rounds by
 * keys, painted into a framebuffer with the real fonts (test/fakescreen.h).
 * Every screen must write every pixel, the secret must never be drawn while
 * it is typed, and the score and the setter must move as the rules say.
 *
 * HANGMAN_DUMP=dir writes the screens there (hangman_*.ppm), to look at. */
#include <string.h>

#include "tinytest.h"
#include "fakescreen.h"

#define capp_info hangman_capp_info
#define capp_main hangman_capp_main
#include "apps/hangman/hangman.c"
#undef capp_info
#undef capp_main

static CardApi HA;

static void h_boot(void) {
  fakeapi_init(&HA);
  fakescreen_install(&HA);
  hangman_capp_main(&HA, 1, 0);
  fakescreen_paint();
}

static void h_keys(const char *s) {
  for (; *s; s++)
    if (fakescreen_ui.key(fakescreen_ui.state, (uint8_t)*s)) fakescreen_paint();
}

static void shot(const char *name) {
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("HANGMAN_DUMP", name);
}

void test_hangman_paint_a_round_won_by_the_guesser(void) {
  h_boot();
  CHECK_EQ(G.screen, S_SCORE);
  shot("hangman_score");
  h_keys("e");                                    /* names */
  CHECK_EQ(G.screen, S_NAMES);
  h_keys("\x08\x08" "Alex\r");
  h_keys("\x08\x08" "Sam\r");
  CHECK(!strcmp(G.m.name[0], "Alex"));
  CHECK(!strcmp(G.m.name[1], "Sam"));
  CHECK_EQ(G.screen, S_SCORE);

  h_keys("\r");
  CHECK_EQ(G.screen, S_SECRET);
  CHECK(fakescreen_ui.wants_text(fakescreen_ui.state));
  h_keys("\r");                                   /* nothing typed */
  CHECK_EQ(G.screen, S_SECRET);
  CHECK(G.msg[0] != 0);
  h_keys("big red bus");
  CHECK(!strcmp(G.secret, "BIG RED BUS"));
  shot("hangman_secret");
  /* Masked: nothing of the field is the colour of the text but the dots,
   * and no letters are drawn -- compare with the secret shown. */
  {
    int masked = fakescreen_count(capp_rect(8, 48, 224, 22), C_TEXT), shown;
    h_keys("\t");
    shown = fakescreen_count(capp_rect(8, 48, 224, 22), C_TEXT);
    CHECK(masked != shown);
    shot("hangman_secret_shown");
    h_keys("\t");
  }
  h_keys("\r");
  CHECK_EQ(G.screen, S_HAND);
  shot("hangman_hand");
  h_keys("\r");
  CHECK_EQ(G.screen, S_PLAY);
  h_keys("eqz");
  CHECK_EQ(G.h.misses, 2);
  h_keys("e");                                    /* again: a message, no cost */
  CHECK_EQ(G.h.misses, 2);
  CHECK(G.msg[0] != 0);
  shot("hangman_play");
  h_keys("bigrdus");
  CHECK_EQ(G.screen, S_OVER);
  CHECK_EQ(G.h.state, HM_WON);
  CHECK_EQ(G.m.score[1], 1);                      /* Sam guessed it */
  CHECK_EQ(G.m.setter, 1);                        /* and sets the next */
  shot("hangman_won");
  h_keys("\r");
  CHECK_EQ(G.screen, S_SCORE);
  shot("hangman_score2");
}

void test_hangman_paint_a_round_lost_and_one_given_up(void) {
  h_boot();
  h_keys("\r" "cat\r\r");
  CHECK_EQ(G.screen, S_PLAY);
  h_keys("bdefgh");
  CHECK_EQ(G.screen, S_OVER);
  CHECK_EQ(G.h.state, HM_LOST);
  CHECK_EQ(G.m.score[0], 1);                      /* P1, the setter */
  shot("hangman_lost");
  h_keys("\r");
  /* Round two: P2 sets; P1 gives up. */
  CHECK_EQ(G.m.setter, 1);
  h_keys("\r" "dog\r\r");
  h_keys("\x1b");
  CHECK(G.asking);
  CHECK(!fakescreen_ui.wants_text(fakescreen_ui.state));
  shot("hangman_giveup");
  h_keys("n");
  CHECK(!G.asking);
  CHECK_EQ(G.screen, S_PLAY);
  h_keys("\x1by");
  CHECK_EQ(G.screen, S_OVER);
  CHECK_EQ(G.m.score[1], 1);
  CHECK_EQ(G.m.setter, 0);
}

void test_hangman_paint_a_phrase_on_two_lines(void) {
  h_boot();
  h_keys("\r" "the quick brown fox\r\r");
  CHECK_EQ(G.screen, S_PLAY);
  h_keys("o");
  shot("hangman_two_lines");
  h_keys("\x1by");
  shot("hangman_reveal");
}
