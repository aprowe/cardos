/* Jar Factory's world, apart from the screen: the factory in the jar, its
 * coins, its upgrades, the items placed in it and what they do. Header-only
 * and libc-free, like apps/petsim.h, so apps/jar.c and the host tests
 * (test/test_jarsim.c) share one copy. The design is
 * docs/superpowers/specs/2026-10-09-jar-factory-design.md.
 *
 * THE LOOP. A plant grows a berry on a timer. A free mossling walks over,
 * picks it up and drops it into the thimble vat. Each machine holds a unit
 * for its share of the work and puts it on the bottle-cap belt to the next;
 * the twig crane, last, lifts a finished jar onto the dock. The snail takes
 * up to three at a time out through the cork door, and the coins are added
 * at the moment it leaves the screen -- js_ship is the only place coins come
 * from, so every coin is a jar someone could have watched go. Nothing is
 * made while the app is closed: there are no away earnings (js_away).
 *
 * THE RATE. js_rate_ph is what the factory can do an hour: the least of what
 * the beds grow, the mosslings carry and the machines turn out, less a fifth
 * for naps and jams. It is the number on the top bar.
 *
 * TIME. One step is JS_STEP_MS of jar time; positions are 1/64 pixel. An
 * item's tick, the event, is every JS_ITEM_TICK steps -- four a second.
 * Nothing here reads a clock: the app hands in the minute of the day (or -1
 * when there is no clock) and the epoch, for plants and senses.
 *
 * ITEMS. A placed item keeps what it needs to draw and behave; its frames sit
 * in a shared pool, so only placed items' pictures are in memory. Every habit
 * goes through js_item_event and js_act, which are what phase 2's bytecode
 * will call, with js_sense for what it can read.
 */
#ifndef CARDOS_JARSIM_H
#define CARDOS_JARSIM_H

#include <stdint.h>
#include "apps/jaritem.h"
#include "apps/jarvm.h"

#if defined(__GNUC__)
#define JS_OPT __attribute__((unused))
/* For the small things called from everywhere: -Os inlined js_rr at every
 * one of its thirty-odd calls, and jar.capp has a size budget. */
#define JS_SMALL __attribute__((unused, noinline))
#else
#define JS_OPT
#define JS_SMALL
#endif

/* ---- the numbers (placeholders from the spec, tuned to the scene) -------- */

#define JS_STEP_MS     25
#define JS_HZ          40              /* steps a second */
#define JS_FX          64              /* sub-pixels a pixel */
#define JS_ITEM_TICK   10              /* steps between item ticks: 4 a second */

#define JS_SOIL        112             /* where feet stand */
#define JS_LID         10              /* where hanging things hang from */
#define JS_HANG_LO     22              /* ... and how far along the lid */
#define JS_HANG_HI     218

#define JS_MAX_BEDS    6
#define JS_MAX_MOSS    6
#define JS_MAX_MACH    4
#define JS_MAX_BELT    3
#define JS_UNITS       8
#define JS_PARTS       24
#define JS_FLOATS      4
#define JS_FLIES       8
#define JS_MAX_PLACED  24
#define JS_POOL        32              /* placed items' frames, 96 bytes each */
#define JS_MAX_OWNED   64
#define JS_SPOOL       768             /* placed items' scripts, packed */

/* An app that keeps the jar but never runs it (the companion apps: they load
 * the save, change coins and items, save it again) defines JS_KEEP_ONLY and
 * goes without room for placed items and their pictures -- 6.6 KB of data
 * it would never touch. js_place refuses everything there. */
#ifdef JS_KEEP_ONLY
#define JS_PLACED_ROOM 1
#define JS_POOL_ROOM   1
#define JS_SPOOL_ROOM  1
#else
#define JS_PLACED_ROOM JS_MAX_PLACED
#define JS_POOL_ROOM   JS_POOL
#define JS_SPOOL_ROOM  JS_SPOOL
#endif

#define JS_GROW_MS     32000           /* a mature plant's berry, every so often */
#define JS_WORK_MS     30000           /* a jar's machine time, shared out */
#define JS_JAR_VALUE   1

#define JS_MOSS_SPD    18              /* sub-pixels a step: 11 px/s */
#define JS_SNAIL_SPD   10
#define JS_SNAIL_FAST  40
#define JS_PICK_STEPS  20
#define JS_BOOST_STEPS (60 * JS_HZ)    /* a liked item: +20% for a minute */
#define JS_SAY_STEPS   (2 * JS_HZ)

/* Where things are, in pixels. */
#define JS_BED_X(i)    (11 + 12 * (i))
#define JS_POND_X      86
#define JS_DROP_X      94              /* a mossling stands here to fill the vat */
#define JS_PILE_X      188             /* the dock's pile of jars, its middle */
#define JS_HOME_X      214             /* the snail at home, by the crates */
#define JS_NAP_X       212             /* the top crate */
#define JS_NAP_H       22
#define JS_DOOR_X      232
#define JS_EXIT_X      248             /* the snail's middle, once it is off the screen */

/* Levels (2026-10-10: "the ground will become quite crowded"). Level 0 is
 * the soil; the others are ledges at the back of the jar, made of junk like
 * the factory: 1 a wooden ruler on a stack of bottle caps and three cotton
 * reels, over the garden; 2 a lolly stick on a tower of matchboxes behind
 * the pond, its far end hung from the lid on a thread. Floor decor can be
 * put on a ledge, and critters that walk or hop climb up at UP (the caps,
 * the matchboxes), potter along, sit, and climb down again. H is the top,
 * in pixels above the soil; LO..HI is where an item's middle may be. */
#define JS_LEVELS      3
static JS_OPT const int16_t JS_LEDGE_H[JS_LEVELS]  = { 0, 34, 51 };
static JS_OPT const int16_t JS_LEDGE_LO[JS_LEVELS] = { 10, 22, 90 };
static JS_OPT const int16_t JS_LEDGE_HI[JS_LEVELS] = { 226, 66, 140 };
static JS_OPT const int16_t JS_LEDGE_UP[JS_LEVELS] = { 0, 23, 91 };

/* A climb, a step at a time: walking to the foot (or the top) of the
 * ledge's climb, then up or down it. */
enum { JC_NONE = 0, JC_TO_UP, JC_UP, JC_TO_DOWN, JC_DOWN };

/* The machines, in order along the belt: vat, press, spool, crane. */
static const int16_t JS_MACH_X[JS_MAX_MACH] = { 108, 132, 154, 170 };

/* What the critters like (an item's tags, as bits). */
/* Traits: what an item is, from its record's tag field ("shiny,noisy"), as
 * bits -- the same numbers as apps/jarvm.def's trait names, which scripts
 * read with has(). Older items' words map onto them (js_tags). */
enum { JT_FOOD = 0, JT_SHINY, JT_FRAGILE, JT_NOISY, JT_COSY, JT_LIGHT, JT_WET, JT_MUSIC,
       JT_WILD, JT_SWEET, JT_SPOOKY, JT_SOFT, JT_BIG, JT_CLOCKWORK, JT_PLANT, JT_SLEEPY,
       JT_KINDS };
#define JT_BIT(t)      ((uint16_t)(1u << (t)))
#define JS_MOSS_LIKES  (JT_BIT(JT_COSY) | JT_BIT(JT_SOFT) | JT_BIT(JT_SWEET) | JT_BIT(JT_LIGHT))
#define JS_SNAIL_LIKES (JT_BIT(JT_SLEEPY) | JT_BIT(JT_SPOOKY) | JT_BIT(JT_FOOD) | JT_BIT(JT_PLANT))

/* World events (apps/jarvm.def's world names), chores, sounds. */
enum { JWD_NONE = 0, JWD_ANTS, JWD_LEAK, JWD_BREEZE, JWD_VISITOR, JWD_DARK, JWD_BLOOM,
       JWD_KINDS };
#define JC_SULK    1                   /* the snail will not go out */
#define JC_MOULD   2                   /* a bush has mould: it does not grow */
#define JC_PUDDLE  4                   /* the leak's puddle: no bush grows, the belt crawls */
enum { JSD_NONE = 0, JSD_POP, JSD_CHIRP, JSD_BOING, JSD_DING, JSD_HORN, JSD_WHOOSH, JSD_CRUNCH,
       JSD_PLOP, JSD_COIN, JSD_FIZZ, JSD_CLOCK, JSD_SPLASH, JSD_BANG, JSD_TUNE };
#define JSD_KINDS  JSD_TUNE            /* sounds 1..JSD_KINDS (jarvm.def's sound names) */
#define JS_ANTS    5
#define JS_PELLETS 4

enum { JU_MOSS = 0, JU_MACH, JU_BELT, JU_BED, JU_KINDS };
enum { PH_NONE = 0, PH_DAWN, PH_DAY, PH_DUSK, PH_NIGHT };
enum { JWX_NONE = 0, JWX_SUNNY, JWX_CLOUDY, JWX_FOGGY, JWX_RAINY, JWX_SNOWY, JWX_STORMY };
enum { JSN_X = 0, JSN_ZONE, JSN_NEAR_KIND, JSN_NEAR_DIST, JSN_TIME, JSN_WEATHER,
       JSN_DAYS, JSN_GIFT, JSN_RANDOM, JSN_HOUR, JSN_BERRIES, JSN_FED, JSN_WORLD, JSN_TRAITS,
       JSN_NEAR_TRAITS, JSN_NEW_TRAITS, JSN_MUSIC, JSN_JAMMED, JSN_SULKING, JSN_KINDS };

enum { M_IDLE = 0, M_TO_BED, M_PICK, M_TO_VAT, M_TO_NAP, M_NAP, M_TO_FIX, M_FIX };
enum { S_HOME = 0, S_OUT, S_AWAY, S_BACK, S_TO_FIX, S_FIX };

/* Who said what: 0..5 the mosslings, 6 the snail, 8.. a placed item. */
#define JW_SNAIL 6
#define JW_ITEM  8

static const char *const JS_SAYINGS[] = {
  "hi!", "busy busy", "nice jam", "ooh", "hello", "jam time", "sticky!",
  "yum", "phew", "heave ho", "uh oh", "fixed!", "sigh...", "yay!", "ants!",
};
#define JS_NSAY 10                     /* the first ten are small talk */
#define JS_SAY_UHOH 10
#define JS_SAY_FIXED 11
#define JS_SAY_SIGH  12
#define JS_SAY_YAY   13
#define JS_SAY_ANTS  14

/* ---- the state --------------------------------------------------------- */

/* A bed. `type` is what grows in it (JPL_*); `at` is when it was planted, in
 * UTC seconds -- 0 for a plant that is grown (the starting beds, and every
 * bed before the garden existed), 1 for one planted with no clock, which
 * starts counting when a clock arrives. A young plant grows no berries. */
typedef struct { int32_t grow; uint32_t at; uint8_t ready, claimed, type, young, mould; } JBed;

/* The garden (spec step 4): what can be planted, in the order the scene's
 * plant pictures are in. Each steers the daily stock its own way (the
 * companion app says how). */
enum { JPL_BERRY = 0, JPL_FERN, JPL_SHROOM, JPL_FLOWER, JPL_CACTUS, JPL_KINDS };
#define JS_MATURE_S    (3u * 86400u)   /* three real days to grow */
#define JS_SHELF       4
static JS_OPT const char *const JS_PLANT_NAME[JPL_KINDS] = { "berry", "fern", "mushroom", "flower", "cactus" };
static JS_OPT const uint16_t JS_PLANT_COST[JPL_KINDS] = { 20, 20, 35, 35, 40 };

typedef struct {
  int32_t x, tx;                       /* middle, sub-pixels */
  int16_t yoff;                        /* pixels above the soil */
  int16_t t, boost, say_t, idle;
  uint8_t st, carry, face, frame;
  int8_t  bed, say;
} JMoss;

typedef struct { uint8_t st; int16_t t; } JMach;  /* st: 0 empty, 1 working, 2 holding */
typedef struct { int32_t x; int8_t to; } JUnit;   /* to < 0: unused */

typedef struct {
  int32_t x;
  uint32_t load;
  int16_t t, boost, say_t;
  uint8_t st, face, frame, fast;
  int8_t say;
} JSnail;

typedef struct { int16_t x, y; int8_t vx, vy; uint8_t type, life; } JPart; /* 1/16 px */
typedef struct { int16_t x, y; uint16_t amount; uint8_t life; } JFloat;

typedef struct {
  uint32_t id;
  uint8_t  kind, move, speed, zone, nhab, nframes, nbub, flags;
  JHabit   hab[JI_MAX_HAB];
  char     bub[JI_MAX_BUB + 1][JI_BUB + 1];  /* the last is "?": a script stopped */
  int16_t  mem[JI_MEM];
  uint16_t pal[8];                     /* plain RGB565, as in the record */
  uint16_t tags;                       /* JT_* */
  uint8_t  slot[JI_MAX_FRAMES];        /* frames in the pool; 255 none */
  uint32_t made;
  int16_t  home_x, home_y;             /* where it was put: middle x, and the
                                        * string's length for a hanging one */
  /* behaviour */
  int32_t  x, tx;                      /* sub-pixels */
  int16_t  yoff, vy;                   /* 1/16 px above its line */
  int16_t  t, wait, say_t, phase, near_cd;
  uint8_t  cur, flip, glow, moving;
  int8_t   say;
  /* phase 2: mem changed since the item's record was last written
   * (js_mem_sync); its script in j->spool, 0 bytes for none */
  uint8_t  mem_dirty;
  uint16_t soff, slen;
  /* levels: where it stands (and is saved), a climb under way and where
   * to; lift is how high its feet are, px above the soil. Packed into
   * what was padding: 24 of these is a lot of an app's 28 KB. */
  uint8_t  level, climb, goal;
  int8_t   lift;
  /* the living jar: berries eaten; flung (vx, sub-pixels a step, while in
   * the air); and its part -- a frame of its own that leaves home and comes
   * back (a bird from its birdhouse). part is the frame + 1, 0 none. */
  uint8_t  fed, part, pst;             /* pst: 0 home, 1 out, 2 coming back */
  int16_t  vx;
  /* what it is (the record's longer header): JR_*, 1 or 2 times the size,
   * furniture's top row; and the furniture it stands on, by id (0 none) */
  uint8_t  role, scale, surface, voice;
  int8_t   pdx, pdy;
  uint32_t on_id;
  int16_t  on_lo, on_hi;               /* that furniture's top, px */
  int16_t  px, py, ptx, pty, pt;       /* the part, 1/16 px; its target; a timer */
} JPlaced;

