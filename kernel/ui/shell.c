/* See shell.h. */

#include "kernel/sys/prefs.h"
#include "kernel/ui/shell.h"
#include "kernel/console/console.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/launchui.h"

#include "nvs.h"
#include "nvs_flash.h"

static UiShell s_shell;

#define NVS_NS    "cardos"
#define NVS_SHELL "shell"

void ui_set_shell(UiShell s) {
  nvs_handle_t h;
  if (s_shell == s) return;
  s_shell = s;

  /* Written on the change rather than at exit: there is no clean shutdown on
   * a device whose usual way of stopping is having the cable pulled out. */
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, NVS_SHELL, (uint8_t)s);
  nvs_commit(h);
  nvs_close(h);
  prefs_mirror();              /* and /config/settings.txt: see prefs.h */
}

UiShell ui_saved_shell(void) {
  nvs_handle_t h;
  uint8_t v = UI_LAUNCHER;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return UI_LAUNCHER;
  if (nvs_get_u8(h, NVS_SHELL, &v) != ESP_OK) v = UI_LAUNCHER;
  nvs_close(h);
  if (v != UI_NONE && v != UI_DESKTOP && v != UI_LAUNCHER) return UI_LAUNCHER;
  return (UiShell)v;
}
UiShell ui_shell(void) { return s_shell; }

void ui_repaint(void) {
  if (s_shell == UI_DESKTOP) desktop_repaint();
  else if (s_shell == UI_LAUNCHER) launchui_repaint();
  else con_repaint();          /* the console owns the screen, and can redraw */
}

void ui_scroll_into_view(int16_t y, int16_t h) {
  if (s_shell == UI_DESKTOP) desktop_scroll_into_view(y, h);
}

/* ---- what a voice command may ask of the shell ---------------------------
 *
 * See shell.h. Nothing here decides anything; it holds the table main.c
 * installs and forwards. The point of the indirection is that voice.c can be
 * read, and tested, without the desktop underneath it. */

static ShellOps s_ops;

void shell_set_ops(const ShellOps *ops) {
  if (ops) s_ops = *ops;
}

int shell_open_app(const char *name) {
  return (s_ops.open_app && name) ? s_ops.open_app(name) : -1;
}

void shell_switch(const char *which) {
  if (s_ops.switch_shell && which) s_ops.switch_shell(which);
}

void shell_feed_key(uint8_t k) {
  if (s_ops.feed_key) s_ops.feed_key(k);
}

const AppDef *shell_running_app(void) {
  return s_ops.running_app ? s_ops.running_app() : NULL;
}
