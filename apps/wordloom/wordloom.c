/* Wordloom: a word game for two friends, played a move at a time, hours
 * apart or at once, through the server's rooms (server/rooms.py).
 *
 * The rules, the loom, the tiles and the move format are wordloom.h. This is
 * the screen and the network:
 *
 *   the games   every Wordloom room you are in, latest first: who with, the
 *               score, and whose turn. n starts one with a friend (the
 *               friend picker, apps/social.h), Enter opens one, r reads the
 *               list again.
 *   a game      the 11x11 loom on the left; on the right the scores, the
 *               bag, the last move, what the tiles you have put down would
 *               score, and your rack. Type a letter to lay it at the cursor
 *               (it steps on along the direction, over tiles already
 *               there); Space turns the direction, Backspace takes the last
 *               one back, Esc takes them all back (and with none down, goes
 *               back to the games). Enter weaves. Tab is the rest: swap,
 *               pass, resign.
 *
 * WHAT IS SENT. A room's state is the seed (wl_state_format); the players
 * are its members, creator first. Each move is one room move ("play 5 4 a
 * crow", "swap qx", "pass", "resign") sent with say= so the other player is
 * told -- and only them: nothing else is ever posted. Every device replays
 * the moves from the seed (wl_replay), so nothing is ever rewritten.
 *
 * WHAT IS KEPT. /var/wordloom/ID.txt, one per room: "wl <tab> SEED <tab> P0
 * <tab> P1", then the moves seen so far, "SEQ <tab> FROM <tab> MOVE" a line
 * (apps/safefile.h). The list scores games from these without asking the
 * server for every room; a room whose last move is newer than the file is
 * read, one request at a time, after the list comes. While a game is on
 * screen its new moves are asked for every 3 s, only when nothing else is
 * in flight. /var/wordloom/me.txt is your own account name: the server never
 * says it, so it is the member of a room who is not among your friends.
 *
 * Words are on honour: there is no dictionary on the device (README.md).
 *
 * @app folder Games
 * @app release extra
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/footer.h"
#include "apps/safefile.h"
#include "apps/confirm.h"
#include "apps/social.h"
#include "apps/wordloom/wordloom.h"

static const CardApi *api;

#define APP_ID     "wordloom"
#define DATA_DIR   CAPP_VAR "/wordloom"
#define ME_PATH    DATA_DIR "/me.txt"
#define ROOMS_MAX  12
#define LOG_MAX    6144
#define REPLY_MAX  4096
#define POLL_MS    3000
#define MOVE_MAX   60

/* The loom: 11 cells of 10 px with 1 px between, from (1, 1). */
#define CELL   10
#define PITCH  11
#define BX     1
#define BY     1
/* The panel on the right. */
#define PX     126
#define PW     (240 - PX - 2)
#define RACK_Y 96
#define TILE_W 17
#define TILE_H 20
#define TILE_P 19

#define C_BG      CAPP_RGB(26, 22, 30)
#define C_CELL    CAPP_RGB(52, 46, 60)
#define C_KNOT    CAPP_RGB(44, 112, 118)
#define C_GOLD    CAPP_RGB(176, 128, 40)
#define C_WARP    CAPP_RGB(150, 62, 92)
#define C_LOOM    CAPP_RGB(104, 64, 168)
#define C_TILE    CAPP_RGB(240, 228, 200)
#define C_FRESH   CAPP_RGB(255, 214, 120)
#define C_TENT    CAPP_RGB(150, 210, 255)
#define C_INK     CAPP_RGB(36, 30, 40)
#define C_TEXT    CAPP_RGB(238, 232, 240)
#define C_DIM     CAPP_RGB(140, 132, 150)
#define C_GOOD    CAPP_RGB(120, 210, 140)
#define C_WARN    CAPP_RGB(240, 150, 90)
#define C_SEL     CAPP_RGB(70, 60, 92)
#define C_CURSOR  CAPP_RGB(255, 255, 255)
#define C_CURDOWN CAPP_RGB(255, 220, 90)
#define C_MARK    CAPP_RGB(240, 120, 110)
static const uint16_t C_PLAYER[2] = { CAPP_RGB(255, 196, 92), CAPP_RGB(126, 196, 255) };

enum { V_LIST, V_GAME };
enum { M_PLACE, M_MENU, M_SWAP, M_ASK_PASS, M_ASK_RESIGN };
enum { NET_NONE, NET_FRIENDS, NET_LIST, NET_GET, NET_MOVES, NET_SEND, NET_NEW };
enum { MENU_SWAP, MENU_PASS, MENU_RESIGN, MENU_N };
static const char *const MENU_TEXT[MENU_N] = { "Swap tiles", "Pass", "Resign" };

typedef struct {
  int  id;
  int  seq;                   /* the server's last move */
  int  have;                  /* the last move in our file */
  int  known;                 /* the file has the seed */
  int  bad;                   /* not a game this version can play */
  char p[2][SOC_NAME];        /* the players, creator first */
  /* From replaying the file. */
  int  score[2], turn, over, winner;
} Room;

static struct {
  int      view, mode;
  /* The games. */
  Room     rooms[ROOMS_MAX];
  int      nrooms, sel, top;
  int      listed;            /* a list has come */
  int      list_failed;
  SocFriend friends[SOC_FRIENDS];
  int      nfriends;
  char     me[SOC_NAME];
  /* The network: one request at a time. */
  int      net, net_room;
  int      want_friends, want_list;
  int      new_id;            /* a room just made: select it when listed */
  char     new_with[SOC_NAME];
  uint32_t next_poll;
  char     reply[REPLY_MAX];
  char     sending[MOVE_MAX + 4];
  /* The file of one room: the open game's, or the one being read. */
  int      log_room;
  uint32_t seed;
  char     log[LOG_MAX];
  int      log_len;
  /* The open game. */
  int      cur, me_idx, skipped;
  WlGame   g;
  int8_t   tr[WL_RACK], tc[WL_RACK];
  char     tl[WL_RACK];
  int      nt;
  int      cr, cc, dir;
  int      menu_sel;
  uint8_t  swap_mark[WL_RACK];
  /* A line of news under the score, for a while. */
  char     msg[44];
  uint16_t msg_colour;
  uint32_t msg_until;
  SocPick  pick;
  int      f_ui, f_b;
} G;

