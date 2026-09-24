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

#define CLR_BG      CAPP_RGB(24, 26, 32)
#define CLR_ALARM   CAPP_RGB(180, 30, 30)
#define CLR_TEXT    CAPP_RGB(224, 228, 236)
#define CLR_DIM     CAPP_RGB(130, 138, 150)
#define CLR_FIELD   CAPP_RGB(52, 80, 116)
#define CLR_PAUSED  CAPP_RGB(200, 150, 40)
#define CLR_BAR_BG  CAPP_RGB(44, 48, 58)

static struct {
  int      state;
  int      set_min, set_sec;    /* the configured duration */
  int      field;               /* 0 minutes, 1 seconds -- which is selected in ST_SET */
  uint32_t remain_ms;           /* what is on screen */
  uint32_t remain_at_start;     /* remain_ms when this run (or pause) began */
  uint32_t total_ms;            /* the full duration of the run in progress, for the bar */
  uint32_t start_ms;            /* ticks_ms() when the current run segment began */
  int      flash_on;
  uint32_t flash_at;
  uint32_t last_beep_ms;
  int      beep_ready;          /* the wav file exists on the card */
  CRect    at;                  /* the app's rect, cached from paint for tick's damage() */
  int      have_at;
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
    T.total_ms = T.remain_at_start;    /* the bar's 100%; a resume does not reset it */
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

static void remain_mmss(uint32_t ms, int *mm, int *ss) {
  uint32_t s = (ms + 999) / 1000;      /* round up: 59.9s left still reads 1:00 */
  *mm = (int)(s / 60);
  *ss = (int)(s % 60);
}

/* Poor man's bold: the font is one size, so a word is drawn twice, one pixel
 * right, to read heavier than the rest of the screen. */
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

/* ---- the big clock face ----------------------------------------------------
 *
 * Set in clock56, a font made for this app (fonts/fonts.txt): Space Mono
 * Bold's digits and colon at 56 px, every digit the same width so a
 * countdown does not shuffle sideways, the line cut to the ink.
 *
 * Without it -- a card the firmware has not seeded, a font that will not
 * load -- the digits are the seven-segment blocks this app drew before
 * fonts existed, built out of api->fill. A fullscreen timer in the 6x8
 * console font would be a timer nobody can read across a room. */
#define CLOCK_FONT "clock56"
static int s_font = -1;
#define DIG_W   22
#define DIG_H   40
#define DIG_T   5     /* segment thickness */
#define DIG_GAP 8     /* between every digit and the colon */
#define COLON_W 12

static void seg_fill(int x, int y, int w, int h, uint16_t fg) {
  if (w > 0 && h > 0) api->fill(rect(x, y, w, h), fg);
}

/* Segment bits: a top, b top-right, c bottom-right, d bottom, e bottom-left,
 * f top-left, g middle -- the standard seven-segment layout, 0-9. */
static void draw_digit(int x, int y, int d, uint16_t fg) {
  static const uint8_t SEG[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
  };
  int midY  = y + DIG_H / 2 - DIG_T / 2;
  int vTopH = midY - (y + DIG_T);
  int vBotY = midY + DIG_T;
  int vBotH = (y + DIG_H - DIG_T) - vBotY;
  uint8_t m = (d >= 0 && d <= 9) ? SEG[d] : 0;

  if (m & 0x01) seg_fill(x + DIG_T, y, DIG_W - 2 * DIG_T, DIG_T, fg);
  if (m & 0x02) seg_fill(x + DIG_W - DIG_T, y + DIG_T, DIG_T, vTopH, fg);
  if (m & 0x04) seg_fill(x + DIG_W - DIG_T, vBotY, DIG_T, vBotH, fg);
  if (m & 0x08) seg_fill(x + DIG_T, y + DIG_H - DIG_T, DIG_W - 2 * DIG_T, DIG_T, fg);
  if (m & 0x10) seg_fill(x, vBotY, DIG_T, vBotH, fg);
  if (m & 0x20) seg_fill(x, y + DIG_T, DIG_T, vTopH, fg);
  if (m & 0x40) seg_fill(x + DIG_T, midY, DIG_W - 2 * DIG_T, DIG_T, fg);
}

static void draw_colon(int x, int y, uint16_t fg) {
  seg_fill(x, y + DIG_H / 3 - DIG_T / 2, DIG_T, DIG_T, fg);
  seg_fill(x, y + DIG_H * 2 / 3 - DIG_T / 2, DIG_T, DIG_T, fg);
}

/* Where each of the four digits and the colon land, for a block centred on
 * cx -- the one place that layout is computed, so the digits drawn and the
 * field highlighted behind them in paint_set cannot disagree about it. */
#define TIME_BLOCK_W (4 * DIG_W + COLON_W + 4 * DIG_GAP)

/* The face's measurements, whichever face it is. */
static int dig_h(void) { return s_font >= 0 ? api->font_height(s_font) : DIG_H; }
static int block_w(void) {
  return s_font >= 0 ? api->text_width(s_font, "00:00") : TIME_BLOCK_W;
}

static void time_positions(int cx, int x[4], int *colon_x) {
  int cur = cx - TIME_BLOCK_W / 2;
  x[0] = cur; cur += DIG_W + DIG_GAP;
  x[1] = cur; cur += DIG_W + DIG_GAP;
  *colon_x = cur; cur += COLON_W + DIG_GAP;
  x[2] = cur; cur += DIG_W + DIG_GAP;
  x[3] = cur;
}

/* Where field 0 (minutes) or 1 (seconds) sits, for the highlight behind it. */
static void field_span(int cx, int field, int *fx, int *fw) {
  if (s_font >= 0) {
    int x0 = cx - block_w() / 2, w2 = api->text_width(s_font, "00");
    *fx = field ? x0 + w2 + api->text_width(s_font, ":") : x0;
    *fw = w2;
  } else {
    int x[4], colon_x;
    time_positions(cx, x, &colon_x);
    *fx = field ? x[2] : x[0];
    *fw = (field ? x[3] : x[1]) + DIG_W - *fx;
  }
}

/* `bg0` and `bg1` are what each field sits on: a font draws its own
 * background, so the highlighted field has to say which colour it is. */
static void draw_time(int cx, int y, int minutes, int seconds, uint16_t fg,
                      uint16_t bg0, uint16_t bg1) {
  int x[4], colon_x;
  if (s_font >= 0) {
    char mm[4], ss[4];
    int x0 = cx - block_w() / 2, w2 = api->text_width(s_font, "00");
    api->fmt(mm, sizeof mm, "%02d", minutes % 100);
    api->fmt(ss, sizeof ss, "%02d", seconds % 60);
    api->text_font(s_font, (int16_t)x0, (int16_t)y, mm, fg, bg0);
    api->text_font(s_font, (int16_t)(x0 + w2), (int16_t)y, ":", fg, CLR_BG);
    api->text_font(s_font, (int16_t)(x0 + w2 + api->text_width(s_font, ":")),
                   (int16_t)y, ss, fg, bg1);
    return;
  }
  time_positions(cx, x, &colon_x);
  draw_digit(x[0], y, (minutes / 10) % 10, fg);
  draw_digit(x[1], y, minutes % 10, fg);
  draw_colon(colon_x, y, fg);
  draw_digit(x[2], y, (seconds / 10) % 10, fg);
  draw_digit(x[3], y, seconds % 10, fg);
}

static void paint_set(CRect c) {
  int h = dig_h(), cx = c.x + c.w / 2, y = c.y + c.h / 2 - h / 2 - 6;
  int pad = 4, fx, fw;

  field_span(cx, T.field, &fx, &fw);
  if (s_font >= 0) {
    /* Time, then the highlight, then its field again on top: the colon's
     * line fills its own background, and drawn after the highlight it ate
     * the padding on that side, leaving the blue flush against the digit. */
    char v[4];
    draw_time(cx, y, T.set_min, T.set_sec, CLR_TEXT, CLR_BG, CLR_BG);
    api->fill(rect(fx - pad, y - pad, fw + 2 * pad, h + 2 * pad), CLR_FIELD);
    api->fmt(v, sizeof v, "%02d", T.field ? T.set_sec % 60 : T.set_min % 100);
    api->text_font(s_font, (int16_t)fx, (int16_t)y, v, CLR_TEXT, CLR_FIELD);
  } else {
    api->fill(rect(fx - pad, y - pad, fw + 2 * pad, h + 2 * pad), CLR_FIELD);
    draw_time(cx, y, T.set_min, T.set_sec, CLR_TEXT, CLR_BG, CLR_BG);
  }

  text_center(cx, y - 18, "set", CLR_DIM, CLR_BG);
  text_center(cx, y + h + pad + 14, "left/right field  up/down +-1", CLR_DIM, CLR_BG);
  text_center(cx, y + h + pad + 26, "digits type  space start", CLR_DIM, CLR_BG);
}

/* Green with time to spare, red as it runs out, yellow the midpoint between
 * -- interpolated in RGB rather than snapped between three fixed colours, so
 * the bar eases from one to the next instead of jumping. Two ramps, not one
 * straight green-to-red line: a single ramp spends the whole first half
 * looking olive, and olive does not read as "plenty of time". */
static uint16_t lerp_rgb(int r0, int g0, int b0, int r1, int g1, int b1, int t, int tmax) {
  int r = r0 + (r1 - r0) * t / tmax;
  int g = g0 + (g1 - g0) * t / tmax;
  int b = b0 + (b1 - b0) * t / tmax;
  return CAPP_RGB(r, g, b);
}
static uint16_t bar_colour(uint32_t remain, uint32_t total) {
  uint32_t pct = total ? remain * 100 / total : 0;      /* 0..100 left */
  if (pct >= 50) return lerp_rgb(230, 190, 40,  60, 175, 90, (int)pct - 50, 50);  /* yellow -> green */
  return lerp_rgb(200, 45, 40,  230, 190, 40, (int)pct, 50);                     /* red -> yellow */
}

/* The track is always the full width, drawn first; the fill sits on top of it
 * and shrinks from the right as remain_ms counts down toward 0, out of
 * total_ms -- the duration when this run started, not remain_at_start, which
 * a pause and resume would otherwise reset to whatever was left. */
#define BAR_H    8
#define BAR_GAP  6
static void draw_bar(int cx, int y) {
  int w = block_w(), x = cx - w / 2;
  int fill_w = (int)((uint32_t)w * T.remain_ms / T.total_ms);
  uint16_t colour = bar_colour(T.remain_ms, T.total_ms);

  api->fill(rect(x, y, w, BAR_H), CLR_BAR_BG);
  if (fill_w > 0) api->fill(rect(x, y, fill_w, BAR_H), colour);
}

/* The digit block and the bar beneath it -- the only pixels a running
 * countdown changes once a second; the label above and the hint below stay
 * put between ticks. api->fill(c, CLR_BG) at the top of app_paint still runs
 * across the whole app rect, but the shell clips it to whatever tick last
 * damaged, so scoping that to just this saves the label and the hint from
 * being cleared and redrawn alongside the digits. Without it every tick
 * flashed the whole screen to CLR_BG first -- there is no framebuffer here,
 * writes go straight to the panel, so that flash was visible. */
static CRect time_bar_rect(void) {
  int cx = T.at.x + T.at.w / 2;
  int h  = dig_h(), y = T.at.y + T.at.h / 2 - h / 2 - 6;
  return rect(cx - block_w() / 2, y, block_w(), h + BAR_GAP + BAR_H);
}

static void paint_running(CRect c, const char *label, uint16_t colour) {
  int mm, ss, h = dig_h(), cx = c.x + c.w / 2, y = c.y + c.h / 2 - h / 2 - 6;
  int bar_y = y + h + BAR_GAP;

  remain_mmss(T.remain_ms, &mm, &ss);
  draw_time(cx, y, mm, ss, CLR_TEXT, CLR_BG, CLR_BG);
  text_center(cx, y - 18, label, colour, CLR_BG);
  draw_bar(cx, bar_y);
  text_center(cx, bar_y + BAR_H + 12, "space pause  r reset", CLR_DIM, CLR_BG);
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
  T.at = c;
  T.have_at = 1;
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
      if (T.have_at) api->damage(time_bar_rect());
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

static CappUi UI;

/* Minutes on the command line start it at once: `run timer 5`, and what the
 * `start` command opens (CAPP_CMD_OPEN). A number only; anything else is the
 * set screen as before. */
static int parse_minutes(const char *s) {
  int v = 0, any = 0;
  if (!s) return 0;
  for (; *s >= '0' && *s <= '9'; s++) { v = v * 10 + (*s - '0'); any = 1; }
  return any && !*s && v > 0 && v < 100 ? v : 0;
}

enum { ACT_START = 1, ACT_RESET };

static const CappParam P_MIN[] = { { "minutes", CAPP_ARG_INT, "how long, 1 to 99" } };

static const CappAction ACTIONS[] = {
  { "start", "Start / pause", "Timer", 0,    ACT_START,     /* space, in app_key */
    "open Timer counting down from this many minutes", P_MIN, 1,
    CAPP_CMD_YES | CAPP_CMD_OPEN },
  { "reset", "Reset",         "Timer", 0x12, ACT_RESET },   /* ctrl-r */
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

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
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};


static int app_action(void *st, int a) {
  (void)st;
  if (a == ACT_START) {
    if (T.state == ST_RUNNING) pause(api->ticks_ms());
    else start_or_resume(api->ticks_ms());
    return 1;
  }
  if (a == ACT_RESET) { reset(); return 1; }
  return 0;
}

int capp_main(const CardApi *a, int argc, char **argv) {
  int minutes;
  api = a;
  au = api->audio();
  api->mem_set(&T, 0, sizeof T);
  ensure_beep();
  /* Asked for, not assumed: -1 is the seven-segment face. The OS frees it
   * when the app closes. */
  s_font = api->headless() ? -1 : api->font_load(CLOCK_FONT);

  minutes =argc > 1 ? parse_minutes(argv[1]) : 0;
  if (minutes) {
    T.set_min = minutes;
    start_or_resume(api->ticks_ms());
  }

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
