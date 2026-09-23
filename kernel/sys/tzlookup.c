#include "kernel/sys/tzlookup.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "kernel/net/http.h"
#include "kernel/net/update.h"     /* update_base, update_token */
#include "kernel/sys/applog.h"
#include "kernel/sys/env.h"
#include "kernel/sys/tzreply.h"

static const char *TAG = "tz";

/* Measured, not guessed: the task logs its high-water mark. Plain HTTP to the
 * server, and an https:// PROXY would need TLS, which httpq's 6 KB task
 * already shows fits. The heap goes back when the task exits. */
#define TZ_STACK 6144

static volatile int s_running;
static volatile int s_ready;             /* a rule is waiting for the shell */
static char s_rule[64];
static char s_zone[48];

static int tz_unset(void) {
  const char *tz = env_get("TZ");
  return !tz || !tz[0];
}

static void lookup_task(void *arg) {
  char url[128], reply[256], rule[64], zone[48];
  int n;
  (void)arg;

  snprintf(url, sizeof url, "%s/tz", update_base());
  n = http_request_quiet("GET", url, NULL, NULL,
                         update_token()[0] ? update_token() : NULL,
                         reply, sizeof reply, 8000);
  if (n < 0) {
    ESP_LOGW(TAG, "no answer from %s (%d)", url, n);
  } else if (tzreply_parse(reply, rule, sizeof rule, zone, sizeof zone) != 0) {
    ESP_LOGW(TAG, "not a rule: %.60s", reply);
    applogf("tz", "server said: %.60s", reply);
  } else {
    snprintf(s_rule, sizeof s_rule, "%s", rule);
    snprintf(s_zone, sizeof s_zone, "%s", zone);
    s_ready = 1;
  }
  ESP_LOGI(TAG, "stack high water: %u bytes unused of %d",
           (unsigned)uxTaskGetStackHighWaterMark(NULL), TZ_STACK);
  s_running = 0;
  vTaskDelete(NULL);
}

void tzlookup_start(void) {
  if (s_running || s_ready || !tz_unset()) return;
  s_running = 1;
  if (xTaskCreatePinnedToCore(lookup_task, "cardos-tz", TZ_STACK, NULL, 2,
                              NULL, 1) != pdPASS) {
    ESP_LOGW(TAG, "no memory for the lookup task");
    s_running = 0;
  }
}

const char *tzlookup_take(char *zone, size_t n) {
  static char rule[64];
  if (!s_ready) return NULL;
  snprintf(rule, sizeof rule, "%s", s_rule);
  if (zone && n) snprintf(zone, n, "%s", s_zone);
  s_ready = 0;
  /* A person may have set TZ while the request was in the air: theirs wins. */
  return tz_unset() ? rule : NULL;
}
