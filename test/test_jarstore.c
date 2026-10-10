/* Jar Factory on the card: apps/jarstore.h, and the parts of apps/jarsim.h
 * the companion apps lean on -- the garden, the shelf, the save's new lines
 * and an app that keeps the jar without placing anything. */
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "jarfake.h"
#include "apps/jarstore.h"

#define T0 1800000000u

static CardApi A;
static Jar J, K;
static JIo IO;
static char TEXT[2048];

static void card(void) {
  fakeapi_init(&A);
  fakefs_mem(&A);
  jf_server(&A);
  jst_dirs(&A);
}

void test_jarstore_items_round_trip_and_builtins_are_whole(void) {
  int i;
  card();
  for (i = 0; i < JB_COUNT; i++) {
    jst_from_builtin(&BUILTINS[i], &IO.it);
    CHECK_EQ(jst_item_write(&A, &IO), 0);
  }
  CHECK(fakefs_exists("/var/jar/items/5.itm"));
  CHECK(jst_item_read(&A, 5, &IO) > JI_HDR);
  CHECK(strcmp(IO.it.name, "Pond Frog") == 0);
  CHECK(IO.it.flags & JIF_BUILTIN);
  CHECK_EQ(jst_item_read(&A, 99, &IO), -1);
  /* the starters' own table is the price-0 rows of the whole one */
  CHECK_EQ(JB_NSTART, 4);
  for (i = 0; i < JB_NSTART; i++) {
    CHECK_EQ(JB_STARTERS[i].price, 0);
    CHECK(jst_builtin(JB_STARTERS[i].id) != 0);
  }
}

void test_jarstore_a_servers_record_is_kept_byte_for_byte(void) {
  uint8_t raw[JI_MAX];
  int n;
  card();
  jf_item(&IO.it, 7001, "Moth Lamp", "glowing,night");
  n = jf_signed(&IO.it, raw, 1);
  CHECK_EQ(jst_put(&A, "/var/jar/items/7001.itm", raw, n), 0);
  CHECK_EQ(fakefs_size("/var/jar/items/7001.itm"), n);
  CHECK_EQ(jst_item_read(&A, 7001, &IO), n);
  CHECK(memcmp(IO.raw, raw, (size_t)n) == 0);
  CHECK_EQ(IO.it.sig_len, 64);
}

void test_jarstore_verify_checks_the_signed_message(void) {
  uint8_t raw[JI_MAX], pub[65], msg[JI_MAX];
  int n;
  card();
  CHECK_EQ(jst_pub_parse(JF_PUBHEX, pub), 0);
  CHECK_EQ(pub[0], 4);
  CHECK_EQ(jst_pub_parse("04abc", pub), -1);
  jf_item(&IO.it, 7002, "Thing", "");
  n = jf_signed(&IO.it, raw, 1);
  CHECK_EQ(jst_verify(&A, pub, raw, n, msg), 1);
  /* memory a script wrote does not break it */
  raw[140] = 0x7F;
  CHECK_EQ(jst_verify(&A, pub, raw, n, msg), 1);
  /* anything else does */
  raw[30] ^= 1;
  CHECK_EQ(jst_verify(&A, pub, raw, n, msg), 0);
  raw[30] ^= 1;
  n = jf_signed(&IO.it, raw, 0);
  CHECK_EQ(jst_verify(&A, pub, raw, n, msg), 0);
  /* an unsigned record is not the server's */
  jst_from_builtin(&BUILTINS[0], &IO.it);
  n = jitem_encode(&IO.it, raw, JI_MAX);
  CHECK_EQ(jst_verify(&A, pub, raw, n, msg), 0);
}

void test_jarstore_prices_are_in_the_spec_range_and_steady(void) {
  JItem it;
  uint32_t id;
  int lo = 1000, hi = 0;
  for (id = 1; id < 3000; id += 7) {
    int p;
    jf_item(&it, id, "x", "");
    it.nframes = (uint8_t)(1 + id % 4);
    p = jst_price(&it);
    if (p < lo) lo = p;
    if (p > hi) hi = p;
    CHECK_EQ(p % 5, 0);
    CHECK_EQ(p, jst_price(&it));
  }
  CHECK(lo >= 45);
  CHECK(hi <= 200);
  CHECK(hi - lo > 80);
}

