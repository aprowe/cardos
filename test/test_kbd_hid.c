#include "tinytest.h"
#include "kernel/input/kbd_hid.h"
#include "kernel/drv/keyboard.h"
#include <string.h>

/* A boot keyboard report says which keys are *held*, not which were pressed.
 * Turning that into a stream of characters is a diff against the last report,
 * and every interesting failure -- a key that repeats forever, a key that
 * types twice, six characters from leaning on the keyboard -- lives in that
 * diff rather than in the radio. */

static KbdHid k;

static void setup(void) { kbd_hid_init(&k); }

static int feed(uint8_t mods, uint8_t a, uint8_t b, uint32_t now, uint8_t *out) {
  uint8_t r[8];
  memset(r, 0, sizeof r);
  r[0] = mods;
  r[2] = a;
  r[3] = b;
  return kbd_hid_decode(&k, r, sizeof r, now, out, 8);
}

void test_a_pressed_key_yields_its_character(void) {
  uint8_t out[8];
  setup();
  CHECK_EQ(feed(0, 0x04, 0, 0, out), 1);      /* 'a' */
  CHECK_EQ(out[0], 'a');
}

void test_shift_gives_the_other_glyph_not_arithmetic(void) {
  uint8_t out[8];
  setup();
  CHECK_EQ(feed(KBD_MOD_LSHIFT, 0x04, 0, 0, out), 1);
  CHECK_EQ(out[0], 'A');

  /* shift-2 is '@' on this layout. Nothing about '2' produces '@' by
   * arithmetic, which is why there are two tables. */
  kbd_hid_init(&k);
  CHECK_EQ(feed(KBD_MOD_LSHIFT, 0x1F, 0, 0, out), 1);
  CHECK_EQ(out[0], '@');
}

void test_a_held_key_is_not_pressed_again(void) {
  uint8_t out[8];
  setup();
  CHECK_EQ(feed(0, 0x04, 0, 0, out), 1);
  CHECK_EQ(feed(0, 0x04, 0, 10, out), 0);     /* same report, still down */
  CHECK_EQ(feed(0, 0x04, 0, 20, out), 0);
}

void test_releasing_and_pressing_again_types_twice(void) {
  uint8_t out[8];
  setup();
  CHECK_EQ(feed(0, 0x04, 0, 0, out), 1);
  CHECK_EQ(feed(0, 0, 0, 10, out), 0);        /* released */
  CHECK_EQ(feed(0, 0x04, 0, 20, out), 1);
  CHECK_EQ(out[0], 'a');
}

void test_a_second_key_while_the_first_is_held(void) {
  uint8_t out[8];
  setup();
  CHECK_EQ(feed(0, 0x04, 0, 0, out), 1);
  CHECK_EQ(out[0], 'a');
  CHECK_EQ(feed(0, 0x04, 0x05, 10, out), 1);  /* 'a' still down, 'b' added */
  CHECK_EQ(out[0], 'b');
}

void test_two_keys_in_one_report_both_arrive(void) {
  uint8_t out[8];
  setup();
  CHECK_EQ(feed(0, 0x04, 0x05, 0, out), 2);
  CHECK_EQ(out[0], 'a');
  CHECK_EQ(out[1], 'b');
}

/* Every slot 0x01 is ErrorRollOver: more keys are down than the device can
 * report. Six 'a's from leaning on the keyboard is the bug this prevents. */
void test_rollover_yields_nothing(void) {
  uint8_t r[8], out[8];
  int i;
  setup();
  memset(r, 0, sizeof r);
  for (i = 2; i < 8; i++) r[i] = 0x01;
  CHECK_EQ(kbd_hid_decode(&k, r, sizeof r, 0, out, 8), 0);
}

void test_a_keyboard_report_id_prefix_is_skipped(void) {
  uint8_t r[9], out[8];
  setup();
  memset(r, 0, sizeof r);
  r[0] = 0x01;        /* the report ID */
  r[1] = 0;           /* modifiers */
  r[3] = 0x04;        /* first key slot */
  CHECK_EQ(kbd_hid_decode(&k, r, sizeof r, 0, out, 8), 1);
  CHECK_EQ(out[0], 'a');
}

void test_a_short_keyboard_report_is_refused(void) {
  uint8_t r[4] = { 0, 0, 0x04, 0 }, out[8];
  setup();
  CHECK_EQ(kbd_hid_decode(&k, r, sizeof r, 0, out, 8), -1);
}

