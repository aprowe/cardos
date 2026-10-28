/* The jar's side of JarScript 3 (docs/superpowers/specs/
 * 2026-10-14-jarscript3-design.md): items whose script is format 3 run on
 * apps/jar/jv3.h instead of the v2 machine, which is untouched -- a v2 item
 * runs exactly as it did, and a v3 script never reaches it (jv_check wants
 * byte 0 to be 1).
 *
 * MEMORY. The machine owns nothing. The app hands the jar one block
 * (js_v3_open; on the device a big-mode block, CAPP_BIG): a Jv3Jar at its
 * start, then a chunk per scripted item -- its Jv3Item, then its script --
 * packed end to end and closed up when an item leaves, as the v2 spool is.
 * An item whose chunk does not fit is placed without its script: its recipe
 * (habits) if it has one, else its motion parts alone. Nothing fails to open
 * for want of room, and the shortfall is logged (j->v3short).
 *
 * SLOTS mirror the placed list: the machine's slot i is j->placed[i], and
 * js_unplace closes both up together (jv3_remove). Every placed item is a
 * slot, scripted or not, so scripts can find, perch on and bump into
 * anything. Positions: a scripted item's are the machine's, written back to
 * the placed item after each tick (x in sub-pixels; yoff so js_item_y and
 * the scene's item_at put its feet where the machine says).
 *
 * The machine ticks 20 times a second: every second step of the sim's 40.
 *
 * Portable and libc-free like the rest; host-tested in apps/jar/test/test_jarv3.c.
 */
#ifndef CARDOS_JARV3_H
#define CARDOS_JARV3_H

#include "apps/jar/jv3.h"

#define JS_V3_EVERY   2                 /* sim steps a machine tick */
#define JS_V3_ALIGN   4

/* The 32 trait words, as the record's tag field spells them. */
static const char *const JS_V3_TRAITS[] = {
#define J3_OP(c, n, b, m)
#define J3_EVENT(n, w, f, m)
#define J3_SENSE(n, w, k, m)
#define J3_FUNC(n, w, a, b, m)
#define J3_FIELD(n, w, m)
#define J3_TARGET(n, w)
#define J3_ACTION(n, w, a, m)
#define J3_PARTICLE(n, w)
#define J3_NAME(g, w, v) JS_V3_NAME_##g(#w)
#define JS_V3_NAME_trait(w) w,
#define JS_V3_NAME_time(w)
#define JS_V3_NAME_weather(w)
#define JS_V3_NAME_jam(w)
#define JS_V3_NAME_world(w)
#define JS_V3_NAME_zone(w)
#define JS_V3_NAME_glow(w)
#define JS_V3_NAME_sound(w)
#include "apps/jar/jarvm3.def"
#undef J3_OP
#undef J3_EVENT
#undef J3_SENSE
#undef J3_FUNC
#undef J3_FIELD
#undef J3_TARGET
#undef J3_ACTION
#undef J3_PARTICLE
#undef J3_NAME
};
#define JS_V3_NTRAITS ((int)(sizeof JS_V3_TRAITS / sizeof JS_V3_TRAITS[0]))

/* "shiny,critter" -> bits. */
static JS_OPT uint32_t js_v3_traits(const char *tags) {
  uint32_t bits = 0;
  const char *p = tags;
  while (*p) {
    const char *w = p;
    int n = 0, k;
    while (p[n] && p[n] != ',') n++;
    for (k = 0; k < JS_V3_NTRAITS; k++) {
      const char *t = JS_V3_TRAITS[k];
      int m = 0;
      while (m < n && t[m] && t[m] == w[m]) m++;
      if (m == n && !t[m]) { bits |= (uint32_t)1 << k; break; }
    }
    p += n;
    if (*p == ',') p++;
    while (*p == ' ') p++;
  }
  return bits;
}

static JS_OPT int js_v3_is(const uint8_t *s, int n) { return n > 0 && s[0] == JV3_FORMAT; }

static JS_OPT Jv3Jar *js_v3(const Jar *j) { return (Jv3Jar *)j->v3mem; }

static JS_OPT int js_v3_chunk(int script_len) {
  int n = (int)sizeof(Jv3Item) + script_len;
  return (n + JS_V3_ALIGN - 1) & ~(JS_V3_ALIGN - 1);
}

