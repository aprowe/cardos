/* Jar Factory's world on the host: apps/jarsim.h. Where coins come from,
 * that time away makes nothing, upgrades, items and their habits, the save. */
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "apps/jarsim.h"

#define T0 1800000000u

static Jar J;

static void fresh(uint32_t seed) {
  memset(&J, 0, sizeof J);
  js_init(&J, seed);
}

static void run_s(int secs) {
  int i;
  for (i = 0; i < secs * JS_HZ; i++) js_step(&J);
}

static void mk_item(JItem *it, uint32_t id, int kind, int move) {
  int i;
  memset(it, 0, sizeof *it);
  it->id = id;
  it->kind = (uint8_t)kind;
  it->move = (uint8_t)move;
  it->speed = JSP_MEDIUM;
  it->zone = JZ_ANYWHERE;
  it->nframes = 2;
  strcpy(it->name, "Thing");
  for (i = 1; i < 8; i++) it->pal[i] = (uint16_t)(i * 0x0841);
  for (i = 0; i < 256; i++) { ji_set_px(it->frames[0], i, i & 7); ji_set_px(it->frames[1], i, 1); }
}

void test_jarsim_every_coin_is_a_jar_that_left(void) {
  uint32_t seed;
  for (seed = 7; seed < 10; seed++) {
    int i, bad = 0, ships = 0;
    int32_t want;
    fresh(seed);
    for (i = 0; i < 30 * 60 * JS_HZ; i++) {
      uint32_t before = J.coins;
      uint8_t st = J.snail.st;
      js_step(&J);
      if (J.coins != J.shipped * JS_JAR_VALUE) bad++;
      if (J.coins != before) {
        /* coins move only when the snail has just gone off the screen */
        if (!(st == S_OUT && J.snail.st == S_AWAY && J.snail.x >= JS_EXIT_X * JS_FX)) bad++;
        ships++;
      }
    }
    CHECK_EQ(bad, 0);
    CHECK(ships > 10);
    /* and the factory makes about what it says it can: 30 minutes at the rate */
    want = js_rate_ph(&J) / 2;
    printf("    30 min: %u jars shipped, rate %d/h -> %d expected\n", (unsigned)J.coins,
           (int)js_rate_ph(&J), (int)want);
    CHECK((int32_t)J.coins > want * 7 / 10);
    CHECK((int32_t)J.coins < want * 13 / 10);
  }
}

void test_jarsim_starts_at_about_three_a_minute(void) {
  fresh(1);
  CHECK(js_rate_ph(&J) >= 150);
  CHECK(js_rate_ph(&J) <= 220);
}

/* Nothing is made off screen: hours away leave the coins, the dock and the
 * berries as they were; only `seen` moves. */
void test_jarsim_time_away_makes_nothing(void) {
  int i;
  int32_t grow[JS_MAX_BEDS];
  uint8_t ready[JS_MAX_BEDS];
  fresh(3);
  J.coins = 123;
  J.dock = 2;
  J.seen = T0;
  for (i = 0; i < JS_MAX_BEDS; i++) { grow[i] = J.bed[i].grow; ready[i] = J.bed[i].ready; }
  CHECK_EQ(js_away(&J, T0 + 1800), 0);
  CHECK_EQ(js_away(&J, T0 + 2 * 86400), 0);
  CHECK_EQ(J.coins, 123);
  CHECK_EQ(J.dock, 2);
  CHECK_EQ(J.shipped, 0);
  CHECK_EQ(J.seen, T0 + 2 * 86400);
  for (i = 0; i < JS_MAX_BEDS; i++) {
    CHECK_EQ(J.bed[i].grow, grow[i]);
    CHECK_EQ(J.bed[i].ready, ready[i]);
  }
}

void test_jarsim_no_clock_no_away(void) {
  fresh(3);
  CHECK_EQ(js_away(&J, 0), 0);            /* no clock now */
  CHECK_EQ(J.dock, 0);
  CHECK_EQ(js_away(&J, T0), 0);           /* never saved with a clock: start counting */
  CHECK_EQ(J.seen, T0);
  CHECK_EQ(js_away(&J, T0 - 50), 0);      /* a clock that went back */
  CHECK_EQ(J.dock, 0);
  /* and no day or night */
  js_set_minute(&J, -1);
  CHECK_EQ(J.phase, PH_NONE);
}

