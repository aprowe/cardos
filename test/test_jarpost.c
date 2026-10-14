/* Jar Post on the host: friends (the list, adding by code, a name), sending
 * a gift (postage, the item leaving only on "ok", the postage back when it
 * does not), and the post (parcels collected, checked, acknowledged; a
 * forged one dropped; opening one; a thank-you), against a stand-in server.
 *
 * JAR_DUMP=dir writes its screens as PPM (test/jarfake.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "jarfake.h"

#include "apps/jar/jarpost.h"           /* the post screen, standing alone */

#define T0 1800000000u

static CardApi A;
static char RAN[64], RAN_ARGS[64];

static int f_run(const char *name, const char *args) {
  snprintf(RAN, sizeof RAN, "%s", name);
  snprintf(RAN_ARGS, sizeof RAN_ARGS, "%s", args ? args : "");
  return 0;
}

/* ---- the stand-in server ------------------------------------------------------ */

typedef struct { uint32_t id; char body[1800]; } Msg;
static Msg Q[4];
static int NQ, OFFLINE, GIFT_OK;
static char ME[512];

static char SAID[128], TXS[128], HAVE_SAID[64], BEDS_SAID[64], WHO_SAID[16], BOUGHT_SAID[64];
static int TALK_POLLS, COINS_SAID, ITEM_GOOD;
static char OFFER[128], DEAL_BODY[256], DEAL_PATH[64];
static uint32_t ITEM_ID;
static int post_server(const char *m, const char *path, const char *body, char *out, int cap) {
  if (OFFLINE) return -1;
  if (!strcmp(path, "/jar/pubkey")) return snprintf(out, (size_t)cap, "%s\n", JF_PUBHEX);
  if (!strcmp(path, "/jar/me")) return snprintf(out, (size_t)cap, "%s", ME);
  if (!strncmp(path, "/jar/friend?code=", 17)) {
    strcat(ME, "friend sam\tSam\t0\twaiting\n");
    return snprintf(out, (size_t)cap, "ok sam waiting\n");
  }
  if (!strcmp(path, "/people/name")) return snprintf(out, (size_t)cap, "ok\n");
  if (!strncmp(path, "/jar/gift?to=", 13)) return GIFT_OK ? snprintf(out, (size_t)cap, "ok\n") : -500;
  if (!strncmp(path, "/jar/thanks?to=", 15)) return snprintf(out, (size_t)cap, "ok\n");
  if (!strcmp(path, "/q/peek?q=jar.gifts&max=1")) {
    if (!NQ) return 0;
    return snprintf(out, (size_t)cap, "%u\t-\t%u\t%d\n%s\n", Q[0].id, T0, (int)strlen(Q[0].body), Q[0].body);
  }
  if (!strncmp(path, "/jar/talk", 9) && !strcmp(m, "POST")) {
    const char *b = body ? body : "", *w = strstr(path, "who=");
    snprintf(WHO_SAID, sizeof WHO_SAID, "%s", w ? w + 4 : "");
    COINS_SAID = -1;
    HAVE_SAID[0] = BEDS_SAID[0] = BOUGHT_SAID[0] = 0;
    for (;;) {                                  /* what the jar has, before the words */
      const char *e = strchr(b, '\n');
      if (!e) break;
      if (!strncmp(b, "coins ", 6)) COINS_SAID = atoi(b + 6);
      else if (!strncmp(b, "have ", 5)) snprintf(HAVE_SAID, sizeof HAVE_SAID, "%.*s", (int)(e - b - 5), b + 5);
      else if (!strncmp(b, "bought ", 7)) snprintf(BOUGHT_SAID, sizeof BOUGHT_SAID, "%.*s", (int)(e - b - 7), b + 7);
      else if (!strncmp(b, "beds ", 5)) snprintf(BEDS_SAID, sizeof BEDS_SAID, "%.*s", (int)(e - b - 5), b + 5);
      else break;
      b = e + 1;
    }
    snprintf(SAID, sizeof SAID, "%s", b);
    TALK_POLLS = 0;
    return snprintf(out, (size_t)cap, "pending\n");
  }
  if (!strncmp(path, "/jar/deal?who=", 14) && !strcmp(m, "POST")) {   /* yes or no */
    size_t n = strlen(body ? body : "");
    snprintf(DEAL_BODY, sizeof DEAL_BODY, "%s", body ? body : "");
    snprintf(DEAL_PATH, sizeof DEAL_PATH, "%s", path);
    OFFER[0] = 0;
    if (n >= 3 && !strcmp(body + n - 3, "yes")) {
      snprintf(TXS, sizeof TXS, "tx 1 usespores 12\n");
      return snprintf(out, (size_t)cap, "ok\n");
    }
    return snprintf(out, (size_t)cap, "no\n");
  }
  if (!strncmp(path, "/jar/item?id=", 13)) {   /* a deal's item, made again or new */
    JItem it;
    int k;
    jf_item(&it, (uint32_t)atoi(path + 13), "Moth Lamp II", "glowing,night");
    ITEM_ID = it.id;
    k = jf_signed_b64(&it, out, cap, ITEM_GOOD);
    return k;
  }
  if (!strncmp(path, "/jar/talk", 9)) {
    if (SAID[0] && TALK_POLLS++ < 1) return snprintf(out, (size_t)cap, "pending\nday\tA crate came in.\nme\t%s\n", SAID);
    if (!SAID[0]) return snprintf(out, (size_t)cap, "ok\nday\tA crate came in.\n");
    return snprintf(out, (size_t)cap, "ok\nday\tA crate came in.\nme\t%s\nhim\tHm, I know a fellow down at the rail yard who might.\n%s%s%s%s",
                    SAID, TXS, OFFER[0] ? "offer\t" : "", OFFER, OFFER[0] ? "\n" : "");
  }
  if (!strncmp(path, "/q/ack?q=jar.gifts&upto=", 24)) {
    uint32_t upto = (uint32_t)atoi(path + 24);
    while (NQ && Q[0].id <= upto) { memmove(Q, Q + 1, sizeof Q[0] * (size_t)(NQ - 1)); NQ--; }
    return snprintf(out, (size_t)cap, "ok 1\n");
  }
  return -404;
}

