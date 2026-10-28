/* The Traitors: seven at a castle, two of them Traitors. A game for one
 * person, or two on two Cardputers side by side, with the castle's other
 * guests played by the computer -- whose words at the round table are
 * written by Claude through the server (server/traitors.py).
 *
 * The rules and the computer players are apps/traitors/traitors.h (host-
 * tested in test/test_traitors.c); this is the castle on screen. The
 * pictures are tools/make_traitors.py's, in apps/traitors/traitors_art.h:
 * seven portraits and the little icons; the hall behind them -- stone,
 * two moonlit windows, the table in candlelight -- is drawn here a row at a
 * time.
 *
 * Two devices: "Play nearby" pairs them over ESP-NOW (api->link). The one
 * who invites is the host: it runs the game -- the rules, the computers,
 * the talk -- and after every change sends the other what its seat may see
 * (TrView, one message); the guest sends back only its answers. If the
 * guest leaves, the computer takes its seat; if the host leaves, the game
 * is over for the guest.
 *
 * Messages (first byte):
 *   S  host -> guest   the guest's view (tv_pack), then one byte: 1 while
 *                      the table talk is still being written
 *   T  host -> guest   a line to show: the speaker's seat (255 the castle),
 *                      then the words
 *   A  guest -> host   an answer: two bytes, low then high
 *
 * Keys: arrows choose a seat, space puts one in a crew or takes them out,
 * Enter settles it; y / n vote on a crew; g / l (a Traitor's) drop a coin;
 * Enter goes on after anything is shown, and hurries the table talk along.
 * Esc asks before leaving a game.
 *
 * Every repaint is whole and composed off the panel by the OS; nothing marks
 * damage.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/footer.h"
#include "apps/confirm.h"
#include "apps/traitors/traitors.h"
#include "apps/traitors/traitors_art.h"

static const CardApi *api;

#define HALL_H    92                  /* the castle; the words under it */
#define PANEL_Y   HALL_H
#define TALK_MAX  6
#define SAY_MAX   112
#define LOG_N     14
#define LOG_W     96
#define NAME_W    13

#define C_PANEL   CAPP_RGB(22, 14, 26)
#define C_EDGE    CAPP_RGB(110, 80, 40)
#define C_TEXT    CAPP_RGB(232, 222, 206)
#define C_DIM     CAPP_RGB(150, 138, 150)
#define C_GOLD    CAPP_RGB(232, 186, 82)
#define C_BLOOD   CAPP_RGB(196, 36, 48)
#define C_GOOD    CAPP_RGB(132, 196, 132)
#define C_CURSOR  CAPP_RGB(255, 226, 120)
#define C_PICK    CAPP_RGB(200, 140, 40)
#define C_NIGHT   CAPP_RGB(8, 6, 14)

enum { SC_TITLE, SC_LOBBY, SC_GAME };
enum { M_SOLO, M_NEARBY, M_N };
static const char *const MENU[M_N] = { "Play alone", "Play nearby" };

/* Where each seat sits round the table: the portrait's centre, from an
 * ellipse at (120, 44), radii 96 and 32, seat 0 at the top. */
static const int16_t SEAT_X[TR_N] = { 120, 195, 213, 162, 78, 26, 45 };
static const int16_t SEAT_Y[TR_N] = { 12, 24, 51, 73, 73, 51, 24 };

enum { TK_IDLE, TK_POST, TK_WAIT, TK_POLL, TK_DONE };

static struct {
  int      screen, menu;
  int      host, net, me;              /* host: runs the game; net: two devices */
  const CappLink *L;
  int      lsel, lpeers, lstate;
  char     note[48];

  Tr       t;                          /* the host's game */
  TrView   v;                          /* what this seat sees */
  int      hold;                       /* the talk is still being written */
  int      cur;                        /* the seat the cursor is on */
  uint8_t  pick;                       /* a crew being chosen */
  int      sent;                       /* the guest's answer is out: wait for the next view */
  int      asking;                     /* leave this game? */
  uint32_t next_at;                    /* when the host settles a ready phase */
  int      seen_phase;                 /* the phase the narration last spoke of */

  char     name[TR_N][NAME_W];
  char     say[SAY_MAX];               /* the castle's line, or a speaker's */
  int      say_seat;                   /* -1 the castle */
  struct { int seat; char text[SAY_MAX]; } talk[TALK_MAX];
  int      ntalk, italk;
  uint32_t talk_at;

  char     log[LOG_N][LOG_W];          /* the host's account of the game, for Claude */
  int      nlog;
  int      tk;                         /* TK_*: the talk request */
  uint32_t tk_at, tk_until;
  char     tk_id[16];
  char     body[1600];
  char     reply[1000];

  int      f_ui, f_uib;
} G;

/* ---- words ------------------------------------------------------------------ */

static int is_me(int s) { return s == G.me; }

static void logf_(const char *s) {
  int i;
  if (!G.host) return;
  if (G.nlog == LOG_N) {
    for (i = 1; i < LOG_N; i++) api->mem_cpy(G.log[i - 1], G.log[i], LOG_W);
    G.nlog--;
  }
  api->fmt(G.log[G.nlog++], LOG_W, "%s", s);
}

static void send_line(int seat, const char *s) {
  uint8_t b[2 + SAY_MAX];
  int n = (int)api->str_len(s);
  if (!G.net || !G.host || !G.L || G.L->state() != CAPP_LINK_CONNECTED) return;
  if (n > SAY_MAX - 1) n = SAY_MAX - 1;
  b[0] = 'T';
  b[1] = (uint8_t)(seat < 0 ? 255 : seat);
  api->mem_cpy(b + 2, s, (size_t)n);
  G.L->send(b, n + 2);
}

/* The castle says something: on this screen, to the guest, and in the log. */
static void castle(const char *s) {
  api->fmt(G.say, sizeof G.say, "%s", s);
  G.say_seat = -1;
  logf_(s);
  send_line(-1, s);
}

