/* Kart: the game, without the device -- the track, the karts, the race and
 * the picture. apps/kart.c is the app around it (screens, tilt, timing,
 * blitting); test/test_kart.c drives it on the PC, and KART_DUMP=dir there
 * writes frames to look at.
 *
 * THE FLOOR is Mode 7, as the SNES did it: every screen row below the
 * horizon is one line across the ground at the distance that row looks at,
 * stepped across in equal world steps, each pixel a lookup in the track's
 * map. The map is 128x128 cells of 8x8 texels -- a 1024-texel world -- four
 * bits a cell (8 KB), painted at start from the waypoints in kart_data.h:
 * discs of road along the racing line, a ring of kerb outside them, grass
 * in big checks everywhere else.
 *
 * EVERYTHING ELSE is a billboard: karts, item boxes, bananas, coins, trees,
 * each a 16x16 sprite scaled by its distance and drawn far to near. A kart
 * is drawn from whichever of four sides (and their mirrors) faces the
 * camera.
 *
 * THE PICTURE is made a strip of rows at a time into a small buffer and
 * blitted whole: sky, floor, sprites, then the HUD in the 6x8 font. No
 * flicker, because nothing is drawn on the panel twice.
 *
 * Fixed point throughout, and nothing 64-bit: an app links no libgcc.
 * Positions are texels x256, angles 0..65535 for a turn (the sine table
 * takes the top ten bits), speeds texels x256 a second.
 */
#ifndef KART_H
#define KART_H

#include <stdint.h>
#include "apps/kart_data.h"
#include "kernel/console/font6x8.h"

#ifndef K_RGB
#define K_RGB(r, g, b) CAPP_RGB(r, g, b)
#endif

#define K_W        240
#define K_H        135
#define K_HZ       40              /* the horizon's row */
#define K_FOCAL    140
#define K_CAMH     18              /* the camera's height, texels */
#define K_CAMBACK  30              /* and how far behind the player */
#define K_STRIP    15              /* rows a strip */
#define K_LAPS     3
#define K_KARTS    6
#define K_STEP_MS  16

#define K_MAPN     128
#define K_WORLD    1024

enum { T_GRASS, T_GRASS2, T_ROAD, T_KERB_R, T_KERB_W, T_START, T_SAND, T_OUT, T_COUNT };
enum { IT_NONE, IT_MUSHROOM, IT_BANANA };
enum { PH_COUNT, PH_RACE, PH_DONE };

#define K_NBOX     12
#define K_NBAN     10
#define K_NCOIN    25
#define K_NTREE    36
#define K_NSPR     80

typedef struct {
  int32_t  x, y;          /* texels x256 */
  uint16_t ang;           /* heading, 0..65535 a turn */
  int32_t  spd;           /* texels x256 a second */
  int      wp, lap;       /* the waypoint it is nearest, laps begun */
  int      done;          /* finished; place is final */
  int      place;
  uint32_t done_ms;
  int      spin_ms, boost_ms;
  int      item, coins;
  int      cpu, skill;    /* skill: % of top speed */
  int      lane;          /* -1..1, where a CPU drives across the road */
  int      steer;         /* -256..256, as driven: the sprite leans with it */
  uint16_t body, shade;   /* its colours */
  int      item_wait;     /* CPU: ms before it uses what it holds */
} Kart;

typedef struct { int32_t x, y; int on; uint32_t back_ms; } KBox;
typedef struct { int32_t x, y; int on; } KThing;

typedef struct {
  int16_t depth;          /* texels; far first */
  int16_t sx, sy, size;   /* centre x, ground y, height on screen */
  uint8_t spr, mirror, kart;
} KSpr;

static struct {
  uint8_t  map[K_MAPN * K_MAPN / 2];
  uint16_t tex[T_COUNT][64];
  uint16_t pal[16];
  uint8_t  hills[256];
  Kart     k[K_KARTS];
  KBox     box[K_NBOX];
  KThing   ban[K_NBAN];
  KThing   coin[K_NCOIN];
  int16_t  tree[K_NTREE][2];
  int      phase;
  uint32_t t_ms;          /* race clock: from GO */
  int      count_ms;      /* the countdown, 3000 down to 0 */
  int      done_ms;       /* since the player finished */
  uint32_t rng;
  /* the camera, this frame */
  int32_t  camx, camy;    /* texels x256 */
  int      cfx, cfy;      /* forward, Q14 */
  KSpr     spr[K_NSPR];
  int      nspr;
  uint16_t sky[K_HZ];
} K;

/* ---- maths ----------------------------------------------------------------------- */