static void queue_gift(uint32_t qid, uint32_t item, const char *from, const char *note, int good) {
  JItem it;
  Msg *q = &Q[NQ++];
  int k;
  jf_item(&it, item, "Fog Lantern", "misty,night");
  snprintf(it.gifted, sizeof it.gifted, "%s", from);
  it.flags = JIF_GIFT;
  q->id = qid;
  k = snprintf(q->body, sizeof q->body, "%s\t%s\t", from, note);
  jf_signed_b64(&it, q->body + k, (int)sizeof q->body - k, good);
}

/* A card with a jar, coins, and one item from the shop (signed) as 7001. */
static void card(uint32_t coins) {
  static Jar j;
  static char t[2048];
  JItem it;
  uint8_t raw[JI_MAX];
  int n;
  fakeapi_init(&A);
  fakefs_mem(&A);
  jf_server(&A);
  jf_screen(&A);
  jf_handler = post_server;
  A.run = f_run;
  RAN[0] = 0;
  NQ = 0;
  OFFLINE = 0;
  GIFT_OK = 1;
  SAID[0] = 0;
  TXS[0] = 0;
  OFFER[0] = DEAL_BODY[0] = DEAL_PATH[0] = 0;
  ITEM_GOOD = 1;
  ITEM_ID = 0;
  snprintf(ME, sizeof ME, "code K7Q2XP\nname Alex\nfriend maya\tMaya\t%u\tmutual\nfriend bo\tBo\t0\twaiting\n",
           T0 - 3 * 3600);
  fakeapi_epoch = T0;
  fakeapi_ticks = 1000;
  jst_dirs(&A);
  memset(&j, 0, sizeof j);
  js_init(&j, 1);
  jf_item(&it, 7001, "Moth Lamp", "glowing,night");
  n = jf_signed(&it, raw, 1);
  jst_put(&A, "/var/jar/items/7001.itm", raw, n);
  js_own(&j, 7001);
  j.shelf[2] = 7001;
  j.coins = coins;
  jst_save(&A, &j, t, sizeof t);
}

static void launch(const char *screen) {
  char *argv[2];
  argv[0] = "jarpost";
  argv[1] = (char *)screen;
  JF_PENDING = 0;                    /* the OS drops a closed app's request */
  (void)argv;
  api = &A;
  post_open(screen);
}

static void key(int k) { post_key(0, (uint8_t)k); }
static void type(const char *s) { while (*s) key((uint8_t)*s++); }

static void tick(int ms) {
  int t;
  for (t = 0; t < ms; t += 5) { fakeapi_ticks += 5; post_tick(0, fakeapi_ticks); }
}

static void shot(const char *name) {
  CRect all = { 0, 0, SW, SHT };
  post_paint(0, all);
  jf_dump(name);
}

