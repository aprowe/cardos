/* The calendar's date arithmetic, on the host.
 *
 * Everything visible in this app rests on two functions transcribed from
 * Howard Hinnant's civil-date algorithms, and a transcription error in either
 * is the kind of bug that looks fine all month and puts a meeting on the
 * wrong day in March. There is no debugger on the device, so they are checked
 * here: round-tripped across two centuries, and pinned against dates worked
 * out by hand at the awkward places -- leap days, century non-leaps, the ends
 * of years, and the epoch itself.
 *
 * The RFC 3339 reader is checked the same way, against the two shapes Google
 * actually emits and against the malformed input a truncated reply produces.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

/* The app itself, statics and all: its internals are the thing under test.
 *
 * Every app defines capp_info and capp_main with those exact names -- the
 * loader requires it -- so two apps pulled into one test binary collide at
 * the link. Renamed here rather than in the app, because the app's symbols
 * are its contract with the loader and must not be touched to suit a test. */
#define capp_info calendar_capp_info
#define capp_main calendar_capp_main
#include "apps/calendar.c"
#undef capp_info
#undef capp_main

/* ---- a CardApi that does just enough ------------------------------------- */

static int fake_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

static void *fake_memset(void *d, int c, size_t n)         { return memset(d, c, n); }
static void *fake_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t fake_strlen(const char *s)                   { return strlen(s); }

static CardApi FAKE;

static void use_fake_api(void) {
  memset(&FAKE, 0, sizeof FAKE);
  FAKE.fmt = fake_fmt;
  FAKE.mem_set = fake_memset;
  FAKE.mem_cpy = fake_memcpy;
  FAKE.str_len = fake_strlen;
  api = &FAKE;
  memset(&C, 0, sizeof C);
}

/* ---- civil dates ---------------------------------------------------------- */

void test_the_epoch_is_the_first_of_january_1970(void) {
  int y, m, d;
  use_fake_api();
  CHECK_EQ(0, days_from_civil(1970, 1, 1));
  civil_from_days(0, &y, &m, &d);
  CHECK_EQ(1970, y);
  CHECK_EQ(1, m);
  CHECK_EQ(1, d);
  /* 1970-01-01 was a Thursday. Everything else keys off this. */
  CHECK_EQ(4, weekday_of_day(0));
}

void test_dates_worked_out_by_hand(void) {
  use_fake_api();
  CHECK_EQ(-1, days_from_civil(1969, 12, 31));
  CHECK_EQ(365, days_from_civil(1971, 1, 1));
  /* 1970 and 1971 are 365 days each, so 1972 opens on day 730. 1972 is then
   * a leap year, which is why 1973 is 366 further on and not 365. */
  CHECK_EQ(730, days_from_civil(1972, 1, 1));
  CHECK_EQ(1096, days_from_civil(1973, 1, 1));
  /* 2000-01-01, the usual reference point: 10957 days after the epoch. */
  CHECK_EQ(10957, days_from_civil(2000, 1, 1));
  /* 2026-09-13, a date this app was written on. */
  CHECK_EQ(20709, days_from_civil(2026, 9, 13));
}

void test_the_weekday_of_some_known_days(void) {
  use_fake_api();
  /* 2000-01-01 was a Saturday, 2026-09-13 a Sunday. */
  CHECK_EQ(6, weekday_of_day(days_from_civil(2000, 1, 1)));
  CHECK_EQ(0, weekday_of_day(days_from_civil(2026, 9, 13)));
  CHECK_EQ(1, weekday_of_day(days_from_civil(2026, 9, 14)));
}

void test_every_day_of_two_centuries_round_trips(void) {
  int32_t z;
  int bad = 0;
  use_fake_api();
  /* 1900-01-01 to 2100-01-01, which covers both kinds of century year. */
  for (z = days_from_civil(1900, 1, 1); z < days_from_civil(2100, 1, 1); z++) {
    int y, m, d;
    civil_from_days(z, &y, &m, &d);
    if (days_from_civil(y, m, d) != z) bad++;
    if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) bad++;
  }
  CHECK_EQ(0, bad);
}