typedef struct { uint32_t id, on; int16_t x; int8_t y; uint8_t lv; } JWant;

typedef struct {
  /* kept on the card */
  uint32_t coins, shipped, dock, seen, sold;
  uint8_t  nbeds, nmoss, nmach, belt;
  uint16_t nowned;
  uint32_t owned[JS_MAX_OWNED];
  uint32_t shelf[JS_SHELF];            /* item ids, 0 empty */
  uint16_t parcels;                    /* gifts collected, not yet opened */
  uint32_t decor;                      /* an item the companion asked to place */
  uint32_t gseen;                      /* the last gift the jar announced */

  /* the factory */
  JBed   bed[JS_MAX_BEDS];
  JMoss  moss[JS_MAX_MOSS];
  JMach  mach[JS_MAX_MACH];
  JUnit  unit[JS_UNITS];
  JSnail snail;
  uint32_t pile0;                      /* how big the pile was when it began */
  int32_t belt_pos;                    /* how far the caps have turned */
  int32_t jam_clock;                   /* steps to the next jam */
  int16_t jam_x, fix_t;
  int8_t  fixer[2];                    /* a mossling, JW_SNAIL, or -1 */
  uint8_t jammed;
  int32_t nap_clock;

  /* the items */
  JPlaced placed[JS_PLACED_ROOM];
  uint8_t nplaced;
  uint8_t pool[JS_POOL_ROOM][JI_FRAME_BYTES];
  uint8_t pool_used[JS_POOL_ROOM];
  JWant   want[JS_MAX_PLACED];         /* what the save said was placed */
  uint8_t nwant;
  uint8_t spool[JS_SPOOL_ROOM];        /* the placed items' scripts, end to end */
  uint16_t sused;

  /* the air */
  JPart  part[JS_PARTS];
  JFloat fl[JS_FLOATS];
  int16_t fly_x[JS_FLIES], fly_y[JS_FLIES]; /* 1/16 px */
  int8_t  fly_vx[JS_FLIES], fly_vy[JS_FLIES];
  int16_t chat_cd, reply_t;
  int8_t  reply_who;

  int16_t  minute;                     /* of the day, -1 unknown */
  uint8_t  phase;                      /* PH_* */
  /* the living jar */
  uint8_t  world;                      /* JWD_* going on now */
  uint8_t  sulk, puddle;               /* chores: the snail sulking, the leak's puddle 0..60 */
  uint8_t  snd;                        /* a sound an item asked for, 1..: the app plays and clears it */
  uint8_t  music;                      /* the app's soundtrack is on */
  uint8_t  hour1;                      /* the hour + 1 last told, 0 unknown */
  uint8_t  shake_t, sig_n;
  int8_t   sig_from;
  uint16_t newtraits;
  int16_t  belt_boost, boost_cd;
  int32_t  world_t, world_clock, sulk_clock, mould_clock;
  int16_t  ant_x[JS_ANTS];             /* 1/16 px */
  uint8_t  ant_st[JS_ANTS], ant_bed[JS_ANTS];   /* 0 none, 1 marching in, 2 carrying off, 3 fleeing */
  int16_t  pel_x[JS_PELLETS];
  uint16_t pel_t[JS_PELLETS];          /* steps left; 0 none */
  uint8_t  weather;                    /* JWX_* (apps/jarvm.def), 0 unknown */
  uint32_t steps, seed, epoch;         /* epoch: now, for senses; 0 unknown */
} Jar;

/* ---- small things ------------------------------------------------------ */

static JS_SMALL uint32_t js_rnd(Jar *j) {
  uint32_t x = j->seed ? j->seed : 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  j->seed = x;
  return x;
}

/* lo..hi inclusive. */
static JS_SMALL int js_rr(Jar *j, int lo, int hi) {
  return lo + (int)(js_rnd(j) % (uint32_t)(hi - lo + 1));
}

static JS_OPT int js_abs(int v) { return v < 0 ? -v : v; }
static JS_OPT int js_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static JS_OPT int js_isqrt(int v) {
  int r = 0;
  while ((r + 1) * (r + 1) <= v) r++;
  return r;
}

/* Walk *x towards tx at spd sub-pixels a step. 1 when there. */
static JS_SMALL int js_walk(int32_t *x, int32_t tx, int spd, uint8_t *face) {
  int32_t d = tx - *x;
  if (d == 0) return 1;
  if (face) *face = d < 0;
  if (d > spd) d = spd;
  if (d < -spd) d = -spd;
  *x += d;
  return *x == tx;
}

static JS_OPT int js_mach_active(const Jar *j, int i) {
  return i == 0 || i == 3 || (i == 1 && j->nmach >= 3) || (i == 2 && j->nmach >= 4);
}

static JS_OPT int js_next_mach(const Jar *j, int i) {
  for (i++; i < JS_MAX_MACH; i++) if (js_mach_active(j, i)) return i;
  return -1;
}

static JS_OPT const int JS_BELT_PCT[JS_MAX_BELT + 1] = { 100, 82, 68, 56 };
static JS_OPT const int JS_BELT_SPD[JS_MAX_BELT + 1] = { 10, 13, 16, 20 };

/* One machine's share of a jar, in steps. */
static JS_SMALL int js_stage_steps(int nmach, int belt) {
  return JS_WORK_MS / JS_STEP_MS / nmach * JS_BELT_PCT[belt] / 100;
}

/* Jars an hour, for these upgrades: the least of the beds, the carrying and
 * the machines, less a fifth for naps and jams. */
static JS_OPT int32_t js_rate_for(int nbeds, int nmoss, int nmach, int belt) {
  int32_t beds = (int32_t)nbeds * (3600000 / JS_GROW_MS);
  int32_t mach = 3600000 / (js_stage_steps(nmach, belt) * JS_STEP_MS);
  int32_t trip = 0, carry, best;
  int i;
  for (i = 0; i < nbeds; i++) {
    int d = JS_DROP_X - JS_BED_X(i);
    trip += 2 * d * JS_FX * 1000 / (JS_MOSS_SPD * JS_HZ) + 2 * JS_PICK_STEPS * JS_STEP_MS + 600;
  }
  trip /= nbeds;
  carry = (int32_t)nmoss * 3600000 / trip;
  best = beds < carry ? beds : carry;
  if (mach < best) best = mach;
  return best * 80 / 100;
}

/* Only grown plants count: a bed replanted yesterday grows nothing yet. */
static JS_OPT int32_t js_rate_ph(const Jar *j) {
  int i, n = 0;
  for (i = 0; i < j->nbeds; i++) if (!j->bed[i].young) n++;
  return n ? js_rate_for(n, j->nmoss, j->nmach, j->belt) : 0;
}

/* ---- the garden ---------------------------------------------------------- */

/* Is the plant in `b` grown, at `now` (UTC seconds, 0 without a clock)? With
 * no clock nothing grows: a young plant stays young. */
static JS_OPT int js_bed_grown(const JBed *b, uint32_t now) {
  if (b->at == 0) return 1;
  if (b->at == 1 || !now || now < b->at) return 0;
  return now - b->at >= JS_MATURE_S;
}

/* Whole days until it is grown, rounded up; 0 when it is. -1 without a
 * clock. */
static JS_OPT int js_bed_days(const JBed *b, uint32_t now) {
  uint32_t left;
  if (js_bed_grown(b, now)) return 0;
  if (b->at == 1 || !now) return -1;
  left = now < b->at ? JS_MATURE_S : JS_MATURE_S - (now - b->at);
  return (int)((left + 86399u) / 86400u);
}

/* Bring the beds up to `now`: a plant set with no clock starts counting, a
 * grown one is grown for good (at 0), and `young` says which grow nothing. */
static JS_OPT void js_settle_beds(Jar *j, uint32_t now) {
  int i;
  for (i = 0; i < JS_MAX_BEDS; i++) {
    JBed *b = &j->bed[i];
    if (b->at == 1 && now) b->at = now;
    if (js_bed_grown(b, now)) b->at = 0;
    b->young = (uint8_t)(b->at != 0);
    if (b->young) { b->ready = 0; b->grow = 0; }
  }
}

/* Plant `type` in bed i (paid for by the caller). It starts young. */
static JS_OPT void js_plant(Jar *j, int i, int type, uint32_t now) {
  JBed *b = &j->bed[i];
  b->type = (uint8_t)(type % JPL_KINDS);
  b->at = now ? now : 1;
  b->young = 1;
  b->ready = 0;
  b->claimed = 0;
  b->grow = 0;
}

/* Grown beds of each kind, into n[JPL_KINDS]; the total. */
static JS_OPT int js_garden_mix(const Jar *j, int *n) {
  int i, t = 0;
  for (i = 0; i < JPL_KINDS; i++) n[i] = 0;
  for (i = 0; i < j->nbeds; i++)
    if (!j->bed[i].young) { n[j->bed[i].type % JPL_KINDS]++; t++; }
  return t;
}

/* ---- particles, floats, bubbles ------------------------------------------ */

static JS_OPT int js_particle(Jar *j, int type, int x, int y) {
  int i, best = 0;
  for (i = 0; i < JS_PARTS; i++) {
    if (!j->part[i].life) { best = i; break; }
    if (j->part[i].life < j->part[best].life) best = i;
  }
  j->part[best].x = (int16_t)(x * 16);
  j->part[best].y = (int16_t)(y * 16);
  j->part[best].type = (uint8_t)type;
  j->part[best].vx = (int8_t)(type == JP_PUFF ? js_rr(j, -3, 3) : type == JP_ZZZ ? 3 : js_rr(j, -2, 2));
  j->part[best].vy = (int8_t)(type == JP_PUFF ? -6 : -5);
  j->part[best].life = (uint8_t)(type == JP_PUFF ? 50 : 60);
  return best;
}

static JS_OPT void js_float(Jar *j, int x, int y, uint32_t amount) {
  int i, best = 0;
  for (i = 0; i < JS_FLOATS; i++) {
    if (!j->fl[i].life) { best = i; break; }
    if (j->fl[i].life < j->fl[best].life) best = i;
  }
  j->fl[best].x = (int16_t)x;
  j->fl[best].y = (int16_t)y;
  j->fl[best].amount = (uint16_t)(amount > 65535 ? 65535 : amount);
  j->fl[best].life = 60;
}

/* Someone says something: a saying for a worker, a bubble for an item. */
static JS_OPT void js_say(Jar *j, int who, int what) {
  if (who < JS_MAX_MOSS) { j->moss[who].say = (int8_t)what; j->moss[who].say_t = JS_SAY_STEPS; }
  else if (who == JW_SNAIL) { j->snail.say = (int8_t)what; j->snail.say_t = JS_SAY_STEPS; }
  else if (who >= JW_ITEM && who - JW_ITEM < j->nplaced) {
    JPlaced *p = &j->placed[who - JW_ITEM];
    if (what >= 0 && what < p->nbub) { p->say = (int8_t)what; p->say_t = JS_SAY_STEPS; }
  }
}

/* ---- items: placing them ------------------------------------------------- */

static JS_OPT int js_tag_word(const char *s, int n, const char *w) {
  int i;
  for (i = 0; i < n && w[i]; i++) if (s[i] != w[i]) return 0;
  return i == n && !w[i];
}

/* "shiny,noisy" -> JT_BIT(JT_SHINY) | JT_BIT(JT_NOISY): the trait words in
 * order, then the older tag words and what they mean now. */
static JS_OPT uint16_t js_tags(const char *s) {
  static const char *const W[] = { "food", "shiny", "fragile", "noisy", "cosy", "light",
                                   "wet", "music", "wild", "sweet", "spooky", "soft", "big",
                                   "clockwork", "plant", "sleepy",
                                   "glowing", "fancy", "round", "spiky", "odd" };
  static const uint8_t OLD[] = { JT_LIGHT, JT_SHINY, JT_SOFT, JT_WILD, JT_WILD };
  uint16_t m = 0;
  while (*s) {
    int n = 0, k;
    while (s[n] && s[n] != ',') n++;
    while (n && s[n - 1] == ' ') n--;
    for (k = 0; k < (int)(sizeof W / sizeof W[0]); k++)
      if (js_tag_word(s, n, W[k])) m |= JT_BIT(k < JT_KINDS ? k : OLD[k - JT_KINDS]);
    while (s[n] && s[n] != ',') n++;
    s += n;
    if (*s == ',') s++;
    while (*s == ' ') s++;
  }
  return m;
}

/* The x range an item keeps to: a decoration near where it was put, a
 * critter in its favourite zone -- or, up on a ledge, along the ledge. */
static JS_OPT void js_range(const JPlaced *p, int *lo, int *hi) {
  int lv = p->level;
  if (p->on_id) {                              /* on furniture: its top, and no further */
    *lo = p->on_lo;
    *hi = p->on_hi;
    if (p->kind != JK_CRITTER) {
      if (*lo < p->home_x - 12) *lo = p->home_x - 12;
      if (*hi > p->home_x + 12) *hi = p->home_x + 12;
    }
    return;
  }
  if (lv) {
    *lo = JS_LEDGE_LO[lv]; *hi = JS_LEDGE_HI[lv];
    if (p->kind != JK_CRITTER) {
      if (*lo < p->home_x - 12) *lo = p->home_x - 12;
      if (*hi > p->home_x + 12) *hi = p->home_x + 12;
    }
    return;
  }
  if (p->kind != JK_CRITTER) { *lo = p->home_x - 12; *hi = p->home_x + 12; }
  else switch (p->zone) {
    case JZ_GARDEN: *lo = 8;   *hi = 80;  break;
    case JZ_WORKS:  *lo = 100; *hi = 176; break;
    case JZ_DOCK:   *lo = 180; *hi = 224; break;
    case JZ_WATER:  *lo = 78;  *hi = 96;  break;
    default:        *lo = 10;  *hi = 226; break;
  }
  if (*lo < 10) *lo = 10;
  if (*hi > 226) *hi = 226;
}