/* "A, B and C" for a mask of seats. */
static void list_names(uint8_t m, char *out, int n) {
  int s, k = 0, total = tr_bits(m), at = 0;
  out[0] = 0;
  for (s = 0; s < TR_N; s++) {
    if (!tr_in(m, s)) continue;
    at += api->fmt(out + at, (size_t)(n - at), "%s%s", k == 0 ? "" : k == total - 1 ? " and " : ", ",
                   G.name[s]);
    k++;
    if (at >= n - 1) break;
  }
}

/* ---- the table talk ------------------------------------------------------------ */

static const char *const FAITH_LINES[] = {
  "I keep coming back to %s. Something is off.",
  "My vote is %s. I hope I am wrong.",
  "%s has been far too quiet tonight.",
  "Every road leads back to %s for me.",
};
static const char *const TRAIT_LINES[] = {
  "Look at %s. Always near the trouble.",
  "I trust everyone here but %s.",
  "%s, your story does not add up.",
  "Faithful, open your eyes: %s.",
};

static void talk_add(int seat, const char *text) {
  if (G.ntalk >= TALK_MAX) return;
  G.talk[G.ntalk].seat = seat;
  api->fmt(G.talk[G.ntalk].text, SAY_MAX, "%s", text);
  G.ntalk++;
}

/* The computers' own words, for when Claude cannot be asked. */
static void talk_fallback(void) {
  int s;
  char line[SAY_MAX];
  G.ntalk = G.italk = 0;
  for (s = 0; s < TR_N; s++) {
    int target;
    const char *form;
    if (!G.t.alive[s] || G.t.human[s]) continue;
    target = G.t.vote[s];
    if (target < 0 || target >= TR_N) continue;
    form = G.t.role[s] == TR_TRAITOR ? TRAIT_LINES[tr_roll(&G.t, 4)] : FAITH_LINES[tr_roll(&G.t, 4)];
    api->fmt(line, sizeof line, form, G.name[target]);
    talk_add(s, line);
  }
}

static void talk_send_all(void) {
  int i;
  for (i = 0; i < G.ntalk; i++) send_line(G.talk[i].seat, G.talk[i].text);
}

static void talk_ready(void) {
  G.tk = TK_DONE;
  G.italk = 0;
  G.talk_at = 0;
  G.hold = 0;
  talk_send_all();
}

/* A name with no spaces, for the request's lines. */
static void token(const char *s, char *out, int n) {
  int k = 0;
  for (; *s && k < n - 1; s++) out[k++] = *s == ' ' ? '_' : *s;
  out[k] = 0;
}

/* Ask the server for the computers' words: who they are (in secret), whom
 * each will vote for, and what has happened. */
static void talk_start(void) {
  int s, at = 0, i;
  char url[160], nm[NAME_W];
  G.ntalk = G.italk = 0;
  G.hold = 1;
  if (!(api->caps_ok() & CAPP_CAP_PROXY)) { talk_fallback(); talk_ready(); return; }
  at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "round %d\n", G.t.round);
  for (s = 0; s < TR_N; s++) {
    token(G.name[s], nm, sizeof nm);
    if (G.t.alive[s] && !G.t.human[s]) {
      int sus = tr_best(&G.t, s, tr_alive_mask(&G.t) & (uint8_t)~(1 << s), 0, 0);
      at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "seat %d %s %s %d %d\n", s, nm,
                     G.t.role[s] == TR_TRAITOR ? "traitor" : "faithful", G.t.vote[s], sus);
    } else if (G.t.alive[s]) {
      at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "person %d %s\n", s, nm);
    } else {
      at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "gone %d %s %s\n", s, nm,
                     !G.t.shown[s] ? "murdered" : G.t.role[s] == TR_TRAITOR ? "banished-traitor"
                                                                           : "banished-faithful");
    }
  }
  for (i = 0; i < G.nlog; i++)
    at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "log %s\n", G.log[i]);
  api->fmt(url, sizeof url, "%s/traitors/talk", api->proxy());
  if (api->http_start("POST", url, G.body, "text/plain", "", 20000) != 0) {
    talk_fallback();
    talk_ready();
    return;
  }
  G.tk = TK_POST;
  G.tk_until = api->ticks_ms() + 45000;
}

/* "ok" then "SEAT\tTEXT" lines: the talk. 0 if it was not that. */
static int talk_parse(const char *r) {
  const char *p = r;
  char line[SAY_MAX];
  if (!str_starts(p, "ok")) return 0;
  while (*p && *p != '\n') p++;
  G.ntalk = 0;
  while (*p) {
    int seat, k = 0;
    if (*p == '\n') p++;
    if (*p < '0' || *p > '9') break;
    seat = *p - '0';
    p++;
    if (*p != '\t') break;
    p++;
    while (*p && *p != '\n' && k < SAY_MAX - 1) line[k++] = *p++;
    line[k] = 0;
    while (*p && *p != '\n') p++;
    if (seat < TR_N && G.t.alive[seat] && !G.t.human[seat]) talk_add(seat, line);
  }
  return G.ntalk > 0;
}

/* The request, a step a tick. 1 if anything changed on screen. */
static int talk_tick(uint32_t now) {
  int n;
  char url[200];
  if (G.tk != TK_POST && G.tk != TK_WAIT && G.tk != TK_POLL) return 0;
  if ((int32_t)(now - G.tk_until) > 0) { talk_fallback(); talk_ready(); return 1; }
  if (G.tk == TK_WAIT) {
    if ((int32_t)(now - G.tk_at) < 0) return 0;
    api->fmt(url, sizeof url, "%s/traitors/talk?id=%s", api->proxy(), G.tk_id);
    if (api->http_start("GET", url, 0, 0, "", 15000) == 0) G.tk = TK_POLL;
    else G.tk_at = now + 1000;
    return 0;
  }
  n = api->http_poll(G.reply, sizeof G.reply);
  if (n == CAPP_HTTP_PENDING) return 0;
  if (n < 0) { talk_fallback(); talk_ready(); return 1; }
  G.reply[n < (int)sizeof G.reply ? n : (int)sizeof G.reply - 1] = 0;
  if (G.tk == TK_POST) {
    int k = 0;
    while (G.reply[k] && G.reply[k] != '\n' && k < (int)sizeof G.tk_id - 1) { G.tk_id[k] = G.reply[k]; k++; }
    G.tk_id[k] = 0;
    if (!k || str_starts(G.reply, "error")) { talk_fallback(); talk_ready(); return 1; }
    G.tk = TK_WAIT;
    G.tk_at = now + 1500;
    return 0;
  }
  if (str_starts(G.reply, "pending")) { G.tk = TK_WAIT; G.tk_at = now + 1200; return 0; }
  if (!talk_parse(G.reply)) talk_fallback();
  talk_ready();
  return 1;
}

