/* Painting an app that has the whole screen. Device-only.
 *
 * One function for both shells: the launcher's running app and the
 * desktop's fullscreen one were painted by two near-identical copies, and a
 * fix to one (damage, the clear on entry) had to be remembered in the other.
 *
 * What it adds over calling app->paint: a full repaint is composed off the
 * panel (draw_offscreen), so an app that fills its background and draws
 * over it -- which is most of them -- no longer shows the fill. A repaint
 * narrowed to what the app marked stays direct: those apps draw over
 * themselves, which is exactly why they marked it.
 */
#ifndef CARDOS_APPHOST_H
#define CARDOS_APPHOST_H

#include "kernel/ui/app.h"

/* How a paint came about. */
#define AH_FULL     0x01   /* all of it, whatever it marked: something covered it */
#define AH_SURROUND 0x02   /* the screen outside its rect is not the app's: paint that */
#define AH_OPENED   0x04   /* it has just opened over something else */
#define AH_ASKED    0x08   /* the app asked (key, tick, ...); without it only `extra` wants paint */

/* Paint `a` into `rect`. `extra`, when not NULL, is something the shell
 * drew over the app that has gone (the busy badge): repainted as well, and
 * alone if the app did not ask. Leaves the clip at the whole screen. */
void apphost_paint(const AppDef *a, Rect rect, int how, const Rect *extra);

#endif /* CARDOS_APPHOST_H */
