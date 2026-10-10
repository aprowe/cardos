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

void test_jarshop_my_stuff_card_and_place(void) {
  card(0);
  launch("stuff");
  CHECK_EQ(G.view, V_STUFF);
  CHECK_EQ(G.ntile, 4);
  CHECK(G.tile[0].in_jar);                       /* saved as placed */
  shot("s03_stuff");
  key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_CARD);
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
  CHECK_EQ(SEEDS[JPL_CACTUS], 1);         /* a card with no seeds file starts with one of each */
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
  CHECK_EQ(J.coins, 200);                 /* seeds, not coins */
  CHECK_EQ(SEEDS[JPL_CACTUS], 0);
  CHECK(strcmp(fakefs_get("/var/jar/seeds.txt"), "1 1 1 1 0 ") == 0);
  CHECK(strstr(saved(), "plant 4 1800000000 ") != 0);
  CHECK_EQ(G.view, V_GARDEN);
  /* a young bed is replanted without asking */
  key(CAPP_KEY_ENTER);
  key(CAPP_KEY_UP);
  key(CAPP_KEY_ENTER);
  CHECK(!G.asking);
  CHECK_EQ(J.bed[0].type, JPL_FLOWER);
  /* no cactus seeds left: nothing planted */
  key(CAPP_KEY_ENTER);
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(J.bed[0].type, JPL_FLOWER);
  CHECK(strstr(U.msg, "No seeds") != 0);
  key(CAPP_KEY_ESC);
  /* beds not bought yet */
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_GARDEN);
  shot("s09_garden_planted");
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
  launch("shop");
  CHECK_EQ(jf_count("/jar/pubkey"), 1);       /* the key first, pinned */
  tick(10);
  CHECK(fakefs_exists("/var/jar/server.pub"));
  CHECK_EQ(jf_count("/jar/day"), 1);
  r = jf_last("/jar/day");
  CHECK(strcmp(r->method, "POST") == 0);
  CHECK(strstr(r->body, "jar Moss Bench") == r->body);   /* what is in the jar: Tibbs knows */
  CHECK(strstr(r->body, "garden") == 0);                 /* plants and the shelf steer nothing */
  CHECK(strstr(r->body, "shelf") == 0);
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
  /* opened again the same day: only asked how it stands, and as nothing
   * changed, nothing is fetched or said */
  JF_NREQ = 0;
  U.msg[0] = 0;
  launch("shop");
  tick(100);
  CHECK_EQ(jf_count("/jar/day"), 1);
  CHECK(strcmp(jf_last("/jar/day")->method, "GET") == 0);
  CHECK_EQ(jf_count("/jar/item?i="), 0);
  CHECK_EQ(U.msg[0], 0);
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

/* A server that has the day made already answers the POST "ok DATE" alone
 * (an older one did): that is not a stock of none -- it once emptied the
 * shop the second time it was opened. */
static int made_server(const char *m, const char *path, const char *body, char *out, int cap) {
  if (!strcmp(path, "/jar/day") && !strcmp(m, "POST")) return snprintf(out, (size_t)cap, "ok 2027-01-15\n");
  DAY_POLLS = 5;
  return day_server(m, path, body, out, cap);
}

void test_jarshop_an_ok_without_a_count_does_not_empty_the_shop(void) {
  card(500);
  jf_handler = made_server;
  FORGE = 0;
  launch("shop");
  tick(200);
  CHECK_EQ(jf_count("/jar/item?i="), 3);
  CHECK_EQ(S.n, 3);
  CHECK(strstr(fakefs_get("/var/jar/stock.txt"), "item 9002") != 0);
}

/* The server makes a stock one item at a time: "more" until it is whole. */
static int GROW;
static int grow_server(const char *m, const char *path, const char *body, char *out, int cap) {
  if (!strcmp(path, "/jar/day") && strcmp(m, "POST")) {
    GROW++;
    if (GROW == 1) return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 1\nmore\n");
    return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 3\n");
  }
  DAY_POLLS = 5;
  return day_server(m, path, body, out, cap);
}

void test_jarshop_items_show_as_they_are_made(void) {
  card(500);
  jf_handler = grow_server;
  GROW = 0;
  FORGE = 0;
  launch("shop");
  tick(3200);                                 /* the POST's pending, then the first */
  tick(100);
  CHECK_EQ(GROW, 1);
  CHECK_EQ(S.n, 1);                           /* on show already */
  CHECK(strcmp(S.date, "2027-01-15") == 0);
  CHECK(strstr(fakefs_get("/var/jar/stock.txt"), "item 9000") != 0);
  key(CAPP_KEY_ENTER);                        /* bought while the rest is made */
  CHECK(js_owns(&J, 9000));
  key(CAPP_KEY_ESC);
  tick(4200);
  tick(100);
  CHECK_EQ(GROW, 2);
  CHECK_EQ(S.n, 3);
  CHECK_EQ(jf_count("/jar/item?i="), 3);      /* each fetched once */
  CHECK(jst_sold(&S, &J, 0));                 /* still sold */
  CHECK(!jst_sold(&S, &J, 1));
}

