/* Game Night's dice (apps/gamenight.c): N dice of S sides plus or minus M,
 * written the way players write it -- "2d6", "d20", "3d6+2", "d%" (a d100),
 * "1d4-1" -- rolled with the dice a player chose to hold kept as they were
 * (Yahtzee). A d2 is a coin.
 *
 * Portable and libc-free; host-tested in test/test_gamenight.c. */
#ifndef CARDOS_GN_DICE_H
#define CARDOS_GN_DICE_H

#include <stdint.h>

#if defined(__GNUC__)
#define GD_OPT __attribute__((unused))
#else
#define GD_OPT
#endif

#define GD_MAXN   9                     /* dice at once: what fits on the screen */
#define GD_MAXS   1000
#define GD_MAXM   999

typedef struct {
  int      n, sides, mod;
  int      v[GD_MAXN];                  /* the faces, 1..sides; 0 not rolled */
  uint8_t  hold[GD_MAXN];
  uint32_t rng;
} GDice;

/* Sides a player steps through with left and right. */
static GD_OPT const int16_t GD_SIDES[] = { 2, 4, 6, 8, 10, 12, 20, 100 };
#define GD_NSIDES ((int)(sizeof GD_SIDES / sizeof GD_SIDES[0]))

static GD_OPT uint32_t gd_rnd(GDice *d) {
  uint32_t x = d->rng ? d->rng : 0x6D2B79F5u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  d->rng = x;
  return x;
}

/* 1..sides, without the bias of a plain % for a side count that does not
 * divide 2^32. */
static GD_OPT int gd_face(GDice *d, int sides) {
  uint32_t lim = 0xFFFFFFFFu - (0xFFFFFFFFu % (uint32_t)sides), x;
  do { x = gd_rnd(d); } while (x >= lim);
  return 1 + (int)(x % (uint32_t)sides);
}

static GD_OPT void gd_init(GDice *d, uint32_t seed) {
  int i;
  d->n = 2;
  d->sides = 6;
  d->mod = 0;
  d->rng = seed ? seed : 1u;
  for (i = 0; i < GD_MAXN; i++) { d->v[i] = 0; d->hold[i] = 0; }
}

static GD_OPT void gd_unhold(GDice *d) {
  int i;
  for (i = 0; i < GD_MAXN; i++) d->hold[i] = 0;
}

/* "2d6+1" and the like into d (its faces cleared). 0, or -1 if it is not
 * dice, leaving d as it was. A bare number is a die with that many sides. */
static GD_OPT int gd_parse(GDice *d, const char *s) {
  int n = 0, sides = 0, mod = 0, any_n = 0, sign = 1, i;
  while (*s == ' ') s++;
  while (*s >= '0' && *s <= '9') { if (n < 1000) n = n * 10 + (*s - '0'); s++; any_n = 1; }
  if (*s == 'd' || *s == 'D') {
    s++;
    if (*s == '%') { sides = 100; s++; }
    else while (*s >= '0' && *s <= '9') { if (sides < 100000) sides = sides * 10 + (*s - '0'); s++; }
    if (!any_n) n = 1;
  } else if (any_n) {
    sides = n;                                    /* "20": a d20 */
    n = 1;
  }
  while (*s == ' ') s++;
  if (*s == '+' || *s == '-') {
    int any = 0;
    sign = *s == '-' ? -1 : 1;
    s++;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') { if (mod < 100000) mod = mod * 10 + (*s - '0'); s++; any = 1; }
    if (!any) return -1;
  }
  while (*s == ' ') s++;
  if (*s || n < 1 || n > GD_MAXN || sides < 2 || sides > GD_MAXS || mod > GD_MAXM) return -1;
  d->n = n;
  d->sides = sides;
  d->mod = sign * mod;
  for (i = 0; i < GD_MAXN; i++) { d->v[i] = 0; d->hold[i] = 0; }
  return 0;
}

/* The dice as written: "2d6", "3d6+2", "d20". */
static GD_OPT void gd_name(const GDice *d, char *out, int cap) {
  char t[24];
  int k = 0, i;
  int parts[3];
  parts[0] = d->n; parts[1] = d->sides; parts[2] = d->mod < 0 ? -d->mod : d->mod;
  for (i = 0; i < 3; i++) {
    char dig[8];
    int m = 0, v = parts[i];
    if (i == 0 && v == 1) { t[k++] = 'd'; continue; }
    if (i == 2) {
      if (!d->mod) break;
      t[k++] = d->mod < 0 ? '-' : '+';
    }
    do { dig[m++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (m) t[k++] = dig[--m];
    if (i == 0) t[k++] = 'd';
  }
  t[k] = 0;
  for (i = 0; i < cap - 1 && t[i]; i++) out[i] = t[i];
  out[i] = 0;
}

/* Roll every die not held. */
static GD_OPT void gd_roll(GDice *d) {
  int i;
  for (i = 0; i < d->n; i++)
    if (!d->hold[i] || !d->v[i]) d->v[i] = gd_face(d, d->sides);
}

/* While the dice tumble: faces that change, held ones still. */
static GD_OPT void gd_jiggle(GDice *d) {
  gd_roll(d);
}

static GD_OPT int gd_rolled(const GDice *d) {
  int i;
  for (i = 0; i < d->n; i++) if (!d->v[i]) return 0;
  return d->n > 0;
}

static GD_OPT int32_t gd_total(const GDice *d) {
  int32_t t = d->mod;
  int i;
  for (i = 0; i < d->n; i++) t += d->v[i];
  return t;
}

/* More or fewer dice (1..GD_MAXN); the new ones unrolled. */
static GD_OPT void gd_set_n(GDice *d, int n) {
  int i;
  if (n < 1) n = 1;
  if (n > GD_MAXN) n = GD_MAXN;
  for (i = d->n; i < n; i++) { d->v[i] = 0; d->hold[i] = 0; }
  d->n = n;
}

/* The next or previous of GD_SIDES (a custom count goes to the nearest one
 * that way); every face cleared. */
static GD_OPT void gd_step_sides(GDice *d, int dir) {
  int i, at;
  for (at = 0; at < GD_NSIDES && GD_SIDES[at] < d->sides; at++) {}   /* first not smaller */
  if (at < GD_NSIDES && GD_SIDES[at] == d->sides) at += dir;
  else if (dir < 0) at--;                         /* a custom count: to the one below */
  if (at < 0) at = GD_NSIDES - 1;
  if (at >= GD_NSIDES) at = 0;
  d->sides = GD_SIDES[at];
  for (i = 0; i < GD_MAXN; i++) { d->v[i] = 0; d->hold[i] = 0; }
}

#endif
