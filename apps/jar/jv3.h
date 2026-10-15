/* JarScript 3: the device machine (docs/superpowers/specs/
 * 2026-10-14-jarscript3-design.md is THE semantics; apps/jar/jarvm3.def is
 * THE table, built into the enums and tables below with X-macros).
 *
 * The server compiles the language to bytes (server/jar/jarvm3.py) and runs
 * the same machine; tools/make_jarvm3_fixtures.py writes worlds and the
 * Python machine's traces, and test/test_jv3.c replays them here and needs
 * them equal line for line. Where the spec left a choice, the choice is
 * written down below under DECISIONS.
 *
 * Portable and libc-free, no allocation, no floating point (an ESP32-S3 app
 * links no libgcc): int32 maths, no 64-bit multiply or divide. The caller
 * owns every byte: the Jv3Items, the scripts, the memory slots.
 *
 * THE WORLD a machine sees is a Jv3Jar: 24 slots, each an item in the jar
 * (scripted or not), the jar's ledges, one xorshift32 generator, the host's
 * callbacks. jv3_tick is one 1/20 s tick: near, run, move. Positions are
 * raw 12.4 jar coordinates (raw = px * 16): x the centre, y the feet. While
 * an item is scripted its position is the machine's; it copies it to the
 * slot after every tick. A host that moves an item itself (a gift, a throw)
 * writes the slot's x, y and calls jv3_moved (its path, walk, perch and
 * speed go); a slot found changed at the start of a tick is taken as the
 * item's new place even without the call. jv3_remove compacts the slots
 * when an item leaves, as the jar's list does.
 *
 * DECISIONS (each exact; server/jar/jarvm3.py makes the same ones, and the
 * fixtures hold both to them):
 *  - Integer helpers: asr(v, k) is floor(v / 2^k); ticks(raw) = asr(raw *
 *    20 + 8, 4); sat() clamps to -32768..32767; "/" in C is trunc. A handle
 *    is asr(raw, 4), good if 1..nslots and the slot is used; a trait is
 *    asr(raw, 4), good if 0..31. Squares are summed in unsigned 32 bits
 *    (wrapping) before isqrt. An item's centre: (x, y - h * scale * 8).
 *  - The check: flags other than bit 0, anim frames or FRAME K >= the
 *    item's nframes (jv3_check alone: 8), a SAY or LOG whose value count is
 *    not its string's 0x01 count (LOG 255: at most 16), a near radius or
 *    every period of 0, a chime outside 1..15 minutes: refused. A body's GO
 *    is checked as if it fell through (the compiler always follows it).
 *  - The queue: at the very start of a tick each item's queue is noted;
 *    only those events are delivered this tick. Everything queued during
 *    the tick -- near (phase 1), signals, land, hit, bump -- waits for the
 *    next. 8 new events at most (the rest are dropped); a stopped item is
 *    queued nothing. hit and bump carry the impact as arg.
 *  - Handler choice: the first in table order whose event matches, whose
 *    state is the current one or 255, and whose arg is 0 or the event's.
 *    chime: arg is minutes the jar has been open; matches when arg > 0 and
 *    arg % param == 0. near: arg is the index + 1 (in the handler table) of
 *    the near handler whose radius saw the arrival, and only that handler
 *    runs it, if it is still active (else the event is dropped) -- so two
 *    near handlers on one trait at different radii each hear only their
 *    own arrivals; the entry stays {ev, other, arg}. Running one sets
 *    other, and impact = arg for hit and bump, else 0; tick and every
 *    handlers run with other 0, impact 0. A handler that cannot start
 *    because the tick's budget is spent (<= 0) is not run: its event is
 *    dropped silently (no fault, other and impact untouched; an every
 *    counter at 0 still reloads). One that starts and runs out mid-way is
 *    a fault.
 *  - every: counters all start at their period when the item is placed;
 *    each tick an active one counts down and at 0 runs and reloads.
 *    Entering a state reloads only that state's every counters and clears
 *    only that state's near masks.
 *  - near: one 32-bit mask over slots per near handler; in range = another
 *    used slot with the trait at centre distance <= radius * 16; arrivals
 *    queued in slot order.
 *  - wait: WAIT sets max(1, ticks(T)) and yields; in step f a wait over 0
 *    counts down by one first and the body runs only if it is then 0. The
 *    trace's w= is that counter after the tick. JZY that yields gives back
 *    its own step (the condition's steps are spent).
 *  - Body GO: entered at once, the body run again; at most 4 runs a tick.
 *  - Faults: host->fault (may be NULL) with the instruction's pc; a handler
 *    out of budget faults at the pc it stopped on. 8: stopped for good.
 *  - Anims: PLAY restarting: t = 0, frame = first (if < nframes). Each move
 *    phase t += 1, k = t * fps / 20: a loop shows frames[k % n] (t kept
 *    below 20 n), a once frames[k] or, at k >= n, the last and is done. A
 *    frame >= nframes is never shown. PLAY of the looping anim already on
 *    keeps its place. FRAME K: only if K < nframes. FRAMEE: idx = round(N) /
 *    16 - 1; only if 0 <= idx < nframes. Either stops the anim.
 *  - Path: cx = asr(x0 + x1, 1), cy = asr(y0 + y1, 1) - 2 * arc (int32).
 *    B(s) is de Casteljau in 1/4096ths: lerp(a, b, s) = a + asr((b - a) s,
 *    12), B = lerp(lerp(P0, C, s), lerp(C, P1, s), s). Length: the sum of
 *    isqrt over 12 pieces at s = i * 4096 / 12. dur = max(3, ticks(len * 16
 *    / max(80, speed))) (no saturation); step = ceil(4096 / dur); each move
 *    phase T = min(4096, T + step), eased s = (((T T) >> 12) (12288 - 2T))
 *    >> 12, position B(s); at T == 4096 exactly (x1, y1). A path zeroes vx,
 *    vy, faces x1 - x0 if they differ, and leaves physics as it is (the
 *    mover runs instead of physics while it goes).
 *  - MOVE: xy as given; item: (ox, oy - oh * os * 16), nobody -> the
 *    ground; perch T > 0: each other used slot with trait T-1 and a good
 *    script, each of its perches in order; perch with no T: the 4 rim spots
 *    only. A spot anyone scripted is perched on (me included) is not free;
 *    the choice is rng % free; none free -> the ground: x = (14 + rng %
 *    213) * 16, y = ground(x). x clamped to [w*s*8, 3840 - w*s*8], y to
 *    [128, ground(x)]. Spot positions: declared (ox - ow*os*8 + px*os*16,
 *    oy - oh*os*16 + py*os*16), rim (rx * 16, ry * 16).
 *  - Hop: dx 0 on a perch, which is kept; x1 = clamp(x + dx, half + 32,
 *    3840 - half - 32), y1 = y, speed 640 raw, arc H as given. No land.
 *  - Walk: cancels a path, zeroes vx, vy, leaves the perch, clears blocked;
 *    |dx| >= 8 raw walks with step max(1, |S| / 20) a tick, d = min(left,
 *    step). Glass half + 32 from each wall (clamped, a stop). A solid
 *    (other slots' boxes, then the first 8 ledges) stops it when y > top +
 *    8, y - h*s*16 < bottom and the new span x +- half overlaps it and the
 *    old did not; off the ground (|y - ground(x)| > 16) on a solid's top,
 *    leaving that top's x range stops it. A stop keeps x (glass: clamped).
 *  - Physics, per tick, raw, "/" trunc: vy += gravity / 20; grounded and
 *    friction > 0: vx toward 0 by friction / 20; cx = x + vx / 20, cy = y -
 *    h*s*8 + vy / 20; r = body * 16 else max(16, min(w, h) * s * 8 - 16).
 *    Soil ground(cx), glass 48 / 3792, lid 128: clamped, the normal speed
 *    reflected * bounce / 1600 (bounce is raw percent, default 640); a
 *    floor bounce under 192 is 0; impacts over 240 queue hit, after the
 *    four walls, in order. Solids (other slots' declared boxes, the first 8
 *    ledges): closest point; d2 >= r r: none; d = isqrt(d2); d 0: normal (0,
 *    -1) and cy = top - r; else pushed out by dx (r - d) / d. vn = (vx nx +
 *    vy ny) / nd; vn < 0: t = vn (1600 + bounce) / 1600, vx -= t nx / nd,
 *    vy -= t ny / nd; a top (5 ny < -3 nd) with |vy| < 192 stops vy; -vn >
 *    160 and not touched last tick: bump (other = the slot + 1, 0 for a
 *    ledge) to me and to it. Touch bits: slot b bit b, ledge l bit 24 + l.
 *  - Turning: spin (when physics is off, or a mover ran) adds spin / 450
 *    steps a tick; roll adds (vx / 20) * 163 / (r * 4).
 *  - Move phase per item: path, else walk, else physics; spin; perch
 *    following (not while a path goes); without physics grounded = no
 *    perch, no mover and (|y - ground(x)| <= 16 or on a solid's top: |y -
 *    top| <= 16, x within the box +- 16); the anim; ticks in state; say,
 *    sway, shake count down.
 *  - Text: a template's 0x01 bytes take the values in order; log 255 is
 *    the values joined by spaces. say is cut to 24 characters, log is not.
 *    A number: whole -> "%d"; else hundredths = (|raw| * 25 + 2) / 4, "-"
 *    if negative, "I.FF" with one trailing zero removed.
 *  - say ticks = max(6, ticks(T)); sway ticks max(1, ticks(T)), amp clamped
 *    0..128 raw; shake max(0, ticks(T)); emit n = clamp(asr(N, 4), 0, 12),
 *    nothing when 0. FACE / TOWARD: any sign / any difference.
 *  - STP x, y leave a perch; bounce clamps 0..1600; friction >= 0; angle
 *    is the step.
 *  - angleto: its own table, J3_ANG[k] = round(sin(2 pi k / 256) * 16384)
 *    (halves away from zero), not the 1/16 J3_SIN that sin()/cos() read.
 *    dx, dy (raw centre differences) clamped to -65535..65535; the step k
 *    0..255 maximising dx * ANG[(k + 64) & 255] + dy * ANG[k], ties to the
 *    lowest, as degrees raw. int32 is enough: each product < 2^30, and the
 *    sum is at most |(dx, dy)| * 16385 < 2^31 (Cauchy-Schwarz).
 *  - The jar's tick counter goes up at the end of jv3_tick: `time` reads 0
 *    in the first tick, and the trace prints 1 after it.
 */
#ifndef CARDOS_JV3_H
#define CARDOS_JV3_H

#include <stdint.h>

#if defined(__GNUC__)
#define JV3_OPT __attribute__((unused))
#else
#define JV3_OPT
#endif

#define JV3_FORMAT 3
#define JV3_SLOTS  24
#define JV3_STACK  16
#define JV3_QUEUE  8
#define JV3_NEARS  4
#define JV3_EVERYS 6
#define JV3_SAY    24
#define JV3_FAULTS_STOP 8
#define JV3_MAX    1024          /* script bytes */
#define JV3_HEAD   14

