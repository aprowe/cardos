/* The ctrl-h key list. See help.h. */

#include "kernel/ui/help.h"
#include "kernel/ui/draw.h"

#include <stdio.h>
#include <string.h>

#define PAD      4
#define ROW_H    9
#define KEY_COL  46      /* where the meaning starts, so the keys line up */

/* A dark panel regardless of what is underneath, because it sits over
 * everything -- a light one over the editor would be the only bright thing on
 * the screen, and a light one over the launcher would vanish into it. */
#define H_BG     0x1821    /* pre-swapped RGB565, a near-black blue */
#define H_EDGE   0x6B4A
#define H_TITLE  0xFFFF
#define H_KEY    0x1FA6    /* the caret blue, so keys read as keys */
#define H_TEXT   0xDAD6

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int count_lines(const char *s) {
  int n = 0;
  if (!s || !*s) return 0;
  for (; *s; s++) if (*s == '\n') n++;
  return n + (s[-1] == '\n' ? 0 : 1);
}

/* One line: everything before the tab is the key, everything after is what it
 * does. A line with no tab is a heading and spans the width. */
static const char *draw_line(const char *p, int16_t x, int16_t y, int16_t w) {
  char key[16], text[48];
  const char *tab = NULL, *q;
  size_t kn = 0, tn = 0;

  for (q = p; *q && *q != '\n'; q++) if (*q == '\t' && !tab) tab = q;

  if (!tab) {
    while (p < q && tn < sizeof text - 1) text[tn++] = *p++;
    text[tn] = 0;
    draw_text_ellipsis(x, y, w, text, H_TITLE, H_BG);
  } else {
    while (p < tab && kn < sizeof key - 1) key[kn++] = *p++;
    key[kn] = 0;
    p = tab + 1;
    while (p < q && tn < sizeof text - 1) text[tn++] = *p++;
    text[tn] = 0;
    draw_text_ellipsis(x, y, KEY_COL - 2, key, H_KEY, H_BG);
    draw_text_ellipsis((int16_t)(x + KEY_COL), y, (int16_t)(w - KEY_COL),
                       text, H_TEXT, H_BG);
  }

  return *q == '\n' ? q + 1 : q;
}

void help_paint(const char *title, const char *app_keys, const char *shell_keys) {
  int rows = 1 + count_lines(app_keys) + count_lines(shell_keys) + 1;
  int16_t h, y;
  Rect panel;

  /* Sized to what it holds, and clamped to the panel. A list longer than the
   * screen loses its tail rather than running off the bottom silently -- the
   * shells keep their own lists short enough that this does not happen. */
  h = (int16_t)(rows * ROW_H + 2 * PAD);
  if (h > DISPLAY_H) h = DISPLAY_H;

  panel = R(6, (DISPLAY_H - h) / 2, DISPLAY_W - 12, h);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(panel, H_BG);
  draw_frame(panel, H_EDGE);
  draw_set_clip(rect_inset(panel, 1));

  y = (int16_t)(panel.y + PAD);
  {
    char head[32];
    snprintf(head, sizeof head, "%s keys", title ? title : "CardOS");
    draw_text_ellipsis((int16_t)(panel.x + PAD), y,
                       (int16_t)(panel.w - 2 * PAD), head, H_TITLE, H_BG);
    y = (int16_t)(y + ROW_H);
  }

  {
    const char *p = app_keys;
    while (p && *p && y + ROW_H <= panel.y + panel.h - PAD - ROW_H)
      { p = draw_line(p, (int16_t)(panel.x + PAD), y, (int16_t)(panel.w - 2 * PAD)); y = (int16_t)(y + ROW_H); }
    p = shell_keys;
    while (p && *p && y + ROW_H <= panel.y + panel.h - PAD)
      { p = draw_line(p, (int16_t)(panel.x + PAD), y, (int16_t)(panel.w - 2 * PAD)); y = (int16_t)(y + ROW_H); }
  }

  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}
