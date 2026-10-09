/* draw_offscreen against a fake panel: what reaches the panel, and how many
 * times, when a screen is composed off it.
 *
 * The panel here is a 240x135 array that counts writes per pixel. A screen
 * that flickers is one where some pixel is written twice in one repaint --
 * a fill, then what is drawn over it -- so "every pixel written once" is the
 * property the OS's off-panel repaint exists for. */
#include "tinytest.h"
#include "kernel/ui/draw.h"
#include "kernel/drv/display.h"

#include <string.h>

static uint16_t s_panel[DISPLAY_H][DISPLAY_W];
static uint8_t  s_writes[DISPLAY_H][DISPLAY_W];
static int      s_blits;

/* The device's display_blit, minus the SPI: a target takes it first, as on
 * the device, and only what is left reaches the panel. */
void display_blit(int x, int y, int w, int h, const uint16_t *px) {
  int r, c;
  if (w <= 0 || h <= 0) return;
  if (disptarget_take(x, y, w, h, px)) return;
  s_blits++;
  for (r = 0; r < h; r++)
    for (c = 0; c < w; c++) {
      int py = y + r, pxx = x + c;
      if (py < 0 || py >= DISPLAY_H || pxx < 0 || pxx >= DISPLAY_W) continue;
      s_panel[py][pxx] = px[r * w + c];
      if (s_writes[py][pxx] < 255) s_writes[py][pxx]++;
    }
}

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static void reset(void) {
  memset(s_panel, 0, sizeof s_panel);
  memset(s_writes, 0, sizeof s_writes);
  s_blits = 0;
  display_target(NULL, 0, 0, 0, 0);
  draw_reserve_top(0);
  draw_occlude(R(0, 0, 0, 0));
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}

/* Most writes any pixel in r got, and the fewest. */
static int most_writes(Rect r) {
  int x, y, m = 0;
  for (y = r.y; y < r.y + r.h; y++)
    for (x = r.x; x < r.x + r.w; x++) if (s_writes[y][x] > m) m = s_writes[y][x];
  return m;
}
static int fewest_writes(Rect r) {
  int x, y, m = 255;
  for (y = r.y; y < r.y + r.h; y++)
    for (x = r.x; x < r.x + r.w; x++) if (s_writes[y][x] < m) m = s_writes[y][x];
  return m;
}
static int all_colour(Rect r, uint16_t c) {
  int x, y;
  for (y = r.y; y < r.y + r.h; y++)
    for (x = r.x; x < r.x + r.w; x++) if (s_panel[y][x] != c) return 0;
  return 1;
}

#define BG  ((uint16_t)0x1111)
#define INK ((uint16_t)0x2222)
#define PRE ((uint16_t)0x3333)
#define BOX ((uint16_t)0x4444)

/* A screen that clears itself and draws over the clear: the pattern that
 * flickers on the panel. */
static int s_calls;
static Rect s_seen_area, s_seen_clip;
static int s_area_steady;
static void clear_and_draw(void *ctx) {
  Rect a = draw_paint_area();
  (void)ctx;
  if (s_calls == 0) s_seen_area = a;
  else if (!rect_equals(a, s_seen_area)) s_area_steady = 0;
  s_seen_clip = draw_clip();
  s_calls++;
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), BG);
  draw_rect(R(10, 10, 50, 30), INK);
  draw_text(20, 100, "hello", INK, BG);
}

void test_draw_offscreen_sends_each_pixel_once(void) {
  reset();
  s_calls = 0; s_area_steady = 1;
  draw_offscreen(R(0, 0, DISPLAY_W, DISPLAY_H), PRE, clear_and_draw, NULL);
  CHECK_EQ(most_writes(R(0, 0, DISPLAY_W, DISPLAY_H)), 1);
  CHECK_EQ(fewest_writes(R(0, 0, DISPLAY_W, DISPLAY_H)), 1);
  CHECK(all_colour(R(10, 10, 50, 30), INK));
  CHECK(all_colour(R(100, 50, 20, 20), BG));
  CHECK_EQ(s_blits, (DISPLAY_H + 15) / 16);    /* a blit a strip, nothing else */
  CHECK(s_calls > 1);                          /* it really was strips */
  CHECK(s_area_steady);                        /* and paint_area never said so */
  CHECK(rect_equals(s_seen_area, R(0, 0, DISPLAY_W, DISPLAY_H)));
  CHECK(s_seen_clip.h <= 16);
  /* Everything put back. */
  CHECK(!draw_composing());
  CHECK(rect_equals(draw_clip(), R(0, 0, DISPLAY_W, DISPLAY_H)));
}

/* The same body straight onto the panel, for contrast: the clear shows. */
void test_draw_direct_writes_twice(void) {
  reset();
  s_calls = 0; s_area_steady = 1;
  clear_and_draw(NULL);
  CHECK_EQ(most_writes(R(10, 10, 50, 30)), 2);
}

static void draw_nothing(void *ctx) { (void)ctx; }

void test_draw_offscreen_prefills(void) {
  reset();
  draw_offscreen(R(30, 20, 100, 40), PRE, draw_nothing, NULL);
  CHECK(all_colour(R(30, 20, 100, 40), PRE));
  CHECK_EQ(most_writes(R(30, 20, 100, 40)), 1);
  CHECK_EQ(most_writes(R(0, 0, 30, DISPLAY_H)), 0);       /* nothing outside */
  CHECK_EQ(most_writes(R(130, 0, DISPLAY_W - 130, DISPLAY_H)), 0);
}

