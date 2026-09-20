/* Key auto-repeat policy and timer. See keyrepeat.h. Portable. */
#include "kernel/input/keyrepeat.h"

int keyrepeat_wanted(uint8_t key) {
  if (key == 0x08 || key == 0x09 || key == 0x7F) return 1;   /* backspace, tab, delete */
  if (key >= 0x20 && key < 0x7F) return 1;                    /* printable */
  if (key >= 0x80 && key <= 0x83) return 1;                   /* arrows */
  return 0;              /* enter, escape, ctrl chars, fn-`, every opt/fn chord */
}

void keyrepeat_press(KeyRepeat *r, uint8_t key, int x, int y, uint32_t now_ms) {
  /* The newest press is the one that repeats, and a press that does not
   * repeat cancels the one before it: holding an arrow and tapping enter
   * should not leave the arrow repeating. */
  r->active = keyrepeat_wanted(key);
  r->x = x;
  r->y = y;
  r->at_ms = now_ms + KEYREPEAT_DELAY_MS;
}

int keyrepeat_due(KeyRepeat *r, int still_down, uint32_t now_ms) {
  if (!r->active) return 0;
  if (!still_down) { r->active = 0; return 0; }
  /* Signed difference, so the comparison survives the millisecond counter
   * wrapping -- the same reason the scheduler compares deadlines this way. */
  if ((int32_t)(now_ms - r->at_ms) < 0) return 0;
  r->at_ms = now_ms + KEYREPEAT_RATE_MS;
  return 1;
}

void keyrepeat_clear(KeyRepeat *r) {
  r->active = 0;
  r->x = r->y = 0;
  r->at_ms = 0;
}