void test_jarsim_upgrades(void) {
  int32_t r0;
  fresh(5);
  r0 = js_rate_ph(&J);
  CHECK_EQ(js_buy(&J, JU_BED), -2);       /* no coins */
  J.coins = 10000;
  CHECK_EQ(js_up_cost(&J, JU_BED), 25);
  CHECK_EQ(js_buy(&J, JU_BED), 0);
  CHECK_EQ(J.coins, 10000 - 25);
  CHECK_EQ(J.nbeds, 3);
  CHECK(js_rate_ph(&J) > r0);
  CHECK_EQ(js_rate_after(&J, JU_MACH), js_rate_for(3, 1, 3, 0));
  while (js_buy(&J, JU_MACH) == 0) {}
  CHECK_EQ(J.nmach, JS_MAX_MACH);
  CHECK_EQ(js_up_cost(&J, JU_MACH), -1);
  CHECK_EQ(js_buy(&J, JU_MACH), -1);
  CHECK(js_mach_active(&J, 1) && js_mach_active(&J, 2));
  while (js_buy(&J, JU_MOSS) == 0) {}
  while (js_buy(&J, JU_BELT) == 0) {}
  while (js_buy(&J, JU_BED) == 0) {}
  CHECK_EQ(J.nmoss, JS_MAX_MOSS);
  CHECK_EQ(J.belt, JS_MAX_BELT);
  CHECK_EQ(J.nbeds, JS_MAX_BEDS);
  /* everything bought makes a busier jar, and the loop still balances */
  CHECK(js_rate_ph(&J) > 2 * r0);
  {
    uint32_t c = J.coins;
    run_s(600);
    CHECK_EQ(J.coins - c, J.shipped * JS_JAR_VALUE);
    CHECK((int32_t)J.shipped > js_rate_ph(&J) / 6 * 7 / 10);
  }
}

void test_jarsim_a_jam_is_fixed_even_with_one_mossling(void) {
  static JItem it;
  int i;
  fresh(9);
  mk_item(&it, 1, JK_FLOOR, JM_SITS);
  it.nhab = 2;
  it.hab[0].event = JE_JAM; it.hab[0].earg = 1; it.hab[0].action = JA_GLOW; it.hab[0].aarg = 1;
  it.hab[1].event = JE_JAM; it.hab[1].earg = 2; it.hab[1].action = JA_GLOW; it.hab[1].aarg = 0;
  CHECK_EQ(js_place(&J, &it, 30, 0), 0);
  run_s(20);
  J.jam_clock = 1;
  js_step(&J);
  CHECK(J.jammed);
  CHECK_EQ(J.placed[0].glow, 1);          /* jam:jammed fired */
  for (i = 0; i < 90 * JS_HZ && J.jammed; i++) js_step(&J);
  CHECK(!J.jammed);
  CHECK_EQ(J.placed[0].glow, 0);          /* jam:fixed fired */
  /* and the factory goes on afterwards */
  {
    uint32_t s = J.shipped;
    run_s(180);
    CHECK(J.shipped > s);
  }
}

void test_jarsim_two_mosslings_fix_a_jam(void) {
  int i;
  fresh(11);
  J.nmoss = 3;
  run_s(10);
  J.jam_clock = 1;
  js_step(&J);
  for (i = 0; i < 90 * JS_HZ && J.jammed; i++) {
    js_step(&J);
    CHECK(J.fixer[0] != JW_SNAIL && J.fixer[1] != JW_SNAIL);
  }
  CHECK(!J.jammed);
}

void test_jarsim_a_mossling_naps_and_wakes(void) {
  int i, napped = 0, woke = 0;
  fresh(13);
  J.nap_clock = 1;
  for (i = 0; i < 60 * JS_HZ; i++) {
    js_step(&J);
    if (J.moss[0].st == M_NAP) napped = 1;
    else if (napped) woke = 1;
  }
  CHECK(napped);
  CHECK(woke);
}

void test_jarsim_habits_go_through_events_and_actions(void) {
  static JItem it;
  JPlaced *p;
  fresh(15);
  mk_item(&it, 2, JK_HANGING, JM_SWAYS);
  it.nbub = 2;
  strcpy(it.bub[0], "hello");
  strcpy(it.bub[1], "night!");
  it.nhab = 3;
  it.hab[0].event = JE_TIME; it.hab[0].earg = PH_NIGHT; it.hab[0].action = JA_GLOW; it.hab[0].aarg = 1;
  it.hab[1].event = JE_TIME; it.hab[1].earg = PH_DAY;   it.hab[1].action = JA_GLOW; it.hab[1].aarg = 0;
  it.hab[2].event = JE_POKE; it.hab[2].earg = 0;        it.hab[2].action = JA_SAY;  it.hab[2].aarg = 1;
  CHECK_EQ(js_place(&J, &it, 100, 20), 0);
  p = &J.placed[0];
  js_set_minute(&J, 22 * 60);              /* night begins */
  CHECK_EQ(p->glow, 1);
  js_set_minute(&J, 22 * 60 + 5);          /* still night: no new event */
  CHECK_EQ(js_event(&J, JE_TIME, PH_DAWN), 0);   /* nothing listens for dawn */
  js_set_minute(&J, 12 * 60);              /* day */
  CHECK_EQ(p->glow, 0);
  CHECK_EQ(js_item_event(&J, 0, JE_POKE, 0), 1);
  CHECK_EQ(p->say, 1);
  CHECK(p->say_t > 0);
  /* the same actions, called directly, as a script will */
  CHECK_EQ(js_act(&J, 0, JA_GLOW, 2), 0);
  CHECK_EQ(p->glow, 1);
  CHECK_EQ(js_act(&J, 0, JA_SAY, 3), -1);  /* it has no fourth bubble */
  CHECK_EQ(js_act(&J, 0, JA_HOP, 4), -1);  /* hanging things do not hop */
  CHECK_EQ(js_act(&J, 0, JA_FRAME, 1), 0);
  CHECK_EQ(p->cur, 1);
  CHECK_EQ(js_sense(&J, 0, JSN_TIME), PH_DAY);
  /* the weather: a sense, and an event on a change only, narrowed by its tag */
  CHECK_EQ(js_sense(&J, 0, JSN_WEATHER), 0);
  p->hab[2].event = JE_WEATHER; p->hab[2].earg = JWX_SNOWY; p->hab[2].action = JA_GLOW; p->hab[2].aarg = 1;
  p->glow = 0;
  js_set_weather(&J, JWX_RAINY);
  CHECK_EQ(p->glow, 0);                    /* rain is not snow */
  CHECK_EQ(js_sense(&J, 0, JSN_WEATHER), JWX_RAINY);
  js_set_weather(&J, JWX_SNOWY);
  CHECK_EQ(p->glow, 1);
  p->glow = 0;
  js_set_weather(&J, JWX_SNOWY);           /* no change, no event */
  CHECK_EQ(p->glow, 0);
  CHECK_EQ(js_sense(&J, 0, JSN_X), 100);
  CHECK_EQ(js_sense(&J, 0, JSN_ZONE), JZ_WORKS);
}

