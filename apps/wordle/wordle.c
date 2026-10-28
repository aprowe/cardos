/* Wordle: a five-letter word in six guesses, green for the right letter in
 * the right place, yellow for one in the word somewhere else, grey for none.
 *
 * Two games. The daily puzzle is the same word for every device on a date
 * (apps/wordle/wordle.h: wd_day, wd_daily_index), counts towards the stats
 * and is kept as it goes, so leaving the app and coming back finds the board
 * as it was. Practice is a random answer, as many as you like, counted
 * nowhere. With no clock yet (no NTP since power-on) there is no date, so
 * only practice.
 *
 * The screen: the six rows of tiles on the left, and on the right the
 * puzzle's name, a line of news and the keyboard, each key coloured by the
 * best that letter has been. Tab shows the stats.
 *
 * Every repaint is a whole one, which the OS composes off the panel (no
 * damage marks): a key changes a tile, a key on the keyboard strip and a
 * line of text, and the whole screen is 30 tiles and 26 keys -- cheaper to
 * redraw than to track, and nothing can flicker.
 *
 * Friends (apps/wordle/wdfriends.h): a finished daily publishes its result
 * -- the colours, never the letters -- for friends to see, and reads
 * theirs for the same day: "Sam 4/6" under yours, and `f` shows each one's
 * grid. `s` sends yours to Chat. Nothing else leaves the device, and with no
 * network Wordle is as it was.
 *
 * The rules, the word list and the stats file are wordle.h, host-tested in
 * apps/wordle/test/test_wordle.c; the screen in apps/wordle/test/test_wordle_paint.c.
 *
 * @app folder Games
 * @app release extra
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/footer.h"
#include "apps/safefile.h"
#include "apps/social.h"
#include "apps/wordle/wordle.h"
#include "apps/wordle/wdfriends.h"

static const CardApi *api;

#define STATS_DIR  CAPP_VAR "/wordle"
#define STATS_PATH STATS_DIR "/stats.txt"

/* The board. */
#define TILE   18
#define PITCH  20
#define BX     4
#define BY     3

/* The panel on the right, and the keyboard in it. */
#define PX     108
#define KW     11
#define KH     14
#define KP     12
#define KY     44
#define KROW   16
#define MSG_Y  95

#define C_BG      CAPP_RGB(18, 18, 19)
#define C_EDGE    CAPP_RGB(58, 58, 60)
#define C_EDGE_IN CAPP_RGB(110, 110, 114)
#define C_TEXT    CAPP_RGB(236, 236, 238)
#define C_DIM     CAPP_RGB(130, 132, 138)
#define C_GREEN   CAPP_RGB(83, 141, 78)
#define C_YELLOW  CAPP_RGB(181, 159, 59)
#define C_GREY    CAPP_RGB(58, 58, 60)
#define C_KEY     CAPP_RGB(110, 112, 118)
#define C_WARN    CAPP_RGB(240, 150, 90)

enum { MODE_DAILY, MODE_PRACTICE };
enum { V_GAME, V_STATS, V_FRIENDS };
/* The one request in flight: publishing ours, reading theirs, sharing. */
enum { RQ_NONE, RQ_PUB, RQ_EACH, RQ_SHARE };