/* ---- the table ------------------------------------------------------------ */

#define J3_OP(c, n, o, d)
#define J3_EVENT(v, w, f, d)
#define J3_SENSE(v, w, k, d)
#define J3_FUNC(v, w, lo, hi, d)
#define J3_FIELD(v, w, d)
#define J3_TARGET(v, w)
#define J3_ACTION(v, w, a, d)
#define J3_PARTICLE(v, w)
#define J3_NAME(g, w, v)

#undef J3_OP
#define J3_OP(c, n, o, d) J3O_##n = c,
enum {
#include "apps/jar/jarvm3.def"
  J3_NOPS
};
#undef J3_OP
#define J3_OP(c, n, o, d)

#undef J3_EVENT
#define J3_EVENT(v, w, f, d) J3E_##w = v,
enum {
#include "apps/jar/jarvm3.def"
  J3E_END_
};
#undef J3_EVENT
#define J3_EVENT(v, w, f, d) + 1
enum { J3_NEVENTS = 0
#include "apps/jar/jarvm3.def"
};
#undef J3_EVENT
#define J3_EVENT(v, w, f, d)

#undef J3_SENSE
#define J3_SENSE(v, w, k, d) J3S_##w = v,
enum {
#include "apps/jar/jarvm3.def"
  J3_NSENSES
};
#undef J3_SENSE
#define J3_SENSE(v, w, k, d)

#undef J3_FUNC
#define J3_FUNC(v, w, lo, hi, d) J3F_##w = v,
enum {
#include "apps/jar/jarvm3.def"
  J3_NFUNCS
};
#undef J3_FUNC
#define J3_FUNC(v, w, lo, hi, d)

#undef J3_FIELD
#define J3_FIELD(v, w, d) J3D_##w = v,
enum {
#include "apps/jar/jarvm3.def"
  J3_NFIELDS
};
#undef J3_FIELD
#define J3_FIELD(v, w, d)

#undef J3_TARGET
#define J3_TARGET(v, w) J3T_##w = v,
enum {
#include "apps/jar/jarvm3.def"
  J3_NTARGETS
};
#undef J3_TARGET
#define J3_TARGET(v, w)

#undef J3_ACTION
#define J3_ACTION(v, w, a, d) + 1
enum { J3_NACTIONS = 0
#include "apps/jar/jarvm3.def"
};
#undef J3_ACTION
#define J3_ACTION(v, w, a, d)

#undef J3_PARTICLE
#define J3_PARTICLE(v, w) + 1
enum { J3_NPARTICLES = 0
#include "apps/jar/jarvm3.def"
};
#undef J3_PARTICLE
#define J3_PARTICLE(v, w)

/* Operand bytes per op. */
#undef J3_OP
#define J3_OP(c, n, o, d) (uint8_t)(o),
static JV3_OPT const uint8_t J3_OPLEN[J3_NOPS] = {
#include "apps/jar/jarvm3.def"
};
#undef J3_OP
#define J3_OP(c, n, o, d) #n,
static JV3_OPT const char *const J3_OPNAME[J3_NOPS] = {
#include "apps/jar/jarvm3.def"
};
#undef J3_OP
#define J3_OP(c, n, o, d)

/* 1 for a sense a script may write. */
#define J3_KIND_rw 1
#define J3_KIND_ro 0
#define J3_KIND_jar 2
#undef J3_SENSE
#define J3_SENSE(v, w, k, d) (uint8_t)J3_KIND_##k,
static JV3_OPT const uint8_t J3_SKIND[J3_NSENSES] = {
#include "apps/jar/jarvm3.def"
};
#undef J3_SENSE
#define J3_SENSE(v, w, k, d)

#undef J3_FUNC
#define J3_FUNC(v, w, lo, hi, d) (uint8_t)((lo) | ((hi) << 4)),
static JV3_OPT const uint8_t J3_FARGS[J3_NFUNCS] = {
#include "apps/jar/jarvm3.def"
};
#undef J3_FUNC

#undef J3_OP
#undef J3_EVENT
#undef J3_SENSE
#undef J3_FIELD
#undef J3_TARGET
#undef J3_ACTION
#undef J3_PARTICLE
#undef J3_NAME

/* SIN[k] = round(sin(2 pi k / 256) * 16), halves away from zero: raw. */
static JV3_OPT const int8_t J3_SIN[256] = {
  0,0,1,1,2,2,2,3,3,4,4,4,5,5,5,6,6,6,7,7,8,8,8,9,9,9,10,10,10,10,11,11,
  11,12,12,12,12,13,13,13,13,14,14,14,14,14,14,15,15,15,15,15,15,15,16,16,16,16,16,16,16,16,16,16,
  16,16,16,16,16,16,16,16,16,16,16,15,15,15,15,15,15,15,14,14,14,14,14,14,13,13,13,13,12,12,12,12,
  11,11,11,10,10,10,10,9,9,9,8,8,8,7,7,6,6,6,5,5,5,4,4,4,3,3,2,2,2,1,1,0,
  0,0,-1,-1,-2,-2,-2,-3,-3,-4,-4,-4,-5,-5,-5,-6,-6,-6,-7,-7,-8,-8,-8,-9,-9,-9,-10,-10,-10,-10,-11,-11,
  -11,-12,-12,-12,-12,-13,-13,-13,-13,-14,-14,-14,-14,-14,-14,-15,-15,-15,-15,-15,-15,-15,-16,-16,-16,-16,-16,-16,-16,-16,-16,-16,
  -16,-16,-16,-16,-16,-16,-16,-16,-16,-16,-16,-15,-15,-15,-15,-15,-15,-15,-14,-14,-14,-14,-14,-14,-13,-13,-13,-13,-12,-12,-12,-12,
  -11,-11,-11,-10,-10,-10,-10,-9,-9,-9,-8,-8,-8,-7,-7,-6,-6,-6,-5,-5,-5,-4,-4,-4,-3,-3,-2,-2,-2,-1,-1,0,
};

/* ANG[k] = round(sin(2 pi k / 256) * 16384), halves away from zero: angleto's
 * table (server/jar/jarvm3.py ANG, D24). */
static JV3_OPT const int16_t J3_ANG[256] = {
  0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,5139,5520,5897,
  6270,6639,7005,7366,7723,8076,8423,8765,9102,9434,9760,10080,10394,10702,11003,11297,
  11585,11866,12140,12406,12665,12916,13160,13395,13623,13842,14053,14256,14449,14635,14811,14978,
  15137,15286,15426,15557,15679,15791,15893,15986,16069,16143,16207,16261,16305,16340,16364,16379,
  16384,16379,16364,16340,16305,16261,16207,16143,16069,15986,15893,15791,15679,15557,15426,15286,
  15137,14978,14811,14635,14449,14256,14053,13842,13623,13395,13160,12916,12665,12406,12140,11866,
  11585,11297,11003,10702,10394,10080,9760,9434,9102,8765,8423,8076,7723,7366,7005,6639,
  6270,5897,5520,5139,4756,4370,3981,3590,3196,2801,2404,2006,1606,1205,804,402,
  0,-402,-804,-1205,-1606,-2006,-2404,-2801,-3196,-3590,-3981,-4370,-4756,-5139,-5520,-5897,
  -6270,-6639,-7005,-7366,-7723,-8076,-8423,-8765,-9102,-9434,-9760,-10080,-10394,-10702,-11003,-11297,
  -11585,-11866,-12140,-12406,-12665,-12916,-13160,-13395,-13623,-13842,-14053,-14256,-14449,-14635,-14811,-14978,
  -15137,-15286,-15426,-15557,-15679,-15791,-15893,-15986,-16069,-16143,-16207,-16261,-16305,-16340,-16364,-16379,
  -16384,-16379,-16364,-16340,-16305,-16261,-16207,-16143,-16069,-15986,-15893,-15791,-15679,-15557,-15426,-15286,
  -15137,-14978,-14811,-14635,-14449,-14256,-14053,-13842,-13623,-13395,-13160,-12916,-12665,-12406,-12140,-11866,
  -11585,-11297,-11003,-10702,-10394,-10080,-9760,-9434,-9102,-8765,-8423,-8076,-7723,-7366,-7005,-6639,
  -6270,-5897,-5520,-5139,-4756,-4370,-3981,-3590,-3196,-2801,-2404,-2006,-1606,-1205,-804,-402,
};

/* The rim: where the glass narrows, spots anyone may perch on (px). */
static JV3_OPT const uint8_t J3_RIM[4][2] = { { 16, 22 }, { 32, 22 }, { 208, 22 }, { 224, 22 } };

/* ---- the C API ----------------------------------------------------------- */

typedef struct { uint8_t ev, other; int16_t arg; } Jv3Ev;

typedef struct Jv3Item Jv3Item;
struct Jv3Item {
  int16_t x, y, vx, vy;            /* raw */
  int16_t spin, gravity, friction, impact;
  int16_t stack[JV3_STACK];        /* the body's */
  int32_t m[6];                    /* path: x0 y0 x1 y1 cx cy; walk: left, step, dir */
  int16_t bounce;                  /* raw percent: 640 is 40 */
  int16_t sway_amp;
  uint16_t pc, wait, statet;       /* body pc, wait ticks, ticks in state */
  uint16_t mk, mdur;               /* path: t in 1/4096ths, its step a tick */
  uint16_t hoff, aoff, soff;       /* handlers, anims, strings */
  uint16_t anim_t;
  uint16_t every[JV3_EVERYS];
  uint16_t say_ticks, sway_ticks, sway_t0, shake_ticks;
  uint32_t nearm[JV3_NEARS];
  uint32_t touch;
  Jv3Ev q[2 * JV3_QUEUE];          /* the tick's events, then up to 8 new ones */
  uint8_t qn, qready, sp, state, pending, halted, faults, stopped;
  uint8_t anim, anim_once, anim_done, frame;  /* anim 255: none; frame 0-based */
  int8_t facing;                   /* 1 or -1 */
  uint8_t angle;                   /* step 0..255 */
  uint8_t mover, hop, spot_owner, spot_idx;   /* mover 0 none, 1 path, 2 walk; spot_idx 255: no perch aimed at */
  uint8_t perch, perch_owner;      /* perch 0 none, else index + 1; owner slot + 1, 0 the rim */
  uint8_t physics, roll, grounded, blocked, other;
  char say[JV3_SAY + 1];
};

typedef struct {
  Jv3Item *it;                     /* NULL: not scripted (others can still find, perch on, collide with it) */
  const uint8_t *s; uint16_t n;    /* its script (format 3) */
  int16_t *mem;                    /* its 8 vars: the record's memory slots */
  int16_t x, y;                    /* raw: centre x, feet y */
  uint8_t w, h, scale;
  uint8_t nframes;
  uint8_t used;
  uint32_t traits;
} Jv3Slot;

