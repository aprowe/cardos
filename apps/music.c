/* Music: the tracks dragged onto the dashboard, on the card, played.
 *
 * The dashboard turns each track into what the speaker plays -- 22050 Hz
 * mono 16-bit WAV, converted in the browser -- and the server keeps it
 * (server/music.py). Opening this shows what is on the card at once, then
 * syncs: the list, then each track not here yet, streamed to the card with
 * a bar ("getting 2 of 5, 40%"); a key stops it. A track gone from the
 * server goes from /music. Synced tracks are /music/<id>.wav, their titles
 * in /music/.tracks.
 *
 * Playing is the kernel's audio task (api->audio): enter plays, space
 * pauses and goes on, [ and ] (or shift with the arrows) go back and on ten
 * seconds, left and right are the tracks either side, esc stops, and when
 * one ends the next starts -- in order, or at random with shuffle. Leaving
 * the app lets the track that is playing finish; nothing follows it.
 */
#include "kernel/app/capp.h"
#include "apps/footer.h"
#include "apps/safefile.h"

static const CardApi *api;
static const CappAudio *au;

#define DIR       "/music"
#define INDEX     DIR "/.tracks"
#define MAXT      64
#define TITLE     40
#define IDL       40
#define TOP_H     14
#define ROW_H     12
#define NOW_H     24
#define TICK_MS   250

#define CLR_BG    CAPP_RGB(15, 17, 23)
#define CLR_TEXT  CAPP_RGB(232, 236, 244)
#define CLR_DIM   CAPP_RGB(128, 136, 152)
#define CLR_SEL   CAPP_RGB(36, 44, 62)
#define CLR_ACC   CAPP_RGB(250, 170, 80)
#define CLR_BAD   CAPP_RGB(240, 110, 96)
#define CLR_BAR   CAPP_RGB(46, 52, 66)
#define CLR_NOW   CAPP_RGB(22, 26, 36)

enum { ACT_SYNC = 1, ACT_SHUFFLE, ACT_DELETE };

static struct {
  char     id[MAXT][IDL];          /* the server's id, or a local file's name */
  uint8_t  local[MAXT];            /* put on the card by hand: not the server's */
  char     title[MAXT][TITLE];
  uint32_t ms[MAXT];
  int      n, sel, top;
  int      playing;                /* index, or -1 */
  int      stopped;                /* the user stopped it: do not go on */
  int      shuffle;
  int      ask_delete;
  int      sync_soon;
  char     status[48];
  int      bad;
  uint32_t next_tick, seed;
  uint32_t shown_s;                /* the second the strip last showed */
  CRect    c;
  int      have_c;
  char     reply[MAXT * 72];
  /* a download under way */
  int      fd, got, want, last_drawn, cancelled;
} M;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)(w > 0 ? w : 0); r.h = (int16_t)h;
  return r;
}

