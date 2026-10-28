/* Wordloom's rules: apps/wordloom/wordloom.h. Every score below was worked by
 * hand from WL_LAYOUT and WL_VALUE. */
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "apps/wordloom/wordloom.h"

static WlGame g;

/* A game whose racks are what the test says (the bag stays the seed's). */
static void with_racks(const char *r0, const char *r1) {
  wl_new(&g, 1);
  strcpy(g.rack[0], r0);
  strcpy(g.rack[1], r1);
}

static int same_game(const WlGame *a, const WlGame *b) {
  return !memcmp(a->board, b->board, sizeof a->board) && !memcmp(a->rack, b->rack, sizeof a->rack) &&
         a->nbag == b->nbag && !memcmp(a->bag, b->bag, (size_t)a->nbag) &&
         a->score[0] == b->score[0] && a->score[1] == b->score[1] && a->turn == b->turn &&
         a->over == b->over;
}

void test_wordloom_the_tiles_and_the_deal(void) {
  int i, total = 0, n[26];
  for (i = 0; i < 26; i++) { total += WL_COUNT[i]; CHECK(WL_VALUE[i] >= 1); }
  CHECK_EQ(total, WL_TILES);
  wl_new(&g, 12345);
  CHECK_EQ(wl_len(g.rack[0]), WL_RACK);
  CHECK_EQ(wl_len(g.rack[1]), WL_RACK);
  CHECK_EQ(g.nbag, WL_TILES - 2 * WL_RACK);
  CHECK_EQ(g.turn, 0);
  CHECK_EQ(g.over, WL_PLAYING);
  /* Bag and racks together are the eighty tiles. */
  memset(n, 0, sizeof n);
  for (i = 0; i < g.nbag; i++) n[g.bag[i] - 'a']++;
  for (i = 0; i < WL_RACK; i++) { n[g.rack[0][i] - 'a']++; n[g.rack[1][i] - 'a']++; }
  for (i = 0; i < 26; i++) CHECK_EQ(n[i], WL_COUNT[i]);
}

void test_wordloom_a_seed_is_the_same_game_everywhere(void) {
  WlGame a, b;
  wl_new(&a, 777);
  wl_new(&b, 777);
  CHECK(same_game(&a, &b));
  wl_new(&b, 778);
  CHECK(memcmp(a.bag, b.bag, sizeof a.bag) != 0);
  /* Seed 0 is a fixed one, not a stuck generator. */
  wl_new(&a, 0);
  wl_new(&b, WL_SEED0);
  CHECK(same_game(&a, &b));
  CHECK(a.rack[0][0] != 0);
}

void test_wordloom_the_layout_is_symmetric_and_the_centre_a_knot(void) {
  int r, c, n[5] = { 0, 0, 0, 0, 0 };
  for (r = 0; r < WL_N; r++)
    for (c = 0; c < WL_N; c++) {
      int p = wl_premium(r, c);
      n[p]++;
      CHECK_EQ(p, wl_premium(c, r));
      CHECK_EQ(p, wl_premium(WL_N - 1 - r, c));
      CHECK_EQ(p, wl_premium(r, WL_N - 1 - c));
    }
  CHECK_EQ(wl_premium(WL_MID, WL_MID), WL_KNOT);
  CHECK_EQ(n[WL_LOOM], 4);
  CHECK_EQ(n[WL_GOLD], 8);
  CHECK_EQ(n[WL_WARP], 12);
  CHECK_EQ(n[WL_KNOT], 9);
  CHECK_EQ(wl_premium(-1, 0), WL_PLAIN);
}

