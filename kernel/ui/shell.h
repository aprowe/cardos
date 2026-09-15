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

#include "kernel/ui/app.h"

typedef enum { UI_NONE = 0, UI_DESKTOP, UI_LAUNCHER } UiShell;

/* Setting the shell also remembers it, so the machine comes back up in
 * whichever one you were last using rather than in whichever one the firmware
 * author preferred. */
void    ui_set_shell(UiShell s);
UiShell ui_shell(void);

/* What to start in. UI_LAUNCHER on a machine that has never been told. */
UiShell ui_saved_shell(void);

/* Repaint the active shell immediately. For an action that is about to block
 * for long enough to be noticed. */
void ui_repaint(void);

/* Bring a band of the focused app's content into view, if the shell scrolls
 * its apps. The launcher does not: everything it runs is fullscreen. */
void ui_scroll_into_view(int16_t y, int16_t h);

/* What a voice command is allowed to ask of the shell.
 *
 * Installed by main.c, which is the only file that knows how the three shells
 * relate to each other. An installed table rather than direct calls because
 * the caller is kernel/sys/voice.c, which has host-testable neighbours and no
 * business including the desktop. */
typedef struct {
  int  (*open_app)(const char *name);      /* 0 if it opened */
  void (*switch_shell)(const char *which); /* launcher | desktop | console */
  void (*feed_key)(uint8_t k);             /* as if the key were pressed */
  const AppDef *(*running_app)(void); /* what has the keyboard, or NULL */
} ShellOps;

void shell_set_ops(const ShellOps *ops);

int  shell_open_app(const char *name);
void shell_switch(const char *which);
void shell_feed_key(uint8_t k);
const AppDef *shell_running_app(void);

#endif /* CARDOS_SHELL_H */
