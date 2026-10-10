/* The game in Iron (apps/iron.c): a garment on a board of cells, each with a
 * wrinkle and a scorch, and an iron that presses a 5x3 patch of them flat.
 * Portable and API-free, so the host suite plays it (test/test_iron_game.c);
 * the app owns the screens, the drawing and the feel.
 *
 * TIME. iron_advance(g, ms) moves everything on, in 10 ms steps. The keys
 * only open windows (the keyboard has no key-up and only the newest key
 * repeats): Space keeps the iron down until press_until, an arrow keeps it
 * gliding until move_until. A first arrow press steps one cell at once and
 * glides only if the key is still held when its repeats begin, so a tap is
 * a cell and a hold is a stroke. An arrow pressed while the iron is down
 * keeps it down too: Space then an arrow irons a stroke.
 *
 * THE CLOTH. Each cell's wrinkle is 0 (flat) .. IR_WR_MAX. A garment is laid
 * out with a few creases (ridges along random lines) over a low rumple. A
 * cell under the sole loses wrinkle at a rate set by the heat (too cool barely
 * works), the steam (nearly twice as fast) and where it is under the sole.
 * Below IR_FLAT a cell counts as flat; the moment it gets there it sparks,
 * earns points at the combo's multiplier and fills the combo meter.
 *
 * HEAT. The dial (0 off, 1..IR_DIAL_MAX) sets a target the iron warms and
 * cools towards. Each fabric has a window, need .. burn: below it the iron is
 * too cool, above it too hot. Pressing builds `danger` -- fast when too hot,
 * slowly when held still at the right heat -- and at 255 the cloth scorches:
 * a mark, a penalty, the combo gone. A cell scorched through ends the run.
 *
 * STEAM is a tank that empties while the iron presses hot enough to boil,
 * and fills again while it is up.
 *
 * A GARMENT ends when it is all flat, when Enter is pressed past the pass
 * mark, or when the clock runs out -- done if past the pass mark, the run
 * over if not. Each level is the next of towel, shirt and sheet, with less
 * time, a higher pass mark and a narrower heat window. */
#ifndef IRON_GAME_H
#define IRON_GAME_H

#include <stdint.h>

#define IR_FN static inline

#define IR_W        32                  /* the board, in cells */
#define IR_H        24
#define IR_CELLS    (IR_W * IR_H)
#define IR_FX       2                   /* the sole: 2*IR_FX+1 by 2*IR_FY+1 cells */
#define IR_FY       1
#define IR_PM       1000                /* per mille */
#define IR_WR_MAX   1000
#define IR_FLAT     60                  /* a cell below this is flat */
#define IR_HEAT_MAX 1000
#define IR_DIAL_MAX 5
#define IR_STEAM_MAX 1000
#define IR_METER_MAX 1000
#define IR_SPARKS   12                  /* cells that went flat, per advance */
#define IR_PEN_MARK 25                  /* points a scorch costs */
#define IR_LOW_TIME_MS 10000
#define IR_STEP_MS  10u

enum { IR_TOWEL = 0, IR_SHIRT, IR_SHEET, IR_KINDS };
enum { IR_UP = 0, IR_DOWN, IR_LEFT, IR_RIGHT };
enum { IR_READY = 0, IR_PLAY, IR_DONE, IR_OVER };
enum { IR_Z_COOL = 0, IR_Z_GOOD, IR_Z_HOT };
enum { IR_WHY_NONE = 0, IR_WHY_TIME, IR_WHY_BURNT, IR_WHY_GIVEUP };
enum { IR_R_PRESSED = 0, IR_R_SMOOTH, IR_R_CRISP };

/* What one advance did, for the feel. */
#define IR_EV_PRESS      0x01u          /* the iron came down */
#define IR_EV_STROKE     0x02u          /* a clean stroke ended: many cells, no scorch */
#define IR_EV_TIER_UP    0x04u
#define IR_EV_SCORCH     0x08u
#define IR_EV_COMBO_LOST 0x10u
#define IR_EV_LOW_TIME   0x20u