void test_wordloom_the_first_weave_covers_the_centre(void) {
  WlPlay p;
  with_racks("crowbe", "aeiost");
  CHECK_EQ(wl_check(&g, 0, 4, 4, WL_ACROSS, "crow", 0, 0, &p), WL_E_CENTRE);
  CHECK_EQ(wl_check(&g, 0, 5, 5, WL_ACROSS, "c", 0, 0, &p), WL_E_SHORT);
  CHECK_EQ(wl_check(&g, 1, 5, 4, WL_ACROSS, "aei", 0, 0, &p), WL_E_TURN);
  CHECK_EQ(wl_check(&g, 0, 5, 4, WL_ACROSS, "crowz", 0, 0, &p), WL_E_RACK);
  CHECK_EQ(wl_check(&g, 0, 5, 4, WL_ACROSS, "cc", 0, 0, &p), WL_E_RACK);   /* only one c */
  CHECK_EQ(wl_check(&g, 0, 5, 8, WL_ACROSS, "crow", 0, 0, &p), WL_E_OFF);
  CHECK_EQ(wl_check(&g, 0, 5, 4, WL_ACROSS, "", 0, 0, &p), WL_E_NONE);
  CHECK_EQ(wl_check(&g, 0, 5, 4, WL_ACROSS, "Crow", 0, 0, &p), WL_E_PARSE);
  CHECK_EQ(wl_check(&g, 0, 11, 4, WL_ACROSS, "crow", 0, 0, &p), WL_E_OFF);
  /* c3 + r1 on the centre knot (2) + o1 + w4 = 10. */
  CHECK_EQ(wl_check(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0, &p), WL_OK);
  CHECK_EQ(p.points, 10);
  CHECK_EQ(p.nwords, 1);
  CHECK(!strcmp(p.w[0].text, "crow"));
  /* Down through the centre works too. */
  CHECK_EQ(wl_check(&g, 0, 4, 5, WL_DOWN, "ow", 0, 0, &p), WL_OK);
  CHECK_EQ(p.points, 1 + 4 * 2);
}

void test_wordloom_a_weave_refills_the_rack_from_the_end_of_the_bag(void) {
  char want[WL_RACK + 1];
  int nbag;
  with_racks("crowbe", "aeiost");
  nbag = g.nbag;
  want[0] = 'b'; want[1] = 'e';
  want[2] = g.bag[nbag - 1]; want[3] = g.bag[nbag - 2];
  want[4] = g.bag[nbag - 3]; want[5] = g.bag[nbag - 4]; want[6] = 0;
  CHECK_EQ(wl_play(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0), 10);
  CHECK(!strcmp(g.rack[0], want));
  CHECK_EQ(g.nbag, nbag - 4);
  CHECK_EQ(g.score[0], 10);
  CHECK_EQ(g.turn, 1);
  CHECK_EQ(g.tiles, 4);
  CHECK_EQ(g.board[5][5], 'r');
  CHECK(g.fresh[5][4] && g.fresh[5][7] && !g.fresh[4][4]);
  CHECK_EQ(g.last_kind, WL_K_PLAY);
  CHECK(!strcmp(g.last_word, "crow"));
  CHECK_EQ(g.last_points, 10);
  /* The same move twice is refused: it is not their turn now. */
  CHECK_EQ(wl_play(&g, 0, 4, 4, WL_ACROSS, "be", 0, 0), WL_E_TURN);
}

void test_wordloom_later_weaves_touch_and_step_over_tiles(void) {
  WlPlay p;
  with_racks("crowbe", "aeiost");
  wl_play(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0);
  CHECK_EQ(wl_check(&g, 1, 0, 0, WL_ACROSS, "at", 0, 0, &p), WL_E_ALONE);
  CHECK_EQ(wl_check(&g, 1, 5, 5, WL_ACROSS, "at", 0, 0, &p), WL_E_TAKEN);
  /* t on gold (4,6), the o stepped over, e on gold (6,6): 3 + 1 + 3 = 7. */
  CHECK_EQ(wl_check(&g, 1, 4, 6, WL_DOWN, "te", 0, 0, &p), WL_OK);
  CHECK_EQ(p.points, 7);
  CHECK_EQ(p.pr[1], 6);
  CHECK(!strcmp(p.w[0].text, "toe"));
  /* Extending a word at its end: "crows", the s plain at (5,8). */
  CHECK_EQ(wl_check(&g, 1, 5, 8, WL_ACROSS, "s", 0, 0, &p), WL_OK);
  CHECK(!strcmp(p.w[0].text, "crows"));
  CHECK_EQ(p.points, 3 + 1 + 1 + 4 + 1);   /* the knot under r was used */
  /* At its start: "acrow" -- the a at (5,3). */
  CHECK_EQ(wl_check(&g, 1, 5, 3, WL_ACROSS, "a", 0, 0, &p), WL_OK);
  CHECK(!strcmp(p.w[0].text, "acrow"));
  /* A run that steps over the whole word and off the end. */
  CHECK_EQ(wl_check(&g, 1, 5, 3, WL_ACROSS, "aeiost", 0, 0, &p), WL_E_OFF);
}

