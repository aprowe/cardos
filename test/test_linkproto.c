/* Two devices through a fake radio: kernel/net/linkproto.c.
 *
 * The air is a queue of frames, each addressed to one device or everyone.
 * It can drop every Nth frame, or everything from one side, so the
 * retransmits, the acknowledgements and "lost them" all get exercised. */
#include <string.h>

#include "tinytest.h"
#include "kernel/net/linkproto.h"

#define AIR 256

typedef struct {
  int     from;            /* 0 or 1 */
  int     to;              /* 0, 1, or -1 for everyone */
  uint8_t f[LP_FRAME];
  int     len;
} Frame;

static Frame air[AIR];
static int nair, sent, drop_every, deaf[2];
static LinkProto dev[2];
static const uint8_t MAC[2][6] = { { 2, 0, 0, 0, 0, 1 }, { 2, 0, 0, 0, 0, 2 } };
static uint32_t now;

static void tx(void *ctx, const uint8_t *mac, const uint8_t *f, int len) {
  int from = (int)(intptr_t)ctx;
  Frame *a;
  sent++;
  if (drop_every && sent % drop_every == 0) return;
  if (nair >= AIR) return;
  a = &air[nair++];
  a->from = from;
  a->to = !mac ? -1 : !memcmp(mac, MAC[0], 6) ? 0 : !memcmp(mac, MAC[1], 6) ? 1 : 9;
  memcpy(a->f, f, (size_t)len);
  a->len = len;
}

/* Deliver what is in the air -- frames sent while delivering wait their turn. */
static void deliver(void) {
  static Frame was[AIR];
  int n = nair, i, d;
  memcpy(was, air, sizeof(Frame) * (size_t)n);
  nair = 0;
  for (i = 0; i < n; i++)
    for (d = 0; d < 2; d++) {
      if (d == was[i].from || deaf[d]) continue;
      if (was[i].to == -1 || was[i].to == d) lp_rx(&dev[d], MAC[was[i].from], was[i].f, was[i].len, now);
    }
}

/* `ms` of time, 10 at a time, the radio delivering as it goes. */
static void run(uint32_t ms) {
  uint32_t end = now + ms;
  while (now < end) {
    now += 10;
    lp_tick(&dev[0], now);
    lp_tick(&dev[1], now);
    deliver();
  }
}

static void boot(void) {
  memset(air, 0, sizeof air);
  nair = sent = drop_every = 0;
  deaf[0] = deaf[1] = 0;
  now = 1000;
  lp_init(&dev[0], MAC[0], tx, (void *)(intptr_t)0, 7);
  lp_init(&dev[1], MAC[1], tx, (void *)(intptr_t)1, 9);
  lp_open(&dev[0], "quoridor", "Alex", now);
  lp_open(&dev[1], "quoridor", "Britney", now);
}

static void pair(void) {
  boot();
  run(600);
  lp_invite(&dev[0], 0, now);
  run(100);
  lp_answer(&dev[1], 1, now);
  run(100);
}

void test_link_finds_the_other_by_name_and_game(void) {
  LinkProto other;
  boot();
  run(600);
  CHECK_EQ(dev[0].npeers, 1);
  CHECK(!strcmp(dev[0].peers[0].name, "Britney"));
  CHECK(!strcmp(dev[1].peers[0].name, "Alex"));
  /* someone playing something else is not listed */
  lp_init(&other, MAC[1], tx, (void *)(intptr_t)1, 3);
  lp_open(&other, "chess", "Sam", now);
  lp_close(&dev[1]);
  dev[1] = other;
  run(LP_PEER_MS + 500);
  CHECK_EQ(dev[0].npeers, 0);                    /* and a peer gone quiet drops off */
}

void test_link_invite_accept_and_roles(void) {
  pair();
  CHECK_EQ(dev[0].state, LP_CONNECTED);
  CHECK_EQ(dev[1].state, LP_CONNECTED);
  CHECK_EQ(dev[0].role, 0);                       /* the inviter goes first */
  CHECK_EQ(dev[1].role, 1);
  CHECK(!strcmp(dev[0].peer_name, "Britney"));
  CHECK(!strcmp(dev[1].peer_name, "Alex"));
}

