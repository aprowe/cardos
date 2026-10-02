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

/* MSVC builds these same files for the host tests and does not know the
 * attribute, as apps/toolbar.h found first. */
#if defined(__GNUC__)
#define FOOT_OPT __attribute__((unused))
#else
#define FOOT_OPT
#endif

static FOOT_OPT CRect rect_of(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)(w > 0 ? w : 0); r.h = (int16_t)h;
  return r;
}

/* The bar across the bottom of `c`, with `keys` in it. */
static FOOT_OPT void
footer_paint(const CardApi *api, CRect c, const char *keys) {
  CRect r;
  r.x = c.x;
  r.y = (int16_t)(c.y + c.h - FOOT_H);
  r.w = c.w;
  r.h = FOOT_H;
  /* Round the text, not under it: text paints its own 6x8 background, and
   * filling the bar first and writing over it blinked the hints on every
   * repaint that crossed the footer. */
  {
    int w = keys ? (int)api->str_len(keys) * 6 : 0;
    if (c.x + 4 + w > c.x + c.w) w = c.w - 4;
    api->fill(rect_of(r.x, r.y, r.w, 2), FOOT_BG);                       /* above */
    api->fill(rect_of(r.x, r.y + 10, r.w, FOOT_H - 10), FOOT_BG);       /* below */
    api->fill(rect_of(r.x, r.y + 2, 4, 8), FOOT_BG);                     /* left */
    api->fill(rect_of(r.x + 4 + w, r.y + 2, r.w - 4 - w, 8), FOOT_BG);  /* right */
  }
  if (keys) api->text((int16_t)(c.x + 4), (int16_t)(r.y + 2), keys, FOOT_FG, FOOT_BG);
}

#endif /* CARDOS_FOOTER_H */
