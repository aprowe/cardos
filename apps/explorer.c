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
 * the right with columns, a toolbar of verbs, and a footer that says what it
 * is showing. Grey face, white wells, sunken borders, navy selection.
 *
 * The menu bar is the OS's (apps/toolbar.h): File, Edit and View, built from
 * the action table, there when a pointer moves or with fn-b. It used to be
 * drawn by hand -- three titles of which one opened and two said "use the
 * toolbar" -- which was decoration pretending to be a menu.
 *
 * Everything is reachable by clicking. The keyboard still works, with the
 * same letters every app uses (CLAUDE.md, "Every app speaks the same keys"),
 * because a machine with a keyboard under your thumbs should never require
 * reaching for a mouse -- but nothing is keyboard-only.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/toolbar.h"
#include "apps/footer.h"
#include "apps/confirm.h"

#define MAX_ENTRIES 96
#define PATH_MAX   128

#define TOOL_H      16        /* the verbs */
#define HEAD_H       9        /* Name / Size column headers */
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
#define C_TYPED  CAPP_RGB(228, 232, 240)   /* a name being typed, on the footer */
#define C_WARN   CAPP_RGB(236, 120, 110)
#define C_HELD   CAPP_RGB(120, 200, 140)

#define KEY_DEL  0x7F          /* Delete on a Bluetooth keyboard */

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

  int       ask;
  char      buf[CAPP_NAME_MAX + 1];
  int       buf_len;

  char      marked[PATH_MAX];   /* cut, waiting to be pasted */
  char      status[64];         /* what just happened; shown until a key */

  CRect     full;               /* the app's rectangle, menu bar included */
  CRect     at;                 /* below the menu bar */
  int       have_at;
} X;

/* ---- helpers -------------------------------------------------------------- */

static void join(char *out, size_t n, const char *dir, const char *name) {
  if (api->str_len(dir) == 1 && dir[0] == '/') api->fmt(out, n, "/%s", name);
  else api->fmt(out, n, "%s/%s", dir, name);
}

static const char *leaf(const char *path) {
  const char *p = path, *last = path;
  for (; *p; p++) if (*p == '/' && p[1]) last = p + 1;
  return last;
}

