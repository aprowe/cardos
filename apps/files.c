/* Files -- a file manager with two hands.
 *
 * The same listing, driven two ways, because this machine is two machines. On
 * the launcher it is a keyboard device: one column, arrows, and a letter per
 * action. On the desktop, with a mouse, it has a menu bar and rows you click.
 *
 * It does not ask which. The menu bar is apps/toolbar.h, which appears when a
 * pointer first moves (or with fn-b, which also puts the keyboard in it), and
 * its menus come from the same action table as everything else. This used to
 * be a mode of its own -- a strip of six hand-drawn buttons that a keypress
 * took away again -- and switching back to the keys moved every row while
 * marking only two of them, which left the screen half old.
 *
 * What it can do: walk the card, make folders, rename, move, delete, and open
 * a file in the app that suits it. Opening is `api->run`, so this contains no
 * editor, no image decoder and no browser -- it knows which app to hand a name
 * to, and that is all a file manager should know.
 *
 * The keys are the shared vocabulary (CLAUDE.md, "Every app speaks the same
 * keys"): e renames, o opens in the editor, v pastes, r reads again, d
 * deletes after asking, and Escape backs out of whatever is being asked.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/toolbar.h"
#include "apps/footer.h"
#include "apps/confirm.h"
#include "apps/dirmodel.h"

#define PATH_MAX    DIR_PATH_MAX
#define ROW_H         9
#define BAR_H        10          /* the path, along the top */

#define CLR_BG      CAPP_RGB(20, 22, 28)
#define CLR_BAR     CAPP_RGB(38, 44, 64)
#define CLR_FG      CAPP_RGB(228, 232, 240)
#define CLR_DIM     CAPP_RGB(140, 148, 164)
#define CLR_DIR     CAPP_RGB(240, 200, 110)
#define CLR_SEL     CAPP_RGB(58, 92, 150)
#define CLR_MARK    CAPP_RGB(120, 200, 140)
#define CLR_ERR     CAPP_RGB(236, 120, 110)

#define KEY_DEL     0x7F         /* Delete on a Bluetooth keyboard */

enum { ASK_NONE = 0, ASK_NEWDIR, ASK_RENAME, ASK_DELETE };

static const CardApi *api;

static struct {
  DirList   d;                   /* the folder: cwd, the listing, its order */
  int       sel;
  int       top;                 /* first visible row */
  int       rows;                /* how many fit, from the last paint */

  int       ask;                 /* a prompt is open */
  char      buf[CAPP_NAME_MAX + 1];
  int       buf_len;

  char      marked[PATH_MAX];    /* the file waiting to be moved */
  char      status[64];          /* what just happened; shown until a key */

  CRect     full;                /* the app's rectangle, toolbar included */
  CRect     at;                  /* below the toolbar */
  int       have_at;
} F;

/* The entry at a display position. */
static const CappEntry *at(int i) { return dir_at(&F.d, i); }

/* ---- what changed ---------------------------------------------------------
 *
 * Moving the selection changes two rows out of thirteen. Saying so is the
 * difference between a keypress costing two rows and costing the window; the
 * shell clips the next paint to whatever is marked here. Marking nothing
 * means the whole window. Once anything is marked, though, only the marks are
 * painted -- so whatever changes the listing marks all of it (reload). */

static int list_top(void) { return F.at.y + BAR_H; }

static void damage_row(int idx) {
  if (!F.have_at || idx < F.top || idx >= F.top + F.rows) return;
  api->damage(capp_rect(F.at.x, list_top() + (idx - F.top) * ROW_H, F.at.w, ROW_H));
}

/* The footer, which is also the prompt and the status line. */
static void damage_footer(void) {
  if (!F.have_at) return;
  api->damage(capp_rect(F.at.x, F.at.y + F.at.h - FOOT_H, F.at.w, FOOT_H));
}

static void damage_all(void) {
  if (F.have_at) api->damage(F.at);
}

static void say(const char *s) {
  api->fmt(F.status, sizeof F.status, "%s", s);
  damage_footer();
}

/* A key or a click puts the hints back: a message is for the moment after
 * whatever caused it, not for good. */
static void unsay(void) {
  if (!F.status[0]) return;
  F.status[0] = 0;
  damage_footer();
}

/* ---- the listing (apps/dirmodel.h) ------------------------------------------ */

