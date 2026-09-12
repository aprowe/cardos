/* The desktop shell. See desktop.h. */

#include "kernel/ui/desktop.h"
#include "kernel/ui/draw.h"
#include "kernel/drv/keyboard.h"
#include "kernel/mem/mem.h"
#include "kernel/fs/fs.h"
#include "kernel/ui/app.h"
#include "kernel/app/launcher.h"
#include "kernel/app/capprun.h"
#include "kernel/ui/icons.h"
#include "kernel/ui/shell.h"
#include "kernel/ui/help.h"
#include "kernel/drv/bthid.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <stdio.h>
#include <string.h>

#define DESK_H (DISPLAY_H - TASKBAR_H)

/* What a window shows. The desktop owns the content because there is no app
 * model yet -- that arrives with the context switch, when each window becomes
 * a task with its own paint callback. */
#define MAX_OPEN 4

static WinId s_win[MAX_OPEN];
static const AppDef *s_app[MAX_OPEN];   /* built-in or loaded, indistinguishable */

/* Minimised windows.
 *
 * A minimised window has no WinId -- it is destroyed, because the window
 * system has no notion of one that is not on screen, and giving it one would
 * mean teaching every hit test, paint and damage calculation to skip it.
 * What is kept is the pair that matters: which app, and the frame it had. The
 * app object outlives its window (that is why an app's state survives being
 * maximised too), so restoring is opening it again at the same rectangle
 * without calling open() -- which would wipe what it was in the middle of. */
static Rect s_min_frame[MAX_OPEN];
static int  s_minimised[MAX_OPEN];
static int16_t s_scroll[MAX_OPEN];      /* pixels of content hidden above */
static int   s_nwin;

/* Width of the scrollbar taken out of the content well. Four pixels is enough
 * to see and to hit with a pointer on a 240px screen, and it only appears when
 * there is something to scroll. */
#define SCROLL_W 4

/* A fullscreen app owns the whole panel: no chrome, no taskbar, and the
 * compositor is bypassed entirely -- the render loop paints its rectangle and
 * nothing else, which is what the user asked for and also what makes a
 * picture viewer worth having on a 240x135 screen. */
static const AppDef *s_full;
static int s_full_dirty;
static int s_full_clear;     /* the desktop is still on the panel underneath */
static Rect s_full_rect;

/* The pointer can also ask to leave for the console, and a mouse handler has
 * no return value that reaches the main loop. */
static int s_leave_for_console;

/* The ctrl-h key list, drawn over the compositor's output like the start
 * menu. Closing it repaints, because nothing below it knows it was there. */
static int s_help;

/* ------------------------------------------------------------- icons ---- */

#define ICON_W     46
#define ICON_H     34
#define ICON_BOX   16

/* Whether the icons have the keyboard rather than a window. With no windows
 * open it is the only thing that could, so it starts true; Tab moves it
 * between the windows and the desktop, the way Tab moves focus anywhere. */
static int  s_icon_focus = 1;
static int  s_sel_icon = 0;
static int  s_last_icon = -1;
static uint32_t s_last_click_ms;

static Rect icon_rect(int i) {
  int per_row = DISPLAY_W / ICON_W;
  int col = i % per_row, row = i / per_row;
  Rect r;
  r.x = (int16_t)(4 + col * ICON_W);
  r.y = (int16_t)(4 + row * ICON_H);
  r.w = ICON_W - 6;
  r.h = ICON_H - 6;
  return r;
}

static int      s_kbd_btn;
static int      s_start_open;
static int      s_start_sel;
static uint32_t s_now_ms;

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int win_index(WinId w) {
  int i;
  /* WIN_NONE is what a minimised slot holds, so asking for it must not match
   * the first one that happens to be down on the taskbar. */
  if (w == WIN_NONE) return -1;
  for (i = 0; i < s_nwin; i++) if (s_win[i] == w) return i;
  return -1;
}

static const AppDef *app_of(WinId w) {
  int i = win_index(w);
  return i < 0 ? app_at(0) : s_app[i];
}

/* How tall the app wants to be, and therefore whether this window scrolls.
 * Zero from the app means "exactly the window", which is the common case. */
static int16_t content_height(const AppDef *a, Rect inner) {
  int16_t want = a->height ? a->height(a->state, inner.w) : 0;
  return want > inner.h ? want : inner.h;
}

static void clamp_scroll(int idx, const AppDef *a, Rect inner) {
  int16_t max = (int16_t)(content_height(a, inner) - inner.h);
  if (max < 0) max = 0;
  if (s_scroll[idx] > max) s_scroll[idx] = max;
  if (s_scroll[idx] < 0) s_scroll[idx] = 0;
}

void desktop_reload_icons(void) {
  icons_reload();
  /* Something stays selected while there is anything to select. Clearing it
   * meant the first arrow press after a reload was spent putting the selection
   * back rather than moving it -- invisible unless you count keystrokes, and
   * wrong every time. */
  s_sel_icon = icons_count() ? 0 : -1;
}

static void paint_icons(Rect clip) {
  int i, n = icons_count();
  for (i = 0; i < n; i++) {
    const Icon *ic = icon_at(i);
    Rect r = icon_rect(i), box;
    const uint8_t *bits;
    if (r.y + r.h > DESK_H) break;
    if (!rect_overlaps(r, clip)) continue;
    draw_set_clip(rect_intersect(clip, r));

    box.x = (int16_t)(r.x + (r.w - ICON_BOX) / 2);
    box.y = r.y;
    box.w = ICON_BOX;
    box.h = ICON_BOX;
    /* Firmware gets a sunken slab, an app a raised one, so the two kinds are
     * distinguishable before reading the label. */
    if (ic->kind == ICON_FIRMWARE) draw_bevel(box, C_TITLE_UN, C_SHADOW, C_LIGHT);
    else                           draw_bevel(box, C_FACE, C_LIGHT, C_DARK);

    {
      const uint16_t *px = icon_colour(i);
      if (px) {
        draw_image_scaled(box.x, box.y, CAPP_ICON_W, CAPP_ICON_H, px, 1, 0x0000);
        bits = NULL;
        goto labelled;
      }
    }
    bits = icon_bitmap(i);
    if (bits) {
      /* The app supplied this. 16x16 is exactly the slab, so it replaces the
       * face rather than sitting inside it. */
      draw_bitmap1(box.x, box.y, CAPP_ICON_W, CAPP_ICON_H, bits, C_TEXT, C_FACE);
    } else {
      draw_text((int16_t)(box.x + 5), (int16_t)(box.y + 4),
                ic->kind == ICON_FIRMWARE ? "F" : "A", C_TEXT,
                ic->kind == ICON_FIRMWARE ? C_TITLE_UN : C_FACE);
    }

labelled:
    if (i == s_sel_icon) draw_frame(r, C_TITLE_FG);
    draw_text_ellipsis(r.x, (int16_t)(r.y + ICON_BOX + 2), r.w,
                       ic->name, C_TITLE_FG, C_DESKTOP);
    draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  }
}

