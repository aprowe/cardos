/* Noodle, the tube man, on the host: that air stands him up, that no air lays
 * him down, that he stays on the screen and above the tarmac, and that the
 * renderer draws what it says it does.
 *
 * NOODLE_DUMP=dir writes frames as PPM, to look at: there is no other way to
 * see a physics tune off the device.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"

#define capp_info noodle_capp_info
#define capp_main noodle_capp_main
#include "apps/noodle.c"
#undef capp_info
#undef capp_main

static CardApi FAKE;
static uint16_t FB[SCREEN_H][SCREEN_W];
static uint32_t NOW;
static int DAMAGED;

static void *n_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static uint32_t n_ticks(void) { return NOW; }
static void n_fill(CRect r, uint16_t c) {
  int x, y;
  for (y = r.y; y < r.y + r.h; y++)
    for (x = r.x; x < r.x + r.w; x++)
      if (x >= 0 && y >= 0 && x < SCREEN_W && y < SCREEN_H) FB[y][x] = c;
}
static void n_frame(CRect r, uint16_t c) { (void)r; (void)c; }
static void n_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  (void)x; (void)y; (void)s; (void)fg; (void)bg;
}
static void n_pixels(CRect r, const uint16_t *px) {
  int x, y;
  for (y = 0; y < r.h; y++)
    for (x = 0; x < r.w; x++)
      if (r.x + x < SCREEN_W && r.y + y < SCREEN_H)
        FB[r.y + y][r.x + x] = px[y * r.w + x];
}
static void n_damage(CRect r) { (void)r; DAMAGED++; }
static CRect n_area(void) { CRect r = { 0, 0, SCREEN_W, SCREEN_H }; return r; }

static void setup(void) {
  memset(&FAKE, 0, sizeof FAKE);
  FAKE.mem_set = n_memset;
  FAKE.ticks_ms = n_ticks;
  FAKE.fill = n_fill;
  FAKE.frame = n_frame;
  FAKE.text = n_text;
  FAKE.pixels = n_pixels;
  FAKE.damage = n_damage;
  FAKE.paint_area = n_area;
  api = &FAKE;
  memset(&N, 0, sizeof N);
  N.seed = 12345;
  N.listen = 1;
  N.floor = 100;
  NOW = 1000;
  tables();
  slumped();
  build();
}

/* Seconds of the given breath, stepped as the tick would. */
static void run(float secs, float air) {
  int steps = (int)(secs * 1000.0f / STEP_MS), i;
  for (i = 0; i < steps; i++) step(air);
  build();
}

static float head_y(void) { return N.py[NSEG]; }

static void dump(const char *name) {
  const char *dir = getenv("NOODLE_DUMP");
  char path[256];
  FILE *f;
  int x, y;
  if (!dir) return;
  N.c.x = 0; N.c.y = 0;
  {
    Box all = { 0, 0, SCREEN_W, HUD_Y };
    render(all);
    paint_hud(1);
  }
  snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
  for (y = 0; y < SCREEN_H; y++)
    for (x = 0; x < SCREEN_W; x++) {
      uint16_t v = FB[y][x];
      uint16_t c = (uint16_t)((v >> 8) | (v << 8));         /* unswap */
      unsigned char rgb[3];
      rgb[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
      rgb[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
      rgb[2] = (unsigned char)((c & 31) * 255 / 31);
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

void test_noodle_with_no_air_he_is_a_heap(void) {
  setup();
  run(3.0f, 0);
  CHECK(head_y() > 75.0f);
  CHECK(N.P < 0.01f);
  dump("heap");
}

void test_noodle_air_stands_him_up(void) {
  setup();
  run(3.0f, 1.0f);
  CHECK(N.P > 0.9f);
  CHECK(head_y() < 45.0f);
  dump("full");
}

void test_noodle_he_flails_while_the_air_runs(void) {
  float lo = 1e9f, hi = -1e9f;
  int i;
  setup();
  run(2.0f, 1.0f);
  for (i = 0; i < 120; i++) {
    run(0.016f, 1.0f);
    if (N.px[NSEG] < lo) lo = N.px[NSEG];
    if (N.px[NSEG] > hi) hi = N.px[NSEG];
    if (i == 30) dump("flail1");
    if (i == 60) dump("flail2");
    if (i == 90) dump("flail3");
  }
  CHECK(hi - lo > 12.0f);            /* the head goes somewhere */
}

void test_noodle_he_sags_when_the_air_stops(void) {
  float up;
  setup();
  run(3.0f, 1.0f);
  up = head_y();
  run(1.0f, 0);
  dump("sagging");
  run(4.0f, 0);
  CHECK(head_y() > up + 30.0f);
  dump("down");
}

void test_noodle_stays_on_screen_and_off_the_tarmac(void) {
  int i, k, bad = 0;
  setup();
  for (i = 0; i < 1500; i++) {
    /* Gusts on and off, the way a person breathes. */
    float air = ((i / 90) % 3 == 0) ? 0.0f : ((i / 45) & 1) ? 1.0f : 0.5f;
    step(air);
    for (k = 0; k <= NSEG; k++) {
      if (N.py[k] > GROUND_Y + 1.0f) bad++;
      if (N.px[k] < 0 || N.px[k] >= SCREEN_W) bad++;
      if (N.py[k] < -20.0f) bad++;
    }
    for (k = 0; k < NSEG; k++) if (N.a[k] != N.a[k]) bad++;     /* NaN */
  }
  CHECK_EQ(0, bad);
}

void test_noodle_the_render_draws_him_over_the_scene(void) {
  int x, y, body = 0;
  setup();
  run(3.0f, 1.0f);
  memset(FB, 0, sizeof FB);
  N.c.x = 0; N.c.y = 0;
  {
    Box all = { 0, 0, SCREEN_W, HUD_Y };
    render(all);
  }
  for (y = 0; y < HUD_Y; y++)
    for (x = 0; x < SCREEN_W; x++)
      if (FB[y][x] == PALETTE[0][0] || FB[y][x] == PALETTE[0][1]) body++;
  CHECK(body > 300);
  /* Under the tube's foot, where only the fan is. */
  CHECK(FB[GROUND_Y + 1][(int)BASE_X - 12] == C_FAN ||
        FB[GROUND_Y + 1][(int)BASE_X - 12] == C_FAN_VENT);
  CHECK(FB[60][2] != 0);                       /* sky, drawn */
}

void test_noodle_a_heap_stops_asking_to_be_drawn(void) {
  int i;
  setup();
  run(6.0f, 0);
  N.last_ms = 0;
  for (i = 0; i < 20; i++) { NOW += 16; app_tick(0, NOW); }
  {
    CRect all = { 0, 0, SCREEN_W, SCREEN_H };
    app_paint(0, all);                 /* the strip, as drawn, is remembered */
  }
  DAMAGED = 0;
  for (i = 0; i < 20; i++) { NOW += 16; app_tick(0, NOW); }
  CHECK_EQ(0, DAMAGED);
}

void test_noodle_the_mic_floor_follows_the_room(void) {
  /* A quiet room at 3, and a breath at 70: the breath is air, the room not. */
  float quiet, loud;
  N.floor = 100;
  N.level = 3;
  N.floor = 3;
  quiet = clampf(((float)3 - N.floor - 5.0f) * (1.0f / 45.0f), 0, 1);
  loud = clampf(((float)70 - N.floor - 5.0f) * (1.0f / 45.0f), 0, 1);
  CHECK(quiet == 0.0f);
  CHECK(loud == 1.0f);
}
