/* A small code editor, as a loadable CardOS app.
 *
 * Dark, because that is what an editor looks like and because a 240x135 panel
 * held close is easier on the eyes dark than a sheet of white. Line numbers in
 * a gutter, the current line marked, a status bar that says what file and
 * whether it is saved.
 *
 * Two views: a file browser and the editor. It opens in the browser, because an
 * editor with no file is an editor with nothing to do -- and because the
 * previous version was handed a path by the launcher and opened its own
 * binary, which is how you learn that "open a file" and "here is your icon's
 * path" are different questions.
 *
 * The buffer is a flat array of fixed-width lines rather than a gap buffer: a
 * file this thing is for is a few kilobytes, and a flat array is the version
 * that is obviously correct at a glance.
 */

#include "kernel/app/capp.h"

#define MAXLINES  96
#define MAXCOL    64
#define ROWH      9
#define GUTTER    18      /* three digits and a separator */
#define CHARW     6

#define DIRMAX    28
#define NAMELEN   32

/* A dark palette that keeps syntax-free text readable at this size. Comment
 * green is used for the gutter, not for comments -- there is no parser here,
 * and pretending otherwise would highlight the wrong things. */
#define CLR_BG      CAPP_RGB(24, 26, 32)
#define CLR_GUTTER  CAPP_RGB(34, 37, 45)
#define CLR_LINENO  CAPP_RGB(92, 102, 120)
#define CLR_TEXT    CAPP_RGB(214, 220, 232)
#define CLR_CUR_BG  CAPP_RGB(38, 42, 52)
#define CLR_CARET   CAPP_RGB(120, 200, 255)
#define CLR_BAR     CAPP_RGB(48, 82, 128)
#define CLR_BAR_FG  CAPP_RGB(232, 238, 248)
#define CLR_DIRTY   CAPP_RGB(255, 190, 90)
#define CLR_SEL     CAPP_RGB(52, 80, 116)
#define CLR_DIM     CAPP_RGB(130, 140, 158)

typedef enum { VIEW_BROWSE = 0, VIEW_EDIT, VIEW_NAME } View;

/* What the filename prompt is for. One view serves both, because "what shall
 * it be called" is the same question either way -- only what happens after the
 * answer differs. */
typedef enum { NAME_NEW = 0, NAME_SAVE_AS } NameFor;

static const CardApi *api;

static struct {
  View view;

  /* browser */
  char dir[64];
  char names[DIRMAX][NAMELEN];
  int  ndir;
  int  bsel;

  /* buffer */
  char line[MAXLINES][MAXCOL + 1];
  short len[MAXLINES];
  int   nlines;
  int   cx, cy;
  int   top, leftcol;
  int   dirty;
  int   truncated;
  char  path[96];
  char  status[40];

  /* the filename prompt */
  NameFor name_for;
  char    name[40];
  int     name_len;
} E;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *m) { api->fmt(E.status, sizeof E.status, "%s", m); }

/* ------------------------------------------------------------- buffer ---- */

static void blank(void) {
  api->mem_set(E.line, 0, sizeof E.line);
  api->mem_set(E.len, 0, sizeof E.len);
  E.nlines = 1;
  E.cx = E.cy = E.top = E.leftcol = 0;
  E.dirty = 0;
  E.truncated = 0;
}

static void load(const char *path) {
  char buf[512];
  int fd, n, i, col = 0;

  blank();
  api->fmt(E.path, sizeof E.path, "%s", path);

  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) { say("new file"); return; }

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c == 13) continue;
      if (c == 10) {
        E.len[E.nlines - 1] = (short)col;
        col = 0;
        if (E.nlines >= MAXLINES) { E.truncated = 1; goto out; }
        E.nlines++;
        continue;
      }
      if (c == 9) {                     /* tabs become two spaces */
        int t;
        for (t = 0; t < 2 && col < MAXCOL; t++) E.line[E.nlines - 1][col++] = ' ';
        continue;
      }
      /* A control byte means this is not text. Say so rather than drawing
       * 14 KB of ELF as characters. */
      if (c < 32 || (unsigned char)c > 126) { E.truncated = 2; goto out; }
      if (col < MAXCOL) E.line[E.nlines - 1][col++] = c;
      else E.truncated = 1;
    }
  }
  E.len[E.nlines - 1] = (short)col;
