/* The busy indicator. See busy.h for why it has a task of its own. */

#include "kernel/sys/busy.h"

#include "kernel/ui/draw.h"
#include "kernel/ui/shell.h"
#include "kernel/ui/launchui.h"
#include "kernel/ui/desktop.h"
#include "kernel/drv/display.h"
#include "kernel/console/font6x8.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define BADGE_W   96
#define BADGE_H   13
#define DOTS      3
#define FRAME_MS  110

/* Not at all for a request that is over before anyone could wonder. Every
 * blocking api->http used to paint the badge on its first frame and then
 * repaint the whole shell to take it away -- in Build and Claude, which
 * poll, that was a blink every second or so. Most of those polls answer
 * well inside this. */
#define SHOW_AFTER_MS 350

/* Top centre. Not a corner: the corners hold a clock, a battery and a status
 * line depending on the shell, and the middle of the top edge is the one
 * strip every shell leaves alone. */
static int badge_x(void) { return (DISPLAY_W - BADGE_W) / 2; }
static int badge_y(void) { return 0; }

#define CLR_BADGE  RGB565(28, 32, 42)
#define CLR_EDGE   RGB565(70, 110, 170)
#define CLR_DOT    RGB565(120, 170, 240)
#define CLR_DIM    RGB565(52, 60, 76)
#define CLR_TEXT   RGB565(214, 224, 240)

static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static volatile int      s_depth;          /* nested begins */
static volatile int      s_shown;          /* something is on the screen */
static volatile TickType_t s_began;        /* when the outermost begin came */
static char              s_what[20];
static void            (*s_repaint)(void);

/* The frame is composed here and sent in one blit, straight to the panel.
 * It used to be drawn with draw.c from this task -- while the shell, which
 * draw.c's statics (the clip, the staging buffers) belong to, sat blocked
 * part way through whatever it was doing. Owning its pixels means only the
 * shell ever draws through draw.c, and the badge cannot land in a strip the
 * shell was composing off the panel. 2.5 KB of .bss. */
static uint16_t s_px[BADGE_W * BADGE_H];

int busy_active(void) { return s_depth > 0; }

void busy_on_done(void (*repaint)(void)) { s_repaint = repaint; }

static void box(int x, int y, int w, int h, uint16_t c) {
  int r, k;
  for (r = y; r < y + h && r < BADGE_H; r++)
    for (k = x; k < x + w && k < BADGE_W; k++) s_px[r * BADGE_W + k] = c;
}

static void text(int x, int y, const char *s) {
  for (; *s && x + FONT_W <= BADGE_W; s++, x += FONT_W) {
    unsigned char ch = (unsigned char)*s;
    const uint8_t *g = (ch >= FONT_FIRST && ch <= FONT_LAST) ? font6x8[ch - FONT_FIRST] : NULL;
    int col, row;
    for (col = 0; col < FONT_W; col++)
      for (row = 0; row < FONT_H && y + row < BADGE_H; row++)
        s_px[(y + row) * BADGE_W + x + col] =
            (g && ((g[col] >> row) & 1)) ? CLR_TEXT : CLR_BADGE;
  }
}

/* One frame. Called only from the task, only under the lock. */
static void paint_badge(int phase) {
  int i;
  box(0, 0, BADGE_W, BADGE_H, CLR_BADGE);
  box(0, BADGE_H - 1, BADGE_W, 1, CLR_EDGE);
  for (i = 0; i < DOTS; i++)
    box(6 + i * 6, 5, 4, 4, (i == phase % DOTS) ? CLR_DOT : CLR_DIM);
  if (s_what[0]) text(26, 3, s_what);
  display_blit_panel(badge_x(), badge_y(), BADGE_W, BADGE_H, s_px);
}

static void busy_task(void *arg) {
  int phase = 0;
  (void)arg;
  for (;;) {
    if (s_depth > 0 && xTaskGetTickCount() - s_began >= pdMS_TO_TICKS(SHOW_AFTER_MS)) {
      if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        /* Re-checked under the lock: busy_end may have run while we waited,
         * and drawing after it would leave the badge on screen forever.
         * Not under a notification's banner, which owns those rows. */
        if (s_depth > 0 && !draw_reserved_top()) { paint_badge(phase++); s_shown = 1; }
        xSemaphoreGive(s_lock);
      }
      vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    } else {
      vTaskDelay(pdMS_TO_TICKS(40));
    }
  }
}

void busy_init(void) {
  if (s_task) return;
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return;
  /* Priority 4: above the background task, because this must get a slice
   * while a network call holds the shell. Core 0 is where the shell runs, and
   * the shell is blocked whenever this has anything to do. 2 KB is enough for
   * composing a frame into a static buffer. */
  xTaskCreatePinnedToCore(busy_task, "busy", 2048, NULL, 4, &s_task, 0);
}

void busy_begin(const char *what) {
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_depth == 0) {
    snprintf(s_what, sizeof s_what, "%s", what ? what : "");
    s_began = xTaskGetTickCount();
  }
  s_depth++;
  xSemaphoreGive(s_lock);
}

/* Put back what the badge covered: the shell repaints that rectangle, and
 * an app on screen its own damage plus it -- not the whole shell, which is
 * what this did after every request that showed the badge at all. */
static void repaint_under(void) {
  Rect r;
  r.x = (int16_t)badge_x(); r.y = (int16_t)badge_y();
  r.w = BADGE_W; r.h = BADGE_H;
  switch (ui_shell()) {
  case UI_LAUNCHER: launchui_damage(r); break;
  case UI_DESKTOP:  desktop_damage(r); break;
  default:          if (s_repaint) s_repaint(); break;
  }
}

void busy_end(void) {
  int repaint = 0;
  if (!s_lock) return;
  /* Taking the lock is what makes this safe: it cannot return while the task
   * is part way through a frame, so the caller never starts drawing into a
   * half-finished badge. */
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_depth > 0) s_depth--;
  if (s_depth == 0 && s_shown) { s_shown = 0; repaint = 1; }
  xSemaphoreGive(s_lock);

  /* Outside the lock: the repaint runs on this thread and may take a while,
   * and holding the lock through it would stall a task that has nothing left
   * to do anyway. */
  if (repaint) repaint_under();
}
