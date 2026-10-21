/* Pain's log: the entries, how they are changed, saved, loaded and counted.
 *
 * Portable: no libc, no drawing, no clock of its own. The only thing it asks
 * of the device is the card, and only in pain_load and pain_save, through
 * apps/safefile.h. Everything else (adding, editing, deleting, the line
 * format, the statistics) is plain arithmetic over an array, so a host test
 * can drive it with nothing but this file. apps/pain.c is the screens around
 * it.
 *
 * An entry is one time the person noticed pain: which day, what time, how bad
 * (0-10), where (cramps, back, head, other), and up to PAIN_NOTE_MAX
 * characters of words. A day is the number of days since 1970-01-01 in the
 * person's own time, the number apps/datetime.h calls a day everywhere
 * (api->now is local time already), so "the same day" is one integer compare.
 *
 * ---- No clock ------------------------------------------------------------
 * A device that has never been on a network and was never told the date has
 * no day to give. The entry is still logged: its day is PAIN_NO_DAY (0, which
 * is 1970-01-01, a day no entry can be on) and its time PAIN_NO_TIME. Every
 * entry also has a seq, one more than the highest before it, so undated
 * entries keep the order they were made in and the app has a number to call
 * them by. In the statistics an undated entry counts as a day of its own,
 * because without a date it cannot be told from another day's; the daily
 * chart leaves them out, and pain_recent gives the last few by count
 * instead.
 *
 * ---- Order ---------------------------------------------------------------
 * The array is always sorted oldest first by (day, minute, seq), undated
 * entries before every dated one. So an entry's day-mates are the entries
 * beside it, the last entry is the latest, and History groups by day with a
 * compare against the row above. Adding or editing puts an entry where it
 * belongs, and says where that was: an index is good until the next change.
 *
 * ---- More than fits ------------------------------------------------------
 * The log holds the newest PAIN_MAX entries (11 KB of the app's 28; define
 * PAIN_MAX before including to change it). A card with more -- months of
 * bad days -- is not cut down: load keeps the newest PAIN_MAX, remembers the
 * key of the oldest one it kept (the floor), and pain_save writes back every
 * line on the card older than the floor exactly as it was, ahead of the
 * entries in memory. Adding to a full log retires the oldest in memory the
 * same way (its line is already on the card, because the app saves after
 * every change). So nothing is lost to a limit; the statistics are of what
 * is in memory, and `older` says how many entries are not. The one rule it
 * costs: nothing can be added or moved to before the floor (PAIN_E_OLD),
 * which would put the same entry in memory and in the carried lines.
 *
 * ---- The file ------------------------------------------------------------
 * /var/pain/log.txt, a line an entry, in tab-separated fields:
 *
 *     2026-10-21<TAB>14:05<TAB>7<TAB>cramps<TAB>12<TAB>after lunch
 *     -<TAB>-<TAB>4<TAB>head<TAB>13<TAB>
 *
 * date (or -), time (or -), level, area word, seq, note. Written with the
 * tabs and newlines of a note already turned to spaces, so a line is never
 * more than one line. Read forgivingly -- a time may be 9:05, an area word
 * in capitals, a missing seq or note, an unknown area word is "other" -- but
 * a line whose date, time or level cannot be read is skipped and counted in
 * `skipped`, and the file is rewritten without it by the next save. Blank
 * lines are neither. Saving goes through safe_begin/safe_commit, so a device
 * pulled from a pocket mid-save keeps the old file.
 *
 * An app has no memcpy, so entries are copied by hand (pain_copy) and never
 * with struct assignment, which compiles to one.
 */
#ifndef CARDOS_PAIN_LOG_H
#define CARDOS_PAIN_LOG_H

#include "kernel/app/capp.h"
#include "apps/datetime.h"
#include "apps/safefile.h"
#include "apps/str.h"

#if defined(__GNUC__)
#define PAIN_OPT __attribute__((unused))
#else
#define PAIN_OPT
#endif

#ifndef PAIN_MAX
#define PAIN_MAX 256                 /* entries held in memory */
#endif
#if PAIN_MAX < 2
#error "PAIN_MAX must be at least 2"
#endif

#define PAIN_NOTE_MAX   31           /* characters of note */
#define PAIN_LEVEL_MAX  10
#define PAIN_BAD        7            /* this level or more makes a bad day */
#define PAIN_CHART_DAYS 14           /* the days of the Trends chart */

