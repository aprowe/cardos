/* See shell.h. */

#include "kernel/ui/shell.h"
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
}

void ui_scroll_into_view(int16_t y, int16_t h) {
  if (s_shell == UI_DESKTOP) desktop_scroll_into_view(y, h);
}
