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
#include "fakeapi.h"

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


static CardApi FAKE;

/* The day view marks the rows that changed instead of the window, so a fake
 * that only formats strings is no longer enough to drive the keys. */
static void fake_damage(CRect r) { (void)r; }

/* No card: a parse test's absorb() saves its cache, and there is nowhere to. */
static int fake_no_card(const char *p, int f) { (void)p; (void)f; return -1; }
static const char *fake_proxy(void) { return "http://server:8080"; }

/* What the app asked the OS to remind about (API 40). */
static int  NSCHED, NCANCEL_ALL;
static uint32_t SCHED_SECS[32];
static char SCHED_TITLE[32][40], SCHED_TEXT[32][64];
static int fake_notify_at(uint32_t secs, const char *key, const char *title,
                          const char *text, int ring) {
  (void)key; (void)ring;
  if (NSCHED < 32) {
    SCHED_SECS[NSCHED] = secs;
    snprintf(SCHED_TITLE[NSCHED], sizeof SCHED_TITLE[0], "%s", title);
    snprintf(SCHED_TEXT[NSCHED], sizeof SCHED_TEXT[0], "%s", text);
    NSCHED++;
  }
  return 0;
}
static void fake_notify_cancel(const char *key) { if (!strcmp(key, "*")) NCANCEL_ALL++; NSCHED = 0; }
static uint32_t fake_no_epoch(void) { return 0; }

