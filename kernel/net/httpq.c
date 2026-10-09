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
 * a 120 KB heap and there is nothing to hold it for between syncs. A caller
 * with its own buffer (httpq_start_into) costs nothing here at all. */
#define REPLY_MAX   8192
/* A URL longer than this is refused, not cut: a cut URL is a different
 * request, and the server answers it as one. */
#define URL_MAX     384

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
static char        *s_reply;           /* the reply: ours or the caller's */
static size_t       s_reply_cap;
static int          s_reply_ours;      /* 1: malloc'd here, freed on collection */

/* The request's strings, copied into one block that lives from start until
 * the task has sent it -- a body used to be cut at 511 bytes into a static,
 * silently, and Dashboard Link's 4 KB answers reached the server as their
 * first half-kilobyte. Pointers into s_req; NULL for one not given. */
static char       *s_req;
static const char *s_method, *s_url, *s_body, *s_ct, *s_bearer;
static const char *s_body_path, *s_reply_path;   /* the file mode */
static int         s_timeout, s_files;

int httpq_active(void) { return s_slot.state == HTTPSLOT_RUNNING; }

static void drop_reply(void) {
  if (s_reply_ours) free(s_reply);
  s_reply = NULL;
  s_reply_cap = 0;
  s_reply_ours = 0;
}

