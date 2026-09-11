/* The launcher. See launchui.h.
 *
 * A carousel rather than a grid. The panel is 240x135 -- very wide and very
 * short -- so a grid of small icons wastes the width and makes every label
 * unreadable, while one large icon flanked by its neighbours uses the shape
 * the hardware actually has. It is also how a launcher is meant to feel: you
 * move along a row of things rather than hunting a cell in a table.
 *
 * Icons are 16x16 drawn at 4x, so they are pixel art at 64x64 rather than
 * blurred enlargements. The selected one is full size and full contrast, its
 * neighbours half that and dimmed, which puts the focus somewhere without
 * needing a highlight box around it.
 *
 * There is no compositor here. The whole screen is either the carousel or the
 * running app, so a repaint is a handful of rectangles and nothing overlaps.
 */

#include "kernel/ui/launchui.h"
#include "kernel/ui/icons.h"
#include "kernel/ui/draw.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/shell.h"
#include "kernel/app/capprun.h"
#include "kernel/app/capp.h"
#include "kernel/drv/keyboard.h"
#include "kernel/drv/bthid.h"
#include "kernel/net/wifi.h"

#include <stdio.h>
#include <string.h>

/* ---- geometry ------------------------------------------------------------
 *
 * 240x135, laid out around the centre icon:
 *
 *   y 0..11     status strip
 *   y 16..79    icons: 64x64 centred, 32x32 neighbours on the same centreline
 *   y 86..101   the name, at 2x
 *   y 107..114  what it is, or what just happened
 *   y 124..126  position pips
 */
#define BAR_H     12
#define BIG       64          /* 16 x 4 */
#define SMALL     32          /* 16 x 2 */
#define ICON_TOP  16
#define BIG_X     ((DISPLAY_W - BIG) / 2)
#define SIDE_GAP  12
#define NAME_Y    86
#define KIND_Y    107
#define PIP_Y     124
#define PIP_W     3

static int      s_sel;
static int      s_dirty;
static uint32_t s_now_ms;
static char     s_note[48];

/* The app currently running fullscreen, or NULL for the carousel. */
static const AppDef *s_app;
static int            s_app_dirty;
static int            s_app_clear;    /* the screen still has the carousel on it */
static Rect           s_app_rect;

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* Wrapping, because a carousel that stops at the ends is a list. */
static int wrap(int i) {
  int n = icons_count();
  if (n <= 0) return 0;
  return ((i % n) + n) % n;
}

static const char *kind_word(const Icon *ic) {
  if (!ic) return "";
  switch (ic->kind) {
  case ICON_FIRMWARE: return "firmware - replaces CardOS";
  case ICON_CAPP:     return "app";
  default:            return "built in";
  }
}

/* ------------------------------------------------------------- paint ---- */

static void paint_bar(void) {
  char right[28];

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, BAR_H), C_TITLE);
  draw_text(4, 2, "CardOS", C_TITLE_FG, C_TITLE);

  /* The two things worth knowing here are whether the radios are up. */
  snprintf(right, sizeof right, "%s%s%u:%02u",
           wifi_is_connected() ? "wifi  " : "",
           bthid_state(BTHID_MOUSE) == BTH_CONNECTED ? "mouse  " : "",
           (unsigned)(s_now_ms / 60000u) % 100u,
           (unsigned)((s_now_ms / 1000u) % 60u));
  draw_text_ellipsis((int16_t)(DISPLAY_W - 128), 2, 124, right,
                     C_TITLE_FG, C_TITLE);
}

static void paint_icon(int idx, int16_t x, int16_t y, int scale, uint16_t fg) {
  const uint8_t *bits = icon_bitmap(idx);
  if (!bits) return;
  draw_bitmap1_scaled(x, y, CAPP_ICON_W, CAPP_ICON_H, bits, scale,
                      fg, C_DESKTOP);
}

static void paint_pips(int n) {
  int16_t total, x;
  int i;

  draw_rect(R(0, PIP_Y, DISPLAY_W, PIP_W), C_DESKTOP);
  /* One app needs no map, and past twenty the pips are a solid bar. */
  if (n < 2 || n > 20) return;

  total = (int16_t)(n * (PIP_W + 4) - 4);
  x = (int16_t)((DISPLAY_W - total) / 2);
  for (i = 0; i < n; i++)
    draw_rect(R(x + i * (PIP_W + 4), PIP_Y, PIP_W, PIP_W),
              i == s_sel ? C_TITLE_FG : C_SHADOW);
}