void test_jarsim_near_and_tick_habits(void) {
  static JItem it;
  int i, said = 0, frames = 0;
  fresh(17);
  mk_item(&it, 3, JK_FLOOR, JM_SITS);
  it.nbub = 1;
  strcpy(it.bub[0], "ribbit");
  it.nhab = 2;
  it.hab[0].event = JE_NEAR; it.hab[0].earg = JN_MOSS; it.hab[0].action = JA_SAY; it.hab[0].aarg = 0;
  it.hab[1].event = JE_TICK; it.hab[1].earg = 8;       it.hab[1].action = JA_FLIP; it.hab[1].aarg = 0;
  CHECK_EQ(js_place(&J, &it, JS_BED_X(0) + 4, 0), 0);
  for (i = 0; i < 40 * JS_HZ; i++) {
    uint8_t f = J.placed[0].flip;
    js_step(&J);
    if (J.placed[0].say_t == JS_SAY_STEPS) said++;
    if (J.placed[0].flip != f) frames++;
  }
  CHECK(said >= 1);                        /* a mossling came by for a berry */
  CHECK(frames >= 19 && frames <= 21);     /* every 2 s, for 40 s */
}

void test_jarsim_a_liked_item_speeds_a_mossling(void) {
  static JItem it;
  fresh(19);
  mk_item(&it, 4, JK_FLOOR, JM_SITS);
  strcpy(it.tags, "cosy,soft");
  CHECK_EQ(js_place(&J, &it, J.moss[0].x / JS_FX, 0), 0);
  CHECK_EQ(J.placed[0].tags, JT_BIT(JT_COSY) | JT_BIT(JT_SOFT));
  J.steps = JS_ITEM_TICK - 1;
  js_step(&J);
  CHECK(J.moss[0].boost > 0);
  CHECK(J.moss[0].boost <= JS_BOOST_STEPS);
  /* spooky things do nothing for a mossling */
  fresh(19);
  strcpy(it.tags, "spooky");
  js_place(&J, &it, J.moss[0].x / JS_FX, 0);
  J.steps = JS_ITEM_TICK - 1;
  js_step(&J);
  CHECK_EQ(J.moss[0].boost, 0);
}

void test_jarsim_placement_limits(void) {
  static JItem it;
  int i, used = 0;
  fresh(21);
  for (i = 0; i < JS_MAX_PLACED; i++) {
    mk_item(&it, 100 + (uint32_t)i, i % 3, JM_SITS);
    it.nframes = 1;
    CHECK_EQ(js_place(&J, &it, 20 + i * 8, 0), i);
  }
  mk_item(&it, 999, JK_FLOOR, JM_SITS);
  CHECK_EQ(js_place(&J, &it, 50, 0), -1);   /* the 25th */
  js_unplace(&J, 3);
  CHECK_EQ(J.nplaced, JS_MAX_PLACED - 1);
  CHECK_EQ(js_find(&J, 103), -1);
  CHECK_EQ(js_find(&J, 104), 3);            /* the rest moved up */
  CHECK(js_place(&J, &it, 50, 0) >= 0);
  CHECK_EQ(js_place(&J, &it, 60, 0), -1);   /* the same item twice */
  for (i = 0; i < JS_POOL; i++) used += J.pool_used[i];
  CHECK_EQ(used, JS_MAX_PLACED - 1 + 2);
  /* x is kept inside the glass; a hanging string has a length */
  fresh(21);
  mk_item(&it, 7, JK_HANGING, JM_SWAYS);
  js_place(&J, &it, 400, 300);
  CHECK_EQ(J.placed[0].home_x, JS_HANG_HI);   /* under the lid, not the shoulder */
  CHECK_EQ(J.placed[0].home_y, 60);
  js_move_to(&J, 0, 2, 10);
  CHECK_EQ(J.placed[0].home_x, JS_HANG_LO);
  CHECK_EQ(J.placed[0].home_y, 10);
}

