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
#include "kernel/drv/bthid.h"
#include "esp_timer.h"
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

/* ------------------------------------------------------------- icons ---- */

#define ICON_W     46
#define ICON_H     34
#define ICON_BOX   16

static int  s_sel_icon = -1;
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
  s_sel_icon = -1;
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

    if (i == s_sel_icon) draw_frame(r, C_TITLE_FG);
    draw_text_ellipsis(r.x, (int16_t)(r.y + ICON_BOX + 2), r.w,
                       ic->name, C_TITLE_FG, C_DESKTOP);
    draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  }
}

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
                       (int16_t)(title.w - WM_CLOSE_W - 4), wm_title(w),
                       C_TITLE_FG, focused ? C_TITLE : C_TITLE_UN);
    /* Close box, raised, with an x. */
    close.x = (int16_t)(title.x + title.w - WM_CLOSE_W);
    close.y = (int16_t)(title.y);
    close.w = WM_CLOSE_W;
    close.h = (int16_t)(WM_TITLE_H - 2);
    draw_bevel(close, C_FACE, C_LIGHT, C_DARK);
    draw_text((int16_t)(close.x + 1), (int16_t)(close.y - 1), "x", C_DARK, C_FACE);
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
    int focused = (wm_focus() == s_win[i]);
    if (b.x + b.w > DISPLAY_W - 32) break;
    draw_bevel(b, C_FACE, focused ? C_SHADOW : C_LIGHT,
               focused ? C_LIGHT : C_DARK);
    tb_clip(rect_inset(b, 2));
    draw_text_ellipsis((int16_t)(b.x + 2), (int16_t)(b.y + 1), 42,
                       wm_title(s_win[i]), C_TEXT, C_FACE);
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
  if (!s_full_dirty) return;
  s_full_dirty = 0;

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

  draw_set_clip(s_full_rect);
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
static void leave_fullscreen(void) {
  if (!s_full) return;
  s_full = NULL;
  desktop_repaint();
}

/* ------------------------------------------------------------ input ----- */

static void open_def(const AppDef *a);

static void launch_icon(int i) {
  const Icon *ic = icon_at(i);
  const AppDef *a;

  if (!ic) return;
  if (ic->kind == ICON_FIRMWARE) { icons_boot_firmware(i); return; }

  a = icon_app(i);
  if (!a) return;
  /* Deliberately not capprun_set_file(ic->path): that path is the app's own
   * binary, and handing Edit its own .capp made it open 14 KB of ELF as text.
   * set_file is for an icon that names a document, which nothing does yet. */

  if (ic->kind == ICON_CAPP && capprun_fullscreen(ic->slot)) {
    int16_t w = DISPLAY_W, h = DISPLAY_H;
    if (a->pref_w > 0 && a->pref_w < w) w = a->pref_w;
    if (a->pref_h > 0 && a->pref_h < h) h = a->pref_h;
    if (a->open) a->open(a->state);
    s_full = a;
    s_full_rect = R((DISPLAY_W - w) / 2, (DISPLAY_H - h) / 2, w, h);
    s_full_clear = 1;
    s_full_dirty = 1;
    desktop_flush();
    return;
  }
  open_def(a);
  desktop_repaint();
}

void desktop_icon_click(int16_t x, int16_t y) {
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  int i, hit = -1;

  for (i = 0; i < icons_count(); i++)
    if (rect_contains(icon_rect(i), x, y)) { hit = i; break; }

  if (hit < 0) {
    if (s_sel_icon >= 0) { s_sel_icon = -1; desktop_repaint(); }
    return;
  }

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

static void open_def(const AppDef *a) {
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
  if (a->open) a->open(a->state);
  s_win[s_nwin] = w;
  s_app[s_nwin] = a;
  s_scroll[s_nwin] = 0;
  s_nwin++;
}

static void open_app(int k) { open_def(app_at(k)); }

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
    }
    s_nwin--;
    break;
  }
}

static void cycle_focus(void) {
  WinId f = wm_focus();
  int i;
  if (s_nwin < 2) return;
  for (i = 0; i < s_nwin; i++) {
    if (s_win[i] == f) {
      wm_raise(s_win[(i + 1) % s_nwin]);
      return;
    }
  }
}

static void nudge(int16_t dx, int16_t dy) {
  WinId f = wm_focus();
  Rect r;
  if (f == WIN_NONE) return;
  r = wm_frame(f);
  wm_move(f, (int16_t)(r.x + dx), (int16_t)(r.y + dy));
}

int desktop_key(uint8_t key) {
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

void desktop_tick(uint32_t ms) {
  uint32_t before = s_now_ms / 1000u;
  s_now_ms = ms;
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
  for (i = 0; i < MAX_OPEN; i++) { s_win[i] = WIN_NONE; s_app[i] = NULL; }

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

  /* A fullscreen app sees clicks in screen coordinates and draws its own
   * pointer if it wants one. Ours would have to be erased by repainting the
   * area underneath, which only the app knows how to do. */
  if (s_full) {
    int btn = mouse_pressed(MOUSE_LEFT) ? MOUSE_LEFT
            : mouse_pressed(MOUSE_RIGHT) ? MOUSE_RIGHT : 0;
    s_cursor_on = 0;
    if (btn && s_full->click &&
        rect_contains(s_full_rect, (int16_t)mouse_x(), (int16_t)mouse_y()) &&
        s_full->click(s_full->state, (int16_t)(mouse_x() - s_full_rect.x),
                      (int16_t)(mouse_y() - s_full_rect.y), btn)) {
      s_full_dirty = 1;
      desktop_flush();
    }
    mouse_released(MOUSE_LEFT);
    mouse_released(MOUSE_RIGHT);
    mouse_take_moved();
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
      inner.w = (int16_t)(inner.w - SCROLL_W);
      s_scroll[idx] = (int16_t)(s_scroll[idx] - w * 12);
      clamp_scroll(idx, a, inner);
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
          if (mouse_x() >= x && mouse_x() < x + 48) { wm_raise(s_win[i]); break; }
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
