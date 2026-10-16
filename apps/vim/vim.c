/* Vim: a small modal code editor, after Neovim.
 *
 * Normal, insert, visual (by character and by line) and command-line
 * modes; vi's grammar of [count] operator [count] motion; undo and redo
 * by change; the dot; search; the ex commands people type every day;
 * syntax colours for C, Python and Markdown; completion from the buffer
 * and the language's keywords. The editing is apps/vim/vimcore.h, which
 * the host suite drives a key at a time (test/test_vim.c); this file is the
 * memory, the pixels and the OS's file picker.
 *
 * Memory. The text is a gap buffer, the undo log and the register blocks
 * of their own. The 28 KB arena holds a 20 KB pool; on its own that is
 * 14 KB of text, 4 KB of undo and 2 KB of register -- a page or two of
 * code. So Vim is a big app (CAPP_BIG): at launch it asks the heap for
 * one block, as much as big_room offers less 4 KB, up to 96 KB, and the
 * text goes there while the whole pool becomes 12 KB of undo and 8 KB of
 * register. On firmware without big-app mode, or with no room, it keeps
 * the pool's split and says how big a file it can hold. A file that does
 * not fit is refused, never opened in part.
 *
 * Painting. The screen is 13 rows of 40 characters in the 6x8 font, a
 * status line, and the command line. vi_layout (after every key) leaves
 * the syntax state at the start of each row, so a paint colours only the
 * rows it draws and never scans the file. After a key the app marks what
 * changed -- the rows the cursor left and reached, the lines an edit
 * touched, the status and command lines -- and the shell repaints only
 * those; a scroll, a change of line count, a selection or the completion
 * list repaints all of it, which the OS composes off the panel.
 *
 * Keys. Vim takes text in every mode (wants_text), so h j k l ; , . /
 * arrive as themselves, and fn with ; , . / are the arrows. Esc (the `
 * key) is vim's Esc; fn-` leaves the app, as everywhere. An app cannot
 * close itself, so :q closes the file and opens the picker for the next.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/vim/vimcore.h"

static const CardApi *api;

#define CHW   6
#define ROWH  9
#define BARH  9

/* The arena's pool, and how it is divided. */
#define POOL       (20 * 1024)
#define POOL_TEXT  (14 * 1024)
#define POOL_UNDO  (4 * 1024)
#define BIG_UNDO   (12 * 1024)
#define BIG_MIN    (24 * 1024)
#define BIG_MAX    (96 * 1024)

static char s_pool[POOL];
static Vim  V;

#define C_BG       CAPP_RGB(24, 26, 32)
#define C_CURLINE  CAPP_RGB(36, 40, 50)
#define C_GUTTER   CAPP_RGB(30, 33, 40)
#define C_LINENO   CAPP_RGB(92, 102, 120)
#define C_LINENO_C CAPP_RGB(229, 192, 123)
#define C_TILDE    CAPP_RGB(70, 90, 130)
#define C_TEXT     CAPP_RGB(214, 220, 232)
#define C_KW       CAPP_RGB(198, 120, 221)
#define C_TYPE     CAPP_RGB(229, 192, 123)
#define C_STR      CAPP_RGB(152, 195, 121)
#define C_COM      CAPP_RGB(110, 120, 136)
#define C_NUM      CAPP_RGB(209, 154, 102)
#define C_PRE      CAPP_RGB(224, 108, 117)
#define C_HEAD     CAPP_RGB(97, 175, 239)
#define C_CODE     CAPP_RGB(86, 182, 194)
#define C_CURSOR   CAPP_RGB(220, 224, 232)
#define C_BAR      CAPP_RGB(120, 200, 255)
#define C_SEL      CAPP_RGB(52, 80, 116)
#define C_STATUS   CAPP_RGB(40, 44, 54)
#define C_STATUSFG CAPP_RGB(200, 205, 215)
#define C_ERR      CAPP_RGB(240, 110, 110)
#define C_POP      CAPP_RGB(52, 57, 70)
#define C_POPSEL   CAPP_RGB(97, 175, 239)
#define C_DIRTY    CAPP_RGB(255, 190, 90)

static const uint16_t HL_FG[HL_COUNT] = {
  C_TEXT, C_KW, C_TYPE, C_STR, C_COM, C_NUM, C_PRE, C_HEAD, C_CODE, C_TYPE
};

static int s_bigblock;               /* the text is in a big-mode block */