static JS_SMALL int js_find(const Jar *j, uint32_t id) {
  int i;
  for (i = 0; i < j->nplaced; i++) if (j->placed[i].id == id) return i;
  return -1;
}

/* Put an item in the jar at (x, y): x its middle, y the string's length for
 * a hanging one. Its index, or -1: the jar is full (JS_MAX_PLACED) or it is
 * already in. Frames the pool cannot hold are left out; the item shows the
 * ones it got. */
static JS_OPT int js_place(Jar *j, const JItem *it, int x, int y) {
  JPlaced *p;
  int i, f;
  if (JS_PLACED_ROOM < JS_MAX_PLACED) return -1;          /* JS_KEEP_ONLY */
  if (j->nplaced >= JS_MAX_PLACED || js_find(j, it->id) >= 0) return -1;
  p = &j->placed[j->nplaced];
  ji_zero(p, (int)sizeof *p);
  p->id = it->id;
  p->kind = it->kind;
  p->move = it->move;
  p->speed = it->speed;
  p->zone = it->zone;
  p->nhab = it->nhab;
  p->nbub = it->nbub;
  p->flags = it->flags;
  p->made = it->made;
  ji_copy(p->hab, it->hab, (int)sizeof p->hab);
  for (i = 0; i < it->nbub; i++) ji_copy(p->bub[i], it->bub[i], JI_BUB + 1);
  p->bub[JI_MAX_BUB][0] = '?';
  for (i = 0; i < JI_MEM; i++) p->mem[i] = it->mem[i];
  for (i = 0; i < 8; i++) p->pal[i] = it->pal[i];
  p->tags = js_tags(it->tags);
  for (i = 0; i < JI_MAX_FRAMES; i++) p->slot[i] = 255;
  p->nframes = 0;
  for (i = 0; i < it->nframes; i++) {
    for (f = 0; f < JS_POOL_ROOM && j->pool_used[f]; f++) {}
    if (f == JS_POOL_ROOM) break;
    j->pool_used[f] = 1;
    ji_copy(j->pool[f], it->frames[i], JI_FRAME_BYTES);
    p->slot[i] = (uint8_t)f;
    p->nframes++;
  }
  if (!p->nframes) return -1;
  p->role = it->role < JR_KINDS ? it->role : JR_THING;
  p->scale = it->scale == 2 ? 2 : 1;
  p->surface = it->surface;
  p->voice = it->voice;
  p->pdx = it->pdx;
  p->pdy = it->pdy;
  if (p->role == JR_BACKGROUND) { p->kind = JK_FLOOR; p->move = JM_SITS; }   /* scenery */
  if (p->role == JR_FURNITURE && p->kind == JK_CRITTER) p->kind = JK_FLOOR;
  if (p->scale == 2) p->tags |= JT_BIT(JT_BIG);
  if (it->part && p->nframes >= 2) {           /* the last frame is the part, not a pose */
    p->part = 1;
    p->nframes--;
    p->pt = (int16_t)js_rr(j, 10 * JS_HZ, 40 * JS_HZ);
  }
  /* Its script, if this machine can run it and there is room: else the
   * recipe does everything, as in phase 1. */
  if (it->script_len && jv_check(it->script, it->script_len) == 0 &&
      j->sused + it->script_len <= JS_SPOOL_ROOM) {
    p->soff = j->sused;
    p->slen = it->script_len;
    ji_copy(j->spool + p->soff, it->script, p->slen);
    j->sused = (uint16_t)(j->sused + p->slen);
  }
  x = js_clamp(x, 10, 226);
  if (p->kind == JK_HANGING) x = js_clamp(x, JS_HANG_LO, JS_HANG_HI);
  p->home_x = (int16_t)x;
  p->home_y = (int16_t)(p->kind == JK_HANGING ? js_clamp(y, 0, 60) :
                        p->kind == JK_CRITTER && p->move == JM_FLOATS ?
                        (p->zone == JZ_HIGH ? 64 : 26) : 0);
  p->x = p->tx = (int32_t)x * JS_FX;
  p->say = -1;
  p->t = (int16_t)js_rr(j, 20, 120);
  p->phase = (int16_t)js_rr(j, 0, 255);
  j->nplaced++;
  return j->nplaced - 1;
}

/* Take item i out of the jar (back to My Stuff), freeing its frames. */
static JS_OPT void js_get_off(Jar *j, int i);

static JS_OPT void js_unplace(Jar *j, int i) {
  int k;
  if (i < 0 || i >= j->nplaced) return;
  for (k = 0; k < j->nplaced; k++)             /* what stood on it comes down */
    if (k != i && j->placed[k].on_id && j->placed[k].on_id == j->placed[i].id) js_get_off(j, k);
  for (k = 0; k < JI_MAX_FRAMES; k++)
    if (j->placed[i].slot[k] < JS_POOL_ROOM) j->pool_used[j->placed[i].slot[k]] = 0;
  if (j->placed[i].slen) {                    /* close the gap its script leaves */
    int at = j->placed[i].soff, n = j->placed[i].slen;
    ji_copy(j->spool + at, j->spool + at + n, j->sused - at - n);
    j->sused = (uint16_t)(j->sused - n);
    for (k = 0; k < j->nplaced; k++)
      if (j->placed[k].slen && j->placed[k].soff > at) j->placed[k].soff = (uint16_t)(j->placed[k].soff - n);
  }
  for (k = i; k + 1 < j->nplaced; k++) ji_copy(&j->placed[k], &j->placed[k + 1], (int)sizeof j->placed[k]);
  j->nplaced--;
}

/* Furniture f's top: the span things stand on (px) and its height above
 * the soil. */
static JS_OPT void js_top(const Jar *j, int f, int *lo, int *hi, int *h) {
  const JPlaced *q = &j->placed[f];
  int half = 8 * q->scale;
  *lo = q->home_x - half + 3;
  *hi = q->home_x + half - 3;
  *h = q->lift + (16 - q->surface) * q->scale;
}

/* Stand item i on furniture f (an index): 0, or -1 if f is not furniture or
 * i cannot stand there (it hangs, flies, is furniture or scenery itself). */
static JS_OPT int js_put_on(Jar *j, int i, int f) {
  JPlaced *p;
  int lo, hi, h;
  if (i < 0 || i >= j->nplaced) return -1;
  p = &j->placed[i];
  if (f < 0 || f >= j->nplaced || f == i || j->placed[f].role != JR_FURNITURE) return -1;
  if (p->kind == JK_HANGING || p->role != JR_THING || (p->kind == JK_CRITTER && p->move == JM_FLOATS))
    return -1;
  js_top(j, f, &lo, &hi, &h);
  p->on_id = j->placed[f].id;
  p->on_lo = (int16_t)lo;
  p->on_hi = (int16_t)hi;
  p->home_x = (int16_t)js_clamp(p->home_x, lo, hi);
  p->x = p->tx = (int32_t)p->home_x * JS_FX;
  p->lift = (int8_t)h;
  p->level = j->placed[f].level;
  p->yoff = 0;
  p->vy = 0;
  p->vx = 0;
  p->moving = 0;
  p->climb = p->goal = 0;
  return 0;
}

/* After the app has placed what the save wanted: each back on its
 * furniture, if that is in the jar too. */
static JS_OPT void js_stand_wanted(Jar *j) {
  int w;
  for (w = 0; w < j->nwant; w++)
    if (j->want[w].on) js_put_on(j, js_find(j, j->want[w].id), js_find(j, j->want[w].on));
}

/* Item i steps off whatever furniture it is on, onto its level. */
static JS_OPT void js_get_off(Jar *j, int i) {
  JPlaced *p = &j->placed[i];
  if (!p->on_id) return;
  p->on_id = 0;
  p->lift = (int8_t)JS_LEDGE_H[p->level];
}

/* Furniture f moved by dx: what stands on it goes with it. */
static JS_OPT void js_carry(Jar *j, int f, int dx) {
  int k;
  for (k = 0; k < j->nplaced; k++)
    if (j->placed[k].on_id && j->placed[k].on_id == j->placed[f].id) {
      j->placed[k].home_x = (int16_t)(j->placed[k].home_x + dx);
      j->placed[k].on_lo = (int16_t)(j->placed[k].on_lo + dx);
      j->placed[k].on_hi = (int16_t)(j->placed[k].on_hi + dx);
      j->placed[k].x += (int32_t)dx * JS_FX;
      j->placed[k].tx = j->placed[k].x;
    }
}

/* Does it stand on something -- the soil or a ledge? Floor decor and the
 * critters that walk, hop or sit; not hanging things, not fliers. */
static JS_OPT int js_has_levels(const JPlaced *p) {
  return p->kind == JK_FLOOR || (p->kind == JK_CRITTER && p->move != JM_FLOATS);
}

/* Move a placed item somewhere else along its line, on its level. A climb
 * under way is called off: it is back on the level it set off from. */
static JS_OPT void js_move_to(Jar *j, int i, int x, int y) {
  JPlaced *p = &j->placed[i];
  int lv = p->level, was = p->home_x;
  if (p->on_id) {                              /* along its furniture's top */
    int f = js_find(j, p->on_id), lo, hi, h;
    if (f >= 0) {
      js_top(j, f, &lo, &hi, &h);
      p->home_x = (int16_t)js_clamp(x, lo, hi);
      p->x = p->tx = (int32_t)p->home_x * JS_FX;
      p->lift = (int8_t)h;
      return;
    }
    p->on_id = 0;
  }
  x = p->kind == JK_HANGING ? js_clamp(x, JS_HANG_LO, JS_HANG_HI) : js_clamp(x, JS_LEDGE_LO[lv], JS_LEDGE_HI[lv]);
  p->home_x = (int16_t)x;
  if (p->kind == JK_HANGING) p->home_y = (int16_t)js_clamp(y, 0, 60);
  p->x = p->tx = (int32_t)x * JS_FX;
  p->yoff = 0;
  p->vy = 0;
  p->moving = 0;
  p->lift = (int8_t)JS_LEDGE_H[lv];
  p->climb = p->goal = 0;
  if (p->role == JR_FURNITURE && p->home_x != was) js_carry(j, i, p->home_x - was);
}

/* Put item i on level lv: 0 the soil, 1.. a ledge (only what stands, see
 * js_has_levels; anything else stays at 0), keeping its x as far as the
 * ledge allows. The level it is on. */
static JS_OPT int js_set_level(Jar *j, int i, int lv) {
  JPlaced *p = &j->placed[i];
  lv = js_has_levels(p) ? js_clamp(lv, 0, JS_LEVELS - 1) : 0;
  p->level = (uint8_t)lv;
  js_move_to(j, i, p->home_x, p->home_y);
  return lv;
}

/* Can it climb by itself? The critters that walk or hop. */
static JS_OPT int js_can_climb(const JPlaced *p) {
  return p->kind == JK_CRITTER && (p->move == JM_HOPS || p->move == JM_WANDERS) && !p->on_id;
}

/* Item i sets off for level `to`: up a ledge from the soil, or down to the
 * soil; ledge to ledge goes down first. Nothing if it cannot climb, is
 * already climbing, or is there. */
static JS_OPT void js_trip(Jar *j, int i, int to) {
  JPlaced *p = &j->placed[i];
  if (!js_can_climb(p) || p->climb || to == p->level) return;
  p->goal = (uint8_t)to;
  p->climb = p->level ? JC_TO_DOWN : JC_TO_UP;
  p->moving = 1;
}

/* The ledge whose climb is nearer x. */
static JS_SMALL int js_near_ledge(int x) {
  return x < (JS_LEDGE_UP[1] + JS_LEDGE_UP[2]) / 2 ? 1 : 2;
}

/* ---- items: senses, actions, events -------------------------------------- */

static JS_OPT int js_item_y(const Jar *j, int i) {     /* its middle, px */
  const JPlaced *p = &j->placed[i];
  if (p->kind == JK_HANGING) return JS_LID + p->home_y + 8;
  return JS_SOIL - 8 - p->lift - p->home_y - p->yoff / 16;
}

static JS_OPT int js_zone_of(int x) {
  if (x < 78) return JZ_GARDEN;
  if (x < 98) return JZ_WATER;
  if (x < 178) return JZ_WORKS;
  return JZ_DOCK;
}

/* What is nearest item i: a JN_* kind, and its distance in pixels. */
static JS_OPT int js_nearest(const Jar *j, int i, int *dist) {
  int x = j->placed[i].x / JS_FX, y = js_item_y(j, i), best = 9999, kind = JN_ANY, k, d;
  for (k = 0; k < j->nmoss; k++) {
    d = js_abs(j->moss[k].x / JS_FX - x);
    d += js_abs(JS_SOIL - 5 - j->moss[k].yoff - y) / 2;
    if (d < best) { best = d; kind = JN_MOSS; }
  }
  if (j->snail.st != S_AWAY) {
    d = js_abs(j->snail.x / JS_FX - x) + js_abs(JS_SOIL - 6 - y) / 2;
    if (d < best) { best = d; kind = JN_SNAIL; }
  }
  for (k = 0; k < j->nplaced; k++) {
    if (k == i || j->placed[k].role == JR_BACKGROUND) continue;
    d = js_abs(j->placed[k].x / JS_FX - x) + js_abs(js_item_y(j, k) - y) / 2;
    if (d < best) { best = d; kind = j->placed[k].kind == JK_CRITTER ? JN_CRITTER : JN_DECOR; }
  }
  if (dist) *dist = best;
  return kind;
}