void test_jarsim_pool_runs_out_gracefully(void) {
  static JItem it;
  int i;
  fresh(23);
  for (i = 0; i < 8; i++) {
    mk_item(&it, 200 + (uint32_t)i, JK_FLOOR, JM_SITS);
    it.nframes = 4;
    CHECK_EQ(js_place(&J, &it, 30 + i * 10, 0), i);
  }
  mk_item(&it, 300, JK_FLOOR, JM_SITS);
  it.nframes = 4;
  CHECK_EQ(js_place(&J, &it, 150, 0), -1);  /* 32 frames, all taken */
  js_unplace(&J, 0);                        /* four come back */
  mk_item(&it, 301, JK_FLOOR, JM_SITS);
  it.nframes = 2;
  CHECK(js_place(&J, &it, 150, 0) >= 0);
  mk_item(&it, 302, JK_FLOOR, JM_SITS);
  it.nframes = 4;
  i = js_place(&J, &it, 160, 0);
  CHECK(i >= 0);
  CHECK_EQ(J.placed[i].nframes, 2);         /* what was left: it shows those */
}

void test_jarsim_save_and_load(void) {
  static JItem it;
  static char buf[2048];
  static Jar k;
  int n;
  fresh(25);
  J.coins = 1234;
  J.shipped = 5678;
  J.seen = T0;
  J.dock = 4;
  J.sold = 0x5A;
  J.coins += 0;
  J.nbeds = 4; J.nmoss = 3; J.nmach = 3; J.belt = 2;
  J.bed[2].grow = 12000;
  js_own(&J, 1); js_own(&J, 2); js_own(&J, 77);
  mk_item(&it, 77, JK_HANGING, JM_SWAYS);
  js_place(&J, &it, 120, 33);
  n = js_save(&J, buf, sizeof buf);
  CHECK(n > 40 && n < (int)sizeof buf - 1);
  memset(&k, 0, sizeof k);
  js_init(&k, 1);
  CHECK_EQ(js_load(&k, buf), 0);
  CHECK_EQ(k.coins, 1234);
  CHECK_EQ(k.shipped, 5678);
  CHECK_EQ(k.seen, T0);
  CHECK_EQ(k.dock, 4);
  CHECK_EQ(k.sold, 0x5A);
  CHECK_EQ(k.nbeds, 4); CHECK_EQ(k.nmoss, 3); CHECK_EQ(k.nmach, 3); CHECK_EQ(k.belt, 2);
  CHECK_EQ(k.bed[2].grow, 12000);
  CHECK_EQ(k.nowned, 3);
  CHECK(js_owns(&k, 77));
  CHECK_EQ(k.nwant, 1);
  CHECK_EQ(k.want[0].id, 77);
  CHECK_EQ(k.want[0].x, 120);
  CHECK_EQ(k.want[0].y, 33);
  CHECK_EQ(js_load(&k, "not a jar"), -1);
}

void test_jarsim_a_snail_load_in_flight_is_saved_as_on_the_dock(void) {
  static char buf[2048];
  static Jar k;
  fresh(27);
  J.dock = 1;
  J.snail.load = 2;
  J.snail.st = S_OUT;
  js_save(&J, buf, sizeof buf);
  memset(&k, 0, sizeof k);
  js_init(&k, 1);
  js_load(&k, buf);
  CHECK_EQ(k.dock, 3);
}

/* ---- levels: the ledges ---------------------------------------------------- */

void test_jarsim_floor_decor_goes_on_a_ledge(void) {
  static JItem it;
  int i, lo, hi, y0;
  fresh(31);
  mk_item(&it, 40, JK_FLOOR, JM_SITS);
  CHECK_EQ(js_place(&J, &it, 150, 0), 0);
  y0 = js_item_y(&J, 0);
  CHECK_EQ(js_set_level(&J, 0, 1), 1);
  CHECK_EQ(J.placed[0].level, 1);
  CHECK_EQ(J.placed[0].level, 1);
  CHECK_EQ(J.placed[0].lift, JS_LEDGE_H[1]);
  CHECK_EQ(J.placed[0].home_x, JS_LEDGE_HI[1]);       /* kept on the ruler */
  CHECK_EQ(js_item_y(&J, 0), y0 - JS_LEDGE_H[1]);
  CHECK_EQ(js_sense(&J, 0, JSN_ZONE), JZ_HIGH);
  js_move_to(&J, 0, 0, 0);                            /* along the ledge, not off it */
  CHECK_EQ(J.placed[0].home_x, JS_LEDGE_LO[1]);
  js_move_to(&J, 0, 300, 0);
  CHECK_EQ(J.placed[0].home_x, JS_LEDGE_HI[1]);
  js_range(&J.placed[0], &lo, &hi);
  CHECK(lo >= JS_LEDGE_LO[1] && hi <= JS_LEDGE_HI[1]);
  CHECK_EQ(js_set_level(&J, 0, 9), JS_LEVELS - 1);    /* there is no higher */
  CHECK_EQ(js_set_level(&J, 0, 0), 0);
  CHECK_EQ(J.placed[0].lift, 0);
  /* hanging things and fliers stay as they are */
  mk_item(&it, 41, JK_HANGING, JM_SWAYS);
  i = js_place(&J, &it, 100, 20);
  CHECK_EQ(js_set_level(&J, i, 2), 0);
  CHECK_EQ(J.placed[i].lift, 0);
  mk_item(&it, 42, JK_CRITTER, JM_FLOATS);
  i = js_place(&J, &it, 100, 0);
  CHECK_EQ(js_set_level(&J, i, 1), 0);
  /* a hopping frog up there hops along the ledge, never off it */
  mk_item(&it, 43, JK_FLOOR, JM_HOPS);
  i = js_place(&J, &it, 120, 0);
  js_set_level(&J, i, 2);
  for (y0 = 0; y0 < 60 * JS_HZ; y0++) {
    js_step(&J);
    CHECK(J.placed[i].x / JS_FX >= JS_LEDGE_LO[2] && J.placed[i].x / JS_FX <= JS_LEDGE_HI[2]);
    CHECK_EQ(J.placed[i].level, 2);
  }
}