/* Move the selection by a whole row or a single icon, and repaint only the two
 * that changed -- a full repaint for a selection frame is what made the Start
 * menu flicker, and the same would happen here. */
static void select_icon(int delta) {
  int n = icons_count();
  int was = s_sel_icon;

  if (n == 0) return;
  if (s_sel_icon < 0) s_sel_icon = 0;
  else s_sel_icon += delta;

  if (s_sel_icon < 0) s_sel_icon = 0;
  if (s_sel_icon >= n) s_sel_icon = n - 1;
  if (s_sel_icon == was) return;

  if (was >= 0 && was < n) wm_damage(icon_rect(was));
  wm_damage(icon_rect(s_sel_icon));
}

static int icons_per_row(void) { return DISPLAY_W / ICON_W; }

/* ---------------------------------------------------------- chrome ------ */

static void paint_window(WinId w, Rect clip) {
  Rect f = wm_frame(w);
  Rect title, close, content;
  int focused = (wm_focus() == w);

  draw_set_clip(clip);

  /* Outer bevel: raised, the way a window should look. */
  draw_bevel(f, C_FACE, C_LIGHT, C_DARK);
  draw_frame(rect_inset(f, 1), C_FACE);

  title.x = (int16_t)(f.x + WM_BORDER + 1);
  title.y = (int16_t)(f.y + WM_BORDER + 1);
  title.w = (int16_t)(f.w - 2 * WM_BORDER - 2);
  title.h = WM_TITLE_H - 1;
  if (title.w > 0)
    draw_rect(title, focused ? C_TITLE : C_TITLE_UN);

  if (title.w > 12) {
    draw_text_ellipsis((int16_t)(title.x + 2), (int16_t)(title.y),
                       (int16_t)(title.w - 3 * WM_CLOSE_W - 7), wm_title(w),
                       C_TITLE_FG, focused ? C_TITLE : C_TITLE_UN);
    /* Close box, raised, with an x. */
    close.x = (int16_t)(title.x + title.w - WM_CLOSE_W);
    close.y = (int16_t)(title.y);
    close.w = WM_CLOSE_W;
    close.h = (int16_t)(WM_TITLE_H - 2);
    draw_bevel(close, C_FACE, C_LIGHT, C_DARK);
    draw_text((int16_t)(close.x + 1), (int16_t)(close.y - 1), "x", C_DARK, C_FACE);

    /* Maximise, to its left: a little empty frame, which is what the box does
     * to the screen. Drawn rather than lettered because at seven pixels a
     * glyph would be a smudge. */
    if (title.w > 24) {
      Rect mx = close;
      mx.x = (int16_t)(close.x - WM_CLOSE_W - 1);
      draw_bevel(mx, C_FACE, C_LIGHT, C_DARK);
      draw_frame(R((int16_t)(mx.x + 1), (int16_t)(mx.y + 1),
                   (int16_t)(mx.w - 2), (int16_t)(mx.h - 2)), C_DARK);
      draw_rect(R((int16_t)(mx.x + 1), (int16_t)(mx.y + 1),
                  (int16_t)(mx.w - 2), 1), C_DARK);
    }
    /* Minimise: a line along the bottom, the shape of a window lying down. */
    if (title.w > 36) {
      Rect mn = close;
      mn.x = (int16_t)(close.x - 2 * (WM_CLOSE_W + 1));
      draw_bevel(mn, C_FACE, C_LIGHT, C_DARK);
      draw_rect(R((int16_t)(mn.x + 2), (int16_t)(mn.y + mn.h - 3),
                  (int16_t)(mn.w - 4), 2), C_DARK);
    }
  }

  /* Content: sunken, so it reads as a well rather than a panel. */
  content = wm_content(w);
  if (rect_is_empty(content)) { draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H)); return; }
  draw_bevel(content, C_WHITE, C_SHADOW, C_LIGHT);

  {
    const AppDef *a = app_of(w);
    int idx = win_index(w);
    Rect inner = rect_inset(content, 2);
    Rect vis;
    int16_t natural = content_height(a, inner);
    int scrolls = (natural > inner.h) && idx >= 0;

    if (scrolls) inner.w = (int16_t)(inner.w - SCROLL_W);
    if (scrolls) clamp_scroll(idx, a, inner);

    vis = rect_intersect(clip, inner);
    if (a->paint && !rect_is_empty(vis)) {
      /* Confine the app to its own content well: an app that draws too far
       * must not be able to scribble over another window's chrome. The clip is
       * also what makes scrolling free -- the app paints its whole self at a
       * shifted origin and everything outside the well is discarded. */
      Rect full = inner;
      if (scrolls) {
        full.y = (int16_t)(inner.y - s_scroll[idx]);
        full.h = natural;
      }
      draw_set_clip(vis);
      a->paint(a->state, full);
    }

    if (scrolls) {
      Rect bar = R(inner.x + inner.w, inner.y, SCROLL_W, inner.h);
      int16_t th = (int16_t)((int32_t)inner.h * inner.h / natural);
      int16_t ty;
      if (th < 6) th = 6;
      ty = (int16_t)(inner.y + (int32_t)(inner.h - th) * s_scroll[idx] /
                     (natural - inner.h));
      draw_set_clip(rect_intersect(clip, bar));
      draw_rect(bar, C_TITLE_UN);
      draw_bevel(R(bar.x, ty, SCROLL_W, th), C_FACE, C_LIGHT, C_DARK);
    }
  }
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}

/* ---------------------------------------------------------- taskbar ----- */

/* Clip for the taskbar currently being painted, so it can be redrawn for just
 * the damaged sliver rather than in full. */
static Rect s_tb_clip;
static void tb_clip(Rect r) { draw_set_clip(rect_intersect(r, s_tb_clip)); }

