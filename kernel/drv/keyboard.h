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

/* ctrl-h, which ASCII says is 0x08 -- the same byte the Backspace key sends.
 * Only one of them can keep it, and Backspace is a key while ctrl-h is a
 * chord, so the chord is the one that moves. Nothing here wanted the ASCII
 * meaning of ctrl-h; every caller that wants a backspace has a key for it. */
#define KEY_HELP      0x86

/* Opt is the global-shortcut modifier.
 *
 * Its chords are given codes of their own rather than being folded into the
 * ASCII range, because they have to survive being typed *inside* an app --
 * they are how you leave one -- and an app that is taking text would otherwise
 * swallow them. Nothing below 0x90 is affected.
 *
 * 0xA0..0xA9 are opt with a digit, 0xC0..0xD9 opt with a letter. */
#define KEY_OPT_DIGIT(d)  ((uint8_t)(0xA0 + (d)))
#define KEY_OPT_LETTER(c) ((uint8_t)(0xC0 + ((c) - 'a')))
#define KEY_IS_OPT(k)     ((k) >= 0xA0)

#define KEY_UP        0x80   /* the ; , . / keys double as arrows under Fn */
#define KEY_DOWN      0x81
#define KEY_LEFT      0x82
#define KEY_RIGHT     0x83

int keyboard_init(void);

/* Scan once and return the next newly-pressed key, or 0 if none.
 * Call from the main loop; it debounces by edge, so a held key reports once. */
uint8_t keyboard_poll(void);

/* Whether a key is held at this moment, sampled for up to settle_ms. For the
 * boot-time question "is someone holding escape", which has no edge to catch:
 * the key went down before the matrix was powered. */
int keyboard_held(uint8_t key, int settle_ms);

int keyboard_shift_down(void);
int keyboard_ctrl_down(void);
int keyboard_fn_down(void);
int keyboard_opt_down(void);

/* The arrow this key stands for, or 0. On this keyboard ; . , / double as the
 * arrow cluster under Fn; a shell with nothing to type into maps them without
 * it, because reaching for Fn to move a selection is a lot of hand for one
 * step. The caller decides when that is appropriate -- see AppDef.wants_text. */
uint8_t keyboard_arrow_for(uint8_t k);

#endif /* CARDOS_KEYBOARD_H */