static const char *const KB_ROWS[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char *const PRAISE[WD_TRIES] = {
  "Genius!", "Magnificent!", "Impressive!", "Splendid!", "Great!", "Phew!"
};

static struct {
  WdGame  g;
  WdStats st;
  int     mode, view;
  int32_t today;              /* the day number, or WD_NO_DAY with no clock */
  char    msg[40];
  uint16_t msg_colour;
  uint32_t msg_until;         /* ticks_ms; 0 keeps it */
  int     shake_row;          /* the row a refused guess was typed in, or -1 */
  uint32_t seed;
  int     f_tile, f_ui, f_num;
  int     rq;                 /* RQ_* */
  int     published;          /* today's result is on the server */
  int     nfr;                /* friends' results for today */
  WdFriend fr[WDF_MAX];
  char    reply[1600];
} G;

/* ---- stats on the card ----------------------------------------------------- */

static void load_stats(void) {
  char buf[400];
  int fd, n, got = 0;
  wd_stats_init(&G.st);
  fd = safe_open_read(api, STATS_PATH);
  if (fd < 0) return;
  while (got < (int)sizeof buf - 1 && (n = api->read(fd, buf + got, sizeof buf - 1 - got)) > 0)
    got += n;
  api->close(fd);
  buf[got] = 0;
  wd_stats_parse(&G.st, buf);
}

static void save_stats(void) {
  char buf[400];
  SafeFile f;
  int n = wd_stats_format(&G.st, buf, sizeof buf);
  if (n < 0) return;
  api->mkdir(CAPP_VAR);
  api->mkdir(STATS_DIR);
  if (safe_begin(&f, api, STATS_PATH) != 0) return;
  safe_write(&f, buf, (size_t)n);
  safe_commit(&f);
}

/* ---- games ----------------------------------------------------------------- */

static void say(const char *s, uint16_t colour, uint32_t ms) {
  api->fmt(G.msg, sizeof G.msg, "%s", s);
  G.msg_colour = colour;
  G.msg_until = ms ? api->ticks_ms() + ms : 0;
}

static uint32_t rnd(void) {
  G.seed ^= G.seed << 13;
  G.seed ^= G.seed >> 17;
  G.seed ^= G.seed << 5;
  return G.seed;
}

static int32_t find_today(void) {
  CappTime t;
  int32_t d;
  api->now(&t);
  if (!t.synced || t.year < 2026) return WD_NO_DAY;
  d = wd_day(t.year, t.month, t.day);
  return d < 0 ? WD_NO_DAY : d;
}

static void start_practice(void) {
  char w[WD_LEN + 1];
  wd_answer((int)(rnd() % WD_NANSWERS), w);
  wd_start(&G.g, w);
  G.mode = MODE_PRACTICE;
  G.view = V_GAME;
  say("", C_TEXT, 0);
}

/* Today's puzzle, with whatever was played of it already. */
static void start_daily(void) {
  char w[WD_LEN + 1];
  int i;
  G.today = find_today();
  if (G.today == WD_NO_DAY) {
    start_practice();
    say("no date yet: practice", C_DIM, 0);
    return;
  }
  wd_answer(wd_daily_index(G.today), w);
  wd_start(&G.g, w);
  G.mode = MODE_DAILY;
  G.view = V_GAME;
  say("", C_TEXT, 0);
  if (G.st.day == G.today)
    for (i = 0; i < G.st.nguess; i++) wd_guess(&G.g, G.st.guess[i]);
  if (G.g.over) wd_stats_finish(&G.st, G.today, G.g.over == WD_WON, G.g.n);
}

static void friends_sync(void);

/* A daily guess played: kept at once, and counted when the game ends. */
static void keep_daily(void) {
  int i;
  G.st.day = G.today;
  G.st.nguess = G.g.n;
  for (i = 0; i < G.g.n; i++) api->mem_cpy(G.st.guess[i], G.g.guess[i], WD_LEN + 1);
  if (G.g.over) wd_stats_finish(&G.st, G.today, G.g.over == WD_WON, G.g.n);
  save_stats();
  if (G.g.over) friends_sync();
}

static void submit(void) {
  int r = wd_submit(&G.g);
  if (r == WD_SHORT) { say("not enough letters", C_WARN, 1800); G.shake_row = G.g.n; return; }
  if (r == WD_UNKNOWN) { say("not in word list", C_WARN, 1800); G.shake_row = G.g.n; return; }
  if (r != WD_OK) return;
  G.shake_row = -1;
  say("", C_TEXT, 0);
  if (G.mode == MODE_DAILY) keep_daily();
}

/* ---- friends ---------------------------------------------------------------- */

static int daily_over(void) {
  return G.mode == MODE_DAILY && G.g.over != WD_PLAYING && G.today != WD_NO_DAY;
}

/* Publish today's result, then read the friends'. Quietly does nothing
 * with no network: Wordle never asks for one. */
static void friends_sync(void) {
  char body[120];
  if (!daily_over() || G.rq != RQ_NONE || !api->net_ready()) return;
  if (wdf_format(&G.g, G.today, wd_stats_streak(&G.st, G.today), body, sizeof body) < 0) return;
  if (soc_pub_put(api, "wordle", "day", body) == 0) G.rq = RQ_PUB;
}

/* /kv/each: yours first (published, so it is there), then each friend's;
 * only today's finished ones are kept. */
static void friends_read(int n) {
  const char *p = G.reply, *end = G.reply + n, *head, *body;
  int len, first = 1;
  G.nfr = 0;
  while (soc_block_next(&p, end, &head, &body, &len)) {
    WdFriend *f = &G.fr[G.nfr];
    if (first) { first = 0; continue; }
    if (G.nfr >= WDF_MAX) break;
    tsv_field(head, 0, f->name, sizeof f->name);
    tsv_field(head, 1, f->display, sizeof f->display);
    if (wdf_parse(body, len, f) && f->day == G.today) G.nfr++;
  }
}

static void chat_name(char *out, int n) {
  int fd = api->open(CAPP_CONFIG "/chat.txt", CAPP_O_READ), got = 0, i;
  out[0] = 0;
  if (fd < 0) return;
  got = api->read(fd, out, (size_t)(n - 1));
  api->close(fd);
  if (got < 0) got = 0;
  out[got] = 0;
  for (i = 0; out[i]; i++) if (out[i] == '\n' || out[i] == '\r') { out[i] = 0; break; }
}

/* `s`: today's result, as a line in Chat. */
static void share(void) {
  char line[40], name[24], sc[4];
  WdFriend me;
  if (!daily_over()) { say("finish today's first", C_DIM, 1800); return; }
  if (G.rq != RQ_NONE) { say("busy, try again", C_DIM, 1500); return; }
  me.result = G.g.over == WD_WON ? G.g.n : WDF_X;
  wdf_score(&me, sc);
  api->fmt(line, sizeof line, "Wordle #%d %s", (int)G.today + 1, sc);
  chat_name(name, sizeof name);
  if (soc_share(api, "Wordle", line, name) == 0) { G.rq = RQ_SHARE; say("sending...", C_DIM, 0); }
  else say("no network", C_WARN, 1800);
}

/* 1 when what is drawn changed. */
static int friends_tick(void) {
  int n;
  if (G.rq == RQ_NONE) return 0;
  n = api->http_poll(G.reply, sizeof G.reply);
  if (n == CAPP_HTTP_PENDING) return 0;
  if (G.rq == RQ_PUB) {
    G.rq = RQ_NONE;
    if (n < 0) return 0;
    G.published = 1;
    if (soc_pub_each(api, "wordle", "day") == 0) G.rq = RQ_EACH;
    return 0;
  }
  if (G.rq == RQ_EACH) {
    G.rq = RQ_NONE;
    if (n < 0) return 0;
    friends_read(n);
    return 1;
  }
  G.rq = RQ_NONE;                                /* RQ_SHARE */
  say(n < 0 ? "could not send" : "sent to Chat", n < 0 ? C_WARN : C_GREEN, 2500);
  return 1;
}

/* ---- drawing --------------------------------------------------------------- */

static uint16_t state_colour(int s) {
  return s == WD_CORRECT ? C_GREEN : s == WD_PRESENT ? C_YELLOW : s == WD_ABSENT ? C_GREY : C_BG;
}

static void text_centred(int f, int cx, int y, const char *s, uint16_t fg, uint16_t bg) {
  api->text_font(f, (int16_t)(cx - api->text_width(f, s) / 2), (int16_t)y, s, fg, bg);
}

/* One tile: filled for a scored letter, outlined for a typed or empty one. */
static void paint_tile(int x, int y, char c, int state, int typed, int warn) {
  CRect r = capp_rect(x, y, TILE, TILE);
  uint16_t bg = state ? state_colour(state) : C_BG;
  api->fill(r, bg);
  if (c) {
    char s[2];
    s[0] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    s[1] = 0;
    text_centred(G.f_tile, x + TILE / 2, y + (TILE - api->font_height(G.f_tile)) / 2 + 1, s, C_TEXT, bg);
  }
  if (!state) api->frame(r, warn ? C_WARN : typed ? C_EDGE_IN : C_EDGE);
}

static void paint_board(void) {
  int row, col;
  for (row = 0; row < WD_TRIES; row++)
    for (col = 0; col < WD_LEN; col++) {
      int x = BX + col * PITCH, y = BY + row * PITCH;
      if (row < G.g.n) paint_tile(x, y, G.g.guess[row][col], G.g.score[row][col], 0, 0);
      else if (row == G.g.n && !G.g.over)
        paint_tile(x, y, col < G.g.len ? G.g.cur[col] : 0, WD_EMPTY, col < G.g.len,
                   G.shake_row == row);
      else paint_tile(x, y, 0, WD_EMPTY, 0, 0);
    }
}

static void paint_keys(void) {
  int r;
  for (r = 0; r < 3; r++) {
    const char *k = KB_ROWS[r];
    int i, x0 = PX + 2 + (r == 1 ? 6 : r == 2 ? 18 : 0);
    for (i = 0; k[i]; i++) {
      int s = G.g.keys[k[i] - 'a'];
      uint16_t bg = s ? state_colour(s) : C_KEY;
      char t[2];
      CRect key = capp_rect(x0 + i * KP, KY + r * KROW, KW, KH);
      t[0] = (char)(k[i] - 32);
      t[1] = 0;
      api->fill(key, bg);
      api->text((int16_t)(key.x + 3), (int16_t)(key.y + 3), t, s == WD_ABSENT ? C_DIM : C_TEXT, bg);
    }
  }
}

static void title(char *b, int n) {
  if (G.mode == MODE_DAILY) api->fmt(b, (size_t)n, "#%d", (int)G.today + 1);
  else api->fmt(b, (size_t)n, "practice");
}

/* The line under the title: how far through, or how it ended. */
static void status_line(char *b, int n, uint16_t *colour) {
  *colour = C_DIM;
  if (G.g.over == WD_PLAYING) api->fmt(b, (size_t)n, "guess %d of %d", G.g.n + 1, WD_TRIES);
  else if (G.g.over == WD_WON) { api->fmt(b, (size_t)n, "%s", PRAISE[G.g.n - 1]); *colour = C_GREEN; }
  else {
    int i;
    api->fmt(b, (size_t)n, "it was %s", G.g.answer);
    for (i = 7; b[i]; i++) if (b[i] >= 'a' && b[i] <= 'z') b[i] = (char)(b[i] - 32);
    *colour = C_WARN;
  }
}

static void paint_panel(void) {
  char b[40];
  uint16_t colour;
  int w;
  api->text_font(G.f_tile, PX, 3, "Wordle", C_TEXT, C_BG);
  w = api->text_width(G.f_tile, "Wordle");
  title(b, sizeof b);
  api->text_font(G.f_ui, (int16_t)(PX + w + 6), 3, b, C_DIM, C_BG);
  status_line(b, sizeof b, &colour);
  api->text_font(G.f_ui, PX, 21, b, colour, C_BG);
  paint_keys();
  if (G.msg[0]) api->text_font(G.f_ui, PX, MSG_Y, G.msg, G.msg_colour, C_BG);
  else if (G.g.over && G.mode == MODE_DAILY && G.nfr) {
    char line[48], sc[4];
    int i, at = 0;
    line[0] = 0;
    for (i = 0; i < G.nfr && at < (int)sizeof line - 12; i++) {
      wdf_score(&G.fr[i], sc);
      at += api->fmt(line + at, sizeof line - (size_t)at, "%s%s %s", i ? "  " : "",
                     G.fr[i].display[0] ? G.fr[i].display : G.fr[i].name, sc);
    }
    api->text_font(G.f_ui, PX, MSG_Y, line, C_TEXT, C_BG);
  } else if (G.g.over && G.mode == MODE_DAILY)
    api->text_font(G.f_ui, PX, MSG_Y, "new one tomorrow", C_DIM, C_BG);
}

static void paint_game(CRect c) {
  /* One fill under everything: the OS composes a whole repaint off the
   * panel, so nothing drawn over it reaches the screen twice. */
  api->fill(capp_rect(c.x, c.y, c.w, c.h - FOOT_H), C_BG);
  paint_board();
  paint_panel();
  footer_paint(api, c, daily_over() ? "s share  f friends  tab stats"
                       : G.g.over ? "n practice  d daily  tab stats"
                       : "enter guess  bksp delete  tab stats");
}

static void paint_stats(CRect c) {
  static const char *const LABEL[4] = { "played", "win %", "streak", "best" };
  int v[4], i, most = 1, top = 67, bar_x = 22, bar_max;
  char b[24];
  int32_t today = G.today;

  api->fill(capp_rect(c.x, c.y, c.w, c.h - FOOT_H), C_BG);
  api->text_font(G.f_tile, 8, 3, "Statistics", C_TEXT, C_BG);
  api->text_font(G.f_ui, 120, 3, "the daily puzzle", C_DIM, C_BG);

  v[0] = G.st.played;
  v[1] = G.st.played ? (G.st.won * 100 + G.st.played / 2) / G.st.played : 0;
  v[2] = today == WD_NO_DAY ? G.st.streak : wd_stats_streak(&G.st, today);
  v[3] = G.st.best;
  for (i = 0; i < 4; i++) {
    int cx = 30 + i * 60;
    api->fmt(b, sizeof b, "%d", v[i]);
    text_centred(G.f_num, cx, 20, b, C_TEXT, C_BG);
    text_centred(G.f_ui, cx, 48, LABEL[i], C_DIM, C_BG);
  }

  for (i = 0; i < WD_TRIES; i++) if (G.st.dist[i] > most) most = G.st.dist[i];
  bar_max = c.w - bar_x - 12;
  for (i = 0; i < WD_TRIES; i++) {
    int y = top + i * 9, n = G.st.dist[i];
    int w = 12 + (bar_max - 12) * n / most;
    int last = G.mode == MODE_DAILY && G.g.over == WD_WON && G.g.n == i + 1;
    uint16_t bar = last ? C_GREEN : C_GREY;
    api->fmt(b, sizeof b, "%d", i + 1);
    api->text(10, (int16_t)y, b, C_TEXT, C_BG);
    api->fmt(b, sizeof b, "%d", n);
    if (w < 6 * (int)api->str_len(b) + 4) w = 6 * (int)api->str_len(b) + 4;
    api->fill(capp_rect(bar_x, y, w, 8), bar);
    api->text((int16_t)(bar_x + w - 2 - 6 * (int)api->str_len(b)), (int16_t)y, b, C_TEXT, bar);
  }
  footer_paint(api, c, "esc back  n practice  d daily");
}

/* Each friend's day: their name and result over their grid of colours. */
static void paint_friends(CRect c) {
  char b[40], sc[4];
  int i;
  api->fill(capp_rect(c.x, c.y, c.w, c.h - FOOT_H), C_BG);
  api->text_font(G.f_tile, 8, 3, "Friends", C_TEXT, C_BG);
  api->fmt(b, sizeof b, "#%d", (int)G.today + 1);
  api->text_font(G.f_ui, 80, 3, b, C_DIM, C_BG);
  if (!G.nfr) {
    api->text_font(G.f_ui, 8, 30, G.rq != RQ_NONE ? "asking the server..." :
                   "no friend has finished today's yet", C_DIM, C_BG);
    api->text_font(G.f_ui, 8, 48, "friends are added on the dashboard", C_DIM, C_BG);
  }
  for (i = 0; i < G.nfr && i < 4; i++) {
    const WdFriend *f = &G.fr[i];
    int x = 8 + i * 58, r, k;
    api->fmt(b, sizeof b, "%s", f->display[0] ? f->display : f->name);
    if (api->str_len(b) > 8) b[8] = 0;
    api->text_font(G.f_ui, (int16_t)x, 22, b, C_TEXT, C_BG);
    wdf_score(f, sc);
    api->text_font(G.f_ui, (int16_t)x, 36, sc, f->result == WDF_X ? C_WARN : C_GREEN, C_BG);
    for (r = 0; r < f->nrows; r++)
      for (k = 0; k < WD_LEN; k++)
        api->fill(capp_rect(x + k * 9, 52 + r * 9, 8, 8), state_colour(f->rows[r][k]));
  }
  footer_paint(api, c, "esc back  r refresh  s share");
}

static void app_paint(void *st, CRect c) {
  (void)st;
  if (G.view == V_FRIENDS) paint_friends(c);
  else if (G.view == V_STATS) paint_stats(c);
  else paint_game(c);
}

/* ---- keys and the clock ------------------------------------------------------ */

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (G.view == V_FRIENDS) {
    if (api->key_repeat()) return 1;
    if (k == CAPP_KEY_ESC || k == 'f') { G.view = V_GAME; return 1; }
    if (k == 'r') { friends_sync(); return 1; }
    if (k == 's') { share(); return 1; }
    return 0;
  }
  if (G.view == V_STATS) {
    if (api->key_repeat()) return 1;
    if (k == CAPP_KEY_ESC || k == '\t') { G.view = V_GAME; return 1; }
    if (k == 'n') { start_practice(); return 1; }
    if (k == 'd') { start_daily(); return 1; }
    return 0;
  }
  if (k == '\t') { if (!api->key_repeat()) G.view = V_STATS; return 1; }
  if (G.g.over) {
    if (api->key_repeat()) return 1;
    if (k == 's') { share(); return 1; }
    if (k == 'f' && daily_over()) { G.view = V_FRIENDS; friends_sync(); return 1; }
    if (k == 'r') { friends_sync(); return 1; }
    if (k == 'n' || k == CAPP_KEY_ENTER) { start_practice(); return 1; }
    if (k == 'd') { start_daily(); return 1; }
    return 0;
  }
  if (k == CAPP_KEY_ENTER) { if (!api->key_repeat()) submit(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (wd_back(&G.g)) { G.shake_row = -1; if (G.msg_until) say("", C_TEXT, 0); }
    return 1;
  }
  if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z')) {
    wd_type(&G.g, k);
    return 1;
  }
  return 0;
}

