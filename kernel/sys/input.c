/* Text in, from wherever. See input.h. */

#include "kernel/sys/input.h"

#include <stddef.h>

/* Matching kernel/drv/keyboard.h without including it: this file is portable
 * and has host tests, and that header is device code. The test asserts the two
 * agree. */
#define IN_KEY_ENTER 0x0D
#define IN_KEY_TAB   0x09

static InputSink s_sink;

void input_set_sink(const InputSink *sink) {
  if (sink) s_sink = *sink;
  else { s_sink.wants_text = NULL; s_sink.key = NULL; }
}

int input_wants_text(void) {
  return s_sink.wants_text ? s_sink.wants_text() : 0;
}

static int s_repeat;
void input_set_repeat(int repeat) { s_repeat = repeat; }
int  input_is_repeat(void) { return s_repeat; }

void input_key(uint8_t k) {
  if (s_sink.key) s_sink.key(k);
}

int input_text(const char *s) {
  int sent = 0;

  if (!s || !s_sink.key) return 0;

  /* Asked once, before the first character rather than before each one. An
   * app that closes its own text field partway through a sentence -- pressing
   * enter in a form, say -- should still receive the rest as keys, because
   * that is what a fast typist would have produced. */
  if (!input_wants_text()) return 0;

  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;

    if (c == '\n' || c == '\r') {
      /* A transcript with a line break in it means the speaker paused, not
       * that they want a new line in a single-line field. Enter is what a
       * person would have pressed, so enter is what is sent. */
      input_key(IN_KEY_ENTER);
      sent++;
      continue;
    }
    if (c == '\t') { input_key(IN_KEY_TAB); sent++; continue; }

    /* Printable ASCII only. Recognition emits smart quotes and em dashes, and
     * a font with 95 glyphs cannot draw them -- dropping is honest, and
     * transliterating would be a guess about what was said. */
    if (c < 0x20 || c > 0x7E) continue;

    input_key(c);
    sent++;
  }
  return sent;
}