/* After the folder was read again: the selection kept in range, and all of
 * it marked. */
static void relisted(void) {
  if (F.sel >= F.d.n) F.sel = F.d.n ? F.d.n - 1 : 0;
  if (F.sel < 0) F.sel = 0;
  F.top = 0;
  damage_all();
}

static void reload(void) {
  if (dir_reload(api, &F.d) < 0) say("cannot read that folder");
  relisted();
}

static void go_to(const char *path) {
  F.sel = 0;
  if (dir_go(api, &F.d, path) < 0) say("cannot read that folder");
  relisted();
}

static void go_up(void) {
  F.sel = 0;
  if (dir_up(api, &F.d)) relisted();
}

static void enter_selected(void) {
  char path[PATH_MAX];
  const CappEntry *e;
  const char *app;
  int how;

  if (!F.d.n) return;
  e = at(F.sel);
  dir_path(api, &F.d, F.sel, path, sizeof path);

  if (e->is_dir) { go_to(path); return; }

  dir_kind(e->name, &how, &app);
  if (how == DIR_OPEN_RUN) {
    /* A program: run it, rather than showing someone its bytes. */
    say(api->run(e->name, (const char *)0) == 0 ? "started" : "would not start");
    return;
  }
  if (how == DIR_OPEN_NONE) { say("nothing here opens that: o shows its bytes"); return; }
  if (api->run(app, path) == 0) api->fmt(F.status, sizeof F.status, "%s %s", app, e->name);
  else api->fmt(F.status, sizeof F.status, "no %s app", app);
  damage_footer();
}

/* Whatever it is, as text: a .c file opens in the editor rather than being
 * run, and a picture shows its bytes, which is sometimes what you wanted. */
static void open_in_editor(void) {
  char path[PATH_MAX];
  if (!F.d.n || at(F.sel)->is_dir) return;
  dir_path(api, &F.d, F.sel, path, sizeof path);
  if (api->run("edit", path) != 0) say("no edit app");
}

/* ---- the operations -------------------------------------------------------- */

static void begin_ask(int what) {
  if (what != ASK_NEWDIR && !F.d.n) return;
  F.ask = what;
  F.buf[0] = 0;
  F.buf_len = 0;
  if (what == ASK_RENAME) {
    api->fmt(F.buf, sizeof F.buf, "%s", at(F.sel)->name);
    F.buf_len = (int)api->str_len(F.buf);
  }
  damage_footer();
}

static void cancel_ask(void) {
  if (F.ask == ASK_NONE) return;
  F.ask = ASK_NONE;
  damage_footer();
}

static void finish_ask(void) {
  int what = F.ask;

  F.ask = ASK_NONE;
  damage_footer();
  if (what == ASK_NEWDIR && F.buf_len) {
    say(dir_mkdir(api, &F.d, F.buf));
    relisted();
  } else if (what == ASK_RENAME && F.buf_len && F.d.n) {
    say(dir_rename(api, &F.d, F.sel, F.buf));
    relisted();
  }
}

static void mark_for_move(void) {
  if (!F.d.n) return;
  dir_path(api, &F.d, F.sel, F.marked, sizeof F.marked);
  damage_footer();                     /* the footer says what is held */
}

static void paste_here(void) {
  if (!F.marked[0]) { say("nothing marked: m marks one"); return; }
  say(dir_move_here(api, &F.d, F.marked));
  F.marked[0] = 0;
  relisted();
}

static void ask_delete(void) {
  if (!F.d.n) return;
  F.ask = ASK_DELETE;
  damage_footer();
}

static void delete_selected(void) {
  F.ask = ASK_NONE;
  if (!F.d.n) return;
  say(dir_delete(api, &F.d, F.sel));
  relisted();
}

/* Moving the selection: the row it left and the row it arrived at. */
static void select_row(int idx) {
  if (idx < 0 || idx >= F.d.n || idx == F.sel) return;
  damage_row(F.sel);
  F.sel = idx;
  damage_row(F.sel);
  /* Scrolling changes every row, so if the new selection is off-screen the
   * marks above are not enough -- mark the whole listing. */
  if (F.sel < F.top || F.sel >= F.top + F.rows) damage_all();
}

