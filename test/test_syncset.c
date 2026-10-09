/* apps/syncset.h: when a sync may delete what the server's list leaves out. */

#include <string.h>

#include "tinytest.h"
#include "apps/syncset.h"

void test_syncset_a_reply_with_room_left_is_whole(void) {
  char buf[32] = "a\t1\nb\t2\n";
  CHECK_EQ(sync_reply(buf, 8, (int)sizeof buf), 1);
  CHECK(!strcmp(buf, "a\t1\nb\t2\n"));
}

void test_syncset_a_reply_that_filled_the_buffer_drops_its_cut_line(void) {
  char buf[12];
  memcpy(buf, "a\t1\nb\t2\nccc", 11);       /* "ccc..." was cut at the end */
  buf[11] = 0;
  CHECK_EQ(sync_reply(buf, 11, (int)sizeof buf), 0);
  CHECK(!strcmp(buf, "a\t1\nb\t2\n"));
}

void test_syncset_a_full_buffer_ending_on_a_line_is_still_partial(void) {
  char buf[9];
  memcpy(buf, "a\t1\nb\t2\n", 8);
  buf[8] = 0;
  /* Exactly full: there may have been more behind it. */
  CHECK_EQ(sync_reply(buf, 8, (int)sizeof buf), 0);
  CHECK(!strcmp(buf, "a\t1\nb\t2\n"));
}

void test_syncset_a_failed_request_is_an_empty_partial_list(void) {
  char buf[8] = "junk";
  CHECK_EQ(sync_reply(buf, -3, (int)sizeof buf), 0);
  CHECK_EQ(buf[0], 0);
}

void test_syncset_deletes_only_against_a_whole_list_under_its_maximum(void) {
  CHECK_EQ(sync_may_delete(1, 3, 64), 1);
  CHECK_EQ(sync_may_delete(1, 0, 64), 1);    /* the server has nothing: delete it all */
  CHECK_EQ(sync_may_delete(1, 64, 64), 0);   /* ran to the maximum */
  CHECK_EQ(sync_may_delete(0, 3, 64), 0);    /* cut short */
}

void test_syncset_finds_an_id_in_a_strided_list(void) {
  char ids[3][12] = { "t1", "t22", "t3" };
  CHECK(sync_listed("t22", &ids[0][0], 3, 12));
  CHECK(sync_listed("t3", &ids[0][0], 3, 12));
  CHECK(!sync_listed("t2", &ids[0][0], 3, 12));
  CHECK(!sync_listed("t222", &ids[0][0], 3, 12));
  CHECK(!sync_listed("t1", &ids[0][0], 0, 12));
}
