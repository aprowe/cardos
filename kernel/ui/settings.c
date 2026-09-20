/* The Settings app: the things that used to need a shell command.
 *
 * Pairing a mouse or joining a network had no way in from the desktop at all --
 * you had to leave for the console, type, and come back -- which is the wrong
 * shape for a machine with a pointer and a keyboard.
 *
 * Three views in one app rather than three apps, because they are one task:
 * the row list, the network scan it opens, and the password entry that scan
 * opens. A view owns the whole content area while it is up.
 */

#include "kernel/ui/settings.h"
#include "kernel/ui/draw.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/shell.h"
#include "kernel/drv/bthid.h"
#include "kernel/sys/bg.h"
#include "kernel/drv/keyboard.h"
#include "kernel/drv/display.h"
#include "kernel/drv/speaker.h"
#include "kernel/app/capprun.h"
#include "kernel/net/wifi.h"

#include "esp_system.h"

#include <stdio.h>
#include <string.h>

/* Taller rows than the old nine-pixel bands, and a heading above each group.
 * Eleven settings in one undifferentiated list is a list you read twice to
 * find anything; four short groups is one you scan. */
#define ROW_H   12
#define HEAD_H  11
#define PAD_X   4

#define S_BG     RGB565(22, 24, 30)
#define S_ROW    RGB565(31, 34, 42)
#define S_SEL    RGB565(42, 62, 94)
#define S_ACCENT RGB565(96, 156, 244)
#define S_TEXT   RGB565(224, 230, 240)
#define S_DIM    RGB565(128, 138, 154)
#define S_HEAD   RGB565(142, 162, 200)
#define S_ON     RGB565(112, 208, 140)
#define S_OFF    RGB565(110, 120, 134)
#define S_BAR    RGB565(38, 62, 98)

typedef enum { VIEW_ROWS = 0, VIEW_SCAN, VIEW_PASS } View;

typedef struct {
  View view;
  int  sel;

  /* Its own viewport, for the shells that do not provide one.
   *
   * ui_scroll_into_view only does anything on the desktop, where a window
   * scrolls its content. In the launcher an app is the whole screen and there
   * is nothing outside it to scroll -- so this list, which grew past 135
   * pixels when it gained section headings, simply could not reach its last
   * rows. `top` is the pixel offset, used only when the shell is not going to
   * do it; letting both scroll would double every movement. */
  int  top;
  int  view_h;         /* what the last paint was given */
  char note[44];       /* what just happened, or what is about to */

  WifiAp aps[WIFI_MAX_SCAN];
  int    nap;
  int    ap_sel;

  char pass[WIFI_PASS_MAX];
  int  pass_len;
} SettingsState;

static SettingsState s_state;

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* Anything that blocks for seconds has to be on screen before it starts, or
 * the user stares at an unchanged window wondering whether the click
 * registered. */
void settings_paint_now(void) { ui_repaint(); }

/* ---- actions -------------------------------------------------------------
 *
 * Each one leaves a note saying what happened. Failure is a sentence, not a
 * silence: "wrong password" and "network not found" are different problems
 * and the radio can tell them apart. */

/* Asked for, not done here -- the scan is six seconds and this is the drawing
 * loop. The answer arrives as a note from the background task. */
static void act_pair(SettingsState *st) {
  bg_submit(BG_BT_PAIR_MOUSE);
  snprintf(st->note, sizeof st->note, "scanning, keep the mouse awake");
}

static void act_pair_kbd(SettingsState *st) {
  bg_submit(BG_BT_PAIR_KBD);
  snprintf(st->note, sizeof st->note, "put the keyboard in pairing mode");
}

static void act_radio_off(SettingsState *st) {
  bthid_stop_all();
  snprintf(st->note, sizeof st->note, "bluetooth off");
}

static void act_bt_boot(SettingsState *st) {
  int on = !bthid_autostart();
  bthid_set_autostart(on);
  snprintf(st->note, sizeof st->note,
           on ? "on: costs ~67K of heap from boot" : "off: pair from here instead");
}