out:
  api->close(fd);
  if (E.truncated == 2) {
    blank();
    say("not a text file");
  } else if (E.truncated) {
    say("opened, truncated to fit");
  } else {
    api->fmt(E.status, sizeof E.status, "%d lines", E.nlines);
  }
}

static void begin_name(NameFor why, const char *initial) {
  E.name_for = why;
  api->fmt(E.name, sizeof E.name, "%s", initial ? initial : "");
  E.name_len = (int)api->str_len(E.name);
  E.view = VIEW_NAME;
}

static void save(void) {
  int fd, i;
  char nl = 10;

  if (!E.path[0]) { begin_name(NAME_SAVE_AS, "untitled.txt"); return; }
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

/* ------------------------------------------------------------ browser ---- */

static void rescan(void) {
  static char raw[DIRMAX][NAMELEN];
  int n, i;

  E.ndir = 0;
  E.bsel = 0;
  n = api->list(E.dir, &raw[0][0], DIRMAX, NAMELEN);
  if (n < 0) { say("cannot read folder"); return; }
  for (i = 0; i < n && E.ndir < DIRMAX; i++) {
    api->fmt(E.names[E.ndir], NAMELEN, "%s", raw[i]);
    E.ndir++;
  }
  api->fmt(E.status, sizeof E.status, "%s  %d items", E.dir, E.ndir);
}

static void go_up(void) {
  int i, cut = 0;
  for (i = 0; E.dir[i]; i++) if (E.dir[i] == '/') cut = i;
  E.dir[cut ? cut : 1] = 0;
  rescan();
}

static void open_selected(void) {
  char path[96];
  int fd;

  if (E.bsel < 0 || E.bsel >= E.ndir) return;

  api->fmt(path, sizeof path, "%s%s%s", E.dir,
           E.dir[1] == 0 ? "" : "/", E.names[E.bsel]);

  /* A folder cannot be opened for reading, which is how this tells the two
   * apart -- there is no is_dir in the API an app is given. */
  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) {
    api->fmt(E.dir, sizeof E.dir, "%s", path);
    rescan();
    return;
  }
  api->close(fd);

  load(path);
  E.view = VIEW_EDIT;
}

/* ------------------------------------------------------------- editing --- */

static void scroll_to_cursor(int rows, int cols) {
  if (E.cy < E.top) E.top = E.cy;
  if (E.cy >= E.top + rows) E.top = E.cy - rows + 1;
  if (E.top < 0) E.top = 0;

  if (E.cx < E.leftcol) E.leftcol = E.cx;
  if (E.cx >= E.leftcol + cols) E.leftcol = E.cx - cols + 1;
  if (E.leftcol < 0) E.leftcol = 0;
}

static void insert_char(char c) {
  int i, n = E.len[E.cy];
  if (n >= MAXCOL) return;
  for (i = n; i > E.cx; i--) E.line[E.cy][i] = E.line[E.cy][i - 1];
  E.line[E.cy][E.cx] = c;
  E.len[E.cy] = (short)(n + 1);
  E.cx++;
  E.dirty = 1;
}

