/* Noodle -- the inflatable tube man from the car lot, and you are the fan.
 *
 * Blow into the mic (the grille on the front) and he fills with air: he
 * straightens, stands, and the air running up him throws him about -- a wave
 * climbing the tube, the arms flailing, the head snapping round. Stop and he
 * sags, folds over at the middle and slumps towards the tarmac, and the next
 * breath brings him back up. Space does a puff for anyone without a mic.
 *
 * THE MIC. It only listens: api->audio()->record(NULL, ...) runs the mic
 * without a file (firmware from 2026-09-25), and level() is the loudness of
 * the last 32 ms. Blowing across a MEMS mic is loud and low, so it reads near
 * the top of the scale, well above speech. A noise floor is tracked -- it
 * falls at once and creeps back up -- so a noisy room is not a gale. Older
 * firmware refuses a NULL path, and then it records to /cache/noodle.wav
 * instead, which works and costs the card some writes. Recordings run in
 * eight-second pieces, restarted from tick, so that leaving the app -- apps
 * are not told -- leaves the mic running for at most that long.
 *
 * THE BODY. Twelve joints stacked on the fan, and two arms of four. Each
 * joint is a damped spring pulled towards a target angle: straight up and
 * stiff when full of air, a slumped curl to one side when empty. Moving air
 * adds a travelling wave to the targets and random kicks to the speeds, and
 * that is the whole dance. It is not a simulation of anything, and it is
 * tuned to look right rather than be right.
 *
 * DRAWING. No back buffer, and clearing then drawing flickers, so the scene
 * is rendered a strip of rows at a time into a small buffer and blitted
 * (api->pixels): sky, bunting, fan, then the body as a string of discs. Only
 * the rectangle the figure moved through is redrawn each frame, marked with
 * api->damage; a still, empty tube asks for nothing at all.
 *
 * No divisions by variables and nothing from libm: an app links no libgcc,
 * and a float divide is a call into it. Sines are a polynomial.
 */

#include "kernel/app/capp.h"

#define SCREEN_W  240
#define SCREEN_H  135
#define GROUND_Y  112
#define BASE_X    120.0f
#define BASE_Y    101.0f          /* the top of the fan, where he is tied on */

#define NSEG      12
#define NARM      4
#define ARM_AT    9               /* the joint the arms leave from */

#define STEP_MS   16
#define DT        0.016f
#define BAND_H    15
#define MAX_DISC  320

#define MIC_CHUNK_MS 8000
#define MIC_FILE  CAPP_CACHE "/noodle.wav"

#define PI_F      3.14159265f

/* The scene. */
#define C_SKY_TOP  CAPP_RGB(104, 170, 236)
#define C_GROUND   CAPP_RGB(74, 76, 84)
#define C_LINE     CAPP_RGB(226, 208, 96)
#define C_CURB     CAPP_RGB(150, 152, 160)
#define C_FAN      CAPP_RGB(46, 50, 60)
#define C_FAN_HI   CAPP_RGB(92, 98, 112)
#define C_FAN_VENT CAPP_RGB(28, 30, 36)
#define C_STRING   CAPP_RGB(60, 60, 70)
#define C_OUTLINE  CAPP_RGB(28, 26, 34)
#define C_WHITE    CAPP_RGB(255, 255, 255)
#define C_PUPIL    CAPP_RGB(16, 16, 24)
#define C_MOUTH    CAPP_RGB(110, 20, 30)
#define C_HUD_BG   CAPP_RGB(74, 76, 84)
#define C_HUD_FG   CAPP_RGB(230, 232, 240)
#define C_HUD_DIM  CAPP_RGB(160, 164, 176)
#define C_METER    CAPP_RGB(120, 220, 140)

static const uint16_t PENNANT[4] = {
  CAPP_RGB(232, 64, 56), CAPP_RGB(250, 210, 60),
  CAPP_RGB(70, 140, 236), CAPP_RGB(250, 250, 250),
};

