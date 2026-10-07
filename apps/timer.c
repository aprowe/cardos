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
 *
 * IT GOES ON WITHOUT THE APP (API 40). Starting asks the OS for a ringing
 * notification at the finish (api->notify_at), and pausing or resetting
 * cancels it, so a timer left running rings over whatever is open -- or
 * after a restart, when the clock is known. The run is kept in
 * /cache/timer.state (its end, by the wall clock if there is one, else by
 * uptime), so opening Timer again shows it still counting, or finished.
 *
 * The keys are the hint bar along the bottom (apps/footer.h), the same bar
 * every app has; Start and Reset are also a menu (apps/toolbar.h), fn-b or
 * a mouse, from the same action table as the ctrl-r chord.
 */

#include "kernel/app/capp.h"
#include "apps/toolbar.h"
#include "apps/footer.h"

static const CardApi *api;
static const CappAudio *au;

#define BEEP_PATH CAPP_CACHE "/timer_beep.wav"
#define BEEP_RATE 16000u        /* matches the mic's rate; nothing resamples */
#define BEEP_FREQ 1000u
#define BEEP_MS   220u
#define BEEP_AMPL 9000
#define BEEP_BYTES (44u + BEEP_RATE * BEEP_MS / 1000u * 2u)   /* header + samples */

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
  /* What the running face last drew, so a tick can mark only the seconds
   * (and the sliver of bar that moved) instead of the whole clock. */
  int      drawn_mm, drawn_fill, drawn_ok;
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

/* There and the right size. Existing was not enough: a write cut short -- a
 * full card, the power pulled mid-way -- left a file that stat finds, so it
 * was never made again and the alarm stayed silent, or played a header
 * promising samples that were not there. The size is fixed by the defines,
 * so anything else is a beep to write again. */
static void ensure_beep(void) {
  CappStat st;
  if (T.beep_ready) return;
  if (api->stat(BEEP_PATH, &st) != 0 || st.is_dir || st.size != BEEP_BYTES) make_beep();
  T.beep_ready = 1;
}

/* ---- the run, kept, and the OS told -------------------------------------------- */

#define STATE_PATH CAPP_CACHE "/timer.state"