typedef struct { uint16_t wr; uint8_t scorch, cloth; } IronCell;

typedef struct {
  int kind;
  int bx, by, w, h;                     /* the garment's box on the board */
  int need, burn;                       /* the heat window */
  int pass_pm;
  uint32_t time_ms;
} IronLevel;

typedef struct {
  int rating;
  int flat_pm;
  int32_t earned;                       /* pressing and strokes */
  int32_t flat_bonus, time_bonus;
  int32_t total;                        /* earned - scorch + bonuses: what the score gained */
} IronResult;

typedef struct {
  IronCell  cell[IR_CELLS];
  IronLevel lv;
  IronResult res;
  int       phase, why, level;
  int32_t   score;                      /* banked: garments finished */
  int32_t   earned;                     /* this garment, so far */
  int       marks;                      /* scorches on this garment */
  uint32_t  rng, t, tick;               /* t: the game's clock, ms */
  uint32_t  acc;                        /* ms not yet stepped */
  uint32_t  left_ms;                    /* time left on the garment */
  int       x, y;                       /* the sole's middle, 1/256 cell */
  int       face;                       /* IR_LEFT or IR_RIGHT */
  int       dial, heat, steam, danger, burn;
  int       pressing, steaming, meter, tier;
  int       flat_pm, ncloth, nflat;
  uint32_t  sumw;
  uint32_t  press_until, move_until, glide_from, last_flat;
  int       move_dir;
  int       stroke_n, stroke_burnt;
  int       low_said;
  uint32_t  ev;
  int       nspark;
  int16_t   spark[IR_SPARKS];
} Iron;

/* ---- small things ---------------------------------------------------------- */

IR_FN int ir_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
IR_FN int ir_abs(int v) { return v < 0 ? -v : v; }

IR_FN uint32_t ir_rnd(Iron *g) {
  uint32_t x = g->rng ? g->rng : 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  g->rng = x;
  return x;
}

IR_FN int ir_rand(Iron *g, int n) { return n > 1 ? (int)(ir_rnd(g) % (uint32_t)n) : 0; }

IR_FN const char *iron_kind_name(int kind) {
  return kind == IR_TOWEL ? "tea towel" : kind == IR_SHIRT ? "shirt" : "bedsheet";
}

IR_FN const char *iron_fabric_name(int kind) {
  return kind == IR_TOWEL ? "linen" : kind == IR_SHIRT ? "silk" : "cotton";
}

IR_FN const char *iron_rating_name(int r) {
  return r == IR_R_CRISP ? "Crisp!" : r == IR_R_SMOOTH ? "Smooth" : "Pressed";
}

/* The heat each dial setting warms to. */
IR_FN int ir_dial_heat(int d) { return d <= 0 ? 0 : d * 190; }

IR_FN int iron_cloth(const Iron *g, int x, int y) {
  if (x < 0 || y < 0 || x >= IR_W || y >= IR_H) return 0;
  return g->cell[y * IR_W + x].cloth;
}

IR_FN int iron_ix(const Iron *g) { return g->x >> 8; }
IR_FN int iron_iy(const Iron *g) { return g->y >> 8; }

IR_FN int iron_zone(const Iron *g) {
  return g->heat < g->lv.need ? IR_Z_COOL : g->heat > g->lv.burn ? IR_Z_HOT : IR_Z_GOOD;
}

/* The setting whose heat is nearest the middle of the window. */
IR_FN int iron_ideal_dial(const Iron *g) {
  int d, best = 1, mid = (g->lv.need + g->lv.burn) / 2;
  for (d = 2; d <= IR_DIAL_MAX; d++)
    if (ir_abs(ir_dial_heat(d) - mid) < ir_abs(ir_dial_heat(best) - mid)) best = d;
  return best;
}

IR_FN int iron_flat_pct(const Iron *g) { return g->flat_pm / 10; }
IR_FN int iron_pass_pct(const Iron *g) { return g->lv.pass_pm / 10; }
IR_FN int iron_pass_ok(const Iron *g) { return g->flat_pm >= g->lv.pass_pm; }
IR_FN int iron_secs_left(const Iron *g) { return (int)((g->left_ms + 999u) / 1000u); }

