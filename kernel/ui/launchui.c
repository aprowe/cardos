/* The launcher. See launchui.h. */

#include "kernel/ui/launchui.h"
#include "kernel/ui/icons.h"
#include "kernel/ui/draw.h"
#include "kernel/ui/desktop.h"
#include "kernel/app/capprun.h"
#include "kernel/app/capp.h"
#include "kernel/drv/keyboard.h"
#include "kernel/drv/bthid.h"
#include "kernel/net/wifi.h"
#include "kernel/ui/shell.h"

#include <stdio.h>
#include <string.h>

/* Grid geometry. Five columns of 48 across 240, three rows of 38 under a
 * 14-pixel title bar, which is fifteen apps on screen at once -- more than
 * /desktop is ever likely to hold, so scrolling is the exception. */
#define BAR_H   14
#define CELL_W  48
#define CELL_H  38
#define COLS    (DISPLAY_W / CELL_W)
#define BOX     16

static int      s_sel;
static int      s_top_row;       /* first visible row */
static int      s_dirty;
static uint32_t s_now_ms;
static char     s_note[48];

/* The app currently running fullscreen, or NULL for the grid. */
static const AppDef *s_app;
static int            s_app_dirty;

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int rows_visible(void) { return (DISPLAY_H - BAR_H) / CELL_H; }

static Rect cell_rect(int i) {
  int col = i % COLS, row = i / COLS - s_top_row;
  return R(col * CELL_W, BAR_H + row * CELL_H, CELL_W, CELL_H);
}

static void scroll_to_selection(void) {
  int row = s_sel / COLS;
  if (row < s_top_row) s_top_row = row;
  if (row >= s_top_row + rows_visible()) s_top_row = row - rows_visible() + 1;
  if (s_top_row < 0) s_top_row = 0;
}

/* ------------------------------------------------------------- paint ---- */

static void paint_bar(void) {
  char right[26];
  Rect bar = R(0, 0, DISPLAY_W, BAR_H);

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(bar, C_TITLE);
  draw_text(3, 3, "CardOS", C_TITLE_FG, C_TITLE);

  /* Whatever the user most needs to know is one radio away: whether the
   * network is up, and whether the pointer is. */
  snprintf(right, sizeof right, "%s%s %u:%02u",
           wifi_is_connected() ? "wifi " : "",
           bthid_state(BTHID_MOUSE) == BTH_CONNECTED ? "mouse" : "",
           (unsigned)(s_now_ms / 60000u) % 100u,
           (unsigned)((s_now_ms / 1000u) % 60u));
  draw_text_ellipsis((int16_t)(DISPLAY_W - 110), 3, 107, right,
                     C_TITLE_FG, C_TITLE);
}

static void paint_grid(void) {
  int i, n = icons_count();
  int first = s_top_row * COLS;
  int last = first + rows_visible() * COLS;

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, BAR_H, DISPLAY_W, DISPLAY_H - BAR_H), C_DESKTOP);

  for (i = first; i < n && i < last; i++) {
    const Icon *ic = icon_at(i);
    Rect c = cell_rect(i);
    Rect box = R(c.x + (CELL_W - BOX) / 2, c.y + 4, BOX, BOX);
    const uint8_t *bits = icon_bitmap(i);

    if (i == s_sel) {
      draw_rect(rect_inset(c, 1), C_TITLE);
      draw_frame(rect_inset(c, 1), C_TITLE_FG);
    }

    if (ic->kind == ICON_FIRMWARE) draw_bevel(box, C_TITLE_UN, C_SHADOW, C_LIGHT);
    else                           draw_bevel(box, C_FACE, C_LIGHT, C_DARK);

    if (bits)
      draw_bitmap1(box.x, box.y, CAPP_ICON_W, CAPP_ICON_H, bits, C_TEXT, C_FACE);
    else
      draw_text((int16_t)(box.x + 5), (int16_t)(box.y + 4),
                ic->kind == ICON_FIRMWARE ? "F" : "A", C_TEXT,
                ic->kind == ICON_FIRMWARE ? C_TITLE_UN : C_FACE);

    draw_text_ellipsis((int16_t)(c.x + 2), (int16_t)(box.y + BOX + 3),
                       CELL_W - 4, ic->name, C_TITLE_FG,
                       i == s_sel ? C_TITLE : C_DESKTOP);
  }

  if (n == 0)
    draw_text(6, BAR_H + 8, "nothing in /desktop", C_TITLE_FG, C_DESKTOP);

  /* One line of help, and whatever just happened. Always the same place, so
   * it can be read without looking for it. */
  draw_rect(R(0, DISPLAY_H - 9, DISPLAY_W, 9), C_DESKTOP);
  draw_text_ellipsis(3, (int16_t)(DISPLAY_H - 9), DISPLAY_W - 6,
                     s_note[0] ? s_note : "enter opens  ` console  d desktop",
                     C_SHADOW, C_DESKTOP);
}

