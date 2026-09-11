/* The desktop shell. See desktop.h. */

#include "kernel/ui/desktop.h"
#include "kernel/ui/draw.h"
#include "kernel/drv/keyboard.h"
#include "kernel/mem/mem.h"
#include "kernel/fs/fs.h"
#include "kernel/ui/app.h"
#include "kernel/app/launcher.h"
#include "kernel/drv/btmouse.h"
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
static int   s_app[MAX_OPEN];       /* index into the app registry */
static int   s_nwin;

/* ------------------------------------------------------------- icons ---- */

#define ICONS_DIR  "/desktop"
#define MAX_ICONS  8
#define ICON_W     46
#define ICON_H     34
#define ICON_BOX   16

typedef struct {
  char name[20];        /* shown under the icon */
  char path[80];        /* full path, for firmware */
  int  app;             /* app registry index, or -1 for firmware */
} Icon;

static Icon s_icon[MAX_ICONS];
static int  s_nicon;
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

static const AppDef *app_of(WinId w) {
  int i;
  for (i = 0; i < s_nwin; i++)
    if (s_win[i] == w) return app_at(s_app[i]);
  return app_at(0);
}

/* Seed the folder the first time, so a fresh card still has something to
 * click rather than an empty desktop with no clue what to do. */
static void seed_icons_dir(void) {
  static const char *seed[] = { "Files.app", "Edit.app", "Mines.app" };
  size_t i;
  char path[80];
  FsDir d;
  FsEntry e;
  int any = 0;

  if (fs_mkdir(ICONS_DIR) != 0) { /* already there, or no card */ }
  if (fs_opendir(ICONS_DIR, &d) != 0) return;
  while (fs_readdir(&d, &e) == 1) { any = 1; break; }
  fs_closedir(&d);
  if (any) return;

  for (i = 0; i < sizeof seed / sizeof seed[0]; i++) {
    int fd;
    snprintf(path, sizeof path, "%s/%s", ICONS_DIR, seed[i]);
    fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd >= 0) { fs_write(fd, "cardos app\n", 11); fs_close(fd); }
  }
}

void desktop_reload_icons(void) {
  FsDir d;
  FsEntry e;

  s_nicon = 0;
  s_sel_icon = -1;
  if (!fs_mounted()) return;

  seed_icons_dir();
  if (fs_opendir(ICONS_DIR, &d) != 0) return;

  while (s_nicon < MAX_ICONS && fs_readdir(&d, &e) == 1) {
    size_t n = strlen(e.name);
    Icon *ic = &s_icon[s_nicon];
    if (e.is_dir) continue;

    if (n > 4 && strcmp(e.name + n - 4, ".app") == 0) {
      char stem[20];
      snprintf(stem, sizeof stem, "%.*s", (int)(n - 4), e.name);
      ic->app = app_index_by_name(stem);
      if (ic->app < 0) continue;             /* names an app we do not have */
      snprintf(ic->name, sizeof ic->name, "%s", stem);
    } else if (n > 4 && strcmp(e.name + n - 4, ".bin") == 0) {
      ic->app = -1;
      snprintf(ic->name, sizeof ic->name, "%.*s", (int)(n - 4), e.name);
    } else {
      continue;
    }
    snprintf(ic->path, sizeof ic->path, "%s/%s", ICONS_DIR, e.name);
    s_nicon++;
  }
  fs_closedir(&d);
}