/* The talk on screen, a line at a time: 4 s each, or Enter. 1 to repaint. */
static int talk_show(uint32_t now) {
  if (G.italk >= G.ntalk) return 0;
  if (G.talk_at && (int32_t)(now - G.talk_at) < 0) return 0;
  api->fmt(G.say, sizeof G.say, "%s", G.talk[G.italk].text);
  G.say_seat = G.talk[G.italk].seat;
  G.italk++;
  G.talk_at = now + 4000;
  return 1;
}

/* The table talk is still going: being written, waiting to be shown, or
 * its last line still on screen. Nobody answers until it is done. */
static int talking(void) {
  if (G.v.phase != TR_TABLE) return 0;
  if (G.hold || G.italk < G.ntalk) return 1;
  return G.say_seat >= 0 && G.talk_at && (int32_t)(api->ticks_ms() - G.talk_at) < 0;
}

/* ---- the host: narration, and telling the guest ---------------------------------- */

static void narrate(int phase) {
  char a[80], b[SAY_MAX];
  Tr *t = &G.t;
  int s, yes = 0, no = 0;
  switch (phase) {
  case TR_PROPOSE:
    api->fmt(b, sizeof b, "Round %d. %s leads the mission.", t->round, G.name[t->leader]);
    castle(b);
    break;
  case TR_VOTE:
    list_names(t->crew, a, sizeof a);
    api->fmt(b, sizeof b, "%s names the crew: %s.", G.name[t->leader], a);
    castle(b);
    break;
  case TR_VOTED:
    for (s = 0; s < TR_N; s++) { if (t->vote[s] == 1) yes++; else if (t->vote[s] == 0) no++; }
    if (t->sent) api->fmt(b, sizeof b, "The crew goes, %d to %d.", yes, no);
    else api->fmt(b, sizeof b, "Turned down, %d to %d. That is %d of %d.", yes, no, t->rejects + 1,
                  TR_REJECTS);
    castle(b);
    break;
  case TR_VAULT:
    castle("The crew go down to the vault. Each drops a coin in the chest.");
    break;
  case TR_RESULT:
    if (t->lead < 0) api->fmt(b, sizeof b, "Three crews refused: the mission is spoiled.");
    else if (t->lead == 0) api->fmt(b, sizeof b, "The chest: all gold. %d to the pot.", TR_GOLD * tr_bits(t->crew));
    else api->fmt(b, sizeof b, "The chest: %d lead coin%s. The mission is spoiled.", t->lead,
                  t->lead == 1 ? "" : "s");
    castle(b);
    break;
  case TR_SHIELD:
    api->fmt(b, sizeof b, "%s has a shield to give.", G.name[t->leader]);
    castle(b);
    break;
  case TR_TABLE:
    if (t->shield >= 0) {
      api->fmt(b, sizeof b, "%s holds the shield. Now, the round table.", G.name[t->shield]);
      castle(b);
    } else castle("The round table. Who is a Traitor?");
    break;
  case TR_TABLED:
    if (t->banished < 0) castle("A tie. Nobody is banished tonight.");
    else {
      api->fmt(b, sizeof b, "%s is banished... and was %s", G.name[t->banished],
               t->role[t->banished] == TR_TRAITOR ? "a TRAITOR!" : "Faithful.");
      castle(b);
    }
    break;
  case TR_NIGHT:
    castle("Night falls. The Traitors meet in the turret.");
    break;
  case TR_BREAKFAST:
    if (t->murdered < 0) castle("Everyone comes down to breakfast.");
    else {
      api->fmt(b, sizeof b, "%s does not come down to breakfast. Murdered.", G.name[t->murdered]);
      castle(b);
    }
    break;
  case TR_OVER: {
    uint8_t all = 0;
    for (s = 0; s < TR_N; s++) if (t->role[s] == TR_TRAITOR) all |= (uint8_t)(1 << s);
    list_names(all, a, sizeof a);
    api->fmt(b, sizeof b, "%s win! The Traitors were %s.",
             t->winner == TR_FAITHFUL ? "The Faithful" : "The Traitors", a);
    castle(b);
    break;
  }
  }
}

/* The view this device draws, and the guest's, sent. */
static void publish(void) {
  uint8_t b[TV_WIRE + 2];
  TrView gv;
  int n;
  tv_from(&G.t, G.me, &G.v);
  if (G.net && G.L && G.L->state() == CAPP_LINK_CONNECTED) {
    tv_from(&G.t, 1, &gv);
    n = tv_pack(&gv, b);
    b[n++] = (uint8_t)G.hold;
    G.L->send(b, n);
  }
}

static void after_next(void) {
  if (G.t.phase != G.seen_phase) {
    G.seen_phase = G.t.phase;
    narrate(G.t.phase);
    if (G.t.phase == TR_TABLE) talk_start();
  }
}

/* The host settles a phase once everything is in, after a pause for the show. */
static int host_tick(uint32_t now) {
  int changed = talk_tick(now);
  if (G.t.phase == TR_OVER || !tr_ready(&G.t) || G.hold) { G.next_at = 0; if (changed) publish(); return changed; }
  if (G.t.phase == TR_TABLE && G.italk < G.ntalk) return changed;   /* let them speak first */
  if (!G.next_at) {
    int pace = G.t.phase == TR_PROPOSE || G.t.phase == TR_VAULT || G.t.phase == TR_NIGHT ? 1600 : 700;
    G.next_at = now + (uint32_t)pace;
    return changed;
  }
  if ((int32_t)(now - G.next_at) < 0) return changed;
  G.next_at = 0;
  tr_next(&G.t);
  after_next();
  publish();
  return 1;
}

