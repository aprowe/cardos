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
 * from, so every coin is a jar someone could have watched go (away
 * earnings too: they are a pile of jars on the dock, shipped the same way).
 *
 * THE RATE. js_rate_ph is what the factory can do an hour: the least of what
 * the beds grow, the mosslings carry and the machines turn out, less a fifth
 * for naps and jams. It is the number on the top bar and what away time is
 * paid at.
 *
 * TIME. One step is JS_STEP_MS of jar time; positions are 1/64 pixel. An
 * item's tick, the event, is every JS_ITEM_TICK steps -- four a second.
 * Nothing here reads a clock: the app hands in the minute of the day (or -1
 * when there is no clock) and the epoch for away time.
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
#else
#define JS_OPT
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
#define JS_AWAY_CAP_S  (8u * 3600u)

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

/* The machines, in order along the belt: vat, press, spool, crane. */
static const int16_t JS_MACH_X[JS_MAX_MACH] = { 108, 132, 154, 170 };

/* What the critters like (an item's tags, as bits). */
enum { JT_COSY = 1, JT_SOFT = 2, JT_SWEET = 4, JT_GLOWING = 8, JT_SLEEPY = 16,
       JT_FOOD = 32, JT_ROUND = 64, JT_SPOOKY = 128, JT_FANCY = 256, JT_SPIKY = 512 };
#define JS_MOSS_LIKES  (JT_COSY | JT_SOFT | JT_SWEET | JT_GLOWING)
#define JS_SNAIL_LIKES (JT_SLEEPY | JT_ROUND | JT_SPOOKY | JT_FOOD)

enum { JU_MOSS = 0, JU_MACH, JU_BELT, JU_BED, JU_KINDS };
enum { PH_NONE = 0, PH_DAWN, PH_DAY, PH_DUSK, PH_NIGHT };
enum { JSN_X = 0, JSN_ZONE, JSN_NEAR_KIND, JSN_NEAR_DIST, JSN_TIME, JSN_WEATHER,
       JSN_DAYS, JSN_GIFT, JSN_RANDOM, JSN_KINDS };

enum { M_IDLE = 0, M_TO_BED, M_PICK, M_TO_VAT, M_TO_NAP, M_NAP, M_TO_FIX, M_FIX };
enum { S_HOME = 0, S_OUT, S_AWAY, S_BACK, S_TO_FIX, S_FIX };

/* Who said what: 0..5 the mosslings, 6 the snail, 8.. a placed item. */
#define JW_SNAIL 6
#define JW_ITEM  8

static const char *const JS_SAYINGS[] = {
  "hi!", "busy busy", "nice jam", "ooh", "hello", "jam time", "sticky!",
  "yum", "phew", "heave ho", "uh oh", "fixed!",
};
#define JS_NSAY 10                     /* the first ten are small talk */
#define JS_SAY_UHOH 10
#define JS_SAY_FIXED 11

/* ---- the state --------------------------------------------------------- */

/* A bed. `type` is what grows in it (JPL_*); `at` is when it was planted, in
 * UTC seconds -- 0 for a plant that is grown (the starting beds, and every
 * bed before the garden existed), 1 for one planted with no clock, which
 * starts counting when a clock arrives. A young plant grows no berries. */
typedef struct { int32_t grow; uint32_t at; uint8_t ready, claimed, type, young; } JBed;

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
  uint8_t  cur, flip, glow, moving, dirty;
  int8_t   say;
  /* phase 2: its script in j->spool, 0 bytes for none; mem changed since
   * the item's record was last written (js_mem_sync) */
  uint16_t soff, slen;
  uint8_t  mem_dirty;
} JPlaced;

typedef struct { uint32_t id; int16_t x, y; } JWant;

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
  uint32_t steps, seed, epoch;         /* epoch: now, for senses; 0 unknown */
} Jar;

/* ---- small things ------------------------------------------------------ */

static JS_OPT uint32_t js_rnd(Jar *j) {
  uint32_t x = j->seed ? j->seed : 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  j->seed = x;
  return x;
}

