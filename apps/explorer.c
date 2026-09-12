/* Explorer -- the two-pane file manager, for a mouse.
 *
 * apps/files.c is the keyboard one: a column, arrows, a letter per action. It
 * grew a mouse mode and the mouse mode was always the poorer half, because a
 * list built for arrow keys does not become a mouse program by accepting
 * clicks. This is the other half, built the other way round.
 *
 * So it looks like the thing it is imitating, and that is not nostalgia: the
 * Windows layout is a good answer to "show me where I am and what is here" and
 * everyone already knows how to read it. Folders down the left, contents on
 * the right with columns, a toolbar of verbs, a status line that counts what
 * it is showing, and a menu bar that is honest about being decoration at this
 * size. Grey face, white wells, sunken borders, navy selection.
 *
 * Everything is reachable by clicking. The keyboard still works -- arrows and
 * enter, because a machine with a keyboard under your thumbs should never
 * require reaching for a mouse -- but nothing is keyboard-only.
 */

#include "kernel/app/capp.h"

#define MAX_ENTRIES 96
#define PATH_MAX   128

#define MENU_H      10        /* File Edit View */
#define TOOL_H      16        /* the verbs */
#define HEAD_H       9        /* Name / Size column headers */
#define STAT_H       9        /* "12 objects" */
#define TREE_W      74        /* the folder pane */
#define ROW_H        9

/* The palette of the thing it looks like. */
#define C_FACE   CAPP_RGB(198, 198, 198)
#define C_HI     CAPP_RGB(255, 255, 255)
#define C_LO     CAPP_RGB(128, 128, 128)
#define C_DK     CAPP_RGB(64, 64, 64)
#define C_WELL   CAPP_RGB(255, 255, 255)
#define C_TEXT   CAPP_RGB(0, 0, 0)
#define C_DIM    CAPP_RGB(96, 96, 96)
#define C_SEL    CAPP_RGB(0, 0, 128)
#define C_SEL_TX CAPP_RGB(255, 255, 255)
#define C_FOLDER CAPP_RGB(230, 190, 90)
#define C_FOLDTB CAPP_RGB(180, 140, 50)
#define C_PAPER  CAPP_RGB(250, 250, 252)

enum { ASK_NONE = 0, ASK_NEWDIR, ASK_RENAME, ASK_DELETE };

static const CardApi *api;

static struct {
  char      cwd[PATH_MAX];
  CappEntry ent[MAX_ENTRIES];
  unsigned char order[MAX_ENTRIES];
  int       n;                  /* everything in cwd */
  int       nfolders;           /* how many of them are folders */
  int       sel;                /* index into the display order, or -1 */
  int       top;                /* first row shown in the list pane */
  int       rows;               /* rows the list pane fits */
  int       tree_top;

  int       ask;
  char      buf[CAPP_NAME_MAX + 1];
  int       buf_len;

  char      marked[PATH_MAX];   /* cut, waiting to be pasted */
  char      status[64];

  int       menu_open;          /* which menu is down, 1-based, 0 for none */
  CRect     at;
  int       have_at;
} X;

/* ---- helpers -------------------------------------------------------------- */

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static void say(const char *s) { api->fmt(X.status, sizeof X.status, "%s", s); }

static void join(char *out, size_t n, const char *dir, const char *name) {
  if (api->str_len(dir) == 1 && dir[0] == '/') api->fmt(out, n, "/%s", name);
  else api->fmt(out, n, "%s/%s", dir, name);
}

