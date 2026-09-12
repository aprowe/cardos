/* Files -- a file manager with two hands.
 *
 * The same listing, driven two ways, because this machine is two machines. On
 * the launcher it is a keyboard device: one column, arrows, and a letter per
 * action. On the desktop, with a mouse, it is a toolbar and rows you click.
 *
 * It does not ask which. The mode follows the last thing that was used -- a
 * keypress puts it in keyboard mode, a mouse event puts it in mouse mode --
 * because that is always right and never needs configuring. The toolbar
 * appears when a pointer does.
 *
 * What it can do: walk the card, make folders, rename, move, delete, and open
 * a file in the app that suits it. Opening is `api->run`, so this contains no
 * editor, no image decoder and no browser -- it knows which app to hand a name
 * to, and that is all a file manager should know.
 */

#include "kernel/app/capp.h"

#define MAX_ENTRIES  96
#define PATH_MAX    128
#define ROW_H         9
#define BAR_H        10          /* the path, along the top */
#define TOOL_H       13          /* the button strip, mouse mode only */
#define STATUS_H     10

#define CLR_BG      CAPP_RGB(20, 22, 28)
#define CLR_BAR     CAPP_RGB(38, 44, 64)
#define CLR_FG      CAPP_RGB(228, 232, 240)
#define CLR_DIM     CAPP_RGB(140, 148, 164)
#define CLR_DIR     CAPP_RGB(240, 200, 110)
#define CLR_SEL     CAPP_RGB(58, 92, 150)
#define CLR_MARK    CAPP_RGB(120, 200, 140)
#define CLR_FACE    CAPP_RGB(196, 200, 210)
#define CLR_FACE_HI CAPP_RGB(240, 242, 248)
#define CLR_FACE_LO CAPP_RGB(110, 114, 124)
#define CLR_TEXT_D  CAPP_RGB(24, 26, 32)
#define CLR_ERR     CAPP_RGB(236, 120, 110)

enum { MODE_KEYS = 0, MODE_MOUSE };
enum { ASK_NONE = 0, ASK_NEWDIR, ASK_RENAME, ASK_DELETE };

static const CardApi *api;

static struct {
  char      cwd[PATH_MAX];
  CappEntry ent[MAX_ENTRIES];
  /* The display order, as indices into ent. Sorting the array itself would
   * mean assigning 72-byte structs, and a struct assignment compiles to a
   * memcpy an app has nothing to link against -- so the entries stay where
   * the listing put them and this says what order to read them in. */
  unsigned char order[MAX_ENTRIES];
  int       n;
  int       sel;
  int       top;                 /* first visible row */
  int       rows;                /* how many fit, from the last paint */

  int       mode;
  int       ask;                 /* a prompt is open */
  char      buf[CAPP_NAME_MAX + 1];
  int       buf_len;

  char      marked[PATH_MAX];    /* the file waiting to be moved */
  char      status[64];

  CRect     at;
  int       have_at;
} F;

/* ---- small helpers -------------------------------------------------------- */

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static int str_eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Case-insensitive, because a camera writes PHOTO.JPG and a person writing
 * this table thinks in lower case. */
static int ext_is(const char *ext, const char *want) {
  for (;;) {
    if (lower(*ext) != *want) return 0;
    if (!*want) return 1;
    ext++; want++;
  }
}

static void say(const char *s) { api->fmt(F.status, sizeof F.status, "%s", s); }

/* Join a directory and a name into a path, without the double slash that a
 * naive concatenation produces at the root. */
static void join(char *out, size_t n, const char *dir, const char *name) {
  size_t l = api->str_len(dir);
  if (l == 1 && dir[0] == '/') api->fmt(out, n, "/%s", name);
  else api->fmt(out, n, "%s/%s", dir, name);
}

/* The last component of a path, for the title bar. */
static const char *leaf(const char *path) {
  const char *p = path, *last = path;
  for (; *p; p++) if (*p == '/' && p[1]) last = p + 1;
  return last;
}

/* Which app opens this, by extension. The whole of the file manager's
 * knowledge about other apps lives here, deliberately: a longer list is a
 * setting, not a rewrite. */