/* What the last paint was given, for damage after a key. */
static CRect s_shown = { 0, 0, 240, 135 };

enum { PICK_NONE = 0, PICK_OPEN, PICK_SAVE };
static int s_picking;

/* --------------------------------------------------------------- memory --- */

static void setup_memory(void) {
  char *big = 0;
  size_t room = 0, want;
  if (api->version >= 47 && api->big_room && api->big_alloc) room = api->big_room();
  want = room > 4096 ? room - 4096 : 0;
  if (want > BIG_MAX) want = BIG_MAX;
  if (want >= BIG_MIN) big = (char *)api->big_alloc(want);
  if (big) {
    s_bigblock = 1;
    vi_init(&V, big, (int)want, s_pool, BIG_UNDO, s_pool + BIG_UNDO, POOL - BIG_UNDO);
  } else {
    vi_init(&V, s_pool, POOL_TEXT, s_pool + POOL_TEXT, POOL_UNDO,
            s_pool + POOL_TEXT + POOL_UNDO, POOL - POOL_TEXT - POOL_UNDO);
  }
  {
    char m[64];
    api->fmt(m, sizeof m, "vim: %d KB of text (%s), room %u", V.t.cap / 1024,
             big ? "big block" : "arena", (unsigned)room);
    api->log(m);
  }
}

/* ---------------------------------------------------------------- files --- */

static const char *own_dir(char *buf, int n) {
  int i, cut = 0;
  for (i = 0; V.path[i]; i++) if (V.path[i] == '/') cut = i;
  if (cut <= 0 || cut >= n) return 0;
  for (i = 0; i < cut; i++) buf[i] = V.path[i];
  buf[cut] = 0;
  return buf;
}

static void ask_pick(int mode) {
  char dir[VI_PATH];
  CappPick req;
  req.mode = mode == PICK_OPEN ? CAPP_PICK_OPEN : CAPP_PICK_SAVE;
  req.title = mode == PICK_OPEN ? "Open" : "Write";
  req.dir = own_dir(dir, sizeof dir);
  req.filter = 0;
  req.name = mode == PICK_SAVE ? (V.path[0] ? vi_name(V.path) : "untitled.txt") : 0;
  if (api->pick(&req) == 0) s_picking = mode;
  else vi_say(&V, 1, "a picker is already open");
}

/* :q. The OS lets nothing but fn-` close an app, so the file closes and the
 * picker offers the next one; cancelled, an empty buffer. */
static void close_file(void) {
  vi_reset(&V);
  V.path[0] = 0;
  V.lang = LANG_PLAIN;
  V.quit_after = 0;
  vi_say(&V, 0, "closed -- fn-` leaves Vim");
  ask_pick(PICK_OPEN);
}

static void take_request(void) {
  int r = V.req;
  V.req = VREQ_NONE;
  if (r == VREQ_OPEN) ask_pick(PICK_OPEN);
  else if (r == VREQ_SAVEAS) ask_pick(PICK_SAVE);
  else if (r == VREQ_QUIT) close_file();
}

/* -------------------------------------------------------------- painting --- */

static int rows_for(CRect c) {
  int r = (c.h - 2 * BARH) / ROWH;
  if (r > VI_ROWS_MAX) r = VI_ROWS_MAX;
  return r < 1 ? 1 : r;
}

/* One run of same-coloured cells. */
static void draw_run(int x, int y, const char *s, int n, uint16_t fg, uint16_t bg) {
  char buf[48];
  int i;
  if (n > 47) n = 47;
  for (i = 0; i < n; i++) buf[i] = s[i];
  buf[n] = 0;
  api->text((int16_t)x, (int16_t)y, buf, fg, bg);
}

/* The selection, as positions [s, e) or lines. */
static int in_sel(int pos, int L) {
  int a = V.vanchor < V.cur ? V.vanchor : V.cur, b = V.vanchor < V.cur ? V.cur : V.vanchor;
  if (V.mode == VM_VLINE) return L >= vb_line_of(&V.t, a) && L <= vb_line_of(&V.t, b);
  if (V.mode == VM_VISUAL) return pos >= a && pos <= b;
  return 0;
}

