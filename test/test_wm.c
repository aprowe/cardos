#include "tinytest.h"
#include "ui/wm.h"
#include <string.h>

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}
static void setup(void) { wm_init(240, 135); }

/* ---- lifetime and identity --------------------------------------------- */

void test_windows_are_created_with_live_ids(void) {
  WinId a, b;
  setup();
  a = wm_create("Terminal", R(0, 0, 100, 60));
  b = wm_create("Files", R(20, 20, 100, 60));
  CHECK(a != WIN_NONE);
  CHECK(b != WIN_NONE);
  CHECK(a != b);
  CHECK_EQ(wm_count(), 2);
  CHECK_EQ(strcmp(wm_title(a), "Terminal"), 0);
}

void test_the_window_table_can_fill_up(void) {
  int i, made = 0;
  setup();
  for (i = 0; i < WM_MAX_WINDOWS + 3; i++)
    if (wm_create("w", R(0, 0, 20, 20)) != WIN_NONE) made++;
  CHECK_EQ(made, WM_MAX_WINDOWS);
}

void test_a_reused_slot_does_not_answer_to_the_old_id(void) {
  WinId a, b;
  setup();
  a = wm_create("a", R(0, 0, 20, 20));
  wm_destroy(a);
  b = wm_create("b", R(0, 0, 20, 20));
  CHECK(b != a);
  CHECK_EQ(wm_valid(a), 0);
  CHECK_EQ(wm_valid(b), 1);
  wm_destroy(a);                       /* must not destroy b */
  CHECK_EQ(wm_valid(b), 1);
}

void test_long_titles_are_truncated(void) {
  WinId a;
  setup();
  a = wm_create("a-really-long-window-title", R(0, 0, 20, 20));
  CHECK_EQ(strlen(wm_title(a)), WM_TITLE_MAX);
}

/* ---- chrome geometry ---------------------------------------------------- */

void test_content_sits_inside_the_chrome(void) {
  WinId a;
  Rect c;
  setup();
  a = wm_create("t", R(10, 20, 100, 60));
  c = wm_content(a);
  CHECK_EQ(c.x, 10 + WM_BORDER);
  CHECK_EQ(c.y, 20 + WM_BORDER + WM_TITLE_H);
  CHECK_EQ(c.w, 100 - 2 * WM_BORDER);
  CHECK_EQ(c.h, 60 - 2 * WM_BORDER - WM_TITLE_H);
}

void test_a_window_too_small_for_chrome_has_no_content(void) {
  WinId a;
  setup();
  a = wm_create("t", R(0, 0, 4, 4));
  CHECK_EQ(rect_is_empty(wm_content(a)), 1);
}

void test_hit_testing_finds_the_chrome_parts(void) {
  WinId a;
  setup();
  a = wm_create("t", R(10, 10, 100, 60));
  CHECK_EQ(wm_hit_test(a, 10, 10), WM_HIT_BORDER);          /* corner */
  CHECK_EQ(wm_hit_test(a, 50, 11), WM_HIT_TITLE);           /* title bar */
  CHECK_EQ(wm_hit_test(a, 50, 40), WM_HIT_CONTENT);
  CHECK_EQ(wm_hit_test(a, 200, 200), WM_HIT_NONE);          /* outside */
  /* The close box lives at the right end of the title bar. */
  CHECK_EQ(wm_hit_test(a, 10 + 100 - 2 - 3, 11), WM_HIT_CLOSE);
}

/* ---- z-order ------------------------------------------------------------ */

void test_new_windows_go_on_top_and_take_focus(void) {
  WinId a, b;
  setup();
  a = wm_create("a", R(0, 0, 50, 50));
  b = wm_create("b", R(0, 0, 50, 50));
  CHECK_EQ(wm_z(a), 0);
  CHECK_EQ(wm_z(b), 1);
  CHECK_EQ(wm_focus(), b);
}

void test_raising_moves_a_window_to_the_top(void) {
  WinId a, b, c;
  setup();
  a = wm_create("a", R(0, 0, 50, 50));
  b = wm_create("b", R(0, 0, 50, 50));
  c = wm_create("c", R(0, 0, 50, 50));
  wm_raise(a);
  CHECK_EQ(wm_z(a), 2);
  CHECK_EQ(wm_z(b), 0);
  CHECK_EQ(wm_z(c), 1);
  CHECK_EQ(wm_focus(), a);
}

void test_destroying_a_window_closes_the_gap_in_the_order(void) {
  WinId a, b, c;
  setup();
  a = wm_create("a", R(0, 0, 50, 50));
  b = wm_create("b", R(0, 0, 50, 50));
  c = wm_create("c", R(0, 0, 50, 50));
  wm_destroy(b);
  CHECK_EQ(wm_z(a), 0);
  CHECK_EQ(wm_z(c), 1);
  CHECK_EQ(wm_focus(), c);
}