static void paint_taskbar(Rect clip) {
  Rect bar = R(0, DESK_H, DISPLAY_W, TASKBAR_H);
  Rect start = R(2, DESK_H + 2, 34, TASKBAR_H - 4);
  int i;
  int16_t x;
  char clock[8];

  s_tb_clip = rect_intersect(clip, bar);
  if (rect_is_empty(s_tb_clip)) return;
  tb_clip(bar);
  draw_bevel(bar, C_FACE, C_LIGHT, C_SHADOW);

  draw_bevel(start, C_FACE, s_start_open ? C_SHADOW : C_LIGHT,
             s_start_open ? C_LIGHT : C_DARK);
  draw_text((int16_t)(start.x + 3), (int16_t)(start.y + 1), "Start", C_TEXT, C_FACE);

  /* One button per window, pressed-in for the focused one. */
  x = 40;
  for (i = 0; i < s_nwin; i++) {
    Rect b = R(x, DESK_H + 2, 48, TASKBAR_H - 4);
    int focused = (!s_minimised[i] && wm_focus() == s_win[i]);
    if (b.x + b.w > DISPLAY_W - 32) break;
    draw_bevel(b, C_FACE, focused ? C_SHADOW : C_LIGHT,
               focused ? C_LIGHT : C_DARK);
    tb_clip(rect_inset(b, 2));
    /* A minimised window has no title bar to read a name off, so the button
     * uses the app's own -- which is where the window's title came from. */
    draw_text_ellipsis((int16_t)(b.x + 2), (int16_t)(b.y + 1), 42,
                       s_minimised[i] ? s_app[i]->name : wm_title(s_win[i]),
                       C_TEXT, C_FACE);
    tb_clip(bar);
    x = (int16_t)(x + 50);
  }

  /* Clock, sunken, at the right. Uptime rather than time of day: there is no
   * RTC on this board, so pretending to know the time would be a lie. */
  {
    Rect c = R(DISPLAY_W - 30, DESK_H + 2, 28, TASKBAR_H - 4);
    unsigned mins = (unsigned)(s_now_ms / 60000u);
    unsigned secs = (unsigned)((s_now_ms / 1000u) % 60u);
    draw_bevel(c, C_FACE, C_SHADOW, C_LIGHT);
    snprintf(clock, sizeof clock, "%u:%02u", mins % 100u, secs);
    draw_text((int16_t)(c.x + 2), (int16_t)(c.y + 1), clock, C_TEXT, C_FACE);
  }
}

/* The menu is drawn on top of the compositor's output rather than through it,
 * so it needs its own dirty flag: nothing else knows the region exists. */
static Rect s_menu_rect;
static int  s_menu_dirty;
static int  s_menu_hit;      /* something repainted underneath it this pass */

static int menu_items(void) { return app_count() + 1; }   /* apps, then Console */

static Rect menu_rect(void) {
  int items = menu_items();
  int h = items * 11 + 6;
  return R(2, DESK_H - h, 84, h);
}

static void menu_touch(void) {
  s_menu_rect = menu_rect();
  s_menu_dirty = 1;
}

/* Closing has to go through the compositor: whatever the menu was covering
 * must be repainted, and only the window system knows what that was. */
static void menu_close(void) {
  if (!s_start_open) return;
  s_start_open = 0;
  wm_damage(menu_rect());
}

static void paint_start_menu(void) {
  Rect m = menu_rect();
  int items = menu_items();
  int i;

  s_menu_rect = m;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_bevel(m, C_FACE, C_LIGHT, C_DARK);

  for (i = 0; i < items; i++) {
    Rect item = R(m.x + 3, m.y + 3 + i * 11, m.w - 6, 10);
    int sel = (i == s_start_sel);
    const char *label = (i < app_count()) ? app_at(i)->name : "Console";
    draw_rect(item, sel ? C_TITLE : C_FACE);
    draw_text((int16_t)(item.x + 2), (int16_t)(item.y + 1), label,
              sel ? C_TITLE_FG : C_TEXT, sel ? C_TITLE : C_FACE);
  }
}

/* Which item the pointer is over, or -1. */
static int menu_item_at(int16_t x, int16_t y) {
  Rect m = menu_rect();
  int i;
  if (!rect_contains(m, x, y)) return -1;
  i = (y - (m.y + 3)) / 11;
  if (i < 0 || i >= menu_items()) return -1;
  return i;
}

static void draw_pointer(void);
static int  s_kbd_mouse;
static int  s_dragging;
static WinId s_drag_win;

/* ------------------------------------------------------------ paint ----- */

static void paint_job(void *ctx, WinId w, Rect r) {
  (void)ctx;
  if (s_start_open && rect_overlaps(r, s_menu_rect)) s_menu_hit = 1;
  if (w == WIN_NONE) {
    draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
    draw_rect(rect_intersect(r, R(0, 0, DISPLAY_W, DESK_H)), C_DESKTOP);
    paint_icons(rect_intersect(r, R(0, 0, DISPLAY_W, DESK_H)));
    /* The taskbar is part of the background, repainted only where damaged.
     * Repainting it on every flush was redrawing 240x13 pixels of bevels and
     * text for a nine-pixel pointer move -- which is what flickered. */
    if (r.y + r.h > DESK_H) paint_taskbar(r);
  } else {
    paint_window(w, r);
  }
}

/* A fullscreen app gets the panel and nothing else: no desktop, no chrome, no
 * taskbar, no pointer. The compositor is not involved, so there is no damage
 * to merge and no window to clip against -- the render loop is one call into
 * the app's paint with the screen as its rectangle. */
static void paint_fullscreen(void) {
  int cleared;
  if (!s_full_dirty) return;
  s_full_dirty = 0;
  cleared = s_full_clear;

  /* The desktop is still on the panel when an app takes it over, and an app
   * that does not cover every pixel would otherwise be drawn on top of it.
   * Once on entry, not per frame: per frame would flicker. */
  if (s_full_clear) {
    s_full_clear = 0;
    draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
    draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_DESKTOP);
    draw_rect(s_full_rect, C_WHITE);
    if (s_full_rect.w < DISPLAY_W || s_full_rect.h < DISPLAY_H)
      draw_frame(rect_inset(s_full_rect, -1), C_SHADOW);
  }

  /* Narrowed to what the app says changed, when it says. The same mechanism
   * the launcher uses; see AppDef.take_damage. */
  {
    Rect area = s_full_rect, want;
    if (!cleared && s_full->take_damage &&
        s_full->take_damage(s_full->state, &want)) {
      Rect vis = rect_intersect(want, s_full_rect);
      if (!rect_is_empty(vis)) area = vis;
    }
    draw_set_clip(area);
  }
  if (s_full->paint) s_full->paint(s_full->state, s_full_rect);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}