static void paint_carousel(void) {
  int n = icons_count();
  const Icon *ic;

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, BAR_H, DISPLAY_W, DISPLAY_H - BAR_H), C_DESKTOP);

  if (n == 0) {
    draw_text_scaled(28, 46, "no apps", 2, C_TITLE_FG, C_DESKTOP);
    draw_text(28, 74, "put .capp or .bin files", C_SHADOW, C_DESKTOP);
    draw_text(28, 86, "in /desktop, then press r", C_SHADOW, C_DESKTOP);
    return;
  }

  /* Neighbours are dimmed as well as smaller: at 32x32 a full-contrast icon
   * still competes with the one in focus. */
  if (n > 1) {
    int16_t sy = (int16_t)(ICON_TOP + (BIG - SMALL) / 2);
    paint_icon(wrap(s_sel - 1), (int16_t)(BIG_X - SIDE_GAP - SMALL), sy, 2, C_SHADOW);
    paint_icon(wrap(s_sel + 1), (int16_t)(BIG_X + BIG + SIDE_GAP), sy, 2, C_SHADOW);
  }
  paint_icon(s_sel, BIG_X, ICON_TOP, 4, C_TITLE_FG);

  ic = icon_at(s_sel);
  if (ic) {
    /* Centred on the glyph width rather than a guess, so a long name and a
     * short one both sit under the icon. */
    int16_t w = (int16_t)(draw_text_width(ic->name) * 2);
    int16_t nx = (int16_t)((DISPLAY_W - w) / 2);
    const char *k = s_note[0] ? s_note : kind_word(ic);
    int16_t kx = (int16_t)((DISPLAY_W - draw_text_width(k)) / 2);

    if (nx < 2) nx = 2;
    if (kx < 2) kx = 2;
    draw_text_scaled(nx, NAME_Y, ic->name, 2, C_TITLE_FG, C_DESKTOP);
    draw_text_ellipsis(kx, KIND_Y, (int16_t)(DISPLAY_W - 4), k,
                       C_SHADOW, C_DESKTOP);
  }

  paint_pips(n);
}

/* Where an app sits. One that asked for a size smaller than the panel gets
 * exactly that, centred, rather than being stretched -- Minesweeper's board is
 * 90x106 and has no meaningful way to fill 240x135. */
static Rect app_rect(const AppDef *a) {
  int16_t w = DISPLAY_W, h = DISPLAY_H;
  if (a->pref_w > 0 && a->pref_w < w) w = a->pref_w;
  if (a->pref_h > 0 && a->pref_h < h) h = a->pref_h;
  return R((DISPLAY_W - w) / 2, (DISPLAY_H - h) / 2, w, h);
}

static void paint_app(void) {
  /* The carousel is still on the panel when an app opens, and an app that does
   * not cover every pixel would otherwise be drawn on top of it. Clearing is
   * done once on entry rather than every frame: doing it per frame would make
   * anything that repaints itself flicker. */
  if (s_app_clear) {
    s_app_clear = 0;
    draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
    draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_DESKTOP);
    draw_rect(s_app_rect, C_WHITE);
    if (s_app_rect.w < DISPLAY_W || s_app_rect.h < DISPLAY_H)
      draw_frame(rect_inset(s_app_rect, -1), C_SHADOW);
  }

  /* Clipped to its own rectangle, so an app that draws past its declared size
   * cannot scribble over the surround it does not own. */
  draw_set_clip(s_app_rect);
  if (s_app->paint) s_app->paint(s_app->state, s_app_rect);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}

static void flush(void) {
  if (s_app) {
    if (!s_app_dirty) return;
    s_app_dirty = 0;
    paint_app();
    return;
  }
  if (!s_dirty) return;
  s_dirty = 0;
  paint_bar();
  paint_carousel();
}

/* ------------------------------------------------------------ running --- */

void launchui_repaint(void) {
  if (s_app) s_app_dirty = 1;
  else s_dirty = 1;
  flush();
}

static void leave_app(void) {
  s_app = NULL;
  s_note[0] = 0;
  s_dirty = 1;
  flush();
}

static void launch(int i) {
  const Icon *ic = icon_at(i);
  const AppDef *a;

  if (!ic) return;

  if (ic->kind == ICON_FIRMWARE) {
    snprintf(s_note, sizeof s_note, "booting...");
    s_dirty = 1;
    flush();
    icons_boot_firmware(i);                /* does not return on success */
    snprintf(s_note, sizeof s_note, "not a bootable image");
    s_dirty = 1;
    flush();
    return;
  }

  a = icon_app(i);
  if (!a) return;
  if (ic->kind == ICON_CAPP) capprun_set_file(ic->slot, ic->path);
  if (a->open) a->open(a->state);

  /* Everything runs fullscreen here, whatever size it asked for: there is no
   * desktop behind it for a window to sit on. */
  s_app = a;
  s_app_rect = app_rect(a);
  s_app_clear = 1;
  s_app_dirty = 1;
  flush();
}