static void paint_row(CRect c, int r) {
  int y = c.y + r * ROWH, n, L, g = vi_gutter(&V), gx = g * CHW, cols = vi_cols(&V);
  int curL = vi_line(&V), i, vc = 0, cell, ls, curvc = -1, vis;
  char ch[64];
  uint16_t fg[64], bg[64], rowbg;
  L = vi_row(&V, r, &n);
  vis = V.mode == VM_VISUAL || V.mode == VM_VLINE;
  rowbg = (L == curL && !vis) ? C_CURLINE : C_BG;

  /* the gutter: the number, and round it */
  if (g) {
    if (L >= 0) {
      char num[12];
      api->fmt(num, sizeof num, "%*d ", g - 1, L + 1);
      api->text((int16_t)c.x, (int16_t)y, num, L == curL ? C_LINENO_C : C_LINENO, C_GUTTER);
    } else {
      api->fill(capp_rect(c.x, y, gx, 8), C_GUTTER);
    }
    api->fill(capp_rect(c.x, y + 8, gx, ROWH - 8), C_GUTTER);
  }

  if (L < 0) {                                 /* past the end: vim's tildes */
    api->text((int16_t)(c.x + gx), (int16_t)y, "~", C_TILDE, C_BG);
    api->fill(capp_rect(c.x + gx + CHW, y, c.w - gx - CHW, 8), C_BG);
    api->fill(capp_rect(c.x + gx, y + 8, c.w - gx, ROWH - 8), C_BG);
    return;
  }

  /* The cells on screen: the line from V.left, tabs opened out. */
  if (cols > 64) cols = 64;
  for (i = 0; i < cols; i++) { ch[i] = ' '; fg[i] = C_TEXT; bg[i] = rowbg; }
  ls = vb_line_start(&V.t, L);
  if (L == curL) curvc = vi_vcol(&V, V.cur);
  for (i = 0; i < n; i++) {
    int c0 = (unsigned char)V.lb[i], w = 1, k;
    if (c0 == '\t') w = V.ts - vc % V.ts;
    for (k = 0; k < w; k++) {
      cell = vc + k - V.left;
      if (cell < 0 || cell >= cols) continue;
      ch[cell] = (c0 == '\t' || c0 < 32 || c0 > 126) ? (c0 == '\t' ? ' ' : '?') : (char)c0;
      fg[cell] = HL_FG[V.lc[i] < HL_COUNT ? V.lc[i] : 0];
      if (vis && in_sel(ls + i, L)) bg[cell] = C_SEL;
    }
    vc += w;
    if (vc - V.left >= cols) break;
  }
  /* A selection that takes a line's end shows one cell of it. */
  if (vis && in_sel(ls + n, L) && n < VI_LB) {
    cell = vc - V.left;
    if (cell >= 0 && cell < cols && (V.mode == VM_VLINE || vb_at(&V.t, ls + n) == '\n')) bg[cell] = C_SEL;
  }
  /* The block cursor, everywhere but insert mode. */
  if (curvc >= 0 && V.mode != VM_INSERT) {
    cell = curvc - V.left;
    if (cell >= 0 && cell < cols) { fg[cell] = C_BG; bg[cell] = C_CURSOR; }
  }

  /* Same-coloured runs; text paints its own background, so nothing under
   * it is filled first (a fill the text then covers is a visible blink). */
  {
    int a = 0;
    while (a < cols) {
      int b = a + 1;
      while (b < cols && fg[b] == fg[a] && bg[b] == bg[a]) b++;
      draw_run(c.x + gx + a * CHW, y, ch + a, b - a, fg[a], bg[a]);
      a = b;
    }
  }
  if (c.w > gx + cols * CHW)
    api->fill(capp_rect(c.x + gx + cols * CHW, y, c.w - gx - cols * CHW, 8), rowbg);
  api->fill(capp_rect(c.x + gx, y + 8, c.w - gx, ROWH - 8), rowbg);
  if (curvc >= 0 && V.mode == VM_INSERT) {
    cell = curvc - V.left;
    if (cell >= 0 && cell <= cols)
      api->fill(capp_rect(c.x + gx + cell * CHW, y, 1, 8), C_BAR);
  }
}

static uint16_t mode_colour(void) {
  switch (V.mode) {
  case VM_INSERT: return C_STR;
  case VM_VISUAL: case VM_VLINE: return C_KW;
  case VM_CMD: return C_TYPE;
  }
  return C_HEAD;
}