/* ---- small things ------------------------------------------------------------ */

static void say(const char *s, uint16_t colour, uint32_t ms) {
  api->fmt(G.msg, sizeof G.msg, "%s", s);
  G.msg_colour = colour;
  G.msg_until = ms ? api->ticks_ms() + ms : 0;
}

static void copy(char *out, int n, const char *s) {
  int i = 0;
  while (s && s[i] && i < n - 1) { out[i] = s[i]; i++; }
  out[i] = 0;
}

static int is_friend(const char *name) {
  int i;
  for (i = 0; i < G.nfriends; i++) if (str_same(G.friends[i].name, name)) return 1;
  return 0;
}

/* The name to show for an account: "You", a friend's chosen name, or it. */
static const char *who(const char *name) {
  int i;
  if (G.me[0] && str_same(name, G.me)) return "You";
  for (i = 0; i < G.nfriends; i++)
    if (str_same(G.friends[i].name, name) && G.friends[i].display[0]) return G.friends[i].display;
  return name;
}

static int me_in(const Room *r) {
  if (!G.me[0]) return -1;
  return str_same(r->p[0], G.me) ? 0 : str_same(r->p[1], G.me) ? 1 : -1;
}

static void room_path(char *out, int n, int id) { api->fmt(out, (size_t)n, DATA_DIR "/%d.txt", id); }

static void load_me(void) {
  int fd = safe_open_read(api, ME_PATH), n;
  G.me[0] = 0;
  if (fd < 0) return;
  n = api->read(fd, G.me, SOC_NAME - 1);
  api->close(fd);
  G.me[n > 0 ? n : 0] = 0;
  for (n = 0; G.me[n]; n++) if (G.me[n] == '\n' || G.me[n] == '\r') G.me[n] = 0;
}

static void save_me(void) {
  SafeFile f;
  api->mkdir(CAPP_VAR);
  api->mkdir(DATA_DIR);
  if (safe_begin(&f, api, ME_PATH) != 0) return;
  safe_line(&f, G.me);
  safe_commit(&f);
}

/* Who this device is: the member of a room who is not a friend. */
static void find_me(void) {
  int i;
  if (G.me[0]) return;
  for (i = 0; i < G.nrooms; i++) {
    Room *r = &G.rooms[i];
    const char *m = 0;
    if (G.new_id == r->id && G.new_with[0])
      m = str_same(r->p[0], G.new_with) ? r->p[1] : str_same(r->p[1], G.new_with) ? r->p[0] : 0;
    else if (G.nfriends > 0 && is_friend(r->p[0]) != is_friend(r->p[1]))
      m = is_friend(r->p[0]) ? r->p[1] : r->p[0];
    if (m && m[0]) {
      copy(G.me, SOC_NAME, m);
      save_me();
      return;
    }
  }
}

/* ---- a room's file ------------------------------------------------------------ */

/* Read room i's file into G.log (the header taken off into G.seed and the
 * players). */
static void load_room(int i) {
  Room *r = &G.rooms[i];
  char path[48], head[96], f[24];
  int fd, n, got = 0, h;
  G.log_room = i;
  G.log_len = 0;
  G.log[0] = 0;
  G.seed = 0;
  r->known = 0;
  r->have = 0;
  room_path(path, sizeof path, r->id);
  fd = safe_open_read(api, path);
  if (fd < 0) return;
  while (got < LOG_MAX - 1 && (n = api->read(fd, G.log + got, (size_t)(LOG_MAX - 1 - got))) > 0)
    got += n;
  api->close(fd);
  G.log[got] = 0;
  for (h = 0; h < got && G.log[h] != '\n'; h++) {}
  if (h >= got || h >= (int)sizeof head) { G.log[0] = 0; return; }
  api->mem_cpy(head, G.log, (size_t)h);
  head[h] = 0;
  tsv_field(head, 0, f, sizeof f);
  if (!str_same(f, "wl")) { G.log[0] = 0; return; }
  tsv_field(head, 1, f, sizeof f);
  {
    const char *q = f;
    G.seed = str_uint(&q);
  }
  h++;
  G.log_len = got - h;
  api->mem_move(G.log, G.log + h, (size_t)G.log_len);
  G.log[G.log_len] = 0;
  /* Drop a last line a cut left without its newline. */
  while (G.log_len > 0 && G.log[G.log_len - 1] != '\n') G.log[--G.log_len] = 0;
  r->known = 1;
  {
    const char *p = G.log, *last = 0;
    while (*p) { last = p; p = tsv_next_line(p); }
    if (last) { const char *q = last; r->have = (int)str_uint(&q); }
  }
}

static void save_room(int i) {
  Room *r = &G.rooms[i];
  char path[48], head[96];
  SafeFile f;
  if (G.log_room != i || !r->known) return;
  api->mkdir(CAPP_VAR);
  api->mkdir(DATA_DIR);
  room_path(path, sizeof path, r->id);
  if (safe_begin(&f, api, path) != 0) return;
  api->fmt(head, sizeof head, "wl\t%u\t%s\t%s\n", (unsigned)G.seed, r->p[0], r->p[1]);
  safe_line(&f, head);
  safe_write(&f, G.log, (size_t)G.log_len);
  safe_commit(&f);
}

/* The game in G.log, into G.g, and room i's summary from it. */
static void replay(int i) {
  Room *r = &G.rooms[i];
  G.skipped = wl_replay(&G.g, G.seed, r->p[0], r->p[1], G.log);
  r->score[0] = G.g.score[0];
  r->score[1] = G.g.score[1];
  r->turn = G.g.turn;
  r->over = G.g.over;
  r->winner = G.g.winner;
}


