/* Recording and playback on a task. See audio.h. */
#include "kernel/sys/audio.h"
#include "kernel/drv/mic.h"
#include "kernel/drv/speaker.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* The mic's and the speaker's 1 KB blocks live on this stack, with the
 * FAT driver's own needs under them. 6 KB has headroom. */
#define A_STACK    6144
#define A_PRIORITY 5

static volatile int      s_state;
static volatile int      s_stop;
static volatile int      s_level = -1;
static volatile uint32_t s_pos_bytes;
static uint32_t          s_total_bytes;
static uint32_t          s_byte_rate;      /* bytes per second of the file playing */
static int               s_last_bytes = -1;
static char              s_path[128];
static int               s_max_ms;
static char              s_error[48];

static int  stop_cb(void) { return s_stop; }
static void level_cb(int pct) { s_level = pct; }
static void progress_cb(uint32_t bytes) { s_pos_bytes = bytes; }

static void record_task(void *param) {
  int n;
  (void)param;
  n = mic_record_wav(s_path[0] ? s_path : NULL, s_max_ms, stop_cb, level_cb);
  s_last_bytes = n;
  if (n < 0) snprintf(s_error, sizeof s_error, "%s", "the mic or the card refused");
  ESP_LOGI(TAG, "recorded %d bytes to %s", n, s_path);
  s_level = -1;
  s_state = AUDIO_IDLE;
  vTaskDelete(NULL);
}

static void play_task(void *param) {
  (void)param;
  if (speaker_play_wav(s_path, stop_cb, progress_cb) != 0)
    snprintf(s_error, sizeof s_error, "%s", speaker_error());
  ESP_LOGI(TAG, "played %u of %u bytes of %s", (unsigned)s_pos_bytes,
           (unsigned)s_total_bytes, s_path);
  s_state = AUDIO_IDLE;
  vTaskDelete(NULL);
}

static int start(void (*task)(void *), int state, const char *name) {
  s_stop = 0;
  s_error[0] = 0;
  s_state = state;
  if (xTaskCreatePinnedToCore(task, name, A_STACK, NULL, A_PRIORITY, NULL, 0) != pdPASS) {
    s_state = AUDIO_IDLE;
    snprintf(s_error, sizeof s_error, "%s", "no memory for the task");
    return -2;
  }
  return 0;
}

int audio_record(const char *path, int max_ms) {
  if (s_state != AUDIO_IDLE) return -1;
  if (path && !*path) return -2;
  snprintf(s_path, sizeof s_path, "%s", path ? path : "");   /* "" listens */
  s_max_ms = max_ms;
  s_level = 0;
  s_last_bytes = -1;
  return start(record_task, AUDIO_RECORDING, "record");
}

int audio_play(const char *path) {
  WavInfo w;
  const char *why = "";
  if (s_state != AUDIO_IDLE) return -1;
  if (!path || speaker_wav_info(path, &w, &why) != 0) {
    snprintf(s_error, sizeof s_error, "%s", why);
    return -2;
  }
  snprintf(s_path, sizeof s_path, "%s", path);
  s_total_bytes = w.data_bytes;
  s_byte_rate = w.rate * w.channels * 2;
  s_pos_bytes = 0;
  return start(play_task, AUDIO_PLAYING, "play");
}

void audio_stop(void) { s_stop = 1; }
int  audio_state(void) { return s_state; }
int  audio_level(void) { return s_state == AUDIO_RECORDING ? s_level : -1; }

uint32_t audio_pos_ms(void) {
  return s_byte_rate ? (uint32_t)((uint64_t)s_pos_bytes * 1000 / s_byte_rate) : 0;
}
uint32_t audio_total_ms(void) {
  return s_byte_rate ? (uint32_t)((uint64_t)s_total_bytes * 1000 / s_byte_rate) : 0;
}
int audio_last_bytes(void) { return s_last_bytes; }
const char *audio_error(void) { return s_error; }
