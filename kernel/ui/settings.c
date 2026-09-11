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
#include "kernel/drv/keyboard.h"
#include "kernel/drv/display.h"
#include "kernel/app/capprun.h"
#include "kernel/net/wifi.h"

#include "esp_system.h"

#include <stdio.h>
#include <string.h>

#define ROW_H   9
#define VALUE_X 78

typedef enum { VIEW_ROWS = 0, VIEW_SCAN, VIEW_PASS } View;

typedef struct {
  View view;
  int  sel;
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

static void act_pair(SettingsState *st) {
  snprintf(st->note, sizeof st->note, "scanning, keep the mouse awake");
  settings_paint_now();
  if (bthid_start(6, BTHID_MOUSE) == 0)
    snprintf(st->note, sizeof st->note, "paired: %s", bthid_status(BTHID_MOUSE));
  else
    snprintf(st->note, sizeof st->note, "no mouse found");
}

static void act_pair_kbd(SettingsState *st) {
  snprintf(st->note, sizeof st->note, "put the keyboard in pairing mode");
  settings_paint_now();
  if (bthid_start(8, BTHID_KEYBOARD) == 0)
    snprintf(st->note, sizeof st->note, "paired: %s", bthid_status(BTHID_KEYBOARD));
  else
    snprintf(st->note, sizeof st->note, "no keyboard found");
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
  int n;
  snprintf(st->note, sizeof st->note, "reconnecting...");
  settings_paint_now();
  if (!wifi_is_connected()) wifi_connect_saved(15000);
  n = bthid_autoconnect(3);
  snprintf(st->note, sizeof st->note, "%s, %d bluetooth",
           wifi_is_connected() ? "wifi up" : "no wifi", n);
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

static void act_reboot(SettingsState *st) {
  (void)st;
  esp_restart();
}

/* ---- the rows ----------------------------------------------------------- */

typedef struct {
  const char *label;
  void (*value)(char *buf, size_t n);
  void (*action)(SettingsState *st);
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
  { "WiFi",      v_wifi,   act_wifi_scan  },
  { "Network",   v_saved,  act_wifi_saved },
  { "Mouse",     v_mouse,  act_pair       },
  { "Keyboard",  v_kbd,    act_pair_kbd   },
  { "BT at boot", v_btboot, act_bt_boot   },
  { "Reconnect", NULL,     act_reconnect  },
  { "Bluetooth off", NULL, act_radio_off  },
  { "Forget all", NULL,    act_forget     },
  { "RAM",       v_ram,    NULL           },
  { "Restart",   NULL,     act_reboot     },
};

#define NROWS ((int)(sizeof ROWS / sizeof ROWS[0]))

/* ---- painting ----------------------------------------------------------- */

static void band(Rect c, int16_t y, int sel, const char *left, const char *right) {
  uint16_t fg = sel ? C_TITLE_FG : C_TEXT;
  uint16_t bg = sel ? C_TITLE : C_WHITE;
  draw_rect(R(c.x, y, c.w, ROW_H), bg);
  draw_text(c.x, y, left, fg, bg);
  if (right && right[0])
    draw_text_ellipsis((int16_t)(c.x + VALUE_X), y, (int16_t)(c.w - VALUE_X),
                       right, fg, bg);
}

static void paint_rows(SettingsState *st, Rect c) {
  int i;
  for (i = 0; i < NROWS; i++) {
    char val[32];
    val[0] = 0;
    if (ROWS[i].value) ROWS[i].value(val, sizeof val);
    else if (ROWS[i].action) snprintf(val, sizeof val, "%s", "...");
    band(c, (int16_t)(c.y + i * ROW_H), i == st->sel, ROWS[i].label, val);
  }
  draw_text_ellipsis(c.x, (int16_t)(c.y + NROWS * ROW_H + 1), c.w, st->note,
                     C_SHADOW, C_WHITE);
}

static void paint_scan(SettingsState *st, Rect c) {
  int i;
  draw_text(c.x, c.y, "pick a network   ` back", C_SHADOW, C_WHITE);
  for (i = 0; i < st->nap; i++) {
    char rssi[16];
    snprintf(rssi, sizeof rssi, "%d%s", st->aps[i].rssi,
             st->aps[i].open ? " open" : "");
    band(c, (int16_t)(c.y + (i + 1) * ROW_H), i == st->ap_sel,
         st->aps[i].ssid, rssi);
  }
}

static void paint_pass(SettingsState *st, Rect c) {
  char shown[WIFI_PASS_MAX + 2];
  int i;

  draw_text(c.x, c.y, st->aps[st->ap_sel].ssid, C_TEXT, C_WHITE);
  draw_text(c.x, (int16_t)(c.y + ROW_H), "password, then enter", C_SHADOW, C_WHITE);

  /* Shown as dots with the last character in clear: on a keyboard this small,
   * typing a passphrase blind is how you end up believing the password is
   * wrong. */
  for (i = 0; i < st->pass_len; i++)
    shown[i] = (i == st->pass_len - 1) ? st->pass[i] : '*';
  shown[st->pass_len] = '_';
  shown[st->pass_len + 1] = 0;

  draw_bevel(R(c.x, c.y + 2 * ROW_H + 2, c.w, ROW_H + 2), C_WHITE, C_SHADOW, C_LIGHT);
  draw_text_ellipsis((int16_t)(c.x + 2), (int16_t)(c.y + 2 * ROW_H + 4),
                     (int16_t)(c.w - 4), shown, C_TEXT, C_WHITE);
  draw_text_ellipsis(c.x, (int16_t)(c.y + 4 * ROW_H), c.w, st->note,
                     C_SHADOW, C_WHITE);
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
  default:        return (int16_t)(NROWS * ROW_H + 10);
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
    ui_scroll_into_view((int16_t)(st->sel * ROW_H), ROW_H);
    return 1;
  case KEY_DOWN:
    st->sel = (st->sel + 1) % NROWS;
    ui_scroll_into_view((int16_t)(st->sel * ROW_H), ROW_H);
    return 1;
  case KEY_ENTER:
  case ' ':
    if (ROWS[st->sel].action) ROWS[st->sel].action(st);
    return 1;
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
    int i = y / ROW_H;
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
    "Settings", settings_paint, settings_key, settings_click,
    settings_open, &s_state, settings_height, 0, 0, settings_wants_text,
    "arrows\tmove the selection\nenter\trun the selected row\nbackspace\tback out of a list\n", NULL
  };
  return &def;
}
