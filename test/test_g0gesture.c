/* The G0 button: hold to talk to the machine, tap then hold to record a memo.
 * One button, told apart by timing alone, so the timing is what is tested. */

#include <string.h>

#include "tinytest.h"
#include "kernel/sys/g0gesture.h"

void test_g0_a_plain_hold_is_voice(void) {
  G0Gesture g = {0};
  CHECK_EQ(g0_press(&g, 1000), G0_VOICE);
  CHECK_EQ(g0_release(&g, 1500, 2500), 0);        /* held: a sentence, kept */
  CHECK_EQ(g0_press(&g, 2700), G0_VOICE);         /* and arms nothing */
}

void test_g0_tap_then_press_is_a_memo(void) {
  G0Gesture g = {0};
  CHECK_EQ(g0_press(&g, 1000), G0_VOICE);         /* not known yet: a tap */
  CHECK_EQ(g0_release(&g, 120, 1120), 1);         /* ...it was: discard it */
  CHECK_EQ(g0_press(&g, 1400), G0_MEMO);          /* 280 ms later: memo */
}

void test_g0_a_slow_second_press_is_voice_again(void) {
  G0Gesture g = {0};
  g0_press(&g, 1000);
  g0_release(&g, 120, 1120);
  CHECK_EQ(g0_press(&g, 1120 + G0_GAP_MS + 1), G0_VOICE);
}

void test_g0_the_memo_uses_the_tap_up(void) {
  G0Gesture g = {0};
  g0_press(&g, 1000);
  g0_release(&g, 120, 1120);
  CHECK_EQ(g0_press(&g, 1400), G0_MEMO);
  /* After a memo, the next press is an ordinary one. */
  CHECK_EQ(g0_press(&g, 1500), G0_VOICE);
}

void test_g0_a_press_just_over_a_tap_is_a_sentence(void) {
  G0Gesture g = {0};
  g0_press(&g, 1000);
  CHECK_EQ(g0_release(&g, G0_TAP_MS, 1000 + G0_TAP_MS), 0);
  CHECK_EQ(g0_press(&g, 1000 + G0_TAP_MS + 100), G0_VOICE);
}

void test_g0_survives_the_millisecond_counter_wrapping(void) {
  G0Gesture g = {0};
  g0_press(&g, 0xFFFFFF00u);
  CHECK_EQ(g0_release(&g, 100, 0xFFFFFF64u), 1);
  CHECK_EQ(g0_press(&g, 0x00000100u), G0_MEMO);   /* 412 ms later, across 0 */
}

void test_g0_memo_names_sort_by_when(void) {
  char name[48];
  memo_filename(name, sizeof name, "/home/memos", 1, 9, 23, 15, 2, 7, 0);
  CHECK(strcmp(name, "/home/memos/0923-150207.wav") == 0);
  memo_filename(name, sizeof name, "/home/memos", 0, 0, 0, 0, 0, 0, 12);
  CHECK(strcmp(name, "/home/memos/m0012.wav") == 0);
}