static void new_game(void) {
  uint8_t human[TR_N];
  int s;
  for (s = 0; s < TR_N; s++) human[s] = 0;
  human[0] = 1;
  if (G.net) human[1] = 2;
  tr_new(&G.t, api->ticks_ms() * 2654435761u + 17, human);
  G.nlog = 0;
  G.ntalk = G.italk = 0;
  G.hold = 0;
  G.tk = TK_IDLE;
  G.seen_phase = TR_ROLES;
  G.next_at = 0;
  castle("Welcome to the castle. Seven guests; two are Traitors.");
  publish();
}

/* ---- names ----------------------------------------------------------------- */

/* Everyone goes by their castle name -- people too, on both devices, in
 * what the castle says and in what Claude writes -- so a line means the
 * same on either screen. Your own seat's name is drawn in gold. */
static void set_names(void) {
  int s;
  for (s = 0; s < TR_N; s++) api->fmt(G.name[s], NAME_W, "%s", TA_NAME[s]);
}

/* ---- painting: the hall ----------------------------------------------------------- */

static uint16_t ROW[240];

/* The stone, the windows, the table in candlelight: a row at a time. */
static void paint_hall(void) {
  int x, y;
  for (y = 0; y < HALL_H; y++) {
    for (x = 0; x < 240; x++) {
      int r, g, b, brick_row = y / 8, bx = (x + (brick_row & 1) * 8) % 16, mortar = (y % 8 == 7) || bx == 15;
      int dx = x - 120, dy = (y - 44) * 3, d2 = dx * dx + dy * dy, glow;
      /* stone, darker to the edges and top */
      r = 40; g = 35; b = 48;
      if (mortar) { r = 22; g = 19; b = 28; }
      r -= y < 20 ? (20 - y) / 2 : 0;
      g -= y < 20 ? (20 - y) / 2 : 0;
      /* two lancet windows, moonlight through them */
      {
        int wx = x < 120 ? x - 8 : 232 - x, in = 0;
        if (wx >= 0 && wx < 14 && y >= 6 && y < 44) {
          int ax = wx - 7, ay = y - 13;
          in = y >= 13 || ax * ax * 4 + ay * ay * 3 <= 150;
          if (in && wx != 7 && y != 26) { r = 52 + y / 3; g = 66 + y / 3; b = 110 + y / 2; }
          else if (in) { r = 18; g = 16; b = 24; }   /* the mullion and transom */
        }
      }
      /* candlelight from the table */
      glow = d2 < 9000 ? (9000 - d2) / 300 : 0;
      r += glow; g += glow / 2;
      /* the table */
      {
        int tx = x - 120, ty = y - 44;
        int e = tx * tx * 400 + ty * ty * 3364;          /* (tx/58)^2 + (ty/20)^2 scaled by 58^2*20^2 */
        if (e <= 1345600) {
          if (e >= 1180000) { r = 96; g = 58; b = 32; }  /* the rim */
          else { r = 62 + (ty + 20) / 3; g = 32 + (ty + 20) / 5; b = 22; }
        }
      }
      r = r < 0 ? 0 : r > 255 ? 255 : r;
      g = g < 0 ? 0 : g > 255 ? 255 : g;
      b = b < 0 ? 0 : b > 255 ? 255 : b;
      ROW[x] = CAPP_RGB(r, g, b);
    }
    api->pixels(capp_rect(0, y, 240, 1), ROW);
  }
}

static void icon(int which, int x, int y) {
  int r, c;
  const uint8_t *px = TA_ICON_PX[which];
  for (r = 0; r < TA_ICON; r++) {
    c = 0;
    while (c < TA_ICON) {
      int v = px[r * TA_ICON + c], s = c;
      if (v == 0xFF) { c++; continue; }
      while (c < TA_ICON && px[r * TA_ICON + c] == v) c++;
      if (y + r >= 0 && x + s >= 0) api->fill(capp_rect(x + s, y + r, c - s, 1), TA_PAL[v]);
    }
  }
}

/* A portrait, greyed if they are gone. */
static void portrait(int s, int x, int y, int grey) {
  uint16_t px[TA_FACE * TA_FACE];
  int i;
  for (i = 0; i < TA_FACE * TA_FACE; i++) {
    uint8_t b = TA_FACES[s][i / 2];
    int v = i & 1 ? b & 15 : b >> 4;
    if (grey && v > 1) {
      /* the palette's own greys, by how light it was */
      static const uint8_t GREY[16] = { 0, 1, 13, 12, 1, 12, 1, 1, 12, 12, 13, 13, 1, 12, 12, 13 };
      v = GREY[v];
    }
    px[i] = TA_PAL[v];
  }
  api->pixels(capp_rect(x, y, TA_FACE, TA_FACE), px);
}

static void frame(int x, int y, int w, int h, uint16_t c) {
  api->fill(capp_rect(x, y, w, 1), c);
  api->fill(capp_rect(x, y + h - 1, w, 1), c);
  api->fill(capp_rect(x, y, 1, h), c);
  api->fill(capp_rect(x + w - 1, y, 1, h), c);
}

/* Can the cursor stop on seat s, for what this seat is asked? */
static int pickable(int s) {
  const TrView *v = &G.v;
  int alive = (v->alive >> s) & 1;
  switch (v->need) {
  case TR_ASK_CREW: return alive;
  case TR_ASK_SEAT:
    if (!alive) return 0;
    if (v->phase == TR_NIGHT) return !((v->traitors >> s) & 1) && s != v->shield;
    return s != G.me;
  }
  return 0;
}