void test_jarsim_levels_are_saved_and_an_old_save_is_on_the_soil(void) {
  static JItem it;
  static char buf[2048];
  static Jar k;
  fresh(33);
  mk_item(&it, 50, JK_FLOOR, JM_SITS);
  js_place(&J, &it, 40, 0);
  js_set_level(&J, 0, 1);
  mk_item(&it, 51, JK_FLOOR, JM_SITS);
  js_place(&J, &it, 160, 0);
  mk_item(&it, 52, JK_CRITTER, JM_WANDERS);
  js_place(&J, &it, 100, 0);
  js_set_level(&J, 2, 2);
  js_save(&J, buf, sizeof buf);
  CHECK(strstr(buf, "place 50 40 0 1\n") != 0);
  CHECK(strstr(buf, "place 51 160 0\n") != 0);      /* on the soil: the line as it always was */
  CHECK(strstr(buf, "place 52 100 0 2\n") != 0);
  memset(&k, 0, sizeof k);
  js_init(&k, 1);
  CHECK_EQ(js_load(&k, buf), 0);
  CHECK_EQ(k.nwant, 3);
  CHECK_EQ(k.want[0].lv, 1);
  CHECK_EQ(k.want[1].lv, 0);
  CHECK_EQ(k.want[2].lv, 2);
  /* a companion that loads and saves without placing keeps the levels */
  js_save(&k, buf, sizeof buf);
  CHECK(strstr(buf, "place 50 40 0 1\n") != 0);
  CHECK(strstr(buf, "place 52 100 0 2\n") != 0);
  /* a save from before levels: everything on the soil */
  memset(&k, 0, sizeof k);
  js_init(&k, 1);
  CHECK_EQ(js_load(&k, "jar 1\ncoins 5\nplace 50 40 0\nplace 77 120 33\n"), 0);
  CHECK_EQ(k.nwant, 2);
  CHECK_EQ(k.want[0].lv, 0);
  CHECK_EQ(k.want[1].lv, 0);
  CHECK_EQ(k.want[1].y, 33);
  /* and a level from the future, or nonsense, is the top or the soil */
  memset(&k, 0, sizeof k);
  js_init(&k, 1);
  js_load(&k, "jar 1\nplace 50 40 0 7\n");
  CHECK_EQ(k.want[0].lv, JS_LEVELS - 1);
}

/* A critter that likes it high climbs a ledge, potters about up there and
 * comes down again; one that does not goes up now and then. Never off the
 * ledge while up, and the climb is at the ledge's climb. */
void test_jarsim_critters_climb_up_and_come_down(void) {
  static JItem it;
  int s, up = 0, downs = 0, was = 0, other_up = 0, bad = 0;
  fresh(35);
  mk_item(&it, 60, JK_CRITTER, JM_WANDERS);
  it.zone = JZ_HIGH;
  CHECK_EQ(js_place(&J, &it, 150, 0), 0);
  mk_item(&it, 61, JK_CRITTER, JM_HOPS);
  it.zone = JZ_GARDEN;
  CHECK_EQ(js_place(&J, &it, 40, 0), 1);
  for (s = 0; s < 30 * 60 * JS_HZ; s++) {
    const JPlaced *p = &J.placed[0];
    int x;
    js_step(&J);
    x = p->x / JS_FX;
    if (p->climb == JC_UP && x != JS_LEDGE_UP[p->goal]) bad++;
    if (p->climb == JC_DOWN && x != JS_LEDGE_UP[p->level]) bad++;
    if (p->level && !p->climb) {
      if (x < JS_LEDGE_LO[p->level] || x > JS_LEDGE_HI[p->level]) bad++;
      if (p->lift != JS_LEDGE_H[p->level]) bad++;
      up++;
    }
    if (was == JC_DOWN && !p->level && !p->climb) downs++;   /* down, and off */
    was = p->climb;
    if (J.placed[1].level && !J.placed[1].climb) other_up++;
  }
  printf("      30 min: up %d%% of the time, came down %d times; the other up %d%%\n",
         up * 100 / (30 * 60 * JS_HZ), downs, other_up * 100 / (30 * 60 * JS_HZ));
  CHECK_EQ(bad, 0);
  CHECK(up > 30 * 60 * JS_HZ / 4);          /* it likes it up there */
  CHECK(downs >= 2);                         /* ... and comes down */
  CHECK(other_up > 0);                       /* a garden hopper goes up now and then */
  CHECK(other_up < 30 * 60 * JS_HZ / 2);     /* ... but not for long */
  /* "walk high" in a recipe is a climb; "walk garden" from a ledge, down */
  fresh(36);
  mk_item(&it, 62, JK_CRITTER, JM_SITS);
  it.move = JM_WANDERS;
  js_place(&J, &it, 50, 0);
  J.placed[0].t = 30000;                     /* no wandering of its own */
  CHECK_EQ(js_act(&J, 0, JA_WALK, JZ_HIGH), 0);
  CHECK_EQ(J.placed[0].climb, JC_TO_UP);
  CHECK_EQ(J.placed[0].goal, 1);             /* the nearer ledge */
  for (s = 0; s < 20 * JS_HZ && J.placed[0].climb; s++) js_step(&J);
  CHECK_EQ(J.placed[0].level, 1);
  CHECK_EQ(J.placed[0].lift, JS_LEDGE_H[1]);
  J.placed[0].t = 30000;
  CHECK_EQ(js_act(&J, 0, JA_WALK, JZ_GARDEN), 0);
  CHECK_EQ(J.placed[0].climb, JC_TO_DOWN);
  for (s = 0; s < 20 * JS_HZ && J.placed[0].climb; s++) js_step(&J);
  CHECK_EQ(J.placed[0].level, 0);
  CHECK_EQ(J.placed[0].lift, 0);
  /* moving it in Decorate calls a climb off: it is where it was put */
  js_trip(&J, 0, 2);
  js_step(&J);
  js_move_to(&J, 0, 120, 0);
  CHECK_EQ(J.placed[0].climb, 0);
  CHECK_EQ(J.placed[0].level, 0);
}


