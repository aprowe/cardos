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

void test_jar_keys_open_the_companions_after_saving(void) {
  static const struct { int k; const char *app, *screen; } K[] = {
    { 's', "Jar Shop", "shop" }, { 'p', "Jar Shop", "garden" }, { 'h', "Jar Shop", "shelf" },
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
  /* Enter does nothing without a parcel on the dock */
  RAN[0] = 0;
  key(CAPP_KEY_ENTER);
  CHECK_EQ(RAN[0], 0);
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
  fakeapi_epoch = T0 + 3600;
  reopen();
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
  /* back from a minute in the shop: paid, but no banner about it */
  save();
  fakeapi_epoch += 60;
  reopen();
  CHECK_EQ(G.msg[0], 0);
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
  /* Enter opens the post (the first key only brings the bars back) */
  key(CAPP_KEY_ENTER);
  CHECK_EQ(RAN[0], 0);
  ticks(400);
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