static void paint_seat(int s) {
  const TrView *v = &G.v;
  int x = SEAT_X[s] - 10, y = SEAT_Y[s] - 12, alive = (v->alive >> s) & 1, w;
  int traitor_known = (v->traitors >> s) & 1;
  uint16_t edge = 0;
  int has_edge = 0;
  if (y < 1) y = 1;
  portrait(s, x, y, !alive);
  if (traitor_known) { edge = C_BLOOD; has_edge = 1; }
  if (v->need == TR_ASK_CREW && tr_in(G.pick, s)) { edge = C_PICK; has_edge = 1; }
  if (pickable(s) && s == G.cur && !talking()) { edge = C_CURSOR; has_edge = 1; }
  if (has_edge) frame(x - 1, y - 1, TA_FACE + 2, TA_FACE + 2, edge);
  if (!alive) icon((v->shown >> s) & 1 ? ((v->traitors >> s) & 1 ? TA_CLOAK : TA_SKULL) : TA_DAGGER,
                   x + 5, y + 6);
  if (s == v->leader && alive && v->phase != TR_OVER) icon(TA_CROWN, x - 9, y - 1);
  if (tr_in(v->crew, s) && (v->phase == TR_VOTE || v->phase == TR_VOTED || v->phase == TR_VAULT ||
                            v->phase == TR_RESULT))
    icon(TA_GOLD, x + 13, y + 13);
  if (s == v->shield && alive) icon(TA_SHIELD, x + 13, y);
  /* votes: a dot while they are cast, yes or no once shown */
  if (v->phase == TR_VOTED) {
    if (alive) api->fill(capp_rect(x, y + 15, 5, 5), (v->yes >> s) & 1 ? C_GOOD : C_BLOOD);
  } else if ((v->phase == TR_VOTE || v->phase == TR_TABLE) && ((v->voted >> s) & 1)) {
    api->fill(capp_rect(x + 1, y + 16, 3, 3), C_GOLD);
  }
  if (v->phase == TR_TABLED) {
    int k, count = 0;
    char n[4];
    for (k = 0; k < TR_N; k++) if (v->target[k] == s) count++;
    if (count) {
      api->fmt(n, sizeof n, "%d", count);
      api->fill(capp_rect(x + 13, y, 8, 9), C_BLOOD);
      api->text((int16_t)(x + 14), (int16_t)(y + 1), n, C_TEXT, C_BLOOD);
    }
  }
  /* the name under the portrait */
  w = (int)api->str_len(G.name[s]) * 6;
  {
    char nm[8];
    api->fmt(nm, sizeof nm, "%.6s", G.name[s]);
    w = (int)api->str_len(nm) * 6;
    api->text((int16_t)(SEAT_X[s] - w / 2), (int16_t)(y + TA_FACE + 1), nm,
              is_me(s) ? C_GOLD : alive ? C_TEXT : C_DIM, C_NIGHT);
  }
}

/* What lies on the table: the missions so far, and the round. */
static void paint_table(void) {
  const TrView *v = &G.v;
  char b[24];
  int i, n = v->clean + v->spoiled, x = 120 - n * 6;
  api->fmt(b, sizeof b, "Round %d", v->round);
  api->text_font(G.f_uib, (int16_t)(120 - api->text_width(G.f_uib, b) / 2), 33, b, C_GOLD,
                 CAPP_RGB(72, 38, 22));
  for (i = 0; i < v->clean; i++, x += 12) icon(TA_GOLD, x, 49);
  for (i = 0; i < v->spoiled; i++, x += 12) icon(TA_LEAD, x, 49);
  if (v->pot) {
    api->fmt(b, sizeof b, "%d gold", v->pot * 10);
    api->text((int16_t)(120 - (int)api->str_len(b) * 3), 59, b, C_TEXT, CAPP_RGB(78, 40, 24));
  }
}

/* Your role, big, at the start: a card over the table. */
static void paint_role_card(void) {
  const TrView *v = &G.v;
  int x = 50, y = 14, w = 140, h = 64, s;
  char b[64];
  api->fill(capp_rect(x, y, w, h), C_PANEL);
  frame(x, y, w, h, v->my_role == TR_TRAITOR ? C_BLOOD : C_GOLD);
  icon(v->my_role == TR_TRAITOR ? TA_CLOAK : TA_SHIELD, x + 8, y + 8);
  api->text_font(G.f_uib, (int16_t)(x + 22), (int16_t)(y + 6),
                 v->my_role == TR_TRAITOR ? "You are a TRAITOR" : "You are FAITHFUL",
                 v->my_role == TR_TRAITOR ? C_BLOOD : C_GOLD, C_PANEL);
  api->fmt(b, sizeof b, "You sit as %s.", G.name[G.me]);
  api->text(x + 8, y + h - 10, b, C_GOLD, C_PANEL);
  if (v->my_role == TR_TRAITOR) {
    b[0] = 0;
    for (s = 0; s < TR_N; s++)
      if (((v->traitors >> s) & 1) && s != G.me) api->fmt(b, sizeof b, "With you: %s", G.name[s]);
    api->text_font(G.f_ui, (int16_t)(x + 8), (int16_t)(y + 24), b, C_TEXT, C_PANEL);
    api->text_font(G.f_ui, (int16_t)(x + 8), (int16_t)(y + 38), "Spoil missions. Lie.", C_DIM, C_PANEL);
  } else {
    api->text_font(G.f_ui, (int16_t)(x + 8), (int16_t)(y + 24), "Two among you lie.", C_TEXT, C_PANEL);
    api->text_font(G.f_ui, (int16_t)(x + 8), (int16_t)(y + 38), "Find them. Banish them.", C_DIM, C_PANEL);
  }
}

static void paint_over(void) {
  const TrView *v = &G.v;
  int x = 40, y = 22, w = 160, h = 40, mine = v->winner == v->my_role;
  api->fill(capp_rect(x, y, w, h), C_PANEL);
  if (v->winner < 0) {                         /* the host left */
    frame(x, y, w, h, C_DIM);
    api->text_font(G.f_uib, (int16_t)(x + 10), (int16_t)(y + 12), "The game is over", C_TEXT, C_PANEL);
    return;
  }
  frame(x, y, w, h, v->winner == TR_TRAITOR ? C_BLOOD : C_GOLD);
  api->text_font(G.f_uib, (int16_t)(x + 10), (int16_t)(y + 6),
                 v->winner == TR_TRAITOR ? "The Traitors win" : "The Faithful win",
                 v->winner == TR_TRAITOR ? C_BLOOD : C_GOLD, C_PANEL);
  api->text_font(G.f_ui, (int16_t)(x + 10), (int16_t)(y + 22), mine ? "...and so do you." : "...and you do not.",
                 C_TEXT, C_PANEL);
}