static const char *opener(const char *name) {
  size_t l = api->str_len(name);
  const char *ext = name + l;
  while (ext > name && *ext != '.') ext--;
  if (ext == name) return "edit";               /* no extension: text */

  if (ext_is(ext, ".capp")) return NULL;        /* run it, not open it */
  if (ext_is(ext, ".jpg") || ext_is(ext, ".jpeg") ||
      ext_is(ext, ".png") || ext_is(ext, ".bmp")) return "photo";
  if (ext_is(ext, ".cpx")) return "web";
  return "edit";                                /* txt, c, h, md, cfg, ini */
}

/* ---- the listing ---------------------------------------------------------- */

/* Directories first, then names, both case-insensitively. An insertion sort:
 * ninety-six entries at most, and it runs when a directory is entered rather
 * than per frame. */
static int before(const CappEntry *a, const CappEntry *b) {
  const char *p = a->name, *q = b->name;
  if (a->is_dir != b->is_dir) return a->is_dir;
  for (;;) {
    char x = *p, y = *q;
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
    if (x != y) return x < y;
    if (!x) return 0;
    p++; q++;
  }
}

/* The entry at a display position. */
static const CappEntry *at(int i) {
  return &F.ent[F.order[i]];
}

static void reload(void) {
  int i, j;

  F.n = api->list_ex(F.cwd, F.ent, MAX_ENTRIES);
  if (F.n < 0) { F.n = 0; say("cannot read that folder"); }

  for (i = 0; i < F.n; i++) F.order[i] = (unsigned char)i;
  for (i = 1; i < F.n; i++) {
    unsigned char tmp = F.order[i];
    for (j = i; j > 0 && before(&F.ent[tmp], &F.ent[F.order[j - 1]]); j--)
      F.order[j] = F.order[j - 1];
    F.order[j] = tmp;
  }
  if (F.sel >= F.n) F.sel = F.n ? F.n - 1 : 0;
  if (F.sel < 0) F.sel = 0;
  F.top = 0;
}

static void go_to(const char *path) {
  api->fmt(F.cwd, sizeof F.cwd, "%s", path);
  F.sel = 0;
  reload();
}

static void go_up(void) {
  int i, cut = 0;
  if (str_eq(F.cwd, "/")) return;
  for (i = 0; F.cwd[i]; i++) if (F.cwd[i] == '/') cut = i;
  F.cwd[cut ? cut : 1] = 0;
  F.sel = 0;
  reload();
}

static void enter_selected(void) {
  char path[PATH_MAX];
  const CappEntry *e;

  if (!F.n) return;
  e = at(F.sel);
  join(path, sizeof path, F.cwd, e->name);

  if (e->is_dir) { go_to(path); return; }

  {
    const char *app = opener(e->name);
    if (!app) {
      /* A program: run it, rather than showing someone its bytes. */
      if (api->run(e->name, (const char *)0) == 0) say("started");
      else say("would not start");
      return;
    }
    if (api->run(app, path) == 0) api->fmt(F.status, sizeof F.status, "%s %s", app, e->name);
    else api->fmt(F.status, sizeof F.status, "no %s app", app);
  }
}

/* ---- the operations -------------------------------------------------------- */

static void begin_ask(int what) {
  F.ask = what;
  F.buf[0] = 0;
  F.buf_len = 0;
  if (what == ASK_RENAME && F.n) {
    api->fmt(F.buf, sizeof F.buf, "%s", at(F.sel)->name);
    F.buf_len = (int)api->str_len(F.buf);
  }
}

static void finish_ask(void) {
  char path[PATH_MAX], to[PATH_MAX];

  if (F.ask == ASK_NEWDIR && F.buf_len) {
    join(path, sizeof path, F.cwd, F.buf);
    if (api->mkdir(path) == 0) say("folder made");
    else say("could not make it");
    reload();
  } else if (F.ask == ASK_RENAME && F.buf_len && F.n) {
    join(path, sizeof path, F.cwd, at(F.sel)->name);
    join(to, sizeof to, F.cwd, F.buf);
    if (api->rename(path, to) == 0) say("renamed");
    else say("could not rename");
    reload();
  }
  F.ask = ASK_NONE;
}

static void mark_for_move(void) {
  if (!F.n) return;
  join(F.marked, sizeof F.marked, F.cwd, at(F.sel)->name);
  api->fmt(F.status, sizeof F.status, "move %s -- go and press p",
           at(F.sel)->name);
}