static const char *saved(void) { return fakefs_get("/var/jar/jar.txt"); }

/* ---- friends ------------------------------------------------------------------------ */

void test_jarpost_friends_list_add_and_name(void) {
  card(0);
  launch("friends");
  tick(20);
  CHECK_EQ(jf_count("/jar/me"), 1);
  CHECK(strcmp(G.code, "K7Q2XP") == 0);
  CHECK(strcmp(G.me, "Alex") == 0);
  CHECK_EQ(G.nfr, 2);
  CHECK(G.fr[0].mutual && !G.fr[1].mutual);
  CHECK(fakefs_exists("/var/jar/friends.txt"));
  shot("p01_friends");
  /* add by code */
  key('a');
  CHECK(post_wants_text(0));                 /* ; , . / are letters while typing */
  type("zz9;");
  shot("p02_add");
  key(CAPP_KEY_ENTER);
  CHECK(!post_wants_text(0));
  CHECK_EQ(jf_count("/jar/friend?code=zz9%3B"), 1);
  tick(20);
  CHECK(strstr(U.msg, "sam") != 0);
  tick(20);
  CHECK_EQ(G.nfr, 3);                        /* asked again */
  /* a name: eight at most */
  key('n');
  type("Alexandra");
  CHECK(strcmp(G.input, "Alexandr") == 0);
  key(CAPP_KEY_ENTER);
  {
    const JfReq *r = jf_last("/people/name");
    CHECK(r && strcmp(r->body, "Alexandr") == 0);
  }
  /* offline: the list kept from last time */
  OFFLINE = 1;
  launch("friends");
  tick(20);
  CHECK_EQ(G.nfr, 3);
  CHECK(U.msg[0] != 0);
  key(CAPP_KEY_ESC);
  CHECK(strcmp(RAN, "Jar Factory") == 0);
}

/* ---- gifts -------------------------------------------------------------------------------- */

void test_jarpost_a_gift_leaves_only_when_the_server_has_it(void) {
  const JfReq *r;
  uint8_t back[JI_MAX];
  int n;
  card(25);
  launch("gift 7001");
  CHECK_EQ(G.view, V_GIFT);
  tick(20);                                  /* who the friends are */
  CHECK_EQ(mutual_n(), 1);
  shot("p03_gift_pick");
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.gift_step, 1);
  type("for your shelf, love M");
  type("XXXXXXXX");                          /* past 24: dropped */
  shot("p04_gift_note");
  key(CAPP_KEY_ENTER);
  CHECK(strcmp(G.note, "for your shelf, love MXXX") == 0 || (int)strlen(G.note) == 24);
  CHECK_EQ(G.gift_step, 2);
  shot("p05_gift_sure");
  /* refused the first time: the postage comes back, the item stays */
  GIFT_OK = 0;
  key('y');
  CHECK_EQ(J.coins, 15);                     /* in hand while it goes */
  tick(20);
  CHECK_EQ(J.coins, 25);
  CHECK(js_owns(&J, 7001));
  CHECK(fakefs_exists("/var/jar/items/7001.itm"));
  CHECK(U.msg[0] != 0);
  CHECK_EQ(G.gift_step, 2);
  /* then it goes */
  GIFT_OK = 1;
  key('y');
  r = jf_last("/jar/gift?to=maya");
  CHECK(r != 0);
  CHECK(!strncmp(r->body, G.note, strlen(G.note)));
  n = b64_decode(strchr(r->body, '\n') + 1, back, sizeof back);
  CHECK_EQ(n, fakefs_size("/var/jar/items/7001.itm"));
  tick(20);
  CHECK_EQ(J.coins, 15);
  CHECK(!js_owns(&J, 7001));
  CHECK(!fakefs_exists("/var/jar/items/7001.itm"));
  CHECK_EQ(J.shelf[2], 0);                   /* off the shelf too */
  CHECK(strstr(saved(), "coins 15") != 0);
  CHECK(strstr(saved(), "7001") == 0);
  CHECK(strstr(U.msg, "Maya") != 0);
}

void test_jarpost_hand_made_things_do_not_travel(void) {
  static JIo io;
  card(100);
  jst_from_builtin(&BUILTINS[0], &io.it);
  jst_item_write(&A, &io);
  launch("gift 1");
  CHECK(G.view != V_GIFT);
  CHECK(U.msg[0] != 0);
}

/* ---- the post ------------------------------------------------------------------------------- */