void test_link_an_invite_seen_twice_is_one_invite_and_a_no_goes_back_to_looking(void) {
  boot();
  run(600);
  lp_invite(&dev[0], 0, now);
  run(1000);                                      /* asked three more times */
  CHECK_EQ(dev[1].state, LP_INVITED);
  lp_answer(&dev[1], 0, now);
  run(100);
  CHECK_EQ(dev[0].state, LP_LOOKING);
  CHECK(!strcmp(dev[0].why, "they said no"));
  CHECK_EQ(dev[1].state, LP_LOOKING);
}

void test_link_messages_arrive_once_in_order_through_a_lossy_radio(void) {
  char buf[64];
  int i, n, got = 0, ok = 1;
  pair();
  drop_every = 3;                                 /* a third of everything lost */
  for (i = 0; i < 20; i++) {
    char m[8];
    int len = 0;
    m[len++] = 'm';
    m[len++] = (char)('a' + i);
    while (lp_send(&dev[0], m, len, now) != 0) run(50);   /* the queue fills; wait */
    run(30);
    while ((n = lp_recv(&dev[1], buf, sizeof buf)) > 0) {
      if (n != 2 || buf[1] != 'a' + got) ok = 0;
      got++;
    }
  }
  run(3000);
  while ((n = lp_recv(&dev[1], buf, sizeof buf)) > 0) {
    if (n != 2 || buf[1] != 'a' + got) ok = 0;
    got++;
  }
  CHECK_EQ(got, 20);
  CHECK(ok);
  CHECK_EQ(dev[0].nout, 0);                       /* every one acknowledged */
  /* and the other way */
  lp_send(&dev[1], "hi", 2, now);
  run(500);
  CHECK_EQ(lp_recv(&dev[0], buf, sizeof buf), 2);
  CHECK(!memcmp(buf, "hi", 2));
}

void test_link_crossed_invites_make_one_game(void) {
  boot();
  run(600);
  lp_invite(&dev[0], 0, now);                     /* both at once, before either hears */
  lp_invite(&dev[1], 0, now);
  run(500);
  CHECK_EQ(dev[0].state, LP_CONNECTED);
  CHECK_EQ(dev[1].state, LP_CONNECTED);
  CHECK(dev[0].role != dev[1].role);
  CHECK_EQ(dev[0].session, dev[1].session);
}

void test_link_leaving_and_going_out_of_range(void) {
  pair();
  lp_close(&dev[1]);
  run(100);
  CHECK_EQ(dev[0].state, LP_ENDED);
  CHECK(!strcmp(dev[0].why, "they left"));
  lp_look(&dev[0], now);
  CHECK_EQ(dev[0].state, LP_LOOKING);

  pair();
  deaf[0] = 1;                                    /* walked out of range */
  run(LP_LOST_MS + 500);
  CHECK_EQ(dev[0].state, LP_ENDED);
  CHECK(!strcmp(dev[0].why, "lost them: out of range?"));
  CHECK_EQ(dev[1].state, LP_CONNECTED);           /* still hearing pings, until it too times out */
}

void test_link_a_busy_device_declines_a_second_invitation(void) {
  LinkProto third;
  static const uint8_t M3[6] = { 2, 0, 0, 0, 0, 3 };
  uint8_t f[LP_FRAME];
  pair();
  /* A third device's invite reaches dev[0] mid-game: it says busy, and the
   * game goes on. Built by hand: the fake air has only two devices. */
  lp_init(&third, M3, 0, 0, 5);
  lp_open(&third, "quoridor", "Sam", now);
  f[0] = 'C'; f[1] = 'L'; f[2] = 1; f[3] = LPT_INVITE;
  {
    uint32_t g = lp_hash("quoridor");
    f[4] = (uint8_t)g; f[5] = (uint8_t)(g >> 8); f[6] = (uint8_t)(g >> 16); f[7] = (uint8_t)(g >> 24);
  }
  f[8] = 5; f[9] = 0; f[10] = f[11] = f[12] = f[13] = 0;
  memcpy(f + LP_HEAD, "Sam", 3);
  lp_rx(&dev[0], M3, f, LP_HEAD + 3, now);
  CHECK_EQ(dev[0].state, LP_CONNECTED);
  CHECK(!strcmp(dev[0].peer_name, "Britney"));
}
