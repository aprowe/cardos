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

int rect_subtract(Rect a, Rect b, Rect *out) {
  Rect i = rect_intersect(a, b);
  int n = 0;

  if (rect_is_empty(a)) return 0;
  if (rect_is_empty(i)) { out[0] = a; return 1; }

  /* Up to four bands around the overlap: above, below, then left and right of
   * it within the overlapping band of rows. Splitting this way keeps the
   * pieces disjoint, which is what stops the compositor painting a pixel
   * twice. */
  if (i.y > a.y) {
    Rect t;
    t.x = a.x; t.y = a.y; t.w = a.w; t.h = (int16_t)(i.y - a.y);
    out[n++] = t;
  }
  if (i.y + i.h < a.y + a.h) {
    Rect t;
    t.x = a.x;
    t.y = (int16_t)(i.y + i.h);
    t.w = a.w;
    t.h = (int16_t)((a.y + a.h) - (i.y + i.h));
    out[n++] = t;
  }
  if (i.x > a.x) {
    Rect t;
    t.x = a.x; t.y = i.y; t.w = (int16_t)(i.x - a.x); t.h = i.h;
    out[n++] = t;
  }
  if (i.x + i.w < a.x + a.w) {
    Rect t;
    t.x = (int16_t)(i.x + i.w);
    t.y = i.y;
    t.w = (int16_t)((a.x + a.w) - (i.x + i.w));
    t.h = i.h;
    out[n++] = t;
  }
  return n;
}