void test_the_weekday_advances_by_one_every_day(void) {
  int32_t z;
  int bad = 0;
  use_fake_api();
  for (z = days_from_civil(1999, 1, 1); z < days_from_civil(2031, 1, 1); z++)
    if (weekday_of_day(z + 1) != (weekday_of_day(z) + 1) % 7) bad++;
  CHECK_EQ(0, bad);
}

void test_leap_years_and_month_lengths(void) {
  use_fake_api();
  CHECK_EQ(1, is_leap(2024));
  CHECK_EQ(0, is_leap(2025));
  CHECK_EQ(1, is_leap(2000));   /* divisible by 400 */
  CHECK_EQ(0, is_leap(1900));   /* divisible by 100, not 400 */
  CHECK_EQ(29, days_in_month(2024, 2));
  CHECK_EQ(28, days_in_month(2025, 2));
  CHECK_EQ(29, days_in_month(2000, 2));
  CHECK_EQ(28, days_in_month(1900, 2));
  CHECK_EQ(31, days_in_month(2026, 1));
  CHECK_EQ(30, days_in_month(2026, 4));
}

void test_the_day_after_a_leap_day_is_the_first_of_march(void) {
  int y, m, d;
  use_fake_api();
  civil_from_days(days_from_civil(2024, 2, 29) + 1, &y, &m, &d);
  CHECK_EQ(2024, y);
  CHECK_EQ(3, m);
  CHECK_EQ(1, d);
  /* And in a non-leap year February simply stops at the 28th. */
  civil_from_days(days_from_civil(2025, 2, 28) + 1, &y, &m, &d);
  CHECK_EQ(3, m);
  CHECK_EQ(1, d);
}

void test_the_day_after_new_years_eve_is_new_years_day(void) {
  int y, m, d;
  use_fake_api();
  civil_from_days(days_from_civil(2026, 12, 31) + 1, &y, &m, &d);
  CHECK_EQ(2027, y);
  CHECK_EQ(1, m);
  CHECK_EQ(1, d);
}

/* ---- RFC 3339 ------------------------------------------------------------- */

void test_a_timed_event_in_utc(void) {
  uint32_t t = 0;
  int all_day = 9;
  use_fake_api();
  CHECK_EQ(0, rfc3339_parse("2026-09-13T09:30:00Z", &t, &all_day));
  CHECK_EQ(0, all_day);
  CHECK_EQ((int)(20709u * 86400u + 9 * 3600 + 30 * 60), (int)t);
}

void test_a_zone_offset_is_subtracted_to_reach_utc(void) {
  uint32_t utc = 0, bst = 0;
  use_fake_api();
  /* 09:30 in London in September is 08:30 UTC. */
  CHECK_EQ(0, rfc3339_parse("2026-09-13T08:30:00Z", &utc, 0));
  CHECK_EQ(0, rfc3339_parse("2026-09-13T09:30:00+01:00", &bst, 0));
  CHECK_EQ((int)utc, (int)bst);
}

void test_a_negative_zone_offset_is_added(void) {
  uint32_t utc = 0, edt = 0;
  use_fake_api();
  /* 09:30 in New York in September is 13:30 UTC. */
  CHECK_EQ(0, rfc3339_parse("2026-09-13T13:30:00Z", &utc, 0));
  CHECK_EQ(0, rfc3339_parse("2026-09-13T09:30:00-04:00", &edt, 0));
  CHECK_EQ((int)utc, (int)edt);
}

void test_an_all_day_event_is_a_bare_date(void) {
  uint32_t t = 0;
  int all_day = 0;
  use_fake_api();
  CHECK_EQ(0, rfc3339_parse("2026-09-13", &t, &all_day));
  CHECK_EQ(1, all_day);
  CHECK_EQ((int)(20709u * 86400u), (int)t);
}

void test_malformed_timestamps_are_refused(void) {
  uint32_t t = 0;
  use_fake_api();
  /* What a reply truncated by the buffer limit actually looks like. */
  CHECK(rfc3339_parse("2026-09-1", &t, 0) != 0);
  CHECK(rfc3339_parse("", &t, 0) != 0);
  CHECK(rfc3339_parse("not-a-date", &t, 0) != 0);
  CHECK(rfc3339_parse("2026/09/13", &t, 0) != 0);
  CHECK(rfc3339_parse("2026-13-01T00:00:00Z", &t, 0) != 0);  /* month 13 */
  CHECK(rfc3339_parse("2026-09-13T9:30:00Z", &t, 0) != 0);   /* unpadded hour */
}

