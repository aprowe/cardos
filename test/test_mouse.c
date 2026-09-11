#include "tinytest.h"
#include "input/mouse.h"
#include <string.h>

static void setup(void) { mouse_init(240, 135); }

/* A HID boot-protocol mouse report: buttons bitmap, then signed 8-bit dx and
 * dy, then an optional wheel. The deltas being *signed* is the whole trick --
 * treating them as unsigned gives a cursor that only ever moves down and
 * right, which is the classic version of this bug. */

void test_decode_a_three_byte_report(void) {
  const uint8_t r[3] = { 0x01, 5, 7 };
  MouseReport m;
  CHECK_EQ(mouse_decode_boot(r, sizeof r, &m), 0);
  CHECK_EQ(m.buttons, MOUSE_LEFT);
  CHECK_EQ(m.dx, 5);
  CHECK_EQ(m.dy, 7);
  CHECK_EQ(m.wheel, 0);
}

void test_decode_a_four_byte_report_with_wheel(void) {
  const uint8_t r[4] = { 0x02, 0, 0, 0xFF };   /* wheel -1 */
  MouseReport m;
  CHECK_EQ(mouse_decode_boot(r, sizeof r, &m), 0);
  CHECK_EQ(m.buttons, MOUSE_RIGHT);
  CHECK_EQ(m.wheel, -1);
}

void test_negative_deltas_are_signed(void) {
  const uint8_t r[3] = { 0x00, 0xFF, 0xF6 };   /* -1, -10 */
  MouseReport m;
  CHECK_EQ(mouse_decode_boot(r, sizeof r, &m), 0);
  CHECK_EQ(m.dx, -1);
  CHECK_EQ(m.dy, -10);
}

void test_all_three_buttons_decode(void) {
  const uint8_t r[3] = { 0x07, 0, 0 };
  MouseReport m;
  CHECK_EQ(mouse_decode_boot(r, sizeof r, &m), 0);
  CHECK_EQ(m.buttons & MOUSE_LEFT, MOUSE_LEFT);
  CHECK_EQ(m.buttons & MOUSE_RIGHT, MOUSE_RIGHT);
  CHECK_EQ(m.buttons & MOUSE_MIDDLE, MOUSE_MIDDLE);
}

void test_a_short_report_is_refused(void) {
  const uint8_t r[2] = { 0x00, 0 };
  MouseReport m;
  CHECK(mouse_decode_boot(r, sizeof r, &m) != 0);
  CHECK(mouse_decode_boot(r, 0, &m) != 0);
}

/* Some devices prefix a report ID. A 5-byte report is ID + the usual four. */
void test_a_report_id_prefix_is_skipped(void) {
  const uint8_t r[5] = { 0x01, 0x04, 3, 0xFD, 1 };  /* id, middle, +3, -3, +1 */
  MouseReport m;
  CHECK_EQ(mouse_decode_boot(r, sizeof r, &m), 0);
  CHECK_EQ(m.buttons, MOUSE_MIDDLE);
  CHECK_EQ(m.dx, 3);
  CHECK_EQ(m.dy, -3);
  CHECK_EQ(m.wheel, 1);
}

/* ---- cursor tracking ---------------------------------------------------- */

void test_the_cursor_starts_centred(void) {
  setup();
  CHECK_EQ(mouse_x(), 120);
  CHECK_EQ(mouse_y(), 67);
}

void test_deltas_accumulate(void) {
  MouseReport m = { 0, 10, 5, 0 };
  setup();
  mouse_apply(&m);
  CHECK_EQ(mouse_x(), 130);
  CHECK_EQ(mouse_y(), 72);
  mouse_apply(&m);
  CHECK_EQ(mouse_x(), 140);
  CHECK_EQ(mouse_y(), 77);
}

