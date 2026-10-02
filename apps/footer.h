/* The key-hint bar along the bottom of an app: one look for every app.
 *
 * 11 pixels, one fill, 6x8 text in a dim colour, "key word" pairs two
 * spaces apart -- "enter open  n new  d delete". At most FOOT_CHARS
 * characters; what does not fit belongs in the app's help (fn-h), which is
 * the full list. Before this there were five heights and a dozen colours,
 * and one bar wider than the screen.
 *
 * Header-only, static helpers, the same arrangement as apps/safefile.h and
 * apps/toolbar.h.
 */
#ifndef CARDOS_FOOTER_H
#define CARDOS_FOOTER_H

#include "kernel/app/capp.h"

#define FOOT_H      11
#define FOOT_BG     CAPP_RGB(30, 34, 44)
#define FOOT_FG     CAPP_RGB(128, 136, 152)
#define FOOT_CHARS  38          /* (240 - 4) / 6, less one for luck */

/* The bar across the bottom of `c`, with `keys` in it. */
static void __attribute__((unused))
footer_paint(const CardApi *api, CRect c, const char *keys) {
  CRect r;
  r.x = c.x;
  r.y = (int16_t)(c.y + c.h - FOOT_H);
  r.w = c.w;
  r.h = FOOT_H;
  api->fill(r, FOOT_BG);
  if (keys) api->text((int16_t)(c.x + 4), (int16_t)(r.y + 2), keys, FOOT_FG, FOOT_BG);
}

#endif /* CARDOS_FOOTER_H */