static void use_fake_api(void) {
  fakeapi_init(&FAKE);
  FAKE.notify_at = fake_notify_at;
  FAKE.notify_cancel = fake_notify_cancel;
  FAKE.epoch = fake_no_epoch;
  NSCHED = NCANCEL_ALL = 0;
  FAKE.open = fake_no_card;
  FAKE.proxy = fake_proxy;
  FAKE.damage = fake_damage;
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

/* What the server sends (server/google.py): an event a line, id, start, end
 * and summary, start and end as Google wrote them. */
static const char REPLY[] =
  "aaa111\t2026-09-13T09:30:00+01:00\t2026-09-13T09:45:00+01:00\tStandup\n"
  "bbb222\t2026-09-13T14:00:00+01:00\t2026-09-13T15:00:00+01:00\tDentist\n"
  "ccc333\t2026-09-14\t2026-09-15\tBirthday\n";

/* The app's own absorb, over a reply put where the network would. */
static void parse_reply(const char *lines) {
  snprintf(C.reply, sizeof C.reply, "%s", lines);
  C.n = 0;
  absorb();
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
  /* A bare date is an all-day event, starting at its midnight UTC. */
  CHECK_EQ((int)(20710u * 86400u), (int)C.ev[2].start);
}

void test_an_item_with_no_summary_still_appears(void) {
  use_fake_api();
  parse_reply("zzz\t2026-09-13T09:00:00Z\t2026-09-13T10:00:00Z\t\n");
  CHECK_EQ(1, C.n);
  CHECK(strcmp(C.ev[0].summary, "(no title)") == 0);
}

void test_a_truncated_reply_does_not_run_off_the_end(void) {
  use_fake_api();
  /* The buffer filled mid-event, which is what a 40-event day would do. */
  parse_reply("aaa\t2026-09-13T09:00:00Z\t2026-09-13T10:00:00Z\tFine\n"
              "bbb\t2026-09-13T1");
  /* The whole event survives; the fragment is dropped for want of a start. */
  CHECK_EQ(1, C.n);
  CHECK(strcmp(C.ev[0].summary, "Fine") == 0);
}

void test_events_come_out_in_time_order(void) {
  use_fake_api();
  /* Deliberately out of order, which orderBy=startTime should prevent and a
   * queued local event will cause anyway. */
  parse_reply("b\t2026-09-13T15:00:00Z\t2026-09-13T16:00:00Z\tLater\n"
              "a\t2026-09-13T09:00:00Z\t2026-09-13T10:00:00Z\tEarlier\n");
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

/* ---- a start the queue refused ------------------------------------------ */

/* The device runs one request at a time, so http_start can say no. The app
 * used to return without a word and try again in ten minutes, which on the
 * screen looks exactly like a sync that never works. */
static uint32_t    fake_ticks(void)        { return 1000; }
static int         fake_net_ready(void)    { return 1; }
static const char *fake_token(void)        { return "ya29.token"; }
static int fake_start_refused(const char *m, const char *u, const char *b,
                              const char *ct, const char *tok, int ms) {
  (void)m; (void)u; (void)b; (void)ct; (void)tok; (void)ms;
  return -1;
}

void test_calendar_a_refused_start_says_so_and_retries_soon(void) {
  use_fake_api();
  FAKE.ticks_ms = fake_ticks;
  FAKE.net_ready = fake_net_ready;
  FAKE.google_token = fake_token;
  FAKE.http_start = fake_start_refused;
  C.have_clock = 1;             /* a sync without one never reaches http_start */
  snprintf(C.status, sizeof C.status, "%s", "17 events");

  sync_begin("s");

  CHECK_EQ(C.stage, SYNC_IDLE);
  CHECK(strstr(C.status, "busy") != NULL);
  CHECK_EQ(C.next_auto, 1000 + RETRY_MS);
}

/* ---- the day view --------------------------------------------------------
 *
 * The agenda draws nothing at all for a day with nothing on it, which is the
 * right answer to "what is next" and the wrong one to "am I free on
 * Thursday". These check the two ways in, the stepping that makes it a week,
 * and the Escape rule -- which is the fragile part, because the shell now
 * offers Escape to the app first and leaves the app only if the app says no.
 */

/* Two events on the 13th, one on the 14th, and nothing on the 12th. */
static void three_events_no_zone(void) {
  use_fake_api();
  C.offset = 0;
  parse_reply(REPLY);
}

void test_calendar_enter_opens_the_day_of_the_selected_event(void) {
  three_events_no_zone();
  C.sel = 2;                                  /* the birthday, on the 14th */
  CHECK_EQ(1, key_agenda(CAPP_KEY_ENTER));
  CHECK_EQ(VIEW_DAY, C.view);
  CHECK_EQ(days_from_civil(2026, 9, 14), (int)C.day_shown);
  CHECK_EQ(1, day_event_count(C.day_shown));
}

void test_calendar_the_day_view_opens_on_the_event_not_the_top_of_the_day(void) {
  three_events_no_zone();
  C.sel = 1;                                  /* the dentist, second that day */
  key_agenda(CAPP_KEY_ENTER);
  CHECK_EQ(1, C.day_sel);
}

void test_calendar_left_and_right_page_through_the_week(void) {
  three_events_no_zone();
  C.sel = 0;
  key_agenda(CAPP_KEY_ENTER);
  CHECK_EQ(days_from_civil(2026, 9, 13), (int)C.day_shown);
  CHECK_EQ(1, key_day(CAPP_KEY_RIGHT));
  CHECK_EQ(days_from_civil(2026, 9, 14), (int)C.day_shown);
  key_day(CAPP_KEY_LEFT);
  key_day(CAPP_KEY_LEFT);
  /* The 12th is a day the agenda never draws, and the point of the view. */
  CHECK_EQ(days_from_civil(2026, 9, 12), (int)C.day_shown);
  CHECK_EQ(0, day_event_count(C.day_shown));
  CHECK_EQ(0, C.day_sel);
}

void test_calendar_up_and_down_move_between_that_days_events(void) {
  three_events_no_zone();
  C.sel = 0;
  key_agenda(CAPP_KEY_ENTER);
  CHECK_EQ(2, day_event_count(C.day_shown));
  CHECK_EQ(0, C.day_sel);
  key_day(CAPP_KEY_DOWN);
  CHECK_EQ(1, C.day_sel);
  key_day(CAPP_KEY_DOWN);
  CHECK_EQ(1, C.day_sel);          /* the end of the day is the end of it */
  key_day(CAPP_KEY_UP);
  CHECK_EQ(0, C.day_sel);
  key_day(CAPP_KEY_UP);
  CHECK_EQ(0, C.day_sel);
}

void test_calendar_escape_leaves_the_day_view_and_not_the_app(void) {
  three_events_no_zone();
  C.sel = 1;
  key_agenda(CAPP_KEY_ENTER);
  /* Handled, so the shell keeps the app open. */
  CHECK_EQ(1, app_key(0, CAPP_KEY_ESC));
  CHECK_EQ(VIEW_AGENDA, C.view);
  CHECK_EQ(1, C.sel);
  /* And at the top there is nowhere to go back to, so the shell may leave. */
  CHECK_EQ(0, app_key(0, CAPP_KEY_ESC));
  CHECK_EQ(VIEW_AGENDA, C.view);
}

void test_calendar_enter_on_the_month_grid_opens_that_day(void) {
  three_events_no_zone();
  C.view = VIEW_MONTH;
  C.cur_y = 2026; C.cur_m = 9; C.cur_d = 14;
  CHECK_EQ(1, key_month(CAPP_KEY_ENTER));
  CHECK_EQ(VIEW_DAY, C.view);
  CHECK_EQ(days_from_civil(2026, 9, 14), (int)C.day_shown);
  /* Paged forward, then back to the grid -- which should be sitting on the
   * day the paging ended on, not on the one it started from. */
  key_day(CAPP_KEY_RIGHT);
  CHECK_EQ(1, app_key(0, CAPP_KEY_ESC));
  CHECK_EQ(VIEW_MONTH, C.view);
  CHECK_EQ(15, C.cur_d);
}

void test_calendar_the_day_view_is_one_of_the_apps_declared_actions(void) {
  int i, found = 0;
  three_events_no_zone();
  for (i = 0; i < NMAIN; i++)
    if (strcmp(MAIN_ACTIONS[i].id, "day") == 0) found = 1;
  CHECK_EQ(1, found);
  /* The menu bar and the fn-h panel reach it through the same door the key
   * does, so it is the action that must open the view. */
  C.sel = 0;
  CHECK_EQ(1, do_action(ACT_DAY));
  CHECK_EQ(VIEW_DAY, C.view);
}

void test_calendar_adding_from_the_day_view_adds_to_that_day(void) {
  three_events_no_zone();
  C.sel = 0;
  key_agenda(CAPP_KEY_ENTER);
  key_day(CAPP_KEY_RIGHT);
  key_day('a');
  CHECK_EQ(VIEW_ADD, C.view);
  CHECK_EQ(days_from_civil(2026, 9, 14), (int)C.draft_day);
}

/* ---- the sync, driven end to end ------------------------------------------
 *
 * The report was "it fails now and then", which is the report a sync with no
 * log produces. These pin the three things that made it fail silently, and
 * the log lines that would have said so.
 */

static char LOGGED[24][96];
static int  NLOGGED;

static void fake_log(const char *s) {
  if (NLOGGED >= (int)(sizeof LOGGED / sizeof LOGGED[0])) return;
  snprintf(LOGGED[NLOGGED++], sizeof LOGGED[0], "%s", s);
}

static int logged_has(const char *needle) {
  int i;
  for (i = 0; i < NLOGGED; i++)
    if (strstr(LOGGED[i], needle)) return 1;
  return 0;
}

static int  STARTS, START_RESULT, POLL_RESULT;
static char LAST_BODY[256];
static char LAST_URL[URL_MAX];
static const char *POLL_BODY;

static char LAST_METHOD[8];

static int fake_start(const char *m, const char *u, const char *b,
                      const char *ct, const char *tok, int ms) {
  (void)ct; (void)tok; (void)ms;
  STARTS++;
  snprintf(LAST_METHOD, sizeof LAST_METHOD, "%s", m ? m : "");
  snprintf(LAST_URL, sizeof LAST_URL, "%s", u);
  snprintf(LAST_BODY, sizeof LAST_BODY, "%s", b ? b : "");
  return START_RESULT;
}

static int fake_poll(char *out, size_t n) {
  if (POLL_BODY) snprintf(out, n, "%s", POLL_BODY);
  return POLL_RESULT;
}

/* No card in the tests, so every cache write is a no-op. */
static int fake_open(const char *path, int flags) { (void)path; (void)flags; return -1; }

static uint32_t fake_epoch(void) { return (uint32_t)(20709u * 86400u + 12u * 3600u); }

static void fake_now(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->year = 2026; t->month = 9; t->day = 13; t->hour = 12; t->synced = 2;
}

static void fake_now_unsynced(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->year = 1970; t->month = 1; t->day = 1;
}

static const char *fake_no_token(void) { return ""; }
static const char *fake_gstatus(void)  { return "not signed in on the PC"; }

static void use_sync_api(void) {
  use_fake_api();
  FAKE.ticks_ms = fake_ticks;
  FAKE.net_ready = fake_net_ready;
  FAKE.google_token = fake_token;
  FAKE.google_status = fake_gstatus;
  FAKE.http_start = fake_start;
  FAKE.http_poll = fake_poll;
  FAKE.open = fake_open;
  FAKE.now = fake_now;
  FAKE.epoch = fake_epoch;
  FAKE.log = fake_log;
  NLOGGED = 0;
  STARTS = 0;
  START_RESULT = 0;
  POLL_RESULT = CAPP_HTTP_PENDING;
  POLL_BODY = 0;
  LAST_BODY[0] = 0;
  LAST_URL[0] = 0;
  refresh_clock();
}

void test_calendar_a_sync_logs_what_started_it_and_what_came_back(void) {
  use_sync_api();
  sync_begin("opened");
  CHECK_EQ(1, STARTS);
  CHECK(logged_has("opened"));

  /* -1000 means the request is still running. It arrives every few
   * milliseconds and is not a result; logging it would fill the card and
   * read like a failure. */
  NLOGGED = 0;
  sync_tick();
  CHECK_EQ(0, NLOGGED);

  POLL_RESULT = (int)strlen(REPLY);
  POLL_BODY = REPLY;
  sync_tick();
  CHECK_EQ(SYNC_IDLE, C.stage);
  CHECK_EQ(3, C.n);
  CHECK(logged_has("3 events absorbed"));
}

void test_calendar_a_failed_request_is_logged_with_its_code(void) {
  use_sync_api();
  sync_begin("s");
  POLL_RESULT = -403;                 /* the HTTP status, negated */
  sync_tick();
  CHECK(logged_has("-403"));
  CHECK(strstr(C.status, "403") != NULL);
  CHECK_EQ(SYNC_IDLE, C.stage);
}

void test_calendar_a_refused_start_is_logged_as_a_busy_queue(void) {
  use_sync_api();
  START_RESULT = -1;                  /* one request at a time, device-wide */
  sync_begin("auto");
  CHECK_EQ(SYNC_IDLE, C.stage);
  CHECK(logged_has("busy"));
  CHECK_EQ((int)(1000 + RETRY_MS), (int)C.next_auto);
}

/* The Google login is the server's now (server/google.py): no token here to
 * be missing, and the server says 401 when its own is gone. That is the one
 * thing only the owner can fix, so the status says where. */
void test_calendar_the_servers_401_says_where_to_sign_in(void) {
  use_sync_api();
  sync_begin("opened");
  CHECK_EQ(1, STARTS);
  POLL_RESULT = -401;
  sync_tick();
  CHECK(strstr(C.status, "/dash") != NULL);
  CHECK_EQ(SYNC_IDLE, C.stage);
}

/* The bug behind "it works some mornings": the clock arrives from NTP a few
 * seconds after boot, and a calendar opened inside those seconds sampled
 * have_clock once and never again. Every fetch then asked for sixty days from
 * 1 January 1970, got nothing, and wrote the nothing over the cache. */
void test_calendar_a_sync_without_a_clock_does_not_fetch_1970(void) {
  use_sync_api();
  FAKE.now = fake_now_unsynced;
  C.have_clock = 0;
  sync_begin("opened");
  CHECK_EQ(0, STARTS);
  CHECK_EQ(SYNC_IDLE, C.stage);
  CHECK(logged_has("clock"));
  CHECK_EQ((int)(1000 + RETRY_MS), (int)C.next_auto);
}

void test_calendar_a_clock_that_arrives_late_is_picked_up(void) {
  use_sync_api();
  C.have_clock = 0;                   /* opened before NTP answered */
  C.today_y = 0;
  sync_begin("auto");
  CHECK_EQ(1, C.have_clock);
  CHECK_EQ(2026, C.today_y);
  CHECK_EQ(1, STARTS);
  CHECK(strstr(LAST_URL, "from=2026-09-13") != NULL);
}

/* The other silent one: a POST was remembered by its index into the list, and
 * anything typed or deleted while it was in the air re-sorted that list. */
void test_calendar_a_push_is_matched_to_its_event_not_its_index(void) {
  use_sync_api();
  C.n = 1;
  memset(&C.ev[0], 0, sizeof C.ev[0]);
  snprintf(C.ev[0].summary, sizeof C.ev[0].summary, "Dentist");
  C.ev[0].start = 20709u * 86400u + 14u * 3600u;
  C.ev[0].dirty = 1;

  sync_begin("s");
  CHECK_EQ(SYNC_PUSH, C.stage);
  CHECK(strstr(LAST_BODY, "Dentist") != NULL);

  /* Typed while the POST was still in the air, and earlier in the day, so it
   * sorts in front of the event being pushed. */
  memset(&C.ev[1], 0, sizeof C.ev[1]);
  snprintf(C.ev[1].summary, sizeof C.ev[1].summary, "Standup");
  C.ev[1].start = 20709u * 86400u + 9u * 3600u;
  C.ev[1].dirty = 1;
  C.n = 2;
  sort_events();

  POLL_RESULT = 2;
  POLL_BODY = "";
  START_RESULT = -1;                  /* stop the chain here; the bookkeeping
                                         is what is under test */
  sync_tick();

  CHECK(strcmp(C.ev[0].summary, "Standup") == 0);
  CHECK_EQ(1, C.ev[0].dirty);         /* never sent, so still queued */
  CHECK_EQ(0, C.ev[1].dirty);         /* Dentist, which Google now has */
  CHECK_EQ(0, C.ev[1].sending);
}

/* And the same request failing to start halfway through a sync used to drop
 * to idle without a word, with the next attempt ten minutes away. */
void test_calendar_a_refused_push_mid_sync_says_so_and_retries_soon(void) {
  use_sync_api();
  C.n = 2;
  memset(C.ev, 0, sizeof C.ev[0] * 2);
  snprintf(C.ev[0].summary, sizeof C.ev[0].summary, "One");
  C.ev[0].start = 20709u * 86400u + 9u * 3600u;
  C.ev[0].dirty = 1;
  snprintf(C.ev[1].summary, sizeof C.ev[1].summary, "Two");
  C.ev[1].start = 20709u * 86400u + 10u * 3600u;
  C.ev[1].dirty = 1;

  sync_begin("s");
  CHECK_EQ(SYNC_PUSH, C.stage);
  POLL_RESULT = 2;
  POLL_BODY = "";
  START_RESULT = -1;
  sync_tick();

  CHECK_EQ(SYNC_IDLE, C.stage);
  CHECK(strstr(C.status, "busy") != NULL);
  CHECK_EQ((int)(1000 + RETRY_MS), (int)C.next_auto);
}

/* A fetch that comes back shorter left the agenda scrolled off the end of
 * the list, which paints an empty screen -- and on a device with no other
 * report, an empty screen is a sync that lost the diary. */
void test_calendar_absorbing_a_shorter_list_pulls_the_scroll_back(void) {
  use_sync_api();
  parse_reply(REPLY);
  C.sel = 2;
  C.top = 2;
  snprintf(C.reply, sizeof C.reply, "%s", "");
  CHECK_EQ(0, absorb());
  CHECK_EQ(0, C.n);
  CHECK_EQ(0, C.sel);
  CHECK_EQ(0, C.top);
}

/* And `d` still deletes, which is what it does in Todo too. A letter that
 * deletes in one app and changes the view in another is how an event gets
 * lost by someone who learned the other app first. */
void test_calendar_d_deletes_rather_than_changing_the_view(void) {
  three_events_no_zone();
  C.sel = 0;
  C.view = VIEW_AGENDA;
  key_agenda('d');
  CHECK_EQ(VIEW_AGENDA, C.view);
}

/* A chord in the action table is matched by the SHELL, before the app's key
 * handler runs (kernel/app/capprun.c). So an action that claims a control
 * byte a real key already sends takes that key away from every view in the
 * app, and no test that calls key_agenda() directly can see it: the table is
 * not consulted on that path.
 *
 * That is exactly what happened. Month claimed 0x0D "ctrl-m", which is the
 * byte Enter sends, so Enter opened the month grid and the day view could
 * never have it. It is the same collision that took the help key, where
 * ctrl-h is the byte Backspace sends. */
void test_calendar_no_action_chord_collides_with_a_real_key(void) {
  int i;
  for (i = 0; i < (int)(sizeof MAIN_ACTIONS / sizeof MAIN_ACTIONS[0]); i++) {
    unsigned char k = (unsigned char)MAIN_ACTIONS[i].key;
    CHECK(k != CAPP_KEY_ENTER);        /* 0x0D, ctrl-m */
    CHECK(k != CAPP_KEY_BACK);         /* 0x08, ctrl-h */
    CHECK(k != 0x09);                  /* tab, ctrl-i */
    CHECK(k != CAPP_KEY_ESC);          /* 0x1B */
  }
}

/* The month grid is a view you went into, so Escape comes back out of it.
 * It used to fall through to the shell, which took that as "leave the app" --
 * the same key going back in one view and quitting from another. */
void test_calendar_escape_leaves_the_month_grid_and_not_the_app(void) {
  three_events_no_zone();
  C.view = VIEW_AGENDA;
  do_action(ACT_MONTH);
  CHECK_EQ(VIEW_MONTH, C.view);
  CHECK_EQ(1, key_month(CAPP_KEY_ESC));
  CHECK_EQ(VIEW_AGENDA, C.view);
  /* And the agenda still declines it, so the shell can leave. */
  CHECK_EQ(0, key_agenda(CAPP_KEY_ESC));
}

static const char *fake_net_status_memory(void) {
  return "not enough memory: 37 KB free, TLS needs about 33";
}

/* -4 is the kernel refusing to start the request at all, and it already
 * knows why. Printing the code sends the reader to look at their wifi; the
 * sentence sends them to close an app, which is the thing that works. It is
 * also transient, so the next attempt belongs in seconds, not ten minutes. */
void test_calendar_a_refusal_for_memory_says_so_and_retries_soon(void) {
  three_events_no_zone();
  FAKE.net_status = fake_net_status_memory;
  FAKE.ticks_ms = fake_ticks;
  C.next_auto = 0;

  sync_failed(-4);

  CHECK(strstr(C.status, "not enough memory") != NULL);
  CHECK(strstr(C.status, "-4") == NULL);
  CHECK_EQ(C.next_auto, fake_ticks() + RETRY_MS);
  CHECK_EQ(SYNC_IDLE, C.stage);
}

/* ---- editing ----------------------------------------------------------------
 *
 * `e` opens the add form on the selected event; Save queues the change, and
 * the push sends an event Google already has as a PATCH to its id.
 */

static void synced_three_events(void) {
  use_sync_api();
  C.offset = 0;
  parse_reply(REPLY);
  C.view = VIEW_AGENDA;
}

void test_calendar_e_opens_the_form_on_the_selected_event(void) {
  synced_three_events();
  C.sel = 1;                                  /* Dentist, 13:00 UTC */
  key_agenda('e');
  CHECK_EQ(VIEW_ADD, C.view);
  CHECK_EQ(1, C.form_edit);
  CHECK(strcmp(C.draft, "Dentist") == 0);
  CHECK_EQ(13, C.draft_hour);
  CHECK_EQ(0, C.draft_min);
  CHECK_EQ(days_from_civil(2026, 9, 13), (int)C.draft_day);
}

void test_calendar_an_edit_is_sent_as_a_patch_and_keeps_the_length(void) {
  synced_three_events();
  C.sel = 0;                                  /* Standup, 08:30, 15 minutes */
  key_agenda('e');
  key_add('\t');                              /* the day */
  key_add(CAPP_KEY_RIGHT);                    /* the 14th */
  key_add('\t');                              /* the time */
  key_add(CAPP_KEY_UP);                       /* 09:30 */
  key_add(CAPP_KEY_ENTER);

  CHECK_EQ(VIEW_AGENDA, C.view);
  CHECK_EQ(0, C.form_edit);
  CHECK(strcmp(LAST_METHOD, "POST") == 0);
  CHECK(strstr(LAST_URL, "/calendar/event?id=aaa111") != NULL);
  CHECK(strstr(LAST_BODY, "summary=Standup\n") != NULL);
  CHECK(strstr(LAST_BODY, "start=2026-09-14T09:30:00Z\n") != NULL);
  CHECK(strstr(LAST_BODY, "end=2026-09-14T09:45:00Z\n") != NULL);
}

void test_calendar_a_new_event_is_still_a_post(void) {
  use_sync_api();
  add_event("Lunch", days_from_civil(2026, 9, 13), 12, 0);
  sync_begin("s");
  CHECK(strcmp(LAST_METHOD, "POST") == 0);
  CHECK(strstr(LAST_URL, "?id=") == NULL);
}

void test_calendar_an_all_day_event_stays_all_day(void) {
  synced_three_events();
  C.sel = 2;                                  /* Birthday, the 14th */
  key_agenda('e');
  CHECK_EQ(1, C.draft_all_day);
  key_add('\t');                              /* the day */
  key_add('\t');                              /* skips the time: back to the title */
  CHECK_EQ(FIELD_TITLE, C.field);
  key_add('\t');
  key_add(CAPP_KEY_RIGHT);                    /* the 15th */
  key_add(CAPP_KEY_ENTER);
  CHECK(strstr(LAST_BODY, "start=2026-09-15\n") != NULL);     /* a bare date */
  CHECK(strstr(LAST_BODY, "end=2026-09-16\n") != NULL);
}

/* A title from Google can hold a quote. The body is lines now, not JSON, so
 * it goes as it is -- the server makes the JSON. */
void test_calendar_a_quote_in_a_title_goes_as_it_is(void) {
  synced_three_events();
  snprintf(C.ev[1].summary, sizeof C.ev[1].summary, "%s", "Say \"hi\"");
  C.sel = 1;
  key_agenda('e');
  key_add(CAPP_KEY_ENTER);
  CHECK(strstr(LAST_BODY, "summary=Say \"hi\"\n") != NULL);
}

/* A fetch that lands while an edit is still queued must not undo it. */
void test_calendar_a_queued_edit_survives_a_fetch(void) {
  synced_three_events();
  snprintf(C.ev[1].summary, sizeof C.ev[1].summary, "%s", "Dentist, moved");
  C.ev[1].dirty = 1;
  snprintf(C.reply, sizeof C.reply, "%s", REPLY);
  absorb();
  CHECK_EQ(3, C.n);
  {
    int i, found = 0, stale = 0;
    for (i = 0; i < C.n; i++) {
      if (!strcmp(C.ev[i].summary, "Dentist, moved")) found++;
      if (!strcmp(C.ev[i].summary, "Dentist")) stale++;
    }
    CHECK_EQ(1, found);
    CHECK_EQ(0, stale);
  }
}

/* And a sync that lands while the form is open leaves it on the same event. */
void test_calendar_the_form_follows_its_event_through_a_sync(void) {
  synced_three_events();
  C.sel = 1;
  key_agenda('e');
  snprintf(C.reply, sizeof C.reply, "%s", REPLY);
  absorb();                                   /* every struct made anew */
  C.draft[0] = 0;
  C.draft_len = 0;
  {
    const char *t = "Dentist 2";
    while (*t) key_add((unsigned char)*t++);
  }
  key_add(CAPP_KEY_ENTER);
  CHECK(strstr(LAST_URL, "?id=bbb222") != NULL);
  CHECK(strstr(LAST_BODY, "Dentist 2") != NULL);
}

/* Deleted in a browser while the edit was queued: dropped, not retried at
 * every sync from now on. */
void test_calendar_a_patch_to_a_deleted_event_is_dropped(void) {
  synced_three_events();
  C.ev[1].dirty = 1;
  sync_begin("s");
  CHECK(strstr(LAST_URL, "?id=bbb222") != NULL);
  POLL_RESULT = -404;
  POLL_BODY = "";
  sync_tick();
  CHECK_EQ(0, C.ev[1].dirty);
  CHECK_EQ(SYNC_FETCH, C.stage);              /* and it carries on */
  CHECK(logged_has("edit dropped"));
}

/* Other failures are still failures: the edit stays queued. */
void test_calendar_a_failed_patch_keeps_the_edit(void) {
  synced_three_events();
  C.ev[1].dirty = 1;
  sync_begin("s");
  POLL_RESULT = -500;
  sync_tick();
  CHECK_EQ(1, C.ev[1].dirty);
  CHECK_EQ(SYNC_IDLE, C.stage);
}

/* West of Greenwich an all-day event used to show on the day before: a bare
 * date is midnight UTC, and the zone was taken off it like a timed event. */
void test_calendar_an_all_day_event_is_on_its_own_date_anywhere(void) {
  synced_three_events();
  C.offset = -7 * 3600;                       /* Pacific daylight time */
  CHECK_EQ(days_from_civil(2026, 9, 14), (int)ev_day(&C.ev[2]));
  C.offset = 9 * 3600;                        /* Tokyo */
  CHECK_EQ(days_from_civil(2026, 9, 14), (int)ev_day(&C.ev[2]));
}

void test_calendar_edit_is_one_of_the_apps_declared_actions(void) {
  int i, found = 0;
  for (i = 0; i < (int)(sizeof MAIN_ACTIONS / sizeof MAIN_ACTIONS[0]); i++)
    if (MAIN_ACTIONS[i].action == ACT_EDIT) found = 1;
  CHECK(found);
}

/* ---- one key vocabulary ------------------------------------------------------
 *
 * Escape goes back one level out of everything, d always asks, and a chord
 * from the action table -- matched by the shell before any key handler, in
 * every view -- only does what the view on screen means.
 */

/* The form ignored Escape: it fell through every test in key_add and came
 * back declined. It is a form, so Escape cancels it, back to where it came
 * from. */
void test_calendar_escape_cancels_the_form_back_to_where_it_came_from(void) {
  three_events_no_zone();
  C.sel = 0;
  key_agenda(CAPP_KEY_ENTER);
  CHECK_EQ(VIEW_DAY, C.view);
  key_day('n');
  CHECK_EQ(VIEW_ADD, C.view);
  key_add('x');
  CHECK_EQ(1, app_key(0, CAPP_KEY_ESC));
  CHECK_EQ(VIEW_DAY, C.view);
  CHECK_EQ(3, C.n);                           /* nothing added */

  app_key(0, CAPP_KEY_ESC);                   /* the day view's own Escape */
  CHECK_EQ(VIEW_AGENDA, C.view);
  app_key(0, 'a');
  CHECK_EQ(VIEW_ADD, C.view);
  CHECK_EQ(1, app_key(0, CAPP_KEY_ESC));
  CHECK_EQ(VIEW_AGENDA, C.view);
}

/* ctrl-g, ctrl-y, ctrl-n and ctrl-d used to fire in the form: out of it to
 * another view with the form's menus still up, or a delete of whatever the
 * agenda had selected. */
void test_calendar_view_chords_do_nothing_in_the_form(void) {
  three_events_no_zone();
  C.sel = 0;
  key_agenda('e');
  CHECK_EQ(VIEW_ADD, C.view);
  CHECK_EQ(1, app_action(0, ACT_AGENDA));
  CHECK_EQ(1, app_action(0, ACT_DAY));
  CHECK_EQ(1, app_action(0, ACT_MONTH));
  CHECK_EQ(1, app_action(0, ACT_DELETE));
  CHECK_EQ(VIEW_ADD, C.view);
  CHECK_EQ(0, C.confirm);
  CHECK_EQ(1, C.form_edit);                   /* still editing it */
  CHECK_EQ(1, app_action(0, ACT_CANCEL));
  CHECK_EQ(VIEW_AGENDA, C.view);
  CHECK_EQ(0, C.form_edit);
}

/* Two unsynced events, one each on two days. ctrl-d in the day view of the
 * second deleted the agenda's selection -- the first. It must ask about the
 * one highlighted on screen, and only y deletes it. */
void test_calendar_delete_in_the_day_view_asks_about_that_days_event(void) {
  int32_t a = days_from_civil(2026, 9, 13), b = days_from_civil(2026, 9, 14);
  use_fake_api();
  C.offset = 0;
  add_event("First", a, 9, 0);
  add_event("Second", b, 9, 0);
  C.sel = 0;                                  /* the agenda is on First */
  open_day(b, VIEW_AGENDA);

  CHECK_EQ(1, app_action(0, ACT_DELETE));
  CHECK_EQ(1, C.confirm);
  CHECK(strcmp(C.ev[asking_index()].summary, "Second") == 0);
  CHECK_EQ(1, app_key(0, CAPP_KEY_DOWN));     /* not an answer: swallowed */
  CHECK_EQ(1, C.confirm);
  CHECK_EQ(1, app_key(0, 'n'));
  CHECK_EQ(0, C.confirm);
  CHECK_EQ(2, C.n);

  app_key(0, 'd');
  CHECK_EQ(1, C.confirm);
  app_key(0, CAPP_KEY_BACK);
  CHECK_EQ(0, C.confirm);
  CHECK_EQ(2, C.n);
  app_key(0, 0x7F);
  app_key(0, 'y');
  CHECK_EQ(0, C.confirm);
  CHECK_EQ(1, C.n);
  CHECK(strcmp(C.ev[0].summary, "First") == 0);
  CHECK_EQ(VIEW_DAY, C.view);
}

/* Google's events cannot be deleted from here (the server has no delete),
 * so d says so rather than asking a question it cannot act on. */
void test_calendar_delete_of_a_synced_event_says_where_instead_of_asking(void) {
  three_events_no_zone();
  C.sel = 0;
  key_agenda('d');
  CHECK_EQ(0, C.confirm);
  CHECK(strstr(C.status, "Google Calendar") != NULL);
  CHECK_EQ(3, C.n);
}

void test_calendar_n_is_new_and_r_syncs(void) {
  use_sync_api();
  C.view = VIEW_AGENDA;
  key_agenda('n');
  CHECK_EQ(VIEW_ADD, C.view);
  app_key(0, CAPP_KEY_ESC);
  CHECK_EQ(VIEW_AGENDA, C.view);
  key_agenda('r');
  CHECK_EQ(1, STARTS);
}

/* ---- paper ------------------------------------------------------------------ */

static char PRINTED[2600];
static int fake_print(const char *doc, const char *body, const char *bold,
                      const char *head) {
  (void)body; (void)bold; (void)head;
  snprintf(PRINTED, sizeof PRINTED, "%s", doc);
  return 0;
}
static void fake_print_now(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->synced = 2; t->year = 2026; t->month = 9; t->day = 20; t->hour = 10; t->min = 5;
}

/* The agenda as it reads: a heading per day, a line per event. */
void test_calendar_prints_the_agenda_a_heading_per_day(void) {
  three_events_no_zone();
  FAKE.print_fonts = fake_print;
  FAKE.now = fake_print_now;
  C.view = VIEW_AGENDA;
  CHECK_EQ(1, app_action(0, ACT_PRINT));
  CHECK(strcmp(PRINTED,
    "# Calendar\n## Sun 13 Sep\n08:30 Standup\n13:00 Dentist\n"
    "## Mon 14 Sep\nall day Birthday\n---\nprinted 20 Sep 10:05\n") == 0);
  CHECK_EQ(1, C.printing);
}

/* The day view prints that day, under its full date -- empty or not. */
void test_calendar_prints_the_day_on_screen(void) {
  three_events_no_zone();
  FAKE.print_fonts = fake_print;
  FAKE.now = fake_print_now;
  C.sel = 0;
  key_agenda(CAPP_KEY_ENTER);
  key_day('p');
  CHECK(strcmp(PRINTED,
    "# Sun 13 Sep 2026\n08:30 Standup\n13:00 Dentist\n---\nprinted 20 Sep 10:05\n") == 0);
  key_day(CAPP_KEY_LEFT);
  key_day('p');
  CHECK(strstr(PRINTED, "# Sat 12 Sep 2026\nNothing on.\n") == PRINTED);
}

/* fn-p is the print chord in every app that prints: an action with the key. */
void test_calendar_print_is_fn_p_in_the_action_table(void) {
  int i, found = 0;
  for (i = 0; i < NMAIN; i++)
    if (MAIN_ACTIONS[i].action == ACT_PRINT && MAIN_ACTIONS[i].key == CAPP_KEY_PRINT)
      found = 1;
  CHECK(found);
}

/* ---- repainting only what changed ------------------------------------------- */

static CRect DAMAGED[8];
static int   NDAMAGED;
static void record_damage(CRect r) {
  if (NDAMAGED < 8) DAMAGED[NDAMAGED] = r;
  NDAMAGED++;
}
static int fake_not_headless(void) { return 0; }

/* A sync in the air used to make every tick answer "repaint" with nothing
 * marked -- the whole window, every 5 ms, for the length of a request. Now a
 * tick with nothing new asks for nothing, and a status that moved asks for
 * the footer alone. */
void test_calendar_a_sync_in_the_air_does_not_repaint_the_window(void) {
  CRect c;
  use_sync_api();
  FAKE.damage = record_damage;
  FAKE.headless = fake_not_headless;
  c.x = 0; c.y = 0; c.w = 240; c.h = 135;
  C.full = c;
  C.content = c;
  sync_begin("s");
  CHECK(C.stage != SYNC_IDLE);
  snprintf(C.last_status, sizeof C.last_status, "%s", C.status);

  NDAMAGED = 0;
  CHECK_EQ(0, app_tick(0, fake_ticks()));
  CHECK_EQ(0, NDAMAGED);

  say("something new");
  CHECK_EQ(1, app_tick(0, fake_ticks()));
  CHECK_EQ(1, NDAMAGED);
  CHECK_EQ(135 - FOOT_H, DAMAGED[0].y);
  CHECK_EQ(FOOT_H, DAMAGED[0].h);
}

/* After a sync the app sets the week's reminders itself: at the minutes
 * Google gave, not for all-day events, past ones, or ones with none, and
 * replacing what it set before. */
void test_calendar_a_sync_sets_the_weeks_reminders(void) {
  static char reply[512];
  uint32_t now;
  use_sync_api();
  now = fake_epoch();                           /* 2026-09-13 12:00 UTC, offset 0 */
  snprintf(reply, sizeof reply,
           "a1\t2026-09-13T14:00:00Z\t2026-09-13T15:00:00Z\tDentist\t30\n"   /* in 2 h */
           "a2\t2026-09-13T13:00:00Z\t2026-09-13T14:00:00Z\tLunch\t-1\n"     /* none */
           "a3\t2026-09-14\t2026-09-15\tHoliday\t0\n"                       /* all day */
           "a4\t2026-09-13T09:00:00Z\t2026-09-13T10:00:00Z\tPast\t10\n"     /* over */
           "a5\t2026-09-30T09:00:00Z\t2026-09-30T10:00:00Z\tLater\t10\n");  /* past the week */
  memcpy(C.reply, reply, strlen(reply) + 1);
  absorb();
  CHECK_EQ(NCANCEL_ALL, 1);
  CHECK_EQ(NSCHED, 1);
  CHECK_EQ((int)SCHED_SECS[0], 2 * 3600 - 30 * 60);
  CHECK(!strcmp(SCHED_TITLE[0], "14:00, in 30 min"));
  CHECK(!strcmp(SCHED_TEXT[0], "Dentist"));
  (void)now;
}
