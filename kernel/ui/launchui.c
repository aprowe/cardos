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
#include "kernel/sys/bg.h"
#include "kernel/net/httpq.h"
#include "kernel/app/capp.h"
#include "kernel/drv/keyboard.h"
#include "kernel/drv/bthid.h"
#include "kernel/net/wifi.h"
#include "kernel/ui/help.h"
#include "kernel/ui/picker.h"
#include "kernel/ui/icons_builtin.h"
#include "kernel/sys/hotkeys.h"
#include "kernel/sys/clock.h"
#include "kernel/drv/battery.h"

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
static int      s_folder = -1;    /* flat index of the open folder, or -1 for the top */
static int      s_dirty;
static uint32_t s_now_ms;
static char     s_note[48];

/* The app currently running fullscreen, or NULL for the carousel. */
static const AppDef *s_app;
static int            s_app_dirty;
static int            s_app_clear;    /* the screen still has the carousel on it */
static int            s_help;         /* the key list is over everything */
static int            s_binding;      /* k was pressed: the next letter binds */
static int            s_pointer_on;   /* a mouse has moved: there is a cursor */

/* Defined below, beside the rest of the running-app code. */
static void paint_spinner(uint32_t ms);
static Rect           s_app_rect;

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* Wrapping, because a carousel that stops at the ends is a list. */
static int wrap(int i) {
  int n = icons_in_count(s_folder);
  if (n <= 0) return 0;
  return ((i % n) + n) % n;
}

/* The entry at carousel position i, at the current level. */
static const Icon *at(int i) { return icons_in_at(s_folder, i); }

static const char *kind_word(const Icon *ic) {
  if (!ic) return "";
  switch (ic->kind) {
  case ICON_FIRMWARE: return "firmware - replaces CardOS";
  case ICON_CAPP:     return "app";
  case ICON_FOLDER:   return "folder";
  default:            return "built in";
  }
}

/* ------------------------------------------------------------- paint ---- */

/* The status strip. Icons rather than words: "wifi mouse 3:41" spends most of
 * a 240-pixel bar saying things that a glyph says in eight. A radio that is
 * off is drawn dim rather than hidden, so the strip does not reflow and the
 * eye learns where to look. */
static void paint_bar(void) {
  char clock[8];
  int16_t x = DISPLAY_W - 4;

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, BAR_H), C_TITLE);
  draw_text(4, 2, "CardOS", C_TITLE_FG, C_TITLE);

  /* The time, if the device has been told it. Before this it showed
   * uptime in minutes and seconds, formatted as a clock -- right for the
   * first hour after a reboot and wrong ever after. "--:--" is the honest
   * version of not knowing. */
  clock_hm(clock, sizeof clock);
  x = (int16_t)(x - draw_text_width(clock));
  draw_text(x, 2, clock, C_TITLE_FG, C_TITLE);

  /* The battery, as a little cell: an outline, a nub, and a bar inside it.
   * A percentage would cost eighteen pixels of a bar that has three radios
   * to fit as well, and the eye reads a bar faster anyway. */
  {
    int pct = battery_percent();
    if (pct >= 0) {
      int fill = (pct * 10) / 100;
      x = (int16_t)(x - 18);
      draw_frame(R(x, 3, 13, 7), C_TITLE_FG);
      draw_rect(R(x + 13, 5, 1, 3), C_TITLE_FG);           /* the nub */
      if (fill > 0)
        draw_rect(R(x + 2, 5, fill, 3),
                  pct <= 15 ? C_RED : C_TITLE_FG);        /* red when low */
    }
  }

  x = (int16_t)(x - 12);
  draw_bitmap1(x, 2, 8, 8, ICON8_KBD,
               bthid_state(BTHID_KEYBOARD) == BTH_CONNECTED ? C_TITLE_FG : C_SHADOW,
               C_TITLE);
  x = (int16_t)(x - 11);
  draw_bitmap1(x, 2, 8, 8, ICON8_MOUSE,
               bthid_state(BTHID_MOUSE) == BTH_CONNECTED ? C_TITLE_FG : C_SHADOW,
               C_TITLE);
  x = (int16_t)(x - 11);
  draw_bitmap1(x, 2, 8, 8, ICON8_WIFI,
               wifi_is_connected() ? C_TITLE_FG : C_SHADOW, C_TITLE);
}

