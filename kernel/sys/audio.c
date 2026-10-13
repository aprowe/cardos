/* Recording and playback on a task. See audio.h. */
#include "kernel/sys/audio.h"
#include "kernel/drv/mic.h"
#include "kernel/drv/speaker.h"
#include "kernel/fs/fs.h"
#include "kernel/sys/mp3stream.h"
#include "kernel/app/capprun.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/* The mic's and the speaker's 1 KB blocks live on this stack, with the
 * FAT driver's own needs under them. 6 KB has headroom. */
#define A_STACK    6144
#define A_PRIORITY 5
/* An MP3's decoder and scratch are on the heap (two blocks, taken in
 * audio_play), so its task's stack is a WAV's. */
#define MP3_STACK  A_STACK

static volatile int      s_state;
static volatile int      s_stop;
static volatile int      s_paused;
static volatile int32_t  s_seek = -1;      /* a time asked for, in ms, or -1 */
static volatile int      s_level = -1;
static volatile uint32_t s_pos_ms;
static uint32_t          s_total_ms;
static uint32_t          s_byte_rate;      /* a WAV's bytes per second */
static Mp3Info           s_mp3;            /* the MP3 playing */
static void             *s_mp3_work, *s_mp3_scratch;   /* its, while it plays */
static int               s_last_bytes = -1;
static char              s_path[128];
static int               s_max_ms;
static char              s_error[48];

/* Who else has the audio: the voice button, recording from the shell
 * outside any job here. NULL when nobody. Under s_mux with s_state, so a
 * claim and a job cannot both start. */
static const char       *s_claim;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Make it ours for `state`, or say who has it. */
static int take(int state) {
  int ok;
  portENTER_CRITICAL(&s_mux);
  ok = s_state == AUDIO_IDLE && !s_claim;
  if (ok) s_state = state;
  portEXIT_CRITICAL(&s_mux);
  return ok ? 0 : -1;
}

int audio_claim(const char *who) {
  int ok;
  portENTER_CRITICAL(&s_mux);
  ok = s_state == AUDIO_IDLE && !s_claim;
  if (ok) s_claim = who ? who : "someone";
  portEXIT_CRITICAL(&s_mux);
  return ok ? 0 : -1;
}

void audio_release(void) {
  portENTER_CRITICAL(&s_mux);
  s_claim = NULL;
  portEXIT_CRITICAL(&s_mux);
}

const char *audio_holder(void) {
  const char *c = s_claim;
  if (c) return c;
  return s_state == AUDIO_RECORDING ? "a recording" :
         s_state == AUDIO_PLAYING ? "playback" : NULL;
}

static int  stop_cb(void) { return s_stop; }
static int  paused_cb(void) { return s_paused; }
static int32_t seek_ms_cb(void) { int32_t s = s_seek; s_seek = -1; return s; }
/* A WAV seeks and counts in bytes. */
static int32_t seek_cb(void) {
  int32_t s = seek_ms_cb();
  return s < 0 ? -1 : (int32_t)((uint64_t)(uint32_t)s * s_byte_rate / 1000);
}
static void level_cb(int pct) { s_level = pct; }
static void progress_cb(uint32_t bytes) {
  if (s_byte_rate) s_pos_ms = (uint32_t)((uint64_t)bytes * 1000 / s_byte_rate);
}
static void progress_ms_cb(uint32_t ms) { s_pos_ms = ms; }

static void record_task(void *param) {
  int n;
  (void)param;
  n = mic_record_wav(s_path[0] ? s_path : NULL, s_max_ms, stop_cb, level_cb);
  s_last_bytes = n;
  if (n == -2) snprintf(s_error, sizeof s_error, "%s", "the mic is in use");
  else if (n < 0) snprintf(s_error, sizeof s_error, "%s", "the mic or the card refused");
  ESP_LOGI(TAG, "recorded %d bytes to %s", n, s_path);
  s_level = -1;
  s_state = AUDIO_IDLE;
  vTaskDelete(NULL);
}

