/* A countdown timer.
 *
 * Set minutes and seconds while stopped, space starts it, space again pauses
 * it, r resets back to the set screen with the same duration ready to go
 * again. tick counts down once a second and repaints; hitting zero flips the
 * screen and beeps until any key answers it.
 *
 * The beep is a WAV file this app writes to itself the first time it needs
 * one -- there is no tone generator in the API, only api->audio()->play(a
 * file), so a short square wave is synthesised once into /cache and replayed
 * from there. No card, no beep; the flash still happens, because the alarm
 * should not depend on the SD card working.
 */

#include "kernel/app/capp.h"

static const CardApi *api;
static const CappAudio *au;

#define BEEP_PATH CAPP_CACHE "/timer_beep.wav"
#define BEEP_RATE 16000u        /* matches the mic's rate; nothing resamples */
#define BEEP_FREQ 1000u
#define BEEP_MS   220u
#define BEEP_AMPL 9000

#define RING_PERIOD_MS 900      /* how often a fresh beep is fired while ringing */
#define FLASH_MS       400      /* how fast the alarm screen flips */

enum { ST_SET = 0, ST_RUNNING, ST_PAUSED, ST_DONE };

#define CLR_BG     CAPP_RGB(24, 26, 32)
#define CLR_ALARM  CAPP_RGB(180, 30, 30)
#define CLR_TEXT   CAPP_RGB(224, 228, 236)
#define CLR_DIM    CAPP_RGB(130, 138, 150)
#define CLR_FIELD  CAPP_RGB(52, 80, 116)
#define CLR_PAUSED CAPP_RGB(200, 150, 40)

static struct {
  int      state;
  int      set_min, set_sec;    /* the configured duration */
  int      field;               /* 0 minutes, 1 seconds -- which is selected in ST_SET */
  uint32_t remain_ms;           /* what is on screen */
  uint32_t remain_at_start;     /* remain_ms when this run (or pause) began */
  uint32_t start_ms;            /* ticks_ms() when the current run segment began */
  int      flash_on;
  uint32_t flash_at;
  uint32_t last_beep_ms;
  int      beep_ready;          /* the wav file exists on the card */
} T;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* A plain square wave, synthesised straight into a WAV file: no trig, no
 * libc, just toggling between two levels every half period. Written once and
 * kept, like a cache -- rewriting it every launch would be one more thing
 * asking the card to do something it already did. */
static void make_beep(void) {
  uint8_t hdr[44];
  int fd;
  uint32_t nsamples = BEEP_RATE * BEEP_MS / 1000;
  uint32_t data_bytes = nsamples * 2;
  uint32_t period = BEEP_RATE / BEEP_FREQ;
  uint32_t half = period / 2;
  uint32_t done;
  int16_t buf[256];

  api->mkdir(CAPP_CACHE);
  fd = api->open(BEEP_PATH, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;

  api->mem_set(hdr, 0, sizeof hdr);
  api->mem_cpy(hdr, "RIFF", 4);
  put_u32(hdr + 4, 36 + data_bytes);
  api->mem_cpy(hdr + 8, "WAVE", 4);
  api->mem_cpy(hdr + 12, "fmt ", 4);
  put_u32(hdr + 16, 16);
  put_u16(hdr + 20, 1);                 /* PCM */
  put_u16(hdr + 22, 1);                 /* mono */
  put_u32(hdr + 24, BEEP_RATE);
  put_u32(hdr + 28, BEEP_RATE * 2);     /* byte rate */
  put_u16(hdr + 32, 2);                 /* block align */
  put_u16(hdr + 34, 16);                /* bits per sample */
  api->mem_cpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, data_bytes);
  api->write(fd, hdr, sizeof hdr);

  for (done = 0; done < nsamples; ) {
    uint32_t n = nsamples - done, i;
    if (n > 256) n = 256;
    for (i = 0; i < n; i++)
      buf[i] = ((done + i) % period < half) ? BEEP_AMPL : -BEEP_AMPL;
    api->write(fd, buf, n * 2);
    done += n;
  }
  api->close(fd);
}

static void ensure_beep(void) {
  CappStat st;
  if (T.beep_ready) return;
  if (api->stat(BEEP_PATH, &st) != 0) make_beep();
  T.beep_ready = 1;
}

/* ---- the clock ------------------------------------------------------------ */

static uint32_t configured_ms(void) {
  return (uint32_t)(T.set_min * 60 + T.set_sec) * 1000u;
}

