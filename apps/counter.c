/* A tally counter: space increments, enter records a lap, del resets.
 *
 * The count is drawn as big seven-segment digits -- the same construction
 * apps/timer.c uses, since there is no scale argument on api->text and a
 * counter's whole point is the number being readable across the room. A
 * ring of 24 ticks behind it lights up clockwise as the count climbs,
 * wrapping every 24 (an odometer, not a percentage: there is no maximum to
 * be a percentage of), and glows on the tick that just lit for a couple of
 * hundred milliseconds -- the only per-frame animation here, so it is the
 * only thing that asks tick() for a repaint, and only while it is running.
 *
 * A lap is the count at the moment enter was pressed, stamped with the wall
 * clock if it is synced and with time-since-open if it is not; newest is
 * always laps[0], which is why adding one is a shift rather than an append.
 * del resets both to zero, but asks first if there is anything to lose --
 * the same y/n pattern apps/habits.c and apps/memo.c use before a delete.
 */

#include "kernel/app/capp.h"

static const CardApi *api;

#define MAX_LAPS   100
#define LAP_ROW_H  11
#define RING_TICKS 24
#define RING_R     22
#define HERO_H     78     /* the ring, the number and the hint below it */
#define PULSE_MS   220    /* how long a freshly-lit tick glows */

#define STATE_DIR  CAPP_VAR "/counter"
#define STATE_PATH STATE_DIR "/state.txt"

#define CLR_BG         CAPP_RGB(16, 18, 24)
#define CLR_TEXT       CAPP_RGB(236, 239, 245)
#define CLR_DIM        CAPP_RGB(120, 128, 142)
#define CLR_DIVIDER    CAPP_RGB(44, 48, 58)
#define CLR_RING_TRACK CAPP_RGB(42, 46, 56)
#define CLR_RING_LIT   CAPP_RGB(96, 210, 196)
#define CLR_LAP_NUM    CAPP_RGB(255, 196, 92)
#define CLR_PANEL      CAPP_RGB(24, 27, 34)
#define CLR_PANEL_ALT  CAPP_RGB(28, 32, 41)
#define CLR_BAR        CAPP_RGB(40, 44, 54)
#define CLR_BAR_FG     CAPP_RGB(232, 236, 244)
#define CLR_WARN       CAPP_RGB(220, 90, 70)

typedef struct {
  uint32_t count;
  char     stamp[12];   /* "14:32:07" if the clock is synced, else "+3:41" */
} Lap;

enum { ASK_NONE = 0, ASK_RESET };

static struct {
  uint32_t count;
  Lap      laps[MAX_LAPS];
  int      nlaps;
  int      top;          /* scroll offset into laps, newest-first */
  int      rows;          /* how many lap rows currently fit */
  int      ask;
  uint32_t pulse_at;      /* ticks_ms() of the last increment, or 0 */
  int      dirty;         /* counted since the last save, not yet written */
  uint32_t start_ms;      /* for elapsed lap stamps when the clock is not synced */
  CRect    content;
} C;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static void text_center(int cx, int y, const char *s, uint16_t fg, uint16_t bg) {
  int w = (int)api->str_len(s) * 6;
  api->text((int16_t)(cx - w / 2), (int16_t)y, s, fg, bg);
}

/* ---- state on the card --------------------------------------------------
 *
 * Whole-file rewrite, the same choice apps/habits.c makes for its logs: the
 * state is a handful of lines, well inside a stack buffer, and a file only
 * ever fully read then fully written cannot be left half-updated by a save
 * that lands mid-count. "count=N" on its own line, then one "lap=N stamp"
 * per lap, newest first -- both plain text, so a card reader or `cat` in
 * Files reads it without this app. */

static int starts_with(const char *s, const char *prefix) {
  while (*prefix) { if (*s != *prefix) return 0; s++; prefix++; }
  return 1;
}

static void apply_line(const char *line) {
  const char *p;
  uint32_t v = 0;
  if (starts_with(line, "count=")) {
    for (p = line + 6; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
    C.count = v;
  } else if (starts_with(line, "lap=") && C.nlaps < MAX_LAPS) {
    Lap *lap = &C.laps[C.nlaps];
    for (p = line + 4; *p >= '0' && *p <= '9'; p++) v = v * 10 + (uint32_t)(*p - '0');
    while (*p == ' ') p++;
    lap->count = v;
    api->fmt(lap->stamp, sizeof lap->stamp, "%s", p);
    C.nlaps++;
  }
}

static void state_load(void) {
  char buf[256], line[64];
  int fd, n, i, len = 0;
  fd = api->open(STATE_PATH, CAPP_O_READ);
  if (fd < 0) return;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      len = 0;
      apply_line(line);
    }
  }
  api->close(fd);
}