/* The words under the hall: who speaks, and what, over two lines. */
static void paint_panel(CRect c) {
  int y = PANEL_Y, lh = api->font_height(G.f_ui), x = 6, avail, k, cut, len;
  if (lh > 14) lh = 14;
  char line[SAY_MAX], who[NAME_W + 2];
  const char *s = G.say;
  api->fill(capp_rect(0, y, 240, 1), C_EDGE);
  api->fill(capp_rect(0, y + 1, 240, c.h - FOOT_H - y - 1), C_PANEL);
  y += 1;
  if (G.say_seat >= 0) {
    api->fmt(who, sizeof who, "%s:", G.name[G.say_seat]);
    api->text_font(G.f_uib, (int16_t)x, (int16_t)y, who,
                   G.v.traitors >> G.say_seat & 1 ? C_BLOOD : C_GOLD, C_PANEL);
    x += api->text_width(G.f_uib, who) + 4;
  }
  /* two lines, broken at a space */
  for (k = 0; k < 2 && *s; k++) {
    avail = (k == 0 ? 228 - x : 222);
    len = (int)api->str_len(s);
    cut = len;
    api->fmt(line, sizeof line, "%s", s);
    while (cut > 0 && api->text_width(G.f_ui, line) > avail) {
      int b = cut - 1;
      while (b > 0 && s[b] != ' ') b--;
      cut = b > 0 ? b : cut - 1;
      api->fmt(line, sizeof line, "%.*s", cut, s);
    }
    api->text_font(G.f_ui, (int16_t)(k == 0 ? x : 6), (int16_t)y, line, G.say_seat < 0 ? C_DIM : C_TEXT,
                   C_PANEL);
    s += cut;
    while (*s == ' ') s++;
    y += lh;
  }
  /* your role, always in the corner */
  icon(G.v.my_role == TR_TRAITOR ? TA_CLOAK : TA_SHIELD, 229, PANEL_Y + 20);
}

/* The footer: what this seat is asked, in keys. */
static const char *hint(void) {
  static char b[48];
  const TrView *v = &G.v;
  if (G.asking) return "leave this game? y / n";
  if (G.screen == SC_GAME && G.v.phase == TR_OVER) return G.host ? "enter play again  esc leave" : "esc leave";
  if (talking()) return G.hold ? "they are thinking..." : "enter next";
  if (G.sent) return "waiting...";
  switch (v->need) {
  case TR_ASK_ACK: return "enter go on";
  case TR_ASK_CREW:
    api->fmt(b, sizeof b, "space pick %d of %d  enter send", tr_bits(G.pick), v->crew_size);
    return b;
  case TR_ASK_YESNO: return "send this crew?  y yes  n no";
  case TR_ASK_COIN: return v->my_role == TR_TRAITOR ? "your coin:  g gold  l lead" : "your coin:  g gold";
  case TR_ASK_SEAT:
    return v->phase == TR_SHIELD ? "arrows  enter give the shield" :
           v->phase == TR_NIGHT ? "arrows  enter murder" : "arrows  enter banish";
  }
  return "";
}

static void paint_game(CRect c) {
  int s;
  paint_hall();
  paint_table();
  for (s = 0; s < TR_N; s++) paint_seat(s);
  if (G.v.phase == TR_ROLES && G.v.need == TR_ASK_ACK) paint_role_card();
  if (G.v.phase == TR_OVER) paint_over();
  paint_panel(c);
  footer_paint(api, c, hint());
}

static void paint_title(CRect c) {
  int i, y = 54;
  paint_hall();
  api->fill(capp_rect(60, 8, 120, 20), C_PANEL);
  frame(60, 8, 120, 20, C_BLOOD);
  api->text_font(G.f_uib, (int16_t)(120 - api->text_width(G.f_uib, "THE TRAITORS") / 2), 11,
                 "THE TRAITORS", C_BLOOD, C_PANEL);
  icon(TA_CANDLE, 48, 12);
  icon(TA_CANDLE, 183, 12);
  for (i = 0; i < M_N; i++) {
    uint16_t bg = i == G.menu ? C_BLOOD : C_PANEL;
    api->fill(capp_rect(70, y, 100, 15), bg);
    api->text_font(G.f_ui, (int16_t)(120 - api->text_width(G.f_ui, MENU[i]) / 2), (int16_t)(y + 1), MENU[i],
                   C_TEXT, bg);
    y += 17;
  }
  api->fill(capp_rect(0, HALL_H, 240, c.h - FOOT_H - HALL_H), C_PANEL);
  api->text_font(G.f_ui, 6, HALL_H + 4, "Seven guests. Two Traitors. One castle.", C_DIM, C_PANEL);
  footer_paint(api, c, "enter start  fn-h rules");
}

static void paint_lobby(CRect c) {
  int i, y = 30, st = G.L ? G.L->state() : CAPP_LINK_OFF;
  char line[48];
  api->fill(capp_rect(0, 0, 240, c.h - FOOT_H), C_PANEL);
  api->text_font(G.f_uib, 10, 6, "Play nearby", C_GOLD, C_PANEL);
  G.lstate = st;
  G.lpeers = G.L ? G.L->peers() : 0;
  if (st == CAPP_LINK_INVITED) {
    api->fmt(line, sizeof line, "%.16s invites you to the castle.", G.L->peer_name(-1));
    api->text_font(G.f_ui, 12, (int16_t)y, line, C_TEXT, C_PANEL);
    footer_paint(api, c, "y join  n no");
    return;
  }
  if (st == CAPP_LINK_INVITING) {
    api->fmt(line, sizeof line, "asking %.16s...", G.L->peer_name(-1));
    api->text_font(G.f_ui, 12, (int16_t)y, line, C_TEXT, C_PANEL);
    footer_paint(api, c, "esc stop asking");
    return;
  }
  if (!G.lpeers)
    api->text_font(G.f_ui, 12, (int16_t)y, "looking for others with The Traitors open...", C_DIM, C_PANEL);
  if (G.lsel >= G.lpeers) G.lsel = G.lpeers ? G.lpeers - 1 : 0;
  for (i = 0; i < G.lpeers && i < 5; i++) {
    uint16_t bg = i == G.lsel ? C_BLOOD : C_PANEL;
    api->fill(capp_rect(8, y, 224, 16), bg);
    api->text_font(G.f_ui, 14, (int16_t)(y + 1), G.L->peer_name(i), C_TEXT, bg);
    y += 17;
  }
  if (G.note[0]) api->text(12, (int16_t)(c.h - FOOT_H - 12), G.note, C_BLOOD, C_PANEL);
  footer_paint(api, c, G.lpeers ? "enter invite  esc back" : "esc back");
}