void test_the_cursor_cannot_leave_the_screen(void) {
  MouseReport left = { 0, -100, -100, 0 };
  MouseReport right = { 0, 100, 100, 0 };
  int i;
  setup();
  for (i = 0; i < 10; i++) mouse_apply(&left);
  CHECK_EQ(mouse_x(), 0);
  CHECK_EQ(mouse_y(), 0);
  for (i = 0; i < 10; i++) mouse_apply(&right);
  CHECK_EQ(mouse_x(), 239);      /* not 240: the last addressable pixel */
  CHECK_EQ(mouse_y(), 134);
}

/* Clamping must not make the cursor sticky: after running into an edge it has
 * to move again on the very next report, not sit there working off a debt. */
void test_the_cursor_is_not_sticky_at_an_edge(void) {
  MouseReport left = { 0, -100, 0, 0 };
  MouseReport right = { 0, 1, 0, 0 };
  setup();
  mouse_apply(&left);
  mouse_apply(&left);
  CHECK_EQ(mouse_x(), 0);
  mouse_apply(&right);
  CHECK_EQ(mouse_x(), 1);
}

void test_button_presses_are_edges_not_levels(void) {
  MouseReport down = { MOUSE_LEFT, 0, 0, 0 };
  MouseReport up = { 0, 0, 0, 0 };
  setup();

  mouse_apply(&down);
  CHECK_EQ(mouse_pressed(MOUSE_LEFT), 1);
  CHECK_EQ(mouse_pressed(MOUSE_LEFT), 0);   /* consumed: it is an event */

  mouse_apply(&down);                       /* still held, not a new press */
  CHECK_EQ(mouse_pressed(MOUSE_LEFT), 0);
  CHECK_EQ(mouse_down(MOUSE_LEFT), 1);      /* but it is still down */

  mouse_apply(&up);
  CHECK_EQ(mouse_released(MOUSE_LEFT), 1);
  CHECK_EQ(mouse_released(MOUSE_LEFT), 0);
  CHECK_EQ(mouse_down(MOUSE_LEFT), 0);
}

void test_buttons_are_tracked_independently(void) {
  MouseReport l = { MOUSE_LEFT, 0, 0, 0 };
  MouseReport lr = { MOUSE_LEFT | MOUSE_RIGHT, 0, 0, 0 };
  setup();
  mouse_apply(&l);
  CHECK_EQ(mouse_pressed(MOUSE_LEFT), 1);
  mouse_apply(&lr);
  CHECK_EQ(mouse_pressed(MOUSE_RIGHT), 1);
  CHECK_EQ(mouse_pressed(MOUSE_LEFT), 0);   /* was already down */
  CHECK_EQ(mouse_down(MOUSE_LEFT), 1);
}

void test_wheel_accumulates_and_is_consumed(void) {
  MouseReport up = { 0, 0, 0, 3 };
  setup();
  mouse_apply(&up);
  mouse_apply(&up);
  CHECK_EQ(mouse_take_wheel(), 6);
  CHECK_EQ(mouse_take_wheel(), 0);
}

void test_a_moved_flag_tells_the_compositor_to_repaint(void) {
  MouseReport still = { 0, 0, 0, 0 };
  MouseReport move = { 0, 1, 0, 0 };
  setup();
  CHECK_EQ(mouse_take_moved(), 0);
  mouse_apply(&still);
  CHECK_EQ(mouse_take_moved(), 0);   /* no move: no damage rectangle */
  mouse_apply(&move);
  CHECK_EQ(mouse_take_moved(), 1);
  CHECK_EQ(mouse_take_moved(), 0);
}

/* Clamping at an edge is not movement, so it must not force a repaint. */
void test_a_clamped_report_does_not_report_movement(void) {
  MouseReport left = { 0, -100, 0, 0 };
  setup();
  mouse_apply(&left);                /* 120 -> 20 */
  mouse_apply(&left);                /* 20 -> 0, still a real move */
  CHECK_EQ(mouse_x(), 0);
  mouse_take_moved();
  mouse_apply(&left);                /* pinned at 0: nothing changed */
  CHECK_EQ(mouse_take_moved(), 0);
}