static void move(int delta) {
  if (icons_count() == 0) return;
  s_sel = wrap(s_sel + delta);
  s_note[0] = 0;
  s_dirty = 1;
  flush();
}

/* -------------------------------------------------------------- input --- */

void launchui_init(void) {
  ui_set_shell(UI_LAUNCHER);
  icons_reload();
  mouse_init(DISPLAY_W, DISPLAY_H);
  s_sel = 0;
  s_app = NULL;
  s_note[0] = 0;
  s_dirty = 1;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_DESKTOP);
  flush();
}

int launchui_key(uint8_t key) {
  /* ; . , / are the arrow cluster here without needing Fn. The carousel takes
   * no text at all, and a running app only gets the raw keys back while it is
   * actually taking some. */
  if (!s_app || !s_app->wants_text || !s_app->wants_text(s_app->state)) {
    uint8_t arrow = keyboard_arrow_for(key);
    if (arrow) key = arrow;
  }

  if (s_app) {
    if (key == KEY_ESC) { leave_app(); return 0; }
    if (s_app->key && s_app->key(s_app->state, key)) {
      s_app_dirty = 1;
      flush();
    }
    return 0;
  }

  switch (key) {
  case KEY_ESC: desktop_set_autostart(0); return 1;     /* to the console */

  /* Up and down move along the row too. There is nothing else to move, and a
   * key that does nothing is worse than a duplicate. */
  case KEY_LEFT:
  case KEY_UP:    move(-1); return 0;
  case KEY_RIGHT:
  case KEY_DOWN:  move(1);  return 0;

  case KEY_ENTER:
  case ' ':
    if (icons_count()) launch(s_sel);
    return 0;

  case 'r': case 'R':
    icons_reload();
    if (s_sel >= icons_count()) s_sel = 0;
    snprintf(s_note, sizeof s_note, "%d apps", icons_count());
    s_dirty = 1;
    flush();
    return 0;

  case 'd': case 'D':
    /* The desktop is one keystroke away rather than a mode to configure. */
    desktop_init();
    return 2;

  default:
    return 0;
  }
}

void launchui_tick(uint32_t ms) {
  uint32_t before = s_now_ms / 1000u;
  s_now_ms = ms;
  if (s_now_ms / 1000u == before) return;
  if (s_app) return;               /* the app owns the screen */

  /* A mouse that wanders out of range or sleeps drops the link. Look for it
   * again rather than sitting there with a dead pointer -- but not every
   * second, because each attempt is a multi-second scan. */
  if (bthid_state(BTHID_MOUSE) == BTH_FAILED && (s_now_ms / 1000u) % 15 == 0)
    bthid_start(4, BTHID_MOUSE);

  paint_bar();                     /* just the clock strip */
}

/* The launcher draws no pointer. There is nothing to drag, and a cursor would
 * have to be erased by repainting whatever is under it -- which during a
 * fullscreen app only the app knows how to do. The wheel and the two sides of
 * the screen move the carousel instead, and a click on the centre opens. */
void launchui_mouse_apply(const MouseReport *r) {
  int btn, wheel;

  mouse_apply(r);
  btn = mouse_pressed(MOUSE_LEFT) ? CAPP_BTN_LEFT
      : mouse_pressed(MOUSE_RIGHT) ? CAPP_BTN_RIGHT : 0;
  wheel = mouse_take_wheel();
  mouse_released(MOUSE_LEFT);
  mouse_released(MOUSE_RIGHT);
  mouse_take_moved();

  if (s_app) {
    int16_t lx = (int16_t)(mouse_x() - s_app_rect.x);
    int16_t ly = (int16_t)(mouse_y() - s_app_rect.y);
    if (btn && s_app->click && rect_contains(s_app_rect, (int16_t)mouse_x(),
                                             (int16_t)mouse_y()) &&
        s_app->click(s_app->state, lx, ly, btn)) {
      s_app_dirty = 1;
      flush();
    }
    return;
  }

  if (wheel) { move(wheel > 0 ? -1 : 1); return; }
  if (!btn) return;

  /* The centre opens what is selected; either side steps towards it, which is
   * the same gesture as clicking the neighbour you can already see. */
  if (mouse_x() < BIG_X) move(-1);
  else if (mouse_x() > BIG_X + BIG) move(1);
  else if (icons_count()) launch(s_sel);
}

void launchui_mouse_done(void) { flush(); }