/* The block: the Jv3Jar, its host table (written at run time, so not a
 * constant -- an app's constants are in flash), then the chunks. */
static JS_OPT int js_v3_base(void) {
  return ((int)(sizeof(Jv3Jar) + sizeof(Jv3Host)) + JS_V3_ALIGN - 1) & ~(JS_V3_ALIGN - 1);
}

static JS_OPT Jv3Host *js_v3_host(const Jar *j) { return (Jv3Host *)(j->v3mem + sizeof(Jv3Jar)); }

/* ---- the host: what a script reaches of the jar ------------------------------ */

static JS_OPT int js_sense(Jar *j, int i, int sense);
static JS_OPT int js_act(Jar *j, int i, int action, int arg);
static JS_OPT int js_particle(Jar *j, int type, int x, int y);

static JS_OPT int16_t js_v3_sense(void *ctx, int slot, int k) {
  static const uint8_t map[] = {           /* the def's jar senses, 26.. */
    JSN_HOUR, JSN_TIME, JSN_WEATHER, JSN_DAYS, JSN_BERRIES, JSN_FED, JSN_WORLD,
    JSN_ZONE, JSN_GIFT, JSN_MUSIC, JSN_JAMMED, JSN_SULKING };
  Jar *j = (Jar *)ctx;
  int v;
  if (k < 26 || k >= 26 + (int)sizeof map || slot < 0 || slot >= j->nplaced) return 0;
  v = js_sense(j, slot, map[k - 26]);
  if (v > 2047) v = 2047;
  if (v < -2048) v = -2048;
  return (int16_t)(v * 16);
}

static JS_OPT void js_v3_act(void *ctx, int slot, int k, int16_t arg) {
  static const uint8_t map[] = {           /* the def's J3_ACTION order */
    JA_SOUND, JA_GLOW, JA_EAT, JA_THROW, JA_DROP, JA_BOOST, JA_NUDGE, JA_SHAKE, JA_FLY, JA_HOME };
  Jar *j = (Jar *)ctx;
  if (k < 0 || k >= (int)sizeof map || slot < 0 || slot >= j->nplaced) return;
  js_act(j, slot, map[k], arg / 16);
}

static JS_OPT void js_v3_say(void *ctx, int slot, const char *text, int ticks) {
  (void)ctx; (void)slot; (void)text; (void)ticks;   /* the scene reads the item's own text */
}

static JS_OPT void js_v3_emit(void *ctx, int slot, int kind, int n) {
  /* leaf dust feather spark heart note -> the jar's particles */
  static const uint8_t map[] = { JP_PUFF, JP_PUFF, JP_PUFF, JP_SPARKLE, JP_HEART, JP_NOTE };
  Jar *j = (Jar *)ctx;
  Jv3Jar *v = js_v3(j);
  int x, y;
  if (!v || kind < 0 || kind >= (int)sizeof map || slot < 0 || slot >= j->nplaced) return;
  x = v->slot[slot].x / 16;
  y = v->slot[slot].y / 16 - 8 * v->slot[slot].scale;
  if (n > 12) n = 12;
  while (n-- > 0) js_particle(j, map[kind], x, y);
}

/* ---- the block ---------------------------------------------------------------- */

/* The app's block for v3: `cap` bytes at `mem` (NULL: no v3 -- format 3
 * items are placed without their scripts). Call before anything is placed. */
static JS_OPT void js_v3_open(Jar *j, void *mem, int cap) {
  Jv3Jar *v;
  j->v3mem = (uint8_t *)mem;
  j->v3cap = 0;
  j->v3used = 0;
  if (!mem || cap < js_v3_base() + js_v3_chunk(16)) { j->v3mem = 0; return; }
  j->v3cap = (uint16_t)(cap > 65535 ? 65535 : cap);
  j->v3used = (uint16_t)js_v3_base();
  v = js_v3(j);
  ji_zero(v, (int)(sizeof *v + sizeof(Jv3Host)));
  js_v3_host(j)->sense = js_v3_sense;
  js_v3_host(j)->act = js_v3_act;
  js_v3_host(j)->say = js_v3_say;
  js_v3_host(j)->emit = js_v3_emit;
  v->host = js_v3_host(j);
  v->rng = j->seed ? j->seed : 0x2545F491u;
  v->max_bodies = 12;                        /* 24 measured at 0.5% of a tick */
}