/* ---- the games list ----------------------------------------------------------- */

static int row_h(void);
static int list_rows(void);

/* /room/list: ID, TITLE, MEMBERS, SEQ, UPDATED, CREATOR a line. */
static void parse_list(const char *p) {
  int keep = G.new_id ? G.new_id : (G.sel < G.nrooms ? G.rooms[G.sel].id : 0), i;
  char f[96];
  G.nrooms = 0;
  while (*p && G.nrooms < ROOMS_MAX) {
    Room *r = &G.rooms[G.nrooms];
    const char *q;
    int k;
    api->mem_set(r, 0, sizeof *r);
    tsv_field(p, 0, f, sizeof f);
    q = f;
    r->id = (int)str_uint(&q);
    tsv_field(p, 3, f, sizeof f);
    q = f;
    r->seq = (int)str_uint(&q);
    tsv_field(p, 2, f, sizeof f);
    for (k = 0, q = f; *q && *q != ','; q++) if (k < SOC_NAME - 1) r->p[0][k++] = *q;
    r->p[0][k] = 0;
    if (*q == ',') {
      q++;
      for (k = 0; *q && *q != ','; q++) if (k < SOC_NAME - 1) r->p[1][k++] = *q;
      r->p[1][k] = 0;
    }
    /* Two players, or it is not a game of this. */
    if (r->id > 0 && r->p[0][0] && r->p[1][0] && *q != ',') G.nrooms++;
    p = tsv_next_line(p);
  }
  G.sel = 0;
  for (i = 0; i < G.nrooms; i++) if (G.rooms[i].id == keep) G.sel = i;
  G.top = G.sel >= list_rows() ? G.sel - list_rows() + 1 : 0;
  G.log_room = -1;
  for (i = 0; i < G.nrooms; i++) {
    load_room(i);
    if (G.rooms[i].known) replay(i);
  }
  find_me();
  G.new_id = 0;
  G.new_with[0] = 0;
}

/* ---- the open game -------------------------------------------------------------- */

static void clear_tiles(void) {
  G.nt = 0;
  api->mem_set(G.swap_mark, 0, sizeof G.swap_mark);
}

static void open_game(int i) {
  G.cur = i;
  if (G.log_room != i) load_room(i);
  replay(i);
  G.me_idx = me_in(&G.rooms[i]);
  clear_tiles();
  G.cr = WL_MID;
  G.cc = WL_MID;
  G.dir = WL_ACROSS;
  G.mode = M_PLACE;
  G.view = V_GAME;
  G.msg[0] = 0;
  G.next_poll = 0;
}

/* The open game again from its file, after another room used the buffer. */
static void refresh_cur(void) {
  int keep_nt = G.nt;
  if (G.view != V_GAME || G.cur < 0) return;
  if (G.log_room != G.cur) load_room(G.cur);
  replay(G.cur);
  G.me_idx = me_in(&G.rooms[G.cur]);
  G.nt = keep_nt;
}

static int caught_up(void) {
  const Room *r = &G.rooms[G.cur];
  return r->known && r->have >= r->seq;
}

static int my_turn(void) {
  return G.view == V_GAME && G.me_idx >= 0 && !G.g.over && G.g.turn == G.me_idx && caught_up() &&
         G.net != NET_SEND && !G.sending[0];
}

static int tent_at(int r, int c) {
  int i;
  for (i = 0; i < G.nt; i++) if (G.tr[i] == r && G.tc[i] == c) return i;
  return -1;
}

/* Which rack tiles the tiles put down came from. */
static void rack_used(uint8_t used[WL_RACK]) {
  const char *rack = G.me_idx >= 0 ? G.g.rack[G.me_idx] : "";
  int i, k;
  for (i = 0; i < WL_RACK; i++) used[i] = 0;
  for (k = 0; k < G.nt; k++)
    for (i = 0; rack[i]; i++)
      if (!used[i] && rack[i] == G.tl[k]) { used[i] = 1; break; }
}

/* Drop tiles put down that the loom has since taken, or all of them when it
 * is no longer this player's turn. */
static void check_tiles(void) {
  int i, k = 0;
  if (!my_turn()) { if (G.net != NET_SEND && !G.sending[0]) clear_tiles(); return; }
  for (i = 0; i < G.nt; i++)
    if (!G.g.board[G.tr[i]][G.tc[i]]) { G.tr[k] = G.tr[i]; G.tc[k] = G.tc[i]; G.tl[k] = G.tl[i]; k++; }
  G.nt = k;
}

/* What the tiles put down would do: WL_OK and *pl, or why not. */
static int preview(WlPlay *pl, char *move, int n) {
  int R, C, D, e;
  char letters[WL_RACK + 1];
  e = wl_from_cells(&G.g, G.tr, G.tc, G.tl, G.nt, G.dir, &R, &C, &D, letters);
  if (e != WL_OK) { api->mem_set(pl, 0, sizeof *pl); return e; }
  e = wl_check(&G.g, G.me_idx, R, C, D, letters, 0, 0, pl);
  if (e == WL_OK && move) wl_fmt_play(move, n, R, C, D, letters);
  return e;
}

/* ---- the network ------------------------------------------------------------------ */

static void start(int kind, int rc, int room) {
  if (rc == 0) { G.net = kind; G.net_room = room; return; }
  G.next_poll = api->ticks_ms() + 10000;
  if (kind == NET_SEND) { G.sending[0] = 0; say("busy: try again", C_WARN, 2500); }
}

static void sync_room(int i) {
  if (G.log_room != i) load_room(i);
  if (!G.rooms[i].known) start(NET_GET, soc_room_get(api, G.rooms[i].id), i);
  else start(NET_MOVES, soc_room_moves(api, G.rooms[i].id, G.rooms[i].have), i);
}

