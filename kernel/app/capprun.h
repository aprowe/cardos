/* Loaded apps, wearing a built-in app's clothes. Device-only.
 *
 * elfload.c gets a .capp into memory; this turns it into an AppDef, so the
 * window system cannot tell a loaded app from a compiled-in one. That is the
 * whole point of the split: the desktop, the taskbar and the focus rules never
 * learn that loadable apps exist.
 */
#ifndef CARDOS_CAPPRUN_H
#define CARDOS_CAPPRUN_H

#include "kernel/ui/app.h"
#include "kernel/app/capp.h"

/* Executable RAM is a small, separate pool, and every loaded app holds some
 * for as long as its icon is on the desktop. Four is more than the screen
 * fits icons for. */
#define CAPPRUN_MAX 4

/* Load and register. Returns a slot index, or -1. The app stays loaded until
 * unloaded: keeping it resident is what lets the desktop show its real name
 * and icon rather than a placeholder. */
int capprun_load(const char *path);

void capprun_unload_all(void);

const AppDef  *capprun_def(int slot);
const uint8_t *capprun_icon(int slot);    /* 16x16 1bpp, CAPP_ICON_BYTES */
int            capprun_fullscreen(int slot);

/* Hand the app the file it was opened on, if it takes one. */
void capprun_set_file(int slot, const char *path);

/* Executable RAM still free, for the Settings app to report. */
uint32_t capprun_exec_free(void);

#endif /* CARDOS_CAPPRUN_H */
