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

#endif /* CARDOS_DESKTOP_H */