static int k_sin(int a1024) {
  a1024 &= 1023;
  if (a1024 < 256) return K_SIN[a1024];
  if (a1024 < 512) return K_SIN[512 - a1024];
  if (a1024 < 768) return -K_SIN[a1024 - 512];
  return -K_SIN[1024 - a1024];
}
static int k_cos(int a1024) { return k_sin(a1024 + 256); }

/* atan2 in 1024 to the turn, y down: 0 is +x, 256 is +y. Good to a few
 * units -- (pi/4)r + 0.273 r(1-r) on each octant. */
static int k_atan2(int y, int x) {
  int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, r, a;
  if (!ax && !ay) return 0;
  if (ax >= ay) { r = (ay << 10) / ax; a = (128 * r + 44 * (r * (1024 - r) >> 10)) >> 10; }
  else { r = (ax << 10) / ay; a = 256 - ((128 * r + 44 * (r * (1024 - r) >> 10)) >> 10); }
  if (x < 0) a = 512 - a;
  if (y < 0) a = -a;
  return a & 1023;
}

static int k_isqrt(int v) {
  int r = 0, b = 1 << 30;
  if (v <= 0) return 0;
  while (b > v) b >>= 2;
  while (b) {
    if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
    else r >>= 1;
    b >>= 2;
  }
  return r;
}

static uint32_t k_rand(void) {
  uint32_t x = K.rng ? K.rng : 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return K.rng = x;
}

/* ---- the map --------------------------------------------------------------------- */

static int k_cell(int cx, int cy) {
  uint8_t b;
  if ((unsigned)cx >= K_MAPN || (unsigned)cy >= K_MAPN) return T_OUT;
  b = K.map[(cy * K_MAPN + cx) >> 1];
  return (cx & 1) ? b >> 4 : b & 15;
}
static void k_set(int cx, int cy, int t) {
  uint8_t *b;
  if ((unsigned)cx >= K_MAPN || (unsigned)cy >= K_MAPN) return;
  b = &K.map[(cy * K_MAPN + cx) >> 1];
  *b = (cx & 1) ? (uint8_t)((*b & 15) | t << 4) : (uint8_t)((*b & 0xF0) | t);
}

/* What is under a point, texels. */
static int k_ground(int tx, int ty) { return k_cell(tx >> 3, ty >> 3); }

/* Cells whose centre is within r texels of (x, y) become t, if `over` says
 * they may: grass for the kerb, anything for the road. */
static void k_disc(int x, int y, int r, int t, int only_grass) {
  int cx, cy, c0 = (x - r) >> 3, c1 = (x + r) >> 3, r0 = (y - r) >> 3, r1 = (y + r) >> 3;
  for (cy = r0; cy <= r1; cy++)
    for (cx = c0; cx <= c1; cx++) {
      int dx = cx * 8 + 4 - x, dy = cy * 8 + 4 - y, now;
      if (dx * dx + dy * dy > r * r) continue;
      now = k_cell(cx, cy);
      if (only_grass && now != T_GRASS && now != T_GRASS2) continue;
      k_set(cx, cy, t);
    }
}

/* Along the racing line, a point every few texels. */
static void k_along(int r, int t, int only_grass) {
  int i, s;
  for (i = 0; i < K_NWP; i++) {
    int ax = K_WP[i][0], ay = K_WP[i][1];
    int bx = K_WP[(i + 1) % K_NWP][0], by = K_WP[(i + 1) % K_NWP][1];
    for (s = 0; s < 4; s++)
      k_disc(ax + (bx - ax) * s / 4, ay + (by - ay) * s / 4, r, t, only_grass);
  }
}

static uint16_t k_mix(int r, int g, int b, int v) {
  r += v; g += v; b += v;
  r = r < 0 ? 0 : r > 255 ? 255 : r;
  g = g < 0 ? 0 : g > 255 ? 255 : g;
  b = b < 0 ? 0 : b > 255 ? 255 : b;
  return K_RGB(r, g, b);
}

