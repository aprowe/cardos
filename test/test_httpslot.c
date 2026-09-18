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
