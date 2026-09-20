/* Pinball -- a small table, in the spirit of the one that shipped with
 * Windows.
 *
 * Three things shape this and are worth knowing before reading the rest.
 *
 * No floating point. An app links against nothing -- no libc, no libgcc -- so
 * a float divide would become a call to __divsf3 that has nowhere to go. All
 * of the physics is integer, in sixteenths of a pixel (Q4). Positions reach
 * 240*16 = 3840, so a squared distance fits in an int32 with room to spare,
 * which is the reason for that scale rather than a finer one.
 *
 * No key-up. The keyboard is a scanned matrix and reports presses, not
 * releases, so a flipper cannot be *held*. Pressing one flicks it: it snaps up
 * and falls back on a timer. That is not a compromise the player notices --
 * a real flipper is mostly used as a flick anyway -- but it is why there is a
 * `hold_until` rather than a `down` flag.
 *
 * Damage, not frames. The table is redrawn only where something moved: the
 * ball's old square, the flippers when they are swinging, a bumper when it
 * lights. Repainting all 240x135 at 60 Hz would be 65 KB down the SPI bus
 * every frame and would flicker, because there is no back buffer to hide the
 * moment when the background has been drawn and the ball has not.
 */

#include "kernel/app/capp.h"

/* ---- the table, in pixels ------------------------------------------------ */

#define SCREEN_W   240
#define SCREEN_H   135
#define BAR_H       11            /* score strip along the top */
#define FIELD_TOP   (BAR_H + 4)

#define WALL_L       4
#define WALL_R     214            /* the field's right edge; the lane is past it */
/* The lane has to be wider than twice the ball's collision reach or the ball
 * scrapes both walls all the way up. At 12 pixels it was in contact with each
 * of them, and the endpoint normals where the walls stop pointed diagonally --
 * so every launch had its climb eaten and fell back short of the deflector.
 * Eighteen leaves nine pixels of clearance either side of a ball that reaches
 * four. */
#define LANE_L     220
#define LANE_R     238
#define FLOOR      135            /* below this the ball is gone */

#define BALL_R       3
/* How close the ball's centre gets to a line before it counts as touching.
 * The walls are drawn three pixels thick, so a pixel past the radius puts the
 * contact where the paint is. */
#define REACH      PX(BALL_R + 1)
#define FLIP_LEN    26
#define N_BUMPERS    3
#define N_TARGETS    3
#define N_WALLS     14

#define SUB         16            /* sixteenths of a pixel */
#define PX(v)       ((v) * SUB)
#define TO_PX(v)    ((v) / SUB)

/* Per 8 ms step, which is what makes these numbers small.
 *
 * The first set was three times too fast: gravity of 3 sixteenths per step is
 * about 2900 pixels per second squared, and a cap of nine pixels a step sent
 * the ball across the whole 240-pixel table in a fifth of a second. It was
 * unwatchable, let alone playable -- the ball simply vanished downwards and
 * the game started again. A quarter of that gravity and a cap of five gives
 * roughly a second to cross the table, which is about what a real one looks
 * like at this scale. */
#define GRAVITY      1
#define VMAX       PX(5)
#define BOUNCE_WALL  9            /* out of 16: how much speed a wall returns */
#define BOUNCE_BUMP 12
#define BUMP_KICK  PX(2)
#define SLING_KICK PX(2)
#define FLIP_KICK  PX(4)
/* The physics ticks at 100 Hz rather than 125. The same per-step numbers over
 * fewer steps a second is the cleanest speed knob there is -- it slows the
 * whole table by a fifth without retuning gravity, kicks and the cap against
 * each other -- and it costs less CPU per second as well. */
#define STEP_MS     10
#define FLIP_MS    150            /* how long a flick holds the flipper up */

#define C_BAR      CAPP_RGB(0, 0, 0)
#define C_BAR_FG   CAPP_RGB(255, 176, 32)
#define C_FIELD    CAPP_RGB(10, 20, 56)
#define C_FIELD2   CAPP_RGB(16, 30, 78)     /* the lane, a shade lighter */
#define C_WALL     CAPP_RGB(150, 160, 180)
#define C_WALL_HI  CAPP_RGB(210, 220, 236)
#define C_BUMPER   CAPP_RGB(0, 168, 208)
#define C_BUMPER_C CAPP_RGB(200, 244, 255)
#define C_BUMPER_L CAPP_RGB(255, 240, 140)  /* lit */
#define C_TARGET   CAPP_RGB(220, 60, 60)
#define C_TARGET_D CAPP_RGB(70, 40, 60)     /* knocked down */
#define C_SLING    CAPP_RGB(226, 120, 40)
#define C_FLIP     CAPP_RGB(196, 204, 216)
#define C_FLIP_ED  CAPP_RGB(96, 104, 120)
#define C_BALL     CAPP_RGB(248, 248, 252)
#define C_BALL_SH  CAPP_RGB(120, 128, 148)

