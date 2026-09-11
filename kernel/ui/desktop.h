/* The desktop shell: taskbar, window chrome, start menu. Device-only.
 *
 * Draws what kernel/ui/wm.c decides. Everything reaches the panel through
 * damage rectangles -- there is no back buffer, so a repaint is a set of small
 * clipped draws rather than a frame.
 */
#ifndef CARDOS_DESKTOP_H
#define CARDOS_DESKTOP_H

#include <stdint.h>

#include "kernel/ui/wm.h"
#include "kernel/input/mouse.h"

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

/* Apply a report without repainting, then repaint once. A mouse sends reports
 * far faster than the panel can be redrawn, and repainting per report is what
 * makes it flicker and lag rather than move. */
void desktop_mouse_apply(const MouseReport *r);
void desktop_mouse_done(void);

/* Keyboard-driven pointer, for when no mouse is paired. Arrows move it and
 * space clicks -- which is also how the whole interaction path gets tested
 * before the radio exists. */
/* Icons live in /desktop on the card: NAME.app opens a built-in app, and any
 * .bin is a firmware to chain-boot. Clicking selects, double-clicking runs. */
void desktop_icon_click(int16_t x, int16_t y);
void desktop_reload_icons(void);

/* Remembered across a reboot: set when the desktop launches a firmware, so
 * that the rollback after the guest is reset lands back on the desktop rather
 * than at a console the user never asked for. */
void desktop_set_autostart(int on);
int  desktop_autostart(void);

void desktop_set_kbd_mouse(int on);
int  desktop_kbd_mouse(void);

#endif /* CARDOS_DESKTOP_H */