static void paste_here(void) {
  char to[PATH_MAX];
  if (!F.marked[0]) { say("nothing marked -- press m first"); return; }
  join(to, sizeof to, F.cwd, leaf(F.marked));
  /* A rename across directories is a move on FAT, and it costs nothing: no
   * bytes are copied, only the entry. */
  if (api->rename(F.marked, to) == 0) say("moved");
  else say("could not move it");
  F.marked[0] = 0;
  reload();
}

static void delete_selected(void) {
  char path[PATH_MAX];
  if (!F.n) return;
  join(path, sizeof path, F.cwd, at(F.sel)->name);
  if (api->remove(path) == 0) say("deleted");
  else say("could not delete it");
  F.ask = ASK_NONE;
  reload();
}

/* ---- what changed ---------------------------------------------------------
 *
 * Moving the selection changes two rows out of thirteen. Saying so is the
 * difference between a keypress costing two rows and costing the window; the
 * shell clips the next paint to whatever is marked here. Marking nothing --
 * which is what everything else in this app does -- means the whole window,
 * exactly as before. */

static int list_top(void) {
  return F.at.y + BAR_H + (F.mode == MODE_MOUSE ? TOOL_H : 0);
}

static void damage_row(int idx) {
  if (!F.have_at || idx < F.top || idx >= F.top + F.rows) return;
  api->damage(rect(F.at.x, list_top() + (idx - F.top) * ROW_H, F.at.w, ROW_H));
}

/* The status strip, which is also the prompt. */
static void damage_status(void) {
  if (!F.have_at) return;
  api->damage(rect(F.at.x, F.at.y + F.at.h - STATUS_H, F.at.w, STATUS_H));
}

/* Moving the selection: the row it left and the row it arrived at. */
static void select_row(int idx) {
  if (idx < 0 || idx >= F.n || idx == F.sel) return;
  damage_row(F.sel);
  F.sel = idx;
  damage_row(F.sel);
  /* Scrolling changes every row, so if the new selection is off-screen the
   * marks above are not enough -- say nothing and take the full repaint. */
  if (F.sel < F.top || F.sel >= F.top + F.rows) api->damage(F.at);
}

/* ---- painting -------------------------------------------------------------- */

static void button(CRect r, const char *label, int on) {
  api->bevel(r, CLR_FACE, on ? CLR_FACE_LO : CLR_FACE_HI,
             on ? CLR_FACE_HI : CLR_FACE_LO);
  api->text((short)(r.x + 3), (short)(r.y + 3), label, CLR_TEXT_D, CLR_FACE);
}

/* The toolbar, in mouse mode. Six buttons across 240 pixels is 38 each, which
 * fits three characters and a margin -- hence the abbreviations. */
#define NBUTTONS 6
static const char *BTN[NBUTTONS] = { "up", "new", "ren", "mov", "del", "opn" };

static CRect button_box(CRect c, int i) {
  int w = c.w / NBUTTONS;
  return rect(c.x + i * w, c.y + BAR_H, (i == NBUTTONS - 1) ? c.w - i * w : w - 1,
              TOOL_H - 1);
}