static void play_task(void *param) {
  (void)param;
  if (speaker_play_wav_ex(s_path, stop_cb, progress_cb, paused_cb, seek_cb) != 0)
    snprintf(s_error, sizeof s_error, "%s", speaker_error());
  ESP_LOGI(TAG, "played %u of %u ms of %s", (unsigned)s_pos_ms,
           (unsigned)s_total_ms, s_path);
  s_paused = 0;
  s_state = AUDIO_IDLE;
  vTaskDelete(NULL);
}

/* ---- MP3: the card in, the speaker out (kernel/sys/mp3stream.c) ---- */

static void mp3_free(void);

static int fd_read(void *ctx, uint8_t *buf, int n) { return fs_read(*(int *)ctx, buf, (size_t)n); }
static int fd_seek(void *ctx, uint32_t off) {
  return fs_seek(*(int *)ctx, (int32_t)off, FS_SEEK_SET) < 0 ? -1 : 0;
}
static int out_cb(void *ctx, int16_t *pcm, int n) { (void)ctx; return speaker_stream_write(pcm, n); }
static int hush_cb(void *ctx) { (void)ctx; return speaker_stream_hush(); }

static int mp3_open(const char *path, int *fd, Mp3Src *src) {
  FsStat st;
  if (fs_stat(path, &st) != 0) return -1;
  *fd = fs_open(path, FS_O_READ);
  if (*fd < 0) return -1;
  src->ctx = fd; src->read = fd_read; src->seek = fd_seek; src->size = st.size;
  return 0;
}

static void mp3_task(void *param) {
  int fd = -1;
  Mp3Src src;
  Mp3Hooks h = { NULL, out_cb, hush_cb, stop_cb, paused_cb, seek_ms_cb, progress_ms_cb };
  (void)param;
  if (mp3_open(s_path, &fd, &src) != 0) snprintf(s_error, sizeof s_error, "%s", "cannot open");
  else if (speaker_stream_open(s_mp3.rate) != 0) snprintf(s_error, sizeof s_error, "%s", speaker_error());
  else if (mp3_play(&src, &s_mp3, s_mp3_work, s_mp3_scratch, &h) != 0)
    snprintf(s_error, sizeof s_error, "%s", speaker_error()[0] ? speaker_error() : "the card would not read");
  speaker_stream_close();
  if (fd >= 0) fs_close(fd);
  mp3_free();
  ESP_LOGI(TAG, "played %u of %u ms of %s (stack spare %u)", (unsigned)s_pos_ms,
           (unsigned)s_total_ms, s_path, (unsigned)uxTaskGetStackHighWaterMark(NULL));
  s_paused = 0;
  s_state = AUDIO_IDLE;
  vTaskDelete(NULL);
}

static void mp3_free(void) {
  free(s_mp3_work);
  free(s_mp3_scratch);
  s_mp3_work = s_mp3_scratch = NULL;
}

static int is_mp3(const char *path) {
  size_t n = strlen(path);
  return n > 4 && path[n - 4] == '.' && (path[n - 3] | 32) == 'm' &&
         (path[n - 2] | 32) == 'p' && path[n - 1] == '3';
}

/* Called with s_state already set by take(). */
static int start(void (*task)(void *), const char *name, uint32_t stack, int core) {
  s_stop = 0;
  s_paused = 0;
  s_seek = -1;
  s_error[0] = 0;
  if (xTaskCreatePinnedToCore(task, name, stack, NULL, A_PRIORITY, NULL, core) != pdPASS) {
    s_state = AUDIO_IDLE;
    snprintf(s_error, sizeof s_error, "%s", "no memory for the task");
    return -2;
  }
  return 0;
}