void test_jarpost_collects_checks_and_opens_parcels(void) {
  card(0);
  queue_gift(41, 8101, "Maya", "found it by the pond", 1);
  queue_gift(42, 8102, "Bo", "trust me", 0);            /* forged */
  queue_gift(43, 8103, "Maya", "", 1);
  launch("mail");
  tick(100);
  CHECK_EQ(jf_count("/jar/pubkey"), 1);
  CHECK(fakefs_exists("/var/jar/server.pub"));
  CHECK_EQ(NQ, 0);                                       /* all acknowledged */
  CHECK_EQ(jf_count("/q/ack?q=jar.gifts&upto="), 3);
  CHECK_EQ(G.mail_n, 2);                                 /* the forged one dropped */
  CHECK(fakefs_exists("/var/jar/mail/41.itm"));
  CHECK(!fakefs_exists("/var/jar/mail/42.itm"));
  CHECK(fakefs_exists("/var/jar/mail/43.itm"));
  CHECK_EQ(J.parcels, 2);
  CHECK(strstr(saved(), "parcels 2") != 0);
  CHECK(!js_owns(&J, 8101));                             /* a parcel until opened */
  shot("p06_mail");
  /* newest first: down to Maya's first one, and open it */
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_CARD);
  CHECK(js_owns(&J, 8101));
  CHECK(fakefs_exists("/var/jar/items/8101.itm"));
  CHECK(!fakefs_exists("/var/jar/mail/41.itm"));
  CHECK_EQ(J.parcels, 1);
  CHECK(strstr(saved(), "parcels 1") != 0);
  CHECK(jst_item_read(&A, 8101, &IO) > 0 && IO.it.sig_len == 64);
  shot("p07_gift_card");
  /* a thank-you */
  key('t');
  CHECK_EQ(jf_count("/jar/thanks?to=Maya&id=8101"), 1);
  tick(20);
  CHECK(M[G.card_mail].thanked);
  CHECK(strstr(fakefs_get("/var/jar/mail.txt"), "8101\t") != 0);
  key('t');
  CHECK_EQ(jf_count("/jar/thanks"), 1);                  /* once */
  /* Enter: put it in the jar */
  key(CAPP_KEY_ENTER);
  CHECK(strcmp(RAN, "Jar Factory") == 0);
  CHECK(strstr(saved(), "decor 8101") != 0);
  /* reopened: the mail is still there */
  launch("mail");
  tick(20);
  CHECK_EQ(G.mail_n, 2);
  CHECK_EQ(J.parcels, 1);
}

void test_jarpost_a_parcel_that_cannot_be_checked_stays_on_the_server(void) {
  card(0);
  queue_gift(51, 8201, "Maya", "hi", 1);
  A.sig_verify = 0;                                      /* no way to check here */
  {
    /* the key is there, but nothing can check against it */
    jst_put(&A, JST_PUB, JF_PUBHEX, 130);
  }
  launch("mail");
  tick(100);
  CHECK_EQ(NQ, 1);
  CHECK_EQ(jf_count("/q/ack"), 0);
  CHECK_EQ(G.mail_n, 0);
}

void test_jarpost_offline_the_post_says_so(void) {
  card(0);
  OFFLINE = 1;
  launch("mail");
  tick(100);
  CHECK(U.msg[0] != 0);
  CHECK_EQ(G.mail_n, 0);
  shot("p08_mail_empty");
}

/* ---- sending from the friends list, and talking to Tibbs ------------------------------ */

void test_jarpost_a_gift_starts_from_a_friend(void) {
  const JfReq *r;
  card(100);
  launch("friends");
  tick(20);
  CHECK_EQ(G.nfr, 2);
  key(CAPP_KEY_DOWN);                         /* Bo has not added us back */
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_FRIENDS);
  CHECK(strstr(U.msg, "not added you back") != 0);
  key(CAPP_KEY_UP);
  shot("p20_friends_pick");
  key(CAPP_KEY_ENTER);                        /* Maya: which of your things? */
  CHECK_EQ(G.view, V_PICK);
  CHECK_EQ(G.npk, 1);                         /* the signed one; hand-made ones stay home */
  CHECK(strcmp(G.pkname[0], "Moth Lamp") == 0);
  shot("p21_pick");
  key(CAPP_KEY_ENTER);                        /* straight to the note: the friend is chosen */
  CHECK_EQ(G.view, V_GIFT);
  CHECK_EQ(G.gift_step, 1);
  type("hi");
  key(CAPP_KEY_ENTER);
  key('y');
  tick(20);
  r = jf_last("/jar/gift?to=");
  CHECK(r && strcmp(r->path, "/jar/gift?to=maya") == 0);
  CHECK(!js_owns(&J, 7001));
  CHECK_EQ(G.view, V_FRIENDS);
}

