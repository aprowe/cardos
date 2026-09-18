/* See applog.h. */

#include "kernel/sys/applog.h"
#include "kernel/sys/logring.h"
#include "kernel/fs/fs.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#define LINE_CAP  200
#define FILE_CAP  32768u

static SemaphoreHandle_t s_lock;
static int s_off;                    /* logging to the card turned off */

static void lock_init(void) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

void applog(const char *tag, const char *msg) {
  char line[LINE_CAP];
  int n, fd;

  /* Serial as well as the card. When a cable is attached it is still the
   * fastest way to watch, and this costs nothing when it is not. */
  ESP_LOGI(tag && *tag ? tag : "app", "%s", msg ? msg : "");

  if (s_off || !fs_mounted()) return;
  lock_init();
  if (!s_lock) return;

  n = logring_line(line, sizeof line,
                   (uint32_t)(esp_timer_get_time() / 1000), tag, msg);
  if (n <= 0) return;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  {
    FsStat st;
    uint32_t size = (fs_stat(APPLOG_PATH, &st) == 0) ? st.size : 0;
    if (logring_rotate_needed(size, n, FILE_CAP)) {
      /* One generation back, not a numbered series: two files are enough to
       * catch a failure that happened a few minutes ago, and a card with
       * fifty log files on it is its own problem. */
      fs_remove(APPLOG_PREV);
      fs_rename(APPLOG_PATH, APPLOG_PREV);
    }
    fd = fs_open(APPLOG_PATH, FS_O_WRITE | FS_O_CREATE | FS_O_APPEND);
    if (fd >= 0) {
      fs_write(fd, line, (size_t)n);
      fs_close(fd);
    } else {
      /* A card that will not take the log must not turn every log call into
       * a failed open: that is slow enough to be felt in the shell's loop. */
      s_off = 1;
    }
  }
  xSemaphoreGive(s_lock);
}

void applogf(const char *tag, const char *fmt, ...) {
  char msg[LINE_CAP];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  applog(tag, msg);
}

void applog_clear(void) {
  lock_init();
  if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
  fs_remove(APPLOG_PATH);
  fs_remove(APPLOG_PREV);
  s_off = 0;
  if (s_lock) xSemaphoreGive(s_lock);
}

/* The tail, without holding the file in memory: the log is up to 32 KB and
 * the heap is not. Read forward in chunks, keeping a ring of line offsets,
 * then go back and print from the one that is `max_lines` from the end. */
int applog_tail(int max_lines, void (*emit)(const char *line)) {
#define RING 64
  uint32_t start[RING];
  char buf[256], line[LINE_CAP];
  int fd, n, i, count = 0, len = 0, printed = 0;
  uint32_t pos = 0;

  if (!emit) return 0;
  if (max_lines > RING) max_lines = RING;
  if (max_lines <= 0) max_lines = 20;

  fd = fs_open(APPLOG_PATH, FS_O_READ);
  if (fd < 0) return 0;
  start[0] = 0;
  while ((n = fs_read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++, pos++)
      if (buf[i] == '\n') start[++count % RING] = pos + 1;
  }
  fs_close(fd);

  fd = fs_open(APPLOG_PATH, FS_O_READ);
  if (fd < 0) return 0;
  {
    uint32_t from = (count > max_lines) ? start[(count - max_lines) % RING] : 0;
    fs_seek(fd, (int32_t)from, FS_SEEK_SET);
    while ((n = fs_read(fd, buf, sizeof buf)) > 0) {
      for (i = 0; i < n; i++) {
        if (buf[i] == '\n') {
          line[len] = 0;
          emit(line);
          printed++;
          len = 0;
        } else if (len < (int)sizeof line - 1) {
          line[len++] = buf[i];
        }
      }
    }
    if (len) { line[len] = 0; emit(line); printed++; }
  }
  fs_close(fd);
  return printed;
#undef RING
}