typedef struct { int16_t x, y, w, h; } Jv3Box;   /* raw */

typedef struct {
  void *ctx;
  int16_t (*sense)(void *ctx, int slot, int k);
  void (*act)(void *ctx, int slot, int k, int16_t arg);
  void (*say)(void *ctx, int slot, const char *text, int ticks);
  void (*emit)(void *ctx, int slot, int kind, int n);
  void (*log)(void *ctx, int slot, const char *text);   /* may be NULL */
  int16_t (*ground)(void *ctx, int16_t x);              /* NULL: soil 112*16 */
  void (*fault)(void *ctx, int slot, int pc);           /* may be NULL */
} Jv3Host;

typedef struct {
  Jv3Slot slot[JV3_SLOTS];
  int nslots;
  const Jv3Box *ledges; int nledges;
  uint32_t rng;
  uint32_t tick;
  int max_bodies;
  const Jv3Host *host;
} Jv3Jar;

/* ---- numbers ------------------------------------------------------------- */

static JV3_OPT int16_t j3_sat(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
static JV3_OPT int32_t j3_asr(int32_t v, int k) { return v >= 0 ? v >> k : ~((~v) >> k); }
static JV3_OPT int32_t j3_abs(int32_t v) { return v < 0 ? -v : v; }
static JV3_OPT int32_t j3_ticks(int32_t raw) { return j3_asr(raw * 20 + 8, 4); }
static JV3_OPT int32_t j3_clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
/* floor(a / b) for b > 0 */
static JV3_OPT int32_t j3_fdiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static JV3_OPT uint32_t j3_isqrt(uint32_t v) {
  uint32_t r = 0, bit = 1u << 30;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
    else r >>= 1;
    bit >>= 2;
  }
  return r;
}

static JV3_OPT int32_t j3_hyp(int32_t dx, int32_t dy) {
  uint32_t a = (uint32_t)dx, b = (uint32_t)dy;
  return (int32_t)j3_isqrt(a * a + b * b);
}

/* nearest whole, halves away from zero, on raw */
static JV3_OPT int16_t j3_round(int16_t v) {
  int32_t a = j3_abs(v);
  a = ((a + 8) >> 4) << 4;
  return j3_sat(v < 0 ? -a : a);
}

static JV3_OPT int16_t j3_mul(int16_t a, int16_t b) { return j3_sat(j3_asr((int32_t)a * b, 4)); }
static JV3_OPT int16_t j3_div(int16_t a, int16_t b) { return b ? j3_sat((int32_t)a * 16 / b) : 0; }
static JV3_OPT int16_t j3_mod(int16_t a, int16_t b) {
  int32_t q;
  if (!b) return 0;
  q = (int32_t)a / b;
  if ((int32_t)a % b != 0 && ((a < 0) != (b < 0))) q--;
  return j3_sat((int32_t)a - (int32_t)b * q);
}

/* degrees raw -> step 0..255, and back */
static JV3_OPT uint8_t j3_step(int16_t raw) { return (uint8_t)(j3_fdiv((int32_t)raw * 256, 5760) & 255); }
static JV3_OPT int16_t j3_deg(int k) { return (int16_t)((k * 45) >> 1); }

static JV3_OPT uint32_t j3_rnd(uint32_t *x) {
  uint32_t v = *x;
  v ^= v << 13; v ^= v >> 17; v ^= v << 5;
  *x = v;
  return v;
}

/* ---- the script's layout ------------------------------------------------- */

typedef struct { uint16_t states, handlers, anims, perches, strings, code; } Jv3Layout;

/* Where everything starts, or -1 if the header does not add up. */
static JV3_OPT int j3_layout(const uint8_t *s, int n, Jv3Layout *L) {
  int p, i;
  if (!s || n < JV3_HEAD || n > JV3_MAX || s[0] != JV3_FORMAT) return -1;
  if (s[2] > 8 || s[3] < 1 || s[3] > 16 || s[4] > 24 || s[5] > 8 || s[6] > 4 || s[7] > 16) return -1;
  p = JV3_HEAD + 2 * s[2];
  L->states = (uint16_t)p;
  p += 2 * s[3];
  L->handlers = (uint16_t)p;
  p += 7 * s[4];
  L->anims = (uint16_t)p;
  for (i = 0; i < s[5]; i++) {
    if (p + 2 > n) return -1;
    p += 2 + s[p + 1];
  }
  L->perches = (uint16_t)p;
  p += 2 * s[6];
  L->strings = (uint16_t)p;
  for (i = 0; i < s[7]; i++) {
    if (p + 1 > n) return -1;
    p += 1 + s[p];
  }
  L->code = (uint16_t)p;
  return p < n ? 0 : -1;
}

static JV3_OPT int j3_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* ---- the check ----------------------------------------------------------- */

#define J3_BIT(m, i) ((m)[(i) >> 3] >> ((i) & 7) & 1)
#define J3_SET(m, i) ((m)[(i) >> 3] |= (uint8_t)(1 << ((i) & 7)))

/* Walk what is reachable from the entries in `todo`; -1 if it falls off the
 * end or (handler) reaches an op that takes time. */
static JV3_OPT int j3_reach(const uint8_t *s, int n, int c, const uint8_t *at, uint8_t *todo, int handler) {
  uint8_t seen[JV3_MAX / 8];
  int pc, i, changed = 1;
  for (i = 0; i < JV3_MAX / 8; i++) seen[i] = 0;
  while (changed) {
    changed = 0;
    for (pc = c; pc < n; pc += 1 + J3_OPLEN[s[pc]]) {
      int op = s[pc], next = pc + 1 + J3_OPLEN[op], tgt = -1, fall = 1;
      if (!J3_BIT(todo, pc) || J3_BIT(seen, pc)) continue;
      J3_SET(seen, pc);
      changed = 1;
      if (handler && (op == J3O_WAIT || op == J3O_AWAIT || op == J3O_JZY)) return -1;
      switch (op) {
      case J3O_HALT: case J3O_RET: fall = 0; break;
      case J3O_GO: fall = !handler; break;          /* a body goes on after GO (in the check) */
      case J3O_JMP: fall = 0; tgt = j3_u16(s + pc + 1); break;
      case J3O_JZ: case J3O_JZY: case J3O_RPT: tgt = j3_u16(s + pc + 1); break;
      default: break;
      }
      if (tgt >= 0) J3_SET(todo, tgt);
      if (fall) {
        if (next >= n || !J3_BIT(at, next)) return -1;
        J3_SET(todo, next);
      }
    }
  }
  return 0;
}

/* How many value slots string k has. */
static JV3_OPT int j3_marks(const uint8_t *s, int soff, int k) {
  const uint8_t *p = s + soff;
  int i, c = 0;
  for (i = 0; i < k; i++) p += 1 + p[0];
  for (i = 0; i < p[0]; i++) if (p[1 + i] == 1) c++;
  return c;
}

/* jv3_check for an item of nf frames: anims and FRAME must name one. */
static JV3_OPT int jv3_check_nf(const uint8_t *s, int n, int nf) {
  uint8_t at[JV3_MAX / 8], todo[JV3_MAX / 8];
  Jv3Layout L;
  int pc, i, nears = 0, everys = 0, p;
  if (j3_layout(s, n, &L) < 0 || (s[1] & ~1)) return -1;
  for (i = 0; i < JV3_MAX / 8; i++) at[i] = 0;
  /* anims, strings */
  for (i = 0, p = L.anims; i < s[5]; i++) {
    int k, fps = s[p], cnt = s[p + 1];
    if (fps < 1 || fps > 30 || cnt < 1 || cnt > 8) return -1;
    for (k = 0; k < cnt; k++) if (s[p + 2 + k] >= nf) return -1;
    p += 2 + cnt;
  }
  for (i = 0, p = L.strings; i < s[7]; i++) {
    if (s[p] > JV3_SAY) return -1;
    p += 1 + s[p];
  }
  /* instructions */
  for (pc = L.code; pc < n; ) {
    int op = s[pc], a, b;
    if (op >= J3_NOPS || pc + 1 + J3_OPLEN[op] > n) return -1;
    a = J3_OPLEN[op] >= 1 ? s[pc + 1] : 0;
    b = J3_OPLEN[op] >= 2 ? s[pc + 2] : 0;
    switch (op) {
    case J3O_LDV: case J3O_STV: if (a >= s[2]) return -1; break;
    case J3O_LDS: if (a >= J3_NSENSES) return -1; break;
    case J3O_STP: if (a >= J3_NSENSES || J3_SKIND[a] != 1) return -1; break;
    case J3O_FIELD: if (a >= J3_NFIELDS) return -1; break;
    case J3O_CALL: if (a >= J3_NFUNCS || b < (J3_FARGS[a] & 15) || b > (J3_FARGS[a] >> 4)) return -1; break;
    case J3O_GO: if (a >= s[3]) return -1; break;
    case J3O_FRAME: if (a >= nf) return -1; break;
    case J3O_PLAY: if (a >= s[5] || b > 1) return -1; break;
    case J3O_FLAG: if (a > 1 || b > 1) return -1; break;
    case J3O_SAY: if (a >= s[7] || b != j3_marks(s, L.strings, a)) return -1; break;
    case J3O_LOG: if (a != 255 ? (a >= s[7] || b != j3_marks(s, L.strings, a)) : b > JV3_STACK) return -1; break;
    case J3O_EMIT: if (a >= J3_NPARTICLES) return -1; break;
    case J3O_MOVE: if (a >= J3_NTARGETS || b > 32) return -1; break;
    case J3O_SIGNAL: if (a < 1 || b > 1) return -1; break;
    case J3O_ACT: if (a >= J3_NACTIONS || b > 1) return -1; break;
    default: break;
    }
    J3_SET(at, pc);
    pc += 1 + J3_OPLEN[op];
  }
  for (pc = L.code; pc < n; pc += 1 + J3_OPLEN[s[pc]]) {
    int op = s[pc];
    if (op == J3O_JMP || op == J3O_JZ || op == J3O_JZY || op == J3O_RPT) {
      int t = j3_u16(s + pc + 1);
      if (t < L.code || t >= n || !J3_BIT(at, t)) return -1;
    }
  }
  /* bodies */
  for (i = 0; i < JV3_MAX / 8; i++) todo[i] = 0;
  for (i = 0; i < s[3]; i++) {
    int e = j3_u16(s + L.states + 2 * i);
    if (e < L.code || e >= n || !J3_BIT(at, e)) return -1;
    J3_SET(todo, e);
  }
  if (j3_reach(s, n, L.code, at, todo, 0) < 0) return -1;
  /* handlers */
  for (i = 0; i < JV3_MAX / 8; i++) todo[i] = 0;
  for (i = 0; i < s[4]; i++) {
    const uint8_t *h = s + L.handlers + 7 * i;
    int e = j3_u16(h + 5), par = j3_u16(h + 3);
    if (h[0] >= J3_NEVENTS || (h[2] != 255 && h[2] >= s[3])) return -1;
    if (h[0] == J3E_every && (par < 1 || ++everys > JV3_EVERYS)) return -1;
    if (h[0] == J3E_near && (h[1] < 1 || h[1] > 32 || par < 1 || ++nears > JV3_NEARS)) return -1;
    if (h[0] == J3E_chime && (par < 1 || par > 15)) return -1;
    if (e < L.code || e >= n || !J3_BIT(at, e)) return -1;
    J3_SET(todo, e);
  }
  if (j3_reach(s, n, L.code, at, todo, 1) < 0) return -1;
  return 0;
}