/* The shop closed while the server was still making the stock (or gave
 * up on it): opened again it picks up the rest -- the bug where the new
 * stock was made but never reached the shop. */
static int HALF;
static int half_server(const char *m, const char *path, const char *body, char *out, int cap) {
  if (!strcmp(path, "/jar/day") && strcmp(m, "POST")) {
    if (HALF) return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 3\nbatch 7\n");
    return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 1\nbatch 7\nmore\n");
  }
  DAY_POLLS = 5;
  return day_server(m, path, body, out, cap);
}

void test_jarshop_opened_again_it_picks_up_the_rest_of_a_stock(void) {
  card(500);
  jf_handler = half_server;
  HALF = 0;
  FORGE = 0;
  launch("shop");
  tick(3300);
  CHECK_EQ(S.n, 1);
  CHECK(strstr(fakefs_get("/var/jar/stock.txt"), "batch 7") != 0);
  key(CAPP_KEY_ENTER);                        /* bought */
  CHECK(js_owns(&J, 9000));
  /* closed; later the server has finished */
  HALF = 1;
  JF_NREQ = 0;
  launch("shop");
  tick(200);
  CHECK_EQ(jf_count("/jar/item?i="), 2);      /* only the two not here */
  CHECK_EQ(S.n, 3);
  CHECK(jst_sold(&S, &J, 0));                 /* still bought */
}

/* Space holds an item: it is said with the next request, kept by id into
 * the new stock, and let go of by Space again or by buying it. */
static int NEXT_BATCH;
static int hold_server(const char *m, const char *path, const char *body, char *out, int cap) {
  if (!strncmp(path, "/jar/day", 8) && !strcmp(m, "POST")) { NEXT_BATCH++; return snprintf(out, (size_t)cap, "pending\n"); }
  if (!strcmp(path, "/jar/day")) {
    if (NEXT_BATCH < 2) return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 3\nbatch 1\n");
    return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 2\nbatch 2\n");
  }
  if (NEXT_BATCH >= 2 && !strncmp(path, "/jar/item?i=", 12)) {
    /* the new stock: the held one (9001) first, then a new one */
    JItem it;
    int k = atoi(path + 12), n;
    if (k > 1) return -404;
    jf_item(&it, k == 0 ? 9001 : 9010, k == 0 ? "Moon Moth" : "Kettle", "cosy,rainy");
    n = jf_signed_b64(&it, out, cap - 1, 1);
    out[n++] = '\n';
    return n;
  }
  return day_server(m, path, body, out, cap);
}

void test_jarshop_held_items_stay_into_the_next_stock(void) {
  const JfReq *r;
  card(5000);
  jf_handler = hold_server;
  NEXT_BATCH = 0;
  FORGE = 0;
  launch("shop");
  tick(3300);
  CHECK_EQ(S.n, 3);
  key(CAPP_KEY_RIGHT);
  key(' ');                                   /* hold 9001 */
  CHECK_EQ(S.held, 2);
  CHECK(G.tile[1].held);
  shot("s15_held");
  key(CAPP_KEY_RIGHT);
  key(' ');                                   /* and 9002, then let it go */
  key(' ');
  CHECK_EQ(S.held, 2);
  key(CAPP_KEY_LEFT);
  key(CAPP_KEY_LEFT);
  key(CAPP_KEY_ENTER);                        /* buy 9000 */
  CHECK(js_owns(&J, 9000));
  key(CAPP_KEY_ESC);
  key('r');                                   /* a new stock */
  r = jf_last("/jar/day");
  CHECK(r && strcmp(r->method, "POST") == 0);
  CHECK(r && strstr(r->body, "\nbought 9000\n") != 0);
  CHECK(r && strstr(r->body, "\nheld 9001\n") != 0);
  tick(3300);
  CHECK_EQ(S.n, 2);
  CHECK_EQ(S.id[0], 9001);
  CHECK_EQ(S.held, 1);                        /* still held, in its new place */
  CHECK_EQ(S.sold, 0);
  /* buying a held one lets go of it */
  key(CAPP_KEY_ENTER);
  CHECK(js_owns(&J, 9001));
  CHECK_EQ(S.held, 0);
}