static void split_line(void) {
  int i, tail;
  if (E.nlines >= MAXLINES) { say("too many lines"); return; }
  for (i = E.nlines; i > E.cy + 1; i--) {
    api->mem_cpy(E.line[i], E.line[i - 1], MAXCOL + 1);
    E.len[i] = E.len[i - 1];
  }
  E.nlines++;
  tail = E.len[E.cy] - E.cx;
  api->mem_cpy(E.line[E.cy + 1], E.line[E.cy] + E.cx, (size_t)tail);
  E.len[E.cy + 1] = (short)tail;
  E.len[E.cy] = (short)E.cx;

  /* Keep the new line's indent. An editor that drops back to column zero on
   * every return is an editor you fight. */
  {
    int ind = 0;
    while (ind < E.len[E.cy] && E.line[E.cy][ind] == ' ') ind++;
    if (ind > 0 && E.len[E.cy + 1] + ind <= MAXCOL) {
      for (i = E.len[E.cy + 1]; i >= 0; i--)
        E.line[E.cy + 1][i + ind] = E.line[E.cy + 1][i];
      for (i = 0; i < ind; i++) E.line[E.cy + 1][i] = ' ';
      E.len[E.cy + 1] = (short)(E.len[E.cy + 1] + ind);
      E.cx = ind;
    } else {
      E.cx = 0;
    }
  }
  E.cy++;
  E.dirty = 1;
}

