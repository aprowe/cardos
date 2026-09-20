/* Voice memos: record into /home/memos, listen back, throw away.
 *
 * The list is the whole screen: one row per memo with its length, the
 * newest at the top. `r` starts a recording and `r` again ends it, with a
 * meter and a clock while it runs; enter plays the highlighted one with a
 * bar that fills; `d` deletes after asking. The sound itself happens on a
 * kernel task (api->audio), so the meter moves and a key still stops it.
 *
 * Files are named by the clock when it is set -- 0920-1142.wav -- and by
 * count when it is not, so the list sorts into the order they were made
 * either way. 16 kHz mono 16-bit, the mic's native form: 32 KB a second,
 * which the card takes without noticing. */

#include "kernel/app/capp.h"
#include "apps/toolbar.h"

#define DIR       CAPP_HOME "/memos"
#define MAX_MEMOS 40
#define ROW_H     11
#define BYTES_PER_SEC 32000          /* 16 kHz * 2 bytes, mono */
#define MAX_MS    (10 * 60 * 1000)   /* ten minutes; the driver's ceiling */

#define CLR_BG      CAPP_RGB(28, 30, 36)
#define CLR_TEXT    CAPP_RGB(220, 224, 232)
#define CLR_DIM     CAPP_RGB(130, 138, 150)
#define CLR_SEL     CAPP_RGB(52, 80, 116)
#define CLR_BAR     CAPP_RGB(48, 82, 128)
#define CLR_BAR_FG  CAPP_RGB(232, 238, 248)
#define CLR_REC     CAPP_RGB(220, 60, 60)
#define CLR_METER   CAPP_RGB(90, 200, 120)
#define CLR_PLAY    CAPP_RGB(120, 180, 255)

typedef struct {
  char     name[40];
  uint32_t bytes;                  /* of audio, header excluded */
} Memo;

static const CardApi *api;
static const CappAudio *au;

enum { ASK_NONE = 0, ASK_DELETE };

static struct {
  Memo  memo[MAX_MEMOS];
  int   n;
  int   sel;
  int   top;
  int   rows;
  int   ask;
  int   busy;                      /* our own recording or playback is running */
  int   was_state;
  char  recording[64];             /* the file being recorded, for the list after */
  uint32_t started_ms;
  uint32_t strip_sig;              /* what the strip last showed, to skip repaints */
  uint32_t strip_at;
  char  status[48];
  CRect content;
} M;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *s) { api->fmt(M.status, sizeof M.status, "%s", s); }

static void path_of(const Memo *m, char *out, size_t n) {
  api->fmt(out, n, "%s/%s", DIR, m->name);
}