static void app_paint(void *st, CRect c) {
  int list_top = c.y + BAR_H + (F.mode == MODE_MOUSE ? TOOL_H : 0);
  int list_h = c.h - (list_top - c.y) - STATUS_H;
  int i;
  (void)st;

  F.rows = list_h / ROW_H;
  if (F.rows < 1) F.rows = 1;
  if (F.sel < F.top) F.top = F.sel;
  if (F.sel >= F.top + F.rows) F.top = F.sel - F.rows + 1;

  /* The path, always. Knowing where you are is most of a file manager. */
  api->fill(rect(c.x, c.y, c.w, BAR_H), CLR_BAR);
  api->text((short)(c.x + 2), (short)(c.y + 1), F.cwd, CLR_FG, CLR_BAR);

  if (F.mode == MODE_MOUSE)
    for (i = 0; i < NBUTTONS; i++) button(button_box(c, i), BTN[i], 0);

  api->fill(rect(c.x, list_top, c.w, list_h), CLR_BG);
  for (i = 0; i < F.rows; i++) {
    int idx = F.top + i;
    short y = (short)(list_top + i * ROW_H);
    const CappEntry *e;
    uint16_t fg;

    if (idx >= F.n) break;
    e = at(idx);

    if (idx == F.sel) api->fill(rect(c.x, y, c.w, ROW_H), CLR_SEL);
    fg = e->is_dir ? CLR_DIR : CLR_FG;

    /* A folder gets a slash rather than an icon: at nine pixels a row, one
     * character says it more clearly than four pixels of drawing. */
    api->text((short)(c.x + 3), (short)(y + 1), e->is_dir ? "/" : " ", fg,
              idx == F.sel ? CLR_SEL : CLR_BG);
    api->text((short)(c.x + 11), (short)(y + 1), e->name, fg,
              idx == F.sel ? CLR_SEL : CLR_BG);

    if (!e->is_dir) {
      char size[12];
      /* Kilobytes past a kilobyte: a column of four-digit byte counts is
       * harder to read than the thing it is measuring. */
      if (e->size < 1024) api->fmt(size, sizeof size, "%luB", (unsigned long)e->size);
      else api->fmt(size, sizeof size, "%luK", (unsigned long)(e->size / 1024));
      api->text((short)(c.x + c.w - 6 * (int)api->str_len(size) - 3), (short)(y + 1),
                size, CLR_DIM, idx == F.sel ? CLR_SEL : CLR_BG);
    }
  }

  if (!F.n)
    api->text((short)(c.x + 6), (short)(list_top + 6), "(empty)", CLR_DIM, CLR_BG);

  /* The status line doubles as the prompt: one row at the bottom that is
   * either telling you what happened or asking for a name. */
  {
    short y = (short)(c.y + c.h - STATUS_H);
    api->fill(rect(c.x, y, c.w, STATUS_H), CLR_BAR);
    if (F.ask == ASK_NEWDIR || F.ask == ASK_RENAME) {
      api->text((short)(c.x + 2), (short)(y + 1),
                F.ask == ASK_NEWDIR ? "folder:" : "name:", CLR_DIM, CLR_BAR);
      api->text((short)(c.x + 50), (short)(y + 1), F.buf, CLR_FG, CLR_BAR);
      api->fill(rect(c.x + 50 + F.buf_len * 6, y + 1, 5, 8), CLR_FG);
    } else if (F.ask == ASK_DELETE) {
      api->text((short)(c.x + 2), (short)(y + 1), "delete? y/n", CLR_ERR, CLR_BAR);
    } else if (F.marked[0]) {
      api->text((short)(c.x + 2), (short)(y + 1), "holding:", CLR_MARK, CLR_BAR);
      api->text((short)(c.x + 52), (short)(y + 1), leaf(F.marked), CLR_MARK, CLR_BAR);
    } else {
      api->text((short)(c.x + 2), (short)(y + 1), F.status, CLR_DIM, CLR_BAR);
    }
  }
}

/* ---- keys ------------------------------------------------------------------ */