void desktop_flush(void) {
  if (s_full) { paint_fullscreen(); return; }
  if (wm_damage_count() == 0 && !s_menu_dirty) return;

  s_menu_hit = 0;
  if (wm_damage_count()) wm_paint(paint_job, NULL);

  /* Redrawn only when its own contents changed, or when the compositor
   * repainted something underneath it. Redrawing it on every flush was
   * 84x100 pixels of bevels and text for a one-row selection change, which is
   * what flickered. */
  if (s_start_open && (s_menu_dirty || s_menu_hit)) paint_start_menu();
  s_menu_dirty = 0;

  draw_pointer();      /* always last: the pointer is above everything */
}

void desktop_repaint(void) {
  if (s_full) { s_full_dirty = 1; desktop_flush(); return; }
  wm_damage(R(0, 0, DISPLAY_W, DISPLAY_H));
  desktop_flush();
}

/* Leaving fullscreen throws away nothing: the window system's state was never
 * touched, so the desktop comes back exactly as it was. */
static void open_def_ex(const AppDef *a, int fresh);

static void leave_fullscreen(void) {
  const AppDef *a = s_full;
  if (!a) return;
  s_full = NULL;
  /* Back into a window, not into nothing. This used to just drop the app: the
   * screen came back to the desktop and whatever had been running was no
   * longer anywhere, because a fullscreen app has no window to return to
   * unless one is made. Escape means "stop filling the screen", and it keeps
   * the app's state -- fresh = 0 -- for the same reason maximise does. */
  open_def_ex(a, 0);
  desktop_repaint();
}

/* ------------------------------------------------------------ input ----- */

static void open_def(const AppDef *a);
static void toggle_fullscreen(void);
static void open_def_ex(const AppDef *a, int fresh);

static void launch_icon(int i) {
  const Icon *ic = icon_at(i);
  const AppDef *a = NULL;

  if (!ic) return;
  if (ic->kind == ICON_FIRMWARE) { icons_boot_firmware(i); return; }
  if (ic->kind == ICON_FOLDER) return;    /* the desktop is flat; the launcher has folders */

  if (ic->kind == ICON_CAPP) {
    /* Running the program *is* opening it: capp_main builds whatever state it
     * has and installs an interface if it wants one. There is no AppDef before
     * that -- capprun_def returns NULL until the program has run -- which is
     * why asking for one first made every loadable icon do nothing at all,
     * from the keyboard and from a double click alike.
     *
     * No arguments: the only path an icon has is the program's own binary, and
     * handing Edit its own .capp made it open 14 KB of ELF as text. */
    capprun_start(ic->slot, ic->name, NULL);
    if (!capprun_is_app(ic->slot)) return;    /* a command, already finished */
    a = capprun_def(ic->slot);
  } else {
    a = icon_app(i);
    if (a && a->open) a->open(a->state);
  }
  if (!a) return;

  if (ic->kind == ICON_CAPP && capprun_fullscreen(ic->slot)) {
    int16_t w = DISPLAY_W, h = DISPLAY_H;
    if (a->pref_w > 0 && a->pref_w < w) w = a->pref_w;
    if (a->pref_h > 0 && a->pref_h < h) h = a->pref_h;
    s_full = a;
    s_full_rect = R((DISPLAY_W - w) / 2, (DISPLAY_H - h) / 2, w, h);
    s_full_clear = 1;
    s_full_dirty = 1;
    desktop_flush();
    return;
  }

  /* open_def_ex(a, 0): capp_main already did the opening, and a built-in had
   * its open called above. Calling it again here would reset the app the
   * moment its window appeared. */
  open_def_ex(a, 0);
  desktop_repaint();
}

static void launch_selected(void) {
  if (s_sel_icon >= 0 && s_sel_icon < icons_count()) launch_icon(s_sel_icon);
}

void desktop_icon_click(int16_t x, int16_t y) {
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  int i, hit = -1;

  for (i = 0; i < icons_count(); i++)
    if (rect_contains(icon_rect(i), x, y)) { hit = i; break; }

  if (hit < 0) {
    /* Clicking bare desktop gives the keyboard back to the icons without
     * clearing the selection: something has to stay selected for an arrow key
     * to mean anything. */
    if (!s_icon_focus) { s_icon_focus = 1; desktop_repaint(); }
    return;
  }
  s_icon_focus = 1;

  /* Two clicks on the same icon within half a second is a double click. */
  if (hit == s_last_icon && now - s_last_click_ms < 500) {
    s_last_icon = -1;
    launch_icon(hit);
    return;
  }
  s_last_icon = hit;
  s_last_click_ms = now;
  s_sel_icon = hit;
  desktop_repaint();
}

/* `fresh` calls the app's open callback, which resets it. A window being
 * created because the user asked for one does; a window being created because
 * an app came out of fullscreen does not -- that would throw away whatever
 * they were in the middle of, which is the opposite of what a toggle means. */
static void open_def_ex(const AppDef *a, int fresh) {
  Rect frame;
  WinId w;
  if (!a || s_nwin >= MAX_OPEN) return;
  /* Cascade, so a new window is visibly on top rather than exactly covering
   * the last one. An app with a preferred content size gets a frame built
   * around it; the chrome is a border and title bar on top, and the content
   * well costs two pixels a side. */
  frame = R(8 + s_nwin * 14, 6 + s_nwin * 10, 132, 74);
  if (a->pref_w > 0 && a->pref_h > 0) {
    frame.w = (int16_t)(a->pref_w + 2 * WM_BORDER + 6);
    frame.h = (int16_t)(a->pref_h + 2 * WM_BORDER + WM_TITLE_H + 6);
    if (frame.w > DISPLAY_W) frame.w = DISPLAY_W;
    if (frame.h > DESK_H) frame.h = DESK_H;
    if (frame.x + frame.w > DISPLAY_W) frame.x = (int16_t)(DISPLAY_W - frame.w);
    if (frame.y + frame.h > DESK_H) frame.y = (int16_t)(DESK_H - frame.h);
    if (frame.x < 0) frame.x = 0;
    if (frame.y < 0) frame.y = 0;
  }
  w = wm_create(a->name, frame);
  if (w == WIN_NONE) return;
  if (fresh && a->open) a->open(a->state);
  s_win[s_nwin] = w;
  s_app[s_nwin] = a;
  s_minimised[s_nwin] = 0;
  s_scroll[s_nwin] = 0;
  s_nwin++;
  s_icon_focus = 0;            /* the new window has the keyboard */
}

