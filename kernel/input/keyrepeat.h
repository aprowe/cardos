/* Key auto-repeat: which keys repeat, and when.
 *
 * The matrix keyboard reports rising edges and nothing else, so holding an
 * arrow moved one row and holding backspace deleted one character. The
 * Bluetooth decoder (kbd_hid.c) already repeated, on its own schedule and
 * for every key including fn-w. Both now come here for the answer, so the
 * two keyboards agree on what repeats and how fast.
 *
 * What repeats: what a PC repeats and nothing a chord means. Letters,
 * digits, punctuation, space, backspace, delete, tab and the four arrows.
 * Not enter (a held enter in the launcher would open an app and keep
 * pressing enter inside it), not escape, and no fn/opt chord -- a repeated
 * opt-3 or fn-w is a disaster, not a convenience.
 *
 * An app that wants none of it sets CAPP_NO_REPEAT and the shell drops
 * repeats before they arrive; one that wants some of it asks
 * api->key_repeat() inside its key handler. */
#ifndef CARDOS_KEYREPEAT_H
#define CARDOS_KEYREPEAT_H

#include <stdint.h>

#define KEYREPEAT_DELAY_MS 400
#define KEYREPEAT_RATE_MS  60

/* Does this key code repeat when held? */
int keyrepeat_wanted(uint8_t key);

/* The timer for one keyboard. `press` is called for every key delivered on a
 * rising edge with where it came from (any two ints the driver can use to
 * check the key is still down); `due` asks whether a repeat should be
 * delivered now, given whether that key is still down. The key that is
 * repeated is whatever the driver translates the position to *now*, so a
 * modifier pressed or released mid-hold takes effect -- `due` only says
 * when. */
typedef struct {
  int      active;
  int      x, y;
  uint32_t at_ms;
} KeyRepeat;

void keyrepeat_press(KeyRepeat *r, uint8_t key, int x, int y, uint32_t now_ms);
int  keyrepeat_due(KeyRepeat *r, int still_down, uint32_t now_ms);
void keyrepeat_clear(KeyRepeat *r);

#endif /* CARDOS_KEYREPEAT_H */
