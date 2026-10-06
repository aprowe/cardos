/* MIDI: a sequencer for a Grove-to-MIDI converter, its songs written by
 * Claude or by hand.
 *
 * A song is a text file in /songs (NAME.song), in the format apps/midiseq.h
 * reads: notes at beats with lengths and velocities, chords, controller
 * changes and ramps, pitch bends, a tempo, a loop. `n` asks Claude for one
 * ("write the first section of Fur Elise") through the server
 * (server/midi.py), which checks it before it comes back; `e` opens one in
 * Edit, and leaving Edit comes back here.
 *
 * Playing is the kernel's (api->midi, API 38): the whole song goes over as
 * time-stamped messages and a task sends them on time, so a repaint here
 * cannot make a note late. While it plays, a piano roll: each channel its
 * own colour, a line where the song is.
 *
 * The converter listens on G1 or G2 depending on which it is; `g` swaps,
 * and the choice is kept in /config/midi.txt. `t` plays an arpeggio, to
 * find out which.
 */
#define MS_MAX_EV     768
#define MS_MAX_NOTES  384
#include "kernel/app/capp.h"
#include "apps/midiseq.h"
#include "apps/footer.h"
#include "apps/safefile.h"

static const CardApi *api;
static const CappMidi *mi;

#define DIR        "/songs"
#define CONF       "/config/midi.txt"
#define MAXS       32
#define NAMEL      40
#define TEXT_MAX   8192
#define TOP_H      14
#define ROW_H      12

#define CLR_BG     CAPP_RGB(15, 17, 23)
#define CLR_TEXT   CAPP_RGB(232, 236, 244)
#define CLR_DIM    CAPP_RGB(128, 136, 152)
#define CLR_SEL    CAPP_RGB(36, 44, 62)
#define CLR_ACC    CAPP_RGB(160, 140, 255)
#define CLR_BAD    CAPP_RGB(240, 110, 96)
#define CLR_ROLL   CAPP_RGB(22, 26, 36)
#define CLR_GRID   CAPP_RGB(34, 40, 54)
#define CLR_HEAD   CAPP_RGB(255, 255, 255)

static const uint16_t CHAN[8] = {
  CAPP_RGB(120, 180, 255), CAPP_RGB(255, 150, 110), CAPP_RGB(130, 220, 140), CAPP_RGB(250, 210, 90),
  CAPP_RGB(210, 140, 250), CAPP_RGB(100, 220, 220), CAPP_RGB(255, 120, 180), CAPP_RGB(200, 200, 200),
};

enum { V_LIST, V_PLAY, V_ASK };

static struct {
  char     file[MAXS][NAMEL];
  char     title[MAXS][NAMEL];
  int      n, sel, top;
  int      view;
  int      pin;                    /* 1 or 2 */
  int      loop_on;                /* -1 the song's own, 0 off, 1 on */
  int      tempo_add;
  int      ask_delete;
  char     status[48];
  int      bad;
  /* asking Claude */
  char     draft[120];
  char     job[16];
  uint32_t next_poll, asked_at;
  /* the song playing */
  MsSong   S;
  uint32_t len_ms;
  int      lo, hi;                 /* pitch range on the roll */
  int      head_x;                 /* the playhead as drawn */
  CRect    c;
  int      have_c;
  char     text[TEXT_MAX];
} M;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)(w > 0 ? w : 0); r.h = (int16_t)(h > 0 ? h : 0);
  return r;
}

static void say(int bad, const char *s) { api->fmt(M.status, sizeof M.status, "%s", s); M.bad = bad; }
static void mark_all(void) { if (M.have_c) api->damage(M.c); }

static int ends_song(const char *s) {
  int n = (int)api->str_len(s);
  return n > 5 && s[n - 5] == '.' && s[n - 4] == 's' && s[n - 3] == 'o' && s[n - 2] == 'n' && s[n - 1] == 'g';
}

/* ---- files ----------------------------------------------------------------------- */

static int read_text(const char *path) {
  int fd = safe_open_read(api, path), n = 0, r;
  M.text[0] = 0;
  if (fd < 0) return -1;
  while (n < TEXT_MAX - 1 && (r = api->read(fd, M.text + n, (size_t)(TEXT_MAX - 1 - n))) > 0) n += r;
  api->close(fd);
  M.text[n] = 0;
  return n;
}

