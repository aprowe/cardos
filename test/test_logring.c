/* The app log's line format and rotation policy. See kernel/sys/logring.h. */
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/logring.h"

void test_logring_a_line_is_stamped_tagged_and_terminated(void) {
  char buf[64];
  int n = logring_line(buf, sizeof buf, 12345, "todo", "syncing");
  CHECK(!strcmp(buf, "[   12.345] todo: syncing\n"));
  CHECK_EQ(n, (int)strlen(buf));
}

void test_logring_the_stamp_keeps_its_column(void) {
  char a[64], b[64];
  logring_line(a, sizeof a, 0, "x", "zero");
  logring_line(b, sizeof b, 999999, "x", "later");
  CHECK(!strcmp(a, "[    0.000] x: zero\n"));
  CHECK(!strcmp(b, "[  999.999] x: later\n"));
  /* The tag starts at the same offset in both, which is the point. */
  CHECK_EQ((int)(strstr(a, "x:") - a), (int)(strstr(b, "x:") - b));
}

/* A line that does not end in a newline swallows the next one. */
void test_logring_an_overlong_message_is_cut_but_still_ends_the_line(void) {
  char buf[24];
  int n = logring_line(buf, sizeof buf, 0, "t",
                       "a message far longer than the buffer allows");
  CHECK_EQ(n, (int)strlen(buf));
  CHECK_EQ(buf[n - 1], '\n');
  CHECK(n < (int)sizeof buf);
}

/* Two entries must not read as one. */
void test_logring_newlines_in_a_message_become_spaces(void) {
  char buf[64];
  logring_line(buf, sizeof buf, 0, "t", "first\nsecond");
  CHECK(!strcmp(buf, "[    0.000] t: first second\n"));
  CHECK_EQ((int)(strchr(buf, '\n') - buf), (int)strlen(buf) - 1);
}

void test_logring_no_tag_is_allowed(void) {
  char buf[64];
  logring_line(buf, sizeof buf, 1000, 0, "bare");
  CHECK(!strcmp(buf, "[    1.000] bare\n"));
}

void test_logring_rotation_happens_before_the_cap_is_passed(void) {
  CHECK_EQ(logring_rotate_needed(900, 100, 1000), 0);   /* exactly full: fine */
  CHECK_EQ(logring_rotate_needed(901, 100, 1000), 1);
  CHECK_EQ(logring_rotate_needed(0, 0, 1000), 0);
  /* A line bigger than the whole cap still rotates, and is then written. */
  CHECK_EQ(logring_rotate_needed(0, 2000, 1000), 1);
}
