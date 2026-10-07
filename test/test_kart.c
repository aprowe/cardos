/* Kart on the host: the track is painted where the waypoints say, the
 * angles come out right, a race with every kart driven by the AI finishes
 * in a sensible time with every place given once, and a banana spins a
 * kart.
 *
 * KART_DUMP=dir writes frames as PPM -- the countdown, the start, a corner,
 * the finish -- because there is no other way to see the renderer off the
 * device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"

/* Plain RGB565 here, so a dump can be read back as colour. */
#define K_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#include "apps/kart.h"

static uint16_t FRAME[K_H][K_W];

static void render_all(void) {
  int y;
  k_camera();
  for (y = 0; y < K_H; y += K_STRIP) k_render(&FRAME[y][0], y, K_STRIP);
}

static void dump(const char *name) {
  const char *dir = getenv("KART_DUMP");
  char path[512];
  FILE *f;
  int x, y;
  if (!dir) return;
  render_all();
  snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", K_W, K_H);
  for (y = 0; y < K_H; y++)
    for (x = 0; x < K_W; x++) {
      uint16_t c = FRAME[y][x];
      fputc((c >> 11) << 3, f); fputc(((c >> 5) & 63) << 2, f); fputc((c & 31) << 3, f);
    }
  fclose(f);
}

void test_kart_track_is_painted(void) {
  int i, x, y;
  k_build();
  for (i = 0; i < K_NWP; i += 8) {
    int t = k_ground(K_WP[i][0], K_WP[i][1]);
    CHECK(t == T_ROAD || t == T_START);
  }
  CHECK_EQ(k_ground(K_WP[0][0], K_WP[0][1]), T_START);
  /* the middle of the world is not road */
  CHECK(k_ground(300, 600) == T_GRASS || k_ground(300, 600) == T_GRASS2);
  CHECK_EQ(k_ground(-5, 10), T_OUT);
  for (i = 0; i < K_NTREE; i++) CHECK(k_clear_of_road(K.tree[i][0], K.tree[i][1], 10));
  /* the edge of the road has a kerb outside it */
  k_across(64, K_ROAD_R + 4, &x, &y);
  i = k_ground(x, y);
  CHECK(i == T_KERB_R || i == T_KERB_W);
}

void test_kart_atan2(void) {
  CHECK_EQ(k_atan2(0, 10), 0);
  CHECK_EQ(k_atan2(10, 0), 256);
  CHECK_EQ(k_atan2(0, -10), 512);
  CHECK_EQ(k_atan2(-10, 0), 768);
  CHECK(abs(k_atan2(10, 10) - 128) <= 2);
  CHECK(abs(k_atan2(-7, 12) - (1024 - 85)) <= 3);     /* atan(7/12) = 30.3 deg = 86 */
}

/* Every kart on the AI, the player too, to the end. */
static int race_to_end(uint32_t seed, int max_s) {
  int t;
  k_build();
  k_new_race(seed);
  for (t = 0; t < max_s * 1000; t += K_STEP_MS) {
    int i, all = 1;
    k_step(K_STEP_MS, k_cpu_steer(&K.k[0]), 0, K.k[0].item != 0);
    for (i = 0; i < K_KARTS; i++) all &= K.k[i].done;
    if (all) return t;
  }
  return -1;
}

void test_kart_a_race_finishes(void) {
  int t = race_to_end(7, 600), i, j, seen = 0;
  CHECK(t > 0);
  if (t < 0) {
    for (i = 0; i < K_KARTS; i++)
      printf("    kart %d lap %d wp %d done %d\n", i, K.k[i].lap, K.k[i].wp, K.k[i].done);
    return;
  }
  /* Three laps of about 3500 texels at 100-150 a second: a minute or two. */
  printf("    race over at %d s; winner %d s\n", t / 1000, (int)(K.k[0].done_ms / 1000));
  CHECK(t > 45 * 1000 && t < 240 * 1000);
  for (i = 0; i < K_KARTS; i++) {
    CHECK(K.k[i].place >= 1 && K.k[i].place <= K_KARTS);
    for (j = 0; j < i; j++) CHECK(K.k[i].place != K.k[j].place);
    seen |= 1 << K.k[i].place;
  }
  CHECK_EQ(seen, 0x7E);
}

void test_kart_stays_on_the_road(void) {
  /* The AI's line must keep it on the road nearly all the time: a kart in
   * the grass is a racing line that cuts corners. */
  int t, on = 0, n = 0;
  k_build();
  k_new_race(3);
  for (t = 0; t < 60000; t += K_STEP_MS) {
    int g;
    k_step(K_STEP_MS, k_cpu_steer(&K.k[0]), 0, 0);
    if (K.phase != PH_RACE) continue;
    g = k_ground(K.k[1].x >> 8, K.k[1].y >> 8);
    n++;
    if (g == T_ROAD || g == T_START || g == T_KERB_R || g == T_KERB_W) on++;
  }
  printf("    on the road %d%% of the time\n", on * 100 / (n ? n : 1));
  CHECK(on * 100 / (n ? n : 1) >= 90);
}

void test_kart_banana_spins(void) {
  k_build();
  k_new_race(1);
  K.phase = PH_RACE;
  K.ban[0].on = 1;
  K.ban[0].x = K.k[0].x + (k_cos(K.k[0].ang >> 6) * 4 >> 6);
  K.ban[0].y = K.k[0].y + (k_sin(K.k[0].ang >> 6) * 4 >> 6);
  K.k[0].spd = 100 * 256;
  k_step(K_STEP_MS, 0, 0, 0);
  CHECK(K.k[0].spin_ms > 0);
  CHECK_EQ(K.ban[0].on, 0);
}

void test_kart_frames(void) {
  int t;
  k_build();
  k_new_race(5);
  dump("kart-0-countdown");
  for (t = 0; t < 4000; t += K_STEP_MS) k_step(K_STEP_MS, k_cpu_steer(&K.k[0]), 0, 0);
  dump("kart-1-go");
  for (t = 0; t < 9000; t += K_STEP_MS) k_step(K_STEP_MS, k_cpu_steer(&K.k[0]), 0, 0);
  dump("kart-2-racing");
  for (t = 0; t < 9000; t += K_STEP_MS) k_step(K_STEP_MS, k_cpu_steer(&K.k[0]), 0, 0);
  dump("kart-3-racing");
  K.k[0].item = IT_MUSHROOM;
  K.k[0].steer = 200;
  dump("kart-4-item-lean");
  CHECK(1);
}