static void title_of(const char *path, char *out) {
  int fd = api->open(path, CAPP_O_READ), n, i = 0, k = 0;
  char buf[64];
  out[0] = 0;
  if (fd < 0) return;
  n = api->read(fd, buf, sizeof buf - 1);
  api->close(fd);
  if (n <= 0) return;
  buf[n] = 0;
  if (buf[0] != '#') return;
  i = 1;
  while (buf[i] == ' ') i++;
  while (buf[i] && buf[i] != '\n' && buf[i] != '\r' && k < NAMEL - 1) out[k++] = buf[i++];
  out[k] = 0;
}

static void rescan(void) {
  static char raw[MAXS][NAMEL];
  int n = api->list(DIR, &raw[0][0], MAXS, NAMEL), i;
  M.n = 0;
  if (n < 0) { api->mkdir(DIR); n = 0; }
  for (i = 0; i < n && M.n < MAXS; i++) {
    char path[64];
    if (!ends_song(raw[i])) continue;
    api->fmt(M.file[M.n], NAMEL, "%s", raw[i]);
    api->fmt(path, sizeof path, DIR "/%s", raw[i]);
    title_of(path, M.title[M.n]);
    if (!M.title[M.n][0]) api->fmt(M.title[M.n], NAMEL, "%s", raw[i]);
    M.n++;
  }
  if (M.sel >= M.n) M.sel = M.n ? M.n - 1 : 0;
}

static void pin_load(void) {
  char b[4];
  int fd = api->open(CONF, CAPP_O_READ), n;
  M.pin = 2;
  if (fd < 0) return;
  n = api->read(fd, b, sizeof b);
  api->close(fd);
  if (n > 0 && b[0] == '1') M.pin = 1;
}

static void pin_save(void) {
  SafeFile f;
  if (safe_begin(&f, api, CONF) != 0) return;
  safe_write(&f, M.pin == 1 ? "1\n" : "2\n", 2);
  safe_commit(&f);
}

/* ---- playing ----------------------------------------------------------------------- */

/* The song's events as the kernel takes them, written over the parsed ones:
 * a CappMidiEvent is smaller than an MsEvent, so event i is written to bytes
 * the parse has already been read past. Saves a second array of them. */
static int to_kernel(void) {
  CappMidiEvent *out = (CappMidiEvent *)(void *)M.S.ev;
  int i;
  for (i = 0; i < M.S.nev; i++) {
    uint32_t at = ms_ms(&M.S, M.S.ev[i].tick);
    uint8_t n = M.S.ev[i].n, b0 = M.S.ev[i].b[0], b1 = M.S.ev[i].b[1], b2 = M.S.ev[i].b[2];
    out[i].at_ms = at; out[i].n = n; out[i].b[0] = b0; out[i].b[1] = b1; out[i].b[2] = b2;
  }
  return M.S.nev;
}

static void play_sel(void) {
  char path[64];
  int loop, n, i, rc;
  if (!M.n || !mi) return;
  api->fmt(path, sizeof path, DIR "/%s", M.file[M.sel]);
  if (read_text(path) < 0) { say(1, "cannot read it"); return; }
  if (ms_parse(&M.S, M.text) != 0) {
    api->fmt(M.status, sizeof M.status, "line %d: %s", M.S.err_line, M.S.err);
    M.bad = 1;
    return;
  }
  M.S.tempo += M.tempo_add;
  if (M.S.tempo < 20) M.S.tempo = 20;
  loop = M.loop_on < 0 ? M.S.loop : M.loop_on;
  if (loop && !M.S.loop_ticks) M.S.loop_ticks = (M.S.end_ticks + 4 * MS_PPQ - 1) / (4 * MS_PPQ) * (4 * MS_PPQ);
  M.len_ms = ms_ms(&M.S, loop ? M.S.loop_ticks : M.S.end_ticks);
  M.lo = 127; M.hi = 0;
  for (i = 0; i < M.S.nnotes; i++) {
    if (M.S.note[i].pitch < M.lo) M.lo = M.S.note[i].pitch;
    if (M.S.note[i].pitch > M.hi) M.hi = M.S.note[i].pitch;
  }
  if (M.lo > M.hi) { M.lo = 48; M.hi = 72; }
  if (mi->open(M.pin == 1 ? 1 : 2) != 0) { say(1, "the Grove port would not open"); return; }
  n = to_kernel();
  rc = mi->play((const CappMidiEvent *)(void *)M.S.ev, n, loop ? M.len_ms : 0);
  if (rc != 0) { say(1, rc == -2 ? "not enough memory to play it" : "MIDI is not open"); return; }
  M.view = V_PLAY;
  M.head_x = -1;
  api->fmt(M.status, sizeof M.status, "%d bpm%s  G%d", M.S.tempo, loop ? "  loop" : "", M.pin);
  M.bad = 0;
}

