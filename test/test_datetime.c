/* apps/datetime.h: the parts test_calendar.c does not already hammer -- the
 * zone offset from the kernel's two clocks, and the durations. */

#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "apps/datetime.h"

static CardApi TA;

void test_datetime_the_offset_is_local_minus_utc_across_midnight(void) {
  int32_t off;
  fakeapi_init(&TA);
  /* 2026-10-09 23:30 UTC; Pacific daylight time is 16:30 the same day. */
  fakeapi_epoch = (uint32_t)dt_days_from_civil(2026, 10, 9) * DT_DAY_SECS + 23 * 3600 + 30 * 60;
  fakeapi_now.year = 2026; fakeapi_now.month = 10; fakeapi_now.day = 9;
  fakeapi_now.hour = 16; fakeapi_now.min = 30; fakeapi_now.sec = 0;
  CHECK_EQ(dt_utc_offset(&TA, &off), 1);
  CHECK_EQ(off, -7 * 3600);
  /* And east of it, already the next day: 2026-10-10 08:30 in Tokyo. */
  fakeapi_now.day = 10; fakeapi_now.hour = 8;
  dt_utc_offset(&TA, &off);
  CHECK_EQ(off, 9 * 3600);
}

void test_datetime_no_clock_is_no_offset(void) {
  int32_t off = 123;
  fakeapi_init(&TA);
  fakeapi_now.synced = 0;
  CHECK_EQ(dt_utc_offset(&TA, &off), 0);
  CHECK_EQ(off, 0);
  fakeapi_now.synced = 2;
  fakeapi_epoch = 0;                   /* the kernel's "I do not know" */
  CHECK_EQ(dt_utc_offset(&TA, &off), 0);
}

void test_datetime_durations(void) {
  char s[16];
  fakeapi_init(&TA);
  dt_mmss(&TA, 187000, s, sizeof s); CHECK(!strcmp(s, "3:07"));
  dt_mmss(&TA, 999, s, sizeof s);    CHECK(!strcmp(s, "0:00"));
  dt_hms(&TA, 3727, s, sizeof s);    CHECK(!strcmp(s, "1:02:07"));
  dt_hm(&TA, 3727, s, sizeof s);     CHECK(!strcmp(s, "1:02"));
}

void test_datetime_weekdays_and_months(void) {
  CHECK_EQ(dt_weekday(dt_days_from_civil(2026, 10, 9)), 5);   /* a Friday */
  CHECK_EQ(dt_days_in_month(2028, 2), 29);
  CHECK_EQ(dt_days_in_month(2026, 13), 30);                    /* not a month */
}