static void start_or_resume(uint32_t now_ms) {
  if (T.state == ST_SET) {
    if (!configured_ms()) return;      /* nothing to time */
    T.remain_at_start = configured_ms();
  } else if (T.state == ST_PAUSED) {
    T.remain_at_start = T.remain_ms;
  } else {
    return;
  }
  T.remain_ms = T.remain_at_start;     /* so the repaint before the first tick is right */
  T.start_ms = now_ms;
  T.state = ST_RUNNING;
}

static void pause(uint32_t now_ms) {
  uint32_t elapsed = now_ms - T.start_ms;
  if (T.state != ST_RUNNING) return;
  T.remain_ms = elapsed >= T.remain_at_start ? 0 : T.remain_at_start - elapsed;
  T.state = ST_PAUSED;
}

static void go_off(uint32_t now_ms) {
  T.remain_ms = 0;
  T.state = ST_DONE;
  T.flash_on = 1;
  T.flash_at = now_ms;
  if (au->state() == CAPP_AUDIO_IDLE) au->play(BEEP_PATH);
  T.last_beep_ms = now_ms;
}

static void dismiss(void) {
  if (au->state() != CAPP_AUDIO_IDLE) au->stop();
  T.state = ST_SET;
}

static void reset(void) {
  if (au->state() != CAPP_AUDIO_IDLE) au->stop();
  if (T.state == ST_SET) { T.set_min = 0; T.set_sec = 0; }
  T.state = ST_SET;
}

/* ---- painting -------------------------------------------------------------- */