void test_destroying_the_focused_window_refocuses_the_next(void) {
  WinId a, b;
  setup();
  a = wm_create("a", R(0, 0, 50, 50));
  b = wm_create("b", R(0, 0, 50, 50));
  CHECK_EQ(wm_focus(), b);
  wm_destroy(b);
  CHECK_EQ(wm_focus(), a);
  wm_destroy(a);
  CHECK_EQ(wm_focus(), WIN_NONE);
}

void test_the_topmost_window_wins_a_click(void) {
  WinId a, b;
  setup();
  a = wm_create("a", R(0, 0, 100, 100));
  b = wm_create("b", R(50, 50, 100, 60));
  CHECK_EQ(wm_at(10, 10), a);         /* only a is here */
  CHECK_EQ(wm_at(60, 60), b);         /* both, but b is above */
  CHECK_EQ(wm_at(200, 120), WIN_NONE);
  wm_raise(a);
  CHECK_EQ(wm_at(60, 60), a);         /* now a is above */
}

void test_occlusion_is_per_pixel(void) {
  WinId a, b;
  setup();
  a = wm_create("a", R(0, 0, 100, 100));
  b = wm_create("b", R(50, 0, 100, 100));
  CHECK_EQ(wm_visible_at(a, 10, 10), 1);     /* a is uncovered here */
  CHECK_EQ(wm_visible_at(a, 60, 10), 0);     /* b is over this pixel */
  CHECK_EQ(wm_visible_at(b, 60, 10), 1);     /* nothing above b */
  CHECK_EQ(wm_visible_at(a, 500, 500), 0);   /* not even in a */
}

/* ---- damage ------------------------------------------------------------- */

void test_a_single_damage_rect_comes_back(void) {
  Rect out[WM_MAX_DAMAGE];
  setup();
  wm_damage(R(10, 10, 20, 20));
  CHECK_EQ(wm_damage_count(), 1);
  CHECK_EQ(wm_take_damage(out, WM_MAX_DAMAGE), 1);
  CHECK(rect_equals(out[0], R(10, 10, 20, 20)));
  CHECK_EQ(wm_damage_count(), 0);        /* taking clears */
}

void test_empty_damage_is_ignored(void) {
  setup();
  wm_damage(R(10, 10, 0, 0));
  wm_damage(R(10, 10, 5, 0));
  CHECK_EQ(wm_damage_count(), 0);
}

void test_overlapping_damage_merges(void) {
  Rect out[WM_MAX_DAMAGE];
  setup();
  wm_damage(R(0, 0, 20, 20));
  wm_damage(R(10, 10, 20, 20));
  CHECK_EQ(wm_take_damage(out, WM_MAX_DAMAGE), 1);
  CHECK(rect_equals(out[0], R(0, 0, 30, 30)));
}

void test_disjoint_damage_stays_separate(void) {
  Rect out[WM_MAX_DAMAGE];
  setup();
  wm_damage(R(0, 0, 10, 10));
  wm_damage(R(100, 100, 10, 10));
  CHECK_EQ(wm_take_damage(out, WM_MAX_DAMAGE), 2);
  /* Merging these would repaint most of the screen for 200 pixels of change. */
}

/* The invariant that actually matters. However the merging works, the result
 * must cover everything that was added -- repainting too much is slow, but
 * repainting too little leaves rubbish on the screen forever. */
void test_damage_always_covers_everything_added(void) {
  Rect added[40];
  Rect out[WM_MAX_DAMAGE];
  int n, i, j, count;
  uint32_t seed = 12345;

  setup();
  for (i = 0; i < 40; i++) {
    seed = seed * 1103515245u + 12345u;
    added[i] = R((int)((seed >> 16) % 230), (int)((seed >> 8) % 125),
                 1 + (int)(seed % 10), 1 + (int)((seed >> 4) % 10));
    wm_damage(added[i]);
  }
  count = wm_take_damage(out, WM_MAX_DAMAGE);
  CHECK(count > 0);
  CHECK(count <= WM_MAX_DAMAGE);          /* never grows without bound */

  for (i = 0; i < 40; i++) {
    int covered = 0;
    for (j = 0; j < count && !covered; j++)
      if (rect_equals(rect_intersect(added[i], out[j]), added[i])) covered = 1;
    CHECK_EQ(covered, 1);
    if (!covered)
      printf("      (%d,%d %dx%d) was left unpainted\n",
             added[i].x, added[i].y, added[i].w, added[i].h);
  }
  (void)n;
}

/* ---- damage from window operations -------------------------------------- */