/* 0 if the script can be run, -1 if it must be refused (the item is idle). */
static JV3_OPT int jv3_check(const uint8_t *s, int n) { return jv3_check_nf(s, n, 8); }

static JV3_OPT int jv3_budget(const Jv3Slot *s) { return s->n > 512 ? 100 : 50; }

static JV3_OPT const char *jv3_disasm_op(int op) { return op >= 0 && op < J3_NOPS ? J3_OPNAME[op] : "?"; }

/* ---- the world, as the machine sees it ------------------------------------ */

static JV3_OPT int16_t j3_ground(const Jv3Jar *j, int32_t x) {
  const Jv3Host *H = j->host;
  return H && H->ground ? H->ground(H->ctx, j3_sat(x)) : (int16_t)(112 * 16);
}

static JV3_OPT int j3_used(const Jv3Jar *j, int o) { return o >= 0 && o < j->nslots && j->slot[o].used; }
static JV3_OPT int16_t j3_px(const Jv3Jar *j, int o) { const Jv3Slot *s = &j->slot[o]; return s->it ? s->it->x : s->x; }
static JV3_OPT int16_t j3_py(const Jv3Jar *j, int o) { const Jv3Slot *s = &j->slot[o]; return s->it ? s->it->y : s->y; }
static JV3_OPT int32_t j3_cy(const Jv3Jar *j, int o) { const Jv3Slot *s = &j->slot[o]; return j3_py(j, o) - s->h * s->scale * 8; }
static JV3_OPT int j3_handle(const Jv3Jar *j, int16_t raw) {   /* slot, or -1 */
  int h = (int)j3_asr(raw, 4);
  return h >= 1 && h <= j->nslots && j->slot[h - 1].used ? h - 1 : -1;
}
static JV3_OPT int j3_trait(int16_t raw) { int t = (int)j3_asr(raw, 4); return t >= 0 && t <= 31 ? t : -1; }
static JV3_OPT int j3_has(const Jv3Jar *j, int o, int t) { return t >= 0 && (j->slot[o].traits >> t & 1); }
static JV3_OPT int32_t j3_dist(const Jv3Jar *j, int a, int b) {
  return j3_hyp((int32_t)j3_px(j, b) - j3_px(j, a), j3_cy(j, b) - j3_cy(j, a));
}

/* Slot o's declared solid, raw; 0 if none. */
static JV3_OPT int j3_solid(const Jv3Jar *j, int o, Jv3Box *b) {
  const Jv3Slot *s = &j->slot[o];
  int sc = s->scale;
  if (!s->used || !s->s || s->n < JV3_HEAD || s->s[0] != JV3_FORMAT || !s->s[12]) return 0;
  b->x = j3_sat((int32_t)j3_px(j, o) - s->w * sc * 8 + s->s[10] * sc * 16);
  b->y = j3_sat((int32_t)j3_py(j, o) - s->h * sc * 16 + s->s[11] * sc * 16);
  b->w = j3_sat(s->s[12] * sc * 16);
  b->h = j3_sat(s->s[13] * sc * 16);
  return 1;
}

/* The ledges the machine knows: 8 at most (their touch bits are 24..31). */
static JV3_OPT int j3_nledges(const Jv3Jar *j) { return j->nledges < 8 ? j->nledges : 8; }

/* Solid number k: 0..nslots-1 slots (not `me`), then the ledges. */
static JV3_OPT int j3_solid_k(const Jv3Jar *j, int me, int k, Jv3Box *b) {
  if (k < j->nslots) return k != me && j3_solid(j, k, b);
  k -= j->nslots;
  if (k < j3_nledges(j)) { *b = j->ledges[k]; return 1; }
  return 0;
}

/* The solid whose top the feet at (x, y) stand on: its number, or -1. */
static JV3_OPT int j3_on_top(const Jv3Jar *j, int me, int32_t x, int32_t y, Jv3Box *out) {
  int k;
  Jv3Box b;
  for (k = 0; k < j->nslots + j3_nledges(j); k++) {
    if (!j3_solid_k(j, me, k, &b)) continue;
    if (j3_abs(y - b.y) <= 16 && x >= b.x - 16 && x <= (int32_t)b.x + b.w + 16) { if (out) *out = b; return k; }
  }
  return -1;
}

/* A perch spot's place: owner slot + 1 (0 the rim), index. 0 if gone. */
static JV3_OPT int j3_spot(const Jv3Jar *j, int owner, int idx, int32_t *x, int32_t *y) {
  const Jv3Slot *s;
  Jv3Layout L;
  int o = owner - 1, sc;
  if (!owner) {
    if (idx < 0 || idx > 3) return 0;
    *x = J3_RIM[idx][0] * 16;
    *y = J3_RIM[idx][1] * 16;
    return 1;
  }
  if (!j3_used(j, o)) return 0;
  s = &j->slot[o];
  if (j3_layout(s->s, s->n, &L) < 0 || idx >= s->s[6]) return 0;
  sc = s->scale;
  *x = (int32_t)j3_px(j, o) - s->w * sc * 8 + s->s[L.perches + 2 * idx] * sc * 16;
  *y = (int32_t)j3_py(j, o) - s->h * sc * 16 + s->s[L.perches + 2 * idx + 1] * sc * 16;
  return 1;
}

static JV3_OPT int j3_spot_taken(const Jv3Jar *j, int me, int owner, int idx) {
  int o;
  const Jv3Item *mi = j->slot[me].it;
  if (mi->perch == idx + 1 && mi->perch_owner == owner) return 1;   /* my own: never */
  for (o = 0; o < j->nslots; o++) {
    const Jv3Item *it = j->slot[o].it;
    if (o == me || !j->slot[o].used || !it) continue;
    if (it->perch == idx + 1 && it->perch_owner == owner) return 1;
  }
  return 0;
}

/* The free perch spots for trait t (-1: the rim). With pick < 0, count
 * them; else the pick-th, into owner/idx. */
static JV3_OPT int j3_spots(const Jv3Jar *j, int me, int t, int pick, int *owner, int *idx) {
  int o, i, cnt = 0;
  for (o = 0; o < j->nslots && t >= 0; o++) {
    const Jv3Slot *s = &j->slot[o];
    Jv3Layout L;
    if (o == me || !s->used || j3_layout(s->s, s->n, &L) < 0) continue;
    if (!j3_has(j, o, t)) continue;
    for (i = 0; i < s->s[6]; i++) {
      if (j3_spot_taken(j, me, o + 1, i)) continue;
      if (cnt++ == pick) { *owner = o + 1; *idx = i; return cnt; }
    }
  }
  if (t < 0)
    for (i = 0; i < 4; i++) {
      if (j3_spot_taken(j, me, 0, i)) continue;
      if (cnt++ == pick) { *owner = 0; *idx = i; return cnt; }
    }
  return cnt;
}

/* ---- the item ------------------------------------------------------------ */

static JV3_OPT void j3_queue(Jv3Item *it, int ev, int arg, int other) {
  if (!it || it->stopped || it->qn - it->qready >= JV3_QUEUE) return;
  it->q[it->qn].ev = (uint8_t)ev;
  it->q[it->qn].arg = j3_sat(arg);
  it->q[it->qn].other = (uint8_t)other;
  it->qn++;
}

static JV3_OPT void jv3_event(Jv3Jar *j, int slot, int ev, int arg, int other) {
  if (slot < 0 || slot >= j->nslots || !j->slot[slot].used) return;
  j3_queue(j->slot[slot].it, ev, arg, other);
}

static JV3_OPT void j3_leave_perch(Jv3Item *it) { it->perch = 0; it->perch_owner = 0; }

static JV3_OPT void j3_enter(Jv3Jar *j, int si, int k) {
  Jv3Slot *sl = &j->slot[si];
  Jv3Item *it = sl->it;
  const uint8_t *s = sl->s;
  int i, ne = 0, nn = 0;
  it->state = (uint8_t)k;
  it->statet = 0;
  it->pc = (uint16_t)j3_u16(s + JV3_HEAD + 2 * s[2] + 2 * k);
  it->sp = 0;
  it->wait = 0;
  it->halted = 0;
  it->pending = 255;
  it->mover = 0;
  it->spot_idx = 255;
  for (i = 0; i < s[4]; i++) {
    const uint8_t *h = s + it->hoff + 7 * i;
    if (h[0] == J3E_every && ne < JV3_EVERYS) { if (h[2] == k) it->every[ne] = (uint16_t)j3_u16(h + 3); ne++; }
    if (h[0] == J3E_near && nn < JV3_NEARS) { if (h[2] == k) it->nearm[nn] = 0; nn++; }
  }
}

static JV3_OPT void j3_fault(Jv3Jar *j, int si, int pc) {
  Jv3Item *it = j->slot[si].it;
  const Jv3Host *H = j->host;
  if (H && H->fault) H->fault(H->ctx, si, pc);
  if (++it->faults >= JV3_FAULTS_STOP) { it->stopped = 1; it->mover = 0; it->qn = 0; it->qready = 0; }
}

static JV3_OPT int jv3_place(Jv3Jar *j, int si) {
  Jv3Slot *sl;
  Jv3Item *it;
  Jv3Layout L;
  int i, zero = 1;
  uint8_t *b;
  if (si < 0 || si >= JV3_SLOTS) return -1;
  sl = &j->slot[si];
  it = sl->it;
  if (!it) return -1;
  b = (uint8_t *)it;
  for (i = 0; i < (int)sizeof *it; i++) b[i] = 0;
  it->x = sl->x;
  it->y = sl->y;
  it->facing = 1;
  it->anim = 255;
  it->spot_idx = 255;
  it->pending = 255;
  it->gravity = 160 * 16;
  it->bounce = 40 * 16;
  it->friction = 60 * 16;
  if (jv3_check_nf(sl->s, sl->n, sl->nframes) != 0 || j3_layout(sl->s, sl->n, &L) < 0) {
    it->stopped = 2;                         /* idle: nothing runs (2: never ran) */
    return -1;
  }
  it->hoff = L.handlers;
  it->aoff = L.anims;
  it->soff = L.strings;
  if (sl->mem) {
    for (i = 0; i < 8; i++) if (sl->mem[i]) zero = 0;
    if (zero)
      for (i = 0; i < sl->s[2]; i++) sl->mem[i] = (int16_t)j3_u16(sl->s + JV3_HEAD + 2 * i);
  }
  for (i = 0, zero = 0; i < sl->s[4]; i++) {   /* every counter starts full */
    const uint8_t *h = sl->s + it->hoff + 7 * i;
    if (h[0] == J3E_every && zero < JV3_EVERYS) it->every[zero++] = (uint16_t)j3_u16(h + 3);
  }
  j3_enter(j, si, 0);
  return 0;
}

