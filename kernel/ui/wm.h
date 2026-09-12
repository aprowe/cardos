/* Window manager core: z-order, hit testing, damage tracking.
 *
 * Portable C -- no drawing. This half decides *what* needs repainting and
 * *which* window owns a pixel; the compositor turns that into pixels. Split
 * that way because the decisions are where the bugs are and they are all
 * testable on a PC.
 *
 * The defining constraint, from the kernel spec: a full-screen 16-bit canvas
 * is 65 KB, a fifth of the heap on a board with 322 KB and no PSRAM. There is
 * therefore **no back buffer, per window or otherwise**. Everything repaints
 * from damage rectangles straight to the panel, which is why damage tracking
 * is a first-class part of this module rather than an optimisation.
 *
 * See docs/specs/NEXT-window-system.md.
 */
#ifndef CARDOS_WM_H
#define CARDOS_WM_H

#include <stdint.h>

#include "kernel/ui/rect.h"

#define WM_MAX_WINDOWS 8
#define WM_MAX_DAMAGE  8
#define WM_TITLE_MAX   15

/* Chrome geometry. A 1px border and a 9px title bar cost 12 pixels of height
 * before any content, on a 135px screen -- which is why windows overlap
 * rather than tile. */
#define WM_BORDER   1
#define WM_TITLE_H  9
#define WM_CLOSE_W  7

/* Low 8 bits index, high 8 bits generation, never 0 -- as with Handle and
 * Tid. A stale window id must not address whichever window inherited the
 * slot. */
typedef uint16_t WinId;
#define WIN_NONE ((WinId)0)

typedef enum {
  WM_HIT_NONE = 0,
  WM_HIT_CONTENT,
  WM_HIT_TITLE,      /* drag here */
  WM_HIT_CLOSE,
  WM_HIT_MAX,        /* the box that fills the screen with this app */
  WM_HIT_MIN,        /* the box that puts it down on the taskbar */
  WM_HIT_BORDER
} WmHit;

void  wm_init(int16_t screen_w, int16_t screen_h);

WinId wm_create(const char *title, Rect frame);
void  wm_destroy(WinId w);
int   wm_valid(WinId w);
int   wm_count(void);

Rect        wm_frame(WinId w);      /* including chrome */
Rect        wm_content(WinId w);    /* inside the chrome */
const char *wm_title(WinId w);

void  wm_move(WinId w, int16_t x, int16_t y);
void  wm_resize(WinId w, int16_t width, int16_t height);

/* Raising also focuses. Focus follows the top window, because with one
 * pointer and one keyboard there is no useful way for them to differ. */
void  wm_raise(WinId w);
WinId wm_focus(void);

/* 0 is the bottom of the stack. -1 if the id is stale. */
int   wm_z(WinId w);

/* Topmost window whose frame contains the point, or WIN_NONE. */
WinId wm_at(int16_t x, int16_t y);
WmHit wm_hit_test(WinId w, int16_t x, int16_t y);

/* Is this pixel of this window actually visible, or covered by one above it? */
int   wm_visible_at(WinId w, int16_t x, int16_t y);

/* ---- damage ----
 *
 * Rectangles are merged when they overlap, and when the list is full the two
 * cheapest to combine are merged. The invariant that matters is coverage:
 * whatever merging happens, the rects handed back must cover every rect that
 * was added. Repainting too much is slow; repainting too little leaves
 * rubbish on the screen. */
void wm_damage(Rect r);
int  wm_damage_count(void);
int  wm_take_damage(Rect *out, int max);   /* count, and clears the list */

/* ---- painting ----
 *
 * Turns accumulated damage into a sequence of "window W must redraw rectangle
 * R" calls, with WIN_NONE meaning the desktop background. Consumes the damage.
 *
 * A callback rather than a filled array on purpose. Partitioning a full screen
 * behind eight overlapping windows needs far more rectangles than fit
 * comfortably on a 1 KB task stack, and an array with a cap silently drops
 * jobs -- which leaves pixels unpainted, the one failure this module exists to
 * prevent.
 *
 * Calls arrive **back to front**: the desktop first, then each window from the
 * bottom of the stack upward. Painting them in the order given is therefore
 * always correct, even where a region is covered more than once.
 *
 * Guarantees, both checked per pixel by the tests:
 *   - every damaged pixel is painted at least once;
 *   - the last call covering a pixel names the topmost window there.
 */
typedef void (*PaintFn)(void *ctx, WinId w, Rect r);

void wm_paint(PaintFn fn, void *ctx);

#endif /* CARDOS_WM_H */
