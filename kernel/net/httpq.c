/* Asynchronous HTTP. See httpq.h. */

#include "kernel/net/httpq.h"
#include "kernel/net/http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "httpq";

/* Enough for a trimmed Calendar reply (about 120 bytes an event, forty of
 * them) with room over. Allocated per request rather than held: it is 8 KB of
 * a 120 KB heap and there is nothing to hold it for between syncs. */
#define REPLY_MAX   8192
#define URL_MAX     384
#define BODY_MAX    512
#define BEARER_MAX  512
#define CT_MAX      48

/* Below the shell so a request never competes with drawing, above nothing
 * else of ours. Core 1, with the other background work. 6 KB of stack because
 * mbedTLS's handshake is not frugal. */
#define Q_PRIORITY  3
#define Q_STACK     6144
#define Q_CORE      1

enum { IDLE = 0, RUNNING, DONE };

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_go;         /* signals the task that work is ready */
static SemaphoreHandle_t s_lock;

static volatile int s_state;
static int          s_result;
static char        *s_reply;           /* owned here; freed on collection */

static char s_method[8];
static char s_url[URL_MAX];
static char s_body[BODY_MAX];
static char s_ct[CT_MAX];
static char s_bearer[BEARER_MAX];
static int  s_has_body, s_has_ct, s_has_bearer, s_timeout;

int httpq_active(void) { return s_state == RUNNING; }

static void httpq_task(void *arg) {
  (void)arg;
  for (;;) {
    if (xSemaphoreTake(s_go, portMAX_DELAY) != pdTRUE) continue;

    /* The quiet form: the shell is alive and drawing throughout, so the busy
     * badge must not be armed -- two writers to the panel is the one thing
     * kernel/sys/busy.c is careful to avoid. The shell draws its own
     * indicator instead, which it can, because it is not blocked. */
    s_result = http_request_quiet(s_method, s_url,
                                  s_has_body ? s_body : NULL,
                                  s_has_ct ? s_ct : NULL,
                                  s_has_bearer ? s_bearer : NULL,
                                  s_reply, REPLY_MAX, s_timeout);
    s_state = DONE;
  }
}

void httpq_init(void) {
  if (s_task) return;
  s_go = xSemaphoreCreateBinary();
  s_lock = xSemaphoreCreateMutex();
  if (!s_go || !s_lock) { ESP_LOGE(TAG, "no memory for the queue"); return; }
  xTaskCreatePinnedToCore(httpq_task, "httpq", Q_STACK, NULL, Q_PRIORITY,
                          &s_task, Q_CORE);
}

int httpq_start(const char *method, const char *url, const char *body,
                const char *content_type, const char *bearer, int timeout_ms) {
  int rc = 0;

  if (!s_task || !url || !method) return -1;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_state != IDLE) { xSemaphoreGive(s_lock); return -1; }

  s_reply = malloc(REPLY_MAX);
  if (!s_reply) { xSemaphoreGive(s_lock); return -2; }
  s_reply[0] = 0;

  /* Copied, not referenced: the caller is an app that may be unloaded, or a
   * stack frame that is about to go away. */
  snprintf(s_method, sizeof s_method, "%s", method);
  snprintf(s_url, sizeof s_url, "%s", url);
  s_has_body = body && body[0];
  s_has_ct = content_type && content_type[0];
  s_has_bearer = bearer && bearer[0];
  if (s_has_body) snprintf(s_body, sizeof s_body, "%s", body);
  if (s_has_ct) snprintf(s_ct, sizeof s_ct, "%s", content_type);
  if (s_has_bearer) snprintf(s_bearer, sizeof s_bearer, "%s", bearer);
  s_timeout = timeout_ms;
  s_result = HTTPQ_PENDING;
  s_state = RUNNING;
  xSemaphoreGive(s_lock);

  xSemaphoreGive(s_go);
  return rc;
}

int httpq_poll(char *out, size_t out_size) {
  int r;

  if (s_state == IDLE) return -1;           /* nothing was ever started */
  if (s_state == RUNNING) return HTTPQ_PENDING;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  r = s_result;
  if (out && out_size) {
    snprintf(out, out_size, "%s", s_reply ? s_reply : "");
  }
  free(s_reply);
  s_reply = NULL;
  s_state = IDLE;
  xSemaphoreGive(s_lock);
  return r;
}
