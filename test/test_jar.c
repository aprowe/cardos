/* Jar Factory, the jar app, on the host: the first run, a restart with time
 * away, the bars, decorate, the keys that open the companions, a gift
 * arriving on its parachute and a thank-you, and the garden in the scene.
 *
 * JAR_DUMP=dir writes frames as PPM -- the jar by day and night, busy, a
 * parcel coming down, decorating -- since there is no other way to see the
 * art off the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "jarfake.h"

/* The scene's own tests ask for the shop or the post and check what was
 * asked (RAN); they stay on the scene. The screens' tests switch for real. */
static int jar_test_open(const char *app, const char *args);
#define JAR_TEST_OPEN(a, b) do { if (jar_test_open((a), (b))) return 0; } while (0)
#define capp_info jar_capp_info
#define capp_main jar_capp_main
#include "apps/jar.c"
#undef capp_info
#undef capp_main

#define T0 1800000000u

static CardApi A;
static uint16_t FB[SHT][SW];
static int BLITS;
static char RAN[64], RAN_ARGS[64];

static void f_pixels(CRect r, const uint16_t *p) {
  int x, y;
  BLITS++;
  for (y = 0; y < r.h; y++)
    for (x = 0; x < r.w; x++)
      if (r.x + x < SW && r.y + y < SHT) FB[r.y + y][r.x + x] = p[y * r.w + x];
}

static int f_run(const char *name, const char *args) {
  snprintf(RAN, sizeof RAN, "%s", name);
  snprintf(RAN_ARGS, sizeof RAN_ARGS, "%s", args ? args : "");
  return 0;
}

static int STAY = 1;                 /* a screen asked for is recorded, not opened */
static int jar_test_open(const char *app, const char *args) {
  if (strcmp(app, "Jar Shop") && strcmp(app, "Jar Post")) return 0;
  f_run(app, args);
  if (STAY) save();                  /* as jar_open does, before a screen */
  return STAY;
}

static int NET_UP;
static int f_net_ready(void) { return NET_UP; }

/* A fresh app on the card as it is. */
static void reopen(void) {
  memset(&J, 0, sizeof J);
  memset(&G, 0, sizeof G);
  jar_capp_main(&A, 0, 0);
}