static int ext_is(const char *ext, const char *want) {
  for (;;) {
    if (str_lower(*ext) != *want) return 0;
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

/* ---- what changed ----------------------------------------------------------
 *
 * The window used to be repainted whole for every key: a selection that moved
 * one row redrew both panes, the toolbar and the footer. Each change now
 * marks what it touched and the shell clips the next paint to that (files.c
 * is the pattern). Once anything is marked only the marks are painted, so a
 * change of folder marks everything. */

static int body_y(void) { return X.at.y + TOOL_H; }
static int body_h(void) { return X.at.h - TOOL_H - FOOT_H; }

static void damage_all(void) { if (X.have_at) api->damage(X.at); }

static void damage_footer(void) {
  if (X.have_at) api->damage(capp_rect(X.at.x, X.at.y + X.at.h - FOOT_H, X.at.w, FOOT_H));
}

static void damage_tools(void) {
  if (X.have_at) api->damage(capp_rect(X.at.x, X.at.y, X.at.w, TOOL_H));
}

/* The folder pane: a selected folder is highlighted there too. */
static void damage_tree(void) {
  if (X.have_at) api->damage(capp_rect(X.at.x + 2, body_y(), TREE_W, body_h()));
}

/* The list pane, headers and all. */
static void damage_list(void) {
  if (X.have_at)
    api->damage(capp_rect(X.at.x + TREE_W + 6, body_y(), X.at.w - TREE_W - 9, body_h()));
}

static void damage_row(int idx) {
  if (!X.have_at || idx < X.top || idx >= X.top + X.rows) return;
  api->damage(capp_rect(X.at.x + TREE_W + 6, body_y() + HEAD_H + 1 + (idx - X.top) * ROW_H,
                        X.at.w - TREE_W - 9, ROW_H));
}

static void say(const char *s) {
  api->fmt(X.status, sizeof X.status, "%s", s);
  damage_footer();
}

/* A key or a click puts the hints back. */
static void unsay(void) {
  if (!X.status[0]) return;
  X.status[0] = 0;
  damage_footer();
}

/* ---- the listing ----------------------------------------------------------- */

static int before(const CappEntry *a, const CappEntry *b) {
  const char *p = a->name, *q = b->name;
  if (a->is_dir != b->is_dir) return a->is_dir;
  for (;;) {
    char x = str_lower(*p), y = str_lower(*q);
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
  api->fmt(X.status, sizeof X.status, "%d object%s", X.n, X.n == 1 ? "" : "s");
  damage_all();
}

static void go_to(const char *path) {
  api->fmt(X.cwd, sizeof X.cwd, "%s", path);
  reload();
}

static void go_up(void) {
  int i, cut = 0;
  if (str_same(X.cwd, "/")) return;
  for (i = 0; X.cwd[i]; i++) if (X.cwd[i] == '/') cut = i;
  X.cwd[cut ? cut : 1] = 0;
  reload();
}

/* Moving the selection: the row it left, the row it reached, and the folder
 * pane, which highlights a selected folder. A selection that scrolls the
 * list marks the list. -1 selects nothing. */
static void select_idx(int idx) {
  if (idx < -1 || idx >= X.n || idx == X.sel) return;
  damage_row(X.sel);
  X.sel = idx;
  damage_row(X.sel);
  damage_tree();
  if (idx >= 0 && X.rows) {
    if (idx < X.top) { X.top = idx; damage_list(); }
    else if (idx >= X.top + X.rows) { X.top = idx - X.rows + 1; damage_list(); }
  }
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
    say(api->run(e->name, (const char *)0) == 0 ? "started" : "nothing here opens that");
    return;
  }
  if (api->run(app, path) == 0) api->fmt(X.status, sizeof X.status, "opening %s", e->name);
  else api->fmt(X.status, sizeof X.status, "no %s app", app);
  damage_footer();
}

static void open_in_editor(void) {
  char path[PATH_MAX];
  if (X.sel < 0 || at(X.sel)->is_dir) { say("select a file first"); return; }
  join(path, sizeof path, X.cwd, at(X.sel)->name);
  if (api->run("edit", path) != 0) say("no edit app");
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
  damage_footer();
}

static void cancel_ask(void) {
  if (X.ask == ASK_NONE) return;
  X.ask = ASK_NONE;
  damage_footer();
}

static void finish_ask(void) {
  char path[PATH_MAX], to[PATH_MAX];
  int what = X.ask, r;

  cancel_ask();
  if (what == ASK_NEWDIR && X.buf_len) {
    join(path, sizeof path, X.cwd, X.buf);
    r = api->mkdir(path);
    reload();
    say(r == 0 ? "folder created" : "could not create it");
  } else if (what == ASK_RENAME && X.buf_len && X.sel >= 0) {
    join(path, sizeof path, X.cwd, at(X.sel)->name);
    join(to, sizeof to, X.cwd, X.buf);
    r = api->rename(path, to);
    reload();
    say(r == 0 ? "renamed" : "could not rename it");
  }
}

static void do_delete(void) {
  char path[PATH_MAX];
  int r;
  cancel_ask();
  if (X.sel < 0) return;
  join(path, sizeof path, X.cwd, at(X.sel)->name);
  r = api->remove(path);
  reload();
  say(r == 0 ? "deleted" : "could not delete it");
}

/* The Cut button turns into Paste while something is held, so the strip is
 * marked along with the footer. */
static void cut_selected(void) {
  if (X.sel < 0) { say("select something to move"); return; }
  join(X.marked, sizeof X.marked, X.cwd, at(X.sel)->name);
  damage_tools();
  damage_footer();
}

static void paste_here(void) {
  char to[PATH_MAX];
  int r;
  if (!X.marked[0]) { say("nothing cut: m cuts"); return; }
  join(to, sizeof to, X.cwd, leaf(X.marked));
  /* A rename across directories is a move on FAT: only the entry moves. */
  r = api->rename(X.marked, to);
  X.marked[0] = 0;
  reload();
  say(r == 0 ? "moved" : "could not move it");
}

/* ---- the action table -------------------------------------------------------
 *
 * The menus, the verb buttons and the keys all come here. No chords: a chord
 * is matched before the key handler, and every letter has to type into a
 * name while one is being asked for.
 *
 * `ls`: what is in a folder, for a sentence or an AI to read -- folders with a
 * slash, files with their size. Read straight off the card, so it answers the
 * same with this app closed. */
enum {
  ACT_LS = 1, ACT_OPEN, ACT_EDITOR, ACT_NEWDIR, ACT_RENAME, ACT_DELETE,
  ACT_CUT, ACT_PASTE, ACT_UP, ACT_REREAD
};

static const CappParam P_PATH[] = { { "path", CAPP_ARG_TEXT, "a folder, like /home" } };

static const CappAction ACTIONS[] = {
  { "open",    "Open",          "File", 0, ACT_OPEN },
  { "editor",  "Open in Edit",  "File", 0, ACT_EDITOR },
  { "newdir",  "New folder",    "File", 0, ACT_NEWDIR },
  { "rename",  "Rename",        "File", 0, ACT_RENAME },
  { "delete",  "Delete",        "File", 0, ACT_DELETE },
  { "cut",     "Cut",           "Edit", 0, ACT_CUT },
  { "paste",   "Paste",         "Edit", 0, ACT_PASTE },
  { "up",      "Up a folder",   "View", 0, ACT_UP },
  { "reread",  "Refresh",       "View", 0, ACT_REREAD },
  { "ls", "List", 0, 0, ACT_LS, "what is in a folder, with sizes", P_PATH, 1, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int do_action(int a) {
  /* Del clicked while its own question is up is the answer -- the button
   * asks, and a second click means yes. */
  if (a == ACT_DELETE && X.ask == ASK_DELETE) { do_delete(); return 1; }
  /* Anything else abandons a question, as clicking away from one does. */
  if (a != ACT_LS) cancel_ask();
  switch (a) {
  case ACT_OPEN:   open_index(X.sel); return 1;
  case ACT_EDITOR: open_in_editor(); return 1;
  case ACT_NEWDIR: begin_ask(ASK_NEWDIR); return 1;
  case ACT_RENAME:
    if (X.sel >= 0) begin_ask(ASK_RENAME); else say("select something first");
    return 1;
  case ACT_DELETE:
    if (X.sel >= 0) { X.ask = ASK_DELETE; damage_footer(); }
    else say("select something first");
    return 1;
  case ACT_CUT:    cut_selected(); return 1;
  case ACT_PASTE:  paste_here(); return 1;
  case ACT_UP:     go_up(); return 1;
  case ACT_REREAD: reload(); return 1;
  default: return 0;
  }
}

/* ---- chrome ---------------------------------------------------------------- */

/* Raised and sunken, the two shapes everything in this window is made of. */
static void raised(CRect r) { api->bevel(r, C_FACE, C_HI, C_DK); }
static void sunken(CRect r) { api->bevel(r, C_WELL, C_DK, C_HI); }

static void folder_icon(int x, int y, int open) {
  api->fill(capp_rect(x, y + 2, 8, 5), C_FOLDER);
  api->fill(capp_rect(x, y + 1, 4, 1), C_FOLDER);
  api->fill(capp_rect(x, y + 6, 8, 1), C_FOLDTB);
  if (open) api->fill(capp_rect(x + 1, y + 3, 6, 1), C_HI);
}

static void file_icon(int x, int y) {
  api->fill(capp_rect(x + 1, y + 1, 6, 6), C_PAPER);
  api->frame(capp_rect(x + 1, y + 1, 6, 6), C_LO);
  api->fill(capp_rect(x + 2, y + 3, 4, 1), C_LO);
  api->fill(capp_rect(x + 2, y + 5, 3, 1), C_LO);
}

#define TOOLS 5
static const char *TOOL[TOOLS] = { "Up", "New", "Ren", "Cut", "Del" };

static CRect tool_box(CRect c, int i) {
  return capp_rect(c.x + 3 + i * 37, c.y + 2, 34, TOOL_H - 4);
}

/* ---- painting --------------------------------------------------------------- */

static void paint_tree(CRect c, int y0, int h) {
  CRect pane = capp_rect(c.x + 2, y0, TREE_W, h);
  int i, row = 0;

  sunken(pane);
  api->fill(capp_rect(pane.x + 1, pane.y + 1, pane.w - 2, pane.h - 2), C_WELL);

  /* The parent, then the folders here. One level either side is all that fits
   * and all anyone needs to navigate with: up, or down into one of these. */
  if (!str_same(X.cwd, "/")) {
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
      api->fill(capp_rect(pane.x + 1, ry - 1, pane.w - 2, ROW_H), C_SEL);
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
  CRect pane = capp_rect(lx, y0 + HEAD_H, lw, h - HEAD_H);
  int i, most;

  /* Column headers, which are buttons in the original and are drawn as
   * buttons here even though sorting is not offered -- a header that looks
   * like a label would leave the columns unexplained. */
  raised(capp_rect(lx, y0, lw - 40, HEAD_H));
  raised(capp_rect(lx + lw - 40, y0, 40, HEAD_H));
  api->text((short)(lx + 3), (short)(y0 + 1), "Name", C_TEXT, C_FACE);
  api->text((short)(lx + lw - 37), (short)(y0 + 1), "Size", C_TEXT, C_FACE);

  sunken(pane);
  api->fill(capp_rect(pane.x + 1, pane.y + 1, pane.w - 2, pane.h - 2), C_WELL);

  X.rows = (pane.h - 2) / ROW_H;
  if (X.rows < 1) X.rows = 1;
  /* The top is not pulled back to the selection here: that is what made the
   * wheel do nothing while anything was selected, every scroll undone by the
   * next paint. Selecting scrolls (select_idx); scrolling leaves the
   * selection where it is, off screen if need be. */
  most = X.n - X.rows;
  if (X.top > most) X.top = most;
  if (X.top < 0) X.top = 0;

  for (i = 0; i < X.rows; i++) {
    int idx = X.top + i;
    short ry = (short)(pane.y + 2 + i * ROW_H);
    const CappEntry *e;
    uint16_t fg = C_TEXT, bg = C_WELL;
    char size[12];

    if (idx >= X.n) break;
    e = at(idx);

    if (idx == X.sel) {
      api->fill(capp_rect(pane.x + 1, ry - 1, pane.w - 2, ROW_H), C_SEL);
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

/* The footer (apps/footer.h): a question, else what just happened, else what
 * is cut, else the keys. */
static void paint_footer(CRect c) {
  char line[FOOT_CHARS + 1];
  short y = (short)(c.y + c.h - FOOT_H + 2);

  if (X.ask == ASK_NEWDIR || X.ask == ASK_RENAME) {
    const char *label = X.ask == ASK_NEWDIR ? "folder: " : "rename: ";
    int lw = (int)api->str_len(label), room = FOOT_CHARS - lw - 1;
    int from = X.buf_len > room ? X.buf_len - room : 0;
    short x = (short)(c.x + 4 + lw * 6);
    footer_paint(api, c, label);
    api->text(x, y, X.buf + from, C_TYPED, FOOT_BG);
    api->fill(capp_rect(x + (X.buf_len - from) * 6, y, 5, 8), C_TYPED);
  } else if (X.ask == ASK_DELETE) {
    /* The name cut short enough that the question still fits. */
    char t[24];
    api->fmt(t, sizeof t, "%s", at(X.sel)->name);
    footer_paint(api, c, 0);
    api->fmt(line, sizeof line, "delete %s? y / n", t);
    api->text((short)(c.x + 4), y, line, C_WARN, FOOT_BG);
  } else if (X.status[0]) {
    api->fmt(line, sizeof line, "%s", X.status);
    footer_paint(api, c, line);
  } else if (X.marked[0]) {
    footer_paint(api, c, 0);
    api->fmt(line, sizeof line, "v paste %s here", leaf(X.marked));
    api->text((short)(c.x + 4), y, line, C_HELD, FOOT_BG);
  } else {
    footer_paint(api, c, "enter open  n new  e rename  d delete");
  }
}

static void app_paint(void *st, CRect full) {
  CRect c;
  int i;
  (void)st;

  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  c = toolbar_rest(full);

  X.full = full;
  X.at = c;
  X.have_at = 1;

  api->fill(capp_rect(c.x, c.y, c.w, TOOL_H), C_FACE);
  for (i = 0; i < TOOLS; i++) {
    CRect b = tool_box(c, i);
    raised(b);
    api->text((short)(b.x + 4), (short)(b.y + 2),
              (i == 3 && X.marked[0]) ? "Pst" : TOOL[i], C_TEXT, C_FACE);
  }
  /* Where you are, after the verbs: the last part of the path, cut to fit. */
  {
    char where[24];
    int room = (c.w - (3 + TOOLS * 37) - 3) / 6;
    if (room > (int)sizeof where - 1) room = (int)sizeof where - 1;
    if (room > 0) {
      api->fmt(where, (size_t)room + 1, "%s", leaf(X.cwd));
      api->text((short)(c.x + c.w - 3 - 6 * (int)api->str_len(where)), (short)(c.y + 4),
                where, C_DIM, C_FACE);
    }
  }

  api->fill(capp_rect(c.x, body_y(), c.w, body_h()), C_FACE);
  paint_tree(c, body_y(), body_h());
  paint_list(c, body_y(), body_h());
  paint_footer(c);

  toolbar_paint_menu(full);
}

/* ---- input ------------------------------------------------------------------ */

static void do_tool(int i) {
  switch (i) {
  case 0: do_action(ACT_UP); break;
  case 1: do_action(ACT_NEWDIR); break;
  case 2: do_action(ACT_RENAME); break;
  case 3: do_action(X.marked[0] ? ACT_PASTE : ACT_CUT); break;
  case 4: do_action(ACT_DELETE); break;
  default: break;
  }
}

/* Which entry the nth row of the tree pane is, or -1. */
static int tree_index(int row) {
  int i, r = 0;
  if (!str_same(X.cwd, "/")) {
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

/* The first sign of a mouse brings the menu bar, which moves everything down
 * by its height -- the whole window, whatever else was marked. */
static int saw_mouse(void) {
  if (!toolbar_saw_mouse()) return 0;
  if (X.have_at) api->damage(X.full);
  return 1;
}

static int app_click(void *st, short x, short y, int button) {
  CRect c = X.at;
  int a, by, i;
  (void)st;

  if (!X.have_at) return 1;
  saw_mouse();
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  unsay();

  /* Clicks arrive local to the app; every box below is in the screen
   * coordinates paint was handed (see the note in toolbar.h). Translating
   * once here is what makes the tools, tree and list line up in a window
   * that is not at the origin. The menu bar's height comes off first. */
  x = (short)(x + c.x);
  y = (short)(y - toolbar_h() + c.y);
  by = body_y();

  if (y < by) {
    for (i = 0; i < TOOLS; i++) {
      CRect b = tool_box(c, i);
      if (x >= b.x && x < b.x + b.w) { do_tool(i); return 1; }
    }
    return 1;
  }

  /* Clicking anywhere but the toolbar abandons a question, as a dialogue. */
  if (X.ask != ASK_NONE) {
    if (y < c.y + c.h - FOOT_H) cancel_ask();
    return 1;
  }
  if (y >= c.y + c.h - FOOT_H) return 1;

  /* The folder pane: one click walks. There is nothing else a folder in a
   * tree can usefully mean. */
  if (x < c.x + TREE_W + 4) {
    int idx = tree_index((y - by - 2) / ROW_H);
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
    int row = (y - by - HEAD_H - 2) / ROW_H;
    int idx = X.top + row;
    if (row < 0 || idx >= X.n) { select_idx(-1); return 1; }
    if (idx == X.sel || button == CAPP_BTN_RIGHT) open_index(idx);
    else select_idx(idx);
  }
  return 1;
}

static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  int changed;
  (void)st; (void)buttons;
  if (!X.have_at) return 0;
  changed = saw_mouse();
  if (toolbar_hover(x, y)) changed = 1;
  if (wheel) {
    int was = X.top;
    X.top -= wheel * 2;
    if (X.top > X.n - X.rows) X.top = X.n - X.rows;
    if (X.top < 0) X.top = 0;
    if (X.top != was) { damage_list(); changed = 1; }
  }
  return changed;
}

static int key_prompt(unsigned char k) {
  if (k == CAPP_KEY_ENTER) { finish_ask(); return 1; }
  if (k == CAPP_KEY_ESC) { cancel_ask(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (X.buf_len) { X.buf[--X.buf_len] = 0; damage_footer(); }
    else cancel_ask();
    return 1;
  }
  if (k >= ' ' && k < 0x7F && X.buf_len < CAPP_NAME_MAX) {
    X.buf[X.buf_len++] = (char)k;
    X.buf[X.buf_len] = 0;
    damage_footer();
  }
  return 1;
}

/* The bar first: while it has the keyboard it answers for every key. */
static int menu_key(unsigned char k, int *handled) {
  int a = toolbar_key(k);
  *handled = 1;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  *handled = 0;
  return 0;
}

static int app_key(void *st, unsigned char k) {
  int handled, r;
  (void)st;

  r = menu_key(k, &handled);
  if (handled) return r;
  unsay();

  if (X.ask == ASK_NEWDIR || X.ask == ASK_RENAME) return key_prompt(k);
  /* Asked to delete: y does it; n, Escape or Backspace is a no; anything
   * else -- Enter included -- leaves the question up (apps/confirm.h). */
  if (X.ask == ASK_DELETE) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) do_delete();
    else if (a == CONFIRM_NO) cancel_ask();
    return 1;
  }

  switch (k) {
  case CAPP_KEY_UP:    select_idx(X.sel > 0 ? X.sel - 1 : (X.n ? 0 : -1)); return 1;
  case CAPP_KEY_DOWN:  if (X.sel + 1 < X.n) select_idx(X.sel + 1); return 1;
  case CAPP_KEY_LEFT:
  case CAPP_KEY_BACK:  go_up(); return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_ENTER: return do_action(ACT_OPEN);
  case 'n': case 'N':  return do_action(ACT_NEWDIR);
  case 'e': case 'E':  return do_action(ACT_RENAME);
  case 'm': case 'M':  return do_action(ACT_CUT);
  case 'v': case 'V':  return do_action(ACT_PASTE);
  case 'd': case 'D':
  case KEY_DEL:        return do_action(ACT_DELETE);
  case 'o': case 'O':  return do_action(ACT_EDITOR);
  case 'r': case 'R':  return do_action(ACT_REREAD);
  case CAPP_KEY_ESC:
    /* Something cut is a step in, so Escape puts it back. With nothing cut
     * this is the top level, which keeps Escape. */
    if (!X.marked[0]) return 0;
    X.marked[0] = 0;
    damage_tools();
    damage_footer();
    return 1;
  default: return 0;
  }
}

static int app_wants_text(void *st) {
  (void)st;
  if (toolbar_has_keys()) return 0;
  return X.ask == ASK_NEWDIR || X.ask == ASK_RENAME;
}

/* ---- commands --------------------------------------------------------------- */

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  static CappEntry ent[48];
  size_t o = 0;
  int i, cnt;
  (void)st;
  (void)argc;
  if (action != ACT_LS) { api->fmt(out, n, "no command %d", action); return -1; }
  cnt = api->list_ex(argv[0], ent, 48);
  if (cnt < 0) { api->fmt(out, n, "no folder %s", argv[0]); return -1; }
  if (cnt == 0) { api->fmt(out, n, "%s is empty", argv[0]); return 0; }
  for (i = 0; i < cnt && o + 8 < n; i++) {
    if (ent[i].is_dir)
      o += (size_t)api->fmt(out + o, n - o, "%s/\n", ent[i].name);
    else
      o += (size_t)api->fmt(out + o, n - o, "%s  %u\n", ent[i].name, (unsigned)ent[i].size);
  }
  if (i < cnt) api->fmt(out + o, n - o, "...and %d more", cnt - i);
  return 0;
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
  "click\tselect; again to open\nright click\topen\nwheel\tscroll the list\n"
  "toolbar\tup, new, rename, cut/paste, delete (Del twice)\n"
  "up down\tselect\nenter right\topen\nleft backspace\tup a folder\n"
  "n\tnew folder\ne\trename\nd\tdelete: y yes, n or esc no\n"
  "m\tcut, to move\nv\tpaste here\nesc\tput back what was cut\n"
  "o\topen in the editor\nr\tread the folder again\nfn-b\tthe menus\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  api->mem_set(&X, 0, sizeof X);
  X.sel = -1;

  api->fmt(X.cwd, sizeof X.cwd, "%s", CAPP_HOME);
  if (argc > 1 && argv[1][0]) {
    CappStat st;
    if (api->stat(argv[1], &st) == 0 && st.is_dir)
      api->fmt(X.cwd, sizeof X.cwd, "%s", argv[1]);
  }
  reload();
  toolbar_init(api, ACTIONS, NACT, 0, 0);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
