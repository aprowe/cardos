/* The pinball table, simulated on the host.
 *
 * There is no debugger on the device and a ball that wedges itself in a wall
 * is invisible until someone plays the game and gets bored. The physics is
 * portable integer code that includes nothing but capp.h, so it runs here --
 * with a fake CardApi whose fill() asserts that every rectangle lands inside
 * the app's own area, which is the one drawing bug the shell cannot catch.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

/* The app itself, statics and all: its internals are the thing under test. */
#include "apps/pinball.c"

static CappUi   INST;          /* what the app installed */
static uint32_t NOW;
static int      OUT_OF_BOUNDS;
static int      FILLS;

static void fake_fill(CRect r, uint16_t colour) {
  (void)colour;
  FILLS++;
  if (r.x < 0 || r.y < 0 || r.x + r.w > SCREEN_W || r.y + r.h > SCREEN_H)
    OUT_OF_BOUNDS++;
}

static void fake_text(int16_t x, int16_t y, const char *s, uint16_t f, uint16_t b) {
  (void)f; (void)b;
  if (x < 0 || y < 0 || y + 8 > SCREEN_H || x + 6 * (int)strlen(s) > SCREEN_W + 6)
    OUT_OF_BOUNDS++;
}

static void *fake_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static size_t fake_strlen(const char *s) { return strlen(s); }
static uint32_t fake_ticks(void) { return NOW; }

static int fake_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

static void fake_ui(const CappUi *ui) { INST = *ui; }

static CardApi API;

static void boot(void) {
  char arg0[] = "pinball";
  char *argv[1];
  argv[0] = arg0;

  memset(&API, 0, sizeof API);
  API.version = CAPP_API_VERSION;
  API.fill = fake_fill;
  API.text = fake_text;
  API.mem_set = fake_memset;
  API.str_len = fake_strlen;
  API.fmt = fake_fmt;
  API.ticks_ms = fake_ticks;
  API.ui = fake_ui;

  NOW = 1000;
  OUT_OF_BOUNDS = 0;
  FILLS = 0;
  memset(&INST, 0, sizeof INST);
  capp_main(&API, 1, argv);
}

/* Advance the game by `ms`, painting whenever the app asks, exactly as the
 * shell does. Returns the number of frames painted. */
static int run(uint32_t ms, int flip_every_ms) {
  CRect c;
  uint32_t end = NOW + ms;
  int painted = 0;

  c.x = 0; c.y = 0; c.w = SCREEN_W; c.h = SCREEN_H;
  while (NOW < end) {
    NOW += 5;                                  /* the shell's loop period */
    if (flip_every_ms && NOW % (uint32_t)flip_every_ms < 5) {
      INST.key(INST.state, CAPP_KEY_LEFT);
      INST.key(INST.state, CAPP_KEY_RIGHT);
    }
    if (INST.tick(INST.state, NOW)) {
      INST.paint(INST.state, c);
      painted++;
    }
  }
  return painted;
}

void test_pinball_installs_a_ui_and_starts_a_game(void) {
  boot();
  CHECK(INST.paint != NULL);
  CHECK(INST.tick != NULL);
  CHECK(INST.key != NULL);
  CHECK_EQ(G.balls, 3);
  CHECK_EQ((int)G.score, 0);
  CHECK(G.waiting);           /* on the plunger, waiting for space */
}

/* Nothing may be drawn outside the app's rectangle. The shell clips, so a bug
 * here is invisible on the device -- and fatal in a window, where the clip is
 * the window and the overspill would be someone else's pixels. */
void test_pinball_never_draws_outside_its_own_rectangle(void) {
  boot();
  INST.key(INST.state, ' ');
  run(20000, 900);
  CHECK(FILLS > 1000);
  CHECK_EQ(OUT_OF_BOUNDS, 0);
}

void test_pinball_ball_stays_on_the_table(void) {
  int i;
  boot();
  INST.key(INST.state, ' ');
  for (i = 0; i < 400; i++) {
    run(100, 0);
    if (!G.b.live) continue;
    /* Inside the panel, with a pixel of slack for the moment between moving
     * and being pushed back out of a wall. */
    CHECK(TO_PX(G.b.x) >= -1);
    CHECK(TO_PX(G.b.x) <= SCREEN_W + 1);
    CHECK(TO_PX(G.b.y) >= BAR_H - 1);
    CHECK(TO_PX(G.b.y) <= FLOOR + BALL_R + 2);
  }
}

