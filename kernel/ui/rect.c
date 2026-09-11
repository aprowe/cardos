/* Rectangle arithmetic. See rect.h. */

#include "ui/rect.h"

static int16_t min16(int16_t a, int16_t b) { return a < b ? a : b; }
static int16_t max16(int16_t a, int16_t b) { return a > b ? a : b; }

int rect_is_empty(Rect r) { return (r.w <= 0 || r.h <= 0) ? 1 : 0; }

int rect_equals(Rect a, Rect b) {
  /* All empty rects are the same rect as far as callers are concerned. */
  if (rect_is_empty(a) && rect_is_empty(b)) return 1;
  return (a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h) ? 1 : 0;
}

int rect_contains(Rect r, int16_t x, int16_t y) {
  if (rect_is_empty(r)) return 0;
  return (x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h) ? 1 : 0;
}

Rect rect_intersect(Rect a, Rect b) {
  Rect out;
  int16_t x0, y0, x1, y1;
  if (rect_is_empty(a) || rect_is_empty(b)) return RECT_EMPTY;

  x0 = max16(a.x, b.x);
  y0 = max16(a.y, b.y);
  x1 = min16((int16_t)(a.x + a.w), (int16_t)(b.x + b.w));
  y1 = min16((int16_t)(a.y + a.h), (int16_t)(b.y + b.h));
  if (x1 <= x0 || y1 <= y0) return RECT_EMPTY;

  out.x = x0;
  out.y = y0;
  out.w = (int16_t)(x1 - x0);
  out.h = (int16_t)(y1 - y0);
  return out;
}

int rect_overlaps(Rect a, Rect b) { return !rect_is_empty(rect_intersect(a, b)); }

Rect rect_union(Rect a, Rect b) {
  Rect out;
  int16_t x0, y0, x1, y1;
  /* An empty rect still has coordinates; treating them as real would drag
   * every union back towards the origin. */
  if (rect_is_empty(a)) return rect_is_empty(b) ? RECT_EMPTY : b;
  if (rect_is_empty(b)) return a;

  x0 = min16(a.x, b.x);
  y0 = min16(a.y, b.y);
  x1 = max16((int16_t)(a.x + a.w), (int16_t)(b.x + b.w));
  y1 = max16((int16_t)(a.y + a.h), (int16_t)(b.y + b.h));

  out.x = x0;
  out.y = y0;
  out.w = (int16_t)(x1 - x0);
  out.h = (int16_t)(y1 - y0);
  return out;
}

Rect rect_inset(Rect r, int16_t n) {
  Rect out;
  out.x = (int16_t)(r.x + n);
  out.y = (int16_t)(r.y + n);
  out.w = (int16_t)(r.w - 2 * n);
  out.h = (int16_t)(r.h - 2 * n);
  if (rect_is_empty(out)) return RECT_EMPTY;
  return out;
}

Rect rect_offset(Rect r, int16_t dx, int16_t dy) {
  r.x = (int16_t)(r.x + dx);
  r.y = (int16_t)(r.y + dy);
  return r;
}

Rect rect_clip(Rect r, int16_t w, int16_t h) {
  Rect screen;
  screen.x = 0; screen.y = 0; screen.w = w; screen.h = h;
  return rect_intersect(r, screen);
}

long rect_area(Rect r) {
  if (rect_is_empty(r)) return 0;
  return (long)r.w * (long)r.h;
}
