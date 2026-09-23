/* Push to talk, and what the words turn into. See voice.h. */

#include "kernel/app/capp.h"   /* CAPP_PROXY_DEFAULT */
#include "kernel/sys/voice.h"

#include "kernel/sys/input.h"
#include "kernel/sys/rpc.h"
#include "kernel/sys/agent.h"
#include "kernel/sys/env.h"
#include "kernel/drv/mic.h"
#include "kernel/drv/display.h"
#include "kernel/net/http.h"
#include "kernel/net/update.h"
#include "kernel/net/wifi.h"
#include "kernel/ui/shell.h"
#include "kernel/ui/overlay.h"
#include "kernel/sys/g0gesture.h"
#include "kernel/sys/applog.h"
#include "kernel/sys/clock.h"
#include "kernel/fs/fs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* The button on the top edge. Also the ROM downloader's button, which is why
 * it is read here and not at boot: held during reset it never reaches us. */
#define PIN_GO 0

#define WAV_PATH   "/cache/voice.wav"

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

/* Hold to talk; tap, then hold, for a memo. See kernel/sys/g0gesture.h. */
static G0Gesture s_g0;
static uint32_t  s_press_ms;         /* when this press went down */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void level_cb(int pct) {
  s_level = pct;
  /* Drawn from inside the recording loop, every 32 ms, because that loop is
   * where the machine is while someone is talking. A meter that only moved
   * when the recording ended would be a picture of a meter.
   *
   * Not for the first G0_TAP_MS of a press: until then it may be the tap
   * before a memo, and a panel that flashed up and away on every tap would
   * be noise. */
  if (now_ms() - s_press_ms < G0_TAP_MS) return;
  overlay_listening(pct);
}

static void memo_level_cb(int pct) {
  s_level = pct;
  overlay_memo(pct, (int)((now_ms() - s_press_ms) / 1000));
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
  return (p && p[0]) ? p : CAPP_PROXY_DEFAULT;
}

/* ---- what to do with what was heard --------------------------------------- */

/* Execute one line of the RPC vocabulary. The verbs themselves live in
 * kernel/sys/agent.c now, where the on-device Claude runs the same list
 * through the same function; this only parses the line and shows what
 * happened. */
static void run_command(const char *line) {
  RpcCmd c;

  if (!rpc_parse(line, &c)) {
    snprintf(s_status, sizeof s_status, "not a command: %.40s", c.arg);
    return;
  }
  agent_execute(&c, s_status, sizeof s_status);
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

  /* The proxy may be remote and behind the device's shared secret. */
  {
    const char *t = update_token();
    n = http_post_file_progress(url, WAV_PATH, "audio/wav", *t ? t : NULL,
                                reply, sizeof reply, 60000, send_cb);
  }
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

void voice_once(int max_ms, int hold) {
  int bytes;

  /* Recording first, the network after. It used to join WiFi before
   * listening, so a sentence waited up to twenty seconds for a radio -- and
   * a tap, which is how a memo starts, would have too. */
  s_recording = 1;
  s_level = 0;
  s_press_ms = now_ms();
  say(hold ? "listening" : "listening (timed)");
  bytes = mic_record_wav(WAV_PATH, max_ms, hold ? stop_cb : (int (*)(void))0,
                         level_cb);
  s_recording = 0;

  /* A tap: the first half of tap-then-hold. Nothing to say and nothing to
   * send; the next press decides. */
  if (hold && g0_release(&s_g0, now_ms() - s_press_ms, now_ms())) {
    say("tap");
    overlay_close();
    return;
  }

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

  ESP_LOGI(TAG, "%d bytes recorded", bytes);
  send_and_act();

  /* Long enough to read, then the screen goes back to whatever it was. Two
   * seconds is about how long it takes to check that a machine understood
   * you, and short enough not to be in the way. */
  overlay_result(voice_status());
  vTaskDelay(pdMS_TO_TICKS(2000));
  overlay_close();
}

/* ---- memos: tap, then hold ------------------------------------------------ */

#define MEMO_DIR "/home/memos"

/* One past the highest mNNNN.wav in the folder, for a device that does not
 * know the time -- the same rule apps/memo.c follows, so the two agree. */
static int next_counter(void) {
  FsDir d;
  FsEntry e;
  int high = 0;
  if (fs_opendir(MEMO_DIR, &d) != 0) return 1;
  while (fs_readdir(&d, &e) == 1) {
    const char *p = e.name;
    int v = 0;
    if (p[0] != 'm') continue;
    for (p++; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (v > high) high = v;
  }
  fs_closedir(&d);
  return high + 1;
}

/* Record straight to the card for as long as G0 is held. No network, no
 * server, no app: it works in any shell, over any app, offline. */
static void memo_once(void) {
  char path[64], line[80];
  const char *name;
  FsStat st;
  int bytes, secs;

  if (fs_stat(MEMO_DIR, &st) != 0) fs_mkdir(MEMO_DIR);
  if (clock_have_time()) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    memo_filename(path, sizeof path, MEMO_DIR, 1, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, 0);
  } else {
    memo_filename(path, sizeof path, MEMO_DIR, 0, 0, 0, 0, 0, 0, next_counter());
  }
  name = strrchr(path, '/') + 1;

  s_recording = 1;
  s_level = 0;
  s_press_ms = now_ms();
  say("recording a memo");
  overlay_memo(0, 0);
  bytes = mic_record_wav(path, MIC_HARD_MAX_MS, stop_cb, memo_level_cb);
  s_recording = 0;

  if (bytes < 0) {
    say("the microphone did not start");
    overlay_memo_done(s_status);
  } else if (bytes < MIC_RATE) {       /* under half a second: a slip */
    fs_remove(path);
    say("too short -- nothing saved");
    overlay_memo_done(s_status);
  } else {
    secs = bytes / (MIC_RATE * 2);
    snprintf(line, sizeof line, "saved %s (%d:%02d)", name, secs / 60, secs % 60);
    say(line);
    overlay_memo_done(line);
    applogf("memo", "%s, %d s", name, secs);
  }
  vTaskDelay(pdMS_TO_TICKS(1500));
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
  if (g0_press(&s_g0, now_ms()) == G0_MEMO) memo_once();
  else voice_once(MIC_MAX_MS, 1);
  return 1;
}