static void close_focused(void);

/* Put a window down on the taskbar. Its slot stays in the list -- that is what
 * the taskbar draws from -- with the frame remembered so it comes back where
 * it was rather than at the top of the cascade. */
static void minimise(WinId w) {
  int i = win_index(w);
  if (i < 0) return;
  s_min_frame[i] = wm_frame(w);
  s_minimised[i] = 1;
  s_win[i] = WIN_NONE;
  wm_destroy(w);
  if (wm_count() == 0) s_icon_focus = 1;
  desktop_repaint();
}

/* And pick it back up. fresh = 0: the app has been sitting there with its
 * state the whole time, and calling open() would throw it away. */
static void unminimise(int i) {
  WinId w;
  if (i < 0 || i >= s_nwin || !s_minimised[i]) return;
  w = wm_create(s_app[i]->name, s_min_frame[i]);
  if (w == WIN_NONE) return;
  s_win[i] = w;
  s_minimised[i] = 0;
  s_icon_focus = 0;
  wm_raise(w);
  desktop_repaint();
}

static void open_def(const AppDef *a) { open_def_ex(a, 1); }
static void open_app(int k) { open_def(app_at(k)); }

/* Whether an app fills the screen is the user's call, not the app's. The
 * fullscreen flag a .capp carries is the default it opens with; this is how it
 * gets changed, and it keeps the app's state either way. */
static void toggle_fullscreen(void) {
  const AppDef *a;

  if (s_full) {
    a = s_full;
    s_full = NULL;
    open_def_ex(a, 0);
    desktop_repaint();
    return;
  }

  {
    WinId f = wm_focus();
    int16_t w = DISPLAY_W, h = DISPLAY_H;
    if (f == WIN_NONE) return;
    a = app_of(f);
    close_focused();
    if (a->pref_w > 0 && a->pref_w < w) w = a->pref_w;
    if (a->pref_h > 0 && a->pref_h < h) h = a->pref_h;
    s_full = a;
    s_full_rect = R((DISPLAY_W - w) / 2, (DISPLAY_H - h) / 2, w, h);
    s_full_clear = 1;
    s_full_dirty = 1;
    desktop_flush();
  }
}

static void close_focused(void) {
  WinId f = wm_focus();
  int i, j;
  if (f == WIN_NONE) return;
  wm_destroy(f);
  for (i = 0; i < s_nwin; i++) {
    if (s_win[i] != f) continue;
    for (j = i; j < s_nwin - 1; j++) {
      s_win[j] = s_win[j + 1];
      s_app[j] = s_app[j + 1];
      s_scroll[j] = s_scroll[j + 1];
      s_minimised[j] = s_minimised[j + 1];
      s_min_frame[j] = s_min_frame[j + 1];
    }
    s_nwin--;
    break;
  }
  if (s_nwin == 0) s_icon_focus = 1;
}

/* Tab walks the windows and then the desktop, and round again. The desktop is
 * a focus target like any other here: with a window open there would otherwise
 * be no way back to the icons without reaching for the mouse. */
static void cycle_focus(void) {
  WinId f = wm_focus();
  int i;

  if (s_nwin == 0) { s_icon_focus = 1; return; }

  if (s_icon_focus) {              /* desktop -> the bottom window */
    s_icon_focus = 0;
    wm_raise(s_win[0]);
    desktop_repaint();
    return;
  }

  for (i = 0; i < s_nwin; i++) {
    if (s_win[i] != f) continue;
    if (i + 1 < s_nwin) wm_raise(s_win[i + 1]);
    else s_icon_focus = 1;         /* the last window -> the desktop */
    desktop_repaint();
    return;
  }
  s_icon_focus = 1;
  desktop_repaint();
}

static void nudge(int16_t dx, int16_t dy) {
  WinId f = wm_focus();
  Rect r;
  if (f == WIN_NONE) return;
  r = wm_frame(f);
  wm_move(f, (int16_t)(r.x + dx), (int16_t)(r.y + dy));
}

