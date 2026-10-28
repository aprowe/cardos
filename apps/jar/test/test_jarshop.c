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

#include "apps/jar/jarshop.h"           /* the shop screen, standing alone */

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
  (void)argv;
  api = &A;
  shop_open(screen);
}

static void key(int k) { shop_key(0, (uint8_t)k); }

static void tick(int ms) {
  int t;
  for (t = 0; t < ms; t += 5) { fakeapi_ticks += 5; shop_tick(0, fakeapi_ticks); }
}

static void shot(const char *name) {
  CRect all = { 0, 0, SW, SHT };
  shop_paint(0, all);
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
  CHECK_EQ(G.buying, 1);                          /* sure? nothing paid yet */
  CHECK_EQ(J.coins, 100);
  shot("s01b_sure");
  key(CAPP_KEY_ENTER);                            /* Enter is not an answer */
  CHECK_EQ(G.buying, 1);
  key('n');
  CHECK_EQ(J.coins, 100);
  CHECK(!js_owns(&J, 5));
  key(CAPP_KEY_ENTER);
  key('y');
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
  key(CAPP_KEY_ENTER);                    /* another mossling: 30 -- sure? */
  CHECK_EQ(G.buying, 3);
  shot("s05b_sure");
  key('y');
  CHECK_EQ(J.coins, 470);                 /* paid; built in a quarter of an hour */
  CHECK_EQ(J.nmoss, 1);
  CHECK_EQ(J.build_k, JU_MOSS + 1);
  CHECK_EQ(J.build_at, T0 + 900);
  CHECK(strstr(saved(), "build 1 1800000900") != 0);
  shot("s05c_building");
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);                    /* one at a time */
  CHECK_EQ(G.buying, 0);
  CHECK(strstr(U.msg, "one thing") != 0 || strstr(U.msg, "One thing") != 0);
  fakeapi_epoch = T0 + 900;               /* a quarter of an hour on */
  launch("up");
  CHECK_EQ(J.nmoss, 2);
  CHECK_EQ(J.build_k, 0);
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);                    /* the press: 120 */
  key('y');
  fakeapi_epoch = T0 + 900 + 900;
  launch("up");
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
  CHECK_EQ(G.plant, JPL_FLOWER);
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
  CHECK_EQ(J.bed[0].type, JPL_FLOWER);
  CHECK(J.bed[0].young);
  CHECK_EQ(J.coins, 200);                 /* free */
  CHECK(strstr(saved(), "plant 3 1800000000 ") != 0);
  CHECK_EQ(G.view, V_GARDEN);
  /* a young bed is replanted without asking */
  key(CAPP_KEY_ENTER);
  key(CAPP_KEY_UP);
  key(CAPP_KEY_ENTER);
  CHECK(!G.asking);
  CHECK_EQ(J.bed[0].type, JPL_SHROOM);
  /* and again: no seeds to run out of */
  key(CAPP_KEY_ENTER);
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(J.bed[0].type, JPL_FLOWER);
  CHECK(strstr(U.msg, "an hour") != 0);
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
  if (!strncmp(path, "/jar/day", 8) && !strcmp(m, "POST")) return snprintf(out, (size_t)cap, "pending\n");
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
    key('y');
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
  key('y');
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
  key('y');
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

/* A commission struck in talk: /jar/day says "deal", and the shop asks for
 * the fresh stock itself -- with its report, and charging nothing (the coins
 * went at the counter). */
static int DEAL;
static int deal_server(const char *m, const char *path, const char *body, char *out, int cap) {
  if (!strcmp(path, "/jar/day") && strcmp(m, "POST") && DEAL && NEXT_BATCH < 2)
    return snprintf(out, (size_t)cap, "ok 2027-01-15\ntags rainy\nitems 3\nbatch 1\ndeal\n");
  if (!strncmp(path, "/jar/day?fresh=1", 16)) DEAL = 0;
  return hold_server(m, path, body, out, cap);
}