int audio_record(const char *path, int max_ms) {
  if (path && !*path) return -2;
  if (take(AUDIO_RECORDING) != 0) return -1;
  snprintf(s_path, sizeof s_path, "%s", path ? path : "");   /* "" listens */
  s_max_ms = max_ms;
  s_level = 0;
  s_last_bytes = -1;
  return start(record_task, "record", A_STACK, 0);
}

int audio_play(const char *path) {
  WavInfo w;
  const char *why = "";
  if (take(AUDIO_PLAYING) != 0) return -1;
  s_pos_ms = 0;
  if (path && is_mp3(path)) {
    /* Probed here, so a file that is not one says so at once and the
     * length is known before the first frame. */
    int fd = -1, r = -1;
    Mp3Src src;
    uint8_t *buf = malloc(4096);
    why = "not enough memory";
    if (buf && mp3_open(path, &fd, &src) == 0) r = mp3_probe(&src, &s_mp3, buf, 4096, &why);
    else if (buf) why = "cannot open";
    if (fd >= 0) fs_close(fd);
    free(buf);
    if (r != 0) {
      snprintf(s_error, sizeof s_error, "%s", why);
      s_state = AUDIO_IDLE;
      return -2;
    }
    /* The decoder is 29 KB, and with WiFi up (52) and an app's fonts
     * loaded that left a few KB of heap in pieces: the serial link could
     * not take a frame, and a notify poll would have failed. So an MP3
     * lets go of the radios first, as a print does -- not WiFi under a
     * request, not Bluetooth under a print or a link. They come back
     * when something next wants them (Music's sync, an app that needs
     * the network); meanwhile nothing polls for notifications. */
    capprun_make_room();
    /* The bigger first, while the heap's biggest piece is still whole. */
    s_mp3_scratch = malloc(mp3_scratch_bytes());
    s_mp3_work = s_mp3_scratch ? malloc(mp3_work_bytes()) : NULL;
    if (!s_mp3_work) {
      ESP_LOGW(TAG, "an MP3 wants %u + %u bytes; largest free %u", (unsigned)mp3_scratch_bytes(),
               (unsigned)mp3_work_bytes(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      mp3_free();
      snprintf(s_error, sizeof s_error, "%s", "not enough memory for an MP3");
      s_state = AUDIO_IDLE;
      return -2;
    }
    snprintf(s_path, sizeof s_path, "%s", path);
    s_total_ms = s_mp3.ms;
    s_byte_rate = 0;
    /* Core 1, as MIDI's player: decoding is real work, a frame every 26
     * ms, and on core 0 above the shell it starved it -- the music played
     * and the screen stood still. */
    if (start(mp3_task, "mp3", MP3_STACK, 1) != 0) { mp3_free(); return -2; }
    return 0;
  }
  if (!path || speaker_wav_info(path, &w, &why) != 0) {
    snprintf(s_error, sizeof s_error, "%s", why);
    s_state = AUDIO_IDLE;
    return -2;
  }
  snprintf(s_path, sizeof s_path, "%s", path);
  s_byte_rate = w.rate * w.channels * 2;
  s_total_ms = (uint32_t)((uint64_t)w.data_bytes * 1000 / s_byte_rate);
  return start(play_task, "play", A_STACK, 0);
}

void audio_stop(void) { s_stop = 1; }

void audio_pause(int on) { if (s_state == AUDIO_PLAYING) s_paused = on ? 1 : 0; }
int  audio_paused(void) { return s_state == AUDIO_PLAYING && s_paused; }

void audio_seek_ms(uint32_t ms) {
  if (s_state != AUDIO_PLAYING) return;
  s_seek = (int32_t)ms;
}
int  audio_state(void) { return s_state; }
int  audio_level(void) { return s_state == AUDIO_RECORDING ? s_level : -1; }

uint32_t audio_pos_ms(void) { return s_pos_ms; }
uint32_t audio_total_ms(void) { return s_total_ms; }
int audio_last_bytes(void) { return s_last_bytes; }
const char *audio_error(void) { return s_error; }