int desktop_key(uint8_t key) {
  if (s_help) {
    s_help = 0;
    desktop_repaint();
    return 0;
  }
  if (key == KEY_HELP) {
    const AppDef *a = s_full ? s_full
                    : (wm_focus() != WIN_NONE ? app_of(wm_focus()) : NULL);
    s_help = 1;
    help_paint(a ? a->name : "Desktop", a ? a->help : NULL,
               s_full
               ? "escape\tleave fullscreen\nctrl-f\tleave fullscreen\nctrl-h\tclose this\n"
               : "arrows\tmove between icons\nenter\topen the selected icon\ntab\twindows, then the desktop\nctrl-s\tstart menu\nctrl-f\tfullscreen / window\nctrl-w\tclose window\nescape\tthe console\nctrl-h\tclose this\n");
    return 0;
  }

  /* ; . , / stand in for the arrow cluster unless something is taking text.
   * Same rule as the launcher, so a key does the same thing in both shells. */
  {
    const AppDef *focused = s_full ? s_full
                          : (wm_focus() != WIN_NONE ? app_of(wm_focus()) : NULL);
    if (!focused || !focused->wants_text || !focused->wants_text(focused->state)) {
      uint8_t arrow = keyboard_arrow_for(key);
      if (arrow) key = arrow;
    }
  }

  /* A fullscreen app has the keyboard as well as the panel. Escape is the one
   * key it does not get, because something has to bring the desktop back. */
  if (s_full) {
    if (key == KEY_ESC) { leave_fullscreen(); return 0; }
    if (key == 0x06) { toggle_fullscreen(); return 0; }      /* ctrl-F */
    if (s_full->key && s_full->key(s_full->state, key)) {
      s_full_dirty = 1;
      desktop_flush();
    }
    return 0;
  }

  if (s_start_open) {
    switch (key) {
    case KEY_UP:   s_start_sel = (s_start_sel + menu_items() - 1) % menu_items(); break;
    case KEY_DOWN: s_start_sel = (s_start_sel + 1) % menu_items(); break;
    case KEY_ENTER:
      menu_close();
      if (s_start_sel >= app_count()) {
        desktop_set_autostart(0);   /* leaving on purpose: stay at the console */
        return 1;
      }
      open_app(s_start_sel);
      desktop_flush();
      return 0;
    case KEY_ESC: menu_close(); desktop_flush(); return 0;
    default: break;
    }
    menu_touch();
    desktop_flush();
    return 0;
  }

  /* Escape always works, whatever has focus: it is the one way back to the
   * console and must never be something an app can swallow. */
  if (key == KEY_ESC) { desktop_set_autostart(0); return 1; }

  /* The keyboard-driven pointer is an explicit mode, so while it is on the
   * arrows belong to it rather than to the focused app. */
  if (s_kbd_mouse) {
    MouseReport r;
    memset(&r, 0, sizeof r);
    r.buttons = s_kbd_btn ? MOUSE_LEFT : 0;
    switch (key) {
    case KEY_LEFT:  r.dx = -4; desktop_mouse(&r); return 0;
    case KEY_RIGHT: r.dx =  4; desktop_mouse(&r); return 0;
    case KEY_UP:    r.dy = -4; desktop_mouse(&r); return 0;
    case KEY_DOWN:  r.dy =  4; desktop_mouse(&r); return 0;
    case ' ':
      /* Space latches the button rather than clicking, so a drag is
       * possible: press over a title bar, steer, press again to drop. */
      s_kbd_btn = !s_kbd_btn;
      r.buttons = s_kbd_btn ? MOUSE_LEFT : 0;
      desktop_mouse(&r);
      return 0;
    default: break;
    }
  }

  /* The icons have the keyboard when no window does. Arrows move the
   * selection, enter or space opens it -- the same two gestures the carousel
   * uses, so the two shells do not need learning separately. */
  if (s_icon_focus || wm_focus() == WIN_NONE) {
    switch (key) {
    case KEY_LEFT:  select_icon(-1); desktop_flush(); return 0;
    case KEY_RIGHT: select_icon(1);  desktop_flush(); return 0;
    case KEY_UP:    select_icon(-icons_per_row()); desktop_flush(); return 0;
    case KEY_DOWN:  select_icon(icons_per_row());  desktop_flush(); return 0;
    case KEY_ENTER:
    case ' ':       launch_selected(); return 0;
    default: break;
    }
  }

  /* The focused app gets first refusal on everything else, ctrl-chords
   * included. Reserving chords for the shell meant an editor could not have
   * ctrl-S -- the Start menu took it first -- and reserving the arrows meant
   * no app could have a cursor at all. An app that wants a key has a better
   * claim on it than the desktop does, and Escape above is the way out of an
   * app that wants them all. */
  {
    WinId f = wm_focus();
    if (f != WIN_NONE) {
      const AppDef *a = app_of(f);
      if (a->key && a->key(a->state, key)) {
        wm_damage(wm_frame(f));
        desktop_flush();
        return 0;
      }
    }
  }

  switch (key) {
  case 0x10: desktop_set_kbd_mouse(!s_kbd_mouse); return 0;                  /* ctrl-P */
  case 0x13: s_start_open = 1; s_start_sel = 0; menu_touch();                 /* ctrl-S */
             desktop_flush(); return 0;
  case 0x17: close_focused(); desktop_repaint(); return 0;                   /* ctrl-W */
  case 0x06: toggle_fullscreen(); return 0;                                  /* ctrl-F */
  case '	':      cycle_focus();   desktop_flush(); return 0;
  case KEY_LEFT:  nudge(-6, 0);    desktop_flush(); return 0;
  case KEY_RIGHT: nudge(6, 0);     desktop_flush(); return 0;
  case KEY_UP:    nudge(0, -5);    desktop_flush(); return 0;
  case KEY_DOWN:  nudge(0, 5);     desktop_flush(); return 0;
  default: break;
  }

  desktop_flush();
  return 0;
}

/* Is the focused app taking text? Fullscreen app, focused window, or nothing
 * -- the same order the keyboard already resolves. */
int desktop_wants_text(void) {
  const AppDef *a = s_full ? s_full
                  : (wm_focus() != WIN_NONE ? app_of(wm_focus()) : NULL);
  if (!a || !a->wants_text) return 0;
  return a->wants_text(a->state);
}

void desktop_tick(uint32_t ms) {
  uint32_t before = s_now_ms / 1000u;
  s_now_ms = ms;

  /* Animating apps first, and every pass rather than every second. A
   * fullscreen app is asked directly; a windowed one is asked through the
   * window it lives in, so its damage goes through the window system and
   * whatever is stacked above it stays on top. */
  if (s_full && s_full->tick) {
    if (s_full->tick(s_full->state, ms)) {
      draw_set_clip(s_full_rect);
      s_full->paint(s_full->state, s_full_rect);
      draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
    }
  } else {
    int i, any = 0;
    for (i = 0; i < s_nwin; i++) {
      const AppDef *a = s_app[i];
      if (!a || !a->tick) continue;
      /* A minimised app keeps running -- a game should not pause because its
       * window is down on the taskbar -- but it has nowhere to draw, so its
       * request for a repaint is answered by doing nothing. */
      if (a->tick(a->state, ms) && !s_minimised[i]) {
        Rect want;
        /* The app's own rectangle if it named one -- through the compositor,
         * so whatever is stacked above it still covers it. A tick that moves
         * a ball should not repaint the window it is in. */
        if (a->take_damage && a->take_damage(a->state, &want))
          wm_damage(rect_intersect(want, wm_content(s_win[i])));
        else
          wm_damage(wm_content(s_win[i]));
        any = 1;
      }
    }
    if (any) desktop_flush();
  }

  if (s_now_ms / 1000u == before) return;

  /* Nothing of ours is on screen while an app owns it -- not the clock, and
   * not a repaint that would trample the app's picture. */
  if (s_full) return;

  /* A mouse that wanders out of range or sleeps drops the link. Look for it
   * again rather than sitting there with a dead pointer -- but not every
   * second, because each attempt is a six-second scan. */
  if (bthid_state(BTHID_MOUSE) == BTH_FAILED && (s_now_ms / 1000u) % 15 == 0)
    bthid_start(4, BTHID_MOUSE);
  wm_damage(R(DISPLAY_W - 30, DESK_H + 2, 28, TASKBAR_H - 4));
  desktop_flush();
}

