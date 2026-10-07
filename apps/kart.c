/* Kart: a race in the manner of SNES Super Mario Kart -- Mode 7 road,
 * billboard karts -- steered by tilting the Cardputer ADV.
 *
 * The game is apps/kart.h, and this is the app around it: the title, the
 * race, the results, the controls and the clock.
 *
 * STEERING. Tilt it like a wheel: the roll from how it was held when the
 * race began is the steering, full lock at about 25 degrees. Lean it back,
 * the top edge away from you, to brake. Space, or the button on top (G0),
 * uses the item. On the original Cardputer, which has no motion sensor, the
 * arrows steer (left/right) and brake (down), and Space uses the item.
 * Which axis is the wheel and which way is left depends on how the sensor
 * sits on the board, so `x` swaps the axes and `i` turns it round; both are
 * kept in /config/kart.txt, with the best time.
 *
 * DRAWING. Physics runs in fixed 16 ms steps from tick; a frame is asked for
 * with damage() and drawn in paint, a strip of rows at a time into one
 * buffer and blitted -- nothing is drawn on the panel twice, so nothing
 * flickers.
 */
#include "kernel/app/capp.h"
#include "apps/kart.h"
#include "apps/safefile.h"

#define CONF     "/config/kart.txt"
#define FRAME_MS 30

enum { S_TITLE, S_RACE, S_RESULTS };

static const CardApi *api;
static uint16_t strip[K_W * K_STRIP];

static struct {
  int      screen;
  int      have_motion;
  int      swap, invert;           /* tilt: which axis steers, which way */
  int      roll0, pitch0;          /* neutral, tenths of a degree */
  int      roll, pitch;            /* now, smoothed */
  int      key_steer;              /* -256..256, decaying: keyboard steering */
  uint32_t key_at;
  int      key_brake_until;
  int      use;                    /* an item use waiting for the next step */
  uint32_t last_ms, acc_ms, frame_at;
  uint32_t best_ms;
  int      new_best;
  CRect    c;
  int      have_c;
} A;

/* ---- tilt --------------------------------------------------------------------- */

static int atan2_10(int y, int x) {              /* tenths of a degree, -1800..1800 */
  return (int)(((k_atan2(y, x) + 512) & 1023) - 512) * 3600 / 1024;
}

static void read_tilt(int smooth) {
  CappMotion m;
  int ax, ay, roll, pitch;
  if (!A.have_motion || api->motion(&m) != 0) return;
  ax = A.swap ? m.ay : m.ax;
  ay = A.swap ? m.ax : m.ay;
  roll = atan2_10(ax, k_isqrt(ay * ay + m.az * m.az));
  pitch = atan2_10(ay, k_isqrt(ax * ax + m.az * m.az));
  if (smooth) { A.roll += (roll - A.roll) / 2; A.pitch += (pitch - A.pitch) / 2; }
  else { A.roll = roll; A.pitch = pitch; }
}

static int tilt_steer(void) {
  int d = A.roll - A.roll0, s;
  /* The ADV's sensor reads roll the other way round from the steering:
   * tilting right came out as left (2026-10-06). `i` turns it back. */
  if (!A.invert) d = -d;
  if (d > -20 && d < 20) return 0;                         /* 2 degrees of nothing */
  s = d * 256 / 250;                                       /* full lock at 25 degrees */
  return s > 256 ? 256 : s < -256 ? -256 : s;
}

static int tilt_brake(void) { return A.pitch - A.pitch0 < -180; }   /* leaned back 18 degrees */

/* ---- settings ------------------------------------------------------------------- */

static int num(const char *s, int *v) {
  int n = 0, any = 0;
  while (*s >= '0' && *s <= '9') { n = n * 10 + (*s++ - '0'); any = 1; }
  if (any) *v = n;
  return any;
}

static void conf_load(void) {
  char b[64];
  int fd = safe_open_read(api, CONF), n, v;
  if (fd < 0) return;
  n = api->read(fd, b, sizeof b - 1);
  api->close(fd);
  if (n <= 0) return;
  b[n] = 0;
  /* "swap invert best_ms" */
  {
    const char *p = b;
    if (num(p, &v)) A.swap = v;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (num(p, &v)) A.invert = v;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (num(p, &v)) A.best_ms = (uint32_t)v;
  }
}

static void conf_save(void) {
  SafeFile f;
  char b[48];
  int n = api->fmt(b, sizeof b, "%d %d %lu\n", A.swap, A.invert, (unsigned long)A.best_ms);
  if (safe_begin(&f, api, CONF) != 0) return;
  safe_write(&f, b, (size_t)n);
  safe_commit(&f);
}

/* ---- the screens -------------------------------------------------------------- */

static void mark(void) { if (A.have_c) api->damage(A.c); }