/* Re-point every scripted slot at its chunk (chunks move when one leaves). */
static JS_OPT void js_v3_bind(Jar *j) {
  Jv3Jar *v = js_v3(j);
  int i;
  if (!v) return;
  js_v3_host(j)->ctx = j;                     /* the Jar may have moved (a save, a screen) */
  v->host = js_v3_host(j);
  v->nslots = j->nplaced;
  for (i = 0; i < j->nplaced; i++) {
    JPlaced *p = &j->placed[i];
    Jv3Slot *s = &v->slot[i];
    s->used = 1;
    if (p->v3) {
      s->it = (Jv3Item *)(j->v3mem + p->v3off);
      s->s = j->v3mem + p->v3off + sizeof(Jv3Item);
      s->n = p->v3len;
      s->mem = p->mem;
    } else {
      s->it = 0;
      s->s = 0;
      s->n = 0;
    }
  }
}

/* An unscripted item's place, as the machine sees it: middle x, feet y. */
static JS_OPT void js_v3_place_of(const Jar *j, int i, int16_t *x, int16_t *y) {
  const JPlaced *p = &j->placed[i];
  int feet = js_item_y(j, i) + 8;              /* js_item_y is the middle of a 16 px box */
  *x = (int16_t)(p->x / JS_FX * 16);
  *y = (int16_t)(feet * 16);
}

/* Item i (just placed, from `it`): its v3 script given a chunk and started.
 * 1 running, 0 not a v3 item, -1 a v3 item that could not run (no block,
 * no room, refused): it is placed without its script. */
static JS_OPT int js_v3_attach(Jar *j, int i, const JItem *it) {
  JPlaced *p = &j->placed[i];
  Jv3Jar *v = js_v3(j);
  Jv3Slot *s;
  int16_t x0, y0;
  int need;
  if (!js_v3_is(it->script, it->script_len)) return 0;
  if (!j->v3mem && j->v3_want) {               /* the first v3 item: the block now */
    j->v3_want(j);
    v = js_v3(j);
  }
  js_v3_place_of(j, i, &x0, &y0);              /* where the sim put it, before it is v3 */
  if (!v || jv3_check(it->script, it->script_len) != 0) { j->v3short++; return -1; }
  need = js_v3_chunk(it->script_len);
  if (j->v3used + need > j->v3cap) { j->v3short++; return -1; }
  p->v3 = 1;
  p->v3off = j->v3used;
  p->v3len = it->script_len;
  j->v3used = (uint16_t)(j->v3used + need);
  ji_zero(j->v3mem + p->v3off, (int)sizeof(Jv3Item));
  ji_copy(j->v3mem + p->v3off + sizeof(Jv3Item), it->script, it->script_len);
  js_v3_bind(j);
  s = &v->slot[i];
  s->x = x0;
  s->y = y0;
  s->w = 16;
  s->h = 16;
  s->scale = p->scale;
  s->nframes = p->nframes;
  s->traits = js_v3_traits(it->tags);
  if (jv3_place(v, i) != 0) {                  /* refused after all: no script */
    j->v3used = (uint16_t)(j->v3used - need);
    p->v3 = 0;
    p->v3off = p->v3len = 0;
    js_v3_bind(j);
    j->v3short++;
    return -1;
  }
  p->nhab = 0;                                 /* the script does everything; no recipe beside it */
  return 1;
}

/* Item i is leaving the placed list: its chunk closed up, the slots too. */
static JS_OPT void js_v3_detach(Jar *j, int i) {
  JPlaced *p = &j->placed[i];
  Jv3Jar *v = js_v3(j);
  int k;
  if (!v) return;
  js_v3_bind(j);
  if (p->v3) {
    int at = p->v3off, n = js_v3_chunk(p->v3len);
    ji_copy(j->v3mem + at, j->v3mem + at + n, j->v3used - at - n);
    j->v3used = (uint16_t)(j->v3used - n);
    for (k = 0; k < j->nplaced; k++)
      if (j->placed[k].v3 && j->placed[k].v3off > at)
        j->placed[k].v3off = (uint16_t)(j->placed[k].v3off - n);
    p->v3 = 0;
    js_v3_bind(j);
  }
  jv3_remove(v, i);
}

