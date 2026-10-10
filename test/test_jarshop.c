/* Jar Shop on the host: the hand-made stock and buying, My Stuff and the
 * item card, upgrades, the garden and the shelf, and the day's stock from
 * a stand-in server -- signed records kept, a forged one dropped, the old
 * batch kept when the new one does not arrive.
 *
 * JAR_DUMP=dir writes its screens as PPM (test/jarfake.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "jarfake.h"

#define capp_info jarshop_capp_info
#define capp_main jarshop_capp_main
#include "apps/jarshop.c"
#undef capp_info
#undef capp_main

#define T0 1800000000u                 /* 2027-01-15 08:00 UTC */

static CardApi A;
static char RAN[64], RAN_ARGS[64];

static int f_run(const char *name, const char *args) {
  snprintf(RAN, sizeof RAN, "%s", name);
  snprintf(RAN_ARGS, sizeof RAN_ARGS, "%s", args ? args : "");
  return 0;
}

static char ENV[64];
static int f_shell(const char *line, char *out, size_t n) {
  if (strcmp(line, "env")) return -1;
  snprintf(out, n, "PATH=/apps\n%s", ENV);
  return 0;
}

/* A card with a jar on it: the starters, and some coins. */
static void card(uint32_t coins) {
  static Jar j;
  static JIo io;
  static char t[2048];
  int i;
  fakeapi_init(&A);
  fakefs_mem(&A);
  jf_server(&A);
  jf_screen(&A);
  A.run = f_run;
  A.shell = f_shell;
  ENV[0] = 0;
  fakeapi_epoch = T0;
  fakeapi_ticks = 1000;
  RAN[0] = 0;
  jst_dirs(&A);
  memset(&j, 0, sizeof j);
  js_init(&j, 1);
  for (i = 0; i < JB_NSTART; i++) {
    jst_from_builtin(&JB_STARTERS[i], &io.it);
    jst_item_write(&A, &io);
    js_own(&j, io.it.id);
  }
  j.coins = coins;
  j.nwant = 1;
  j.want[0].id = 1; j.want[0].x = 58;
  jst_save(&A, &j, t, sizeof t);
}

static void launch(const char *screen) {
  char *argv[2];
  argv[0] = "jarshop";
  argv[1] = (char *)screen;
  JF_PENDING = 0;                    /* the OS drops a closed app's request */
  jarshop_capp_main(&A, screen ? 2 : 1, argv);
}

static void key(int k) { app_key(0, (uint8_t)k); }

static void tick(int ms) {
  int t;
  for (t = 0; t < ms; t += 5) { fakeapi_ticks += 5; app_tick(0, fakeapi_ticks); }
}

static void shot(const char *name) {
  CRect all = { 0, 0, SW, SHT };
  app_paint(0, all);
  jf_dump(name);
}

static const char *saved(void) { return fakefs_get("/var/jar/jar.txt"); }

void test_jarshop_without_a_jar_opens_the_jar(void) {
  fakeapi_init(&A);
  fakefs_mem(&A);
  A.run = f_run;
  RAN[0] = 0;
  launch("shop");
  CHECK(strcmp(RAN, "Jar Factory") == 0);
}

void test_jarshop_buys_from_the_hand_made_stock(void) {
  card(100);
  launch("shop");                                  /* offline: no server answers */
  CHECK_EQ(G.view, V_STOCK);
  CHECK_EQ(S.n, 8);
  CHECK_EQ(U.card.id, 5);                         /* the frog, 60 */
  CHECK_EQ(U.card.price, 60);
  shot("s01_stock");
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_CARD);
  CHECK_EQ(J.coins, 40);
  CHECK(J.sold & (1u << 5));
  CHECK(js_owns(&J, 5));
  CHECK(fakefs_exists("/var/jar/items/5.itm"));
  CHECK(strstr(saved(), "coins 40") != 0);
  shot("s02_card");
  /* sold: Enter does nothing; the owl at 150 is too dear */
  key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_STOCK);
  CHECK(!U.card.ok);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(J.coins, 40);
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);
  CHECK_EQ(U.card.id, 10);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_STOCK);
  CHECK_EQ(J.coins, 40);
  /* Esc: back to the jar, saved */
  key(CAPP_KEY_ESC);
  CHECK(strcmp(RAN, "Jar Factory") == 0);
}