/* ---- text ---------------------------------------------------------------- */

#define J3_LOGMAX 160                      /* a log line: up to 16 values and a template */

typedef struct { char *o; int at, cap; } J3Text;

static JV3_OPT void j3_put(J3Text *t, char c) { if (t->at < t->cap) t->o[t->at++] = c; }

static JV3_OPT void j3_putu(J3Text *t, uint32_t v) {
  char d[12];
  int n = 0;
  do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) j3_put(t, d[--n]);
}

static JV3_OPT void j3_putnum(J3Text *t, int16_t raw) {
  int32_t a = j3_abs(raw);
  if (raw < 0) j3_put(t, '-');
  if (!(a & 15)) { j3_putu(t, (uint32_t)(a >> 4)); return; }
  a = (a * 25 + 2) / 4;                       /* hundredths, halves away from zero */
  j3_putu(t, (uint32_t)(a / 100));
  j3_put(t, '.');
  j3_put(t, (char)('0' + a / 10 % 10));
  if (a % 10) j3_put(t, (char)('0' + a % 10));
}

/* String k (255: none) with the values in, into o (cap + 1 bytes). */
static JV3_OPT void j3_text(const Jv3Slot *sl, const Jv3Item *it, int k, const int16_t *v, int nv, char *o, int cap) {
  J3Text t;
  int i, used = 0;
  t.o = o; t.at = 0; t.cap = cap;
  if (k == 255) {
    for (i = 0; i < nv; i++) {
      if (i) j3_put(&t, ' ');
      j3_putnum(&t, v[i]);
    }
  } else {
    const uint8_t *p = sl->s + it->soff;
    for (i = 0; i < k; i++) p += 1 + p[0];
    for (i = 0; i < p[0]; i++) {
      if (p[1 + i] == 1) { if (used < nv) j3_putnum(&t, v[used]); used++; }
      else j3_put(&t, (char)p[1 + i]);
    }
  }
  o[t.at] = 0;
}

/* ---- movers -------------------------------------------------------------- */

/* The curve at s (1/4096ths), de Casteljau with a floor at each step. */
static JV3_OPT int32_t j3_lerp(int32_t a, int32_t b, int32_t s) { return a + j3_asr((b - a) * s, 12); }
static JV3_OPT int32_t j3_bez(int32_t p0, int32_t c, int32_t p1, int32_t s) {
  int32_t a = j3_lerp(p0, c, s), b = j3_lerp(c, p1, s);
  return j3_lerp(a, b, s);
}

/* A path from where the item is to (x1, y1). owner/idx: the perch aimed at
 * (idx < 0: none). */
static JV3_OPT void j3_path(Jv3Jar *j, int si, int32_t x1, int32_t y1, int32_t speed, int32_t arc,
                            int owner, int idx, int hop) {
  Jv3Item *it = j->slot[si].it;
  int32_t x0 = it->x, y0 = it->y, cx, cy, len = 0, px = x0, py = y0, dur, spd;
  int i;
  cx = j3_asr(x0 + x1, 1);
  cy = j3_asr(y0 + y1, 1) - 2 * arc;
  for (i = 1; i <= 12; i++) {
    int32_t s = i * 4096 / 12;
    int32_t qx = j3_bez(x0, cx, x1, s), qy = j3_bez(y0, cy, y1, s);
    len += j3_hyp(qx - px, qy - py);
    px = qx; py = qy;
  }
  spd = speed > 80 ? speed : 80;
  dur = j3_ticks(len * 16 / spd);
  if (dur < 3) dur = 3;
  it->m[0] = x0; it->m[1] = y0; it->m[2] = x1; it->m[3] = y1; it->m[4] = cx; it->m[5] = cy;
  it->mk = 0;
  it->mdur = (uint16_t)((4096 + dur - 1) / dur);
  it->mover = 1;
  it->hop = (uint8_t)hop;
  it->spot_owner = (uint8_t)owner;
  it->spot_idx = (uint8_t)(idx < 0 ? 255 : idx);
  it->vx = it->vy = 0;
  if (x1 != x0) it->facing = (int8_t)(x1 > x0 ? 1 : -1);
}

static JV3_OPT void j3_step_path(Jv3Jar *j, int si) {
  Jv3Item *it = j->slot[si].it;
  int32_t T = it->mk + it->mdur, s;
  if (T > 4096) T = 4096;
  it->mk = (uint16_t)T;
  s = (((T * T) >> 12) * (12288 - 2 * T)) >> 12;
  it->x = j3_sat(j3_bez(it->m[0], it->m[4], it->m[2], s));
  it->y = j3_sat(j3_bez(it->m[1], it->m[5], it->m[3], s));
  if (T >= 4096) {
    it->x = j3_sat(it->m[2]);
    it->y = j3_sat(it->m[3]);
    it->mover = 0;
    if (it->spot_idx != 255) { it->perch = (uint8_t)(it->spot_idx + 1); it->perch_owner = it->spot_owner; }
    if (!it->hop) j3_queue(it, J3E_land, 0, it->spot_idx != 255 ? it->spot_owner : 0);
    it->spot_idx = 255;
  }
}

static JV3_OPT void j3_step_walk(Jv3Jar *j, int si) {
  Jv3Slot *sl = &j->slot[si];
  Jv3Item *it = sl->it;
  int32_t half = sl->w * sl->scale * 8, hs = sl->h * sl->scale * 16;
  int32_t left = it->m[0], dir = it->m[2], d = it->m[1], x = it->x, nx, y = it->y;
  int32_t lo = half + 32, hi = 3840 - half - 32;
  int k, stop = 0;
  Jv3Box b;
  if (d > left) d = left;
  nx = x + dir * d;
  if (nx < lo || nx > hi) { nx = j3_clampi(nx, lo, hi); stop = 1; }
  for (k = 0; k < j->nslots + j3_nledges(j); k++) {
    if (!j3_solid_k(j, si, k, &b)) continue;
    if (y > b.y + 8 && y - hs < (int32_t)b.y + b.h && nx + half > b.x && nx - half < (int32_t)b.x + b.w &&
        !(x + half > b.x && x - half < (int32_t)b.x + b.w)) { stop = 1; nx = x; }
  }
  if (j3_abs(y - j3_ground(j, x)) > 16 && j3_on_top(j, si, x, y, &b) >= 0)
    if (nx < b.x || nx > (int32_t)b.x + b.w) { stop = 1; nx = x; }
  it->x = j3_sat(nx);
  left -= d;
  it->m[0] = left;
  if (stop) { it->blocked = 1; it->mover = 0; }
  else if (left <= 0) it->mover = 0;
}

static JV3_OPT int32_t j3_radius(const Jv3Slot *sl) {
  int32_t r;
  if (sl->s && sl->n > 9 && sl->s[9]) return sl->s[9] * 16;
  r = (sl->w < sl->h ? sl->w : sl->h) * sl->scale * 8 - 16;
  return r < 16 ? 16 : r;
}

static JV3_OPT void j3_step_physics(Jv3Jar *j, int si) {
  Jv3Slot *sl = &j->slot[si];
  Jv3Item *it = sl->it;
  int32_t r = j3_radius(sl), hh = sl->h * sl->scale * 8, e = it->bounce;
  int32_t vx = it->vx, vy, cx, cy, g, dxm, imps[4];
  int k, grounded = 0, nimp = 0;
  uint32_t prev = it->touch, touch = 0;
  Jv3Box b;
  vy = j3_sat((int32_t)it->vy + it->gravity / 20);
  if (it->grounded && it->friction > 0) {
    int32_t f = it->friction / 20;
    if (j3_abs(vx) <= f) vx = 0; else vx -= vx > 0 ? f : -f;
  }
  dxm = vx / 20;
  cx = it->x + dxm;
  cy = it->y - hh + vy / 20;
  g = j3_ground(j, cx);
  if (cy + r > g) {
    cy = g - r;
    if (vy > 0) { imps[nimp++] = vy; vy = -(vy * e / 1600); if (j3_abs(vy) < 192) vy = 0; }
    grounded = 1;
  }
  if (cx - r < 48) { cx = 48 + r; if (vx < 0) { imps[nimp++] = -vx; vx = -vx * e / 1600; } }
  if (cx + r > 3792) { cx = 3792 - r; if (vx > 0) { imps[nimp++] = vx; vx = -(vx * e / 1600); } }
  if (cy - r < 128) { cy = 128 + r; if (vy < 0) { imps[nimp++] = -vy; vy = -vy * e / 1600; } }
  for (k = 0; k < nimp; k++) if (imps[k] > 240) j3_queue(it, J3E_hit, imps[k], 0);
  for (k = 0; k < j->nslots + j3_nledges(j); k++) {
    int32_t nx, ny, dx, dy, d, ndx, ndy, nd, vn;
    uint32_t bit;
    int top;
    if (!j3_solid_k(j, si, k, &b)) continue;
    nx = j3_clampi(cx, b.x, (int32_t)b.x + b.w);
    ny = j3_clampi(cy, b.y, (int32_t)b.y + b.h);
    dx = cx - nx; dy = cy - ny;
    if (j3_abs(dx) >= r || j3_abs(dy) >= r || dx * dx + dy * dy >= r * r) continue;
    d = (int32_t)j3_isqrt((uint32_t)(dx * dx + dy * dy));
    if (d == 0) { ndx = 0; ndy = -1; nd = 1; cy = b.y - r; }
    else { ndx = dx; ndy = dy; nd = d; cx += dx * (r - d) / d; cy += dy * (r - d) / d; }
    top = 5 * ndy < -3 * nd;
    bit = k < j->nslots ? (1u << k) : (1u << (24 + k - j->nslots));
    vn = (vx * ndx + vy * ndy) / nd;
    if (vn < 0) {
      int32_t t = vn * (1600 + e) / 1600;
      vx = j3_sat(vx - t * ndx / nd);
      vy = j3_sat(vy - t * ndy / nd);
      if (top && j3_abs(vy) < 192) vy = 0;
      if (-vn > 160 && !(prev & bit)) {
        if (k < j->nslots) {
          j3_queue(it, J3E_bump, -vn, k + 1);
          j3_queue(j->slot[k].it, J3E_bump, -vn, si + 1);
        } else j3_queue(it, J3E_bump, -vn, 0);
      }
    }
    if (top) grounded = 1;
    touch |= bit;
  }
  it->touch = touch;
  it->grounded = (uint8_t)grounded;
  it->x = j3_sat(cx);
  it->y = j3_sat(cy + hh);
  it->vx = j3_sat(vx);
  it->vy = j3_sat(vy);
  if (it->roll) it->angle = (uint8_t)((it->angle + dxm * 163 / (r * 4)) & 255);
}

