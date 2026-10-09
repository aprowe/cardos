/* Level: a bubble level, from the Cardputer ADV's motion sensor.
 *
 * The test of api->motion (API 37): acceleration says which way is down,
 * and the angle the board leans each way is how far down is from straight
 * through it. Shown as a bubble in a round vial -- green within half a
 * degree -- and as numbers, tenths of a degree, smoothed so they do not
 * flicker between two values.
 *
 * Apps link no libm and no float division, so the angles are integer:
 * atan2 in tenths of a degree from the polynomial
 *   atan(t) = 45t - t(t-1)(14.02 + 3.79t)  degrees, t in [0,1]
 * which is good to about 0.1 degree, and an integer square root.
 *
 * The vial is drawn a row at a time into a row buffer and sent once, so the
 * bubble moving is never a clear and a redraw.
 *
 *   c   take the way it is lying now as level (a surface that is not)
 *   r   back to true level
 *   h   hold the reading
 */
#include "kernel/app/capp.h"
#include "apps/footer.h"

static const CardApi *api;

#define R        52              /* the vial's radius */
#define CX       (8 + R)         /* its centre, from the content's corner */
#define CY       (6 + R)
#define FULL     200             /* tenths of a degree at the vial's edge */
#define BUBBLE   9
#define TICK_MS  50

#define CLR_BG     CAPP_RGB(15, 17, 23)
#define CLR_VIAL   CAPP_RGB(28, 52, 40)
#define CLR_RING   CAPP_RGB(96, 108, 118)
#define CLR_MARK   CAPP_RGB(70, 96, 82)
#define CLR_BUBBLE CAPP_RGB(236, 240, 190)
#define CLR_LEVEL  CAPP_RGB(120, 232, 140)
#define CLR_TEXT   CAPP_RGB(232, 236, 244)
#define CLR_DIM    CAPP_RGB(128, 136, 152)

static struct {
  int      have;                 /* the sensor answered */
  int      x10, y10;             /* smoothed angles, tenths of a degree */
  int      zx, zy;               /* calibration */
  int      hold;
  int      bx, by;               /* bubble, as drawn */
  int      sx, sy;               /* numbers, as drawn */
  uint32_t next;
  int      f_big, f_ui;
  CRect    area;
  uint16_t row[2 * R + 1];
} L;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* ---- integer angles ---------------------------------------------------------------- */

/* atan(r / 1024), r in 0..1024, in tenths of a degree. */
static int atan_unit(int r) {
  int p = r * (1024 - r) / 1024;               /* -t(t-1), times 1024 */
  return 450 * r / 1024 + p * (1402 + 379 * r / 1024) / 10240;
}

static int atan2_10(int y, int x) {
  int ay = y < 0 ? -y : y, ax = x < 0 ? -x : x, a;
  if (!ax && !ay) return 0;
  if (ax >= ay) a = atan_unit(ay * 1024 / ax);
  else a = 900 - atan_unit(ax * 1024 / ay);
  if (x < 0) a = 1800 - a;
  return y < 0 ? -a : a;
}

static int isqrt(int v) {
  int r = 0, b = 1 << 30;
  if (v <= 0) return 0;
  while (b > v) b >>= 2;
  while (b) {
    if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
    else r >>= 1;
    b >>= 2;
  }
  return r;
}

/* ---- paint -------------------------------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int is_level(void) {
  int x = L.x10 - L.zx, y = L.y10 - L.zy;
  return x > -5 && x < 5 && y > -5 && y < 5;
}

/* Where the bubble sits for the angles now: it rises to the high side, so
 * it moves against the tilt, and stops at the vial's edge. */
static void bubble_at(int *bx, int *by) {
  int x = clampi(L.x10 - L.zx, -FULL, FULL), y = clampi(L.y10 - L.zy, -FULL, FULL);
  int lim = R - BUBBLE - 3;
  *bx = -x * lim / FULL;
  *by = y * lim / FULL;
}

static void paint_vial(void) {
  int dy, dx, ox = L.area.x + CX - R, oy = L.area.y + CY - R;
  int r2 = R * R, rin = (R - 3) * (R - 3), half = (R / 2) * (R / 2);
  uint16_t bub = is_level() ? CLR_LEVEL : CLR_BUBBLE;
  CRect clip = api->paint_area();
  for (dy = -R; dy <= R; dy++) {
    int sy = oy + dy + R;
    if (sy < clip.y || sy >= clip.y + clip.h) continue;
    for (dx = -R; dx <= R; dx++) {
      int d2 = dx * dx + dy * dy, b2 = (dx - L.bx) * (dx - L.bx) + (dy - L.by) * (dy - L.by);
      uint16_t c;
      if (d2 > r2) c = CLR_BG;
      else if (d2 > rin) c = CLR_RING;
      else if (b2 <= BUBBLE * BUBBLE) c = bub;
      else if ((d2 >= half - R && d2 <= half + R) || dx == 0 || dy == 0 ||
               (d2 >= 100 - 10 && d2 <= 100 + 10)) c = CLR_MARK;
      else c = CLR_VIAL;
      L.row[dx + R] = c;
    }
    api->pixels(rect(ox, sy, 2 * R + 1, 1), L.row);
  }
}

static void angle_text(char *out, int n, int v) {
  int a = v < 0 ? -v : v;
  api->fmt(out, (size_t)n, "%s%d.%d  ", v < 0 ? "-" : " ", a / 10, a % 10);
}