/* The placed item nearest item i, or -1; its distance in *dist. */
static JS_OPT int js_nearest_item(const Jar *j, int i, int *dist) {
  int x = j->placed[i].x / JS_FX, y = js_item_y(j, i), best = 9999, at = -1, k, d;
  for (k = 0; k < j->nplaced; k++) {
    if (k == i || j->placed[k].role == JR_BACKGROUND) continue;
    d = js_abs(j->placed[k].x / JS_FX - x) + js_abs(js_item_y(j, k) - y) / 2;
    if (d < best) { best = d; at = k; }
  }
  if (dist) *dist = best;
  return at;
}

/* What item i can read (spec, "Senses"). Phase 2's scripts read these. */
static JS_OPT int js_sense(Jar *j, int i, int sense) {
  const JPlaced *p = &j->placed[i];
  int d;
  switch (sense) {
  case JSN_X:         return p->x / JS_FX;
  case JSN_ZONE:      return p->level ? JZ_HIGH : js_zone_of(p->x / JS_FX);
  case JSN_NEAR_KIND: return js_nearest(j, i, 0);
  case JSN_NEAR_DIST: js_nearest(j, i, &d); return d;
  case JSN_TIME:      return j->phase;
  case JSN_WEATHER:   return j->weather;              /* JWX_*, 0 not known */
  case JSN_DAYS:      return (j->epoch && p->made && j->epoch > p->made)
                             ? (int)((j->epoch - p->made) / 86400u) : 0;
  case JSN_GIFT:      return (p->flags & JIF_GIFT) ? 1 : 0;
  case JSN_RANDOM:    return (int)(js_rnd(j) & 255);
  case JSN_HOUR:      return j->hour1 ? j->hour1 - 1 : -1;
  case JSN_BERRIES: {
    int k, n = 0;
    for (k = 0; k < j->nbeds; k++) n += j->bed[k].ready;
    return n;
  }
  case JSN_FED:       return p->fed;
  case JSN_WORLD:     return j->world;
  case JSN_TRAITS:    return (int16_t)p->tags;        /* bit 15 too: has() reads it */
  case JSN_NEAR_TRAITS: {
    int k = js_nearest_item(j, i, 0);
    return k >= 0 ? (int16_t)j->placed[k].tags : 0;
  }
  case JSN_NEW_TRAITS: return (int16_t)j->newtraits;
  case JSN_MUSIC:     return j->music;
  case JSN_JAMMED:    return j->jammed;
  case JSN_SULKING:   return j->sulk;
  default:            return 0;
  }
}

static JS_OPT int js_speed(const JPlaced *p) {
  return p->speed == JSP_SLOW ? 5 : p->speed == JSP_FAST ? 19 : 10;
}

/* Ants scatter: those not already gone run off, dropping what they carry. */
static JS_OPT void js_scatter_ants(Jar *j, int x, int reach) {
  int k;
  for (k = 0; k < JS_ANTS; k++)
    if (j->ant_st[k] && j->ant_st[k] != 3 && (reach < 0 || js_abs(j->ant_x[k] / 16 - x) <= reach)) {
      if (j->ant_st[k] == 2 && j->ant_bed[k] < j->nbeds) j->bed[j->ant_bed[k]].ready = 1;  /* dropped */
      j->ant_st[k] = 3;
    }
}

/* Item i flings the nearest small thing it can reach: an ant, else a placed
 * critter or floor thing (not a big one) -- up and away, to land and bump. */
static JS_OPT int js_throw(Jar *j, int i) {
  JPlaced *p = &j->placed[i];
  int x = p->x / JS_FX, k, best = -1, bd = 33;
  if (j->world == JWD_ANTS) {
    for (k = 0; k < JS_ANTS; k++)
      if (j->ant_st[k] && j->ant_st[k] != 3 && js_abs(j->ant_x[k] / 16 - x) <= 32) {
        js_scatter_ants(j, x, 32);
        return 0;
      }
  }
  for (k = 0; k < j->nplaced; k++) {
    const JPlaced *q = &j->placed[k];
    int d;
    if (k == i || q->kind == JK_HANGING || (q->tags & JT_BIT(JT_BIG)) || q->level != p->level ||
        q->yoff || q->vy || q->role != JR_THING)
      continue;
    d = js_abs(q->x / JS_FX - x);
    if (d < bd) { bd = d; best = k; }
  }
  if (best < 0) return -1;
  {
    JPlaced *q = &j->placed[best];
    int dir = q->x >= p->x ? 1 : -1;
    js_get_off(j, best);
    q->vy = (int16_t)js_isqrt(2 * 6 * 22 * 16);       /* about 22 px up */
    q->vx = (int16_t)(dir * js_rr(j, 60, 110));       /* and 1 to 2 px a step along */
    q->moving = 0;
    js_particle(j, JP_PUFF, q->x / JS_FX, js_item_y(j, best) + 6);
  }
  return 0;
}

/* The jar shakes: everything on the ground hops, ants scatter. */
static JS_OPT void js_shake(Jar *j) {
  int k;
  j->shake_t = 12;
  for (k = 0; k < j->nplaced; k++) {
    JPlaced *q = &j->placed[k];
    if (q->kind != JK_HANGING && !q->yoff && !q->vy) q->vy = (int16_t)js_isqrt(2 * 6 * 4 * 16);
  }
  js_scatter_ants(j, 0, -1);
}

/* Do something, as item i (spec, "Actions"). The one way a habit, and later
 * a script, changes anything. 0 done, -1 not something this item can do. */
static JS_OPT int js_act(Jar *j, int i, int action, int arg) {
  JPlaced *p;
  int lo, hi, x, y;
  if (i < 0 || i >= j->nplaced) return -1;
  p = &j->placed[i];
  x = p->x / JS_FX;
  y = js_item_y(j, i);
  switch (action) {
  case JA_NONE: return 0;
  case JA_HOP:
    if (p->kind == JK_HANGING) return -1;
    if (p->yoff == 0 && p->vy == 0) {
      int h = arg > 0 ? (arg > 24 ? 24 : arg) : 6;
      p->vy = (int16_t)js_isqrt(2 * 6 * h * 16);
    }
    return 0;
  case JA_WALK:
    if (p->kind == JK_HANGING) return -1;
    /* "Walk high" from the soil is a climb; walking to somewhere on the
     * ground from a ledge, the climb down. */
    if (js_can_climb(p) && (p->level ? arg < JZ_HIGH : arg == JZ_HIGH)) {
      js_trip(j, i, p->level ? 0 : js_near_ledge(x));
      return 0;
    }
    if (p->climb) return 0;
    if (p->on_id) {                              /* up on furniture: along its top */
      js_range(p, &lo, &hi);
      p->tx = (int32_t)js_rr(j, lo, hi) * JS_FX;
      p->moving = 1;
      return 0;
    }
    {
      JPlaced tmp;
      tmp.kind = JK_CRITTER;
      tmp.zone = (uint8_t)(arg < JZ_KINDS ? arg : JZ_ANYWHERE);
      tmp.home_x = p->home_x;
      tmp.level = p->level;
      js_range(&tmp, &lo, &hi);
      if (p->kind != JK_CRITTER) js_range(p, &lo, &hi);   /* decor stays put */
      p->tx = (int32_t)js_rr(j, lo, hi) * JS_FX;
      p->moving = 1;
    }
    return 0;
  case JA_FLOAT: p->phase = 0; p->wait = 0; return 0;
  case JA_FACE: {
    int k, best = 9999, bx = x;
    for (k = 0; k < j->nmoss; k++) {
      int d = js_abs(j->moss[k].x / JS_FX - x);
      if (d < best) { best = d; bx = j->moss[k].x / JS_FX; }
    }
    p->flip = bx < x;
    return 0;
  }
  case JA_STOP: p->tx = p->x; p->moving = 0; return 0;
  case JA_FRAME: p->cur = (uint8_t)(p->nframes ? arg % p->nframes : 0); return 0;
  case JA_FLIP: p->flip ^= 1; return 0;
  case JA_GLOW: p->glow = (uint8_t)(arg == 2 ? !p->glow : arg != 0); return 0;
  case JA_PARTICLE: js_particle(j, arg < JP_KINDS ? arg : JP_SPARKLE, x, y - 8); return 0;
  case JA_SAY:
    if (arg < 0 || arg >= p->nbub) return -1;
    js_say(j, JW_ITEM + i, arg);
    return 0;
  case JA_WAIT: p->wait = (int16_t)(js_clamp(arg, 0, 400) * JS_ITEM_TICK); return 0;
  case JA_SOUND:
    if (arg < 1 || arg > JSD_KINDS) return -1;
    j->snd = (uint8_t)arg;
    return 0;
  case JA_BURST: {
    int k, t = arg >= 0 && arg < JP_KINDS ? arg : JP_SPARKLE;
    for (k = 0; k < 10; k++) {
      int s = js_particle(j, t, x, y - 8);
      j->part[s].vx = (int8_t)js_rr(j, -14, 14);
      j->part[s].vy = (int8_t)js_rr(j, -20, 0);
      j->part[s].life = (uint8_t)js_rr(j, 20, 36);
    }
    return 0;
  }
  case JA_THROW: return js_throw(j, i);
  case JA_EAT: {
    int k, best = -1, bd = 25;
    for (k = 0; k < j->nbeds; k++) {
      int d = js_abs(JS_BED_X(k) - x);
      if (j->bed[k].ready && !j->bed[k].mould && d < bd && p->kind != JK_HANGING) { bd = d; best = k; }
    }
    if (best < 0) return -1;
    j->bed[best].ready = 0;
    j->bed[best].claimed = 0;
    j->bed[best].grow = 0;
    if (p->fed < 255) p->fed++;
    js_particle(j, JP_HEART, x, y - 10);
    return 0;
  }
  case JA_DROP: {
    int k, s = -1, best = -1, bd = 41;
    if (p->kind == JK_HANGING) return -1;
    for (k = 0; k < JS_PELLETS; k++) if (!j->pel_t[k]) { s = k; break; }
    if (s < 0) return -1;
    j->pel_x[s] = (int16_t)x;
    j->pel_t[s] = (uint16_t)(90 * JS_HZ);
    for (k = 0; k < j->nbeds; k++) {
      int d = js_abs(JS_BED_X(k) - x);
      if (d < bd) { bd = d; best = k; }
    }
    if (best >= 0 && !j->bed[best].ready && !j->bed[best].young)
      j->bed[best].grow += JS_GROW_MS / 3;          /* fed: it ripens sooner */
    return 0;
  }
  case JA_SIGNAL:
    if (arg < 1 || arg > 255) return -1;
    j->sig_n = (uint8_t)arg;                         /* heard next step: no recursion */
    j->sig_from = (int8_t)i;
    return 0;
  case JA_SEEK: {
    int k, best = -1, bd = 9999;
    if (p->kind != JK_CRITTER || arg < 0 || arg >= JT_KINDS) return -1;
    for (k = 0; k < j->nplaced; k++) {
      int d;
      if (k == i || !(j->placed[k].tags & JT_BIT(arg)) || j->placed[k].level != p->level) continue;
      d = js_abs(j->placed[k].x / JS_FX - x);
      if (d < bd) { bd = d; best = k; }
    }
    if (best < 0) return -1;
    js_range(p, &lo, &hi);
    p->tx = (int32_t)js_clamp(j->placed[best].x / JS_FX + (j->placed[best].x / JS_FX > x ? -10 : 10),
                              lo, hi) * JS_FX;
    p->moving = 1;
    return 0;
  }
  case JA_FLY:
    if (!p->part) return -1;
    if (p->pst == 0) { p->px = (int16_t)(x * 16); p->py = (int16_t)((y - 6) * 16); }
    p->pst = 1;
    p->pt = (int16_t)js_rr(j, 8 * JS_HZ, 20 * JS_HZ);
    p->ptx = (int16_t)(js_rr(j, 20, 220) * 16);
    p->pty = (int16_t)(js_rr(j, 20, 90) * 16);
    return 0;
  case JA_HOME:
    if (!p->part || p->pst == 0) return -1;
    p->pst = 2;
    return 0;
  case JA_BOOST:
    if (j->boost_cd > 0) return -1;
    j->belt_boost = (int16_t)(js_clamp(arg > 0 ? arg : 10, 1, 30) * JS_HZ);
    j->boost_cd = (int16_t)(60 * JS_HZ);
    return 0;
  case JA_NUDGE:
    if (!j->sulk) return -1;
    j->sulk = 0;
    j->snail.boost = JS_BOOST_STEPS;
    js_say(j, JW_SNAIL, JS_SAY_YAY);
    return 0;
  case JA_SHAKE:
    js_shake(j);
    return 0;
  default: return -1;
  }
}

/* ---- phase 2: an item's script runs through the same senses and actions --- */

typedef struct { Jar *j; int i; } JsVm;

static JS_OPT int js_vm_sense(void *c, int s) { return js_sense(((JsVm *)c)->j, ((JsVm *)c)->i, s); }
static JS_OPT int js_vm_act(void *c, int a, int arg) { return js_act(((JsVm *)c)->j, ((JsVm *)c)->i, a, arg); }
static JS_OPT uint32_t js_vm_rnd(void *c) { return js_rnd(((JsVm *)c)->j); }

/* Run item i's script for an event: a JV_* result (apps/jarvm.h). */
static JS_OPT int js_script(Jar *j, int i, int ev, int arg) {
  JPlaced *p = &j->placed[i];
  JsVm c;
  JvIo io;
  int16_t was[JI_MEM];
  int k, r;
  c.j = j;
  c.i = i;
  io.ctx = &c;
  io.sense = js_vm_sense;
  io.act = js_vm_act;
  io.rnd = js_vm_rnd;
  ji_copy(was, p->mem, (int)sizeof was);
  r = jv_run(j->spool + p->soff, p->slen, ev, arg, p->mem, &io, 0);
  for (k = 0; k < JI_MEM; k++) if (was[k] != p->mem[k]) p->mem_dirty = 1;
  return r;
}