void test_jarpost_talking_to_tibbs(void) {
  const JfReq *r;
  card(0);
  launch("talk");
  CHECK_EQ(G.view, V_TALK);
  tick(20);
  CHECK(TK_N >= 1);                           /* his line of the day */
  CHECK(strcmp(TK[0], "A crate came in.") == 0);
  type("got any gears?");
  key(CAPP_KEY_ENTER);
  tick(20);
  r = jf_last("/jar/talk");
  CHECK(r && strcmp(r->method, "POST") == 0 && strcmp(SAID, "got any gears?") == 0);
  CHECK_EQ(COINS_SAID, 0);                       /* what is in hand goes with it: he can deal */
  CHECK(TK_PENDING);
  shot("p22_talk_waiting");
  tick(3100);                                 /* still thinking */
  CHECK(TK_PENDING);
  tick(3100);
  CHECK(!TK_PENDING);
  CHECK(strcmp(TK[TK_N - 1], "the rail yard who might.") == 0 || strstr(TK[TK_N - 2], "fellow") != 0);
  shot("p23_talk");
  key(CAPP_KEY_ESC);
  CHECK(strcmp(RAN, "Jar Factory") == 0);
}


/* The Town: the lane's four; Pip takes spores and alters the Moth Lamp in
 * place -- the same id, the new record fetched, checked and written. */
void test_jarpost_the_town_and_a_job_done(void) {
  card(50);
  launch("town");
  J.res[JRS_SPORES] = 6;                         /* as if the save had said so */
  J.res[JRS_TWIGS] = 2;
  J.bed[0].lv = 2;
  CHECK_EQ(G.view, V_TOWN);
  shot("p30_town");
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_TALK);
  CHECK_EQ(G.npc, 1);
  tick(400);                                     /* after the Town's job list */
  CHECK(jf_last("/jar/talk") && strstr(jf_last("/jar/talk")->path, "who=pip") != 0);
  snprintf(TXS, sizeof TXS, "tx 1 usespores 4\ntx 2 remake 7001\ntx 3 get 5\n");
  type("make my lamp glow green");
  key(CAPP_KEY_ENTER);
  tick(20);
  CHECK(strcmp(WHO_SAID, "pip") == 0);
  CHECK(BOUGHT_SAID[0] == 0);                    /* nothing bought from the shop */
  CHECK_EQ(COINS_SAID, 50);
  CHECK(strcmp(HAVE_SAID, "spores=6 nectar=0 twigs=2") == 0);
  CHECK(strcmp(BEDS_SAID, "berry2,berry1") == 0);
  CHECK(strcmp(SAID, "make my lamp glow green") == 0);
  shot("p31_pip");
  tick(9000);
  CHECK(!TK_PENDING);
  CHECK_EQ(J.res[JRS_SPORES], 2);
  CHECK_EQ(ITEM_ID, 7001);                       /* fetched */
  CHECK_EQ(J.txseen, 3);                         /* and the one after it, heard again */
  CHECK_EQ(J.coins, 55);
  {
    JIo io;
    CHECK(jst_item_read(&A, 7001, &io) >= 0);
    CHECK(strcmp(io.it.name, "Moth Lamp II") == 0);
  }
  CHECK(js_owns(&J, 7001));
  key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_TOWN);                      /* back to the lane, not the jar */
  key(CAPP_KEY_ESC);
  CHECK(strcmp(RAN, "Jar Factory") == 0);
}

/* A deal is an offer until the player says yes: shown in the code's words,
 * n turns it down with nothing paid, y sends what the jar has and "yes". */