void test_formatting_and_parsing_are_inverses(void) {
  int i, bad = 0;
  use_fake_api();
  for (i = 0; i < 400; i++) {
    /* Every 37 hours from the start of 2026, which walks days, months and a
     * year boundary without landing on the same time of day twice. */
    uint32_t t = (uint32_t)(days_from_civil(2026, 1, 1) * 86400) + (uint32_t)i * 133200u;
    uint32_t back = 0;
    char buf[32];
    rfc3339_utc(t, buf, sizeof buf);
    if (rfc3339_parse(buf, &back, 0) != 0 || back != t) bad++;
  }
  CHECK_EQ(0, bad);
}

/* ---- reading a reply ------------------------------------------------------- */

/* Exactly the shape `fields=items(id,summary,start,end)` returns. */
static const char REPLY[] =
  "{\"items\":["
  "{\"id\":\"aaa111\",\"summary\":\"Standup\","
  "\"start\":{\"dateTime\":\"2026-09-13T09:30:00+01:00\"},"
  "\"end\":{\"dateTime\":\"2026-09-13T09:45:00+01:00\"}},"
  "{\"id\":\"bbb222\",\"summary\":\"Dentist\","
  "\"start\":{\"dateTime\":\"2026-09-13T14:00:00+01:00\"},"
  "\"end\":{\"dateTime\":\"2026-09-13T15:00:00+01:00\"}},"
  "{\"id\":\"ccc333\",\"summary\":\"Birthday\","
  "\"start\":{\"date\":\"2026-09-14\"},"
  "\"end\":{\"date\":\"2026-09-15\"}}"
  "]}";

/* The parse loop out of fetch(), with the network taken out of it. */
static void parse_reply(const char *json) {
  char *p;
  int got = 0;
  snprintf(C.reply, sizeof C.reply, "%s", json);
  C.n = 0;
  p = find_pat(C.reply, "\"id\"");
  while (p && C.n < MAX_EVENTS) {
    char *next = find_pat(p + 4, "\"id\"");
    char saved = 0;
    Event *e = &C.ev[C.n];
    char *sp, *ep;
    if (next) { saved = *next; *next = 0; }
    memset(e, 0, sizeof *e);
    json_str(p, "id", e->id, ID_MAX);
    if (!json_str(p, "summary", e->summary, SUMMARY_MAX + 1))
      snprintf(e->summary, sizeof e->summary, "(no title)");
    sp = find_pat(p, "\"start\"");
    ep = find_pat(p, "\"end\"");
    if (sp) {
      char keep2 = 0;
      if (ep && ep > sp) { keep2 = *ep; *ep = 0; }
      if (json_when(sp, &e->start, &got)) e->all_day = (uint8_t)got;
      if (ep && ep > sp) *ep = keep2;
    }
    if (ep) json_when(ep, &e->end, &got);
    if (!e->end) e->end = e->start + 3600u;
    if (e->id[0] && e->start) C.n++;
    if (next) *next = saved;
    p = next;
  }
  sort_events();
}

void test_a_reply_yields_one_event_per_item(void) {
  use_fake_api();
  parse_reply(REPLY);
  CHECK_EQ(3, C.n);
  CHECK(strcmp(C.ev[0].id, "aaa111") == 0);
  CHECK(strcmp(C.ev[0].summary, "Standup") == 0);
  CHECK(strcmp(C.ev[1].summary, "Dentist") == 0);
  CHECK(strcmp(C.ev[2].summary, "Birthday") == 0);
}

void test_each_event_keeps_its_own_start_and_end(void) {
  use_fake_api();
  parse_reply(REPLY);
  /* 09:30+01:00 is 08:30 UTC, and the meeting is a quarter of an hour. */
  CHECK_EQ((int)(20709u * 86400u + 8 * 3600 + 30 * 60), (int)C.ev[0].start);
  CHECK_EQ(15 * 60, (int)(C.ev[0].end - C.ev[0].start));
  CHECK_EQ(60 * 60, (int)(C.ev[1].end - C.ev[1].start));
}