void test_jarstore_stock_file(void) {
  JStock s, t;
  card();
  jst_stock_load(&A, &s, TEXT, sizeof TEXT);      /* none yet: the hand-made batch */
  CHECK_EQ(s.n, 8);
  CHECK_EQ(s.date[0], 0);
  CHECK_EQ(s.price[0], 60);
  memset(&s, 0, sizeof s);
  strcpy(s.date, "2026-10-09");
  strcpy(s.tags, "rainy, cosy, glowing");
  s.gen = 9;
  s.sold = 5;
  s.n = 2;
  s.id[0] = 41; s.price[0] = 120;
  s.id[1] = 42; s.price[1] = 55;
  CHECK_EQ(jst_stock_save(&A, &s, TEXT, sizeof TEXT), 0);
  jst_stock_load(&A, &t, TEXT, sizeof TEXT);
  CHECK(strcmp(t.date, "2026-10-09") == 0);
  CHECK(strcmp(t.tags, "rainy, cosy, glowing") == 0);
  CHECK_EQ(t.gen, 9);
  CHECK_EQ(t.n, 2);
  CHECK_EQ(t.id[1], 42);
  CHECK_EQ(t.price[0], 120);
  CHECK(jst_sold(&t, &J, 0));
  CHECK(!jst_sold(&t, &J, 1));
}

void test_jarstore_mail_file_and_room(void) {
  JMail m[JST_MAIL_MAX], r[JST_MAIL_MAX];
  int n, i;
  card();
  memset(m, 0, sizeof m);
  CHECK_EQ(jst_mail_load(&A, r, TEXT, sizeof TEXT), 0);
  for (i = 0; i < 3; i++) {
    m[i].qid = 10 + (uint32_t)i;
    m[i].item = 500 + (uint32_t)i;
    m[i].at = T0;
    m[i].opened = (uint8_t)(i == 0);
    snprintf(m[i].from, sizeof m[i].from, "Maya");
    snprintf(m[i].note, sizeof m[i].note, "for your jar %d", i);
  }
  CHECK_EQ(jst_mail_save(&A, m, 3, TEXT, sizeof TEXT), 0);
  memset(r, 0, sizeof r);
  n = jst_mail_load(&A, r, TEXT, sizeof TEXT);
  CHECK_EQ(n, 3);
  CHECK_EQ(r[2].item, 502);
  CHECK_EQ(r[0].opened, 1);
  CHECK(strcmp(r[1].note, "for your jar 1") == 0);
  CHECK(strcmp(r[2].from, "Maya") == 0);
  /* full: the oldest opened one makes room; all unopened, none does */
  n = JST_MAIL_MAX;
  for (i = 0; i < n; i++) { m[i].opened = (uint8_t)(i == 3); m[i].item = 900 + (uint32_t)i; }
  CHECK_EQ(jst_mail_room(m, &n), 0);
  CHECK_EQ(n, JST_MAIL_MAX - 1);
  CHECK_EQ(m[3].item, 904);
  m[n++].item = 1;
  for (i = 0; i < n; i++) m[i].opened = 0;
  CHECK_EQ(jst_mail_room(m, &n), -1);
  /* a note keeps no tabs or newlines */
  {
    char s[] = "a\tb\nc";
    jst_clean(s);
    CHECK(strcmp(s, "a b c") == 0);
  }
}

void test_jarstore_garden_grows_in_real_time(void) {
  int mix[JPL_KINDS];
  memset(&J, 0, sizeof J);
  js_init(&J, 3);
  J.nbeds = 4;
  CHECK_EQ(js_garden_mix(&J, mix), 4);              /* the starting beds are grown */
  CHECK_EQ(mix[JPL_BERRY], 1);
  CHECK_EQ(mix[JPL_SHROOM], 1);
  js_plant(&J, 1, JPL_CACTUS, T0);
  CHECK(J.bed[1].young);
  CHECK_EQ(js_garden_mix(&J, mix), 3);
  CHECK_EQ(mix[JPL_CACTUS], 0);
  CHECK_EQ(js_bed_days(&J.bed[1], T0 + 3600), 3);
  CHECK_EQ(js_bed_days(&J.bed[1], T0 + 86400 * 2 + 10), 1);
  js_settle_beds(&J, T0 + 86400 * 2);
  CHECK(J.bed[1].young);
  js_settle_beds(&J, T0 + 86400 * 3);
  CHECK(!J.bed[1].young);
  CHECK_EQ(J.bed[1].at, 0);
  CHECK_EQ(js_garden_mix(&J, mix), 4);
  CHECK_EQ(mix[JPL_CACTUS], 1);
  /* with no clock a plant waits, and starts counting when one comes */
  js_plant(&J, 2, JPL_FERN, 0);
  CHECK_EQ(J.bed[2].at, 1);
  CHECK_EQ(js_bed_days(&J.bed[2], 0), -1);
  js_settle_beds(&J, 0);
  CHECK(J.bed[2].young);
  js_settle_beds(&J, T0);
  CHECK_EQ(J.bed[2].at, T0);
  CHECK(J.bed[2].young);
}