typedef struct { int16_t x0, y0, x1, y1; } Seg;
typedef struct { int16_t x, y, r; } Disc;

/* The fixed furniture. Every one of these is a line the ball bounces off;
 * open ends are deliberate, because that is how the ball gets from the launch
 * lane into the field. */
static const Seg WALLS[N_WALLS] = {
  /* The field. The ceiling stops short of the right edge: the gap between it
   * and the lane's inner wall is the mouth the ball comes out of, and falls
   * back through when it runs out of speed up there -- which is what a real
   * table's return lane does. */
  { WALL_L,  FIELD_TOP,  200,      FIELD_TOP },   /* ceiling */
  { WALL_L,  FIELD_TOP,  WALL_L,   96 },          /* left wall */
  /* Both of these start well below the deflector's exit. Their top ends are
   * corners, and a corner is a point the ball can catch on: with them at 34
   * and 48 the deflected ball arrived at the mouth, hit one and stopped dead
   * every single launch. */
  { WALL_R,  60,         WALL_R,   96 },          /* right wall, below the mouth */

  /* The launch lane, and the deflector that turns a rising ball into the
   * field. Without the diagonal the lane is a sealed tube: the ball goes up,
   * meets a flat roof and comes straight back down onto the plunger. */
  /* Forty-five degrees, near enough. A mirror at 45 turns a ball travelling
   * straight up into one travelling straight sideways; at the 35 it had
   * before, the reflection still pointed downwards and the launch came back
   * down the lane it went up. */
  { LANE_R,  50,         200,      FIELD_TOP },   /* deflector */
  { LANE_R,  50,         LANE_R,   FLOOR },       /* outer wall of the lane */
  { LANE_L,  60,         LANE_L,   FLOOR },       /* inner wall of the lane */

  { WALL_L,  96,         62,       112 },         /* left inlane */
  { WALL_R,  96,         164,      112 },         /* right inlane */
  { 62,      112,        62,       120 },
  { 164,     112,        164,      120 },

  { 58,      98,         74,       114 },         /* left slingshot face */
  { 168,     98,         152,      114 },         /* right slingshot face */
  { 0, 0, 0, 0 },
  { 0, 0, 0, 0 },
};

static const Disc BUMPERS[N_BUMPERS] = {
  { 62, 46, 9 }, { 118, 34, 9 }, { 176, 48, 9 },
};

/* Drop targets, in a row across the top left. */
static const Disc TARGETS[N_TARGETS] = {   /* x, y are the top-left; r is width */
  { 30, 26, 14 }, { 52, 26, 14 }, { 74, 26, 14 },
};
#define TARGET_H 5

#define PIV_LX 78
#define PIV_RX 150
#define PIV_Y  116

static const CardApi *api;

typedef struct {
  int  x, y, vx, vy;        /* Q4 */
  int  live;
  int  in_field;            /* has it made it out of the launch lane yet */
} Ball;

typedef struct {
  int      angle;           /* degrees from horizontal, positive = tip down */
  int      prev_angle;
  uint32_t hold_until;
} Flipper;

static struct {
  CRect    c;               /* where the shell put us, this frame */
  int      have_c;
  int      full;            /* redraw everything */
  int      expect_paint;    /* this paint is one we asked for */

  Ball     b;
  Flipper  fl, fr;
  int      prev_bx, prev_by;

  int      target_up[N_TARGETS];
  uint32_t bumper_lit[N_BUMPERS];
  int      bumper_was_lit[N_BUMPERS];

  uint32_t score, shown_score;
  int      balls, shown_balls;
  int      over;
  int      waiting;         /* on the plunger, waiting for space */
  int      plunge;          /* how far the plunger is drawn back */

  uint32_t last_ms;
  uint32_t acc_ms;
  uint32_t seed;
} G;

/* ---- small maths --------------------------------------------------------- */

/* Integer square root, by the digit-by-digit method. The starting bit must be
 * the highest power of four in an int32, not in an int16: the squared
 * distances this is asked about reach about 30 million, and a 1 << 15 start
 * silently returned nonsense above 65535 -- which is most of the table. */
