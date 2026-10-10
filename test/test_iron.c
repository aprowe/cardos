/* Iron, the app, on the host: a run from the title through a garment to its
 * result card and the next, the leave question, and the scores file.
 *
 * IRON_DUMP=dir writes frames as PPM (the title, the level card, the board
 * cold, mid-stroke and nearly flat, the result card, the run over), since
 * there is no other way to see the drawing off the device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "jarfake.h"

#define capp_info iron_capp_info
#define capp_main iron_capp_main
#include "apps/iron.c"
#undef capp_info
#undef capp_main

static CardApi A;
static const CappUi *IUI;

static int f_ui(const CappUi *ui) { IUI = ui; return 0; }
static void f_text_font(int f, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  (void)f;
  jf_text(x, y, s, fg, bg);
}
static void f_paint_direct(int on) { (void)on; }

static void paint(void) {
  IUI->paint(0, capp_rect(0, 0, 240, 135));
}

static void ticks(int ms) {
  while (ms > 0) {
    fakeapi_ticks += 20;
    if (IUI->tick(0, fakeapi_ticks)) paint();
    ms -= 20;
  }
}

static void key(uint8_t k) {
  fakeapi_repeat = 0;
  IUI->key(0, k);
  paint();
}

static void key_rep(uint8_t k) {
  fakeapi_repeat = 1;
  IUI->key(0, k);
  fakeapi_repeat = 0;
}

static void dump(const char *name) {
  const char *dir = getenv("IRON_DUMP");
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
  A.paint_direct = f_paint_direct;
  A.audio = 0;
  fakeapi_ticks = 1000;
  memset(JF_FB, 0, sizeof JF_FB);
  iron_capp_main(&A, 0, 0);
  paint();
}

/* Space, then an arrow held for `ms`, as the keyboard sends it. */
static void stroke(uint8_t arrow, int ms) {
  int t;
  key_rep(' ');
  IUI->key(0, ' ');
  ticks(40);
  IUI->key(0, arrow);
  for (t = 0; t < ms; t += 60) {
    ticks(60);
    if (t >= 400) key_rep(arrow);
  }
}

void test_iron_app_plays_a_garment_through(void) {
  int i, band;
  start();
  CHECK_EQ(S.screen, SC_TITLE);
  dump("i01_title");
  key(CAPP_KEY_ENTER);
  CHECK_EQ(S.screen, SC_INTRO);
  for (i = 0; i < 2; i++) key('=');               /* hotter than the start */
  ticks(3000);
  dump("i02_level_card");
  while (game.dial != iron_ideal_dial(&game)) key(game.dial > iron_ideal_dial(&game) ? '-' : '=');
  ticks(3000);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(S.screen, SC_PLAY);
  ticks(200);
  dump("i03_board");
  for (band = 0; band < 14 && S.screen == SC_PLAY; band++) {
    int y = game.lv.by + 1 + (band * 3) % (game.lv.h - 1);
    game.x = (band & 1 ? game.lv.bx + game.lv.w - 1 : game.lv.bx) * 256 + 128;
    game.y = y * 256 + 128;
    stroke(band & 1 ? CAPP_KEY_LEFT : CAPP_KEY_RIGHT, game.lv.w * 100 + 200);
    if (band == 1) dump("i04_mid_stroke");
    ticks(500);
    if (band == 5) dump("i05_half_done");
  }
  if (S.screen == SC_PLAY) key(CAPP_KEY_ENTER);  /* past the mark: done */
  CHECK_EQ(S.screen, SC_DONE);
  ticks(1000);
  dump("i06_folding");
  ticks(3000);
  dump("i07_result");
  CHECK(S.best > 0);
  CHECK(strstr(fakefs_get("/config/iron.txt"), "best=") != 0);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(S.screen, SC_INTRO);
  CHECK_EQ(game.level, 2);
  dump("i08_level_2");
  /* Esc asks; n keeps going; Esc, y leaves */
  key(CAPP_KEY_ENTER);
  ticks(200);
  key(CAPP_KEY_ESC);
  CHECK_EQ(S.screen, SC_LEAVE);
  key('n');
  CHECK_EQ(S.screen, SC_PLAY);
  key(CAPP_KEY_ESC);
  key('y');
  CHECK_EQ(S.screen, SC_OVER);
  dump("i09_over");
}

void test_iron_app_burns_the_towel(void) {
  int i;
  start();
  key(CAPP_KEY_ENTER);
  for (i = 0; i < 5; i++) key('=');
  ticks(6000);
  key(CAPP_KEY_ENTER);
  game.x = (game.lv.bx + 6) * 256 + 128;
  game.y = (game.lv.by + 6) * 256 + 128;
  for (i = 0; i < 40 && game.marks == 0; i++) { key_rep(' '); ticks(100); }
  CHECK(game.marks > 0);
  ticks(60);
  dump("i10_scorch");
  for (i = 0; i < 300 && S.screen == SC_PLAY; i++) { key_rep(' '); ticks(100); }
  CHECK_EQ(S.screen, SC_OVER);
  CHECK_EQ(game.why, IR_WHY_BURNT);
  dump("i11_burnt");
}

void test_iron_app_draws_each_garment(void) {
  start();
  key(CAPP_KEY_ENTER);
  key(CAPP_KEY_ENTER);
  ticks(100);
  dump("i12_towel");
  iron_next_garment(&game);
  go(SC_INTRO, fakeapi_ticks);
  go(SC_PLAY, fakeapi_ticks);
  paint();
  CHECK_EQ(game.lv.kind, IR_SHIRT);
  dump("i13_shirt");
  iron_next_garment(&game);
  go(SC_INTRO, fakeapi_ticks);
  go(SC_PLAY, fakeapi_ticks);
  paint();
  CHECK_EQ(game.lv.kind, IR_SHEET);
  dump("i14_sheet");
}