/* One button for "get me back to where I was": the saved network and whatever
 * was paired, which between them is everything that drops when the machine is
 * put down for a while. */
static void act_reconnect(SettingsState *st) {
  bg_submit(BG_RECONNECT_ALL);
  snprintf(st->note, sizeof st->note, "reconnecting...");
}

static void act_wifi_scan(SettingsState *st) {
  snprintf(st->note, sizeof st->note, "scanning for networks...");
  settings_paint_now();
  st->nap = wifi_scan(st->aps, WIFI_MAX_SCAN);
  st->ap_sel = 0;
  if (st->nap == 0) {
    snprintf(st->note, sizeof st->note, "no networks in range");
    return;
  }
  st->view = VIEW_SCAN;
  snprintf(st->note, sizeof st->note, "%d networks", st->nap);
}

static void act_wifi_saved(SettingsState *st) {
  if (!wifi_saved_ssid()[0]) {
    snprintf(st->note, sizeof st->note, "nothing saved yet");
    return;
  }
  snprintf(st->note, sizeof st->note, "joining %s...", wifi_saved_ssid());
  settings_paint_now();
  wifi_connect_saved(20000);
  snprintf(st->note, sizeof st->note, "%s", wifi_status());
}

static void act_forget(SettingsState *st) {
  wifi_forget();
  wifi_stop();
  bthid_stop_all();
  snprintf(st->note, sizeof st->note, "network forgotten, radios off");
}

/* Four levels, not a slider: on a 240-pixel screen the difference between
 * 60 and 70 percent is not one anyone will pick, and a row that cycles on
 * enter is how every other setting here works. Left and right step it too,
 * because a level is the one thing in this list that has a direction. */
#define BRIGHT_STEP 25

static void bright_step(SettingsState *st, int dir) {
  int pct = display_brightness() + dir * BRIGHT_STEP;
  if (pct > 100) pct = DISPLAY_BRIGHT_MIN;
  if (pct < DISPLAY_BRIGHT_MIN) pct = 100;
  display_set_brightness(pct);
  snprintf(st->note, sizeof st->note, "%d%%, saved", display_brightness());
}

static void act_bright(SettingsState *st) { bright_step(st, +1); }

#define VOLUME_STEP 10

static void volume_step(SettingsState *st, int dir) {
  int pct = speaker_volume() + dir * VOLUME_STEP;
  if (pct > 100) pct = 0;
  if (pct < 0) pct = 100;
  speaker_set_volume(pct);
  snprintf(st->note, sizeof st->note, "%d%%, saved", speaker_volume());
}

static void act_volume(SettingsState *st) { volume_step(st, +1); }

static void act_reboot(SettingsState *st) {
  (void)st;
  esp_restart();
}

/* ---- the rows ----------------------------------------------------------- */

typedef struct {
  const char *section;     /* a heading above this row, or NULL to continue */
  const char *label;
  void (*value)(char *buf, size_t n);
  void (*action)(SettingsState *st);
  int  toggle;             /* the value is on/off and reads as a pill */
} Row;

static void v_mouse(char *b, size_t n) { snprintf(b, n, "%s", bthid_status(BTHID_MOUSE)); }
static void v_kbd(char *b, size_t n)   { snprintf(b, n, "%s", bthid_status(BTHID_KEYBOARD)); }
static void v_wifi(char *b, size_t n)  { snprintf(b, n, "%s", wifi_status()); }
static void v_saved(char *b, size_t n) {
  const char *ssid = wifi_saved_ssid();
  snprintf(b, n, "%s", ssid[0] ? ssid : "none");
}
static void v_btboot(char *b, size_t n) {
  snprintf(b, n, "%s", bthid_autostart() ? "on" : "off");
}
static void v_bright(char *b, size_t n) { snprintf(b, n, "%d%%", display_brightness()); }
static void v_volume(char *b, size_t n) {
  if (!speaker_volume()) snprintf(b, n, "%s", "muted");
  else snprintf(b, n, "%d%%", speaker_volume());
}