/* The combo: tier 0..4 from the meter, x1.00 to x2.00. */
IR_FN int ir_tier_of(int meter) {
  return meter >= 900 ? 4 : meter >= 650 ? 3 : meter >= 400 ? 2 : meter >= 180 ? 1 : 0;
}
IR_FN int iron_tier(const Iron *g) { return g->tier; }
IR_FN int iron_mult_pct(const Iron *g) { return 100 + 25 * g->tier; }

/* The run's score as it stands: what is banked, and on a garment still being
 * ironed what it has earned less its scorches. */
IR_FN uint32_t iron_score(const Iron *g) {
  int32_t s = g->score;
  if (g->phase == IR_PLAY) s += g->earned - g->marks * IR_PEN_MARK;
  return s > 0 ? (uint32_t)s : 0u;
}

IR_FN void iron_dial_set(Iron *g, int d) { g->dial = ir_clamp(d, 0, IR_DIAL_MAX); }
IR_FN void iron_dial_adjust(Iron *g, int by) { iron_dial_set(g, g->dial + by); }

/* ---- laying out a garment ------------------------------------------------------ */

IR_FN void ir_cloth_rect(Iron *g, int x0, int y0, int w, int h) {
  int x, y;
  for (y = y0; y < y0 + h; y++)
    for (x = x0; x < x0 + w; x++)
      if (x >= 0 && y >= 0 && x < IR_W && y < IR_H) g->cell[y * IR_W + x].cloth = 1;
}

/* The shape, in the box lv.bx, by, w, h. A shirt is a body, two sleeves out
 * from the shoulders and a notch for the collar. */
IR_FN void ir_shape(Iron *g) {
  IronLevel *l = &g->lv;
  int i;
  for (i = 0; i < IR_CELLS; i++) g->cell[i].cloth = 0;
  if (l->kind == IR_SHIRT) {
    int bw = 16, sl = (l->w - bw) / 2;
    ir_cloth_rect(g, l->bx + sl, l->by, bw, l->h);               /* the body */
    ir_cloth_rect(g, l->bx, l->by + 1, sl, 7);                   /* the sleeves */
    ir_cloth_rect(g, l->bx + sl + bw, l->by + 1, sl, 7);
    ir_cloth_rect(g, l->bx + 1, l->by, sl - 1, 1);
    ir_cloth_rect(g, l->bx + sl + bw, l->by, sl - 1, 1);
    for (i = 0; i < 4; i++) {                                    /* the collar's notch */
      int cx = l->bx + l->w / 2 - 2 + i;
      g->cell[l->by * IR_W + cx].cloth = 0;
      if (i == 1 || i == 2) g->cell[(l->by + 1) * IR_W + cx].cloth = 0;
    }
  } else {
    ir_cloth_rect(g, l->bx, l->by, l->w, l->h);
    if (l->kind == IR_TOWEL) {                                   /* a hanging loop */
      g->cell[(l->by - 1) * IR_W + l->bx + l->w / 2].cloth = l->by > 0;
    }
  }
}

/* Creases: ridges along lines through the garment, the deeper in the middle
 * of a ridge, over a rumple that is mostly already flat. */
/* The rumple: a smooth field, random heights on a lattice every 4 cells
 * blended between, so it is soft hills and dips and not a speckle. */
#define IR_LAT_W (IR_W / 4 + 2)
#define IR_LAT_H (IR_H / 4 + 2)

IR_FN int ir_rumple(const uint8_t *lat, int x, int y) {
  int lx = x / 4, ly = y / 4, fx = x % 4, fy = y % 4;
  int a = lat[ly * IR_LAT_W + lx], b = lat[ly * IR_LAT_W + lx + 1];
  int c = lat[(ly + 1) * IR_LAT_W + lx], d = lat[(ly + 1) * IR_LAT_W + lx + 1];
  int top = a * (4 - fx) + b * fx, bot = c * (4 - fx) + d * fx;
  return (top * (4 - fy) + bot * fy) / 16;
}