static int isqrt32(int v) {
  unsigned int num, res = 0, bit = 1u << 30;
  if (v <= 0) return 0;
  num = (unsigned int)v;
  while (bit > num) bit >>= 2;
  while (bit) {
    if (num >= res + bit) { num -= res + bit; res = (res >> 1) + bit; }
    else res >>= 1;
    bit >>= 2;
  }
  return (int)res;
}

/* Sine and cosine of a whole number of degrees, in Q8. A 91-entry quarter
 * table costs 182 bytes and removes any need for a series -- and the flippers
 * only ever ask about a dozen distinct angles. */
static const int16_t SIN_Q8[91] = {
    0,   4,   9,  13,  18,  22,  27,  31,  36,  40,
   44,  49,  53,  58,  62,  66,  71,  75,  79,  83,
   88,  92,  96, 100, 104, 108, 112, 116, 120, 124,
  128, 132, 136, 140, 143, 147, 151, 154, 158, 161,
  165, 168, 171, 175, 178, 181, 184, 187, 190, 193,
  196, 199, 202, 204, 207, 210, 212, 215, 217, 219,
  222, 224, 226, 228, 230, 232, 234, 236, 237, 239,
  241, 242, 243, 245, 246, 247, 248, 249, 250, 251,
  252, 253, 254, 255, 255, 256, 256, 256, 256, 256,
  256,
};

static int sin_q8(int deg) {
  while (deg < 0) deg += 360;
  while (deg >= 360) deg -= 360;
  if (deg <= 90)  return SIN_Q8[deg];
  if (deg <= 180) return SIN_Q8[180 - deg];
  if (deg <= 270) return -SIN_Q8[deg - 180];
  return -SIN_Q8[360 - deg];
}

static int cos_q8(int deg) { return sin_q8(deg + 90); }