/* Main colour, a darker band, and the hair. c cycles them. */
static const uint16_t PALETTE[4][3] = {
  { CAPP_RGB(238, 54, 46),  CAPP_RGB(198, 30, 34),  CAPP_RGB(250, 210, 60) },
  { CAPP_RGB(60, 150, 250), CAPP_RGB(36, 104, 210), CAPP_RGB(250, 250, 250) },
  { CAPP_RGB(120, 220, 70), CAPP_RGB(70, 170, 50),  CAPP_RGB(250, 130, 40) },
  { CAPP_RGB(246, 120, 200),CAPP_RGB(200, 80, 170), CAPP_RGB(120, 60, 200) },
};

typedef struct { int16_t x, y; uint8_t r; uint16_t c; } Disc;
typedef struct { int16_t x0, y0, x1, y1; } Box;      /* x1, y1 exclusive */

static const CardApi *api;

static struct {
  /* the tube */
  float a[NSEG], w[NSEG];            /* relative angle and its speed */
  float arm_a[2][NARM], arm_w[2][NARM];
  float px[NSEG + 1], py[NSEG + 1];  /* joint positions, from the angles */
  float th[NSEG];                    /* absolute angle of each segment */
  float P;                           /* how full of air, 0..1 */
  float A;                           /* how much air is moving, 0..1 */
  float t;                           /* seconds, for the wave */
  float lean;                        /* which way he falls: -1 or 1 */

  /* the mic */
  int   listen;                      /* 1 record(NULL); 0 fall back to a file */
  int   level;                       /* the last reading, 0..100, or -1 */
  float floor;                       /* the room's noise */
  uint32_t mic_retry;                /* when to try the mic again */
  int   mic_state;                   /* MIC_* below */
  uint32_t puff_until;               /* space held: air until then */

  /* drawing */
  Disc  disc[MAX_DISC];
  int   ndisc;
  Box   box, was;                    /* what the figure covers, now and last */
  int   pal;
  int   hud_meter;                   /* the meter as last drawn, in pixels */
  int   hud_state;                   /* the line as last drawn */
  int   still;                       /* frames with nothing moving */
  int   asked;                       /* damage marked since the last paint */

  uint32_t last_ms, acc_ms;
  uint32_t seed;
  CRect  c;                          /* where the last paint put us */
} N;

enum { MIC_NONE = 0, MIC_ON, MIC_BUSY, MIC_REFUSED };

static uint16_t BAND[SCREEN_W * BAND_H];
static uint8_t  SPAN[14][14];        /* half-widths of a disc, by radius and row */
static uint16_t SKY[GROUND_Y];

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

/* ---- numbers ------------------------------------------------------------- */

static uint32_t rnd(void) {
  uint32_t x = N.seed;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  N.seed = x;
  return x;
}

/* -1..1 */
static float frand(void) {
  return (float)(int32_t)(rnd() & 0xFFFF) * (1.0f / 32768.0f) - 1.0f;
}

static float fabs_(float x) { return x < 0 ? -x : x; }

static float clampf(float x, float lo, float hi) {
  return x < lo ? lo : x > hi ? hi : x;
}

/* sin, by symmetry onto [-pi/2, pi/2] and a seventh-order polynomial there:
 * better than a thousandth, and no libm. */
static float fsin(float x) {
  float x2;
  while (x > PI_F) x -= 2.0f * PI_F;
  while (x < -PI_F) x += 2.0f * PI_F;
  if (x > 0.5f * PI_F) x = PI_F - x;
  else if (x < -0.5f * PI_F) x = -PI_F - x;
  x2 = x * x;
  return x * (1.0f - x2 * (1.0f / 6.0f - x2 * (1.0f / 120.0f - x2 * (1.0f / 5040.0f))));
}

static float fcos(float x) { return fsin(x + 0.5f * PI_F); }

static int iround(float x) { return x < 0 ? -(int)(0.5f - x) : (int)(x + 0.5f); }