static int next_request(uint32_t now) {
  int i;
  if (G.sending[0] && G.cur >= 0) {
    const char *s = str_starts(G.sending, "resign") ? "resigned" : "your move";
    start(NET_SEND, soc_room_move(api, G.rooms[G.cur].id, G.sending, s), G.cur);
    return 1;
  }
  if (G.want_friends) { G.want_friends = 0; start(NET_FRIENDS, soc_friends(api), -1); return 0; }
  if (G.new_with[0] && !G.new_id) {
    char state[48];
    uint32_t seed = (api->ticks_ms() * 2654435761u) ^ (api->epoch() * 40503u) ^ 0x5EED5EEDu;
    wl_state_format(state, sizeof state, seed ? seed : 1);
    G.new_id = -1;                              /* asked; the reply says which */
    start(NET_NEW, soc_room_new(api, APP_ID, G.new_with, "a game of Wordloom", state), -1);
    if (!G.net) { G.new_id = 0; G.new_with[0] = 0; say("could not start the game", C_WARN, 3000); }
    return 1;
  }
  if (G.want_list) { G.want_list = 0; start(NET_LIST, soc_room_list(api, APP_ID), -1); return 0; }
  if ((int32_t)(now - G.next_poll) < 0) return 0;
  if (G.view == V_GAME && G.cur >= 0) {
    G.next_poll = now + POLL_MS;
    if (!G.rooms[G.cur].bad) sync_room(G.cur);
    return 0;
  }
  for (i = 0; i < G.nrooms; i++) {
    Room *r = &G.rooms[i];
    if (!r->bad && (!r->known || r->have < r->seq)) { sync_room(i); return 0; }
  }
  return 0;
}

/* /room/get: "ID VERSION SEQ MEMBERS" then the state. */
static void got_room(int i, const char *reply) {
  Room *r = &G.rooms[i];
  const char *state = tsv_next_line(reply);
  uint32_t seed;
  if (wl_state_parse(state, &seed) != WL_VERSION) { r->bad = 1; return; }
  G.log_room = i;
  G.seed = seed;
  G.log_len = 0;
  G.log[0] = 0;
  r->known = 1;
  r->have = 0;
  save_room(i);
  replay(i);
}

/* /room/moves: blocks of "SEQ FROM AT SIZE" and the move. */
static void got_moves(int i, const char *reply, int n) {
  Room *r = &G.rooms[i];
  const char *p = reply, *end = reply + n, *head, *body;
  int len, count = 0, changed = 0;
  char f[SOC_NAME + 2], line[MOVE_MAX + 48], mv[MOVE_MAX + 1];
  if (G.log_room != i) load_room(i);
  while (soc_block_next(&p, end, &head, &body, &len)) {
    const char *q;
    int seq, k, at;
    count++;
    tsv_field(head, 0, f, sizeof f);
    q = f;
    seq = (int)str_uint(&q);
    if (seq <= r->have) continue;
    for (k = 0; k < len && k < MOVE_MAX && body[k] != '\n' && body[k] != '\r' && body[k] != '\t'; k++)
      mv[k] = str_lower(body[k]);
    mv[k] = 0;
    tsv_field(head, 1, f, sizeof f);
    at = api->fmt(line, sizeof line, "%d\t%s\t%s\n", seq, f, mv);
    if (at <= 0 || G.log_len + at >= LOG_MAX) { r->bad = 1; break; }
    api->mem_cpy(G.log + G.log_len, line, (size_t)at);
    G.log_len += at;
    G.log[G.log_len] = 0;
    r->have = seq;
    changed = 1;
  }
  if (r->have > r->seq) r->seq = r->have;
  if (changed) { save_room(i); replay(i); }
  if (CAPP_HTTP_FILLED(n, sizeof G.reply) || count >= 50) G.next_poll = 0;   /* more to come */
  else if (r->have < r->seq) r->seq = r->have;   /* the server has no more than this */
}

static void handle(int n) {
  int kind = G.net, i = G.net_room;
  G.net = NET_NONE;
  if (n < 0) {
    if (kind == NET_LIST) { G.list_failed = 1; G.listed = 0; }
    if (kind == NET_SEND) { G.sending[0] = 0; say("not sent: enter tries again", C_WARN, 4000); }
    if (kind == NET_NEW) { G.new_id = 0; G.new_with[0] = 0; say("could not start the game", C_WARN, 3000); }
    if ((kind == NET_GET || kind == NET_MOVES) && i >= 0 && (n == -403 || n == -404)) G.rooms[i].bad = 1;
    if (kind == NET_GET || kind == NET_MOVES) G.next_poll = api->ticks_ms() + 10000;
    return;
  }
  G.reply[n] = 0;
  switch (kind) {
  case NET_FRIENDS:
    G.nfriends = soc_friend_parse(G.reply, G.friends, SOC_FRIENDS);
    find_me();
    break;
  case NET_LIST:
    G.listed = 1;
    G.list_failed = 0;
    parse_list(G.reply);
    break;
  case NET_GET:
    if (i >= 0 && i < G.nrooms) got_room(i, G.reply);
    break;
  case NET_MOVES:
    if (i >= 0 && i < G.nrooms) got_moves(i, G.reply, n);
    break;
  case NET_SEND:
    if (str_starts(G.reply, "ok ") && G.cur >= 0) {
      const char *q = G.reply + 3;
      int seq = (int)str_uint(&q);
      if (seq > G.rooms[G.cur].seq) G.rooms[G.cur].seq = seq;
      clear_tiles();
      G.mode = M_PLACE;
      say("sent", C_GOOD, 1500);
    } else {
      say("not sent: enter tries again", C_WARN, 4000);
    }
    G.sending[0] = 0;
    G.next_poll = 0;
    break;
  case NET_NEW:
    if (str_starts(G.reply, "ok ")) {
      const char *q = G.reply + 3;
      G.new_id = (int)str_uint(&q);
      G.want_list = 1;
      say("game started: your move", C_GOOD, 3000);
    } else {
      G.new_id = 0;
      G.new_with[0] = 0;
    }
    break;
  }
  if (G.view == V_GAME && (kind == NET_GET || kind == NET_MOVES || kind == NET_FRIENDS)) {
    if (i != G.cur) refresh_cur();
    else G.me_idx = me_in(&G.rooms[G.cur]);
    check_tiles();
  }
}