static void state_save(void) {
  char line[80];
  int fd, i, n;
  api->mkdir(STATE_DIR);
  fd = api->open(STATE_PATH, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  n = api->fmt(line, sizeof line, "count=%u\n", (unsigned)C.count);
  api->write(fd, line, (size_t)n);
  for (i = 0; i < C.nlaps; i++) {
    n = api->fmt(line, sizeof line, "lap=%u %s\n", (unsigned)C.laps[i].count, C.laps[i].stamp);
    api->write(fd, line, (size_t)n);
  }
  api->close(fd);
  C.dirty = 0;
}

/* ---- the ring --------------------------------------------------------
 *
 * 24 points around a circle, 15 degrees apart, starting at the top and
 * going clockwise -- precomputed rather than called through sin/cos, since
 * an app here links against no libm. (dx, dy) is the unit offset scaled by
 * 1000; a tick's position is the centre plus radius * offset / 1000. */
static const int16_t RING_DX[RING_TICKS] = {
     0, 259, 500, 707, 866, 966,1000, 966, 866, 707, 500, 259,
     0,-259,-500,-707,-866,-966,-1000,-966,-866,-707,-500,-259,
};
static const int16_t RING_DY[RING_TICKS] = {
 -1000,-966,-866,-707,-500,-259,   0, 259, 500, 707, 866, 966,
  1000, 966, 866, 707, 500, 259,   0,-259,-500,-707,-866,-966,
};

static uint16_t lerp_rgb(int r0, int g0, int b0, int r1, int g1, int b1, int t, int tmax) {
  int r = r0 + (r1 - r0) * t / tmax;
  int g = g0 + (g1 - g0) * t / tmax;
  int b = b0 + (b1 - b0) * t / tmax;
  return CAPP_RGB(r, g, b);
}

static void draw_ring(int cx, int cy, int r) {
  int lit = C.count == 0 ? 0 : (int)((C.count - 1) % RING_TICKS) + 1;
  int newest = lit - 1;
  uint32_t since = C.pulse_at ? api->ticks_ms() - C.pulse_at : PULSE_MS;
  int i;

  for (i = 0; i < RING_TICKS; i++) {
    int x = cx + r * (int)RING_DX[i] / 1000;
    int y = cy + r * (int)RING_DY[i] / 1000;
    int on = i < lit;
    uint16_t col = on ? CLR_RING_LIT : CLR_RING_TRACK;
    int sz = on ? 4 : 2;
    if (on && i == newest && since < PULSE_MS) {
      col = lerp_rgb(180, 245, 230, 96, 210, 196, (int)since, PULSE_MS);
      sz = 5;
    }
    api->fill(rect(x - sz / 2, y - sz / 2, sz, sz), col);
  }
}

/* ---- the big number, seven segments per digit -------------------------
 *
 * The same construction as apps/timer.c's clock face: no font is big
 * enough, so each digit is drawn as filled blocks rather than text. The
 * size shrinks as the count grows more digits, so a counter that reaches
 * the thousands still reads rather than running off the ring. */

static void seg_fill(int x, int y, int w, int h, uint16_t fg) {
  if (w > 0 && h > 0) api->fill(rect(x, y, w, h), fg);
}

static void draw_digit(int x, int y, int d, int w, int h, int t, uint16_t fg) {
  static const uint8_t SEG[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
  };
  int midY  = y + h / 2 - t / 2;
  int vTopH = midY - (y + t);
  int vBotY = midY + t;
  int vBotH = (y + h - t) - vBotY;
  uint8_t m = (d >= 0 && d <= 9) ? SEG[d] : 0;

  if (m & 0x01) seg_fill(x + t, y, w - 2 * t, t, fg);
  if (m & 0x02) seg_fill(x + w - t, y + t, t, vTopH, fg);
  if (m & 0x04) seg_fill(x + w - t, vBotY, t, vBotH, fg);
  if (m & 0x08) seg_fill(x + t, y + h - t, w - 2 * t, t, fg);
  if (m & 0x10) seg_fill(x, vBotY, t, vBotH, fg);
  if (m & 0x20) seg_fill(x, y + t, t, vTopH, fg);
  if (m & 0x40) seg_fill(x + t, midY, w - 2 * t, t, fg);
}

static int count_digits(uint32_t v) {
  int n = 1;
  while (v >= 10) { v /= 10; n++; }
  return n;
}

static void digit_size(int n, int *w, int *h, int *t, int *gap) {
  if (n <= 2)      { *w = 14; *h = 26; *t = 5; }
  else if (n == 3) { *w = 11; *h = 22; *t = 4; }
  else if (n == 4) { *w = 9;  *h = 18; *t = 3; }
  else if (n == 5) { *w = 7;  *h = 15; *t = 2; }
  else             { *w = 6;  *h = 12; *t = 2; }
  *gap = *t < 3 ? 2 : 3;
}

static void draw_number(int cx, int y, uint32_t value, uint16_t fg) {
  char digits[10];
  int n = 0, i, w, h, t, gap, total, x;
  uint32_t v = value;
  do { digits[n++] = (char)(v % 10); v /= 10; } while (v && n < (int)sizeof digits);
  digit_size(n, &w, &h, &t, &gap);
  total = n * w + (n - 1) * gap;
  x = cx - total / 2;
  for (i = n - 1; i >= 0; i--) {
    draw_digit(x, y, digits[i], w, h, t, fg);
    x += w + gap;
  }
}

/* ---- laps -------------------------------------------------------------- */

static void stamp_now(char *out, size_t n) {
  CappTime t;
  api->now(&t);
  if (t.synced) {
    api->fmt(out, n, "%02u:%02u:%02u", t.hour, t.min, t.sec);
  } else {
    uint32_t s = (api->ticks_ms() - C.start_ms) / 1000;
    api->fmt(out, n, "+%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  }
}

/* Newest first, so a new lap is a shift rather than an append. */
static void add_lap(void) {
  int i;
  if (C.nlaps < MAX_LAPS) C.nlaps++;
  for (i = C.nlaps - 1; i > 0; i--)
    api->mem_cpy(&C.laps[i], &C.laps[i - 1], sizeof(Lap));
  C.laps[0].count = C.count;
  stamp_now(C.laps[0].stamp, sizeof C.laps[0].stamp);
  C.top = 0;
  state_save();
}

static void do_reset(void) {
  C.count = 0;
  C.nlaps = 0;
  C.top = 0;
  C.pulse_at = 0;
  state_save();
}

/* immediate: save now rather than waiting for the pulse to end. A single
 * tap wants to be durable at once, like every other app's save-on-change;
 * a space held down to fast-count would otherwise rewrite the file every
 * 60 ms while it repeats, which is a lot to ask of an SD card, so a
 * held burst instead saves once, when app_tick sees the pulse expire. */
static void bump(int immediate) {
  C.count++;
  C.pulse_at = api->ticks_ms();
  C.dirty = 1;
  if (immediate) state_save();
}

/* ---- painting ------------------------------------------------------------ */

static CRect hero_rect(void) {
  return rect(C.content.x, C.content.y, C.content.w, HERO_H);
}

static void paint_laps(CRect r) {
  char hdr[24];
  int i, y, header_h = 10;

  api->fmt(hdr, sizeof hdr, "laps  (%d)", C.nlaps);
  api->text((int16_t)(r.x + 6), (int16_t)(r.y + 1), hdr, CLR_DIM, CLR_BG);
  r.y += header_h; r.h -= header_h;
  C.rows = r.h / LAP_ROW_H;

  if (!C.nlaps) {
    api->text((int16_t)(r.x + 6), (int16_t)(r.y + 2), "enter records one", CLR_DIM, CLR_BG);
    return;
  }
  for (i = C.top, y = r.y; i < C.nlaps && i < C.top + C.rows; i++, y += LAP_ROW_H) {
    uint16_t bg = (i % 2) ? CLR_PANEL : CLR_PANEL_ALT;
    char num[12];
    int slen;
    api->fill(rect(r.x, y, r.w, LAP_ROW_H), bg);
    api->fmt(num, sizeof num, "%u", (unsigned)C.laps[i].count);
    api->text((int16_t)(r.x + 6), (int16_t)(y + 1), num, CLR_LAP_NUM, bg);
    slen = (int)api->str_len(C.laps[i].stamp);
    api->text((int16_t)(r.x + r.w - 6 - 6 * slen), (int16_t)(y + 1), C.laps[i].stamp, CLR_DIM, bg);
  }
}

static void paint_status(CRect s) {
  api->fill(s, CLR_BAR);
  if (C.ask == ASK_RESET) {
    char q[48];
    api->fmt(q, sizeof q, "reset and erase %d lap%s?  y/n", C.nlaps, C.nlaps == 1 ? "" : "s");
    api->text((int16_t)(s.x + 4), (int16_t)(s.y + 2), q, CLR_WARN, CLR_BAR);
  } else {
    api->text((int16_t)(s.x + 4), (int16_t)(s.y + 2), "del resets everything", CLR_BAR_FG, CLR_BAR);
  }
}

static void app_paint(void *st, CRect c) {
  int cx, ring_cy, w, h, t, gap, num_y, hint_y, div_y;
  (void)st;
  C.content = c;
  api->fill(c, CLR_BG);

  cx = c.x + c.w / 2;
  ring_cy = c.y + 30;
  draw_ring(cx, ring_cy, RING_R);

  digit_size(count_digits(C.count), &w, &h, &t, &gap);
  num_y = ring_cy - h / 2;
  draw_number(cx, num_y, C.count, CLR_TEXT);

  hint_y = ring_cy + RING_R + 8;
  text_center(cx, hint_y, "space +1   enter lap", CLR_DIM, CLR_BG);

  div_y = hint_y + 14;
  api->fill(rect(c.x + 10, div_y, c.w - 20, 1), CLR_DIVIDER);

  paint_laps(rect(c.x, div_y + 3, c.w, c.y + c.h - 11 - (div_y + 3)));
  paint_status(rect(c.x, c.y + c.h - 11, c.w, 11));
}

/* ---- input ----------------------------------------------------------- */

static int app_key(void *st, uint8_t k) {
  (void)st;
  /* Space is meant to be held for a fast count; nothing else here repeats. */
  if (api->key_repeat() && k != ' ') return 0;

  if (C.ask == ASK_RESET) {
    if (k == 'y' || k == 'Y' || k == CAPP_KEY_ENTER) { C.ask = ASK_NONE; do_reset(); }
    else if (k == 'n' || k == 'N' || k == CAPP_KEY_ESC) C.ask = ASK_NONE;
    return 1;
  }

  switch (k) {
  case ' ':            bump(!api->key_repeat()); return 1;
  case CAPP_KEY_ENTER:  add_lap(); return 1;
  case 0x7F:            /* Del */
    if (C.count || C.nlaps) C.ask = ASK_RESET;
    return 1;
  case CAPP_KEY_ESC:
    return 0;           /* top level: nothing to go back to */
  default:
    return 0;
  }
}

static int app_click(void *st, int16_t x, int16_t y, int button) {
  (void)st; (void)x; (void)button;
  if (C.ask != ASK_NONE) return 0;
  if (y < C.content.y + HERO_H) { bump(1); return 1; }
  return 0;
}

static int app_wants_text(void *st) { (void)st; return 0; }

/* Only the pulse on the ring moves by itself, and only for PULSE_MS after
 * an increment -- damage() keeps that to its own small rectangle so a held
 * space does not reflash the whole screen on every one of the shell's ~5 ms
 * passes. The same expiry is also when a fast-counted burst gets written to
 * the card: see bump(). */
static int app_tick(void *st, uint32_t now_ms) {
  (void)st;
  if (!C.pulse_at) return 0;
  if (now_ms - C.pulse_at >= PULSE_MS) {
    C.pulse_at = 0;
    if (C.dirty) state_save();
  }
  api->damage(hero_rect());
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Counter",
  /* 16x16: a rounded badge with a plus in it. */
  { 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFC, 0x20, 0x04,
    0x20, 0x04, 0x21, 0x84, 0x21, 0x84, 0x27, 0xE4,
    0x27, 0xE4, 0x21, 0x84, 0x21, 0x84, 0x20, 0x04,
    0x20, 0x04, 0x3F, 0xFC, 0x00, 0x00, 0x00, 0x00 },
  "space\t+1\nenter\trecord a lap\ndel\treset, asks first\nclick\t+1 too\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  (void)argc; (void)argv;
  api->mem_set(&C, 0, sizeof C);
  C.start_ms = api->ticks_ms();
  api->mkdir(STATE_DIR);
  state_load();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