void test_arrows_and_control_keys_map_to_the_kernels_bytes(void) {
  uint8_t out[8];
  CHECK_EQ(kbd_hid_translate(0x52, 0), 0x80);     /* up */
  CHECK_EQ(kbd_hid_translate(0x51, 0), 0x81);     /* down */
  CHECK_EQ(kbd_hid_translate(0x50, 0), 0x82);     /* left */
  CHECK_EQ(kbd_hid_translate(0x4F, 0), 0x83);     /* right */
  CHECK_EQ(kbd_hid_translate(0x28, 0), 0x0D);     /* enter */
  CHECK_EQ(kbd_hid_translate(0x29, 0), 0x1B);     /* escape */
  CHECK_EQ(kbd_hid_translate(0x2A, 0), 0x08);     /* backspace */
  setup();
  CHECK_EQ(feed(0, 0x52, 0, 0, out), 1);
  CHECK_EQ(out[0], 0x80);
}

/* ctrl-s has to be the same byte from either keyboard, or an editor that
 * saves on ctrl-s works on one and not the other. */
void test_ctrl_makes_the_same_control_characters_as_the_builtin_keyboard(void) {
  CHECK_EQ(kbd_hid_translate(0x16, KBD_MOD_LCTRL), 0x13);   /* ctrl-s */
  CHECK_EQ(kbd_hid_translate(0x1A, KBD_MOD_RCTRL), 0x17);   /* ctrl-w */
  CHECK_EQ(kbd_hid_translate(0x13, KBD_MOD_LCTRL), 0x10);   /* ctrl-p */
}

/* ctrl-h used to be special-cased into the help code, because ctrl-h and
 * Backspace are both 0x08 and only one of them could have it. Under the
 * modifier convention the tie stops existing: help is a window operation and
 * lives on fn, so ctrl-h is the app's like every other ctrl chord. */
void test_ctrl_h_is_the_apps_and_backspace_keeps_its_byte(void) {
  CHECK_EQ(kbd_hid_translate(0x0B, KBD_MOD_LCTRL), 0x08);   /* ctrl-h, the app's */
  CHECK_EQ(kbd_hid_translate(0x2A, 0), 0x08);               /* backspace */
  CHECK_EQ(kbd_hid_translate(0x0B, 0), 'h');                /* plain h */
}

/* The GUI key stands in for Fn, which a Bluetooth keyboard does not have and
 * whose Alt is already Opt. Window operations, with codes of their own so an
 * app taking text cannot swallow the chord that closes it. */
void test_the_gui_key_makes_the_window_chords(void) {
  CHECK_EQ(kbd_hid_translate(0x1A, KBD_MOD_LGUI), KBD_KEY_FN_LETTER('w'));
  CHECK_EQ(kbd_hid_translate(0x09, KBD_MOD_LGUI), KBD_KEY_FN_LETTER('f'));
  CHECK_EQ(kbd_hid_translate(0x16, KBD_MOD_RGUI), KBD_KEY_FN_LETTER('s'));
  CHECK_EQ(kbd_hid_translate(0x0B, KBD_MOD_LGUI), KBD_KEY_FN_LETTER('h'));
  /* And they are outside the opt range, or the global handler would swallow
   * every one of them before a shell saw it. */
  CHECK(KBD_KEY_FN_LETTER('a') > KBD_KEY_OPT_LETTER('z'));
}

/* The same chord from the built-in keyboard and from Bluetooth must be the
 * same byte, or a window closes from one keyboard and not the other. */
void test_the_two_keyboards_agree_about_fn(void) {
  CHECK_EQ(KBD_KEY_FN_LETTER('w'), KEY_FN_LETTER('w'));
  CHECK_EQ(KBD_KEY_OPT_LETTER('t'), KEY_OPT_LETTER('t'));
  CHECK_EQ(KBD_KEY_HELP, KEY_HELP);
}

/* The help key must be a chord a keyboard actually sends. It was 0x86 while
 * both drivers emitted the fn-letter code for fn-h, and the shells waited
 * for 0x86: no key on either keyboard opened the key list. */
void test_fn_h_is_the_help_key_on_both_keyboards(void) {
  CHECK_EQ(kbd_hid_translate(0x0B, KBD_MOD_LGUI), KBD_KEY_HELP);
  CHECK_EQ(KEY_FN_LETTER('h'), KEY_HELP);
}

/* Alt stands in for Opt, and its chords get codes of their own so an app
 * taking text cannot swallow the shortcut that leaves it. */
