/* Jar Factory, the app, on the host: the first run, buying, placing, a
 * restart with time away, the bars, and what the built-in items encode to.
 *
 * JAR_DUMP=dir writes frames as PPM -- the jar by day and night, busy, the
 * upgrades, the shop, an item card, decorating -- since there is no other
 * way to see the art off the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define capp_info jar_capp_info
#define capp_main jar_capp_main
#include "apps/jar.c"
#undef capp_info
#undef capp_main

#define T0 1800000000u

static CardApi A;
static uint16_t FB[SHT][SW];
static int BLITS;

static void f_pixels(CRect r, const uint16_t *p) {
  int x, y;
  BLITS++;
  for (y = 0; y < r.h; y++)
    for (x = 0; x < r.w; x++)
      if (r.x + x < SW && r.y + y < SHT) FB[r.y + y][r.x + x] = p[y * r.w + x];
}

/* A fresh card and a fresh app. */
static void start(uint32_t epoch) {
  fakeapi_init(&A);
  fakefs_mem(&A);
  A.pixels = f_pixels;
  fakeapi_epoch = epoch;
  fakeapi_ticks = 1000;
  jar_capp_main(&A, 0, 0);
}

static void ticks(int ms) {
  int t;
  for (t = 0; t < ms; t += 5) {
    fakeapi_ticks += 5;
    app_tick(0, fakeapi_ticks);
  }
}

static void paint(void) {
  CRect all = { 0, 0, SW, SHT };
  G.asked = 0;
  app_paint(0, all);
}

static void key(int k) { app_key(0, (unsigned char)k); }

static void dump(const char *name) {
  const char *dir = getenv("JAR_DUMP");
  char path[256];
  FILE *f;
  int x, y;
  paint();
  if (!dir) return;
  snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", SW, SHT);
  for (y = 0; y < SHT; y++)
    for (x = 0; x < SW; x++) {
      uint16_t c = swap16(FB[y][x]);
      unsigned char rgb3[3];
      rgb3[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
      rgb3[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
      rgb3[2] = (unsigned char)((c & 31) * 255 / 31);
      fwrite(rgb3, 1, 3, f);
    }
  fclose(f);
}

void test_jar_builtins_are_valid_records(void) {
  static JItem b;
  uint8_t buf[JI_MAX];
  int i, n, starters = 0, stock = 0;
  fakeapi_init(&A);
  api = &A;
  for (i = 0; i < JB_COUNT; i++) {
    from_builtin(&BUILTINS[i], &SCR.io.it);
    n = jitem_encode(&SCR.io.it, buf, sizeof buf);
    CHECK(n > JI_HDR && n <= JI_MAX);
    CHECK_EQ(jitem_decode(&b, buf, n), 0);
    CHECK_EQ(b.id, BUILTINS[i].id);
    CHECK(b.flags & JIF_BUILTIN);
    CHECK_EQ(b.sig_len, 0);                 /* built in: trusted, unsigned */
    CHECK_EQ(b.nhab, BUILTINS[i].nhab);
    CHECK(b.name[0] != 0 && b.line[0] != 0);
    CHECK(strcmp(b.maker, "Alex") == 0);
    CHECK(b.id < 32);                       /* the sold bits */
    if (BUILTINS[i].price) {
      stock++;
      CHECK(BUILTINS[i].price >= 45 && BUILTINS[i].price <= 200);
    } else starters++;
  }
  CHECK_EQ(JB_COUNT, 12);
  CHECK_EQ(starters, 4);
  CHECK_EQ(stock, 8);
}

void test_jar_first_run_writes_and_places_the_starters(void) {
  start(T0);
  CHECK(fakefs_exists("/var/jar/jar.txt"));
  CHECK(fakefs_exists("/var/jar/items/1.itm"));
  CHECK(fakefs_exists("/var/jar/items/4.itm"));
  CHECK(!fakefs_exists("/var/jar/items/5.itm"));
  CHECK_EQ(J.nowned, 4);
  CHECK_EQ(J.nplaced, 3);
  CHECK_EQ(J.coins, 0);
  CHECK_EQ(G.view, V_JAR);
  dump("01_first");
}

void test_jar_buy_and_place(void) {
  const char *sv;
  start(T0);
  J.coins = 100;
  key('s');
  CHECK_EQ(G.view, V_SHOP);
  CHECK_EQ(G.tab, 0);
  /* the first tile is the frog, 60 */
  CHECK_EQ(G.card.id, 5);
  CHECK_EQ(G.card.price, 60);
  dump("05_shop");
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_CARD);
  CHECK_EQ(J.coins, 40);
  CHECK(J.sold & (1u << 5));
  CHECK(js_owns(&J, 5));
  CHECK(fakefs_exists("/var/jar/items/5.itm"));
  dump("06_card");
  /* bought once: Enter on the sold tile does nothing */
  key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_SHOP);
  CHECK(!G.card.ok);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(J.coins, 40);
  /* the owl at 150 is too dear: nothing happens */
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);
  CHECK_EQ(G.card.id, 10);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_SHOP);
  CHECK_EQ(J.coins, 40);
  /* My Stuff, then the frog's card, then into the jar */
  key(0x09);
  CHECK_EQ(G.tab, 1);
  dump("07_stuff");
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);
  CHECK_EQ(G.card.id, 5);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_CARD);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_DECOR);
  CHECK(G.dmove);
  CHECK_EQ(J.nplaced, 4);
  key(CAPP_KEY_LEFT); key(CAPP_KEY_LEFT);
  CHECK_EQ(J.placed[G.dsel].home_x, 118);
  dump("08_decorate");
  key(CAPP_KEY_ENTER);
  CHECK(!G.dmove);
  sv = fakefs_get("/var/jar/jar.txt");
  CHECK(sv && strstr(sv, "place 5 118 0"));
  CHECK(sv && strstr(sv, "coins 40"));
  /* take one out: back to My Stuff, still owned */
  key(CAPP_KEY_LEFT);
  {
    uint32_t id = J.placed[G.dsel].id;
    key('x');
    CHECK_EQ(J.nplaced, 3);
    CHECK(js_owns(&J, id));
    CHECK_EQ(js_find(&J, id), -1);
  }
  key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_JAR);
}

