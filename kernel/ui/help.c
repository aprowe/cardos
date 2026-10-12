/* The fn-h panel and the fn-space palette, painted. The state and the keys
 * are kernel/ui/helpmodel.c; this draws them over the whole screen, off the
 * panel a strip at a time (draw_offscreen), so a scroll never flickers. */

#include "kernel/ui/help.h"
#include "kernel/ui/draw.h"

#include <stdio.h>
#include <string.h>

#define C_BG     RGB565(16, 18, 26)
#define C_HEAD   RGB565(26, 30, 42)
#define C_RULE   RGB565(52, 58, 78)
#define C_ACC    RGB565(110, 160, 255)    /* the active tab, the selection */
#define C_ACC_FG RGB565(10, 14, 26)
#define C_CHIP   RGB565(38, 44, 64)       /* a key, as a keycap */
#define C_KEY    RGB565(150, 196, 255)
#define C_TXT    RGB565(222, 226, 236)
#define C_DIM    RGB565(120, 128, 148)
#define C_SECT   RGB565(255, 190, 92)     /* a heading inside a tab */
#define C_SEL    RGB565(40, 56, 96)

#define TOP_H    16
#define FOOT_H   11
#define ROW_H    11
#define KEY_W    66                        /* the key column: "ctrl-opt-u" fits */
#define P_ROW_H  12

static HelpModel s_help;
static int       s_help_on;
static Palette   s_pal;
static int       s_pal_on;
static char      s_pal_title[24];

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int help_rows(void) { return (DISPLAY_H - TOP_H - 3 - FOOT_H) / ROW_H; }
static int pal_rows(void)  { return (DISPLAY_H - 20 - FOOT_H) / P_ROW_H; }

/* ---- the help panel ---------------------------------------------------------- */

static void tabs(void) {
  int t, x = 4;
  draw_rect(R(0, 0, DISPLAY_W, TOP_H), C_HEAD);
  for (t = 0; t < HELP_TABS; t++) {
    char name[16];
    int w;
    if (!helpm_count(s_help.text[t])) continue;
    snprintf(name, sizeof name, "%s", s_help.name[t] ? s_help.name[t] : "");
    w = draw_text_width(name) + 10;
    if (x + w > DISPLAY_W - 4) break;
    if (t == s_help.tab) {
      draw_rect(R(x, 2, w, TOP_H - 4), C_ACC);
      draw_text((int16_t)(x + 5), 4, name, C_ACC_FG, C_ACC);
    } else {
      draw_text((int16_t)(x + 5), 4, name, C_DIM, C_HEAD);
    }
    x += w + 3;
  }
  draw_rect(R(0, TOP_H, DISPLAY_W, 1), C_RULE);
}

/* One line: a heading (no tab) in amber across the width, or a key as a
 * keycap and what it does beside it. */
static void row(const char *p, int y) {
  char key[20], text[64];
  const char *tab = NULL, *q;
  int kn = 0, tn = 0, kw;
  for (q = p; *q && *q != '\n'; q++) if (*q == '\t' && !tab) tab = q;
  if (!tab) {
    while (p < q && tn < (int)sizeof text - 1) text[tn++] = *p++;
    text[tn] = 0;
    draw_text_ellipsis(6, (int16_t)(y + 2), DISPLAY_W - 16, text, C_SECT, C_BG);
    return;
  }
  while (p < tab && kn < (int)sizeof key - 1) key[kn++] = *p++;
  key[kn] = 0;
  for (p = tab + 1; p < q && tn < (int)sizeof text - 1; ) text[tn++] = *p++;
  text[tn] = 0;
  kw = draw_text_width(key) + 6;
  if (kw > KEY_W - 2) kw = KEY_W - 2;
  draw_rect(R(4, y, kw, ROW_H - 1), C_CHIP);
  draw_text_ellipsis(7, (int16_t)(y + 1), (int16_t)(kw - 6), key, C_KEY, C_CHIP);
  draw_text_ellipsis(4 + KEY_W + 2, (int16_t)(y + 1), (int16_t)(DISPLAY_W - KEY_W - 16),
                     text, C_TXT, C_BG);
}

static void help_body(void *ctx) {
  const char *text = s_help.text[s_help.tab];
  int n = helpm_count(text), rows = help_rows(), i, y = TOP_H + 3;
  (void)ctx;
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_BG);
  tabs();
  for (i = s_help.top; i < n && i < s_help.top + rows; i++, y += ROW_H)
    row(helpm_line(text, i), y);
  /* A scrollbar when there is more than fits: the thumb is where you are. */
  if (n > rows) {
    int track = rows * ROW_H, th = track * rows / n, ty = track * s_help.top / n;
    if (th < 6) th = 6;
    draw_rect(R(DISPLAY_W - 3, TOP_H + 3, 2, track), C_CHIP);
    draw_rect(R(DISPLAY_W - 3, TOP_H + 3 + ty, 2, th), C_ACC);
  }
  draw_rect(R(0, DISPLAY_H - FOOT_H, DISPLAY_W, FOOT_H), C_HEAD);
  draw_text(5, DISPLAY_H - FOOT_H + 2,
            n > rows ? "< > tabs   ; . scroll   esc close" : "< > tabs   esc close", C_DIM, C_HEAD);
}

