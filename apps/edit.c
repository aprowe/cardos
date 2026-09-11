/* A small text editor, as a loadable CardOS app.
 *
 * The buffer is a flat array of lines rather than a gap buffer: files this
 * thing is for are a few kilobytes, and a flat array is the version that is
 * obviously correct at a glance.
 */

#include "kernel/app/capp.h"

#define MAXLINES 64
#define MAXCOL   40
#define ROWH     10
#define VISROWS  9

static const CardApi *api;

static struct {
  char line[MAXLINES][MAXCOL + 1];
  int  len[MAXLINES];
  int  nlines;
  int  cx, cy;      /* cursor, in buffer coordinates */
  int  top;         /* first visible line */
  int  dirty;
  char path[64];
  char status[40];
} E;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *m) { api->fmt(E.status, sizeof E.status, "%s", m); }

static void blank(void) {
  api->mem_set(&E, 0, sizeof E);
  E.nlines = 1;
  api->fmt(E.path, sizeof E.path, "%s", "/untitled.txt");
  say("^s save  ` exit");
}

static void load(const char *path) {
  char buf[512];
  int fd, n, i;
  int col = 0;

  blank();
  api->fmt(E.path, sizeof E.path, "%s", path);

  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) { say("new file"); return; }

  E.nlines = 1;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c == 13) continue;
      if (c == 10) {
        E.len[E.nlines - 1] = col;
        col = 0;
        if (E.nlines >= MAXLINES) { say("truncated"); goto out; }
        E.nlines++;
        continue;
      }
      if (col < MAXCOL) E.line[E.nlines - 1][col++] = c;
    }
  }
  E.len[E.nlines - 1] = col;
out:
  api->close(fd);
}

static void save(void) {
  int fd, i;
  char nl = 10;
  fd = api->open(E.path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) { say("cannot write"); return; }
  for (i = 0; i < E.nlines; i++) {
    if (E.len[i]) api->write(fd, E.line[i], (size_t)E.len[i]);
    if (i + 1 < E.nlines) api->write(fd, &nl, 1);
  }
  api->close(fd);
  E.dirty = 0;
  say("saved");
}

static void scroll_to_cursor(void) {
  if (E.cy < E.top) E.top = E.cy;
  if (E.cy >= E.top + VISROWS) E.top = E.cy - VISROWS + 1;
  if (E.top < 0) E.top = 0;
}

static void insert_char(char c) {
  int i, n = E.len[E.cy];
  if (n >= MAXCOL) return;
  for (i = n; i > E.cx; i--) E.line[E.cy][i] = E.line[E.cy][i - 1];
  E.line[E.cy][E.cx] = c;
  E.len[E.cy] = n + 1;
  E.cx++;
  E.dirty = 1;
}

static void split_line(void) {
  int i, tail;
  if (E.nlines >= MAXLINES) return;
  for (i = E.nlines; i > E.cy + 1; i--) {
    api->mem_cpy(E.line[i], E.line[i - 1], MAXCOL + 1);
    E.len[i] = E.len[i - 1];
  }
  E.nlines++;
  tail = E.len[E.cy] - E.cx;
  api->mem_cpy(E.line[E.cy + 1], E.line[E.cy] + E.cx, (size_t)tail);
  E.len[E.cy + 1] = tail;
  E.len[E.cy] = E.cx;
  E.cy++;
  E.cx = 0;
  E.dirty = 1;
}

static void join_prev(void) {
  int i, prev;
  if (E.cy == 0) return;
  prev = E.len[E.cy - 1];
  if (prev + E.len[E.cy] > MAXCOL) return;
  api->mem_cpy(E.line[E.cy - 1] + prev, E.line[E.cy], (size_t)E.len[E.cy]);
  E.len[E.cy - 1] = prev + E.len[E.cy];
  for (i = E.cy; i + 1 < E.nlines; i++) {
    api->mem_cpy(E.line[i], E.line[i + 1], MAXCOL + 1);
    E.len[i] = E.len[i + 1];
  }
  E.nlines--;
  E.cy--;
  E.cx = prev;
  E.dirty = 1;
}