/* The 8x8 texture of each kind of ground, and the sky's colours. */
static void k_textures(void) {
  int i, t;
  for (i = 0; i < 64; i++) {
    uint32_t h = (uint32_t)i * 2654435761u;
    int n = (int)((h >> 27) & 7) - 3;             /* -3..4: speckle */
    int x = i & 7, y = i >> 3;
    K.tex[T_GRASS][i]  = k_mix(64, 168, 64, n * 3);
    K.tex[T_GRASS2][i] = k_mix(52, 148, 54, n * 3);
    K.tex[T_ROAD][i]   = k_mix(108, 108, 116, n * 4);
    K.tex[T_KERB_R][i] = k_mix(214, 44, 40, n * 2);
    K.tex[T_KERB_W][i] = k_mix(236, 236, 236, n * 2);
    K.tex[T_START][i]  = ((x >> 1) ^ (y >> 1)) & 1 ? K_RGB(240, 240, 240) : K_RGB(24, 24, 28);
    K.tex[T_SAND][i]   = k_mix(214, 188, 120, n * 4);
    K.tex[T_OUT][i]    = k_mix(52, 104, 196, (x + y) & 2 ? 10 : 0);
  }
  for (t = 0; t < 16; t++) K.pal[t] = K_RGB(K_SPR_RGB[t][0], K_SPR_RGB[t][1], K_SPR_RGB[t][2]);
  for (i = 0; i < K_HZ; i++)
    K.sky[i] = K_RGB(84 + i * 100 / K_HZ, 140 + i * 80 / K_HZ, 236 + i * 14 / K_HZ);
  /* Hills on the horizon: a few sines, 256 across a turn's worth of sky. */
  for (i = 0; i < 256; i++) {
    int h = 7 + (k_sin(i * 4) * 3 >> 14) + (k_sin(i * 12 + 100) * 2 >> 14) + (k_sin(i * 28 + 37) >> 14);
    K.hills[i] = (uint8_t)(h < 1 ? 1 : h);
  }
}

/* Is (x, y) texels well clear of the road? */
static int k_clear_of_road(int x, int y, int margin) {
  int i;
  for (i = 0; i < K_NWP; i += 2) {
    int dx = x - K_WP[i][0], dy = y - K_WP[i][1];
    if (dx * dx + dy * dy < (K_ROAD_R + margin) * (K_ROAD_R + margin)) return 0;
  }
  return 1;
}

/* A point across the road at waypoint w: `lat` texels to the right. */
static void k_across(int w, int lat, int *x, int *y) {
  int a = K_WP[(w + K_NWP - 1) % K_NWP][0], b = K_WP[(w + K_NWP - 1) % K_NWP][1];
  int c = K_WP[(w + 1) % K_NWP][0], d = K_WP[(w + 1) % K_NWP][1];
  int ang = k_atan2(d - b, c - a);
  *x = K_WP[w][0] + (-k_sin(ang) * lat >> 14);
  *y = K_WP[w][1] + (k_cos(ang) * lat >> 14);
}

static void k_build(void) {
  int cx, cy, i, n;
  k_textures();
  for (cy = 0; cy < K_MAPN; cy++)
    for (cx = 0; cx < K_MAPN; cx++)
      k_set(cx, cy, ((cx >> 2) ^ (cy >> 2)) & 1 ? T_GRASS2 : T_GRASS);
  k_along(K_ROAD_R + 6, T_KERB_R, 1);
  for (cy = 0; cy < K_MAPN; cy++)              /* the kerb in red and white blocks */
    for (cx = 0; cx < K_MAPN; cx++)
      if (k_cell(cx, cy) == T_KERB_R && ((cx + cy) & 1)) k_set(cx, cy, T_KERB_W);
  k_along(K_ROAD_R, T_ROAD, 0);
  /* The start line, square across the road at waypoint 0. */
  for (i = -K_ROAD_R; i <= K_ROAD_R; i += 4) {
    int x, y;
    k_across(0, i, &x, &y);
    k_set(x >> 3, y >> 3, T_START);
  }
  /* Trees in the grass, where they will be seen: near the road, not on it. */
  K.rng = 12345;
  for (n = 0, i = 0; n < K_NTREE && i < 4000; i++) {
    int w = (int)(k_rand() % K_NWP), side = (k_rand() & 1) ? 1 : -1;
    int lat = side * (K_ROAD_R + 24 + (int)(k_rand() % 60)), x, y;
    k_across(w, lat, &x, &y);
    if (x < 16 || y < 16 || x > K_WORLD - 16 || y > K_WORLD - 16) continue;
    if (!k_clear_of_road(x, y, 18)) continue;
    K.tree[n][0] = (int16_t)x; K.tree[n][1] = (int16_t)y;
    n++;
  }
  for (; n < K_NTREE; n++) { K.tree[n][0] = 4; K.tree[n][1] = 4; }
}

/* ---- the race ------------------------------------------------------------------ */

static const uint8_t K_COLOURS[K_KARTS][3] = {
  { 220, 40, 40 }, { 40, 170, 60 }, { 240, 200, 40 }, { 60, 110, 230 }, { 230, 120, 200 }, { 250, 140, 30 },
};

