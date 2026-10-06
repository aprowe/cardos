/* The OS's small sounds. See blip.h. */
#include "kernel/sys/blip.h"
#include "kernel/sys/audio.h"
#include "kernel/sys/prefs.h"
#include "kernel/drv/speaker.h"
#include "kernel/drv/mic.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define RATE      16000
#define MAX_MS    130
#define MAX_N     (RATE * MAX_MS / 1000)
#define T_STACK   3072
#define T_PRIO    3           /* below the shell (5): a sound is never urgent */

/* A sound is up to four notes, one after another. A note: a frequency, how
 * long, and how loud relative to the others; frequency 0 is a rest. `wave`
 * 0 is a sine, 1 a soft square (for the error buzz, which should not sound
 * like the others). The whole sound is scaled by `level` %, of the volume
 * setting. */
typedef struct { uint16_t hz, ms; uint8_t amp; } Note;
typedef struct { uint8_t level, wave, n; Note note[4]; } Sound;

static const Sound SOUNDS[BLIP_COUNT] = {
  [BLIP_KEY]    = { 50, 0, 1, { { 2200, 10, 100 } } },
  [BLIP_MOVE]   = { 28, 0, 1, { { 1760, 14, 100 } } },
  [BLIP_SOFT]   = { 30, 0, 1, { { 1320, 22, 100 } } },
  [BLIP_OPEN]   = { 38, 0, 2, { {  784, 30, 90 }, { 1175, 50, 100 } } },
  [BLIP_BACK]   = { 34, 0, 2, { { 1175, 30, 100 }, {  784, 50, 90 } } },
  [BLIP_ERROR]  = { 34, 1, 3, { {  196, 50, 100 }, {    0, 25, 0 }, { 196, 50, 100 } } },
  [BLIP_DONE]   = { 38, 0, 3, { {  1047, 35, 90 }, { 1319, 35, 95 }, { 1568, 55, 100 } } },
  [BLIP_NOTIFY] = { 42, 0, 2, { {  1319, 45, 100 }, { 1760, 70, 100 } } },
  [BLIP_BOOT]   = { 40, 0, 3, { {  523, 40, 90 }, { 784, 40, 95 }, { 1047, 50, 100 } } },
};

static QueueHandle_t s_q;
static int s_ui = -1, s_keys = -1;

int blip_ui_on(void) {
  if (s_ui < 0) s_ui = prefs_get_u16("ui_snd", 1) != 0;
  return s_ui;
}
void blip_set_ui(int on) { s_ui = on != 0; prefs_set_u16("ui_snd", s_ui); }
int blip_keys_on(void) {
  if (s_keys < 0) s_keys = prefs_get_u16("key_snd", 1) != 0;
  return s_keys;
}
void blip_set_keys(int on) { s_keys = on != 0; prefs_set_u16("key_snd", s_keys); }

/* The sound as samples: each note with a 1 ms rise and an exponential fall,
 * so it is a tap rather than a tone that switches on and off; the phase is
 * carried from note to note so a two-note rise has no click in the middle. */
static int render(const Sound *s, int16_t *out, int gain) {
  int i, k, n = 0;
  float phase = 0;
  for (k = 0; k < s->n; k++) {
    const Note *nt = &s->note[k];
    int len = RATE * nt->ms / 1000, attack = RATE / 1000;
    float step = 6.2831853f * nt->hz / RATE, decay = expf(-5.0f / (len ? len : 1));
    float env = 1.0f, amp = (float)gain * nt->amp / 100.0f;
    if (n + len > MAX_N) len = MAX_N - n;
    for (i = 0; i < len; i++, n++) {
      float v = 0;
      if (nt->hz) {
        v = sinf(phase);
        if (s->wave == 1) v = v > 0.2f ? 0.7f : v < -0.2f ? -0.7f : v * 3.5f;
        phase += step;
        if (phase > 6.2831853f) phase -= 6.2831853f;
      }
      out[n] = (int16_t)(v * amp * env * (i < attack ? (float)i / attack : 1.0f));
      if (i >= attack) env *= decay;
    }
  }
  return n;
}

static void task(void *arg) {
  static int16_t pcm[MAX_N];
  uint8_t which;
  (void)arg;
  for (;;) {
    if (xQueueReceive(s_q, &which, portMAX_DELAY) != pdTRUE) continue;
    vTaskDelay(pdMS_TO_TICKS(4));    /* a key's click, then its move: the move */
    /* One sound for a burst: twenty key clicks queued behind each other
     * would still be ticking after the typing stopped. The newest, unless
     * it is only a click and something said more. */
    while (uxQueueMessagesWaiting(s_q)) {
      uint8_t next;
      if (xQueueReceive(s_q, &next, 0) != pdTRUE) break;
      if (next != BLIP_KEY || which == BLIP_KEY) which = next;
    }
    if (which >= BLIP_COUNT) continue;
    if (audio_state() != AUDIO_IDLE || mic_is_open()) continue;
    {
      const Sound *s = &SOUNDS[which];
      int gain = 32767 * speaker_volume() / 100 * s->level / 100;
      int n;
      if (!gain) continue;
      n = render(s, pcm, gain);
      speaker_play_pcm(pcm, n, RATE);
    }
  }
}

void blip_init(void) {
  if (s_q) return;
  s_q = xQueueCreate(4, 1);
  if (s_q) xTaskCreate(task, "blip", T_STACK, NULL, T_PRIO, NULL);
}

void blip(Blip which) {
  uint8_t w = (uint8_t)which;
  if (!s_q || !blip_ui_on()) return;
  if (which == BLIP_KEY && !blip_keys_on()) return;
  xQueueSend(s_q, &w, 0);          /* full: this one is dropped, not waited for */
}