/* ---- drawing ------------------------------------------------------------------------- */

static void upper(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32); }

/* A row of the 6x8-free fonts: the font's own cell, so a selection under it
 * covers what the text paints. */
static int row_h(void) { int h = api->font_height(G.f_ui); return h > 8 ? h + 1 : 14; }
static int list_rows(void) { return (135 - FOOT_H - 20) / row_h(); }

static void right_text(int f, int x_end, int y, const char *s, uint16_t fg, uint16_t bg) {
  api->text_font(f, (int16_t)(x_end - api->text_width(f, s)), (int16_t)y, s, fg, bg);
}

static uint16_t premium_colour(int r, int c) {
  switch (wl_premium(r, c)) {
  case WL_KNOT: return C_KNOT;
  case WL_GOLD: return C_GOLD;
  case WL_WARP: return C_WARP;
  case WL_LOOM: return C_LOOM;
  default: return C_CELL;
  }
}

static void paint_loom(void) {
  int r, c;
  char s[2];
  s[1] = 0;
  for (r = 0; r < WL_N; r++)
    for (c = 0; c < WL_N; c++) {
      int x = BX + c * PITCH, y = BY + r * PITCH, t = tent_at(r, c);
      char ch = G.g.board[r][c] ? G.g.board[r][c] : t >= 0 ? G.tl[t] : 0;
      CRect cell = capp_rect(x, y, CELL, CELL);
      if (ch) {
        uint16_t bg = t >= 0 && !G.g.board[r][c] ? C_TENT : G.g.fresh[r][c] ? C_FRESH : C_TILE;
        api->fill(cell, bg);
        s[0] = (char)(ch - 32);
        api->text((int16_t)(x + 2), (int16_t)(y + 1), s, C_INK, bg);
      } else {
        api->fill(cell, premium_colour(r, c));
        if (r == WL_MID && c == WL_MID) api->fill(capp_rect(x + 4, y + 4, 2, 2), C_TEXT);
      }
    }
  if (G.me_idx >= 0 && !G.g.over)
    api->frame(capp_rect(BX + G.cc * PITCH - 1, BY + G.cr * PITCH - 1, CELL + 2, CELL + 2),
               G.dir == WL_DOWN ? C_CURDOWN : C_CURSOR);
}

static void last_move(char *b, int n) {
  const Room *r = &G.rooms[G.cur];
  const char *name;
  char w[WL_N + 1];
  if (G.g.last_player < 0) { api->fmt(b, (size_t)n, "no moves yet"); return; }
  name = who(r->p[G.g.last_player]);
  switch (G.g.last_kind) {
  case WL_K_PLAY:
    copy(w, sizeof w, G.g.last_word);
    upper(w);
    api->fmt(b, (size_t)n, "%s: %s +%d", name, w, G.g.last_points);
    break;
  case WL_K_SWAP: api->fmt(b, (size_t)n, "%s swapped %d", name, G.g.last_count); break;
  case WL_K_PASS: api->fmt(b, (size_t)n, "%s passed", name); break;
  default: api->fmt(b, (size_t)n, "%s resigned", name); break;
  }
}

static void status_line(char *b, int n, uint16_t *col) {
  const Room *r = &G.rooms[G.cur];
  *col = C_DIM;
  if (G.g.over) {
    if (G.g.winner < 0) api->fmt(b, (size_t)n, "a draw");
    else if (G.g.winner == G.me_idx) { api->fmt(b, (size_t)n, "you won!"); *col = C_GOOD; }
    else api->fmt(b, (size_t)n, "%s won", who(r->p[G.g.winner]));
  } else if (G.net == NET_SEND || G.sending[0]) api->fmt(b, (size_t)n, "sending...");
  else if (!caught_up()) api->fmt(b, (size_t)n, "reading moves...");
  else if (G.me_idx < 0) api->fmt(b, (size_t)n, "who are you? r");
  else if (G.g.turn == G.me_idx) { api->fmt(b, (size_t)n, "your turn"); *col = C_PLAYER[G.me_idx]; }
  else api->fmt(b, (size_t)n, "%s's turn", who(r->p[G.g.turn]));
}

static void paint_rack(void) {
  const char *rack = G.me_idx >= 0 ? G.g.rack[G.me_idx] : "";
  uint8_t used[WL_RACK];
  int i;
  char s[2], v[4];
  s[1] = 0;
  rack_used(used);
  for (i = 0; i < WL_RACK; i++) {
    int x = PX + i * TILE_P;
    CRect t = capp_rect(x, RACK_Y, TILE_W, TILE_H);
    if (!rack[i] || used[i] || i >= wl_len(rack)) {
      api->fill(t, C_BG);
      api->frame(t, C_CELL);
      continue;
    }
    {
      uint16_t bg = G.mode == M_SWAP && G.swap_mark[i] ? C_MARK : C_TILE;
      api->fill(t, bg);
      s[0] = (char)(rack[i] - 32);
      api->text_font(G.f_b, (int16_t)(x + 2), (int16_t)(RACK_Y + 1), s, C_INK, bg);
      api->fmt(v, sizeof v, "%d", wl_value(rack[i]));
      api->text((int16_t)(x + TILE_W - 6), (int16_t)(RACK_Y + TILE_H - 8), v, C_INK, bg);
    }
  }
}