/* The app, saving: if item i's memory changed, copy it into `it` (the same
 * item's record, read from the card) and say 1 -- the app writes it back.
 * This is how the 8 slots persist. */
static JS_OPT int js_mem_sync(Jar *j, int i, JItem *it) {
  JPlaced *p = &j->placed[i];
  if (!p->mem_dirty || p->id != it->id) return 0;
  ji_copy(it->mem, p->mem, (int)sizeof it->mem);
  p->mem_dirty = 0;
  return 1;
}

/* An event reaches item i: its script, if it has one, else each habit it
 * matches does its action. Returns how many fired. */
static JS_OPT int js_item_event(Jar *j, int i, int ev, int arg) {
  JPlaced *p = &j->placed[i];
  int h, n = 0;
  /* ---- PHASE 2 SCRIPT HOOK -------------------------------------------------
   * The script has this event first. It returned: the event is handled and
   * the recipe is skipped. No handler: the recipe takes it. Stopped (the
   * step limit, or a fault): a ? bubble, and the recipe takes this event --
   * the script is tried again at the next one. */
  if (p->slen) {
    int r = js_script(j, i, ev, arg);
    if (r == JV_DONE) return 1;
    if (r == JV_LIMIT || r == JV_FAULT) { p->say = JI_MAX_BUB; p->say_t = JS_SAY_STEPS; }
  }
  /* ---- end of the hook ---------------------------------------------------- */
  for (h = 0; h < p->nhab; h++) {
    const JHabit *hb = &p->hab[h];
    if (hb->event != ev) continue;
    if (ev == JE_TICK) {
      int per = hb->earg ? hb->earg : 1;
      if ((arg + p->phase) % per) continue;
    } else if (hb->earg && hb->earg != arg && (ev == JE_NEAR || ev == JE_JAM || ev == JE_TIME ||
                                                ev == JE_WEATHER || ev == JE_HOUR ||
                                                ev == JE_SIGNAL || ev == JE_WORLD)) {
      continue;
    }
    js_act(j, i, hb->action, hb->aarg);
    n++;
  }
  return n;
}

/* An event reaches every placed item. */
static JS_OPT int js_event(Jar *j, int ev, int arg) {
  int i, n = 0;
  for (i = 0; i < j->nplaced; i++) n += js_item_event(j, i, ev, arg);
  return n;
}

/* ---- the economy --------------------------------------------------------- */

static JS_OPT int js_owns(const Jar *j, uint32_t id) {
  int i;
  for (i = 0; i < j->nowned; i++) if (j->owned[i] == id) return 1;
  return 0;
}

static JS_OPT int js_own(Jar *j, uint32_t id) {
  if (js_owns(j, id)) return 0;
  if (j->nowned >= JS_MAX_OWNED) return -1;
  j->owned[j->nowned++] = id;
  return 0;
}

/* In the jar: placed, or (in the companion, which places nothing) saved as
 * placed. */
static JS_OPT int js_in_jar(const Jar *j, uint32_t id) {
  int i;
  if (js_find(j, id) >= 0) return 1;
  for (i = 0; i < j->nwant; i++) if (j->want[i].id == id) return 1;
  return 0;
}

static JS_OPT int js_on_shelf(const Jar *j, uint32_t id) {
  int i;
  for (i = 0; i < JS_SHELF; i++) if (id && j->shelf[i] == id) return i;
  return -1;
}

/* An item gone for good (gifted): out of My Stuff, the shelf and the jar. */
static JS_OPT void js_disown(Jar *j, uint32_t id) {
  int i, k;
  for (i = 0; i < j->nowned; i++)
    if (j->owned[i] == id) {
      for (k = i; k + 1 < j->nowned; k++) j->owned[k] = j->owned[k + 1];
      j->nowned--;
      break;
    }
  for (i = 0; i < JS_SHELF; i++) if (j->shelf[i] == id) j->shelf[i] = 0;
  for (i = 0; i < j->nwant; i++)
    if (j->want[i].id == id) {
      for (k = i; k + 1 < j->nwant; k++) ji_copy(&j->want[k], &j->want[k + 1], (int)sizeof j->want[k]);
      j->nwant--;
      break;
    }
  if (js_find(j, id) >= 0) js_unplace(j, js_find(j, id));
  if (j->decor == id) j->decor = 0;
}

static JS_OPT int js_spend(Jar *j, uint32_t n) {
  if (j->coins < n) return -1;
  j->coins -= n;
  return 0;
}

static JS_OPT const uint16_t JS_COST_MOSS[JS_MAX_MOSS] = { 0, 30, 70, 150, 320, 700 };
static JS_OPT const uint16_t JS_COST_MACH[JS_MAX_MACH] = { 0, 0, 120, 400 };
static JS_OPT const uint16_t JS_COST_BELT[JS_MAX_BELT] = { 50, 150, 450 };
static JS_OPT const uint16_t JS_COST_BED[JS_MAX_BEDS]  = { 0, 0, 25, 60, 140, 300 };

static JS_OPT int js_up_level(const Jar *j, int k) {
  return k == JU_MOSS ? j->nmoss : k == JU_MACH ? j->nmach : k == JU_BELT ? j->belt : j->nbeds;
}

static JS_OPT int js_up_max(int k) {
  return k == JU_MOSS ? JS_MAX_MOSS : k == JU_MACH ? JS_MAX_MACH : k == JU_BELT ? JS_MAX_BELT : JS_MAX_BEDS;
}

/* What the next one costs, or -1 at the top. */
static JS_OPT int js_up_cost(const Jar *j, int k) {
  int lv = js_up_level(j, k);
  if (lv >= js_up_max(k)) return -1;
  return k == JU_MOSS ? JS_COST_MOSS[lv] : k == JU_MACH ? JS_COST_MACH[lv] :
         k == JU_BELT ? JS_COST_BELT[lv] : JS_COST_BED[lv];
}

/* The rate an upgrade would give. */
static JS_OPT int32_t js_rate_after(const Jar *j, int k) {
  return js_rate_for(j->nbeds + (k == JU_BED), j->nmoss + (k == JU_MOSS),
                     j->nmach + (k == JU_MACH), j->belt + (k == JU_BELT));
}

static JS_OPT void js_new_moss(Jar *j, int k) {
  JMoss *m = &j->moss[k];
  ji_zero(m, (int)sizeof *m);
  m->x = m->tx = (int32_t)(JS_DROP_X + 8 + 6 * k) * JS_FX;
  m->bed = -1;
  m->say = -1;
  m->t = (int16_t)js_rr(j, 10, 60);
}

/* Buy upgrade k: 0, -1 at the top, -2 not enough coins. It appears in the
 * scene at once, with a sparkle. */
static JS_OPT int js_buy(Jar *j, int k) {
  int c = js_up_cost(j, k);
  if (c < 0) return -1;
  if (js_spend(j, (uint32_t)c)) return -2;
  switch (k) {
  case JU_MOSS:
    js_new_moss(j, j->nmoss);
    js_particle(j, JP_SPARKLE, j->moss[j->nmoss].x / JS_FX, JS_SOIL - 12);
    j->nmoss++;
    break;
  case JU_MACH:
    j->nmach++;
    js_particle(j, JP_SPARKLE, JS_MACH_X[j->nmach == 3 ? 1 : 2], JS_SOIL - 20);
    break;
  case JU_BELT:
    j->belt++;
    js_particle(j, JP_SPARKLE, 140, JS_SOIL - 6);
    break;
  default:
    j->bed[j->nbeds].grow = 0;
    j->bed[j->nbeds].ready = 0;
    j->bed[j->nbeds].claimed = 0;
    js_particle(j, JP_SPARKLE, JS_BED_X(j->nbeds), JS_SOIL - 10);
    j->nbeds++;
    break;
  }
  return 0;
}

/* ---- starting, and coming back ------------------------------------------- */

/* A new jar. `j` must be zeroed first (an app has no memset: the caller has
 * api->mem_set). */
static JS_OPT void js_init(Jar *j, uint32_t seed) {
  static const uint8_t TYPES[JS_MAX_BEDS] = { 0, 1, 2, 3, 4, 0 };
  int i;
  j->seed = seed | 1u;
  j->nbeds = 2;
  j->nmoss = 1;
  j->nmach = 2;
  j->belt = 0;
  j->minute = -1;
  for (i = 0; i < JS_MAX_BEDS; i++) {
    j->bed[i].type = TYPES[i];
    j->bed[i].grow = JS_GROW_MS - 4000 - 9000 * i;
  }
  for (i = 0; i < JS_MAX_MOSS; i++) js_new_moss(j, i);
  for (i = 0; i < JS_UNITS; i++) j->unit[i].to = -1;
  j->snail.x = JS_HOME_X * JS_FX;
  j->snail.say = -1;
  j->fixer[0] = j->fixer[1] = -1;
  j->jam_clock = js_rr(j, 120, 240) * JS_HZ;
  j->nap_clock = js_rr(j, 60, 150) * JS_HZ;
  j->world_clock = js_rr(j, 20, 60) * 60 * JS_HZ;
  j->sulk_clock = js_rr(j, 40, 80) * 60 * JS_HZ;
  j->mould_clock = js_rr(j, 60, 120) * 60 * JS_HZ;
  j->sig_from = -1;
  j->chat_cd = (int16_t)(15 * JS_HZ);
  j->reply_who = -1;
  for (i = 0; i < JS_FLIES; i++) {
    j->fly_x[i] = (int16_t)(js_rr(j, 20, 220) * 16);
    j->fly_y[i] = (int16_t)(js_rr(j, 24, 100) * 16);
  }
}

/* Time away makes nothing (the owner's rule, 2026-10-10): jam and coins
 * come only from a factory on screen, so a jar opened after a night shut is
 * the jar it was. This only stamps `seen`. Plants still come of age by the
 * wall clock (js_settle_beds), but no berry ripens and no jar is made while
 * the app is closed. Returns the jars made while away: 0, always. */
static JS_OPT uint32_t js_away(Jar *j, uint32_t now) {
  if (now) j->seen = now;
  return 0;
}

static JS_OPT int js_phase_of(int minute) {
  if (minute < 0) return PH_NONE;
  if (minute >= 300 && minute < 480) return PH_DAWN;
  if (minute >= 480 && minute < 1020) return PH_DAY;
  if (minute >= 1020 && minute < 1200) return PH_DUSK;
  return PH_NIGHT;
}

/* The minute of the local day, or -1 without a clock. Dawn, day, dusk and
 * night are events when they begin. */
static JS_OPT void js_set_minute(Jar *j, int minute) {
  int ph = js_phase_of(minute);
  j->minute = (int16_t)minute;
  if (ph != j->phase) {
    j->phase = (uint8_t)ph;
    if (ph) js_event(j, JE_TIME, ph);
  }
}

/* The real weather outside (apps/jar.c asks the server), JWX_* or 0; a
 * change is an event. */
static JS_OPT void js_set_weather(Jar *j, int w) {
  if (w == j->weather) return;
  j->weather = (uint8_t)w;
  if (w) js_event(j, JE_WEATHER, w);
}

/* The real hour, 0..23 or -1 (the app hands it in; js_set_minute's minute is
 * moved to the real sunrise). On the hour is an event: `on hour 18:`. */
static JS_OPT void js_set_hour(Jar *j, int hour) {
  int h1 = hour >= 0 && hour < 24 ? hour + 1 : 0;
  if (h1 == j->hour1) return;
  j->hour1 = (uint8_t)h1;
  if (h1) js_event(j, JE_HOUR, h1);
}

/* The player put item i in the jar just now: the others hear `on new`. */
static JS_OPT void js_announce(Jar *j, int i) {
  int k;
  if (i < 0 || i >= j->nplaced) return;
  j->newtraits = j->placed[i].tags;
  for (k = 0; k < j->nplaced; k++) if (k != i) js_item_event(j, k, JE_NEW, 0);
}

/* What needs the player now: JC_* bits. Production waits on them -- nothing
 * is lost, it only stops making -- so a jar left open all night stops. */
static JS_OPT int js_chores(const Jar *j) {
  int k, m = 0;
  if (j->sulk) m |= JC_SULK;
  if (j->puddle >= 20) m |= JC_PUDDLE;
  for (k = 0; k < j->nbeds; k++) if (j->bed[k].mould) m |= JC_MOULD;
  return m;
}

/* The player sees to a chore. 0, or -1 if there was nothing to do. */
static JS_OPT int js_fix(Jar *j, int chore) {
  int k, did = 0;
  if ((chore & JC_SULK) && j->sulk) {
    j->sulk = 0;
    j->snail.boost = JS_BOOST_STEPS;
    js_say(j, JW_SNAIL, JS_SAY_YAY);
    did = 1;
  }
  if ((chore & JC_PUDDLE) && j->puddle) { j->puddle = 0; did = 1; }
  if (chore & JC_MOULD)
    for (k = 0; k < j->nbeds; k++)
      if (j->bed[k].mould) { j->bed[k].mould = 0; did = 1; }
  if (did) js_say(j, 0, JS_SAY_FIXED);
  return did ? 0 : -1;
}

/* ---- one step ------------------------------------------------------------ */

/* The snail leaves the screen: the only place coins are made. */
static JS_OPT void js_ship(Jar *j) {
  uint32_t n = j->snail.load;
  if (!n) return;
  j->coins += n * JS_JAR_VALUE;
  j->shipped += n;
  j->snail.load = 0;
  js_float(j, JS_DOOR_X - 6, JS_SOIL - 30, n * JS_JAR_VALUE);
  js_event(j, JE_SHIPPED, 0);
}

static JS_OPT void js_step_beds(Jar *j) {
  int i;
  for (i = 0; i < j->nbeds; i++) {
    JBed *b = &j->bed[i];
    if (b->ready || b->young || b->mould || j->puddle >= 20) continue;
    b->grow += JS_STEP_MS;
    if (b->grow >= JS_GROW_MS) { b->grow = JS_GROW_MS; b->ready = 1; js_event(j, JE_BERRY, 0); }
  }
}

