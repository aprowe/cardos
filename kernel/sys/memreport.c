/* See memreport.h. */
#include "kernel/sys/memreport.h"
#include "kernel/drv/bthid.h"
#include "kernel/net/wifi.h"
#include "kernel/app/arena.h"
#include "kernel/app/xipflash.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "nvs.h"

void mem_report(void (*out)(const char *line, void *ctx), void *ctx) {
  char b[MEMREPORT_LINE];

  /* With the largest block: an app's data has to fit in one piece of the
   * 8-bit heap and its code in one piece of executable RAM, and the total
   * said 44 KB free the day Calendar could not load in 13.9. */
  snprintf(b, sizeof b, "heap free %6u  largest %6u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  out(b, ctx);
  snprintf(b, sizeof b, "exec free %6u  largest %6u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_EXEC),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC));
  out(b, ctx);
  snprintf(b, sizeof b, "low water %6u",
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
  out(b, ctx);
  /* The arena is always 28 KB out of the heap; say who has it, because
   * "heap free" alone no longer says what an open app costs. */
  snprintf(b, sizeof b, "arena     %6u  %s", (unsigned)ARENA_SIZE,
           arena_held() ? arena_holder() : "free");
  out(b, ctx);
  if (xipflash_ready()) {
    uint32_t live, total;
    xipflash_usage(&live, &total);
    snprintf(b, sizeof b, "appcode   %u of %u sectors live", (unsigned)live, (unsigned)total);
  } else {
    snprintf(b, sizeof b, "appcode   none (code in RAM)");
  }
  out(b, ctx);
  if (bthid_radio_on()) snprintf(b, sizeof b, "bluetooth %6u", (unsigned)bthid_heap_cost());
  else snprintf(b, sizeof b, "bluetooth    off");
  out(b, ctx);
  snprintf(b, sizeof b, "wifi      %6u", (unsigned)wifi_heap_cost());
  out(b, ctx);
  /* The settings store, because when it fills up the next boot erases it
   * and every credential with it. Entries are 32 bytes; a page holds 126. */
  {
    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK) {
      snprintf(b, sizeof b, "nvs       %u of %u entries, %u free",
               (unsigned)st.used_entries, (unsigned)st.total_entries,
               (unsigned)st.free_entries);
      out(b, ctx);
    }
  }
}
