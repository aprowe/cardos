/* Alarms ringing. See alarm.h; the format is alarmfmt.h. */

#include "kernel/sys/alarm.h"
#include "kernel/sys/alarmfmt.h"
#include "kernel/sys/audio.h"
#include "kernel/sys/clock.h"
#include "kernel/sys/power.h"
#include "kernel/sys/applog.h"
#include "kernel/fs/fs.h"
#include "kernel/ui/draw.h"
#include "kernel/ui/fontres.h"
#include "kernel/drv/display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"

#define RING_MAX_MS   (5 * 60 * 1000)   /* unanswered, it gives up */
#define BEEP_EVERY_MS 1500
#define FLASH_MS      500
#define BEEP_PATH     "/cache/alarm.wav"

#define A_BACK   RGB565(14, 16, 24)
#define A_EDGE   RGB565(255, 176, 76)
#define A_TEXT   RGB565(240, 244, 250)
#define A_SUB    RGB565(150, 160, 180)
#define A_HOT    RGB565(230, 92, 70)

static int      s_ringing;
static uint32_t s_ring_at, s_beep_at, s_flash_at;
static int      s_flash;
static int      s_last_key = -1;          /* the minute last checked, as a number */
static Alarm    s_alarm;                  /* the one ringing */
static void   (*s_repaint)(void);
static int      s_font_big = -1, s_font_ui = -1;
static char     s_next[40];
static const char s_owner = 0;            /* an identity for the hold and the fonts */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void alarm_set_repaint(void (*repaint)(void)) { s_repaint = repaint; }
int  alarm_ringing(void) { return s_ringing; }
const char *alarm_next_text(void) { return s_next; }

/* ---- the file ---------------------------------------------------------- */

static int read_file(char *buf, int size) {
  int fd = fs_open(ALARM_FILE, FS_O_READ), n;
  if (fd < 0) {
    /* a save stranded by a power cut: finish it (apps/safefile.h's rule) */
    if (fs_rename(ALARM_FILE ".tmp", ALARM_FILE) != 0) return 0;
    if ((fd = fs_open(ALARM_FILE, FS_O_READ)) < 0) return 0;
  }
  n = fs_read(fd, buf, (size_t)(size - 1));
  fs_close(fd);
  if (n < 0) n = 0;
  buf[n] = 0;
  return n;
}

static void write_file(const char *text, size_t len) {
  int fd = fs_open(ALARM_FILE ".tmp", FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) return;
  if (fs_write(fd, text, len) != (int)len) { fs_close(fd); fs_remove(ALARM_FILE ".tmp"); return; }
  fs_close(fd);
  fs_remove(ALARM_FILE);
  fs_rename(ALARM_FILE ".tmp", ALARM_FILE);
}

/* After a ring: a `once` is switched off, a snooze is removed, and a new
 * snooze -- if `snooze_to` is a time -- is added. Every other line, comments
 * included, goes back exactly as it was. */
#define TEXT_MAX 2048

/* The file's buffers come from the heap for the moment they are needed: an
 * alarm is read once a minute, and 6 KB held for the uptime to do it was
 * most of what this module cost. */
static void rewrite_after(const Alarm *rang, int snooze_h, int snooze_m) {
  char *in = (char *)malloc(TEXT_MAX), *out = (char *)malloc(TEXT_MAX + 64);
  const char *p;
  size_t len = 0, cap = TEXT_MAX + 64;
  int done = 0;
  if (!in || !out) { free(in); free(out); return; }
  read_file(in, TEXT_MAX);
  p = in;
  while (*p) {
    const char *line = p;
    char one[96];
    size_t n;
    Alarm a;
    while (*p && *p != '\n') p++;
    n = (size_t)(p - line);
    if (*p == '\n') p++;
    if (n >= sizeof one) n = sizeof one - 1;
    memcpy(one, line, n);
    one[n] = 0;
    if (!done && alarm_parse(one, &a) && a.on && a.hour == rang->hour &&
        a.min == rang->min && a.kind == rang->kind && !strcmp(a.label, rang->label)) {
      done = 1;
      if (a.kind == ALARM_SNOOZE) continue;              /* rung: gone */
      if (a.kind == ALARM_ONCE) { a.on = 0; alarm_format(&a, one, sizeof one); }
    }
    if (len + strlen(one) + 2 < cap) {
      memcpy(out + len, one, strlen(one));
      len += strlen(one);
      out[len++] = '\n';
    }
  }
  if (snooze_h >= 0) {
    Alarm z = *rang;
    char one[96];
    z.on = 1; z.kind = ALARM_SNOOZE; z.hour = (uint8_t)snooze_h; z.min = (uint8_t)snooze_m;
    alarm_format(&z, one, sizeof one);
    if (len + strlen(one) + 2 < cap) {
      memcpy(out + len, one, strlen(one));
      len += strlen(one);
      out[len++] = '\n';
    }
  }
  write_file(out, len);
  free(in);
  free(out);
}

