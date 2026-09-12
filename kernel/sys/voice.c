/* Push to talk, and what the words turn into. See voice.h. */

#include "kernel/sys/voice.h"

#include "kernel/sys/input.h"
#include "kernel/sys/rpc.h"
#include "kernel/sys/env.h"
#include "kernel/drv/mic.h"
#include "kernel/drv/display.h"
#include "kernel/net/http.h"
#include "kernel/net/wifi.h"
#include "kernel/ui/shell.h"
#include "kernel/ui/overlay.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The button on the top edge. Also the ROM downloader's button, which is why
 * it is read here and not at boot: held during reset it never reaches us. */
#define PIN_GO 0

#define WAV_PATH   "/cache/voice.wav"
#define PROXY_DEFAULT "http://192.168.1.74:8080"

static const char *TAG = "voice";

static int  s_ready;
static int  s_recording;
static int  s_level;
static char s_status[96];

/* The button is active low and mechanical, so it bounces. 30 ms of agreement
 * either way is well under the time it takes to decide to speak. */
static int  s_down;
static int64_t s_edge_us;

static int button_raw(void) { return gpio_get_level(PIN_GO) == 0; }

static void setup(void) {
  gpio_config_t cfg = {
    .pin_bit_mask = 1ULL << PIN_GO,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  s_ready = 1;
}

int voice_recording(void) { return s_recording; }
int voice_level(void) { return s_level; }
const char *voice_status(void) { return s_status[0] ? s_status : "idle"; }

static void say(const char *s) { snprintf(s_status, sizeof s_status, "%s", s); }

static void level_cb(int pct) {
  s_level = pct;
  /* Drawn from inside the recording loop, every 32 ms, because that loop is
   * where the machine is while someone is talking. A meter that only moved
   * when the recording ended would be a picture of a meter. */
  overlay_listening(pct);
}

static void send_cb(int sent, int total) {
  overlay_sending(total > 0 ? (int)((int64_t)sent * 100 / total) : 0);
}

/* Recording stops when the button comes back up. Read raw rather than
 * debounced: by the time a finger is lifting, the press is long established
 * and a stray high reading means the same thing as a settled one. */
static int stop_cb(void) { return !button_raw(); }

static const char *base_url(void) {
  const char *p = env_get("PROXY");
  return (p && p[0]) ? p : PROXY_DEFAULT;
}

/* ---- what to do with what was heard --------------------------------------- */

/* Execute one line of the RPC vocabulary. Everything here is a verb the device
 * already had a way to do; the model chooses between them and never supplies
 * anything but the choice. */
static void run_command(const char *line) {
  RpcCmd c;

  if (!rpc_parse(line, &c)) {
    snprintf(s_status, sizeof s_status, "not a command: %.40s", c.arg);
    return;
  }

  switch (c.verb) {
  case RPC_OPEN:
    /* "notes" is what a person calls the editor. */
    if (!strcmp(c.arg, "notes")) snprintf(c.arg, sizeof c.arg, "%s", "edit");
    if (shell_open_app(c.arg) == 0)
      snprintf(s_status, sizeof s_status, "opened %.40s", c.arg);
    else
      snprintf(s_status, sizeof s_status, "no app called %.40s", c.arg);
    break;

  case RPC_SHELL:
    shell_switch(c.arg);
    snprintf(s_status, sizeof s_status, "%.40s", c.arg);
    break;

  case RPC_BRIGHT:
    display_set_brightness(c.num);
    snprintf(s_status, sizeof s_status, "brightness %d%%", display_brightness());
    break;

  case RPC_WIFI:
    if (c.num) { wifi_connect_saved(20000); say(wifi_status()); }
    else { wifi_stop(); say("wifi off"); }
    break;

  case RPC_SAY:
    if (input_text(c.arg) > 0) say("typed");
    else say("nothing here is taking text");
    break;

  case RPC_KEY: {
    /* The names the model is allowed to use, mapped to what the keyboard
     * would have produced. */
    uint8_t k = 0;
    if (!strcmp(c.arg, "escape")) k = 0x1B;
    else if (!strcmp(c.arg, "enter")) k = 0x0D;
    else if (!strcmp(c.arg, "up")) k = 0x80;
    else if (!strcmp(c.arg, "down")) k = 0x81;
    else if (!strcmp(c.arg, "left")) k = 0x82;
    else if (!strcmp(c.arg, "right")) k = 0x83;
    if (k) { shell_feed_key(k); say(c.arg); }
    break;
  }

  case RPC_NONE:
    snprintf(s_status, sizeof s_status, "%.60s", c.arg[0] ? c.arg : "not understood");
    break;

  default:
    say("not a command");
    break;
  }
}

/* Send the recording and act on the answer.
 *
 * The reply is one line: "text ...", "cmd ...", or "error ...". A prefix
 * rather than JSON because the device has no parser and this needs none -- and
 * because the three cases are genuinely different things, not fields. */
static void send_and_act(void) {
  static char reply[600];
  char url[160];
  int n;

  snprintf(url, sizeof url, "%s/voice", base_url());
  say("recognising...");

  n = http_post_file_progress(url, WAV_PATH, "audio/wav", reply, sizeof reply,
                              60000, send_cb);
  /* The upload finished; whatever happens next has no progress to report --
   * whisper is chewing, or a model is deciding what the sentence meant. */
  overlay_working("recognising");
  if (n < 0) {
    snprintf(s_status, sizeof s_status, "server did not answer (%d)", n);
    return;
  }

  {
    char *body = reply;
    while (*body && *body != ' ') body++;
    if (*body == ' ') *body++ = 0;
    /* Trim the newline the server ends with. */
    {
      size_t l = strlen(body);
      while (l && (body[l - 1] == '\n' || body[l - 1] == '\r')) body[--l] = 0;
    }

    if (!strcmp(reply, "text")) {
      if (input_text(body) > 0) snprintf(s_status, sizeof s_status, "%.60s", body);
      else snprintf(s_status, sizeof s_status, "heard, but nothing is taking text");
    } else if (!strcmp(reply, "cmd")) {
      run_command(body);
    } else {
      snprintf(s_status, sizeof s_status, "%.60s", body);
    }
  }
}

void voice_once(int max_ms) {
  int bytes;

  if (!wifi_is_connected()) {
    overlay_working("joining wifi");
    if (wifi_connect_saved(20000) != 0) {
      say(wifi_status());
      overlay_result(s_status);
      vTaskDelay(pdMS_TO_TICKS(2000));
      overlay_close();
      return;
    }
  }

  s_recording = 1;
  s_level = 0;
  say("listening");
  bytes = mic_record_wav(WAV_PATH, max_ms, stop_cb, level_cb);
  s_recording = 0;

  if (bytes < 0) {
    say("the microphone did not start");
    overlay_result(s_status);
    vTaskDelay(pdMS_TO_TICKS(1500));
    overlay_close();
    return;
  }
  /* Under a third of a second is a mis-press, not a sentence. Taken down
   * quickly, because a mis-press does not deserve a message to dismiss. */
  if (bytes < MIC_RATE * 2 / 3) {
    say("too short");
    overlay_close();
    return;
  }

  ESP_LOGI(TAG, "%d bytes recorded", bytes);
  send_and_act();

  /* Long enough to read, then the screen goes back to whatever it was. Two
   * seconds is about how long it takes to check that a machine understood
   * you, and short enough not to be in the way. */
  overlay_result(voice_status());
  vTaskDelay(pdMS_TO_TICKS(2000));
  overlay_close();
}

int voice_tick(void) {
  int now_down;
  int64_t us = esp_timer_get_time();

  if (!s_ready) setup();

  now_down = button_raw();
  if (now_down == s_down) return 0;
  if (us - s_edge_us < 30000) return 0;      /* still bouncing */
  s_edge_us = us;
  s_down = now_down;

  if (!now_down) return 0;                   /* released: recording ended */

  /* Pressed. Everything -- recording, upload, acting -- happens inside this
   * call, which blocks for as long as the button is held plus a second or
   * two. That is deliberate: while someone is talking to the machine, there
   * is nothing else for it to be doing, and the alternative is a state
   * machine spread across three subsystems for no gain. */
  voice_once(MIC_MAX_MS);
  return 1;
}
