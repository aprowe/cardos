/* The Settings app. Device-only.
 *
 * Kept out of apps.c because it reaches across the whole system -- radio,
 * panel, desktop -- and apps.c is meant to stay a collection of small
 * self-contained things.
 */
#ifndef CARDOS_SETTINGS_H
#define CARDOS_SETTINGS_H

#include "kernel/ui/app.h"

const AppDef *settings_app(void);

/* Repaint immediately, for an action that is about to block for seconds. */
void settings_paint_now(void);

#endif /* CARDOS_SETTINGS_H */
