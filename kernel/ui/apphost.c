/* Painting an app that has the whole screen. See apphost.h. */
#include "kernel/ui/apphost.h"
#include "kernel/ui/draw.h"
#include "kernel/app/capprun.h"

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* An app smaller than the screen (Mines is 90x106) sits in the desktop
 * colour with a shadow line round it. Only the outside is painted: the app
 * paints its own rectangle, and filling that white first -- what both shells
 * did -- was a white frame on the panel before every app opened. */
static void paint_surround(Rect rect) {
  Rect o = rect_inset(rect, -1);
  if (rect.w >= DISPLAY_W && rect.h >= DISPLAY_H) return;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, o.y), C_DESKTOP);
  draw_rect(R(0, o.y + o.h, DISPLAY_W, DISPLAY_H - (o.y + o.h)), C_DESKTOP);
  draw_rect(R(0, o.y, o.x, o.h), C_DESKTOP);
  draw_rect(R(o.x + o.w, o.y, DISPLAY_W - (o.x + o.w), o.h), C_DESKTOP);
  draw_frame(o, C_SHADOW);
}

static const AppDef *s_def;
static Rect          s_rect;

static void paint_body(void *ctx) {
  (void)ctx;
  s_def->paint(s_def->state, s_rect);
}

void apphost_paint(const AppDef *a, Rect rect, int how, const Rect *extra) {
  Rect area = rect, want;
  int marked, direct;

  if (!a || !a->paint) return;
  direct = capprun_paint_direct(a);
  if (how & AH_SURROUND) paint_surround(rect);

  /* Clipped to its own rectangle, so an app that draws past its declared
   * size cannot scribble over the surround it does not own -- and narrowed
   * further to whatever the app says actually changed. That is what stops a
   * keypress redrawing a whole screen. Until an app marks damage it gets its
   * full rectangle, exactly as before. */
  marked = a->take_damage && a->take_damage(a->state, &want);
  if (!(how & AH_FULL)) {
    if (marked) {
      Rect vis = rect_intersect(want, rect);
      if (!rect_is_empty(vis)) area = vis;
    }
    if (extra) {
      Rect ex = rect_intersect(*extra, rect);
      if (!(how & AH_ASKED)) area = marked ? rect_union(area, ex) : ex;
      else if (!rect_equals(area, rect)) area = rect_union(area, ex);
    }
  }
  if (rect_is_empty(area)) { draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H)); return; }

  draw_set_clip(area);
  if (rect_equals(area, rect) && !direct) {
    /* All of it: off the panel, a strip at a time, each strip starting
     * white -- what the shells used to fill the rect with on the panel. */
    s_def = a;
    s_rect = rect;
    draw_offscreen(rect, C_WHITE, paint_body, NULL);
  } else {
    /* An app that paints direct gets the white it always had on opening:
     * it may not cover every pixel, and what is under it is not its own. */
    if ((how & AH_OPENED) && direct) draw_rect(rect, C_WHITE);
    a->paint(a->state, rect);
  }
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}
