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
#define TALK_MAX  8
#define CHAT_N    40
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
  char     say[SAY_MAX];               /* the last line in the room, for the hall's panel */
  int      say_seat;                   /* -1 the castle */
  struct { int8_t seat; char text[SAY_MAX]; } chat[CHAT_N];   /* the room, a ring */
  int      nchat;                      /* lines ever said; the newest is nchat - 1 */
  int      table_from;                 /* where this round table's talk began */
  struct { int8_t seat; char text[SAY_MAX]; } queue[TALK_MAX];  /* computers waiting to speak */
  int      nq, typing;                 /* typing: the seat "is typing...", or -1 */
  uint32_t q_at;
  int      chat_view, scroll;          /* the room on screen; lines scrolled back */
  char     input[SAY_MAX - 8];
  int      ilen;
  int      was_open;                   /* the table's talk was open last tick */
  int      seen_need, seen_view_phase; /* the view as the last tick left it */

  char     log[LOG_N][LOG_W];          /* the host's account of the game, for Claude */
  int      nlog;
  int      tk, tk_mode, want_reply;    /* TK_*: the talk request; open or reply; a reply owed */
  uint32_t tk_at, tk_until;
  char     tk_id[16];
  char     body[3000];
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

/* ---- the chat room ------------------------------------------------------------
 *
 * One transcript for the game: the castle's announcements and everyone's
 * words, a seat each (-1 the castle). The host keeps the game's account and
 * sends each line to the guest as it is said, so both rooms read the same.
 * A computer's line waits in a queue first, with "NAME is typing..." for a
 * moment as long as the line, so they speak one after another as people do. */

static void chat_add(int seat, const char *text) {
  int i = G.nchat % CHAT_N;
  G.chat[i].seat = (int8_t)seat;
  api->fmt(G.chat[i].text, SAY_MAX, "%s", text);
  G.nchat++;
  G.scroll = 0;                                   /* back to the newest */
  api->fmt(G.say, sizeof G.say, "%s", text);
  G.say_seat = seat;
}

/* The castle says something: in the room, to the guest, and in the log. */
static void castle(const char *s) {
  chat_add(-1, s);
  logf_(s);
  send_line(-1, s);
}

static void send_typing(int seat) {
  uint8_t b[2];
  if (!G.net || !G.host || !G.L || G.L->state() != CAPP_LINK_CONNECTED) return;
  b[0] = 'Y';
  b[1] = (uint8_t)(seat < 0 ? 255 : seat);
  G.L->send(b, 2);
}

static void queue_add(int seat, const char *text) {
  if (G.nq >= TALK_MAX || !text[0]) return;
  G.queue[G.nq].seat = (int8_t)seat;
  api->fmt(G.queue[G.nq].text, SAY_MAX, "%s", text);
  G.nq++;
}

/* The host lets the queue speak, a line at a time. 1 to repaint. */
static int queue_tick(uint32_t now) {
  int i, len;
  if (!G.nq) return 0;
  if (!G.q_at) {
    len = (int)api->str_len(G.queue[0].text);
    G.q_at = now + 700u + (uint32_t)(len * 22 > 2600 ? 2600 : len * 22);
    G.typing = G.queue[0].seat;
    send_typing(G.typing);
    return 1;
  }
  if ((int32_t)(now - G.q_at) < 0) return 0;
  chat_add(G.queue[0].seat, G.queue[0].text);
  logf_(G.queue[0].text);
  send_line(G.queue[0].seat, G.queue[0].text);
  for (i = 1; i < G.nq; i++) {
    G.queue[i - 1].seat = G.queue[i].seat;
    api->fmt(G.queue[i - 1].text, SAY_MAX, "%s", G.queue[i].text);
  }
  G.nq--;
  G.q_at = 0;
  G.typing = -1;
  if (!G.nq) send_typing(-1);
  return 1;
}

/* ---- what the computers say ------------------------------------------------------ */

static const char *const FAITH_LINES[] = {
  "Honestly? My gut says %s. Something's off.",
  "I'm going with %s. I really hope I'm wrong.",
  "%s has been way too quiet.",
  "Every time I think it through I land on %s.",
};
static const char *const TRAIT_LINES[] = {
  "Look at %s. Always around when it goes wrong.",
  "I trust everyone at this table except %s.",
  "%s, your story doesn't add up for me.",
  "Faithfuls, wake up. It's %s.",
};
static const char *const DEFEND_LINES[] = {
  "Me? I played that mission straight, I swear.",
  "That's such a reach and you know it.",
  "Vote me out then. You'll see I'm Faithful.",
  "Funny how fast you pointed at me.",
};
static const char *const MURMUR_LINES[] = {
  "Okay. Keep going.",
  "I hear you. I'm not sure yet.",
  "Maybe. Let's see how the vote goes.",
};