void test_jarpost_an_offer_waits_for_yes(void) {
  card(50);
  launch("town");
  J.res[JRS_SPORES] = 20;
  {
    /* bought from the shop on show, not yet told: talk tells it, so Pip can
     * find a thing bought an hour ago (2026-10-14) */
    JStock st;
    char text[384];
    memset(&st, 0, sizeof st);
    strcpy(st.date, "20261014-3");
    st.n = 3;
    st.id[0] = 9001; st.id[1] = 9002; st.id[2] = 9003;
    st.sold = 1 | 4;
    jst_stock_save(&A, &st, text, (int)sizeof text);
  }
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);                           /* Pip */
  tick(400);
  snprintf(OFFER, sizeof OFFER, "Pay 12 spores: Pip alters Moth Lamp: make it glow. Ready in 1h.");
  type("can you make my lamp glow?");
  key(CAPP_KEY_ENTER);
  tick(9000);
  CHECK(strcmp(BOUGHT_SAID, "9001,9003") == 0);
  CHECK(!TK_PENDING);
  CHECK(strstr(G.offer, "Pay 12 spores") == G.offer);
  CHECK_EQ(J.res[JRS_SPORES], 20);               /* nothing yet */
  shot("p32_offer");
  type("x");                                     /* not an answer: still asking */
  CHECK(G.offer[0] != 0);
  CHECK_EQ(G.inlen, 0);                          /* and not typed either */
  key('n');
  tick(400);
  CHECK(strstr(DEAL_PATH, "who=pip") != 0);
  CHECK(strstr(DEAL_BODY, "bought 9001,9003\ncoins ") == DEAL_BODY);
  CHECK(strstr(DEAL_BODY, "have spores=20 ") != 0);
  CHECK(strstr(DEAL_BODY, "\nno") != 0);
  CHECK_EQ(J.res[JRS_SPORES], 20);
  CHECK(G.offer[0] == 0);
  /* asked again, and this time yes */
  snprintf(OFFER, sizeof OFFER, "Pay 12 spores: Pip alters Moth Lamp. Ready in 1h.");
  type("ok, go on then");
  key(CAPP_KEY_ENTER);
  tick(9000);
  CHECK(G.offer[0] != 0);
  key('y');
  tick(2000);
  CHECK(strstr(DEAL_BODY, "\nyes") != 0);
  CHECK_EQ(J.res[JRS_SPORES], 8);                /* the tx that came of it */
  CHECK_EQ(J.txseen, 1);
}

/* Wren's furniture arrives as a new thing; one that fails its signature is
 * not taken, and the deal waits. */
void test_jarpost_a_delivery_is_checked(void) {
  card(50);
  launch("town");
  key(CAPP_KEY_DOWN); key(CAPP_KEY_DOWN); key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.npc, 3);
  tick(400);
  ITEM_GOOD = 0;
  snprintf(TXS, sizeof TXS, "tx 1 give 9100\n");
  type("a shelf please");
  key(CAPP_KEY_ENTER);
  tick(9000);
  CHECK(!js_owns(&J, 9100));
  CHECK_EQ(J.txseen, 0);                         /* not done: asked for again later */
  ITEM_GOOD = 1;
  type("hello?");
  key(CAPP_KEY_ENTER);
  tick(9000);
  CHECK(js_owns(&J, 9100));
  CHECK_EQ(J.txseen, 1);
  CHECK(fakefs_exists("/var/jar/items/9100.itm"));
}

/* A deal struck in talk: he took 120 coins and the Moth Lamp, gave 30 back;
 * each done once, even when the same lines come again. */
void test_jarpost_a_deal_in_talk_is_carried_out_once(void) {
  card(500);
  launch("talk");
  tick(20);
  snprintf(TXS, sizeof TXS, "tx 1 pay 120\ntx 2 lose 7001\ntx 3 get 30\n");
  type("120 for owls, and the lamp?");
  key(CAPP_KEY_ENTER);
  tick(20);
  CHECK_EQ(COINS_SAID, 500);
  CHECK(strcmp(SAID, "120 for owls, and the lamp?") == 0);
  tick(6500);
  CHECK(!TK_PENDING);
  CHECK_EQ(J.coins, 500 - 120 + 30);
  CHECK_EQ(J.txseen, 3);
  CHECK(!js_owns(&J, 7001));
  CHECK(fakefs_get("/var/jar/items/7001.itm") == 0);
  CHECK(strstr(fakefs_get("/var/jar/jar.txt"), "txseen 3") != 0);
  CHECK(strstr(jf_last("/jar/talk")->path, "tx=") != 0);
  /* heard again: nothing twice */
  type("thanks");
  key(CAPP_KEY_ENTER);
  tick(6500);
  CHECK_EQ(J.coins, 410);
  /* a pay he asks for that is more than is here takes what there is */
  snprintf(TXS, sizeof TXS, "tx 4 pay 9999\n");
  type("deal");
  key(CAPP_KEY_ENTER);
  tick(6500);
  CHECK_EQ(J.coins, 0);
  CHECK_EQ(J.txseen, 4);
}