#define PAIN_NO_DAY     0            /* no clock when it was logged */
#define PAIN_NO_TIME    0xFFFF       /* no time of day */
#define PAIN_DAY_MAX    2932896      /* 9999-12-31 */

#ifndef PAIN_DIR
#define PAIN_DIR  CAPP_VAR "/pain"
#endif
#ifndef PAIN_PATH
#define PAIN_PATH PAIN_DIR "/log.txt"
#endif

#define PAIN_LINE_MAX 80             /* a formatted line, newline and NUL included */
#define PAIN_LINE_IN  128            /* the longest line read; the rest of a longer one is dropped */

/* What a change can say. Not an index: indices are 0 and up. */
#define PAIN_OK       0
#define PAIN_E_ARG   (-1)            /* a level, area, day or index that cannot be */
#define PAIN_E_OLD   (-2)            /* before the oldest entry held, when more are on the card */

enum { PAIN_CRAMPS = 0, PAIN_BACK, PAIN_HEAD, PAIN_OTHER, PAIN_AREAS };

typedef struct {
  int32_t  day;                      /* days since 1970-01-01, local; PAIN_NO_DAY */
  uint32_t seq;                      /* one more than the entry before it was made */
  uint16_t minute;                   /* minutes since midnight; PAIN_NO_TIME */
  uint8_t  level;                    /* 0-10 */
  uint8_t  area;                     /* PAIN_CRAMPS .. PAIN_OTHER */
  char     note[PAIN_NOTE_MAX + 1];  /* NUL-ended; only up to the NUL is meaningful */
} PainEntry;

typedef struct {
  PainEntry e[PAIN_MAX];             /* e[0..n), oldest first */
  int       n;
  uint32_t  next_seq;
  int       skipped;                 /* lines the last load could not read */
  int       older;                   /* entries on the card that are not in e[] */
  int       has_floor;               /* older > 0 once: the key below which lines are carried */
  int32_t   floor_day;
  uint16_t  floor_min;
  uint32_t  floor_seq;
} PainLog;

typedef struct {
  int     n;                         /* entries counted */
  int     avg10;                     /* their mean level in tenths: 47 is 4.7; 0 when n is 0 */
  int     worst;                     /* the highest level; -1 when n is 0 */
  int32_t worst_day;                 /* the day of it (the latest, if tied); PAIN_NO_DAY if undated */
  int     days;                      /* days with an entry (each undated entry is one) */
  int     bad_days;                  /* of them, days whose worst entry was PAIN_BAD or more */
} PainStats;

/* ---- words ------------------------------------------------------------------- */

/* The area as the file writes it. Anything out of range is "other". */
static PAIN_OPT const char *pain_area_name(int area) {
  static const char *const NAME[PAIN_AREAS] = { "cramps", "back", "head", "other" };
  return (area >= 0 && area < PAIN_AREAS) ? NAME[area] : NAME[PAIN_OTHER];
}

/* The area as the screen shows it. */
static PAIN_OPT const char *pain_area_label(int area) {
  static const char *const LABEL[PAIN_AREAS] = { "Cramps", "Back", "Head", "Other" };
  return (area >= 0 && area < PAIN_AREAS) ? LABEL[area] : LABEL[PAIN_OTHER];
}

/* The area a file's word means, ignoring case; "other" for any word it does
 * not know, so a line from a newer Pain keeps its entry. */
static PAIN_OPT int pain_area_from_name(const char *s) {
  int a, i;
  for (a = 0; a < PAIN_AREAS; a++) {
    const char *n = pain_area_name(a);
    i = 0;
    while (n[i] && str_lower(s[i]) == n[i]) i++;
    if (!n[i] && !s[i]) return a;
  }
  return PAIN_OTHER;
}

/* One word for a level: none, mild (1-3), moderate (4-6), bad (7-9), worst (10).
 * "Bad" starts where PAIN_BAD does. */
static PAIN_OPT const char *pain_level_word(int level) {
  if (level <= 0) return "none";
  if (level <= 3) return "mild";
  if (level < PAIN_BAD) return "moderate";
  if (level < PAIN_LEVEL_MAX) return "bad";
  return "worst";
}

/* ---- one entry ------------------------------------------------------------------ */

/* `in` as a note: printable ASCII only, any run of anything else (tabs,
 * newlines, accents, spaces) one space, none at either end, at most
 * PAIN_NOTE_MAX characters. Into `out` (PAIN_NOTE_MAX + 1), which may be `in`
 * itself. The length. */
