/* Rectangle arithmetic for the window system.
 *
 * Portable C. Every compositor bug worth having is a rectangle bug, and they
 * are all reproducible on a PC, so none of this touches hardware.
 *
 * Coordinates are int16_t: the screen is 240x135, but intermediate results
 * during a move can legitimately go negative or past the edge, and clamping
 * early is how a window ends up mysteriously stuck.
 */
#ifndef CARDOS_RECT_H
#define CARDOS_RECT_H

#include <stdint.h>

typedef struct {
  int16_t x, y, w, h;
} Rect;

static const Rect RECT_EMPTY = { 0, 0, 0, 0 };

int  rect_is_empty(Rect r);
int  rect_equals(Rect a, Rect b);
int  rect_contains(Rect r, int16_t x, int16_t y);
int  rect_overlaps(Rect a, Rect b);
long rect_area(Rect r);

/* The overlapping part, or an empty rect when they do not overlap. */
Rect rect_intersect(Rect a, Rect b);

/* The smallest rect covering both. The union of anything with an empty rect
 * is the other rect -- otherwise an empty rect at the origin would drag every
 * union back to (0,0). */
Rect rect_union(Rect a, Rect b);

/* Shrink by `n` on every side; may become empty. */
Rect rect_inset(Rect r, int16_t n);

Rect rect_offset(Rect r, int16_t dx, int16_t dy);

/* Clip to a screen of w x h. */
Rect rect_clip(Rect r, int16_t w, int16_t h);

#endif /* CARDOS_RECT_H */