/* A fresh card and a fresh app. */
static void start(uint32_t epoch) {
  fakeapi_init(&A);
  fakefs_mem(&A);
  jf_server(&A);
  A.pixels = f_pixels;
  A.run = f_run;
  A.net_ready = f_net_ready;
  NET_UP = 0;
  RAN[0] = RAN_ARGS[0] = 0;
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

/* Own every built-in in the stock (as Jar Shop would have sold them). */
static void own_stock(void) {
  int i;
  for (i = 0; i < JB_COUNT; i++) {
    if (!BUILTINS[i].price) continue;
    jst_from_builtin(&BUILTINS[i], &SCR.io.it);
    jst_item_write(&A, &SCR.io);
    js_own(&J, BUILTINS[i].id);
  }
}

void test_jar_builtins_are_valid_records(void) {
  static JItem b;
  uint8_t buf[JI_MAX];
  int i, n, starters = 0, stock = 0;
  for (i = 0; i < JB_COUNT; i++) {
    static JItem it;
    jst_from_builtin(&BUILTINS[i], &it);
    n = jitem_encode(&it, buf, sizeof buf);
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

/* One app: the shop and the post are screens of the jar now. Opening one
 * saves the scene, hands its item memory to the screen and lets the OS
 * compose the screen's paint; Esc comes back to the scene, loaded afresh
 * with what the screen changed. */
void test_jar_the_shop_and_the_post_are_screens_of_one_app(void) {
  uint32_t before;
  int placed;
  start(T0);
  J.coins = 500;
  placed = J.nplaced;
  CHECK(placed >= 3);
  STAY = 0;
  key('s');
  CHECK_EQ(SCREEN, SC_SHOP);
  CHECK_EQ(fakeapi_paint_direct, 0);                 /* the OS composes the shop */
  CHECK_EQ(J.nplaced, 0);                            /* its memory is the shop's now */
  CHECK_EQ(J.nwant, placed);                         /* what is in the jar, as a save says it */
  CHECK((void *)SHOP_P >= (void *)J.pool && (void *)POST_P == (void *)J.placed);
  CHECK_EQ(SHOP_P->s.n, 8);                          /* the hand-made stock, offline */
  CHECK(UI_P->card.ok);
  before = J.coins;
  key(CAPP_KEY_ENTER);                               /* bought */
  CHECK(J.coins < before);
  key(CAPP_KEY_ESC);                                 /* the card */
  key(CAPP_KEY_ESC);                                 /* the jar */
  CHECK_EQ(SCREEN, SC_JAR);
  CHECK_EQ(fakeapi_paint_direct, 1);                 /* the scene paints its own strips */
  CHECK_EQ(J.nplaced, placed);                       /* everything back where it was */
  CHECK(J.coins < before);                           /* and the purchase kept */
  ticks(200);
  key('t');                                          /* Tibbs */
  CHECK_EQ(SCREEN, SC_POST);
  CHECK_EQ(POST_P->g.view, V_TALK);
  key(CAPP_KEY_ESC);
  CHECK_EQ(SCREEN, SC_JAR);
  CHECK_EQ(J.nplaced, placed);
  STAY = 1;
}

void test_jar_keys_open_the_companions_after_saving(void) {
  static const struct { int k; const char *app, *screen; } K[] = {
    { 's', "Jar Shop", "shop" }, { 'p', "Jar Shop", "garden" }, { 't', "Jar Post", "talk" },
    { 'u', "Jar Shop", "up" }, { 'f', "Jar Post", "friends" }, { 'm', "Jar Post", "mail" },
  };
  int i;
  start(T0);
  for (i = 0; i < 6; i++) {
    RAN[0] = 0;
    J.coins = 10 + (uint32_t)i;
    key(K[i].k);
    CHECK(strcmp(RAN, K[i].app) == 0);
    CHECK(strcmp(RAN_ARGS, K[i].screen) == 0);
    {
      char want[32];
      snprintf(want, sizeof want, "coins %d\n", 10 + i);
      CHECK(strstr(fakefs_get("/var/jar/jar.txt"), want) != 0);    /* saved first */
    }
  }
  /* Enter opens the menu: big tiles, arrows choose, Enter opens, Esc closes */
  RAN[0] = 0;
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.menu, 1);
  CHECK_EQ(RAN[0], 0);
  dump("22_menu");
  key(CAPP_KEY_ESC);
  CHECK_EQ(G.menu, 0);
  key(CAPP_KEY_ENTER);
  G.msel = 0;
  key(CAPP_KEY_DOWN);                     /* Shop -> Decorate, a row down */
  CHECK_EQ(G.msel, 2);
  key(CAPP_KEY_RIGHT);                    /* -> Garden */
  CHECK_EQ(G.msel, 3);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.menu, 0);
  CHECK(strcmp(RAN, "Jar Shop") == 0 && strcmp(RAN_ARGS, "garden") == 0);
  RAN[0] = 0;
  key(CAPP_KEY_ENTER);                    /* My Stuff, the inventory */
  G.msel = M_STUFF;
  key(CAPP_KEY_ENTER);
  CHECK(strcmp(RAN, "Jar Shop") == 0 && strcmp(RAN_ARGS, "stuff") == 0);
  RAN[0] = 0;
  key(CAPP_KEY_ENTER);                    /* a letter in the menu works as from the jar */
  key('u');
  CHECK(strcmp(RAN, "Jar Shop") == 0 && strcmp(RAN_ARGS, "up") == 0);
  RAN[0] = 0;
  /* decorate's N picks from My Stuff, in Jar Shop */
  key('d');
  CHECK_EQ(G.view, V_DECOR);
  key('n');
  CHECK(strcmp(RAN, "Jar Shop") == 0 && strcmp(RAN_ARGS, "decor") == 0);
}

void test_jar_places_what_a_companion_asked_for(void) {
  const char *sv;
  start(T0);
  own_stock();
  J.decor = 5;                                   /* Jar Shop: "put the frog in the jar" */
  save();
  reopen();
  CHECK_EQ(G.view, V_DECOR);
  CHECK(G.dmove && G.dnew);
  CHECK_EQ(J.placed[G.dsel].id, 5);
  CHECK_EQ(J.decor, 0);
  key(CAPP_KEY_LEFT); key(CAPP_KEY_LEFT);
  CHECK_EQ(J.placed[G.dsel].home_x, 118);
  dump("08_decorate");
  key(CAPP_KEY_ENTER);
  CHECK(!G.dmove);
  sv = fakefs_get("/var/jar/jar.txt");
  CHECK(sv && strstr(sv, "place 5 118 0"));
  CHECK(sv && !strstr(sv, "decor "));
  /* one already in the jar: decorate, on it */
  J.decor = 1;
  save();
  reopen();
  CHECK_EQ(G.view, V_DECOR);
  CHECK(!G.dmove);
  CHECK_EQ(J.placed[G.dsel].id, 1);
  /* take one out: back to My Stuff, still owned, and it stays out */
  {
    uint32_t id = J.placed[G.dsel].id;
    key('x');
    CHECK_EQ(js_find(&J, id), -1);
    CHECK(js_owns(&J, id));
    reopen();
    CHECK_EQ(js_find(&J, id), -1);
  }
  key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_JAR);
}