/* ---- the action table -------------------------------------------------------
 *
 * The menus (fn-b, or a mouse) and the keys reach the same place. None of
 * these has a chord: a chord is matched before the key handler in every view,
 * and the letters must type into a name while one is being asked for.
 *
 * `ls`: what is in a folder, for a sentence or an AI to read -- folders with a
 * slash, files with their size. Read straight off the card, so it answers the
 * same with this app closed. */
enum {
  ACT_LS = 1, ACT_OPEN, ACT_EDITOR, ACT_NEWDIR, ACT_RENAME, ACT_DELETE,
  ACT_MARK, ACT_PASTE, ACT_UP, ACT_REREAD
};

static const CappParam P_PATH[] = { { "path", CAPP_ARG_TEXT, "a folder, like /home" } };

static const CappAction ACTIONS[] = {
  { "open",    "Open",           "File", 0, ACT_OPEN },
  { "editor",  "Open in Edit",   "File", 0, ACT_EDITOR },
  { "newdir",  "New folder",     "File", 0, ACT_NEWDIR },
  { "rename",  "Rename",         "File", 0, ACT_RENAME },
  { "delete",  "Delete",         "File", 0, ACT_DELETE },
  { "mark",    "Move...",        "Edit", 0, ACT_MARK },
  { "paste",   "Paste here",     "Edit", 0, ACT_PASTE },
  { "up",      "Up a folder",    "View", 0, ACT_UP },
  { "reread",  "Read again",     "View", 0, ACT_REREAD },
  { "ls", "List", 0, 0, ACT_LS, "what is in a folder, with sizes", P_PATH, 1, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static const TbIcon ICONS[] = { { "+", ACT_NEWDIR }, { "^", ACT_UP } };

/* The one place that knows what anything does. A menu item can arrive while
 * a name is being typed (the bar takes the keyboard from the prompt), and
 * then the prompt is abandoned, as clicking away from a dialogue would. */
static int do_action(int a) {
  if (a != ACT_LS) cancel_ask();
  switch (a) {
  case ACT_OPEN:   enter_selected(); return 1;
  case ACT_EDITOR: open_in_editor(); return 1;
  case ACT_NEWDIR: begin_ask(ASK_NEWDIR); return 1;
  case ACT_RENAME: begin_ask(ASK_RENAME); return 1;
  case ACT_DELETE: ask_delete(); return 1;
  case ACT_MARK:   mark_for_move(); return 1;
  case ACT_PASTE:  paste_here(); return 1;
  case ACT_UP:     go_up(); return 1;
  case ACT_REREAD: reload(); say("read again"); return 1;
  default: return 0;
  }
}

/* ---- painting -------------------------------------------------------------- */

/* The bottom line: what is being asked, else what just happened, else what
 * is held for moving, else the keys. One footer (apps/footer.h), so it looks
 * like every other app's. */
static void paint_footer(CRect c) {
  char line[FOOT_CHARS + 1];
  short y = (short)(c.y + c.h - FOOT_H + 2);

  if (F.ask == ASK_NEWDIR || F.ask == ASK_RENAME) {
    const char *label = F.ask == ASK_NEWDIR ? "folder: " : "name: ";
    int lw = (int)api->str_len(label), room = FOOT_CHARS - lw - 1;
    /* The end of a long name, since that is where the typing is. */
    int from = F.buf_len > room ? F.buf_len - room : 0;
    short x = (short)(c.x + 4 + lw * 6);
    footer_paint(api, c, label);
    api->text(x, y, F.buf + from, CLR_FG, FOOT_BG);
    api->fill(capp_rect(x + (F.buf_len - from) * 6, y, 5, 8), CLR_FG);
  } else if (F.ask == ASK_DELETE) {
    /* The name cut short enough that the question still fits. */
    char t[24];
    api->fmt(t, sizeof t, "%s", at(F.sel)->name);
    footer_paint(api, c, 0);
    api->fmt(line, sizeof line, "delete %s? y / n", t);
    api->text((short)(c.x + 4), y, line, CLR_ERR, FOOT_BG);
  } else if (F.status[0]) {
    api->fmt(line, sizeof line, "%s", F.status);
    footer_paint(api, c, line);
  } else if (F.marked[0]) {
    footer_paint(api, c, 0);
    api->fmt(line, sizeof line, "v paste %s here", dir_leaf(F.marked));
    api->text((short)(c.x + 4), y, line, CLR_MARK, FOOT_BG);
  } else {
    footer_paint(api, c, "enter open  n new  e rename  d delete");
  }
}

static void app_paint(void *st, CRect full) {
  CRect c;
  int top, list_h, i;
  (void)st;

  /* Only the dropdown moved: draw it and nothing else. Repainting the list
   * underneath first is what makes a menu flicker. */
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  c = toolbar_rest(full);

  /* The rectangles are needed by click and damage, and only paint is told
   * them, so paint records them. */
  F.full = full;
  F.at = c;
  F.have_at = 1;

  top = c.y + BAR_H;
  list_h = c.h - BAR_H - FOOT_H;
  F.rows = list_h / ROW_H;
  if (F.rows < 1) F.rows = 1;
  if (F.sel < F.top) F.top = F.sel;
  if (F.sel >= F.top + F.rows) F.top = F.sel - F.rows + 1;

  /* The path, always. Knowing where you are is most of a file manager. */
  api->fill(capp_rect(c.x, c.y, c.w, BAR_H), CLR_BAR);
  api->text((short)(c.x + 2), (short)(c.y + 1), F.d.cwd, CLR_FG, CLR_BAR);

  api->fill(capp_rect(c.x, top, c.w, list_h), CLR_BG);
  for (i = 0; i < F.rows; i++) {
    int idx = F.top + i;
    short y = (short)(top + i * ROW_H);
    const CappEntry *e;
    uint16_t fg, bg;

    if (idx >= F.d.n) break;
    e = at(idx);

    bg = idx == F.sel ? CLR_SEL : CLR_BG;
    if (idx == F.sel) api->fill(capp_rect(c.x, y, c.w, ROW_H), CLR_SEL);
    fg = e->is_dir ? CLR_DIR : CLR_FG;

    /* A folder gets a slash rather than an icon: at nine pixels a row, one
     * character says it more clearly than four pixels of drawing. */
    api->text((short)(c.x + 3), (short)(y + 1), e->is_dir ? "/" : " ", fg, bg);
    api->text((short)(c.x + 11), (short)(y + 1), e->name, fg, bg);

    if (!e->is_dir) {
      char size[12];
      /* Kilobytes past a kilobyte: a column of four-digit byte counts is
       * harder to read than the thing it is measuring. */
      if (e->size < 1024) api->fmt(size, sizeof size, "%luB", (unsigned long)e->size);
      else api->fmt(size, sizeof size, "%luK", (unsigned long)(e->size / 1024));
      api->text((short)(c.x + c.w - 6 * (int)api->str_len(size) - 3), (short)(y + 1),
                size, CLR_DIM, bg);
    }
  }

  if (!F.d.n)
    api->text((short)(c.x + 6), (short)(top + 6), "(empty)", CLR_DIM, CLR_BG);

  paint_footer(c);

  /* Last: a dropdown is drawn over the list it covers. */
  toolbar_paint_menu(full);
}

/* ---- keys ------------------------------------------------------------------ */

static int key_prompt(unsigned char k) {
  if (k == CAPP_KEY_ENTER) { finish_ask(); return 1; }
  if (k == CAPP_KEY_ESC) { cancel_ask(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (F.buf_len) { F.buf[--F.buf_len] = 0; damage_footer(); }
    else cancel_ask();
    return 1;
  }
  if (k >= ' ' && k < 0x7F && F.buf_len < CAPP_NAME_MAX) {
    F.buf[F.buf_len++] = (char)k;
    F.buf[F.buf_len] = 0;
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

  /* Asked to delete: y does it; n, Escape or Backspace is a no; anything
   * else, a held d's repeat included, leaves the question up. */
  if (F.ask == ASK_DELETE) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) delete_selected();
    else if (a == CONFIRM_NO) cancel_ask();
    return 1;
  }
  if (F.ask != ASK_NONE) return key_prompt(k);

  switch (k) {
  case CAPP_KEY_UP:    select_row(F.sel - 1); return 1;
  case CAPP_KEY_DOWN:  select_row(F.sel + 1); return 1;
  case CAPP_KEY_LEFT:
  case CAPP_KEY_BACK:  go_up(); return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_ENTER: return do_action(ACT_OPEN);
  case 'n': case 'N':  return do_action(ACT_NEWDIR);
  case 'e': case 'E':  return do_action(ACT_RENAME);
  case 'm': case 'M':  return do_action(ACT_MARK);
  case 'v': case 'V':  return do_action(ACT_PASTE);
  case 'd': case 'D':
  case KEY_DEL:        return do_action(ACT_DELETE);
  case 'o': case 'O':  return do_action(ACT_EDITOR);
  case 'r': case 'R':  return do_action(ACT_REREAD);
  case CAPP_KEY_ESC:
    /* A file held for moving is a step in, so Escape lets go of it. With
     * nothing held this is the top level, which keeps Escape. */
    if (!F.marked[0]) return 0;
    F.marked[0] = 0;
    damage_footer();
    return 1;
  default: return 0;
  }
}

/* ---- mouse ------------------------------------------------------------------ */

/* The first sign of a mouse brings the menu bar, which moves everything down
 * by its height -- the whole window, whatever else was marked. */
static void saw_mouse(void) {
  if (toolbar_saw_mouse() && F.have_at) api->damage(F.full);
}

static int app_click(void *st, short x, short y, int button) {
  int a;
  (void)st;

  if (!F.have_at) return 1;
  saw_mouse();
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;             /* opened or closed a menu */
  if (a != TB_NONE) return do_action(a);
  y = (short)(y - toolbar_h());
  unsay();

  /* A prompt takes the screen while it is open: a click anywhere cancels,
   * which is what clicking away from a dialogue means everywhere else. */
  if (F.ask != ASK_NONE) { cancel_ask(); return 1; }

  if (y < BAR_H) { go_up(); return 1; }        /* the path bar walks up */
  if (y >= F.at.h - FOOT_H) return 1;

  {
    int row = (y - BAR_H) / ROW_H;
    int idx = F.top + row;
    if (idx < 0 || idx >= F.d.n) return 1;

    /* Second click on the same row opens it -- a double-click without the
     * timing, which on a device with one pointer and no drag is the same
     * gesture and easier to hit. The right button opens straight away. */
    if (idx == F.sel || button == CAPP_BTN_RIGHT) enter_selected();
    else select_row(idx);
  }
  return 1;
}

static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  int changed = 0;
  (void)st; (void)buttons;
  if (!F.have_at) return 0;
  if (toolbar_saw_mouse()) { api->damage(F.full); changed = 1; }
  if (toolbar_hover(x, y)) changed = 1;
  if (wheel) {
    int want = F.sel - wheel;
    if (want < 0) want = 0;
    if (want >= F.d.n) want = F.d.n ? F.d.n - 1 : 0;
    select_row(want);
    changed = 1;
  }
  return changed;
}

static int app_wants_text(void *st) {
  (void)st;
  /* Not while the menu has the keyboard: a spoken sentence would otherwise
   * type into a name that is not listening. */
  if (toolbar_has_keys()) return 0;
  return F.ask == ASK_NEWDIR || F.ask == ASK_RENAME;
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
  0,      /* a window on the desktop, fullscreen from the launcher */
  "Files",
  /* 16x16: a folder with a tab. */
  { 0x00, 0x00, 0x78, 0x00, 0x84, 0x00, 0xFF, 0xFC,
    0x80, 0x04, 0x80, 0x04, 0x80, 0x04, 0x80, 0x04,
    0x80, 0x04, 0x80, 0x04, 0x80, 0x04, 0x80, 0x04,
    0x80, 0x04, 0xFF, 0xFC, 0x00, 0x00, 0x00, 0x00 },
  "up down\tmove\nenter right\topen; a folder goes in\n"
  "left backspace\tup a folder\nn\tnew folder\ne\trename\n"
  "d\tdelete: y yes, n or esc no\nm\tmark a file to move\nv\tpaste it here\n"
  "esc\tlet go of the marked file\no\topen in the editor\n"
  "r\tread the folder again\nfn-b\tthe menus\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  api->mem_set(&F, 0, sizeof F);

  /* Somewhere useful by default, and wherever you say if you say. */
  api->fmt(F.d.cwd, sizeof F.d.cwd, "%s", CAPP_HOME);
  if (argc > 1 && argv[1][0]) {
    CappStat st;
    if (api->stat(argv[1], &st) == 0 && st.is_dir)
      api->fmt(F.d.cwd, sizeof F.d.cwd, "%s", argv[1]);
  }
  reload();
  toolbar_init(api, ACTIONS, NACT, ICONS, 2);

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
