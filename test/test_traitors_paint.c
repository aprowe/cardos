/* The real Traitors app (apps/traitors/traitors.c) played to the end by keys,
 * painted into a framebuffer with the real fonts (test/fakescreen.h): every
 * screen must write every pixel, and a game alone must finish whatever the
 * person does. The table talk comes from a scripted server, or -- with none
 * -- from the app's own lines.
 *
 * TRAITORS_DUMP=dir writes the screens there (traitors_*.ppm), to look at. */
#include <string.h>

#include "tinytest.h"
#include "fakescreen.h"

#define capp_info traitors_capp_info
#define capp_main traitors_capp_main
#include "apps/traitors/traitors.c"
#undef capp_info
#undef capp_main

static CardApi TA;

/* A server: the POST gives an id, one poll is pending, the next has the talk. */
static struct {
  int   on, polls, posts;
  char  body[1600];
  char  reply[400];
} TS;

static int ts_caps_none(void) { return 0; }

static int ts_start(const char *m, const char *u, const char *b, const char *ct, const char *bearer, int ms) {
  (void)ct; (void)bearer; (void)ms;
  if (!strcmp(m, "POST")) {
    TS.posts++;
    snprintf(TS.body, sizeof TS.body, "%s", b ? b : "");
    snprintf(TS.reply, sizeof TS.reply, "j1\n");
  } else if (strstr(u, "id=j1")) {
    int s, at;
    if (TS.polls++ == 0) { snprintf(TS.reply, sizeof TS.reply, "pending\n"); return 0; }
    at = snprintf(TS.reply, sizeof TS.reply, "ok\n");
    for (s = 0; s < TR_N; s++)
      if (G.t.alive[s] && !G.t.human[s])
        at += snprintf(TS.reply + at, sizeof TS.reply - (size_t)at, "%d\tThe server speaks for %s.\n", s, G.name[s]);
  } else {
    return -1;
  }
  return 0;
}

static int ts_poll(char *out, size_t n) {
  size_t k = strlen(TS.reply);
  if (k >= n) k = n - 1;
  memcpy(out, TS.reply, k);
  out[k] = 0;
  return (int)k;
}

static void t_boot(int server) {
  fakeapi_init(&TA);
  fakescreen_install(&TA);
  memset(&TS, 0, sizeof TS);
  if (server) { TA.http_start = ts_start; TA.http_poll = ts_poll; }
  else TA.caps_ok = ts_caps_none;
  fakeapi_ticks = 1000;
  traitors_capp_main(&TA, 0, 0);
  fakescreen_paint();
}

static int t_key(int k) {
  int r = fakescreen_ui.key(fakescreen_ui.state, (uint8_t)k);
  fakescreen_paint();
  return r;
}

static void t_tick(unsigned ms) {
  fakeapi_ticks += ms;
  fakescreen_ui.tick(fakescreen_ui.state, fakeapi_ticks);
  fakescreen_paint();
}

/* Whatever the screen asks, the obvious answer. */
static void t_answer(void) {
  if (talking()) { t_key(CAPP_KEY_ENTER); return; }
  switch (G.v.need) {
  case TR_ASK_ACK: t_key(CAPP_KEY_ENTER); break;
  case TR_ASK_YESNO: t_key('y'); break;
  case TR_ASK_COIN: t_key(G.v.my_role == TR_TRAITOR ? 'l' : 'g'); break;
  case TR_ASK_CREW:
    while (tr_bits(G.pick) < G.v.crew_size) { t_key(CAPP_KEY_RIGHT); t_key(' '); }
    t_key(CAPP_KEY_ENTER);
    break;
  case TR_ASK_SEAT: t_key(CAPP_KEY_ENTER); break;
  }
}

/* Play until it is over; which phases were seen, as a mask. */
static int t_play(const char *dump_prefix) {
  int i, seen = 0;
  char name[40];
  for (i = 0; i < 3000 && G.v.phase != TR_OVER; i++) {
    if (!(seen & (1 << G.v.phase))) {
      seen |= 1 << G.v.phase;
      CHECK_EQ(fakescreen_unset(), 0);
      snprintf(name, sizeof name, "%s_%d", dump_prefix, G.v.phase);
      fakescreen_dump("TRAITORS_DUMP", name);
    }
    t_answer();
    t_tick(900);
  }
  return seen;
}

void test_traitors_paint_the_title_then_a_game_alone_to_the_end(void) {
  int seen;
  t_boot(0);
  CHECK_EQ(G.screen, SC_TITLE);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("TRAITORS_DUMP", "traitors_title");
  t_key(CAPP_KEY_ENTER);
  CHECK_EQ(G.screen, SC_GAME);
  CHECK(G.host);
  CHECK_EQ(G.v.phase, TR_ROLES);
  CHECK_EQ(G.v.need, TR_ASK_ACK);
  CHECK(!strcmp(G.name[0], TA_NAME[0]));            /* everyone by their castle name */
  seen = t_play("traitors");
  CHECK_EQ(G.v.phase, TR_OVER);
  CHECK(G.v.winner == TR_FAITHFUL || G.v.winner == TR_TRAITOR);
  CHECK(seen & (1 << TR_TABLE));
  CHECK(seen & (1 << TR_VOTED));
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("TRAITORS_DUMP", "traitors_over");
  /* Enter plays again. */
  t_key(CAPP_KEY_ENTER);
  CHECK_EQ(G.v.phase, TR_ROLES);
}

void test_traitors_paint_the_talk_comes_from_the_server_with_secrets_only_there(void) {
  int i;
  t_boot(1);
  t_key(CAPP_KEY_ENTER);
  for (i = 0; i < 400 && !(G.v.phase == TR_TABLE && TS.polls >= 2); i++) { t_answer(); t_tick(900); }
  if (G.v.phase == TR_OVER) return;               /* the game ended before a table: a rare deal */
  CHECK_EQ(TS.posts, 1);
  /* The request names every computer's role -- to the server only. */
  CHECK(strstr(TS.body, "round 1\n") != NULL);
  CHECK(strstr(TS.body, " traitor ") != NULL || strstr(TS.body, "person") != NULL);
  CHECK(strstr(TS.body, "person 0 Morwen") != NULL);
  t_tick(100);
  CHECK(G.ntalk > 0);
  CHECK(strstr(G.talk[0].text, "The server speaks for") != NULL);
  /* Lines show one at a time; nobody votes while they do. */
  CHECK(talking());
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("TRAITORS_DUMP", "traitors_talk");
}

void test_traitors_paint_a_traitor_sees_the_other_and_a_faithful_nobody(void) {
  int s, tries;
  for (tries = 0; tries < 30; tries++) {
    t_boot(0);
    fakeapi_ticks = 1000 + (uint32_t)tries * 7919;
    t_key(CAPP_KEY_ENTER);
    if (G.t.role[0] == TR_TRAITOR) break;
  }
  CHECK_EQ(G.t.role[0], TR_TRAITOR);
  CHECK_EQ(tr_bits(G.v.traitors), 2);
  for (s = 0; s < TR_N; s++) CHECK_EQ((G.v.traitors >> s) & 1, G.t.role[s] == TR_TRAITOR);
  CHECK(fakescreen_count(capp_rect(50, 18, 140, 56), C_BLOOD) > 50);   /* the role card, in red */
  fakescreen_dump("TRAITORS_DUMP", "traitors_role_traitor");
}