/* The seat a computer suspects most, for its words. */
static int top_suspect(int s) {
  return tr_best(&G.t, s, tr_alive_mask(&G.t) & (uint8_t)~(1 << s), 0, 0);
}

/* Case-blind: does `text` mention the name? */
static int mentions(const char *text, const char *name) {
  int i, k, n = (int)api->str_len(name);
  for (i = 0; text[i]; i++) {
    for (k = 0; k < n; k++) {
      char a = text[i + k], b = name[k];
      if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
      if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
      if (a != b) break;
    }
    if (k == n) return 1;
  }
  return 0;
}

/* The computers' own words, for when Claude cannot be asked: openings, or
 * an answer to the last thing a person said -- the named defend themselves,
 * and a name said leans two of the others toward it. */
static void talk_fallback(int mode, const char *last) {
  int s, n = 0, named = -1;
  char line[SAY_MAX];
  if (mode == 0) {
    for (s = 0; s < TR_N; s++) {
      int target;
      if (!G.t.alive[s] || G.t.human[s]) continue;
      target = top_suspect(s);
      if (target < 0) continue;
      api->fmt(line, sizeof line, G.t.role[s] == TR_TRAITOR ? TRAIT_LINES[tr_roll(&G.t, 4)]
                                                             : FAITH_LINES[tr_roll(&G.t, 4)], G.name[target]);
      queue_add(s, line);
    }
    return;
  }
  for (s = 0; s < TR_N; s++)
    if (G.t.alive[s] && last && mentions(last, G.name[s])) { named = s; break; }
  if (named >= 0 && !G.t.human[named]) queue_add(named, DEFEND_LINES[tr_roll(&G.t, 4)]);
  for (s = 0; s < TR_N && named >= 0 && n < 2; s++)
    if (s != named && G.t.alive[s] && !G.t.human[s] && tr_roll(&G.t, 2)) { tr_lean(&G.t, s, named, 200); n++; }
  if (named < 0 || G.t.human[named]) {
    for (s = tr_roll(&G.t, TR_N); ; s = (s + 1) % TR_N)
      if (G.t.alive[s] && !G.t.human[s]) { queue_add(s, MURMUR_LINES[tr_roll(&G.t, 3)]); break; }
  }
}

/* A name with no spaces, for the request's lines. */
static void token(const char *s, char *out, int n) {
  int k = 0;
  for (; *s && k < n - 1; s++) out[k++] = *s == ' ' ? '_' : *s;
  out[k] = 0;
}

static const char *last_said(void) {
  int i;
  for (i = G.nchat - 1; i >= 0 && i >= G.nchat - CHAT_N; i--)
    if (G.chat[i % CHAT_N].seat >= 0 && G.t.human[(int)G.chat[i % CHAT_N].seat]) return G.chat[i % CHAT_N].text;
  return 0;
}

/* Ask the server for the computers' words: mode 0 the round table's
 * openings, 1 an answer to what was just said. What it is told: who they
 * are (in secret), whom each suspects, what has happened, and the talk. */
static void talk_request(int mode) {
  int s, at = 0, i, from;
  char url[160], nm[NAME_W];
  if (G.tk == TK_POST || G.tk == TK_WAIT || G.tk == TK_POLL) { if (mode) G.want_reply = 1; return; }
  G.tk_mode = mode;
  if (!(api->caps_ok() & CAPP_CAP_PROXY)) { talk_fallback(mode, last_said()); return; }
  G.hold = 1;                                     /* "..." in the room until they answer */
  at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "mode %s\nround %d\n", mode ? "reply" : "open",
                 G.t.round);
  for (s = 0; s < TR_N; s++) {
    token(G.name[s], nm, sizeof nm);
    if (G.t.alive[s] && !G.t.human[s])
      at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "seat %d %s %s -1 %d\n", s, nm,
                     G.t.role[s] == TR_TRAITOR ? "traitor" : "faithful", top_suspect(s));
    else if (G.t.alive[s])
      at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "person %d %s\n", s, nm);
    else
      at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "gone %d %s %s\n", s, nm,
                     !G.t.shown[s] ? "murdered" : G.t.role[s] == TR_TRAITOR ? "banished-traitor"
                                                                           : "banished-faithful");
  }
  for (s = 0; s < TR_N; s++)
    at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "bio %d %s\n", s, TA_BIO[s]);
  for (i = 0; i < G.nlog; i++)
    at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "log %s\n", G.log[i]);
  from = G.nchat - 16 > G.table_from ? G.nchat - 16 : G.table_from;
  if (from < G.nchat - CHAT_N) from = G.nchat - CHAT_N;
  for (i = from; i < G.nchat; i++)
    if (G.chat[i % CHAT_N].seat >= 0)
      at += api->fmt(G.body + at, sizeof G.body - (size_t)at, "say %d %s\n", G.chat[i % CHAT_N].seat,
                     G.chat[i % CHAT_N].text);
  api->fmt(url, sizeof url, "%s/traitors/talk", api->proxy());
  if (api->http_start("POST", url, G.body, "text/plain", "", 20000) != 0) {
    G.hold = 0;
    talk_fallback(mode, last_said());
    return;
  }
  G.tk = TK_POST;
  G.tk_until = api->ticks_ms() + 45000;
}