static void start_race(void) {
  k_new_race(api->ticks_ms() | 1);
  read_tilt(0);
  A.roll0 = A.roll; A.pitch0 = A.pitch;    /* however it is held now is straight */
  A.key_steer = 0; A.use = 0;
  A.acc_ms = 0;
  A.last_ms = api->ticks_ms();
  A.new_best = 0;
  A.screen = S_RACE;
  api->keep_awake(1);
  k_camera();
  mark();
}

static void to_title(void) {
  A.screen = S_TITLE;
  api->keep_awake(0);
  k_new_race(1);
  k_camera();
  mark();
}

/* A dark band behind text, for the title and the results. */
static void shade_row(uint16_t *row) {
  int x;
  for (x = 0; x < K_W; x++) {
    uint16_t c = row[x];
    /* Halve each channel; CAPP_RGB swaps bytes, so do it on the swapped form:
     * clearing each field's top bit and shifting works the same either way
     * round only for a plain 565, so undo, halve, redo. */
    uint16_t p = (uint16_t)((c >> 8) | (c << 8));
    p = (uint16_t)((p >> 1) & 0x7BEF);
    row[x] = (uint16_t)((p >> 8) | (p << 8));
  }
}

static const char *NAMES[K_KARTS] = { "YOU", "GREEN", "YELLOW", "BLUE", "PINK", "ORANGE" };

static void overlay_title(uint16_t *row, int y) {
  uint16_t gold = CAPP_RGB(255, 214, 60), white = CAPP_RGB(255, 255, 255), dim = CAPP_RGB(190, 200, 220);
  char b[32];
  if (y >= 28 && y < 112) shade_row(row);
  k_text_row(row, y, K_W / 2 - 4 * 12 * 3 / 2 + 2, 32, "KART", 3, gold);
  k_text_row(row, y, 30, 64, A.have_motion ? "tilt to steer, lean back" : "arrows steer, down brakes", 1, white);
  k_text_row(row, y, 30, 74, A.have_motion ? "to brake. space: item" : "space: use the item", 1, white);
  k_text_row(row, y, 30, 88, "enter: race", 1, gold);
  if (A.have_motion) {
    api->fmt(b, sizeof b, "x axis %s  i %s", A.swap ? "Y" : "X", A.invert ? "flipped" : "normal");
    k_text_row(row, y, 30, 98, b, 1, dim);
  }
  if (A.best_ms) {
    char t[8];
    k_time(t, A.best_ms);
    api->fmt(b, sizeof b, "best %s", t);
    k_text_row(row, y, K_W - 6 * 10 - 6, 116, b, 1, dim);
  }
}

static void overlay_results(uint16_t *row, int y) {
  uint16_t gold = CAPP_RGB(255, 214, 60), white = CAPP_RGB(255, 255, 255), dim = CAPP_RGB(190, 200, 220);
  int p, i;
  char b[32], t[8];
  if (y >= 16 && y < 128) shade_row(row);
  k_text_row(row, y, 40, 20, k_place_word(K.k[0].place), 2, K.k[0].place == 1 ? gold : white);
  k_text_row(row, y, 40 + 3 * 12 + 8, 24, A.new_best ? "new best!" : "place", 1, A.new_best ? gold : dim);
  for (p = 1; p <= K_KARTS; p++)
    for (i = 0; i < K_KARTS; i++) {
      if (K.k[i].place != p) continue;
      if (K.k[i].done) k_time(t, K.k[i].done_ms);
      else { t[0] = '-'; t[1] = '-'; t[2] = 0; }
      api->fmt(b, sizeof b, "%d %-7s %s", p, NAMES[i], t);
      k_text_row(row, y, 40, 30 + p * 12, b, 1, i == 0 ? gold : white);
      if (y >= 30 + p * 12 && y < 38 + p * 12) {           /* their colour */
        int x;
        for (x = 30; x < 36; x++) row[x] = K.k[i].body;
      }
    }
  k_text_row(row, y, 40, 116, "enter: again   esc: title", 1, dim);
}

static void app_paint(void *st, CRect c) {
  int y;
  (void)st;
  A.c = c;
  A.have_c = 1;
  for (y = 0; y < K_H; y += K_STRIP) {
    int h = K_H - y < K_STRIP ? K_H - y : K_STRIP, r;
    CRect s;
    k_render(strip, y, h);
    for (r = 0; r < h; r++) {
      if (A.screen == S_TITLE) overlay_title(strip + r * K_W, y + r);
      else if (A.screen == S_RESULTS) overlay_results(strip + r * K_W, y + r);
    }
    s.x = c.x; s.y = (int16_t)(c.y + y); s.w = K_W; s.h = (int16_t)h;
    api->pixels(s, strip);
  }
}

/* ---- time ------------------------------------------------------------------------ */