static void paint_numbers(void) {
  char line[24];
  int x = L.area.x + 2 * R + 26, y = L.area.y + 10;
  api->text_font(L.f_ui, (int16_t)x, (int16_t)y, "left-right", CLR_DIM, CLR_BG);
  angle_text(line, sizeof line, L.x10 - L.zx);
  api->text_font(L.f_big, (int16_t)x, (int16_t)(y + 16), line, CLR_TEXT, CLR_BG);
  api->text_font(L.f_ui, (int16_t)x, (int16_t)(y + 44), "front-back", CLR_DIM, CLR_BG);
  angle_text(line, sizeof line, L.y10 - L.zy);
  api->text_font(L.f_big, (int16_t)x, (int16_t)(y + 60), line, CLR_TEXT, CLR_BG);
  api->text_font(L.f_ui, (int16_t)x, (int16_t)(y + 88),
                 L.hold ? "held        " : is_level() ? "level       " : "            ",
                 L.hold ? CLR_DIM : CLR_LEVEL, CLR_BG);
  L.sx = L.x10 - L.zx;
  L.sy = L.y10 - L.zy;
}

static CRect numbers_rect(void) {
  return rect(L.area.x + 2 * R + 26, L.area.y + 10, L.area.w - (2 * R + 26), 104);
}

static CRect vial_rect(void) {
  return rect(L.area.x + CX - R, L.area.y + CY - R, 2 * R + 1, 2 * R + 1);
}

static void app_paint(void *st, CRect c) {
  CRect a = api->paint_area();
  (void)st;
  L.area = c;
  if (!L.have) {
    api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_BG);
    api->text_font(L.f_ui, (int16_t)(c.x + 10), (int16_t)(c.y + 20), "No motion sensor here.", CLR_TEXT, CLR_BG);
    api->text_font(L.f_ui, (int16_t)(c.x + 10), (int16_t)(c.y + 40), "The Cardputer ADV has one;", CLR_DIM, CLR_BG);
    api->text_font(L.f_ui, (int16_t)(c.x + 10), (int16_t)(c.y + 56), "the original does not.", CLR_DIM, CLR_BG);
    footer_paint(api, c, "fn-` leaves");
    return;
  }
  /* The whole screen once; after that the vial and the numbers mark
   * themselves, and nothing else is touched. */
  if (a.w >= c.w && a.h >= c.h - FOOT_H) {
    api->fill(rect(c.x, c.y, CX - R, c.h - FOOT_H), CLR_BG);
    api->fill(rect(c.x + CX + R + 1, c.y, c.w - (CX + R + 1), c.h - FOOT_H), CLR_BG);
    api->fill(rect(c.x + CX - R, c.y, 2 * R + 1, CY - R), CLR_BG);
    api->fill(rect(c.x + CX - R, c.y + CY + R + 1, 2 * R + 1, c.h - FOOT_H - (CY + R + 1)), CLR_BG);
    footer_paint(api, c, "c set level  r reset  h hold");
  }
  paint_vial();
  paint_numbers();
}

/* ---- the sensor --------------------------------------------------------------------- */

static int app_tick(void *st, uint32_t now) {
  CappMotion m;
  int x, y, nbx, nby, moved = 0;
  (void)st;
  if (!L.have || L.hold || (int32_t)(now - L.next) < 0) return 0;
  L.next = now + TICK_MS;
  if (api->motion(&m) != 0) return 0;
  /* Left-right is the lean about the sensor's y axis, front-back about x. */
  x = atan2_10(m.ax, isqrt(m.ay * m.ay + m.az * m.az));
  y = atan2_10(m.ay, isqrt(m.ax * m.ax + m.az * m.az));
  /* Smoothed: a quarter of the way to the new reading each time. */
  L.x10 += (x - L.x10) / 4;
  L.y10 += (y - L.y10) / 4;
  bubble_at(&nbx, &nby);
  if (nbx != L.bx || nby != L.by) {
    L.bx = nbx; L.by = nby;
    api->damage(vial_rect());
    moved = 1;
  }
  if (L.x10 - L.zx != L.sx || L.y10 - L.zy != L.sy) {
    api->damage(numbers_rect());
    moved = 1;
  }
  return moved;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (!L.have) return 0;
  switch (k) {
  case 'c': case 'C': L.zx = L.x10; L.zy = L.y10; break;
  case 'r': case 'R': L.zx = L.zy = 0; break;
  case 'h': case 'H': case ' ': L.hold = !L.hold; break;
  default: return 0;
  }
  bubble_at(&L.bx, &L.by);
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_PAINT_DIRECT,   /* paint moves the bubble it draws */
  "Level",
  /* 16x16: a round vial with a bubble. */
  { 0x07, 0xE0, 0x18, 0x18, 0x20, 0x04, 0x40, 0x02,
    0x41, 0x82, 0x83, 0xC1, 0x83, 0xC1, 0xFF, 0xFF,
    0x81, 0x81, 0x81, 0x81, 0x40, 0x02, 0x40, 0x02,
    0x20, 0x04, 0x18, 0x18, 0x07, 0xE0, 0x00, 0x00 },
  "c\ttake the way it lies now as level\n"
  "r\tback to true level\n"
  "h, space\thold the reading\n"
  "\n"
  "needs the Cardputer ADV's motion sensor.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  CappMotion m;
  (void)argc; (void)argv;
  api = a;
  L.f_big = api->font_load("ui13b");
  L.f_ui = api->font_load("ui13");
  L.have = api->motion(&m) == 0;
  if (L.have) {
    L.x10 = atan2_10(m.ax, isqrt(m.ay * m.ay + m.az * m.az));
    L.y10 = atan2_10(m.ay, isqrt(m.ax * m.ax + m.az * m.az));
    bubble_at(&L.bx, &L.by);
    api->keep_awake(1);              /* nobody presses keys at a level */
  }
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  api->ui(&UI);
  return 0;
}