/* ---- the tube ------------------------------------------------------------ */

static float seg_len(void) { return 3.4f + 3.5f * N.P; }
static float arm_len(void) { return 2.4f + 3.2f * N.P; }

/* Where every joint is, from the angles. Absolute angle 0 is straight up;
 * positive leans right. */
static void forward(void) {
  float th = 0, L = seg_len();
  int k;
  N.px[0] = BASE_X;
  N.py[0] = BASE_Y;
  for (k = 0; k < NSEG; k++) {
    th += N.a[k];
    N.th[k] = th;
    N.px[k + 1] = N.px[k] + L * fsin(th);
    N.py[k + 1] = N.py[k] - L * fcos(th);
  }
}

/* Heaped on the fan, before the first breath. */
static void slumped(void) {
  int k, s;
  N.lean = (rnd() & 1) ? 1.0f : -1.0f;
  for (k = 0; k < NSEG; k++) {
    N.a[k] = N.lean * (k == 0 ? 0.35f : 0.24f);
    N.w[k] = 0;
  }
  for (s = 0; s < 2; s++)
    for (k = 0; k < NARM; k++) {
      N.arm_a[s][k] = (s ? 1.0f : -1.0f) * (k == 0 ? 2.4f : 0.2f);
      N.arm_w[s][k] = 0;
    }
  N.P = 0;
  N.A = 0;
  forward();
}

/* One step of the dance. `air` is the breath this step, 0..1. */
static void step(float air) {
  float stiff, damp, wave_w, droop;
  int k, s;

  /* Air in fills him quickly; air stopping lets him down slowly, which is
   * the sag that makes the next breath worth taking. */
  N.A += (air - N.A) * (air > N.A ? 0.35f : 0.08f);
  N.P += (N.A - N.P) * (N.A > N.P ? 0.09f : 0.018f);
  N.P = clampf(N.P, 0, 1);
  N.t += DT;

  /* Which way he falls: whichever way he is already leaning, once there is
   * not enough air to argue. */
  if (N.P < 0.25f) {
    float sum = 0;
    for (k = 0; k < NSEG; k++) sum += N.a[k];
    if (sum > 0.3f) N.lean = 1.0f;
    else if (sum < -0.3f) N.lean = -1.0f;
  }

  stiff = 30.0f + 520.0f * N.P;
  damp = 3.0f + 16.0f * N.P;
  wave_w = 7.0f + 7.0f * N.A;
  /* Empty, each joint wants a curl; the sum over twelve is a fold of more
   * than a right angle, so the head ends near the ground. */
  droop = (1.0f - N.P);
  droop = droop * droop * 0.26f;

  for (k = 0; k < NSEG; k++) {
    float target = N.lean * droop * (k == 0 ? 0.6f : 1.0f);
    /* The wave that climbs a tube man: bigger towards the top, where the
     * tube is thin and the air has nowhere to go but sideways. */
    target += N.A * (0.10f + 0.030f * (float)k) * fsin(wave_w * N.t - 0.75f * (float)k);
    N.w[k] += (stiff * (target - N.a[k]) - damp * N.w[k]) * DT;
    /* Gusts: random kicks, more of them the harder the air is moving. */
    if (N.A > 0.05f && (rnd() & 7) == 0)
      N.w[k] += frand() * (2.0f + 0.9f * (float)k) * N.A;
    N.a[k] += N.w[k] * DT;
    N.a[k] = clampf(N.a[k], k == 0 ? -0.7f : -0.9f, k == 0 ? 0.7f : 0.9f);
  }

  /* The arms: out and up when full, hanging when empty, and thrown about by
   * the air more than anything else is. */
  for (s = 0; s < 2; s++) {
    float side = s ? 1.0f : -1.0f;
    float st = 18.0f + 160.0f * N.P, dm = 2.0f + 6.0f * N.P;
    for (k = 0; k < NARM; k++) {
      float target;
      if (k == 0)
        target = side * (1.05f + 1.6f * (1.0f - N.P)
                         - 0.9f * N.A * fsin(wave_w * 1.3f * N.t + side * 1.7f));
      else
        target = side * (0.15f * (1.0f - N.P))
               + 0.45f * N.A * fsin(wave_w * 1.6f * N.t - (float)k + side);
      N.arm_w[s][k] += (st * (target - N.arm_a[s][k]) - dm * N.arm_w[s][k]) * DT;
      if (N.A > 0.05f && (rnd() & 3) == 0)
        N.arm_w[s][k] += frand() * 9.0f * N.A;
      N.arm_a[s][k] += N.arm_w[s][k] * DT;
      N.arm_a[s][k] = clampf(N.arm_a[s][k], -3.0f, 3.0f);
    }
  }

  forward();

  /* The tarmac: a joint that has gone below it is pushed back, by turning
   * the segment under it towards level. */
  for (k = 1; k <= NSEG; k++) {
    float lim = (float)GROUND_Y - 6.0f;
    if (N.py[k] > lim && fabs_(N.th[k - 1]) > 1.2f) {
      float sgn = N.th[k - 1] > 0 ? 1.0f : -1.0f;
      N.a[k - 1] -= sgn * 0.012f * (N.py[k] - lim);
      N.w[k - 1] *= 0.5f;
    }
  }
  forward();
}