/* ---- the living jar (language v2, world events, chores) -------------------- */

static int place_at(uint32_t id, int kind, int move, int x, const char *tags) {
  static JItem it;
  mk_item(&it, id, kind, move);
  strcpy(it.tags, tags);
  return js_place(&J, &it, x, 0);
}

void test_jarsim_traits_old_words_and_new(void) {
  fresh(3);
  CHECK_EQ(js_tags("shiny,noisy"), JT_BIT(JT_SHINY) | JT_BIT(JT_NOISY));
  CHECK_EQ(js_tags("glowing, fancy"), JT_BIT(JT_LIGHT) | JT_BIT(JT_SHINY));   /* older words */
  CHECK_EQ(js_tags("sleepy"), 0x8000);
  CHECK_EQ(js_tags("purple,,food"), JT_BIT(JT_FOOD));
  CHECK_EQ(place_at(1, JK_FLOOR, JM_SITS, 100, "sleepy,food"), 0);
  CHECK_EQ(js_sense(&J, 0, JSN_TRAITS), -32768 | JT_BIT(JT_FOOD));   /* bit 15 kept */
}

void test_jarsim_eat_drop_and_the_berry_event(void) {
  int b;
  fresh(4);
  b = 0;
  J.bed[b].ready = 1;
  CHECK_EQ(place_at(1, JK_CRITTER, JM_SITS, JS_BED_X(b) + 4, "wild"), 0);
  CHECK_EQ(js_sense(&J, 0, JSN_BERRIES), 1);
  CHECK_EQ(js_act(&J, 0, JA_EAT, 0), 0);
  CHECK_EQ(J.bed[b].ready, 0);
  CHECK_EQ(J.placed[0].fed, 1);
  CHECK_EQ(js_sense(&J, 0, JSN_FED), 1);
  CHECK_EQ(js_act(&J, 0, JA_EAT, 0), -1);              /* nothing ripe in reach */
  /* a pellet feeds the bush: it ripens sooner, and ripening is an event */
  J.placed[0].hab[0].event = JE_BERRY;
  J.placed[0].hab[0].action = JA_SAY;
  J.placed[0].nhab = 1;
  J.placed[0].nbub = 1;
  J.bed[b].grow = 0;
  CHECK_EQ(js_act(&J, 0, JA_DROP, 0), 0);
  CHECK_EQ(J.bed[b].grow, JS_GROW_MS / 3);
  CHECK(J.pel_t[0] > 0);
  J.bed[b].grow = JS_GROW_MS - JS_STEP_MS;
  J.placed[0].say_t = 0;
  js_step(&J);
  CHECK_EQ(J.bed[b].ready, 1);
  CHECK(J.placed[0].say_t > 0);                        /* on berry: it said so */
}

void test_jarsim_throw_lands_and_bumps(void) {
  int i, bumped = 0;
  fresh(5);
  CHECK_EQ(place_at(1, JK_CRITTER, JM_SITS, 100, ""), 0);
  CHECK_EQ(place_at(2, JK_FLOOR, JM_SITS, 112, ""), 1);
  CHECK_EQ(place_at(3, JK_FLOOR, JM_SITS, 200, "big"), 2);
  J.placed[1].hab[0].event = JE_BUMPED;
  J.placed[1].hab[0].action = JA_SAY;
  J.placed[1].nhab = 1;
  J.placed[1].nbub = 1;
  CHECK_EQ(js_act(&J, 0, JA_THROW, 0), 0);
  CHECK(J.placed[1].vy > 0 && J.placed[1].vx > 0);       /* away from the thrower */
  for (i = 0; i < 200; i++) {
    js_step_item(&J, 1);
    if (J.placed[1].say_t > 0) bumped = 1;
  }
  CHECK(bumped);
  CHECK_EQ(J.placed[1].yoff, 0);
  CHECK_EQ(J.placed[1].x, (int32_t)J.placed[1].home_x * JS_FX);   /* decor drifts home */
  /* a big thing is never thrown; nothing in reach, nothing thrown */
  J.placed[1].x = 300 * JS_FX;
  CHECK_EQ(js_act(&J, 0, JA_THROW, 0), -1);
}

