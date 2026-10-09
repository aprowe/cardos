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
#include "kernel/console/console.h"
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
#include "kernel/ui/appsearch.h"
#include "kernel/ui/picker.h"
#include "kernel/ui/apphost.h"
#include "kernel/ui/icons_builtin.h"
#include "kernel/sys/hotkeys.h"
#include "kernel/sys/clock.h"
#include "kernel/drv/battery.h"
#include "kernel/drv/display.h"
#include "kernel/sys/blip.h"
#include "kernel/sys/notify.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
/* How the next paint of it came about: AH_FULL and friends (apphost.h). The
 * carousel is still on the panel when an app opens, and help or the picker
 * was over it when they close: a whole repaint, composed off the panel, and
 * the surround outside a smaller app's rect. Never a white fill of the app's
 * rect first -- that was the white frame between the carousel and the app. */
static int            s_app_how;
static int            s_help;         /* the key list is over everything */
static int            s_binding;      /* k was pressed: the next letter binds */
static int            s_pointer_on;   /* a mouse has moved: there is a cursor */

/* The way back. An app that opens another -- Notes opening a note in Edit,
 * Files a file -- is closed first and remembered here, and leaving the one
 * it opened opens it again. Closed first because both at once did not fit:
 * Edit would not load beside Notes, and the key that asked did nothing at
 * all. Names, not runs: the app comes back fresh, as it would from the row. */
#define BACK_MAX 4
static char s_back[BACK_MAX][20];
/* What each was opened with, so Edit comes back to its file. */
static char s_back_args[BACK_MAX][128];
static int  s_nback;
static char s_app_args[128];                /* what the app on screen was given */
static char s_next[20], s_next_args[128];   /* asked for from a handler */
static int  s_has_next;

static void start_app(int slot, const char *name, const char *args) {
  snprintf(s_app_args, sizeof s_app_args, "%s", args ? args : "");
  notify_opened(name);                         /* its notifications are read */
  capprun_start(slot, name, args);
}

/* The search (Space): what has been typed, and the best matches for it as
 * flat icon indices. */
#define HITS 6
static int  s_search;
static char s_q[24];
static int  s_hit[HITS], s_nhit, s_hsel;

