/* The launcher: a fullscreen list of apps, and a host for the one that runs.
 * Device-only.
 *
 * A different shape from the desktop rather than a reskin of it. The desktop
 * exists to show that windows work; the launcher exists to be used. It has no
 * chrome, no taskbar, no pointer to chase and no compositor -- the whole screen
 * is either the grid or the running app, so a repaint is one rectangle and
 * there is nothing to composite.
 *
 * Apps run fullscreen here whatever they asked for, because a window with
 * nothing behind it is just a smaller screen with a border.
 */
#ifndef CARDOS_LAUNCHUI_H
#define CARDOS_LAUNCHUI_H

#include <stdint.h>

#include "kernel/input/mouse.h"

void launchui_init(void);

/* Repaint now, for an app that is about to block. */
void launchui_repaint(void);

/* Open a named app straight away, skipping the carousel -- what the console's
 * `run` command does. The name is matched against what the launcher shows,
 * case-insensitively. Returns 0 if it was found and started.
 *
 * Escape then returns to the carousel rather than to the console, because that
 * is what Escape does everywhere else in this shell. */
int launchui_run(const char *name);

/* Returns 1 when the user asked to leave for the text console. */
int  launchui_key(uint8_t key);

void launchui_tick(uint32_t ms);
void launchui_mouse_apply(const MouseReport *r);
void launchui_mouse_done(void);

#endif /* CARDOS_LAUNCHUI_H */