static void paint_menu(void) {
  int i, rh = row_h(), h = MENU_N * rh + 6;
  CRect box = capp_rect(PX - 4, 30, PW + 6, h);
  api->fill(box, C_DIM);
  api->fill(capp_rect(box.x + 1, box.y + 1, box.w - 2, box.h - 2), C_BG);
  for (i = 0; i < MENU_N; i++) {
    int y = box.y + 3 + i * rh, on = i == G.menu_sel;
    int can = i == MENU_RESIGN || my_turn();
    if (on) api->fill(capp_rect(box.x + 2, y, box.w - 4, rh), C_SEL);
    api->text_font(G.f_ui, (int16_t)(box.x + 6), (int16_t)y, MENU_TEXT[i], can ? C_TEXT : C_DIM,
                   on ? C_SEL : C_BG);
  }
}

static void paint_game(CRect c) {
  const Room *r = &G.rooms[G.cur];
  char b[48];
  uint16_t col;
  int k, y;
  api->fill(capp_rect(c.x, c.y, c.w, c.h - FOOT_H), C_BG);
  paint_loom();
  /* The players: you first. */
  for (k = 0; k < 2; k++) {
    int p = G.me_idx == 1 ? 1 - k : k;
    y = 2 + k * 14;
    if (!G.g.over && G.g.turn == p) api->fill(capp_rect(PX, y + 4, 4, 6), C_PLAYER[p]);
    api->text_font(k == 0 ? G.f_b : G.f_ui, PX + 8, (int16_t)y, who(r->p[p]), C_TEXT, C_BG);
    api->fmt(b, sizeof b, "%d", G.g.score[p]);
    right_text(G.f_b, 238, y, b, C_PLAYER[p], C_BG);
  }
  api->fmt(b, sizeof b, "bag %d", G.g.nbag);
  api->text_font(G.f_ui, PX, 32, b, C_DIM, C_BG);
  if (my_turn() && G.mode == M_PLACE) right_text(G.f_ui, 238, 32, G.dir == WL_DOWN ? "down" : "across",
                                                 G.dir == WL_DOWN ? C_CURDOWN : C_CURSOR, C_BG);
  last_move(b, sizeof b);
  api->text_font(G.f_ui, PX, 46, b, C_TEXT, C_BG);
  /* What is happening now: a message, or what the tiles down would score. */
  if (G.msg[0]) api->text_font(G.f_ui, PX, 60, G.msg, G.msg_colour, C_BG);
  else if (G.mode == M_SWAP) api->text_font(G.f_ui, PX, 60, "type tiles to swap", C_DIM, C_BG);
  else if (G.mode == M_ASK_PASS) api->text_font(G.f_ui, PX, 60, "pass? y/n", C_WARN, C_BG);
  else if (G.mode == M_ASK_RESIGN) api->text_font(G.f_ui, PX, 60, "resign? y/n", C_WARN, C_BG);
  else if (my_turn() && G.nt > 0) {
    WlPlay pl;
    int e = preview(&pl, 0, 0);
    if (e == WL_OK) {
      char w[WL_N + 1];
      copy(w, sizeof w, pl.nwords ? pl.w[0].text : "");
      upper(w);
      api->fmt(b, sizeof b, "+%d %s%s", pl.points, w, pl.nwords > 1 ? " ..." : "");
      api->text_font(G.f_ui, PX, 60, b, C_GOOD, C_BG);
    } else {
      api->text_font(G.f_ui, PX, 60, wl_why(e), C_WARN, C_BG);
    }
  } else if (my_turn()) api->text_font(G.f_ui, PX, 60, "type to lay tiles", C_DIM, C_BG);
  status_line(b, sizeof b, &col);
  api->text_font(G.f_b, PX, 76, b, col, C_BG);
  paint_rack();
  if (G.mode == M_MENU) paint_menu();
  if (G.mode == M_SWAP) footer_paint(api, c, "letters mark  enter swap  esc back");
  else if (G.mode == M_MENU) footer_paint(api, c, "enter choose  esc back");
  else if (G.mode == M_ASK_PASS || G.mode == M_ASK_RESIGN) footer_paint(api, c, "y yes  n no");
  else if (my_turn()) footer_paint(api, c, "enter weave  space dir  tab more");
  else if (G.g.over) footer_paint(api, c, "esc games  n new game");
  else footer_paint(api, c, "esc games  r refresh  tab more");
}

static void room_status(const Room *r, char *b, int n, uint16_t *col) {
  int me = me_in(r);
  *col = C_DIM;
  if (r->bad) api->fmt(b, (size_t)n, "can't read");
  else if (!r->known || r->have < r->seq) { api->fmt(b, (size_t)n, "new move"); *col = C_GOOD; }
  else if (r->over) {
    if (r->winner < 0) api->fmt(b, (size_t)n, "a draw");
    else if (r->winner == me) { api->fmt(b, (size_t)n, "you won"); *col = C_GOOD; }
    else api->fmt(b, (size_t)n, "%s", me < 0 ? "over" : "you lost");
  } else if (me >= 0 && r->turn == me) { api->fmt(b, (size_t)n, "your turn"); *col = C_GOOD; }
  else api->fmt(b, (size_t)n, "their turn");
}

