/* The fn-h panel and the fn-space palette. Device-only; the state and the
 * keys are kernel/ui/helpmodel.c, host-tested.
 *
 * One panel over every shell, opened by src/main.c: the app's keys, the
 * shell's own, and the shortcuts that work anywhere, a tab each, scrolled.
 * The texts are one entry a line, "key<TAB>meaning"; a line with no tab is
 * a heading. They are not copied: each must stay valid while the panel is
 * up (an app's help lives as long as the app; the shells' are static).
 *
 * While either is up a shell must not paint (check overlay_active() before
 * a paint), and when one closes the shell underneath repaints.
 */
#ifndef CARDOS_HELP_H
#define CARDOS_HELP_H

#include <stdint.h>
#include "kernel/ui/helpmodel.h"

void help_open(int tab, const char *app_title, const char *app_keys,
               const char *os_title, const char *os_keys, const char *shortcuts);
int  help_active(void);
int  help_key(uint8_t k);              /* 0 when that key closed it */

/* The palette: begin (cleared), pal_add entries, show. palette_key gives
 * PAL_OPEN, PAL_CLOSED, or PAL_CHOSEN with *chosen set (the palette closed;
 * the entry stays readable until the next palette_begin). */
Palette *palette_begin(const char *title);
void     palette_show(void);
int      palette_active(void);
int      palette_key(uint8_t k, const PalEntry **chosen);

int  overlay_active(void);             /* either one is up */
void overlay_paint(void);              /* paint whichever it is, whole */

#endif /* CARDOS_HELP_H */