/* ---- the mic -------------------------------------------------------------- */

/* The breath this step, 0..1: the mic above the room's floor, or a puff. */
static float read_air(uint32_t now) {
  const CappAudio *au = api->audio ? api->audio() : 0;
  float air = 0;

  if (au) {
    if (au->state() == CAPP_AUDIO_IDLE) {
      if ((int32_t)(now - N.mic_retry) >= 0) {
        int r = au->record(N.listen ? 0 : MIC_FILE, MIC_CHUNK_MS);
        if (r == -2 && N.listen) {
          N.listen = 0;                        /* older firmware: a file */
          r = au->record(MIC_FILE, MIC_CHUNK_MS);
        }
        if (r == 0) N.mic_state = MIC_ON;
        else {
          N.mic_state = (r == -1) ? MIC_BUSY : MIC_REFUSED;
          N.mic_retry = now + 1000;
        }
      }
    } else if (au->state() != CAPP_AUDIO_RECORDING) {
      N.mic_state = MIC_BUSY;                  /* something is playing */
    }
    N.level = au->level();
  } else {
    N.mic_state = MIC_NONE;
    N.level = -1;
  }

  if (N.level >= 0) {
    float l = (float)N.level;
    /* Down at once, up slowly: the floor is the quietest the room has been
     * lately, not the average, or a long breath would become the floor. */
    if (l < N.floor) N.floor = l;
    else N.floor += 0.004f;
    air = (l - N.floor - 5.0f) * (1.0f / 45.0f);
    air = clampf(air, 0, 1);
  }
  if ((int32_t)(N.puff_until - now) > 0) air = 1.0f;
  return air;
}

/* ---- what to draw ----------------------------------------------------------- */

static void disc(float x, float y, float r, uint16_t c) {
  Disc *d;
  int ri = iround(r);
  if (N.ndisc >= MAX_DISC) return;
  if (ri < 1) ri = 1;
  if (ri > 13) ri = 13;
  d = &N.disc[N.ndisc++];
  d->x = (int16_t)iround(x);
  d->y = (int16_t)iround(y);
  d->r = (uint8_t)ri;
  d->c = c;
  if (d->x - ri < N.box.x0) N.box.x0 = (int16_t)(d->x - ri);
  if (d->y - ri < N.box.y0) N.box.y0 = (int16_t)(d->y - ri);
  if (d->x + ri + 1 > N.box.x1) N.box.x1 = (int16_t)(d->x + ri + 1);
  if (d->y + ri + 1 > N.box.y1) N.box.y1 = (int16_t)(d->y + ri + 1);
}

