/* The launcher's search: kernel/ui/appsearch.c. */
#include "tinytest.h"
#include "kernel/ui/appsearch.h"

static const char *const APPS[] = {
  "Todo", "Calendar", "Notes", "Dashboard Link", "Toggl", "Today", "Edit", "Memo",
};
#define NAPPS ((int)(sizeof APPS / sizeof APPS[0]))

void test_appsearch_the_start_of_the_name_wins(void) {
  int out[8], n = appsearch_rank(APPS, NAPPS, "to", out, 8);
  CHECK_EQ(n, 3);
  CHECK_EQ(out[0], 0);            /* Todo: the shortest */
  CHECK_EQ(out[1], 4);            /* Toggl and Today tie, and keep their order */
  CHECK_EQ(out[2], 5);
}

void test_appsearch_case_does_not_matter(void) {
  int out[8];
  CHECK_EQ(appsearch_rank(APPS, NAPPS, "NOTES", out, 8), 1);
  CHECK_EQ(out[0], 2);
}

void test_appsearch_a_word_then_anywhere_then_letters_in_order(void) {
  int out[8], n;
  CHECK(appsearch_score("Dashboard Link", "link") > appsearch_score("Calendar", "lend"));
  CHECK(appsearch_score("Calendar", "lend") > appsearch_score("Dashboard Link", "dl"));
  CHECK(appsearch_score("Dashboard Link", "dl") >= 0);
  CHECK_EQ(appsearch_score("Notes", "xyz"), -1);
  n = appsearch_rank(APPS, NAPPS, "dl", out, 8);
  CHECK_EQ(n, 1);
  CHECK_EQ(out[0], 3);
}

void test_appsearch_an_empty_query_keeps_the_launchers_order(void) {
  int out[4], n = appsearch_rank(APPS, NAPPS, "", out, 4);
  CHECK_EQ(n, 4);
  CHECK_EQ(out[0], 0);
  CHECK_EQ(out[3], 3);
}

void test_appsearch_more_matches_than_room_keeps_the_best(void) {
  int out[2], n = appsearch_rank(APPS, NAPPS, "o", out, 2);
  CHECK_EQ(n, 2);
  /* No name starts with o; every one with an o in it is "anywhere", and the
   * earliest o wins: Todo, Toggl and Notes have it second. */
  CHECK_EQ(out[0], 0);
  CHECK_EQ(out[1], 2);
}
