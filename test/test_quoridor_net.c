/* Quoridor across two devices, on the host: the real app (apps/quoridor.c)
 * on one side, a LinkProto driven by the test on the other, and a fake
 * radio between them -- so the lobby, the invitation, the moves going
 * across and a partner leaving all run through the real protocol. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "kernel/net/linkproto.h"

#define capp_info quoridor_capp_info
#define capp_main quoridor_capp_main
#include "apps/quoridor.c"

/* ---- the air: this device (0) and the other (1) ---- */

typedef struct { int from; uint8_t f[LP_FRAME]; int len; } QFrame;
static QFrame qair[64];
static int nqair;
static LinkProto me, them;
static const uint8_t QMAC[2][6] = { { 2, 0, 0, 0, 0, 7 }, { 2, 0, 0, 0, 0, 8 } };
static uint32_t QNOW;

static void qtx(void *ctx, const uint8_t *mac, const uint8_t *f, int len) {
  (void)mac;
  if (nqair >= 64) return;
  qair[nqair].from = (int)(intptr_t)ctx;
  memcpy(qair[nqair].f, f, (size_t)len);
  qair[nqair].len = len;
  nqair++;
}

static void qdeliver(void) {
  static QFrame was[64];
  int n = nqair, i;
  memcpy(was, qair, sizeof(QFrame) * (size_t)n);
  nqair = 0;
  for (i = 0; i < n; i++)
    lp_rx(was[i].from == 0 ? &them : &me, QMAC[was[i].from], was[i].f, was[i].len, QNOW);
}

/* ---- the link the app sees: `me` ---- */

static int l_open(const char *g, const char *n) { (void)n; lp_open(&me, g, "Alex", QNOW); return 0; }
static void l_close(void) { lp_close(&me); }
static int l_state(void) { return me.state; }
static int l_peers(void) { return me.state == LP_LOOKING ? me.npeers : 0; }
static const char *l_name(int i) { return i < 0 ? me.peer_name : me.peers[i].name; }
static int l_invite(int i) { return lp_invite(&me, i, QNOW); }
static int l_answer(int y) { return lp_answer(&me, y, QNOW); }
static int l_role(void) { return me.role; }
static int l_send(const void *b, int n) { return lp_send(&me, b, n, QNOW); }
static int l_recv(void *b, int n) { return lp_recv(&me, b, n); }
static void l_look(void) { lp_look(&me, QNOW); }
static const char *l_why(void) { return me.why; }
static const CappLink QLINK = { l_open, l_close, l_state, l_peers, l_name, l_invite, l_answer,
                                l_role, l_send, l_recv, l_look, l_why };
static const CappLink *q_link(void) { return &QLINK; }

/* ---- the rest of the API Quoridor uses ---- */

static CappUi QUI;
static void q_fill(CRect r, uint16_t c) { (void)r; (void)c; }
static void q_text(int16_t x, int16_t y, const char *s, uint16_t f, uint16_t b) { (void)x; (void)y; (void)s; (void)f; (void)b; }
static void q_text_font(int f, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) { (void)f; (void)x; (void)y; (void)s; (void)fg; (void)bg; }
static int q_font_load(const char *n) { (void)n; return -1; }
static size_t q_strlen(const char *s) { return strlen(s); }
static uint32_t q_ticks(void) { return QNOW; }
static void q_ui(const CappUi *ui) { QUI = *ui; }
static int q_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap; int r;
  va_start(ap, fmt); r = vsnprintf(buf, n, fmt, ap); va_end(ap);
  return r;
}

static CardApi QAPI;
static const CRect QWIN = { 0, 0, 240, 135 };

static void run_ms(uint32_t ms) {
  uint32_t end = QNOW + ms;
  while (QNOW < end) {
    QNOW += 10;
    lp_tick(&me, QNOW);
    lp_tick(&them, QNOW);
    qdeliver();
    if (QUI.tick) QUI.tick(QUI.state, QNOW);
    QUI.paint(QUI.state, QWIN);              /* every screen, painted: nothing crashes */
  }
}

static void key(uint8_t k) { QUI.key(QUI.state, k); QUI.paint(QUI.state, QWIN); }