/* Colour if the card has one, the app's own 1bpp shape otherwise. The colour
 * version is the one somebody drew on purpose, so it wins; the shape is the
 * fallback that always exists because it is compiled into the program. */
static void paint_icon(int idx, int16_t x, int16_t y, int scale, uint16_t fg) {
  const uint16_t *px = icon_colour(idx);
  const uint8_t *bits;

  if (px) {
    draw_image_scaled(x, y, CAPP_ICON_W, CAPP_ICON_H, px, scale, 0x0000);
    return;
  }
  bits = icon_bitmap(idx);
  if (bits) draw_bitmap1_scaled(x, y, CAPP_ICON_W, CAPP_ICON_H, bits, scale,
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
  int n = icons_in_count(s_folder);
  const Icon *ic;

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, BAR_H, DISPLAY_W, DISPLAY_H - BAR_H), C_DESKTOP);

  if (n == 0) {
    if (s_folder >= 0) {
      draw_text_scaled(28, 46, "nothing here", 2, C_TITLE_FG, C_DESKTOP);
      draw_text(28, 74, "escape goes back up", C_DESK_DIM, C_DESKTOP);
      return;
    }
    draw_text_scaled(28, 46, "no apps", 2, C_TITLE_FG, C_DESKTOP);
    draw_text(28, 74, "put .capp or .bin files", C_DESK_DIM, C_DESKTOP);
    draw_text(28, 86, "in /apps, then press r", C_DESK_DIM, C_DESKTOP);
    return;
  }

  /* Neighbours are dimmed as well as smaller: at 32x32 a full-contrast icon
   * still competes with the one in focus. */
  if (n > 1) {
    int16_t sy = (int16_t)(ICON_TOP + (BIG - SMALL) / 2);
    paint_icon(icon_index(at(wrap(s_sel - 1))), (int16_t)(BIG_X - SIDE_GAP - SMALL), sy, 2, C_SHADOW);
    paint_icon(icon_index(at(wrap(s_sel + 1))), (int16_t)(BIG_X + BIG + SIDE_GAP), sy, 2, C_SHADOW);
  }
  paint_icon(icon_index(at(s_sel)), BIG_X, ICON_TOP, 4, C_TITLE_FG);

  ic = at(s_sel);
  if (ic) {
    /* Centred on the glyph width rather than a guess, so a long name and a
     * short one both sit under the icon. */
    int16_t w = (int16_t)(draw_text_width(ic->name) * 2);
    int16_t nx = (int16_t)((DISPLAY_W - w) / 2);
    char where[48];
    const char *k;
    int16_t kx;

    /* Inside a folder the level is always on screen: "Games / app" rather
     * than a bare "app" that looks the same at the top. */
    if (s_note[0]) k = s_note;
    else if (s_folder >= 0) {
      const Icon *f = icon_at(s_folder);
      snprintf(where, sizeof where, "%s / %s", f ? f->name : "?", kind_word(ic));
      k = where;
    } else k = kind_word(ic);
    kx = (int16_t)((DISPLAY_W - draw_text_width(k)) / 2);

    if (nx < 2) nx = 2;
    if (kx < 2) kx = 2;
    draw_text_scaled(nx, NAME_Y, ic->name, 2, C_TITLE_FG, C_DESKTOP);
    draw_text_ellipsis(kx, KIND_Y, (int16_t)(DISPLAY_W - 4), k,
                       C_DESK_DIM, C_DESKTOP);
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
  /* Read before the clearing below resets it: a frame that clears the screen
   * has to be a whole repaint, whatever the app thinks changed. */
  int cleared = s_app_clear;

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
   * cannot scribble over the surround it does not own -- and narrowed further
   * to whatever the app says actually changed.
   *
   * This is what stops a keypress redrawing a whole screen. Until an app
   * marks damage it gets its full rectangle, exactly as before, so nothing
   * written before this notices; an app that marks a row gets a row, and the
   * drawing it does outside that row costs a clip test each rather than a
   * blit. A full clear is still available to the shell -- s_app_clear -- for
   * the cases where the app genuinely cannot know what is underneath. */
  {
    Rect area = s_app_rect;
    Rect want;
    if (!cleared && s_app->take_damage &&
        s_app->take_damage(s_app->state, &want)) {
      Rect vis = rect_intersect(want, s_app_rect);
      if (!rect_is_empty(vis)) area = vis;
    }
    draw_set_clip(area);
  }
  if (s_app->paint) s_app->paint(s_app->state, s_app_rect);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}

