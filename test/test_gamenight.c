/* Game Night on the host: the scoreboard and the dice as they are, and the
 * app through its four tools. GN_DUMP=dir writes frames as PPM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "jarfake.h"

#define capp_info gn_capp_info
#define capp_main gn_capp_main
#include "apps/gamenight.c"
#undef capp_info
#undef capp_main

/* ---- the scoreboard ------------------------------------------------------------ */

void test_gn_score_rounds_totals_and_leaders(void) {
  static GScore g;
  gs_init(&g);
  CHECK_EQ(gs_add_player(&g, "  Alice "), 0);
  CHECK_EQ(gs_add_player(&g, "Bob"), 1);
  CHECK_EQ(gs_add_player(&g, "Charlotte the Great"), 2);
  CHECK(strcmp(g.name[2], "Charlotte ") != 0);           /* trimmed after the cut */
  CHECK_EQ((int)strlen(g.name[2]), 9);
  CHECK_EQ(gs_add_player(&g, "   "), -1);
  CHECK_EQ(gs_leaders(&g), 0u);                          /* nothing scored: no leader */
  CHECK_EQ(gs_set(&g, 0, 0, 10), 0);
  CHECK_EQ(g.nr, 1);
  CHECK_EQ(gs_set(&g, 2, 0, 5), -1);                     /* not past the next round */
  CHECK_EQ(gs_set(&g, 0, 1, -3), 0);
  CHECK_EQ(gs_set(&g, 1, 1, 20), 0);
  CHECK_EQ(gs_set(&g, 1, 2, 10000), -1);                 /* too big */
  CHECK_EQ(gs_total(&g, 0), 10);
  CHECK_EQ(gs_total(&g, 1), 17);
  CHECK_EQ(gs_total(&g, 2), 0);
  CHECK_EQ(gs_leaders(&g), 2u);                          /* Bob */
  g.low = 1;
  CHECK_EQ(gs_leaders(&g), 4u);                          /* low wins: Charlotte's 0 */
  g.low = 0;
  gs_set(&g, 1, 0, 7);
  CHECK_EQ(gs_leaders(&g), 3u);                          /* a tie: both */
  gs_del_round(&g, 0);
  CHECK_EQ(g.nr, 1);
  CHECK_EQ(gs_total(&g, 1), 20);
  gs_clear(&g, 0, 1);
  CHECK_EQ(gs_total(&g, 1), 0);
  g.first = 2;
  gs_del_player(&g, 0);
  CHECK_EQ(g.np, 2);
  CHECK_EQ(g.first, 1);                                  /* moved with its column */
  CHECK(strcmp(g.name[0], "Bob") == 0);
  gs_new_game(&g);
  CHECK_EQ(g.nr, 0);
  CHECK_EQ(g.np, 2);
  CHECK_EQ(g.first, -1);
}

void test_gn_score_saves_and_loads(void) {
  static GScore g, h;
  static char buf[4096];
  gs_init(&g);
  gs_add_player(&g, "Red team");
  gs_add_player(&g, "Blue");
  gs_add_player(&g, "Green");
  gs_set(&g, 0, 0, 12);
  gs_set(&g, 0, 2, -40);
  gs_set(&g, 1, 1, 7);
  g.first = 1;
  g.low = 1;
  gs_save(&g, buf, sizeof buf);
  CHECK(strstr(buf, "round 12 _ -40\n") != 0);
  CHECK(strstr(buf, "player Red team\n") != 0);
  gs_load(&h, buf);
  CHECK_EQ(h.np, 3);
  CHECK_EQ(h.nr, 2);
  CHECK_EQ(h.cell[0][2], -40);
  CHECK_EQ(h.cell[0][1], GS_EMPTY);
  CHECK_EQ(h.cell[1][1], 7);
  CHECK_EQ(h.first, 1);
  CHECK_EQ(h.low, 1);
  /* hand-edited: what it understands, kept */
  gs_load(&h, "player A\r\nplayer B\nround 3 x\nround 1 2 3\nnonsense\nfirst 9\n");
  CHECK_EQ(h.np, 2);
  CHECK_EQ(h.nr, 2);
  CHECK_EQ(h.cell[0][0], 3);
  CHECK_EQ(h.cell[1][1], 2);
  CHECK_EQ(h.first, -1);                                 /* no ninth player */
}

