/* apps/social.h: the calls an app makes to reach friends, rooms and Chat,
 * and the parsers for what comes back -- against the wire formats in
 * server/friends.py, server/kv.py and server/rooms.py, whose own tests
 * (server/tests/test_friends.py) pin the server's side. */

#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "apps/social.h"

static char s_method[8], s_url[400], s_body[256];
static int s_starts;
static const char *s_reply;
static int s_reply_rc;

static int f_start(const char *m, const char *u, const char *b, const char *ct, const char *br,
                   int ms) {
  (void)ct; (void)br; (void)ms;
  strncpy(s_method, m, sizeof s_method - 1);
  strncpy(s_url, u, sizeof s_url - 1);
  s_url[sizeof s_url - 1] = 0;
  strncpy(s_body, b ? b : "", sizeof s_body - 1);
  s_body[sizeof s_body - 1] = 0;
  s_starts++;
  return 0;
}

static int f_poll(char *o, size_t n) {
  size_t k;
  if (!s_reply) return CAPP_HTTP_PENDING;
  if (s_reply_rc < 0) return s_reply_rc;
  k = strlen(s_reply);
  if (k > n - 1) k = n - 1;
  memcpy(o, s_reply, k);
  o[k] = 0;
  return (int)k;
}

static CardApi A;

static void setup(void) {
  fakeapi_init(&A);
  A.http_start = f_start;
  A.http_poll = f_poll;
  s_starts = 0;
  s_reply = 0;
  s_reply_rc = 0;
  s_method[0] = s_url[0] = s_body[0] = 0;
}

void test_social_requests_are_the_servers_routes(void) {
  setup();
  CHECK_EQ(soc_pub_put(&A, "wordle", "stats", "day 300 3"), 0);
  CHECK(!strcmp(s_method, "POST"));
  CHECK(!strcmp(s_url, "http://srv/kv/put?ns=pub/wordle&key=stats"));
  CHECK(!strcmp(s_body, "day 300 3"));
  soc_pub_each(&A, "wordle", "stats");
  CHECK(!strcmp(s_url, "http://srv/kv/each?ns=pub/wordle&key=stats"));
  soc_share(&A, "Wordle", "Wordle 300 3/6", "arty");
  CHECK(!strcmp(s_url, "http://srv/share?app=Wordle&name=arty"));
  soc_share(&A, "Wordle", "x", 0);
  CHECK(!strcmp(s_url, "http://srv/share?app=Wordle"));
  soc_room_new(&A, "words", "sam", "Sunday game", "board 0");
  CHECK(!strcmp(s_url, "http://srv/room/new?app=words&with=sam&title=Sunday%20game"));
  CHECK(!strcmp(s_body, "board 0"));
  soc_room_state(&A, 7, 3, "b");
  CHECK(!strcmp(s_url, "http://srv/room/state?id=7&if=3"));
  soc_room_state(&A, 7, -1, "b");
  CHECK(!strcmp(s_url, "http://srv/room/state?id=7"));
  soc_room_move(&A, 7, "e4", "your move");
  CHECK(!strcmp(s_url, "http://srv/room/move?id=7&say=your%20move"));
  soc_room_move(&A, 7, "1 2 A", 0);
  CHECK(!strcmp(s_url, "http://srv/room/move?id=7"));
  soc_room_moves(&A, 7, 12);
  CHECK(!strcmp(s_url, "http://srv/room/moves?id=7&after=12"));
  CHECK(!strcmp(s_method, "GET"));
}

void test_social_friends_are_only_the_friends(void) {
  SocFriend f[4];
  int n = soc_friend_parse("sam\tSam\tfriend\t1700000000\n"
                           "kim\tkim\tasking\t0\n"
                           "lee\tLee\tfriend\t0\n", f, 4);
  CHECK_EQ(n, 2);
  CHECK(!strcmp(f[0].name, "sam") && !strcmp(f[0].display, "Sam"));
  CHECK(!strcmp(f[1].name, "lee"));
  CHECK_EQ(soc_friend_parse("", f, 4), 0);
  CHECK_EQ(soc_friend_parse("a\ta\tfriend\t0\nb\tb\tfriend\t0\n", f, 1), 1);
}