/* A save stamped hours ago loads with the same coins and jam: nothing is
 * made while the jar is off screen. Only running the scene makes any. */
void test_jar_restart_brings_back_the_jar_and_makes_nothing_away(void) {
  uint32_t coins, owned, placed, dock, shipped;
  start(T0);
  J.coins = 77;
  J.nbeds = 3;
  J.dock = 2;
  save();
  coins = J.coins; owned = J.nowned; placed = J.nplaced;
  dock = J.dock + J.snail.load; shipped = J.shipped;
  /* Opened again five hours later, on the same card. */
  fakeapi_epoch = T0 + 5 * 3600;
  reopen();
  CHECK_EQ(J.coins, coins);
  CHECK_EQ(J.dock, dock);
  CHECK_EQ(J.shipped, shipped);
  CHECK_EQ(J.nowned, owned);
  CHECK_EQ(J.nplaced, placed);
  CHECK_EQ(J.nbeds, 3);
  CHECK_EQ(J.seen, T0 + 5 * 3600);
  CHECK_EQ(G.msg[0], 0);                  /* no "while you were away" */
  dump("02_back_after_hours");
  /* and on screen it makes jam again, a coin a jar */
  ticks(60000);
  CHECK(J.coins > coins);
  CHECK_EQ(J.coins - coins, J.shipped - shipped);
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
  CHECK_EQ(RAN[0], 0);                    /* ...but only brings the bars back */
  ticks(400);
  CHECK_EQ(G.bars, 0);
  key('s');
  CHECK(strcmp(RAN, "Jar Shop") == 0);
}

/* 2x zoom: every scene pixel drawn as exactly 2x2 -- pixel for pixel the
 * same picture, only bigger -- with the "2x" mark in the corner. */
static uint16_t FB1[SHT][SW];
void test_jar_zoom_is_the_scene_at_exactly_2x(void) {
  int x, y, bad = 0;
  start(T0);
  ticks(11000);                           /* bars away: the frame is all scene */
  CHECK_EQ(G.bars, BAR);
  G.msg[0] = 0;
  paint();
  memcpy(FB1, FB, sizeof FB1);
  G.zoom = 1;
  G.follow = -1;
  G.zx = 40;
  G.zy = 30;
  paint();                                /* the same moment, zoomed */
  for (y = 12; y < SHT; y++)
    for (x = 0; x < SW; x++)
      if (FB[y][x] != FB1[30 + y / 2][40 + x / 2]) bad++;
  CHECK_EQ(bad, 0);
  for (y = 0; y < 12; y++)                /* and outside the mark, the top rows too */
    for (x = 0; x < SW - 20; x++)
      if (FB[y][x] != FB1[30 + y / 2][40 + x / 2]) bad++;
  CHECK_EQ(bad, 0);
  dump("20_zoom");
}

