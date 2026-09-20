/* Where text comes from, and where it goes.
 *
 * Three things can produce a character on this machine: the keyboard under
 * your thumbs, a Bluetooth keyboard, and a sentence said out loud. The apps
 * know about none of them. A key is a key, and `input_text` is how anything
 * that is not a keyboard becomes one.
 *
 * That is the whole design. Voice needed no change to any app because it does
 * not deliver text to apps -- it delivers keystrokes to the shell, along the
 * path keys already take, and the app's own key handler does the rest. An
 * editor that scrolls to its caret keeps scrolling to its caret; a field that
 * rejects control characters keeps rejecting them.
 */
#ifndef CARDOS_INPUT_H
#define CARDOS_INPUT_H

#include <stdint.h>

/* Is whatever has focus taking text right now?
 *
 * The same question `; . , /` already asks to decide between arrows and
 * characters -- promoted here because voice needs the same answer, and two
 * sources asking it separately would eventually disagree. */
int  input_wants_text(void);

/* Deliver a string as if it had been typed, one character at a time, to
 * whatever has focus. Newlines arrive as enter, tabs as tab; anything else
 * unprintable is dropped rather than guessed at.
 *
 * Returns the number of characters delivered. Zero means nothing was taking
 * text -- the caller should say so rather than assume it landed. */
int  input_text(const char *s);

/* One character, the same way. Exposed because the voice path wants to send a
 * transcript and then a return, and "and then a return" should not require
 * building a string to hold one. */
void input_key(uint8_t k);

/* The shell installs these at startup: the one that answers "is text wanted"
 * and the one that takes a key. Kept as function pointers so this module
 * depends on neither the desktop nor the launcher -- both of which depend on
 * things that only exist on the device, and this has host tests. */
typedef struct {
  int  (*wants_text)(void);
  void (*key)(uint8_t k);
} InputSink;

void input_set_sink(const InputSink *sink);

/* Is the key being delivered right now an auto-repeat of a held key? The
 * shell sets this before handing each key down and clears it after, so an
 * app's key handler can ask (api->key_repeat) and treat a held Enter
 * differently from a held arrow. Keys from voice or the serial line are
 * never repeats. */
void input_set_repeat(int repeat);
int  input_is_repeat(void);

#endif /* CARDOS_INPUT_H */