/* The sim moved item i itself (placed, dragged, put on furniture). */
static JS_OPT void js_v3_moved(Jar *j, int i) {
  Jv3Jar *v = js_v3(j);
  if (!v || i < 0 || i >= j->nplaced || !j->placed[i].v3) return;
  js_v3_bind(j);
  j->placed[i].v3 = 0;                         /* read its place the way an unscripted one is */
  js_v3_place_of(j, i, &v->slot[i].x, &v->slot[i].y);
  j->placed[i].v3 = 1;
  jv3_moved(v, i);
}

/* One machine tick: everything's place in, the tick, the scripted ones' out. */
static JS_OPT void js_v3_tick(Jar *j) {
  Jv3Jar *v = js_v3(j);
  int i, any = 0;
  if (!v) return;
  for (i = 0; i < j->nplaced; i++) any |= j->placed[i].v3;
  if (!any) return;
  js_v3_bind(j);
  for (i = 0; i < j->nplaced; i++) {
    JPlaced *p = &j->placed[i];
    Jv3Slot *s = &v->slot[i];
    if (!p->v3) {
      js_v3_place_of(j, i, &s->x, &s->y);
      s->w = 16;
      s->h = 16;
      s->scale = p->scale;
      s->nframes = p->nframes;
      s->traits = 0;
    }
  }
  jv3_tick(v);
  for (i = 0; i < j->nplaced; i++) {
    JPlaced *p = &j->placed[i];
    const Jv3Slot *s = &v->slot[i];
    const Jv3Item *it = s->it;
    int k;
    uint16_t sum = 0;
    if (!p->v3 || !it) continue;
    p->x = p->tx = (int32_t)s->x * (JS_FX / 16);
    p->lift = 0;
    p->home_y = 0;
    p->yoff = (int16_t)(JS_SOIL * 16 - s->y);  /* js_item_y: feet at the machine's y */
    p->cur = it->frame;
    p->flip = it->facing < 0;
    for (k = 0; k < JI_MEM; k++) sum = (uint16_t)(sum * 31 + (uint16_t)p->mem[k]);
    if (sum != p->v3sum) { p->v3sum = sum; p->mem_dirty = 1; }
  }
}

/* A sim event for a v3 item, in v3's words. 1 if it was one (handled here). */
static JS_OPT int js_v3_event(Jar *j, int i, int ev, int arg) {
  Jv3Jar *v = js_v3(j);
  int e = -1, other = 0;
  if (!j->placed[i].v3 || !v) return 0;
  switch (ev) {
    case JE_TICK: case JE_NEAR: return 1;     /* the machine has its own */
    case JE_POKE:    e = 0; arg = 0; break;
    case JE_TIME:    e = 8; break;
    case JE_WEATHER: e = 9; break;
    case JE_JAM:     e = 10; break;
    case JE_GIFT:    e = 11; arg = 0; break;
    case JE_SHIPPED: e = 12; arg = 0; break;
    case JE_CHIME:   e = 13; break;
    case JE_SIGNAL:  e = 6; break;
    case JE_WORLD:   e = 14; break;
    case JE_NEW:     e = 15; other = j->nplaced; arg = 0; break;   /* the newest is last */
    case JE_BERRY:   e = 16; arg = 0; break;
    case JE_BUMPED:  e = 4; arg = 0; break;
    default: return 1;
  }
  js_v3_bind(j);
  jv3_event(v, i, e, arg, other);
  return 1;
}

/* What the scene draws for a v3 item: its say text (NULL none), sway and
 * shake. */
static JS_OPT const char *js_v3_said(const Jar *j, int i) {
  const Jv3Jar *v = js_v3(j);
  const Jv3Item *it;
  if (!v || i >= j->nplaced || !j->placed[i].v3) return 0;
  it = (const Jv3Item *)(j->v3mem + j->placed[i].v3off);
  return it->say_ticks > 0 && it->say[0] ? it->say : 0;
}

static JS_OPT const Jv3Item *js_v3_item(const Jar *j, int i) {
  if (!j->v3mem || i >= j->nplaced || !j->placed[i].v3) return 0;
  return (const Jv3Item *)(j->v3mem + j->placed[i].v3off);
}

/* A v3 item whose script stopped for good (8 faults) wears the "?". */
static JS_OPT int js_v3_stopped(const Jar *j, int i) {
  const Jv3Item *it = js_v3_item(j, i);
  return it && it->stopped;
}

#endif /* CARDOS_JARV3_H */