#define NVS_NS        "cardos"
#define NVS_AUTOSTART "autodesk"

/* Bring a band of the focused window's content into view. An app calls this
 * from its key handler when it moves a selection that the window is scrolling:
 * the window system knows where the viewport is, and the app knows where the
 * selection went, and neither can work it out alone. */
int desktop_take_leave(void) {
  int v = s_leave_for_console;
  s_leave_for_console = 0;
  return v;
}

void desktop_scroll_into_view(int16_t y, int16_t h) {
  int idx = win_index(wm_focus());
  Rect inner;
  const AppDef *a;

  if (idx < 0) return;
  a = s_app[idx];
  inner = rect_inset(wm_content(s_win[idx]), 2);
  if (content_height(a, inner) <= inner.h) return;
  inner.w = (int16_t)(inner.w - SCROLL_W);

  if (y < s_scroll[idx]) s_scroll[idx] = y;
  else if (y + h > s_scroll[idx] + inner.h)
    s_scroll[idx] = (int16_t)(y + h - inner.h);
  clamp_scroll(idx, a, inner);
}

void desktop_set_autostart(int on) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, NVS_AUTOSTART, (uint8_t)(on ? 1 : 0));
  nvs_commit(h);
  nvs_close(h);
}

int desktop_autostart(void) {
  nvs_handle_t h;
  uint8_t v = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
  if (nvs_get_u8(h, NVS_AUTOSTART, &v) != ESP_OK) v = 0;
  nvs_close(h);
  return v ? 1 : 0;
}

void desktop_init(void) {
  int i;
  ui_set_shell(UI_DESKTOP);
  wm_init(DISPLAY_W, DISPLAY_H);
  mouse_init(DISPLAY_W, DISPLAY_H);
  s_nwin = 0;
  s_start_open = 0;
  s_start_sel = 0;
  s_full = NULL;
  s_icon_focus = 1;
  s_sel_icon = 0;
  for (i = 0; i < MAX_OPEN; i++) {
    s_win[i] = WIN_NONE; s_app[i] = NULL; s_minimised[i] = 0;
  }

  desktop_reload_icons();

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_DESKTOP);
  desktop_repaint();
}

/* ------------------------------------------------------------- mouse ---- */

static int s_cursor_on;
static int16_t s_drag_dx, s_drag_dy;   /* pointer offset within the frame */

static Rect cursor_rect(void) {
  return draw_cursor_bounds((int16_t)mouse_x(), (int16_t)mouse_y());
}

static void draw_pointer(void) {
  if (!s_cursor_on) return;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_cursor((int16_t)mouse_x(), (int16_t)mouse_y());
}

int desktop_kbd_mouse(void) { return s_kbd_mouse; }

void desktop_set_kbd_mouse(int on) {
  s_kbd_mouse = on;
  s_cursor_on = on;
  /* Through the shell, not straight to desktop_repaint: Settings can be open
   * under the launcher, and painting a desktop over it would be the only
   * visible effect. */
  ui_repaint();
}

void desktop_mouse(const MouseReport *r) {
  desktop_mouse_apply(r);
  desktop_mouse_done();
}

void desktop_mouse_done(void) {
  desktop_flush();        /* draws the pointer last, on top of everything */
}

