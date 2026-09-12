/* Window manager core. See wm.h. */

#include "kernel/ui/wm.h"

#include <string.h>

typedef struct {
  char    title[WM_TITLE_MAX + 1];
  Rect    frame;
  uint8_t gen;
  uint8_t used;
} Win;

static Win     g_win[WM_MAX_WINDOWS];
static uint8_t g_order[WM_MAX_WINDOWS];   /* indices, bottom first */
static int     g_n;                       /* windows in the stack */

static Rect    g_damage[WM_MAX_DAMAGE];
static int     g_damage_n;

static int16_t g_sw = 240, g_sh = 135;

/* ------------------------------------------------------------ identity -- */

static uint8_t idx_of(WinId w) { return (uint8_t)(w & 0xFFu); }
static uint8_t gen_of(WinId w) { return (uint8_t)(w >> 8); }

static Win *win_of(WinId w) {
  Win *p;
  if (gen_of(w) == 0) return NULL;
  if (idx_of(w) >= WM_MAX_WINDOWS) return NULL;
  p = &g_win[idx_of(w)];
  if (!p->used || p->gen != gen_of(w)) return NULL;
  return p;
}

static WinId id_of(const Win *p) {
  uint8_t i = (uint8_t)(p - g_win);
  return (WinId)(((uint16_t)p->gen << 8) | i);
}

int wm_valid(WinId w) { return win_of(w) != NULL; }
int wm_count(void) { return g_n; }

/* -------------------------------------------------------------- damage -- */

void wm_damage(Rect r) {
  int i;

  r = rect_clip(r, g_sw, g_sh);
  if (rect_is_empty(r)) return;

  /* Merge into anything it already touches: repainting the union once beats
   * repainting the overlap twice. */
  for (i = 0; i < g_damage_n; i++) {
    if (rect_overlaps(g_damage[i], r)) {
      g_damage[i] = rect_union(g_damage[i], r);
      return;
    }
  }

  if (g_damage_n < WM_MAX_DAMAGE) {
    g_damage[g_damage_n++] = r;
    return;
  }

  /* Full. Merge whichever pairing wastes the least: either this rect into an
   * existing one, or two existing ones together to make room. Coverage is
   * preserved either way, which is the property that must not break. */
  {
    long best_cost = -1;
    int best_a = 0, best_b = -1;      /* b < 0 means "merge r into a" */
    int a, b;

    for (a = 0; a < g_damage_n; a++) {
      long cost = rect_area(rect_union(g_damage[a], r))
                - rect_area(g_damage[a]) - rect_area(r);
      if (best_cost < 0 || cost < best_cost) { best_cost = cost; best_a = a; best_b = -1; }
    }
    for (a = 0; a < g_damage_n; a++) {
      for (b = a + 1; b < g_damage_n; b++) {
        long cost = rect_area(rect_union(g_damage[a], g_damage[b]))
                  - rect_area(g_damage[a]) - rect_area(g_damage[b]);
        if (cost < best_cost) { best_cost = cost; best_a = a; best_b = b; }
      }
    }

    if (best_b < 0) {
      g_damage[best_a] = rect_union(g_damage[best_a], r);
    } else {
      g_damage[best_a] = rect_union(g_damage[best_a], g_damage[best_b]);
      g_damage[best_b] = g_damage[g_damage_n - 1];
      g_damage_n--;
      g_damage[g_damage_n++] = r;
    }
  }
}

int wm_damage_count(void) { return g_damage_n; }

int wm_take_damage(Rect *out, int max) {
  int n = g_damage_n < max ? g_damage_n : max;
  int i;
  for (i = 0; i < n; i++) out[i] = g_damage[i];
  g_damage_n = 0;
  return n;
}

/* ------------------------------------------------------------ lifetime -- */

void wm_init(int16_t screen_w, int16_t screen_h) {
  int i;
  memset(g_win, 0, sizeof g_win);
  for (i = 0; i < WM_MAX_WINDOWS; i++) g_win[i].gen = 1;
  g_n = 0;
  g_damage_n = 0;
  g_sw = screen_w;
  g_sh = screen_h;
}

