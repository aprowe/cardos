/* Crossword's statistics: solves, times and the streak -- the parts a person
 * would argue about, kept apart from the screen so the host suite can pin
 * them (test/test_crossword.c).
 *
 * Portable C with no libc, on apps/crossword/xwcore.h's small helpers.
 *
 * What counts: a solve with no letter revealed (a "clean" one) is a solve
 * for its weekday -- the Times makes Monday easy and Saturday hard, so a time
 * means something only against the same weekday -- and sets that weekday's
 * best if it is the fastest. A solve with some letters revealed only adds to
 * `helped`; a puzzle revealed whole is not counted at all (the app decides
 * which is which). The streak is days in a row with a clean solve, by the
 * device's calendar day when it has one.
 *
 * The file (/var/crossword/stats.txt), a few lines of text:
 *
 *   cwstats 1
 *   wd 0 12 252 4500      weekday (0 Monday), clean solves, best seconds
 *                         (0 for none), seconds over all those solves
 *   ... seven of them
 *   helped 3
 *   streak 5 9 20456      the streak now, the best, and the day of the last
 *                         clean solve (0 for none)
 */
#ifndef CARDOS_XWSTATS_H
#define CARDOS_XWSTATS_H

#include <stdint.h>
#include "apps/crossword/xwcore.h"

#define XWS_NO_DAY (-100000)

/* What a clean solve did to its weekday's best. */
enum { XWS_NOT_BEST = 0, XWS_BEST = 1, XWS_FIRST = 2 };

typedef struct {
  int      solved[7];                  /* clean solves, by weekday, 0 Monday */
  uint32_t best[7];                    /* seconds; 0 for none */
  uint32_t total[7];                   /* seconds over the clean solves */
  int      helped;                     /* solves with some letters revealed */
  int      streak, best_streak;
  int32_t  last_day;                   /* the day of the last clean solve */
} XwStats;

static XW_OPT void xws_init(XwStats *s) {
  xw_zero(s, (int)sizeof *s);
  s->last_day = XWS_NO_DAY;
}

/* A clean solve in `secs` of a puzzle printed on weekday `wd` (0-6; anything
 * else counts towards the streak only), finished on calendar day `day`
 * (XWS_NO_DAY when the device has no clock). Says whether it set a best. */
static XW_OPT int xws_solve(XwStats *s, int wd, uint32_t secs, int32_t day) {
  int r = XWS_NOT_BEST;
  if (secs == 0) secs = 1;
  if (wd >= 0 && wd < 7) {
    if (s->best[wd] == 0) r = XWS_FIRST;
    else if (secs < s->best[wd]) r = XWS_BEST;
    if (r != XWS_NOT_BEST) s->best[wd] = secs;
    s->solved[wd]++;
    s->total[wd] += secs;
  }
  if (day != XWS_NO_DAY && day != s->last_day) {
    s->streak = day == s->last_day + 1 ? s->streak + 1 : 1;
    s->last_day = day;
    if (s->streak > s->best_streak) s->best_streak = s->streak;
  }
  return r;
}

/* A solve with letters revealed. */
static XW_OPT void xws_help(XwStats *s) { s->helped++; }

/* The average clean solve of a weekday, in seconds; 0 with none. */
static XW_OPT uint32_t xws_avg(const XwStats *s, int wd) {
  if (wd < 0 || wd >= 7 || s->solved[wd] <= 0) return 0;
  return s->total[wd] / (uint32_t)s->solved[wd];
}

static XW_OPT int xws_solved_all(const XwStats *s) {
  int i, n = 0;
  for (i = 0; i < 7; i++) n += s->solved[i];
  return n;
}

/* The streak as it stands on `today`: one not carried to yesterday or today is
 * over. With no clock (XWS_NO_DAY) it is as it was left. */
static XW_OPT int xws_streak_now(const XwStats *s, int32_t today) {
  if (today == XWS_NO_DAY) return s->streak;
  return (s->last_day == today || s->last_day == today - 1) ? s->streak : 0;
}

/* The file's text into buf; its length, or -1 if buf is too small. */
static XW_OPT int xws_format(const XwStats *s, char *buf, int n) {
  int at = 0, i;
  if (n > 0) buf[0] = 0;
  at = xw_cat(buf, n, at, "cwstats 1\n");
  for (i = 0; i < 7; i++) {
    at = xw_cat(buf, n, at, "wd ");
    at = xw_cat_num(buf, n, at, i);
    at = xw_cat(buf, n, at, " ");
    at = xw_cat_num(buf, n, at, s->solved[i]);
    at = xw_cat(buf, n, at, " ");
    at = xw_cat_num(buf, n, at, (int32_t)s->best[i]);
    at = xw_cat(buf, n, at, " ");
    at = xw_cat_num(buf, n, at, (int32_t)s->total[i]);
    at = xw_cat(buf, n, at, "\n");
  }
  at = xw_cat(buf, n, at, "helped ");
  at = xw_cat_num(buf, n, at, s->helped);
  at = xw_cat(buf, n, at, "\nstreak ");
  at = xw_cat_num(buf, n, at, s->streak);
  at = xw_cat(buf, n, at, " ");
  at = xw_cat_num(buf, n, at, s->best_streak);
  at = xw_cat(buf, n, at, " ");
  at = xw_cat_num(buf, n, at, s->last_day == XWS_NO_DAY ? 0 : s->last_day);
  at = xw_cat(buf, n, at, "\n");
  return at;
}

/* Read the file's text. What is not understood is left at nothing, so a
 * damaged file is a fresh start rather than a wrong number. */
static XW_OPT void xws_parse(XwStats *s, const char *text) {
  const char *p = text;
  xws_init(s);
  while (*p) {
    const char *e = p, *rest, *lend;
    int n, wd = 0, a = 0, b = 0, c = 0;
    while (*e && *e != '\n') e++;
    n = (int)(e - p);
    while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
    lend = p + n;
    if ((rest = xw_key(p, n, "wd")) != 0) {
      if (xw_int(&rest, lend, &wd) && xw_int(&rest, lend, &a) && xw_int(&rest, lend, &b) &&
          xw_int(&rest, lend, &c) && wd >= 0 && wd < 7) {
        s->solved[wd] = a;
        s->best[wd] = (uint32_t)b;
        s->total[wd] = (uint32_t)c;
      }
    } else if ((rest = xw_key(p, n, "helped")) != 0) {
      if (xw_int(&rest, lend, &a)) s->helped = a;
    } else if ((rest = xw_key(p, n, "streak")) != 0) {
      if (xw_int(&rest, lend, &a) && xw_int(&rest, lend, &b) && xw_int(&rest, lend, &c)) {
        s->streak = a;
        s->best_streak = b;
        s->last_day = c ? c : XWS_NO_DAY;
      }
    }
    p = *e ? e + 1 : e;
  }
}

#endif /* CARDOS_XWSTATS_H */
