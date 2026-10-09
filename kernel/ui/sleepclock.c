/* The sleep clock: what a sleeping screen shows instead of black, when
 * Settings > Display > Sleep says clock (or fn-c asks for it now).
 *
 * Black, the time in clock56 in a dark grey, the date small beneath -- at
 * the backlight's lowest, drawn once a minute and not otherwise. While it
 * shows, display_freeze() keeps everything else off the panel: apps and
 * shells go on running and painting into nothing, so nothing has to stop,
 * and opt-backspace, or Enter, Space, Esc or Del twice, unlocks it and repaints it all --
 * not any key, so a key brushed by accident does not light it
 * (src/main.c, kernel/sys/power.c).
 */
#include "kernel/ui/sleepclock.h"
#include "kernel/ui/draw.h"
#include "kernel/ui/fontres.h"
#include "kernel/ui/cfont.h"
#include "kernel/sys/clock.h"
#include "kernel/drv/display.h"
#include "kernel/sys/notify.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define C_BLACK  RGB565(0, 0, 0)
#define C_TIME   RGB565(92, 98, 112)
#define C_DATE   RGB565(60, 64, 76)
#define C_NAPP   RGB565(70, 100, 130)
#define C_NTEXT  RGB565(84, 88, 100)
#define LIST_MAX 3

static const char s_owner = 0;
static int s_font = -2;              /* -2 not loaded yet; -1 none on the card */

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static void body(void *ctx) {
  char hm[8], date[24] = "";
  const CFont *big;
  int16_t w;
  uint32_t t = clock_epoch();
  (void)ctx;
  if (s_font == -2) s_font = fontres_load("clock56", &s_owner);
  big = fontres_get(s_font);
  clock_hm(hm, sizeof hm);
  if (t) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(date, sizeof date, "%a %d %b", &tm);
  }
  const char *app, *title, *text;
  int n = 0, i, top;
  while (n < LIST_MAX && notify_unread_at(n, &app, &title, &text)) n++;
  /* With notifications the clock moves up to make room beneath it. */
  top = 30 - 5 * n;
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_BLACK);
  if (big) {
    w = (int16_t)cfont_width(big, hm);
    draw_text_cfont(big, (int16_t)((DISPLAY_W - w) / 2), (int16_t)top, hm, C_TIME, C_BLACK);
  } else {
    w = (int16_t)(draw_text_width(hm) * 5);
    draw_text_scaled((int16_t)((DISPLAY_W - w) / 2), (int16_t)(top + 10), hm, 5, C_TIME, C_BLACK);
  }
  w = draw_text_width(date);
  draw_text((int16_t)((DISPLAY_W - w) / 2), (int16_t)(top + (n ? 62 : 70)), date, C_DATE, C_BLACK);
  for (i = 0; i < n; i++) {
    char head[48];
    int y = top + 76 + i * 12;
    int16_t aw;
    notify_unread_at(i, &app, &title, &text);
    snprintf(head, sizeof head, "%s %s", app, title);
    aw = draw_text_width(head);
    if (aw > 90) aw = 90;
    draw_text_ellipsis(8, (int16_t)y, 90, head, C_NAPP, C_BLACK);
    draw_text_ellipsis((int16_t)(8 + aw + 6), (int16_t)y, (int16_t)(DISPLAY_W - 22 - aw), text, C_NTEXT, C_BLACK);
  }
}

void sleepclock_paint(void) {
  display_freeze(0);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_offscreen(R(0, 0, DISPLAY_W, DISPLAY_H), C_BLACK, body, NULL);
  display_freeze(1);
}
