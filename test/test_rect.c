#include "tinytest.h"
#include "kernel/ui/rect.h"

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* Parameters are ex/ey/ew/eh, not x/y/w/h: naming them after the struct
 * members turns _g.x into _g.5 during expansion. */
#define RECT_IS(got, ex, ey, ew, eh) do {                                 \
    Rect _g = (got), _e = R(ex, ey, ew, eh);                              \
    CHECK(rect_equals(_g, _e));                                           \
    if (!rect_equals(_g, _e))                                             \
      printf("      got (%d,%d %dx%d) wanted (%d,%d %dx%d)\n",            \
             _g.x, _g.y, _g.w, _g.h, _e.x, _e.y, _e.w, _e.h);             \
  } while (0)

void test_emptiness(void) {
  CHECK_EQ(rect_is_empty(R(0, 0, 0, 0)), 1);
  CHECK_EQ(rect_is_empty(R(5, 5, 0, 10)), 1);
  CHECK_EQ(rect_is_empty(R(5, 5, 10, 0)), 1);
  CHECK_EQ(rect_is_empty(R(5, 5, -3, 10)), 1);   /* negative is empty, not huge */
  CHECK_EQ(rect_is_empty(R(0, 0, 1, 1)), 0);
}

void test_contains_is_half_open(void) {
  Rect r = R(10, 10, 5, 5);            /* covers x 10..14, y 10..14 */
  CHECK_EQ(rect_contains(r, 10, 10), 1);
  CHECK_EQ(rect_contains(r, 14, 14), 1);
  CHECK_EQ(rect_contains(r, 15, 14), 0);   /* the far edge is outside */
  CHECK_EQ(rect_contains(r, 14, 15), 0);
  CHECK_EQ(rect_contains(r, 9, 10), 0);
  CHECK_EQ(rect_contains(R(0, 0, 0, 0), 0, 0), 0);
}

void test_intersection(void) {
  RECT_IS(rect_intersect(R(0, 0, 10, 10), R(5, 5, 10, 10)), 5, 5, 5, 5);
  RECT_IS(rect_intersect(R(0, 0, 10, 10), R(0, 0, 10, 10)), 0, 0, 10, 10);
  /* Fully contained. */
  RECT_IS(rect_intersect(R(0, 0, 20, 20), R(5, 5, 5, 5)), 5, 5, 5, 5);
}

void test_non_overlapping_rects_intersect_to_empty(void) {
  CHECK_EQ(rect_is_empty(rect_intersect(R(0, 0, 10, 10), R(20, 20, 5, 5))), 1);
  /* Touching edge to edge is not overlapping: 0..9 then 10..14. */
  CHECK_EQ(rect_is_empty(rect_intersect(R(0, 0, 10, 10), R(10, 0, 5, 10))), 1);
  CHECK_EQ(rect_overlaps(R(0, 0, 10, 10), R(10, 0, 5, 10)), 0);
  CHECK_EQ(rect_overlaps(R(0, 0, 10, 10), R(9, 0, 5, 10)), 1);
}

void test_union(void) {
  RECT_IS(rect_union(R(0, 0, 5, 5), R(10, 10, 5, 5)), 0, 0, 15, 15);
  RECT_IS(rect_union(R(10, 10, 5, 5), R(0, 0, 5, 5)), 0, 0, 15, 15);
  RECT_IS(rect_union(R(0, 0, 20, 20), R(5, 5, 5, 5)), 0, 0, 20, 20);
}

/* An empty rect sits at some coordinate, usually (0,0). If union treated it as
 * a real rect, every union would be dragged back to the origin and the
 * compositor would repaint the whole screen forever. */
void test_union_with_empty_is_the_other_rect(void) {
  RECT_IS(rect_union(R(50, 50, 10, 10), R(0, 0, 0, 0)), 50, 50, 10, 10);
  RECT_IS(rect_union(R(0, 0, 0, 0), R(50, 50, 10, 10)), 50, 50, 10, 10);
  CHECK_EQ(rect_is_empty(rect_union(R(0, 0, 0, 0), R(9, 9, 0, 0))), 1);
}

void test_inset(void) {
  RECT_IS(rect_inset(R(10, 10, 20, 20), 1), 11, 11, 18, 18);
  RECT_IS(rect_inset(R(10, 10, 20, 20), 5), 15, 15, 10, 10);
  /* Inset past the middle collapses rather than inverting. */
  CHECK_EQ(rect_is_empty(rect_inset(R(10, 10, 4, 4), 3)), 1);
}

