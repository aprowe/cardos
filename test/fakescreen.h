/* A 240x135 framebuffer for an app's paint test, with the real fonts.
 *
 * test_vim_paint.c and test_chat_paint.c each carry their own copy of the
 * fill, frame and 6x8 text; this one also draws text_font with the .cfnt
 * files in fonts/ (read with kernel/ui/cfont.c, laid out as draw.c does:
 * the run's cell filled with bg, each glyph pixel blended onto it), so a
 * dumped screen looks like the device's. Used by the Wordle and Hangman
 * paint tests.
 *
 *   fakescreen_install(&api);         after fakeapi_init: drawing, fonts, ui
 *   fakescreen_paint();               a whole repaint into fakescreen_fb
 *   fakescreen_unset()                pixels the paint never wrote
 *   fakescreen_dump("DIR_ENV", "x")   writes $DIR_ENV/x.ppm if it is set
 *
 * Static, header-only: one copy per test file that includes it. */
#ifndef CARDOS_FAKESCREEN_H
#define CARDOS_FAKESCREEN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakeapi.h"
#include "kernel/console/font6x8.h"
#include "kernel/ui/cfont.h"

#define FS_W 240
#define FS_H 135
#define FS_UNSET 0xF81F

static uint16_t fakescreen_fb[FS_H][FS_W];
static CappUi   fakescreen_ui;
static int      fakescreen_painted;

#define FS_FONTS 6
static struct { char name[16]; uint8_t data[24000]; CFont f; } fs_font[FS_FONTS];
static int fs_nfonts;

static void fs_put(int x, int y, uint16_t c) {
  if (x >= 0 && y >= 0 && x < FS_W && y < FS_H) fakescreen_fb[y][x] = c;
}
static void fs_fill(CRect r, uint16_t c) {
  int x, y;
  for (y = r.y; y < r.y + r.h; y++) for (x = r.x; x < r.x + r.w; x++) fs_put(x, y, c);
}
static void fs_frame(CRect r, uint16_t c) {
  CRect t;
  t = r; t.h = 1; fs_fill(t, c);
  t.y = (int16_t)(r.y + r.h - 1); fs_fill(t, c);
  t = r; t.w = 1; fs_fill(t, c);
  t.x = (int16_t)(r.x + r.w - 1); fs_fill(t, c);
}
static void fs_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  for (; *s; s++, x += 6) {
    int ch = (unsigned char)*s, i, j;
    const uint8_t *g = font6x8[(ch >= 32 && ch <= 126 ? ch : '?') - 32];
    for (i = 0; i < 6; i++)
      for (j = 0; j < 8; j++) fs_put(x + i, y + j, (g[i] >> j) & 1 ? fg : bg);
  }
}

/* fonts/NAME.cfnt, found beside this file's folder. */
static int fs_font_load(const char *name) {
  char path[600];
  const char *f = __FILE__, *cut = f;
  const char *p;
  FILE *in;
  size_t n;
  int i;
  for (i = 0; i < fs_nfonts; i++) if (!strcmp(fs_font[i].name, name)) return i;
  if (fs_nfonts >= FS_FONTS) return -1;
  for (p = f; *p; p++) if (*p == '/' || *p == '\\') cut = p;
  /* cut is the separator before this file's name: test/ -> the repo. */
  snprintf(path, sizeof path, "%.*s/../fonts/%s.cfnt", (int)(cut - f), f, name);
  if (cut == f) snprintf(path, sizeof path, "fonts/%s.cfnt", name);
  if (!(in = fopen(path, "rb"))) return -1;
  n = fread(fs_font[fs_nfonts].data, 1, sizeof fs_font[0].data, in);
  fclose(in);
  if (cfont_parse(&fs_font[fs_nfonts].f, fs_font[fs_nfonts].data, n) != 0) return -1;
  snprintf(fs_font[fs_nfonts].name, sizeof fs_font[0].name, "%s", name);
  return fs_nfonts++;
}
static int fs_font_height(int f) { return f >= 0 && f < fs_nfonts ? fs_font[f].f.height : 8; }
static int fs_text_width(int f, const char *s) {
  return f >= 0 && f < fs_nfonts ? cfont_width(&fs_font[f].f, s) : (int)strlen(s) * 6;
}
static void fs_text_font(int fi, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  const CFont *f;
  int w, pen = x;
  CRect cell;
  if (fi < 0 || fi >= fs_nfonts) { fs_text(x, y, s, fg, bg); return; }
  f = &fs_font[fi].f;
  w = cfont_width(f, s);
  cell.x = x; cell.y = y; cell.w = (int16_t)w; cell.h = f->height;
  fs_fill(cell, bg);
  for (; *s; s++) {
    CGlyph g;
    int gx, gy;
    cfont_glyph(f, (unsigned char)*s, &g);
    for (gy = 0; gy < g.h; gy++)
      for (gx = 0; gx < g.w; gx++) {
        int px = pen + g.x + gx, py = y + g.y + gy, a;
        if (px < x || px >= x + w || py < y || py >= y + f->height) continue;
        a = cfont_pixel(f, &g, gx, gy);
        if (a) fs_put(px, py, cfont_blend(fg, bg, a));
      }
    pen += g.adv;
  }
}
static void fs_ui(const CappUi *ui) { fakescreen_ui = *ui; }

static void fakescreen_install(CardApi *a) {
  a->fill = fs_fill; a->frame = fs_frame; a->text = fs_text; a->ui = fs_ui;
  a->font_load = fs_font_load; a->text_font = fs_text_font;
  a->text_width = fs_text_width; a->font_height = fs_font_height;
}

static void fakescreen_paint(void) {
  CRect all;
  int x, y;
  all.x = 0; all.y = 0; all.w = FS_W; all.h = FS_H;
  for (y = 0; y < FS_H; y++) for (x = 0; x < FS_W; x++) fakescreen_fb[y][x] = FS_UNSET;
  fakescreen_ui.paint(fakescreen_ui.state, all);
  fakescreen_painted++;
}

static int fakescreen_unset(void) {
  int x, y, n = 0;
  for (y = 0; y < FS_H; y++) for (x = 0; x < FS_W; x++) n += fakescreen_fb[y][x] == FS_UNSET;
  return n;
}

/* How many pixels of `colour` in the rectangle. */
static int fakescreen_count(CRect r, uint16_t colour) {
  int x, y, n = 0;
  for (y = r.y; y < r.y + r.h; y++)
    for (x = r.x; x < r.x + r.w; x++)
      if (x >= 0 && y >= 0 && x < FS_W && y < FS_H) n += fakescreen_fb[y][x] == colour;
  return n;
}

static void fakescreen_dump(const char *env, const char *name) {
  const char *dir = getenv(env);
  char f[512];
  FILE *o;
  int x, y;
  if (!dir) return;
  snprintf(f, sizeof f, "%s/%s.ppm", dir, name);
  if (!(o = fopen(f, "wb"))) return;
  fprintf(o, "P6 %d %d 255\n", FS_W, FS_H);
  for (y = 0; y < FS_H; y++)
    for (x = 0; x < FS_W; x++) {
      uint16_t c = (uint16_t)(fakescreen_fb[y][x] >> 8 | fakescreen_fb[y][x] << 8);
      fputc((c >> 11) << 3, o); fputc(((c >> 5) & 63) << 2, o); fputc((c & 31) << 3, o);
    }
  fclose(o);
}

#endif /* CARDOS_FAKESCREEN_H */