static void paint_list(CRect c) {
  char b[48];
  int i;
  api->fill(capp_rect(c.x, c.y, c.w, c.h - FOOT_H), C_BG);
  api->text_font(G.f_b, 6, 3, "Wordloom", C_TEXT, C_BG);
  if (G.listed) {
    api->fmt(b, sizeof b, G.nrooms == 1 ? "1 game" : "%d games", G.nrooms);
    if (!G.msg[0]) right_text(G.f_ui, 234, 3, G.net ? "..." : b, C_DIM, C_BG);
  }
  if (G.msg[0]) right_text(G.f_ui, 234, 3, G.msg, G.msg_colour, C_BG);
  if (!G.listed) {
    api->text_font(G.f_ui, 8, 26, G.list_failed ? "Could not reach the server." : "Asking the server...",
                   C_DIM, C_BG);
    if (G.list_failed) api->text_font(G.f_ui, 8, 42, "r tries again.", C_DIM, C_BG);
  } else if (G.nrooms == 0) {
    api->text_font(G.f_ui, 8, 26, "No games yet.", C_TEXT, C_BG);
    api->text_font(G.f_ui, 8, 42, "n starts one with a friend.", C_DIM, C_BG);
  }
  for (i = G.top; i < G.nrooms && i < G.top + list_rows(); i++) {
    const Room *r = &G.rooms[i];
    int y = 21 + (i - G.top) * row_h(), on = i == G.sel, me = me_in(r);
    uint16_t bg = on ? C_SEL : C_BG, col;
    if (on) api->fill(capp_rect(2, y - 1, 236, row_h()), C_SEL);
    if (me >= 0) api->fmt(b, sizeof b, "%s", who(r->p[1 - me]));
    else api->fmt(b, sizeof b, "%s v %s", who(r->p[0]), who(r->p[1]));
    api->text_font(G.f_ui, 8, (int16_t)y, b, C_TEXT, bg);
    if (r->known) {
      int a = me == 1 ? 1 : 0;
      api->fmt(b, sizeof b, "%d - %d", r->score[a], r->score[1 - a]);
      api->text_font(G.f_ui, 104, (int16_t)y, b, C_TEXT, bg);
    }
    room_status(r, b, sizeof b, &col);
    right_text(G.f_ui, 234, y, b, col, bg);
  }
  footer_paint(api, c, "enter open  n new  r refresh");
  if (G.pick.open) soc_pick_paint(api, &G.pick, capp_rect(c.x, c.y, c.w, c.h - FOOT_H));
}

static void app_paint(void *st, CRect c) {
  (void)st;
  if (G.view == V_GAME && G.cur >= 0) paint_game(c);
  else paint_list(c);
}

/* ---- keys ---------------------------------------------------------------------------- */

static void new_game(void) {
  if (G.net || G.sending[0] || G.new_with[0]) { say("busy: try again", C_WARN, 2000); return; }
  G.view = V_LIST;
  soc_pick_open(api, &G.pick, "Play Wordloom with");
}

static void after_pick(void) {
  if (G.pick.n > 0) {
    api->mem_cpy(G.friends, G.pick.f, sizeof G.friends);
    G.nfriends = G.pick.n;
    find_me();
  }
  if (G.pick.done == 1) {
    copy(G.new_with, SOC_NAME, G.pick.chosen.name);
    G.new_id = 0;
  }
}

static void send(const char *move) {
  copy(G.sending, sizeof G.sending, move);
  G.mode = M_PLACE;
  G.menu_sel = 0;
}

static void step(int *r, int *c, int dir) {
  if (dir == WL_DOWN) (*r)++;
  else (*c)++;
}

static void lay(char ch) {
  char avail[WL_RACK + 1];
  uint8_t used[WL_RACK];
  int i, k = 0, r = G.cr, c = G.cc;
  const char *rack = G.g.rack[G.me_idx];
  rack_used(used);
  for (i = 0; rack[i]; i++) if (!used[i]) avail[k++] = rack[i];
  avail[k] = 0;
  for (i = 0; avail[i] && avail[i] != ch; i++) {}
  if (!avail[i]) { say("not in your rack", C_WARN, 1500); return; }
  while (r < WL_N && c < WL_N && (G.g.board[r][c] || tent_at(r, c) >= 0)) step(&r, &c, G.dir);
  if (r >= WL_N || c >= WL_N) { say("no room that way", C_WARN, 1500); return; }
  G.tr[G.nt] = (int8_t)r;
  G.tc[G.nt] = (int8_t)c;
  G.tl[G.nt] = ch;
  G.nt++;
  step(&r, &c, G.dir);
  if (r < WL_N && c < WL_N) { G.cr = r; G.cc = c; }
  if (G.msg_until) G.msg[0] = 0;
}

static void weave(void) {
  WlPlay pl;
  char move[MOVE_MAX];
  int e;
  if (G.nt == 0) { say("type letters to lay tiles", C_DIM, 2000); return; }
  e = preview(&pl, move, sizeof move);
  if (e != WL_OK) { say(wl_why(e), C_WARN, 2500); return; }
  send(move);
}

static void swap_key(uint8_t k) {
  const char *rack = G.g.rack[G.me_idx];
  int i;
  if (k == CAPP_KEY_ESC) { G.mode = M_PLACE; return; }
  if (k == CAPP_KEY_ENTER) {
    char letters[WL_RACK + 1], move[16];
    int n = 0;
    for (i = 0; rack[i]; i++) if (G.swap_mark[i]) letters[n++] = rack[i];
    letters[n] = 0;
    if (!n) { say("type the tiles to swap", C_DIM, 2000); return; }
    if (G.g.nbag < n) { say(wl_why(WL_E_BAG), C_WARN, 2000); return; }
    api->fmt(move, sizeof move, "swap %s", letters);
    send(move);
    return;
  }
  if (k >= 'A' && k <= 'Z') k = (uint8_t)(k + 32);
  if (k >= 'a' && k <= 'z') {
    for (i = 0; rack[i]; i++) if (rack[i] == (char)k && !G.swap_mark[i]) { G.swap_mark[i] = 1; return; }
    for (i = 0; rack[i]; i++) if (rack[i] == (char)k) { G.swap_mark[i] = 0; return; }
  }
}

static void menu_key(uint8_t k) {
  if (k == CAPP_KEY_ESC || k == '\t') { G.mode = M_PLACE; return; }
  if (k == CAPP_KEY_UP && G.menu_sel > 0) G.menu_sel--;
  else if (k == CAPP_KEY_DOWN && G.menu_sel < MENU_N - 1) G.menu_sel++;
  else if (k == CAPP_KEY_ENTER) {
    if (G.menu_sel == MENU_RESIGN) { G.mode = M_ASK_RESIGN; return; }
    if (!my_turn()) { say("not your turn", C_WARN, 1500); G.mode = M_PLACE; return; }
    clear_tiles();
    G.mode = G.menu_sel == MENU_SWAP ? M_SWAP : M_ASK_PASS;
  }
}