static void httpq_task(void *arg) {
  (void)arg;
  for (;;) {
    if (xSemaphoreTake(s_go, portMAX_DELAY) != pdTRUE) continue;

    /* The quiet form: the shell is alive and drawing throughout, so the busy
     * badge must not be armed -- two writers to the panel is the one thing
     * kernel/sys/busy.c is careful to avoid. The shell draws its own
     * indicator instead, which it can, because it is not blocked. */
    if (s_files)
      s_result = http_exchange_files(s_url, s_body_path, s_ct, s_bearer,
                                     s_reply_path, s_timeout);
    else
      s_result = http_request_quiet(s_method, s_url, s_body, s_ct, s_bearer,
                                    s_reply, s_reply_cap, s_timeout);

    /* What a TLS request left of the 6 KB: the number Q_STACK was chosen
     * by, logged when it reaches a new low so the device says it rather
     * than this comment guessing. */
    {
      static unsigned low = ~0u;
      unsigned spare = (unsigned)uxTaskGetStackHighWaterMark(NULL);
      if (spare < low) {
        low = spare;
        ESP_LOGI(TAG, "stack: %u bytes never used (of %u)", spare, (unsigned)Q_STACK);
      }
    }

    /* Under the lock, because the owner may be being unloaded on the other
     * core at this very moment. A reply nobody is coming for is freed here
     * rather than left as DONE: that was the reply that sat in the slot until
     * reboot and answered "busy" to every app that asked after it. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_req);                         /* sent: the strings are done with */
    s_req = NULL;
    s_method = s_url = s_body = s_ct = s_bearer = s_body_path = s_reply_path = NULL;
    {
      const void *owner = s_slot.owner;
      if (httpslot_finish(&s_slot)) {
        ESP_LOGI(TAG, "%d for %p, who left: dropped", s_result, owner);
        drop_reply();
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

/* Copy up to NSTR strings into one block; each *dst points at its copy, or
 * is NULL for a string that was NULL or empty. NULL if there was no memory
 * (and nothing is set). Called with the lock held. */
#define NSTR 6
static char *pack(const char *src[NSTR], const char **dst[NSTR]) {
  size_t total = 1, len[NSTR];
  char *b, *p;
  int i;
  for (i = 0; i < NSTR; i++) {
    len[i] = (src[i] && src[i][0]) ? strlen(src[i]) : 0;
    total += len[i] ? len[i] + 1 : 0;
  }
  if ((b = (char *)malloc(total)) == NULL) return NULL;
  for (i = 0, p = b; i < NSTR; i++) {
    if (!dst[i]) continue;
    if (!len[i]) { *dst[i] = NULL; continue; }
    memcpy(p, src[i], len[i] + 1);
    *dst[i] = p;
    p += len[i] + 1;
  }
  return b;
}

/* The one way in. `into` NULL means a reply buffer of `cap` is malloc'd
 * here; otherwise the reply is written straight into `into`. */
static int start(const void *owner, char *into, size_t cap, int files,
                 const char *method, const char *url, const char *body,
                 const char *content_type, const char *bearer,
                 const char *body_path, const char *reply_path, int timeout_ms) {
  const char *src[NSTR] = { method, url, body, content_type, bearer, NULL };
  const char **dst[NSTR] = { &s_method, &s_url, &s_body, &s_ct, &s_bearer, NULL };

  if (!s_task || !url) return -1;
  if (strlen(url) >= URL_MAX) {
    ESP_LOGW(TAG, "refused %p: a %u-character URL", owner, (unsigned)strlen(url));
    return -3;
  }
  if (files) {
    /* the method is POST; the paths take the method's and the body's places */
    src[0] = body_path;  dst[0] = &s_body_path;
    src[2] = reply_path; dst[2] = &s_reply_path;
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (httpslot_claim(&s_slot, owner) != 0) {
    ESP_LOGW(TAG, "refused %p: slot is %s, owner %p", owner,
             s_slot.state == HTTPSLOT_RUNNING ? "running" : "done", s_slot.owner);
    xSemaphoreGive(s_lock);
    return -1;
  }
  ESP_LOGI(TAG, "%p starts %s %s", owner, files ? "POST" : method, url);

  /* Copied, not referenced: the caller is an app that may be unloaded, or a
   * stack frame that is about to go away. */
  s_method = s_url = s_body = s_ct = s_bearer = s_body_path = s_reply_path = NULL;
  s_req = pack(src, dst);
  if (!s_req) { httpslot_collect(&s_slot); xSemaphoreGive(s_lock); return -2; }

  if (files) {
    s_reply = NULL;                        /* the reply goes to the card */
    s_reply_cap = 0;
    s_reply_ours = 0;
  } else if (into) {
    s_reply = into;
    s_reply_cap = cap;
    s_reply_ours = 0;
    s_reply[0] = 0;
  } else {
    s_reply = (char *)malloc(cap);
    if (!s_reply) {
      free(s_req); s_req = NULL;
      httpslot_collect(&s_slot);
      xSemaphoreGive(s_lock);
      return -2;
    }
    s_reply_cap = cap;
    s_reply_ours = 1;
    s_reply[0] = 0;
  }
  s_timeout = timeout_ms;
  s_files = files;
  s_result = HTTPQ_PENDING;
  xSemaphoreGive(s_lock);

  xSemaphoreGive(s_go);
  return 0;
}

int httpq_start(const void *owner, const char *method, const char *url,
                const char *body, const char *content_type, const char *bearer,
                int timeout_ms) {
  if (!method) return -1;
  return start(owner, NULL, REPLY_MAX, 0, method, url, body, content_type, bearer,
               NULL, NULL, timeout_ms);
}

int httpq_start_into(const void *owner, char *buf, size_t cap,
                     const char *method, const char *url,
                     const char *body, const char *content_type, const char *bearer,
                     int timeout_ms) {
  if (!method || !buf || cap < 2) return -1;
  return start(owner, buf, cap, 0, method, url, body, content_type, bearer,
               NULL, NULL, timeout_ms);
}

int httpq_start_files(const void *owner, const char *url,
                      const char *body_path, const char *content_type,
                      const char *auth, const char *reply_path,
                      int timeout_ms) {
  if (!body_path || !reply_path) return -1;
  return start(owner, NULL, 0, 1, NULL, url, NULL, content_type, auth,
               body_path, reply_path, timeout_ms);
}

int httpq_poll(char *out, size_t out_size) {
  int r;

  if (s_slot.state == HTTPSLOT_IDLE) return -1;   /* nothing was ever started */
  if (s_slot.state == HTTPSLOT_RUNNING) return HTTPQ_PENDING;

  xSemaphoreTake(s_lock, portMAX_DELAY);
  /* What was copied, not what came: see httpslot_deliver. A reply already
   * in `out` (httpq_start_into) is measured where it is. */
  r = httpslot_deliver(s_result, s_reply, out, out_size);
  drop_reply();
  httpslot_collect(&s_slot);
  xSemaphoreGive(s_lock);
  return r;
}

int httpq_poll_as(const void *owner, char *out, size_t out_size) {
  /* Another's request -- running or waiting to be collected -- is not
   * yours to take: the notification watcher and an app share the one slot,
   * and the first to poll used to walk off with the other's reply. */
  if (s_slot.state != HTTPSLOT_IDLE && s_slot.owner != owner) return -1;
  return httpq_poll(out, out_size);
}

void httpq_abandon(const void *owner) {
  if (!s_lock || !owner) return;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_slot.state != HTTPSLOT_IDLE)
    ESP_LOGD(TAG, "%p leaves; slot is %s for %p", owner,
             s_slot.state == HTTPSLOT_RUNNING ? "running" : "done", s_slot.owner);
  if (httpslot_abandon(&s_slot, owner)) drop_reply();
  xSemaphoreGive(s_lock);
}
