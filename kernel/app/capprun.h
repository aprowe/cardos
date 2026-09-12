/* Running loaded programs. Device-only.
 *
 * elfload.c gets a .capp into memory; this runs it. A program that installs a
 * CappUi becomes an AppDef, so the window system cannot tell a loaded app from
 * a compiled-in one -- that is the point of the split: the desktop, the
 * taskbar and the focus rules never learn that loadable apps exist.
 *
 * A program that installs nothing was a command: it did its work, wrote to
 * stdout, and returned.
 */
#ifndef CARDOS_CAPPRUN_H
#define CARDOS_CAPPRUN_H

#include "kernel/ui/app.h"
#include "kernel/app/capp.h"

/* Sixteen. Four when /desktop held three programs, eight when it held five,
 * and then Pinball arrived as the ninth and simply was not there: the scan
 * asked for a slot, got -1, and skipped the app without a word. Nothing was
 * broken and nothing was reported -- the icon was just missing, which is the
 * worst way for a limit to make itself known.
 *
 * A slot costs a name, a help string and some pointers -- about 250 bytes --
 * whether or not anything is in it. The memory that matters is the per-program
 * allocation, and that is only paid by programs that exist. */
#define CAPPRUN_MAX 16

/* Load and read the descriptor. Nothing runs. Returns a slot index, or -1. */
int capprun_load(const char *path);

void capprun_unload_all(void);

/* Run it. `args` is split into argv here: splitting a command line is the
 * shell's job everywhere else, and there is no reason for it to be the
 * program's here. Returns the exit status, or -1 if the slot is empty. */
int capprun_start(int slot, const char *name, const char *args);

/* Did the program that just ran install a user interface? Non-zero means there
 * is an app to host; zero means it was a command and has already finished. */
int capprun_is_app(int slot);

/* The descriptor's own facts, available without running anything. */
const char    *capprun_name(int slot);
const uint8_t *capprun_icon(int slot);    /* 16x16 1bpp, CAPP_ICON_BYTES */
int            capprun_is_cli(int slot);
int            capprun_fullscreen(int slot);

/* Which of the running app's declared needs were met. See CAPP_NEEDS_*. */
int            capprun_caps_ok(void);

/* Valid only after a run that installed an interface. */
const AppDef *capprun_def(int slot);

/* Executable RAM still free, for the Settings app to report. */
uint32_t capprun_exec_free(void);

#endif /* CARDOS_CAPPRUN_H */