void test_wordloom_premiums_count_once(void) {
  WlPlay p;
  with_racks("crowbe", "aeiost");
  wl_play(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0);
  wl_play(&g, 1, 4, 6, WL_DOWN, "te", 0, 0);       /* t on gold at (4,6) */
  strcpy(g.rack[0], "abcdef");
  /* a at (4,5): "at" across (a1 + t1, the gold already used) and "ar" down
   * (a1 + r1, the knot already used) = 4. */
  CHECK_EQ(wl_check(&g, 0, 4, 5, WL_ACROSS, "a", 0, 0, &p), WL_OK);
  CHECK_EQ(p.nwords, 2);
  CHECK_EQ(p.points, 4);
}

void test_wordloom_word_multipliers_stack_and_the_loom_triples(void) {
  WlPlay p;
  with_racks("catbxy", "aeiost");
  g.board[1][5] = 'o';
  g.tiles = 1;
  /* c, a, t across (0,4)-(0,6): warp, plain, warp -> (3+1+1) x2 x2 = 20, and
   * "ao" down through (1,5) = 2. */
  CHECK_EQ(wl_check(&g, 0, 0, 4, WL_ACROSS, "cat", 0, 0, &p), WL_OK);
  CHECK_EQ(p.nwords, 2);
  CHECK_EQ(p.w[0].points, 20);
  CHECK_EQ(p.points, 22);
  /* b on the loom at (1,1), beside an a: "ba" x3. */
  g.board[1][2] = 'a';
  g.tiles = 2;
  CHECK_EQ(wl_check(&g, 0, 1, 1, WL_ACROSS, "b", 0, 0, &p), WL_OK);
  CHECK(!strcmp(p.w[0].text, "ba"));
  CHECK_EQ(p.points, (4 + 1) * 3);
}

void test_wordloom_the_weave_bonus_for_three_words(void) {
  WlPlay p;
  with_racks("crowbe", "aeiost");
  wl_play(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0);
  /* a on gold (6,4), e plain (6,5) under c and r: "ae" 3+1 = 4, "ca" 3+3 = 6,
   * "re" 1+1 = 2, and the weave's 5. */
  CHECK_EQ(wl_check(&g, 1, 6, 4, WL_ACROSS, "ae", 0, 0, &p), WL_OK);
  CHECK_EQ(p.nwords, 3);
  CHECK_EQ(p.weave, WL_WEAVE);
  CHECK_EQ(p.points, 4 + 6 + 2 + WL_WEAVE);
  CHECK_EQ(p.bolt, 0);
}

void test_wordloom_the_bolt_for_all_six(void) {
  WlPlay p;
  with_racks("abcdef", "aeiost");
  /* (5,0)-(5,5): a1, b4 on a knot 8, c3, d2, e1, f4 on the centre knot 8 = 23. */
  CHECK_EQ(wl_check(&g, 0, 5, 0, WL_ACROSS, "abcdef", 0, 0, &p), WL_OK);
  CHECK_EQ(p.bolt, WL_BOLT);
  CHECK_EQ(p.points, 23 + WL_BOLT);
  CHECK_EQ(wl_play(&g, 0, 5, 0, WL_ACROSS, "abcdef", 0, 0), 43);
  CHECK_EQ(wl_len(g.rack[0]), WL_RACK);
}

static int no_q_words(const char *w, void *ctx) {
  (*(int *)ctx)++;
  while (*w) if (*w++ == 'q') return 0;
  return 1;
}

