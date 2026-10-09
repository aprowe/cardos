/* Drawing off the panel: where a blit goes while a target is set.
 *
 * While a target is set, a blit lands in that buffer instead of on the
 * panel: the part inside the target's rectangle is copied, the rest is
 * dropped. Everything in draw.c ends in display_blit, so every drawing call
 * can compose a strip of the screen in memory, and the strip goes to the
 * panel in one blit -- no fill showing before what is drawn over it, which
 * is what flicker was. One target at a time, on the drawing task.
 *
 * Portable, apart from the one call display_blit makes into it, so the host
 * suite can drive draw.c's composition against a fake panel
 * (test/test_draw.c). */
#include "kernel/drv/display.h"

#include <string.h>

static DispTarget s_t;

void display_target(uint16_t *buf, int x, int y, int w, int h) {
  s_t.buf = buf;
  s_t.x = x; s_t.y = y; s_t.w = w; s_t.h = h;
}

void display_target_get(DispTarget *out) { if (out) *out = s_t; }

void display_target_set(const DispTarget *t) {
  if (t) s_t = *t;
  else display_target(NULL, 0, 0, 0, 0);
}

int disptarget_take(int x, int y, int w, int h, const uint16_t *pixels) {
  int x0, y0, x1, y1, r;
  if (!s_t.buf) return 0;
  x0 = x > s_t.x ? x : s_t.x;
  y0 = y > s_t.y ? y : s_t.y;
  x1 = x + w < s_t.x + s_t.w ? x + w : s_t.x + s_t.w;
  y1 = y + h < s_t.y + s_t.h ? y + h : s_t.y + s_t.h;
  if (x0 >= x1 || y0 >= y1) return 1;
  for (r = y0; r < y1; r++)
    memcpy(s_t.buf + (size_t)(r - s_t.y) * (size_t)s_t.w + (size_t)(x0 - s_t.x),
           pixels + (size_t)(r - y) * (size_t)w + (size_t)(x0 - x),
           (size_t)(x1 - x0) * 2);
  return 1;
}
