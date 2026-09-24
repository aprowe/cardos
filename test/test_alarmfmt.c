/* /config/alarms.txt: a line per alarm, read by the kernel (which rings them)
 * and by the Clock app (which edits them). The rules that matter are when an
 * alarm rings -- which days, once, a snooze -- and that a line a person typed
 * by hand still reads. */
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/alarmfmt.h"

enum { SUN, MON, TUE, WED, THU, FRI, SAT };

void test_alarm_parses_what_it_writes(void) {
  Alarm a;
  char line[64];
  CHECK_EQ(alarm_parse("on 07:05 -MTWTF- Wake up", &a), 1);
  CHECK_EQ(a.on, 1); CHECK_EQ(a.hour, 7); CHECK_EQ(a.min, 5);
  CHECK_EQ(a.days, 0x3E);                       /* Mon..Fri */
  CHECK_EQ(a.kind, ALARM_REPEAT);
  CHECK(!strcmp(a.label, "Wake up"));
  alarm_format(&a, line, sizeof line);
  CHECK(!strcmp(line, "on 07:05 -MTWTF- Wake up"));
  CHECK_EQ(alarm_parse("off 23:59 once", &a), 1);
  CHECK_EQ(a.on, 0); CHECK_EQ(a.kind, ALARM_ONCE); CHECK(!strcmp(a.label, ""));
  alarm_format(&a, line, sizeof line);
  CHECK(!strcmp(line, "off 23:59 once"));
  CHECK_EQ(alarm_parse("on 6:30 snooze  Tea ", &a), 1);
  CHECK_EQ(a.kind, ALARM_SNOOZE); CHECK_EQ(a.hour, 6); CHECK(!strcmp(a.label, "Tea"));
}

void test_alarm_refuses_nonsense(void) {
  Alarm a;
  CHECK_EQ(alarm_parse("", &a), 0);
  CHECK_EQ(alarm_parse("# a comment", &a), 0);
  CHECK_EQ(alarm_parse("maybe 07:00 once", &a), 0);
  CHECK_EQ(alarm_parse("on 24:00 once", &a), 0);
  CHECK_EQ(alarm_parse("on 07:60 once", &a), 0);
  CHECK_EQ(alarm_parse("on 0700 once", &a), 0);
  CHECK_EQ(alarm_parse("on 07:00 SMTWT", &a), 0);  /* six days is not a week */
  CHECK_EQ(alarm_parse("on 07:00", &a), 0);
}

void test_alarm_rings_on_its_days_only(void) {
  Alarm a;
  alarm_parse("on 07:00 -MTWTF- Work", &a);
  CHECK(alarm_rings_at(&a, MON, 7, 0));
  CHECK(alarm_rings_at(&a, FRI, 7, 0));
  CHECK(!alarm_rings_at(&a, SAT, 7, 0));
  CHECK(!alarm_rings_at(&a, MON, 7, 1));
  a.on = 0;
  CHECK(!alarm_rings_at(&a, MON, 7, 0));
  alarm_parse("on 09:30 once", &a);
  CHECK(alarm_rings_at(&a, SAT, 9, 30));         /* once: any day */
}

void test_alarm_minutes_until_the_next_ring(void) {
  Alarm a;
  alarm_parse("on 07:00 -MTWTF-", &a);
  CHECK_EQ(alarm_minutes_until(&a, MON, 6, 0), 60);
  CHECK_EQ(alarm_minutes_until(&a, MON, 7, 0), 1440);         /* now: Tuesday's */
  CHECK_EQ(alarm_minutes_until(&a, FRI, 8, 0), 3 * 1440 - 60); /* Fri 08 -> Mon 07 */
  CHECK_EQ(alarm_minutes_until(&a, SAT, 7, 0), 2 * 1440);
  alarm_parse("on 07:00 S------", &a);
  CHECK_EQ(alarm_minutes_until(&a, SUN, 7, 0), 7 * 1440);     /* now: next week's */
  alarm_parse("on 07:00 once", &a);
  CHECK_EQ(alarm_minutes_until(&a, WED, 8, 0), 1440 - 60);    /* tomorrow */
  a.on = 0;
  CHECK_EQ(alarm_minutes_until(&a, WED, 8, 0), -1);
  alarm_parse("on 07:00 -------", &a);                         /* no days: never */
  CHECK_EQ(alarm_minutes_until(&a, WED, 8, 0), -1);
}

void test_alarm_says_its_days_in_words(void) {
  Alarm a;
  char s[32];
  alarm_parse("on 07:00 SMTWTFS", &a);  alarm_days_text(&a, s, sizeof s);
  CHECK(!strcmp(s, "every day"));
  alarm_parse("on 07:00 -MTWTF-", &a);  alarm_days_text(&a, s, sizeof s);
  CHECK(!strcmp(s, "weekdays"));
  alarm_parse("on 07:00 S-----S", &a);  alarm_days_text(&a, s, sizeof s);
  CHECK(!strcmp(s, "weekends"));
  alarm_parse("on 07:00 -M-W-F-", &a);  alarm_days_text(&a, s, sizeof s);
  CHECK(!strcmp(s, "Mon Wed Fri"));
  alarm_parse("on 07:00 once", &a);     alarm_days_text(&a, s, sizeof s);
  CHECK(!strcmp(s, "once"));
}

void test_alarm_reads_a_time_as_people_say_it(void) {
  int h, m;
  CHECK(alarm_parse_time("7", &h, &m) && h == 7 && m == 0);
  CHECK(alarm_parse_time("7:30", &h, &m) && h == 7 && m == 30);
  CHECK(alarm_parse_time("07:30", &h, &m) && h == 7 && m == 30);
  CHECK(alarm_parse_time("7am", &h, &m) && h == 7 && m == 0);
  CHECK(alarm_parse_time("7:15pm", &h, &m) && h == 19 && m == 15);
  CHECK(alarm_parse_time("12am", &h, &m) && h == 0);
  CHECK(alarm_parse_time("12pm", &h, &m) && h == 12);
  CHECK(alarm_parse_time("19:00", &h, &m) && h == 19);
  CHECK(alarm_parse_time("7.30", &h, &m) && h == 7 && m == 30);   /* a recogniser's colon */
  CHECK(!alarm_parse_time("25:00", &h, &m));
  CHECK(!alarm_parse_time("13pm", &h, &m));
  CHECK(!alarm_parse_time("soon", &h, &m));
  CHECK(!alarm_parse_time("", &h, &m));
}
