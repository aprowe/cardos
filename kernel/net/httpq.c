/* Asynchronous HTTP. See httpq.h. */

#include "kernel/net/httpq.h"
#include "kernel/net/http.h"
#include "kernel/net/httpslot.h"

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

static TaskHandle_t      s_task;
static SemaphoreHandle_t s_go;         /* signals the task that work is ready */
static SemaphoreHandle_t s_lock;

/* Who has the slot and whether they are still around: kernel/net/httpslot.c,
 * which is where the sequence that used to wedge this is written down. Read
 * without the lock only to answer "is it running" -- every transition is
 * made under it. */
static HttpSlot     s_slot;
static int          s_result;
static char        *s_reply;           /* owned here; freed on collection */

static char s_method[8];
static char s_url[URL_MAX];
static char s_body[BODY_MAX];
static char s_ct[CT_MAX];
static char s_bearer[BEARER_MAX];
static int  s_has_body, s_has_ct, s_has_bearer, s_timeout;

/* The file mode: body read from one path, reply written to another, and
 * s_reply stays NULL because nothing comes back through it. */
static char s_body_path[80], s_reply_path[80];
static int  s_files;

int httpq_active(void) { return s_slot.state == HTTPSLOT_RUNNING; }

static void httpq_task(void *arg) {
  (void)arg;
  for (;;) {
    if (xSemaphoreTake(s_go, portMAX_DELAY) != pdTRUE) continue;

    /* The quiet form: the shell is alive and drawing throughout, so the busy
     * badge must not be armed -- two writers to the panel is the one thing
     * kernel/sys/busy.c is careful to avoid. The shell draws its own
     * indicator instead, which it can, because it is not blocked. */
    if (s_files)
      s_result = http_exchange_files(s_url, s_body_path,
                                     s_has_ct ? s_ct : NULL,
                                     s_has_bearer ? s_bearer : NULL,
                                     s_reply_path, s_timeout);
    else
      s_result = http_request_quiet(s_method, s_url,
                                    s_has_body ? s_body : NULL,
                                    s_has_ct ? s_ct : NULL,
                                    s_has_bearer ? s_bearer : NULL,
                                    s_reply, REPLY_MAX, s_timeout);

    /* Under the lock, because the owner may be being unloaded on the other
     * core at this very moment. A reply nobody is coming for is freed here
     * rather than left as DONE: that was the reply that sat in the slot until
     * reboot and answered "busy" to every app that asked after it. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    {
      const void *owner = s_slot.owner;
      if (httpslot_finish(&s_slot)) {
        ESP_LOGI(TAG, "%d for %p, who left: dropped", s_result, owner);
        free(s_reply); s_reply = NULL;
      } else {
        ESP_LOGD(TAG, "%d for %p, waiting to be collected", s_result, owner);
      }
    }
    xSemaphoreGive(s_lock);
  }
}

void httpq_init(void) {
  if (s_task) return;
  s_go = xSemaphoreCreateBinary();
  s_lock = xSemaphoreCreateMutex();
  if (!s_go || !s_lock) { ESP_LOGE(TAG, "no memory for the queue"); return; }
  httpslot_init(&s_slot);
  xTaskCreatePinnedToCore(httpq_task, "httpq", Q_STACK, NULL, Q_PRIORITY,
                          &s_task, Q_CORE);
}

int httpq_start(const void *owner, const char *method, const char *url,
                const char *body, const char *content_type, const char *bearer,
                int timeout_ms) {
  int rc = 0;

  if (!s_task || !url || !method) return -1;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (httpslot_claim(&s_slot, owner) != 0) {
    ESP_LOGW(TAG, "refused %p: slot is %s, owner %p", owner,
             s_slot.state == HTTPSLOT_RUNNING ? "running" : "done", s_slot.owner);
    xSemaphoreGive(s_lock);
    return -1;
  }
  ESP_LOGI(TAG, "%p starts %s %s", owner, method, url);

  s_reply = malloc(REPLY_MAX);
  if (!s_reply) { httpslot_collect(&s_slot); xSemaphoreGive(s_lock); return -2; }
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
  s_files = 0;
  s_result = HTTPQ_PENDING;
  xSemaphoreGive(s_lock);

  xSemaphoreGive(s_go);
  return rc;
}

int httpq_start_files(const void *owner, const char *url,
                      const char *body_path, const char *content_type,
                      const char *auth, const char *reply_path,
                      int timeout_ms) {
  if (!s_task || !url || !body_path || !reply_path) return -1;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (httpslot_claim(&s_slot, owner) != 0) { xSemaphoreGive(s_lock); return -1; }

  s_reply = NULL;                          /* the reply goes to the card */
  snprintf(s_url, sizeof s_url, "%s", url);
  snprintf(s_body_path, sizeof s_body_path, "%s", body_path);
  snprintf(s_reply_path, sizeof s_reply_path, "%s", reply_path);
  s_has_ct = content_type && content_type[0];
  s_has_bearer = auth && auth[0];
  if (s_has_ct) snprintf(s_ct, sizeof s_ct, "%s", content_type);
  if (s_has_bearer) snprintf(s_bearer, sizeof s_bearer, "%s", auth);
  s_timeout = timeout_ms;
  s_files = 1;
  s_result = HTTPQ_PENDING;
  xSemaphoreGive(s_lock);

  xSemaphoreGive(s_go);
  return 0;
}

int httpq_poll(char *out, size_t out_size) {
  int r;

  if (s_slot.state == HTTPSLOT_IDLE) return -1;   /* nothing was ever started */
  if (s_slot.state == HTTPSLOT_RUNNING) return HTTPQ_PENDING;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  r = s_result;
  if (out && out_size) {
    snprintf(out, out_size, "%s", s_reply ? s_reply : "");
  }
  free(s_reply);
  s_reply = NULL;
  httpslot_collect(&s_slot);
  xSemaphoreGive(s_lock);
  return r;
}

void httpq_abandon(const void *owner) {
  if (!s_lock || !owner) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_slot.state != HTTPSLOT_IDLE)
    ESP_LOGD(TAG, "%p leaves; slot is %s for %p", owner,
             s_slot.state == HTTPSLOT_RUNNING ? "running" : "done", s_slot.owner);
  if (httpslot_abandon(&s_slot, owner)) { free(s_reply); s_reply = NULL; }
  xSemaphoreGive(s_lock);
}