static void paint_status(CRect c, int y) {
  char left[48], right[24], mode[12];
  const char *name = V.path[0] ? vi_name(V.path) : "[No Name]";
  int L = vi_line(&V), last = V.t.nl, lw, rw, mw, x;
  api->fmt(mode, sizeof mode, " %s ", vi_mode_name(&V));
  api->fmt(left, sizeof left, " %s%s", name, vi_modified(&V) ? " [+]" : "");
  if (V.top == 0 && V.top + vi_rows(&V) > last) api->fmt(right, sizeof right, "%d:%d  All ", L + 1, vi_col(&V) + 1);
  else if (V.top == 0) api->fmt(right, sizeof right, "%d:%d  Top ", L + 1, vi_col(&V) + 1);
  else if (V.top + vi_rows(&V) > last) api->fmt(right, sizeof right, "%d:%d  Bot ", L + 1, vi_col(&V) + 1);
  else api->fmt(right, sizeof right, "%d:%d  %d%% ", L + 1, vi_col(&V) + 1, L * 100 / (last ? last : 1));
  mw = (int)api->str_len(mode);
  rw = (int)api->str_len(right);
  lw = (int)api->str_len(left);
  if (mw + lw + rw > c.w / CHW) {
    lw = c.w / CHW - mw - rw;
    if (lw < 0) lw = 0;
    left[lw] = 0;
  }
  api->fill(capp_rect(c.x, y, c.w, 1), C_STATUS);
  api->text((int16_t)c.x, (int16_t)(y + 1), mode, C_BG, mode_colour());
  x = c.x + mw * CHW;
  api->text((int16_t)x, (int16_t)(y + 1), left, vi_modified(&V) ? C_DIRTY : C_STATUSFG, C_STATUS);
  x += lw * CHW;
  if (c.x + c.w - rw * CHW > x) api->fill(capp_rect(x, y + 1, c.x + c.w - rw * CHW - x, 8), C_STATUS);
  api->text((int16_t)(c.x + c.w - rw * CHW), (int16_t)(y + 1), right, C_STATUSFG, C_STATUS);
}

/* The command line: what is being typed after : / ?, or the last message,
 * and the keys of a command not finished yet at the right (showcmd). */
static void paint_cmdline(CRect c, int y) {
  char line[48], sc[12];
  int n, x, scn = 0, i;
  uint16_t fg = C_TEXT;
  if (V.mode == VM_CMD) api->fmt(line, sizeof line, "%c%s", V.clk, V.cl + (V.cln > 38 ? V.cln - 38 : 0));
  else if (V.msg[0]) { api->fmt(line, sizeof line, "%s", V.msg); if (V.msg_err) fg = C_ERR; }
  else if (V.mode == VM_INSERT) api->fmt(line, sizeof line, "-- INSERT --");
  else if (V.mode == VM_VISUAL) api->fmt(line, sizeof line, "-- VISUAL --");
  else if (V.mode == VM_VLINE) api->fmt(line, sizeof line, "-- VISUAL LINE --");
  else line[0] = 0;
  if (V.mode != VM_CMD && V.mode != VM_INSERT && (V.count || V.op || V.pend) && V.recording) {
    int a = V.recn > 8 ? V.recn - 8 : 0;
    for (i = a; i < V.recn; i++) {
      int k = V.rec[i];
      sc[scn++] = (char)(k >= 32 && k < 127 ? k : '^');
    }
  }
  sc[scn] = 0;
  n = (int)api->str_len(line);
  if (n > c.w / CHW - scn - 1) { n = c.w / CHW - scn - 1; if (n < 0) n = 0; line[n] = 0; }
  api->fill(capp_rect(c.x, y, c.w, 1), C_BG);
  api->text((int16_t)c.x, (int16_t)(y + 1), line, fg, C_BG);
  x = c.x + n * CHW;
  if (V.mode == VM_CMD) {
    api->fill(capp_rect(x, y + 1, CHW, 8), C_CURSOR);
    x += CHW;
  }
  if (scn) {
    int sx = c.x + c.w - (scn + 1) * CHW;
    if (sx > x) api->fill(capp_rect(x, y + 1, sx - x, 8), C_BG);
    api->text((int16_t)sx, (int16_t)(y + 1), sc, C_STATUSFG, C_BG);
    x = sx + scn * CHW;
  }
  if (c.x + c.w > x) api->fill(capp_rect(x, y + 1, c.x + c.w - x, 8), C_BG);
}

/* Where the completion list goes: under the cursor's row, or over it when
 * there is no room below. */
