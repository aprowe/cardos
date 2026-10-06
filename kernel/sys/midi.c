/* MIDI out and a player. See midi.h. */
#include "kernel/sys/midi.h"
#include "kernel/app/capprun.h"

#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PORT      UART_NUM_1
#define BAUD      31250
#define MAX_EV    4096
#define T_STACK   3072
#define T_PRIO    6           /* above the shell: a note late is a note wrong */

static const char *TAG = "midi";

static int        s_open = 0;
static int        s_pin = -1;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static MidiEvent *s_ev;
static int        s_n;
static uint32_t   s_loop_ms;
static volatile int s_stop;
static volatile int s_playing;
static volatile int64_t s_start_us;      /* this round's start */
static const void *s_owner;
/* What is sounding: a bit per channel and note, so a stop can end it. */
static uint8_t    s_on[16][16];

int midi_open(int tx_pin) {
  uart_config_t cfg;
  if (s_open && s_pin == tx_pin) return 0;
  if (s_open) midi_close();
  memset(&cfg, 0, sizeof cfg);
  cfg.baud_rate = BAUD;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  if (uart_driver_install(PORT, 256, 1024, 0, NULL, 0) != ESP_OK) return -1;
  if (uart_param_config(PORT, &cfg) != ESP_OK ||
      uart_set_pin(PORT, tx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    uart_driver_delete(PORT);
    return -1;
  }
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  s_open = 1;
  s_pin = tx_pin;
  s_owner = capprun_caller();           /* closed with the app that opened it */
  ESP_LOGI(TAG, "out on G%d at %d baud", tx_pin, BAUD);
  return 0;
}

static void track(const uint8_t *b, int n) {
  int ch, note;
  if (n < 3) return;
  ch = b[0] & 0x0F;
  note = b[1] & 0x7F;
  if ((b[0] & 0xF0) == 0x90 && b[2]) s_on[ch][note >> 3] |= (uint8_t)(1 << (note & 7));
  else if ((b[0] & 0xF0) == 0x80 || (b[0] & 0xF0) == 0x90)
    s_on[ch][note >> 3] &= (uint8_t)~(1 << (note & 7));
}

int midi_send(const uint8_t *b, int n) {
  if (!s_open || n <= 0) return -1;
  track(b, n);
  return uart_write_bytes(PORT, b, (size_t)n) == n ? 0 : -1;
}

/* Everything sounding off, then all-notes-off on every channel: a synth
 * that missed a note-off still goes quiet. */
static void silence(void) {
  int ch, k;
  uint8_t m[3];
  for (ch = 0; ch < 16; ch++) {
    for (k = 0; k < 128; k++)
      if (s_on[ch][k >> 3] & (1 << (k & 7))) {
        m[0] = (uint8_t)(0x80 | ch); m[1] = (uint8_t)k; m[2] = 0;
        midi_send(m, 3);
      }
    m[0] = (uint8_t)(0xB0 | ch); m[1] = 123; m[2] = 0;
    midi_send(m, 3);
  }
  memset(s_on, 0, sizeof s_on);
}

static void play_task(void *arg) {
  int i = 0;
  (void)arg;
  s_start_us = esp_timer_get_time();
  while (!s_stop) {
    int64_t now = esp_timer_get_time(), due;
    if (i >= s_n) {
      if (!s_loop_ms) break;
      /* The next round starts at loop_ms, not when the last event went. */
      due = s_start_us + (int64_t)s_loop_ms * 1000;
      if (now < due) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
      s_start_us = due;
      i = 0;
      continue;
    }
    due = s_start_us + (int64_t)s_ev[i].at_ms * 1000;
    if (now + 1500 < due) {                 /* more than a tick and a half away: sleep */
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }
    while (esp_timer_get_time() < due) ;    /* the last moment, spun */
    midi_send(s_ev[i].b, s_ev[i].n);
    i++;
  }
  silence();
  s_playing = 0;
  s_task = NULL;
  vTaskDelete(NULL);
}

void midi_stop(void) {
  int waited = 0;
  if (!s_task) return;
  s_stop = 1;
  while (s_task && waited < 200) { vTaskDelay(pdMS_TO_TICKS(2)); waited += 2; }
}

int midi_play(const MidiEvent *ev, int n, uint32_t loop_ms) {
  MidiEvent *copy;
  if (!s_open) return -1;
  if (n < 0) n = 0;
  if (n > MAX_EV) n = MAX_EV;
  midi_stop();
  copy = (MidiEvent *)malloc((size_t)(n ? n : 1) * sizeof *copy);
  if (!copy) return -2;
  if (n) memcpy(copy, ev, (size_t)n * sizeof *copy);
  free(s_ev);
  s_ev = copy;
  s_n = n;
  s_loop_ms = loop_ms;
  s_stop = 0;
  s_playing = 1;
  s_owner = capprun_caller();
  if (xTaskCreatePinnedToCore(play_task, "midi", T_STACK, NULL, T_PRIO, &s_task, 1) != pdPASS) {
    s_playing = 0;
    s_task = NULL;
    return -2;
  }
  return 0;
}

int midi_playing(void) { return s_playing; }

uint32_t midi_pos_ms(void) {
  if (!s_playing) return 0;
  return (uint32_t)((esp_timer_get_time() - s_start_us) / 1000);
}

void midi_close(void) {
  midi_stop();
  if (!s_open) return;
  silence();
  uart_wait_tx_done(PORT, pdMS_TO_TICKS(100));
  uart_driver_delete(PORT);
  s_open = 0;
  s_pin = -1;
  free(s_ev);
  s_ev = NULL;
  s_n = 0;
}

void midi_release_owner(const void *owner) {
  if (owner && owner == s_owner) {
    midi_close();
    s_owner = NULL;
  }
}