void test_an_all_day_item_is_recognised_by_its_shape(void) {
  use_fake_api();
  parse_reply(REPLY);
  CHECK_EQ(0, C.ev[0].all_day);
  CHECK_EQ(1, C.ev[2].all_day);
  /* "date" must not be matched by the front of "dateTime". */
  CHECK_EQ((int)(20710u * 86400u), (int)C.ev[2].start);
}

void test_an_item_with_no_summary_still_appears(void) {
  use_fake_api();
  parse_reply("{\"items\":[{\"id\":\"zzz\","
              "\"start\":{\"dateTime\":\"2026-09-13T09:00:00Z\"},"
              "\"end\":{\"dateTime\":\"2026-09-13T10:00:00Z\"}}]}");
  CHECK_EQ(1, C.n);
  CHECK(strcmp(C.ev[0].summary, "(no title)") == 0);
}

void test_a_truncated_reply_does_not_run_off_the_end(void) {
  use_fake_api();
  /* The buffer filled mid-event, which is what a 40-event day would do. */
  parse_reply("{\"items\":[{\"id\":\"aaa\",\"summary\":\"Fine\","
              "\"start\":{\"dateTime\":\"2026-09-13T09:00:00Z\"},"
              "\"end\":{\"dateTime\":\"2026-09-13T10:00:00Z\"}},"
              "{\"id\":\"bbb\",\"summ");
  /* The whole event survives; the fragment is dropped for want of a start. */
  CHECK_EQ(1, C.n);
  CHECK(strcmp(C.ev[0].summary, "Fine") == 0);
}

void test_events_come_out_in_time_order(void) {
  use_fake_api();
  /* Deliberately out of order, which orderBy=startTime should prevent and a
   * queued local event will cause anyway. */
  parse_reply("{\"items\":["
              "{\"id\":\"b\",\"summary\":\"Later\","
              "\"start\":{\"dateTime\":\"2026-09-13T15:00:00Z\"},"
              "\"end\":{\"dateTime\":\"2026-09-13T16:00:00Z\"}},"
              "{\"id\":\"a\",\"summary\":\"Earlier\","
              "\"start\":{\"dateTime\":\"2026-09-13T09:00:00Z\"},"
              "\"end\":{\"dateTime\":\"2026-09-13T10:00:00Z\"}}]}");
  CHECK_EQ(2, C.n);
  CHECK(strcmp(C.ev[0].summary, "Earlier") == 0);
  CHECK(strcmp(C.ev[1].summary, "Later") == 0);
}

/* ---- local time ------------------------------------------------------------ */

void test_the_local_day_follows_the_zone_offset(void) {
  uint32_t t;
  use_fake_api();
  /* 23:30 UTC on the 13th is 00:30 on the 14th in Berlin. */
  t = (uint32_t)(20709 * 86400 + 23 * 3600 + 30 * 60);
  C.offset = 0;
  CHECK_EQ(20709, (int)local_day(t));
  C.offset = 3600;
  CHECK_EQ(20710, (int)local_day(t));
  /* And 00:30 UTC on the 14th is still the 13th in New York. */
  t = (uint32_t)(20710 * 86400 + 30 * 60);
  C.offset = -4 * 3600;
  CHECK_EQ(20709, (int)local_day(t));
}

void test_the_clock_face_is_local_not_utc(void) {
  uint32_t t = (uint32_t)(20709 * 86400 + 8 * 3600 + 30 * 60);
  int h = 0, m = 0;
  use_fake_api();
  C.offset = 0;
  local_hm(t, &h, &m);
  CHECK_EQ(8, h);
  CHECK_EQ(30, m);
  C.offset = 3600;
  local_hm(t, &h, &m);
  CHECK_EQ(9, h);
  CHECK_EQ(30, m);
}