void test_jar_zoom_keys(void) {
  start(T0);
  ticks(100);
  key('z');
  CHECK_EQ(G.zoom, 1);
  CHECK_EQ(G.follow, 0);                  /* on the first mossling */
  ticks(3000);                            /* the view glides onto it */
  dump("21_zoom_follow");
  key('s');                               /* zoomed: not the shop */
  CHECK_EQ(RAN[0], 0);
  key(CAPP_KEY_LEFT);
  CHECK_EQ(G.follow, -1);                 /* panning lets go of the critter */
  {
    int i;
    for (i = 0; i < 40; i++) key(CAPP_KEY_RIGHT);
    CHECK_EQ(G.zx, SW / 2);               /* and stays inside the jar */
    for (i = 0; i < 40; i++) key(CAPP_KEY_DOWN);
    CHECK_EQ(G.zy, SHT - (SHT + 1) / 2);
  }
  key('\t');
  CHECK_EQ(G.follow, 0);
  key(CAPP_KEY_ESC);
  CHECK_EQ(G.zoom, 0);
  ticks(400);
  key('s');
  CHECK(strcmp(RAN, "Jar Shop") == 0);
}

/* The server for the jar: two parcels waiting, the newest from Maya, and a
 * thank-you from Sam. */
static int GIFTS = 2, THANKS = 1;
static int jar_server(const char *m, const char *path, const char *body, char *out, int cap) {
  (void)m; (void)body;
  if (!strcmp(path, "/q/len?q=jar.gifts")) return snprintf(out, (size_t)cap, "%d\n", GIFTS);
  if (!strncmp(path, "/q/peek?q=jar.gifts&after=", 26)) {
    const char *b = "Maya\thello!\tAAAA";
    if (atoi(path + 26) >= 31) return 0;
    return snprintf(out, (size_t)cap, "31\t-\t%u\t%d\n%s\n", T0, (int)strlen(b), b);
  }
  if (!strncmp(path, "/q/peek?q=jar.thanks", 20)) {
    if (!THANKS) return 0;
    return snprintf(out, (size_t)cap, "8\t-\t%u\t7\nSam\t501\n", T0);
  }
  if (!strncmp(path, "/q/ack?q=jar.thanks&upto=8", 26)) { THANKS = 0; return snprintf(out, (size_t)cap, "ok 1\n"); }
  return -404;
}

void test_jar_a_gift_floats_down_and_a_thank_you_is_hearts(void) {
  int i, hearts = 0, looked = 0;
  start(T0);
  jf_handler = jar_server;
  GIFTS = 2;
  THANKS = 1;
  ticks(2000);
  CHECK_EQ(JF_NREQ, 0);                     /* drawn first, then asked */
  ticks(1500);
  CHECK_EQ(jf_count("/q/len?q=jar.gifts"), 1);
  ticks(100);
  CHECK_EQ(G.waiting, 2);
  CHECK_EQ(jf_count("/q/peek?q=jar.gifts&after=0"), 1);
  CHECK_EQ(J.gseen, 31);
  CHECK(G.para_y > 0);                      /* the parachute */
  CHECK(strstr(G.msg, "Maya") != 0);        /* the hearts did not talk over it */
  for (i = 0; i < J.nmoss; i++) if (J.moss[i].say_t > 0) looked = 1;
  CHECK(looked);                            /* the critters look up */
  CHECK(strstr(fakefs_get("/var/jar/jar.txt"), "gseen 31") != 0);
  ticks(1200);
  dump("17_parachute");
  /* then the thank-you: hearts, a banner, and it is acknowledged */
  CHECK_EQ(jf_count("/q/peek?q=jar.thanks"), 1);
  CHECK_EQ(jf_count("/q/ack?q=jar.thanks&upto=8"), 1);
  CHECK_EQ(THANKS, 0);
  ticks(3000);
  for (i = 0; i < JS_PARTS; i++) if (J.part[i].life && J.part[i].type == JP_HEART) hearts++;
  CHECK(hearts > 0 || G.hearts == 0);
  ticks(5000);
  CHECK_EQ(G.para_y, 0);                    /* landed: waiting on the dock */
  dump("18_parcels_on_the_dock");
  /* Enter opens the menu on Mail, Enter again the post (the first key only
   * brings the bars back) */
  key(CAPP_KEY_ENTER);
  CHECK_EQ(RAN[0], 0);
  ticks(400);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.menu, 1);
  CHECK_EQ(G.msel, M_MAIL);
  key(CAPP_KEY_ENTER);
  CHECK(strcmp(RAN, "Jar Post") == 0 && strcmp(RAN_ARGS, "mail") == 0);
  /* the same gift is not announced twice, after a restart either */
  JF_NREQ = 0;
  reopen();
  NET_UP = 1;
  ticks(4000);
  CHECK_EQ(jf_count("/q/len"), 1);
  CHECK_EQ(G.para_y, 0);
  /* offline after the first ask: it does not ask again */
  NET_UP = 0;
  JF_NREQ = 0;
  ticks(200000);
  CHECK_EQ(JF_NREQ, 0);
  NET_UP = 1;
  ticks(200000);
  CHECK(JF_NREQ > 0);
}