static void paint_app(void) {
  Rect all = R(0, 0, DISPLAY_W, DISPLAY_H);
  draw_set_clip(all);
  if (s_app->paint) s_app->paint(s_app->state, all);
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
  paint_grid();
}

/* ------------------------------------------------------------ running --- */

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
    snprintf(s_note, sizeof s_note, "booting %s...", ic->name);
    s_dirty = 1;
    flush();
    icons_boot_firmware(i);                /* does not return on success */
    snprintf(s_note, sizeof s_note, "not a bootable image: %.16s", ic->name);
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
  s_app_dirty = 1;
  flush();
}

/* -------------------------------------------------------------- input --- */

void launchui_repaint(void) {
  if (s_app) s_app_dirty = 1;
  else s_dirty = 1;
  flush();
}

void launchui_init(void) {
  ui_set_shell(UI_LAUNCHER);
  icons_reload();
  mouse_init(DISPLAY_W, DISPLAY_H);
  s_sel = 0;
  s_top_row = 0;
  s_app = NULL;
  s_note[0] = 0;
  s_dirty = 1;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_DESKTOP);
  flush();
}

int launchui_key(uint8_t key) {
  int n = icons_count();

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
  case KEY_LEFT:  if (s_sel > 0) s_sel--; break;
  case KEY_RIGHT: if (s_sel + 1 < n) s_sel++; break;
  case KEY_UP:    if (s_sel - COLS >= 0) s_sel -= COLS; break;
  case KEY_DOWN:  if (s_sel + COLS < n) s_sel += COLS; break;
  case KEY_ENTER:
  case ' ':
    if (n) launch(s_sel);
    return 0;
  case 'r': case 'R':
    icons_reload();
    if (s_sel >= icons_count()) s_sel = 0;
    snprintf(s_note, sizeof s_note, "reloaded: %d apps", icons_count());
    break;
  case 'd': case 'D':
    /* The desktop is one keystroke away rather than a mode to configure. */
    desktop_init();
    return 2;
  default:
    return 0;
  }

  scroll_to_selection();
  s_dirty = 1;
  flush();
  return 0;
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

/* The launcher has no pointer of its own: there is nothing to drag, and a
 * cursor would have to be erased by repainting whatever is under it, which
 * during a fullscreen app only the app knows how to do. A click is taken at
 * the position the mouse has reached. */
void launchui_mouse_apply(const MouseReport *r) {
  int btn;
  mouse_apply(r);
  btn = mouse_pressed(MOUSE_LEFT) ? CAPP_BTN_LEFT
      : mouse_pressed(MOUSE_RIGHT) ? CAPP_BTN_RIGHT : 0;
  mouse_released(MOUSE_LEFT);
  mouse_released(MOUSE_RIGHT);
  mouse_take_moved();
  mouse_take_wheel();

  if (!btn) return;

  if (s_app) {
    if (s_app->click &&
        s_app->click(s_app->state, (int16_t)mouse_x(), (int16_t)mouse_y(), btn)) {
      s_app_dirty = 1;
      flush();
    }
    return;
  }

  {
    int i, n = icons_count();
    int first = s_top_row * COLS;
    int last = first + rows_visible() * COLS;
    for (i = first; i < n && i < last; i++) {
      if (!rect_contains(cell_rect(i), (int16_t)mouse_x(), (int16_t)mouse_y()))
        continue;
      /* A single click launches. There is nothing else a click could mean
       * here -- no selection to make, no file to rename -- and a double click
       * on a device with no desk to rest a hand on is a nuisance. */
      s_sel = i;
      launch(i);
      return;
    }
  }
}

void launchui_mouse_done(void) { flush(); }