static const char *leaf(const char *path) {
  const char *p = path, *last = path;
  for (; *p; p++) if (*p == '/' && p[1]) last = p + 1;
  return last;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int ext_is(const char *ext, const char *want) {
  for (;;) {
    if (lower(*ext) != *want) return 0;
    if (!*want) return 1;
    ext++; want++;
  }
}

static const char *ext_of(const char *name) {
  size_t l = api->str_len(name);
  const char *ext = name + l;
  while (ext > name && *ext != '.') ext--;
  return ext;
}

/* The Type column, and the app that opens it. Two answers from one table so
 * they cannot disagree about what a file is. */
static const char *kind_of(const char *name, const char **opener) {
  const char *e = ext_of(name);
  if (e == name)            { *opener = "edit";  return "File"; }
  if (ext_is(e, ".capp"))   { *opener = NULL;    return "Program"; }
  if (ext_is(e, ".txt") || ext_is(e, ".md") || ext_is(e, ".c") ||
      ext_is(e, ".h") || ext_is(e, ".cfg") || ext_is(e, ".ini"))
                            { *opener = "edit";  return "Text"; }
  if (ext_is(e, ".jpg") || ext_is(e, ".jpeg") || ext_is(e, ".png") ||
      ext_is(e, ".bmp"))    { *opener = "photo"; return "Image"; }
  if (ext_is(e, ".cpx"))    { *opener = "web";   return "Page"; }
  if (ext_is(e, ".wav"))    { *opener = NULL;    return "Sound"; }
  if (ext_is(e, ".bin"))    { *opener = NULL;    return "Firmware"; }
  *opener = "edit";
  return "File";
}

/* ---- the listing ----------------------------------------------------------- */

static int before(const CappEntry *a, const CappEntry *b) {
  const char *p = a->name, *q = b->name;
  if (a->is_dir != b->is_dir) return a->is_dir;
  for (;;) {
    char x = lower(*p), y = lower(*q);
    if (x != y) return x < y;
    if (!x) return 0;
    p++; q++;
  }
}

static const CappEntry *at(int i) { return &X.ent[X.order[i]]; }

static void reload(void) {
  int i, j;

  X.n = api->list_ex(X.cwd, X.ent, MAX_ENTRIES);
  if (X.n < 0) { X.n = 0; say("cannot read that folder"); }

  for (i = 0; i < X.n; i++) X.order[i] = (unsigned char)i;
  for (i = 1; i < X.n; i++) {
    unsigned char tmp = X.order[i];
    for (j = i; j > 0 && before(&X.ent[tmp], &X.ent[X.order[j - 1]]); j--)
      X.order[j] = X.order[j - 1];
    X.order[j] = tmp;
  }

  X.nfolders = 0;
  for (i = 0; i < X.n; i++) if (at(i)->is_dir) X.nfolders++;

  X.sel = -1;
  X.top = 0;
  X.tree_top = 0;
  api->fmt(X.status, sizeof X.status, "%d object%s", X.n, X.n == 1 ? "" : "s");
}

static void go_to(const char *path) {
  api->fmt(X.cwd, sizeof X.cwd, "%s", path);
  reload();
}

static void go_up(void) {
  int i, cut = 0;
  if (str_eq(X.cwd, "/")) return;
  for (i = 0; X.cwd[i]; i++) if (X.cwd[i] == '/') cut = i;
  X.cwd[cut ? cut : 1] = 0;
  reload();
}

static void open_index(int i) {
  char path[PATH_MAX];
  const CappEntry *e;
  const char *app;

  if (i < 0 || i >= X.n) return;
  e = at(i);
  join(path, sizeof path, X.cwd, e->name);
  if (e->is_dir) { go_to(path); return; }

  kind_of(e->name, &app);
  if (!app) {
    if (api->run(e->name, (const char *)0) == 0) say("started");
    else say("nothing here opens that");
    return;
  }
  if (api->run(app, path) == 0) api->fmt(X.status, sizeof X.status, "opening %s", e->name);
  else api->fmt(X.status, sizeof X.status, "no %s app", app);
}

/* ---- operations ------------------------------------------------------------ */

static void begin_ask(int what) {
  X.ask = what;
  X.buf[0] = 0;
  X.buf_len = 0;
  if (what == ASK_RENAME && X.sel >= 0) {
    api->fmt(X.buf, sizeof X.buf, "%s", at(X.sel)->name);
    X.buf_len = (int)api->str_len(X.buf);
  }
}

static void finish_ask(void) {
  char path[PATH_MAX], to[PATH_MAX];

  if (X.ask == ASK_NEWDIR && X.buf_len) {
    join(path, sizeof path, X.cwd, X.buf);
    say(api->mkdir(path) == 0 ? "folder created" : "could not create it");
    reload();
  } else if (X.ask == ASK_RENAME && X.buf_len && X.sel >= 0) {
    join(path, sizeof path, X.cwd, at(X.sel)->name);
    join(to, sizeof to, X.cwd, X.buf);
    say(api->rename(path, to) == 0 ? "renamed" : "could not rename it");
    reload();
  }
  X.ask = ASK_NONE;
}

static void do_delete(void) {
  char path[PATH_MAX];
  if (X.sel >= 0) {
    join(path, sizeof path, X.cwd, at(X.sel)->name);
    say(api->remove(path) == 0 ? "deleted" : "could not delete it");
  }
  X.ask = ASK_NONE;
  reload();
}

static void cut_or_paste(void) {
  char to[PATH_MAX];
  if (X.marked[0]) {
    join(to, sizeof to, X.cwd, leaf(X.marked));
    say(api->rename(X.marked, to) == 0 ? "moved" : "could not move it");
    X.marked[0] = 0;
    reload();
    return;
  }
  if (X.sel < 0) { say("select something to move"); return; }
  join(X.marked, sizeof X.marked, X.cwd, at(X.sel)->name);
  api->fmt(X.status, sizeof X.status, "cut %s -- open a folder and press paste",
           leaf(X.marked));
}

/* ---- chrome ---------------------------------------------------------------- */

/* Raised and sunken, the two shapes everything in this window is made of. */
static void raised(CRect r) { api->bevel(r, C_FACE, C_HI, C_DK); }
static void sunken(CRect r) { api->bevel(r, C_WELL, C_DK, C_HI); }

static void folder_icon(int x, int y, int open) {
  api->fill(rect(x, y + 2, 8, 5), C_FOLDER);
  api->fill(rect(x, y + 1, 4, 1), C_FOLDER);
  api->fill(rect(x, y + 6, 8, 1), C_FOLDTB);
  if (open) api->fill(rect(x + 1, y + 3, 6, 1), C_HI);
}

static void file_icon(int x, int y) {
  api->fill(rect(x + 1, y + 1, 6, 6), C_PAPER);
  api->frame(rect(x + 1, y + 1, 6, 6), C_LO);
  api->fill(rect(x + 2, y + 3, 4, 1), C_LO);
  api->fill(rect(x + 2, y + 5, 3, 1), C_LO);
}

static const char *MENUS[3] = { "File", "Edit", "View" };
#define TOOLS 5
static const char *TOOL[TOOLS] = { "Up", "New", "Ren", "Cut", "Del" };

static CRect tool_box(CRect c, int i) {
  return rect(c.x + 3 + i * 37, c.y + MENU_H + 2, 34, TOOL_H - 4);
}

static CRect menu_box(CRect c, int i) {
  return rect(c.x + 2 + i * 32, c.y, 32, MENU_H);
}

/* ---- painting --------------------------------------------------------------- */

static void paint_tree(CRect c, int y0, int h) {
  CRect pane = rect(c.x + 2, y0, TREE_W, h);
  int i, row = 0;

  sunken(pane);
  api->fill(rect(pane.x + 1, pane.y + 1, pane.w - 2, pane.h - 2), C_WELL);

  /* The parent, then the folders here. One level either side is all that fits
   * and all anyone needs to navigate with: up, or down into one of these. */
  if (!str_eq(X.cwd, "/")) {
    folder_icon(pane.x + 3, pane.y + 2 + row * ROW_H, 0);
    api->text((short)(pane.x + 13), (short)(pane.y + 2 + row * ROW_H), "..",
              C_TEXT, C_WELL);
    row++;
  }
  for (i = 0; i < X.n && (row + 1) * ROW_H < pane.h; i++) {
    const CappEntry *e = at(i);
    short ry;
    if (!e->is_dir) continue;
    ry = (short)(pane.y + 2 + row * ROW_H);
    if (i == X.sel) {
      api->fill(rect(pane.x + 1, ry - 1, pane.w - 2, ROW_H), C_SEL);
      folder_icon(pane.x + 3, ry, 1);
      api->text((short)(pane.x + 13), ry, e->name, C_SEL_TX, C_SEL);
    } else {
      folder_icon(pane.x + 3, ry, 0);
      api->text((short)(pane.x + 13), ry, e->name, C_TEXT, C_WELL);
    }
    row++;
  }
  if (row == 0)
    api->text((short)(pane.x + 4), (short)(pane.y + 2), "no folders", C_DIM, C_WELL);
}

static void paint_list(CRect c, int y0, int h) {
  int lx = c.x + TREE_W + 6;
  int lw = c.w - TREE_W - 9;
  CRect pane = rect(lx, y0 + HEAD_H, lw, h - HEAD_H);
  int i;

  /* Column headers, which are buttons in the original and are drawn as
   * buttons here even though sorting is not offered -- a header that looks
   * like a label would leave the columns unexplained. */
  raised(rect(lx, y0, lw - 40, HEAD_H));
  raised(rect(lx + lw - 40, y0, 40, HEAD_H));
  api->text((short)(lx + 3), (short)(y0 + 1), "Name", C_TEXT, C_FACE);
  api->text((short)(lx + lw - 37), (short)(y0 + 1), "Size", C_TEXT, C_FACE);

  sunken(pane);
  api->fill(rect(pane.x + 1, pane.y + 1, pane.w - 2, pane.h - 2), C_WELL);

  X.rows = (pane.h - 2) / ROW_H;
  if (X.rows < 1) X.rows = 1;
  if (X.sel >= 0) {
    if (X.sel < X.top) X.top = X.sel;
    if (X.sel >= X.top + X.rows) X.top = X.sel - X.rows + 1;
  }

  for (i = 0; i < X.rows; i++) {
    int idx = X.top + i;
    short ry = (short)(pane.y + 2 + i * ROW_H);
    const CappEntry *e;
    uint16_t fg = C_TEXT, bg = C_WELL;
    char size[12];

    if (idx >= X.n) break;
    e = at(idx);

    if (idx == X.sel) {
      api->fill(rect(pane.x + 1, ry - 1, pane.w - 2, ROW_H), C_SEL);
      fg = C_SEL_TX;
      bg = C_SEL;
    }

    if (e->is_dir) folder_icon(pane.x + 2, ry, 0);
    else file_icon(pane.x + 2, ry);

    api->text((short)(pane.x + 12), ry, e->name, fg, bg);

    if (e->is_dir) api->fmt(size, sizeof size, "%s", "");
    else if (e->size < 1024) api->fmt(size, sizeof size, "%lu", (unsigned long)e->size);
    else api->fmt(size, sizeof size, "%luK", (unsigned long)(e->size / 1024));
    if (size[0])
      api->text((short)(pane.x + pane.w - 3 - 6 * (int)api->str_len(size)), ry,
                size, fg, bg);
  }
}

/* The File menu, when it is down. Three verbs, because a menu with fifteen
 * items on a screen this size is a joke at the reader's expense. */
static void paint_menu(CRect c) {
  static const char *ITEMS[3] = { "New folder", "Rename", "Delete" };
  CRect box = rect(c.x + 2, c.y + MENU_H, 74, 3 * ROW_H + 4);
  int i;

  raised(box);
  for (i = 0; i < 3; i++)
    api->text((short)(box.x + 4), (short)(box.y + 3 + i * ROW_H), ITEMS[i],
              C_TEXT, C_FACE);
}

static void app_paint(void *st, CRect c) {
  int body_y = c.y + MENU_H + TOOL_H;
  int body_h = c.h - MENU_H - TOOL_H - STAT_H;
  int i;
  (void)st;

  X.at = c;
  X.have_at = 1;

  api->fill(rect(c.x, c.y, c.w, c.h), C_FACE);

  /* Menu bar. */
  for (i = 0; i < 3; i++) {
    CRect m = menu_box(c, i);
    if (X.menu_open == i + 1) api->fill(m, C_SEL);
    api->text((short)(m.x + 3), (short)(m.y + 1), MENUS[i],
              X.menu_open == i + 1 ? C_SEL_TX : C_TEXT,
              X.menu_open == i + 1 ? C_SEL : C_FACE);
  }
  /* The path, right-aligned in the menu bar: the title bar already has the
   * app's name, and where you are matters more than that. */
  {
    int w = 6 * (int)api->str_len(X.cwd);
    if (w < c.w - 104)
      api->text((short)(c.x + c.w - w - 3), (short)(c.y + 1), X.cwd, C_DIM, C_FACE);
  }

  for (i = 0; i < TOOLS; i++) {
    CRect b = tool_box(c, i);
    raised(b);
    api->text((short)(b.x + 4), (short)(b.y + 2),
              (i == 3 && X.marked[0]) ? "Pst" : TOOL[i], C_TEXT, C_FACE);
  }

  paint_tree(c, body_y, body_h);
  paint_list(c, body_y, body_h);

  /* Status bar: what is here, or what is being asked. */
  {
    CRect s = rect(c.x + 2, c.y + c.h - STAT_H, c.w - 4, STAT_H - 1);
    sunken(s);
    if (X.ask == ASK_NEWDIR || X.ask == ASK_RENAME) {
      api->text((short)(s.x + 2), (short)(s.y + 1),
                X.ask == ASK_NEWDIR ? "Name:" : "Rename:", C_DIM, C_WELL);
      api->fill(rect(s.x + 1, s.y + 1, s.w - 2, s.h - 2), C_WELL);
      api->text((short)(s.x + 2), (short)(s.y + 1),
                X.ask == ASK_NEWDIR ? "Name:" : "Rename:", C_DIM, C_WELL);
      api->text((short)(s.x + 46), (short)(s.y + 1), X.buf, C_TEXT, C_WELL);
      api->fill(rect(s.x + 46 + X.buf_len * 6, s.y + 1, 5, 7), C_TEXT);
    } else if (X.ask == ASK_DELETE) {
      api->fill(rect(s.x + 1, s.y + 1, s.w - 2, s.h - 2), C_WELL);
      api->text((short)(s.x + 2), (short)(s.y + 1), "Delete? click Del again",
                C_TEXT, C_WELL);
    } else {
      api->fill(rect(s.x + 1, s.y + 1, s.w - 2, s.h - 2), C_FACE);
      api->text((short)(s.x + 2), (short)(s.y + 1), X.status, C_TEXT, C_FACE);
    }
  }

  if (X.menu_open == 1) paint_menu(c);
}

/* ---- input ------------------------------------------------------------------ */

static void do_tool(int i) {
  switch (i) {
  case 0: go_up(); break;
  case 1: begin_ask(ASK_NEWDIR); break;
  case 2: if (X.sel >= 0) begin_ask(ASK_RENAME); else say("select something first"); break;
  case 3: cut_or_paste(); break;
  case 4:
    if (X.ask == ASK_DELETE) do_delete();
    else if (X.sel >= 0) X.ask = ASK_DELETE;
    else say("select something first");
    break;
  default: break;
  }
}

/* Which entry the nth row of the tree pane is, or -1. */
static int tree_index(int row) {
  int i, r = 0;
  if (!str_eq(X.cwd, "/")) {
    if (row == 0) return -2;              /* the ".." row */
    r = 1;
  }
  for (i = 0; i < X.n; i++) {
    if (!at(i)->is_dir) continue;
    if (r == row) return i;
    r++;
  }
  return -1;
}

static int app_click(void *st, short x, short y, int button) {
  CRect c = X.at;
  int body_y, i;
  (void)st;

  if (!X.have_at) return 1;
  body_y = c.y + MENU_H + TOOL_H;

  /* A menu that is down eats the next click, wherever it lands -- which is
   * what clicking away from an open menu means. */
  if (X.menu_open) {
    int item = (y - (c.y + MENU_H) - 3) / ROW_H;
    if (x < c.x + 76 && y > c.y + MENU_H && item >= 0 && item < 3) {
      X.menu_open = 0;
      do_tool(item == 0 ? 1 : item == 1 ? 2 : 4);
      return 1;
    }
    X.menu_open = 0;
    return 1;
  }

  if (X.ask == ASK_NEWDIR || X.ask == ASK_RENAME) {
    /* Clicking outside the field abandons it, as a dialogue does. */
    if (y < c.y + c.h - STAT_H) { X.ask = ASK_NONE; return 1; }
    return 1;
  }

  if (y < c.y + MENU_H) {
    for (i = 0; i < 3; i++) {
      CRect m = menu_box(c, i);
      if (x >= m.x && x < m.x + m.w) {
        /* Edit and View have nothing behind them yet; saying so is better
         * than a menu that opens on nothing. */
        if (i == 0) X.menu_open = 1;
        else say(i == 1 ? "Edit: use the toolbar" : "View: one view, for now");
        return 1;
      }
    }
    return 1;
  }

  if (y < body_y) {
    for (i = 0; i < TOOLS; i++) {
      CRect b = tool_box(c, i);
      if (x >= b.x && x < b.x + b.w) { do_tool(i); return 1; }
    }
    return 1;
  }

  if (y >= c.y + c.h - STAT_H) return 1;

  /* The folder pane: one click walks. There is nothing else a folder in a
   * tree can usefully mean. */
  if (x < c.x + TREE_W + 4) {
    int idx = tree_index((y - body_y - 2) / ROW_H);
    if (idx == -2) { go_up(); return 1; }
    if (idx >= 0) {
      char path[PATH_MAX];
      join(path, sizeof path, X.cwd, at(idx)->name);
      go_to(path);
    }
    return 1;
  }

  /* The list: select, and open on the second click or the right button --
   * a double-click without the timing, which on one pointer is the same
   * gesture and easier to hit. */
  {
    int row = (y - body_y - HEAD_H - 2) / ROW_H;
    int idx = X.top + row;
    if (row < 0 || idx >= X.n) { X.sel = -1; return 1; }
    if (idx == X.sel || button == CAPP_BTN_RIGHT) open_index(idx);
    else X.sel = idx;
  }
  return 1;
}

static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  (void)st; (void)x; (void)y; (void)buttons;
  if (!wheel) return 0;
  X.top -= wheel * 2;
  if (X.top > X.n - 1) X.top = X.n - 1;
  if (X.top < 0) X.top = 0;
  return 1;
}

