/* The HTTP queue's one slot, and the sequence that wedged it.
 *
 * Todo starts its sync on the first tick after it opens. Leave it within
 * the two seconds a TLS round trip takes and the reply lands after the app
 * is gone. It sat there as DONE, nobody collected it, and every http_start
 * from then on -- Calendar's, Todo's next time, the Claude terminal's --
 * came back "busy" until the device was rebooted. Neither app said so: a
 * start that fails returns before the status line is touched. */
#include "tinytest.h"
#include "kernel/net/httpslot.h"

#include <stdlib.h>
#include <string.h>

static int todo, calendar;                  /* owners are just addresses */

void test_httpslot_a_reply_left_by_a_closed_app_wedged_everyone(void) {
  HttpSlot s;
  httpslot_init(&s);
  CHECK_EQ(httpslot_claim(&s, &todo), 0);
  CHECK_EQ(httpslot_finish(&s), 0);          /* DONE, waiting for Todo */
  CHECK_EQ(httpslot_claim(&s, &calendar), -1); /* the wedge, as it was */

  /* Now the app is unloaded and says so. */
  CHECK_EQ(httpslot_abandon(&s, &todo), 1);  /* free the reply now */
  CHECK_EQ(s.state, HTTPSLOT_IDLE);
  CHECK_EQ(httpslot_claim(&s, &calendar), 0);
}

void test_httpslot_leaving_mid_request_drops_the_reply_when_it_lands(void) {
  HttpSlot s;
  httpslot_init(&s);
  CHECK_EQ(httpslot_claim(&s, &todo), 0);
  CHECK_EQ(httpslot_abandon(&s, &todo), 0);  /* the worker still has it */
  CHECK_EQ(s.state, HTTPSLOT_RUNNING);
  CHECK_EQ(httpslot_claim(&s, &calendar), -1); /* one TLS session at a time */
  CHECK_EQ(httpslot_finish(&s), 1);          /* nobody is coming: drop it */
  CHECK_EQ(s.state, HTTPSLOT_IDLE);
  CHECK_EQ(httpslot_claim(&s, &calendar), 0);
}

void test_httpslot_someone_elses_request_is_left_alone(void) {
  HttpSlot s;
  httpslot_init(&s);
  CHECK_EQ(httpslot_claim(&s, &todo), 0);
  CHECK_EQ(httpslot_abandon(&s, &calendar), 0);
  CHECK_EQ(s.abandoned, 0);
  CHECK_EQ(httpslot_finish(&s), 0);
  CHECK_EQ(httpslot_abandon(&s, &calendar), 0);
  CHECK_EQ(s.state, HTTPSLOT_DONE);          /* still Todo's to collect */
  httpslot_collect(&s);
  CHECK_EQ(s.state, HTTPSLOT_IDLE);
}

void test_httpslot_collecting_readies_the_next_request(void) {
  HttpSlot s;
  httpslot_init(&s);
  CHECK_EQ(httpslot_claim(&s, &todo), 0);
  CHECK_EQ(httpslot_finish(&s), 0);
  httpslot_collect(&s);
  CHECK_EQ(httpslot_claim(&s, &calendar), 0);
  CHECK(s.owner == &calendar);
}

void test_httpslot_an_abandoned_slot_does_not_haunt_the_next_owner(void) {
  HttpSlot s;
  httpslot_init(&s);
  CHECK_EQ(httpslot_claim(&s, &todo), 0);
  CHECK_EQ(httpslot_abandon(&s, &todo), 0);
  CHECK_EQ(httpslot_finish(&s), 1);
  CHECK_EQ(httpslot_claim(&s, &calendar), 0);
  CHECK_EQ(s.abandoned, 0);                  /* the flag went with Todo */
  CHECK_EQ(httpslot_finish(&s), 0);
}

/* ---- delivering a reply ----
 *
 * Stocks polls into 2600 bytes and writes S.buf[n] = 0. The poll used to
 * return the bytes received -- up to 8191 -- so a 7 KB reply wrote a NUL
 * 4.5 KB past the end of the app's data. */

void test_httpslot_a_long_reply_returns_what_fits(void) {
  char *reply = (char *)malloc(7001);
  char buf[2600 + 8];
  int n;
  memset(reply, 'x', 7000); reply[7000] = 0;
  memset(buf, '#', sizeof buf);
  n = httpslot_deliver(7000, reply, buf, 2600);
  CHECK_EQ(n, 2599);                         /* buf[n] = 0 is in bounds */
  CHECK_EQ(buf[2599], 0);
  CHECK_EQ(buf[2600], '#');                  /* nothing past the buffer */
  CHECK(n >= 0 && (size_t)n + 1 >= 2600);    /* the CAPP_HTTP_FILLED rule */
  free(reply);
}

void test_httpslot_a_short_reply_and_a_failure_pass_through(void) {
  char buf[64];
  CHECK_EQ(httpslot_deliver(5, "hello", buf, sizeof buf), 5);
  CHECK(strcmp(buf, "hello") == 0);
  /* an error keeps its code, and its document is still handed over */
  CHECK_EQ(httpslot_deliver(-403, "{\"error\":1}", buf, sizeof buf), -403);
  CHECK(strcmp(buf, "{\"error\":1}") == 0);
  CHECK_EQ(httpslot_deliver(-3, NULL, buf, sizeof buf), -3);
  CHECK_EQ(buf[0], 0);
}

void test_httpslot_no_buffer_keeps_the_result(void) {
  /* the file mode (agent.c) polls with nothing: the bytes went to the card */
  CHECK_EQ(httpslot_deliver(12345, NULL, NULL, 0), 12345);
  CHECK_EQ(httpslot_deliver(-1, "x", NULL, 0), -1);
}

void test_httpslot_a_reply_already_in_place_is_measured(void) {
  /* httpq_start_into: the reply was written straight into the owner's buffer */
  char buf[16];
  memcpy(buf, "0123456789abcdef", 16);       /* no NUL anywhere */
  CHECK_EQ(httpslot_deliver(16, buf, buf, sizeof buf), 15);
  CHECK_EQ(buf[15], 0);
}