static CRect popup_rect(CRect c) {
  int g = vi_gutter(&V), r = vi_line(&V) - V.top, w = 0, i, h, x, y;
  for (i = 0; i < V.ncand; i++) {
    int n = vi_strlen(V.cand[i]);
    if (n > w) w = n;
  }
  w = (w + 2) * CHW;
  h = V.ncand * ROWH + 2;
  x = c.x + (g + vi_vcol(&V, V.cs) - V.left) * CHW - CHW;
  if (x + w > c.x + c.w) x = c.x + c.w - w;
  if (x < c.x) x = c.x;
  y = c.y + (r + 1) * ROWH;
  if (y + h > c.y + rows_for(c) * ROWH) y = c.y + r * ROWH - h;
  if (y < c.y) y = c.y;
  return capp_rect(x, y, w, h);
}

static void paint_popup(CRect c) {
  CRect p = popup_rect(c);
  int i;
  api->frame(p, C_LINENO);
  for (i = 0; i < V.ncand; i++) {
    char item[VI_CANDLEN + 4];
    int sel = i == V.csel, n;
    int16_t y = (int16_t)(p.y + 1 + i * ROWH);
    api->fmt(item, sizeof item, " %s", V.cand[i]);
    n = (int)api->str_len(item);
    api->text((int16_t)(p.x + 1), y, item, sel ? C_BG : C_TEXT, sel ? C_POPSEL : C_POP);
    if (p.w - 2 > n * CHW)
      api->fill(capp_rect(p.x + 1 + n * CHW, y, p.w - 2 - n * CHW, 8), sel ? C_POPSEL : C_POP);
    api->fill(capp_rect(p.x + 1, y + 8, p.w - 2, ROWH - 8), C_POP);
  }
}

static void app_paint(void *st, CRect c) {
  CRect area = api->paint_area();
  int rows = rows_for(c), r, sy = c.y + rows * ROWH;
  (void)st;
  s_shown = c;
  for (r = 0; r < rows; r++) {
    int y = c.y + r * ROWH;
    if (y + ROWH <= area.y || y >= area.y + area.h) continue;
    paint_row(c, r);
  }
  if (sy + BARH > area.y && sy < area.y + area.h) paint_status(c, sy);
  sy += BARH;
  if (sy < area.y + area.h) paint_cmdline(c, sy);
  if (sy + BARH < c.y + c.h) api->fill(capp_rect(c.x, sy + BARH, c.w, c.y + c.h - sy - BARH), C_BG);
  if (V.cpop && V.mode == VM_INSERT) paint_popup(c);
}

/* ----------------------------------------------------------------- keys --- */

static void damage_rows(int a, int b) {
  CRect c = s_shown;
  int rows = rows_for(c);
  if (a < 0) a = 0;
  if (b >= rows) b = rows - 1;
  if (b < a) return;
  api->damage(capp_rect(c.x, c.y + a * ROWH, c.w, (b - a + 1) * ROWH));
}

static int app_key(void *st, uint8_t k) {
  int top0 = V.top, left0 = V.left, gut0 = vi_gutter(&V), L0 = vi_line(&V), mode0 = V.mode;
  int pop0 = V.cpop, a0 = vb_line_of(&V.t, V.vanchor), r, same = 1, rows;
  uint8_t st0[VI_ROWS_MAX + 1];
  (void)st;
  rows = vi_rows(&V);
  for (r = 0; r <= rows; r++) st0[r] = V.row_st[r];
  V.chg_lo = 0x7FFFFFFF;
  V.chg_hi = -1;
  V.chg_all = 0;

  vi_key(&V, k);
  if (V.req) take_request();

  for (r = 0; r <= rows; r++) if (st0[r] != V.row_st[r]) same = 0;
  /* Everything, when the screen moved or the shape of the text did. */
  if (!same || V.chg_all || V.top != top0 || V.left != left0 || vi_gutter(&V) != gut0 ||
      pop0 || V.cpop || s_picking)
    return 1;
  {
    int L1 = vi_line(&V), lo = L0 < L1 ? L0 : L1, hi = L0 < L1 ? L1 : L0;
    int vis0 = mode0 == VM_VISUAL || mode0 == VM_VLINE;
    int vis1 = V.mode == VM_VISUAL || V.mode == VM_VLINE;
    if (vis0 || vis1) {
      int a1 = vb_line_of(&V.t, V.vanchor);
      if (a0 < lo) lo = a0;
      if (a0 > hi) hi = a0;
      if (a1 < lo) lo = a1;
      if (a1 > hi) hi = a1;
    }
    damage_rows(lo - V.top, hi - V.top);
    if (V.chg_hi >= 0) damage_rows(V.chg_lo - V.top, V.chg_hi - V.top);
    damage_rows(rows, rows);                        /* status */
    api->damage(capp_rect(s_shown.x, s_shown.y + rows * ROWH, s_shown.w, s_shown.h - rows * ROWH));
  }
  return 1;
}