static void paint_icons(Rect clip) {
  int i;
  for (i = 0; i < s_nicon; i++) {
    Rect r = icon_rect(i), box;
    if (!rect_overlaps(r, clip)) continue;
    draw_set_clip(rect_intersect(clip, r));

    box.x = (int16_t)(r.x + (r.w - ICON_BOX) / 2);
    box.y = r.y;
    box.w = ICON_BOX;
    box.h = ICON_BOX;
    /* Firmware gets a sunken slab, an app a raised one, so the two kinds are
     * distinguishable before reading the label. */
    if (s_icon[i].app < 0) draw_bevel(box, C_TITLE_UN, C_SHADOW, C_LIGHT);
    else                   draw_bevel(box, C_FACE, C_LIGHT, C_DARK);
    draw_text((int16_t)(box.x + 5), (int16_t)(box.y + 4),
              s_icon[i].app < 0 ? "F" : "A", C_TEXT,
              s_icon[i].app < 0 ? C_TITLE_UN : C_FACE);

    if (i == s_sel_icon) draw_frame(r, C_TITLE_FG);
    draw_text_ellipsis(r.x, (int16_t)(r.y + ICON_BOX + 2), r.w,
                       s_icon[i].name, C_TITLE_FG, C_DESKTOP);
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
    Rect inner = rect_inset(content, 2);
    Rect vis = rect_intersect(clip, inner);
    if (a->paint && !rect_is_empty(vis)) {
      /* Confine the app to its own content well: an app that draws too far
       * must not be able to scribble over another window's chrome. */
      draw_set_clip(vis);
      a->paint(a->state, inner);
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

static void paint_start_menu(void) {
  Rect m;
  int i;
  if (!s_start_open) return;

  {
  int items = app_count() + 1;    /* apps, then Console */
  m = R(2, DESK_H - (items * 11 + 6), 84, items * 11 + 6);
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
}

static void draw_pointer(void);
static int  s_kbd_mouse;
static int  s_dragging;
static WinId s_drag_win;

/* ------------------------------------------------------------ paint ----- */

static void paint_job(void *ctx, WinId w, Rect r) {
  (void)ctx;
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

void desktop_flush(void) {
  if (wm_damage_count() == 0) return;
  wm_paint(paint_job, NULL);
  paint_start_menu();
  draw_pointer();      /* always last: the pointer is above everything */
}

void desktop_repaint(void) {
  wm_damage(R(0, 0, DISPLAY_W, DISPLAY_H));
  desktop_flush();
}

/* ------------------------------------------------------------ input ----- */

static void open_app(int k);

static void launch_icon(int i) {
  if (i < 0 || i >= s_nicon) return;

  if (s_icon[i].app >= 0) { open_app(s_icon[i].app); desktop_repaint(); return; }

  /* Firmware. Remember that the desktop launched it, so that when the
   * bootloader rolls back after the guest is reset we come straight back here
   * rather than to a console the user never asked for. */
  desktop_set_autostart(1);
  {
    AppImageInfo info;
    AppImageResult why;
    if (launcher_check(s_icon[i].path, &info, &why) != LAUNCH_OK) {
      desktop_set_autostart(0);
      return;
    }
    launcher_boot(s_icon[i].path, NULL, NULL);   /* does not return on success */
    desktop_set_autostart(0);                    /* only reached on failure */
  }
}

void desktop_icon_click(int16_t x, int16_t y) {
  uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
  int i, hit = -1;

  for (i = 0; i < s_nicon; i++)
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

static void open_app(int k) {
  const AppDef *a = app_at(k);
  Rect frame;
  WinId w;
  if (s_nwin >= MAX_OPEN) return;
  /* Cascade, so a new window is visibly on top rather than exactly covering
   * the last one. */
  frame = R(8 + s_nwin * 14, 6 + s_nwin * 10, 132, 74);
  w = wm_create(a->name, frame);
  if (w == WIN_NONE) return;
  if (a->open) a->open(a->state);
  s_win[s_nwin] = w;
  s_app[s_nwin] = k;
  s_nwin++;
}

static void close_focused(void) {
  WinId f = wm_focus();
  int i, j;
  if (f == WIN_NONE) return;
  wm_destroy(f);
  for (i = 0; i < s_nwin; i++) {
    if (s_win[i] != f) continue;
    for (j = i; j < s_nwin - 1; j++) { s_win[j] = s_win[j + 1]; s_app[j] = s_app[j + 1]; }
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
  if (s_start_open) {
    switch (key) {
    case KEY_UP:   s_start_sel = (s_start_sel + app_count()) % (app_count() + 1); break;
    case KEY_DOWN: s_start_sel = (s_start_sel + 1) % (app_count() + 1); break;
    case KEY_ENTER:
      s_start_open = 0;
      if (s_start_sel >= app_count()) {
        desktop_set_autostart(0);   /* leaving on purpose: stay at the console */
        return 1;
      }
      open_app(s_start_sel);
      desktop_repaint();
      return 0;
    case KEY_ESC: s_start_open = 0; desktop_repaint(); return 0;
    default: break;
    }
    desktop_repaint();
    return 0;
  }

  /* Desktop commands are ctrl-chords, so every ordinary key stays free to
   * reach the focused app. Without that a window you can type into is
   * impossible: `s` would always mean Start. */
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

  switch (key) {
  case 0x10: desktop_set_kbd_mouse(!s_kbd_mouse); return 0;                  /* ctrl-P */
  case 0x13: s_start_open = 1; s_start_sel = 0; desktop_repaint(); return 0; /* ctrl-S */
  case 0x17: close_focused(); desktop_repaint(); return 0;                   /* ctrl-W */
  case '\t':      cycle_focus();   desktop_flush(); return 0;
  case KEY_LEFT:  nudge(-6, 0);    desktop_flush(); return 0;
  case KEY_RIGHT: nudge(6, 0);     desktop_flush(); return 0;
  case KEY_UP:    nudge(0, -5);    desktop_flush(); return 0;
  case KEY_DOWN:  nudge(0, 5);     desktop_flush(); return 0;
  case KEY_ESC:   desktop_set_autostart(0); return 1;   /* to the console */
  default: break;
  }

  /* Everything else belongs to whichever window has focus. */
  {
    WinId f = wm_focus();
    const AppDef *a;
    if (f == WIN_NONE) return 0;
    a = app_of(f);
    if (a->key && a->key(a->state, key)) wm_damage(wm_frame(f));
  }
  desktop_flush();
  return 0;
}

void desktop_tick(uint32_t ms) {
  uint32_t before = s_now_ms / 1000u;
  s_now_ms = ms;
  if (s_now_ms / 1000u == before) return;

  /* A mouse that wanders out of range or sleeps drops the link. Look for it
   * again rather than sitting there with a dead pointer -- but not every
   * second, because each attempt is a six-second scan. */
  if (btmouse_state() == BTM_FAILED && (s_now_ms / 1000u) % 15 == 0)
    btmouse_start(4);
  wm_damage(R(DISPLAY_W - 30, DESK_H + 2, 28, TASKBAR_H - 4));
  desktop_flush();
}

#define NVS_NS        "cardos"
#define NVS_AUTOSTART "autodesk"

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
  wm_init(DISPLAY_W, DISPLAY_H);
  mouse_init(DISPLAY_W, DISPLAY_H);
  s_nwin = 0;
  s_start_open = 0;
  s_start_sel = 0;
  for (i = 0; i < MAX_OPEN; i++) s_win[i] = WIN_NONE;

  desktop_reload_icons();

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_DESKTOP);
  desktop_repaint();
}

/* ------------------------------------------------------------- mouse ---- */

static int s_cursor_on;
static int16_t s_drag_dx, s_drag_dy;   /* pointer offset within the frame */

static Rect cursor_rect(void) {
  Rect r;
  r.x = (int16_t)mouse_x();
  r.y = (int16_t)mouse_y();
  r.w = CURSOR_W;
  r.h = CURSOR_H;
  return r;
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
  desktop_repaint();
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
  s_cursor_on = 1;

  /* Consume both edges every time, whether or not they are used here.
   * Reading the release only while dragging left stale edges behind, so the
   * next drag saw a release from some earlier click and ended after a single
   * move -- which is exactly what a drag that "sometimes works" looks like. */
  pressed = mouse_pressed(MOUSE_LEFT);
  released = mouse_released(MOUSE_LEFT);

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

  hit = wm_at((int16_t)mouse_x(), (int16_t)mouse_y());
  if (hit == WIN_NONE) {
    if (mouse_y() >= DESK_H) {
      if (mouse_x() < 38) {                 /* Start button */
        s_start_open = !s_start_open;
        desktop_repaint();
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
    if (s_start_open) { s_start_open = 0; desktop_repaint(); return; }
    desktop_icon_click((int16_t)mouse_x(), (int16_t)mouse_y());
    return;
  }

  {
    WmHit what = wm_hit_test(hit, (int16_t)mouse_x(), (int16_t)mouse_y());
    wm_raise(hit);
    if (s_start_open) { s_start_open = 0; desktop_repaint(); return; }
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
      Rect c = rect_inset(wm_content(hit), 2);
      if (a->click &&
          a->click(a->state, (int16_t)(mouse_x() - c.x),
                   (int16_t)(mouse_y() - c.y), MOUSE_LEFT))
        wm_damage(wm_frame(hit));
    }
  }
}