static void k_new_race(uint32_t seed) {
  int i, j;
  K.rng = seed ? seed : 1;
  for (i = 0; i < K_KARTS; i++) {
    Kart *k = &K.k[i];
    int row = i / 2, col = (i & 1) ? 1 : -1, x, y;
    int w = (K_NWP - 2 - row * 2) % K_NWP;
    k_across(w, col * 14, &x, &y);
    for (j = 0; j < (int)sizeof *k; j++) ((uint8_t *)k)[j] = 0;
    k->x = x << 8; k->y = y << 8;
    k->ang = (uint16_t)(k_atan2(K_WP[1][1] - K_WP[K_NWP - 1][1], K_WP[1][0] - K_WP[K_NWP - 1][0]) << 6);
    k->wp = w;
    k->lap = 0;
    k->cpu = i != 0;
    k->skill = 90 + (int)((i * 37) % 13);          /* 90..102 % */
    k->lane = (int)(k_rand() % 3) - 1;
    k->body = K_RGB(K_COLOURS[i][0], K_COLOURS[i][1], K_COLOURS[i][2]);
    k->shade = K_RGB(K_COLOURS[i][0] * 2 / 3, K_COLOURS[i][1] * 2 / 3, K_COLOURS[i][2] * 2 / 3);
    k->item_wait = 2000 + (int)(k_rand() % 4000);
  }
  /* The player starts at the back of the grid, as tradition has it. */
  {                       /* fields, not the struct: that would be a memcpy */
    Kart *a = &K.k[0], *z = &K.k[K_KARTS - 1];
    int32_t px = z->x, py = z->y;
    int pw = z->wp;
    z->x = a->x; z->y = a->y; z->wp = a->wp;
    a->x = px; a->y = py; a->wp = pw;
  }
  for (i = 0; i < K_NBOX; i++) {
    static const int WPS[4] = { 40, 102, 160, 216 };
    int x, y;
    k_across(WPS[i / 3], (i % 3 - 1) * 16, &x, &y);
    K.box[i].x = x << 8; K.box[i].y = y << 8; K.box[i].on = 1;
  }
  for (i = 0; i < K_NBAN; i++) K.ban[i].on = 0;
  for (i = 0; i < K_NCOIN; i++) {
    static const int WPS[5] = { 16, 70, 128, 186, 236 };
    int x, y;
    k_across(WPS[i / 5] + i % 5, ((i / 5) % 2 ? 1 : -1) * 12, &x, &y);
    K.coin[i].x = x << 8; K.coin[i].y = y << 8; K.coin[i].on = 1;
  }
  K.phase = PH_COUNT;
  K.count_ms = 3000;
  K.t_ms = 0;
  K.done_ms = 0;
}

/* The nearest waypoint, looked for a little way either side of the last. */
static void k_progress(Kart *k) {
  int best = k->wp, bd = 1 << 30, d, i;
  for (d = -3; d <= 8; d++) {
    int w = (k->wp + d + K_NWP) % K_NWP;
    int dx = (k->x >> 8) - K_WP[w][0], dy = (k->y >> 8) - K_WP[w][1];
    int dd = dx * dx + dy * dy;
    if (dd < bd) { bd = dd; best = w; }
  }
  i = k->wp;
  if (i > K_NWP - 16 && best < 16) k->lap++;          /* over the line, forwards */
  else if (i < 16 && best > K_NWP - 16) k->lap--;     /* back over it */
  k->wp = best;
}

static int k_score(const Kart *k) { return k->lap * K_NWP + k->wp; }

static void k_places(void) {
  int i, j;
  for (i = 0; i < K_KARTS; i++) {
    int p = 1;
    if (K.k[i].done) continue;
    for (j = 0; j < K_KARTS; j++) {
      if (j == i) continue;
      if (K.k[j].done || k_score(&K.k[j]) > k_score(&K.k[i]) ||
          (k_score(&K.k[j]) == k_score(&K.k[i]) && j < i)) p++;
    }
    K.k[i].place = p;
  }
}

static int k_top_speed(const Kart *k) {
  int g, top = 150 * 256;
  top += top * k->coins * 2 / 100;                         /* coins: +2% each */
  if (k->cpu) {
    int lead = k_score(k) - k_score(&K.k[0]);            /* rubber band */
    top = top * k->skill / 100;
    if (lead > 24) top = top * 90 / 100;
    else if (lead < -24) top = top * 108 / 100;
  }
  g = k_ground(k->x >> 8, k->y >> 8);
  if (g == T_GRASS || g == T_GRASS2 || g == T_OUT) top = top * 45 / 100;
  else if (g == T_KERB_R || g == T_KERB_W) top = top * 85 / 100;
  if (k->boost_ms > 0) top = 230 * 256;
  return top;
}

/* The CPU's hands: aim at a point a few waypoints on, in its lane. */
static int k_cpu_steer(Kart *k) {
  int x, y, want, diff;
  k_across((k->wp + 5) % K_NWP, k->lane * K_ROAD_R / 2, &x, &y);
  want = k_atan2(y - (k->y >> 8), x - (k->x >> 8));
  diff = ((want - (k->ang >> 6) + 512) & 1023) - 512;
  diff *= 10;
  return diff > 256 ? 256 : diff < -256 ? -256 : diff;
}

