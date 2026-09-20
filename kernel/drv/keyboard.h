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

/* The help panel. Fn-h produces it, so it IS the fn-h chord.
 *
 * It was ctrl-h, which ASCII says is 0x08 -- the same byte Backspace sends --
 * and a code of its own (0x86) existed to break the tie. Under the modifier
 * convention the collision simply stops existing: ctrl belongs to the app, so
 * ctrl-h is whatever the app wants it to be, and help moved to the modifier
 * that owns the window. For a while the header said "fn-h" while the code
 * stayed 0x86 and the drivers emitted the fn-letter code below, so no key on
 * either keyboard opened help. Defining it as the chord closes that gap. */
#define KEY_HELP      (0xE0 + ('h' - 'a'))   /* == KEY_FN_LETTER('h') */

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
#define KEY_IS_OPT(k)     ((k) >= 0xA0 && (k) <= 0xD9)

/* THE MODIFIER CONVENTION. Three modifiers, three scopes, read outwards:
 *
 *   ctrl  the app      save, run, sync -- whatever has focus owns every one
 *   fn    the window   close, fullscreen, minimise, the Start menu, help
 *   opt   the OS       switch shell, brightness, radios, user hotkeys
 *
 * Before this, the shells took ctrl-S, ctrl-P, ctrl-W and ctrl-F out from
 * under every app -- so in a desktop window ctrl-S opened the Start menu
 * instead of saving, and ctrl-P toggled a pointer instead of previewing. A
 * key now means one thing, and an app may take every ctrl chord it likes.
 *
 * Fn chords get codes of their own for the same reason opt's do: they have to
 * survive being typed inside a text field, where an app is swallowing letters.
 * 0xE0..0xF9 is fn with a letter. Fn with ; , . / stays the arrow cluster --
 * nobody thinks of an arrow as a chord. */
#define KEY_FN_LETTER(c)  ((uint8_t)(0xE0 + ((c) - 'a')))
#define KEY_IS_FN(k)      ((k) >= 0xE0)

/* Fn plus the ` key, or opt plus backspace: leave the app, whatever it
 * thinks about it.
 *
 * Escape alone used to do this, unconditionally, in the launcher and over a
 * fullscreen app -- so the same key went back inside a windowed app and quit
 * outright everywhere else, which is the sort of inconsistency you learn by
 * losing a draft. Escape is now offered to the app first everywhere, and this
 * is the way out of one that wants to keep it. It is a chord of its own for
 * the reason every fn chord is: it has to survive being typed into a text
 * field. */
#define KEY_QUIT      0x84

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

/* Any key down right now, consuming nothing. For code inside a long
 * operation that wants to know it should stop. */
int keyboard_any_down(void);

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