IR_FN void ir_wrinkle(Iron *g, int ncrease) {
  int i, k, x, y;
  struct { int px, py, dx, dy, amp, wid; } cr[10];
  uint8_t lat[IR_LAT_W * IR_LAT_H];
  if (ncrease > 10) ncrease = 10;
  for (i = 0; i < IR_LAT_W * IR_LAT_H; i++) lat[i] = (uint8_t)ir_rand(g, 256);
  for (k = 0; k < ncrease; k++) {
    int a = ir_rand(g, 8);
    /* eight directions, a unit vector times 16 */
    static const int8_t DX[8] = { 16, 15, 11, 6, 0, -6, -11, -15 };
    static const int8_t DY[8] = { 0, 6, 11, 15, 16, 15, 11, 6 };
    cr[k].px = g->lv.bx * 16 + ir_rand(g, g->lv.w * 16);
    cr[k].py = g->lv.by * 16 + ir_rand(g, g->lv.h * 16);
    cr[k].dx = DX[a];
    cr[k].dy = DY[a];
    cr[k].amp = 650 + ir_rand(g, 350);
    cr[k].wid = 18 + ir_rand(g, 18);                             /* 1/16 cell, half-width */
  }
  g->ncloth = 0;
  for (y = 0; y < IR_H; y++) {
    for (x = 0; x < IR_W; x++) {
      IronCell *c = &g->cell[y * IR_W + x];
      int v = 0;
      c->scorch = 0;
      if (!c->cloth) { c->wr = 0; continue; }
      g->ncloth++;
      /* the rumple: soft, and flat where it dips */
      v = ir_rumple(lat, x, y) * 2 - 130 + ir_rand(g, 24);
      if (v < 0) v = 0;
      for (k = 0; k < ncrease; k++) {
        /* distance from the line, in 1/16 cell: the cross product with
         * the direction (whose length is 16) */
        int rx = x * 16 + 8 - cr[k].px, ry = y * 16 + 8 - cr[k].py;
        int d = ir_abs(rx * cr[k].dy - ry * cr[k].dx) / 16;
        if (d < cr[k].wid) {
          int q = (cr[k].wid - d) * 32 / cr[k].wid;             /* 0..32 from the edge in */
          int r = cr[k].amp * (64 - (32 - q) * (32 - q) / 16) / 64;  /* rounded, not a tent */
          if (r > v) v = r;
        }
      }
      c->wr = (uint16_t)ir_clamp(v, 0, IR_WR_MAX);
    }
  }
}

IR_FN void ir_count(Iron *g) {
  int i, flat = 0;
  uint32_t s = 0;
  for (i = 0; i < IR_CELLS; i++) {
    if (!g->cell[i].cloth) continue;
    s += g->cell[i].wr;
    if (g->cell[i].wr < IR_FLAT) flat++;
  }
  g->sumw = s;
  g->nflat = flat;
  g->flat_pm = g->ncloth ? flat * IR_PM / g->ncloth : IR_PM;
}