void test_jarshop_a_deal_struck_in_talk_brings_a_fresh_stock(void) {
  const JfReq *r;
  card(5000);
  jf_handler = deal_server;
  NEXT_BATCH = 0;
  DEAL = 1;
  FORGE = 0;
  launch("shop");
  tick(3300);
  r = jf_last("/jar/day?fresh=1");
  CHECK(r != 0);
  CHECK(r && strcmp(r->method, "POST") == 0);
  CHECK_EQ(J.coins, 5000);                    /* nothing taken here */
  tick(3300);
  CHECK_EQ(S.batch, 2);                       /* the new stock is on show */
  CHECK_EQ(jf_count("/jar/day?fresh=1"), 1);  /* and asked for once */
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
  key('y');
  CHECK(js_owns(&J, 9000));
  key(CAPP_KEY_ESC);
  key('r');                                   /* a new stock, paid for */
  key('y');
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
  key('y');
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
                    "say Power went out, so I sorted the back shelf by candle.\nseed reed 25\n");
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

/* Paying Tibbs, so as not to wait for the next stock: r a new stock now,
 * f he goes out looking (what for is typed, or nothing) for eight new finds.
 * The coins come back if the server cannot be asked. */
void test_jarshop_paying_tibbs(void) {
  const JfReq *r;
  const char *s;
  card(500);
  jf_handler = day_server;
  DAY_POLLS = 5;
  FORGE = 0;
  launch("shop");
  tick(3500);
  JF_NREQ = 0;
  key('r');
  CHECK_EQ(G.pay, 1);                         /* sure? */
  shot("s16_restock");
  key('n');
  CHECK_EQ(G.pay, 0);
  CHECK_EQ((int)J.coins, 500);
  CHECK_EQ(jf_count("/jar/day"), 0);
  key('r');
  key('y');
  CHECK_EQ((int)J.coins, 500 - RESTOCK_COST);
  r = jf_last("/jar/day?fresh=1");
  CHECK(r && strcmp(r->method, "POST") == 0 && strstr(r->body, "finds") == 0);
  tick(3500);
  key('f');
  CHECK_EQ(G.pay, 2);                         /* what to look for */
  for (s = "brass birds"; *s; s++) key(*s);
  shot("s17_send_tibbs_out");
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.pay, 3);
  key('y');
  CHECK_EQ((int)J.coins, 500 - RESTOCK_COST - FIND_COST);
  r = jf_last("/jar/day?fresh=1");
  CHECK(r && strstr(r->body, "\nfinds 8\nask brass birds\n") != 0);
  tick(3500);
  /* the server refuses: the coins come back */
  jf_handler = 0;
  JF_PENDING = 0;
  key('r');
  key('y');
  tick(200);
  CHECK_EQ((int)J.coins, 500 - RESTOCK_COST - FIND_COST);
  /* not enough coins: nothing asked */
  J.coins = 10;
  key('f');
  CHECK_EQ(G.pay, 0);
  CHECK(strstr(U.msg, "Not enough") != 0);
}

/* The device's one HTTP slot is busy now and then (its own polls): a paid
 * ask waits for it rather than failing -- on the device it said "No server:
 * coins back" -- and is refunded only if the slot never comes free. */
void test_jarshop_a_paid_ask_waits_for_a_busy_slot(void) {
  card(500);
  jf_handler = day_server;
  DAY_POLLS = 5;
  FORGE = 0;
  launch("shop");
  tick(3500);
  JF_NREQ = 0;
  JF_PENDING = 1;                             /* someone else's request in flight */
  U.net = 0;
  key('r');
  key('y');
  CHECK_EQ((int)J.coins, 500 - RESTOCK_COST);  /* paid; waiting */
  CHECK_EQ(JF_NREQ, 0);
  tick(2000);
  CHECK_EQ(JF_NREQ, 0);
  JF_PENDING = 0;                             /* the slot comes free */
  tick(20);
  CHECK(jf_last("/jar/day?fresh=1") != 0);
  CHECK_EQ((int)J.coins, 500 - RESTOCK_COST);
  tick(3500);
  /* never free: the coins come back after a while */
  JF_PENDING = 1;
  U.net = 0;
  key('r');
  key('y');
  tick(16000);
  CHECK_EQ((int)J.coins, 500 - RESTOCK_COST);
  CHECK(strstr(U.msg, "coins back") != 0);
  JF_PENDING = 0;
}

/* His line, his prices -- and a seed line from an older server sells nothing:
 * plants are free now. */
