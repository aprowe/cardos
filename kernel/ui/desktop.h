/* The desktop shell: taskbar, window chrome, start menu. Device-only.
 *
 * Draws what kernel/ui/wm.c decides. Everything reaches the panel through
 * damage rectangles -- there is no back buffer, so a repaint is a set of small
 * clipped draws rather than a frame.
 */
#ifndef CARDOS_DESKTOP_H
#define CARDOS_DESKTOP_H

#include <stdint.h>

#include "ui/wm.h"
#include "input/mouse.h"

#define TASKBAR_H 13

void desktop_init(void);

/* Paint whatever is currently damaged. Cheap when nothing is. */
void desktop_flush(void);

/* Force a full repaint -- on entry, and after anything that cannot be
 * expressed as damage. */
void desktop_repaint(void);

/* Returns 0 to stay in the desktop, 1 to leave for the text console. */
int  desktop_key(uint8_t key);

/* Content painter for a window; the desktop supplies one per window. */
void desktop_tick(uint32_t ms);

/* Feed one decoded mouse report in. Handles the cursor, focus, dragging and
 * the close box. Safe to call whether or not a real mouse exists. */
void desktop_mouse(const MouseReport *r);

/* Keyboard-driven pointer, for when no mouse is paired. Arrows move it and
 * space clicks -- which is also how the whole interaction path gets tested
 * before the radio exists. */
void desktop_set_kbd_mouse(int on);
int  desktop_kbd_mouse(void);

#endif /* CARDOS_DESKTOP_H */