void test_a_day_label_names_today_and_tomorrow(void) {
  char buf[24];
  use_fake_api();
  C.have_clock = 1;
  C.today_y = 2026; C.today_m = 9; C.today_d = 13;
  day_label(days_from_civil(2026, 9, 13), buf, sizeof buf);
  CHECK(strcmp(buf, "Today") == 0);
  day_label(days_from_civil(2026, 9, 14), buf, sizeof buf);
  CHECK(strcmp(buf, "Tomorrow") == 0);
  day_label(days_from_civil(2026, 9, 15), buf, sizeof buf);
  CHECK(strcmp(buf, "Tue 15 Sep") == 0);
  /* Yesterday is a date, not "Yesterday" -- the past is not a special case. */
  day_label(days_from_civil(2026, 9, 12), buf, sizeof buf);
  CHECK(strcmp(buf, "Sat 12 Sep") == 0);
}

void test_without_a_clock_nothing_claims_to_be_today(void) {
  char buf[24];
  use_fake_api();
  C.have_clock = 0;
  day_label(days_from_civil(2026, 9, 13), buf, sizeof buf);
  CHECK(strcmp(buf, "Sun 13 Sep") == 0);
}

/* ---- the month grid --------------------------------------------------------- */

void test_stepping_a_day_crosses_months_and_years(void) {
  use_fake_api();
  C.cur_y = 2026; C.cur_m = 9; C.cur_d = 30;
  month_step(1);
  CHECK_EQ(10, C.cur_m);
  CHECK_EQ(1, C.cur_d);
  C.cur_y = 2026; C.cur_m = 12; C.cur_d = 31;
  month_step(1);
  CHECK_EQ(2027, C.cur_y);
  CHECK_EQ(1, C.cur_m);
  CHECK_EQ(1, C.cur_d);
  /* And backwards over a leap day. */
  C.cur_y = 2024; C.cur_m = 3; C.cur_d = 1;
  month_step(-1);
  CHECK_EQ(2, C.cur_m);
  CHECK_EQ(29, C.cur_d);
}

void test_a_week_step_lands_on_the_same_weekday(void) {
  int i, bad = 0;
  use_fake_api();
  C.cur_y = 2026; C.cur_m = 1; C.cur_d = 1;
  for (i = 0; i < 60; i++) {
    int before = weekday_of_day(days_from_civil(C.cur_y, C.cur_m, C.cur_d));
    month_step(7);
    if (weekday_of_day(days_from_civil(C.cur_y, C.cur_m, C.cur_d)) != before) bad++;
  }
  CHECK_EQ(0, bad);
}

void test_the_grid_starts_the_month_on_the_right_column(void) {
  use_fake_api();
  /* 1 September 2026 was a Tuesday, so it sits in column 2 counting from
   * Sunday -- which is what the grid's `first` is. */
  CHECK_EQ(2, weekday_of_day(days_from_civil(2026, 9, 1)));
  /* 1 February 2026 was a Sunday: column 0, the first cell. */
  CHECK_EQ(0, weekday_of_day(days_from_civil(2026, 2, 1)));
}

void test_every_month_of_a_decade_fits_in_six_rows(void) {
  int y, m, bad = 0;
  use_fake_api();
  /* The grid draws six rows. A month starting late enough in the week could
   * in principle need a seventh, and the paint loop drops those days -- so
   * check that no month in a decade actually does. */
  for (y = 2020; y < 2040; y++) {
    for (m = 1; m <= 12; m++) {
      int first = weekday_of_day(days_from_civil(y, m, 1));
      if (first + days_in_month(y, m) - 1 > 41) bad++;
    }
  }
  CHECK_EQ(0, bad);
}

void test_a_day_with_an_event_is_marked(void) {
  use_fake_api();
  C.offset = 0;
  parse_reply(REPLY);
  CHECK_EQ(1, day_has_event(days_from_civil(2026, 9, 13)));
  CHECK_EQ(1, day_has_event(days_from_civil(2026, 9, 14)));
  CHECK_EQ(0, day_has_event(days_from_civil(2026, 9, 15)));
  /* A deleted event stops marking its day. */
  C.ev[2].deleted = 1;
  CHECK_EQ(0, day_has_event(days_from_civil(2026, 9, 14)));
}

/* ---- the cache line --------------------------------------------------------- */

void test_a_cache_number_scans_back_out(void) {
  int pos = 0;
  use_fake_api();
  CHECK_EQ(1789567800, (int)scan_ul(" 1789567800 x", &pos));
  /* Stops on the space, ready for the next field. */
  CHECK_EQ(11, pos);
}