void test_wordloom_a_dictionary_is_asked_about_every_word(void) {
  WlPlay p;
  int asked = 0;
  with_racks("crowbe", "aeqost");
  wl_play(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0);
  CHECK_EQ(wl_check(&g, 1, 6, 4, WL_ACROSS, "ae", no_q_words, &asked, &p), WL_OK);
  CHECK_EQ(asked, 3);
  CHECK_EQ(wl_check(&g, 1, 6, 4, WL_ACROSS, "aq", no_q_words, &asked, &p), WL_E_WORD);
  CHECK_EQ(p.bad, 0);                          /* "aq", the word along the line */
  CHECK_EQ(wl_play(&g, 1, 6, 4, WL_ACROSS, "aq", no_q_words, &asked), WL_E_WORD);
  CHECK_EQ(g.board[6][4], 0);                  /* nothing changed */
  CHECK_EQ(g.turn, 1);
}

void test_wordloom_swaps_keep_the_tiles_and_need_a_bag(void) {
  WlGame a, b;
  int i, n[26];
  wl_new(&a, 99);
  b = a;
  CHECK_EQ(wl_swap(&a, 1, "a"), WL_E_TURN);
  {
    char two[3];
    two[0] = a.rack[0][0]; two[1] = a.rack[0][1]; two[2] = 0;
    CHECK_EQ(wl_swap(&a, 0, two), WL_OK);
    CHECK_EQ(wl_swap(&b, 0, two), WL_OK);
  }
  CHECK(same_game(&a, &b));                    /* the same everywhere */
  CHECK_EQ(a.nbag, WL_TILES - 12);
  CHECK_EQ(a.turn, 1);
  CHECK_EQ(a.stall, 1);
  CHECK_EQ(a.last_kind, WL_K_SWAP);
  CHECK_EQ(a.last_count, 2);
  memset(n, 0, sizeof n);
  for (i = 0; i < a.nbag; i++) n[a.bag[i] - 'a']++;
  for (i = 0; i < WL_RACK; i++) { n[a.rack[0][i] - 'a']++; n[a.rack[1][i] - 'a']++; }
  for (i = 0; i < 26; i++) CHECK_EQ(n[i], WL_COUNT[i]);
  /* A bag of two takes a swap of two, not three. */
  a.nbag = 2;
  {
    char three[4];
    memcpy(three, a.rack[1], 3); three[3] = 0;
    CHECK_EQ(wl_swap(&a, 1, three), WL_E_BAG);
    three[2] = 0;
    CHECK_EQ(wl_swap(&a, 1, three), WL_OK);
  }
  CHECK_EQ(wl_swap(&a, 0, "zz"), WL_E_RACK);   /* there is one z */
}

void test_wordloom_four_scoreless_turns_end_it(void) {
  int v0, v1;
  with_racks("crowbe", "aeiost");
  wl_play(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0);
  CHECK_EQ(wl_pass(&g, 1), WL_OK);
  CHECK_EQ(wl_pass(&g, 1), WL_E_TURN);
  CHECK_EQ(wl_pass(&g, 0), WL_OK);
  CHECK_EQ(wl_pass(&g, 1), WL_OK);
  CHECK_EQ(g.over, WL_PLAYING);
  v0 = wl_rack_value(g.rack[0]);
  v1 = wl_rack_value(g.rack[1]);
  CHECK_EQ(wl_pass(&g, 0), WL_OK);
  CHECK_EQ(g.over, WL_ENDED);
  CHECK_EQ(g.score[0], 10 - v0);
  CHECK_EQ(g.score[1], 0 - v1);
  CHECK_EQ(g.winner, g.score[0] > g.score[1] ? 0 : 1);
  CHECK_EQ(wl_pass(&g, 1), WL_E_OVER);
  CHECK_EQ(wl_resign(&g, 1), WL_E_OVER);
}

void test_wordloom_a_weave_resets_the_scoreless_count(void) {
  with_racks("crowbe", "aeiost");
  wl_pass(&g, 0);
  wl_pass(&g, 1);
  wl_pass(&g, 0);
  CHECK_EQ(g.stall, 3);
  CHECK_EQ(wl_play(&g, 1, 5, 5, WL_ACROSS, "at", 0, 0) > 0, 1);
  CHECK_EQ(g.stall, 0);
  CHECK_EQ(g.over, WL_PLAYING);
}