void test_social_blocks_carry_their_bytes_newlines_and_all(void) {
  static const char R[] = "alex\talex\t9\nday 300 3\nsam\tSam\t10\n3 1\n2 4\nX\n\n";
  const char *p = R, *end = R + sizeof R - 1, *head, *body;
  char who[16], disp[16];
  int len;
  CHECK(soc_block_next(&p, end, &head, &body, &len));
  tsv_field(head, 0, who, sizeof who);
  CHECK(!strcmp(who, "alex"));
  CHECK_EQ(len, 9);
  CHECK(!memcmp(body, "day 300 3", 9));
  CHECK(soc_block_next(&p, end, &head, &body, &len));
  tsv_field(head, 1, disp, sizeof disp);
  CHECK(!strcmp(disp, "Sam"));
  CHECK_EQ(len, 10);
  CHECK(!memcmp(body, "3 1\n2 4\nX\n", 10));      /* a body may hold newlines */
  CHECK(!soc_block_next(&p, end, &head, &body, &len));
}

void test_social_a_cut_reply_stops_at_its_last_whole_block(void) {
  static const char R[] = "1\talex\t1700000000\t5\n1 1 A\n2\tsam\t1700000001\t5\n1 2";
  const char *p = R, *end = R + sizeof R - 1, *head, *body;
  int len;
  CHECK(soc_block_next(&p, end, &head, &body, &len));
  CHECK(!memcmp(body, "1 1 A", 5));
  CHECK(!soc_block_next(&p, end, &head, &body, &len));
  CHECK(p == R + 26);                             /* where to ask again from */
  {
    const char *q = "bad\tline\n";
    CHECK(!soc_block_next(&q, q + strlen(q), &head, &body, &len));
  }
}

void test_social_the_picker_chooses_a_friend(void) {
  SocPick pk;
  setup();
  soc_pick_open(&A, &pk, "Play with");
  CHECK(pk.open && pk.loading);
  CHECK(!strcmp(s_url, "http://srv/friends"));
  CHECK_EQ(soc_pick_tick(&A, &pk), 0);              /* still on its way */
  soc_pick_key(&pk, CAPP_KEY_DOWN);                 /* nothing to move over yet */
  CHECK_EQ(pk.sel, 0);
  s_reply = "sam\tSam\tfriend\t0\nlee\tLee\tfriend\t0\n";
  CHECK_EQ(soc_pick_tick(&A, &pk), 1);
  CHECK_EQ(pk.n, 2);
  soc_pick_paint(&A, &pk, fakeapi_screen);          /* draws without falling over */
  soc_pick_key(&pk, CAPP_KEY_DOWN);
  soc_pick_key(&pk, CAPP_KEY_DOWN);                 /* stops at the last */
  CHECK_EQ(pk.sel, 1);
  soc_pick_key(&pk, CAPP_KEY_ENTER);
  CHECK(!pk.open);
  CHECK_EQ(pk.done, 1);
  CHECK(!strcmp(pk.chosen.name, "lee"));
}

void test_social_the_picker_cancels_and_says_why_it_is_empty(void) {
  SocPick pk;
  setup();
  soc_pick_open(&A, &pk, 0);
  soc_pick_key(&pk, CAPP_KEY_ESC);                  /* even while loading */
  CHECK(!pk.open);
  CHECK_EQ(pk.done, -1);
  soc_pick_open(&A, &pk, 0);
  s_reply = "";
  s_reply_rc = -403;
  soc_pick_tick(&A, &pk);
  CHECK(pk.failed);
  soc_pick_key(&pk, CAPP_KEY_ENTER);                /* nothing to choose */
  CHECK_EQ(pk.done, -1);
}