void test_gn_score_pick_first_is_fair(void) {
  static GScore g;
  int counts[4] = { 0 }, i;
  uint32_t x = 12345;
  gs_init(&g);
  for (i = 0; i < 4; i++) gs_add_player(&g, "P");
  for (i = 0; i < 4000; i++) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    counts[gs_pick_first(&g, x)]++;
  }
  for (i = 0; i < 4; i++) CHECK(counts[i] > 850 && counts[i] < 1150);
}

/* ---- the dice ------------------------------------------------------------------- */

void test_gn_dice_parse_and_name(void) {
  static GDice d;
  char s[16];
  gd_init(&d, 7);
  CHECK_EQ(gd_parse(&d, "3d6+2"), 0);
  CHECK(d.n == 3 && d.sides == 6 && d.mod == 2);
  gd_name(&d, s, sizeof s);
  CHECK(strcmp(s, "3d6+2") == 0);
  CHECK_EQ(gd_parse(&d, "d20"), 0);
  gd_name(&d, s, sizeof s);
  CHECK(strcmp(s, "d20") == 0);
  CHECK_EQ(gd_parse(&d, "D%"), 0);
  CHECK_EQ(d.sides, 100);
  CHECK_EQ(gd_parse(&d, " 2d8 - 1 "), 0);
  CHECK_EQ(d.mod, -1);
  gd_name(&d, s, sizeof s);
  CHECK(strcmp(s, "2d8-1") == 0);
  CHECK_EQ(gd_parse(&d, "12"), 0);                        /* a d12 */
  CHECK(d.n == 1 && d.sides == 12);
  CHECK_EQ(gd_parse(&d, "10d6"), -1);                     /* nine at most */
  CHECK_EQ(gd_parse(&d, "2d1"), -1);
  CHECK_EQ(gd_parse(&d, "2d6+"), -1);
  CHECK_EQ(gd_parse(&d, "cat"), -1);
  CHECK(d.n == 1 && d.sides == 12);                       /* a refusal changes nothing */
}

void test_gn_dice_roll_hold_and_spread(void) {
  static GDice d;
  int counts[7] = { 0 }, i, keep;
  gd_init(&d, 99);
  gd_parse(&d, "2d6");
  CHECK(!gd_rolled(&d));
  gd_roll(&d);
  CHECK(gd_rolled(&d));
  CHECK(d.v[0] >= 1 && d.v[0] <= 6);
  keep = d.v[0];
  d.hold[0] = 1;
  for (i = 0; i < 50; i++) { gd_roll(&d); CHECK_EQ(d.v[0], keep); }
  gd_unhold(&d);
  gd_parse(&d, "1d6");
  for (i = 0; i < 6000; i++) { gd_roll(&d); counts[d.v[0]]++; }
  for (i = 1; i <= 6; i++) CHECK(counts[i] > 850 && counts[i] < 1150);
  gd_parse(&d, "3d4+5");
  gd_roll(&d);
  CHECK_EQ(gd_total(&d), d.v[0] + d.v[1] + d.v[2] + 5);
  gd_step_sides(&d, 1);
  CHECK_EQ(d.sides, 6);
  gd_step_sides(&d, -1);
  gd_step_sides(&d, -1);
  CHECK_EQ(d.sides, 2);
  gd_step_sides(&d, -1);
  CHECK_EQ(d.sides, 100);                                 /* round */
  gd_parse(&d, "d7");
  gd_step_sides(&d, 1);
  CHECK_EQ(d.sides, 8);
  gd_parse(&d, "d7");
  gd_step_sides(&d, -1);
  CHECK_EQ(d.sides, 6);
  gd_set_n(&d, 30);
  CHECK_EQ(d.n, GD_MAXN);
}

/* ---- the app --------------------------------------------------------------------- */

static CardApi A;
static const CappUi *GUI;
static int MOTION;