/* The shell's own keys, appended under the app's. Two lists rather than one so
 * an app cannot accidentally claim a key the shell owns. */
static const char *shell_keys(void) {
  static char buf[512];
  char c;
  size_t n;

  if (s_app) return "escape\tback a level, inside the app\nfn-b\tmenu bar, by keyboard\n"
                    "fn-`\tleave the app (or opt-backspace)\nfn-h\tclose this\n";

  /* The bindings are listed here rather than on a screen of their own: the
   * key list is where someone looks to find out what opt-p does. */
  snprintf(buf, sizeof buf,
           "arrows\tmove along the row\nenter\topen\nk\tbind opt-letter to this app\n"
           "r\treload the app list\nd\tswitch to the desktop\n"
           "escape\tout of a folder\nfn-`\tout of a folder, or the console (or opt-backspace)\nfn-h\tclose this\n");
  n = strlen(buf);
  for (c = 'a'; c <= 'z' && n < sizeof buf - 40; c++) {
    const char *name = hotkey_get(c);
    if (!name) continue;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "opt-%c\t%s\n", c, name);
  }
  return buf;
}

static void flush(void) {
  if (s_help) {
    const Icon *ic = icon_at(s_sel);
    if (picker_active())
      help_paint("Files", picker_help(), "fn-`\tcancel and leave the app\nfn-h\tclose this\n");
    else
      help_paint(s_app ? s_app->name : (ic ? ic->name : "CardOS"),
                 s_app ? s_app->help : NULL, shell_keys());
    return;
  }
  /* The picker over an app: it owns the screen until it answers, and the
   * app underneath is repainted only once it has gone. */
  if (picker_active()) {
    if (s_app_dirty) { s_app_dirty = 0; picker_paint_now(); }
    else picker_paint();
    if (s_pointer_on) {
      draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
      draw_cursor((int16_t)mouse_x(), (int16_t)mouse_y());
    }
    return;
  }
  if (s_app) {
    if (!s_app_dirty) return;
    s_app_dirty = 0;
    paint_app();
    /* Last, and every time: an app that animates repaints over the pointer
     * otherwise, and a cursor that blinks out whenever the ball moves is
     * worse than no cursor at all. */
    if (s_pointer_on) {
      draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
      draw_cursor((int16_t)mouse_x(), (int16_t)mouse_y());
    }
    return;
  }
  if (!s_dirty) return;
  s_dirty = 0;
  paint_bar();
  paint_carousel();
  if (httpq_active()) paint_spinner(s_now_ms);
}

/* A request is in flight somewhere.
 *
 * Drawn by the shell rather than by kernel/sys/busy.c's task, and the
 * difference is the whole point of httpq: that badge exists because the shell
 * is BLOCKED and cannot draw, and it is only safe because nothing else is
 * drawing either. Here the shell is alive -- which is what the async path
 * bought -- so it paints its own, and there is only ever one writer to the
 * panel.
 *
 * Three dots in the top-right corner, away from the clock on the left. Small
 * enough that the area can be repainted from the shell's own background when
 * it finishes, without a full redraw. */
#define SPIN_W  20
#define SPIN_H  7
#define SPIN_X  (DISPLAY_W - SPIN_W - 2)
#define SPIN_Y  1

static int s_spin_on;

static void paint_spinner(uint32_t ms) {
  int i, phase = (int)(ms / 140u) % 3;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  for (i = 0; i < 3; i++)
    draw_rect(R(SPIN_X + i * 7, SPIN_Y + 1, 4, 4),
              i == phase ? RGB565(120, 180, 250) : RGB565(56, 64, 80));
}

/* ------------------------------------------------------------ running --- */

/* The app the launcher is showing, or NULL. For anything that wants to drive
 * it without a finger -- see capprun_actions. */
const AppDef *launchui_running(void) { return s_app; }

void launchui_repaint(void) {
  if (s_app) s_app_dirty = 1;
  else s_dirty = 1;
  flush();
}