static void stop(void) {
  if (mi) mi->stop();
  M.view = V_LIST;
  say(0, "");
}

/* A C-major arpeggio, now: to hear whether the converter is on this pin. */
static void test_tone(void) {
  static const int8_t NOTES[4] = { 60, 64, 67, 72 };
  CappMidiEvent ev[8];
  int i;
  if (!mi || mi->open(M.pin == 1 ? 1 : 2) != 0) { say(1, "the Grove port would not open"); return; }
  for (i = 0; i < 4; i++) {
    ev[2 * i].at_ms = (uint32_t)i * 250; ev[2 * i].n = 3;
    ev[2 * i].b[0] = 0x90; ev[2 * i].b[1] = (uint8_t)NOTES[i]; ev[2 * i].b[2] = 100;
    ev[2 * i + 1].at_ms = (uint32_t)i * 250 + 230; ev[2 * i + 1].n = 3;
    ev[2 * i + 1].b[0] = 0x80; ev[2 * i + 1].b[1] = (uint8_t)NOTES[i]; ev[2 * i + 1].b[2] = 0;
  }
  mi->play(ev, 8, 0);
  api->fmt(M.status, sizeof M.status, "a test arpeggio on G%d", M.pin);
  M.bad = 0;
}

/* ---- asking Claude ---------------------------------------------------------------------- */

static void ask_send(void) {
  char url[128], reply[24];
  int r, i;
  if (!M.draft[0]) { M.view = V_LIST; return; }
  if (!api->net_ready() && api->net_connect(15000) != 0) { say(1, "offline: Claude needs the server"); M.view = V_LIST; return; }
  api->fmt(url, sizeof url, "%s/midi/compose", api->proxy());
  r = api->http("POST", url, M.draft, "text/plain", "", reply, sizeof reply, 15000);
  if (r < 0 || reply[0] == 'e') { say(1, "the server did not take it"); M.view = V_LIST; return; }
  for (i = 0; reply[i] && reply[i] != '\n' && i < (int)sizeof M.job - 1; i++) M.job[i] = reply[i];
  M.job[i] = 0;
  M.asked_at = api->ticks_ms();
  M.next_poll = M.asked_at + 3000;
  say(0, "Claude is writing it...");
}