/* A message timing out, and the friends' replies arriving. */
static int app_tick(void *st, uint32_t now) {
  (void)st;
  if (friends_tick()) return 1;
  if (G.msg_until && (int32_t)(now - G.msg_until) >= 0) {
    G.msg[0] = 0;
    G.msg_until = 0;
    G.shake_row = -1;
    return 1;
  }
  return 0;
}

static int app_wants_text(void *st) {
  (void)st;
  return G.view == V_GAME && G.g.over == WD_PLAYING;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Wordle",
  /* 16x16: a three-by-three of tiles. */
  { 0x00, 0x00, 0x7B, 0xDE, 0x7B, 0xDE, 0x7B, 0xDE,
    0x7B, 0xDE, 0x00, 0x00, 0x7B, 0xDE, 0x7B, 0xDE,
    0x7B, 0xDE, 0x7B, 0xDE, 0x00, 0x00, 0x7B, 0xDE,
    0x7B, 0xDE, 0x7B, 0xDE, 0x7B, 0xDE, 0x00, 0x00 },
  "letters\ttype a guess\nenter\tplay it\nbksp\tdelete a letter\n"
  "tab\tstatistics\nn\tpractice game (when one is over)\n"
  "d\ttoday's puzzle (when one is over)\n"
  "f\tfriends' results for today (when it is done)\n"
  "s\tsend your result to Chat\nr\tread friends' results again\n"
  "green\tright letter, right place\nyellow\tin the word, elsewhere\n"
  "grey\tnot in the word\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  G.seed = (api->ticks_ms() ^ api->epoch() * 2654435761u) | 1u;
  G.shake_row = -1;
  G.f_tile = api->font_load("ui13b");
  G.f_ui = api->font_load("ui13");
  G.f_num = api->font_load("num30");
  load_stats();
  start_daily();
  friends_sync();                     /* today's already done: show theirs */

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