void test_creating_a_window_damages_its_frame(void) {
  Rect out[WM_MAX_DAMAGE];
  setup();
  wm_create("a", R(10, 10, 50, 40));
  CHECK_EQ(wm_take_damage(out, WM_MAX_DAMAGE), 1);
  CHECK(rect_equals(out[0], R(10, 10, 50, 40)));
}

/* A move has to repaint what was uncovered as well as where the window went;
 * repainting only the destination leaves a smear of the old window behind. */
void test_moving_a_window_damages_both_places(void) {
  WinId a;
  Rect out[WM_MAX_DAMAGE];
  int n, i;
  long covered = 0;
  setup();
  a = wm_create("a", R(0, 0, 40, 40));
  wm_take_damage(out, WM_MAX_DAMAGE);       /* clear the creation damage */

  wm_move(a, 100, 0);
  n = wm_take_damage(out, WM_MAX_DAMAGE);
  CHECK(n >= 1);
  for (i = 0; i < n; i++) covered += rect_area(out[i]);
  /* Both the vacated and the new position, so at least two windows' worth. */
  CHECK(covered >= 2 * 40 * 40);
}

void test_destroying_a_window_damages_what_it_covered(void) {
  WinId a;
  Rect out[WM_MAX_DAMAGE];
  setup();
  a = wm_create("a", R(30, 30, 50, 50));
  wm_take_damage(out, WM_MAX_DAMAGE);
  wm_destroy(a);
  CHECK_EQ(wm_take_damage(out, WM_MAX_DAMAGE), 1);
  CHECK(rect_equals(out[0], R(30, 30, 50, 50)));
}

void test_raising_damages_only_the_window_raised(void) {
  WinId a, b;
  Rect out[WM_MAX_DAMAGE];
  setup();
  a = wm_create("a", R(0, 0, 50, 50));
  b = wm_create("b", R(100, 0, 50, 50));
  wm_take_damage(out, WM_MAX_DAMAGE);
  wm_raise(a);
  CHECK_EQ(wm_take_damage(out, WM_MAX_DAMAGE), 1);
  CHECK(rect_equals(out[0], R(0, 0, 50, 50)));
  (void)b;
}

void test_damage_is_clipped_to_the_screen(void) {
  Rect out[WM_MAX_DAMAGE];
  int n, i;
  setup();
  wm_damage(R(-20, -20, 50, 50));
  wm_damage(R(230, 130, 50, 50));
  n = wm_take_damage(out, WM_MAX_DAMAGE);
  for (i = 0; i < n; i++) {
    CHECK(out[i].x >= 0);
    CHECK(out[i].y >= 0);
    CHECK(out[i].x + out[i].w <= 240);
    CHECK(out[i].y + out[i].h <= 135);
  }
}

/* ---- paint scheduling ---------------------------------------------------
 *
 * These use a per-pixel oracle rather than checking rectangles by eye. For a
 * 240x135 screen that is 32400 checks per case, which costs nothing on a PC
 * and is the only way to be sure about occlusion geometry.
 */

#define MAX_COLLECT 512
typedef struct { WinId win; Rect r; } Job;
static Job g_jobs[MAX_COLLECT];
static int g_jobs_n;

static void collect(void *ctx, WinId w, Rect r) {
  (void)ctx;
  if (g_jobs_n < MAX_COLLECT) { g_jobs[g_jobs_n].win = w; g_jobs[g_jobs_n].r = r; g_jobs_n++; }
}

static int run_paint(void) { g_jobs_n = 0; wm_paint(collect, NULL); return g_jobs_n; }

/* Who *should* end up owning pixel (x,y): the topmost window covering it, else
 * the desktop. Computed the slow obvious way so it is independent of the code
 * under test. */
static WinId oracle_owner(int x, int y) { return wm_at((int16_t)x, (int16_t)y); }

/* Painting runs back to front, so the correctness condition is that the LAST
 * call covering a pixel is its true owner -- and that every damaged pixel is
 * covered at least once. Returns pixels that got it wrong; `overdraw` counts
 * pixels painted more than once, which is waste rather than error. */
static int audit(Rect damage, int *overdraw) {
  int bad = 0, x, y, j;
  *overdraw = 0;
  for (y = damage.y; y < damage.y + damage.h; y++) {
    for (x = damage.x; x < damage.x + damage.w; x++) {
      int hits = 0;
      WinId last = WIN_NONE;
      for (j = 0; j < g_jobs_n; j++) {
        if (rect_contains(g_jobs[j].r, (int16_t)x, (int16_t)y)) { hits++; last = g_jobs[j].win; }
      }
      if (hits > 1) (*overdraw)++;
      if (hits < 1 || last != oracle_owner(x, y)) {
        if (bad < 4)
          printf("      (%d,%d): %d calls, ended as %u, wanted %u\n",
                 x, y, hits, (unsigned)last, (unsigned)oracle_owner(x, y));
        bad++;
      }
    }
  }
  return bad;
}

