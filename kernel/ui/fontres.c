/* Fonts loaded from the card. See fontres.h. */

#include "kernel/ui/fontres.h"
#include "kernel/fs/fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Big enough for a whole ASCII set at a heading size in 4 bits; a file past
 * it is a mistake (a 96 px font of everything), not something to fill the
 * heap with. */
#define FONT_FILE_MAX (48 * 1024)

typedef struct {
  int         used;
  const void *owner;
  uint8_t    *data;
  CFont       font;
  char        path[64];
} Font;

static const char *TAG = "font";
static Font s_font[FONTRES_MAX];

/* The shell loads for apps and the print task frees its job's, so the table
 * is touched from two tasks. */
static SemaphoreHandle_t s_lock;

static void lock(void) {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void resolve(const char *name, char *out, size_t n) {
  if (strchr(name, '/')) snprintf(out, n, "%s", name);
  else snprintf(out, n, "%s/%s.cfnt", FONTS_DIR, name);
}

int fontres_load(const char *name, const void *owner) {
  char path[64];
  FsStat st;
  uint8_t *data;
  int i, fd, h = -1, got;

  if (!name || !*name) return -1;
  resolve(name, path, sizeof path);

  lock();
  for (i = 0; i < FONTRES_MAX; i++)
    if (s_font[i].used && s_font[i].owner == owner && !strcmp(s_font[i].path, path)) {
      unlock();
      return i;                          /* asked for twice: the same one */
    }
  for (i = 0; i < FONTRES_MAX; i++) if (!s_font[i].used) { h = i; break; }
  if (h >= 0) s_font[h].used = 1;        /* claimed before the slow part */
  unlock();
  if (h < 0) {
    ESP_LOGW(TAG, "%s: all %d fonts in use", path, FONTRES_MAX);
    return -1;
  }

  if (fs_stat(path, &st) != 0 || st.is_dir || st.size == 0 || st.size > FONT_FILE_MAX) {
    ESP_LOGW(TAG, "%s: missing, or not a font-sized file", path);
    goto fail;
  }
  data = (uint8_t *)malloc(st.size);
  if (!data) {
    ESP_LOGW(TAG, "%s: no memory for %u bytes", path, (unsigned)st.size);
    goto fail;
  }
  fd = fs_open(path, FS_O_READ);
  got = fd >= 0 ? fs_read(fd, data, st.size) : -1;
  if (fd >= 0) fs_close(fd);
  if (got != (int)st.size || cfont_parse(&s_font[h].font, data, st.size) != 0) {
    ESP_LOGW(TAG, "%s: not a readable .cfnt", path);
    free(data);
    goto fail;
  }

  lock();
  s_font[h].owner = owner;
  s_font[h].data = data;
  snprintf(s_font[h].path, sizeof s_font[h].path, "%s", path);
  unlock();
  ESP_LOGI(TAG, "%s: %u bytes, line %d", path, (unsigned)st.size, s_font[h].font.height);
  return h;

fail:
  lock();
  s_font[h].used = 0;
  unlock();
  return -1;
}

static void drop(Font *f) {
  free(f->data);
  memset(f, 0, sizeof *f);
}

void fontres_free(int h) {
  if (h < 0 || h >= FONTRES_MAX) return;
  lock();
  if (s_font[h].used && s_font[h].data) drop(&s_font[h]);
  unlock();
}

void fontres_release_owner(const void *owner) {
  int i;
  lock();
  for (i = 0; i < FONTRES_MAX; i++)
    if (s_font[i].used && s_font[i].data && s_font[i].owner == owner) drop(&s_font[i]);
  unlock();
}

const CFont *fontres_get(int h) {
  if (h < 0 || h >= FONTRES_MAX || !s_font[h].used || !s_font[h].data) return NULL;
  return &s_font[h].font;
}