static JS_OPT int js_need_fixer(const Jar *j) {
  return j->jammed && (j->fixer[0] < 0 || j->fixer[1] < 0);
}

static JS_OPT void js_add_fixer(Jar *j, int who) {
  int k = j->fixer[0] < 0 ? 0 : 1;
  j->fixer[k] = (int8_t)who;
}

static JS_OPT int32_t js_fix_spot(const Jar *j, int who) {
  return (int32_t)(j->jam_x + (j->fixer[0] == who ? -7 : 7)) * JS_FX;
}

static JS_OPT void js_step_moss(Jar *j, int k) {
  JMoss *m = &j->moss[k];
  int spd = m->boost > 0 ? JS_MOSS_SPD * 6 / 5 : JS_MOSS_SPD;
  int moving = 0, i;
  if (m->boost > 0) m->boost--;
  if (m->say_t > 0) m->say_t--;
  switch (m->st) {
  case M_IDLE: {
    int best = -1, bd = 1 << 30;
    if (js_need_fixer(j) && j->fixer[0] != k && j->fixer[1] != k) {
      js_add_fixer(j, k);
      m->st = M_TO_FIX;
      m->tx = js_fix_spot(j, k);
      break;
    }
    for (i = 0; i < j->nbeds; i++) {
      int d;
      if (!j->bed[i].ready || j->bed[i].claimed) continue;
      d = js_abs(JS_BED_X(i) * JS_FX - m->x);
      if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0) {
      j->bed[best].claimed = 1;
      m->bed = (int8_t)best;
      m->st = M_TO_BED;
      m->tx = (int32_t)JS_BED_X(best) * JS_FX;
      m->idle = 0;
      break;
    }
    if (j->nap_clock <= 0) {
      j->nap_clock = js_rr(j, 300, 540) * JS_HZ;
      m->st = M_TO_NAP;
      m->tx = JS_NAP_X * JS_FX;
      break;
    }
    /* Pottering about the works between jobs. */
    m->idle++;
    if (m->x != m->tx) { js_walk(&m->x, m->tx, spd / 2 + 1, &m->face); moving = 1; }
    else if (--m->t <= 0) {
      m->tx = (int32_t)js_rr(j, 96, 160) * JS_FX;
      m->t = (int16_t)js_rr(j, 40, 200);
    }
    break;
  }
  case M_TO_BED:
    moving = 1;
    if (!j->bed[m->bed].ready) { m->st = M_IDLE; m->bed = -1; m->t = 0; break; }  /* eaten, or ants */
    if (js_walk(&m->x, m->tx, spd, &m->face)) { m->st = M_PICK; m->t = JS_PICK_STEPS; }
    break;
  case M_PICK:
    if (!j->bed[m->bed].ready) { m->st = M_IDLE; m->bed = -1; m->t = 0; break; }
    if (--m->t <= 0) {
      JBed *b = &j->bed[m->bed];
      b->ready = 0;
      b->claimed = 0;
      b->grow = 0;
      m->carry = 1;
      m->bed = -1;
      m->st = M_TO_VAT;
      m->tx = (int32_t)(JS_DROP_X - 3 * k) * JS_FX;
    }
    break;
  case M_TO_VAT:
    if (!js_walk(&m->x, m->tx, spd, &m->face)) { moving = 1; break; }
    m->face = 0;
    if (j->mach[0].st != 0 && js_need_fixer(j) && j->fixer[0] != k && j->fixer[1] != k) {
      js_add_fixer(j, k);                   /* the vat is stuck behind the jam: help */
      m->st = M_TO_FIX;
      m->tx = js_fix_spot(j, k);
      break;
    }
    if (j->mach[0].st == 0) {               /* into the vat */
      j->mach[0].st = 1;
      j->mach[0].t = (int16_t)js_stage_steps(j->nmach, j->belt);
      m->carry = 0;
      m->st = M_IDLE;
      m->t = 20;
      m->tx = m->x;
      js_particle(j, JP_PUFF, JS_MACH_X[0], JS_SOIL - 18);
    }
    break;
  case M_TO_NAP:
    moving = 1;
    if (js_walk(&m->x, m->tx, spd, &m->face)) {
      if (m->yoff < JS_NAP_H) { if ((j->steps & 1) == 0) m->yoff++; }
      else { m->st = M_NAP; m->t = (int16_t)js_rr(j, 12 * JS_HZ, 20 * JS_HZ); }
    }
    break;
  case M_NAP:
    if ((j->steps + (uint32_t)k * 13) % 60 == 0)
      js_particle(j, JP_ZZZ, m->x / JS_FX + 3, JS_SOIL - m->yoff - 10);
    if (--m->t <= 0 || js_need_fixer(j)) {
      m->st = M_IDLE;
      m->t = 10;
      m->tx = m->x;
    }
    break;
  case M_TO_FIX:
    moving = 1;
    if (js_walk(&m->x, m->tx, spd, &m->face)) m->st = M_FIX;
    break;
  case M_FIX:
    break;
  }
  /* Down off the crate when not napping. */
  if (m->st != M_NAP && m->st != M_TO_NAP && m->yoff > 0) m->yoff = (int16_t)(m->yoff > 3 ? m->yoff - 3 : 0);
  m->frame = (uint8_t)(m->st == M_NAP ? 2 : moving ? ((j->steps >> 3) & 1) : 0);
}

static JS_OPT void js_step_works(Jar *j) {
  int i, k, stage = js_stage_steps(j->nmach, j->belt);
  int spd = JS_BELT_SPD[j->belt];
  if (j->belt_boost > 0 || j->world == JWD_BREEZE) spd *= 2;
  if (j->puddle >= 40) spd /= 2;
  /* The belt: units ride to their machine and go in when it is free. */
  if (!j->jammed) j->belt_pos += spd;
  for (i = 0; i < JS_UNITS; i++) {
    JUnit *u = &j->unit[i];
    int32_t stop;
    int blocked = 0;
    if (u->to < 0) continue;
    stop = (int32_t)(JS_MACH_X[(int)u->to] - 8) * JS_FX;
    if (u->x >= stop) {
      if (j->mach[(int)u->to].st == 0) {
        j->mach[(int)u->to].st = 1;
        j->mach[(int)u->to].t = (int16_t)stage;
        u->to = -1;
      }
      continue;
    }
    if (j->jammed) continue;
    for (k = 0; k < JS_UNITS; k++)
      if (k != i && j->unit[k].to == u->to && j->unit[k].x > u->x &&
          j->unit[k].x - u->x < 8 * JS_FX) blocked = 1;
    if (!blocked) {
      u->x += spd;
      if (u->x > stop) u->x = stop;
    }
  }
  /* The machines. */
  for (i = 0; i < JS_MAX_MACH; i++) {
    JMach *m = &j->mach[i];
    if (!js_mach_active(j, i)) continue;
    if (m->st == 1 && --m->t <= 0) m->st = 2;
    if (m->st != 2) continue;
    if (i == JS_MAX_MACH - 1) {              /* the crane sets a jar on the dock */
      j->dock++;
      m->st = 0;
      continue;
    }
    if (j->jammed) continue;
    {
      int nx = js_next_mach(j, i), free_slot = -1;
      int32_t at = (int32_t)(JS_MACH_X[i] + 8) * JS_FX;
      for (k = 0; k < JS_UNITS; k++) {
        if (j->unit[k].to < 0) { if (free_slot < 0) free_slot = k; }
        else if (js_abs(j->unit[k].x - at) < 8 * JS_FX) free_slot = -2;
        if (free_slot == -2) break;
      }
      if (nx >= 0 && free_slot >= 0) {
        j->unit[free_slot].x = at;
        j->unit[free_slot].to = (int8_t)nx;
        m->st = 0;
      }
    }
  }
  /* The vat steams while it cooks, and now and then puffs properly. */
  if (j->mach[0].st == 1 && js_rr(j, 0, 30) == 0)
    js_particle(j, JP_PUFF, JS_MACH_X[0] + js_rr(j, -4, 4), JS_SOIL - 18);
  if (js_rr(j, 0, 20 * JS_HZ) == 0) {
    js_particle(j, JP_PUFF, JS_MACH_X[0] - 3, JS_SOIL - 18);
    js_particle(j, JP_PUFF, JS_MACH_X[0] + 2, JS_SOIL - 20);
    js_particle(j, JP_PUFF, JS_MACH_X[0], JS_SOIL - 22);
  }
}

static JS_OPT void js_step_snail(Jar *j) {
  JSnail *s = &j->snail;
  int spd = s->fast ? JS_SNAIL_FAST : s->boost > 0 ? JS_SNAIL_SPD * 6 / 5 : JS_SNAIL_SPD;
  if (s->boost > 0) s->boost--;
  if (s->say_t > 0) s->say_t--;
  s->frame = 0;
  switch (s->st) {
  case S_HOME:
    if (js_need_fixer(j) && j->nmoss < 2 && j->fixer[0] != JW_SNAIL) {
      js_add_fixer(j, JW_SNAIL);
      s->st = S_TO_FIX;
      break;
    }
    if (j->dock == 0) { s->t = 0; j->pile0 = 0; break; }
    if (j->sulk) {                                 /* a chore: it will not go out */
      if ((j->steps % (20 * JS_HZ)) == 0) js_say(j, JW_SNAIL, JS_SAY_SIGH);
      break;
    }
    if (j->dock >= 3 || ++s->t >= 6 * JS_HZ) {
      uint32_t cap = 3;
      if (j->dock > 6) {
        if (j->pile0 < j->dock) j->pile0 = j->dock;
        cap = (j->pile0 + 9) / 10;
        if (cap < 3) cap = 3;
      } else {
        j->pile0 = 0;
      }
      s->load = j->dock < cap ? j->dock : cap;
      s->fast = j->dock > 6;
      j->dock -= s->load;
      s->st = S_OUT;
      s->t = 0;
    }
    break;
  case S_OUT:
    s->frame = (uint8_t)((j->steps >> 4) & 1);
    if (js_walk(&s->x, JS_EXIT_X * JS_FX, spd, &s->face)) {
      js_ship(j);
      s->st = S_AWAY;
      s->t = (int16_t)(s->fast ? 10 : 50);
    }
    break;
  case S_AWAY:
    if (--s->t <= 0) s->st = S_BACK;
    break;
  case S_BACK:
    s->frame = (uint8_t)((j->steps >> 4) & 1);
    if (js_walk(&s->x, JS_HOME_X * JS_FX, spd, &s->face)) {
      s->st = S_HOME;
      s->face = 0;
      s->fast = 0;
      s->t = 0;
    }
    break;
  case S_TO_FIX:
    s->frame = (uint8_t)((j->steps >> 4) & 1);
    if (js_walk(&s->x, js_fix_spot(j, JW_SNAIL), spd, &s->face)) s->st = S_FIX;
    break;
  case S_FIX:
    break;
  }
}

static JS_OPT int js_fixer_ready(const Jar *j, int who) {
  if (who < 0) return 0;
  if (who == JW_SNAIL) return j->snail.st == S_FIX;
  return j->moss[who].st == M_FIX;
}

static JS_OPT void js_step_jam(Jar *j) {
  int k;
  if (!j->jammed) {
    if (--j->jam_clock > 0) return;
    j->jammed = 1;
    j->jam_x = (int16_t)js_rr(j, 118, 150);
    j->fixer[0] = j->fixer[1] = -1;
    j->fix_t = 0;
    js_particle(j, JP_PUFF, j->jam_x, JS_SOIL - 6);
    js_say(j, 0, JS_SAY_UHOH);
    js_event(j, JE_JAM, 1);
    return;
  }
  if (!js_fixer_ready(j, j->fixer[0]) || !js_fixer_ready(j, j->fixer[1])) return;
  j->fix_t++;
  if (j->fix_t % 24 == 0) js_particle(j, (j->fix_t / 24) & 1 ? JP_SPARKLE : JP_PUFF, j->jam_x, JS_SOIL - 8);
  if (j->fix_t < 4 * JS_HZ) return;
  j->jammed = 0;
  j->jam_clock = js_rr(j, 240, 480) * JS_HZ;
  for (k = 0; k < 2; k++) {
    int who = j->fixer[k];
    if (who == JW_SNAIL) j->snail.st = S_BACK;
    else if (who >= 0) {
      JMoss *m = &j->moss[who];
      m->st = m->carry ? M_TO_VAT : M_IDLE;     /* back to what it was doing */
      m->t = 20;
      m->tx = m->carry ? (int32_t)(JS_DROP_X - 3 * who) * JS_FX : m->x;
    }
  }
  js_say(j, j->fixer[0], JS_SAY_FIXED);
  j->fixer[0] = j->fixer[1] = -1;
  js_event(j, JE_JAM, 2);
}

/* A climb under way, one step: along to the climb, then up it (a pixel
 * every other step) or down it (a pixel a step). 1 while it lasts: the
 * item does nothing else meanwhile. */
static JS_OPT int js_step_climb(Jar *j, int i) {
  JPlaced *p = &j->placed[i];
  int c = p->climb;
  if (!c) return 0;
  if (p->nframes > 1 && (j->steps & 7) == 0) p->cur = (uint8_t)((p->cur + 1) % p->nframes);
  if (c == JC_TO_UP || c == JC_TO_DOWN) {
    uint8_t f = p->flip;
    p->tx = (int32_t)JS_LEDGE_UP[c == JC_TO_UP ? p->goal : p->level] * JS_FX;
    if (js_walk(&p->x, p->tx, js_speed(p), &f)) p->climb++;    /* there: on to the climb */
    p->flip = f;
    return 1;
  }
  if (c == JC_UP) {
    if ((j->steps & 1) || ++p->lift < JS_LEDGE_H[p->goal]) return 1;
    p->level = p->goal;
    p->goal = 0;
  } else {
    if (--p->lift > 0) return 1;
    p->level = 0;
    if (p->goal) { p->climb = JC_TO_UP; p->lift = 0; return 1; }   /* ledge to ledge */
  }
  p->lift = (int8_t)JS_LEDGE_H[p->level];
  p->climb = 0;
  p->moving = 0;
  p->t = 30;                                         /* a look round first */
  return 1;
}