void test_jar_restart_brings_back_the_jar_and_pays_for_time_away(void) {
  uint32_t coins, owned, placed;
  int32_t rate;
  start(T0);
  J.coins = 77;
  J.nbeds = 3;
  save();
  coins = J.coins; owned = J.nowned; placed = J.nplaced;
  rate = js_rate_ph(&J);
  /* Opened again an hour later, on the same card. */
  memset(&J, 0, sizeof J);
  memset(&G, 0, sizeof G);
  fakeapi_epoch = T0 + 3600;
  jar_capp_main(&A, 0, 0);
  CHECK_EQ(J.coins, coins);
  CHECK_EQ(J.nowned, owned);
  CHECK_EQ(J.nplaced, placed);
  CHECK_EQ(J.nbeds, 3);
  CHECK_EQ(J.dock, (uint32_t)rate);
  CHECK(G.msg[0] != 0);                   /* "While you were away: ..." */
  dump("02_away");
  /* it ships quickly, a coin a jar */
  ticks(60000);
  CHECK(J.coins >= coins + (uint32_t)rate * 9 / 10);
  CHECK_EQ(J.coins - coins, J.shipped);
}

void test_jar_no_clock_still_runs(void) {
  start(0);
  fakeapi_now.synced = 0;
  ticks(2000);
  CHECK_EQ(J.phase, PH_NONE);
  CHECK_EQ(J.seen, 0);
  ticks(60000);
  CHECK(J.steps > 2000);
  fakeapi_now.synced = 2;
}

void test_jar_bars_slide_away_and_a_key_only_wakes_them(void) {
  start(T0);
  ticks(9000);
  CHECK_EQ(G.bars, 0);
  ticks(2000);
  CHECK_EQ(G.bars, BAR);                  /* gone, and the jar has the screen */
  dump("03_full");
  key('s');                               /* would open the shop... */
  CHECK_EQ(G.view, V_JAR);                /* ...but only brings the bars back */
  ticks(400);
  CHECK_EQ(G.bars, 0);
  key('s');
  CHECK_EQ(G.view, V_SHOP);
  ticks(20000);
  CHECK_EQ(G.bars, 0);                    /* menus keep their bars */
}

void test_jar_upgrades_screen_buys(void) {
  start(T0);
  J.coins = 500;
  key('u');
  CHECK_EQ(G.view, V_UP);
  dump("09_upgrades");
  key(CAPP_KEY_ENTER);                    /* another mossling: 30 */
  CHECK_EQ(J.nmoss, 2);
  CHECK_EQ(J.coins, 470);
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);                    /* the press: 120 */
  CHECK_EQ(J.nmach, 3);
  CHECK(strstr(fakefs_get("/var/jar/jar.txt"), "up 2 2 3 0") != 0);
  J.coins = 0;
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(J.belt, 0);                    /* not enough */
  CHECK(G.msg[0] != 0);
}

void test_jar_frames(void) {
  int i;
  start(T0);
  /* a busy afternoon: everything bought, a few things placed */
  J.coins = 5000;
  for (i = 0; i < 6; i++) { js_buy(&J, JU_MOSS); js_buy(&J, JU_BED); js_buy(&J, JU_MACH); js_buy(&J, JU_BELT); }
  for (i = 0; i < JB_COUNT; i++) {
    if (!BUILTINS[i].price) continue;
    from_builtin(&BUILTINS[i], &SCR.io.it);
    item_write(&SCR.io.it);
    js_own(&J, BUILTINS[i].id);
  }
  place_new(5); key(CAPP_KEY_ENTER);               /* frog, by the pond */
  js_move_to(&J, js_find(&J, 5), 86, 0);
  place_new(9); key(CAPP_KEY_ENTER);               /* the moon */
  js_move_to(&J, js_find(&J, 9), 100, 4);
  place_new(12); key(CAPP_KEY_ENTER);              /* a jelly */
  place_new(7); key(CAPP_KEY_ENTER);               /* the shell house */
  js_move_to(&J, js_find(&J, 7), 222, 0);
  place_new(8); key(CAPP_KEY_ENTER);               /* a bee */
  key(CAPP_KEY_ESC);
  ticks(90000);
  dump("10_busy_day");
  ticks(1500);
  dump("11_busy_day_b");
  /* the same jar at night */
  fakeapi_now.hour = 22;
  ticks(1500);
  dump("12_night");
  fakeapi_now.hour = 6;
  fakeapi_now.min = 20;
  ticks(1500);
  dump("13_dawn");
  fakeapi_now.hour = 18;
  fakeapi_now.min = 0;
  ticks(1500);
  dump("14_dusk");
  fakeapi_now.hour = 12;
  fakeapi_now.min = 0;
  /* a jam */
  J.jam_clock = 1;
  ticks(4000);
  dump("15_jam");
  key('d');
  dump("16_decorate");
  CHECK(BLITS > 0);
}
