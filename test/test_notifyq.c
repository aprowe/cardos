/* Notifications, portable part: the list keeps the newest sixteen, unread
 * counts and clears, and the server's poll reply is read whole. */
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

static char names[3][16], texts[3][32];
static int got;
static void each(void *ctx, const char *name, size_t nl, const char *text, size_t tl) {
  (void)ctx;
  if (got >= 3) return;
  memcpy(names[got], name, nl); names[got][nl] = 0;
  memcpy(texts[got], text, tl); texts[got][tl] = 0;
  got++;
}

static char napp[16], ntitle[16], ntext[32];
static int ngot;
static void each_note(void *ctx, NqStr app, NqStr title, NqStr text) {
  (void)ctx;
  memcpy(napp, app.s, app.n); napp[app.n] = 0;
  memcpy(ntitle, title.s, title.n); ntitle[title.n] = 0;
  memcpy(ntext, text.s, text.n); ntext[text.n] = 0;
  ngot++;
}

void test_notifyq_reads_notes(void) {
  int nl = 99;
  got = ngot = 0;
  CHECK_EQ(nq_parse_poll("ok 3 12\nchat\tb\thi\nnote\tBuild\tdone\tmade timer.c\n",
                         each, each_note, &nl, 0), 3);
  CHECK_EQ(nl, 12);
  CHECK_EQ(got, 1);
  CHECK_EQ(ngot, 1);
  CHECK(!strcmp(napp, "Build"));
  CHECK(!strcmp(ntitle, "done"));
  CHECK(!strcmp(ntext, "made timer.c"));
  /* an older server: one number, so no notes and no note id */
  CHECK_EQ(nq_parse_poll("ok 3\n", each, each_note, &nl, 0), 3);
  CHECK_EQ(nl, -1);
  /* a note with no text still is one */
  ngot = 0;
  nq_parse_poll("ok 1 2\nnote\tBuild\tdone", 0, each_note, &nl, 0);
  CHECK_EQ(ngot, 1);
  CHECK(!strcmp(ntext, ""));
}

void test_notifyq_reads_the_poll_reply(void) {
  got = 0;
  CHECK_EQ(nq_parse_poll("ok 42\nchat\tbritney\thi there\nchat\tAlex\tyo\n", each, 0, 0, 0), 42);
  CHECK_EQ(got, 2);
  CHECK(!strcmp(names[0], "britney"));
  CHECK(!strcmp(texts[0], "hi there"));
  CHECK(!strcmp(texts[1], "yo"));
  got = 0;
  CHECK_EQ(nq_parse_poll("ok 7\n", each, 0, 0, 0), 7);
  CHECK_EQ(got, 0);
  CHECK_EQ(nq_parse_poll("error signed out\n", each, 0, 0, 0), -1);
  CHECK_EQ(nq_parse_poll("ok \n", each, 0, 0, 0), -1);
}