/* A segment as discs, four to it: the widest gap is under two pixels. */
static void tube(float x0, float y0, float x1, float y1, float r, uint16_t c) {
  int i;
  for (i = 0; i <= 4; i++) {
    float f = (float)i * 0.25f;
    disc(x0 + (x1 - x0) * f, y0 + (y1 - y0) * f, r, c);
  }
}

static float body_r(int k) {
  return (7.2f - 2.2f * (float)k * (1.0f / NSEG)) * (0.82f + 0.18f * N.P);
}

/* The arms, both passes: outline, then colour. */
static void arms(int outline) {
  const uint16_t *p = PALETTE[N.pal];
  float L = arm_len(), r = 2.4f + 0.5f * N.P;
  int s, k;
  for (s = 0; s < 2; s++) {
    float x = N.px[ARM_AT], y = N.py[ARM_AT], th = N.th[ARM_AT];
    for (k = 0; k < NARM; k++) {
      float nx, ny;
      th += N.arm_a[s][k];
      nx = x + L * fsin(th);
      ny = y - L * fcos(th);
      if (ny > (float)GROUND_Y - 2.0f) ny = (float)GROUND_Y - 2.0f;
      tube(x, y, nx, ny, outline ? r + 1.0f : r, outline ? C_OUTLINE : p[1]);
      x = nx;
      y = ny;
    }
    /* A hand, of a sort: the end of a tube, a little fatter. */
    disc(x, y, outline ? r + 2.0f : r + 1.0f, outline ? C_OUTLINE : p[0]);
  }
}

/* Everything the figure is, as discs in drawing order, and the box round it. */
static void build(void) {
  const uint16_t *p = PALETTE[N.pal];
  float ux, uy, vx, vy, hx, hy, hr, look;
  int k;

  N.ndisc = 0;
  N.box.x0 = N.box.y0 = 32767;
  N.box.x1 = N.box.y1 = -32767;

  arms(1);
  for (k = 0; k < NSEG; k++)
    tube(N.px[k], N.py[k], N.px[k + 1], N.py[k + 1], body_r(k) + 1.0f, C_OUTLINE);
  arms(0);
  for (k = 0; k < NSEG; k++)
    tube(N.px[k], N.py[k], N.px[k + 1], N.py[k + 1], body_r(k),
         ((k / 3) & 1) ? p[1] : p[0]);

  /* The head, on the end of the tube, pointing the way the last segment
   * does. u is up the head, v across it. */
  ux = fsin(N.th[NSEG - 1]);
  uy = -fcos(N.th[NSEG - 1]);
  vx = -uy;
  vy = ux;
  hr = 5.5f + 1.5f * N.P;
  hx = N.px[NSEG] + ux * hr * 0.5f;
  hy = N.py[NSEG] + uy * hr * 0.5f;

  /* Hair: a tuft of three, standing up off the top. */
  for (k = -1; k <= 1; k++)
    disc(hx + ux * hr + vx * 3.0f * (float)k, hy + uy * hr + vy * 3.0f * (float)k,
         2.6f, C_OUTLINE);
  for (k = -1; k <= 1; k++)
    disc(hx + ux * hr + vx * 3.0f * (float)k, hy + uy * hr + vy * 3.0f * (float)k,
         1.8f, p[2]);
  disc(hx, hy, hr + 1.0f, C_OUTLINE);
  disc(hx, hy, hr, p[0]);

  /* The face. Googly: the pupils lag the way the head is swinging. */
  look = clampf(N.w[NSEG - 1] * -0.25f, -1.2f, 1.2f);
  for (k = -1; k <= 1; k += 2) {
    float ex = hx + vx * 2.6f * (float)k + ux * 1.2f;
    float ey = hy + vy * 2.6f * (float)k + uy * 1.2f;
    disc(ex, ey, 2.3f, C_WHITE);
    disc(ex + vx * look, ey + vy * look, 1.0f, C_PUPIL);
  }
  /* Mouth: open as wide as the air is moving -- he is having a great time. */
  disc(hx - ux * 2.8f, hy - uy * 2.8f, 0.8f + 1.8f * N.A, C_MOUTH);
}