static int app_tick(void *st, uint32_t now) {
  int steps = 0;
  (void)st;
  if (A.screen == S_TITLE) {
    /* The grid, slowly turning in place, behind the title. */
    if ((int32_t)(now - A.frame_at) < 60) return 0;
    A.frame_at = now;
    K.k[0].ang = (uint16_t)(K.k[0].ang + 90);
    k_camera();
    mark();
    return 1;
  }
  if (A.screen == S_RESULTS) return 0;

  A.acc_ms += now - A.last_ms;
  A.last_ms = now;
  if (A.acc_ms > 100) A.acc_ms = 100;                     /* after a stall, do not run on */
  read_tilt(1);
  while (A.acc_ms >= K_STEP_MS && steps < 6) {
    int steer, brake;
    if (A.have_motion) { steer = tilt_steer(); brake = tilt_brake(); }
    else { steer = 0; brake = 0; }
    /* The keyboard, on either board: a press steers hard and lets go over
     * 420 ms -- longer than a held key's first repeat. */
    if (A.key_steer) {
      int age = (int)(now - A.key_at);
      int k = age >= 420 ? 0 : A.key_steer * (420 - age) / 420;
      if (!k) A.key_steer = 0;
      if (k) steer = k;
    }
    if ((int32_t)(now - (uint32_t)A.key_brake_until) < 0) brake = 1;
    k_step(K_STEP_MS, steer, brake, A.use);
    A.use = 0;
    A.acc_ms -= K_STEP_MS;
    steps++;
  }
  if (K.phase == PH_DONE && K.done_ms > 2500 && A.screen == S_RACE) {
    A.screen = S_RESULTS;
    api->keep_awake(0);
    if (!A.best_ms || K.k[0].done_ms < A.best_ms) { A.best_ms = K.k[0].done_ms; A.new_best = 1; conf_save(); }
    k_camera();
    mark();
    return 1;
  }
  if ((int32_t)(now - A.frame_at) >= FRAME_MS) {
    A.frame_at = now;
    k_camera();
    mark();
    return 1;
  }
  return 0;
}

/* ---- keys ------------------------------------------------------------------------- */

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (A.screen == S_TITLE) {
    switch (k) {
    case CAPP_KEY_ENTER: case ' ': start_race(); return 1;
    case 'x': case 'X': A.swap = !A.swap; conf_save(); mark(); return 1;
    case 'i': case 'I': A.invert = !A.invert; conf_save(); mark(); return 1;
    default: return 0;
    }
  }
  if (A.screen == S_RESULTS) {
    if (k == CAPP_KEY_ENTER || k == ' ') { start_race(); return 1; }
    if (k == CAPP_KEY_ESC) { to_title(); return 1; }
    return 0;
  }
  switch (k) {
  case CAPP_KEY_LEFT:  A.key_steer = -256; A.key_at = api->ticks_ms(); return 1;
  case CAPP_KEY_RIGHT: A.key_steer = 256;  A.key_at = api->ticks_ms(); return 1;
  case CAPP_KEY_DOWN:  A.key_brake_until = (int)(api->ticks_ms() + 250); return 1;
  case ' ':            A.use = 1; return 1;
  case CAPP_KEY_ESC:   to_title(); return 1;
  default: return 0;
  }
}

/* The button on top uses the item, as a press. */
static int app_button(void *st, int event, const char *text) {
  (void)st; (void)text;
  if (event == CAPP_G0_ASK) return A.screen == S_RACE ? CAPP_G0_PRESS : CAPP_G0_NONE;
  if (event == CAPP_G0_PRESSED) { A.use = 1; return 1; }
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Kart",
  /* 16x16: a kart from behind. */
  { 0x07, 0xE0, 0x08, 0x10, 0x08, 0x10, 0x07, 0xE0,
    0x1F, 0xF8, 0x20, 0x04, 0x7F, 0xFE, 0xFF, 0xFF,
    0xFF, 0xFF, 0xF0, 0x0F, 0xFF, 0xFF, 0x6F, 0xF6,
    0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00 },
  "A race, three laps against five.\n"
  "tilt\tsteer, like a wheel (Cardputer ADV)\nlean back\tbrake\n"
  "left/right\tsteer (no motion sensor)\ndown\tbrake\n"
  "space, G0\tuse the item: a mushroom is a burst of speed,\n\ta banana is left behind you\n"
  "enter\tstart a race\nesc\tback to the title\n"
  "x\tthe other tilt axis steers\ni\tsteer the other way round\n"
  "\nCoins add a little top speed, up to ten; a banana\ncosts two. The grass is slow.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  CappMotion m;
  (void)argc; (void)argv;
  api = a;
  A.have_motion = api->motion && api->motion(&m) == 0;
  conf_load();
  k_build();
  to_title();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.button = app_button;
  api->ui(&UI);
  return 0;
}