static int f_ui(const CappUi *ui) { GUI = ui; return 0; }
static void f_text_font(int f, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  (void)f;
  jf_text(x, y, s, fg, bg);
}
static int f_motion(CappMotion *m) {
  memset(m, 0, sizeof *m);
  m->az = 1000;
  if (MOTION) { m->ax = 2500; MOTION = 0; }
  return 0;
}

static void paint(void) { GUI->paint(0, capp_rect(0, 0, 240, 135)); }

static void ticks(int ms) {
  while (ms > 0) {
    fakeapi_ticks += 20;
    if (GUI->tick(0, fakeapi_ticks)) paint();
    ms -= 20;
  }
}

static void key(int k) {
  fakeapi_repeat = 0;
  GUI->key(0, (uint8_t)k);
  paint();
}

static void type(const char *s) { while (*s) key(*s++); }

static void dump(const char *name) {
  const char *dir = getenv("GN_DUMP");
  char path[256];
  FILE *f;
  int x, y;
  if (!dir) return;
  snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n240 135\n255\n");
  for (y = 0; y < 135; y++)
    for (x = 0; x < 240; x++) {
      uint16_t c = (uint16_t)((JF_FB[y][x] >> 8) | (JF_FB[y][x] << 8));
      unsigned char p[3];
      p[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
      p[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
      p[2] = (unsigned char)((c & 31) * 255 / 31);
      fwrite(p, 1, 3, f);
    }
  fclose(f);
}

static void start(void) {
  fakeapi_init(&A);
  fakefs_mem(&A);
  jf_screen(&A);
  A.text_font = f_text_font;
  A.ui = f_ui;
  A.audio = 0;
  A.motion = f_motion;
  fakeapi_ticks = 1000;
  memset(JF_FB, 0, sizeof JF_FB);
  gn_capp_main(&A, 0, 0);
  paint();
}

void test_gn_app_keeps_score(void) {
  start();
  dump("g01_menu");
  key('1');
  CHECK_EQ(S.screen, SC_PLAYERS);
  CHECK(GUI->wants_text(0));
  type("Alex"); key(CAPP_KEY_ENTER);
  type("Britney"); key(CAPP_KEY_ENTER);
  type("Sam"); key(CAPP_KEY_ENTER);
  type("Kit"); key(CAPP_KEY_ENTER);
  dump("g02_players");
  CHECK_EQ(G.np, 4);
  key(CAPP_KEY_ENTER);                                    /* done */
  CHECK_EQ(S.screen, SC_SCORE);
  CHECK(!GUI->wants_text(0));
  /* round 1: 5, -3, 12, 0 */
  type("5"); key(CAPP_KEY_ENTER);
  type("-3"); key(CAPP_KEY_ENTER);
  type("12"); key(CAPP_KEY_ENTER);
  type("0"); key(CAPP_KEY_ENTER);
  CHECK_EQ(G.nr, 1);
  CHECK_EQ(S.row, 1);
  CHECK_EQ(S.col, 0);
  type("8"); key(CAPP_KEY_ENTER);
  type("26"); key(CAPP_KEY_ENTER);
  type("1"); key(CAPP_KEY_ENTER);
  type("3");
  dump("g03_typing");
  key(CAPP_KEY_ENTER);
  CHECK_EQ(gs_total(&G, 0), 13);
  CHECK_EQ(gs_total(&G, 1), 23);
  CHECK_EQ(gs_leaders(&G), 2u);
  key('f');                                               /* who goes first: a spin */
  CHECK(S.spin > 0);
  ticks(5000);
  CHECK_EQ(S.spin, 0);
  CHECK(G.first >= 0 && G.first < 4);
  dump("g04_board");
  key('l');                                               /* low wins: Kit */
  CHECK_EQ(gs_leaders(&G), 8u);
  dump("g05_low_wins");
  /* saved, and back after a restart */
  ticks(1200);
  CHECK(strstr(fakefs_get("/var/gamenight/score.txt"), "player Britney\n") != 0);
  memset(&G, 0, sizeof G);
  gn_capp_main(&A, 0, 0);
  CHECK_EQ(G.np, 4);
  CHECK_EQ(gs_total(&G, 1), 23);
  CHECK_EQ(G.low, 1);
  /* delete a round, asked first */
  key('1');
  key(CAPP_KEY_UP);
  key('d');
  CHECK_EQ(S.ask, ASK_DEL_ROUND);
  key('n');
  CHECK_EQ(G.nr, 2);
  key('d');
  key('y');
  CHECK_EQ(G.nr, 1);
  /* rename from the names' row */
  key(CAPP_KEY_UP); key(CAPP_KEY_UP);
  CHECK_EQ(S.row, -1);
  key('e');
  CHECK(GUI->wants_text(0));
  key(CAPP_KEY_BACK); key(CAPP_KEY_BACK); key(CAPP_KEY_BACK); key(CAPP_KEY_BACK);
  type("Al");
  key(CAPP_KEY_ENTER);
  CHECK(strcmp(G.name[S.col], "Al") == 0);
  key(CAPP_KEY_ESC);
  CHECK_EQ(S.screen, SC_HOME);
}

void test_gn_app_rolls_dice(void) {
  int i;
  start();
  key('2');
  CHECK_EQ(S.screen, SC_DICE);
  dump("g06_dice_unrolled");
  key(' ');
  CHECK(S.rolling);
  ticks(300);
  dump("g07_dice_rolling");
  ticks(600);
  CHECK(!S.rolling);
  CHECK(gd_rolled(&D));
  dump("g08_dice_2d6");
  key('1');                                               /* hold the first */
  CHECK_EQ(D.hold[0], 1);
  i = D.v[0];
  key(' ');
  ticks(900);
  CHECK_EQ(D.v[0], i);
  for (i = 0; i < 3; i++) key(CAPP_KEY_UP);
  CHECK_EQ(D.n, 5);
  key(' ');
  ticks(900);
  dump("g09_dice_5d6");
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);               /* d10 */
  CHECK_EQ(D.sides, 10);
  key(' ');
  ticks(900);
  dump("g10_dice_5d10");
  key('e');
  CHECK(GUI->wants_text(0));
  type("d%");
  key(CAPP_KEY_ENTER);
  CHECK(D.n == 1 && D.sides == 100);
  MOTION = 1;                                             /* a shake rolls */
  ticks(100);
  CHECK(S.rolling);
  ticks(900);
  dump("g11_dice_d100");
  key('e'); type("2d2"); key(CAPP_KEY_ENTER);
  key(' '); ticks(900);
  dump("g12_coins");
  CHECK(strstr(fakefs_get("/var/gamenight/prefs.txt"), "dice 2d2\n") != 0);
}

void test_gn_app_times_and_buzzes(void) {
  start();
  key('3');
  CHECK_EQ(S.screen, SC_TIMER);
  key(CAPP_KEY_LEFT);                                     /* 45 s */
  CHECK_EQ(S.t_preset, 45);
  dump("g13_timer_set");
  key(' ');
  CHECK(S.t_running);
  ticks(40000);
  dump("g14_timer_low");
  CHECK(S.t_left_ms <= 5000);
  /* on to the buzzer while it runs: it shows the time left */
  key(CAPP_KEY_ESC);
  key('4');
  CHECK_EQ(S.screen, SC_BUZZ);
  key('c'); key('c'); key(' ');
  CHECK_EQ(S.b_correct, 2);
  CHECK_EQ(S.b_taboo, 1);
  dump("g15_buzz");
  ticks(6000);
  CHECK_EQ(S.screen, SC_TIMER);                          /* time's up: shown */
  CHECK(S.t_done);
  dump("g16_times_up");
  key('x');
  CHECK(!S.t_done);
  CHECK_EQ(S.t_left_ms, 45000);
}

void test_gn_commands(void) {
  char out[256];
  const char *argv[1];
  start();
  argv[0] = "3d6+2";
  CHECK_EQ(GUI->command(0, ACT_ROLL, 1, argv, out, sizeof out), 0);
  CHECK(atoi(out) >= 5 && atoi(out) <= 20);
  CHECK(strstr(out, "+2)") != 0);
  argv[0] = "lots";
  CHECK_EQ(GUI->command(0, ACT_ROLL, 1, argv, out, sizeof out), -1);
  CHECK_EQ(GUI->command(0, ACT_SCORES, 0, argv, out, sizeof out), 0);
  CHECK(strstr(out, "no players") != 0);
}