static void leave_app(void) {
  picker_close();                 /* a picker the app was waiting on goes with it */
  /* Hand the image back: an app the launcher is no longer showing is not
   * going to be called into, and its code is 4 KB of a small pool. Starting
   * it again reloads it, which costs a card read nobody notices. */
  capprun_release(s_app);
  s_app = NULL;
  s_note[0] = 0;
  s_dirty = 1;
  flush();
}

static void open_folder(int flat) {
  s_folder = flat;
  s_sel = 0;
  s_note[0] = 0;
  s_dirty = 1;
  flush();
}

/* Point the carousel at a flat entry: its folder becomes the level and its
 * position within it the selection, so Escape from an app lands on it. */
static void select_flat(int flat) {
  const Icon *ic = icon_at(flat);
  int i, n;
  s_folder = ic ? ic->parent : -1;
  s_sel = 0;
  n = icons_in_count(s_folder);
  for (i = 0; i < n; i++) if (icons_in_at(s_folder, i) == ic) { s_sel = i; break; }
}

/* Up a level, landing on the folder just left rather than on the first icon. */
static void close_folder(void) {
  select_flat(s_folder);
  s_note[0] = 0;
  s_dirty = 1;
  flush();
}

/* `i` is a flat index: what every icons.c call takes. */
static void launch_with(int i, const char *args) {
  const Icon *ic = icon_at(i);
  const AppDef *a;

  if (!ic) return;

  if (ic->kind == ICON_FOLDER) { open_folder(i); return; }

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

  if (ic->kind == ICON_CAPP) {
    /* Running the program *is* opening it: capp_main constructs whatever state
     * it has and installs an interface if it wants one. A program that
     * installs nothing was a command, and has already finished. */
    capprun_start(ic->slot, ic->name, args);
    if (!capprun_is_app(ic->slot)) return;
    a = capprun_def(ic->slot);
    if (!a) return;
  } else {
    a = icon_app(i);
    if (!a) return;
    if (a->open) a->open(a->state);
    if (args && *args && a->set_args) a->set_args(a->state, args);
  }

  /* Everything runs fullscreen here, whatever size it asked for: there is no
   * desktop behind it for a window to sit on. */
  s_app = a;
  s_app_rect = app_rect(a);
  s_app_clear = 1;
  s_app_dirty = 1;
  flush();
}

/* `pos` is a carousel position at the current level. */
static void launch(int pos) { launch_with(icon_index(at(pos)), NULL); }

static void move(int delta) {
  if (icons_in_count(s_folder) == 0) return;
  s_sel = wrap(s_sel + delta);
  s_note[0] = 0;
  s_dirty = 1;
  flush();
}

/* -------------------------------------------------------------- input --- */

/* Take the screen. Separate from launchui_init because running a *command*
 * must not do this: a command prints and returns, and the console it was typed
 * at should still be there afterwards. Conflating the two meant the line after
 * "cat foo" was typed into the carousel. */
static void enter(void) {
  ui_set_shell(UI_LAUNCHER);
  mouse_init(DISPLAY_W, DISPLAY_H);
  s_note[0] = 0;
  s_binding = 0;
  s_dirty = 1;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_DESKTOP);
}

/* The icon list, loaded if it is not already. Not reloaded on every run: a
 * reload unloads every program, and one of them may be the one about to
 * run. */
static void need_icons(void) {
  if (icons_total() == 0) icons_reload();
}

void launchui_init(void) {
  capprun_release(s_app);      /* from the last visit, if any */
  s_app = NULL;
  icons_reload();
  s_sel = 0;
  s_folder = -1;
  enter();
  flush();
}

/* Put an app on the screen. The launcher runs everything fullscreen: there is
 * no desktop behind it for a window to sit on. */
static void host(const AppDef *a) {
  /* Whatever was on screen is not any more, and its image is not needed:
   * Files handing over to Edit used to leave Files resident, in a pool that
   * holds about a dozen apps, until the next reload. capprun defers the
   * release if the old app is the one asking (it is, when run() is called
   * from a handler), so this is safe from inside a callback. */
  if (s_app && s_app != a) capprun_release(s_app);
  s_app = a;
  s_app_rect = app_rect(a);
  s_app_clear = 1;
  s_app_dirty = 1;
  flush();
}