void test_wordloom_tying_off_ends_it(void) {
  with_racks("ab", "zzq");
  g.nbag = 0;
  /* a at the centre (knot, 2) and b: "ab" = 2 + 4 = 6, then the tie-off's
   * 10; the other loses z9 z9 q9. */
  CHECK_EQ(wl_play(&g, 0, 5, 5, WL_ACROSS, "ab", 0, 0), 6);
  CHECK_EQ(g.over, WL_ENDED);
  CHECK_EQ(g.score[0], 6 + WL_TIEOFF);
  CHECK_EQ(g.score[1], -27);
  CHECK_EQ(g.winner, 0);
}

void test_wordloom_a_draw_and_a_resignation(void) {
  with_racks("ab", "ab");
  g.nbag = 0;
  wl_pass(&g, 0); wl_pass(&g, 1); wl_pass(&g, 0); wl_pass(&g, 1);
  CHECK_EQ(g.over, WL_ENDED);
  CHECK_EQ(g.winner, -1);
  /* Resigning on the other's turn, while ahead: the other wins anyway. */
  with_racks("crowbe", "aeiost");
  wl_play(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0);
  CHECK_EQ(g.turn, 1);
  CHECK_EQ(wl_resign(&g, 0), WL_OK);
  CHECK_EQ(g.over, WL_RESIGNED);
  CHECK_EQ(g.winner, 1);
  CHECK_EQ(g.last_kind, WL_K_RESIGN);
}

void test_wordloom_the_wire_format(void) {
  char buf[64];
  uint32_t seed;
  CHECK_EQ(wl_fmt_play(buf, sizeof buf, 5, 4, WL_ACROSS, "crow"), 15);
  CHECK(!strcmp(buf, "play 5 4 a crow"));
  wl_fmt_play(buf, sizeof buf, 10, 0, WL_DOWN, "qi");
  CHECK(!strcmp(buf, "play 10 0 d qi"));
  wl_state_format(buf, sizeof buf, 4000000000u);
  CHECK(!strcmp(buf, "wordloom 1\nseed 4000000000\n"));
  CHECK_EQ(wl_state_parse(buf, &seed), 1);
  CHECK(seed == 4000000000u);
  CHECK_EQ(wl_state_parse("something else\n", &seed), -1);

  with_racks("crowbe", "aeiost");
  CHECK_EQ(wl_apply(&g, 0, "play 5 4 a crow\n", 0, 0), WL_OK);
  CHECK_EQ(g.score[0], 10);
  CHECK_EQ(wl_apply(&g, 1, "play 4 6 d te", 0, 0), WL_OK);
  CHECK_EQ(g.score[1], 7);
  CHECK_EQ(wl_apply(&g, 0, "play 4 6 x te", 0, 0), WL_E_PARSE);
  CHECK_EQ(wl_apply(&g, 0, "play a 6 d te", 0, 0), WL_E_PARSE);
  CHECK_EQ(wl_apply(&g, 0, "play 4", 0, 0), WL_E_PARSE);
  CHECK_EQ(wl_apply(&g, 0, "dance", 0, 0), WL_E_PARSE);
  CHECK_EQ(wl_apply(&g, 0, "", 0, 0), WL_E_PARSE);
  CHECK_EQ(wl_apply(&g, 0, "play 4 6 d abcdefghijklmnopq", 0, 0), WL_E_RACK);
  CHECK_EQ(wl_apply(&g, 1, "pass", 0, 0), WL_E_TURN);
  CHECK_EQ(wl_apply(&g, 0, "pass", 0, 0), WL_OK);
  CHECK_EQ(wl_apply(&g, 1, "swap zzzzzzz", 0, 0), WL_E_RACK);
  CHECK_EQ(wl_apply(&g, 0, "resign", 0, 0), WL_OK);
  CHECK_EQ(g.winner, 1);
}