/* ---- drawing ------------------------------------------------------------------ */

static void tables(void) {
  int r, dy, y;
  for (r = 0; r < 14; r++)
    for (dy = 0; dy < 14; dy++) {
      int w = 0;
      if (dy > r) { SPAN[r][dy] = 0; continue; }
      while ((w + 1) * (w + 1) + dy * dy <= r * r + r) w++;   /* rounder than r*r */
      SPAN[r][dy] = (uint8_t)w;
    }
  /* The sky: deeper at the top, paler at the horizon. */
  for (y = 0; y < GROUND_Y; y++) {
    int f = y * 256 / GROUND_Y;
    int rr = 104 + (206 - 104) * f / 256;
    int gg = 170 + (228 - 170) * f / 256;
    int bb = 236 + (250 - 236) * f / 256;
    SKY[y] = CAPP_RGB(rr, gg, bb);
  }
}

/* The background, one row of it, from x0 for w pixels. */
static void scene_row(uint16_t *out, int y, int x0, int w) {
  int i;
  if (y >= GROUND_Y) {
    uint16_t c = (y == GROUND_Y) ? C_CURB : C_GROUND;
    for (i = 0; i < w; i++) {
      int x = x0 + i;
      /* Parking-bay lines, slanting away. */
      if (y > GROUND_Y + 2 && ((x + (y - GROUND_Y) * 2) % 60) < 3 && y < GROUND_Y + 14)
        out[i] = C_LINE;
      else out[i] = c;
    }
  } else {
    uint16_t c = SKY[y < 0 ? 0 : y];
    for (i = 0; i < w; i++) out[i] = c;

    /* Bunting across the top: a string that sags between posts, and
     * pennants hanging from it. */
    for (i = 0; i < w; i++) {
      int x = x0 + i, lx = x % 60, sag, dy, k, half;
      sag = 4 + (lx * (60 - lx)) / 150;
      if (y == sag) { out[i] = C_STRING; continue; }
      dy = y - sag;
      if (dy <= 0 || dy > 9) continue;
      k = x % 12;
      half = (10 - dy) * 5 / 10;
      if (k >= 6 - half && k <= 5 + half)
        out[i] = PENNANT[(x / 12) & 3];
    }
  }

  /* The fan: a box on the tarmac, vents on its face. */
  if (y >= (int)BASE_Y - 1 && y < GROUND_Y + 3) {
    int fx0 = (int)BASE_X - 16, fx1 = (int)BASE_X + 16;
    for (i = 0; i < w; i++) {
      int x = x0 + i;
      if (x < fx0 || x >= fx1) continue;
      if (y == (int)BASE_Y - 1) out[i] = C_FAN_HI;
      else if (x == fx0 || x == fx1 - 1) out[i] = C_FAN_HI;
      else if ((y - (int)BASE_Y) % 3 == 1 && x > fx0 + 3 && x < fx1 - 4) out[i] = C_FAN_VENT;
      else out[i] = C_FAN;
    }
  }
}

/* The scene and the figure over it, for rows y0.. of a box, into the band. */
static void render_band(Box b, int y0, int h) {
  int w = b.x1 - b.x0, y, i;
  for (y = 0; y < h; y++) scene_row(BAND + y * w, y0 + y, b.x0, w);

  for (i = 0; i < N.ndisc; i++) {
    const Disc *d = &N.disc[i];
    int r = d->r, yy, ya = d->y - r, yb = d->y + r;
    if (yb < y0 || ya >= y0 + h) continue;
    if (d->x + r < b.x0 || d->x - r >= b.x1) continue;
    if (ya < y0) ya = y0;
    if (yb > y0 + h - 1) yb = y0 + h - 1;
    for (yy = ya; yy <= yb; yy++) {
      int dy = yy - d->y, half, xa, xb;
      uint16_t *row = BAND + (yy - y0) * w;
      if (dy < 0) dy = -dy;
      half = SPAN[r][dy];
      xa = d->x - half;
      xb = d->x + half;
      if (xa < b.x0) xa = b.x0;
      if (xb > b.x1 - 1) xb = b.x1 - 1;
      for (; xa <= xb; xa++) row[xa - b.x0] = d->c;
    }
  }
}