/* "ok", then "SEAT\tTEXT" a speaker and "lean\tSEAT\tTARGET" a mind changed.
 * The number of lines queued. */
static int talk_parse(const char *r) {
  const char *p = r;
  char line[SAY_MAX];
  int n = 0;
  if (!str_starts(p, "ok")) return 0;
  while (*p && *p != '\n') p++;
  while (*p) {
    int seat, k = 0;
    if (*p == '\n') p++;
    if (str_starts(p, "lean\t")) {
      int o = p[5] - '0', target = p[7] - '0';
      if (p[6] == '\t' && o >= 0 && o < TR_N && target >= 0 && target < TR_N) tr_lean(&G.t, o, target, 300);
      while (*p && *p != '\n') p++;
      continue;
    }
    if (*p < '0' || *p > '9') break;
    seat = *p - '0';
    p++;
    if (*p != '\t') break;
    p++;
    while (*p && *p != '\n' && k < SAY_MAX - 1) line[k++] = *p++;
    line[k] = 0;
    while (*p && *p != '\n') p++;
    if (seat < TR_N && G.t.alive[seat] && !G.t.human[seat]) { queue_add(seat, line); n++; }
  }
  return n;
}

static void talk_done(int ok) {
  if (!ok) talk_fallback(G.tk_mode, last_said());
  G.tk = TK_DONE;
  G.hold = 0;
  if (G.want_reply) { G.want_reply = 0; talk_request(1); }
}

/* The request, a step a tick. 1 if anything changed on screen. */
static int talk_tick(uint32_t now) {
  int n;
  char url[200];
  if (G.tk != TK_POST && G.tk != TK_WAIT && G.tk != TK_POLL) return 0;
  if ((int32_t)(now - G.tk_until) > 0) { talk_done(0); return 1; }
  if (G.tk == TK_WAIT) {
    if ((int32_t)(now - G.tk_at) < 0) return 0;
    api->fmt(url, sizeof url, "%s/traitors/talk?id=%s", api->proxy(), G.tk_id);
    if (api->http_start("GET", url, 0, 0, "", 15000) == 0) G.tk = TK_POLL;
    else G.tk_at = now + 1000;
    return 0;
  }
  n = api->http_poll(G.reply, sizeof G.reply);
  if (n == CAPP_HTTP_PENDING) return 0;
  if (n < 0) { talk_done(0); return 1; }
  G.reply[n < (int)sizeof G.reply ? n : (int)sizeof G.reply - 1] = 0;
  if (G.tk == TK_POST) {
    int k = 0;
    while (G.reply[k] && G.reply[k] != '\n' && k < (int)sizeof G.tk_id - 1) { G.tk_id[k] = G.reply[k]; k++; }
    G.tk_id[k] = 0;
    if (!k || str_starts(G.reply, "error")) { talk_done(0); return 1; }
    G.tk = TK_WAIT;
    G.tk_at = now + 1500;
    return 0;
  }
  if (str_starts(G.reply, "pending")) { G.tk = TK_WAIT; G.tk_at = now + 1200; return 0; }
  talk_done(talk_parse(G.reply) > 0);
  return 1;
}

/* A person (this one, or the guest) said something. */
static void person_says(int seat, const char *text) {
  chat_add(seat, text);
  logf_(text);
  send_line(seat, text);
  talk_request(1);
}

/* The computers are still speaking: being written, or waiting their turn. */
static int talking(void) {
  return G.hold || G.nq > 0 || G.typing >= 0;
}

/* ---- the host: narration, and telling the guest ---------------------------------- */

/* The pot, in dollars: TR_GOLD a crew member is $500 a unit. */
static int pot_k(int pot) { return pot / 2; }

/* A contestant says something themselves -- the banished, revealing. */
static void speak_as(int seat, const char *text) {
  chat_add(seat, text);
  logf_(text);
  send_line(seat, text);
}