static int game_key(uint8_t k) {
  if (G.mode == M_MENU) { menu_key(k); return 1; }
  if (G.mode == M_ASK_PASS || G.mode == M_ASK_RESIGN) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) send(G.mode == M_ASK_PASS ? "pass" : "resign");
    if (a != CONFIRM_WAIT) G.mode = M_PLACE;
    return 1;
  }
  if (G.mode == M_SWAP) {
    if (!my_turn()) G.mode = M_PLACE;
    else { if (!api->key_repeat()) swap_key(k); return 1; }
  }
  /* Moving the cursor works whoever's turn it is. */
  if (k == CAPP_KEY_UP) { if (G.cr > 0) G.cr--; return 1; }
  if (k == CAPP_KEY_DOWN) { if (G.cr < WL_N - 1) G.cr++; return 1; }
  if (k == CAPP_KEY_LEFT) { if (G.cc > 0) G.cc--; return 1; }
  if (k == CAPP_KEY_RIGHT) { if (G.cc < WL_N - 1) G.cc++; return 1; }
  if (k == CAPP_KEY_ESC) {
    if (api->key_repeat()) return 1;
    if (G.nt > 0) { G.cr = G.tr[0]; G.cc = G.tc[0]; clear_tiles(); return 1; }
    G.view = V_LIST;
    G.mode = M_PLACE;
    G.msg[0] = 0;
    G.want_list = 1;
    return 1;
  }
  if (k == '\t') {
    if (api->key_repeat()) return 1;
    if (G.g.over || G.me_idx < 0) { say("nothing to do", C_DIM, 1500); return 1; }
    G.mode = M_MENU;
    G.menu_sel = 0;
    return 1;
  }
  if (my_turn()) {
    if (k == ' ') { if (!api->key_repeat()) G.dir = 1 - G.dir; return 1; }
    if (k == CAPP_KEY_BACK) {
      if (G.nt > 0) { G.nt--; G.cr = G.tr[G.nt]; G.cc = G.tc[G.nt]; }
      return 1;
    }
    if (k == CAPP_KEY_ENTER) { if (!api->key_repeat()) weave(); return 1; }
    if (k >= 'A' && k <= 'Z') k = (uint8_t)(k + 32);
    if (k >= 'a' && k <= 'z') { if (G.nt < WL_RACK) lay((char)k); return 1; }
    return 0;
  }
  if (api->key_repeat()) return 1;
  if (k == 'r') { G.next_poll = 0; say("reading...", C_DIM, 1000); if (!G.me[0]) G.want_friends = 1; return 1; }
  if (k == 'n' && G.g.over) { new_game(); return 1; }
  return 0;
}

static int list_key(uint8_t k) {
  if (k == CAPP_KEY_UP) {
    if (G.sel > 0) G.sel--;
    if (G.sel < G.top) G.top = G.sel;
    return 1;
  }
  if (k == CAPP_KEY_DOWN) {
    if (G.sel < G.nrooms - 1) G.sel++;
    if (G.sel > G.top + list_rows() - 1) G.top = G.sel - list_rows() + 1;
    return 1;
  }
  if (api->key_repeat()) return 1;
  if (k == CAPP_KEY_ENTER) {
    if (G.sel < G.nrooms) {
      if (G.rooms[G.sel].bad) say("made by a newer Wordloom", C_WARN, 2500);
      else open_game(G.sel);
    }
    return 1;
  }
  if (k == 'n') { new_game(); return 1; }
  if (k == 'r') {
    G.want_list = 1;
    if (!G.me[0] || G.nfriends == 0) G.want_friends = 1;
    G.next_poll = 0;
    return 1;
  }
  return 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (G.pick.open) {
    soc_pick_key(&G.pick, k);
    if (!G.pick.open) after_pick();
    return 1;
  }
  if (G.view == V_GAME && G.cur >= 0) return game_key(k);
  return list_key(k);
}

static int app_tick(void *st, uint32_t now) {
  int r = 0;
  (void)st;
  if (G.msg_until && (int32_t)(now - G.msg_until) >= 0) {
    G.msg[0] = 0;
    G.msg_until = 0;
    r = 1;
  }
  if (G.pick.open) return r | soc_pick_tick(api, &G.pick);
  if (G.net) {
    int n = api->http_poll(G.reply, sizeof G.reply);
    if (n == CAPP_HTTP_PENDING) return r;
    handle(n);
    return 1;
  }
  return r | next_request(now);
}

static int app_wants_text(void *st) {
  (void)st;
  if (G.pick.open) return 0;
  if (my_turn() && (G.mode == M_PLACE || G.mode == M_SWAP)) return CAPP_TEXT_ARROWS;
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Wordloom",
  /* 16x16: a loom's frame with tiles woven across and down it. */
  {
    0x00, 0x00, 0x7F, 0xFF, 0x40, 0x01, 0x5D, 0xDD,
    0x5D, 0xDD, 0x5D, 0xDD, 0x40, 0x01, 0x41, 0xC1,
    0x41, 0xC1, 0x41, 0xC1, 0x40, 0x01, 0x41, 0xDD,
    0x41, 0xDD, 0x41, 0xDD, 0x40, 0x01, 0x7F, 0xFF,
  },
  "the games:\nenter\topen a game\nn\tnew game with a friend\nr\tread the list again\n"
  "a game:\nletters\tlay a tile at the cursor\nspace\tacross or down\n"
  "bksp\ttake the last tile back\nesc\ttake all back, or to the games\n"
  "enter\tweave: play the tiles\ntab\tswap, pass, resign\n"
  "knot\tteal: letter x2\ngold\tamber: letter x3\nwarp\trose: word x2\n"
  "loom\tviolet: word x3\nweave\t+5 for 3+ words at once\nbolt\t+20 for all six tiles\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  G.log_room = -1;
  G.cur = -1;
  G.me_idx = -1;
  G.f_ui = api->font_load("ui13");
  G.f_b = api->font_load("ui13b");
  load_me();
  G.want_friends = 1;
  G.want_list = 1;

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
