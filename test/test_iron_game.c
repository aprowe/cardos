/* Iron's game (apps/iron_game.h), played without a screen. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "tinytest.h"
#include "apps/iron_game.h"

static Iron G;

/* Keys and time as the app gives them: an arrow held repeats every 60 ms
 * after 400, Space likewise. */
static void run_ms(int ms) {
  while (ms > 0) { iron_advance(&G, 33); ms -= 33; }
}

/* Hold Space, then an arrow, for `cells` cells' worth of gliding. */
static void stroke(int dir, int ms) {
  int t;
  iron_press_key(&G, 0);
  run_ms(60);
  iron_move_key(&G, dir, 0);
  for (t = 0; t < ms; t += 60) {
    run_ms(60);
    if (G.phase != IR_PLAY) return;
    if (t >= 360) iron_move_key(&G, dir, 1);
    if (getenv("IRON_TRACE2")) printf("      t%d x%d press%d steam%d heat%d zone%d st%d sumw%u\n", t, G.x >> 8,
                                      G.pressing, G.steaming, G.heat, iron_zone(&G), G.steam, (unsigned)G.sumw);
  }
}

static void go_to(int cx, int cy) {
  G.x = cx * 256 + 128;
  G.y = cy * 256 + 128;
}

/* A player who sets the right heat and irons the garment row band by row
 * band, back and forth. Returns the ms it took or -1. */
static int play_level(void) {
  int band, used = 0;
  iron_dial_set(&G, iron_ideal_dial(&G));
  run_ms(4000);                                  /* the level card: warming */
  iron_begin(&G);
  for (band = 0; band < 30 && G.phase == IR_PLAY; band++) {
    int y = G.lv.by + 1 + (band * 3) % (G.lv.h > 2 ? G.lv.h - 1 : 1);
    go_to(band & 1 ? G.lv.bx + G.lv.w - 1 : G.lv.bx, y);
    stroke(band & 1 ? IR_LEFT : IR_RIGHT, G.lv.w * 100 + 200);
    iron_lift(&G);
    run_ms(100);
    used = (int)(G.lv.time_ms - G.left_ms);
    if (getenv("IRON_TRACE")) printf("    band %d y %d: %d%% flat, %d s left, steam %d\n", band, y, G.flat_pm / 10, (int)(G.left_ms / 1000), G.steam);
    if (G.phase == IR_PLAY && iron_pass_ok(&G) && band >= 10) iron_finish_early(&G);
  }
  return G.phase == IR_DONE ? used : -1;
}

void test_iron_lays_out_three_garments(void) {
  int k, i;
  iron_new_run(&G, 1234);
  for (k = 0; k < 3; k++) {
    int cloth = 0, wr = 0;
    CHECK_EQ(G.lv.kind, k);
    CHECK_EQ(G.phase, IR_READY);
    CHECK(G.lv.w <= 31 && G.lv.h <= 22);         /* what the fold animation holds */
    for (i = 0; i < IR_CELLS; i++) {
      if (G.cell[i].cloth) { cloth++; if (G.cell[i].wr >= IR_FLAT) wr++; }
    }
    CHECK_EQ(cloth, G.ncloth);
    CHECK(wr * 3 > cloth);                       /* plenty to iron */
    CHECK(!iron_pass_ok(&G));
    CHECK(G.lv.need < G.lv.burn);
    CHECK(iron_ideal_dial(&G) >= 1 && iron_ideal_dial(&G) <= IR_DIAL_MAX);
    {
      int h = ir_dial_heat(iron_ideal_dial(&G));
      CHECK(h >= G.lv.need && h <= G.lv.burn);   /* the ideal dial is in the window */
    }
    iron_next_garment(&G);
  }
  CHECK_EQ(G.lv.kind, IR_TOWEL);                 /* round again */
  CHECK_EQ(G.level, 4);
  CHECK_EQ(iron_kind_name(IR_SHIRT)[0], 's');
}

void test_iron_heat_warms_towards_the_dial(void) {
  iron_new_run(&G, 7);
  CHECK_EQ(G.heat, 0);
  iron_dial_set(&G, 2);
  run_ms(3000);
  CHECK_EQ(G.heat, ir_dial_heat(2));
  iron_dial_adjust(&G, -5);
  CHECK_EQ(G.dial, 0);
  run_ms(500);
  CHECK(G.heat < ir_dial_heat(2));
  iron_dial_set(&G, 99);
  CHECK_EQ(G.dial, IR_DIAL_MAX);
}