static Box clip_box(Box b) {
  if (b.x0 < 0) b.x0 = 0;
  if (b.y0 < 0) b.y0 = 0;
  if (b.x1 > SCREEN_W) b.x1 = SCREEN_W;
  if (b.y1 > SCREEN_H) b.y1 = SCREEN_H;
  return b;
}

static void render(Box b) {
  int y;
  b = clip_box(b);
  if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
  for (y = b.y0; y < b.y1; y += BAND_H) {
    int h = b.y1 - y < BAND_H ? b.y1 - y : BAND_H;
    render_band(b, y, h);
    api->pixels(rect(N.c.x + b.x0, N.c.y + y, b.x1 - b.x0, h), BAND);
  }
}

/* ---- the strip along the bottom ---------------------------------------------- */

#define HUD_Y   121
#define HUD_H   14
#define METER_W 52

static int hud_state(void) {
  if (N.mic_state == MIC_ON) return N.A > 0.1f ? 2 : 1;
  return 10 + N.mic_state;
}

/* The strip, drawn without drawing any pixel twice: the frame and ground
 * once, the meter as its lit part and its dark part side by side, the line
 * only when it changes and padded to the edge. Clearing it and drawing it
 * again was a flash every frame while anyone blew. */
static void paint_hud(int full) {
  int m = iround(N.A * (float)METER_W), st = hud_state(), i;
  const char *line;
  char s[40];
  int x = N.c.x + METER_W + 10, cols = (SCREEN_W - METER_W - 10) / 6;
  if (full) {
    api->fill(rect(N.c.x, N.c.y + HUD_Y, SCREEN_W, HUD_H), C_HUD_BG);
    api->frame(rect(N.c.x + 3, N.c.y + HUD_Y + 3, METER_W + 2, 8), C_HUD_DIM);
  }
  if (full || m != N.hud_meter) {
    if (m > 0) api->fill(rect(N.c.x + 4, N.c.y + HUD_Y + 4, m, 6), C_METER);
    if (m < METER_W) api->fill(rect(N.c.x + 4 + m, N.c.y + HUD_Y + 4, METER_W - m, 6), C_HUD_BG);
  }
  if (full || st != N.hud_state) {
    switch (N.mic_state) {
    case MIC_ON:      line = st == 2 ? "whoosh!" : "blow on the mic"; break;
    case MIC_BUSY:    line = "mic busy -- space puffs"; break;
    case MIC_REFUSED: line = "no mic -- space puffs"; break;
    default:          line = "no mic here -- space puffs"; break;
    }
    if (cols > (int)sizeof s - 1) cols = (int)sizeof s - 1;
    for (i = 0; line[i] && i < cols; i++) s[i] = line[i];
    for (; i < cols; i++) s[i] = ' ';
    s[cols] = 0;
    api->text((short)x, (short)(N.c.y + HUD_Y + 3), s, C_HUD_FG, C_HUD_BG);
  }
  N.hud_meter = m;
  N.hud_state = st;
}

/* ---- the shell's callbacks ------------------------------------------------------ */

static Box box_union(Box a, Box b) {
  if (b.x0 < a.x0) a.x0 = b.x0;
  if (b.y0 < a.y0) a.y0 = b.y0;
  if (b.x1 > a.x1) a.x1 = b.x1;
  if (b.y1 > a.y1) a.y1 = b.y1;
  return a;
}

