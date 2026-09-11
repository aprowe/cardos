/* Cardputer keyboard. Device-only.
 *
 * 8 columns driven through a 74HC138 decoder from three address lines, seven
 * pulled-up row inputs read back active-low. 8 x 7 = 56 switches, presented as
 * the 4x14 layout printed on the keys.
 */
#ifndef CARDOS_KEYBOARD_H
#define CARDOS_KEYBOARD_H

#include <stdint.h>

/* Values above 0x7F are not characters. The ` key, labelled ESC on the case,
 * is the universal escape and is reported as KEY_ESC, never as a backtick. */
#define KEY_ESC       0x1B
#define KEY_ENTER     0x0D
#define KEY_BACKSPACE 0x08
#define KEY_TAB       0x09

#define KEY_UP        0x80   /* the ; , . / keys double as arrows under Fn */
#define KEY_DOWN      0x81
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83

int keyboard_init(void);

/* Scan once and return the next newly-pressed key, or 0 if none.
 * Call from the main loop; it debounces by edge, so a held key reports once. */
uint8_t keyboard_poll(void);

int keyboard_shift_down(void);
int keyboard_ctrl_down(void);
int keyboard_fn_down(void);

/* The arrow this key stands for, or 0. On this keyboard ; . , / double as the
 * arrow cluster under Fn; a shell with nothing to type into maps them without
 * it, because reaching for Fn to move a selection is a lot of hand for one
 * step. The caller decides when that is appropriate -- see AppDef.wants_text. */
uint8_t keyboard_arrow_for(uint8_t k);

#endif /* CARDOS_KEYBOARD_H */