/* The level's garment, its window, its time and its pass mark, in READY. */
IR_FN void ir_lay(Iron *g) {
  IronLevel *l = &g->lv;
  int cycle = (g->level - 1) / IR_KINDS, ideal, half, n;
  static const int16_t NEED[IR_KINDS] = { 640, 260, 470 };
  static const int16_t BURN[IR_KINDS] = { 880, 470, 680 };
  static const uint16_t SECS[IR_KINDS] = { 60, 80, 100 };
  l->kind = (g->level - 1) % IR_KINDS;
  if (l->kind == IR_TOWEL) { l->w = 22; l->h = 14; }
  else if (l->kind == IR_SHIRT) { l->w = 28; l->h = 20; }
  else { l->w = 30; l->h = 22; }
  l->bx = (IR_W - l->w) / 2;
  l->by = (IR_H - l->h) / 2;
  l->need = NEED[l->kind];
  l->burn = BURN[l->kind];
  /* the window narrows round its middle, a little each time round */
  ideal = (l->need + l->burn) / 2;
  half = (l->burn - l->need) / 2 - 18 * cycle;
  if (half < 70) half = 70;
  l->need = ideal - half;
  l->burn = ideal + half;
  l->pass_pm = ir_clamp(780 + 30 * (g->level - 1), 780, 950);
  l->time_ms = (uint32_t)SECS[l->kind] * 1000u;
  l->time_ms -= l->time_ms / 12u * (uint32_t)(cycle > 4 ? 4 : cycle);
  ir_shape(g);
  n = 3 + l->kind + cycle;
  ir_wrinkle(g, n);
  ir_count(g);
  g->left_ms = l->time_ms;
  g->phase = IR_READY;
  g->why = IR_WHY_NONE;
  g->earned = 0;
  g->marks = 0;
  g->meter = 0;
  g->tier = 0;
  g->danger = 0;
  g->burn = 0;
  g->steam = IR_STEAM_MAX;
  g->pressing = g->steaming = 0;
  g->press_until = g->move_until = g->glide_from = 0;
  g->stroke_n = g->stroke_burnt = 0;
  g->low_said = 0;
  g->x = (l->bx + 2) * 256 + 128;
  g->y = (l->by + l->h / 2) * 256 + 128;
  g->face = IR_RIGHT;
  g->ev = 0;
  g->nspark = 0;
  g->res.rating = g->res.flat_pm = 0;
  g->res.earned = g->res.flat_bonus = g->res.time_bonus = g->res.total = 0;
}

IR_FN void iron_new_run(Iron *g, uint32_t seed) {
  g->rng = seed ? seed : 1u;
  g->level = 1;
  g->score = 0;
  g->t = 0;
  g->tick = 0;
  g->dial = 3;
  g->heat = 0;
  ir_lay(g);
}

IR_FN void iron_next_garment(Iron *g) {
  g->level++;
  ir_lay(g);
}

IR_FN void iron_begin(Iron *g) {
  if (g->phase == IR_READY) g->phase = IR_PLAY;
}

/* ---- the keys ------------------------------------------------------------------- */

IR_FN void iron_lift(Iron *g) {
  g->press_until = g->t;
  g->move_until = g->t;
  g->pressing = 0;
  g->steaming = 0;
}

IR_FN void ir_step_cell(Iron *g, int dir, int by) {
  if (dir == IR_LEFT) g->x -= by;
  else if (dir == IR_RIGHT) g->x += by;
  else if (dir == IR_UP) g->y -= by;
  else g->y += by;
  g->x = ir_clamp(g->x, 128, IR_W * 256 - 129);
  g->y = ir_clamp(g->y, 128, IR_H * 256 - 129);
}

IR_FN void iron_press_key(Iron *g, int repeat) {
  uint32_t until = g->t + (repeat ? 180u : 520u);              /* past the first repeat's delay */
  if ((int32_t)(until - g->press_until) > 0) g->press_until = until;
}

IR_FN void iron_move_key(Iron *g, int dir, int repeat) {
  if (dir == IR_LEFT || dir == IR_RIGHT) g->face = dir;
  if (!repeat || dir != g->move_dir) {
    ir_step_cell(g, dir, 256);                                    /* a tap is a cell */
    g->move_until = g->t + 520u;                              /* held, the repeats come at 400 */
    g->glide_from = g->move_until;                               /* and only they glide */
  } else {
    g->glide_from = g->t;
    g->move_until = g->t + 180u;
  }
  g->move_dir = dir;
  /* an arrow while the iron is down keeps it down: a stroke */
  if ((int32_t)(g->press_until - g->t) > 0 && (int32_t)(g->move_until - g->press_until) > 0)
    g->press_until = g->move_until;
}

/* ---- ending a garment ------------------------------------------------------------ */