void test_offset(void) {
  RECT_IS(rect_offset(R(10, 10, 5, 5), 3, -4), 13, 6, 5, 5);
  RECT_IS(rect_offset(R(0, 0, 5, 5), -10, -10), -10, -10, 5, 5);
}

void test_clipping_to_the_screen(void) {
  RECT_IS(rect_clip(R(-5, -5, 20, 20), 240, 135), 0, 0, 15, 15);
  RECT_IS(rect_clip(R(230, 130, 20, 20), 240, 135), 230, 130, 10, 5);
  RECT_IS(rect_clip(R(10, 10, 5, 5), 240, 135), 10, 10, 5, 5);
  CHECK_EQ(rect_is_empty(rect_clip(R(300, 300, 10, 10), 240, 135)), 1);
  CHECK_EQ(rect_is_empty(rect_clip(R(-50, 0, 10, 10), 240, 135)), 1);
}

void test_area(void) {
  CHECK_EQ(rect_area(R(0, 0, 10, 10)), 100);
  CHECK_EQ(rect_area(R(0, 0, 0, 10)), 0);
  CHECK_EQ(rect_area(R(0, 0, 240, 135)), 32400);   /* the whole screen */
}

/* ---- subtraction --------------------------------------------------------
 * The compositor uses this to work out which parts of a window are not hidden
 * behind the windows above it, so the pieces must be disjoint and must add up
 * to exactly the original minus the overlap. */

/* Counts how many of the returned pieces contain a point: must be 1 for a
 * pixel that survives and 0 for one that was subtracted. */
static int cover_count(const Rect *rs, int n, int x, int y) {
  int i, c = 0;
  for (i = 0; i < n; i++)
    if (rect_contains(rs[i], (int16_t)x, (int16_t)y)) c++;
  return c;
}

void test_subtracting_a_disjoint_rect_changes_nothing(void) {
  Rect out[RECT_SUB_MAX];
  CHECK_EQ(rect_subtract(R(0, 0, 10, 10), R(50, 50, 10, 10), out), 1);
  RECT_IS(out[0], 0, 0, 10, 10);
}

void test_subtracting_a_covering_rect_leaves_nothing(void) {
  Rect out[RECT_SUB_MAX];
  CHECK_EQ(rect_subtract(R(10, 10, 10, 10), R(0, 0, 100, 100), out), 0);
}

void test_subtracting_a_hole_leaves_four_pieces(void) {
  Rect out[RECT_SUB_MAX];
  int n = rect_subtract(R(0, 0, 30, 30), R(10, 10, 10, 10), out);
  CHECK_EQ(n, 4);
  /* A pixel in the hole is gone; pixels around it survive exactly once. */
  CHECK_EQ(cover_count(out, n, 15, 15), 0);
  CHECK_EQ(cover_count(out, n, 5, 5), 1);
  CHECK_EQ(cover_count(out, n, 25, 15), 1);
  CHECK_EQ(cover_count(out, n, 15, 25), 1);
}

/* The property, checked over every pixel: a pixel of `a` survives exactly once
 * unless it was in `b`, in which case it is gone. Disjointness matters as much
 * as coverage -- overlapping pieces mean painting the same pixel twice. */
void test_subtraction_is_exact_and_disjoint_everywhere(void) {
  static const Rect bs[] = {
    { 10, 10, 10, 10 },   /* a hole in the middle */
    { -5, -5, 12, 12 },   /* a corner */
    { 0, 10, 30, 5 },     /* a full-width band */
    { 12, 0, 6, 30 },     /* a full-height band */
    { 25, 25, 20, 20 },   /* overlapping one corner, extending outside */
  };
  Rect a = R(0, 0, 30, 30);
  size_t k;
  int x, y;

  for (k = 0; k < sizeof bs / sizeof bs[0]; k++) {
    Rect out[RECT_SUB_MAX];
    int n = rect_subtract(a, bs[k], out);
    int bad = 0;
    for (y = 0; y < 30; y++) {
      for (x = 0; x < 30; x++) {
        int want = rect_contains(bs[k], (int16_t)x, (int16_t)y) ? 0 : 1;
        if (cover_count(out, n, x, y) != want) bad++;
      }
    }
    CHECK_EQ(bad, 0);
    if (bad) printf("      case %u: %d pixels wrong\n", (unsigned)k, bad);
  }
}