/* The launch has to reach the field: a plunger that cannot clear the lane is
 * a game that cannot be played. */
void test_pinball_launch_leaves_the_lane(void) {
  int reached = 0;
  int i;
  boot();
  INST.key(INST.state, ' ');
  for (i = 0; i < 200; i++) {
    run(50, 0);
    if (G.b.live && TO_PX(G.b.x) < LANE_L - 4) { reached = 1; break; }
  }
  CHECK(reached);
}

/* Left alone, a ball drains -- it does not wedge in a corner and stop the
 * game forever. Three balls, so three drains and then game over. */
void test_pinball_a_ball_left_alone_drains(void) {
  int i;
  boot();
  INST.key(INST.state, ' ');
  for (i = 0; i < 600 && !G.over; i++) {
    if (G.waiting) INST.key(INST.state, ' ');
    run(100, 0);
  }
  CHECK(G.over);
  CHECK_EQ(G.balls, 0);
}

/* A ball in play hits things, and hitting things scores. */
void test_pinball_bouncing_around_scores(void) {
  boot();
  INST.key(INST.state, ' ');
  run(8000, 700);
  CHECK(G.score > 0);
}

/* A flick raises the flipper and it falls back on its own, because the
 * keyboard reports no key-up to fall back on. */
void test_pinball_a_flick_raises_the_flipper_then_it_returns(void) {
  int raised;
  boot();
  INST.key(INST.state, CAPP_KEY_LEFT);
  run(40, 0);
  raised = G.fl.angle;
  CHECK(raised < 0);                 /* up is a negative angle */
  run(600, 0);
  CHECK(G.fl.angle > 0);             /* and back down at rest */
}

/* The shell repaints whenever key() returns 1, and a paint the app did not
 * ask for through tick() is read as "something covered me, draw it all".
 * Flipping used to return 1, so every flick redrew the whole table -- for
 * nothing, since the flipper only moves in the next tick anyway. */
void test_pinball_a_flick_does_not_repaint_the_whole_table(void) {
  CRect c;
  int full_paint, flick_paint;
  c.x = 0; c.y = 0; c.w = SCREEN_W; c.h = SCREEN_H;
  boot();
  INST.key(INST.state, ' ');
  run(500, 0);                                  /* settle into incremental paints */

  FILLS = 0;
  G.full = 1;
  INST.paint(INST.state, c);
  full_paint = FILLS;                           /* what a whole table costs */
  run(50, 0);

  FILLS = 0;
  if (INST.key(INST.state, CAPP_KEY_LEFT)) INST.paint(INST.state, c);
  flick_paint = FILLS;
  CHECK(flick_paint < full_paint / 4);
}

void test_pinball_new_game_resets_everything(void) {
  boot();
  INST.key(INST.state, ' ');
  run(6000, 700);
  G.balls = 1;
  INST.key(INST.state, 'n');
  CHECK_EQ(G.balls, 3);
  CHECK_EQ((int)G.score, 0);
  CHECK(G.waiting);
  CHECK(!G.over);
}

/* The trig table is the one piece of maths with no obvious wrongness check at
 * runtime, so it gets one here. */
void test_pinball_trig_table_is_a_circle(void) {
  int deg;
  for (deg = -360; deg <= 720; deg += 7) {
    int s = sin_q8(deg), c = cos_q8(deg);
    int mag = (s * s + c * c) / 256;      /* should be 256, in Q8 */
    CHECK(mag > 248 && mag < 264);
  }
  CHECK_EQ(sin_q8(0), 0);
  CHECK_EQ(sin_q8(90), 256);
  CHECK_EQ(cos_q8(0), 256);
  CHECK_EQ(sin_q8(180), 0);
  CHECK_EQ(sin_q8(270), -256);
}

void test_pinball_isqrt_matches_the_real_thing(void) {
  int v;
  for (v = 0; v < 100000; v += 37) {
    int r = isqrt32(v);
    CHECK(r * r <= v);
    CHECK((r + 1) * (r + 1) > v);
  }
}