IR_FN void ir_finish(Iron *g, int done) {
  IronResult *r = &g->res;
  r->flat_pm = g->flat_pm;
  r->earned = g->earned;
  r->flat_bonus = r->time_bonus = 0;
  if (done) {
    int over = g->flat_pm - g->lv.pass_pm;
    r->flat_bonus = (over > 0 ? over * 2 : 0) + (g->flat_pm >= IR_PM ? 100 : 0);
    r->time_bonus = (int32_t)(g->left_ms / 1000u) * 5;
    r->rating = g->flat_pm >= 980 && g->marks == 0 ? IR_R_CRISP
              : g->flat_pm >= 900 ? IR_R_SMOOTH : IR_R_PRESSED;
  } else {
    r->rating = IR_R_PRESSED;
  }
  r->total = r->earned - g->marks * IR_PEN_MARK + r->flat_bonus + r->time_bonus;
  g->score += r->total;
  if (g->score < 0) g->score = 0;
  g->earned = 0;
  g->pressing = g->steaming = 0;
  g->phase = done ? IR_DONE : IR_OVER;
}

/* Enter: done now, if past the pass mark; -1 (and nothing happens) if not. */
IR_FN int iron_finish_early(Iron *g) {
  if (g->phase != IR_PLAY) return -1;
  if (!iron_pass_ok(g)) return -1;
  ir_finish(g, 1);
  return 0;
}

IR_FN void iron_give_up(Iron *g) {
  if (g->phase != IR_PLAY && g->phase != IR_READY) return;
  g->why = IR_WHY_GIVEUP;
  ir_finish(g, 0);
}

/* ---- the clock -------------------------------------------------------------------- */

/* A cell went flat: a spark, points at the multiplier, the meter up. */
IR_FN void ir_flattened(Iron *g, int idx) {
  int t;
  if (g->nspark < IR_SPARKS) g->spark[g->nspark++] = (int16_t)idx;
  g->earned += 10 * iron_mult_pct(g) / 100;
  g->meter = ir_clamp(g->meter + 45, 0, IR_METER_MAX);
  g->last_flat = g->t;
  g->stroke_n++;
  t = ir_tier_of(g->meter);
  if (t > g->tier) g->ev |= IR_EV_TIER_UP;
  g->tier = t;
}

/* The cloth under the sole scorches: a mark under its middle row. */
IR_FN void ir_scorch(Iron *g) {
  int ix = iron_ix(g), iy = iron_iy(g), dx;
  g->marks++;
  g->ev |= IR_EV_SCORCH;
  if (g->meter > 0 || g->tier > 0) g->ev |= IR_EV_COMBO_LOST;
  g->meter = 0;
  g->tier = 0;
  g->stroke_burnt = 1;
  g->burn = 1;
  g->danger = 110;
  for (dx = -IR_FX; dx <= IR_FX; dx++) {
    int x = ix + dx, s;
    IronCell *c;
    if (!iron_cloth(g, x, iy)) continue;
    c = &g->cell[iy * IR_W + x];
    s = c->scorch + (dx == 0 ? 80 : ir_abs(dx) == 1 ? 60 : 35);
    if (s >= 255) { s = 255; g->why = IR_WHY_BURNT; }
    c->scorch = (uint8_t)s;
  }
}