void test_jarsim_signals_are_heard_next_step_by_the_others(void) {
  fresh(6);
  CHECK_EQ(place_at(1, JK_FLOOR, JM_SITS, 60, ""), 0);
  CHECK_EQ(place_at(2, JK_FLOOR, JM_SITS, 160, ""), 1);
  J.placed[0].hab[0].event = JE_SIGNAL; J.placed[0].hab[0].earg = 2; J.placed[0].hab[0].action = JA_GLOW;
  J.placed[0].hab[0].aarg = 1;
  J.placed[0].nhab = 1;
  J.placed[1].hab[0].event = JE_SIGNAL; J.placed[1].hab[0].earg = 2; J.placed[1].hab[0].action = JA_GLOW;
  J.placed[1].hab[0].aarg = 1;
  J.placed[1].nhab = 1;
  CHECK_EQ(js_act(&J, 0, JA_SIGNAL, 2), 0);
  CHECK_EQ(J.placed[1].glow, 0);                       /* not yet: no recursion */
  js_step(&J);
  CHECK_EQ(J.placed[1].glow, 1);
  CHECK_EQ(J.placed[0].glow, 0);                       /* not to itself */
  CHECK_EQ(js_act(&J, 0, JA_SIGNAL, 0), -1);
  CHECK_EQ(js_act(&J, 0, JA_SIGNAL, 3), 0);
  js_step(&J);                                         /* 3 is not 2: nobody */
}

void test_jarsim_seek_sound_burst_boost_shake(void) {
  int k, live = 0;
  fresh(7);
  CHECK_EQ(place_at(1, JK_CRITTER, JM_SITS, 40, ""), 0);
  CHECK_EQ(place_at(2, JK_FLOOR, JM_SITS, 150, "food"), 1);
  CHECK_EQ(js_act(&J, 0, JA_SEEK, JT_FOOD), 0);
  CHECK(J.placed[0].moving && J.placed[0].tx > 120 * JS_FX);
  CHECK_EQ(js_act(&J, 0, JA_SEEK, JT_MUSIC), -1);       /* nothing like that here */
  CHECK_EQ(js_act(&J, 1, JA_SEEK, JT_FOOD), -1);        /* decor does not walk */
  CHECK_EQ(js_act(&J, 0, JA_SOUND, JSD_HORN), 0);
  CHECK_EQ(J.snd, JSD_HORN);
  CHECK_EQ(js_act(&J, 0, JA_SOUND, 99), -1);
  CHECK_EQ(js_act(&J, 0, JA_BURST, JP_SPARKLE), 0);
  for (k = 0; k < JS_PARTS; k++) live += J.part[k].life > 0;
  CHECK(live >= 10);
  CHECK_EQ(js_act(&J, 0, JA_BOOST, 0), 0);
  CHECK_EQ(J.belt_boost, 10 * JS_HZ);
  CHECK_EQ(js_act(&J, 0, JA_BOOST, 30), -1);            /* once a minute */
  CHECK_EQ(js_act(&J, 0, JA_SHAKE, 0), 0);
  CHECK(J.placed[0].vy > 0 && J.placed[1].vy > 0);
}

void test_jarsim_the_hour_and_a_new_item(void) {
  fresh(8);
  CHECK_EQ(place_at(1, JK_FLOOR, JM_SITS, 60, ""), 0);
  J.placed[0].hab[0].event = JE_HOUR; J.placed[0].hab[0].earg = 19;   /* 18:00 */
  J.placed[0].hab[0].action = JA_GLOW; J.placed[0].hab[0].aarg = 2;
  J.placed[0].hab[1].event = JE_NEW; J.placed[0].hab[1].action = JA_HOP;
  J.placed[0].nhab = 2;
  CHECK_EQ(js_sense(&J, 0, JSN_HOUR), -1);
  js_set_hour(&J, 17);
  CHECK_EQ(J.placed[0].glow, 0);
  js_set_hour(&J, 18);
  CHECK_EQ(J.placed[0].glow, 1);
  js_set_hour(&J, 18);                                 /* the same hour: once */
  CHECK_EQ(J.placed[0].glow, 1);
  CHECK_EQ(js_sense(&J, 0, JSN_HOUR), 18);
  CHECK_EQ(place_at(2, JK_FLOOR, JM_SITS, 160, "sweet"), 1);
  js_announce(&J, 1);
  CHECK(J.placed[0].vy > 0);
  CHECK_EQ(js_sense(&J, 0, JSN_NEW_TRAITS), JT_BIT(JT_SWEET));
}

