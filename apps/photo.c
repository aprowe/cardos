/* A picture viewer, as a loadable CardOS app.
 *
 * Runs fullscreen: the render loop hands it the whole panel and paints nothing
 * else, so there is no chrome to leave room for.
 *
 * Images are raw RGB565, already byte-swapped for the panel, optionally behind
 * an 8-byte header:
 *
 *     'C' 'I' 'M' 'G'  u16 width  u16 height
 *
 * A headerless file is assumed to be 240x135, which is the whole screen -- the
 * one size worth special-casing on a device with exactly one screen.
 *
 * Rows are streamed one at a time. A full screen is 65KB, a fifth of the heap;
 * a row is 480 bytes.
 */

#include "kernel/app/capp.h"

#define SCR_W 240
#define SCR_H 135
#define MAXPICS 24
#define NAMELEN 32

static const CardApi *api;

static struct {
  char  dir[48];
  char  names[MAXPICS][NAMELEN];
  int   count;
  int   cur;
  char  status[48];
  uint16_t row[SCR_W];
} P;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static int ends_with(const char *s, const char *suffix) {
  size_t ls = api->str_len(s), lx = api->str_len(suffix);
  size_t i;
  if (lx > ls) return 0;
  for (i = 0; i < lx; i++) {
    char a = s[ls - lx + i], b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
    if (a != b) return 0;
  }
  return 1;
}

static void rescan(void) {
  static char raw[MAXPICS][NAMELEN];
  int n, i;
  P.count = 0;
  n = api->list(P.dir, &raw[0][0], MAXPICS, NAMELEN);
  if (n < 0) { api->fmt(P.status, sizeof P.status, "no %s", P.dir); return; }
  for (i = 0; i < n && P.count < MAXPICS; i++) {
    if (!ends_with(raw[i], ".img") && !ends_with(raw[i], ".565")) continue;
    api->fmt(P.names[P.count], NAMELEN, "%s", raw[i]);
    P.count++;
  }
  if (P.cur >= P.count) P.cur = 0;
  if (!P.count) api->fmt(P.status, sizeof P.status, "no pictures in %s", P.dir);
}

static void app_open(void *st) {
  (void)st;
  if (!P.dir[0]) api->fmt(P.dir, sizeof P.dir, "%s", "/pics");
  rescan();
}

/* Draws the current picture centred, leaving whatever it does not cover black.
 * Returns 0 if the file could not be shown. */
static int show(CRect c) {
  char path[96];
  unsigned char hdr[8];
  int fd, w, h, ox, oy, y;

  api->fmt(path, sizeof path, "%s/%s", P.dir, P.names[P.cur]);
  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) { api->fmt(P.status, sizeof P.status, "cannot open %s", P.names[P.cur]); return 0; }

  if (api->read(fd, hdr, 8) != 8) { api->close(fd); return 0; }
  if (hdr[0] == 'C' && hdr[1] == 'I' && hdr[2] == 'M' && hdr[3] == 'G') {
    w = hdr[4] | (hdr[5] << 8);
    h = hdr[6] | (hdr[7] << 8);
  } else {
    w = SCR_W;
    h = SCR_H;
    api->seek(fd, 0, 0);         /* headerless: rewind, those were pixels */
  }

  if (w <= 0 || h <= 0 || w > SCR_W || h > SCR_H) {
    api->fmt(P.status, sizeof P.status, "bad size %dx%d", w, h);
    api->close(fd);
    return 0;
  }

  ox = c.x + (c.w - w) / 2;
  oy = c.y + (c.h - h) / 2;
  if (w < c.w || h < c.h) api->fill(c, CAPP_BLACK);

  for (y = 0; y < h; y++) {
    if (api->read(fd, P.row, (size_t)w * 2) != w * 2) break;
    api->pixels(rect(ox, oy + y, w, 1), P.row);
  }
  api->close(fd);
  api->fmt(P.status, sizeof P.status, "%d/%d %s", P.cur + 1, P.count, P.names[P.cur]);
  return 1;
}

static void app_paint(void *st, CRect c) {
  (void)st;
  if (!P.count) {
    api->fill(c, CAPP_BLACK);
    api->text((short)(c.x + 4), (short)(c.y + 4), P.status, CAPP_WHITE, CAPP_BLACK);
    api->text((short)(c.x + 4), (short)(c.y + 16),
              "raw RGB565, .img or .565", CAPP_GREY, CAPP_BLACK);
    return;
  }
  if (!show(c)) {
    api->fill(c, CAPP_BLACK);
    api->text((short)(c.x + 4), (short)(c.y + 4), P.status, CAPP_RED, CAPP_BLACK);
    return;
  }
  /* Caption over the bottom of the picture, where it is least missed. */
  api->text((short)(c.x + 2), (short)(c.y + c.h - 9), P.status, CAPP_WHITE, CAPP_BLACK);
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (!P.count) {
    if (k == 'r' || k == 'R') { rescan(); return 1; }
    return 0;
  }
  switch (k) {
  case CAPP_KEY_LEFT:
  case CAPP_KEY_UP:
    P.cur = (P.cur + P.count - 1) % P.count;
    return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_DOWN:
  case ' ':
  case CAPP_KEY_ENTER:
    P.cur = (P.cur + 1) % P.count;
    return 1;
  case 'r': case 'R': rescan(); return 1;
  default: return 0;
  }
}

/* A click on the right half advances, the left half goes back. */
static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)y; (void)button;
  if (!P.count) return 0;
  if (x < SCR_W / 2) P.cur = (P.cur + P.count - 1) % P.count;
  else P.cur = (P.cur + 1) % P.count;
  return 1;
}

static void app_set_file(void *st, const char *path) {
  size_t i, cut = 0;
  (void)st;
  for (i = 0; path[i]; i++) if (path[i] == '/') cut = i;
  if (cut == 0) cut = 1;
  if (cut >= sizeof P.dir) cut = sizeof P.dir - 1;
  api->mem_cpy(P.dir, path, cut);
  P.dir[cut] = 0;
  rescan();
  for (i = 0; i < (size_t)P.count; i++) {
    if (ends_with(path, P.names[i])) { P.cur = (int)i; break; }
  }
}

/* 16x16: a framed landscape -- hill, sun. */
static const unsigned char ICON[CAPP_ICON_BYTES] = {
  0x00, 0x00, 0x7F, 0xFE, 0x40, 0x02, 0x41, 0x82,
  0x43, 0xC2, 0x41, 0x82, 0x40, 0x02, 0x40, 0x02,
  0x40, 0x82, 0x41, 0xC2, 0x43, 0xE2, 0x47, 0xF2,
  0x4F, 0xFA, 0x5F, 0xFE, 0x7F, 0xFE, 0x00, 0x00,
};

static CappApp APP;

const CappApp *capp_register(const CardApi *a) {
  api = a;
  APP.api_version = CAPP_API_VERSION;
  api->mem_cpy(APP.name, "Photos", 7);
  api->mem_cpy(APP.icon, ICON, CAPP_ICON_BYTES);
  APP.fullscreen = 1;
  APP.paint = app_paint;
  APP.key = app_key;
  APP.click = app_click;
  APP.open = app_open;
  APP.set_file = app_set_file;
  APP.wants_text = 0;      /* a list or a board, never a text field */
  APP.state = 0;
  return &APP;
}