/* state set_min set_sec total_ms remain_ms end_epoch end_ms */
static void state_save(uint32_t now_ms) {
  char b[96];
  int fd, n;
  uint32_t end_ms = 0, end_epoch = 0;
  if (T.state == ST_RUNNING) {
    uint32_t left = T.remain_at_start - (now_ms - T.start_ms);
    end_ms = now_ms + left;
    if (api->epoch()) end_epoch = api->epoch() + (left + 999) / 1000;
  }
  fd = api->open(STATE_PATH, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  n = api->fmt(b, sizeof b, "%d %d %d %lu %lu %lu %lu\n", T.state, T.set_min, T.set_sec,
               (unsigned long)T.total_ms, (unsigned long)T.remain_ms,
               (unsigned long)end_epoch, (unsigned long)end_ms);
  api->write(fd, b, (size_t)n);
  api->close(fd);
}

static unsigned long num(const char **p) {
  unsigned long v = 0;
  while (**p == ' ') (*p)++;
  while (**p >= '0' && **p <= '9') v = v * 10ul + (unsigned long)(*(*p)++ - '0');
  return v;
}

/* Where a run left off, as if the app had stayed open. */
static void state_load(uint32_t now_ms) {
  char b[96];
  const char *p = b;
  int fd = api->open(STATE_PATH, CAPP_O_READ), n;
  unsigned long st, end_epoch, end_ms;
  if (fd < 0) return;
  n = api->read(fd, b, sizeof b - 1);
  api->close(fd);
  if (n <= 0) return;
  b[n] = 0;
  st = num(&p);
  T.set_min = (int)num(&p);
  T.set_sec = (int)num(&p);
  T.total_ms = (uint32_t)num(&p);
  T.remain_ms = (uint32_t)num(&p);
  end_epoch = num(&p);
  end_ms = num(&p);
  if (T.set_min > 99 || T.set_sec > 59) { T.set_min = T.set_sec = 0; return; }
  if (st == ST_PAUSED && T.remain_ms) { T.state = ST_PAUSED; return; }
  if (st == ST_RUNNING) {
    uint32_t left = 0;
    if (end_epoch && api->epoch()) {
      if (end_epoch > api->epoch()) left = (uint32_t)(end_epoch - api->epoch()) * 1000u;
    } else if ((int32_t)(end_ms - now_ms) > 0 && end_ms - now_ms <= T.total_ms) {
      left = (uint32_t)(end_ms - now_ms);       /* by uptime: only if no restart since */
    }
    if (left) {
      T.state = ST_RUNNING;
      T.remain_at_start = T.remain_ms = left;
      T.start_ms = now_ms;
      api->keep_awake(1);
    }
    /* Ran out while closed: the OS rang it, and this is the set screen. */
  }
}

/* The finish, told to the OS so it rings with Timer closed. */
static void tell_os(void) {
  char text[40];
  int mm = T.set_min, ss = T.set_sec;
  if (T.state == ST_RUNNING) {
    api->fmt(text, sizeof text, "the %d:%02d timer", mm, ss);
    api->notify_at((T.remain_at_start + 999) / 1000, "done", "time's up", text, 1);
  } else api->notify_cancel("done");
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
  tell_os();
  state_save(now_ms);
  /* A countdown is watched, not touched: the screen stays on while it runs
   * and while it rings (API 32), and goes back to its own timeouts after. */
  api->keep_awake(1);
}

static void pause(uint32_t now_ms) {
  uint32_t elapsed = now_ms - T.start_ms;
  if (T.state != ST_RUNNING) return;
  T.remain_ms = elapsed >= T.remain_at_start ? 0 : T.remain_at_start - elapsed;
  T.state = ST_PAUSED;
  api->keep_awake(0);
  tell_os();
  state_save(now_ms);
}

static void go_off(uint32_t now_ms) {
  T.remain_ms = 0;
  T.state = ST_DONE;
  api->notify_cancel("done");          /* ringing here, not as a notification */
  state_save(now_ms);
  api->wake();                           /* in case it went dark anyway */
  api->keep_awake(1);
  T.flash_on = 1;
  T.flash_at = now_ms;
  if (au->state() == CAPP_AUDIO_IDLE) au->play(BEEP_PATH);
  T.last_beep_ms = now_ms;
}

static void dismiss(void) {
  if (au->state() != CAPP_AUDIO_IDLE) au->stop();
  T.state = ST_SET;
  api->keep_awake(0);
  state_save(api->ticks_ms());
}

static void reset(void) {
  api->keep_awake(0);
  if (au->state() != CAPP_AUDIO_IDLE) au->stop();
  if (T.state == ST_SET) { T.set_min = 0; T.set_sec = 0; }
  T.state = ST_SET;
  tell_os();
  state_save(api->ticks_ms());
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

/* ---- the face, drawn without clearing it --------------------------------
 *
 * There is no framebuffer: a fill reaches the panel at once, so clearing
 * the face and drawing the clock again showed a blank clock for a moment
 * every second -- the digits blinked. text and text_font paint their own
 * background, so nothing under them is ever cleared; only the margins round
 * what is drawn are filled, and those were background already. */

static void fill_if(int x, int y, int w, int h, uint16_t c) {
  if (w > 0 && h > 0) api->fill(rect(x, y, w, h), c);
}

/* `outer` less `inner`: the four margins round something about to be
 * drawn over itself. `inner` lies inside `outer`. */
static void fill_round(CRect o, CRect i, uint16_t c) {
  fill_if(o.x, o.y, o.w, i.y - o.y, c);
  fill_if(o.x, i.y + i.h, o.w, o.y + o.h - i.y - i.h, c);
  fill_if(o.x, i.y, i.x - o.x, i.h, c);
  fill_if(i.x + i.w, i.y, o.x + o.w - i.x - i.w, i.h, c);
}

/* A row of the highlight's padding from x0 to x1: blue across the selected
 * field (fx..fx+fw, padded), background either side. */
static void hl_strip(int x0, int x1, int y, int h, int fx, int fw, int pad) {
  int a = fx - pad, b = fx + fw + pad;
  if (a < x0) a = x0;
  if (b > x1) b = x1;
  fill_if(x0, y, a - x0, h, CLR_BG);
  fill_if(a, y, b - a, h, CLR_FIELD);
  fill_if(b, y, x1 - b, h, CLR_BG);
}

/* The digit band in the set state: the time with the selected field on
 * blue, padded by `pad` all round. */
static void paint_set_band(int cx, int y, int h, int pad) {
  int bw = block_w(), x0 = cx - bw / 2, fx, fw;
  field_span(cx, T.field, &fx, &fw);
  if (s_font < 0) {
    /* The segments draw only what is lit, so the block has to be cleared
     * under them; this is the face for a card with no clock56. */
    api->fill(rect(x0 - pad, y - pad, bw + 2 * pad, h + 2 * pad), CLR_BG);
    api->fill(rect(fx - pad, y - pad, fw + 2 * pad, h + 2 * pad), CLR_FIELD);
    draw_time(cx, y, T.set_min, T.set_sec, CLR_TEXT, CLR_BG, CLR_BG);
    return;
  }
  /* Each field on its own colour, then the padding round them. The colon's
   * line fills its own background, so the blue padding beside the colon is
   * drawn after it -- four pixels that go background then blue on a key,
   * where the old fill-then-redraw blinked the whole field. */
  draw_time(cx, y, T.set_min, T.set_sec, CLR_TEXT,
            T.field ? CLR_BG : CLR_FIELD, T.field ? CLR_FIELD : CLR_BG);
  hl_strip(x0 - pad, x0 + bw + pad, y - pad, pad, fx, fw, pad);
  hl_strip(x0 - pad, x0 + bw + pad, y + h, pad, fx, fw, pad);
  fill_if(x0 - pad, y, pad, h, T.field ? CLR_BG : CLR_FIELD);
  fill_if(x0 + bw, y, pad, h, T.field ? CLR_FIELD : CLR_BG);
  if (T.field) fill_if(fx - pad, y, pad, h, CLR_FIELD);
  else fill_if(fx + fw, y, pad, h, CLR_FIELD);
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

/* The bar is the full block width: the colour from the left, shrinking from
 * the right as remain_ms counts down toward 0, out of total_ms -- the
 * duration when this run started, not remain_at_start, which a pause and
 * resume would otherwise reset to whatever was left. The colour and the
 * track left over are two fills side by side; filling the track and then
 * the colour over it blinked the bar every second. */
#define BAR_H    8
#define BAR_GAP  6
static int bar_fill_w(uint32_t remain) {
  return T.total_ms ? (int)((uint32_t)block_w() * remain / T.total_ms) : 0;
}
static void draw_bar(int cx, int y) {
  int w = block_w(), x = cx - w / 2;
  int fill_w = bar_fill_w(T.remain_ms);
  if (fill_w > w) fill_w = w;
  if (fill_w > 0) api->fill(rect(x, y, fill_w, BAR_H), bar_colour(T.remain_ms, T.total_ms));
  fill_if(x + fill_w, y, w - fill_w, BAR_H, CLR_BAR_BG);
  T.drawn_fill = fill_w;
}

/* The digit block and the bar beneath it -- the only pixels a running
 * countdown changes once a second; the label above and the hint below stay
 * put between ticks. */
static CRect time_bar_rect(void) {
  int cx = T.at.x + T.at.w / 2;
  int h  = dig_h(), y = T.at.y + T.at.h / 2 - h / 2 - 6;
  return rect(cx - block_w() / 2, y, block_w(), h + BAR_GAP + BAR_H);
}

/* What a running second changed: the seconds digits, and the sliver of bar
 * between the old fill and the new -- or the whole block when the minutes
 * turned over or the face is the segment one, which clears under itself. */
static void damage_second(int mm) {
  CRect r = time_bar_rect();
  int nf;
  if (s_font < 0 || !T.drawn_ok || mm != T.drawn_mm) { api->damage(r); return; }
  {
    int w2 = api->text_width(s_font, "00"), sx = r.x + w2 + api->text_width(s_font, ":");
    api->damage(rect(sx, r.y, r.x + r.w - sx, dig_h()));
  }
  nf = bar_fill_w(T.remain_ms);
  if (nf > r.w) nf = r.w;
  if (nf != T.drawn_fill) {
    int a = nf < T.drawn_fill ? nf : T.drawn_fill;
    int b = nf < T.drawn_fill ? T.drawn_fill : nf;
    api->damage(rect(r.x + a, r.y + dig_h() + BAR_GAP, b - a, BAR_H));
  }
}

/* The face for the set, running and paused states: label, time, and the bar
 * or the highlight, each drawn over itself, with the space between them
 * filled. */
static void paint_face(CRect c) {
  int h = dig_h(), cx = c.x + c.w / 2, y = c.y + c.h / 2 - h / 2 - 6;
  int bw = block_w(), x0 = cx - bw / 2, ly = y - 18, pad = 0, top, bot, lw;
  const char *label = "set";
  uint16_t lc = CLR_DIM;

  if (T.state == ST_RUNNING) { label = "running"; lc = CLR_TEXT; }
  else if (T.state == ST_PAUSED) { label = "paused"; lc = CLR_PAUSED; }
  if (T.state == ST_SET) pad = 4;
  top = y - pad;
  bot = T.state == ST_SET ? y + h + pad : y + h + BAR_GAP + BAR_H;

  /* Above the label, between it and the time, and below everything. */
  fill_if(c.x, c.y, c.w, ly - c.y, CLR_BG);
  fill_if(c.x, ly + 8, c.w, top - ly - 8, CLR_BG);
  fill_if(c.x, bot, c.w, c.y + c.h - bot, CLR_BG);

  /* The label: its row either side of the word, then the word. A longer
   * word before ("running" after "set") is covered by the row's fill. */
  lw = (int)api->str_len(label) * 6;
  fill_round(rect(c.x, ly, c.w, 8), rect(cx - lw / 2, ly, lw, 8), CLR_BG);
  api->text((int16_t)(cx - lw / 2), (int16_t)ly, label, lc, CLR_BG);

  /* Either side of the block, the height of the band. */
  fill_round(rect(c.x, top, c.w, bot - top), rect(x0 - pad, top, bw + 2 * pad, bot - top), CLR_BG);

  if (T.state == ST_SET) {
    paint_set_band(cx, y, h, pad);
    T.drawn_ok = 0;
    return;
  }
  {
    int mm, ss;
    remain_mmss(T.remain_ms, &mm, &ss);
    if (s_font < 0) api->fill(rect(x0, y, bw, h), CLR_BG);   /* segments: see paint_set_band */
    draw_time(cx, y, mm, ss, CLR_TEXT, CLR_BG, CLR_BG);
    fill_if(x0, y + h, bw, BAR_GAP, CLR_BG);
    draw_bar(cx, y + h + BAR_GAP);
    T.drawn_mm = mm;
    T.drawn_ok = 1;
  }
}

static void paint_done(CRect c) {
  uint16_t bg = T.flash_on ? CLR_ALARM : CLR_BG;
  int y = c.y + c.h / 2 - 10;

  api->fill(c, bg);
  text_center_bold(c.x + c.w / 2, y, "time's up", CLR_TEXT, bg);
  text_center(c.x + c.w / 2, y + 20, "any key stops it", CLR_TEXT, bg);
}

/* What the bar says in each state. The paused one says resume: it used to
 * say "space pause" while already paused, which is the one thing space would
 * not do. */
static const char *hint(void) {
  switch (T.state) {
  case ST_SET:     return "arrows set  digits type  space start";
  case ST_RUNNING: return "space pause  r reset";
  case ST_PAUSED:  return "space resume  r reset";
  default:         return 0;
  }
}

static void app_paint(void *st, CRect full) {
  CRect c, face;
  (void)st;
  /* The menu bar's two fast paths (apps/toolbar.h): a highlight moving down
   * an open menu, or the bar alone, repaint only themselves. */
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  c = toolbar_rest(full);

  /* The face is centred above the hint bar. Ringing, the whole of it
   * flashes, bar and all: the alarm is the screen, and "any key stops it"
   * is written in the middle. */
  face = c;
  if (T.state != ST_DONE) face.h = (int16_t)(c.h - FOOT_H);
  T.at = face;
  T.have_at = 1;
  if (T.state == ST_DONE) { T.drawn_ok = 0; paint_done(face); }
  else paint_face(face);
  if (T.state != ST_DONE) footer_paint(api, c, hint());
  toolbar_paint_menu(full);
}

/* ---- input ------------------------------------------------------------- */

static int app_action(void *st, int a);

static int app_key(void *st, uint8_t k) {
  int a;
  (void)st;

  /* Ringing, any key stops it -- fn-b included; the bar can wait. */
  if (T.state == ST_DONE) { dismiss(); return 1; }

  /* The bar first, as in apps/todo.c: while it has the keyboard it answers
   * for every key, and an item chosen there is the same action a click or
   * the chord would be. */
  a = toolbar_key(k);
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return app_action(0, a);

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

static int app_click(void *st, int16_t x, int16_t y, int button) {
  int a;
  (void)st; (void)button;
  if (T.state == ST_DONE) { dismiss(); return 1; }
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return app_action(0, a);
  return 0;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int ch;
  (void)st; (void)buttons; (void)wheel;
  ch = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) ch = 1;
  return ch;
}

static int app_wants_text(void *st) { (void)st; return 0; }

static int app_tick(void *st, uint32_t now_ms) {
  (void)st;

  if (T.state == ST_RUNNING) {
    uint32_t elapsed = now_ms - T.start_ms;
    uint32_t remain = elapsed >= T.remain_at_start ? 0 : T.remain_at_start - elapsed;
    if (!remain) { go_off(now_ms); return 1; }
    if ((remain + 999) / 1000 != (T.remain_ms + 999) / 1000) {
      int mm, ss;
      T.remain_ms = remain;
      remain_mmss(remain, &mm, &ss);
      if (T.have_at) damage_second(mm);
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
  "digits\ttype minutes/seconds\nleft/right tab\tswitch field\nup/down\t+-1\n"
  "space enter\tstart, pause, resume\nr bksp\treset; again to clear the time\n"
  "any key\tstops the alarm\n",
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
  if (!api->headless()) toolbar_init(api, ACTIONS, NACT, 0, 0);

  if (!api->headless()) state_load(api->ticks_ms());
  minutes =argc > 1 ? parse_minutes(argv[1]) : 0;
  if (minutes) {
    T.set_min = minutes;
    start_or_resume(api->ticks_ms());
  }

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