static int app_key(void *st, unsigned char k) {
  (void)st;

  if (X.ask == ASK_NEWDIR || X.ask == ASK_RENAME) {
    if (k == CAPP_KEY_ENTER) { finish_ask(); return 1; }
    if (k == CAPP_KEY_BACK) {
      if (X.buf_len) X.buf[--X.buf_len] = 0;
      else X.ask = ASK_NONE;
      return 1;
    }
    if (k >= ' ' && k < 0x7F && X.buf_len < CAPP_NAME_MAX) {
      X.buf[X.buf_len++] = (char)k;
      X.buf[X.buf_len] = 0;
    }
    return 1;
  }
  if (X.ask == ASK_DELETE) {
    if (k == 'y' || k == 'Y' || k == CAPP_KEY_ENTER) do_delete();
    else X.ask = ASK_NONE;
    return 1;
  }
  if (X.menu_open) { X.menu_open = 0; return 1; }

  switch (k) {
  case CAPP_KEY_UP:    if (X.sel > 0) X.sel--; else X.sel = 0; return 1;
  case CAPP_KEY_DOWN:  if (X.sel + 1 < X.n) X.sel++; return 1;
  case CAPP_KEY_LEFT:  go_up(); return 1;
  case CAPP_KEY_BACK:  go_up(); return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_ENTER: open_index(X.sel); return 1;
  case 'n': case 'N':  do_tool(1); return 1;
  case 'r': case 'R':  do_tool(2); return 1;
  case 'x': case 'X':  do_tool(3); return 1;
  case 'd': case 'D':  do_tool(4); return 1;
  case 'g': case 'G':  reload(); return 1;
  default: return 0;
  }
}

