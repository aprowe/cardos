/* The built-in keyboard's arrows are ; , . / with fn. That is the right way
 * round where text is typed and the wrong way round where text only
 * filters a list: the fn-space palette, the launcher's search, an app's
 * search box over its results. There they are arrows, and fn makes them
 * the characters again.
 *
 * Only the built-in keyboard is swapped (src/main.c, where its key is
 * read): a Bluetooth keyboard has arrow keys of its own. Where nothing
 * takes text the shells already turn ; , . / into arrows
 * (keyboard_arrow_for), so this is only for places that do take text.
 * Portable; test/test_arrowkeys.c.
 */
#ifndef CARDOS_ARROWKEYS_H
#define CARDOS_ARROWKEYS_H

#include <stdint.h>

/* ; , . / to the arrows, the arrows to ; , . /, anything else as it is. */
uint8_t arrowkeys_swap(uint8_t k);

/* Whether to swap: the palette is open; another panel is (help, the
 * notification centre: they take no text, so fn-arrows stay arrows); and
 * what has the keys says about text (0, 1, or CAPP_TEXT_ARROWS). */
int arrowkeys_first(int palette, int other_panel, int wants_text);

#endif /* CARDOS_ARROWKEYS_H */