void test_jarsim_a_birdhouse_bird_flies_and_comes_home(void) {
  int i, away = 0, back = 0;
  fresh(9);
  CHECK_EQ(place_at(1, JK_FLOOR, JM_SITS, 120, ""), 0);
  J.placed[0].part = 2;                                /* its frame 1 is the bird */
  CHECK_EQ(js_act(&J, 0, JA_FLY, 0), 0);
  for (i = 0; i < 60 * JS_HZ; i++) {
    js_step_item(&J, 0);
    if (J.placed[0].pst == 1 && js_abs(J.placed[0].px / 16 - 120) > 30) away = 1;
    if (away && J.placed[0].pst == 0) { back = 1; break; }
  }
  CHECK(away);
  CHECK(back);
  J.placed[0].part = 0;
  CHECK_EQ(js_act(&J, 0, JA_FLY, 0), -1);              /* no part, no flight */
}

void test_jarsim_world_events_come_and_items_react(void) {
  int i, seen[JWD_KINDS] = { 0 }, kinds = 0;
  fresh(10);
  CHECK_EQ(place_at(1, JK_FLOOR, JM_SITS, 60, ""), 0);
  J.placed[0].hab[0].event = JE_WORLD; J.placed[0].hab[0].earg = JWD_DARK;
  J.placed[0].hab[0].action = JA_GLOW; J.placed[0].hab[0].aarg = 1;
  J.placed[0].nhab = 1;
  J.sulk_clock = J.mould_clock = 1 << 30;              /* no chores in this one */
  for (i = 0; i < 12 * 3600 * JS_HZ; i++) {
    int was = J.world;
    js_step(&J);
    if (J.world && J.world != was) seen[J.world]++;
  }
  for (i = 1; i < JWD_KINDS; i++) kinds += seen[i] > 0;
  CHECK(kinds >= 4);                                   /* twelve hours: most of them */
  CHECK(seen[JWD_DARK] == 0 || J.placed[0].glow == 1);
  CHECK_EQ(J.coins, J.shipped * JS_JAR_VALUE);         /* still: every coin a jar that left */
}

void test_jarsim_ants_take_berries_unless_scattered(void) {
  int i, k;
  fresh(11);
  for (k = 0; k < J.nbeds; k++) { J.bed[k].ready = 1; J.bed[k].grow = JS_GROW_MS; }
  J.nmoss = 0;
  js_world_begin(&J, JWD_ANTS);
  for (i = 0; i < 2 * 60 * JS_HZ && J.world; i++) js_step(&J);
  for (k = 0; k < J.nbeds; k++) CHECK_EQ(J.bed[k].ready, 0);   /* they got them */
  fresh(11);
  for (k = 0; k < J.nbeds; k++) { J.bed[k].ready = 1; J.bed[k].grow = JS_GROW_MS; }
  J.nmoss = 0;
  CHECK_EQ(place_at(1, JK_CRITTER, JM_SITS, 20, ""), 0);
  js_world_begin(&J, JWD_ANTS);
  for (i = 0; i < 10; i++) js_step(&J);
  js_act(&J, 0, JA_SHAKE, 0);                          /* routed, before the first one is there */
  for (k = 0; k < JS_ANTS; k++) CHECK(J.ant_st[k] == 0 || J.ant_st[k] == 3);
  for (i = 0; i < 60 * JS_HZ && J.world; i++) js_step(&J);
  CHECK_EQ(J.world, 0);
  for (k = 0; k < J.nbeds; k++) CHECK_EQ(J.bed[k].ready, 1);   /* none taken */
}

void test_jarsim_chores_stop_the_jar_until_the_player_comes(void) {
  int i;
  uint32_t shipped;
  fresh(12);
  J.world_clock = 1 << 30;
  J.sulk_clock = 1;
  js_step(&J);
  CHECK(js_chores(&J) & JC_SULK);
  J.dock = 10;
  shipped = J.shipped;
  for (i = 0; i < 5 * 60 * JS_HZ; i++) js_step(&J);
  CHECK_EQ(J.shipped, shipped);                        /* a sulking snail ships nothing */
  CHECK_EQ(js_fix(&J, JC_SULK), 0);
  CHECK_EQ(js_chores(&J) & JC_SULK, 0);
  for (i = 0; i < 5 * 60 * JS_HZ; i++) js_step(&J);
  CHECK(J.shipped > shipped);
  /* mould stops a bush; the puddle stops them all; the player clears both */
  J.bed[0].mould = 1;
  J.bed[0].ready = 0;
  J.bed[0].grow = 0;
  for (i = 0; i < 60 * JS_HZ; i++) js_step(&J);
  CHECK_EQ(J.bed[0].grow, 0);
  CHECK(js_chores(&J) & JC_MOULD);
  J.puddle = 30;
  CHECK(js_chores(&J) & JC_PUDDLE);
  CHECK_EQ(js_fix(&J, JC_MOULD | JC_PUDDLE), 0);
  CHECK_EQ(js_chores(&J), 0);
  CHECK_EQ(js_fix(&J, JC_MOULD), -1);                  /* nothing left to do */
  /* an item can cheer the snail, too */
  J.sulk = 1;
  CHECK_EQ(place_at(1, JK_FLOOR, JM_SITS, 60, ""), 0);
  CHECK_EQ(js_act(&J, 0, JA_NUDGE, 0), 0);
  CHECK_EQ(J.sulk, 0);
}