/* ---- the sound --------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* Three short beeps and a rest, written once to the card: there is no tone
 * generator, only a WAV player (the same answer Timer came to). 16 kHz mono,
 * 1.2 s. */
static void ensure_beep(void) {
  enum { RATE = 16000, SAMPLES = RATE * 12 / 10, PERIOD = RATE / 1000 };
  FsStat st;
  uint8_t hdr[44];
  int16_t buf[256];
  int fd;
  uint32_t i, done;
  if (fs_stat(BEEP_PATH, &st) == 0 && st.size > 44) return;
  fd = fs_open(BEEP_PATH, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) return;
  memset(hdr, 0, sizeof hdr);
  memcpy(hdr, "RIFF", 4); put32(hdr + 4, 36 + SAMPLES * 2);
  memcpy(hdr + 8, "WAVEfmt ", 8); put32(hdr + 16, 16); put16(hdr + 20, 1); put16(hdr + 22, 1);
  put32(hdr + 24, RATE); put32(hdr + 28, RATE * 2); put16(hdr + 32, 2); put16(hdr + 34, 16);
  memcpy(hdr + 36, "data", 4); put32(hdr + 40, SAMPLES * 2);
  fs_write(fd, hdr, sizeof hdr);
  for (done = 0; done < SAMPLES; ) {
    uint32_t n = SAMPLES - done;
    if (n > 256) n = 256;
    for (i = 0; i < n; i++) {
      uint32_t t = done + i, ms = t * 1000 / RATE;
      int beeping = ms < 600 && (ms % 200) < 120;       /* on 120, off 80, three times */
      buf[i] = beeping ? ((t % PERIOD) < PERIOD / 2 ? 10000 : -10000) : 0;
    }
    fs_write(fd, buf, n * 2);
    done += n;
  }
  fs_close(fd);
}

static void beep(void) {
  /* Not while recording: the microphone's clock is the speaker's LRCLK
   * (G43), and a memo is worth more than one beep. */
  if (audio_state() != AUDIO_IDLE) return;
  audio_play(BEEP_PATH);
  s_beep_at = now_ms();
}

/* ---- the panel ----------------------------------------------------------- */

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

#define PW 224
#define PH 104
#define PX ((DISPLAY_W - PW) / 2)
#define PY ((DISPLAY_H - PH) / 2)

static void paint(void) {
  Rect was = draw_clip();
  const CFont *big = fontres_get(s_font_big), *ui = fontres_get(s_font_ui);
  char t[12];
  uint16_t edge = s_flash ? A_HOT : A_EDGE;
  int y = PY + 10, w;

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(PX, PY, PW, PH), A_BACK);
  draw_rect(R(PX, PY, PW, 3), edge);
  draw_rect(R(PX, PY + PH - 3, PW, 3), edge);
  draw_rect(R(PX, PY, 3, PH), edge);
  draw_rect(R(PX + PW - 3, PY, 3, PH), edge);

  snprintf(t, sizeof t, "%02u:%02u", s_alarm.hour, s_alarm.min);
  if (big) {
    w = cfont_width(big, t);
    draw_text_cfont(big, (int16_t)(PX + (PW - w) / 2), (int16_t)y, t, A_TEXT, A_BACK);
    y += big->height + 4;
  } else {
    draw_text_scaled((int16_t)(PX + (PW - 5 * 18) / 2), (int16_t)y, t, 3, A_TEXT, A_BACK);
    y += 30;
  }
  {
    const char *label = s_alarm.label[0] ? s_alarm.label : "Alarm";
    if (ui) {
      w = cfont_width(ui, label);
      draw_text_cfont(ui, (int16_t)(PX + (PW - w) / 2), (int16_t)y, label, A_EDGE, A_BACK);
      y += ui->height + 4;
    } else {
      draw_text((int16_t)(PX + 10), (int16_t)y, label, A_EDGE, A_BACK);
      y += 12;
    }
  }
  draw_text((int16_t)(PX + (PW - 30 * 6) / 2), (int16_t)(PY + PH - 14),
            "any key stops    s snoozes 9m", A_SUB, A_BACK);
  draw_set_clip(was);
}

