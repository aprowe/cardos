/* The desktop shell. See desktop.h. */

#include "ui/desktop.h"
#include "ui/draw.h"
#include "drv/keyboard.h"
#include "mem/mem.h"
#include "fs/fs.h"

#include <stdio.h>
#include <string.h>

#define DESK_H (DISPLAY_H - TASKBAR_H)

/* What a window shows. The desktop owns the content because there is no app
 * model yet -- that arrives with the context switch, when each window becomes
 * a task with its own paint callback. */
typedef enum { APP_ABOUT = 0, APP_FILES, APP_MEM, APP_COUNT } AppKind;

static const char *APP_NAME[APP_COUNT] = { "About", "Files", "Memory" };

static WinId   s_win[APP_COUNT];
static AppKind s_kind[APP_COUNT];
static int     s_nwin;

static int      s_start_open;
static int      s_start_sel;
static uint32_t s_now_ms;

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static AppKind kind_of(WinId w) {
  int i;
  for (i = 0; i < s_nwin; i++)
    if (s_win[i] == w) return s_kind[i];
  return APP_ABOUT;
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
    Rect inner = rect_inset(content, 2);
    int16_t tx = inner.x, ty = inner.y;
    char line[40];

    switch (kind_of(w)) {
    case APP_ABOUT:
      draw_text(tx, ty, "CardOS 0.1", C_TEXT, C_WHITE);
      draw_text(tx, (int16_t)(ty + 9), "M5Stack Cardputer", C_TEXT, C_WHITE);
      draw_text(tx, (int16_t)(ty + 18), "ESP32-S3, 8MB flash", C_TEXT, C_WHITE);
      draw_text(tx, (int16_t)(ty + 27), "no PSRAM, no MMU", C_TEXT, C_WHITE);
      break;
    case APP_FILES: {
      FsDir d;
      FsEntry e;
      int n = 0;
      if (fs_opendir("/", &d) == 0) {
        while (n < 5 && fs_readdir(&d, &e) == 1) {
          snprintf(line, sizeof line, "%.30s%s", e.name, e.is_dir ? "/" : "");
          draw_text(tx, (int16_t)(ty + n * 9), line, C_TEXT, C_WHITE);
          n++;
        }
        fs_closedir(&d);
      }
      if (n == 0) draw_text(tx, ty, "no card", C_TEXT, C_WHITE);
      break;
    }
    case APP_MEM: {
      MemStats st;
      mem_stats(&st);
      snprintf(line, sizeof line, "heap  %uK", (unsigned)(st.heap_size / 1024));
      draw_text(tx, ty, line, C_TEXT, C_WHITE);
      snprintf(line, sizeof line, "used  %u", (unsigned)(st.movable_used + st.fixed_used));
      draw_text(tx, (int16_t)(ty + 9), line, C_TEXT, C_WHITE);
      snprintf(line, sizeof line, "hnds  %u/%u", (unsigned)st.handles_used,
               (unsigned)MEM_MAX_HANDLES);
      draw_text(tx, (int16_t)(ty + 18), line, C_TEXT, C_WHITE);
      snprintf(line, sizeof line, "cmpct %u", (unsigned)st.compactions);
      draw_text(tx, (int16_t)(ty + 27), line, C_TEXT, C_WHITE);
      break;
    }
    default: break;
    }
  }
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}

/* ---------------------------------------------------------- taskbar ----- */

static void paint_taskbar(void) {
  Rect bar = R(0, DESK_H, DISPLAY_W, TASKBAR_H);
  Rect start = R(2, DESK_H + 2, 34, TASKBAR_H - 4);
  int i;
  int16_t x;
  char clock[8];

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
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
    draw_set_clip(rect_inset(b, 2));
    draw_text_ellipsis((int16_t)(b.x + 2), (int16_t)(b.y + 1), 42,
                       wm_title(s_win[i]), C_TEXT, C_FACE);
    draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
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

  m = R(2, DESK_H - (APP_COUNT * 11 + 6), 84, APP_COUNT * 11 + 6);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_bevel(m, C_FACE, C_LIGHT, C_DARK);

  for (i = 0; i < APP_COUNT; i++) {
    Rect item = R(m.x + 3, m.y + 3 + i * 11, m.w - 6, 10);
    int sel = (i == s_start_sel);
    draw_rect(item, sel ? C_TITLE : C_FACE);
    draw_text((int16_t)(item.x + 2), (int16_t)(item.y + 1), APP_NAME[i],
              sel ? C_TITLE_FG : C_TEXT, sel ? C_TITLE : C_FACE);
  }
}

