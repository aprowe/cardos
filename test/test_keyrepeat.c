/* Which keys repeat and on what schedule; shared by both keyboards. */
#include "tinytest.h"
#include "kernel/input/keyrepeat.h"
#include "kernel/input/kbd_hid.h"

void test_keyrepeat_repeats_text_and_movement_only(void) {
  CHECK(keyrepeat_wanted('a'));
  CHECK(keyrepeat_wanted(' '));
  CHECK(keyrepeat_wanted('9'));
  CHECK(keyrepeat_wanted(0x08));          /* backspace */
  CHECK(keyrepeat_wanted(0x7F));          /* delete */
  CHECK(keyrepeat_wanted(0x09));          /* tab */
  CHECK(keyrepeat_wanted(0x80) && keyrepeat_wanted(0x83));   /* arrows */
  CHECK(!keyrepeat_wanted(0x0D));         /* enter */
  CHECK(!keyrepeat_wanted(0x1B));         /* escape */
  CHECK(!keyrepeat_wanted(0x84));         /* fn-`: leave the app */
  CHECK(!keyrepeat_wanted(0xA3));         /* opt-3 */
  CHECK(!keyrepeat_wanted(0xC0));         /* opt-a */
  CHECK(!keyrepeat_wanted(0xE1));         /* fn-b */
  CHECK(!keyrepeat_wanted(0xEF));         /* fn-p */
  CHECK(!keyrepeat_wanted(0x01));         /* ctrl-a */
  CHECK(!keyrepeat_wanted(0));
}

void test_keyrepeat_waits_then_fires_on_the_rate(void) {
  KeyRepeat r;
  keyrepeat_clear(&r);
  keyrepeat_press(&r, 'x', 3, 1, 1000);
  CHECK_EQ(keyrepeat_due(&r, 1, 1000), 0);
  CHECK_EQ(keyrepeat_due(&r, 1, 1000 + KEYREPEAT_DELAY_MS - 1), 0);
  CHECK_EQ(keyrepeat_due(&r, 1, 1000 + KEYREPEAT_DELAY_MS), 1);
  CHECK_EQ(keyrepeat_due(&r, 1, 1000 + KEYREPEAT_DELAY_MS + 1), 0);
  CHECK_EQ(keyrepeat_due(&r, 1, 1000 + KEYREPEAT_DELAY_MS + KEYREPEAT_RATE_MS), 1);
  CHECK_EQ(r.x, 3); CHECK_EQ(r.y, 1);
}

void test_keyrepeat_stops_when_the_key_lifts(void) {
  KeyRepeat r;
  keyrepeat_clear(&r);
  keyrepeat_press(&r, 'x', 3, 1, 0);
  CHECK_EQ(keyrepeat_due(&r, 0, KEYREPEAT_DELAY_MS), 0);
  CHECK_EQ(r.active, 0);
  CHECK_EQ(keyrepeat_due(&r, 1, KEYREPEAT_DELAY_MS * 2), 0);   /* stays off */
}

void test_keyrepeat_ignores_keys_that_do_not_repeat(void) {
  KeyRepeat r;
  keyrepeat_clear(&r);
  keyrepeat_press(&r, 'x', 3, 1, 0);
  keyrepeat_press(&r, 0x0D, 5, 2, 10);        /* enter: cancels x, does not arm */
  CHECK_EQ(r.active, 0);
  CHECK_EQ(keyrepeat_due(&r, 1, KEYREPEAT_DELAY_MS + 10), 0);
}

void test_keyrepeat_newest_press_wins(void) {
  KeyRepeat r;
  keyrepeat_clear(&r);
  keyrepeat_press(&r, 'x', 3, 1, 0);
  keyrepeat_press(&r, 'y', 4, 1, 100);
  CHECK_EQ(keyrepeat_due(&r, 1, KEYREPEAT_DELAY_MS), 0);    /* x's time; y's not yet */
  CHECK_EQ(keyrepeat_due(&r, 1, KEYREPEAT_DELAY_MS + 100), 1);
  CHECK_EQ(r.x, 4);
}

void test_keyrepeat_survives_the_millisecond_counter_wrapping(void) {
  KeyRepeat r;
  keyrepeat_clear(&r);
  keyrepeat_press(&r, 'x', 0, 0, 0xFFFFFFF0u);
  CHECK_EQ(keyrepeat_due(&r, 1, 0xFFFFFFF0u + 10), 0);
  CHECK_EQ(keyrepeat_due(&r, 1, KEYREPEAT_DELAY_MS - 16), 1);   /* wrapped */
}

void test_bluetooth_repeat_uses_the_shared_policy(void) {
  KbdHid k;
  uint8_t out[4];
  uint8_t report_w[8] = { KBD_MOD_LGUI, 0, 0x1A, 0, 0, 0, 0, 0 };   /* fn-w */
  uint8_t report_a[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };              /* a */
  uint8_t release[8]  = { 0, 0, 0, 0, 0, 0, 0, 0 };
  kbd_hid_init(&k);
  CHECK_EQ(kbd_hid_decode(&k, report_w, 8, 0, out, 4), 1);
  CHECK_EQ(kbd_hid_repeat(&k, KEYREPEAT_DELAY_MS + 1, out, 1), 0);   /* chords never repeat */
  kbd_hid_decode(&k, release, 8, 10, out, 4);
  CHECK_EQ(kbd_hid_decode(&k, report_a, 8, 20, out, 4), 1);
  CHECK_EQ(kbd_hid_repeat(&k, 20 + KEYREPEAT_DELAY_MS, out, 1), 1);
  CHECK_EQ(out[0], 'a');
}