/* lo..hi inclusive. */
static JS_OPT int js_rr(Jar *j, int lo, int hi) {
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
static JS_OPT int js_walk(int32_t *x, int32_t tx, int spd, uint8_t *face) {
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
static JS_OPT int js_stage_steps(int nmach, int belt) {
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

static JS_OPT void js_particle(Jar *j, int type, int x, int y) {
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

/* "cosy,soft" -> JT_COSY | JT_SOFT */
static JS_OPT uint16_t js_tags(const char *s) {
  static const char *const W[] = { "cosy", "soft", "sweet", "glowing", "sleepy",
                                   "food", "round", "spooky", "fancy", "spiky" };
  uint16_t m = 0;
  while (*s) {
    int n = 0, k;
    while (s[n] && s[n] != ',') n++;
    for (k = 0; k < 10; k++) if (js_tag_word(s, n, W[k])) m |= (uint16_t)(1u << k);
    s += n;
    if (*s == ',') s++;
  }
  return m;
}

/* The x range an item keeps to: a decoration near where it was put, a
 * critter in its favourite zone. */
static JS_OPT void js_range(const JPlaced *p, int *lo, int *hi) {
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

static JS_OPT int js_find(const Jar *j, uint32_t id) {
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
static JS_OPT void js_unplace(Jar *j, int i) {
  int k;
  if (i < 0 || i >= j->nplaced) return;
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

/* Move a placed item somewhere else along its line. */
static JS_OPT void js_move_to(Jar *j, int i, int x, int y) {
  JPlaced *p = &j->placed[i];
  x = p->kind == JK_HANGING ? js_clamp(x, JS_HANG_LO, JS_HANG_HI) : js_clamp(x, 10, 226);
  p->home_x = (int16_t)x;
  if (p->kind == JK_HANGING) p->home_y = (int16_t)js_clamp(y, 0, 60);
  p->x = p->tx = (int32_t)x * JS_FX;
  p->yoff = 0;
  p->vy = 0;
  p->moving = 0;
}

/* ---- items: senses, actions, events -------------------------------------- */

static JS_OPT int js_item_y(const Jar *j, int i) {     /* its middle, px */
  const JPlaced *p = &j->placed[i];
  if (p->kind == JK_HANGING) return JS_LID + p->home_y + 8;
  return JS_SOIL - 8 - p->home_y - p->yoff / 16;
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
    if (k == i) continue;
    d = js_abs(j->placed[k].x / JS_FX - x) + js_abs(js_item_y(j, k) - y) / 2;
    if (d < best) { best = d; kind = j->placed[k].kind == JK_CRITTER ? JN_CRITTER : JN_DECOR; }
  }
  if (dist) *dist = best;
  return kind;
}

/* What item i can read (spec, "Senses"). Phase 2's scripts read these. */
static JS_OPT int js_sense(Jar *j, int i, int sense) {
  const JPlaced *p = &j->placed[i];
  int d;
  switch (sense) {
  case JSN_X:         return p->x / JS_FX;
  case JSN_ZONE:      return js_zone_of(p->x / JS_FX);
  case JSN_NEAR_KIND: return js_nearest(j, i, 0);
  case JSN_NEAR_DIST: js_nearest(j, i, &d); return d;
  case JSN_TIME:      return j->phase;
  case JSN_WEATHER:   return 0;                       /* not known offline */
  case JSN_DAYS:      return (j->epoch && p->made && j->epoch > p->made)
                             ? (int)((j->epoch - p->made) / 86400u) : 0;
  case JSN_GIFT:      return (p->flags & JIF_GIFT) ? 1 : 0;
  case JSN_RANDOM:    return (int)(js_rnd(j) & 255);
  default:            return 0;
  }
}

static JS_OPT int js_speed(const JPlaced *p) {
  return p->speed == JSP_SLOW ? 5 : p->speed == JSP_FAST ? 19 : 10;
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
    {
      JPlaced tmp;
      tmp.kind = JK_CRITTER;
      tmp.zone = (uint8_t)(arg < JZ_KINDS ? arg : JZ_ANYWHERE);
      tmp.home_x = p->home_x;
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
    } else if (hb->earg && hb->earg != arg && (ev == JE_NEAR || ev == JE_JAM || ev == JE_TIME)) {
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
  j->chat_cd = (int16_t)(15 * JS_HZ);
  j->reply_who = -1;
  for (i = 0; i < JS_FLIES; i++) {
    j->fly_x[i] = (int16_t)(js_rr(j, 20, 220) * 16);
    j->fly_y[i] = (int16_t)(js_rr(j, 24, 100) * 16);
  }
}

/* Time away: the jars the factory would have made since the last save,
 * capped at eight hours, put on the dock as a pile for the snail; and the
 * plants grown. The number of jars. No clock (either epoch 0), or a clock
 * that went backwards: nothing. */
static JS_OPT uint32_t js_away(Jar *j, uint32_t now) {
  uint32_t secs, jars;
  int i;
  if (!now || !j->seen || now <= j->seen) { if (now) j->seen = now; return 0; }
  secs = now - j->seen;
  if (secs > JS_AWAY_CAP_S) secs = JS_AWAY_CAP_S;
  jars = (uint32_t)js_rate_ph(j) * secs / 3600u;
  j->dock += jars;
  if (j->dock > 6) j->pile0 = j->dock;
  for (i = 0; i < j->nbeds; i++) {
    int32_t g = j->bed[i].grow + (int32_t)(secs > 3600 ? 3600 : secs) * 1000;
    if (j->bed[i].young) continue;
    j->bed[i].grow = g > JS_GROW_MS ? JS_GROW_MS : g;
    if (j->bed[i].grow >= JS_GROW_MS) j->bed[i].ready = 1;
  }
  j->seen = now;
  return jars;
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
    if (b->ready || b->young) continue;
    b->grow += JS_STEP_MS;
    if (b->grow >= JS_GROW_MS) { b->grow = JS_GROW_MS; b->ready = 1; }
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
    if (js_walk(&m->x, m->tx, spd, &m->face)) { m->st = M_PICK; m->t = JS_PICK_STEPS; }
    break;
  case M_PICK:
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
  /* The belt: units ride to their machine and go in when it is free. */
  if (!j->jammed) j->belt_pos += JS_BELT_SPD[j->belt];
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
      u->x += JS_BELT_SPD[j->belt];
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

static JS_OPT void js_step_item(Jar *j, int i) {
  JPlaced *p = &j->placed[i];
  int lo, hi, spd = js_speed(p);
  if (p->say_t > 0) p->say_t--;
  /* in the air */
  if (p->vy || p->yoff) {
    p->yoff = (int16_t)(p->yoff + p->vy);
    p->vy = (int16_t)(p->vy - 6);
    if (p->yoff <= 0) { p->yoff = 0; p->vy = 0; }
  }
  p->phase = (int16_t)((p->phase + (p->speed + 1)) & 1023);
  if (p->wait > 0) { p->wait--; p->moving = 0; return; }
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

/* One step of jar time. */
static JS_OPT void js_step(Jar *j) {
  int k;
  j->steps++;
  if (j->nap_clock > 0) j->nap_clock--;
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

/* The jar as text, into buf. Its length; cap should be 2 KB. */
static JS_OPT int js_save(const Jar *j, char *buf, int cap) {
  int n = 0, i;
#define JS_KV(key, v) do { n = js_put(buf, n, cap, key " "); n = js_put_u(buf, n, cap, (uint32_t)(v)); \
                           n = js_put(buf, n, cap, "\n"); } while (0)
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
   * must not lose it). */
  for (i = 0; i < j->nplaced + j->nwant; i++) {
    uint32_t id = i < j->nplaced ? j->placed[i].id : j->want[i - j->nplaced].id;
    int x = i < j->nplaced ? j->placed[i].home_x : j->want[i - j->nplaced].x;
    int y = i < j->nplaced ? j->placed[i].home_y : j->want[i - j->nplaced].y;
    if (i >= j->nplaced && js_find(j, id) >= 0) continue;
    n = js_put(buf, n, cap, "place ");
    n = js_put_u(buf, n, cap, id);           n = js_put(buf, n, cap, " ");
    n = js_put_u(buf, n, cap, (uint32_t)x);  n = js_put(buf, n, cap, " ");
    n = js_put_u(buf, n, cap, (uint32_t)y);  n = js_put(buf, n, cap, "\n");
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
    else if (js_word_is(w, "own")) {
      while (js_more(p)) js_own(j, js_num(&p));
    } else if (js_word_is(w, "place")) {
      uint32_t id = js_num(&p);
      int x = (int)js_num(&p), y = (int)js_num(&p);
      if (j->nwant < JS_MAX_PLACED && id) {
        j->want[j->nwant].id = id;
        j->want[j->nwant].x = (int16_t)x;
        j->want[j->nwant].y = (int16_t)y;
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