static int clampi(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

static uint32_t rnd(void) {
  G.seed = G.seed * 1664525u + 1013904223u;
  return G.seed >> 8;
}

/* ---- drawing ------------------------------------------------------------- */

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static CRect clip_to(CRect r, CRect c) {
  int x0 = r.x > c.x ? r.x : c.x;
  int y0 = r.y > c.y ? r.y : c.y;
  int x1 = (r.x + r.w) < (c.x + c.w) ? (r.x + r.w) : (c.x + c.w);
  int y1 = (r.y + r.h) < (c.y + c.h) ? (r.y + r.h) : (c.y + c.h);
  return rect(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
}

/* Everything draws through here: a rectangle in table coordinates, clipped to
 * the region being repaired and offset to wherever the shell put the app. */
static void put(CRect r, CRect region, uint16_t colour) {
  /* Clipped to the table as well as to the region being repaired: a repair
   * region is built around the ball, and a ball against the left wall gives
   * one that starts at -4. The shell clips too, but only to the app -- in a
   * window that is someone else's pixels. */
  CRect v = clip_to(clip_to(r, region), rect(0, 0, SCREEN_W, SCREEN_H));
  if (v.w <= 0 || v.h <= 0) return;
  api->fill(rect(G.c.x + v.x, G.c.y + v.y, v.w, v.h), colour);
}

static void disc(int cx, int cy, int r, uint16_t colour, CRect region) {
  int dy;
  for (dy = -r; dy <= r; dy++) {
    int w = isqrt32(r * r - dy * dy);
    put(rect(cx - w, cy + dy, 2 * w + 1, 1), region, colour);
  }
}

static void ring(int cx, int cy, int r, int thick, uint16_t colour, CRect region) {
  int dy;
  for (dy = -r; dy <= r; dy++) {
    int wo = isqrt32(r * r - dy * dy);
    int ri = r - thick;
    int wi = (dy > -ri && dy < ri) ? isqrt32(ri * ri - dy * dy) : -1;
    if (wi < 0) {
      put(rect(cx - wo, cy + dy, 2 * wo + 1, 1), region, colour);
    } else {
      put(rect(cx - wo, cy + dy, wo - wi, 1), region, colour);
      put(rect(cx + wi + 1, cy + dy, wo - wi, 1), region, colour);
    }
  }
}

/* A line as a run of small squares. Good enough at this size, and it keeps
 * every drawing primitive a rectangle -- which is all the API offers. */
static void thick_line(int x0, int y0, int x1, int y1, int t,
                       uint16_t colour, CRect region) {
  int dx = x1 - x0, dy = y1 - y0;
  int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
        ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
  int i;
  if (n == 0) n = 1;
  for (i = 0; i <= n; i++)
    put(rect(x0 + dx * i / n - t / 2, y0 + dy * i / n - t / 2, t, t),
        region, colour);
}

static void flipper_ends(const Flipper *f, int pivot_x, int left,
                         int *tx, int *ty) {
  int dir = left ? 1 : -1;
  *tx = pivot_x + dir * (FLIP_LEN * cos_q8(f->angle)) / 256;
  *ty = PIV_Y + (FLIP_LEN * sin_q8(f->angle)) / 256;
}

static void draw_flipper(const Flipper *f, int pivot_x, int left, CRect region) {
  int tx, ty;
  flipper_ends(f, pivot_x, left, &tx, &ty);
  thick_line(pivot_x, PIV_Y, tx, ty, 6, C_FLIP_ED, region);
  thick_line(pivot_x, PIV_Y, tx, ty, 4, C_FLIP, region);
  disc(pivot_x, PIV_Y, 3, C_FLIP_ED, region);
}

static CRect flipper_box(const Flipper *f, int pivot_x, int left) {
  int tx, ty;
  int x0, x1;
  flipper_ends(f, pivot_x, left, &tx, &ty);
  x0 = pivot_x < tx ? pivot_x : tx;
  x1 = pivot_x > tx ? pivot_x : tx;
  return rect(x0 - 5, PIV_Y - FLIP_LEN - 5, x1 - x0 + 11, FLIP_LEN * 2 + 11);
}

/* The table, minus the ball and the flippers, clipped to one region. Called
 * for the whole screen on a full repaint and for a few square pixels the rest
 * of the time -- which is what makes it cheap enough to run at 60 Hz. */
static void draw_table(CRect region) {
  int i;

  put(rect(0, BAR_H, SCREEN_W, SCREEN_H - BAR_H), region, C_FIELD);
  put(rect(LANE_L, 22, LANE_R - LANE_L, SCREEN_H - 22), region, C_FIELD2);

  for (i = 0; i < N_WALLS; i++) {
    const Seg *s = &WALLS[i];
    if (!s->x0 && !s->y0 && !s->x1 && !s->y1) continue;
    thick_line(s->x0, s->y0, s->x1, s->y1, 3, C_WALL, region);
  }
  /* The two slingshot faces get a colour of their own: they are the parts of
   * the wall that hit back, and the player should be able to see which. */
  thick_line(WALLS[11].x0, WALLS[11].y0, WALLS[11].x1, WALLS[11].y1, 3,
             C_SLING, region);
  thick_line(WALLS[12].x0, WALLS[12].y0, WALLS[12].x1, WALLS[12].y1, 3,
             C_SLING, region);

  for (i = 0; i < N_TARGETS; i++)
    put(rect(TARGETS[i].x, TARGETS[i].y, TARGETS[i].r, TARGET_H), region,
        G.target_up[i] ? C_TARGET : C_TARGET_D);

  for (i = 0; i < N_BUMPERS; i++) {
    const Disc *d = &BUMPERS[i];
    int lit = G.bumper_lit[i] != 0;
    disc(d->x, d->y, d->r, lit ? C_BUMPER_L : C_BUMPER, region);
    ring(d->x, d->y, d->r, 2, C_WALL_HI, region);
    disc(d->x, d->y, 3, lit ? C_WALL_HI : C_BUMPER_C, region);
  }

  /* The plunger, drawn as far back as it is pulled. */
  if (G.waiting) {
    int top = 128 + G.plunge / 4;
    put(rect(LANE_L + 2, top, LANE_R - LANE_L - 4, SCREEN_H - top), region,
        C_WALL);
  }
}

static void draw_ball(CRect region) {
  int x = TO_PX(G.b.x), y = TO_PX(G.b.y);
  if (!G.b.live) return;
  disc(x, y, BALL_R, C_BALL, region);
  put(rect(x, y, 2, 2), region, C_BALL_SH);
}

static void draw_bar(void) {
  char buf[40];
  api->fill(rect(G.c.x, G.c.y, SCREEN_W, BAR_H), C_BAR);
  api->fmt(buf, sizeof buf, "%06lu", (unsigned long)G.score);
  api->text((int16_t)(G.c.x + 3), (int16_t)(G.c.y + 2), buf, C_BAR_FG, C_BAR);

  if (G.over)        api->fmt(buf, sizeof buf, "GAME OVER  n");
  else if (G.waiting) api->fmt(buf, sizeof buf, "SPACE");
  else               api->fmt(buf, sizeof buf, "BALL %d", G.balls);
  api->text((int16_t)(G.c.x + SCREEN_W - 6 * (int)api->str_len(buf) - 3),
            (int16_t)(G.c.y + 2), buf, C_BAR_FG, C_BAR);

  G.shown_score = G.score;
  G.shown_balls = G.balls;
}

/* ---- physics ------------------------------------------------------------- */

static void ball_place_on_plunger(void) {
  G.b.x = PX((LANE_L + LANE_R) / 2);
  G.b.y = PX(124);
  G.b.vx = G.b.vy = 0;
  G.b.live = 1;
  G.b.in_field = 0;
  G.waiting = 1;
  G.plunge = 0;
}

static void new_game(void) {
  int i;
  G.score = 0;
  G.balls = 3;
  G.over = 0;
  for (i = 0; i < N_TARGETS; i++) G.target_up[i] = 1;
  for (i = 0; i < N_BUMPERS; i++) { G.bumper_lit[i] = 0; G.bumper_was_lit[i] = 0; }
  ball_place_on_plunger();
  G.full = 1;
}

/* Reflect the ball off a line segment, if it is touching one. Everything is
 * Q4; the segment is in whole pixels, which is why it is scaled on the way
 * in. */
static void hit_segment(const Seg *s, int bounce, int kick) {
  int ax = PX(s->x0), ay = PX(s->y0);
  int bx = PX(s->x1), by = PX(s->y1);
  int ex = bx - ax, ey = by - ay;
  int len2 = ex * ex / SUB + ey * ey / SUB;   /* keep it inside int32 */
  int px, py, dx, dy, d2, d, nx, ny, dot, pen;
  /* All int32 on purpose. The widest intermediate is a pixel offset (240) by a
   * Q4 edge length (3840) doubled and scaled by 256 -- about 471 million,
   * inside an int32 with room over. A 64-bit divide would be a call to
   * __divdi3, and an app links against no libgcc to find it in. */
  int t;

  if (len2 <= 0) return;

  /* How far along the segment the closest point lies, in Q8 -- 0 at one end,
   * 256 at the other.
   *
   * This was computed as a plain integer fraction, and the units cancel: the
   * answer is between 0 and 1, so integer division made it 0 every time and
   * every wall in the table behaved as a single point at its starting corner.
   * The ball went straight through the walls, which is what made the game
   * unplayable. Scaling by 256 before dividing is the whole fix. */
  t = (((G.b.x - ax) / SUB * ex + (G.b.y - ay) / SUB * ey) * 256) / len2;
  t = clampi(t, 0, 256);
  px = ax + ex * t / 256;
  py = ay + ey * t / 256;

  dx = G.b.x - px;
  dy = G.b.y - py;
  d2 = dx * dx + dy * dy;                    /* Q4 squared; 29M at worst */
  if (d2 > REACH * REACH) return;

  d = isqrt32(d2);
  if (d < 1) { dx = 0; dy = -SUB; d = SUB; }

  nx = dx * 256 / d;
  ny = dy * 256 / d;
  pen = REACH - d;
  if (pen <= 0) return;

  /* Out of the wall first, so the next step does not find it inside again. */
  G.b.x += nx * pen / 256;
  G.b.y += ny * pen / 256;

  dot = (G.b.vx * nx + G.b.vy * ny) / 256;
  if (dot >= 0) return;                       /* already moving away */
  G.b.vx -= (16 + bounce) * dot * nx / (16 * 256);
  G.b.vy -= (16 + bounce) * dot * ny / (16 * 256);
  if (kick) {
    G.b.vx += nx * kick / 256;
    G.b.vy += ny * kick / 256;
  }
}

static int hit_disc(const Disc *dsc, int radius, int bounce, int kick) {
  int dx = G.b.x - PX(dsc->x), dy = G.b.y - PX(dsc->y);
  int reach = PX(radius + BALL_R);
  int d2 = dx * dx + dy * dy;
  int d, nx, ny, dot, pen;

  if (d2 > reach * reach) return 0;
  d = isqrt32(d2);
  if (d < 1) { dx = 0; dy = -SUB; d = SUB; }
  nx = dx * 256 / d;
  ny = dy * 256 / d;
  pen = reach - d;
  G.b.x += nx * pen / 256;
  G.b.y += ny * pen / 256;

  dot = (G.b.vx * nx + G.b.vy * ny) / 256;
  if (dot < 0) {
    G.b.vx -= (16 + bounce) * dot * nx / (16 * 256);
    G.b.vy -= (16 + bounce) * dot * ny / (16 * 256);
  }
  G.b.vx += nx * kick / 256;
  G.b.vy += ny * kick / 256;
  return 1;
}

static void hit_flipper(Flipper *f, int pivot_x, int left) {
  Seg s;
  int tx, ty;
  int swing = f->angle - f->prev_angle;
  flipper_ends(f, pivot_x, left, &tx, &ty);
  s.x0 = (int16_t)pivot_x; s.y0 = (int16_t)PIV_Y;
  s.x1 = (int16_t)tx;      s.y1 = (int16_t)ty;
  /* A moving flipper does not merely reflect the ball, it adds to it. The
   * kick is proportional to how fast the flipper is travelling, which is what
   * makes a well-timed shot go further than a lazy one. */
  hit_segment(&s, BOUNCE_WALL, swing < 0 ? FLIP_KICK : 0);
}

/* One collision pass over the whole table, at wherever the ball is now. */
static void collide(uint32_t now) {
  int i;

  /* The one-way gate across the mouth of the launch lane.
   *
   * Real tables have a sprung flap here for the same reason: the mouth has to
   * be open on the way out and shut on the way back, or a ball that drifts
   * into the top right rolls down the lane and lands on the plunger. Nothing
   * is lost when that happens, but it looks exactly like the game resetting
   * itself, which is worse than losing something. It exists only once the ball
   * is in the field, so the launch still gets through. */
  if (G.b.in_field) {
    Seg gate;
    gate.x0 = LANE_L; gate.y0 = FIELD_TOP;
    gate.x1 = LANE_L; gate.y1 = 60;
    hit_segment(&gate, BOUNCE_WALL, 0);
  }

  /* The screen edges, so a stray ball can never leave the table. */
  if (G.b.x < PX(BALL_R)) { G.b.x = PX(BALL_R); G.b.vx = -G.b.vx / 2; }
  if (G.b.x > PX(SCREEN_W - BALL_R)) {
    G.b.x = PX(SCREEN_W - BALL_R); G.b.vx = -G.b.vx / 2;
  }
  if (G.b.y < PX(FIELD_TOP + BALL_R)) {
    G.b.y = PX(FIELD_TOP + BALL_R); G.b.vy = -G.b.vy / 2;
  }

  for (i = 0; i < N_WALLS; i++) {
    const Seg *s = &WALLS[i];
    int sling = (i == 11 || i == 12);
    if (!s->x0 && !s->y0 && !s->x1 && !s->y1) continue;
    if (sling) {
      int before_vx = G.b.vx, before_vy = G.b.vy;
      hit_segment(s, BOUNCE_WALL, SLING_KICK);
      if (before_vx != G.b.vx || before_vy != G.b.vy) G.score += 50;
    } else {
      hit_segment(s, BOUNCE_WALL, 0);
    }
  }

  for (i = 0; i < N_BUMPERS; i++) {
    if (hit_disc(&BUMPERS[i], BUMPERS[i].r, BOUNCE_BUMP, BUMP_KICK)) {
      G.bumper_lit[i] = now + 120;
      G.score += 100;
    }
  }

  for (i = 0; i < N_TARGETS; i++) {
    const Disc *t = &TARGETS[i];
    int bx = TO_PX(G.b.x), by = TO_PX(G.b.y);
    if (!G.target_up[i]) continue;
    if (bx + BALL_R < t->x || bx - BALL_R > t->x + t->r) continue;
    if (by + BALL_R < t->y || by - BALL_R > t->y + TARGET_H) continue;
    G.target_up[i] = 0;
    G.score += 300;
    G.b.vy = -G.b.vy;
    G.full = 1;
    {
      int all = 0, j;
      for (j = 0; j < N_TARGETS; j++) all += G.target_up[j];
      if (all == 0) {
        G.score += 1000;
        for (j = 0; j < N_TARGETS; j++) G.target_up[j] = 1;
      }
    }
  }

  hit_flipper(&G.fl, PIV_LX, 1);
  hit_flipper(&G.fr, PIV_RX, 0);
}

/* Move, then look for what was hit -- in slices short enough that nothing can
 * be jumped over.
 *
 * A ball at the speed cap travels nine pixels in a step, and a wall is only
 * reached from five pixels away, so a whole wall fits between one position and
 * the next: the ball arrives on the far side having felt nothing. Continuous
 * collision detection is the proper answer and is a great deal of arithmetic;
 * moving in two-pixel slices is the cheap one, and at these speeds it is at
 * most five passes over a dozen segments. */
static void step(uint32_t now) {
  int speed, slices, i;

  if (!G.b.live) return;

  if (G.waiting) {
    if (G.plunge > 0) G.plunge--;
    return;
  }

  G.b.vy += GRAVITY;
  G.b.vx = clampi(G.b.vx, -VMAX, VMAX);
  G.b.vy = clampi(G.b.vy, -VMAX, VMAX);

  speed = (G.b.vx < 0 ? -G.b.vx : G.b.vx) + (G.b.vy < 0 ? -G.b.vy : G.b.vy);
  slices = speed / PX(2) + 1;
  if (slices > 8) slices = 8;

  for (i = 0; i < slices; i++) {
    G.b.x += G.b.vx / slices;
    G.b.y += G.b.vy / slices;
    collide(now);
    if (!G.b.live) return;
  }

  /* Out of the lane and into the field: from here on, coming back down the
   * lane means the ball rolled back, rather than that it has not left yet.
   * Without this the launch cancels itself -- the ball starts in the lane, so
   * the test below is true on the first step after pressing space. */
  if (TO_PX(G.b.x) < LANE_L - 2) G.b.in_field = 1;

  /* Back down the return lane. Not a drain -- the ball is not lost, it is
   * back where it started -- so it waits on the plunger for another launch
   * rather than costing a life. */
  if (G.b.in_field && !G.waiting && TO_PX(G.b.x) > LANE_L && TO_PX(G.b.y) > 100) {
    ball_place_on_plunger();
    G.full = 1;
    return;
  }

  if (G.b.y > PX(FLOOR + BALL_R)) {          /* drained */
    G.b.live = 0;
    G.balls--;
    if (G.balls <= 0) { G.over = 1; G.balls = 0; }
    else ball_place_on_plunger();
    G.full = 1;
  }
}

static void flipper_step(Flipper *f, uint32_t now) {
  int want = (now < f->hold_until) ? -32 : 28;
  f->prev_angle = f->angle;
  if (f->angle < want) f->angle += 12;
  else if (f->angle > want) f->angle -= 12;
  if (f->angle > want - 12 && f->angle < want + 12) f->angle = want;
}

/* ---- the shell's callbacks ----------------------------------------------- */

static int app_tick(void *st, uint32_t now) {
  uint32_t dt;
  int moved = 0;
  int i;
  (void)st;

  if (!G.last_ms) G.last_ms = now;
  dt = now - G.last_ms;
  G.last_ms = now;
  if (dt > 100) dt = 100;                    /* after a pause, do not fast-forward */
  G.acc_ms += dt;

  while (G.acc_ms >= STEP_MS) {
    G.acc_ms -= STEP_MS;
    flipper_step(&G.fl, now);
    flipper_step(&G.fr, now);
    step(now);
    moved = 1;
  }

  for (i = 0; i < N_BUMPERS; i++)
    if (G.bumper_lit[i] && now >= G.bumper_lit[i]) G.bumper_lit[i] = 0;

  if (!moved) return 0;
  G.expect_paint = 1;
  return 1;
}

static void app_paint(void *st, CRect c) {
  CRect whole = rect(0, 0, SCREEN_W, SCREEN_H);
  int i;
  (void)st;

  if (c.x != G.c.x || c.y != G.c.y || !G.have_c) G.full = 1;
  G.c = c;
  G.have_c = 1;

  /* A paint we did not ask for is the shell telling us something covered us
   * -- the help overlay, or the launcher clearing the screen. Only we know
   * that the answer is to draw the whole table again. */
  if (!G.expect_paint) G.full = 1;
  G.expect_paint = 0;

  if (G.full) {
    G.full = 0;
    draw_bar();
    draw_table(whole);
    draw_flipper(&G.fl, PIV_LX, 1, whole);
    draw_flipper(&G.fr, PIV_RX, 0, whole);
    draw_ball(whole);
    G.prev_bx = TO_PX(G.b.x);
    G.prev_by = TO_PX(G.b.y);
    for (i = 0; i < N_BUMPERS; i++) G.bumper_was_lit[i] = G.bumper_lit[i] != 0;
    return;
  }

  if (G.score != G.shown_score || G.balls != G.shown_balls) draw_bar();

  /* Where the ball was, and where it is. One rectangle covering both is
   * cheaper than two when they overlap, which at these speeds they usually
   * do. */
  {
    int bx = TO_PX(G.b.x), by = TO_PX(G.b.y);
    int x0 = (bx < G.prev_bx ? bx : G.prev_bx) - BALL_R - 1;
    int y0 = (by < G.prev_by ? by : G.prev_by) - BALL_R - 1;
    int x1 = (bx > G.prev_bx ? bx : G.prev_bx) + BALL_R + 1;
    int y1 = (by > G.prev_by ? by : G.prev_by) + BALL_R + 1;
    CRect r = rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
    draw_table(r);
    draw_flipper(&G.fl, PIV_LX, 1, r);
    draw_flipper(&G.fr, PIV_RX, 0, r);
    draw_ball(r);
    G.prev_bx = bx;
    G.prev_by = by;
  }

  /* A flipper that is swinging, and a bumper that just lit or went out. */
  if (G.fl.angle != G.fl.prev_angle) {
    CRect r = flipper_box(&G.fl, PIV_LX, 1);
    draw_table(r);
    draw_flipper(&G.fl, PIV_LX, 1, r);
    draw_ball(r);
  }
  if (G.fr.angle != G.fr.prev_angle) {
    CRect r = flipper_box(&G.fr, PIV_RX, 0);
    draw_table(r);
    draw_flipper(&G.fr, PIV_RX, 0, r);
    draw_ball(r);
  }
  for (i = 0; i < N_BUMPERS; i++) {
    int lit = G.bumper_lit[i] != 0;
    if (lit == G.bumper_was_lit[i]) continue;
    G.bumper_was_lit[i] = lit;
    {
      const Disc *d = &BUMPERS[i];
      CRect r = rect(d->x - d->r - 1, d->y - d->r - 1, 2 * d->r + 3, 2 * d->r + 3);
      draw_table(r);
      draw_ball(r);
    }
  }
}

static int app_key(void *st, uint8_t k) {
  uint32_t now = api->ticks_ms();
  (void)st;

  /* A flick returns 0: the flipper only moves in the next tick, which asks
   * for its own paint. Returning 1 here made the shell paint at once, and a
   * paint we did not ask for means "redraw everything" -- the whole table,
   * on every keypress, for nothing. */
  switch (k) {
  case CAPP_KEY_LEFT: case 'a': case 'A':
    G.fl.hold_until = now + FLIP_MS;
    return 0;
  case CAPP_KEY_RIGHT: case 'l': case 'L':
    G.fr.hold_until = now + FLIP_MS;
    return 0;
  case ' ':
    if (G.over) return 0;
    if (G.waiting) {
      /* A little randomness in the launch, so the first ball of every game
       * does not trace the same path. */
      G.waiting = 0;
      G.b.vx = -(int)(rnd() % PX(1));
      G.b.vy = -PX(5) - (int)(rnd() % PX(1));
      G.plunge = 0;
      G.full = 1;
      return 1;
    }
    return 0;
  case 'n': case 'N':
    new_game();
    return 1;
  default:
    return 0;
  }
}

/* Left button, left flipper; right button, right flipper -- and either half
 * of the table works too, for a mouse with one button. Same flick as the
 * keyboard, for the same reason: there is no release to wait for. */
static int app_click(void *st, int16_t x, int16_t y, int button) {
  uint32_t now = api->ticks_ms();
  (void)st; (void)y;

  if (G.waiting && !G.over) {
    G.waiting = 0;
    G.b.vx = -(int)(rnd() % PX(1));
    G.b.vy = -PX(5) - (int)(rnd() % PX(1));
    G.full = 1;
    return 1;
  }
  if (button == CAPP_BTN_RIGHT || x > SCREEN_W / 2) G.fr.hold_until = now + FLIP_MS;
  else G.fl.hold_until = now + FLIP_MS;
  return 0;                                    /* the tick paints it, see app_key */
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  /* No auto-repeat: a flipper flicks on a press, and a held key repeating
   * sixteen times a second would make it a held flipper by accident. */
  CAPP_FULLSCREEN | CAPP_NO_REPEAT,
  "Pinball",
  /* 16x16: a ball on a table with two flippers. */
  { 0x0F, 0xF0, 0x30, 0x0C, 0x40, 0x02, 0x47, 0x02,
    0x8F, 0x81, 0x8F, 0x81, 0x87, 0x01, 0x80, 0x01,
    0x80, 0x01, 0x80, 0x01, 0x40, 0x02, 0x60, 0x06,
    0x38, 0x1C, 0x0C, 0x30, 0x07, 0xE0, 0x00, 0x00 },
  "click / left / a\tleft flipper\nright / l\tright flipper\nspace\tlaunch the ball\n"
  "n\tnew game\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  (void)argc; (void)argv;

  api->mem_set(&G, 0, sizeof G);
  G.seed = api->ticks_ms() | 1u;
  G.fl.angle = G.fl.prev_angle = 28;
  G.fr.angle = G.fr.prev_angle = 28;
  new_game();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.click = app_click;
  UI.pref_w = SCREEN_W;
  UI.pref_h = SCREEN_H;
  api->ui(&UI);
  return 0;
}