void test_jarshop_my_stuff_card_shelf_and_place(void) {
  card(0);
  launch("stuff");
  CHECK_EQ(G.view, V_STUFF);
  CHECK_EQ(G.ntile, 4);
  CHECK(G.tile[0].in_jar);                       /* saved as placed */
  shot("s03_stuff");
  key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_CARD);
  key('h');                                      /* on the shelf */
  CHECK_EQ(J.shelf[0], U.card.id);
  CHECK(strstr(saved(), "shelf 2 0 0 0") != 0);
  key('h');                                      /* and off */
  CHECK_EQ(J.shelf[0], 0);
  /* Enter: into the jar -- the jar does the placing */
  key(CAPP_KEY_ENTER);
  CHECK(strcmp(RAN, "Jar Factory") == 0);
  CHECK(strstr(saved(), "decor 2") != 0);
  /* the place lines the jar wrote are still there */
  CHECK(strstr(saved(), "place 1 58 0") != 0);
}

void test_jarshop_decor_picks_for_the_jar(void) {
  card(0);
  launch("decor");
  CHECK_EQ(G.pick, P_DECOR);
  shot("s04_pick");
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_ENTER);
  CHECK(strcmp(RAN, "Jar Factory") == 0);
  CHECK(strstr(saved(), "decor 3") != 0);
}

void test_jarshop_upgrades(void) {
  card(500);
  launch("up");
  shot("s05_upgrades");
  key(CAPP_KEY_ENTER);                    /* another mossling: 30 */
  CHECK_EQ(J.nmoss, 2);
  CHECK_EQ(J.coins, 470);
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);                    /* the press: 120 */
  CHECK_EQ(J.nmach, 3);
  CHECK(strstr(saved(), "up 2 2 3 0") != 0);
  J.coins = 0;
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(J.belt, 0);                    /* not enough */
  CHECK(U.msg[0] != 0);
}

void test_jarshop_garden_plants_and_asks_before_pulling_up(void) {
  card(200);
  launch("garden");
  CHECK_EQ(G.view, V_GARDEN);
  shot("s06_garden");
  key(CAPP_KEY_ENTER);                    /* bed 1: a grown berry bush */
  CHECK_EQ(G.view, V_PLANT);
  key(CAPP_KEY_DOWN); key(CAPP_KEY_DOWN); key(CAPP_KEY_DOWN); key(CAPP_KEY_DOWN);
  CHECK_EQ(G.plant, JPL_CACTUS);
  shot("s07_plant");
  key(CAPP_KEY_ENTER);
  CHECK(G.asking);                        /* it is grown: sure? */
  shot("s08_pull_up");
  key(CAPP_KEY_ENTER);                    /* Enter is not an answer */
  CHECK(G.asking);
  key('n');
  CHECK(!G.asking);
  CHECK_EQ(J.bed[0].type, JPL_BERRY);
  key(CAPP_KEY_ENTER);
  key('y');
  CHECK_EQ(J.bed[0].type, JPL_CACTUS);
  CHECK(J.bed[0].young);
  CHECK_EQ(J.coins, 160);
  CHECK(strstr(saved(), "plant 4 1800000000 ") != 0);
  CHECK_EQ(G.view, V_GARDEN);
  /* a young bed is replanted without asking */
  key(CAPP_KEY_ENTER);
  key(CAPP_KEY_UP);
  key(CAPP_KEY_ENTER);
  CHECK(!G.asking);
  CHECK_EQ(J.bed[0].type, JPL_FLOWER);
  /* beds not bought yet */
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_GARDEN);
  shot("s09_garden_planted");
}

void test_jarshop_shelf(void) {
  card(0);
  launch("shelf");
  CHECK_EQ(G.view, V_SHELF);
  key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_ENTER);                    /* fill slot 2 from My Stuff */
  CHECK_EQ(G.pick, P_SHELF);
  key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_SHELF);
  CHECK_EQ(J.shelf[1], 2);
  CHECK(G.shelf[1].ok);
  CHECK(G.shelf_tags[0] != 0);
  shot("s10_shelf");
  key('x');
  CHECK_EQ(J.shelf[1], 0);
}

/* ---- the day's stock -------------------------------------------------------- */

static int DAY_POLLS, FORGE;
static int day_server(const char *m, const char *path, const char *body, char *out, int cap) {
  (void)body;
  if (!strcmp(path, "/jar/pubkey")) return snprintf(out, (size_t)cap, "%s\n", JF_PUBHEX);
  if (!strcmp(path, "/jar/day") && !strcmp(m, "POST")) return snprintf(out, (size_t)cap, "pending\n");
  if (!strcmp(path, "/jar/day")) {
    if (DAY_POLLS++ < 1) return snprintf(out, (size_t)cap, "pending\n");
    return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy, cosy, moon\nitems 3\n");
  }
  if (!strncmp(path, "/jar/item?i=", 12)) {
    static const char *const NAMES[3] = { "Rain Snail", "Moon Moth", "Tea Cosy" };
    JItem it;
    int k = atoi(path + 12), n;
    if (k < 0 || k > 2) return -404;
    jf_item(&it, 9000 + (uint32_t)k, NAMES[k], k == 1 ? "glowing,night" : "cosy,rainy");
    n = jf_signed_b64(&it, out, cap - 1, !(FORGE && k == 1));
    out[n++] = '\n';
    return n;
  }
  return -404;
}

