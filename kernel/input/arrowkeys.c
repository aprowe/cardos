/* ; , . / or arrows, by where the keys are going. See arrowkeys.h. */
#include "kernel/input/arrowkeys.h"

#include "kernel/app/capp.h"
#include "kernel/drv/keyboard.h"

uint8_t arrowkeys_swap(uint8_t k) {
  switch (k) {
  case ';':       return KEY_UP;
  case '.':       return KEY_DOWN;
  case ',':       return KEY_LEFT;
  case '/':       return KEY_RIGHT;
  case KEY_UP:    return ';';
  case KEY_DOWN:  return '.';
  case KEY_LEFT:  return ',';
  case KEY_RIGHT: return '/';
  default:        return k;
  }
}

int arrowkeys_first(int palette, int other_panel, int wants_text) {
  if (palette) return 1;
  if (other_panel) return 0;
  return wants_text == CAPP_TEXT_ARROWS;
}