/* What the host says as each part of the day begins. */
static void narrate(int phase) {
  char a[80], b[SAY_MAX];
  Tr *t = &G.t;
  int s, yes = 0, no = 0;
  switch (phase) {
  case TR_PROPOSE:
    api->fmt(b, sizeof b, "Round %d. %s leads today's mission.", t->round, G.name[t->leader]);
    castle(b);
    break;
  case TR_VOTE:
    list_names(t->crew, a, sizeof a);
    api->fmt(b, sizeof b, "%s picks %s. Does the group agree?", G.name[t->leader], a);
    castle(b);
    break;
  case TR_VOTED:
    for (s = 0; s < TR_N; s++) { if (t->vote[s] == 1) yes++; else if (t->vote[s] == 0) no++; }
    if (t->sent) api->fmt(b, sizeof b, "The team goes, %d votes to %d.", yes, no);
    else api->fmt(b, sizeof b, "Rejected, %d to %d. That's %d of %d.", yes, no, t->rejects + 1, TR_REJECTS);
    castle(b);
    break;
  case TR_VAULT:
    castle("The mission begins. Each of them, in secret: play it straight, or sabotage.");
    break;
  case TR_RESULT:
    if (t->lead < 0) api->fmt(b, sizeof b, "Three teams rejected. No mission, no money.");
    else if (t->lead == 0)
      api->fmt(b, sizeof b, "A clean mission. $%d,000 goes in the prize pot.", pot_k(TR_GOLD * tr_bits(t->crew)));
    else api->fmt(b, sizeof b, "Sabotage. %d of them didn't play it straight. Nothing for the pot.", t->lead);
    castle(b);
    break;
  case TR_SHIELD:
    api->fmt(b, sizeof b, "%s, you've won a shield. Who gets it?", G.name[t->leader]);
    castle(b);
    break;
  case TR_TABLE:
    if (t->shield >= 0) {
      api->fmt(b, sizeof b, "%s has a shield tonight. Please, take your seats at the round table.",
               G.name[t->shield]);
      castle(b);
    } else castle("Please, take your seats at the round table.");
    break;
  case TR_TABLED:
    if (t->banished < 0) castle("A tie. Nobody leaves the castle tonight.");
    else {
      api->fmt(b, sizeof b, "%s, you have been banished. Tell them who you are.", G.name[t->banished]);
      castle(b);
      speak_as(t->banished, t->role[t->banished] == TR_TRAITOR ? "I am... a Traitor." : "I am... a Faithful.");
    }
    break;
  case TR_NIGHT:
    castle("Night falls. The Traitors meet in the turret.");
    break;
  case TR_BREAKFAST:
    if (t->murdered < 0) castle("Everyone made it to breakfast. The Traitors didn't strike.");
    else {
      api->fmt(b, sizeof b, "One of you won't be joining us for breakfast. %s was murdered.", G.name[t->murdered]);
      castle(b);
    }
    break;
  case TR_OVER: {
    uint8_t all = 0;
    for (s = 0; s < TR_N; s++) if (t->role[s] == TR_TRAITOR) all |= (uint8_t)(1 << s);
    list_names(all, a, sizeof a);
    if (t->winner == TR_FAITHFUL)
      api->fmt(b, sizeof b, "The Faithful win, and split $%d,000. The Traitors were %s.", pot_k(t->pot), a);
    else
      api->fmt(b, sizeof b, "The Traitors win, and take all $%d,000. They were %s.", pot_k(t->pot), a);
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
    if (G.t.phase == TR_TABLE) {
      G.table_from = G.nchat;
      G.chat_view = 1;
      G.was_open = G.t.talk_open;
      talk_request(0);
    }
  }
}

/* The host settles a phase once everything is in, after a pause for the show. */
static int host_tick(uint32_t now) {
  int changed = talk_tick(now);
  changed |= queue_tick(now);
  if (G.t.phase == TR_TABLE && G.was_open && !G.t.talk_open) {
    G.was_open = 0;
    castle("Time's up. It's time to vote.");
    changed = 1;
  }
  if (changed) publish();
  if (G.t.phase == TR_OVER || !tr_ready(&G.t) || G.hold) { G.next_at = 0; return changed; }
  if (G.t.phase == TR_TABLE && talking()) return changed;           /* let them finish speaking */
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
  G.nchat = G.nq = G.q_at = G.scroll = G.ilen = 0;
  G.input[0] = 0;
  G.typing = -1;
  G.chat_view = 0;
  G.want_reply = 0;
  G.hold = 0;
  G.tk = TK_IDLE;
  G.seen_phase = TR_ROLES;
  G.next_at = 0;
  castle("Welcome to the castle. Seven of you. Among you, two Traitors.");
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
      if (y + r >= 0 && x + s >= 0) api->fill(capp_rect(x + s, y + r, c - s, 1), TA_IPAL[v]);
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
    if (grey && v > 1) v = v == 4 || v == 9 ? 1 : 12;   /* gone: grey, the darks black */
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
  if (pickable(s) && s == G.cur) { edge = C_CURSOR; has_edge = 1; }
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
    api->fmt(b, sizeof b, "$%d,000", pot_k(v->pot * 10));
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
                 v->my_role == TR_TRAITOR ? "You are a Traitor." : "You are a Faithful.",
                 v->my_role == TR_TRAITOR ? C_BLOOD : C_GOLD, C_PANEL);
  api->fmt(b, sizeof b, "%s, %s", G.name[G.me], TA_BIO[G.me]);
  api->text(x + 8, y + h - 10, b, C_GOLD, C_PANEL);
  if (v->my_role == TR_TRAITOR) {
    b[0] = 0;
    for (s = 0; s < TR_N; s++)
      if (((v->traitors >> s) & 1) && s != G.me) api->fmt(b, sizeof b, "The other Traitor: %s", G.name[s]);
    api->text_font(G.f_ui, (int16_t)(x + 8), (int16_t)(y + 24), b, C_TEXT, C_PANEL);
    api->text_font(G.f_ui, (int16_t)(x + 8), (int16_t)(y + 38), "Sabotage. Murder. Lie.", C_DIM, C_PANEL);
  } else {
    api->text_font(G.f_ui, (int16_t)(x + 8), (int16_t)(y + 24), "Two of them are lying.", C_TEXT, C_PANEL);
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
  {
    char b[48];
    api->fmt(b, sizeof b, "$%d,000. %s", pot_k(v->pot * 10), mine ? "You win." : "You lose.");
    api->text_font(G.f_ui, (int16_t)(x + 10), (int16_t)(y + 22), b, C_TEXT, C_PANEL);
  }
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

/* ---- painting: the chat room --------------------------------------------------- */

#define CH_TOP    14                  /* under the title */
#define CH_IN_H   15                  /* the line being typed */
#define CH_LH     13

/* A portrait at half size: every other pixel. */
static void mini_face(int s, int x, int y, int grey) {
  uint16_t px[10 * 10];
  int r, c;
  for (r = 0; r < 10; r++)
    for (c = 0; c < 10; c++) {
      int i = (r * 2) * TA_FACE + c * 2;
      uint8_t b = TA_FACES[s][i / 2];
      int v = b >> 4;
      if (grey && v > 1) v = 12;
      px[r * 10 + c] = TA_PAL[v];
    }
  api->pixels(capp_rect(x, y, 10, 10), px);
}

/* Break `s` into lines of at most `w` pixels in font f: the starts and
 * lengths, at most `max` of them. A word too long breaks inside. */
static int wrap(int f, const char *s, int w, int *start, int *len, int max) {
  char buf[SAY_MAX];
  int n = 0, p = 0, total = (int)api->str_len(s);
  while (p < total && n < max) {
    int cut = total - p, b;
    while (s[p] == ' ') p++;
    cut = total - p;
    for (;;) {
      api->fmt(buf, sizeof buf, "%.*s", cut, s + p);
      if (api->text_width(f, buf) <= w || cut <= 1) break;
      for (b = cut - 1; b > 0 && s[p + b] != ' '; b--) {}
      cut = b > 0 ? b : cut - 1;
    }
    start[n] = p;
    len[n] = cut;
    n++;
    p += cut;
  }
  return n;
}

/* One message, its bottom at `bottom`; its top, or `top` if it would not
 * fit above it (then it is not drawn). */
static int paint_message(int i, int bottom, int top) {
  int seat = G.chat[i % CHAT_N].seat, st[5], ln[5], n, k, h, x, y, w;
  const char *text = G.chat[i % CHAT_N].text;
  char who[NAME_W + 2], buf[SAY_MAX];
  int castle_line = seat < 0;
  if (castle_line) {
    n = wrap(G.f_ui, text, 228, st, ln, 4);
    h = n * CH_LH + 3;
    if (bottom - h < top) return top;
    y = bottom - h + 1;
    api->fill(capp_rect(0, bottom - h, 240, 1), CAPP_RGB(46, 34, 52));
    for (k = 0; k < n; k++) {
      api->fmt(buf, sizeof buf, "%.*s", ln[k], text + st[k]);
      api->text_font(G.f_ui, 6, (int16_t)(y + 1 + k * CH_LH), buf, C_DIM, C_PANEL);
    }
    return bottom - h;
  }
  api->fmt(who, sizeof who, "%s ", G.name[seat]);
  w = api->text_width(G.f_uib, who);
  /* the name opens the first line; the words wrap under it from x 16 */
  n = wrap(G.f_ui, text, 222, st, ln, 5);
  if (n && api->text_width(G.f_ui, text) + w > 222) {
    /* re-wrap so the first line leaves room for the name */
    int s0[5], l0[5], m;
    m = wrap(G.f_ui, text, 222 - w, s0, l0, 1);
    if (m) {
      st[0] = s0[0]; ln[0] = l0[0];
      n = 1 + wrap(G.f_ui, text + st[0] + ln[0], 222, st + 1, ln + 1, 4);
      for (k = 1; k < n; k++) st[k] += st[0] + ln[0];
    }
  }
  h = (n ? n : 1) * CH_LH + 3;
  if (h < 13) h = 13;
  if (bottom - h < top) return top;
  y = bottom - h + 2;
  x = 16;
  mini_face(seat, 3, y + 1, 0);
  api->text_font(G.f_uib, (int16_t)x, (int16_t)y, who,
                 seat == G.me ? C_GOLD : ((G.v.traitors >> seat) & 1) ? C_BLOOD : C_PICK, C_PANEL);
  for (k = 0; k < n; k++) {
    api->fmt(buf, sizeof buf, "%.*s", ln[k], text + st[k]);
    api->text_font(G.f_ui, (int16_t)(k == 0 ? x + w : x), (int16_t)(y + k * CH_LH), buf, C_TEXT, C_PANEL);
  }
  return bottom - h;
}

static void paint_chat(CRect c) {
  int bottom = c.h - FOOT_H - CH_IN_H, i, skipped = 0;
  char b[SAY_MAX + 4], title[40];
  /* the title */
  api->fill(capp_rect(0, 0, 240, CH_TOP), C_NIGHT);
  api->fmt(title, sizeof title, G.v.phase == TR_TABLE ? "The round table" : "Round %d", G.v.round);
  api->text_font(G.f_uib, 6, 0, title, C_GOLD, C_NIGHT);
  api->text(170, 3, "tab: hall", C_DIM, C_NIGHT);
  api->fill(capp_rect(0, CH_TOP - 1, 240, 1), C_EDGE);
  /* the messages, newest at the bottom, scrolled back by `scroll` */
  api->fill(capp_rect(0, CH_TOP, 240, bottom - CH_TOP), C_PANEL);
  if (G.typing >= 0 && G.typing < TR_N && !G.scroll) {
    api->fmt(b, sizeof b, "%s is typing...", G.name[G.typing]);
    api->text_font(G.f_ui, 16, (int16_t)(bottom - CH_LH - 1), b, C_DIM, C_PANEL);
    bottom -= CH_LH + 1;
  } else if (G.hold && !G.scroll) {
    api->text_font(G.f_ui, 16, (int16_t)(bottom - CH_LH - 1), "...", C_DIM, C_PANEL);
    bottom -= CH_LH + 1;
  }
  for (i = G.nchat - 1; i >= 0 && i >= G.nchat - CHAT_N; i--) {
    int top;
    if (skipped < G.scroll) { skipped++; continue; }
    top = paint_message(i, bottom, CH_TOP);
    if (top == CH_TOP && bottom != CH_TOP) break;
    bottom = top;
    if (bottom <= CH_TOP) break;
  }
  /* the line being typed */
  {
    int y = c.h - FOOT_H - CH_IN_H;
    api->fill(capp_rect(0, y, 240, CH_IN_H), CAPP_RGB(36, 26, 42));
    api->fill(capp_rect(0, y, 240, 1), C_EDGE);
    if (G.ilen) {
      api->fmt(b, sizeof b, "%s_", G.input);
      /* the end of it, if it is longer than the line */
      while (api->text_width(G.f_ui, b) > 228 && b[0]) api->fmt(b, sizeof b, "%s", b + 1);
      api->text_font(G.f_ui, 6, (int16_t)(y + 1), b, C_TEXT, CAPP_RGB(36, 26, 42));
    } else {
      api->text_font(G.f_ui, 6, (int16_t)(y + 1),
                     G.v.need == TR_ASK_TALK ? "say something  (enter alone: ready)"
                                             : "say something...",
                     C_DIM, CAPP_RGB(36, 26, 42));
    }
  }
}

/* The footer: what this seat is asked, in keys. */
static const char *hint(void) {
  static char b[48];
  const TrView *v = &G.v;
  if (G.asking) return "leave this game? y / n";
  if (G.screen == SC_GAME && G.v.phase == TR_OVER) return G.host ? "enter play again  esc leave" : "esc leave";
  if (G.sent) return "waiting...";
  switch (v->need) {
  case TR_ASK_ACK: return "enter go on  tab chat";
  case TR_ASK_TALK: return "the table is talking: tab";
  case TR_ASK_CREW:
    api->fmt(b, sizeof b, "space pick %d of %d  enter send", tr_bits(G.pick), v->crew_size);
    return b;
  case TR_ASK_YESNO: return "send this crew?  y yes  n no";
  case TR_ASK_COIN: return v->my_role == TR_TRAITOR ? "enter play it straight  s sabotage" : "enter play it straight";
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
  api->fill(capp_rect(60, 6, 120, 30), C_PANEL);
  frame(60, 6, 120, 30, C_BLOOD);
  api->text_font(G.f_uib, (int16_t)(120 - api->text_width(G.f_uib, "TRAITORS") / 2), 8,
                 "TRAITORS", C_BLOOD, C_PANEL);
  api->text(120 - 4 * 6, 25, "THE GAME", C_DIM, C_PANEL);
  icon(TA_CANDLE, 48, 14);
  icon(TA_CANDLE, 183, 14);
  for (i = 0; i < M_N; i++) {
    uint16_t bg = i == G.menu ? C_BLOOD : C_PANEL;
    api->fill(capp_rect(70, y, 100, 15), bg);
    api->text_font(G.f_ui, (int16_t)(120 - api->text_width(G.f_ui, MENU[i]) / 2), (int16_t)(y + 1), MENU[i],
                   C_TEXT, bg);
    y += 17;
  }
  api->fill(capp_rect(0, HALL_H, 240, c.h - FOOT_H - HALL_H), C_PANEL);
  api->text_font(G.f_ui, 6, HALL_H + 4, "Seven players. Two Traitors. One prize pot.", C_DIM, C_PANEL);
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
  else if (G.chat_view) {
    paint_chat(c);
    footer_paint(api, c, G.ilen ? "enter say it  esc clear" : "type  enter say  tab hall");
  } else paint_game(c);
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

/* Something typed into the room, or said. */
static void say_it(void) {
  if (!G.ilen) return;
  if (G.host) person_says(G.me, G.input);
  else if (G.L && G.L->state() == CAPP_LINK_CONNECTED) {
    uint8_t b[2 + SAY_MAX];
    int n = G.ilen;
    b[0] = 'C';
    api->mem_cpy(b + 1, G.input, (size_t)n);
    G.L->send(b, n + 1);
  }
  G.ilen = 0;
  G.input[0] = 0;
  G.scroll = 0;
}

static int key_chat(uint8_t k) {
  int lines = G.nchat < CHAT_N ? G.nchat : CHAT_N;
  if (k == CAPP_KEY_ESC) {
    if (G.ilen) { G.ilen = 0; G.input[0] = 0; }
    else G.chat_view = 0;
    return 1;
  }
  if (k == CAPP_KEY_UP) { if (G.scroll < lines - 1) G.scroll++; return 1; }
  if (k == CAPP_KEY_DOWN) { if (G.scroll > 0) G.scroll--; return 1; }
  if (k == CAPP_KEY_BACK) { if (G.ilen) G.input[--G.ilen] = 0; return 1; }
  if (k == CAPP_KEY_ENTER) {
    if (api->key_repeat()) return 1;
    if (G.ilen) say_it();
    else if (G.v.need == TR_ASK_TALK && !G.sent) answer(1);      /* ready to vote */
    return 1;
  }
  if (k >= 32 && k < 127 && G.ilen < (int)sizeof G.input - 1) {
    G.input[G.ilen++] = (char)k;
    G.input[G.ilen] = 0;
  }
  return 1;
}

static int key_game(uint8_t k) {
  const TrView *v = &G.v;
  if (G.asking) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) leave_game();
    else if (a == CONFIRM_NO) G.asking = 0;
    return 1;
  }
  if (k == '\t') {                                 /* the hall, or the room */
    G.chat_view = !G.chat_view;
    G.scroll = 0;
    return 1;
  }
  if (G.chat_view) return key_chat(k);
  if (k == CAPP_KEY_ESC) {
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
  if (G.sent) return 1;
  if (v->need == TR_ASK_TALK) {                    /* the table is talking: that is the room */
    if (k == CAPP_KEY_ENTER) G.chat_view = 1;
    return 1;
  }
  switch (v->need) {
  case TR_ASK_ACK:
    if (k == CAPP_KEY_ENTER || k == ' ') answer(0);
    return 1;
  case TR_ASK_YESNO:
    if (k == 'y' || k == 'Y') answer(1);
    else if (k == 'n' || k == 'N') answer(0);
    return 1;
  case TR_ASK_COIN:
    if (k == CAPP_KEY_ENTER || k == 'g' || k == 'G') answer(0);
    else if ((k == 's' || k == 'S') && v->my_role == TR_TRAITOR) answer(1);
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
  G.nchat = G.nq = G.q_at = G.scroll = G.ilen = 0;
  G.input[0] = 0;
  G.typing = -1;
  G.chat_view = 0;
  set_names();
  if (host) new_game();
  else {
    api->mem_set(&G.v, 0, sizeof G.v);
    G.v.me = (uint8_t)G.me;
    G.v.alive = 0x7F;
    G.v.round = 1;
    chat_add(-1, "Waiting for the host...");
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
        {
          char b2[SAY_MAX];
          api->fmt(b2, sizeof b2, "%s's player has left. The game plays %s now.", G.name[1], G.name[1]);
          castle(b2);
        }
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
    } else if (G.host && b[0] == 'C' && n >= 2 && G.t.human[1]) {
      b[n] = 0;
      person_says(1, (const char *)b + 1);
      redraw = 1;
    } else if (!G.host && b[0] == 'S') {
      int was = G.v.phase, need = G.v.need;
      if (tv_unpack(b, n, &G.v) == 0) {
        G.hold = n > 31 ? b[31] : 0;
        if (G.v.phase != was || G.v.need != need) { G.sent = 0; G.pick = 0; }
        if (G.v.need == TR_ASK_CREW && !G.pick) G.pick = (uint8_t)(1 << G.me);
        redraw = 1;
      }
    } else if (!G.host && b[0] == 'T' && n >= 2) {
      int seat = b[1] == 255 ? -1 : b[1];
      b[n] = 0;
      chat_add(seat < TR_N ? seat : -1, (const char *)b + 2);
      if (seat == G.typing) G.typing = -1;
      redraw = 1;
    } else if (!G.host && b[0] == 'Y' && n >= 2) {
      G.typing = b[1] < TR_N ? b[1] : -1;
      redraw = 1;
    }
  }
  return redraw;
}

static int app_tick(void *st, uint32_t now) {
  int redraw = 0, need = G.seen_need, phase = G.seen_view_phase;
  (void)st;
  if (G.L && (G.screen == SC_LOBBY || G.net)) redraw |= net_tick();
  if (G.screen != SC_GAME) return redraw;
  if (G.host) redraw |= host_tick(now);
  if (G.v.phase != phase || G.v.need != need) {
    if (G.v.need == TR_ASK_CREW) G.pick = (uint8_t)(1 << G.me);
    if (G.v.need == TR_ASK_TALK) G.chat_view = 1;             /* the round table opens the room */
    if (need == TR_ASK_TALK && G.v.need != TR_ASK_TALK) G.chat_view = 0;   /* and the vote, the hall */
    cursor_home();
    redraw = 1;
  }
  /* What the last tick saw: an answer given in a key handler changes the
   * view before any tick does, and must still count as a change. */
  G.seen_need = G.v.need;
  G.seen_view_phase = G.v.phase;
  return redraw;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Traitors",
  /* 16x16: a hooded figure. */
  { 0x07, 0xE0, 0x0F, 0xF0, 0x1F, 0xF8, 0x3C, 0x3C,
    0x38, 0x1C, 0x3A, 0x5C, 0x38, 0x1C, 0x3C, 0x3C,
    0x1E, 0x78, 0x3F, 0xFC, 0x7F, 0xFE, 0x7E, 0x7E,
    0xFE, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xFE },
  "Traitors: The Game. Seven players at a\n"
  "castle; two are Traitors.\n"
  "\n"
  "mission\tthe leader picks a team; all vote y/n.\n"
  "\tthree teams rejected and there's no mission\n"
  "\tthe team secretly play it straight or (a\n"
  "\tTraitor) sabotage: any sabotage, no money\n"
  "shield\ta clean mission's leader gives one;\n"
  "\tit stops one murder\n"
  "table\tthe talk, then everyone names someone:\n"
  "\tthe most named is banished\n"
  "night\tthe Traitors murder a Faithful\n"
  "\n"
  "Faithful win when no Traitor is left.\n"
  "Traitors win at two sabotaged missions, or\n"
  "as many as the Faithful.\n"
  "\n"
  "tab\tthe room: everyone's words; type to talk,\n"
  "\tenter says it. At the round table, enter\n"
  "\ton an empty line: ready to vote\n"
  "arrows\tchoose a seat\n"
  "space\tpick for a crew\n"
  "enter\tsettle, go on, next line\n"
  "y n\tvote on a crew\n"
  "s\ta Traitor's sabotage, on a mission\n"
  "esc\tleave (asks)\n",
};

static CappUi UI;

static int app_wants_text(void *st) { (void)st; return G.screen == SC_GAME && G.chat_view; }

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