/* The bug that made the game unplayable, kept as a test.
 *
 * The closest point on a wall was computed as an integer fraction of the
 * wall's length. The answer is between zero and one, so it truncated to zero
 * every time and every wall in the table acted as a single point at its
 * starting corner -- the ball flew straight through the middle of everything.
 * Nothing above catches that: the ball still stayed on the panel (the screen
 * edges clamp it), still drained, and still scored off the bumpers, which are
 * discs and were never broken. */
void test_pinball_the_closest_point_on_a_wall_is_actually_the_closest(void) {
  Seg ceiling = { WALL_L, FIELD_TOP, WALL_R, FIELD_TOP };
  int x;
  boot();
  for (x = 20; x < WALL_R - 20; x += 30) {
    /* Drop the ball onto the ceiling from just below, moving up into it. */
    G.waiting = 0;
    G.b.live = 1;
    G.b.x = PX(x);
    G.b.y = PX(FIELD_TOP + BALL_R);
    G.b.vx = 0;
    G.b.vy = -PX(2);
    hit_segment(&ceiling, BOUNCE_WALL, 0);
    /* Pushed back out and sent downwards, wherever along the wall it was. */
    CHECK(G.b.vy > 0);
    CHECK(TO_PX(G.b.y) >= FIELD_TOP);
  }
}

/* A ball at the speed cap moves nine pixels a step and a wall is felt from
 * five away, so without sub-stepping it passes straight through. */
void test_pinball_a_fast_ball_cannot_pass_through_a_wall(void) {
  int i;
  boot();
  G.waiting = 0;
  G.b.live = 1;
  G.b.x = PX(120);
  G.b.y = PX(60);
  G.b.vx = 0;
  G.b.vy = -PX(9);              /* straight up at the ceiling, flat out */
  for (i = 0; i < 20; i++) {
    step(NOW + (uint32_t)i);
    CHECK(TO_PX(G.b.y) >= FIELD_TOP - 1);
  }
}

/* And the same downwards, where escaping means falling out of the game. */
void test_pinball_a_fast_ball_does_not_escape_sideways(void) {
  int i;
  boot();
  G.waiting = 0;
  G.b.live = 1;
  G.b.x = PX(120);
  G.b.y = PX(60);
  G.b.vx = -PX(9);
  G.b.vy = 0;
  for (i = 0; i < 30 && G.b.live; i++) {
    step(NOW + (uint32_t)i);
    CHECK(TO_PX(G.b.x) >= WALL_L - BALL_R - 1);
  }
}

/* A ball has to last long enough to play with.
 *
 * "It drains eventually" passed happily when the physics was three times too
 * fast and a ball crossed the table in a fifth of a second -- it drained,
 * just immediately, and the game looked like it was resetting itself. This
 * puts a floor under it: from the launch, with nobody touching a flipper, the
 * first ball survives several seconds of bouncing. */
void test_pinball_a_ball_lasts_long_enough_to_play(void) {
  uint32_t start;
  int alive_ms = 0;
  boot();
  INST.key(INST.state, ' ');
  start = NOW;
  while (NOW - start < 20000) {
    run(100, 0);
    if (G.balls < 3) break;          /* the first ball drained */
    alive_ms = (int)(NOW - start);
  }
  /* Two seconds. Measured at about three with nobody touching a flipper,
   * which is roughly what an untouched ball does on a real table; the floor is
   * here to catch a return to the old physics, where it was well under one. */
  CHECK(alive_ms >= 2000);
  if (alive_ms < 3000)
    printf("      first ball lasted %d ms\n", alive_ms);
}

/* And the launch lane shuts behind the ball. Without the gate a ball that
 * drifts into the top right rolls back down onto the plunger, which reads as
 * the game restarting itself. */
void test_pinball_the_ball_does_not_wander_back_into_the_lane(void) {
  int i, returns = 0;
  boot();
  INST.key(INST.state, ' ');
  for (i = 0; i < 200; i++) {
    int was = G.waiting;
    run(100, 900);
    if (!was && G.waiting && G.balls == 3) returns++;   /* back on the plunger */
  }
  CHECK_EQ(returns, 0);
}
