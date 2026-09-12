/* Text arriving from something that is not a keyboard.
 *
 * The claim this module makes is that voice needs no app changes because a
 * transcript becomes keystrokes on the path keys already take. These tests are
 * that claim, written down: what a sink receives from input_text must be
 * indistinguishable from someone typing quickly.
 */

#include <string.h>

#include "tinytest.h"
#include "kernel/sys/input.h"
#include "kernel/drv/keyboard.h"    /* only for the key codes */

static char     GOT[256];
static int      GOT_N;
static int      WANTS;

static void sink_key(uint8_t k) {
  if (GOT_N < (int)sizeof GOT - 1) GOT[GOT_N++] = (char)k;
  GOT[GOT_N] = 0;
}

static int sink_wants(void) { return WANTS; }

static void setup(int wants) {
  static const InputSink s = { sink_wants, sink_key };
  GOT_N = 0;
  GOT[0] = 0;
  WANTS = wants;
  input_set_sink(&s);
}

void test_input_delivers_a_string_as_keystrokes(void) {
  setup(1);
  CHECK_EQ(input_text("hello"), 5);
  CHECK(!strcmp(GOT, "hello"));
}

/* Nothing is taking text: the caller has to know, so it can say so rather than
 * leaving the words to vanish. Dictating into Minesweeper should report that
 * it went nowhere. */
void test_input_delivers_nothing_when_nothing_is_listening(void) {
  setup(0);
  CHECK_EQ(input_text("hello"), 0);
  CHECK_EQ(GOT_N, 0);
}

/* A pause in speech comes back as a line break, and what a person would have
 * pressed there is enter. */
void test_input_turns_line_breaks_into_enter(void) {
  setup(1);
  input_text("one\ntwo");
  CHECK_EQ(GOT[3], (char)KEY_ENTER);
  CHECK(!strcmp(GOT, "one\rtwo") || GOT[3] == 0x0D);
}

/* Recognition emits smart quotes and em dashes; the font has 95 glyphs. A
 * dropped character is honest, and transliterating would be a guess about what
 * was actually said. */
void test_input_drops_what_the_font_cannot_draw(void) {
  setup(1);
  CHECK_EQ(input_text("caf\xc3\xa9"), 3);      /* café, in UTF-8 */
  CHECK(!strcmp(GOT, "caf"));
}

void test_input_survives_the_empty_and_the_absurd(void) {
  setup(1);
  CHECK_EQ(input_text(""), 0);
  CHECK_EQ(input_text(NULL), 0);

  input_set_sink(NULL);
  CHECK_EQ(input_text("nobody home"), 0);      /* no sink: no crash, no keys */
  CHECK_EQ(input_wants_text(), 0);
}

/* input.c cannot include the keyboard header -- it is device code and this is
 * portable -- so it repeats two constants. If they ever drift, the symptom
 * would be voice typing a stray character instead of pressing enter, which is
 * exactly the kind of bug nobody finds by reading. */
void test_input_key_codes_match_the_keyboard_driver(void) {
  setup(1);
  input_text("\n");
  CHECK_EQ((uint8_t)GOT[0], KEY_ENTER);
  GOT_N = 0;
  input_text("\t");
  CHECK_EQ((uint8_t)GOT[0], KEY_TAB);
}
