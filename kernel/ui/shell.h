/* Which shell owns the screen. Device-only.
 *
 * An app can be hosted by the desktop or by the launcher, and it must not have
 * to know which. Settings in particular needs to repaint mid-action -- a WiFi
 * scan blocks for seconds and the user has to see why -- and calling the
 * desktop's repaint from an app running under the launcher would paint a
 * desktop over the top of it.
 */
#ifndef CARDOS_SHELL_H
#define CARDOS_SHELL_H

#include <stdint.h>

typedef enum { UI_NONE = 0, UI_DESKTOP, UI_LAUNCHER } UiShell;

void    ui_set_shell(UiShell s);
UiShell ui_shell(void);

/* Repaint the active shell immediately. For an action that is about to block
 * for long enough to be noticed. */
void ui_repaint(void);

/* Bring a band of the focused app's content into view, if the shell scrolls
 * its apps. The launcher does not: everything it runs is fullscreen. */
void ui_scroll_into_view(int16_t y, int16_t h);

#endif /* CARDOS_SHELL_H */