static int key_prompt(unsigned char k) {
  if (k == CAPP_KEY_ENTER) { finish_ask(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (F.buf_len) { F.buf[--F.buf_len] = 0; damage_status(); }
    else F.ask = ASK_NONE;
    return 1;
  }
  if (k >= ' ' && k < 0x7F && F.buf_len < CAPP_NAME_MAX) {
    F.buf[F.buf_len++] = (char)k;
    F.buf[F.buf_len] = 0;
    damage_status();
    return 1;
  }
  return 1;
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  F.mode = MODE_KEYS;

  if (F.ask == ASK_DELETE) {
    if (k == 'y' || k == 'Y') delete_selected();
    else F.ask = ASK_NONE;
    return 1;
  }
  if (F.ask != ASK_NONE) return key_prompt(k);

  switch (k) {
  case CAPP_KEY_UP:    select_row(F.sel - 1); return 1;
  case CAPP_KEY_DOWN:  select_row(F.sel + 1); return 1;
  case CAPP_KEY_LEFT:  go_up(); return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_ENTER: enter_selected(); return 1;
  case CAPP_KEY_BACK:  go_up(); return 1;
  case 'n': case 'N':  begin_ask(ASK_NEWDIR); return 1;
  case 'r': case 'R':  begin_ask(ASK_RENAME); return 1;
  case 'm': case 'M':  mark_for_move(); return 1;
  case 'p': case 'P':  paste_here(); return 1;
  case 'd': case 'D':  if (F.n) F.ask = ASK_DELETE; return 1;
  case 'e': case 'E': {
    char path[PATH_MAX];
    if (!F.n || at(F.sel)->is_dir) return 1;
    join(path, sizeof path, F.cwd, at(F.sel)->name);
    api->run("edit", path);
    return 1;
  }
  case 'g': case 'G':  reload(); say("reread"); return 1;
  default: return 0;
  }
}

/* ---- mouse ------------------------------------------------------------------ */

static void do_button(int i) {
  switch (i) {
  case 0: go_up(); break;
  case 1: begin_ask(ASK_NEWDIR); break;
  case 2: begin_ask(ASK_RENAME); break;
  case 3: if (F.marked[0]) paste_here(); else mark_for_move(); break;
  case 4: if (F.n) F.ask = ASK_DELETE; break;
  case 5: enter_selected(); break;
  default: break;
  }
}

static int app_click(void *st, short x, short y, int button) {
  CRect c = F.at;
  int list_top, i;
  (void)st;

  F.mode = MODE_MOUSE;            /* a pointer arrived: show the toolbar */
  if (!F.have_at) return 1;

  /* The prompt takes the screen while it is open: a click anywhere cancels,
   * which is what clicking away from a dialogue means everywhere else. */
  if (F.ask != ASK_NONE) { F.ask = ASK_NONE; return 1; }

  if (y < BAR_H) { go_up(); return 1; }        /* the path bar walks up */

  list_top = BAR_H + TOOL_H;
  if (y < list_top) {
    for (i = 0; i < NBUTTONS; i++) {
      CRect b = button_box(rect(0, 0, c.w, c.h), i);
      if (x >= b.x && x < b.x + b.w) { do_button(i); return 1; }
    }
    return 1;
  }

  {
    int row = (y - list_top) / ROW_H;
    int idx = F.top + row;
    if (idx < 0 || idx >= F.n) return 1;

    /* Second click on the same row opens it -- a double-click without the
     * timing, which on a device with one pointer and no drag is the same
     * gesture and easier to hit. The right button opens straight away. */
    if (idx == F.sel || button == CAPP_BTN_RIGHT) enter_selected();
    else select_row(idx);
  }
  return 1;
}

static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  (void)st; (void)x; (void)y; (void)buttons;
  if (F.mode != MODE_MOUSE) { F.mode = MODE_MOUSE; if (!wheel) return 1; }
  if (!wheel) return 0;
  {
    int want = F.sel - wheel;
    if (want < 0) want = 0;
    if (want >= F.n) want = F.n ? F.n - 1 : 0;
    select_row(want);
  }
  return 1;
}

static int app_wants_text(void *st) {
  (void)st;
  return F.ask == ASK_NEWDIR || F.ask == ASK_RENAME;
}

/* The content rect is needed by the click handler and only paint is told it,
 * so paint records it. */
static void paint_and_remember(void *st, CRect c) {
  F.at = c;
  F.have_at = 1;
  app_paint(st, c);
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  0,      /* a window on the desktop, fullscreen from the launcher */
  "Files",
  /* 16x16: a folder with a tab. */
  { 0x00, 0x00, 0x78, 0x00, 0x84, 0x00, 0xFF, 0xFC,
    0x80, 0x04, 0x80, 0x04, 0x80, 0x04, 0x80, 0x04,
    0x80, 0x04, 0x80, 0x04, 0x80, 0x04, 0x80, 0x04,
    0x80, 0x04, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00 },
  "arrows\tmove, left is up a folder\nenter\topen\nn r\tnew folder, rename\n"
  "m p\tmark a file, then move it here\nd\tdelete\ne\topen in the editor\n"
  "g\tread the folder again\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  api->mem_set(&F, 0, sizeof F);

  /* Somewhere useful by default, and wherever you say if you say. */
  api->fmt(F.cwd, sizeof F.cwd, "%s", "/");
  if (argc > 1 && argv[1][0]) {
    CappStat st;
    if (api->stat(argv[1], &st) == 0 && st.is_dir)
      api->fmt(F.cwd, sizeof F.cwd, "%s", argv[1]);
  }
  say("n new  r rename  m move  d delete");
  reload();

  UI.paint = paint_and_remember;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