/* One line rather than four. Free heap is the number that decides whether the
 * next radio will start; the rest is what the mem command is for. */
static void v_ram(char *b, size_t n) {
  snprintf(b, n, "%uK free, %uK app",
           (unsigned)(esp_get_free_heap_size() / 1024),
           (unsigned)(capprun_exec_free() / 1024));
}

/* The radios first, because they are the only settings that change day to
 * day. Rotation and the keyboard-driven pointer were here and are gone: the
 * orientation was baked in once it was right, and a real mouse made the other
 * one a debugging aid. Both survive as console commands -- flip, and ctrl-P on
 * the desktop. */
static const Row ROWS[] = {
  { "Network",   "WiFi",         v_wifi,   act_wifi_scan,  0 },
  { NULL,        "Saved",        v_saved,  act_wifi_saved, 0 },

  { "Bluetooth", "Mouse",        v_mouse,  act_pair,       0 },
  { NULL,        "Keyboard",     v_kbd,    act_pair_kbd,   0 },
  { NULL,        "On at boot",   v_btboot, act_bt_boot,    1 },
  { NULL,        "Reconnect",    NULL,     act_reconnect,  0 },
  { NULL,        "Radio off",    NULL,     act_radio_off,  0 },

  { "Display",   "Brightness",   v_bright, act_bright,     0 },

  { "Sound",     "Volume",       v_volume, act_volume,     0 },

  { "System",    "Memory",       v_ram,    NULL,           0 },
  { NULL,        "Forget all",   NULL,     act_forget,     0 },
  { NULL,        "Restart",      NULL,     act_reboot,     0 },
};

#define NROWS ((int)(sizeof ROWS / sizeof ROWS[0]))

/* ---- layout --------------------------------------------------------------
 *
 * Headings make a row's position no longer i * ROW_H, and three places need
 * the answer -- the paint, the click and the scroll. One function, so they
 * cannot disagree. */
static int16_t row_y(int i) {
  int16_t y = 0;
  int k;
  for (k = 0; k < i && k < NROWS; k++) {
    if (ROWS[k].section) y = (int16_t)(y + HEAD_H);
    y = (int16_t)(y + ROW_H);
  }
  if (i < NROWS && ROWS[i].section) y = (int16_t)(y + HEAD_H);
  return y;
}

static int16_t rows_height(void) {
  return (int16_t)(row_y(NROWS - 1) + ROW_H);
}

/* The row a y offset lands on, or the nearest one. */
static int row_at(int16_t y) {
  int i;
  for (i = 0; i < NROWS; i++)
    if (y >= row_y(i) && y < row_y(i) + ROW_H) return i;
  return -1;
}

/* ---- painting ----------------------------------------------------------- */

/* A row: label on the left, value on the right, and an accent bar down the
 * edge of the selected one rather than a solid block of colour across it --
 * the block is what made the old list look like a spreadsheet. */
static void band(Rect c, int16_t y, int sel, const char *left, const char *right,
                 int toggle) {
  uint16_t bg = sel ? S_SEL : S_ROW;
  int16_t w;

  draw_rect(R(c.x, y, c.w, ROW_H - 1), bg);
  if (sel) draw_rect(R(c.x, y, 2, ROW_H - 1), S_ACCENT);
  draw_text((int16_t)(c.x + PAD_X + 2), (int16_t)(y + 2), left, S_TEXT, bg);

  if (!right || !right[0]) return;
  w = (int16_t)(draw_text_width(right) + 4);
  if (w > c.w / 2) w = (int16_t)(c.w / 2);
  if (toggle) {
    /* On and off are worth seeing without reading, so they get a colour. */
    int on = (right[0] == 'o' && right[1] == 'n');
    draw_rect(R(c.x + c.w - w - PAD_X, y + 2, w, ROW_H - 5), on ? S_ON : S_OFF);
    draw_text((int16_t)(c.x + c.w - w - PAD_X + 2), (int16_t)(y + 2), right,
              S_ROW, on ? S_ON : S_OFF);
  } else {
    draw_text_ellipsis((int16_t)(c.x + c.w - w - PAD_X), (int16_t)(y + 2), w,
                       right, S_DIM, bg);
  }
}