static void k_use_item(Kart *k) {
  int i;
  if (k->item == IT_MUSHROOM) { k->boost_ms = 1200; if (k->spd < 200 * 256) k->spd = 200 * 256; }
  else if (k->item == IT_BANANA)
    for (i = 0; i < K_NBAN; i++)
      if (!K.ban[i].on) {
        K.ban[i].on = 1;
        K.ban[i].x = k->x - (k_cos(k->ang >> 6) * 16 >> 6);
        K.ban[i].y = k->y - (k_sin(k->ang >> 6) * 16 >> 6);
        break;
      }
  k->item = IT_NONE;
}

static int k_near(const Kart *k, int32_t x, int32_t y, int r) {
  int dx = (k->x - x) >> 8, dy = (k->y - y) >> 8;
  return dx * dx + dy * dy < r * r;
}

/* One step for one kart. steer -256..256; brake and use are 0/1. */
static void k_drive(Kart *k, int steer, int brake, int use, int dt) {
  int top = k_top_speed(k), a = k->ang >> 6, i, turn;
  if (k->done && !k->cpu) { steer = k_cpu_steer(k); brake = 0; use = 0; }  /* the lap of honour */
  if (k->spin_ms > 0) {
    k->spin_ms -= dt;
    k->spd -= k->spd * dt / 400;
    steer = 0;
  } else {
    if (k->boost_ms > 0) k->boost_ms -= dt;
    if (brake) k->spd -= 300 * 256 * dt / 1000;
    else if (k->spd < top) k->spd += (top - k->spd) * dt / 900 + 20 * dt;
    if (k->spd > top) k->spd -= (k->spd - top) * dt / 300;
    if (k->spd < 0) k->spd = 0;
    if (use && k->item) k_use_item(k);
  }
  k->steer = steer;
  /* Turning: a full lock is a turn in about three seconds at speed, less
   * when slow, none standing still. */
  turn = steer * 340 / 256;                                   /* 1024ths a second */
  if (k->spd < 50 * 256) turn = turn * (k->spd >> 8) / 50;
  k->ang = (uint16_t)(k->ang + turn * 64 * dt / 1000);
  a = k->ang >> 6;
  {
    int disp = k->spd * dt / 1000;                          /* texels x256 this step */
    k->x += k_cos(a) * disp >> 14;
    k->y += k_sin(a) * disp >> 14;
  }
  if (k->x < 8 << 8) k->x = 8 << 8;
  if (k->y < 8 << 8) k->y = 8 << 8;
  if (k->x > (K_WORLD - 8) << 8) k->x = (K_WORLD - 8) << 8;
  if (k->y > (K_WORLD - 8) << 8) k->y = (K_WORLD - 8) << 8;
  k_progress(k);

  /* Things on the road. */
  for (i = 0; i < K_NBOX; i++)
    if (K.box[i].on && k_near(k, K.box[i].x, K.box[i].y, 9)) {
      K.box[i].on = 0;
      K.box[i].back_ms = K.t_ms + 3000;
      if (!k->item) k->item = (k_rand() % 3) ? IT_MUSHROOM : IT_BANANA;
      if (k->cpu) k->item_wait = 1000 + (int)(k_rand() % 5000);
    }
  for (i = 0; i < K_NCOIN; i++)
    if (K.coin[i].on && k_near(k, K.coin[i].x, K.coin[i].y, 7)) {
      K.coin[i].on = 0;
      if (k->coins < 10) k->coins++;
    }
  for (i = 0; i < K_NBAN; i++)
    if (K.ban[i].on && k->spin_ms <= 0 && k_near(k, K.ban[i].x, K.ban[i].y, 7)) {
      K.ban[i].on = 0;
      k->spin_ms = 900;
      k->coins = k->coins > 2 ? k->coins - 2 : 0;
    }
}

/* Karts that touch push each other apart. */
static void k_bump(void) {
  int i, j;
  for (i = 0; i < K_KARTS; i++)
    for (j = i + 1; j < K_KARTS; j++) {
      Kart *a = &K.k[i], *b = &K.k[j];
      int dx = (b->x - a->x) >> 4, dy = (b->y - a->y) >> 4;     /* texels x16 */
      int d2 = dx * dx + dy * dy, d, push;
      if (d2 >= (9 * 16) * (9 * 16) || !d2) continue;
      d = k_isqrt(d2);
      push = (9 * 16 - d) * 8;                                  /* x256 / 2, in x16 units */
      a->x -= dx * push / d; a->y -= dy * push / d;
      b->x += dx * push / d; b->y += dy * push / d;
      a->spd = a->spd * 97 / 100; b->spd = b->spd * 97 / 100;
    }
}

