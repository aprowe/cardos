/* The background task. See bg.h -- especially the rule about the display. */

#include "kernel/sys/bg.h"

#include "kernel/drv/bthid.h"
#include "kernel/net/wifi.h"
#include "kernel/sys/clock.h"
#include "kernel/sys/env.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "bg";

/* Four is more than enough: the jobs are seconds long and there are two kinds
 * of them. A deeper queue would only store staler requests. */
#define JOBS      4
#define RESULTS   4
#define MSG_MAX  64

/* Below the shell, which is where the user is looking, and above nothing else
 * of ours. Core 1 because the radios' own tasks live mostly on core 0. */
#define BG_PRIORITY   3
#define BG_STACK      4096
#define BG_CORE       1

typedef struct { char text[MSG_MAX]; } BgMsg;

static QueueHandle_t s_jobs;
static QueueHandle_t s_results;
static volatile int  s_running;          /* the job in progress, or 0 */
static volatile int  s_queued;           /* a bitmask, so one scan is one scan */
/* Both are read-modify-written from two cores: the shell sets a bit on core
 * 0 while this task clears one on core 1. Without the lock a lost clear
 * left a job's bit set for good, and bg_submit refused it until reboot --
 * the Bluetooth reconnect just silently stopped happening. */
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_last_activity;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void bg_note_activity(void) { s_last_activity = now_ms(); }

uint32_t bg_idle_ms(void) {
  uint32_t t = now_ms();
  return t - s_last_activity;
}

int bg_busy(void) { return s_running != 0; }

static void post(const char *fmt, const char *arg) {
  BgMsg m;
  snprintf(m.text, sizeof m.text, fmt, arg ? arg : "");
  /* Never block: a full result queue means the shell has not looked in a
   * while, and dropping the older news is better than stalling a radio. */
  xQueueSend(s_results, &m, 0);
}

static void run_job(BgJob job) {
  switch (job) {
  case BG_BT_RECONNECT: {
    /* The scan that used to freeze the launcher for four seconds. */
    int n = bthid_autoconnect(4);
    if (n > 0) post("bluetooth: %s", bthid_status(BTHID_MOUSE));
    /* Silence when nothing was found: this runs every fifteen seconds while a
     * mouse is out of range, and a notification each time would be the same
     * annoyance the freeze was, in a different form. */
    break;
  }
  case BG_TIME_SYNC:
    /* Waits on a UDP round trip, which is exactly the sort of second the
     * shell should not spend. Silent on failure: a device with no network
     * knows it has no network, and saying so twice helps nobody.
     *
     * The radio is brought up first if it is not already. WiFi is otherwise
     * lazy here -- it costs 53 KB of heap and most sessions never need it --
     * but the clock is the one thing that cannot wait to be asked for: there
     * is no RTC, so a device that only learns the date when someone opens a
     * network app spends every boot not knowing what day it is. This runs on
     * the background task, so the twenty seconds belong to nobody's screen,
     * and it costs nothing on a machine with no credentials saved. */
    if (!wifi_is_connected()) wifi_connect_saved(20000);
    if (clock_sync(8000) == 0) {
      char when[40];
      clock_full(when, sizeof when);
      post("clock: %s", when);

      /* The radio is already up and NTP just answered, so this is the
       * cheapest moment there will ever be to also ask where the network
       * seems to be. Skipped once a person has said TZ themselves -- a
       * guess should never overwrite a choice. Applied and saved the same
       * way `set TZ=...` is: env_set persists it to NVS itself, and TZ has
       * to be pushed through clock_apply_zone() before anything reads a
       * clock again or the new value sits there unused until reboot. */
      if (!clock_zone_set()) {
        char tz[48];
        if (clock_geo_tz(tz, sizeof tz, 8000) == 0) {
          env_set("TZ", tz);
          clock_apply_zone();
          post("timezone: %s", tz);
        }
      }
    }
    break;

  /* Pairing is a six to eight second scan. It used to run on the shell's own
   * loop from Settings, which is the same fault the automatic reconnect had
   * and was moved off this task for -- the screen froze, and the one moment
   * the user is being told "keep the mouse awake" is the worst moment to stop
   * responding to them. */
  case BG_BT_PAIR_MOUSE:
    if (bthid_start(6, BTHID_MOUSE) == 0) post("paired: %s",
                                               bthid_status(BTHID_MOUSE));
    else post("no mouse found%s", NULL);
    break;

  case BG_BT_PAIR_KBD:
    if (bthid_start(8, BTHID_KEYBOARD) == 0) post("paired: %s",
                                                  bthid_status(BTHID_KEYBOARD));
    else post("no keyboard found%s", NULL);
    break;

  case BG_RECONNECT_ALL: {
    char what[MSG_MAX];
    int n;
    if (!wifi_is_connected()) wifi_connect_saved(15000);
    n = bthid_autoconnect(3);
    snprintf(what, sizeof what, "%s, %d bluetooth",
             wifi_is_connected() ? "wifi up" : "no wifi", n);
    post("%s", what);
    if (wifi_is_connected() && !clock_synced()) bg_submit(BG_TIME_SYNC);
    break;
  }

  case BG_WIFI_RECONNECT:
    if (wifi_connect_saved(20000) == 0) {
      post("wifi: %s", wifi_status());
      /* The radio just came up, so this is the cheapest moment there will
       * ever be to ask what time it is. */
      if (!clock_synced()) bg_submit(BG_TIME_SYNC);
    }
    else post("wifi: %s", wifi_status());
    break;
  default:
    break;
  }
}

static void bg_task(void *arg) {
  (void)arg;
  for (;;) {
    BgJob job;
    if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;

    portENTER_CRITICAL(&s_lock);
    s_running = (int)job;
    s_queued &= ~(1 << (int)job);
    portEXIT_CRITICAL(&s_lock);
    run_job(job);
    s_running = 0;
  }
}

void bg_init(void) {
  if (s_jobs) return;
  s_jobs = xQueueCreate(JOBS, sizeof(BgJob));
  s_results = xQueueCreate(RESULTS, sizeof(BgMsg));
  s_last_activity = now_ms();
  if (!s_jobs || !s_results) {
    ESP_LOGE(TAG, "no memory for the queues");
    return;
  }
  xTaskCreatePinnedToCore(bg_task, "cardos-bg", BG_STACK, NULL, BG_PRIORITY,
                          NULL, BG_CORE);
}

int bg_submit(BgJob job) {
  int bit = 1 << (int)job;

  if (!s_jobs) return -1;
  /* Already queued or already running: one scan is one scan, however many
   * times a tick asks for it. */
  portENTER_CRITICAL(&s_lock);
  if ((s_queued & bit) || s_running == (int)job) {
    portEXIT_CRITICAL(&s_lock);
    return -1;
  }
  s_queued |= bit;
  portEXIT_CRITICAL(&s_lock);
  if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
    portENTER_CRITICAL(&s_lock);
    s_queued &= ~bit;
    portEXIT_CRITICAL(&s_lock);
    return -1;
  }
  return 0;
}

const char *bg_take_result(void) {
  static BgMsg m;
  if (!s_results) return NULL;
  if (xQueueReceive(s_results, &m, 0) != pdTRUE) return NULL;
  return m.text;
}
