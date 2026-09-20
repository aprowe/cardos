/* The file picker's panel and its card. See picker.h. */
#include "kernel/ui/picker.h"
#include "kernel/ui/draw.h"
#include "kernel/fs/fs.h"

#include <stdio.h>
#include <string.h>

/* Layout, top to bottom, on 240x135: a title bar, the folder, nine rows, and
 * two lines at the bottom -- the field or the question, then the keys. */
#define TITLE_H   12
#define PATH_Y    TITLE_H
#define PATH_H    10
#define LIST_Y    (PATH_Y + PATH_H)
#define ROW_H     10
#define ROWS      9
#define LIST_H    (ROWS * ROW_H)
#define FOOT_Y    (LIST_Y + LIST_H)
#define FOOT_H    (DISPLAY_H - FOOT_Y)
#define PAD       3
#define SIZE_W    36                      /* the right-hand column */

#define P_TITLE   C_TITLE
#define P_TITLEFG C_TITLE_FG
#define P_BG      C_WHITE
#define P_TEXT    C_TEXT
#define P_DIM     C_SHADOW
#define P_SEL     C_TITLE
#define P_SELFG   C_WHITE
#define P_PATHBG  C_FACE
#define P_FOOTBG  C_FACE
#define P_FIELDBG C_WHITE
#define P_WARN    C_RED

static PickModel s_m;
static int       s_active;
static int       s_answer;      /* 1 chosen, 0 cancelled: waiting to be polled */
static int       s_has_answer;

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* ---- the card, as the model wants it ------------------------------------- */

static int card_list(const char *dir, PmEntry *out, int max) {
  FsDir d;
  FsEntry e;
  int n = 0;
  if (!fs_mounted()) return -1;
  if (fs_opendir(dir, &d) != 0) return -1;
  while (n < max && fs_readdir(&d, &e) == 1) {
    snprintf(out[n].name, sizeof out[n].name, "%.*s", (int)sizeof out[n].name - 1, e.name);
    out[n].size = e.size;
    out[n].is_dir = (uint8_t)(e.is_dir != 0);
    out[n].synthetic = 0;
    n++;
  }
  fs_closedir(&d);
  return n;
}

static int card_exists(const char *path, int *is_dir) {
  FsStat st;
  if (!fs_mounted()) return 0;
  if (!strcmp(path, "/")) { if (is_dir) *is_dir = 1; return 1; }
  if (fs_stat(path, &st) != 0) return 0;
  if (is_dir) *is_dir = st.is_dir;
  return 1;
}

static const PmFs CARD = { card_list, fs_mkdir, fs_remove, fs_rename, card_exists };

/* ---- painting ------------------------------------------------------------ */

static void size_text(uint32_t n, char *out, size_t size) {
  if (n < 1000) snprintf(out, size, "%u", (unsigned)n);
  else if (n < 1000000) snprintf(out, size, "%uK", (unsigned)(n / 1000));
  else snprintf(out, size, "%uM", (unsigned)(n / 1000000));
}

static void paint_title(void) {
  const char *mode = s_m.mode == PM_SAVE ? "save" : s_m.mode == PM_FOLDER ? "folder" : "open";
  draw_rect(R(0, 0, DISPLAY_W, TITLE_H), P_TITLE);
  draw_text(PAD, 2, s_m.title, P_TITLEFG, P_TITLE);
  draw_text((int16_t)(DISPLAY_W - PAD - draw_text_width(mode)), 2, mode, C_DESK_DIM, P_TITLE);
}

static void paint_path(void) {
  char count[12];
  draw_rect(R(0, PATH_Y, DISPLAY_W, PATH_H), P_PATHBG);
  snprintf(count, sizeof count, "%d%s", s_m.n - (s_m.mode == PM_FOLDER ? 1 : 0),
           s_m.truncated ? "+" : "");
  draw_text_ellipsis(PAD, PATH_Y + 1, (int16_t)(DISPLAY_W - 2 * PAD - 30), s_m.dir, P_TEXT, P_PATHBG);
  draw_text((int16_t)(DISPLAY_W - PAD - draw_text_width(count)), PATH_Y + 1, count, P_DIM, P_PATHBG);
}

static void paint_row(int i) {
  int y = LIST_Y + (i - s_m.top) * ROW_H;
  int selected = i == s_m.sel && s_m.ask != PM_ASK_NAME;
  uint16_t bg = selected ? P_SEL : P_BG;
  uint16_t fg = selected ? P_SELFG : P_TEXT;
  const PmEntry *e = &s_m.ent[i];
  Rect r = R(0, y, DISPLAY_W, ROW_H);

  draw_rect(r, bg);
  if (e->synthetic) {
    draw_text(PAD, (int16_t)(y + 1), e->name, selected ? fg : P_DIM, bg);
    return;
  }
  if (e->is_dir) {
    /* A small tab-and-body folder, so a folder reads as one at a glance. */
    draw_rect(R(PAD, y + 2, 4, 2), fg);
    draw_rect(R(PAD, y + 3, 9, 5), fg);
    draw_text_ellipsis(PAD + 12, (int16_t)(y + 1), (int16_t)(DISPLAY_W - PAD - 12 - PAD), e->name, fg, bg);
  } else {
    char sz[12];
    size_text(e->size, sz, sizeof sz);
    draw_text_ellipsis(PAD + 12, (int16_t)(y + 1), (int16_t)(DISPLAY_W - PAD - 12 - SIZE_W), e->name, fg, bg);
    draw_text((int16_t)(DISPLAY_W - PAD - draw_text_width(sz)), (int16_t)(y + 1), sz,
              selected ? fg : P_DIM, bg);
  }
}