static PAIN_OPT int pain_clean_note(const char *in, char *out) {
  int n = 0, gap = 0;
  if (!in) { out[0] = 0; return 0; }
  for (; *in; in++) {
    unsigned char c = (unsigned char)*in;
    if (c <= ' ' || c > '~') { gap = 1; continue; }
    if (n >= PAIN_NOTE_MAX) break;
    if (gap && n) {
      if (n + 1 >= PAIN_NOTE_MAX) break;          /* no room for a space and a letter */
      out[n++] = ' ';
    }
    gap = 0;
    out[n++] = (char)c;
  }
  out[n] = 0;
  return n;
}

/* Copy by hand: see the top. The note up to its NUL, which is all that is read. */
static PAIN_OPT void pain_copy(PainEntry *d, const PainEntry *s) {
  int i;
  d->day = s->day;
  d->seq = s->seq;
  d->minute = s->minute;
  d->level = s->level;
  d->area = s->area;
  for (i = 0; i < PAIN_NOTE_MAX && s->note[i]; i++) d->note[i] = s->note[i];
  d->note[i] = 0;
}

/* Order of two entries' keys: -1, 0, 1. Undated (day 0) sorts first, a missing
 * time (the largest minute) last in its day, seq breaks every other tie. */
static PAIN_OPT int pain_key_cmp(int32_t da, uint16_t ma, uint32_t sa,
                                 int32_t db, uint16_t mb, uint32_t sb) {
  if (da != db) return da < db ? -1 : 1;
  if (ma != mb) return ma < mb ? -1 : 1;
  if (sa != sb) return sa < sb ? -1 : 1;
  return 0;
}

static PAIN_OPT int pain_cmp(const PainEntry *a, const PainEntry *b) {
  return pain_key_cmp(a->day, a->minute, a->seq, b->day, b->minute, b->seq);
}

/* An entry from its parts, checked: PAIN_OK, or PAIN_E_ARG for a day outside
 * 0..PAIN_DAY_MAX, a level outside 0-10 or an area that is not one. A minute
 * outside 0..1439 (-1, say) is "no time", and so is any minute of an undated
 * entry, so undated entries sort by seq alone. seq is left 0 for the caller. */
static PAIN_OPT int pain_make(PainEntry *t, int32_t day, int minute, int level, int area,
                              const char *note) {
  if (day < 0 || day > PAIN_DAY_MAX) return PAIN_E_ARG;
  if (level < 0 || level > PAIN_LEVEL_MAX) return PAIN_E_ARG;
  if (area < 0 || area >= PAIN_AREAS) return PAIN_E_ARG;
  t->day = day;
  t->minute = (day == PAIN_NO_DAY || minute < 0 || minute > 1439) ? PAIN_NO_TIME : (uint16_t)minute;
  t->level = (uint8_t)level;
  t->area = (uint8_t)area;
  t->seq = 0;
  pain_clean_note(note, t->note);
  return PAIN_OK;
}

/* The day and minute to log for a clock reading from api->now: PAIN_NO_DAY
 * and -1 when the clock is not set, else the local day and minutes since
 * midnight. Hand the two straight to pain_add. */
static PAIN_OPT void pain_stamp(const CappTime *t, int32_t *day, int *minute) {
  int32_t d = 0;
  *day = PAIN_NO_DAY;
  *minute = -1;
  if (!t->synced) return;
  d = dt_days_from_civil(t->year, t->month, t->day);
  if (d <= 0 || d > PAIN_DAY_MAX) return;
  *day = d;
  *minute = t->hour * 60 + t->min;
}

/* ---- the log ---------------------------------------------------------------------- */

static PAIN_OPT void pain_init(PainLog *l) {
  l->n = 0;
  l->next_seq = 1;
  l->skipped = 0;
  l->older = 0;
  l->has_floor = 0;
  l->floor_day = 0;
  l->floor_min = 0;
  l->floor_seq = 0;
}