static JV3_OPT void j3_move(Jv3Jar *j, int si) {
  Jv3Slot *sl = &j->slot[si];
  Jv3Item *it = sl->it;
  int moved = 0;
  if (it->mover == 1) { j3_step_path(j, si); moved = 1; }
  else if (it->mover == 2) { j3_step_walk(j, si); moved = 1; }
  else if (it->physics) j3_step_physics(j, si);
  if (!it->physics || moved) {
    if (!it->physics) it->touch = 0;
    if (it->spin) it->angle = (uint8_t)((it->angle + it->spin / 450) & 255);
  }
  if (it->perch && it->mover != 1) {
    int32_t x, y;
    if (j3_spot(j, it->perch_owner, it->perch - 1, &x, &y)) { it->x = j3_sat(x); it->y = j3_sat(y); }
    else j3_leave_perch(it);
  }
  if (!it->physics)
    it->grounded = (uint8_t)(!it->perch && !it->mover &&
                             (j3_abs((int32_t)it->y - j3_ground(j, it->x)) <= 16 || j3_on_top(j, si, it->x, it->y, 0) >= 0));
  if (it->anim != 255 && !(it->anim_once && it->anim_done)) {
    const uint8_t *a = sl->s + it->aoff;
    int i, k, n, f;
    for (i = 0; i < it->anim; i++) a += 2 + a[1];
    n = a[1];
    it->anim_t++;
    k = it->anim_t * a[0] / 20;
    if (it->anim_once) {
      if (k >= n) { f = a[1 + n]; it->anim_done = 1; }
      else f = a[2 + k];
    } else {
      f = a[2 + k % n];
      if (it->anim_t >= 20 * n) it->anim_t = (uint16_t)(it->anim_t - 20 * n);
    }
    if (f < sl->nframes) it->frame = (uint8_t)f;
  }
  if (it->statet < 65535) it->statet++;
  if (it->say_ticks && !--it->say_ticks) it->say[0] = 0;
  if (it->sway_ticks && !--it->sway_ticks) it->sway_amp = 0;
  if (it->shake_ticks) it->shake_ticks--;
}

/* ---- the machine --------------------------------------------------------- */

enum { J3R_DONE, J3R_YIELD, J3R_HALT, J3R_GO, J3R_BUDGET, J3R_FAULT };

static JV3_OPT int j3_blocked(const Jv3Item *it) {
  return it->mover == 1 || it->mover == 2 || (it->anim != 255 && it->anim_once && !it->anim_done);
}

static JV3_OPT int16_t j3_sense(Jv3Jar *j, int si, int k) {
  const Jv3Slot *sl = &j->slot[si];
  const Jv3Item *it = sl->it;
  switch (k) {
  case J3S_x: return it->x;
  case J3S_y: return it->y;
  case J3S_vx: return it->vx;
  case J3S_vy: return it->vy;
  case J3S_angle: return j3_deg(it->angle);
  case J3S_spin: return it->spin;
  case J3S_gravity: return it->gravity;
  case J3S_bounce: return it->bounce;
  case J3S_friction: return it->friction;
  case J3S_speed: return j3_sat(j3_hyp(it->vx, it->vy));
  case J3S_grounded: return it->grounded ? 16 : 0;
  case J3S_blocked: return it->blocked ? 16 : 0;
  case J3S_onperch: return it->perch ? 16 : 0;
  case J3S_perched: return (int16_t)(it->perch ? it->perch_owner * 16 : 0);
  case J3S_facing: return (int16_t)(it->facing * 16);
  case J3S_other: return (int16_t)(it->other * 16);
  case J3S_impact: return it->impact;
  case J3S_me: return (int16_t)((si + 1) * 16);
  case J3S_state: return (int16_t)(it->state * 16);
  case J3S_statetime: return j3_sat((int32_t)it->statet * 16 / 20);
  case J3S_frame: return (int16_t)((it->frame + 1) * 16);
  case J3S_time: return (int16_t)((int32_t)(j->tick % 20480u) * 16 / 20);
  case J3S_physics: return it->physics ? 16 : 0;
  case J3S_soil: return 112 * 16;
  case J3S_jarw: return 240 * 16;
  case J3S_jarh: return 135 * 16;
  default:
    return j->host && j->host->sense ? j->host->sense(j->host->ctx, si, k) : 0;
  }
}

static JV3_OPT void j3_store(Jv3Jar *j, int si, int k, int16_t v) {
  Jv3Item *it = j->slot[si].it;
  switch (k) {
  case J3S_x: it->x = v; j3_leave_perch(it); break;
  case J3S_y: it->y = v; j3_leave_perch(it); break;
  case J3S_vx: it->vx = v; break;
  case J3S_vy: it->vy = v; break;
  case J3S_angle: it->angle = j3_step(v); break;
  case J3S_spin: it->spin = v; break;
  case J3S_gravity: it->gravity = v; break;
  case J3S_bounce: it->bounce = (int16_t)j3_clampi(v, 0, 1600); break;
  case J3S_friction: it->friction = v < 0 ? 0 : v; break;
  default: break;
  }
}

static JV3_OPT int16_t j3_field(Jv3Jar *j, int16_t h, int k) {
  int o = j3_handle(j, h);
  const Jv3Slot *s;
  if (o < 0) return 0;
  s = &j->slot[o];
  switch (k) {
  case J3D_x: return j3_px(j, o);
  case J3D_y: return j3_py(j, o);
  case J3D_vx: return s->it ? s->it->vx : 0;
  case J3D_vy: return s->it ? s->it->vy : 0;
  case J3D_w: return j3_sat(s->w * s->scale * 16);
  case J3D_h: return j3_sat(s->h * s->scale * 16);
  case J3D_speed: return s->it ? j3_sat(j3_hyp(s->it->vx, s->it->vy)) : 0;
  case J3D_id: return (int16_t)((o + 1) * 16);
  }
  return 0;
}

static JV3_OPT int16_t j3_call(Jv3Jar *j, int si, int k, const int16_t *a, int n) {
  uint32_t r;
  int o, t, best = -1;
  int32_t bd = 0, lim;
  switch (k) {
  case J3F_random: {
    int32_t lo = a[0], hi = a[1];
    if (hi < lo) { lo = a[1]; hi = a[0]; }
    r = j3_rnd(&j->rng);
    if (lo % 16 == 0 && hi % 16 == 0) return j3_sat(lo + 16 * (int32_t)(r % (uint32_t)((hi - lo) / 16 + 1)));
    return j3_sat(lo + (int32_t)(r % (uint32_t)(hi - lo + 1)));
  }
  case J3F_chance: r = j3_rnd(&j->rng); return (int32_t)(r % 1600) < a[0] ? 16 : 0;
  case J3F_pick: r = j3_rnd(&j->rng); return a[r % (uint32_t)n];
  case J3F_abs: return j3_sat(j3_abs(a[0]));
  case J3F_min: return a[0] < a[1] ? a[0] : a[1];
  case J3F_max: return a[0] > a[1] ? a[0] : a[1];
  case J3F_sign: return (int16_t)(a[0] > 0 ? 16 : a[0] < 0 ? -16 : 0);
  case J3F_clamp: return a[0] < a[1] ? a[1] : a[0] > a[2] ? a[2] : a[0];
  case J3F_round: return j3_round(a[0]);
  case J3F_floor: return (int16_t)(a[0] & ~15);
  case J3F_sqrt: return a[0] <= 0 ? 0 : (int16_t)j3_isqrt((uint32_t)a[0] * 16);
  case J3F_sin: return J3_SIN[j3_step(a[0])];
  case J3F_cos: return J3_SIN[(j3_step(a[0]) + 64) & 255];
  case J3F_dist:
    if (n == 2) return j3_sat(j3_hyp((int32_t)a[0] - j3_px(j, si), (int32_t)a[1] - j3_cy(j, si)));
    o = j3_handle(j, a[0]);
    return o < 0 ? 0 : j3_sat(j3_dist(j, si, o));
  case J3F_angleto: {
    int32_t dx, dy, bv = 0;
    int i;
    o = j3_handle(j, a[0]);
    if (o < 0) return 0;
    dx = j3_clampi((int32_t)j3_px(j, o) - j3_px(j, si), -65535, 65535);
    dy = j3_clampi(j3_cy(j, o) - j3_cy(j, si), -65535, 65535);
    for (i = 0; i < 256; i++) {
      int32_t v = dx * J3_ANG[(i + 64) & 255] + dy * J3_ANG[i];
      if (best < 0 || v > bv) { best = i; bv = v; }
    }
    return j3_deg(best);
  }
  case J3F_find:
    t = j3_trait(a[0]);
    lim = n > 1 ? a[1] : 0x7FFFFFFF;
    for (o = 0; o < j->nslots; o++) {
      int32_t d;
      if (o == si || !j->slot[o].used || !j3_has(j, o, t)) continue;
      d = j3_dist(j, si, o);
      if (d <= lim && (best < 0 || d < bd)) { best = o; bd = d; }
    }
    return (int16_t)(best < 0 ? 0 : (best + 1) * 16);
  case J3F_count:
    t = j3_trait(a[0]);
    for (o = 0, n = 0; o < j->nslots; o++)
      if (o != si && j->slot[o].used && j3_has(j, o, t)) n++;
    return (int16_t)(n * 16);
  case J3F_has:
    o = j3_handle(j, a[0]);
    return (int16_t)(o >= 0 && j3_has(j, o, j3_trait(a[1])) ? 16 : 0);
  }
  return 0;
}

static JV3_OPT int j3_bodies(const Jv3Jar *j) {
  int o, c = 0;
  for (o = 0; o < j->nslots; o++)
    if (j->slot[o].used && j->slot[o].it && j->slot[o].it->physics) c++;
  return c;
}

/* Run from *ppc on stack st (depth *psp). body: the item's thread (else a handler).
 * *budget counts down. Returns J3R_*; *out is the state for GO, the pc for a
 * fault or a budget stop. */