void test_jar_a_young_bed_in_the_scene(void) {
  start(T0);
  js_plant(&J, 0, JPL_CACTUS, T0 - 86400);
  save();
  reopen();
  CHECK(J.bed[0].young);
  ticks(1000);
  dump("19_young_bed");
  fakeapi_epoch = T0 + 3 * 86400;
  ticks(1500);                              /* a minute's tick: it comes of age */
  CHECK(!J.bed[0].young);
}

void test_jar_frames(void) {
  int i;
  start(T0);
  /* a busy afternoon: everything bought, a few things placed */
  J.coins = 5000;
  for (i = 0; i < 6; i++) { js_buy(&J, JU_MOSS); js_buy(&J, JU_BED); js_buy(&J, JU_MACH); js_buy(&J, JU_BELT); }
  own_stock();
  place_new(5); key(CAPP_KEY_ENTER);               /* frog, by the pond */
  js_move_to(&J, js_find(&J, 5), 86, 0);
  place_new(9); key(CAPP_KEY_ENTER);               /* the moon */
  js_move_to(&J, js_find(&J, 9), 100, 4);
  place_new(12); key(CAPP_KEY_ENTER);              /* a jelly */
  place_new(7); key(CAPP_KEY_ENTER);               /* the shell house */
  js_move_to(&J, js_find(&J, 7), 222, 0);
  place_new(8); key(CAPP_KEY_ENTER);               /* a bee */
  key(CAPP_KEY_ESC);
  J.parcels = 2;
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

/* Up a ledge in Decorate: Up while moving puts a standing thing on the
 * ruler shelf, Up again on the matchbox ledge, Down back; Esc puts it back
 * where it was; the level is saved and comes back. */
void test_jar_decorate_puts_things_on_the_ledges(void) {
  const char *sv;
  int i;
  start(T0);
  own_stock();
  place_new(10);                                 /* the button owl, moving */
  CHECK_EQ(J.placed[G.dsel].level, 0);
  key(CAPP_KEY_UP);
  CHECK_EQ(J.placed[G.dsel].level, 1);
  CHECK_EQ(J.placed[G.dsel].lift, JS_LEDGE_H[1]);
  CHECK(J.placed[G.dsel].home_x >= JS_LEDGE_LO[1] && J.placed[G.dsel].home_x <= JS_LEDGE_HI[1]);
  for (i = 0; i < 60; i++) key(CAPP_KEY_LEFT);
  CHECK_EQ(J.placed[G.dsel].home_x, JS_LEDGE_LO[1]);   /* not off the end */
  for (i = 0; i < 8; i++) key(CAPP_KEY_RIGHT);
  dump("30_decorate_on_the_ruler");
  key(CAPP_KEY_ENTER);
  sv = fakefs_get("/var/jar/jar.txt");
  CHECK(sv && strstr(sv, "place 10 30 0 1\n"));
  /* the frog: up twice, onto the matchbox ledge */
  place_new(5);
  key(CAPP_KEY_UP);
  key(CAPP_KEY_UP);
  key(CAPP_KEY_UP);                              /* there is no higher */
  CHECK_EQ(J.placed[G.dsel].level, 2);
  dump("31_decorate_on_the_matchboxes");
  key(CAPP_KEY_ENTER);
  /* move the owl, change its level, think better of it */
  decor_select(js_find(&J, 10));
  key(CAPP_KEY_ENTER);
  key(CAPP_KEY_DOWN);
  CHECK_EQ(J.placed[G.dsel].level, 0);
  CHECK_EQ(J.placed[G.dsel].lift, 0);
  key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_ESC);
  CHECK_EQ(J.placed[G.dsel].level, 1);
  CHECK_EQ(J.placed[G.dsel].home_x, 30);
  /* a hanging thing's Up is still its string; a flier has no levels */
  decor_select(js_find(&J, 2));
  key(CAPP_KEY_ENTER);
  i = J.placed[G.dsel].home_y;
  key(CAPP_KEY_UP);
  CHECK_EQ(J.placed[G.dsel].home_y, i - 2);
  CHECK_EQ(J.placed[G.dsel].level, 0);
  key(CAPP_KEY_ENTER);
  place_new(8);                                  /* the bee */
  key(CAPP_KEY_UP);
  CHECK_EQ(J.placed[G.dsel].level, 0);
  key(CAPP_KEY_ENTER);
  /* back on the card, and back up the ledges after a restart */
  reopen();
  CHECK_EQ(J.placed[js_find(&J, 10)].level, 1);
  CHECK_EQ(J.placed[js_find(&J, 10)].lift, JS_LEDGE_H[1]);
  CHECK_EQ(J.placed[js_find(&J, 5)].level, 2);
  CHECK_EQ(J.placed[js_find(&J, 8)].level, 0);
  CHECK_EQ(J.placed[js_find(&J, 1)].level, 0);
}