static void mmss(uint32_t ms, char *out, size_t n) {
  uint32_t s = (ms + 999) / 1000;      /* round up: 59.9s left still reads 1:00 */
  api->fmt(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

/* Poor man's bold: the font is one size, so the focal clock is drawn twice,
 * one pixel right, to read heavier than the rest of the screen. */
static void text_bold(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  api->text((int16_t)x, (int16_t)y, s, fg, bg);
  api->text((int16_t)(x + 1), (int16_t)y, s, fg, bg);
}

/* Centred on cx, which every caller here gets from strlen rather than a
 * guessed character count -- a guess that does not match the string is a
 * line that is off-centre by exactly the difference. */
static void text_center(int cx, int y, const char *s, uint16_t fg, uint16_t bg) {
  int w = (int)api->str_len(s) * 6;
  api->text((int16_t)(cx - w / 2), (int16_t)y, s, fg, bg);
}
static void text_center_bold(int cx, int y, const char *s, uint16_t fg, uint16_t bg) {
  int w = (int)api->str_len(s) * 6;
  text_bold(cx - w / 2, y, s, fg, bg);
}

static void paint_set(CRect c) {
  char min[4], sec[4];
  int mx, sx, y = c.y + c.h / 2 - 16;
  int boxw = 30, boxh = 20, gap = 10;
  int total = boxw * 2 + gap + 6;      /* + the colon */

  api->fmt(min, sizeof min, "%02d", T.set_min);
  api->fmt(sec, sizeof sec, "%02d", T.set_sec);
  mx = c.x + c.w / 2 - total / 2;
  sx = mx + boxw + gap;

  api->fill(rect(mx, y, boxw, boxh), T.field == 0 ? CLR_FIELD : CLR_BG);
  api->fill(rect(sx, y, boxw, boxh), T.field == 1 ? CLR_FIELD : CLR_BG);
  text_bold(mx + boxw / 2 - 6, y + 6, min, CLR_TEXT, T.field == 0 ? CLR_FIELD : CLR_BG);
  text_bold(sx + boxw / 2 - 6, y + 6, sec, CLR_TEXT, T.field == 1 ? CLR_FIELD : CLR_BG);
  api->text((int16_t)(mx + boxw), (int16_t)(y + 6), ":", CLR_TEXT, CLR_BG);

  text_center(c.x + c.w / 2, y - 14, "set", CLR_DIM, CLR_BG);
  text_center(c.x + c.w / 2, y + boxh + 12, "left/right field  up/down +-1", CLR_DIM, CLR_BG);
  text_center(c.x + c.w / 2, y + boxh + 24, "digits type  space start", CLR_DIM, CLR_BG);
}

static void paint_running(CRect c, const char *label, uint16_t colour) {
  char buf[8];
  int y = c.y + c.h / 2 - 10;

  mmss(T.remain_ms, buf, sizeof buf);
  text_center_bold(c.x + c.w / 2, y, buf, CLR_TEXT, CLR_BG);
  text_center(c.x + c.w / 2, y - 16, label, colour, CLR_BG);
  text_center(c.x + c.w / 2, y + 20, "space pause  r reset", CLR_DIM, CLR_BG);
}

static void paint_done(CRect c) {
  uint16_t bg = T.flash_on ? CLR_ALARM : CLR_BG;
  int y = c.y + c.h / 2 - 10;

  api->fill(c, bg);
  text_center_bold(c.x + c.w / 2, y, "time's up", CLR_TEXT, bg);
  text_center(c.x + c.w / 2, y + 20, "any key stops it", CLR_TEXT, bg);
}

static void app_paint(void *st, CRect c) {
  (void)st;
  if (T.state != ST_DONE) api->fill(c, CLR_BG);
  switch (T.state) {
  case ST_SET:     paint_set(c); break;
  case ST_RUNNING: paint_running(c, "running", CLR_TEXT); break;
  case ST_PAUSED:  paint_running(c, "paused", CLR_PAUSED); break;
  case ST_DONE:    paint_done(c); break;
  }
}

/* ---- input ------------------------------------------------------------- */

static int app_key(void *st, uint8_t k) {
  (void)st;

  if (T.state == ST_DONE) { dismiss(); return 1; }

  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN)
    return 0;

  if (k >= '0' && k <= '9' && T.state == ST_SET) {
    int d = k - '0';
    if (T.field == 0) T.set_min = (T.set_min % 10) * 10 + d;
    else { T.set_sec = (T.set_sec % 10) * 10 + d; if (T.set_sec > 59) T.set_sec = 59; }
    return 1;
  }

  switch (k) {
  case CAPP_KEY_LEFT:
  case CAPP_KEY_RIGHT:
  case 0x09:                          /* tab */
    if (T.state == ST_SET) { T.field = !T.field; return 1; }
    return 0;
  case CAPP_KEY_UP:
  case CAPP_KEY_DOWN:
    if (T.state != ST_SET) return 0;
    {
      int d = k == CAPP_KEY_UP ? 1 : -1;
      if (T.field == 0) T.set_min = (T.set_min + d + 100) % 100;
      else T.set_sec = (T.set_sec + d + 60) % 60;
    }
    return 1;
  case ' ':
  case CAPP_KEY_ENTER:
    if (T.state == ST_RUNNING) pause(api->ticks_ms());
    else start_or_resume(api->ticks_ms());
    return 1;
  case 'r': case 'R':
  case CAPP_KEY_BACK:
    reset();
    return 1;
  case CAPP_KEY_ESC:
    return 0;                         /* top level: nothing to go back to */
  default:
    return 0;
  }
}

static int app_wants_text(void *st) { (void)st; return 0; }

static int app_tick(void *st, uint32_t now_ms) {
  (void)st;

  if (T.state == ST_RUNNING) {
    uint32_t elapsed = now_ms - T.start_ms;
    uint32_t remain = elapsed >= T.remain_at_start ? 0 : T.remain_at_start - elapsed;
    if (!remain) { go_off(now_ms); return 1; }
    if ((remain + 999) / 1000 != (T.remain_ms + 999) / 1000) {
      T.remain_ms = remain;
      return 1;
    }
    T.remain_ms = remain;
    return 0;
  }

  if (T.state == ST_DONE) {
    int changed = 0;
    if (now_ms - T.flash_at >= FLASH_MS) {
      T.flash_at = now_ms;
      T.flash_on = !T.flash_on;
      changed = 1;
    }
    if (now_ms - T.last_beep_ms >= RING_PERIOD_MS && au->state() == CAPP_AUDIO_IDLE) {
      au->play(BEEP_PATH);
      T.last_beep_ms = now_ms;
    }
    return changed;
  }

  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Timer",
  /* 16x16: a clock face, hands at 12 and 3. */
  { 0x03, 0xC0, 0x0C, 0x30, 0x30, 0x0C, 0x60, 0x06,
    0x41, 0x82, 0x81, 0x81, 0x81, 0x81, 0x81, 0xF9,
    0x81, 0x81, 0x81, 0x81, 0x40, 0x02, 0x60, 0x06,
    0x30, 0x0C, 0x0C, 0x30, 0x03, 0xC0, 0x00, 0x00 },
  "digits\ttype minutes/seconds\nleft/right\tswitch field\nup/down\t+-1\n"
  "space\tstart, pause, resume\nr\treset\nany key\tstops the alarm\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  au = api->audio();
  (void)argc; (void)argv;
  api->mem_set(&T, 0, sizeof T);
  ensure_beep();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