void desktop_mouse_apply(const MouseReport *r) {
  Rect before = cursor_rect();
  int pressed, released;
  WinId hit;

  mouse_apply(r);

  /* A fullscreen app gets the pointer too. Erasing it is the part that used to
   * stop this: the panel is three-wire with no MISO, so what was underneath
   * cannot be read back. The app can redraw it, though -- that is what paint
   * is -- so the old cursor square is handed back to the app as a clip, and
   * the cursor is drawn again on top. Every app therefore has a mouse without
   * knowing anything about one. */
  if (s_full) {
    int held = (mouse_pressed(MOUSE_LEFT) || mouse_down(MOUSE_LEFT)
                ? CAPP_BTN_LEFT : 0)
             | (mouse_pressed(MOUSE_RIGHT) || mouse_down(MOUSE_RIGHT)
                ? CAPP_BTN_RIGHT : 0);
    int btn = mouse_pressed(MOUSE_LEFT) ? MOUSE_LEFT
            : mouse_pressed(MOUSE_RIGHT) ? MOUSE_RIGHT : 0;
    int wheel = mouse_take_wheel();
    int moved = mouse_take_moved();
    int16_t lx = (int16_t)(mouse_x() - s_full_rect.x);
    int16_t ly = (int16_t)(mouse_y() - s_full_rect.y);
    int repaint = 0;

    s_cursor_on = 1;

    if (btn && s_full->click &&
        rect_contains(s_full_rect, (int16_t)mouse_x(), (int16_t)mouse_y()) &&
        s_full->click(s_full->state, lx, ly, btn))
      repaint = 1;

    if ((moved || wheel || held) && s_full->mouse &&
        s_full->mouse(s_full->state, lx, ly, held, wheel))
      repaint = 1;

    if (repaint) {
      s_full_dirty = 1;
      desktop_flush();
    } else if (moved) {
      /* Just the two squares the pointer left and arrived in. */
      Rect after = cursor_rect();
      Rect damage = rect_union(before, after);
      draw_set_clip(rect_intersect(damage, s_full_rect));
      if (s_full->paint) s_full->paint(s_full->state, s_full_rect);
      draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
      draw_pointer();
    }
    mouse_released(MOUSE_LEFT);
    mouse_released(MOUSE_RIGHT);
    return;
  }

  s_cursor_on = 1;

  /* The wheel scrolls whatever has focus, which is the only thing it could
   * usefully mean on a machine with one pointer. */
  {
    int w = mouse_take_wheel();
    int idx = win_index(wm_focus());
    if (w && idx >= 0) {
      const AppDef *a = s_app[idx];
      Rect inner = rect_inset(wm_content(s_win[idx]), 2);
      int taken = 0;
      inner.w = (int16_t)(inner.w - SCROLL_W);
      /* The app first. One that scrolls itself -- a page viewer, an editor --
       * takes the notch and says so; anything else leaves it to the window,
       * which is what a list too tall for its frame wants. */
      if (a->mouse && a->mouse(a->state, (int16_t)(mouse_x() - inner.x),
                               (int16_t)(mouse_y() - inner.y), 0, w))
        taken = 1;
      if (!taken) {
        s_scroll[idx] = (int16_t)(s_scroll[idx] - w * 12);
        clamp_scroll(idx, a, inner);
      }
      wm_damage(wm_frame(s_win[idx]));
    }
  }

  /* Consume both edges every time, whether or not they are used here.
   * Reading the release only while dragging left stale edges behind, so the
   * next drag saw a release from some earlier click and ended after a single
   * move -- which is exactly what a drag that "sometimes works" looks like. */
  pressed = mouse_pressed(MOUSE_LEFT);
  released = mouse_released(MOUSE_LEFT);

  /* The right button only ever means "the app's other gesture" -- there is no
   * context menu to own it -- so it goes straight to whatever is under the
   * pointer and skips focus, dragging and the chrome entirely. */
  if (mouse_pressed(MOUSE_RIGHT)) {
    WinId rw = wm_at((int16_t)mouse_x(), (int16_t)mouse_y());
    if (rw != WIN_NONE &&
        wm_hit_test(rw, (int16_t)mouse_x(), (int16_t)mouse_y()) == WM_HIT_CONTENT) {
      const AppDef *a = app_of(rw);
      int idx = win_index(rw);
      Rect c = rect_inset(wm_content(rw), 2);
      int16_t natural = content_height(a, c);
      int16_t ly = (int16_t)(mouse_y() - c.y);
      if (natural > c.h && idx >= 0) ly = (int16_t)(ly + s_scroll[idx]);
      if (a->click &&
          a->click(a->state, (int16_t)(mouse_x() - c.x), ly, MOUSE_RIGHT))
        wm_damage(wm_frame(rw));
    }
  }
  mouse_released(MOUSE_RIGHT);

  /* The panel cannot be read back -- three-wire, no MISO -- so there is no
   * saving the pixels under the pointer. Moving it is two damage rectangles,
   * which is what the compositor is already for. */
  if (mouse_take_moved()) {
    wm_damage(before);
    wm_damage(cursor_rect());
  }

  if (s_dragging) {
    if (wm_valid(s_drag_win)) {
      int16_t nx = (int16_t)(mouse_x() - s_drag_dx);
      int16_t ny = (int16_t)(mouse_y() - s_drag_dy);
      Rect f = wm_frame(s_drag_win);
      /* Keep the title bar on screen: a window dragged fully off can never be
       * grabbed again. */
      if (nx < (int16_t)(8 - f.w)) nx = (int16_t)(8 - f.w);
      if (nx > DISPLAY_W - 8) nx = DISPLAY_W - 8;
      if (ny < 0) ny = 0;
      if (ny > DESK_H - WM_TITLE_H) ny = (int16_t)(DESK_H - WM_TITLE_H);
      wm_move(s_drag_win, nx, ny);
    } else {
      s_dragging = 0;
    }
    if (released) s_dragging = 0;
    return;
  }

  if (!pressed) return;

  /* An open menu is above every window, so it gets the click first. */
  if (s_start_open) {
    int item = menu_item_at((int16_t)mouse_x(), (int16_t)mouse_y());
    if (item >= 0) {
      s_start_sel = item;
      menu_close();
      if (item >= app_count()) { s_leave_for_console = 1; return; }
      open_app(item);
      return;
    }
  }

  hit = wm_at((int16_t)mouse_x(), (int16_t)mouse_y());
  if (hit == WIN_NONE) {
    if (mouse_y() >= DESK_H) {
      if (mouse_x() < 38) {                 /* Start button */
        if (s_start_open) {
          menu_close();
        } else {
          s_start_open = 1;
          s_start_sel = 0;
          menu_touch();
        }
        wm_damage(R(0, DESK_H, 40, TASKBAR_H));
      } else {
        int i;                              /* a taskbar button raises it */
        int16_t x = 40;
        for (i = 0; i < s_nwin; i++, x = (int16_t)(x + 50)) {
          if (mouse_x() >= x && mouse_x() < x + 48) {
            if (s_minimised[i]) unminimise(i);
            else wm_raise(s_win[i]);
            break;
          }
        }
      }
      return;
    }
    /* Bare desktop: dismiss the menu, and select or launch an icon. */
    if (s_start_open) { menu_close(); return; }
    desktop_icon_click((int16_t)mouse_x(), (int16_t)mouse_y());
    return;
  }

  {
    WmHit what = wm_hit_test(hit, (int16_t)mouse_x(), (int16_t)mouse_y());
    wm_raise(hit);
    if (s_start_open) { menu_close(); return; }
    if (what == WM_HIT_CLOSE) {
      close_focused();
      desktop_repaint();
      return;
    }
    if (what == WM_HIT_MIN) {
      minimise(hit);
      return;
    }
    if (what == WM_HIT_MAX) {
      /* The same thing ctrl-F does, and it keeps the app's state either way:
       * the window system's records are not thrown away by filling the
       * screen, so coming back finds the window where it was. */
      toggle_fullscreen();
      return;
    }
    if (what == WM_HIT_TITLE || what == WM_HIT_BORDER) {
      Rect f = wm_frame(hit);
      s_dragging = 1;
      s_drag_win = hit;
      s_drag_dx = (int16_t)(mouse_x() - f.x);
      s_drag_dy = (int16_t)(mouse_y() - f.y);
      return;
    }
    if (what == WM_HIT_CONTENT) {
      const AppDef *a = app_of(hit);
      int idx = win_index(hit);
      Rect c = rect_inset(wm_content(hit), 2);
      int16_t natural = content_height(a, c);
      int scrolls = (natural > c.h) && idx >= 0;
      int16_t lx = (int16_t)(mouse_x() - c.x);
      int16_t ly = (int16_t)(mouse_y() - c.y);

      if (scrolls && lx >= c.w - SCROLL_W) {
        /* On the bar: jump so the clicked fraction of the track becomes the
         * same fraction of the content. */
        s_scroll[idx] = (int16_t)((int32_t)ly * (natural - c.h) / c.h);
        clamp_scroll(idx, a, R(c.x, c.y, c.w - SCROLL_W, c.h));
        wm_damage(wm_frame(hit));
        return;
      }
      if (scrolls) ly = (int16_t)(ly + s_scroll[idx]);
      if (a->click && a->click(a->state, lx, ly, MOUSE_LEFT))
        wm_damage(wm_frame(hit));
    }
  }
}