void test_jarstore_a_young_bed_makes_no_jam(void) {
  int i, picked = 0;
  memset(&J, 0, sizeof J);
  js_init(&J, 5);
  js_plant(&J, 0, JPL_FLOWER, T0);
  js_plant(&J, 1, JPL_FLOWER, T0);
  CHECK_EQ(js_rate_ph(&J), 0);
  for (i = 0; i < 120 * JS_HZ; i++) {
    js_step(&J);
    if (J.moss[0].st == M_TO_BED) picked = 1;
  }
  CHECK(!picked);
  CHECK_EQ(J.shipped, 0);
  CHECK(!J.bed[0].ready);
  /* grown, it makes jam as before */
  js_settle_beds(&J, T0 + JS_MATURE_S);
  CHECK(js_rate_ph(&J) > 0);
  for (i = 0; i < 300 * JS_HZ; i++) js_step(&J);
  CHECK(J.shipped > 0);
}

void test_jarstore_save_keeps_the_new_lines(void) {
  int n;
  memset(&J, 0, sizeof J);
  js_init(&J, 7);
  J.coins = 321;
  J.nbeds = 3;
  js_plant(&J, 2, JPL_SHROOM, T0);
  J.shelf[0] = 12; J.shelf[3] = 7001;
  J.parcels = 2;
  J.gseen = 77;
  J.decor = 7001;
  js_own(&J, 12);
  js_own(&J, 7001);
  J.nwant = 1;                                       /* saved as placed, not placed */
  J.want[0].id = 12; J.want[0].x = 50; J.want[0].y = 0;
  n = js_save(&J, TEXT, sizeof TEXT);
  CHECK(n > 0 && n < (int)sizeof TEXT);
  memset(&K, 0, sizeof K);
  js_init(&K, 8);
  CHECK_EQ(js_load(&K, TEXT), 0);
  CHECK_EQ(K.coins, 321);
  CHECK_EQ(K.bed[2].type, JPL_SHROOM);
  CHECK_EQ(K.bed[2].at, T0);
  CHECK(K.bed[2].young);
  CHECK(!K.bed[0].young);
  CHECK_EQ(K.shelf[0], 12);
  CHECK_EQ(K.shelf[3], 7001);
  CHECK_EQ(K.parcels, 2);
  CHECK_EQ(K.gseen, 77);
  CHECK_EQ(K.decor, 7001);
  CHECK_EQ(K.nwant, 1);                             /* the unplaced "place" line survived */
  CHECK_EQ(K.want[0].id, 12);
  CHECK(js_in_jar(&K, 12));
  CHECK_EQ(js_on_shelf(&K, 7001), 3);
  /* a gift gone: out of everything */
  js_disown(&K, 12);
  CHECK(!js_owns(&K, 12));
  CHECK(!js_in_jar(&K, 12));
  CHECK_EQ(js_on_shelf(&K, 12), -1);
  CHECK_EQ(K.nowned, 1);
  /* an old save, before the garden: every bed grown */
  memset(&K, 0, sizeof K);
  js_init(&K, 9);
  CHECK_EQ(js_load(&K, "jar 1\ncoins 5\nup 3 1 2 0\ngrow 100 200 300\nown 1 2\n"), 0);
  CHECK(!K.bed[0].young && !K.bed[2].young);
  CHECK_EQ(K.parcels, 0);
}

void test_jarstore_load_and_save_through_the_card(void) {
  card();
  memset(&J, 0, sizeof J);
  js_init(&J, 1);
  CHECK_EQ(jst_load(&A, &J, TEXT, sizeof TEXT), -1);   /* no jar yet */
  J.coins = 9;
  fakeapi_epoch = T0;
  CHECK_EQ(jst_save(&A, &J, TEXT, sizeof TEXT), 0);
  CHECK_EQ(J.seen, T0);
  memset(&K, 0, sizeof K);
  js_init(&K, 2);
  CHECK_EQ(jst_load(&A, &K, TEXT, sizeof TEXT), 0);
  CHECK_EQ(K.coins, 9);
  CHECK_EQ(K.seen, T0);
}