/* A file name from the title: letters and digits, the rest as -. */
static void name_from(const char *title, char *out, int n) {
  int i = 0, k = 0, dash = 0;
  for (; title[i] && k < n - 7; i++) {
    char c = title[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { out[k++] = c; dash = 0; }
    else if (c >= 'A' && c <= 'Z') { out[k++] = (char)(c + 32); dash = 0; }
    else if (k && !dash) { out[k++] = '-'; dash = 1; }
  }
  while (k && out[k - 1] == '-') k--;
  if (!k) { out[0] = 's'; out[1] = 'o'; out[2] = 'n'; out[3] = 'g'; k = 4; }
  out[k] = 0;
}

static void ask_poll(void) {
  char url[128], name[NAMEL - 6], file[NAMEL], path[64];
  SafeFile f;
  int r, i;
  api->fmt(url, sizeof url, "%s/midi/compose?id=%s", api->proxy(), M.job);
  r = api->http("GET", url, 0, 0, "", M.text, TEXT_MAX, 15000);
  if (r < 0) { M.next_poll = api->ticks_ms() + 4000; return; }
  if (M.text[0] == 'p') {                       /* pending */
    M.next_poll = api->ticks_ms() + 3000;
    api->fmt(M.status, sizeof M.status, "Claude is writing it... %lus",
             (unsigned long)((api->ticks_ms() - M.asked_at) / 1000));
    if (M.have_c) api->damage(rect(M.c.x, M.c.y, M.c.w, TOP_H));
    return;
  }
  M.job[0] = 0;
  if (!(M.text[0] == 'o' && M.text[1] == 'k' && M.text[2] == '\n')) {
    for (i = 0; M.text[i] && M.text[i] != '\n'; i++) ;
    M.text[i] = 0;
    say(1, M.text[0] == 'e' ? M.text + 6 : "Claude did not write one");
    mark_all();
    return;
  }
  /* "ok\n" and the song: its title names the file. */
  {
    char title[NAMEL];
    const char *song = M.text + 3;
    i = 0;
    title[0] = 0;
    if (song[0] == '#') {
      int k = 0;
      i = 1;
      while (song[i] == ' ') i++;
      while (song[i] && song[i] != '\n' && k < NAMEL - 1) title[k++] = song[i++];
      title[k] = 0;
    }
    name_from(title[0] ? title : M.draft, name, sizeof name);
    api->fmt(file, sizeof file, "%s.song", name);
    api->fmt(path, sizeof path, DIR "/%s", file);
    api->mkdir(DIR);
    if (safe_begin(&f, api, path) != 0) { say(1, "cannot write to the card"); return; }
    safe_write(&f, song, api->str_len(song));
    if (safe_commit(&f) != 0) { say(1, "cannot write to the card"); return; }
    rescan();
    for (i = 0; i < M.n; i++) if (!(M.file[i][0] - file[0]) && api->str_len(M.file[i]) == api->str_len(file)) {
      int k;
      for (k = 0; file[k] && file[k] == M.file[i][k]; k++) ;
      if (!file[k]) M.sel = i;
    }
    say(0, "written: enter plays, e edits");
  }
  mark_all();
}

static void delete_sel(void) {
  char path[64];
  if (!M.n) return;
  api->fmt(path, sizeof path, DIR "/%s", M.file[M.sel]);
  api->remove(path);
  rescan();
  say(0, "deleted");
}

/* ---- the screen ----------------------------------------------------------------------------- */

static void paint_top(void) {
  CRect r = rect(M.c.x, M.c.y, M.c.w, TOP_H);
  int w = (int)api->str_len(M.status) * 6;
  api->text((int16_t)(r.x + 6), (int16_t)(r.y + 3), M.view == V_PLAY ? M.S.title[0] ? M.S.title : "MIDI" : "MIDI",
            CLR_ACC, CLR_BG);
  api->fill(rect(r.x, r.y, r.w, 3), CLR_BG);
  api->fill(rect(r.x, r.y + 11, r.w, 3), CLR_BG);
  api->fill(rect(r.x, r.y + 3, 6, 8), CLR_BG);
  api->fill(rect(r.x + 6 + 6 * (M.view == V_PLAY && M.S.title[0] ? (int)api->str_len(M.S.title) : 4), r.y + 3,
                 r.w - 12 - w - 6 * (M.view == V_PLAY && M.S.title[0] ? (int)api->str_len(M.S.title) : 4), 8), CLR_BG);
  api->text((int16_t)(r.x + r.w - 6 - w), (int16_t)(r.y + 3), M.status, M.bad ? CLR_BAD : CLR_DIM, CLR_BG);
  api->fill(rect(r.x + r.w - 6, r.y + 3, 6, 8), CLR_BG);
}

static void paint_list(void) {
  int rows = (M.c.h - TOP_H - FOOT_H) / ROW_H, i, y = M.c.y + TOP_H;
  if (M.sel < M.top) M.top = M.sel;
  if (M.sel >= M.top + rows) M.top = M.sel - rows + 1;
  for (i = M.top; i < M.n && i - M.top < rows; i++, y += ROW_H) {
    char line[48];
    uint16_t bg = i == M.sel ? CLR_SEL : CLR_BG;
    api->fmt(line, sizeof line, " %-37.37s", M.title[i]);
    api->text((int16_t)M.c.x, (int16_t)(y + 2), line, CLR_TEXT, bg);
    api->fill(rect(M.c.x, y, M.c.w, 2), bg);
    api->fill(rect(M.c.x, y + 10, M.c.w, ROW_H - 10), bg);
    api->fill(rect(M.c.x + 38 * 6, y + 2, M.c.w - 38 * 6, 8), bg);
  }
  if (!M.n) {
    api->text((int16_t)(M.c.x + 8), (int16_t)(y + 4), "no songs: n asks Claude for one", CLR_DIM, CLR_BG);
    api->fill(rect(M.c.x, y, M.c.w, 4), CLR_BG);
    y += 12;
  }
  api->fill(rect(M.c.x, y, M.c.w, M.c.y + M.c.h - FOOT_H - y), CLR_BG);
}

static CRect roll_rect(void) { return rect(M.c.x + 4, M.c.y + TOP_H + 2, M.c.w - 8, M.c.h - TOP_H - FOOT_H - 4); }

static int roll_x(uint32_t ms) {
  CRect r = roll_rect();
  return M.len_ms ? r.x + (int)(ms / 4 * (uint32_t)r.w / (M.len_ms / 4 + 1)) : r.x;
}

/* The roll within the clip: background, notes that reach it, the line. */
static void paint_roll(void) {
  CRect r = roll_rect(), a = api->paint_area();
  int span = M.hi - M.lo + 1, rowh = r.h / (span ? span : 1), i;
  if (rowh < 1) rowh = 1;
  if (rowh > 6) rowh = 6;
  api->fill(r, CLR_ROLL);
  for (i = 0; i < M.S.nnotes; i++) {
    const MsNote *nt = &M.S.note[i];
    int x0 = roll_x(ms_ms(&M.S, nt->start)), x1 = roll_x(ms_ms(&M.S, nt->start + nt->len));
    int y = r.y + r.h - (nt->pitch - M.lo + 1) * r.h / (span ? span : 1);
    if (x1 < a.x || x0 > a.x + a.w) continue;
    if (x1 <= x0) x1 = x0 + 1;
    api->fill(rect(x0, y, x1 - x0 - (x1 - x0 > 2), rowh), CHAN[nt->ch & 7]);
  }
  if (M.head_x >= r.x) api->fill(rect(M.head_x, r.y, 1, r.h), CLR_HEAD);
}

static void app_paint(void *st, CRect c) {
  CRect a = api->paint_area();
  (void)st;
  M.c = c;
  M.have_c = 1;
  /* The playhead moving: its two columns and nothing else. */
  if (M.view == V_PLAY && a.w <= 4 && a.y >= roll_rect().y) { paint_roll(); return; }
  paint_top();
  if (M.view == V_PLAY) {
    api->fill(rect(c.x, c.y + TOP_H, c.w, 2), CLR_BG);
    api->fill(rect(c.x, c.y + TOP_H + 2, 4, roll_rect().h), CLR_BG);
    api->fill(rect(c.x + c.w - 4, c.y + TOP_H + 2, 4, roll_rect().h), CLR_BG);
    api->fill(rect(c.x, c.y + c.h - FOOT_H - 2, c.w, 2), CLR_BG);
    paint_roll();
    footer_paint(api, c, "enter stop  + - tempo  l loop");
  } else {
    paint_list();
    if (M.view == V_ASK) {
      char line[48];
      int n = (int)api->str_len(M.draft);
      api->fmt(line, sizeof line, "write: %s_%-38s", n > 30 ? M.draft + n - 30 : M.draft, "");
      line[38] = 0;
      footer_paint(api, c, line);
    } else if (M.ask_delete) {
      char line[48];
      api->fmt(line, sizeof line, "delete %.22s? y yes  n no", M.title[M.sel]);
      footer_paint(api, c, line);
    } else footer_paint(api, c, "enter play  n new  e edit  g pin");
  }
}

/* ---- keys and time ------------------------------------------------------------------------- */

static int key_ask(uint8_t k) {
  int n = (int)api->str_len(M.draft);
  if (k == CAPP_KEY_ESC) { M.view = V_LIST; return 1; }
  if (k == CAPP_KEY_ENTER) { M.view = V_LIST; ask_send(); return 1; }
  if (k == CAPP_KEY_BACK) { if (n) M.draft[n - 1] = 0; return 1; }
  if (k >= 32 && k < 127 && n < (int)sizeof M.draft - 1) { M.draft[n] = (char)k; M.draft[n + 1] = 0; }
  return 1;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (M.view == V_ASK) { key_ask(k); mark_all(); return 1; }
  if (M.ask_delete) {
    M.ask_delete = 0;
    if (k == 'y' || k == 'Y') delete_sel();
    mark_all();
    return 1;
  }
  if (M.view == V_PLAY) {
    switch (k) {
    case CAPP_KEY_ENTER: case ' ': case CAPP_KEY_ESC: stop(); break;
    case '+': case '=': M.tempo_add += 4; play_sel(); break;
    case '-': case '_': M.tempo_add -= 4; play_sel(); break;
    case 'l': case 'L': M.loop_on = M.S.loop_ticks && (M.loop_on < 0 ? M.S.loop : M.loop_on) ? 0 : 1; play_sel(); break;
    default: return 0;
    }
    mark_all();
    return 1;
  }
  switch (k) {
  case CAPP_KEY_UP:    if (M.sel > 0) M.sel--; M.tempo_add = 0; M.loop_on = -1; break;
  case CAPP_KEY_DOWN:  if (M.sel < M.n - 1) M.sel++; M.tempo_add = 0; M.loop_on = -1; break;
  case CAPP_KEY_ENTER: case ' ': play_sel(); break;
  case 'n': case 'N':  if (M.job[0]) { say(0, "Claude is still writing the last one"); break; }
                       M.draft[0] = 0; M.view = V_ASK; break;
  case 'e': case 'E':
    if (M.n) {
      char path[64];
      api->fmt(path, sizeof path, DIR "/%s", M.file[M.sel]);
      api->run("edit", path);
    }
    break;
  case 'd': case 'D': case 0x7F: if (M.n) M.ask_delete = 1; break;
  case 'g': case 'G':  M.pin = M.pin == 1 ? 2 : 1; pin_save(); if (mi) mi->close();
                       api->fmt(M.status, sizeof M.status, "MIDI out on G%d", M.pin); M.bad = 0; break;
  case 't': case 'T':  test_tone(); break;
  case 'r': case 'R':  rescan(); say(0, ""); break;
  default: return 0;
  }
  mark_all();
  return 1;
}

static int app_wants_text(void *st) { (void)st; return M.view == V_ASK; }

static int app_tick(void *st, uint32_t now) {
  (void)st;
  if (M.job[0] && (int32_t)(now - M.next_poll) >= 0) { ask_poll(); return 1; }
  if (M.view == V_PLAY && mi) {
    int x;
    if (!mi->playing()) { M.view = V_LIST; say(0, ""); mark_all(); return 1; }
    x = roll_x(mi->pos_ms() % (M.len_ms ? M.len_ms : 1));
    if (x != M.head_x && M.have_c) {
      CRect r = roll_rect();
      if (M.head_x >= r.x) api->damage(rect(M.head_x, r.y, 1, r.h));
      M.head_x = x;
      api->damage(rect(x, r.y, 1, r.h));
      return 1;
    }
  }
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "MIDI",
  /* 16x16: piano keys. */
  { 0x00, 0x00, 0xFF, 0xFF, 0x92, 0x49, 0x92, 0x49,
    0x92, 0x49, 0x92, 0x49, 0x92, 0x49, 0xB6, 0xDB,
    0xB6, 0xDB, 0xB6, 0xDB, 0x92, 0x49, 0x92, 0x49,
    0x92, 0x49, 0x92, 0x49, 0xFF, 0xFF, 0x00, 0x00 },
  "up/down\tchoose a song\nenter, space\tplay it; again to stop\nn\task Claude to write one\n"
  "e\tedit it (in Edit)\nd, del\tdelete it (asks)\n+ -\ttempo, while it plays\nl\tloop on and off, while it plays\n"
  "g\tMIDI out on G1 or G2 (whichever your converter uses)\nt\ta test arpeggio\nr\tread the folder again\n"
  "\nsongs are text in /songs; see apps/midiseq.h.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  mi = api->midi();
  M.loop_on = -1;
  pin_load();
  api->mkdir(DIR);
  rescan();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