static void app_paint(void *st, CRect c) {
  (void)st;
  if (G.screen == SC_TITLE) paint_title(c);
  else if (G.screen == SC_LOBBY) paint_lobby(c);
  else paint_game(c);
}

/* ---- answering ------------------------------------------------------------------- */

static void answer(int v) {
  if (G.host) {
    if (tr_act(&G.t, G.me, v)) { publish(); G.next_at = 0; }
    return;
  }
  if (G.L && G.L->state() == CAPP_LINK_CONNECTED) {
    uint8_t b[3];
    b[0] = 'A'; b[1] = (uint8_t)(v & 0xFF); b[2] = (uint8_t)((v >> 8) & 0xFF);
    G.L->send(b, 3);
    G.sent = 1;
  }
}

/* The cursor to the next seat it may stop on, d = +1 or -1 round the table. */
static void move_cursor(int d) {
  int i, s = G.cur;
  for (i = 0; i < TR_N; i++) {
    s = (s + d + TR_N) % TR_N;
    if (pickable(s)) { G.cur = s; return; }
  }
}

static void cursor_home(void) {
  if (!pickable(G.cur)) move_cursor(1);
}

static void leave_game(void) {
  if (G.L) G.L->close();
  G.net = 0;
  G.screen = SC_TITLE;
  G.asking = 0;
}

static int key_game(uint8_t k) {
  const TrView *v = &G.v;
  if (G.asking) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) leave_game();
    else if (a == CONFIRM_NO) G.asking = 0;
    return 1;
  }
  if (k == CAPP_KEY_ESC) {
    if (G.italk < G.ntalk) { G.italk = G.ntalk; G.talk_at = 0; return 1; }   /* hurry them */
    if (v->phase == TR_OVER) { leave_game(); return 1; }
    G.asking = 1;
    return 1;
  }
  if (api->key_repeat() && k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN)
    return 1;
  if (v->phase == TR_OVER) {
    if (k == CAPP_KEY_ENTER && G.host) new_game();
    return 1;
  }
  if (talking()) {
    if (k == CAPP_KEY_ENTER || k == ' ') G.talk_at = api->ticks_ms();    /* the next line now */
    return 1;
  }
  if (G.sent) return 1;
  switch (v->need) {
  case TR_ASK_ACK:
    if (k == CAPP_KEY_ENTER || k == ' ') answer(0);
    return 1;
  case TR_ASK_YESNO:
    if (k == 'y' || k == 'Y') answer(1);
    else if (k == 'n' || k == 'N') answer(0);
    return 1;
  case TR_ASK_COIN:
    if (k == 'g' || k == 'G') answer(0);
    else if ((k == 'l' || k == 'L') && v->my_role == TR_TRAITOR) answer(1);
    return 1;
  case TR_ASK_CREW: case TR_ASK_SEAT:
    if (k == CAPP_KEY_LEFT || k == CAPP_KEY_UP) { move_cursor(-1); return 1; }
    if (k == CAPP_KEY_RIGHT || k == CAPP_KEY_DOWN) { move_cursor(1); return 1; }
    if (v->need == TR_ASK_CREW) {
      if (k == ' ') {
        if (tr_in(G.pick, G.cur)) { if (G.cur != G.me) G.pick &= (uint8_t)~(1 << G.cur); }
        else if (tr_bits(G.pick) < v->crew_size) G.pick |= (uint8_t)(1 << G.cur);
        return 1;
      }
      if (k == CAPP_KEY_ENTER && tr_bits(G.pick) == v->crew_size) answer(G.pick);
      return 1;
    }
    if (k == CAPP_KEY_ENTER && pickable(G.cur)) answer(G.cur);
    return 1;
  }
  return 1;
}

static void lobby_open(void) {
  G.L = api->link();
  G.note[0] = 0;
  G.lsel = 0;
  G.lstate = -1;
  if (!G.L) { api->fmt(G.note, sizeof G.note, "this firmware cannot play nearby"); G.screen = SC_LOBBY; return; }
  G.screen = SC_LOBBY;
  if (G.L->state() == CAPP_LINK_OFF && G.L->open("traitors", 0) != 0)
    api->fmt(G.note, sizeof G.note, "%.46s", G.L->why());
}

static void start_game(int net, int host) {
  G.net = net;
  G.host = host;
  G.me = host ? 0 : 1;
  G.screen = SC_GAME;
  G.asking = 0;
  G.sent = 0;
  G.pick = 0;
  G.cur = G.me;
  G.say[0] = 0;
  G.say_seat = -1;
  G.ntalk = G.italk = 0;
  G.talk_at = 0;
  set_names();
  if (host) new_game();
  else {
    api->mem_set(&G.v, 0, sizeof G.v);
    G.v.me = (uint8_t)G.me;
    G.v.alive = 0x7F;
    G.v.round = 1;
    api->fmt(G.say, sizeof G.say, "Waiting for the host...");
  }
}