static int app_tick(void *st, uint32_t now) {
  char path[VI_PATH];
  int r, what;
  (void)st;
  (void)now;
  if (!s_picking) return 0;
  r = api->pick_poll(path, sizeof path);
  if (r == CAPP_PICK_PENDING) return 0;
  what = s_picking;
  s_picking = PICK_NONE;
  if (r == 1) {
    if (what == PICK_OPEN) vi_load(&V, path);
    else if (vi_save(&V, path) == 0 && V.quit_after) close_file();
  }
  V.quit_after = 0;
  vi_layout(&V);
  return 1;
}

static int app_wants_text(void *st) { (void)st; return !s_picking; }

enum { ACT_SAVE = 1, ACT_SAVEAS, ACT_OPEN };

static const CappAction VIM_ACTIONS[] = {
  { "save",   "Write (:w)",      "File", 0, ACT_SAVE },
  { "saveas", "Write as...",     "File", 0, ACT_SAVEAS },
  { "open",   "Open... (:e)",    "File", 0, ACT_OPEN },
};

static int app_action(void *st, int a) {
  (void)st;
  if (a == ACT_SAVE) vi_ex(&V, "w");
  else if (a == ACT_SAVEAS) V.req = VREQ_SAVEAS;
  else if (a == ACT_OPEN) vi_ex(&V, "e");
  if (V.req) take_request();
  vi_layout(&V);
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_BIG,
  "Vim",
  /* 16x16: a diamond with a V in it. */
  { 0x01, 0x80, 0x02, 0x40, 0x04, 0x20, 0x08, 0x10,
    0x16, 0x68, 0x26, 0x64, 0x46, 0x62, 0x86, 0x61,
    0x43, 0xC2, 0x23, 0xC4, 0x11, 0x88, 0x09, 0x90,
    0x04, 0x20, 0x02, 0x40, 0x01, 0x80, 0x00, 0x00 },
  "Modes\nesc\tnormal mode\ni a I A o O\tinsert\nv V\tvisual, by char or line\n"
  ":\tex command\n/ ?\tsearch, n N next\n"
  "Motions\nh j k l\tor fn-arrows\nw b e W B E\twords\n0 ^ $\tline\n"
  "gg G\ttop, bottom, Ngg line N\nf t F T\tto a char, ; , again\n%\tmatching bracket\n"
  "H M L\tscreen\n{ }\tparagraph\nctrl-d ctrl-u\thalf a screen\n* #\tthe word under it\n"
  "Changes\nd c y\twith a motion; dd cc yy\nx X r s S\tchars\nD C Y\tto the end\n"
  "p P\tput\nJ\tjoin\n>> <<\tindent\n~\tcase\nu ctrl-r\tundo, redo\n.\trepeat\n"
  "Insert\nctrl-n ctrl-p\tcomplete\ntab\tcomplete after a word\nctrl-w ctrl-u\tdelete back\n"
  "Ex\n:w :q :wq :x\twrite, close\n:e PATH\topen; :e alone, the picker\n:N\tline N\n"
  ":s/a/b/g\tthis line; :%s all\n:set nu nonu\tline numbers; sw= ts= et ic\n",
  0, 0
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  vapi = a;
  setup_memory();
  if (argc > 1 && argv[1][0]) {
    char path[VI_PATH];
    vi_resolve(&V, argv[1], path, sizeof path);
    vi_load(&V, path);
  } else {
    ask_pick(PICK_OPEN);
  }
  if (!s_bigblock && !V.msg_err && !V.msg[0]) vi_say(&V, 0, "small buffer: no room for big mode");
  vi_layout(&V);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = VIM_ACTIONS;
  UI.nactions = (uint8_t)(sizeof VIM_ACTIONS / sizeof VIM_ACTIONS[0]);
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