void test_painting_an_empty_desktop_is_all_background(void) {
  Rect d = R(10, 10, 40, 30);
  int over;
  setup();
  wm_damage(d);
  CHECK_EQ(run_paint(), 1);
  CHECK_EQ(g_jobs[0].win, WIN_NONE);
  CHECK(rect_equals(g_jobs[0].r, d));
  CHECK_EQ(audit(d, &over), 0);
  CHECK_EQ(over, 0);
}

void test_a_single_window_splits_damage_from_the_desktop(void) {
  Rect d = R(0, 0, 100, 100);
  int over;
  setup();
  wm_create("a", R(20, 20, 40, 40));
  wm_take_damage(NULL, 0);
  wm_damage(d);
  run_paint();
  CHECK_EQ(audit(d, &over), 0);
  CHECK_EQ(over, 0);            /* a simple case must not waste any pixel */
}

void test_overlapping_windows_are_partitioned_correctly(void) {
  Rect d = R(0, 0, 240, 135);
  int over;
  setup();
  wm_create("a", R(10, 10, 80, 60));
  wm_create("b", R(50, 30, 80, 60));      /* overlaps a, and sits above it */
  wm_create("c", R(0, 90, 200, 40));
  wm_take_damage(NULL, 0);
  wm_damage(d);
  CHECK(run_paint() > 0);
  CHECK_EQ(audit(d, &over), 0);
  CHECK_EQ(over, 0);
}

/* A window entirely behind another must not be painted at all -- that is the
 * whole point of tracking occlusion. */
void test_a_fully_covered_window_is_not_painted(void) {
  WinId a, b;
  Rect d = R(0, 0, 240, 135);
  int over, i, a_jobs = 0;
  setup();
  a = wm_create("a", R(20, 20, 40, 40));
  b = wm_create("b", R(10, 10, 80, 80));   /* completely covers a */
  wm_take_damage(NULL, 0);
  wm_damage(d);
  run_paint();
  for (i = 0; i < g_jobs_n; i++) if (g_jobs[i].win == a) a_jobs++;
  CHECK_EQ(a_jobs, 0);
  CHECK_EQ(audit(d, &over), 0);
  (void)b;
}

void test_raising_changes_who_paints_the_overlap(void) {
  WinId a, b;
  Rect d = R(0, 0, 240, 135);
  int over;
  setup();
  a = wm_create("a", R(0, 0, 100, 100));
  b = wm_create("b", R(50, 0, 100, 100));
  wm_take_damage(NULL, 0);

  wm_damage(d);
  run_paint();
  CHECK_EQ(audit(d, &over), 0);

  wm_raise(a);
  wm_take_damage(NULL, 0);
  wm_damage(d);
  run_paint();
  CHECK_EQ(audit(d, &over), 0);      /* the oracle moved with it */
  (void)b;
}

/* Painting in call order must be correct, so lower windows come first. */
void test_calls_arrive_back_to_front(void) {
  int i, last = -2;
  setup();
  wm_create("a", R(0, 0, 120, 120));
  wm_create("b", R(30, 30, 120, 100));
  wm_create("c", R(60, 10, 100, 110));
  wm_take_damage(NULL, 0);
  wm_damage(R(0, 0, 240, 135));
  run_paint();
  for (i = 0; i < g_jobs_n; i++) {
    int z = (g_jobs[i].win == WIN_NONE) ? -1 : wm_z(g_jobs[i].win);
    CHECK(z >= last);
    last = z;
  }
}

/* Eight overlapping windows across the whole screen: the case that broke an
 * earlier version, which capped the job list and silently dropped the rest. */
void test_a_crowded_desktop_still_paints_every_pixel(void) {
  Rect d = R(0, 0, 240, 135);
  int over, i;
  setup();
  for (i = 0; i < WM_MAX_WINDOWS; i++)
    wm_create("w", R(i * 12, i * 8, 90, 50));
  wm_take_damage(NULL, 0);
  wm_damage(d);
  CHECK(run_paint() > 0);
  CHECK_EQ(audit(d, &over), 0);
  printf("    crowded desktop: %d paint calls, %d pixels overdrawn\n",
         g_jobs_n, over);
}

void test_painting_consumes_the_damage(void) {
  setup();
  wm_damage(R(0, 0, 20, 20));
  run_paint();
  CHECK_EQ(wm_damage_count(), 0);
  CHECK_EQ(run_paint(), 0);
}