WinId wm_create(const char *title, Rect frame) {
  int i;
  for (i = 0; i < WM_MAX_WINDOWS; i++) {
    Win *p = &g_win[i];
    if (p->used) continue;
    memset(p->title, 0, sizeof p->title);
    if (title) strncpy(p->title, title, WM_TITLE_MAX);
    p->title[WM_TITLE_MAX] = '\0';
    p->frame = frame;
    p->used = 1;
    g_order[g_n++] = (uint8_t)i;      /* new windows arrive on top */
    wm_damage(frame);
    return id_of(p);
  }
  return WIN_NONE;
}

static int order_pos(uint8_t index) {
  int i;
  for (i = 0; i < g_n; i++)
    if (g_order[i] == index) return i;
  return -1;
}

void wm_destroy(WinId w) {
  Win *p = win_of(w);
  int pos, i;
  if (!p) return;

  wm_damage(p->frame);              /* whatever it covered must be repainted */

  pos = order_pos(idx_of(w));
  if (pos >= 0) {
    for (i = pos; i < g_n - 1; i++) g_order[i] = g_order[i + 1];
    g_n--;
  }
  p->used = 0;
  p->gen = (uint8_t)(p->gen + 1);
  if (p->gen == 0) p->gen = 1;      /* 0 is reserved, as with Handle and Tid */
}

/* ------------------------------------------------------------ geometry -- */

Rect wm_frame(WinId w) {
  Win *p = win_of(w);
  return p ? p->frame : RECT_EMPTY;
}

Rect wm_content(WinId w) {
  Win *p = win_of(w);
  Rect c;
  if (!p) return RECT_EMPTY;
  c.x = (int16_t)(p->frame.x + WM_BORDER);
  c.y = (int16_t)(p->frame.y + WM_BORDER + WM_TITLE_H);
  c.w = (int16_t)(p->frame.w - 2 * WM_BORDER);
  c.h = (int16_t)(p->frame.h - 2 * WM_BORDER - WM_TITLE_H);
  if (rect_is_empty(c)) return RECT_EMPTY;
  return c;
}

const char *wm_title(WinId w) {
  Win *p = win_of(w);
  return p ? p->title : "";
}

void wm_move(WinId w, int16_t x, int16_t y) {
  Win *p = win_of(w);
  Rect old;
  if (!p) return;
  old = p->frame;
  p->frame.x = x;
  p->frame.y = y;
  if (rect_equals(old, p->frame)) return;
  /* Both places: repainting only the destination leaves a smear of the old
   * window behind, because nothing else knows those pixels are now stale. */
  wm_damage(old);
  wm_damage(p->frame);
}

void wm_resize(WinId w, int16_t width, int16_t height) {
  Win *p = win_of(w);
  Rect old;
  if (!p) return;
  old = p->frame;
  p->frame.w = width;
  p->frame.h = height;
  if (rect_equals(old, p->frame)) return;
  wm_damage(old);
  wm_damage(p->frame);
}

/* ------------------------------------------------------------- z-order -- */

void wm_raise(WinId w) {
  Win *p = win_of(w);
  int pos, i;
  if (!p) return;
  pos = order_pos(idx_of(w));
  if (pos < 0 || pos == g_n - 1) return;     /* already on top */
  for (i = pos; i < g_n - 1; i++) g_order[i] = g_order[i + 1];
  g_order[g_n - 1] = idx_of(w);
  wm_damage(p->frame);
}

WinId wm_focus(void) {
  if (g_n == 0) return WIN_NONE;
  return id_of(&g_win[g_order[g_n - 1]]);
}

int wm_z(WinId w) {
  if (!win_of(w)) return -1;
  return order_pos(idx_of(w));
}

WinId wm_at(int16_t x, int16_t y) {
  int i;
  for (i = g_n - 1; i >= 0; i--) {           /* front to back */
    Win *p = &g_win[g_order[i]];
    if (rect_contains(p->frame, x, y)) return id_of(p);
  }
  return WIN_NONE;
}

int wm_visible_at(WinId w, int16_t x, int16_t y) {
  Win *p = win_of(w);
  int pos, i;
  if (!p || !rect_contains(p->frame, x, y)) return 0;
  pos = order_pos(idx_of(w));
  for (i = pos + 1; i < g_n; i++)            /* anything above covering it? */
    if (rect_contains(g_win[g_order[i]].frame, x, y)) return 0;
  return 1;
}

