/* The server's /tz answer, read on the device. What goes into env TZ is
 * applied by the C library to every clock on the machine, so anything that
 * is not plainly a POSIX rule is refused rather than trusted. */

#include <string.h>

#include "tinytest.h"
#include "kernel/sys/tzreply.h"

void test_tzreply_reads_the_rule_and_the_zone(void) {
  char rule[64], zone[48];
  CHECK_EQ(tzreply_parse("tz PST8PDT,M3.2.0,M11.1.0\nzone America/Los_Angeles\n",
                         rule, sizeof rule, zone, sizeof zone), 0);
  CHECK(strcmp(rule, "PST8PDT,M3.2.0,M11.1.0") == 0);
  CHECK(strcmp(zone, "America/Los_Angeles") == 0);
}

void test_tzreply_rules_with_offsets_and_brackets(void) {
  char rule[64], zone[48];
  CHECK_EQ(tzreply_parse("tz GMT0BST,M3.5.0/1,M10.5.0\nzone Europe/London\n",
                         rule, sizeof rule, zone, sizeof zone), 0);
  CHECK(strcmp(rule, "GMT0BST,M3.5.0/1,M10.5.0") == 0);
  CHECK_EQ(tzreply_parse("tz <+0530>-5:30\nzone Asia/Kolkata\n",
                         rule, sizeof rule, zone, sizeof zone), 0);
  CHECK(strcmp(rule, "<+0530>-5:30") == 0);
}

void test_tzreply_the_zone_line_is_optional(void) {
  char rule[64], zone[48];
  CHECK_EQ(tzreply_parse("tz JST-9\n", rule, sizeof rule, zone, sizeof zone), 0);
  CHECK(strcmp(rule, "JST-9") == 0);
  CHECK(zone[0] == 0);
}

void test_tzreply_refuses_what_is_not_a_rule(void) {
  char rule[64], zone[48];
  CHECK(tzreply_parse("error could not tell where this server is\n",
                      rule, sizeof rule, zone, sizeof zone) != 0);
  CHECK(tzreply_parse("tz \n", rule, sizeof rule, zone, sizeof zone) != 0);
  CHECK(tzreply_parse("tz America/Los_Angeles\n",          /* an IANA name */
                      rule, sizeof rule, zone, sizeof zone) != 0);
  CHECK(tzreply_parse("tz PST8PDT; rm -rf\n", rule, sizeof rule, zone, sizeof zone) != 0);
  CHECK(tzreply_parse("<html>502 Bad Gateway</html>", rule, sizeof rule,
                      zone, sizeof zone) != 0);
  CHECK(tzreply_parse(0, rule, sizeof rule, zone, sizeof zone) != 0);
}

void test_tzreply_a_rule_too_long_for_the_buffer_is_refused_not_cut(void) {
  char rule[8], zone[48];
  /* Cut short, a rule is a different rule: refused instead. */
  CHECK(tzreply_parse("tz PST8PDT,M3.2.0,M11.1.0\n", rule, sizeof rule,
                      zone, sizeof zone) != 0);
}