/* Case-insensitive, because a console user types what they remember seeing and
 * the launcher shows "Mines" while the file is mines.capp. */
static int same_name(const char *a, const char *b) {
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
    if (ca != cb) return 0;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

int launchui_run(const char *name, const char *args) {
  int i;

  if (!name || !*name) return -1;
  need_icons();

  /* Commands included: "grep" has no icon but is still something to run. */
  for (i = 0; i < icons_total(); i++) {
    const Icon *ic = icon_at(i);
    if (!ic || !same_name(ic->name, name)) continue;
    /* A command runs and returns, and the console keeps the screen. Anything
     * else is an app, and the launcher takes over to host it. */
    if (ic->kind == ICON_CAPP) {
      capprun_start(ic->slot, ic->name, args);
      if (!capprun_is_app(ic->slot)) return 0;
      select_flat(i);
      enter();
      host(capprun_def(ic->slot));
      return 0;
    }
    select_flat(i);
    enter();
    launch_with(i, args);
    return 0;
  }
  return -1;
}

/* An app not on the carousel: loaded on demand, into a slot of its own. An
 * icon already pointing at this file is reused rather than loaded twice --
 * executable RAM is not a thing to spend on a second copy of a program that is
 * already resident. */
int launchui_run_path(const char *path, const char *args) {
  const AppDef *a;
  int i, slot;

  if (!path || !*path) return -1;
  need_icons();

  for (i = 0; i < icons_total(); i++) {
    const Icon *ic = icon_at(i);
    if (ic && ic->kind == ICON_CAPP && strcmp(ic->path, path) == 0) {
      capprun_start(ic->slot, ic->name, args);
      if (!capprun_is_app(ic->slot)) return 0;   /* a command, already done */
      select_flat(i);
      enter();
      host(capprun_def(ic->slot));
      return 0;
    }
  }

  slot = capprun_load(path);
  if (slot < 0) return -1;
  capprun_start(slot, path, args);
  if (!capprun_is_app(slot)) return 0;    /* a command, and it is done */
  a = capprun_def(slot);
  if (!a) return -1;

  enter();
  host(a);
  return 0;
}

int launchui_key(uint8_t key) {
  /* The key list is over everything, so it gets the key first: fn-h closes
   * it and so does anything else, because a panel you have to dismiss with
   * one specific key is a panel you fight. */
  if (s_help) {
    s_help = 0;
    if (s_app) { s_app_dirty = 1; s_app_clear = 1; }
    else s_dirty = 1;
    flush();
    return 0;
  }
  if (key == KEY_HELP) { s_help = 1; flush(); return 0; }
  /* The window modifier, in a shell whose windows are the whole screen:
   * closing the app is the only frame operation there is. */
  if (key == KEY_FN_LETTER('w') && s_app) { leave_app(); return 0; }

  /* The file picker has every key while it is up, except the ones that
   * leave the app -- those close it on the way out. */
  if (picker_active() && s_app) {
    if (key == KEY_QUIT) { leave_app(); return 0; }
    if (picker_key(key, s_now_ms)) { s_app_dirty = 1; s_app_clear = 1; }
    flush();
    return 0;
  }

  /* Bind mode: `k` on an app, then the letter. Two keys rather than a chord,
   * because every Opt chord already means "open" -- including here. */
  if (s_binding) {
    const Icon *ic = at(s_sel);
    s_binding = 0;
    if (key >= 'A' && key <= 'Z') key = (uint8_t)(key + 32);
    if (key >= 'a' && key <= 'z' && ic) {
      if (hotkey_set((char)key, ic->name) == 0)
        snprintf(s_note, sizeof s_note, "opt-%c opens %s", key, ic->name);
      else
        snprintf(s_note, sizeof s_note, "opt-%c belongs to the system", key);
    } else {
      s_note[0] = 0;
    }
    s_dirty = 1;
    flush();
    return 0;
  }

  /* ; . , / are the arrow cluster here without needing Fn. The carousel takes
   * no text at all, and a running app only gets the raw keys back while it is
   * actually taking some. */
  if (!s_app || !s_app->wants_text || !s_app->wants_text(s_app->state)) {
    uint8_t arrow = keyboard_arrow_for(key);
    if (arrow) key = arrow;
  }

  if (s_app) {
    /* Escape belongs to the app and never leaves it. It went through two
     * versions: leaving unconditionally, so a dialog's Escape threw the app
     * away; then leaving when the app declined it, which is the same
     * surprise one level down -- back out of a subview once too often and
     * the app is gone. fn-` (KEY_QUIT) is the way out, and the only one. */
    if (key == KEY_QUIT) { leave_app(); return 0; }
    if (s_app->key && s_app->key(s_app->state, key)) {
      s_app_dirty = 1;
      flush();
    }
    return 0;
  }

  switch (key) {
  case KEY_ESC:
    /* Interior only: out of a folder. Never out of the shell. */
    if (s_folder >= 0) close_folder();
    return 0;
  case KEY_QUIT:
    if (s_folder >= 0) { close_folder(); return 0; }
    desktop_set_autostart(0); return 1;     /* to the console */

  /* Up and down move along the row too. There is nothing else to move, and a
   * key that does nothing is worse than a duplicate. */
  case KEY_LEFT:
  case KEY_UP:    move(-1); return 0;
  case KEY_RIGHT:
  case KEY_DOWN:  move(1);  return 0;

  case KEY_ENTER:
  case ' ':
    if (icons_in_count(s_folder)) launch(s_sel);
    return 0;

  case 'k': case 'K': {
    const Icon *ic = at(s_sel);
    if (!ic || ic->kind == ICON_FOLDER || ic->kind == ICON_FIRMWARE) {
      snprintf(s_note, sizeof s_note, "only an app can have a shortcut");
    } else {
      s_binding = 1;
      snprintf(s_note, sizeof s_note, "press a letter for %s", ic->name);
    }
    s_dirty = 1;
    flush();
    return 0;
  }

  case 'r': case 'R':
    icons_reload();
    /* Back to the top: a reload renumbers everything, and the folder that was
     * open may not be there any more. */
    s_folder = -1;
    if (s_sel >= icons_in_count(-1)) s_sel = 0;
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

/* Is the running app taking text? The same question the arrow-key remapping
 * asks, exposed because voice needs the same answer and two callers working it
 * out separately would eventually disagree. */
int launchui_wants_text(void) {
  if (!s_app || !s_app->wants_text) return 0;
  return s_app->wants_text(s_app->state);
}

/* A line under the icon, from something that finished elsewhere -- the
 * background task's radio news, mostly. Shown where the app description goes,
 * because that is the line the eye is already on. */
void launchui_note(const char *text) {
  if (!text) return;
  snprintf(s_note, sizeof s_note, "%s", text);
  s_dirty = 1;
  flush();
}

void launchui_tick(uint32_t ms) {
  uint32_t before = s_now_ms / 1000u;
  s_now_ms = ms;

  /* An app that animates gets every pass, not every second: a game at one
   * frame a second is a slideshow. It runs before the once-a-second work
   * below, and while it owns the screen nothing else here draws. */
  if (s_app && s_app->tick) {
    if (s_app->tick(s_app->state, ms)) { s_app_dirty = 1; flush(); }
  }

  /* Only over the launcher's own screen. A running app owns every pixel it
   * was given, and an overlay in its top corner fought its menu bar: the app
   * repainted the bar, this repainted the dots over it, back and forth at
   * seven frames a second. An app that wants to say it is busy has a better
   * place to do it -- see toolbar_busy in apps/toolbar.h.
   *
   * The spinner is otherwise the one thing that moves while nothing else
   * does, so it needs its own reason to redraw. When the request ends, one
   * full repaint puts back what was under it: there is no back buffer, and
   * the alternative is a row of dots left on the screen. */
  if (s_app) {
    if (s_spin_on) { s_spin_on = 0; launchui_repaint(); }
  } else if (httpq_active()) {
    s_spin_on = 1;
    paint_spinner(ms);
  } else if (s_spin_on) {
    s_spin_on = 0;
    launchui_repaint();
  }

  if (s_now_ms / 1000u == before) return;
  if (s_app) return;               /* the app owns the screen */

  /* A mouse that wanders out of range or sleeps drops the link. Look for it
   * again rather than sitting there with a dead pointer -- but not every
   * second, because each attempt is a multi-second scan. */
  /* Asked for, not done here. This used to call bthid_start(4, ...) inline --
   * a four-second blocking scan, on the tick that draws the screen, every
   * fifteen seconds for as long as a paired mouse stayed out of range. The
   * job runs on the background task now and the shell never waits for it.
   * Only while idle: a scan competing with someone typing is the same fault
   * in a quieter coat. */
  if (bthid_radio_on() && bthid_state(BTHID_MOUSE) != BTH_CONNECTED &&
      bthid_state(BTHID_MOUSE) != BTH_CONNECTING && (s_now_ms / 1000u) % 15 == 0 &&
      bg_idle_ms() > 2000)
    bg_submit(BG_BT_RECONNECT);

  paint_bar();                     /* just the clock strip */
}

/* Over the carousel there is no pointer: nothing to drag, and the wheel and
 * the two sides of the screen are a better gesture than aiming at an icon.
 * Over a running app there is one, because an app is where a mouse is useful.
 *
 * Erasing it is the trick. The panel cannot be read back -- three wires, no
 * MISO -- so what was under the cursor is gone. The app knows, though: paint
 * clipped to the square the cursor just left redraws exactly that, and the
 * cursor goes back on top. */
void launchui_mouse_apply(const MouseReport *r) {
  int btn, wheel, held, moved;
  Rect before = draw_cursor_bounds((int16_t)mouse_x(), (int16_t)mouse_y());

  mouse_apply(r);
  bg_note_activity();
  btn = mouse_pressed(MOUSE_LEFT) ? CAPP_BTN_LEFT
      : mouse_pressed(MOUSE_RIGHT) ? CAPP_BTN_RIGHT : 0;
  held = (mouse_down(MOUSE_LEFT) ? CAPP_BTN_LEFT : 0)
       | (mouse_down(MOUSE_RIGHT) ? CAPP_BTN_RIGHT : 0);
  wheel = mouse_take_wheel();
  moved = mouse_take_moved();
  mouse_released(MOUSE_LEFT);
  mouse_released(MOUSE_RIGHT);

  if (s_app && picker_active()) {
    s_pointer_on = 1;
    if (btn && picker_click((int16_t)mouse_x(), (int16_t)mouse_y(), btn)) {
      s_app_dirty = 1; s_app_clear = 1;
    }
    if (wheel) picker_wheel(wheel > 0 ? -1 : 1);
    flush();
    if (moved && picker_active()) {
      /* The pointer moved over the panel: repaint what it left, then it. */
      Rect after = draw_cursor_bounds((int16_t)mouse_x(), (int16_t)mouse_y());
      draw_set_clip(rect_union(before, after));
      picker_paint_now();
      draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
      draw_cursor((int16_t)mouse_x(), (int16_t)mouse_y());
    }
    return;
  }

  if (s_app) {
    int16_t lx = (int16_t)(mouse_x() - s_app_rect.x);
    int16_t ly = (int16_t)(mouse_y() - s_app_rect.y);
    int repaint = 0;

    s_pointer_on = 1;

    if (btn && s_app->click && rect_contains(s_app_rect, (int16_t)mouse_x(),
                                             (int16_t)mouse_y()) &&
        s_app->click(s_app->state, lx, ly, btn))
      repaint = 1;

    if ((moved || wheel || held) && s_app->mouse &&
        s_app->mouse(s_app->state, lx, ly, held, wheel))
      repaint = 1;

    if (repaint) {
      s_app_dirty = 1;
      flush();
      draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
      draw_cursor((int16_t)mouse_x(), (int16_t)mouse_y());
    } else if (moved) {
      Rect after = draw_cursor_bounds((int16_t)mouse_x(), (int16_t)mouse_y());
      draw_set_clip(rect_intersect(rect_union(before, after), s_app_rect));
      if (s_app->paint) s_app->paint(s_app->state, s_app_rect);
      draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
      draw_cursor((int16_t)mouse_x(), (int16_t)mouse_y());
    }
    return;
  }

  if (wheel) { move(wheel > 0 ? -1 : 1); return; }
  if (!btn) return;

  /* The centre opens what is selected; either side steps towards it, which is
   * the same gesture as clicking the neighbour you can already see. */
  if (mouse_x() < BIG_X) move(-1);
  else if (mouse_x() > BIG_X + BIG) move(1);
  else if (icons_in_count(s_folder)) launch(s_sel);
}

void launchui_mouse_done(void) { flush(); }