/* Does this app have to scroll itself? Only where the shell will not. */
static int scrolls_itself(void) { return ui_shell() != UI_DESKTOP; }

static void clamp_top(SettingsState *st) {
  int16_t max = (int16_t)(rows_height() + ROW_H + 6 - st->view_h);
  if (!scrolls_itself() || max < 0) max = 0;
  if (st->top > max) st->top = max;
  if (st->top < 0) st->top = 0;
}

/* Keep the selected row on screen, whichever mechanism is doing the work. */
static void show_sel(SettingsState *st) {
  if (!scrolls_itself()) { ui_scroll_into_view(row_y(st->sel), ROW_H); return; }
  if (row_y(st->sel) < st->top) st->top = row_y(st->sel);
  if (row_y(st->sel) + ROW_H > st->top + st->view_h)
    st->top = row_y(st->sel) + ROW_H - st->view_h;
  clamp_top(st);
}

static void paint_rows(SettingsState *st, Rect c) {
  int i;
  int16_t off;

  st->view_h = c.h;
  clamp_top(st);
  off = (int16_t)(scrolls_itself() ? st->top : 0);

  draw_rect(R(c.x, c.y, c.w, c.h), S_BG);

  for (i = 0; i < NROWS; i++) {
    char val[32];
    int16_t y = (int16_t)(c.y + row_y(i) - off);

    if (y + ROW_H < c.y || y > c.y + c.h) continue;   /* off the viewport */

    if (ROWS[i].section)
      draw_text((int16_t)(c.x + PAD_X), (int16_t)(y - HEAD_H + 2),
                ROWS[i].section, S_HEAD, S_BG);

    val[0] = 0;
    if (ROWS[i].value) ROWS[i].value(val, sizeof val);
    else if (ROWS[i].action) snprintf(val, sizeof val, "%s", ">");
    band(c, y, i == st->sel, ROWS[i].label, val, ROWS[i].toggle);
  }

  if (st->note[0]) {
    int16_t y = (int16_t)(c.y + rows_height() + 3 - off);
    draw_rect(R(c.x, y, c.w, ROW_H - 1), S_BAR);
    draw_text_ellipsis((int16_t)(c.x + PAD_X), (int16_t)(y + 2), c.w, st->note,
                       S_TEXT, S_BAR);
  }
}

static void paint_scan(SettingsState *st, Rect c) {
  int i;
  draw_rect(R(c.x, c.y, c.w, c.h), S_BG);
  draw_text((int16_t)(c.x + PAD_X), (int16_t)(c.y + 2),
            "pick a network    ` back", S_HEAD, S_BG);
  for (i = 0; i < st->nap; i++) {
    char rssi[16];
    snprintf(rssi, sizeof rssi, "%d%s", st->aps[i].rssi,
             st->aps[i].open ? " open" : "");
    band(c, (int16_t)(c.y + (i + 1) * ROW_H), i == st->ap_sel,
         st->aps[i].ssid, rssi, 0);
  }
}