static void join_prev(void) {
  int i, prev;
  if (E.cy == 0) return;
  prev = E.len[E.cy - 1];
  if (prev + E.len[E.cy] > MAXCOL) { say("line would be too long"); return; }
  api->mem_cpy(E.line[E.cy - 1] + prev, E.line[E.cy], (size_t)E.len[E.cy]);
  E.len[E.cy - 1] = (short)(prev + E.len[E.cy]);
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

/* The name is joined to the folder being browsed, so "notes.txt" lands where
 * you were looking rather than at the root. */
static void finish_name(void) {
  char path[96];

  if (E.name_len == 0) { E.view = VIEW_EDIT; return; }
  api->fmt(path, sizeof path, "%s%s%s", E.dir, E.dir[1] == 0 ? "" : "/", E.name);

  if (E.name_for == NAME_NEW) {
    blank();
    api->fmt(E.path, sizeof E.path, "%s", path);
    say("new file, ctrl-s saves");
  } else {
    api->fmt(E.path, sizeof E.path, "%s", path);
    E.view = VIEW_EDIT;
    save();
    return;
  }
  E.view = VIEW_EDIT;
}

/* ------------------------------------------------------------ painting --- */

static void paint_browse(CRect c) {
  int rows = (c.h - ROWH) / ROWH;
  int top = 0, i;

  api->fill(c, CLR_BG);
  if (E.bsel >= rows) top = E.bsel - rows + 1;

  for (i = 0; i < rows && top + i < E.ndir; i++) {
    int idx = top + i;
    short y = (short)(c.y + i * ROWH);
    int sel = (idx == E.bsel);
    api->fill(rect(c.x, y, c.w, ROWH), sel ? CLR_SEL : CLR_BG);
    api->text((short)(c.x + 3), y, E.names[idx],
              sel ? CLR_BAR_FG : CLR_TEXT, sel ? CLR_SEL : CLR_BG);
  }
  if (E.ndir == 0)
    api->text((short)(c.x + 3), c.y, "empty", CLR_DIM, CLR_BG);

  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  api->text((short)(c.x + 2), (short)(c.y + c.h - ROWH + 1), E.status,
            CLR_BAR_FG, CLR_BAR);
}

static void paint_edit(CRect c) {
  int rows = (c.h - ROWH) / ROWH;
  int cols = (c.w - GUTTER) / CHARW;
  char buf[MAXCOL + 8];
  int r;

  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  scroll_to_cursor(rows, cols);

  /* No full-screen clear. Each row paints its own background as it goes, so a
   * keystroke redraws rows rather than wiping 240x135 to one colour and
   * drawing over it -- which at 40MHz is 12ms of flat background on every
   * character typed, and reads as a flash. */
  for (r = 0; r < rows; r++) {
    int i = E.top + r;
    short y = (short)(c.y + r * ROWH);
    int on_cursor = (i == E.cy);
    uint16_t bg = on_cursor ? CLR_CUR_BG : CLR_BG;
    int n;

    api->fill(rect(c.x, y, GUTTER, ROWH), CLR_GUTTER);
    api->fill(rect(c.x + GUTTER, y, c.w - GUTTER, ROWH), bg);

    if (i >= E.nlines) continue;      /* cleared, so deleted lines disappear */

    api->fmt(buf, sizeof buf, "%3d", i + 1);
    api->text((short)(c.x + 1), y, buf,
              on_cursor ? CLR_TEXT : CLR_LINENO, CLR_GUTTER);

    n = E.len[i] - E.leftcol;
    if (n > cols) n = cols;
    if (n > 0) {
      api->mem_cpy(buf, E.line[i] + E.leftcol, (size_t)n);
      buf[n] = 0;
      api->text((short)(c.x + GUTTER), y, buf, CLR_TEXT, bg);
    }

    if (on_cursor) {
      short cxp = (short)(c.x + GUTTER + (E.cx - E.leftcol) * CHARW);
      api->fill(rect(cxp, y, 1, 8), CLR_CARET);
    }
  }

  /* Status bar: the two things you look down for are which file and whether it
   * is saved. The dot is the unsaved marker, coloured rather than lettered so
   * it reads without being parsed. */
  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  if (E.dirty) api->fill(rect(c.x + 2, c.y + c.h - ROWH + 3, 3, 3), CLR_DIRTY);
  api->fmt(buf, sizeof buf, "%s  %d:%d  %s", E.path, E.cy + 1, E.cx + 1, E.status);
  api->text((short)(c.x + 7), (short)(c.y + c.h - ROWH + 1), buf,
            CLR_BAR_FG, CLR_BAR);
}

static void paint_name(CRect c) {
  char shown[sizeof E.name + 2];
  short y = (short)(c.y + c.h / 2 - 18);

  api->fill(c, CLR_BG);
  api->text((short)(c.x + 8), y,
            E.name_for == NAME_NEW ? "New file" : "Save as", CLR_BAR_FG, CLR_BG);
  api->text((short)(c.x + 8), (short)(y + 11), E.dir, CLR_DIM, CLR_BG);

  api->fill(rect(c.x + 6, y + 24, c.w - 12, 13), CLR_CUR_BG);
  api->fill(rect(c.x + 6, y + 24, c.w - 12, 1), CLR_SEL);
  api->mem_cpy(shown, E.name, (size_t)E.name_len);
  shown[E.name_len] = '_';
  shown[E.name_len + 1] = 0;
  api->text((short)(c.x + 9), (short)(y + 27), shown, CLR_TEXT, CLR_CUR_BG);

  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  api->text((short)(c.x + 3), (short)(c.y + c.h - ROWH + 1),
            "enter confirms   backspace cancels when empty", CLR_BAR_FG, CLR_BAR);
}

static void app_paint(void *st, CRect c) {
  (void)st;
  if (E.view == VIEW_BROWSE) paint_browse(c);
  else if (E.view == VIEW_NAME) paint_name(c);
  else paint_edit(c);
}

/* --------------------------------------------------------------- input --- */

static int key_browse(unsigned char k) {
  switch (k) {
  case CAPP_KEY_UP:   if (E.bsel > 0) E.bsel--; return 1;
  case CAPP_KEY_DOWN: if (E.bsel + 1 < E.ndir) E.bsel++; return 1;
  case CAPP_KEY_LEFT:
  case CAPP_KEY_BACK: go_up(); return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_ENTER: open_selected(); return 1;
  case 'n': case 'N':
    begin_name(NAME_NEW, "");
    return 1;
  case 'r': case 'R': rescan(); return 1;
  default: return 0;
  }
}

static int key_edit(unsigned char k) {
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

  case CAPP_KEY_BACK:  backspace(); return 1;
  case CAPP_KEY_ENTER: split_line(); return 1;

  case 0x13: save(); return 1;                    /* ctrl-s */
  case 0x0E: begin_name(NAME_NEW, ""); return 1;  /* ctrl-n */
  case 0x12:                                      /* ctrl-r, save as */
    begin_name(NAME_SAVE_AS, E.path[0] ? E.path : "untitled.txt");
    return 1;
  case 0x0F:                                      /* ctrl-o, back to the list */
    E.view = VIEW_BROWSE;
    rescan();
    return 1;
  case 0x01: E.cx = 0; return 1;                  /* ctrl-a */
  case 0x05: E.cx = E.len[E.cy]; return 1;        /* ctrl-e */

  default:
    if (k >= 32 && k < 127) { insert_char((char)k); return 1; }
    return 0;
  }
}

