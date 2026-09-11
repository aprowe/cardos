/* See shell.h. */

#include "kernel/ui/shell.h"
#include "kernel/ui/desktop.h"
#include "kernel/ui/launchui.h"

static UiShell s_shell;

void ui_set_shell(UiShell s) { s_shell = s; }
UiShell ui_shell(void) { return s_shell; }

void ui_repaint(void) {
  if (s_shell == UI_DESKTOP) desktop_repaint();
  else if (s_shell == UI_LAUNCHER) launchui_repaint();
}

void ui_scroll_into_view(int16_t y, int16_t h) {
  if (s_shell == UI_DESKTOP) desktop_scroll_into_view(y, h);
}