void test_wordloom_replay_is_the_game_and_skips_what_does_not_fit(void) {
  WlGame a, b;
  char first[3], log[400], mv[32];
  int at;
  wl_new(&a, 2024);
  first[0] = a.rack[0][0]; first[1] = a.rack[0][1]; first[2] = 0;
  /* Step by step. */
  wl_fmt_play(mv, sizeof mv, 5, 5, WL_ACROSS, first);
  CHECK_EQ(wl_apply(&a, 0, mv, 0, 0), WL_OK);
  CHECK_EQ(wl_apply(&a, 1, "pass", 0, 0), WL_OK);
  {
    char one[2];
    one[0] = a.rack[0][0]; one[1] = 0;
    CHECK_EQ(wl_swap(&a, 0, one), WL_OK);
    /* The log: an out-of-turn move, a stranger, then the real ones. */
    at = 0;
    at += sprintf(log + at, "1\tsam\tpass\n");
    at += sprintf(log + at, "2\tmallory\tresign\n");
    at += sprintf(log + at, "3\talex\t%s\n", mv);
    at += sprintf(log + at, "4\tsam\tpass\n");
    at += sprintf(log + at, "5\tsam\tpass\n");             /* twice: skipped */
    at += sprintf(log + at, "6\talex\tswap %s\n", one);
    at += sprintf(log + at, "7\talex\tplay 0 0 a zzz\n");  /* refused */
  }
  CHECK_EQ(wl_replay(&b, 2024, "alex", "sam", log), 4);
  CHECK(same_game(&a, &b));
  CHECK_EQ(b.turn, 1);
  /* An empty log is the deal. */
  CHECK_EQ(wl_replay(&b, 2024, "alex", "sam", ""), 0);
  wl_new(&a, 2024);
  CHECK(same_game(&a, &b));
}

void test_wordloom_tiles_on_the_screen_become_a_move(void) {
  int8_t r[4], c[4];
  char l[5], letters[8];
  int R, C, D;
  with_racks("crowbe", "aeiost");
  wl_play(&g, 0, 5, 4, WL_ACROSS, "crow", 0, 0);
  /* Down through the o, typed bottom first. */
  r[0] = 6; c[0] = 6; l[0] = 'e';
  r[1] = 4; c[1] = 6; l[1] = 't';
  CHECK_EQ(wl_from_cells(&g, r, c, l, 2, WL_ACROSS, &R, &C, &D, letters), WL_OK);
  CHECK_EQ(R, 4); CHECK_EQ(C, 6); CHECK_EQ(D, WL_DOWN);
  CHECK(!strcmp(letters, "te"));
  /* A gap of an empty cell. */
  r[1] = 3;
  CHECK_EQ(wl_from_cells(&g, r, c, l, 2, WL_ACROSS, &R, &C, &D, letters), WL_E_GAP);
  /* Not in a line. */
  r[1] = 4; c[1] = 7;
  CHECK_EQ(wl_from_cells(&g, r, c, l, 2, WL_ACROSS, &R, &C, &D, letters), WL_E_LINE);
  CHECK_EQ(wl_from_cells(&g, r, c, l, 0, WL_ACROSS, &R, &C, &D, letters), WL_E_NONE);
  /* One tile: the way it has a neighbour, whatever was preferred. */
  r[0] = 5; c[0] = 8; l[0] = 's';
  CHECK_EQ(wl_from_cells(&g, r, c, l, 1, WL_DOWN, &R, &C, &D, letters), WL_OK);
  CHECK_EQ(D, WL_ACROSS);
  r[0] = 6; c[0] = 4;
  CHECK_EQ(wl_from_cells(&g, r, c, l, 1, WL_ACROSS, &R, &C, &D, letters), WL_OK);
  CHECK_EQ(D, WL_DOWN);
  /* Across, three typed out of order. */
  r[0] = 6; c[0] = 6; l[0] = 'c';
  r[1] = 6; c[1] = 4; l[1] = 'a';
  r[2] = 6; c[2] = 5; l[2] = 'b';
  CHECK_EQ(wl_from_cells(&g, r, c, l, 3, WL_DOWN, &R, &C, &D, letters), WL_OK);
  CHECK_EQ(R, 6); CHECK_EQ(C, 4); CHECK_EQ(D, WL_ACROSS);
  CHECK(!strcmp(letters, "abc"));
}

void test_wordloom_every_refusal_has_words(void) {
  int e;
  for (e = WL_E_PARSE; e <= WL_E_OVER; e++) CHECK(wl_why(e)[0] != 0);
  CHECK(wl_why(WL_OK)[0] == 0);
}