/* ---- ringing ---------------------------------------------------------------- */

static void start(const Alarm *a) {
  s_alarm = *a;
  s_ringing = 1;
  s_ring_at = s_flash_at = now_ms();
  s_flash = 0;
  power_hold(&s_owner, 1);
  power_wake_now();
  s_font_big = fontres_load("clock56", &s_owner);
  s_font_ui = fontres_load("ui13b", &s_owner);
  ensure_beep();
  beep();
  paint();
  applogf("alarm", "ringing %02u:%02u %s", a->hour, a->min, a->label);
}

static void stop(int snooze) {
  int sh = -1, sm = 0;
  if (!s_ringing) return;
  s_ringing = 0;
  if (audio_state() == AUDIO_PLAYING) audio_stop();
  if (snooze) {
    int m = s_alarm.hour * 60 + s_alarm.min + ALARM_SNOOZE_MIN;
    time_t now = (time_t)clock_epoch();
    struct tm tm;
    /* From now, not from when it first rang: a snooze answered late is still
     * nine minutes of sleep. */
    if (now && localtime_r(&now, &tm)) m = tm.tm_hour * 60 + tm.tm_min + ALARM_SNOOZE_MIN;
    sh = (m / 60) % 24;
    sm = m % 60;
  }
  rewrite_after(&s_alarm, sh, sm);
  fontres_release_owner(&s_owner);
  s_font_big = s_font_ui = -1;
  power_hold(&s_owner, 0);
  power_wake_now();
  applogf("alarm", snooze ? "snoozed to %02d:%02d" : "stopped", sh, sm);
  if (s_repaint) s_repaint();
}

void alarm_paint_over(void) { if (s_ringing) paint(); }

void alarm_key(uint8_t k) {
  if (!s_ringing) return;
  applogf("alarm", "answered with key 0x%02x", k);
  stop(k == 's' || k == 'S');
}

/* Which alarm is next, for alarm_next_text. */
static void note_next(const char *text, int wday, int hour, int min) {
  const char *p = text;
  int best = -1;
  s_next[0] = 0;
  while (*p) {
    char one[96];
    const char *line = p;
    size_t n;
    Alarm a;
    int m;
    while (*p && *p != '\n') p++;
    n = (size_t)(p - line);
    if (*p == '\n') p++;
    if (n >= sizeof one) n = sizeof one - 1;
    memcpy(one, line, n);
    one[n] = 0;
    if (!alarm_parse(one, &a) || (m = alarm_minutes_until(&a, wday, hour, min)) < 0) continue;
    if (best < 0 || m < best) {
      best = m;
      snprintf(s_next, sizeof s_next, "%02u:%02u %s", a.hour, a.min, a.label);
    }
  }
}

void alarm_tick(void) {
  char *text;
  time_t now;
  struct tm tm;
  int key;
  const char *p;

  if (s_ringing) {
    uint32_t t = now_ms();
    if (t - s_ring_at > RING_MAX_MS) { applogf("alarm", "unanswered"); stop(0); return; }
    if (t - s_beep_at > BEEP_EVERY_MS && audio_state() == AUDIO_IDLE) beep();
    if (t - s_flash_at > FLASH_MS) {
      /* Redrawn whole, not just the edge: an app underneath may have painted
       * over part of it in the meantime. */
      s_flash = !s_flash;
      s_flash_at = t;
      paint();
    }
    return;
  }

  if (!clock_have_time() || !fs_mounted()) return;
  now = (time_t)clock_epoch();
  if (!now || !localtime_r(&now, &tm)) return;
  key = tm.tm_yday * 1440 + tm.tm_hour * 60 + tm.tm_min;
  if (key == s_last_key) return;
  s_last_key = key;

  if ((text = (char *)malloc(TEXT_MAX)) == NULL) return;
  read_file(text, TEXT_MAX);
  note_next(text, tm.tm_wday, tm.tm_hour, tm.tm_min);
  for (p = text; *p; ) {
    char one[96];
    const char *line = p;
    size_t n;
    Alarm a;
    while (*p && *p != '\n') p++;
    n = (size_t)(p - line);
    if (*p == '\n') p++;
    if (n >= sizeof one) n = sizeof one - 1;
    memcpy(one, line, n);
    one[n] = 0;
    if (alarm_parse(one, &a) && alarm_rings_at(&a, tm.tm_wday, tm.tm_hour, tm.tm_min)) {
      free(text);
      start(&a);
      return;                  /* one a minute: a second set for the same minute does not ring */
    }
  }
  free(text);
}
