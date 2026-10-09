/* Dates and durations, for apps that have no libc and so no mktime,
 * gmtime or strftime.
 *
 * Howard Hinnant's civil-date algorithms, which are the short exact way to
 * do this in integers: a date to a day count from 1970-01-01 and back.
 * Calendar, Habits and Toggl each carried a transcription of them, and
 * Memo, Music and Toggl their own m:ss. These are those, once; the
 * transcription is hammered in test/test_calendar.c (two centuries of
 * round trips, leap days, century non-leaps) and test/test_datetime.c.
 *
 * Header-only, static helpers, the same arrangement as apps/safefile.h.
 */
#ifndef CARDOS_DATETIME_H
#define CARDOS_DATETIME_H

#include "kernel/app/capp.h"

#if defined(__GNUC__)
#define DT_OPT __attribute__((unused))
#else
#define DT_OPT
#endif

#define DT_DAY_SECS 86400

static DT_OPT int dt_is_leap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* Days in a month; 30 for a month that is not one, rather than nonsense. */
static DT_OPT int dt_days_in_month(int y, int m) {
  static const int LEN[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m == 2 && dt_is_leap(y)) return 29;
  if (m < 1 || m > 12) return 30;
  return LEN[m];
}

/* A date to days since 1970-01-01 (which is day 0). */
static DT_OPT int32_t dt_days_from_civil(int y, int m, int d) {
  int era;
  unsigned yoe, doy, doe;
  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int32_t)era * 146097 + (int32_t)doe - 719468;
}

/* Days since 1970-01-01 to a date. */
static DT_OPT void dt_civil_from_days(int32_t z, int *yy, int *mm, int *dd) {
  int era, y;
  unsigned doe, yoe, doy, mp, d, m;
  z += 719468;
  era = (int)((z >= 0 ? z : z - 146096) / 146097);
  doe = (unsigned)(z - (int32_t)era * 146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : (unsigned)-9);
  *yy = y + (int)(m <= 2);
  *mm = (int)m;
  *dd = (int)d;
}

/* 0 = Sunday. Day 0 was a Thursday, hence the 4. */
static DT_OPT int dt_weekday(int32_t z) {
  int32_t w = (z + 4) % 7;
  return (int)(w < 0 ? w + 7 : w);
}

/* Local time minus UTC, in seconds: api->now is local and api->epoch is
 * not, so the difference is the zone offset without a zone database. 1 and
 * the offset when the clock is known; 0 and an offset of 0 when it is not. */
static DT_OPT int dt_utc_offset(const CardApi *api, int32_t *off) {
  CappTime t;
  uint32_t utc = api->epoch();
  int32_t local;
  api->now(&t);
  *off = 0;
  if (!t.synced || !utc) return 0;
  local = dt_days_from_civil(t.year, t.month, t.day) * (int32_t)DT_DAY_SECS
        + (int32_t)t.hour * 3600 + t.min * 60 + t.sec;
  *off = local - (int32_t)utc;
  return 1;
}

/* m:ss of a length in milliseconds: 3:07. */
static DT_OPT void dt_mmss(const CardApi *api, uint32_t ms, char *out, size_t n) {
  uint32_t s = ms / 1000;
  api->fmt(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* h:mm:ss and h:mm of a length in seconds: 1:02:07, 1:02. */
static DT_OPT void dt_hms(const CardApi *api, uint32_t secs, char *out, size_t n) {
  api->fmt(out, n, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600),
           (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
}

static DT_OPT void dt_hm(const CardApi *api, uint32_t secs, char *out, size_t n) {
  api->fmt(out, n, "%lu:%02lu", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60));
}

#endif /* CARDOS_DATETIME_H */