void test_draw_offscreen_stays_inside_the_clip(void) {
  reset();
  draw_set_clip(R(0, 40, DISPLAY_W, 20));                 /* a damage rect */
  s_calls = 0; s_area_steady = 1;
  draw_offscreen(R(0, 0, DISPLAY_W, DISPLAY_H), PRE, clear_and_draw, NULL);
  CHECK_EQ(most_writes(R(0, 0, DISPLAY_W, 40)), 0);
  CHECK_EQ(most_writes(R(0, 60, DISPLAY_W, DISPLAY_H - 60)), 0);
  CHECK_EQ(fewest_writes(R(0, 40, DISPLAY_W, 20)), 1);
  CHECK(rect_equals(s_seen_area, R(0, 40, DISPLAY_W, 20)));
  CHECK(rect_equals(draw_clip(), R(0, 40, DISPLAY_W, 20)));
}

/* An inner draw_offscreen -- Settings painting itself inside a shell that
 * composes the app's repaint -- lands in the outer strip, and the panel
 * still sees each pixel once. */
static void inner_body(void *ctx) {
  (void)ctx;
  draw_rect(R(60, 30, 20, 50), BOX);
}
static void outer_body(void *ctx) {
  (void)ctx;
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), BG);
  draw_offscreen(R(50, 20, 100, 80), PRE, inner_body, NULL);
  CHECK(draw_composing());                      /* the outer target is back */
  draw_rect(R(140, 90, 30, 30), INK);           /* drawn after, over the inner */
}

void test_draw_offscreen_nests(void) {
  reset();
  draw_offscreen(R(0, 0, DISPLAY_W, DISPLAY_H), C_WHITE, outer_body, NULL);
  CHECK_EQ(most_writes(R(0, 0, DISPLAY_W, DISPLAY_H)), 1);
  CHECK_EQ(fewest_writes(R(0, 0, DISPLAY_W, DISPLAY_H)), 1);
  CHECK(all_colour(R(60, 30, 20, 50), BOX));     /* the inner body */
  CHECK(all_colour(R(50, 20, 10, 80), PRE));     /* the inner prefill */
  CHECK(all_colour(R(140, 90, 30, 30), INK));    /* outer, drawn after it */
  CHECK(all_colour(R(0, 0, 50, 20), BG));        /* outer, outside the inner */
  CHECK(!draw_composing());
}

/* A body that sets its own clip -- a window's chrome, then its content well
 * -- cannot reach outside the strip being composed. */
static void wide_clip_body(void *ctx) {
  (void)ctx;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  CHECK(draw_clip().h <= 16 || draw_clip().h == 0);
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), INK);
}

void test_draw_offscreen_bounds_a_body_that_sets_its_clip(void) {
  reset();
  draw_offscreen(R(0, 32, DISPLAY_W, 32), PRE, wide_clip_body, NULL);
  CHECK_EQ(most_writes(R(0, 0, DISPLAY_W, 32)), 0);
  CHECK_EQ(most_writes(R(0, 64, DISPLAY_W, DISPLAY_H - 64)), 0);
  CHECK(all_colour(R(0, 32, DISPLAY_W, 32), INK));
}

/* A narrow area is composed in fewer, taller strips: the static strip is
 * pixels, not rows. */
void test_draw_offscreen_narrow_area_takes_fewer_strips(void) {
  reset();
  s_calls = 0; s_area_steady = 1;
  draw_offscreen(R(70, 0, 90, DISPLAY_H), PRE, clear_and_draw, NULL);
  CHECK_EQ(s_calls, (DISPLAY_H + (DISPLAY_W * 16 / 90) - 1) / (DISPLAY_W * 16 / 90));
  CHECK_EQ(fewest_writes(R(70, 0, 90, DISPLAY_H)), 1);
  CHECK_EQ(most_writes(R(70, 0, 90, DISPLAY_H)), 1);
}

void test_draw_offscreen_keeps_out_of_the_banner(void) {
  reset();
  draw_reserve_top(24);
  draw_offscreen(R(0, 0, DISPLAY_W, DISPLAY_H), PRE, draw_nothing, NULL);
  CHECK_EQ(most_writes(R(0, 0, DISPLAY_W, 24)), 0);
  CHECK(all_colour(R(0, 24, DISPLAY_W, DISPLAY_H - 24), PRE));
  draw_reserve_top(0);
}

/* The alarm's panel: nothing else draws in it, and a clip that reaches in is
 * cut to the biggest piece left outside. */
void test_draw_occluder_keeps_others_out(void) {
  Rect panel = R(8, 15, 224, 104);
  reset();
  draw_occlude(panel);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  CHECK(rect_equals(draw_clip(), R(0, 119, DISPLAY_W, 16)));   /* below is bigger */
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), INK);
  CHECK_EQ(most_writes(panel), 0);
  draw_set_clip(R(20, 20, 50, 50));                             /* wholly inside */
  CHECK(rect_is_empty(draw_clip()));
  draw_set_clip(R(0, 30, 30, 40));                              /* left edge */
  CHECK(rect_equals(draw_clip(), R(0, 30, 8, 40)));
  draw_occlude(R(0, 0, 0, 0));
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  CHECK(rect_equals(draw_clip(), R(0, 0, DISPLAY_W, DISPLAY_H)));
}