static void paint_list(void) {
  int i;
  draw_rect(R(0, LIST_Y, DISPLAY_W, LIST_H), P_BG);
  if (!s_m.n) {
    draw_text(PAD + 12, LIST_Y + 2, "(empty)", P_DIM, P_BG);
    return;
  }
  for (i = s_m.top; i < s_m.n && i < s_m.top + ROWS; i++) paint_row(i);
}

static const char *keys_line(void) {
  /* A refusal while a field is open ("that name is taken") has nowhere else
   * to go: the field has the prompt line. */
  if (s_m.note[0] && s_m.ask != PM_ASK_NONE) return s_m.note;
  switch (s_m.ask) {
  case PM_ASK_NAME:      return "enter save  tab list  esc cancel";
  case PM_ASK_MKDIR:
  case PM_ASK_RENAME:    return "enter ok  esc back";
  case PM_ASK_DELETE:
  case PM_ASK_OVERWRITE: return "y yes  n no";
  default:
    if (s_m.mode == PM_SAVE) return "enter open  bksp up  tab name  ^n ^r del";
    return "enter open  bksp up  ^n new  ^r ren  del";
  }
}

static void paint_foot(void) {
  const char *label = "", *text = pm_prompt(&s_m, &label);
  int y = FOOT_Y + 2;
  int asking = s_m.ask != PM_ASK_NONE;
  int typing = s_m.ask == PM_ASK_NAME || s_m.ask == PM_ASK_MKDIR || s_m.ask == PM_ASK_RENAME;

  draw_rect(R(0, FOOT_Y, DISPLAY_W, FOOT_H), P_FOOTBG);
  if (asking) {
    int16_t lx = (int16_t)(PAD + draw_text_width(label) + 6);
    draw_text(PAD, (int16_t)y, label, P_TEXT, P_FOOTBG);
    if (typing) {
      char shown[PM_NAME_MAX + 2];
      Rect f = R(lx - 2, y - 2, DISPLAY_W - lx - PAD + 2, 11);
      draw_rect(f, P_FIELDBG);
      draw_frame(f, P_DIM);
      snprintf(shown, sizeof shown, "%s_", text);
      draw_text_ellipsis(lx, (int16_t)y, (int16_t)(f.w - 4), shown, P_TEXT, P_FIELDBG);
    } else {
      draw_text_ellipsis(lx, (int16_t)y, (int16_t)(DISPLAY_W - lx - PAD), text, P_WARN, P_FOOTBG);
    }
  } else if (text[0]) {
    draw_text_ellipsis(PAD, (int16_t)y, (int16_t)(DISPLAY_W - 2 * PAD), text, P_WARN, P_FOOTBG);
  } else {
    draw_text_ellipsis(PAD, (int16_t)y, (int16_t)(DISPLAY_W - 2 * PAD),
                       s_m.mode == PM_FOLDER ? "enter on the top row picks this folder"
                                             : "type a name to jump to it", P_DIM, P_FOOTBG);
  }
  draw_text_ellipsis(PAD, (int16_t)(FOOT_Y + 13), (int16_t)(DISPLAY_W - 2 * PAD), keys_line(),
                     (s_m.note[0] && s_m.ask != PM_ASK_NONE) ? P_WARN : P_DIM, P_FOOTBG);
}

void picker_paint_now(void) {
  if (!s_active) return;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  paint_title();
  paint_path();
  paint_list();
  paint_foot();
  s_m.dirty = 0;
}

void picker_paint(void) {
  if (s_active && s_m.dirty) picker_paint_now();
}

/* ---- the API ------------------------------------------------------------- */

int picker_open(int mode, const char *title, const char *dir,
                const char *filter, const char *name) {
  if (s_active) return -1;
  pm_begin(&s_m, &CARD, mode, title, dir, filter, name);
  s_m.rows = ROWS;
  s_active = 1;
  s_has_answer = 0;
  s_m.dirty = 1;
  picker_paint_now();
  return 0;
}

int picker_active(void) { return s_active; }

static int finished(void) {
  if (!s_m.done) return 0;
  s_active = 0;
  s_answer = s_m.done == 1;
  s_has_answer = 1;
  return 1;
}

int picker_poll(char *out, size_t n) {
  if (!s_has_answer) return PICKER_PENDING;
  s_has_answer = 0;
  if (s_answer && out && n) snprintf(out, n, "%s", s_m.result);
  return s_answer;
}

int picker_key(uint8_t k, uint32_t now_ms) {
  if (!s_active) return 0;
  pm_key(&s_m, k, now_ms);
  if (finished()) return 1;
  picker_paint();
  return 0;
}

int picker_click(int16_t x, int16_t y, int button) {
  (void)x; (void)button;
  if (!s_active) return 0;
  if (y >= LIST_Y && y < FOOT_Y) {
    pm_click(&s_m, (y - LIST_Y) / ROW_H);
    if (finished()) return 1;
    picker_paint();
  } else if (y >= FOOT_Y && s_m.mode == PM_SAVE && s_m.ask == PM_ASK_NONE) {
    pm_key(&s_m, 0x09, 0);                    /* tab: into the name field */
    picker_paint();
  }
  return 0;
}

void picker_wheel(int dy) {
  if (!s_active) return;
  pm_scroll(&s_m, dy);
  picker_paint();
}

void picker_close(void) {
  if (!s_active) return;
  s_m.done = -1;
  finished();
}

const char *picker_help(void) {
  return "arrows\tmove\nenter\topen folder, or choose\nbackspace\tup a folder\n"
         "letters\tjump to a name\ntab\tthe name field (save)\nctrl-n\tnew folder\n"
         "ctrl-r\trename\ndelete\tdelete (asks)\nctrl-a\tshow hidden files\n"
         "escape\tcancel\n";
}