/* ------------------------------------------------------------ paint ----- */

static void paint_job(void *ctx, WinId w, Rect r) {
  (void)ctx;
  if (w == WIN_NONE) {
    draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
    draw_rect(rect_intersect(r, R(0, 0, DISPLAY_W, DESK_H)), C_DESKTOP);
  } else {
    paint_window(w, r);
  }
}

void desktop_flush(void) {
  if (wm_damage_count() == 0) return;
  wm_paint(paint_job, NULL);
  paint_taskbar();
  paint_start_menu();
}

void desktop_repaint(void) {
  wm_damage(R(0, 0, DISPLAY_W, DESK_H));
  desktop_flush();
}

/* ------------------------------------------------------------ input ----- */

static void open_app(AppKind k) {
  Rect frame;
  WinId w;
  if (s_nwin >= APP_COUNT) return;
  /* Cascade, so a new window is visibly on top rather than exactly covering
   * the last one. */
  frame = R(8 + s_nwin * 14, 6 + s_nwin * 10, 132, 74);
  w = wm_create(APP_NAME[k], frame);
  if (w == WIN_NONE) return;
  s_win[s_nwin] = w;
  s_kind[s_nwin] = k;
  s_nwin++;
}

static void close_focused(void) {
  WinId f = wm_focus();
  int i, j;
  if (f == WIN_NONE) return;
  wm_destroy(f);
  for (i = 0; i < s_nwin; i++) {
    if (s_win[i] != f) continue;
    for (j = i; j < s_nwin - 1; j++) { s_win[j] = s_win[j + 1]; s_kind[j] = s_kind[j + 1]; }
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
    case KEY_UP:   s_start_sel = (s_start_sel + APP_COUNT - 1) % APP_COUNT; break;
    case KEY_DOWN: s_start_sel = (s_start_sel + 1) % APP_COUNT; break;
    case KEY_ENTER:
      s_start_open = 0;
      open_app((AppKind)s_start_sel);
      desktop_repaint();
      return 0;
    case KEY_ESC: s_start_open = 0; desktop_repaint(); return 0;
    default: break;
    }
    desktop_repaint();
    return 0;
  }

  switch (key) {
  case '\t':     cycle_focus(); break;
  case 's': case 'S': s_start_open = 1; s_start_sel = 0; desktop_repaint(); return 0;
  case 'w': case 'W': close_focused(); desktop_repaint(); return 0;
  case KEY_LEFT:  nudge(-6, 0); break;
  case KEY_RIGHT: nudge(6, 0); break;
  case KEY_UP:    nudge(0, -5); break;
  case KEY_DOWN:  nudge(0, 5); break;
  case KEY_ESC:   return 1;                 /* back to the text console */
  default: break;
  }
  desktop_flush();
  return 0;
}

void desktop_tick(uint32_t ms) {
  uint32_t before = s_now_ms / 1000u;
  s_now_ms = ms;
  if (s_now_ms / 1000u != before) paint_taskbar();   /* just the clock */
}

void desktop_init(void) {
  int i;
  wm_init(DISPLAY_W, DESK_H);
  s_nwin = 0;
  s_start_open = 0;
  s_start_sel = 0;
  for (i = 0; i < APP_COUNT; i++) s_win[i] = WIN_NONE;

  open_app(APP_ABOUT);
  open_app(APP_MEM);

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_DESKTOP);
  desktop_repaint();
}