static JV3_OPT int j3_exec(Jv3Jar *j, int si, uint16_t *ppc, int16_t *st, uint8_t *psp,
                           int body, int *budget, int *out) {
  Jv3Slot *sl = &j->slot[si];
  Jv3Item *it = sl->it;
  const uint8_t *s = sl->s;
  const Jv3Host *H = j->host;
  int n = sl->n, pc = *ppc, sp = *psp;
#define J3_POP(v) do { if (sp <= 0) goto fault; (v) = st[--sp]; } while (0)
#define J3_PUSH(v) do { if (sp >= JV3_STACK) goto fault; st[sp++] = (int16_t)(v); } while (0)
  for (;;) {
    int op, a = 0, b = 0, next;
    int16_t x, y;
    if (pc < 0 || pc >= n) goto fault;
    op = s[pc];
    if (op >= J3_NOPS || pc + J3_OPLEN[op] >= n + (J3_OPLEN[op] ? 0 : 1)) goto fault;
    if (op == J3O_AWAIT && body && j3_blocked(it)) { *ppc = (uint16_t)pc; *psp = (uint8_t)sp; return J3R_YIELD; }
    if (*budget <= 0) { *ppc = (uint16_t)pc; *psp = (uint8_t)sp; *out = pc; return J3R_BUDGET; }
    (*budget)--;
    if (J3_OPLEN[op] >= 1) a = s[pc + 1];
    if (J3_OPLEN[op] >= 2) b = s[pc + 2];
    next = pc + 1 + J3_OPLEN[op];
    switch (op) {
    case J3O_HALT:
      *ppc = (uint16_t)pc; *psp = (uint8_t)sp;
      return body ? J3R_HALT : J3R_DONE;
    case J3O_RET:
      *ppc = (uint16_t)next; *psp = (uint8_t)sp;
      return body ? J3R_YIELD : J3R_DONE;
    case J3O_PUSH8: J3_PUSH((int8_t)a * 16); break;
    case J3O_PUSH16: J3_PUSH((int16_t)(uint16_t)(a | (b << 8))); break;
    case J3O_LDV: J3_PUSH(sl->mem && a < 8 ? sl->mem[a] : 0); break;
    case J3O_STV: J3_POP(x); if (sl->mem && a < 8) sl->mem[a] = x; break;
    case J3O_LDS: J3_PUSH(j3_sense(j, si, a)); break;
    case J3O_STP: J3_POP(x); j3_store(j, si, a, x); break;
    case J3O_ADD: J3_POP(y); J3_POP(x); J3_PUSH(j3_sat((int32_t)x + y)); break;
    case J3O_SUB: J3_POP(y); J3_POP(x); J3_PUSH(j3_sat((int32_t)x - y)); break;
    case J3O_MUL: J3_POP(y); J3_POP(x); J3_PUSH(j3_mul(x, y)); break;
    case J3O_DIV: J3_POP(y); J3_POP(x); J3_PUSH(j3_div(x, y)); break;
    case J3O_MOD: J3_POP(y); J3_POP(x); J3_PUSH(j3_mod(x, y)); break;
    case J3O_NEG: J3_POP(x); J3_PUSH(j3_sat(-(int32_t)x)); break;
    case J3O_EQ: J3_POP(y); J3_POP(x); J3_PUSH(x == y ? 16 : 0); break;
    case J3O_NE: J3_POP(y); J3_POP(x); J3_PUSH(x != y ? 16 : 0); break;
    case J3O_LT: J3_POP(y); J3_POP(x); J3_PUSH(x < y ? 16 : 0); break;
    case J3O_LE: J3_POP(y); J3_POP(x); J3_PUSH(x <= y ? 16 : 0); break;
    case J3O_GT: J3_POP(y); J3_POP(x); J3_PUSH(x > y ? 16 : 0); break;
    case J3O_GE: J3_POP(y); J3_POP(x); J3_PUSH(x >= y ? 16 : 0); break;
    case J3O_NOT: J3_POP(x); J3_PUSH(x ? 0 : 16); break;
    case J3O_AND: J3_POP(y); J3_POP(x); J3_PUSH(x && y ? 16 : 0); break;
    case J3O_OR: J3_POP(y); J3_POP(x); J3_PUSH(x || y ? 16 : 0); break;
    case J3O_FIELD: J3_POP(x); J3_PUSH(j3_field(j, x, a)); break;
    case J3O_CALL: {
      int16_t v[8];
      int i;
      if (b > 8 || a >= J3_NFUNCS || b < (J3_FARGS[a] & 15) || b > (J3_FARGS[a] >> 4)) goto fault;
      for (i = b - 1; i >= 0; i--) J3_POP(v[i]);
      J3_PUSH(j3_call(j, si, a, v, b));
      break;
    }
    case J3O_JMP: next = a | (b << 8); break;
    case J3O_JZ: J3_POP(x); if (!x) next = a | (b << 8); break;
    case J3O_JZY:
      if (!body) goto fault;
      J3_POP(x);
      if (!x) { (*budget)++; *ppc = (uint16_t)(a | (b << 8)); *psp = (uint8_t)sp; return J3R_YIELD; }   /* the JZY itself is free */
      break;
    case J3O_RPT:
      if (sp <= 0) goto fault;
      if (st[sp - 1] <= 0) { sp--; next = a | (b << 8); }
      else st[sp - 1] = (int16_t)(st[sp - 1] - 16);
      break;
    case J3O_WAIT: {
      int32_t t;
      if (!body) goto fault;
      J3_POP(x);
      t = j3_ticks(x);
      it->wait = (uint16_t)(t < 1 ? 1 : t > 65535 ? 65535 : t);
      *ppc = (uint16_t)next; *psp = (uint8_t)sp;
      return J3R_YIELD;
    }
    case J3O_AWAIT: if (!body) goto fault; break;      /* not blocked: carry on */
    case J3O_GO:
      if (a >= s[3]) goto fault;
      *psp = (uint8_t)sp;
      if (body) { *ppc = (uint16_t)next; *out = a; return J3R_GO; }
      it->pending = (uint8_t)a;
      return J3R_DONE;
    case J3O_FRAME: if (a < sl->nframes) { it->anim = 255; it->frame = (uint8_t)a; } break;
    case J3O_FRAMEE: {
      int f;
      J3_POP(x);
      f = j3_round(x) / 16 - 1;
      if (f >= 0 && f < sl->nframes) { it->anim = 255; it->frame = (uint8_t)f; }
      break;
    }
    case J3O_PLAY:
      if (a >= s[5]) goto fault;
      if (b || it->anim != a || it->anim_once) {
        const uint8_t *an = s + it->aoff;
        int i;
        for (i = 0; i < a; i++) an += 2 + an[1];
        it->anim = (uint8_t)a; it->anim_once = (uint8_t)b; it->anim_done = 0; it->anim_t = 0;
        if (an[2] < sl->nframes) it->frame = an[2];
      }
      break;
    case J3O_STOP: it->anim = 255; break;
    case J3O_FLAG:
      if (a == 0) {
        if (b && !it->physics) {
          if (j3_bodies(j) < j->max_bodies) { it->physics = 1; j3_leave_perch(it); it->mover = 0; it->spot_idx = 255; }
        } else if (!b) it->physics = 0;
      } else it->roll = (uint8_t)(b != 0);
      break;
    case J3O_SAY: case J3O_LOG: {
      int16_t v[JV3_STACK];
      int i, t = 0;
      char txt[J3_LOGMAX + 1];
      if (op == J3O_SAY) { J3_POP(x); t = j3_ticks(x); if (t < 6) t = 6; }
      if (b > sp) goto fault;
      for (i = b - 1; i >= 0; i--) J3_POP(v[i]);
      if (op == J3O_SAY) {
        if (a >= s[7]) goto fault;
        j3_text(sl, it, a, v, b, it->say, JV3_SAY);
        it->say_ticks = (uint16_t)(t > 65535 ? 65535 : t);
        if (H && H->say) H->say(H->ctx, si, it->say, t);
      } else {
        if (a != 255 && a >= s[7]) goto fault;
        if (H && H->log) { j3_text(sl, it, a, v, b, txt, J3_LOGMAX); H->log(H->ctx, si, txt); }
      }
      break;
    }
    case J3O_EMIT: {
      int32_t c;
      J3_POP(x);
      c = j3_clampi(j3_asr(x, 4), 0, 12);
      if (c > 0 && H && H->emit) H->emit(H->ctx, si, a, (int)c);
      break;
    }
    case J3O_SWAY: {
      int32_t t;
      J3_POP(y); J3_POP(x);
      t = j3_ticks(y);
      it->sway_amp = (int16_t)j3_clampi(x, 0, 128);
      it->sway_ticks = (uint16_t)j3_clampi(t, 1, 65535);
      it->sway_t0 = (uint16_t)j->tick;
      break;
    }
    case J3O_SHAKE: J3_POP(x); it->shake_ticks = (uint16_t)j3_clampi(j3_ticks(x), 0, 65535); break;
    case J3O_FACE: J3_POP(x); if (x) it->facing = (int8_t)(x > 0 ? 1 : -1); break;
    case J3O_TOWARD: {
      int o;
      int32_t dx;
      J3_POP(x);
      o = j3_handle(j, x);
      if (o >= 0) { dx = (int32_t)j3_px(j, o) - it->x; if (dx) it->facing = (int8_t)(dx > 0 ? 1 : -1); }
      break;
    }
    case J3O_MOVE: {
      int16_t arc, spd;
      int32_t tx = 0, ty = 0, half = sl->w * sl->scale * 8, g;
      int have = 0, owner = 0, idx = -1, o;
      J3_POP(arc); J3_POP(spd);
      if (a == J3T_xy) { J3_POP(y); J3_POP(x); tx = x; ty = y; have = 1; }
      else if (a == J3T_item) {
        J3_POP(x);
        o = j3_handle(j, x);
        if (o >= 0) { tx = j3_px(j, o); ty = (int32_t)j3_py(j, o) - j->slot[o].h * j->slot[o].scale * 16; have = 1; }
      } else if (a == J3T_perch) {
        int cnt = j3_spots(j, si, b ? b - 1 : -1, -1, &owner, &idx);
        if (cnt > 0) {
          j3_spots(j, si, b ? b - 1 : -1, (int)(j3_rnd(&j->rng) % (uint32_t)cnt), &owner, &idx);
          j3_spot(j, owner, idx, &tx, &ty);
          have = 1;
        } else idx = -1;
      }
      if (!have) {
        idx = -1;
        tx = (14 + (int32_t)(j3_rnd(&j->rng) % 213u)) * 16;
        ty = j3_ground(j, tx);
      }
      tx = j3_clampi(tx, half, 3840 - half);
      g = j3_ground(j, tx);
      ty = j3_clampi(ty, 128, g < 128 ? 128 : g);
      j3_leave_perch(it);
      j3_path(j, si, tx, ty, spd, arc, owner, idx, 0);
      break;
    }
    case J3O_WALK: {
      int16_t spd;
      J3_POP(spd); J3_POP(x);
      it->blocked = 0;
      j3_leave_perch(it);
      it->mover = 0;
      it->spot_idx = 255;
      it->vx = it->vy = 0;
      if (j3_abs(x) >= 8) {
        int32_t v = j3_abs(spd) / 20;
        it->m[0] = j3_abs(x);
        it->m[1] = v < 1 ? 1 : v;
        it->m[2] = x > 0 ? 1 : -1;
        it->mover = 2;
        it->spot_idx = 255;
        it->facing = (int8_t)(x > 0 ? 1 : -1);
      }
      break;
    }
    case J3O_HOP: {
      int32_t half = sl->w * sl->scale * 8, x1, h;
      int on = it->perch != 0;
      J3_POP(y); J3_POP(x);
      x1 = j3_clampi((int32_t)it->x + (on ? 0 : x), half + 32, 3840 - half - 32);
      h = y;
      j3_path(j, si, x1, it->y, 640, h, 0, -1, 1);   /* a perch is kept */
      break;
    }
    case J3O_SIGNAL:
      if (b) {
        int o;
        J3_POP(x);
        o = j3_handle(j, x);
        if (o >= 0) j3_queue(j->slot[o].it, J3E_signal, a, si + 1);
      } else {
        int o;
        for (o = 0; o < j->nslots; o++)
          if (o != si && j->slot[o].used) j3_queue(j->slot[o].it, J3E_signal, a, si + 1);
      }
      break;
    case J3O_ACT:
      x = 0;
      if (b) J3_POP(x);
      if (a >= J3_NACTIONS) goto fault;
      if (H && H->act) H->act(H->ctx, si, a, x);
      break;
    default: goto fault;
    }
    pc = next;
  }
fault:
  *out = pc;
  *ppc = (uint16_t)pc;
  *psp = (uint8_t)(sp < 0 ? 0 : sp);
  return J3R_FAULT;
#undef J3_POP
#undef J3_PUSH
}

