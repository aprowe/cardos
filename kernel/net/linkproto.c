/* The two-device protocol. See linkproto.h. */
#include "kernel/net/linkproto.h"

#include <string.h>


uint32_t lp_hash(const char *s) {
  uint32_t h = 2166136261u;
  for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

static uint32_t next_rand(LinkProto *lp) {
  lp->rand = lp->rand * 1664525u + 1013904223u;
  return lp->rand;
}

static void copy_name(char *out, const char *s, size_t n) {
  size_t k = 0;
  for (; s && *s && k + 1 < n; s++)
    out[k++] = (*s >= 32 && *s < 127) ? *s : '?';
  out[k] = 0;
}

void lp_init(LinkProto *lp, const uint8_t my_mac[6], LpTx tx, void *ctx, uint32_t seed) {
  memset(lp, 0, sizeof *lp);
  memcpy(lp->my_mac, my_mac, 6);
  lp->tx = tx;
  lp->ctx = ctx;
  lp->rand = seed ? seed : 1;
}

/* ---- frames ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static void send_frame(LinkProto *lp, const uint8_t *mac, int type, uint16_t seq, uint16_t ack,
                       const void *payload, int plen, uint32_t now) {
  uint8_t f[LP_FRAME];
  if (plen < 0) plen = 0;
  if (plen > LP_FRAME - LP_HEAD) plen = LP_FRAME - LP_HEAD;
  f[0] = 'C'; f[1] = 'L'; f[2] = 1; f[3] = (uint8_t)type;
  f[4] = (uint8_t)lp->game; f[5] = (uint8_t)(lp->game >> 8);
  f[6] = (uint8_t)(lp->game >> 16); f[7] = (uint8_t)(lp->game >> 24);
  put16(f + 8, lp->session);
  put16(f + 10, seq);
  put16(f + 12, ack);
  if (plen) memcpy(f + LP_HEAD, payload, (size_t)plen);
  if (mac) lp->said = now;
  if (lp->tx) lp->tx(lp->ctx, mac, f, LP_HEAD + plen);
}

static void send_hello(LinkProto *lp, uint32_t now) {
  uint8_t p[1 + LP_NAME_MAX];
  int n = (int)strlen(lp->me);
  p[0] = lp->flags;
  memcpy(p + 1, lp->me, (size_t)n);
  send_frame(lp, NULL, LPT_HELLO, 0, 0, p, 1 + n, now);
  lp->hello_at = now;
}

static void send_invite(LinkProto *lp, uint32_t now) {
  send_frame(lp, lp->peer, LPT_INVITE, 0, 0, lp->me, (int)strlen(lp->me), now);
}

static void send_data(LinkProto *lp, uint32_t now) {
  if (!lp->nout) return;
  send_frame(lp, lp->peer, LPT_DATA, lp->tx_seq, 0, lp->out[0].d, lp->out[0].len, now);
  lp->resend_at = now + LP_RESEND_MS;
}

/* ---- states ------------------------------------------------------------------ */

static void reset_session(LinkProto *lp) {
  memset(lp->peer, 0, 6);
  lp->peer_name[0] = 0;
  lp->session = 0;
  lp->nout = lp->nin = 0;
  lp->tx_seq = 1;
  lp->rx_next = 1;
}

void lp_open(LinkProto *lp, const char *game, const char *me, uint32_t now) {
  lp->game = lp_hash(game);
  copy_name(lp->me, me && *me ? me : "Cardputer", sizeof lp->me);
  lp->npeers = 0;
  lp->why[0] = 0;
  reset_session(lp);
  lp->state = LP_LOOKING;
  send_hello(lp, now);
}

void lp_look(LinkProto *lp, uint32_t now) {
  if (lp->state == LP_OFF) return;
  reset_session(lp);
  lp->state = LP_LOOKING;
  send_hello(lp, now);
}

static void end(LinkProto *lp, const char *why) {
  lp->state = LP_ENDED;
  copy_name(lp->why, why, sizeof lp->why);
  lp->nout = 0;
}

void lp_close(LinkProto *lp) {
  int i;
  if (lp->state == LP_CONNECTED || lp->state == LP_INVITING || lp->state == LP_INVITED)
    for (i = 0; i < 3; i++)                  /* said thrice: no answer is coming */
      send_frame(lp, lp->peer, lp->state == LP_INVITED ? LPT_DECLINE : LPT_BYE, 0, 0, 0, 0, lp->said);
  reset_session(lp);
  lp->state = LP_OFF;
  lp->npeers = 0;
}

static LpPeer *find_peer(LinkProto *lp, const uint8_t mac[6]) {
  int i;
  for (i = 0; i < lp->npeers; i++)
    if (!memcmp(lp->peers[i].mac, mac, 6)) return &lp->peers[i];
  return NULL;
}

static void saw_peer(LinkProto *lp, const uint8_t mac[6], const char *name, int nlen,
                     uint8_t flags, uint32_t now) {
  LpPeer *p = find_peer(lp, mac);
  char nm[LP_NAME_MAX];
  int k;
  if (!p) {
    if (lp->npeers >= LP_PEERS) return;
    p = &lp->peers[lp->npeers++];
    memcpy(p->mac, mac, 6);
  }
  for (k = 0; k < nlen && k + 1 < LP_NAME_MAX; k++) nm[k] = name[k];
  nm[k] = 0;
  copy_name(p->name, nm, sizeof p->name);
  p->heard = now;
  p->flags = flags;
}

int lp_invite(LinkProto *lp, int i, uint32_t now) {
  if (lp->state != LP_LOOKING || i < 0 || i >= lp->npeers) return -1;
  reset_session(lp);
  memcpy(lp->peer, lp->peers[i].mac, 6);
  memcpy(lp->peer_name, lp->peers[i].name, LP_NAME_MAX);
  lp->session = (uint16_t)(next_rand(lp) >> 8);
  if (!lp->session) lp->session = 1;
  lp->role = 0;
  lp->state = LP_INVITING;
  lp->since = lp->heard = now;
  send_invite(lp, now);
  return 0;
}

static void connected(LinkProto *lp, uint32_t now) {
  lp->state = LP_CONNECTED;
  lp->heard = now;
  lp->tx_seq = 1;
  lp->rx_next = 1;
  lp->nout = lp->nin = 0;
}

int lp_answer(LinkProto *lp, int yes, uint32_t now) {
  if (lp->state != LP_INVITED) return -1;
  send_frame(lp, lp->peer, yes ? LPT_ACCEPT : LPT_DECLINE, 0, 0, 0, 0, now);
  if (yes) { lp->role = 1; connected(lp, now); }
  else lp_look(lp, now);
  return 0;
}

int lp_send(LinkProto *lp, const void *buf, int len, uint32_t now) {
  /* Not empty: lp_recv's 0 means there is nothing. */
  if (lp->state != LP_CONNECTED || lp->nout >= LP_QUEUE || len < 1 || len > LP_MSG_MAX) return -1;
  lp->out[lp->nout].len = (uint8_t)len;
  memcpy(lp->out[lp->nout].d, buf, (size_t)len);
  if (++lp->nout == 1) send_data(lp, now);   /* nothing waiting: now */
  return 0;
}

int lp_recv(LinkProto *lp, void *buf, int max) {
  int n;
  if (!lp->nin) return 0;
  n = lp->in[0].len < max ? lp->in[0].len : max;
  memcpy(buf, lp->in[0].d, (size_t)n);
  memmove(&lp->in[0], &lp->in[1], sizeof(LpMsg) * (size_t)(lp->nin - 1));
  lp->nin--;
  return n;
}

/* ---- receiving -------------------------------------------------------------------- */

static int from_peer(const LinkProto *lp, const uint8_t mac[6], uint16_t session) {
  return !memcmp(lp->peer, mac, 6) && session == lp->session;
}

void lp_rx(LinkProto *lp, const uint8_t mac[6], const uint8_t *f, int len, uint32_t now) {
  int type, plen;
  uint16_t session, seq, ack;
  const uint8_t *p;
  if (lp->state == LP_OFF || len < LP_HEAD || f[0] != 'C' || f[1] != 'L' || f[2] != 1) return;
  if ((uint32_t)(f[4] | (f[5] << 8) | (f[6] << 16) | ((uint32_t)f[7] << 24)) != lp->game) return;
  if (!memcmp(mac, lp->my_mac, 6)) return;
  type = f[3];
  session = get16(f + 8);
  seq = get16(f + 10);
  ack = get16(f + 12);
  p = f + LP_HEAD;
  plen = len - LP_HEAD;

  switch (type) {
  case LPT_HELLO:
    if (plen >= 1) saw_peer(lp, mac, (const char *)p + 1, plen - 1, p[0], now);
    return;

  case LPT_INVITE:
    if (lp->state == LP_CONNECTED && from_peer(lp, mac, session)) {
      /* Our ACCEPT was lost: they are still asking. Say it again. */
      send_frame(lp, mac, LPT_ACCEPT, 0, 0, 0, 0, now);
      return;
    }
    if (lp->state == LP_INVITED && from_peer(lp, mac, session)) return;   /* asked again */
    if (lp->state == LP_INVITING && !memcmp(lp->peer, mac, 6)) {
      /* We asked each other at once. The lower address is the inviter;
       * the other takes the invitation as if it had come first. */
      if (memcmp(lp->my_mac, mac, 6) < 0) return;
      lp->session = session;
      send_frame(lp, mac, LPT_ACCEPT, 0, 0, 0, 0, now);
      lp->role = 1;
      connected(lp, now);
      return;
    }
    if (lp->state != LP_LOOKING) {           /* busy: in a game, or asking someone else */
      uint16_t mine = lp->session;
      lp->session = session;
      send_frame(lp, mac, LPT_DECLINE, 0, 0, "busy", 4, now);
      lp->session = mine;
      return;
    }
    reset_session(lp);
    memcpy(lp->peer, mac, 6);
    {
      char nm[LP_NAME_MAX];
      int k;
      for (k = 0; k < plen && k + 1 < LP_NAME_MAX; k++) nm[k] = (char)p[k];
      nm[k] = 0;
      copy_name(lp->peer_name, nm, sizeof lp->peer_name);
    }
    lp->session = session;
    lp->state = LP_INVITED;
    lp->since = lp->heard = now;
    saw_peer(lp, mac, lp->peer_name, (int)strlen(lp->peer_name), 0, now);
    return;

  case LPT_ACCEPT:
    if (lp->state == LP_INVITING && from_peer(lp, mac, session)) connected(lp, now);
    return;

  case LPT_DECLINE:
    if (lp->state == LP_INVITING && from_peer(lp, mac, session)) {
      lp_look(lp, now);
      copy_name(lp->why, plen >= 4 && !memcmp(p, "busy", 4) ? "they are busy" : "they said no",
                sizeof lp->why);
    } else if (lp->state == LP_INVITED && from_peer(lp, mac, session)) {
      lp_look(lp, now);                      /* they gave up asking */
      copy_name(lp->why, "they stopped asking", sizeof lp->why);
    }
    return;

  case LPT_BYE:
    if ((lp->state == LP_CONNECTED || lp->state == LP_INVITED || lp->state == LP_INVITING) &&
        from_peer(lp, mac, session))
      end(lp, "they left");
    return;

  case LPT_PING:
    if (lp->state == LP_CONNECTED && from_peer(lp, mac, session)) lp->heard = now;
    return;

  case LPT_ACK:
    if (lp->state != LP_CONNECTED || !from_peer(lp, mac, session)) return;
    lp->heard = now;
    if (lp->nout && ack == lp->tx_seq) {
      memmove(&lp->out[0], &lp->out[1], sizeof(LpMsg) * (size_t)(lp->nout - 1));
      lp->nout--;
      lp->tx_seq++;
      if (lp->nout) send_data(lp, now);
    }
    return;

  case LPT_DATA:
    if (lp->state != LP_CONNECTED || !from_peer(lp, mac, session)) return;
    lp->heard = now;
    if (seq == lp->rx_next) {
      if (lp->nin >= LP_QUEUE || plen > LP_MSG_MAX) return;   /* no room: no ack, it comes again */
      lp->in[lp->nin].len = (uint8_t)plen;
      memcpy(lp->in[lp->nin].d, p, (size_t)plen);
      lp->nin++;
      lp->rx_next++;
    } else if ((uint16_t)(lp->rx_next - seq) > 0x8000u) {
      return;                                /* from the future: cannot be */
    }
    /* New or a repeat whose ack was lost: acknowledge it. */
    send_frame(lp, mac, LPT_ACK, 0, seq, 0, 0, now);
    return;
  }
}

/* ---- time --------------------------------------------------------------------------- */

void lp_tick(LinkProto *lp, uint32_t now) {
  int i;
  switch (lp->state) {
  case LP_LOOKING:
    if ((uint32_t)(now - lp->hello_at) >= LP_HELLO_MS) send_hello(lp, now);
    for (i = 0; i < lp->npeers; ) {
      if ((uint32_t)(now - lp->peers[i].heard) > LP_PEER_MS) {
        lp->peers[i] = lp->peers[--lp->npeers];
      } else i++;
    }
    return;
  case LP_INVITING:
    if ((uint32_t)(now - lp->since) > LP_INVITE_FOR) {
      send_frame(lp, lp->peer, LPT_DECLINE, 0, 0, 0, 0, now);   /* withdrawn */
      lp_look(lp, now);
      copy_name(lp->why, "no answer", sizeof lp->why);
    } else if ((uint32_t)(now - lp->said) >= LP_INVITE_MS) {
      send_invite(lp, now);
    }
    return;
  case LP_INVITED:
    if ((uint32_t)(now - lp->since) > LP_INVITE_FOR + 2000) {   /* they must have gone */
      lp_look(lp, now);
      copy_name(lp->why, "they stopped asking", sizeof lp->why);
    }
    return;
  case LP_CONNECTED:
    if ((uint32_t)(now - lp->heard) > LP_LOST_MS) { end(lp, "lost them: out of range?"); return; }
    if (lp->nout && (int32_t)(now - lp->resend_at) >= 0) send_data(lp, now);
    else if ((uint32_t)(now - lp->said) >= LP_PING_MS) send_frame(lp, lp->peer, LPT_PING, 0, 0, 0, 0, now);
    return;
  default:
    return;
  }
}