void test_iron_a_tap_is_a_cell_and_a_hold_glides(void) {
  int x0;
  iron_new_run(&G, 7);
  iron_begin(&G);
  x0 = G.x;
  iron_move_key(&G, IR_RIGHT, 0);
  CHECK_EQ(G.x, x0 + 256);
  run_ms(600);
  CHECK_EQ(G.x, x0 + 256);                       /* let go: no glide */
  CHECK(!G.pressing);
  iron_move_key(&G, IR_LEFT, 0);
  CHECK_EQ(G.face, IR_LEFT);
  run_ms(400);
  iron_move_key(&G, IR_LEFT, 1);
  run_ms(60);
  iron_move_key(&G, IR_LEFT, 1);
  run_ms(60);
  CHECK(G.x < x0 - 256);                         /* held: it glides */
}

void test_iron_pressing_flattens_and_scores(void) {
  uint32_t before;
  int flat0;
  iron_new_run(&G, 99);
  iron_dial_set(&G, iron_ideal_dial(&G));
  run_ms(4000);
  iron_begin(&G);
  go_to(G.lv.bx + 3, G.lv.by + 3);
  before = G.sumw;
  flat0 = G.flat_pm;
  iron_press_key(&G, 0);
  iron_advance(&G, 30);
  CHECK(G.ev & IR_EV_PRESS);
  CHECK(G.pressing);
  CHECK(G.steaming);
  run_ms(300);
  CHECK(G.sumw < before);
  CHECK(G.flat_pm >= flat0);
  CHECK(iron_score(&G) > 0 || G.flat_pm == flat0);
  iron_lift(&G);
  run_ms(50);
  CHECK(!G.pressing);
}

void test_iron_cold_does_nothing(void) {
  uint32_t before;
  iron_new_run(&G, 5);
  iron_dial_set(&G, 0);
  iron_begin(&G);
  go_to(G.lv.bx + 3, G.lv.by + 3);
  before = G.sumw;
  iron_press_key(&G, 0);
  run_ms(300);
  CHECK_EQ(G.sumw, before);
  CHECK(!G.steaming);
}

void test_iron_too_hot_and_still_scorches_then_burns_through(void) {
  int i;
  iron_new_run(&G, 5);                           /* linen: dial 5 is past its window */
  iron_dial_set(&G, IR_DIAL_MAX);
  run_ms(6000);
  CHECK_EQ(iron_zone(&G), IR_Z_HOT);
  iron_begin(&G);
  go_to(G.lv.bx + 5, G.lv.by + 5);
  for (i = 0; i < 40 && G.marks == 0; i++) { iron_press_key(&G, i > 0); run_ms(100); }
  CHECK(G.marks >= 1);
  CHECK(G.cell[(G.lv.by + 5) * IR_W + G.lv.bx + 5].scorch > 0);
  CHECK_EQ(G.meter, 0);
  for (i = 0; i < 200 && G.phase == IR_PLAY; i++) { iron_press_key(&G, 1); run_ms(100); }
  CHECK_EQ(G.phase, IR_OVER);
  CHECK_EQ(G.why, IR_WHY_BURNT);
}

void test_iron_runs_out_of_time(void) {
  int i;
  iron_new_run(&G, 5);
  iron_begin(&G);
  for (i = 0; i < 2000 && G.phase == IR_PLAY; i++) iron_advance(&G, 100);
  CHECK_EQ(G.phase, IR_OVER);
  CHECK_EQ(G.why, IR_WHY_TIME);
  CHECK_EQ(iron_secs_left(&G), 0);
}

void test_iron_enter_needs_the_pass_mark(void) {
  iron_new_run(&G, 5);
  iron_begin(&G);
  CHECK_EQ(iron_finish_early(&G), -1);
  CHECK_EQ(G.phase, IR_PLAY);
  iron_give_up(&G);
  CHECK_EQ(G.phase, IR_OVER);
  CHECK_EQ(G.why, IR_WHY_GIVEUP);
}

/* A careful player gets through the first six garments; print how it went,
 * for tuning. */
void test_iron_is_winnable(void) {
  int lvl;
  iron_new_run(&G, 31337);
  for (lvl = 1; lvl <= 6; lvl++) {
    int ms = play_level();
    printf("  iron L%d %-9s %3d%% flat (need %d%%) in %5d of %5u ms, marks %d, score %u, rating %s\n",
           G.level, iron_kind_name(G.lv.kind), G.res.flat_pm / 10, iron_pass_pct(&G), ms,
           (unsigned)G.lv.time_ms, G.marks, (unsigned)iron_score(&G), iron_rating_name(G.res.rating));
    CHECK(ms > 0);
    if (ms <= 0) return;
    CHECK(G.res.total > 0);
    CHECK_EQ((int)iron_score(&G), G.score);
    iron_next_garment(&G);
  }
}