/* The race, advanced dt ms. The player's controls: steer -256..256, brake,
 * and a use that is an edge (1 once per press). */
static void k_step(int dt, int steer, int brake, int use) {
  int i;
  if (K.phase == PH_COUNT) {
    K.count_ms -= dt;
    if (K.count_ms <= 0) { K.phase = PH_RACE; K.count_ms = 0; }
    return;
  }
  K.t_ms += (uint32_t)dt;
  if (K.phase == PH_DONE) K.done_ms += dt;
  for (i = 0; i < K_NBOX; i++)
    if (!K.box[i].on && K.t_ms >= K.box[i].back_ms) K.box[i].on = 1;
  for (i = 0; i < K_KARTS; i++) {
    Kart *k = &K.k[i];
    if (k->cpu) {
      int use_now = 0;
      if (k->item) { k->item_wait -= dt; if (k->item_wait <= 0) use_now = 1; }
      k_drive(k, k_cpu_steer(k), 0, use_now, dt);
    } else k_drive(k, steer, brake, use, dt);
    if (!k->done && k->lap > K_LAPS) {
      int p = 1, j;
      for (j = 0; j < K_KARTS; j++) if (K.k[j].done) p++;
      k->done = 1;
      k->place = p;
      k->done_ms = K.t_ms;
      if (!k->cpu) K.phase = PH_DONE;
    }
  }
  k_bump();
  k_places();
}

/* ---- the picture ------------------------------------------------------------------ */

/* Where the camera is, and the sprites it sees, sorted far to near. */
static void k_camera(void) {
  Kart *p = &K.k[0];
  int a = p->ang >> 6, i, n = 0, j;
  K.cfx = k_cos(a); K.cfy = k_sin(a);
  K.camx = p->x - (K.cfx * K_CAMBACK >> 6);
  K.camy = p->y - (K.cfy * K_CAMBACK >> 6);
#define K_ADD(px, py, spr_, size_, kart_)                                                   \
  do {                                                                                      \
    int dx_ = ((px) - K.camx) >> 8, dy_ = ((py) - K.camy) >> 8;                             \
    int dep_ = (dx_ * K.cfx + dy_ * K.cfy) >> 14, lat_ = (dx_ * -K.cfy + dy_ * K.cfx) >> 14; \
    if (dep_ > 6 && dep_ < 900 && n < K_NSPR) {                                             \
      int sz_ = (size_) * K_FOCAL / dep_;                                                   \
      int sx_ = K_W / 2 + lat_ * K_FOCAL / dep_;                                            \
      if (sz_ > 0 && sx_ + sz_ > 0 && sx_ - sz_ < K_W) {                                    \
        KSpr *s_ = &K.spr[n++];                                                             \
        s_->depth = (int16_t)dep_; s_->sx = (int16_t)sx_;                                   \
        s_->sy = (int16_t)(K_HZ + K_CAMH * K_FOCAL / dep_);                                 \
        s_->size = (int16_t)(sz_ > 110 ? 110 : sz_);                                        \
        s_->spr = (uint8_t)(spr_); s_->mirror = 0; s_->kart = (uint8_t)(kart_);             \
      }                                                                                     \
    }                                                                                       \
  } while (0)
  for (i = 0; i < K_NTREE; i++) K_ADD(K.tree[i][0] << 8, K.tree[i][1] << 8, SPR_TREE, 30, 0);
  for (i = 0; i < K_NBOX; i++) if (K.box[i].on) K_ADD(K.box[i].x, K.box[i].y, SPR_BOX, 9, 0);
  for (i = 0; i < K_NCOIN; i++) if (K.coin[i].on) K_ADD(K.coin[i].x, K.coin[i].y, SPR_COIN, 7, 0);
  for (i = 0; i < K_NBAN; i++) if (K.ban[i].on) K_ADD(K.ban[i].x, K.ban[i].y, SPR_BANANA, 7, 0);
  for (i = 1; i < K_KARTS; i++) {
    Kart *k = &K.k[i];
    int before = n;
    K_ADD(k->x, k->y, SPR_KART_BACK, 11, i);
    if (n > before) {
      /* Which side of it faces us: its heading against the line from us. */
      int view = k_atan2((k->y - K.camy) >> 8, (k->x - K.camx) >> 8);
      int rel = (((k->ang >> 6) - view + 512) & 1023) - 512, ar = rel < 0 ? -rel : rel;
      KSpr *s = &K.spr[n - 1];
      if (k->spin_ms > 0) ar = (int)(K.t_ms / 60 % 4) * 128, rel = ar;
      s->spr = ar < 64 ? SPR_KART_BACK : ar < 192 ? SPR_KART_Q : ar < 352 ? SPR_KART_SIDE : SPR_KART_FRONT;
      s->mirror = rel < 0;
    }
  }
#undef K_ADD
  for (i = 1; i < n; i++) {                       /* far first */
    KSpr t = K.spr[i];
    for (j = i; j > 0 && K.spr[j - 1].depth < t.depth; j--) K.spr[j] = K.spr[j - 1];
    K.spr[j] = t;
  }
  K.nspr = n;
}