WmHit wm_hit_test(WinId w, int16_t x, int16_t y) {
  Win *p = win_of(w);
  Rect title, close, content;
  if (!p || !rect_contains(p->frame, x, y)) return WM_HIT_NONE;

  content = wm_content(w);
  if (rect_contains(content, x, y)) return WM_HIT_CONTENT;

  title.x = (int16_t)(p->frame.x + WM_BORDER);
  title.y = (int16_t)(p->frame.y + WM_BORDER);
  title.w = (int16_t)(p->frame.w - 2 * WM_BORDER);
  title.h = WM_TITLE_H;

  if (rect_contains(title, x, y)) {
    close.x = (int16_t)(title.x + title.w - WM_CLOSE_W);
    close.y = title.y;
    close.w = WM_CLOSE_W;
    close.h = WM_TITLE_H;
    if (rect_contains(close, x, y)) return WM_HIT_CLOSE;
    /* Maximise sits immediately left of close, the same size, the same
     * gesture -- and the same order Windows put them in, which is the order
     * a hand already knows. */
    close.x = (int16_t)(close.x - WM_CLOSE_W - 1);
    if (rect_contains(close, x, y)) return WM_HIT_MAX;
    close.x = (int16_t)(close.x - WM_CLOSE_W - 1);
    if (rect_contains(close, x, y)) return WM_HIT_MIN;
    return WM_HIT_TITLE;
  }
  return WM_HIT_BORDER;
}

/* ------------------------------------------------------------ painting -- */

/* Working set while a rectangle is being cut down. Static because task stacks
 * are 1 KB and this must be allowed to be generous. */
#define WM_MAX_PIECES 64
static Rect g_pieces[WM_MAX_PIECES];
static int  g_pieces_n;

static void pieces_reset(Rect r) {
  g_pieces_n = 0;
  if (!rect_is_empty(r)) g_pieces[g_pieces_n++] = r;
}

/* Remove `cut` from every piece. On overflow the piece is kept whole rather
 * than dropped: over-painting is merely wasted effort, and because painting
 * runs back to front a higher window repaints over it afterwards anyway.
 * Dropping would leave a hole on screen forever. */
static void pieces_subtract(Rect cut) {
  Rect next[WM_MAX_PIECES];
  int n = 0, i, j;

  for (i = 0; i < g_pieces_n; i++) {
    Rect rest[RECT_SUB_MAX];
    int n_rest;

    if (!rect_overlaps(g_pieces[i], cut)) {
      if (n < WM_MAX_PIECES) next[n++] = g_pieces[i];
      continue;
    }
    n_rest = rect_subtract(g_pieces[i], cut, rest);
    if (n + n_rest > WM_MAX_PIECES) {
      if (n < WM_MAX_PIECES) next[n++] = g_pieces[i];   /* keep it whole */
      continue;
    }
    for (j = 0; j < n_rest; j++) next[n++] = rest[j];
  }
  for (i = 0; i < n; i++) g_pieces[i] = next[i];
  g_pieces_n = n;
}

void wm_paint(PaintFn fn, void *ctx) {
  Rect damage[WM_MAX_DAMAGE];
  int n_damage = wm_take_damage(damage, WM_MAX_DAMAGE);
  int d, w, k, i;

  if (!fn) return;

  for (d = 0; d < n_damage; d++) {
    /* The desktop shows wherever no window covers. Painted first, so anything
     * above simply covers it. */
    pieces_reset(damage[d]);
    for (w = 0; w < g_n; w++) pieces_subtract(g_win[g_order[w]].frame);
    for (i = 0; i < g_pieces_n; i++) fn(ctx, WIN_NONE, g_pieces[i]);

    /* Then each window from the bottom up, showing only where nothing above
     * it covers. */
    for (w = 0; w < g_n; w++) {
      Win *p = &g_win[g_order[w]];
      pieces_reset(rect_intersect(damage[d], p->frame));
      for (k = w + 1; k < g_n; k++) pieces_subtract(g_win[g_order[k]].frame);
      for (i = 0; i < g_pieces_n; i++) fn(ctx, id_of(p), g_pieces[i]);
    }
  }
}
