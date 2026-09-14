/* The busy indicator. See busy.h for why it has a task of its own. */

#include "kernel/sys/busy.h"

#include "kernel/ui/draw.h"
#include "kernel/drv/display.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define BADGE_W   96
#define BADGE_H   13
#define DOTS      3
#define FRAME_MS  110

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
static char              s_what[20];
static void            (*s_repaint)(void);

int busy_active(void) { return s_depth > 0; }

void busy_on_done(void (*repaint)(void)) { s_repaint = repaint; }

/* One frame. Called only from the task, only under the lock. */
static void paint_badge(int phase) {
  Rect r;
  int i, x = badge_x(), y = badge_y();
  Rect saved = draw_clip();

  /* The indicator is the OS speaking, not the app, so it ignores whatever
   * clip an app left behind. */
  r.x = 0; r.y = 0; r.w = DISPLAY_W; r.h = DISPLAY_H;
  draw_set_clip(r);

  r.x = (int16_t)x; r.y = (int16_t)y; r.w = BADGE_W; r.h = BADGE_H;
  draw_rect(r, CLR_BADGE);
  r.y = (int16_t)(y + BADGE_H - 1); r.h = 1;
  draw_rect(r, CLR_EDGE);

  for (i = 0; i < DOTS; i++) {
    r.x = (int16_t)(x + 6 + i * 6);
    r.y = (int16_t)(y + 5);
    r.w = 4; r.h = 4;
    draw_rect(r, (i == phase % DOTS) ? CLR_DOT : CLR_DIM);
  }

  if (s_what[0])
    draw_text((int16_t)(x + 26), (int16_t)(y + 3), s_what, CLR_TEXT, CLR_BADGE);

  draw_set_clip(saved);
}

static void busy_task(void *arg) {
  int phase = 0;
  (void)arg;
  for (;;) {
    if (s_depth > 0) {
      if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        /* Re-checked under the lock: busy_end may have run while we waited,
         * and drawing after it would leave the badge on screen forever. */
        if (s_depth > 0) { paint_badge(phase++); s_shown = 1; }
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
   * a few rectangles and a string. */
  xTaskCreatePinnedToCore(busy_task, "busy", 2048, NULL, 4, &s_task, 0);
}

void busy_begin(const char *what) {
  if (!s_lock) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_depth == 0) snprintf(s_what, sizeof s_what, "%s", what ? what : "");
  s_depth++;
  xSemaphoreGive(s_lock);
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
  if (repaint && s_repaint) s_repaint();
}