static int key_name(unsigned char k) {
  if (k == CAPP_KEY_ENTER) { finish_name(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (E.name_len > 0) E.name[--E.name_len] = 0;
    else E.view = (E.name_for == NAME_NEW) ? VIEW_BROWSE : VIEW_EDIT;
    return 1;
  }
  /* No slashes: this names a file in the folder being browsed, and a path
   * typed here would silently land somewhere else. */
  if (k >= 32 && k < 127 && k != '/' && E.name_len < (int)sizeof E.name - 1) {
    E.name[E.name_len++] = (char)k;
    E.name[E.name_len] = 0;
    return 1;
  }
  return 0;
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (E.view == VIEW_BROWSE) return key_browse(k);
  if (E.view == VIEW_NAME) return key_name(k);
  return key_edit(k);
}

static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)button;

  if (E.view == VIEW_BROWSE) {
    int i = y / ROWH;
    if (i < 0 || i >= E.ndir) return 0;
    if (i == E.bsel) open_selected();
    else E.bsel = i;
    return 1;
  }

  {
    int r = y / ROWH;
    int i = E.top + r;
    if (i < 0 || i >= E.nlines) return 0;
    E.cy = i;
    E.cx = E.leftcol + (x - GUTTER) / CHARW;
    if (E.cx < 0) E.cx = 0;
    if (E.cx > E.len[i]) E.cx = E.len[i];
    return 1;
  }
}

/* Only while editing. In the browser the arrows move a selection, and a
 * machine whose arrow keys are ; . , / needs those back. */
static int app_wants_text(void *st) {
  (void)st;
  return E.view != VIEW_BROWSE;
}

static void app_open(void *st) {
  (void)st;
  if (!E.dir[0]) api->fmt(E.dir, sizeof E.dir, "%s", "/");
  E.view = VIEW_BROWSE;
  rescan();
}

static void app_set_file(void *st, const char *path) {
  (void)st;
  load(path);
  E.view = VIEW_EDIT;
}

/* 16x16: a document with a folded corner and ruled lines. */
static const unsigned char ICON[CAPP_ICON_BYTES] = {
  0x00, 0x00, 0x1F, 0xF0, 0x10, 0x18, 0x10, 0x14,
  0x10, 0x12, 0x10, 0x1F, 0x13, 0xC1, 0x10, 0x01,
  0x13, 0xE1, 0x10, 0x01, 0x13, 0xC1, 0x10, 0x01,
  0x11, 0xE1, 0x10, 0x01, 0x1F, 0xFF, 0x00, 0x00,
};

static CappApp APP;

const CappApp *capp_register(const CardApi *a) {
  api = a;
  blank();
  APP.api_version = CAPP_API_VERSION;
  api->mem_cpy(APP.name, "Edit", 5);
  api->mem_cpy(APP.icon, ICON, CAPP_ICON_BYTES);
  APP.fullscreen = 1;          /* an editor wants every pixel it can get */
  APP.paint = app_paint;
  APP.key = app_key;
  APP.click = app_click;
  APP.open = app_open;
  APP.set_file = app_set_file;
  APP.height = 0;
  APP.pref_w = 0;
  APP.pref_h = 0;
  APP.wants_text = app_wants_text;
  APP.help = "arrows\tmove\nenter\topen, or split the line\nbackspace\tup a folder, or delete\nn\tnew file\nctrl-n\tnew file\nctrl-s\tsave\nctrl-r\tsave as\nctrl-o\tback to the file list\nctrl-a\tstart of line\nctrl-e\tend of line\n";
  APP.state = 0;
  return &APP;
}