void help_open(int tab, const char *app_title, const char *app_keys,
               const char *os_title, const char *os_keys, const char *shortcuts) {
  memset(&s_help, 0, sizeof s_help);
  s_help.name[HELP_APP] = app_title ? app_title : "App";
  s_help.text[HELP_APP] = app_keys;
  s_help.name[HELP_OS] = os_title ? os_title : "OS";
  s_help.text[HELP_OS] = os_keys;
  s_help.name[HELP_SHORT] = "Shortcuts";
  s_help.text[HELP_SHORT] = shortcuts;
  s_help.rows = help_rows();
  helpm_show(&s_help, tab);
  s_pal_on = 0;
  s_help_on = 1;
  overlay_paint();
}

int help_active(void) { return s_help_on; }

int help_key(uint8_t k) {
  if (!s_help_on) return 0;
  if (!helpm_key(&s_help, k)) { s_help_on = 0; return 0; }
  overlay_paint();
  return 1;
}

/* ---- the palette -------------------------------------------------------------- */

static void pal_body(void *ctx) {
  int rows = pal_rows(), i, y = 20;
  char line[40];
  (void)ctx;
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_BG);
  draw_rect(R(4, 3, DISPLAY_W - 8, 14), C_HEAD);
  draw_frame(R(4, 3, DISPLAY_W - 8, 14), C_RULE);
  snprintf(line, sizeof line, "> %s_", s_pal.q);
  draw_text(9, 6, line, C_TXT, C_HEAD);
  if (s_pal_title[0])
    draw_text((int16_t)(DISPLAY_W - 9 - draw_text_width(s_pal_title)), 6, s_pal_title, C_DIM, C_HEAD);
  if (!s_pal.nhit) draw_text(9, (int16_t)(y + 3), "no action called that", C_DIM, C_BG);
  for (i = s_pal.top; i < s_pal.nhit && i < s_pal.top + rows; i++, y += P_ROW_H) {
    const PalEntry *e = &s_pal.e[s_pal.hit[i]];
    uint16_t bg = i == s_pal.sel ? C_SEL : C_BG;
    int cw = e->chord[0] ? draw_text_width(e->chord) : 0;
    int gw = e->group[0] ? draw_text_width(e->group) + 8 : 0;
    draw_rect(R(4, y, DISPLAY_W - 8, P_ROW_H), bg);
    if (i == s_pal.sel) draw_rect(R(4, y, 2, P_ROW_H), C_ACC);
    draw_text_ellipsis(10, (int16_t)(y + 2), (int16_t)(DISPLAY_W - 26 - cw - gw), e->label,
                       e->kind == PAL_OS ? C_SECT : C_TXT, bg);
    if (gw) draw_text((int16_t)(DISPLAY_W - 10 - cw - gw), (int16_t)(y + 2), e->group, C_DIM, bg);
    if (cw) draw_text((int16_t)(DISPLAY_W - 10 - cw), (int16_t)(y + 2), e->chord, C_KEY, bg);
  }
  draw_rect(R(0, DISPLAY_H - FOOT_H, DISPLAY_W, FOOT_H), C_HEAD);
  draw_text(5, DISPLAY_H - FOOT_H + 2, "type to find   enter runs   esc close", C_DIM, C_HEAD);
}

Palette *palette_begin(const char *title) {
  pal_clear(&s_pal);
  snprintf(s_pal_title, sizeof s_pal_title, "%s", title ? title : "");
  return &s_pal;
}

void palette_show(void) {
  pal_filter(&s_pal);
  s_help_on = 0;
  s_pal_on = 1;
  overlay_paint();
}

int palette_active(void) { return s_pal_on; }

int palette_key(uint8_t k, const PalEntry **chosen) {
  int r;
  if (chosen) *chosen = NULL;
  if (!s_pal_on) return PAL_CLOSED;
  r = pal_key(&s_pal, k, pal_rows());
  if (r == PAL_CHOSEN && chosen) *chosen = &s_pal.e[s_pal.hit[s_pal.sel]];
  if (r != PAL_OPEN) s_pal_on = 0;
  else overlay_paint();
  return r;
}

/* ---- either --------------------------------------------------------------------- */

int overlay_active(void) { return s_help_on || s_pal_on; }

void overlay_paint(void) {
  if (!overlay_active()) return;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_offscreen(R(0, 0, DISPLAY_W, DISPLAY_H), C_BG, s_help_on ? help_body : pal_body, NULL);
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
}