static void backspace(void) {
  int i;
  if (E.cx == 0) { join_prev(); return; }
  for (i = E.cx - 1; i + 1 < E.len[E.cy]; i++) E.line[E.cy][i] = E.line[E.cy][i + 1];
  E.len[E.cy]--;
  E.cx--;
  E.dirty = 1;
}

static void app_open(void *st) {
  (void)st;
  if (E.nlines == 0) blank();
}

static void app_paint(void *st, CRect c) {
  int r;
  char buf[64];
  (void)st;

  scroll_to_cursor();
  api->fill(c, CAPP_WHITE);

  for (r = 0; r < VISROWS; r++) {
    int i = E.top + r;
    int y = c.y + r * ROWH;
    if (y + ROWH > c.y + c.h - ROWH) break;
    if (i >= E.nlines) break;
    api->mem_cpy(buf, E.line[i], (size_t)E.len[i]);
    buf[E.len[i]] = 0;
    api->text(c.x, (short)y, buf, CAPP_BLACK, CAPP_WHITE);
    if (i == E.cy)
      api->fill(rect(c.x + E.cx * 6, y, 1, 8), CAPP_RED);
  }

  api->fmt(buf, sizeof buf, "%s%s  %d:%d  %s",
           E.dirty ? "*" : " ", E.path, E.cy + 1, E.cx + 1, E.status);
  api->text(c.x, (short)(c.y + c.h - 8), buf, CAPP_WHITE, CAPP_NAVY);
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  switch (k) {
  case CAPP_KEY_LEFT:
    if (E.cx > 0) E.cx--;
    else if (E.cy > 0) { E.cy--; E.cx = E.len[E.cy]; }
    return 1;
  case CAPP_KEY_RIGHT:
    if (E.cx < E.len[E.cy]) E.cx++;
    else if (E.cy + 1 < E.nlines) { E.cy++; E.cx = 0; }
    return 1;
  case CAPP_KEY_UP:
    if (E.cy > 0) { E.cy--; if (E.cx > E.len[E.cy]) E.cx = E.len[E.cy]; }
    return 1;
  case CAPP_KEY_DOWN:
    if (E.cy + 1 < E.nlines) { E.cy++; if (E.cx > E.len[E.cy]) E.cx = E.len[E.cy]; }
    return 1;
  case CAPP_KEY_BACK: backspace(); return 1;
  case CAPP_KEY_ENTER: split_line(); return 1;
  case 19: save(); return 1;            /* ctrl-s */
  default:
    if (k >= 32 && k < 127) { insert_char((char)k); return 1; }
    return 0;
  }
}

static int app_click(void *st, short x, short y, int button) {
  int r = y / ROWH;
  int i = E.top + r;
  (void)st; (void)button;
  if (i < 0 || i >= E.nlines) return 0;
  E.cy = i;
  E.cx = x / 6;
  if (E.cx > E.len[i]) E.cx = E.len[i];
  return 1;
}

/* Always taking text: every printable key is a character, including , . / and
 * ; -- which is exactly why the shell has to be told. */
static int app_wants_text(void *st) { (void)st; return 1; }

static void app_set_file(void *st, const char *path) { (void)st; load(path); }

/* 16x16: a sheet of paper with a folded corner and ruled lines. */
static const unsigned char ICON[CAPP_ICON_BYTES] = {
  0x7F, 0x80, 0x40, 0xC0, 0x40, 0xA0, 0x40, 0x90,
  0x40, 0xF8, 0x40, 0x08, 0x4F, 0xC8, 0x40, 0x08,
  0x4F, 0xC8, 0x40, 0x08, 0x4F, 0xC8, 0x40, 0x08,
  0x47, 0xC8, 0x40, 0x08, 0x7F, 0xF8, 0x00, 0x00,
};

static CappApp APP;

const CappApp *capp_register(const CardApi *a) {
  api = a;
  blank();
  APP.api_version = CAPP_API_VERSION;
  api->mem_cpy(APP.name, "Edit", 5);
  api->mem_cpy(APP.icon, ICON, CAPP_ICON_BYTES);
  APP.fullscreen = 0;
  APP.paint = app_paint;
  APP.key = app_key;
  APP.click = app_click;
  APP.open = app_open;
  APP.set_file = app_set_file;
  APP.wants_text = app_wants_text;
  APP.state = 0;
  return &APP;
}