static void paint_pass(SettingsState *st, Rect c) {
  char shown[WIFI_PASS_MAX + 2];
  int i;

  draw_rect(R(c.x, c.y, c.w, c.h), S_BG);
  draw_text((int16_t)(c.x + PAD_X), (int16_t)(c.y + 2),
            st->aps[st->ap_sel].ssid, S_TEXT, S_BG);
  draw_text((int16_t)(c.x + PAD_X), (int16_t)(c.y + ROW_H + 2),
            "password, then enter", S_DIM, S_BG);

  /* Shown as dots with the last character in clear: on a keyboard this small,
   * typing a passphrase blind is how you end up believing the password is
   * wrong. */
  for (i = 0; i < st->pass_len; i++)
    shown[i] = (i == st->pass_len - 1) ? st->pass[i] : '*';
  shown[st->pass_len] = '_';
  shown[st->pass_len + 1] = 0;

  draw_rect(R(c.x + PAD_X, c.y + 2 * ROW_H + 2, c.w - PAD_X * 2, ROW_H + 2), S_ROW);
  draw_rect(R(c.x + PAD_X, c.y + 2 * ROW_H + 2, 2, ROW_H + 2), S_ACCENT);
  draw_text_ellipsis((int16_t)(c.x + PAD_X + 5), (int16_t)(c.y + 2 * ROW_H + 5),
                     (int16_t)(c.w - PAD_X * 2 - 6), shown, S_TEXT, S_ROW);
  draw_text_ellipsis((int16_t)(c.x + PAD_X), (int16_t)(c.y + 4 * ROW_H), c.w,
                     st->note, S_DIM, S_BG);
}

static void settings_paint(void *state, Rect c) {
  SettingsState *st = (SettingsState *)state;
  draw_rect(c, C_WHITE);
  switch (st->view) {
  case VIEW_SCAN: paint_scan(st, c); break;
  case VIEW_PASS: paint_pass(st, c); break;
  default:        paint_rows(st, c); break;
  }
}

/* The list is as tall as it needs to be and the window scrolls it, rather than
 * this app keeping its own viewport. One scrolling mechanism shared by every
 * app is worth more than a slightly cleverer one here. */
static int16_t settings_height(void *state, int16_t width) {
  SettingsState *st = (SettingsState *)state;
  (void)width;
  switch (st->view) {
  case VIEW_SCAN: return (int16_t)((st->nap + 1) * ROW_H + 2);
  case VIEW_PASS: return (int16_t)(5 * ROW_H);
  default:        return (int16_t)(rows_height() + ROW_H + 6);
  }
}

/* ---- input -------------------------------------------------------------- */

static void join(SettingsState *st) {
  const char *ssid = st->aps[st->ap_sel].ssid;
  snprintf(st->note, sizeof st->note, "joining %s...", ssid);
  st->view = VIEW_ROWS;
  settings_paint_now();
  wifi_connect(ssid, st->pass, 20000);
  snprintf(st->note, sizeof st->note, "%s", wifi_status());
}

static int key_rows(SettingsState *st, uint8_t k) {
  switch (k) {
  case KEY_UP:
    st->sel = (st->sel + NROWS - 1) % NROWS;
    show_sel(st);
    return 1;
  case KEY_DOWN:
    st->sel = (st->sel + 1) % NROWS;
    show_sel(st);
    return 1;
  case KEY_ENTER:
  case ' ':
    if (ROWS[st->sel].action) ROWS[st->sel].action(st);
    return 1;
  case KEY_LEFT:
  case KEY_RIGHT:
    if (ROWS[st->sel].action == act_bright) { bright_step(st, k == KEY_RIGHT ? +1 : -1); return 1; }
    if (ROWS[st->sel].action == act_volume) { volume_step(st, k == KEY_RIGHT ? +1 : -1); return 1; }
    return 0;
  default: return 0;
  }
}

