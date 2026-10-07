/* Notifications, portable part: the list keeps the newest sixteen, unread
 * counts and clears, a calendar line is due only inside its lead and only
 * once, and the server's poll reply is read whole. */
#include <string.h>

#include "tinytest.h"
#include "kernel/sys/notifyq.h"

void test_notifyq_list_keeps_the_newest(void) {
  Nq q;
  int i;
  char t[8];
  nq_init(&q);
  for (i = 0; i < NQ_MAX + 3; i++) {
    t[0] = (char)('a' + i); t[1] = 0;
    nq_push(&q, "Chat", t, "hi", (uint32_t)i);
  }
  CHECK_EQ(q.n, NQ_MAX);
  CHECK(!strcmp(q.it[0].title, "s"));                    /* newest first */
  CHECK_EQ(nq_unread(&q), NQ_MAX);
  nq_push(&q, "Calendar", "Dentist", "in 10 min", 99);
  nq_read_app(&q, "Chat");
  CHECK_EQ(nq_unread(&q), 1);
  nq_remove(&q, 0);
  CHECK_EQ(nq_unread(&q), 0);
  CHECK_EQ(q.n, NQ_MAX - 1);
  nq_push(&q, "A very long app name indeed", "x", "y", 1);
  CHECK_EQ((int)strlen(q.it[0].app), NQ_APP - 1);       /* cut, not overrun */
}

void test_notifyq_calendar_is_due_inside_its_lead(void) {
  uint32_t start;
  char s[32];
  const char *line = "0 0 0 1000600 1004200 abc123 Dentist appointment";
  CHECK_EQ(nq_cal_due(line, 1000000, 600, &start, s, sizeof s), 1);     /* exactly 10 min */
  CHECK_EQ((int)start, 1000600);
  CHECK(!strcmp(s, "Dentist appointment"));
  CHECK_EQ(nq_cal_due(line, 999000, 600, &start, s, sizeof s), 0);      /* too early */
  CHECK_EQ(nq_cal_due(line, 1000600, 600, &start, s, sizeof s), 0);     /* started */
  CHECK_EQ(nq_cal_due("1 0 0 1000600 1004200 x Holiday", 1000000, 600, &start, s, sizeof s), 0);
  CHECK_EQ(nq_cal_due("0 0 1 1000600 1004200 x Gone", 1000000, 600, &start, s, sizeof s), 0);
  CHECK_EQ(nq_cal_due("garbage", 1000000, 600, &start, s, sizeof s), 0);
}

void test_notifyq_an_event_fires_once(void) {
  NqFired f;
  int i;
  memset(&f, 0, sizeof f);
  CHECK_EQ(nq_fired_new(&f, nq_key(100, "a")), 1);
  CHECK_EQ(nq_fired_new(&f, nq_key(100, "a")), 0);
  CHECK_EQ(nq_fired_new(&f, nq_key(100, "b")), 1);
  for (i = 0; i < NQ_FIRED; i++) nq_fired_new(&f, (uint32_t)(5000 + i));
  CHECK_EQ(nq_fired_new(&f, nq_key(100, "a")), 1);       /* long forgotten */
}

static char names[3][16], texts[3][32];
static int got;
static void each(void *ctx, const char *name, size_t nl, const char *text, size_t tl) {
  (void)ctx;
  if (got >= 3) return;
  memcpy(names[got], name, nl); names[got][nl] = 0;
  memcpy(texts[got], text, tl); texts[got][tl] = 0;
  got++;
}

void test_notifyq_reads_the_poll_reply(void) {
  got = 0;
  CHECK_EQ(nq_parse_poll("ok 42\nchat\tbritney\thi there\nchat\tAlex\tyo\n", each, 0), 42);
  CHECK_EQ(got, 2);
  CHECK(!strcmp(names[0], "britney"));
  CHECK(!strcmp(texts[0], "hi there"));
  CHECK(!strcmp(texts[1], "yo"));
  got = 0;
  CHECK_EQ(nq_parse_poll("ok 7\n", each, 0), 7);
  CHECK_EQ(got, 0);
  CHECK_EQ(nq_parse_poll("error signed out\n", each, 0), -1);
  CHECK_EQ(nq_parse_poll("ok \n", each, 0), -1);
}