/* A critter about to pick somewhere new may go up a ledge instead, or come
 * down from one: one that likes it high, mostly; the others now and then.
 * 1 if it set off. */
static JS_OPT int js_maybe_climb(Jar *j, int i) {
  JPlaced *p = &j->placed[i];
  int high = p->zone == JZ_HIGH;
  if (!js_can_climb(p) || js_rr(j, 0, p->level ? (high ? 6 : 1) : (high ? 1 : 15))) return 0;
  js_trip(j, i, p->level ? 0 : high ? js_rr(j, 1, JS_LEVELS - 1) : js_near_ledge(p->x / JS_FX));
  return 1;
}

/* An item's part (the bird of a birdhouse): out, it wanders the jar and
 * comes home when its time is up; home, now and then it goes out on its own
 * (a script's `fly`/`home` steer it too). */
static JS_OPT void js_step_part(Jar *j, JPlaced *p) {
  int hx = (p->x / JS_FX) * 16, hy = (JS_SOIL - 20 - p->lift) * 16;
  if (p->kind == JK_HANGING) hy = (JS_LID + p->home_y + 4) * 16;
  if (p->pst == 0) {
    p->px = (int16_t)hx;
    p->py = (int16_t)hy;
    if (--p->pt <= 0) {
      p->pt = (int16_t)js_rr(j, 20 * JS_HZ, 60 * JS_HZ);
      if (js_rr(j, 0, 2)) {                          /* off it goes */
        p->pst = 1;
        p->pt = (int16_t)js_rr(j, 8 * JS_HZ, 20 * JS_HZ);
        p->ptx = (int16_t)(js_rr(j, 20, 220) * 16);
        p->pty = (int16_t)(js_rr(j, 20, 90) * 16);
      }
    }
    return;
  }
  if (p->pst == 1) {
    if (--p->pt <= 0) p->pst = 2;
    if (js_abs(p->px - p->ptx) < 32 && js_abs(p->py - p->pty) < 32) {
      p->ptx = (int16_t)(js_rr(j, 20, 220) * 16);
      p->pty = (int16_t)(js_rr(j, 20, 90) * 16);
    }
  } else {
    p->ptx = (int16_t)hx;
    p->pty = (int16_t)hy;
    if (js_abs(p->px - hx) < 24 && js_abs(p->py - hy) < 24) {
      p->pst = 0;
      p->pt = (int16_t)js_rr(j, 20 * JS_HZ, 60 * JS_HZ);
      return;
    }
  }
  p->px = (int16_t)(p->px + js_clamp(p->ptx - p->px, -20, 20));
  p->py = (int16_t)(p->py + js_clamp(p->pty - p->py, -14, 14) + (int)((j->steps >> 2) & 1) * 4 - 2);
}

static JS_OPT void js_step_item(Jar *j, int i) {
  JPlaced *p = &j->placed[i];
  int lo, hi, spd = js_speed(p);
  if (p->say_t > 0) p->say_t--;
  /* in the air */
  if (p->vy || p->yoff) {
    p->yoff = (int16_t)(p->yoff + p->vy);
    p->vy = (int16_t)(p->vy - 6);
    if (p->vx) p->x = js_clamp(p->x + p->vx, 10 * JS_FX, 230 * JS_FX);
    if (p->yoff <= 0) {
      p->yoff = 0;
      p->vy = 0;
      if (p->vx) {                                 /* a thrown thing lands: a bump */
        int k, d;
        p->vx = 0;
        p->tx = p->x;
        js_particle(j, JP_PUFF, p->x / JS_FX, js_item_y(j, i) + 6);
        js_item_event(j, i, JE_BUMPED, 0);
        k = js_nearest_item(j, i, &d);
        if (k >= 0 && d <= 16) js_item_event(j, k, JE_BUMPED, 0);
      }
    }
  } else if (p->kind != JK_CRITTER && p->x != (int32_t)p->home_x * JS_FX) {
    /* decor knocked from its place drifts back to it */
    int32_t h = (int32_t)p->home_x * JS_FX;
    p->x += p->x < h ? (h - p->x < 32 ? h - p->x : 32) : -(p->x - h < 32 ? p->x - h : 32);
  }
  if (p->part) js_step_part(j, p);
  p->phase = (int16_t)((p->phase + (p->speed + 1)) & 1023);
  if (p->wait > 0) { p->wait--; p->moving = 0; return; }
  if (js_step_climb(j, i)) return;
  js_range(p, &lo, &hi);
  switch (p->move) {
  case JM_SITS:
    if (--p->t <= 0) {
      if (p->cur && p->nframes > 1) { p->cur = 0; p->t = (int16_t)js_rr(j, 120, 320); }
      else if (p->nframes > 1) { p->cur = 1; p->t = 8; }
      else p->t = 200;
    }
    break;
  case JM_HOPS:
    if (p->yoff > 0) {                          /* moving only while off the ground */
      uint8_t f = p->flip;
      js_walk(&p->x, p->tx, spd * 2, &f);
      p->flip = f;
    } else if (--p->t <= 0) {
      if (js_maybe_climb(j, i)) break;
      p->tx = (int32_t)js_clamp(p->x / JS_FX + js_rr(j, -10, 10), lo, hi) * JS_FX;
      js_act(j, i, JA_HOP, 5);
      p->t = (int16_t)js_rr(j, 60, 200);
    }
    break;
  case JM_WANDERS:
  case JM_FLOATS:
    if (p->kind == JK_HANGING) break;
    if (p->moving) {
      uint8_t f = p->flip;
      if (js_walk(&p->x, p->tx, spd, &f)) { p->moving = 0; p->t = (int16_t)js_rr(j, 40, 200); }
      p->flip = f;
      if (p->nframes > 1 && (j->steps % 6) == 0) p->cur = (uint8_t)((p->cur + 1) % p->nframes);
    } else if (--p->t <= 0) {
      if (js_maybe_climb(j, i)) break;
      p->tx = (int32_t)js_rr(j, lo, hi) * JS_FX;
      p->moving = 1;
    } else if (p->move == JM_FLOATS && p->nframes > 1 && (j->steps % 8) == 0) {
      p->cur = (uint8_t)((p->cur + 1) % p->nframes);   /* wings, tentacles */
    }
    break;
  case JM_SWAYS:
    if (p->kind != JK_HANGING && p->nframes > 1) p->cur = (uint8_t)((p->phase >> 7) & 1);
    break;
  }
}

/* Item ticks: the tick event, who is near, and liked things. */
static JS_OPT void js_item_tick(Jar *j) {
  int i, k, n = (int)(j->steps / JS_ITEM_TICK);
  for (i = 0; i < j->nplaced; i++) {
    JPlaced *p = &j->placed[i];
    int x = p->x / JS_FX, d, kind;
    js_item_event(j, i, JE_TICK, n);
    if (p->near_cd > 0) p->near_cd--;
    kind = js_nearest(j, i, &d);
    if (d <= 16 && p->near_cd == 0) {
      p->near_cd = 20;
      js_item_event(j, i, JE_NEAR, kind);
    }
    if (p->tags & JS_MOSS_LIKES)
      for (k = 0; k < j->nmoss; k++)
        if (js_abs(j->moss[k].x / JS_FX - x) <= 12 && j->moss[k].yoff == 0) {
          if (j->moss[k].boost <= 0) js_particle(j, JP_HEART, j->moss[k].x / JS_FX, JS_SOIL - 14);
          j->moss[k].boost = JS_BOOST_STEPS;
        }
    if ((p->tags & JS_SNAIL_LIKES) && j->snail.st != S_AWAY &&
        js_abs(j->snail.x / JS_FX - x) <= 12) {
      if (j->snail.boost <= 0) js_particle(j, JP_HEART, j->snail.x / JS_FX, JS_SOIL - 16);
      j->snail.boost = JS_BOOST_STEPS;
    }
  }
}

/* Two critters meet and pass the time of day. */
static JS_OPT int js_who_x(const Jar *j, int who) {
  if (who < JS_MAX_MOSS) return j->moss[who].x / JS_FX;
  if (who == JW_SNAIL) return j->snail.st == S_AWAY ? -999 : j->snail.x / JS_FX;
  return j->placed[who - JW_ITEM].x / JS_FX;
}

/* The level someone stands on: the workers the soil; an item climbing,
 * none (99) -- it is busy holding on. */
static JS_OPT int js_who_lv(const Jar *j, int who) {
  if (who < JW_ITEM) return 0;
  return j->placed[who - JW_ITEM].climb ? 99 : j->placed[who - JW_ITEM].level;
}

static JS_OPT int js_can_chat(const Jar *j, int who) {
  if (who < JS_MAX_MOSS) return who < j->nmoss && j->moss[who].st != M_NAP && j->moss[who].yoff == 0;
  if (who == JW_SNAIL) return j->snail.st != S_AWAY && j->snail.x / JS_FX < JS_DOOR_X - 8;
  return j->placed[who - JW_ITEM].kind == JK_CRITTER && j->placed[who - JW_ITEM].nbub > 0;
}

static JS_OPT void js_say_any(Jar *j, int who) {
  if (who >= JW_ITEM) js_say(j, who, js_rr(j, 0, j->placed[who - JW_ITEM].nbub - 1));
  else js_say(j, who, js_rr(j, 0, JS_NSAY - 1));
}

static JS_OPT void js_step_chat(Jar *j) {
  int a, b, n = JW_ITEM + j->nplaced;
  if (j->reply_t > 0 && --j->reply_t == 0 && j->reply_who >= 0) {
    js_say_any(j, j->reply_who);
    j->reply_who = -1;
  }
  if (--j->chat_cd > 0) return;
  j->chat_cd = (int16_t)(js_rr(j, 20, 45) * JS_HZ);
  for (a = 0; a < n; a++) {
    if (!js_can_chat(j, a)) continue;
    for (b = a + 1; b < n; b++) {
      if (!js_can_chat(j, b)) continue;
      if (js_abs(js_who_x(j, a) - js_who_x(j, b)) > 16) continue;
      if (js_who_lv(j, a) != js_who_lv(j, b)) continue;    /* one up a ledge, one below */
      js_say_any(j, a);
      j->reply_who = (int8_t)b;
      j->reply_t = (int16_t)(JS_HZ * 3 / 2);
      return;
    }
  }
  j->chat_cd = (int16_t)(3 * JS_HZ);               /* nobody close: look again soon */
}

static JS_OPT void js_step_air(Jar *j) {
  int i;
  for (i = 0; i < JS_PARTS; i++) {
    JPart *p = &j->part[i];
    if (!p->life) continue;
    p->life--;
    p->x = (int16_t)(p->x + p->vx);
    p->y = (int16_t)(p->y + p->vy);
    if (p->type == JP_ZZZ && (p->life & 15) == 0) p->vx = (int8_t)-p->vx;
  }
  for (i = 0; i < JS_FLOATS; i++)
    if (j->fl[i].life) { j->fl[i].life--; if ((j->fl[i].life & 1) == 0) j->fl[i].y--; }
  if (j->phase == PH_NIGHT || j->phase == PH_DUSK)
    for (i = 0; i < JS_FLIES; i++) {
      if (js_rr(j, 0, 15) == 0) {
        j->fly_vx[i] = (int8_t)js_rr(j, -4, 4);
        j->fly_vy[i] = (int8_t)js_rr(j, -3, 3);
      }
      j->fly_x[i] = (int16_t)js_clamp(j->fly_x[i] + j->fly_vx[i], 12 * 16, 226 * 16);
      j->fly_y[i] = (int16_t)js_clamp(j->fly_y[i] + j->fly_vy[i], 20 * 16, 100 * 16);
    }
}

/* A world event begins (JWD_*): set up, then every item hears `on world`. */
static JS_OPT void js_world_begin(Jar *j, int w) {
  int k;
  j->world = (uint8_t)w;
  switch (w) {
  case JWD_ANTS:
    j->world_t = 3 * 60 * JS_HZ;
    for (k = 0; k < JS_ANTS; k++) {
      j->ant_x[k] = (int16_t)((4 - 9 * k) * 16);     /* in single file, from the left */
      j->ant_st[k] = 1;
      j->ant_bed[k] = (uint8_t)(j->nbeds ? js_rr(j, 0, j->nbeds - 1) : 0);
    }
    js_say(j, 0, JS_SAY_ANTS);
    break;
  case JWD_LEAK:    j->world_t = 90 * JS_HZ; break;
  case JWD_BREEZE:  j->world_t = 60 * JS_HZ; break;
  case JWD_VISITOR:                                  /* leaves jars on the dock: coins are */
    j->world_t = 30 * JS_HZ;                         /* still only jars that left (js_ship) */
    j->dock += (uint32_t)(js_rr(j, 3, 8) + j->nplaced / 3);
    js_particle(j, JP_HEART, JS_PILE_X, JS_SOIL - 30);
    break;
  case JWD_DARK:    j->world_t = 60 * JS_HZ; break;
  case JWD_BLOOM:
    j->world_t = 10 * JS_HZ;
    for (k = 0; k < j->nbeds; k++)
      if (!j->bed[k].young && !j->bed[k].mould) { j->bed[k].ready = 1; j->bed[k].grow = JS_GROW_MS; }
    break;
  }
  js_event(j, JE_WORLD, w);
}

/* The world's clock: the event going on, the next one, the chores coming
 * due, signals, pellets. The weather outside makes a leak likelier. */