static int app_tick(void *st, uint32_t now) {
  uint32_t dt;
  int steps = 0, moved, k;
  float air;
  (void)st;

  if (!N.last_ms) N.last_ms = now;
  dt = now - N.last_ms;
  N.last_ms = now;
  if (dt > 100) dt = 100;                  /* after a pause, do not fast-forward */
  N.acc_ms += dt;

  air = read_air(now);
  while (N.acc_ms >= STEP_MS) {
    N.acc_ms -= STEP_MS;
    step(air);
    steps++;
  }
  if (!steps) return 0;

  /* Nothing moving and no air: he is a heap, and a heap does not need
   * drawing sixty times a second. */
  moved = N.A > 0.01f || N.P > 0.01f;
  for (k = 0; k < NSEG && !moved; k++) if (fabs_(N.w[k]) > 0.02f) moved = 1;
  N.still = moved ? 0 : N.still + 1;

  if (N.still < 3) {
    Box b;
    N.was = N.box;
    build();
    b = box_union(N.was, N.box);
    b = clip_box(b);
    if (b.x1 > b.x0 && b.y1 > b.y0)
      { api->damage(rect(b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0)); N.asked = 1; }
  }
  if (iround(N.A * (float)METER_W) != N.hud_meter || hud_state() != N.hud_state)
    { api->damage(rect(0, HUD_Y, SCREEN_W, HUD_H)); N.asked = 1; }
  return 1;
}

static void app_paint(void *st, CRect c) {
  CRect area = api->paint_area ? api->paint_area() : c;
  Box b;
  (void)st;
  N.c = c;
  if (!N.ndisc) build();

  /* Content-relative, like everything else here. */
  b.x0 = (int16_t)(area.x - c.x);
  b.y0 = (int16_t)(area.y - c.y);
  b.x1 = (int16_t)(b.x0 + area.w);
  b.y1 = (int16_t)(b.y0 + area.h);
  if (b.y1 > HUD_Y) {
    Box above = b;
    above.y1 = HUD_Y;
    if (above.y1 > above.y0) render(above);
    paint_hud(!N.asked);        /* not asked for: the screen was not ours */
  } else {
    render(b);
  }
  N.asked = 0;
}

static int app_key(void *st, unsigned char k) {
  uint32_t now = api->ticks_ms();
  (void)st;
  switch (k) {
  case ' ':
  case CAPP_KEY_ENTER:
  case 'b': case 'B':
    /* A puff that lasts past the key repeat's gap, so holding space is a
     * steady blow rather than a stutter. */
    N.puff_until = now + 300;
    return 1;
  case 'c': case 'C':
    N.pal = (N.pal + 1) & 3;
    N.still = 0;
    build();
    api->damage(rect(N.box.x0, N.box.y0, N.box.x1 - N.box.x0, N.box.y1 - N.box.y0));
    N.asked = 1;
    return 1;
  default:
    return 0;
  }
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Noodle",
  /* 16x16: a tube man, arms up, on his fan. */
  { 0x01, 0x80, 0x03, 0xC0, 0x03, 0xC0, 0x41, 0x82,
    0x21, 0x84, 0x13, 0xC8, 0x0F, 0xF0, 0x03, 0xC0,
    0x03, 0xC0, 0x07, 0xC0, 0x07, 0x80, 0x07, 0x80,
    0x03, 0xC0, 0x1F, 0xF8, 0x1F, 0xF8, 0x1F, 0xF8 },
  "blow\tinto the mic: fill him with air\nspace\ta puff, for no mic\n"
  "c\tanother colour\nfn-`\tleave\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&N, 0, sizeof N);
  N.seed = api->ticks_ms() | 1u;
  N.listen = 1;
  N.floor = 100.0f;                  /* the first readings bring it down */
  N.hud_meter = -1;
  N.hud_state = -1;
  tables();
  slumped();
  build();
  /* Blowing is not a keypress, so without this the screen dims mid-breath. */
  if (api->keep_awake) api->keep_awake(1);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.pref_w = SCREEN_W;
  UI.pref_h = SCREEN_H;
  api->ui(&UI);
  return 0;
}