void test_jarshop_tibbs_and_no_seed_packets(void) {
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
  CHECK_EQ(S.seed, 0);
  CHECK_EQ(page_count(), 3);                  /* no packet tile after the items */
  shot("s14_tibbs");
  key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT); key(CAPP_KEY_RIGHT);
  CHECK_EQ(G.sel, 2);                         /* the last item is the last tile */
  /* opened again: his line is kept */
  launch("shop");
  CHECK(strcmp(SAY, "Power went out, so I sorted the back shelf by candle.") == 0);
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
  CHECK(!js_owns(&J, 9000));                  /* sure? first */
  key('y');
  CHECK(js_owns(&J, 9000));
  CHECK(strcmp(RAN, "Jar Post") == 0);
  CHECK(strcmp(RAN_ARGS, "gift 9000") == 0);
}


/* Letting go (2026-10-11): s sells a thing back for a quarter of its price,
 * d trashes it for good; both ask first; either takes it out of everything
 * on the card -- My Stuff, its file, the factory's places, an extra jar's
 * file -- and queues it for the server. A built-in sells for nothing. */
void test_jarshop_sell_and_trash(void) {
  static JItem it;
  static uint8_t raw[JI_MAX];
  int n, i;
  card(100);
  launch("stuff");
  /* two of the server's things: 7001 (priced 200) and 7002, 7002 in an extra jar */
  for (i = 0; i < 2; i++) {
    jf_item(&it, 7001 + (uint32_t)i, i ? "Tin Kite" : "Moth Lamp", "");
    it.price5 = 40;
    n = jf_signed(&it, raw, 1);
    jst_put(&A, i ? "/var/jar/items/7002.itm" : "/var/jar/items/7001.itm", raw, n);
    js_own(&J, it.id);
  }
  J.njars = 1;
  J.want[J.nwant].id = 7001; J.want[J.nwant].x = 90; J.nwant++;     /* 7001 in the factory */
  fakefs_put("/var/jar/jar2.txt", "jarx 1\nplace 5 30 0\nplace 7002 40 0\n");
  save();
  launch("stuff");
  for (i = 0; i < 8 && U.card.id != 7001; i++) key(CAPP_KEY_RIGHT);
  CHECK_EQ(U.card.id, 7001);
  key('s');                                          /* sure? */
  CHECK_EQ(G.buying, 4);
  CHECK_EQ(G.let_value, 50);
  shot("s20_sell");
  key('n');
  CHECK(js_owns(&J, 7001));
  key('s');
  key('y');
  CHECK(!js_owns(&J, 7001));
  CHECK_EQ((int)J.coins, 150);                       /* a quarter of 200 */
  CHECK(!fakefs_exists("/var/jar/items/7001.itm"));
  CHECK(strstr(saved(), "place 7001") == 0);         /* out of the factory */
  CHECK(strstr(saved(), "sellq 7001") != 0);         /* told with the next poll */
  /* trash the one in jar 2 */
  launch("stuff");
  for (i = 0; i < 8 && U.card.id != 7002; i++) key(CAPP_KEY_RIGHT);
  CHECK_EQ(U.card.id, 7002);
  key('d');
  CHECK_EQ(G.buying, 5);
  shot("s21_trash");
  key('y');
  CHECK(!js_owns(&J, 7002));
  CHECK(strstr(fakefs_get("/var/jar/jar2.txt"), "7002") == 0);
  CHECK(strstr(fakefs_get("/var/jar/jar2.txt"), "place 5 30 0") != 0);   /* the rest stays */
  CHECK(strstr(saved(), "trashq 7002") != 0);
  /* a built-in: nobody pays for it */
  launch("stuff");
  CHECK(U.card.id < 100);
  key('s');
  CHECK_EQ(G.buying, 0);
  CHECK(strstr(U.msg, "Nobody") != 0);
}

/* Tibbs's line, whole: shown over the shop once when it is new, l again. */
void test_jarshop_tibbs_says_it_all(void) {
  const char *line = "Went to the rail yard at dawn and came back with a crate of brass "
                     "birds, a lamp that hums, and a story about a goat I will tell you later.";
  card(100);
  fakefs_put("/var/jar/say.txt", line);
  launch("shop");
  CHECK(G.saying);
  shot("s22_tibbs_says");
  key('x');                                          /* any key */
  CHECK(!G.saying);
  CHECK_EQ(G.view, V_STOCK);
  launch("shop");                                    /* seen: not again */
  CHECK(!G.saying);
  key('l');
  CHECK(G.saying);
}