static void k_floor_row(uint16_t *out, int y) {
  int dy = y - K_HZ, dist16, u, v, su, sv, x;
  int rx = -K.cfy, ry = K.cfx;
  dist16 = K_CAMH * K_FOCAL * 16 / dy;
  /* The row's middle, and a pixel's step across it, in texels x65536. */
  u = (K.camx << 8) + (K.cfx * dist16 >> 2);
  v = (K.camy << 8) + (K.cfy * dist16 >> 2);
  su = (rx * dist16 / K_FOCAL) >> 2;
  sv = (ry * dist16 / K_FOCAL) >> 2;
  u -= su * (K_W / 2);
  v -= sv * (K_W / 2);
  for (x = 0; x < K_W; x++, u += su, v += sv) {
    int tx = u >> 16, ty = v >> 16;
    int t = ((unsigned)tx < K_WORLD && (unsigned)ty < K_WORLD) ? k_cell(tx >> 3, ty >> 3) : T_OUT;
    out[x] = K.tex[t][((ty & 7) << 3) | (tx & 7)];
  }
}

static void k_sky_row(uint16_t *out, int y) {
  int x, off = (K.k[0].ang >> 6) * 240 / 256;          /* a turn is four screens of sky */
  uint16_t sky = K.sky[y], hill = K_RGB(40, 120, 70), far = K_RGB(110, 150, 200);
  for (x = 0; x < K_W; x++) {
    int h = K.hills[((x + off) >> 1) & 255], hf = K.hills[((x + off / 2 + 90) >> 0) & 255] + 5;
    out[x] = (K_HZ - y <= h) ? hill : (K_HZ - y <= hf) ? far : sky;
  }
}

/* A sprite's part of one row: src from the 16x16, body colours swapped. */
static void k_sprite_row(uint16_t *out, const KSpr *s, int y) {
  int top = s->sy - s->size, row, x, x0, x1, half = s->size / 2;
  const uint8_t *src;
  uint16_t body = 0, shade = 0;
  if (y < top || y >= s->sy || s->size <= 0) return;
  row = (y - top) * 16 / s->size;
  src = K_SPR[s->spr] + row * 8;
  if (s->kart) { body = K.k[s->kart].body; shade = K.k[s->kart].shade; }
  x0 = s->sx - half; x1 = x0 + s->size;
  for (x = x0 < 0 ? 0 : x0; x < x1 && x < K_W; x++) {
    int col = (x - x0) * 16 / s->size, v;
    if (s->mirror) col = 15 - col;
    v = (col & 1) ? src[col >> 1] & 15 : src[col >> 1] >> 4;
    if (!v) continue;
    out[x] = v == 1 && s->kart ? body : v == 2 && s->kart ? shade : K.pal[v];
  }
}

/* The player's own kart, big and in front, leaning with the steering. */
static void k_player_sprite(KSpr *s) {
  Kart *p = &K.k[0];
  s->size = 11 * K_FOCAL / K_CAMBACK;
  s->sx = K_W / 2;
  s->sy = (int16_t)(K_HZ + K_CAMH * K_FOCAL / K_CAMBACK + 4);
  s->kart = 0;
  s->mirror = p->steer < 0;
  s->spr = (p->steer > 110 || p->steer < -110) ? SPR_KART_Q : SPR_KART_BACK;
  if (p->spin_ms > 0) {
    int f = (int)(K.t_ms / 60 % 4);
    s->spr = f == 0 ? SPR_KART_BACK : f == 2 ? SPR_KART_FRONT : SPR_KART_SIDE;
    s->mirror = f == 3;
  }
}

/* ---- the HUD, in the 6x8 font ---- */

static void k_glyph_row(uint16_t *out, int x, int y, int row, char c, int scale, uint16_t fg) {
  int col, sx;
  const uint8_t *g;
  if (c < FONT_FIRST || c > FONT_LAST) return;
  g = font6x8[c - FONT_FIRST];
  (void)y;
  for (col = 0; col < 6; col++)
    if (g[col] >> (row / scale) & 1)
      for (sx = 0; sx < scale; sx++) {
        int px = x + col * scale + sx;
        if ((unsigned)px < K_W) out[px] = fg;
      }
}

