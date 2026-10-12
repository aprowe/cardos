/* The launcher's search: kernel/ui/appsearch.c. */
#include <stdio.h>
#include <string.h>

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

/* With nothing typed: what was opened lately first, newest first, then the
 * rest in the launcher's order. */
void test_appsearch_recent_apps_lead_an_empty_search(void) {
  Recents r;
  int out[8], n;
  memset(&r, 0, sizeof r);
  recents_opened(&r, "Notes");
  recents_opened(&r, "Toggl");
  recents_opened(&r, "notes");                    /* again: to the front, once */
  CHECK_EQ(r.n, 2);
  CHECK(!strcmp(r.running, "notes"));
  n = appsearch_rank_recent(APPS, NAPPS, "", &r, out, 8);
  CHECK_EQ(n, NAPPS);
  CHECK_EQ(out[0], 2);                            /* Notes */
  CHECK_EQ(out[1], 4);                            /* Toggl */
  CHECK_EQ(out[2], 0);                            /* then Todo, Calendar, ... */
  CHECK_EQ(out[3], 1);
  /* Typing ranks by the name again. */
  n = appsearch_rank_recent(APPS, NAPPS, "to", &r, out, 8);
  CHECK_EQ(out[0], 0);
}

void test_appsearch_recents_keep_eight_and_survive_the_file(void) {
  Recents r, back;
  char text[400];
  int i;
  memset(&r, 0, sizeof r);
  for (i = 0; i < 10; i++) { char nm[8]; snprintf(nm, sizeof nm, "App%d", i); recents_opened(&r, nm); }
  CHECK_EQ(r.n, RECENT_MAX);
  CHECK(!strcmp(r.name[0], "App9") && !strcmp(r.name[7], "App2"));
  recents_format(&r, text, sizeof text);
  CHECK(!strncmp(text, "@ App9\nApp9\nApp8\n", 17));
  recents_parse(&back, text);
  CHECK_EQ(back.n, RECENT_MAX);
  CHECK(!strcmp(back.running, "App9") && !strcmp(back.name[7], "App2"));
  recents_closed(&back);
  recents_format(&back, text, sizeof text);
  CHECK(!strncmp(text, "App9\n", 5));
  recents_parse(&back, text);
  CHECK(!back.running[0]);
}