static int key_lobby(uint8_t k) {
  int st = G.L ? G.L->state() : CAPP_LINK_OFF;
  if (st == CAPP_LINK_INVITED) {
    if (k == 'y' || k == 'Y' || k == CAPP_KEY_ENTER) G.L->answer(1);
    else if (k == 'n' || k == 'N' || k == CAPP_KEY_ESC) G.L->answer(0);
    return 1;
  }
  if (k == CAPP_KEY_ESC) {
    if (st == CAPP_LINK_INVITING) { G.L->look(); return 1; }
    if (G.L) G.L->close();
    G.screen = SC_TITLE;
    return 1;
  }
  if (k == CAPP_KEY_UP && G.lsel > 0) G.lsel--;
  else if (k == CAPP_KEY_DOWN && G.lsel < G.lpeers - 1) G.lsel++;
  else if ((k == CAPP_KEY_ENTER || k == ' ') && st == CAPP_LINK_LOOKING && G.lpeers) {
    G.note[0] = 0;
    G.L->invite(G.lsel);
  }
  return 1;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (G.screen == SC_TITLE) {
    if (k == CAPP_KEY_UP && G.menu > 0) G.menu--;
    else if (k == CAPP_KEY_DOWN && G.menu < M_N - 1) G.menu++;
    else if (k == CAPP_KEY_ENTER || k == ' ') {
      if (G.menu == M_SOLO) start_game(0, 1);
      else lobby_open();
    } else return k == CAPP_KEY_ESC ? 0 : 1;
    return 1;
  }
  if (G.screen == SC_LOBBY) return key_lobby(k);
  {
    int r = key_game(k);
    if (G.screen == SC_GAME) cursor_home();
    return r;
  }
}

/* ---- the other device -------------------------------------------------------------- */

static int net_tick(void) {
  uint8_t b[TV_WIRE + SAY_MAX + 4];
  int n, st, redraw = 0;
  if (!G.L) return 0;
  st = G.L->state();
  if (G.screen == SC_LOBBY) {
    if (st == CAPP_LINK_CONNECTED) { start_game(1, G.L->role() == 0); return 1; }
    if (st != G.lstate || G.L->peers() != G.lpeers) return 1;
    return 0;
  }
  if (G.screen != SC_GAME || !G.net) return 0;
  if (st == CAPP_LINK_ENDED || st == CAPP_LINK_LOOKING) {
    if (G.host) {
      /* The guest went: the computer takes their seat, and the game goes on. */
      if (G.t.human[1]) {
        G.t.human[1] = 0;
        G.net = 0;
        api->fmt(G.name[1], NAME_W, "%s", TA_NAME[1]);
        castle("Silas's player has gone; the castle plays Silas now.");
        tr_enter(&G.t, G.t.phase);        /* the computer answers for it now */
        publish();
        return 1;
      }
    } else {
      api->fmt(G.say, sizeof G.say, "The host has gone: %s", G.L->why());
      G.say_seat = -1;
      G.v.phase = TR_OVER;
      G.v.winner = -1;
      G.net = 0;
      return 1;
    }
  }
  while ((n = G.L->recv(b, sizeof b - 1)) > 0) {
    if (G.host && b[0] == 'A' && n >= 3) {
      if (tr_act(&G.t, 1, b[1] | (b[2] << 8))) { publish(); G.next_at = 0; }
      redraw = 1;
    } else if (!G.host && b[0] == 'S') {
      int was = G.v.phase, need = G.v.need;
      if (tv_unpack(b, n, &G.v) == 0) {
        G.hold = n > 31 ? b[31] : 0;
        if (G.v.phase != was || G.v.need != need) { G.sent = 0; G.pick = 0; }
        if (G.v.need == TR_ASK_CREW && !G.pick) G.pick = (uint8_t)(1 << G.me);
        if (G.v.phase == TR_TABLE && was != TR_TABLE) { G.ntalk = G.italk = 0; }
        redraw = 1;
      }
    } else if (!G.host && b[0] == 'T' && n >= 2) {
      int seat = b[1] == 255 ? -1 : b[1];
      b[n] = 0;
      if (seat >= 0 && seat < TR_N) talk_add(seat, (const char *)b + 2);
      else { api->fmt(G.say, sizeof G.say, "%s", (const char *)b + 2); G.say_seat = -1; }
      redraw = 1;
    }
  }
  return redraw;
}

static int app_tick(void *st, uint32_t now) {
  int redraw = 0, need = G.v.need, phase = G.v.phase;
  (void)st;
  if (G.L && (G.screen == SC_LOBBY || G.net)) redraw |= net_tick();
  if (G.screen != SC_GAME) return redraw;
  if (G.host) redraw |= host_tick(now);
  redraw |= talk_show(now);
  if (G.v.phase != phase || G.v.need != need) {
    if (G.v.need == TR_ASK_CREW) G.pick = (uint8_t)(1 << G.me);
    cursor_home();
    redraw = 1;
  }
  return redraw;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "The Traitors",
  /* 16x16: a hooded figure. */
  { 0x07, 0xE0, 0x0F, 0xF0, 0x1F, 0xF8, 0x3C, 0x3C,
    0x38, 0x1C, 0x3A, 0x5C, 0x38, 0x1C, 0x3C, 0x3C,
    0x1E, 0x78, 0x3F, 0xFC, 0x7F, 0xFE, 0x7E, 0x7E,
    0xFE, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFE },
  "Seven guests at a castle; two are Traitors.\n"
  "\n"
  "mission\tthe leader picks a crew; all vote y/n.\n"
  "\tthree crews turned down spoil it\n"
  "vault\tthe crew drop coins: Faithful gold,\n"
  "\tTraitors gold or lead. One lead spoils it\n"
  "shield\ta clean mission's leader gives one;\n"
  "\tit stops one murder\n"
  "table\teveryone names someone: most banished\n"
  "night\tthe Traitors murder a Faithful\n"
  "\n"
  "Faithful win when no Traitor is left.\n"
  "Traitors win at two spoiled missions, or\n"
  "as many as the Faithful.\n"
  "\n"
  "arrows\tchoose a seat\n"
  "space\tpick for a crew\n"
  "enter\tsettle, go on, next line\n"
  "y n\tvote on a crew\n"
  "g l\tgold or lead\n"
  "esc\tleave (asks)\n",
};

static CappUi UI;

static int app_wants_text(void *st) { (void)st; return 0; }

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  G.f_ui = api->font_load("ui13");
  G.f_uib = api->font_load("ui13b");
  G.screen = SC_TITLE;
  G.say_seat = -1;
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