static int app_wants_text(void *st) {
  (void)st;
  return X.ask == ASK_NEWDIR || X.ask == ASK_RENAME;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  0,      /* a window on the desktop, which is where a mouse lives */
  "Explorer",
  /* 16x16: two panes with a divider. */
  { 0xFF, 0xFE, 0x80, 0x02, 0xBF, 0xFA, 0x80, 0x02,
    0xA8, 0x0A, 0xA0, 0x02, 0xA7, 0x8A, 0xA0, 0x02,
    0xA7, 0x8A, 0xA0, 0x02, 0xA7, 0x8A, 0xA0, 0x02,
    0xA0, 0x02, 0xBF, 0xFA, 0x80, 0x02, 0xFF, 0xFE },
  "click\tselect; again to open\nright click\topen\ntoolbar\tup, new, rename, "
  "cut/paste, delete\narrows\tmove, left is up\nenter\topen\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  api->mem_set(&X, 0, sizeof X);
  X.sel = -1;

  api->fmt(X.cwd, sizeof X.cwd, "%s", "/");
  if (argc > 1 && argv[1][0]) {
    CappStat st;
    if (api->stat(argv[1], &st) == 0 && st.is_dir)
      api->fmt(X.cwd, sizeof X.cwd, "%s", argv[1]);
  }
  reload();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