/* m:ss, for a length or a clock. */
static void mmss(uint32_t ms, char *out, size_t n) {
  uint32_t s = ms / 1000;
  api->fmt(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

/* ---- the list ------------------------------------------------------------ */

static int is_wav(const char *name) {
  size_t n = api->str_len(name);
  return n > 4 && (name[n - 4] == '.') &&
         (name[n - 3] == 'w' || name[n - 3] == 'W') &&
         (name[n - 2] == 'a' || name[n - 2] == 'A') &&
         (name[n - 1] == 'v' || name[n - 1] == 'V');
}

static int name_cmp(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return (unsigned char)*a - (unsigned char)*b;
}

static void rescan(void) {
  static CappEntry ent[MAX_MEMOS];
  int n, i, j;
  const char *keep = M.n ? M.memo[M.sel].name : "";
  char kept[40];
  api->fmt(kept, sizeof kept, "%s", keep);

  api->mkdir(DIR);
  M.n = 0;
  n = api->list_ex(DIR, ent, MAX_MEMOS);
  for (i = 0; i < n; i++) {
    Memo *m;
    if (ent[i].is_dir || !is_wav(ent[i].name)) continue;
    m = &M.memo[M.n++];
    api->fmt(m->name, sizeof m->name, "%s", ent[i].name);
    m->bytes = ent[i].size > 44 ? ent[i].size - 44 : 0;
  }
  /* Newest first: the names sort by time, so reverse order is it. Copied
   * with the API's mem_cpy: a struct assignment becomes a memcpy the
   * freestanding build has no library for. */
  for (i = 1; i < M.n; i++) {
    Memo t;
    api->mem_cpy(&t, &M.memo[i], sizeof t);
    for (j = i; j > 0 && name_cmp(M.memo[j - 1].name, t.name) < 0; j--)
      api->mem_cpy(&M.memo[j], &M.memo[j - 1], sizeof t);
    api->mem_cpy(&M.memo[j], &t, sizeof t);
  }
  M.sel = 0;
  for (i = 0; i < M.n; i++) if (!name_cmp(M.memo[i].name, kept)) { M.sel = i; break; }
  if (M.sel < M.top) M.top = M.sel;
  if (M.rows && M.sel >= M.top + M.rows) M.top = M.sel - M.rows + 1;
  if (!M.n) say("r records a memo");
  else api->fmt(M.status, sizeof M.status, "%d memo%s", M.n, M.n == 1 ? "" : "s");
}

/* A name that sorts by when it was made: the date and time if the clock
 * knows them, a counter past the highest one on the card if not. */
static void new_name(char *out, size_t n) {
  CappTime t;
  api->now(&t);
  if (t.synced) {
    api->fmt(out, n, "%s/%02u%02u-%02u%02u%02u.wav", DIR, t.month, t.day, t.hour, t.min, t.sec);
  } else {
    int i, high = 0;
    for (i = 0; i < M.n; i++) {
      const char *p = M.memo[i].name;
      int v = 0;
      if (p[0] != 'm') continue;
      for (p++; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
      if (v > high) high = v;
    }
    api->fmt(out, n, "%s/m%04d.wav", DIR, high + 1);
  }
}

/* ---- doing --------------------------------------------------------------- */

static void record(void) {
  int rc;
  if (au->state() != CAPP_AUDIO_IDLE) { au->stop(); return; }
  api->mkdir(DIR);
  new_name(M.recording, sizeof M.recording);
  rc = au->record(M.recording, MAX_MS);
  if (rc == 0) {
    M.busy = 1;
    M.started_ms = api->ticks_ms();
    say("recording  r stops");
  } else if (rc == -1) say("busy");
  else say(au->error()[0] ? au->error() : "could not record");
}

static void play(void) {
  char path[96];
  int rc;
  if (au->state() != CAPP_AUDIO_IDLE) { au->stop(); return; }
  if (!M.n) return;
  path_of(&M.memo[M.sel], path, sizeof path);
  rc = au->play(path);
  if (rc == 0) { M.busy = 1; say("playing  enter stops"); }
  else if (rc == -1) say("busy");
  else say(au->error()[0] ? au->error() : "cannot play that");
}

static void delete_memo(void) {
  char path[96];
  if (!M.n) return;
  path_of(&M.memo[M.sel], path, sizeof path);
  if (api->remove(path) != 0) { say("could not delete"); return; }
  rescan();
  say("deleted");
}

/* ---- painting ------------------------------------------------------------ */

static void paint_rows(CRect c, int list_h) {
  int i, y = c.y;
  M.rows = list_h / ROW_H;
  if (!M.n) {
    api->text((short)(c.x + 6), (short)(c.y + 6), "no memos yet", CLR_DIM, CLR_BG);
    api->text((short)(c.x + 6), (short)(c.y + 18), "r starts one", CLR_DIM, CLR_BG);
    return;
  }
  for (i = M.top; i < M.n && i < M.top + M.rows; i++, y += ROW_H) {
    const Memo *m = &M.memo[i];
    int sel = i == M.sel;
    unsigned short bg = sel ? CLR_SEL : CLR_BG;
    char len[12], line[48];
    mmss(m->bytes / (BYTES_PER_SEC / 1000), len, sizeof len);
    api->fill(rect(c.x, y, c.w, ROW_H), bg);
    api->fmt(line, sizeof line, "%s", m->name);
    api->text((short)(c.x + 6), (short)(y + 1), line, sel ? CLR_BAR_FG : CLR_TEXT, bg);
    api->text((short)(c.x + c.w - 6 - 6 * (short)api->str_len(len)), (short)(y + 1), len,
              sel ? CLR_BAR_FG : CLR_DIM, bg);
  }
}

/* The strip at the bottom: a meter and a clock while recording, a bar
 * while playing, the status line otherwise. */
/* While a job runs the strip is repainted many times a second, so it is
 * drawn in place -- each part paints its own background, the meter is the
 * lit part and the unlit part, no clearing first -- because a clear and a
 * redraw fifteen times a second is a flicker. */
static void paint_strip(CRect c) {
  int st = au->state();
  CRect s = rect(c.x, c.y + c.h - 24, c.w, 24);

  if (M.busy && st == CAPP_AUDIO_RECORDING) {
    char clock[12];
    int lvl = au->level();
    int mw = c.w - 60;
    int w = mw * (lvl < 0 ? 0 : lvl) / 100;
    mmss(api->ticks_ms() - M.started_ms, clock, sizeof clock);
    api->fill(rect(s.x, s.y, c.w, 4), CLR_BAR);
    api->fill(rect(s.x, s.y + 4, 4, 20), CLR_BAR);
    api->fill(rect(s.x + 4, s.y + 4, 8, 8), CLR_REC);
    api->fill(rect(s.x + 12, s.y + 4, 4, 8), CLR_BAR);
    api->text((short)(s.x + 16), (short)(s.y + 4), clock, CLR_BAR_FG, CLR_BAR);
    api->fill(rect(s.x + 16 + 6 * (short)api->str_len(clock), s.y + 4, 52 - 16 - 6 * (short)api->str_len(clock), 8), CLR_BAR);
    api->fill(rect(s.x + 52, s.y + 4, mw, 1), CLR_BAR);
    if (w > 0) api->fill(rect(s.x + 52, s.y + 5, w, 6), CLR_METER);
    if (w < mw) api->fill(rect(s.x + 52 + w, s.y + 5, mw - w, 6), CLR_BG);
    api->fill(rect(s.x + 52, s.y + 11, mw, 1), CLR_BAR);
    api->fill(rect(s.x + 52 + mw, s.y + 4, c.w - 52 - mw, 8), CLR_BAR);
    api->fill(rect(s.x + 4, s.y + 12, c.w - 4, 2), CLR_BAR);
    api->text((short)(s.x + 4), (short)(s.y + 14), "r stops", CLR_BAR_FG, CLR_BAR);
    api->fill(rect(s.x + 4 + 6 * 7, s.y + 14, c.w - 4 - 6 * 7, 10), CLR_BAR);
    api->fill(rect(s.x, s.y + 22, c.w, 2), CLR_BAR);
    return;
  }
  if (M.busy && st == CAPP_AUDIO_PLAYING) {
    char pos[12], tot[12], both[28];
    uint32_t p = au->pos_ms(), t = au->total_ms();
    int bw = c.w - 8;
    int w = t ? (int)((uint32_t)bw * p / t) : 0;
    mmss(p, pos, sizeof pos); mmss(t, tot, sizeof tot);
    api->fmt(both, sizeof both, "%s / %s", pos, tot);
    api->fill(rect(s.x, s.y, c.w, 4), CLR_BAR);
    api->fill(rect(s.x, s.y + 4, 4, 20), CLR_BAR);
    if (w > 0) api->fill(rect(s.x + 4, s.y + 4, w, 5), CLR_PLAY);
    if (w < bw) api->fill(rect(s.x + 4 + w, s.y + 4, bw - w, 5), CLR_BG);
    api->fill(rect(s.x + 4 + bw, s.y + 4, 4, 5), CLR_BAR);
    api->fill(rect(s.x + 4, s.y + 9, c.w - 4, 4), CLR_BAR);
    api->text((short)(s.x + 4), (short)(s.y + 13), both, CLR_BAR_FG, CLR_BAR);
    api->fill(rect(s.x + 4 + 6 * (short)api->str_len(both), s.y + 13, c.w - 8 - 6 * 11 - 6 * (short)api->str_len(both), 10), CLR_BAR);
    api->text((short)(s.x + c.w - 4 - 6 * 11), (short)(s.y + 13), "enter stops", CLR_BAR_FG, CLR_BAR);
    api->fill(rect(s.x + c.w - 4, s.y + 13, 4, 10), CLR_BAR);
    api->fill(rect(s.x, s.y + 21, c.w, 3), CLR_BAR);
    return;
  }
  api->fill(s, CLR_BAR);
  if (M.ask == ASK_DELETE) {
    char q[48];
    api->fmt(q, sizeof q, "delete %s?  y/n", M.n ? M.memo[M.sel].name : "");
    api->text((short)(s.x + 4), (short)(s.y + 8), q, CLR_REC, CLR_BAR);
    return;
  }
  {
    char vol[16];
    if (au->volume()) api->fmt(vol, sizeof vol, "vol %d%%", au->volume());
    else api->fmt(vol, sizeof vol, "%s", "muted");
    api->text((short)(s.x + 4), (short)(s.y + 3), M.status, CLR_BAR_FG, CLR_BAR);
    api->text((short)(s.x + c.w - 4 - 6 * (short)api->str_len(vol)), (short)(s.y + 3), vol, CLR_BAR_FG, CLR_BAR);
    api->text((short)(s.x + 4), (short)(s.y + 13), "r rec  enter play  d del  < > vol", CLR_DIM, CLR_BAR);
  }
}

static void app_paint(void *st, CRect c) {
  (void)st;
  if (toolbar_only_menu()) { toolbar_paint_menu(c); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(c); return; }
  toolbar_paint_bar(c);
  {
    CRect r = toolbar_rest(c);
    M.content = r;
    /* The strip owns its own 24 rows: clearing them here and again in
     * paint_strip is the flicker a moving meter would show. */
    api->fill(rect(r.x, r.y, r.w, r.h - 24), CLR_BG);
    paint_rows(r, r.h - 24);
    paint_strip(r);
  }
  toolbar_paint_menu(c);
}

/* ---- input --------------------------------------------------------------- */

enum { ACT_RECORD = 1, ACT_PLAY, ACT_DELETE, ACT_REFRESH, ACT_LOUDER, ACT_QUIETER };

static const CappAction ACTIONS[] = {
  { "record",  "Record / stop", "Memo", 0x12, ACT_RECORD },   /* ctrl-r */
  { "play",    "Play / stop",   "Memo", 0x10, ACT_PLAY },     /* ctrl-p */
  { "delete",  "Delete",        "Memo", 0x04, ACT_DELETE },   /* ctrl-d */
  { "refresh", "Refresh",       "Memo", 0x0C, ACT_REFRESH },  /* ctrl-l */
  { "louder",  "Louder",        "Volume", 0, ACT_LOUDER },
  { "quieter", "Quieter",       "Volume", 0, ACT_QUIETER },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))
static const TbIcon ICONS[] = { { "o", ACT_RECORD }, { ">", ACT_PLAY } };

static int do_action(int a) {
  char v[24];
  switch (a) {
  case ACT_RECORD:  record(); return 1;
  case ACT_PLAY:    play(); return 1;
  case ACT_DELETE:
    if (M.n && au->state() == CAPP_AUDIO_IDLE) M.ask = ASK_DELETE;
    return 1;
  case ACT_REFRESH: rescan(); return 1;
  case ACT_LOUDER:
  case ACT_QUIETER:
    au->set_volume(au->volume() + (a == ACT_LOUDER ? 10 : -10));
    api->fmt(v, sizeof v, "volume %d%%", au->volume());
    say(v);
    return 1;
  default: return 0;
  }
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int app_key(void *st, unsigned char k) {
  (void)st;
  {
    int a = toolbar_key(k);
    if (a == TB_CONSUMED) return 1;
    if (a != TB_NONE) return do_action(a);
  }
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 0;

  if (M.ask == ASK_DELETE) {
    if (k == 'y' || k == 'Y' || k == CAPP_KEY_ENTER) { M.ask = ASK_NONE; delete_memo(); }
    else if (k == 'n' || k == 'N' || k == CAPP_KEY_ESC) M.ask = ASK_NONE;
    return 1;
  }
  switch (k) {
  case CAPP_KEY_UP:
    if (M.sel > 0) M.sel--;
    if (M.sel < M.top) M.top = M.sel;
    return 1;
  case CAPP_KEY_DOWN:
    if (M.sel + 1 < M.n) M.sel++;
    if (M.rows && M.sel >= M.top + M.rows) M.top = M.sel - M.rows + 1;
    return 1;
  case 'r': case 'R':  return do_action(ACT_RECORD);
  case ' ':
  case CAPP_KEY_ENTER: return do_action(ACT_PLAY);
  case 'd': case 'D':
  case 0x7F:           return do_action(ACT_DELETE);
  case '+': case '=':
  case CAPP_KEY_RIGHT: return do_action(ACT_LOUDER);
  case '-':
  case CAPP_KEY_LEFT:  return do_action(ACT_QUIETER);
  case CAPP_KEY_ESC:
    /* Escape stops whatever is running; with nothing running it declines,
     * which is the top level saying it has nowhere to go back to. */
    if (au->state() != CAPP_AUDIO_IDLE) { au->stop(); return 1; }
    return 0;
  default: return 0;
  }
}

static int app_click(void *st, short x, short y, int button) {
  int row;
  (void)st; (void)button;
  {
    int a = toolbar_click(x, y);
    if (a == TB_CONSUMED) return 1;
    if (a != TB_NONE) return do_action(a);
  }
  y = (short)(y - toolbar_h());
  row = y / ROW_H;
  if (row < 0 || M.top + row >= M.n || y >= M.content.h - 24) return 0;
  if (M.top + row == M.sel) return do_action(ACT_PLAY);
  M.sel = M.top + row;
  return 1;
}

static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  int changed;
  (void)st; (void)buttons;
  changed = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) changed = 1;
  if (wheel) {
    M.sel -= wheel;
    if (M.sel < 0) M.sel = 0;
    if (M.sel >= M.n) M.sel = M.n ? M.n - 1 : 0;
    if (M.sel < M.top) M.top = M.sel;
    if (M.rows && M.sel >= M.top + M.rows) M.top = M.sel - M.rows + 1;
    changed = 1;
  }
  return changed;
}

/* The meter and the clock move on their own, so while a job runs every
 * tick repaints the strip; when it ends, the list is read again so the new
 * memo appears with its length. */
static int app_tick(void *st, uint32_t now_ms) {
  int state = au->state();
  (void)st;
  if (M.busy) {
    if (state == CAPP_AUDIO_IDLE) {
      M.busy = 0;
      if (M.was_state == CAPP_AUDIO_RECORDING) {
        int bytes = au->last_bytes();
        rescan();
        if (bytes > 0) {
          char len[12], line[40];
          int i;
          const char *slash = M.recording, *p;
          for (p = M.recording; *p; p++) if (*p == '/') slash = p + 1;
          for (i = 0; i < M.n; i++) if (!name_cmp(M.memo[i].name, slash)) { M.sel = i; M.top = 0; break; }
          mmss((uint32_t)bytes / (BYTES_PER_SEC / 1000), len, sizeof len);
          api->fmt(line, sizeof line, "saved, %s", len);
          say(line);
        } else {
          say(au->error()[0] ? au->error() : "nothing recorded");
        }
      } else {
        say(au->error()[0] ? au->error() : "done");
      }
      M.was_state = CAPP_AUDIO_IDLE;
      return 1;
    }
    M.was_state = state;
    /* Only when the strip would look different, and no more than fifteen
     * times a second: the meter is 60 ms of sound per reading anyway. */
    {
      uint32_t sig = state == CAPP_AUDIO_RECORDING
                   ? ((uint32_t)au->level() << 16) | ((now_ms - M.started_ms) / 1000)
                   : au->pos_ms() / 200;
      if (sig == M.strip_sig || now_ms - M.strip_at < 66) return 0;
      M.strip_sig = sig;
      M.strip_at = now_ms;
    }
    api->damage(rect(M.content.x, M.content.y + M.content.h - 24, M.content.w, 24));
    return 1;
  }
  return 0;
}

static int app_wants_text(void *st) { (void)st; return 0; }

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Memo",
  /* 16x16: a microphone. */
  { 0x03, 0xC0, 0x07, 0xE0, 0x07, 0xE0, 0x07, 0xE0,
    0x07, 0xE0, 0x07, 0xE0, 0x27, 0xE4, 0x27, 0xE4,
    0x33, 0xCC, 0x1C, 0x38, 0x0F, 0xF0, 0x01, 0x80,
    0x01, 0x80, 0x01, 0x80, 0x07, 0xE0, 0x00, 0x00 },
  "arrows\tmove\nr\trecord, and stop\nenter\tplay, and stop\nd\tdelete\n"
  "left/right\tvolume (also + -; opt-8 / opt-7 anywhere)\nescape\tstop\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  au = api->audio();
  (void)argc; (void)argv;
  api->mem_set(&M, 0, sizeof M);
  toolbar_init(api, ACTIONS, NACT, ICONS, 2);
  rescan();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