void test_alt_makes_the_global_shortcut_codes(void) {
  CHECK_EQ(kbd_hid_translate(0x1E, KBD_MOD_LALT), KBD_KEY_OPT_DIGIT(1));  /* alt-1 */
  CHECK_EQ(kbd_hid_translate(0x20, KBD_MOD_LALT), KBD_KEY_OPT_DIGIT(3));  /* alt-3 */
  CHECK_EQ(kbd_hid_translate(0x27, KBD_MOD_RALT), KBD_KEY_OPT_DIGIT(0));  /* alt-0 */
  CHECK_EQ(kbd_hid_translate(0x17, KBD_MOD_LALT), KBD_KEY_OPT_LETTER('t'));
  CHECK_EQ(kbd_hid_translate(0x16, KBD_MOD_LALT), KBD_KEY_OPT_LETTER('s'));
  /* and without alt they are still themselves */
  CHECK_EQ(kbd_hid_translate(0x17, 0), 't');
  CHECK_EQ(kbd_hid_translate(0x1E, 0), '1');
}

void test_repeat_waits_for_the_delay_then_runs_at_the_rate(void) {
  uint8_t out[8];
  setup();
  CHECK_EQ(feed(0, 0x04, 0, 1000, out), 1);

  CHECK_EQ(kbd_hid_repeat(&k, 1000 + KBD_REPEAT_DELAY_MS - 1, out, 8), 0);
  CHECK_EQ(kbd_hid_repeat(&k, 1000 + KBD_REPEAT_DELAY_MS, out, 8), 1);
  CHECK_EQ(out[0], 'a');

  /* And then at the faster rate, not the delay again. */
  CHECK_EQ(kbd_hid_repeat(&k, 1000 + KBD_REPEAT_DELAY_MS, out, 8), 0);
  CHECK_EQ(kbd_hid_repeat(&k, 1000 + KBD_REPEAT_DELAY_MS + KBD_REPEAT_RATE_MS,
                          out, 8), 1);
}

void test_lifting_the_key_stops_the_repeat(void) {
  uint8_t out[8];
  setup();
  CHECK_EQ(feed(0, 0x04, 0, 0, out), 1);
  CHECK_EQ(feed(0, 0, 0, 10, out), 0);                /* released */
  CHECK_EQ(kbd_hid_repeat(&k, 100000, out, 8), 0);    /* long past the delay */
}

/* The newest key is the one that repeats. Holding 'a' and then pressing 'b'
 * should repeat 'b', the way every keyboard behaves. */
void test_the_newest_key_is_the_one_that_repeats(void) {
  uint8_t out[8];
  setup();
  feed(0, 0x04, 0, 0, out);
  feed(0, 0x04, 0x05, 10, out);
  CHECK_EQ(kbd_hid_repeat(&k, 10 + KBD_REPEAT_DELAY_MS, out, 8), 1);
  CHECK_EQ(out[0], 'b');
}

/* The millisecond counter wraps every 49 days, and a deadline compared with
 * plain < stops repeating for the 25 days after that. */
void test_repeat_survives_the_millisecond_counter_wrapping(void) {
  uint8_t out[8];
  uint32_t near_wrap = 0xFFFFFF00u;
  setup();
  CHECK_EQ(feed(0, 0x04, 0, near_wrap, out), 1);
  /* The deadline is past the wrap; a time just after the wrap is later. */
  CHECK_EQ(kbd_hid_repeat(&k, near_wrap + KBD_REPEAT_DELAY_MS, out, 8), 1);
  CHECK_EQ(out[0], 'a');
}

/* Escape is the app's to use as "back" now, so leaving an app that keeps it
 * needs a chord of its own: fn-` on the Cardputer, GUI-Escape over
 * Bluetooth. The two keyboards must agree, or an app is inescapable from
 * one of them. */
void test_fn_escape_is_the_quit_chord_on_both_keyboards(void) {
  CHECK_EQ(kbd_hid_translate(0x29, KBD_MOD_LGUI), KBD_KEY_QUIT);
  CHECK_EQ(kbd_hid_translate(0x29, KBD_MOD_RGUI), KBD_KEY_QUIT);
  /* alt-backspace is the same chord from the other corner of the keyboard */
  CHECK_EQ(kbd_hid_translate(0x2A, KBD_MOD_LALT), KBD_KEY_QUIT);
  CHECK_EQ(kbd_hid_translate(0x2A, 0), 0x08);
  CHECK_EQ(KBD_KEY_QUIT, KEY_QUIT);
  /* Escape on its own still reaches the app as Escape. */
  CHECK_EQ(kbd_hid_translate(0x29, 0), KEY_ESC);
  /* And the quit chord cannot collide with the arrows or a fn letter. */
  CHECK(KEY_QUIT != KEY_UP && KEY_QUIT != KEY_DOWN);
  CHECK(KEY_QUIT != KEY_LEFT && KEY_QUIT != KEY_RIGHT);
  CHECK(!KEY_IS_FN(KEY_QUIT) && !KEY_IS_OPT(KEY_QUIT));
}