/* The jar with things up the ledges and a critter that likes it high going
 * up and down: frames to look at, at 1x and zoomed. */
void test_jar_ledges_in_the_scene(void) {
  int i, k, was_up = 0, came_down = 0;
  start(T0);
  J.coins = 5000;
  for (i = 0; i < 6; i++) { js_buy(&J, JU_MOSS); js_buy(&J, JU_BED); js_buy(&J, JU_MACH); js_buy(&J, JU_BELT); }
  own_stock();
  place_new(10); key(CAPP_KEY_UP); key(CAPP_KEY_ENTER);   /* the owl on the ruler */
  js_move_to(&J, G.dsel, 52, 0);
  place_new(6); key(CAPP_KEY_UP); key(CAPP_KEY_UP); key(CAPP_KEY_ENTER);  /* the lamp, up top */
  js_move_to(&J, G.dsel, 128, 0);
  place_new(5); key(CAPP_KEY_ENTER);                       /* the frog by the pond */
  js_move_to(&J, G.dsel, 76, 0);
  place_new(11); key(CAPP_KEY_ENTER);                      /* the woodlouse */
  k = G.dsel;
  J.placed[k].zone = JZ_HIGH;                              /* ... who likes it high */
  key(CAPP_KEY_ESC);
  ticks(2000);
  dump("32_ledges");
  for (i = 0; i < 4000 && !(J.placed[k].climb == JC_UP && J.placed[k].lift > 12); i++) ticks(25);
  CHECK_EQ(J.placed[k].climb, JC_UP);
  dump("33_climbing");
  for (i = 0; i < 40000 && !came_down; i++) {
    ticks(25);
    if (J.placed[k].level && !J.placed[k].climb && !was_up) {
      was_up = 1;
      ticks(3000);
      dump("34_up_top");
      G.zoom = 1;
      G.follow = -1;
      G.zx = J.placed[k].x / JS_FX - SW / 4;
      G.zy = JS_SOIL - J.placed[k].lift - SHT / 4;
      zoom_clamp();
      dump("35_up_top_zoomed");
      G.zoom = 0;
    }
    if (was_up && !J.placed[k].level && !J.placed[k].climb) came_down = 1;
  }
  CHECK(was_up);
  CHECK(came_down);
  G.zoom = 1;
  G.zx = 0;
  G.zy = 40;
  G.follow = -1;
  dump("36_ruler_zoomed");
  G.zx = 50;
  G.zy = 20;
  dump("37_matchboxes_zoomed");
  G.zoom = 0;
  fakeapi_now.hour = 22;
  ticks(1500);
  dump("38_ledges_night");
  fakeapi_now.hour = 12;
}