void test_jarshop_fetches_the_days_stock_and_checks_each_record(void) {
  const JfReq *r;
  card(500);
  ENV[0] = 0;
  snprintf(ENV, sizeof ENV, "TZ=PST8PDT,M3.2.0,M11.1.0\n");
  jf_handler = day_server;
  DAY_POLLS = 0;
  FORGE = 1;
  /* a garden with something growing, and a shelf */
  launch("shelf");
  J.shelf[0] = 2;
  js_plant(&J, 1, JPL_SHROOM, T0 - 4 * 86400);
  js_settle_beds(&J, T0);
  save();
  launch("shop");
  CHECK_EQ(jf_count("/jar/pubkey"), 1);       /* the key first, pinned */
  tick(10);
  CHECK(fakefs_exists("/var/jar/server.pub"));
  CHECK_EQ(jf_count("/jar/day"), 1);
  r = jf_last("/jar/day");
  CHECK(strcmp(r->method, "POST") == 0);
  CHECK(strstr(r->body, "garden mushroom=1,berry=1,fern=0,flower=0,cactus=0\n") != 0);
  CHECK(strstr(r->body, "\nshelf ") != 0);
  CHECK(strstr(r->body, "\nowned ") != 0 && strstr(r->body, "Dandelion") != 0);
  CHECK(strstr(r->body, "\ntz PST8PDT,M3.2.0,M11.1.0\n") != 0);
  shot("s11_arriving");
  tick(10);                                   /* "pending": wait */
  CHECK_EQ(jf_count("/jar/day"), 1);
  tick(3100);
  CHECK_EQ(jf_count("/jar/day"), 2);          /* asked again */
  tick(3100);
  tick(50);
  CHECK_EQ(jf_count("/jar/item?i="), 3);
  CHECK_EQ(S.n, 2);                           /* the forged one was dropped */
  CHECK(strcmp(S.date, "2027-01-15") == 0);
  CHECK(strcmp(S.tags, "rainy, cosy, moon") == 0);
  CHECK_EQ(S.id[0], 9000);
  CHECK_EQ(S.id[1], 9002);
  CHECK(S.price[0] >= 45 && S.price[0] <= 200);
  CHECK(strstr(fakefs_get("/var/jar/stock.txt"), "date 2027-01-15") != 0);
  shot("s12_todays_stock");
  /* buying keeps the server's record byte for byte, signature and all */
  {
    int before = (int)J.coins, p = S.price[0];
    key(CAPP_KEY_ENTER);
    CHECK_EQ((int)J.coins, before - p);
    CHECK(js_owns(&J, 9000));
    CHECK_EQ(jst_item_read(&A, 9000, &IO) > 0, 1);
    CHECK_EQ(IO.it.sig_len, 64);
    CHECK(strstr(fakefs_get("/var/jar/stock.txt"), "sold 1") != 0);
    shot("s13_bought");
  }
  /* opened again the same day: no new request */
  JF_NREQ = 0;
  launch("shop");
  tick(100);
  CHECK_EQ(jf_count("/jar/day"), 0);
  CHECK_EQ(S.n, 2);
  CHECK(jst_sold(&S, &J, 0));
  /* the next day, offline: yesterday's stays */
  fakeapi_epoch = T0 + 86400;
  jf_handler = 0;
  launch("shop");
  tick(100);
  CHECK(strcmp(S.date, "2027-01-15") == 0);
  CHECK_EQ(S.n, 2);
  CHECK(U.msg[0] != 0);                       /* "Today's stock: offline" */
  /* no clock: nothing asked */
  JF_NREQ = 0;
  fakeapi_epoch = 0;
  launch("shop");
  tick(100);
  CHECK_EQ(JF_NREQ, 0);
}

void test_jarshop_g_buys_and_goes_to_send_a_gift(void) {
  card(500);
  jf_handler = day_server;
  DAY_POLLS = 5;
  FORGE = 0;
  launch("shop");
  tick(3500);
  CHECK_EQ(S.n, 3);
  key('g');
  CHECK(js_owns(&J, 9000));
  CHECK(strcmp(RAN, "Jar Post") == 0);
  CHECK(strcmp(RAN_ARGS, "gift 9000") == 0);
}