/* The handler for (ev, arg): its index, or -1. A near event names its
 * handler: arg is its index + 1. */
static JV3_OPT int j3_handler(const Jv3Slot *sl, int ev, int arg) {
  const uint8_t *s = sl->s;
  const Jv3Item *it = sl->it;
  int i;
  if (ev == J3E_near) {
    const uint8_t *h;
    if (arg < 1 || arg > s[4]) return -1;
    h = s + it->hoff + 7 * (arg - 1);
    if (h[0] != J3E_near || (h[2] != 255 && h[2] != it->state)) return -1;
    return arg - 1;
  }
  for (i = 0; i < s[4]; i++) {
    const uint8_t *h = s + it->hoff + 7 * i;
    if (h[0] != ev || (h[2] != 255 && h[2] != it->state)) continue;
    if (ev == J3E_chime) {
      int p = j3_u16(h + 3);
      if (p < 1 || arg <= 0 || arg % p != 0) continue;
    } else if (h[1] && h[1] != arg) continue;
    return i;
  }
  return -1;
}

/* Run handler i with other and impact; 1 if the item is still running after
 * it. With no budget left it does not start: dropped, not a fault. */
static JV3_OPT int j3_run_handler(Jv3Jar *j, int si, int i, int other, int impact, int *budget) {
  Jv3Slot *sl = &j->slot[si];
  int16_t st[JV3_STACK];
  uint8_t sp = 0;
  uint16_t pc;
  int out = 0, r;
  if (*budget <= 0) return 1;
  sl->it->other = (uint8_t)other;
  sl->it->impact = (int16_t)impact;
  pc = (uint16_t)j3_u16(sl->s + sl->it->hoff + 7 * i + 5);
  r = j3_exec(j, si, &pc, st, &sp, 0, budget, &out);
  if (r == J3R_FAULT || r == J3R_BUDGET) j3_fault(j, si, out);
  return !sl->it->stopped;
}

static JV3_OPT void j3_run(Jv3Jar *j, int si) {
  Jv3Slot *sl = &j->slot[si];
  Jv3Item *it = sl->it;
  const uint8_t *s = sl->s;
  int budget = jv3_budget(sl), e, i, n = it->qready, ne = 0;
  /* b: the events that were waiting */
  for (e = 0; e < n && !it->stopped; e++) {
    Jv3Ev ev = it->q[e];
    int h = j3_handler(sl, ev.ev, ev.arg);
    if (h < 0) continue;
    j3_run_handler(j, si, h, ev.other, (ev.ev == J3E_hit || ev.ev == J3E_bump) ? ev.arg : 0, &budget);
  }
  if (it->stopped) return;
  for (e = n; e < it->qn; e++) it->q[e - n] = it->q[e];
  it->qn = (uint8_t)(it->qn - n);
  it->qready = 0;
  /* c: one tick */
  i = j3_handler(sl, J3E_tick, 0);
  if (i >= 0 && !j3_run_handler(j, si, i, 0, 0, &budget)) return;
  /* d: every */
  for (i = 0; i < s[4]; i++) {
    const uint8_t *h = s + it->hoff + 7 * i;
    if (h[0] != J3E_every) continue;
    if (ne < JV3_EVERYS && (h[2] == 255 || h[2] == it->state)) {
      if (it->every[ne] > 0) it->every[ne]--;
      if (it->every[ne] == 0) {
        it->every[ne] = (uint16_t)j3_u16(h + 3);
        if (!j3_run_handler(j, si, i, 0, 0, &budget)) return;
      }
    }
    ne++;
  }
  /* e: a pending go */
  if (it->pending != 255) j3_enter(j, si, it->pending);
  /* f: the body */
  if (it->halted) return;
  if (it->wait > 0 && --it->wait > 0) return;
  for (i = 0; i < 4; i++) {
    int out = 0, r = j3_exec(j, si, &it->pc, it->stack, &it->sp, 1, &budget, &out);
    if (r == J3R_GO) { j3_enter(j, si, out); continue; }
    if (r == J3R_HALT) it->halted = 1;
    if (r == J3R_FAULT) { it->halted = 1; j3_fault(j, si, out); }
    break;
  }
}

static JV3_OPT void j3_near(Jv3Jar *j, int si) {
  Jv3Slot *sl = &j->slot[si];
  Jv3Item *it = sl->it;
  const uint8_t *s = sl->s;
  int i, nn = 0, o;
  for (i = 0; i < s[4] && nn < JV3_NEARS; i++) {
    const uint8_t *h = s + it->hoff + 7 * i;
    uint32_t now = 0;
    int32_t rad;
    if (h[0] != J3E_near) continue;
    if (h[2] != 255 && h[2] != it->state) { it->nearm[nn++] = 0; continue; }
    rad = (int32_t)j3_u16(h + 3) * 16;
    for (o = 0; o < j->nslots; o++) {
      if (o == si || !j->slot[o].used || !j3_has(j, o, h[1] - 1)) continue;
      if (j3_dist(j, si, o) > rad) continue;
      now |= 1u << o;
      if (!(it->nearm[nn] >> o & 1)) j3_queue(it, J3E_near, i + 1, o + 1);   /* which handler saw it */
    }
    it->nearm[nn++] = now;
  }
}

static JV3_OPT int j3_live(const Jv3Jar *j, int si) {
  const Jv3Slot *s = &j->slot[si];
  return s->used && s->it && !s->it->stopped;
}

/* One 1/20 s tick: near, run, move (the spec's "One tick"). */
static JV3_OPT void jv3_tick(Jv3Jar *j) {
  int si;
  for (si = 0; si < j->nslots; si++)        /* a host that moved an item */
    if (j3_live(j, si)) {
      Jv3Slot *s = &j->slot[si];
      if (s->x != s->it->x || s->y != s->it->y) { s->it->x = s->x; s->it->y = s->y; }
    }
  for (si = 0; si < j->nslots; si++) if (j3_live(j, si)) j->slot[si].it->qready = j->slot[si].it->qn;
  for (si = 0; si < j->nslots; si++) if (j3_live(j, si)) j3_near(j, si);
  for (si = 0; si < j->nslots; si++) if (j3_live(j, si)) j3_run(j, si);
  for (si = 0; si < j->nslots; si++) if (j3_live(j, si)) j3_move(j, si);
  for (si = 0; si < j->nslots; si++)
    if (j->slot[si].used && j->slot[si].it) { j->slot[si].x = j->slot[si].it->x; j->slot[si].y = j->slot[si].it->y; }
  j->tick++;
}

/* The host put a scripted item somewhere itself (thrown, placed, dragged):
 * its path or walk is cancelled, its perch left, its velocity zeroed; the
 * slot's x, y already hold the new place. */
static JV3_OPT void jv3_moved(Jv3Jar *j, int si) {
  Jv3Item *it;
  if (si < 0 || si >= j->nslots || !(it = j->slot[si].it)) return;
  it->x = j->slot[si].x;
  it->y = j->slot[si].y;
  it->mover = 0;
  it->spot_idx = 255;
  it->vx = it->vy = 0;
  j3_leave_perch(it);
}

/* Slot bit k gone: the bits above it come down one (ledge bits 24.. stay). */
static JV3_OPT uint32_t j3_dropbit(uint32_t m, int k, int ledges) {
  uint32_t keep = ledges ? (m & 0xFF000000u) : 0, s = ledges ? (m & 0x00FFFFFFu) : m;
  s = (s & ((1u << k) - 1)) | ((s >> (k + 1)) << k);
  return keep | s;
}

/* A handle (slot + 1, 0 none) after slot k left: k's -> 0, above -> one down. */
static JV3_OPT uint8_t j3_rehandle(uint8_t h, int k) {
  return (uint8_t)(h == k + 1 ? 0 : h > k + 1 ? h - 1 : h);
}

/* The item in slot k leaves the jar: slots above it move down one (the host
 * moves the Jv3Item storage with its slot, so nothing here keeps a pointer
 * from one item to another -- references are slot numbers, fixed below).
 * A handle kept in a script's vars is the script's business: it may now
 * name another item, or nobody. */
static JV3_OPT void jv3_remove(Jv3Jar *j, int k) {
  int o, i;
  if (k < 0 || k >= j->nslots) return;
  for (o = k; o + 1 < j->nslots; o++) {   /* by bytes: a struct copy is a memcpy, and apps have none */
    uint8_t *d = (uint8_t *)&j->slot[o];
    const uint8_t *f = (const uint8_t *)&j->slot[o + 1];
    for (i = 0; i < (int)sizeof j->slot[o]; i++) d[i] = f[i];
  }
  j->nslots--;
  {
    Jv3Slot *z = &j->slot[j->nslots];
    uint8_t *b = (uint8_t *)z;
    for (i = 0; i < (int)sizeof *z; i++) b[i] = 0;
  }
  for (o = 0; o < j->nslots; o++) {
    Jv3Item *it = j->slot[o].it;
    if (!j->slot[o].used || !it) continue;
    if (it->perch && it->perch_owner) {
      if (it->perch_owner == k + 1) j3_leave_perch(it);
      else if (it->perch_owner > k + 1) it->perch_owner--;
    }
    if (it->mover == 1 && it->spot_idx != 255 && it->spot_owner) {
      if (it->spot_owner == k + 1) it->spot_idx = 255;   /* the path goes on; no perch at its end */
      else if (it->spot_owner > k + 1) it->spot_owner--;
    }
    it->other = j3_rehandle(it->other, k);
    for (i = 0; i < it->qn; i++) it->q[i].other = j3_rehandle(it->q[i].other, k);
    for (i = 0; i < JV3_NEARS; i++) it->nearm[i] = j3_dropbit(it->nearm[i], k, 0);
    it->touch = j3_dropbit(it->touch, k, 1);
  }
}

#endif /* CARDOS_JV3_H */