static void paint_search(void);   /* with the rest of the search, below */

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
static void paint_bar_body(void) {
  char clock[8];
  int16_t x = DISPLAY_W - 4;

  draw_rect(R(0, 0, DISPLAY_W, BAR_H), C_TITLE);
  draw_text(4, 2, "CardOS", C_TITLE_FG, C_TITLE);
  /* Unread notifications: a dot after the name; fn-n shows them. */
  if (notify_unread()) {
    draw_rect(R(4 + 6 * 6 + 3, 4, 4, 4), RGB565(255, 196, 64));
  }

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

/* What the bar shows, in one number: the once-a-second tick repaints it
 * only when that changes. It used to repaint every second regardless, a
 * navy fill and then the text over it, and the fill showed: the bar
 * blinked once a second. */
static uint32_t bar_state(void) {
  char clock[8];
  uint32_t h = 2166136261u;
  const char *p;
  clock_hm(clock, sizeof clock);
  for (p = clock; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  h = (h ^ (uint32_t)(battery_percent() * 10 / 100 + 1)) * 16777619u;
  h = (h ^ (uint32_t)(battery_percent() <= 15)) * 16777619u;
  h = (h ^ (uint32_t)(bthid_state(BTHID_KEYBOARD) == BTH_CONNECTED)) * 16777619u;
  h = (h ^ (uint32_t)(bthid_state(BTHID_MOUSE) == BTH_CONNECTED) << 1) * 16777619u;
  h = (h ^ (uint32_t)wifi_is_connected() << 2) * 16777619u;
  h = (h ^ (uint32_t)(notify_unread() != 0) << 3) * 16777619u;
  return h;
}
static uint32_t s_bar_shown;

/* Rows y0..y1, composed off the panel a strip at a time by `body` -- which
 * draws everything and is clipped to each strip in turn (draw_offscreen).
 * Without the memory for a strip it draws straight to the panel, as it
 * always used to. */
static void (*s_off_body)(void);
static void off_body(void *ctx) { (void)ctx; s_off_body(); }
static void paint_offscreen(int y0, int y1, uint16_t prefill, void (*body)(void)) {
  s_off_body = body;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_offscreen(R(0, y0, DISPLAY_W, y1 - y0), prefill, off_body, NULL);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}

/* Composed off the panel and sent whole, like the carousel. It had a 5.6 KB
 * buffer of its own, held for the uptime; draw_offscreen's strip is shared. */
static void paint_bar(void) {
  if (notify_covers()) return;                   /* under a banner; repainted when it goes */
  s_bar_shown = bar_state();
  paint_offscreen(0, BAR_H, C_TITLE, paint_bar_body);
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

/* ---- the carousel, and how it moves ----
 *
 * Every entry has a place relative to the selection: 0 in the middle, -1 and
 * 1 beside it. A move changes the selection at once and sets s_anim to the
 * distance the row still has to travel, in 1/256 of a slot, which eases to
 * nothing over ANIM_MS: so an entry is drawn at its slot plus that offset,
 * its size and its x following from where that puts it. 64 px in the middle,
 * 32 one slot out, and further out it runs off the edge.
 *
 * Drawn off the panel, a strip at a time (draw_offscreen), and each strip
 * sent whole. The old way filled the area teal and then drew over it, and
 * the teal showed: that was the launcher's flicker. */
#define ANIM_MS   170
#define SLOT_X    (BIG / 2 + SIDE_GAP + SMALL / 2)    /* 60: centre to neighbour */


static int      s_anim;           /* slots still to travel, x256; 0 at rest */
static int      s_anim_from;
static uint32_t s_anim_at;

/* Where an entry `t256` slots from the middle sits, and how big it is. */
static void slot_place(int t256, int16_t *cx, int16_t *size) {
  int a = t256 < 0 ? -t256 : t256;
  if (a <= 256) {
    *size = (int16_t)(BIG - (BIG - SMALL) * a / 256);
    *cx = (int16_t)(DISPLAY_W / 2 + SLOT_X * t256 / 256);
  } else {
    int out = SLOT_X + (a - 256) * 90 / 256;           /* faster off the edge */
    *size = SMALL;
    *cx = (int16_t)(DISPLAY_W / 2 + (t256 < 0 ? -out : out));
  }
}

static void paint_icon_at(int idx, int16_t cx, int16_t size, uint16_t fg) {
  const uint16_t *px = icon_colour(idx);
  int16_t x = (int16_t)(cx - size / 2), y = (int16_t)(ICON_TOP + BIG / 2 - size / 2);
  const uint8_t *bits;
  if (px) { draw_image_fit(x, y, CAPP_ICON_W, CAPP_ICON_H, px, size, size, 0x0000); return; }
  bits = icon_bitmap(idx);
  if (bits) draw_bitmap1_fit(x, y, CAPP_ICON_W, CAPP_ICON_H, bits, size, size, fg, C_DESKTOP);
}

/* Everything below the bar, at the current offset, to whatever the clip is. */
static void paint_carousel_body(void) {
  int n = icons_in_count(s_folder), k, near = 0, best = 1 << 30;
  const Icon *ic;

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

  /* Outside in, so the middle one is drawn last and on top. One entry is
   * never drawn twice: with two or three apps the row wraps onto itself. */
  for (k = 3; k >= 0; k--) {
    int side;
    for (side = (k ? -1 : 1); side <= 1; side += 2) {
      int slot = k * side, t256 = slot * 256 + s_anim;
      int16_t cx, size;
      if (n == 1 && slot) continue;
      if (n == 2 && slot && slot != (s_anim > 0 ? -1 : 1)) continue;
      if (n > 2 && (slot < -(n - 1) / 2 || slot > n / 2)) continue;   /* each entry once */
      slot_place(t256, &cx, &size);
      if (cx + size / 2 < 0 || cx - size / 2 >= DISPLAY_W) continue;
      paint_icon_at(icon_index(at(wrap(s_sel + slot))), cx, size, slot ? C_SHADOW : C_TITLE_FG);
      if ((t256 < 0 ? -t256 : t256) < best) { best = t256 < 0 ? -t256 : t256; near = slot; }
    }
  }

  /* The name of whichever entry is nearest the middle, travelling with it. */
  ic = at(wrap(s_sel + near));
  if (ic) {
    int16_t shift = (int16_t)(SLOT_X * (near * 256 + s_anim) / 256);
    int16_t w = (int16_t)(draw_text_width(ic->name) * 2);
    int16_t nx = (int16_t)((DISPLAY_W - w) / 2);
    char where[48];
    const char *kind;
    int16_t kx;

    /* Inside a folder the level is always on screen: "Games / app" rather
     * than a bare "app" that looks the same at the top. */
    if (s_note[0] && !s_anim) kind = s_note;
    else if (s_folder >= 0) {
      const Icon *f = icon_at(s_folder);
      snprintf(where, sizeof where, "%s / %s", f ? f->name : "?", kind_word(ic));
      kind = where;
    } else kind = kind_word(ic);
    kx = (int16_t)((DISPLAY_W - draw_text_width(kind)) / 2);

    if (nx < 2) nx = 2;
    if (kx < 2) kx = 2;
    draw_text_scaled((int16_t)(nx + shift), NAME_Y, ic->name, 2, C_TITLE_FG, C_DESKTOP);
    draw_text_ellipsis((int16_t)(kx + shift), KIND_Y, (int16_t)(DISPLAY_W - 4), kind,
                       C_DESK_DIM, C_DESKTOP);
  }

  paint_pips(n);
}

/* The carousel's rows, through the same strip as everything else. It
 * mallocked 9.6 KB a frame of its own, 20 rows at a time, and a frame of
 * the slide that found no 9.6 KB block was drawn straight to the panel --
 * memory pressure showing up as flicker. */
static void paint_carousel_rows(int y0, int y1) {
  paint_offscreen(y0, y1, C_DESKTOP, paint_carousel_body);
}
static void paint_carousel(void) {
  int top = notify_covers() > BAR_H ? notify_covers() : BAR_H;   /* not under a banner */
  paint_carousel_rows(top, DISPLAY_H);
}

/* One step of the slide; 1 while there is more to go. Ease-out: fast away,
 * slow to settle, which is what reads as smooth rather than mechanical. */
static int anim_step(uint32_t now) {
  uint32_t dt = now - s_anim_at;
  int left;
  if (!s_anim) return 0;
  if (dt >= ANIM_MS) s_anim = 0;
  else {
    left = (int)(ANIM_MS - dt);                         /* ANIM_MS..1 */
    /* (left/ANIM_MS)^3, in integers */
    s_anim = (int)((int64_t)s_anim_from * left * left / ANIM_MS * left / ANIM_MS / ANIM_MS);
  }
  /* The icons and the two lines under them; the pips are already right. */
  paint_carousel_rows(notify_covers() > ICON_TOP ? notify_covers() : ICON_TOP, KIND_Y + 8);
  return s_anim != 0;
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

/* A rectangle the shell must repaint for reasons of its own -- the busy
 * badge went -- kept apart from the app's damage. */
static Rect s_extra;
static int  s_has_extra;

static void paint_app(int asked) {
  int how = s_app_how | (asked ? AH_ASKED : 0);
  s_app_how = 0;
  apphost_paint(s_app, s_app_rect, how, s_has_extra ? &s_extra : NULL);
  s_has_extra = 0;
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
           "arrows\tmove along the row\nenter\topen\nspace\tsearch: type a name, enter opens\n"
           "k\tbind opt-letter to this app\n"
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
    if (picker_active())
      help_paint("Files", picker_help(), "fn-`\tcancel and leave the app\nfn-h\tclose this\n");
    else
      /* With no app open these are the launcher's keys, whatever icon is
       * highlighted: titled with that icon's name, the panel read as the
       * app's own keys -- and listed none. */
      help_paint(s_app ? s_app->name : "Launcher",
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
    int asked = s_app_dirty;
    if (!asked && !s_has_extra) return;
    s_app_dirty = 0;
    paint_app(asked);
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
  if (s_search) paint_search();
  else paint_carousel();
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

void launchui_damage(Rect r) {
  if (!s_app) { s_dirty = 1; flush(); return; }
  /* Help and the picker are over the app, and are repainted whole. */
  if (s_help || picker_active()) { s_app_dirty = 1; flush(); return; }
  s_extra = s_has_extra ? rect_union(s_extra, r) : r;
  s_has_extra = 1;
  flush();
}

void launchui_repaint(void) {
  /* All of it: whatever asked (a banner going, an alarm, a panel) was over
   * the app, and the app's own damage knows nothing about it. */
  if (s_app) { s_app_dirty = 1; s_app_how |= AH_FULL | AH_SURROUND; }
  else s_dirty = 1;
  flush();
}

/* `show`: paint the launcher now. Not when another app is about to take the
 * screen -- the launcher drawn for a frame between two apps was a flash. */
static void leave_app_ex(int show) {
  picker_close();                 /* a picker the app was waiting on goes with it */
  /* Hand the image back: an app the launcher is no longer showing is not
   * going to be called into, and its code is 4 KB of a small pool. Starting
   * it again reloads it, which costs a card read nobody notices. */
  capprun_release(s_app);
  s_app = NULL;
  s_note[0] = 0;
  s_dirty = 1;
  if (show) flush();
}


static int run_now(const char *name, const char *args);

/* Leaving an app by the quit key: back to the app that opened it, if one
 * did, or to the row. */
static void quit_app(void) {
  char name[20], args[128];
  blip(BLIP_BACK);
  leave_app_ex(s_nback == 0);
  if (s_nback > 0) {
    s_nback--;
    snprintf(name, sizeof name, "%s", s_back[s_nback]);
    snprintf(args, sizeof args, "%s", s_back_args[s_nback]);
    if (run_now(name, args[0] ? args : NULL) != 0 || !s_app) flush();
  }
}

/* A start that started nothing and said why: on the row, where the eye is,
 * and on the console if that is where it was typed. */
static int said_why(const char *name) {
  const char *why = capprun_start_error();
  if (!why[0]) return 0;
  blip(BLIP_ERROR);
  snprintf(s_note, sizeof s_note, "%s: %s", name, why);
  if (ui_shell() != UI_LAUNCHER) con_printf("%s: %s\n", name, why);
  s_dirty = 1;
  flush();
  return 1;
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
    start_app(ic->slot, ic->name, args);
    if (!capprun_is_app(ic->slot)) { said_why(ic->name); return; }
    a = capprun_def(ic->slot);
    if (!a) { flush(); return; }
  } else {
    a = icon_app(i);
    if (!a) { flush(); return; }
    if (a->open) a->open(a->state);
    if (args && *args && a->set_args) a->set_args(a->state, args);
  }

  /* Everything runs fullscreen here, whatever size it asked for: there is no
   * desktop behind it for a window to sit on. */
  s_app = a;
  s_app_rect = app_rect(a);
  s_app_how = AH_FULL | AH_SURROUND | AH_OPENED;
  s_app_dirty = 1;
  flush();
}

/* `pos` is a carousel position at the current level. */
static void launch(int pos) {
  const Icon *ic = at(pos);
  s_nback = 0;                    /* opened from the row: back is the row */
  s_anim = 0;
  blip(BLIP_OPEN);
  launch_with(icon_index(ic), NULL);
}

/* ------------------------------------------------------------- search --- */

/* Every app, in the order the row shows them -- favourites first, then each
 * folder's apps where the folder is -- once each, then ranked. */
static void search_update(void) {
  const char *names[64];
  int flat[64], got[HITS], n = 0, i, j, k, top = icons_in_count(-1);
  for (i = 0; i < top && n < 64; i++) {
    const Icon *ic = icons_in_at(-1, i);
    int inside = ic && ic->kind == ICON_FOLDER ? icons_in_count(icon_index(ic)) : 0;
    for (j = -1; j < inside && n < 64; j++) {
      const Icon *x = j < 0 ? ic : icons_in_at(icon_index(ic), j);
      int f = icon_index(x), dup = 0;
      if (!x || x->cli || x->kind == ICON_FOLDER || x->kind == ICON_FIRMWARE) continue;
      for (k = 0; k < n; k++) if (flat[k] == f) dup = 1;
      if (dup) continue;
      names[n] = x->name;
      flat[n++] = f;
    }
  }
  s_nhit = appsearch_rank(names, n, s_q, got, HITS);
  for (i = 0; i < s_nhit; i++) s_hit[i] = flat[got[i]];
  s_hsel = 0;
}

static void search_open(void) {
  s_search = 1;
  s_q[0] = 0;
  search_update();
  s_dirty = 1;
  flush();
}

/* Off the panel like the carousel: it cleared the screen and redrew every
 * row on each arrow, and the clear showed. */
static void paint_search_body(void);
static void paint_search(void) {
  int top = notify_covers() > BAR_H ? notify_covers() : BAR_H;
  paint_offscreen(top, DISPLAY_H, C_DESKTOP, paint_search_body);
}

static void paint_search_body(void) {
  int i, y = BAR_H + 26;
  char line[32];
  draw_rect(R(0, BAR_H, DISPLAY_W, DISPLAY_H - BAR_H), C_DESKTOP);
  draw_rect(R(8, BAR_H + 5, DISPLAY_W - 16, 16), C_TITLE);
  snprintf(line, sizeof line, "> %s_", s_q);
  draw_text(14, BAR_H + 9, line, C_TITLE_FG, C_TITLE);
  if (!s_nhit) {
    draw_text(14, y + 4, "no app called that", C_DESK_DIM, C_DESKTOP);
    return;
  }
  for (i = 0; i < s_nhit; i++, y += 17) {
    const Icon *ic = icon_at(s_hit[i]);
    const Icon *in = ic && ic->parent >= 0 ? icon_at(ic->parent) : NULL;
    uint16_t bg = i == s_hsel ? C_TITLE : C_DESKTOP;
    if (!ic) continue;
    draw_rect(R(8, y, DISPLAY_W - 16, 17), bg);
    paint_icon(s_hit[i], 12, (int16_t)(y + 1), 1, C_TITLE_FG);
    draw_text(34, (int16_t)(y + 5), ic->name, C_TITLE_FG, bg);
    if (in) draw_text((int16_t)(DISPLAY_W - 12 - draw_text_width(in->name)), (int16_t)(y + 5),
                      in->name, C_DESK_DIM, bg);
  }
}

/* Typing finds, arrows choose, enter opens, escape (or deleting past the
 * start) closes. Arrives with ; and . already turned into up and down. */
static int search_key(uint8_t key) {
  int n = (int)strlen(s_q);
  if (key == KEY_ESC || key == KEY_QUIT) s_search = 0;
  else if (key == KEY_UP) { if (s_hsel > 0) s_hsel--; }
  else if (key == KEY_DOWN) { if (s_hsel + 1 < s_nhit) s_hsel++; }
  else if (key == KEY_ENTER) {
    if (s_nhit) {
      int f = s_hit[s_hsel];
      s_search = 0;
      s_nback = 0;
      select_flat(f);              /* leaving the app lands on it */
      s_dirty = 1;
      launch_with(f, NULL);
      return 0;
    }
  } else if (key == KEY_BACKSPACE) {
    if (n) { s_q[n - 1] = 0; search_update(); }
    else s_search = 0;
  } else if (key >= 32 && key < 127 && n < (int)sizeof s_q - 1) {
    s_q[n] = (char)key;
    s_q[n + 1] = 0;
    search_update();
  }
  s_dirty = 1;
  flush();
  return 0;
}

static void move(int delta) {
  if (icons_in_count(s_folder) == 0) return;
  s_sel = wrap(s_sel + delta);
  s_note[0] = 0;
  /* From wherever the row is now, so keys faster than the slide add up
   * rather than jump; never more than two slots behind. */
  s_anim += delta * 256;
  if (s_anim > 512) s_anim = 512;
  if (s_anim < -512) s_anim = -512;
  s_anim_from = s_anim;
  s_anim_at = s_now_ms;
  blip(BLIP_MOVE);
  paint_pips(icons_in_count(s_folder));
  anim_step(s_now_ms);
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
  /* No fill, from another shell either: what paints next covers the whole
   * panel -- the carousel and its bar, or an app with its surround, both
   * composed off the panel -- and a fill first was a teal flash. */
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
  s_nback = 0;
  s_has_next = 0;
  s_search = 0;
  icons_reload();
  s_sel = 0;
  s_folder = -1;
  enter();
  flush();
}

/* opt-space, from anywhere: the same search Space already opens inside
 * the carousel, reached without going there first. Already in the
 * launcher -- the carousel, or an app running fullscreen over it -- this
 * only opens the overlay: launchui_key/tick/paint all check s_search
 * before s_app already, so whatever is running stays loaded underneath,
 * untouched, the same as pressing Space at the carousel never did
 * anything to it either. From another shell there is no carousel to
 * overlay yet, so this takes the screen the way fn-` already does from
 * the console -- that leaves nothing to come back to on Escape, which is
 * the next thing to fix. */
void launchui_open_search(void) {
  need_icons();
  search_open();
}

int launchui_search_active(void) { return s_search; }

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
  s_app_how = AH_FULL | AH_SURROUND | AH_OPENED;
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

/* A console line run from the dashboard (con_capturing) that turned out to
 * be an app wanting the screen: let it go again, say so in the captured
 * output, and do not host it -- the app on screen is the one keeping the
 * dashboard's link alive. */
static int from_dashboard(int slot) {
  if (!con_capturing()) return 0;
  capprun_release(capprun_def(slot));
  con_write("that app opens on the device's screen; not from the dashboard\n");
  return 1;
}

/* Is there an app (or command) by this name? */
static int find_named(const char *name) {
  int i;
  need_icons();
  for (i = 0; i < icons_total(); i++) {
    const Icon *ic = icon_at(i);
    if (ic && same_name(ic->name, name)) return 1;
  }
  return 0;
}

/* The one asked for from a handler, now that the handler has returned: the
 * app that asked goes first, then this one loads into the room it left. */
static void run_next(void) {
  char name[20], args[128];
  s_has_next = 0;
  snprintf(name, sizeof name, "%s", s_next);
  snprintf(args, sizeof args, "%s", s_next_args);
  if (s_app) {
    if (s_nback == BACK_MAX) {
      memmove(s_back[0], s_back[1], sizeof s_back - sizeof s_back[0]);
      memmove(s_back_args[0], s_back_args[1], sizeof s_back_args - sizeof s_back_args[0]);
      s_nback--;
    }
    snprintf(s_back[s_nback], sizeof s_back[0], "%s", s_app->name);
    snprintf(s_back_args[s_nback], sizeof s_back_args[0], "%s", s_app_args);
    s_nback++;
    leave_app_ex(0);
  }
  if (run_now(name, args[0] ? args : NULL) != 0 || !s_app) {
    /* It did not open, and the row says why; going back to the app that
     * asked would hide that. */
    if (s_nback) s_nback--;
    flush();
  }
}

int launchui_run(const char *name, const char *args) {
  if (!name || !*name) return -1;
  /* From the handler of the app on screen: later, once it has been let go.
   * Its code is on the stack now, so it cannot go yet -- and loading the
   * next one beside it is what ran out of room. */
  if (s_app && capprun_caller() == s_app->state && ui_shell() == UI_LAUNCHER) {
    if (!find_named(name)) return -1;
    snprintf(s_next, sizeof s_next, "%s", name);
    snprintf(s_next_args, sizeof s_next_args, "%s", args ? args : "");
    s_has_next = 1;
    return 0;
  }
  return run_now(name, args);
}

static int run_now(const char *name, const char *args) {
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
      start_app(ic->slot, ic->name, args);
      if (!capprun_is_app(ic->slot)) {
        /* A command, and it is done -- or an app that would not start, and
         * the reason has been said. Either way the name was not unknown. */
        if (said_why(ic->name) && ui_shell() == UI_LAUNCHER) select_flat(i);
        return 0;
      }
      if (from_dashboard(ic->slot)) return 0;
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
      start_app(ic->slot, ic->name, args);
      if (!capprun_is_app(ic->slot)) return 0;   /* a command, already done */
      if (from_dashboard(ic->slot)) return 0;
      select_flat(i);
      enter();
      host(capprun_def(ic->slot));
      return 0;
    }
  }

  slot = capprun_load_once(path);
  if (slot < 0) return -1;
  start_app(slot, path, args);
  if (!capprun_is_app(slot)) return 0;    /* a command, and it is done */
  if (from_dashboard(slot)) return 0;
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
    if (s_app) { s_app_dirty = 1; s_app_how |= AH_FULL | AH_SURROUND; }
    else s_dirty = 1;
    flush();
    return 0;
  }
  if (key == KEY_HELP) { s_help = 1; flush(); return 0; }
  /* The window modifier, in a shell whose windows are the whole screen:
   * closing the app is the only frame operation there is. */
  if (key == KEY_FN_LETTER('w') && s_app) { quit_app(); return 0; }

  /* The file picker has every key while it is up, except the ones that
   * leave the app -- those close it on the way out. */
  if (picker_active() && s_app) {
    if (key == KEY_QUIT) { quit_app(); return 0; }
    if (!picker_wants_text()) {
      uint8_t arrow = keyboard_arrow_for(key);
      if (arrow) key = arrow;
    }
    if (picker_key(key, s_now_ms)) { s_app_dirty = 1; s_app_how |= AH_FULL | AH_SURROUND; }
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
    if (key == KEY_QUIT) { quit_app(); return 0; }
    if (s_app->key && s_app->key(s_app->state, key)) {
      s_app_dirty = 1;
      flush();
    }
    return 0;
  }

  if (s_search) return search_key(key);

  switch (key) {
  case KEY_ESC:
    /* Interior only: out of a folder. Never out of the shell. */
    if (s_folder >= 0) { blip(BLIP_BACK); close_folder(); }
    return 0;
  case KEY_QUIT:
    if (s_folder >= 0) { close_folder(); return 0; }
    return 1;                               /* to the console */

  /* Up and down move along the row too. There is nothing else to move, and a
   * key that does nothing is worse than a duplicate. */
  case KEY_LEFT:
  case KEY_UP:    move(-1); return 0;
  case KEY_RIGHT:
  case KEY_DOWN:  move(1);  return 0;

  case KEY_ENTER:
    if (icons_in_count(s_folder)) launch(s_sel);
    return 0;
  case ' ':
    blip(BLIP_SOFT);
    search_open();
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
int launchui_button(int event, const char *text) {
  int r;
  if (picker_active() || !s_app || !s_app->button) return 0;
  r = s_app->button(s_app->state, event, text);
  if (event != 0 && r) launchui_repaint();
  return r;
}

int launchui_wants_text(void) {
  if (picker_active()) return picker_wants_text();
  if (!s_app && s_search) return 1;
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

  if (s_has_next) run_next();
  if (s_anim && !s_app && !s_search && !s_help && !picker_active()) anim_step(ms);

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

  if (bar_state() != s_bar_shown) paint_bar();   /* just the clock strip, if it changed */
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
      s_app_dirty = 1; s_app_how |= AH_FULL | AH_SURROUND;
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