/* Text with a shadow, at (x, y), the part of it on row `y_row`. */
static void k_text_row(uint16_t *out, int y_row, int x, int y, const char *s, int scale, uint16_t fg) {
  int r = y_row - y, i;
  uint16_t sh = K_RGB(0, 0, 0);
  for (i = 0; s[i]; i++) {
    if (r >= 1 && r - 1 < 8 * scale) k_glyph_row(out, x + i * 6 * scale + 1, y + 1, r - 1, s[i], scale, sh);
    if (r >= 0 && r < 8 * scale) k_glyph_row(out, x + i * 6 * scale, y, r, s[i], scale, fg);
  }
}

static int k_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void k_num(char *out, int v, int w) {          /* zero-padded */
  int i;
  for (i = w - 1; i >= 0; i--) { out[i] = (char)('0' + v % 10); v /= 10; }
  out[w] = 0;
}

static void k_time(char *out, uint32_t ms) {          /* m:ss.t */
  int m = (int)(ms / 60000), s = (int)(ms / 1000 % 60), t = (int)(ms / 100 % 10);
  out[0] = (char)('0' + m % 10); out[1] = ':';
  k_num(out + 2, s, 2);
  out[4] = '.'; out[5] = (char)('0' + t); out[6] = 0;
}

static const char *k_place_word(int p) {
  static const char *W[K_KARTS + 1] = { "", "1ST", "2ND", "3RD", "4TH", "5TH", "6TH" };
  return W[p < 0 ? 0 : p > K_KARTS ? K_KARTS : p];
}

static void k_hud_row(uint16_t *out, int y) {
  Kart *p = &K.k[0];
  char b[16];
  int lap = p->lap < 1 ? 1 : p->lap > K_LAPS ? K_LAPS : p->lap;
  uint16_t white = K_RGB(255, 255, 255), gold = K_RGB(255, 214, 60);
  /* Lap and time, top left. */
  b[0] = 'L'; b[1] = 'A'; b[2] = 'P'; b[3] = ' '; b[4] = (char)('0' + lap); b[5] = '/';
  b[6] = (char)('0' + K_LAPS); b[7] = 0;
  k_text_row(out, y, 4, 3, b, 1, white);
  k_time(b, p->done ? p->done_ms : K.t_ms);
  k_text_row(out, y, 4, 13, b, 1, white);
  /* Coins, under that. */
  b[0] = '$'; b[1] = (char)('0' + p->coins / 10); b[2] = (char)('0' + p->coins % 10); b[3] = 0;
  if (p->coins < 10) { b[1] = (char)('0' + p->coins); b[2] = 0; }
  k_text_row(out, y, 4, 23, b, 1, gold);
  /* Place, top right, big. */
  k_text_row(out, y, K_W - 4 - 3 * 12, 3, k_place_word(p->place), 2, p->place == 1 ? gold : white);
  /* The item slot, top middle. */
  if (y >= 2 && y < 24) {
    int x;
    uint16_t edge = K_RGB(250, 250, 250), in = K_RGB(20, 30, 60);
    for (x = K_W / 2 - 11; x <= K_W / 2 + 10; x++)
      out[x] = (y == 2 || y == 23 || x == K_W / 2 - 11 || x == K_W / 2 + 10) ? edge : in;
    if (p->item) {
      KSpr s;
      s.sx = K_W / 2; s.sy = 22; s.size = 18; s.kart = 0; s.mirror = 0;
      s.spr = p->item == IT_MUSHROOM ? SPR_MUSHROOM : SPR_BANANA;
      k_sprite_row(out, &s, y);
    }
  }
  /* The countdown, and the end. */
  if (K.phase == PH_COUNT) {
    int n = (K.count_ms + 999) / 1000;
    b[0] = (char)('0' + n); b[1] = 0;
    k_text_row(out, y, K_W / 2 - 12, 42, b, 4, gold);
  } else if (K.t_ms < 900 && K.phase == PH_RACE) {
    k_text_row(out, y, K_W / 2 - 36, 42, "GO!", 4, K_RGB(80, 230, 90));
  } else if (K.phase == PH_DONE) {
    k_text_row(out, y, K_W / 2 - k_strlen("FINISH") * 9, 50, "FINISH", 3, gold);
  }
}

/* Rows y0..y0+h of the picture into buf (K_W x h). */
static void k_render(uint16_t *buf, int y0, int h) {
  int y, i;
  KSpr me;
  k_player_sprite(&me);
  for (y = y0; y < y0 + h; y++) {
    uint16_t *row = buf + (y - y0) * K_W;
    if (y < K_HZ) k_sky_row(row, y);
    else if (y == K_HZ) { for (i = 0; i < K_W; i++) row[i] = K_RGB(40, 120, 70); }
    else k_floor_row(row, y);
    for (i = 0; i < K.nspr; i++) k_sprite_row(row, &K.spr[i], y);
    k_sprite_row(row, &me, y);
    k_hud_row(row, y);
  }
}

#endif /* KART_H */
