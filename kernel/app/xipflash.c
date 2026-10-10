/* The appcode partition. See xipflash.h. */

#include "kernel/app/xipflash.h"

#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kernel/fs/path.h"
#include "kernel/sys/applog.h"

static const char *TAG = "xip";

#define APPCODE_SUBTYPE 0x40          /* partitions.csv */

static const esp_partition_t       *s_part;
static esp_partition_mmap_handle_t  s_map;
static const void                  *s_base;
static XipCache                     s_cache;
static int                          s_ready;
static SemaphoreHandle_t            s_lock;

void xipflash_lock(void) {
  if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}
void xipflash_unlock(void) {
  if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

static int p_read(void *ctx, uint32_t off, void *buf, uint32_t n) {
  (void)ctx;
  return esp_partition_read(s_part, off, buf, n) == ESP_OK ? 0 : -1;
}
/* IDF flushes the cache over what it wrote (flash_end_flush_cache in
 * esp_flash_api.c), so a rewritten entry is not served stale -- checked on
 * the device in Task 8, not assumed. */
static int p_write(void *ctx, uint32_t off, const void *buf, uint32_t n) {
  (void)ctx;
  return esp_partition_write(s_part, off, buf, n) == ESP_OK ? 0 : -1;
}
static int p_erase(void *ctx, uint32_t off, uint32_t n) {
  (void)ctx;
  return esp_partition_erase_range(s_part, off, n) == ESP_OK ? 0 : -1;
}

int xipflash_init(void) {
  XipFlash f;
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    (esp_partition_subtype_t)APPCODE_SUBTYPE, "appcode");
  if (!s_part) {
    ESP_LOGW(TAG, "no appcode partition: app code loads into RAM (a full USB flash adds it)");
    return -1;
  }
  /* Once, for the uptime, without BLOCKS_WRITE -- that would refuse our own
   * writes for as long as it is mapped, which is forever. Boot maps in the
   * same order every time, so the address is the same across boots of one
   * build; an entry records it and is rewritten if it ever moves. */
  if (esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_INST,
                         &s_base, &s_map) != ESP_OK) {
    ESP_LOGE(TAG, "cannot map appcode: app code loads into RAM");
    return -1;
  }
  /* Before xip_open, so nothing can reach the cache without it; dropped
   * again below if the cache never becomes ready. */
  s_lock = xSemaphoreCreateRecursiveMutex();
  if (!s_lock) {
    esp_partition_munmap(s_map);
    ESP_LOGE(TAG, "no memory for the appcode lock: app code loads into RAM");
    return -1;
  }
  f.read = p_read;
  f.write = p_write;
  f.erase = p_erase;
  f.ctx = NULL;
  f.size = (uint32_t)s_part->size;
  if (xip_open(&s_cache, &f) != XIP_OK) {
    /* Nothing will ever run from the mapping, so give the address space
     * back; and with no cache there is nothing to lock. */
    esp_partition_munmap(s_map);
    s_base = NULL;
    vSemaphoreDelete(s_lock);
    s_lock = NULL;
    ESP_LOGE(TAG, "cannot read appcode: app code loads into RAM");
    return -1;
  }
  s_ready = 1;
  ESP_LOGI(TAG, "appcode %u KB mapped at %p, head %u", (unsigned)(f.size / 1024), s_base,
           (unsigned)s_cache.head);
  return 0;
}

int xipflash_ready(void) { return s_ready; }
XipCache *xipflash_cache(void) { return s_ready ? &s_cache : NULL; }
uint32_t xipflash_base(void) { return (uint32_t)(uintptr_t)s_base; }

uint32_t xipflash_path_hash(const char *path) {
  char norm[FS_PATH_MAX];
  if (path && path_normalize(path, norm, sizeof norm) == 0) return xip_path_hash(norm);
  return xip_path_hash(path);       /* relative or too long: as given */
}

void xipflash_forget(const char *path) {
  size_t n;
  int r;
  if (!s_ready || !path) return;
  n = strlen(path);
  if (n < 5 || strcasecmp(path + n - 5, ".capp") != 0) return;
  xipflash_lock();
  r = xip_forget(&s_cache, xipflash_path_hash(path));
  xipflash_unlock();
  /* The only guard against stale code for a .capp rewritten with the same
   * size and mtime, so a failure is logged, not dropped. */
  if (r != XIP_OK) applogf("xip", "forget %s failed", path);
}

static void add_live(const XipHdr *h, uint32_t off, int in_use, void *ctx) {
  (void)off; (void)in_use;
  if (h->live == 0xFFFFFFFFu) *(uint32_t *)ctx += h->sectors;
}

void xipflash_usage(uint32_t *live_sectors, uint32_t *total_sectors) {
  *live_sectors = 0;
  *total_sectors = s_ready ? s_cache.f.size / XIP_SECTOR : 0;
  if (!s_ready) return;
  xipflash_lock();
  xip_each(&s_cache, add_live, live_sectors);
  xipflash_unlock();
}