static void qboot(void) {
  memset(&QAPI, 0, sizeof QAPI);
  QAPI.version = CAPP_API_VERSION;
  QAPI.fill = q_fill; QAPI.text = q_text; QAPI.text_font = q_text_font;
  QAPI.font_load = q_font_load; QAPI.str_len = q_strlen; QAPI.fmt = q_fmt;
  QAPI.ticks_ms = q_ticks; QAPI.ui = q_ui; QAPI.link = q_link;
  memset(&G, 0, sizeof G);
  nqair = 0;
  QNOW = 5000;
  lp_init(&me, QMAC[0], qtx, (void *)(intptr_t)0, 3);
  lp_init(&them, QMAC[1], qtx, (void *)(intptr_t)1, 4);
  quoridor_capp_main(&QAPI, 0, 0);
  lp_open(&them, "quoridor", "Britney", QNOW);
}

/* From the title, down to "Play nearby", and in. */
static void to_lobby(void) {
  while (G.pick != PICK_NEAR) key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);
}

static void send_them(int wall, int vert, int r, int c) {
  uint8_t b[5];
  b[0] = 'M'; b[1] = (uint8_t)wall; b[2] = (uint8_t)vert; b[3] = (uint8_t)r; b[4] = (uint8_t)c;
  lp_send(&them, b, 5, QNOW);
}

static int recv_them(uint8_t *b) { return lp_recv(&them, b, 16); }

void test_quoridor_net_invite_play_and_moves_cross(void) {
  uint8_t b[16];
  qboot();
  to_lobby();
  CHECK_EQ(G.screen, SCREEN_LOBBY);
  run_ms(600);
  CHECK_EQ(me.npeers, 1);                        /* Britney is listed */
  key(CAPP_KEY_ENTER);                           /* invite her */
  run_ms(100);
  CHECK_EQ(them.state, LP_INVITED);
  lp_answer(&them, 1, QNOW);
  run_ms(100);
  CHECK_EQ(G.screen, SCREEN_GAME);               /* the game began by itself */
  CHECK_EQ(G.me, 0);                             /* the inviter is blue */
  CHECK_EQ(G.q.turn, 0);

  key(':');                                      /* shift-up: a step */
  run_ms(200);
  CHECK_EQ(G.q.r[0], 7);
  CHECK_EQ(recv_them(b), 5);
  CHECK(b[0] == 'M' && b[1] == 0 && b[3] == 7 && b[4] == 4);

  key(':');                                      /* not my turn: nothing happens */
  CHECK_EQ(G.q.r[0], 7);
  send_them(0, 0, 1, 4);                         /* their red pawn steps down */
  run_ms(300);
  CHECK_EQ(G.q.r[1], 1);
  CHECK_EQ(G.q.turn, 0);                         /* mine again */

  send_them(0, 0, 2, 4);                         /* out of turn: refused, said */
  run_ms(300);
  CHECK_EQ(G.q.r[1], 1);
}

void test_quoridor_net_invited_side_is_red_and_waits(void) {
  qboot();
  to_lobby();
  run_ms(600);
  lp_invite(&them, 0, QNOW);                     /* she asks me */
  run_ms(100);
  CHECK_EQ(me.state, LP_INVITED);
  key('y');
  run_ms(200);
  CHECK_EQ(G.screen, SCREEN_GAME);
  CHECK_EQ(G.me, 1);                             /* red */
  key(':');                                      /* blue moves first: not me */
  CHECK_EQ(G.q.r[1], 0);
  send_them(0, 0, 7, 4);
  run_ms(300);
  CHECK_EQ(G.q.r[0], 7);
  key('>');                                      /* shift-down: red steps toward row 8 */
  CHECK_EQ(G.q.r[1], 1);
}

void test_quoridor_net_partner_leaving_is_said_and_enter_finds_another(void) {
  qboot();
  to_lobby();
  run_ms(600);
  key(CAPP_KEY_ENTER);
  run_ms(100);
  lp_answer(&them, 1, QNOW);
  run_ms(100);
  lp_close(&them);                               /* she quit */
  run_ms(100);
  CHECK(!strcmp(G.note, "they left"));
  CHECK(!humans_turn());
  key(CAPP_KEY_ENTER);                           /* back to the lobby */
  CHECK_EQ(G.screen, SCREEN_LOBBY);
  CHECK_EQ(me.state, LP_LOOKING);
}