/* Where an entry with this key goes: after every entry that is not later. */
static PAIN_OPT int pain_upper(const PainLog *l, int32_t day, uint16_t minute, uint32_t seq) {
  int lo = 0, hi = l->n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    const PainEntry *m = &l->e[mid];
    if (pain_key_cmp(m->day, m->minute, m->seq, day, minute, seq) <= 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

static PAIN_OPT void pain_remove_at(PainLog *l, int idx) {
  int i;
  for (i = idx; i + 1 < l->n; i++) pain_copy(&l->e[i], &l->e[i + 1]);
  l->n--;
}

/* Room is the caller's business: n < PAIN_MAX. */
static PAIN_OPT void pain_insert_at(PainLog *l, int at, const PainEntry *t) {
  int i;
  for (i = l->n; i > at; i--) pain_copy(&l->e[i], &l->e[i - 1]);
  pain_copy(&l->e[at], t);
  l->n++;
}

/* Everything on the card before the oldest entry now in memory is not ours
 * to change: remember where that line is. */
static PAIN_OPT void pain_raise_floor(PainLog *l) {
  if (l->n <= 0) return;
  l->has_floor = 1;
  l->floor_day = l->e[0].day;
  l->floor_min = l->e[0].minute;
  l->floor_seq = l->e[0].seq;
}

static PAIN_OPT int pain_below_floor(const PainLog *l, const PainEntry *t) {
  return l->has_floor &&
         pain_key_cmp(t->day, t->minute, t->seq, l->floor_day, l->floor_min, l->floor_seq) < 0;
}

/* Log an entry. `minute` is minutes since midnight or -1 (see pain_stamp).
 * The index it landed at (indices after it moved up one), PAIN_E_ARG for
 * arguments pain_make refuses, PAIN_E_OLD for an entry before the oldest one
 * held when that is not all there is -- the log is full, or older entries
 * are on the card (see "More than fits"). A full log retires its oldest
 * entry to the card to make room. Nothing changes on an error. */
static PAIN_OPT int pain_add(PainLog *l, int32_t day, int minute, int level, int area,
                             const char *note) {
  PainEntry t;
  int at;
  if (pain_make(&t, day, minute, level, area, note) != PAIN_OK) return PAIN_E_ARG;
  t.seq = l->next_seq;
  if (pain_below_floor(l, &t)) return PAIN_E_OLD;
  if (l->n >= PAIN_MAX) {
    if (pain_cmp(&t, &l->e[0]) < 0) return PAIN_E_OLD;
    pain_remove_at(l, 0);
    l->older++;
    pain_raise_floor(l);
  }
  at = pain_upper(l, t.day, t.minute, t.seq);
  pain_insert_at(l, at, &t);
  l->next_seq++;
  return at;
}

/* Replace entry `idx` with the day, minute, level, area and note of `with`
 * (its seq is ignored: the entry keeps its own). Re-sorted if the day or
 * time moved. The index it is at now, PAIN_E_ARG or PAIN_E_OLD as pain_add;
 * unchanged on an error. `with` may be the entry itself. */
static PAIN_OPT int pain_edit(PainLog *l, int idx, const PainEntry *with) {
  PainEntry t;
  int at;
  if (!with || idx < 0 || idx >= l->n) return PAIN_E_ARG;
  if (pain_make(&t, with->day, with->minute, with->level, with->area, with->note) != PAIN_OK)
    return PAIN_E_ARG;
  t.seq = l->e[idx].seq;
  if (pain_below_floor(l, &t)) return PAIN_E_OLD;
  pain_remove_at(l, idx);
  at = pain_upper(l, t.day, t.minute, t.seq);
  pain_insert_at(l, at, &t);
  return at;
}

/* Change only the note of entry `idx`; it does not move. PAIN_OK or PAIN_E_ARG. */
static PAIN_OPT int pain_set_note(PainLog *l, int idx, const char *note) {
  if (idx < 0 || idx >= l->n) return PAIN_E_ARG;
  pain_clean_note(note, l->e[idx].note);
  return PAIN_OK;
}

/* Remove entry `idx` (later ones move down). PAIN_OK or PAIN_E_ARG. */
static PAIN_OPT int pain_delete(PainLog *l, int idx) {
  if (idx < 0 || idx >= l->n) return PAIN_E_ARG;
  pain_remove_at(l, idx);
  return PAIN_OK;
}

/* How many entries are on `day`; *first (if given) is the index of the first,
 * or the count of all entries when there are none. Day PAIN_NO_DAY asks for
 * the undated ones. */
static PAIN_OPT int pain_day_count(const PainLog *l, int32_t day, int *first) {
  int i, n = 0, f = l->n;
  for (i = 0; i < l->n; i++) {
    if (l->e[i].day != day) continue;
    if (n == 0) f = i;
    n++;
  }
  if (first) *first = f;
  return n;
}

/* ---- statistics ------------------------------------------------------------------- */

/* One pass over the sorted array. Entries are in day order, so the entries of
 * a day are together and its worst is the running maximum until the day
 * changes. `with_undated` says whether entries with no day are counted (each
 * as a day of its own); dated ones count when from <= day <= to. */
static PAIN_OPT void pain_stats_calc(const PainLog *l, int32_t from, int32_t to, int with_undated,
                                     PainStats *s) {
  long sum = 0;
  int i, cur_max = -1;
  int32_t cur_day = -1;
  s->n = 0;
  s->avg10 = 0;
  s->worst = -1;
  s->worst_day = PAIN_NO_DAY;
  s->days = 0;
  s->bad_days = 0;
  for (i = 0; i < l->n; i++) {
    const PainEntry *e = &l->e[i];
    if (e->day == PAIN_NO_DAY) {
      if (!with_undated) continue;
    } else if (e->day < from || e->day > to) {
      continue;
    }
    sum += e->level;
    s->n++;
    if ((int)e->level >= s->worst) { s->worst = e->level; s->worst_day = e->day; }
    if (e->day == PAIN_NO_DAY) {
      s->days++;
      if (e->level >= PAIN_BAD) s->bad_days++;
    } else if (e->day != cur_day) {
      if (cur_max >= PAIN_BAD) s->bad_days++;     /* the day before is over */
      cur_day = e->day;
      cur_max = e->level;
      s->days++;
    } else if ((int)e->level > cur_max) {
      cur_max = e->level;
    }
  }
  if (cur_max >= PAIN_BAD) s->bad_days++;         /* and so is the last */
  if (s->n) s->avg10 = (int)((sum * 10 + s->n / 2) / s->n);
}

/* Everything in memory, undated entries included. */
static PAIN_OPT void pain_stats(const PainLog *l, PainStats *s) {
  pain_stats_calc(l, 0, PAIN_DAY_MAX, 1, s);
}

/* Entries dated from `from` to `to`, both days included. Undated entries are
 * not in any range. */
static PAIN_OPT void pain_stats_range(const PainLog *l, int32_t from, int32_t to, PainStats *s) {
  pain_stats_calc(l, from, to, 0, s);
}

/* The worst level on each of `days` days ending on day `last`: out[0] is day
 * last - days + 1 and out[days - 1] is `last`, so a chart reads left to
 * right into the present, and paging back a week is `last` minus 7. -1 where
 * nothing was logged. Undated entries are on no day. */
static PAIN_OPT void pain_daily_max(const PainLog *l, int32_t last, int days, int8_t *out) {
  int32_t first = last - days + 1;
  int i;
  for (i = 0; i < days; i++) out[i] = -1;
  for (i = 0; i < l->n; i++) {
    const PainEntry *e = &l->e[i];
    int k;
    if (e->day == PAIN_NO_DAY || e->day < first || e->day > last) continue;
    k = (int)(e->day - first);
    if ((int)e->level > out[k]) out[k] = (int8_t)e->level;
  }
}

/* The levels of the last `n` entries, oldest first, into out[]: what a chart
 * shows when there are no days to draw. How many were written. */
static PAIN_OPT int pain_recent(const PainLog *l, int n, int8_t *out) {
  int i, k = 0;
  for (i = l->n > n ? l->n - n : 0; i < l->n; i++) out[k++] = (int8_t)l->e[i].level;
  return k;
}

/* ---- the line ----------------------------------------------------------------------- */

/* `v` into out[pos..] as exactly `width` digits, zero-padded. The new pos. */
static PAIN_OPT int pain_put(char *out, int pos, uint32_t v, int width) {
  int i;
  for (i = width - 1; i >= 0; i--) {
    out[pos + i] = (char)('0' + v % 10);
    v /= 10;
  }
  return pos + width;
}

/* `v` into out[pos..] in as many digits as it takes. The new pos. */
static PAIN_OPT int pain_put_dec(char *out, int pos, uint32_t v) {
  char t[10];
  int n = 0, i;
  do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  for (i = n - 1; i >= 0; i--) out[pos++] = t[i];
  return pos;
}

/* The entry as a file line, without a newline, into `out` (PAIN_LINE_MAX).
 * Its length. */
static PAIN_OPT int pain_format(const PainEntry *e, char *out) {
  int p = 0, y, m, d, i;
  const char *area = pain_area_name(e->area);
  if (e->day == PAIN_NO_DAY) {
    out[p++] = '-';
  } else {
    dt_civil_from_days(e->day, &y, &m, &d);
    p = pain_put(out, p, (uint32_t)y, 4);
    out[p++] = '-';
    p = pain_put(out, p, (uint32_t)m, 2);
    out[p++] = '-';
    p = pain_put(out, p, (uint32_t)d, 2);
  }
  out[p++] = '\t';
  if (e->minute == PAIN_NO_TIME) {
    out[p++] = '-';
  } else {
    p = pain_put(out, p, (uint32_t)(e->minute / 60), 2);
    out[p++] = ':';
    p = pain_put(out, p, (uint32_t)(e->minute % 60), 2);
  }
  out[p++] = '\t';
  p = pain_put_dec(out, p, e->level);
  out[p++] = '\t';
  for (i = 0; area[i]; i++) out[p++] = area[i];
  out[p++] = '\t';
  p = pain_put_dec(out, p, e->seq);
  out[p++] = '\t';
  for (i = 0; i < PAIN_NOTE_MAX && e->note[i]; i++) out[p++] = e->note[i];
  out[p] = 0;
  return p;
}

/* Exactly `n` digits at s: their value, or -1 if any is not a digit (a NUL
 * is not one, so this never reads past the end of a string). */
static PAIN_OPT int pain_num(const char *s, int n) {
  int v = 0, i;
  for (i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  return v;
}

/* All of s is 1 to 9 digits: their value, or -1. */
static PAIN_OPT int32_t pain_dec(const char *s) {
  int32_t v = 0;
  int n = 0;
  while (s[n] >= '0' && s[n] <= '9') {
    if (n >= 9) return -1;
    v = v * 10 + (s[n] - '0');
    n++;
  }
  return (n > 0 && !s[n]) ? v : -1;
}

/* "2026-10-21", a real date after 1970-01-01, or "-". 1 if it was one. */
static PAIN_OPT int pain_parse_day(const char *s, int32_t *day) {
  int y, m, d;
  int32_t z;
  if (s[0] == '-' && !s[1]) { *day = PAIN_NO_DAY; return 1; }
  y = pain_num(s, 4);
  if (y < 1970 || s[4] != '-') return 0;
  m = pain_num(s + 5, 2);
  if (m < 1 || m > 12 || s[7] != '-') return 0;
  d = pain_num(s + 8, 2);
  if (d < 1 || d > dt_days_in_month(y, m) || s[10]) return 0;
  z = dt_days_from_civil(y, m, d);
  if (z <= 0) return 0;
  *day = z;
  return 1;
}

/* "14:05" or "9:05", or "-". 1 if it was one. */
static PAIN_OPT int pain_parse_time(const char *s, uint16_t *minute) {
  int h, m, hl;
  if (s[0] == '-' && !s[1]) { *minute = PAIN_NO_TIME; return 1; }
  if (s[0] < '0' || s[0] > '9') return 0;
  hl = (s[1] == ':') ? 1 : 2;
  h = pain_num(s, hl);
  if (h < 0 || h > 23 || s[hl] != ':') return 0;
  m = pain_num(s + hl + 1, 2);
  if (m < 0 || m > 59 || s[hl + 3]) return 0;
  *minute = (uint16_t)(h * 60 + m);
  return 1;
}

/* A file line into `e`. 1, or 0 if its date, time, level or seq cannot be
 * read. The area and note are never a reason to refuse it. */
static PAIN_OPT int pain_parse(const char *line, PainEntry *e) {
  char f[16], raw[64];
  int32_t day = PAIN_NO_DAY, v;
  uint16_t minute = PAIN_NO_TIME;
  tsv_field(line, 0, f, sizeof f);
  if (!pain_parse_day(f, &day)) return 0;
  tsv_field(line, 1, f, sizeof f);
  if (!pain_parse_time(f, &minute)) return 0;
  tsv_field(line, 2, f, sizeof f);
  v = pain_dec(f);
  if (v < 0 || v > PAIN_LEVEL_MAX) return 0;
  e->level = (uint8_t)v;
  tsv_field(line, 3, f, sizeof f);
  e->area = (uint8_t)pain_area_from_name(f);
  tsv_field(line, 4, f, sizeof f);
  if (f[0]) {
    v = pain_dec(f);
    if (v < 0) return 0;
    e->seq = (uint32_t)v;
  } else {
    e->seq = 0;
  }
  tsv_field(line, 5, raw, sizeof raw);
  pain_clean_note(raw, e->note);
  e->day = day;
  e->minute = (day == PAIN_NO_DAY) ? PAIN_NO_TIME : minute;
  return 1;
}

/* ---- the card ---------------------------------------------------------------------- */

typedef void (*PainLineFn)(const char *line, void *ctx);

/* The lines of an open file, without their line ends, to fn. A line longer
 * than PAIN_LINE_IN - 1 loses its tail; a last line with no newline counts. */
static PAIN_OPT void pain_each_line(const CardApi *api, int fd, PainLineFn fn, void *ctx) {
  char buf[128], line[PAIN_LINE_IN];
  int n, i, len = 0;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      if (buf[i] == '\r') continue;
      if (buf[i] != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = buf[i];
        continue;
      }
      line[len] = 0;
      len = 0;
      fn(line, ctx);
    }
  }
  if (len) {
    line[len] = 0;
    fn(line, ctx);
  }
}

/* One line of the file into the log. Past PAIN_MAX the oldest of what is held
 * and the line goes: counted in `older`, left on the card by the floor. */
static PAIN_OPT void pain_take_line(const char *line, void *ctx) {
  PainLog *l = (PainLog *)ctx;
  PainEntry t;
  if (!line[0]) return;
  if (!pain_parse(line, &t)) { l->skipped++; return; }
  if (t.seq >= l->next_seq) l->next_seq = t.seq + 1;
  if (l->n >= PAIN_MAX) {
    l->older++;
    if (pain_cmp(&t, &l->e[0]) < 0) return;       /* older than all we hold */
    pain_remove_at(l, 0);
  }
  pain_insert_at(l, pain_upper(l, t.day, t.minute, t.seq), &t);
}

/* Read the log from PAIN_PATH, putting back a copy a power cut stranded.
 * Starts from empty; a card with no file is an empty log. The entries held. */
static PAIN_OPT int pain_load(PainLog *l, const CardApi *api) {
  int fd;
  pain_init(l);
  fd = safe_open_read(api, PAIN_PATH);
  if (fd < 0) return 0;
  pain_each_line(api, fd, pain_take_line, l);
  api->close(fd);
  if (l->older > 0) pain_raise_floor(l);
  return l->n;
}

typedef struct {
  SafeFile       *f;
  const PainLog  *l;
} PainCarry;

/* A line of the old file that is older than everything in memory goes into
 * the new one as it is. */
static PAIN_OPT void pain_carry_line(const char *line, void *ctx) {
  PainCarry *c = (PainCarry *)ctx;
  PainEntry t;
  char s[PAIN_LINE_MAX];
  int n;
  if (!pain_parse(line, &t)) return;
  if (pain_key_cmp(t.day, t.minute, t.seq, c->l->floor_day, c->l->floor_min, c->l->floor_seq) >= 0)
    return;
  n = pain_format(&t, s);
  s[n++] = '\n';
  safe_write(c->f, s, (size_t)n);
}

/* Write the log to PAIN_PATH, whole, through a temp file: 0, or -1 with the
 * file on the card as it was (no card, or it filled). With a floor, the lines
 * older than it are copied across first. Call it after every change. */
static PAIN_OPT int pain_save(const PainLog *l, const CardApi *api) {
  SafeFile f;
  PainCarry c;
  char s[PAIN_LINE_MAX];
  int fd = -1, i, n;
  api->mkdir(CAPP_VAR);
  api->mkdir(PAIN_DIR);
  /* The old file is opened before the temp is made: safe_open_read would take
   * a temp it found for a file that was missing. */
  if (l->has_floor) fd = safe_open_read(api, PAIN_PATH);
  if (safe_begin(&f, api, PAIN_PATH) != 0) {
    if (fd >= 0) api->close(fd);
    return -1;
  }
  if (fd >= 0) {
    c.f = &f;
    c.l = l;
    pain_each_line(api, fd, pain_carry_line, &c);
    api->close(fd);
  }
  for (i = 0; i < l->n; i++) {
    n = pain_format(&l->e[i], s);
    s[n++] = '\n';
    safe_write(&f, s, (size_t)n);
  }
  return safe_commit(&f);
}

#endif /* CARDOS_PAIN_LOG_H */