static JS_OPT void js_step_world(Jar *j) {
  int k;
  if (j->belt_boost > 0) j->belt_boost--;
  if (j->boost_cd > 0) j->boost_cd--;
  if (j->shake_t > 0) j->shake_t--;
  for (k = 0; k < JS_PELLETS; k++) if (j->pel_t[k]) j->pel_t[k]--;
  if (j->sig_n) {
    int n = j->sig_n, from = j->sig_from;
    j->sig_n = 0;
    for (k = 0; k < j->nplaced; k++) if (k != from) js_item_event(j, k, JE_SIGNAL, n);
  }
  if (j->world) {
    if (j->world == JWD_LEAK && j->puddle < 60 && (j->steps % (2 * JS_HZ)) == 0) j->puddle++;
    if (j->world == JWD_ANTS) {
      int left = 0;
      for (k = 0; k < JS_ANTS; k++) {
        int bx = JS_BED_X(j->ant_bed[k] < j->nbeds ? j->ant_bed[k] : 0) * 16;
        switch (j->ant_st[k]) {
        case 1:                                        /* marching in */
          left = 1;
          if (j->ant_x[k] < bx) { j->ant_x[k] += 5; break; }
          if (j->ant_bed[k] < j->nbeds && j->bed[j->ant_bed[k]].ready) {
            j->bed[j->ant_bed[k]].ready = 0;           /* a berry, carried off */
            j->bed[j->ant_bed[k]].claimed = 0;
            j->bed[j->ant_bed[k]].grow = 0;
            j->ant_st[k] = 2;
          } else {                                     /* gone: another ripe one, or home */
            int b, to = -1;
            for (b = 0; b < j->nbeds; b++) if (j->bed[b].ready) { to = b; break; }
            if (to >= 0) j->ant_bed[k] = (uint8_t)to;
            else j->ant_st[k] = 3;
          }
          break;
        case 2:                                        /* carrying off */
        case 3:                                        /* fleeing */
          left = 1;
          j->ant_x[k] -= j->ant_st[k] == 3 ? 14 : 4;
          if (j->ant_x[k] < -8 * 16) j->ant_st[k] = 0;
          break;
        }
      }
      if (!left) j->world_t = 0;
    }
    if (--j->world_t <= 0) {
      js_scatter_ants(j, 0, -1);
      for (k = 0; k < JS_ANTS; k++) j->ant_st[k] = 0;
      j->world = 0;
    }
  } else if (--j->world_clock <= 0) {
    static const uint8_t WEIGHT[JWD_KINDS] = { 0, 3, 2, 3, 3, 2, 2 };
    int total = 0, r, w = JWD_BREEZE;
    for (k = 1; k < JWD_KINDS; k++)
      total += WEIGHT[k] + (k == JWD_LEAK && j->weather >= 4 ? 4 : 0);   /* rainy, snowy, stormy */
    r = js_rr(j, 0, total - 1);
    for (k = 1; k < JWD_KINDS; k++) {
      r -= WEIGHT[k] + (k == JWD_LEAK && j->weather >= 4 ? 4 : 0);
      if (r < 0) { w = k; break; }
    }
    j->world_clock = js_rr(j, 20, 60) * 60 * JS_HZ;
    js_world_begin(j, w);
  }
  if (!j->sulk && --j->sulk_clock <= 0) {
    j->sulk = 1;
    j->sulk_clock = js_rr(j, 40, 80) * 60 * JS_HZ;
    js_say(j, JW_SNAIL, JS_SAY_SIGH);
  }
  if (--j->mould_clock <= 0) {
    j->mould_clock = js_rr(j, 60, 120) * 60 * JS_HZ;
    if (j->nbeds) j->bed[js_rr(j, 0, j->nbeds - 1)].mould = 1;
  }
}

/* One step of jar time. */
static JS_OPT void js_step(Jar *j) {
  int k;
  j->steps++;
  if (j->nap_clock > 0) j->nap_clock--;
  js_step_world(j);
  js_step_beds(j);
  for (k = 0; k < j->nmoss; k++) js_step_moss(j, k);
  js_step_works(j);
  js_step_snail(j);
  js_step_jam(j);
  for (k = 0; k < j->nplaced; k++) js_step_item(j, k);
  if (j->steps % JS_ITEM_TICK == 0) js_item_tick(j);
  js_step_chat(j);
  js_step_air(j);
}

/* ---- the save ------------------------------------------------------------ */

static JS_OPT int js_put(char *buf, int n, int cap, const char *s) {
  while (*s && n < cap - 1) buf[n++] = *s++;
  buf[n] = 0;
  return n;
}

static JS_OPT int js_put_u(char *buf, int n, int cap, uint32_t v) {
  char d[11];
  int k = 0;
  do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (k && n < cap - 1) buf[n++] = d[--k];
  buf[n] = 0;
  return n;
}

/* Text, a number, text: one call where it was three, for the size budget. */
static JS_SMALL int js_put3(char *buf, int n, int cap, const char *pre, uint32_t v, const char *post) {
  n = js_put(buf, n, cap, pre);
  n = js_put_u(buf, n, cap, v);
  return js_put(buf, n, cap, post);
}

/* The jar as text, into buf. Its length; cap should be 2 KB. */
static JS_OPT int js_save(const Jar *j, char *buf, int cap) {
  int n = 0, i;
#define JS_KV(key, v) (n = js_put3(buf, n, cap, key " ", (uint32_t)(v), "\n"))
  n = js_put(buf, n, cap, "jar 1\n");
  JS_KV("coins", j->coins);
  JS_KV("shipped", j->shipped);
  JS_KV("seen", j->seen);
  JS_KV("dock", j->dock + j->snail.load);     /* a load out of the door counts as on the dock */
  JS_KV("sold", j->sold);
  n = js_put(buf, n, cap, "up ");
  n = js_put_u(buf, n, cap, j->nbeds);  n = js_put(buf, n, cap, " ");
  n = js_put_u(buf, n, cap, j->nmoss);  n = js_put(buf, n, cap, " ");
  n = js_put_u(buf, n, cap, j->nmach);  n = js_put(buf, n, cap, " ");
  n = js_put_u(buf, n, cap, j->belt);   n = js_put(buf, n, cap, "\ngrow");
  for (i = 0; i < j->nbeds; i++) { n = js_put(buf, n, cap, " "); n = js_put_u(buf, n, cap, (uint32_t)j->bed[i].grow); }
  n = js_put(buf, n, cap, "\nplant");
  for (i = 0; i < j->nbeds; i++) {
    n = js_put(buf, n, cap, " "); n = js_put_u(buf, n, cap, j->bed[i].type);
    n = js_put(buf, n, cap, " "); n = js_put_u(buf, n, cap, j->bed[i].at);
  }
  n = js_put(buf, n, cap, "\nshelf");
  for (i = 0; i < JS_SHELF; i++) { n = js_put(buf, n, cap, " "); n = js_put_u(buf, n, cap, j->shelf[i]); }
  n = js_put(buf, n, cap, "\n");
  JS_KV("parcels", j->parcels);
  JS_KV("gseen", j->gseen);
  if (j->decor) JS_KV("decor", j->decor);
  n = js_put(buf, n, cap, "own");
  for (i = 0; i < j->nowned; i++) { n = js_put(buf, n, cap, " "); n = js_put_u(buf, n, cap, j->owned[i]); }
  n = js_put(buf, n, cap, "\n");
  /* What is in the jar, and what a save said was in it and has not been put
   * back (the companion app, which loads the jar without placing anything,
   * must not lose it). "place ID X Y", and the level after it when it is
   * up a ledge: a reader that does not know levels ignores the fourth
   * number, and a line without one is on the soil. */
  for (i = 0; i < j->nplaced + j->nwant; i++) {
    uint32_t id = i < j->nplaced ? j->placed[i].id : j->want[i - j->nplaced].id;
    int x = i < j->nplaced ? j->placed[i].home_x : j->want[i - j->nplaced].x;
    int y = i < j->nplaced ? j->placed[i].home_y : j->want[i - j->nplaced].y;
    int lv = i < j->nplaced ? j->placed[i].level : j->want[i - j->nplaced].lv;
    uint32_t on = i < j->nplaced ? j->placed[i].on_id : j->want[i - j->nplaced].on;
    if (i >= j->nplaced && js_find(j, id) >= 0) continue;
    n = js_put(buf, n, cap, "place ");
    n = js_put_u(buf, n, cap, id);           n = js_put(buf, n, cap, " ");
    n = js_put_u(buf, n, cap, (uint32_t)x);  n = js_put(buf, n, cap, " ");
    n = js_put_u(buf, n, cap, (uint32_t)y);
    if (lv || on) { n = js_put(buf, n, cap, " "); n = js_put_u(buf, n, cap, (uint32_t)lv); }
    if (on) { n = js_put(buf, n, cap, " "); n = js_put_u(buf, n, cap, on); }   /* on furniture */
    n = js_put(buf, n, cap, "\n");
  }
  /* the chores, so closing the jar does not do them */
  if (j->sulk || j->puddle || js_chores(j) & JC_MOULD) {
    uint32_t m = 0;
    for (i = 0; i < j->nbeds; i++) if (j->bed[i].mould) m |= 1u << i;
    n = js_put3(buf, n, cap, "chores ", j->sulk, " ");
    n = js_put3(buf, n, cap, "", j->puddle, " ");
    n = js_put3(buf, n, cap, "", m, "\n");
  }
#undef JS_KV
  return n;
}

static JS_OPT int js_word_is(const char *p, const char *w) {
  while (*w) if (*p++ != *w++) return 0;
  return *p == ' ' || *p == '\n' || *p == 0;
}

static JS_OPT uint32_t js_num(const char **pp) {
  const char *p = *pp;
  uint32_t v = 0;
  while (*p == ' ') p++;
  while (*p >= '0' && *p <= '9') v = v * 10u + (uint32_t)(*p++ - '0');
  *pp = p;
  return v;
}

static JS_OPT int js_more(const char *p) {
  while (*p == ' ') p++;
  return *p >= '0' && *p <= '9';
}

/* A saved jar into `j` (already js_init'ed). The items it had placed are
 * left in j->want for the caller, which has the card. 0, or -1 if this is
 * not a save. */
static JS_OPT int js_load(Jar *j, const char *t) {
  const char *p = t;
  if (!js_word_is(p, "jar")) return -1;
  while (*p) {
    const char *w = p;
    while (*p && *p != ' ' && *p != '\n') p++;
    if (js_word_is(w, "coins")) j->coins = js_num(&p);
    else if (js_word_is(w, "shipped")) j->shipped = js_num(&p);
    else if (js_word_is(w, "seen")) j->seen = js_num(&p);
    else if (js_word_is(w, "dock")) j->dock = js_num(&p);
    else if (js_word_is(w, "sold")) j->sold = js_num(&p);
    else if (js_word_is(w, "up")) {
      j->nbeds = (uint8_t)js_clamp((int)js_num(&p), 2, JS_MAX_BEDS);
      j->nmoss = (uint8_t)js_clamp((int)js_num(&p), 1, JS_MAX_MOSS);
      j->nmach = (uint8_t)js_clamp((int)js_num(&p), 2, JS_MAX_MACH);
      j->belt = (uint8_t)js_clamp((int)js_num(&p), 0, JS_MAX_BELT);
    } else if (js_word_is(w, "grow")) {
      int i = 0;
      while (js_more(p)) {
        int32_t g = (int32_t)js_num(&p);
        if (i < JS_MAX_BEDS) {
          j->bed[i].grow = g > JS_GROW_MS ? JS_GROW_MS : g;
          j->bed[i].ready = j->bed[i].grow >= JS_GROW_MS;
          i++;
        }
      }
    } else if (js_word_is(w, "plant")) {
      int i = 0;
      while (js_more(p)) {
        uint32_t ty = js_num(&p), at = js_num(&p);
        if (i < JS_MAX_BEDS) {
          j->bed[i].type = (uint8_t)(ty % JPL_KINDS);
          j->bed[i].at = at;
          j->bed[i].young = (uint8_t)(at != 0);   /* js_settle_beds decides */
          if (at) { j->bed[i].ready = 0; j->bed[i].grow = 0; }
          i++;
        }
      }
    } else if (js_word_is(w, "shelf")) {
      int i = 0;
      while (js_more(p)) { uint32_t id = js_num(&p); if (i < JS_SHELF) j->shelf[i++] = id; }
    } else if (js_word_is(w, "parcels")) j->parcels = (uint16_t)js_num(&p);
    else if (js_word_is(w, "gseen")) j->gseen = js_num(&p);
    else if (js_word_is(w, "decor")) j->decor = js_num(&p);
    else if (js_word_is(w, "chores")) {
      uint32_t m;
      int b;
      j->sulk = (uint8_t)(js_num(&p) != 0);
      j->puddle = (uint8_t)js_clamp((int)js_num(&p), 0, 60);
      m = js_num(&p);
      for (b = 0; b < JS_MAX_BEDS; b++) j->bed[b].mould = (uint8_t)((m >> b) & 1);
    }
    else if (js_word_is(w, "own")) {
      while (js_more(p)) js_own(j, js_num(&p));
    } else if (js_word_is(w, "place")) {
      uint32_t id = js_num(&p);
      int x = (int)js_num(&p), y = (int)js_num(&p);
      uint32_t lv = js_more(p) ? js_num(&p) : 0;   /* none: on the soil */
      uint32_t on = js_more(p) ? js_num(&p) : 0;   /* the furniture it stands on */
      if (j->nwant < JS_MAX_PLACED && id) {
        j->want[j->nwant].id = id;
        j->want[j->nwant].on = on;
        j->want[j->nwant].x = (int16_t)x;
        j->want[j->nwant].y = (int8_t)js_clamp(y, 0, 60);   /* a string's length */
        j->want[j->nwant].lv = (uint8_t)(lv < JS_LEVELS ? lv : JS_LEVELS - 1);
        j->nwant++;
      }
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
  if (j->dock > 6) j->pile0 = j->dock;
  return 0;
}

#endif /* CARDOS_JARSIM_H */