static int same(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static void path_of(int i, char *out, int n) { api->fmt(out, (size_t)n, DIR "/%s.wav", M.id[i]); }

static void say(int bad, const char *s) {
  api->fmt(M.status, sizeof M.status, "%s", s);
  M.bad = bad;
}

static long to_num(const char *s) {
  long v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v;
}

/* Field `k` of a tab-separated line into out. */
static void field(const char *line, int k, char *out, int n) {
  int i = 0;
  while (k > 0 && *line && *line != '\n') { if (*line++ == '\t') k--; }
  while (*line && *line != '\t' && *line != '\n' && i < n - 1) out[i++] = *line++;
  out[i] = 0;
}

static const char *next_line(const char *p) {
  while (*p && *p != '\n') p++;
  return *p ? p + 1 : p;
}

/* ---- the index, and what is on the card ------------------------------------------ */

static void index_load(void) {
  int fd = safe_open_read(api, INDEX), n;
  const char *p;
  M.n = 0;
  if (fd < 0) return;
  n = api->read(fd, M.reply, sizeof M.reply - 1);
  api->close(fd);
  if (n <= 0) return;
  M.reply[n] = 0;
  for (p = M.reply; *p && M.n < MAXT; p = next_line(p)) {
    char ms[12], path[64];
    CappStat st;
    field(p, 0, M.id[M.n], sizeof M.id[M.n]);
    field(p, 1, M.title[M.n], TITLE);
    field(p, 2, ms, sizeof ms);
    M.ms[M.n] = (uint32_t)to_num(ms);
    path_of(M.n, path, sizeof path);
    M.local[M.n] = 0;
    if (M.id[M.n][0] && api->stat(path, &st) == 0) M.n++;   /* only what is here */
  }
}

/* WAVs put in /music by hand -- over USB disk mode, say -- that the server
 * does not know: listed by their names, never removed by a sync, and
 * deleted from the card only. Their length is worked out from the header. */
static uint32_t wav_ms(const char *path, uint32_t size) {
  uint8_t h[44];
  uint32_t rate, sec, rem;
  int fd = api->open(path, CAPP_O_READ), n;
  if (fd < 0) return 0;
  n = api->read(fd, h, sizeof h);
  api->close(fd);
  if (n < 44) return 0;
  rate = (uint32_t)h[28] | (uint32_t)h[29] << 8 | (uint32_t)h[30] << 16 | (uint32_t)h[31] << 24;
  if (!rate || size < 44) return 0;
  size -= 44;
  sec = size / rate;
  rem = size % rate;
  return sec * 1000 + rem * 1000 / rate;  /* rem < rate < 400000: no overflow */
}

static void add_local(void) {
  static char names[MAXT][IDL + 8];
  int n = api->list(DIR, &names[0][0], MAXT, IDL + 8), i, j;
  for (i = 0; i < n && M.n < MAXT; i++) {
    char path[64];
    CappStat st;
    int len = (int)api->str_len(names[i]);
    if (len < 5 || len - 4 >= IDL || names[i][0] == '.') continue;
    if (!same(names[i] + len - 4, ".wav") && !same(names[i] + len - 4, ".WAV")) continue;
    names[i][len - 4] = 0;
    for (j = 0; j < M.n; j++) if (same(M.id[j], names[i])) break;
    if (j < M.n) continue;
    /* FAT ignores case, so NAME.WAV opens as NAME.wav too. */
    api->fmt(path, sizeof path, DIR "/%s.wav", names[i]);
    if (api->stat(path, &st) != 0) continue;
    api->fmt(M.id[M.n], sizeof M.id[0], "%s", names[i]);
    api->fmt(M.title[M.n], TITLE, "%s", names[i]);
    M.ms[M.n] = wav_ms(path, (uint32_t)st.size);
    M.local[M.n] = 1;
    M.n++;
  }
}

static void index_save(void) {
  SafeFile f;
  int i;
  if (safe_begin(&f, api, INDEX) != 0) return;
  for (i = 0; i < M.n; i++) {
    char line[96];
    if (M.local[i]) continue;
    int k = api->fmt(line, sizeof line, "%s\t%s\t%lu\n", M.id[i], M.title[i], (unsigned long)M.ms[i]);
    safe_write(&f, line, (size_t)k);
  }
  safe_commit(&f);
}

/* ---- the screen --------------------------------------------------------------------- */

static int list_rows(void) { return (M.c.h - TOP_H - NOW_H - FOOT_H) / ROW_H; }

static CRect now_rect(void) { return rect(M.c.x, M.c.y + M.c.h - FOOT_H - NOW_H, M.c.w, NOW_H); }
static CRect top_rect(void) { return rect(M.c.x, M.c.y, M.c.w, TOP_H); }

static void mmss(uint32_t ms, char *out, int n) {
  uint32_t s = ms / 1000;
  api->fmt(out, (size_t)n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void paint_top(void) {
  char line[48];
  CRect r = top_rect();
  int w;
  api->fmt(line, sizeof line, "%s", M.status[0] ? M.status :
           M.n ? "" : "no tracks: add some on the dashboard");
  /* Written over itself, padded: nothing is cleared under text. */
  api->text((int16_t)(r.x + 6), (int16_t)(r.y + 3), "Music", CLR_ACC, CLR_BG);
  w = (int)api->str_len(line) * 6;
  api->fill(rect(r.x + 36, r.y, r.w - 42 - w, TOP_H), CLR_BG);
  api->text((int16_t)(r.x + r.w - 6 - w), (int16_t)(r.y + 3), line, M.bad ? CLR_BAD : CLR_DIM, CLR_BG);
  api->fill(rect(r.x, r.y, 6, TOP_H), CLR_BG);
  api->fill(rect(r.x, r.y, r.w, 3), CLR_BG);
  api->fill(rect(r.x, r.y + 11, r.w, TOP_H - 11), CLR_BG);
}

static void paint_row(int i, int y) {
  char line[64], len[12];
  uint16_t bg = i == M.sel ? CLR_SEL : CLR_BG;
  int w;
  mmss(M.ms[i], len, sizeof len);
  api->fmt(line, sizeof line, "%s%-30.30s", i == M.playing ? "> " : "  ", M.title[i]);
  api->text((int16_t)(M.c.x + 4), (int16_t)(y + 2), line, i == M.playing ? CLR_ACC : CLR_TEXT, bg);
  w = (int)api->str_len(len) * 6;
  api->text((int16_t)(M.c.x + M.c.w - 6 - w), (int16_t)(y + 2), len, CLR_DIM, bg);
  /* Round the text, not under it. */
  api->fill(rect(M.c.x, y, 4, ROW_H), bg);
  api->fill(rect(M.c.x, y, M.c.w, 2), bg);
  api->fill(rect(M.c.x, y + 10, M.c.w, ROW_H - 10), bg);
  api->fill(rect(M.c.x + 4 + 32 * 6, y + 2, M.c.w - 10 - w - 32 * 6, 8), bg);
  api->fill(rect(M.c.x + M.c.w - 6, y + 2, 6, 8), bg);
}

static void paint_list(void) {
  int rows = list_rows(), i, y = M.c.y + TOP_H;
  if (M.sel < M.top) M.top = M.sel;
  if (M.sel >= M.top + rows) M.top = M.sel - rows + 1;
  for (i = M.top; i < M.n && i - M.top < rows; i++, y += ROW_H) paint_row(i, y);
  if (y < M.c.y + TOP_H + rows * ROW_H) api->fill(rect(M.c.x, y, M.c.w, M.c.y + TOP_H + rows * ROW_H - y), CLR_BG);
  /* What the rows do not reach above the strip. */
  y = M.c.y + TOP_H + rows * ROW_H;
  api->fill(rect(M.c.x, y, M.c.w, now_rect().y - y), CLR_BG);
}

static void paint_now(void) {
  CRect r = now_rect();
  char line[64], a[12], b[12];
  int barw = r.w - 12, fill = 0, w;
  uint32_t pos = 0, tot = 0;
  int paused = M.playing >= 0 && au && au->paused && au->paused();
  if (M.playing >= 0 && au && au->state() == CAPP_AUDIO_PLAYING) {
    pos = au->pos_ms();
    tot = au->total_ms();
    /* 32-bit: an app has no 64-bit division, and barw times half an hour in
     * milliseconds is still under 2^32. */
    if (tot) fill = (int)((uint32_t)barw * pos / tot);
    if (fill > barw) fill = barw;
  }
  if (M.ask_delete) api->fmt(line, sizeof line, "delete %.24s? y yes  n no", M.title[M.sel]);
  else if (M.playing >= 0) api->fmt(line, sizeof line, "%-36.36s", M.title[M.playing]);
  else api->fmt(line, sizeof line, "%-36s", "stopped");
  api->text((int16_t)(r.x + 6), (int16_t)(r.y + 3), line, M.ask_delete ? CLR_BAD : CLR_TEXT, CLR_NOW);
  mmss(pos, a, sizeof a);
  mmss(tot ? tot : (M.playing >= 0 ? M.ms[M.playing] : 0), b, sizeof b);
  api->fmt(line, sizeof line, "%s / %s%s%s  vol %d", a, b, paused ? "  paused" : "",
           M.shuffle ? "  shuffle" : "", au ? au->volume() : 0);
  w = (int)api->str_len(line) * 6;
  api->text((int16_t)(r.x + 6), (int16_t)(r.y + 13), line, CLR_DIM, CLR_NOW);
  api->fill(rect(r.x + 6 + w, r.y + 13, r.w - 6 - w, 8), CLR_NOW);
  api->fill(rect(r.x + 6, r.y + 22, fill, 2), CLR_ACC);              /* the bar: the */
  api->fill(rect(r.x + 6 + fill, r.y + 22, barw - fill, 2), CLR_BAR); /* two parts, once */
  api->fill(rect(r.x, r.y, 6, NOW_H), CLR_NOW);
  api->fill(rect(r.x, r.y, r.w, 3), CLR_NOW);
  api->fill(rect(r.x, r.y + 11, r.w, 2), CLR_NOW);
  api->fill(rect(r.x + r.w - 6, r.y, 6, NOW_H), CLR_NOW);
  M.shown_s = pos / 1000;
}

static void app_paint(void *st, CRect c) {
  (void)st;
  M.c = c;
  M.have_c = 1;
  paint_top();
  paint_list();
  paint_now();
  footer_paint(api, c, "space pause  enter play  [ ] seek");
}

static void mark_all(void) { if (M.have_c) api->damage(M.c); }

/* ---- playing -------------------------------------------------------------------------- */

static void play(int i) {
  char path[40];
  int rc;
  if (!au || i < 0 || i >= M.n) return;
  if (au->state() != CAPP_AUDIO_IDLE) au->stop();
  /* The audio task lets go on its own time: wait a moment for it. */
  for (rc = 0; rc < 40 && au->state() != CAPP_AUDIO_IDLE; rc++) {
    uint32_t t = api->ticks_ms() + 10;
    while ((int32_t)(api->ticks_ms() - t) < 0) ;
  }
  path_of(i, path, sizeof path);
  rc = au->play(path);
  if (rc != 0) {
    say(1, au->error && au->error()[0] ? au->error() : "would not play");
    M.playing = -1;
    return;
  }
  M.playing = i;
  M.sel = i;
  M.stopped = 0;
  say(0, "");
}

static int pick_next(int dir) {
  if (M.n == 0) return -1;
  if (M.shuffle && M.n > 1) {
    int k;
    M.seed = M.seed * 1664525u + 1013904223u;
    k = (int)((M.seed >> 8) % (uint32_t)(M.n - 1));
    return k >= M.playing ? k + 1 : k;                 /* never the same one */
  }
  return ((M.playing < 0 ? M.sel : M.playing) + dir + M.n) % M.n;
}

/* ---- sync -------------------------------------------------------------------------------- */

/* While a track streams in: to the card, and a bar drawn as it comes -- the
 * shell is not painting while this runs. Any key stops it. */
static int on_data(void *ctx, const uint8_t *d, int n) {
  (void)ctx;
  if (api->write(M.fd, d, (size_t)n) != n) return 1;
  M.got += n;
  if (M.have_c && (M.got - M.last_drawn >= 48 * 1024 || M.got >= M.want)) {
    char line[48];
    CRect r = top_rect();
    int pct = M.want >= 100 ? M.got / (M.want / 100) : 0;
    if (pct > 100) pct = 100;
    api->fmt(line, sizeof line, "  getting: %d%%  ", pct);
    api->text((int16_t)(r.x + r.w - 6 - (int)api->str_len(line) * 6), (int16_t)(r.y + 3),
              line, CLR_DIM, CLR_BG);
    M.last_drawn = M.got;
  }
  if (api->key_pending()) { M.cancelled = 1; return 1; }
  return 0;
}

static void sync_run(void) {
  static char ids[MAXT][12], titles[MAXT][TITLE];
  static uint32_t ms[MAXT], bytes[MAXT];
  char url[160], path[64], part[64];
  const char *p;
  int r, n = 0, i, j, fetched = 0, missing = 0;

  if (!api->net_ready() && api->net_connect(15000) != 0) { say(0, "offline: playing what is here"); mark_all(); return; }
  api->fmt(url, sizeof url, "%s/music", api->proxy());
  r = api->http("GET", url, 0, 0, "", M.reply, sizeof M.reply, 15000);
  if (r < 0 || (M.reply[0] == 'e' && M.reply[1] == 'r')) { say(1, "the server did not answer"); mark_all(); return; }
  for (p = M.reply; *p && n < MAXT; p = next_line(p)) {
    char num[16];
    field(p, 0, ids[n], sizeof ids[n]);
    field(p, 1, titles[n], TITLE);
    field(p, 2, num, sizeof num); ms[n] = (uint32_t)to_num(num);
    field(p, 3, num, sizeof num); bytes[n] = (uint32_t)to_num(num);
    if (ids[n][0]) n++;
  }
  /* Gone from the server: gone from here (not the one playing). */
  for (i = 0; i < M.n; i++) {
    for (j = 0; j < n; j++) if (same(M.id[i], ids[j])) break;
    if (j == n && i != M.playing && !M.local[i]) { path_of(i, path, sizeof path); api->remove(path); }
  }
  api->mkdir(DIR);
  /* The rest, each streamed to NAME.part and renamed only when whole. */
  for (j = 0; j < n; j++) {
    CappStat st;
    api->fmt(path, sizeof path, DIR "/%s.wav", ids[j]);
    if (api->stat(path, &st) == 0) continue;
    missing++;
  }
  for (j = 0; j < n && !M.cancelled; j++) {
    CappStat st;
    api->fmt(path, sizeof path, DIR "/%s.wav", ids[j]);
    if (api->stat(path, &st) == 0) continue;
    api->fmt(M.status, sizeof M.status, "getting %d of %d", fetched + 1, missing);
    if (M.have_c) paint_top();
    api->fmt(part, sizeof part, DIR "/%s.part", ids[j]);
    M.fd = api->open(part, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
    if (M.fd < 0) { say(1, "cannot write to the card"); break; }
    M.got = M.last_drawn = 0;
    M.want = (int)bytes[j];
    api->fmt(url, sizeof url, "%s/music/track?id=%s", api->proxy(), ids[j]);
    r = api->http_stream(url, on_data, 0, 60000);
    api->close(M.fd);
    if (r < 0 || M.got != M.want) { api->remove(part); if (!M.cancelled) say(1, "a track did not come whole: r again"); break; }
    api->rename(part, path);
    fetched++;
  }
  /* The list is the server's, as far as the card has it. */
  M.n = 0;
  for (j = 0; j < n; j++) {
    CappStat st;
    api->fmt(path, sizeof path, DIR "/%s.wav", ids[j]);
    if (api->stat(path, &st) != 0) continue;
    api->fmt(M.id[M.n], sizeof M.id[0], "%s", ids[j]);
    M.local[M.n] = 0;
    api->mem_cpy(M.title[M.n], titles[j], sizeof M.title[0]);
    M.ms[M.n] = ms[j];
    M.n++;
  }
  index_save();
  add_local();
  if (M.sel >= M.n) M.sel = M.n ? M.n - 1 : 0;
  if (M.playing >= M.n) M.playing = -1;
  if (M.cancelled) say(0, "sync stopped: r to go on");
  else if (!M.bad) say(0, fetched ? (fetched == 1 ? "1 new track" : "new tracks") : "");
  if (!M.bad && fetched > 1) api->fmt(M.status, sizeof M.status, "%d new tracks", fetched);
  M.cancelled = 0;
  mark_all();
}

static void delete_sel(void) {
  char url[160], reply[32], path[64];
  if (!M.n) return;
  api->fmt(url, sizeof url, "%s/music/track?id=%s", api->proxy(), M.id[M.sel]);
  if (!M.local[M.sel] &&
      (!api->net_ready() || api->http("DELETE", url, 0, 0, "", reply, sizeof reply, 15000) < 0)) {
    say(1, "offline: delete it on the dashboard");
    return;
  }
  if (M.sel == M.playing) { au->stop(); M.playing = -1; M.stopped = 1; }
  path_of(M.sel, path, sizeof path);
  api->remove(path);
  for (; M.sel < M.n - 1; M.sel++) {
    api->mem_cpy(M.id[M.sel], M.id[M.sel + 1], sizeof M.id[0]);
    api->mem_cpy(M.title[M.sel], M.title[M.sel + 1], sizeof M.title[0]);
    M.ms[M.sel] = M.ms[M.sel + 1];
    M.local[M.sel] = M.local[M.sel + 1];
    if (M.playing == M.sel + 1) M.playing = M.sel;
  }
  M.n--;
  if (M.sel >= M.n) M.sel = M.n ? M.n - 1 : 0;
  index_save();
  say(0, "deleted");
}

/* ---- keys and time ------------------------------------------------------------------------- */

static int do_action(int a) {
  switch (a) {
  case ACT_SYNC: say(0, "syncing..."); M.sync_soon = 1; return 1;
  case ACT_SHUFFLE: M.shuffle = !M.shuffle; return 1;
  case ACT_DELETE: if (M.n) M.ask_delete = 1; return 1;
  }
  return 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (M.ask_delete) {
    M.ask_delete = 0;
    if (k == 'y' || k == 'Y') delete_sel();
    mark_all();
    return 1;
  }
  switch (k) {
  case CAPP_KEY_UP:    if (M.sel > 0) M.sel--; break;
  case CAPP_KEY_DOWN:  if (M.sel < M.n - 1) M.sel++; break;
  case CAPP_KEY_ENTER: play(M.sel); break;
  case ' ':
    if (M.playing >= 0 && au->state() == CAPP_AUDIO_PLAYING) au->pause(!au->paused());
    else play(M.sel);
    break;
  /* Ten seconds back or on: [ ], or shift with the arrows (< and ?). */
  case '[': case '<': case ']': case '?':
    if (M.playing >= 0 && au->state() == CAPP_AUDIO_PLAYING) {
      uint32_t at = au->pos_ms(), tot = au->total_ms();
      if (k == '[' || k == '<') at = at > 10000 ? at - 10000 : 0;
      else at = at + 10000 < tot ? at + 10000 : tot;
      au->seek_ms(at);
    }
    break;
  case 'x': case 'X':
    if (M.playing >= 0) { M.stopped = 1; au->stop(); }
    break;
  case CAPP_KEY_LEFT:  if (M.n) play(pick_next(-1)); break;
  case CAPP_KEY_RIGHT: if (M.n) play(pick_next(1)); break;
  case '+': case '=':  au->set_volume(au->volume() + 10 > 100 ? 100 : au->volume() + 10); break;
  case '-': case '_':  au->set_volume(au->volume() < 10 ? 0 : au->volume() - 10); break;
  case 's': case 'S':  return do_action(ACT_SHUFFLE) && (mark_all(), 1);
  case 'r': case 'R':  do_action(ACT_SYNC); break;
  case 'd': case 'D': case 0x7F: do_action(ACT_DELETE); break;
  case CAPP_KEY_ESC:   if (M.playing >= 0) { M.stopped = 1; au->stop(); break; } return 0;
  default: return 0;
  }
  mark_all();
  return 1;
}

static int app_action(void *st, int a) { (void)st; return do_action(a) && (mark_all(), 1); }

static int app_tick(void *st, uint32_t now) {
  int s;
  (void)st;
  if (M.sync_soon && M.have_c) { M.sync_soon = 0; sync_run(); return 1; }
  if ((int32_t)(now - M.next_tick) < 0 || !au) return 0;
  M.next_tick = now + TICK_MS;
  s = au->state();
  /* A track ended by itself: the next one, unless the user stopped it. */
  if (M.playing >= 0 && s == CAPP_AUDIO_IDLE) {
    if (!M.stopped && M.n) play(pick_next(1));
    else M.playing = -1;
    mark_all();
    return 1;
  }
  /* Otherwise only the strip moves, once a second. */
  if (s == CAPP_AUDIO_PLAYING && au->pos_ms() / 1000 != M.shown_s && M.have_c) {
    api->damage(now_rect());
    return 1;
  }
  return 0;
}

static const CappAction ACTIONS[] = {
  { "sync", "Sync", 0, 0, ACT_SYNC, 0, 0, 0, 0 },
  { "shuffle", "Shuffle", 0, 0, ACT_SHUFFLE, 0, 0, 0, 0 },
  { "delete", "Delete", 0, 0, ACT_DELETE, 0, 0, 0, 0 },
};

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Music",
  /* 16x16: two quavers. */
  { 0x00, 0x00, 0x03, 0xFE, 0x03, 0xFE, 0x03, 0x02,
    0x03, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x02, 0x1E, 0x1E, 0x3E, 0x3E, 0x3E,
    0x3E, 0x1C, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "up/down\tchoose a track\nenter\tplay it from the start\nspace\tpause, go on (or play)\n"
  "[ ]\tten seconds back, on (or shift+left/right)\nleft/right\tthe track before or after\n"
  "s\tshuffle on and off\n+ -\tvolume\nr\tsync with the dashboard\nd, del\tdelete (asks; from the dashboard too)\n"
  "esc, x\tstop\n\ntracks come from the dashboard's Music page.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  au = api->audio();
  M.playing = -1;
  M.seed = api->ticks_ms();
  api->mkdir(DIR);
  index_load();
  add_local();
  M.sync_soon = 1;                  /* after the first paint: the list shows first */
  if (au && au->state() == CAPP_AUDIO_PLAYING) say(0, "something is playing");
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.actions = ACTIONS;
  UI.nactions = sizeof ACTIONS / sizeof ACTIONS[0];
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