/* One 10 ms step of a garment being ironed. */
IR_FN void ir_step(Iron *g) {
  int was = g->pressing, moved = 0, zone, dx, dy, rate;
  uint32_t t = g->t;

  /* where the iron goes */
  if ((int32_t)(g->move_until - t) > 0 && (int32_t)(t - g->glide_from) >= 0) {
    int sp = g->pressing ? 10 : 15;                    /* cells a second */
    ir_step_cell(g, g->move_dir, sp * 256 * (int)IR_STEP_MS / 1000);
    moved = 1;
  }
  g->pressing = (int32_t)(g->press_until - t) > 0;
  if (g->pressing && !was) {
    g->ev |= IR_EV_PRESS;
    g->stroke_n = 0;
    g->stroke_burnt = 0;
  }
  if (!g->pressing && was) {
    if (g->stroke_n >= 8 && !g->stroke_burnt) {
      g->ev |= IR_EV_STROKE;
      g->earned += g->stroke_n * 2;
    }
    g->stroke_n = 0;
  }

  zone = iron_zone(g);
  g->steaming = g->pressing && g->steam > 0 && g->heat >= 300;
  if (g->steaming) g->steam = ir_clamp(g->steam - 2, 0, IR_STEAM_MAX);
  else if (!g->pressing) g->steam = ir_clamp(g->steam + 5, 0, IR_STEAM_MAX);

  /* danger: the scorch building */
  if (g->pressing && zone == IR_Z_HOT) g->danger += moved ? 4 : 7;
  else if (g->pressing && zone == IR_Z_GOOD && !moved) g->danger += 1;
  else g->danger -= g->pressing ? 2 : 4;
  g->danger = ir_clamp(g->danger, 0, 255);
  g->burn = g->pressing && g->danger >= 200;
  if (g->danger >= 255) ir_scorch(g);

  if (!g->pressing) return;
  /* how fast the cloth flattens: wrinkle a second at the middle of the sole */
  if (g->heat < 150) rate = 0;
  else if (zone == IR_Z_COOL) rate = 1400 * (g->heat - 150) / (g->lv.need - 150 + 1) / 3;
  else rate = zone == IR_Z_HOT ? 1700 : 1400;
  if (g->steaming) rate = rate * 9 / 5;
  if (!rate) return;
  for (dy = -IR_FY; dy <= IR_FY; dy++) {
    for (dx = -IR_FX; dx <= IR_FX; dx++) {
      int x = iron_ix(g) + dx, y = iron_iy(g) + dy, w, by;
      IronCell *c;
      if (!iron_cloth(g, x, y)) continue;
      c = &g->cell[y * IR_W + x];
      if (c->wr == 0) continue;
      by = rate * IR_STEP_MS / 1000;
      if (dy) by = by * 3 / 4;                         /* the edges of the sole press less */
      if (ir_abs(dx) == IR_FX) by = by * 3 / 4;
      if (by < 1) by = 1;
      w = c->wr;
      c->wr = (uint16_t)(w > by ? w - by : 0);
      g->sumw -= (uint32_t)(w - c->wr);
      if (w >= IR_FLAT && c->wr < IR_FLAT) {
        ir_flattened(g, y * IR_W + x);
        g->nflat++;
        g->flat_pm = g->nflat * IR_PM / g->ncloth;
      }
    }
  }
}

/* Everything on by `ms`: the heat towards the dial (on the level card too),
 * and on a garment the iron, the cloth, the combo and the clock. g->ev and
 * the sparks say what happened in this call and no more. */
IR_FN void iron_advance(Iron *g, uint32_t ms) {
  uint32_t n;
  g->ev = 0;
  g->nspark = 0;
  if (ms > 250u) ms = 250u;                            /* a stall is not a long press */
  g->acc += ms;
  for (n = g->acc / IR_STEP_MS, g->acc %= IR_STEP_MS; n > 0; n--) {
    int target = ir_dial_heat(g->dial);
    g->t += IR_STEP_MS;
    g->tick++;
    if (g->heat < target) g->heat = g->heat + 3 > target ? target : g->heat + 3;
    else if (g->heat > target) g->heat = g->heat - 2 < target ? target : g->heat - 2;
    if (g->phase != IR_PLAY) continue;
    ir_step(g);
    /* the combo cools off when nothing has gone flat for a moment */
    if (g->t - g->last_flat > 900u && g->meter > 0) {
      g->meter -= 2;
      if (g->meter < 0) g->meter = 0;
      g->tier = ir_tier_of(g->meter);
    }
    if (g->why == IR_WHY_BURNT) { ir_count(g); ir_finish(g, 0); return; }
    g->left_ms = g->left_ms > IR_STEP_MS ? g->left_ms - IR_STEP_MS : 0;
    if (!g->low_said && g->left_ms <= (uint32_t)IR_LOW_TIME_MS) { g->low_said = 1; g->ev |= IR_EV_LOW_TIME; }
    if (g->flat_pm >= IR_PM) { ir_count(g); ir_finish(g, 1); return; }
    if (g->left_ms == 0) {
      ir_count(g);
      if (iron_pass_ok(g)) ir_finish(g, 1);
      else { g->why = IR_WHY_TIME; ir_finish(g, 0); }
      return;
    }
  }
}

#endif