/* An item that fails its check while the stock is still being made is not
 * fetched again every poll, and the shop says items failed rather than
 * looking as if there were none (it once read "0 here" for an hour). */
static int DROP_ROUND;
static int drop_server(const char *m, const char *path, const char *body, char *out, int cap) {
  if (!strcmp(path, "/jar/day") && strcmp(m, "POST")) {
    DROP_ROUND++;
    if (DROP_ROUND < 3) return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 2\nbatch 5\nmore\n");
    return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 3\nbatch 5\n");
  }
  DAY_POLLS = 5;
  return day_server(m, path, body, out, cap);
}

void test_jarshop_a_refused_item_is_said_and_not_fetched_again(void) {
  card(500);
  jf_handler = drop_server;
  DROP_ROUND = 0;
  FORGE = 1;                                  /* item 1 is forged */
  launch("shop");
  tick(3300);
  CHECK_EQ(S.n, 1);
  CHECK_EQ(jf_count("/jar/item?i="), 2);
  tick(4200);                                 /* still being made: asked again */
  CHECK_EQ(DROP_ROUND, 2);
  CHECK_EQ(jf_count("/jar/item?i="), 2);      /* nothing fetched twice */
  tick(4200);
  CHECK_EQ(DROP_ROUND, 3);
  CHECK_EQ(jf_count("/jar/item?i="), 3);      /* only the new one */
  CHECK_EQ(S.n, 2);
  CHECK(strstr(U.msg, "1 failed") != 0);
}

/* Tibbs's line of the day, a seed packet, and prices the server chose. */
static int tibbs_server(const char *m, const char *path, const char *body, char *out, int cap) {
  if (!strcmp(path, "/jar/day") && strcmp(m, "POST"))
    return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags \nitems 3\nbatch 9\n"
                    "say Power went out, so I sorted the back shelf by candle.\nseed fern 25\n");
  if (!strncmp(path, "/jar/item?i=", 12)) {
    JItem it;
    int k = atoi(path + 12), n;
    if (k < 0 || k > 2) return -404;
    jf_item(&it, 9000 + (uint32_t)k, k == 0 ? "Padlock Bug" : "Cork Owl", "");
    it.price5 = (uint8_t)(k == 0 ? 90 : 0);                /* 450 coins, or none given */
    n = jf_signed_b64(&it, out, cap - 1, 1);
    out[n++] = '\n';
    return n;
  }
  DAY_POLLS = 5;
  return day_server(m, path, body, out, cap);
}

void test_jarshop_tibbs_and_his_seed_packets(void) {
  card(500);
  jf_handler = tibbs_server;
  FORGE = 0;
  launch("shop");
  tick(3500);
  CHECK_EQ(S.n, 3);
  CHECK(strcmp(SAY, "Power went out, so I sorted the back shelf by candle.") == 0);
  CHECK(fakefs_exists("/var/jar/say.txt"));
  CHECK_EQ(S.price[0], 450);                  /* the server's price */
  CHECK(S.price[1] >= 45 && S.price[1] <= 200); /* none given: worked out as before */
  CHECK_EQ(S.seed, JPL_FERN + 1);
  CHECK_EQ(S.seed_price, 25);
  CHECK(!G.tile[1].seed && !G.tile[2].seed && G.tile[3].seed);   /* only the last is seeds */
  shot("s14_tibbs");
  /* the seed packet is the last tile */
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);
  CHECK(U.card.seed);
  CHECK(strcmp(U.name, "fern seeds") == 0);
  shot("s15_seeds");
  key(' ');                                   /* seeds are not held */
  CHECK_EQ(S.held, 0);
  key(CAPP_KEY_ENTER);
  CHECK_EQ((int)J.coins, 475);
  CHECK_EQ(SEEDS[JPL_FERN], 2);
  CHECK(S.seed_sold);
  CHECK(strstr(fakefs_get("/var/jar/stock.txt"), "seed 2 25 1") != 0);
  key(CAPP_KEY_ENTER);                        /* sold: once */
  CHECK_EQ((int)J.coins, 475);
  /* opened again: his line and the sold packet are kept */
  launch("shop");
  CHECK(strcmp(SAY, "Power went out, so I sorted the back shelf by candle.") == 0);
  CHECK(S.seed_sold);
  /* t: talk to him, in Jar Post */
  key('t');
  CHECK(strcmp(RAN, "Jar Post") == 0 && strcmp(RAN_ARGS, "talk") == 0);
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