static int key_scan(SettingsState *st, uint8_t k) {
  switch (k) {
  case KEY_UP:
    if (st->ap_sel > 0) st->ap_sel--;
    ui_scroll_into_view((int16_t)((st->ap_sel + 1) * ROW_H), ROW_H);
    return 1;
  case KEY_DOWN:
    if (st->ap_sel + 1 < st->nap) st->ap_sel++;
    ui_scroll_into_view((int16_t)((st->ap_sel + 1) * ROW_H), ROW_H);
    return 1;
  case KEY_ENTER:
  case ' ':
    st->pass_len = 0;
    st->pass[0] = 0;
    if (st->aps[st->ap_sel].open) { join(st); return 1; }
    st->view = VIEW_PASS;
    snprintf(st->note, sizeof st->note, "` cancels");
    return 1;
  default:
    /* Escape belongs to the desktop, so the view's own way back is a key it
     * would otherwise ignore. */
    if (k == KEY_BACKSPACE) { st->view = VIEW_ROWS; return 1; }
    return 0;
  }
}

static int key_pass(SettingsState *st, uint8_t k) {
  if (k == KEY_ENTER) { join(st); return 1; }
  if (k == KEY_BACKSPACE) {
    if (st->pass_len > 0) st->pass[--st->pass_len] = 0;
    else st->view = VIEW_SCAN;
    return 1;
  }
  if (k >= 0x20 && k < 0x7F && st->pass_len < (int)sizeof st->pass - 1) {
    st->pass[st->pass_len++] = (char)k;
    st->pass[st->pass_len] = 0;
    return 1;
  }
  return 0;
}

static int settings_key(void *state, uint8_t k) {
  SettingsState *st = (SettingsState *)state;
  switch (st->view) {
  case VIEW_SCAN: return key_scan(st, k);
  case VIEW_PASS: return key_pass(st, k);
  default:        return key_rows(st, k);
  }
}

/* Only the password field. Everywhere else this app is a list, and a list
 * wants ; . , / to move the selection. */
static int settings_wants_text(void *state) {
  return ((SettingsState *)state)->view == VIEW_PASS;
}

/* The wheel, where this app is doing its own scrolling. On the desktop the
 * window owns the wheel and this declines it, which is what returning 0
 * means there. */
static int settings_mouse(void *state, int16_t x, int16_t y, int buttons,
                          int wheel) {
  SettingsState *st = (SettingsState *)state;
  (void)x; (void)y; (void)buttons;
  if (!wheel || !scrolls_itself() || st->view != VIEW_ROWS) return 0;
  st->top -= wheel * ROW_H * 2;
  clamp_top(st);
  return 1;
}

static int settings_click(void *state, int16_t x, int16_t y, int button) {
  SettingsState *st = (SettingsState *)state;
  (void)x; (void)button;
  if (y < 0) return 0;

  if (st->view == VIEW_SCAN) {
    int i = y / ROW_H - 1;
    if (i < 0 || i >= st->nap) return 0;
    if (i == st->ap_sel) return key_scan(st, KEY_ENTER);
    st->ap_sel = i;
    return 1;
  }
  if (st->view == VIEW_PASS) return 0;

  {
    int i = row_at((int16_t)(y + (scrolls_itself() ? st->top : 0)));
    if (i < 0) return 0;
    if (i >= NROWS) return 0;
    /* A click selects; a click on the already-selected row runs it. Two
     * meanings for one gesture, but it is how a list like this gets used with
     * a pointer. */
    if (i == st->sel && ROWS[i].action) ROWS[i].action(st);
    else st->sel = i;
    return 1;
  }
}

static void settings_open(void *state) {
  SettingsState *st = (SettingsState *)state;
  st->view = VIEW_ROWS;
  st->sel = 0;
  st->pass_len = 0;
  st->pass[0] = 0;
  snprintf(st->note, sizeof st->note, "enter runs the selected row");
}

const AppDef *settings_app(void) {
  static const AppDef def = {
    .name = "Settings", .paint = settings_paint, .key = settings_key,
    .click = settings_click, .open = settings_open, .state = &s_state,
    .mouse = settings_mouse,
    .height = settings_height, .wants_text = settings_wants_text,
    .help = "arrows\tmove the selection\nenter\trun the selected row\n"
            "left/right\tstep the brightness or volume\n"
            "backspace\tback out of a list\n"
  };
  return &def;
}
