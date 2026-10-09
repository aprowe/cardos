/* Photos: the pictures dragged onto the dashboard, and any picture in /pics.
 *
 * The server keeps each photo with a screen copy and a print copy, made
 * there at upload (server/photos.py) -- the device has no JPEG or PNG
 * decoder and should not grow one. Opening this syncs: the list from the
 * server, then each picture it does not have yet, one a tick so the screen
 * says how far it has got; a photo deleted on the dashboard goes from here
 * too. Synced ones are /pics/<id>.img, their names in /pics/.photos. Any
 * other .img or .565 in /pics is shown as well, and left alone.
 *
 * Images are raw RGB565, already byte-swapped for the panel, optionally behind
 * an 8-byte header:
 *
 *     'C' 'I' 'M' 'G'  u16 width  u16 height
 *
 * A headerless file is assumed to be 240x135, which is the whole screen.
 * Rows are streamed one at a time: a full screen is 65 KB, a row 480 bytes.
 *
 * Printing fetches the print copy -- the picture dithered to the paper's
 * 384 dots as printdoc `%%` lines -- into /cache and hands it to the
 * console's `print FILE`, which can hold a document this app's memory
 * should not.
 */
#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/safefile.h"
#include "apps/syncset.h"
#include "apps/confirm.h"

#define SCR_W 240
#define SCR_H 135
#define MAXPICS 48
#define NAMELEN 28
#define DIR "/pics"
#define INDEX DIR "/.photos"
#define PRINT_PATH "/cache/photo-print.txt"
#define SHOW_MS 5000                /* a slide */
#define CAPTION_MS 2500             /* the caption after a change */

static const CardApi *api;

enum { SYNC_IDLE, SYNC_LIST, SYNC_GET };
enum { ACT_PRINT = 1, ACT_SYNC, ACT_SHOW, ACT_DELETE };

static struct {
  char  dir[48];
  char  files[MAXPICS][NAMELEN];   /* in DIR */
  int   count, cur;
  /* The synced ones: server id and name, from INDEX. */
  char  id[MAXPICS][12];
  char  name[MAXPICS][NAMELEN];
  int   nsynced;
  /* The sync under way: which ids still to fetch. */
  int   sync, want[MAXPICS], nwant, got;
  char  reply[MAXPICS * 64];
  char  status[48];
  int   bad;
  int   show;                      /* slideshow running */
  uint32_t next_slide, caption_until;
  int   ask_delete;
  CRect at;                        /* the rect the last paint was given */
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

static void say(int bad, const char *s) {
  api->fmt(P.status, sizeof P.status, "%s", s);
  P.bad = bad;
  P.caption_until = api->ticks_ms() + CAPTION_MS * 2;
}

/* ---- the index of synced photos --------------------------------------------------- */

static void index_load(void) {
  static char buf[MAXPICS * 44];
  int fd = safe_open_read(api, INDEX), n, i = 0;
  P.nsynced = 0;
  if (fd < 0) return;
  n = api->read(fd, buf, sizeof buf - 1);
  api->close(fd);
  if (n <= 0) return;
  buf[n] = 0;
  while (buf[i] && P.nsynced < MAXPICS) {
    int k = 0;
    while (buf[i] && buf[i] != '\t' && buf[i] != '\n' && k < 11) P.id[P.nsynced][k++] = buf[i++];
    P.id[P.nsynced][k] = 0;
    while (buf[i] && buf[i] != '\t' && buf[i] != '\n') i++;
    k = 0;
    if (buf[i] == '\t') {
      i++;
      while (buf[i] && buf[i] != '\n' && k < NAMELEN - 1) P.name[P.nsynced][k++] = buf[i++];
    }
    P.name[P.nsynced][k] = 0;
    while (buf[i] && buf[i] != '\n') i++;
    if (buf[i]) i++;
    if (P.id[P.nsynced][0]) P.nsynced++;
  }
}

static void index_save(void) {
  SafeFile f;
  int i;
  if (safe_begin(&f, api, INDEX) != 0) return;
  for (i = 0; i < P.nsynced; i++) {
    char line[48];
    int n = api->fmt(line, sizeof line, "%s\t%s\n", P.id[i], P.name[i]);
    safe_write(&f, line, (size_t)n);
  }
  safe_commit(&f);
}

/* The synced photo a file is, or -1. */
static int synced_at(const char *file) {
  int i;
  if (!str_same(P.dir, DIR)) return -1;     /* opened on a picture elsewhere */
  for (i = 0; i < P.nsynced; i++) {
    char want[24];
    api->fmt(want, sizeof want, "%s.img", P.id[i]);
    if (str_same(file, want)) return i;
  }
  return -1;
}

static const char *title_of(int at) {
  int s = synced_at(P.files[at]);
  return s >= 0 ? P.name[s] : P.files[at];
}

static void rescan(void) {
  static char raw[MAXPICS][NAMELEN];
  char keep[NAMELEN];
  int n, i;
  keep[0] = 0;
  if (P.count) api->fmt(keep, sizeof keep, "%s", P.files[P.cur]);
  P.count = 0;
  n = api->list(P.dir, &raw[0][0], MAXPICS, NAMELEN);
  if (n < 0) { api->mkdir(P.dir); n = 0; }
  for (i = 0; i < n && P.count < MAXPICS; i++) {
    if (!ends_with(raw[i], ".img") && !ends_with(raw[i], ".565")) continue;
    api->fmt(P.files[P.count], NAMELEN, "%s", raw[i]);
    P.count++;
  }
  P.cur = 0;
  for (i = 0; i < P.count; i++) if (str_same(P.files[i], keep)) P.cur = i;
}

/* ---- sync ------------------------------------------------------------------------- */

static void sync_begin(void) {
  if (P.sync != SYNC_IDLE) return;
  if (!api->net_ready() && api->net_connect(15000) != 0) { say(0, "offline: showing what is here"); return; }
  P.sync = SYNC_LIST;
  say(0, "syncing...");
}

/* Each id in the reply, in order, with its name. */
static void sync_list(void) {
  char url[160], path[64];
  int r, i, n = 0, whole, may_delete, kept;
  static char ids[MAXPICS][12], names[MAXPICS][NAMELEN];
  api->fmt(url, sizeof url, "%s/photos", api->proxy());
  r = api->http("GET", url, 0, 0, "", P.reply, sizeof P.reply, 15000);
  if (r < 0 || (P.reply[0] == 'e' && P.reply[1] == 'r')) {
    api->fmt(P.status, sizeof P.status, "the server did not answer (%d)", r);
    P.bad = 1;
    P.sync = SYNC_IDLE;
    return;
  }
  whole = sync_reply(P.reply, r, (int)sizeof P.reply);
  for (i = 0; P.reply[i] && n < MAXPICS; ) {
    int k = 0;
    while (P.reply[i] && P.reply[i] != '\t' && P.reply[i] != '\n' && k < 11) ids[n][k++] = P.reply[i++];
    ids[n][k] = 0;
    while (P.reply[i] && P.reply[i] != '\t' && P.reply[i] != '\n') i++;
    k = 0;
    if (P.reply[i] == '\t') {
      i++;
      while (P.reply[i] && P.reply[i] != '\t' && P.reply[i] != '\n' && k < NAMELEN - 1) names[n][k++] = P.reply[i++];
    }
    names[n][k] = 0;
    while (P.reply[i] && P.reply[i] != '\n') i++;
    if (P.reply[i]) i++;
    if (ids[n][0]) n++;
  }
  /* Gone from the server: gone from here -- but only against the whole
   * list. One that filled the reply or ran to MAXPICS is not all of it:
   * what it leaves out stays, on the card and in the index
   * (apps/syncset.h). */
  may_delete = sync_may_delete(whole, n, MAXPICS);
  kept = 0;
  for (i = 0; i < P.nsynced; i++) {
    if (sync_listed(P.id[i], &ids[0][0], n, (int)sizeof ids[0])) continue;
    if (may_delete) {
      api->fmt(path, sizeof path, DIR "/%s.img", P.id[i]);
      api->remove(path);
    } else if (n + kept < MAXPICS) {
      if (kept != i) {
        api->mem_cpy(P.id[kept], P.id[i], sizeof P.id[0]);
        api->mem_cpy(P.name[kept], P.name[i], sizeof P.name[0]);
      }
      kept++;
    }
  }
  /* The new list after what was kept, and what of it is not here yet. */
  P.nsynced = kept + n;
  P.nwant = 0;
  for (i = kept; i < P.nsynced; i++) {
    CappStat st;
    api->mem_cpy(P.id[i], ids[i - kept], sizeof P.id[i]);
    api->mem_cpy(P.name[i], names[i - kept], sizeof P.name[i]);
    api->fmt(path, sizeof path, DIR "/%s.img", P.id[i]);
    if (api->stat(path, &st) != 0) P.want[P.nwant++] = i;
  }
  index_save();
  P.got = 0;
  P.sync = P.nwant ? SYNC_GET : SYNC_IDLE;
  if (!P.nwant) { rescan(); say(0, n ? "up to date" : "no photos: add some on the dashboard"); }
}

static void sync_get(void) {
  char url[160], path[64];
  int i = P.want[P.got];
  api->fmt(P.status, sizeof P.status, "getting %d of %d", P.got + 1, P.nwant);
  api->fmt(url, sizeof url, "%s/photos/img?id=%s", api->proxy(), P.id[i]);
  api->fmt(path, sizeof path, DIR "/%s.img", P.id[i]);
  if (api->http_download(url, path, 30000) < 0) {
    api->remove(path);                       /* not half a picture */
    say(1, "a download failed: r to try again");
    P.sync = SYNC_IDLE;
    rescan();
    return;
  }
  if (++P.got >= P.nwant) {
    P.sync = SYNC_IDLE;
    rescan();
    api->fmt(P.status, sizeof P.status, "%d new", P.nwant);
    P.caption_until = api->ticks_ms() + CAPTION_MS;
  }
}

/* ---- print and delete ------------------------------------------------------------- */

static void print_current(void) {
  char url[160], out[96];
  int s;
  if (!P.count) return;
  s = synced_at(P.files[P.cur]);
  if (s < 0) { say(1, "only photos from the dashboard print"); return; }
  if (!api->net_ready() && api->net_connect(15000) != 0) { say(1, "offline: printing needs the server"); return; }
  api->fmt(url, sizeof url, "%s/photos/print?id=%s", api->proxy(), P.id[s]);
  if (api->http_download(url, PRINT_PATH, 30000) < 0) { say(1, "could not fetch the print copy"); return; }
  out[0] = 0;
  api->shell("print " PRINT_PATH, out, sizeof out);
  /* "printing /cache/photo-print.txt (19 KB)" says nothing a person wants;
   * anything else is the console saying what went wrong. */
  if (out[0] == 'p' && out[1] == 'r' && out[2] == 'i' && out[3] == 'n' && out[4] == 't' &&
      out[5] == 'i')
    say(0, "printing");
  else {
    int i;
    for (i = 0; out[i]; i++) if (out[i] == '\n') out[i] = 0;
    say(1, out[0] ? out : "the printer did not answer");
  }
}

static void delete_current(void) {
  char url[160], path[64], reply[48];
  int s = synced_at(P.files[P.cur]);
  if (s >= 0) {
    api->fmt(url, sizeof url, "%s/photos/photo?id=%s", api->proxy(), P.id[s]);
    if (!api->net_ready() || api->http("DELETE", url, 0, 0, "", reply, sizeof reply, 15000) < 0) {
      say(1, "offline: delete it on the dashboard");
      return;
    }
  }
  api->fmt(path, sizeof path, "%s/%s", P.dir, P.files[P.cur]);
  api->remove(path);
  rescan();
  say(0, "deleted");
}

/* ---- the screen ------------------------------------------------------------------- */

/* Where the status is: the caption strip over a picture, or the status line
 * of the empty screen. A status change marks only this, so the shell clips
 * the repaint to it -- a sync step used to re-read and redraw the whole
 * picture from the card, and every step of a sync is a status change. */
static CRect status_rect(void) {
  CRect c = P.at;
  if (P.count) return rect(c.x, c.y + c.h - 9, c.w, 9);
  return rect(c.x, c.y + 22, c.w, 8);
}

static int status_changed(void) {
  api->damage(status_rect());
  return 1;
}

/* r, less the part `hole` covers: up to four fills round it. The hole is
 * where text is about to go, and text paints its own background -- filling
 * under it first is a blink of the text on every repaint. */
static void fill_round(CRect r, CRect hole, uint16_t colour) {
  int x0 = hole.x > r.x ? hole.x : r.x;
  int y0 = hole.y > r.y ? hole.y : r.y;
  int x1 = hole.x + hole.w < r.x + r.w ? hole.x + hole.w : r.x + r.w;
  int y1 = hole.y + hole.h < r.y + r.h ? hole.y + hole.h : r.y + r.h;
  if (r.w <= 0 || r.h <= 0) return;
  if (x0 >= x1 || y0 >= y1) { api->fill(r, colour); return; }
  if (y0 > r.y) api->fill(rect(r.x, r.y, r.w, y0 - r.y), colour);
  if (y1 < r.y + r.h) api->fill(rect(r.x, y1, r.w, r.y + r.h - y1), colour);
  if (x0 > r.x) api->fill(rect(r.x, y0, x0 - r.x, y1 - y0), colour);
  if (x1 < r.x + r.w) api->fill(rect(x1, y0, r.x + r.w - x1, y1 - y0), colour);
}

/* Draws the current picture centred, the bands it does not cover black, and
 * nothing under `hole` -- the caption, drawn next, would cover it anyway, and
 * a picture row under the caption and then the caption over it blinks the
 * caption. Only the rows the paint is repairing are read: a caption that
 * comes or goes reads nine rows of the file, not 135.
 * Returns 0 if the file could not be shown. */
static int show(CRect c, CRect hole) {
  char path[96];
  unsigned char hdr[8];
  int fd, w, h, ox, oy, y, y0, y1, base = 8;
  CRect a = api->paint_area(), b;

  api->fmt(path, sizeof path, "%s/%s", P.dir, P.files[P.cur]);
  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) return 0;
  if (api->read(fd, hdr, 8) != 8) { api->close(fd); return 0; }
  if (hdr[0] == 'C' && hdr[1] == 'I' && hdr[2] == 'M' && hdr[3] == 'G') {
    w = hdr[4] | (hdr[5] << 8);
    h = hdr[6] | (hdr[7] << 8);
  } else {
    w = SCR_W;
    h = SCR_H;
    base = 0;                    /* headerless: those were pixels */
  }
  if (w <= 0 || h <= 0 || w > SCR_W || h > SCR_H) { api->close(fd); return 0; }

  ox = c.x + (c.w - w) / 2;
  oy = c.y + (c.h - h) / 2;
  if (w < c.w || h < c.h) {
    /* Four bands round the picture, rather than the whole screen black and
     * the picture over it: that was a black flash on every repaint. */
    b = rect(ox, oy, w, h);
    fill_round(rect(c.x, c.y, c.w, b.y - c.y), hole, CAPP_BLACK);
    fill_round(rect(c.x, b.y + h, c.w, c.y + c.h - b.y - h), hole, CAPP_BLACK);
    fill_round(rect(c.x, b.y, b.x - c.x, h), hole, CAPP_BLACK);
    fill_round(rect(b.x + w, b.y, c.x + c.w - b.x - w, h), hole, CAPP_BLACK);
  }
  y0 = a.y - oy;
  y1 = a.y + a.h - oy;
  if (y0 < 0) y0 = 0;
  if (y1 > h) y1 = h;
  if (y0 < y1 && api->seek(fd, (int32_t)(base + y0 * w * 2), 0) >= 0) {
    for (y = y0; y < y1; y++) {
      int sy = oy + y, hx0 = hole.x, hx1 = hole.x + hole.w;
      if (api->read(fd, P.row, (size_t)w * 2) != w * 2) break;
      if (hole.w <= 0 || sy < hole.y || sy >= hole.y + hole.h) {
        api->pixels(rect(ox, sy, w, 1), P.row);
        continue;
      }
      if (hx0 > ox + w) hx0 = ox + w;
      if (hx1 < ox) hx1 = ox;
      if (hx0 > ox) api->pixels(rect(ox, sy, hx0 - ox, 1), P.row);
      if (hx1 < ox + w) api->pixels(rect(hx1, sy, ox + w - hx1, 1), P.row + (hx1 - ox));
    }
  }
  api->close(fd);
  return 1;
}

/* s, padded with spaces to n characters: a line drawn this way covers the
 * longer one it replaces, so nothing is cleared before it. */
static void padded(char *out, int n, const char *s) {
  int i = 0;
  while (s[i] && i < n) { out[i] = s[i]; i++; }
  while (i < n) out[i++] = ' ';
  out[n] = 0;
}

/* No pictures: five lines, each drawn over its old self and padded to the
 * edge, and only the gaps between them filled. */
static void paint_empty(CRect c) {
  static const int Y[5] = { 6, 22, 40, 50, 66 };
  const char *s[5];
  char line[64];
  int i, prev = 0, n = (c.w - 6) / 6;
  if (n > (int)sizeof line - 1) n = sizeof line - 1;
  s[0] = "Photos";
  s[1] = P.sync ? P.status : P.status[0] ? P.status : "no photos yet";
  s[2] = "drag pictures onto the";
  s[3] = "dashboard's Photos page";
  s[4] = "r syncs";
  for (i = 0; i < 5; i++) {
    uint16_t fg = i == 0 ? CAPP_WHITE : i == 1 && P.bad ? CAPP_RED : CAPP_GREY;
    api->fill(rect(c.x, c.y + prev, c.w, Y[i] - prev), CAPP_BLACK);
    api->fill(rect(c.x, c.y + Y[i], 6, 8), CAPP_BLACK);
    padded(line, n, s[i]);
    api->text((short)(c.x + 6), (short)(c.y + Y[i]), line, fg, CAPP_BLACK);
    if (6 + n * 6 < c.w)
      api->fill(rect(c.x + 6 + n * 6, c.y + Y[i], c.w - 6 - n * 6, 8), CAPP_BLACK);
    prev = Y[i] + 8;
  }
  api->fill(rect(c.x, c.y + prev, c.w, c.h - prev), CAPP_BLACK);
}

static void app_paint(void *st, CRect c) {
  char line[64];
  int showing_caption = (int32_t)(api->ticks_ms() - P.caption_until) < 0;
  int caption = 1, len;
  uint16_t fg = CAPP_WHITE, bg = CAPP_BLACK;
  CRect hole;
  (void)st;
  P.at = c;
  if (!P.count) { paint_empty(c); return; }
  /* A caption over the bottom, for a moment after a change -- not over
   * the picture all the time -- and always while asking or syncing. */
  if (P.ask_delete) {
    api->fmt(line, sizeof line, "delete %s? y yes  n no", title_of(P.cur));
    bg = CAPP_RED;
  } else if (P.sync || showing_caption) {
    if (P.sync || P.status[0])
      api->fmt(line, sizeof line, "%s", P.status);
    else
      api->fmt(line, sizeof line, "%d/%d %s%s", P.cur + 1, P.count, title_of(P.cur),
               P.show ? "  (slideshow)" : "");
    if (P.bad) fg = CAPP_RED;
  } else {
    caption = 0;
  }
  len = caption ? (int)api->str_len(line) * 6 : 0;
  if (len > c.w - 2) len = c.w - 2;
  hole = rect(c.x + 2, c.y + c.h - 9, len, 8);
  if (!show(c, hole)) {
    api->fill(c, CAPP_BLACK);
    api->fmt(line, sizeof line, "cannot show %s", P.files[P.cur]);
    api->text((short)(c.x + 4), (short)(c.y + 4), line, CAPP_RED, CAPP_BLACK);
    return;
  }
  if (caption) api->text((short)(c.x + 2), (short)(c.y + c.h - 9), line, fg, bg);
}

static void moved(int by) {
  if (!P.count) return;
  P.cur = (P.cur + by + P.count) % P.count;
  P.status[0] = 0;
  P.bad = 0;
  P.caption_until = api->ticks_ms() + CAPTION_MS;
  if (P.show) P.next_slide = api->ticks_ms() + SHOW_MS;
}

static void slideshow(int on) {
  P.show = on && P.count > 1;
  api->keep_awake(P.show);         /* a slideshow nobody touches must not go dark */
  P.next_slide = api->ticks_ms() + SHOW_MS;
  say(0, P.show ? "slideshow: space stops" : "slideshow stopped");
}

static int do_action(int a) {
  switch (a) {
  /* Each of these changes the caption and nothing else. */
  case ACT_PRINT:  print_current(); return status_changed();
  case ACT_SYNC:   sync_begin(); return status_changed();
  case ACT_SHOW:   slideshow(!P.show); return status_changed();
  case ACT_DELETE: if (P.count) P.ask_delete = 1; return status_changed();
  }
  return 0;
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (P.ask_delete) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_WAIT) return 1;
    P.ask_delete = 0;
    if (a == CONFIRM_YES) { delete_current(); return 1; }
    return status_changed();     /* the question goes; the picture stays */
  }
  switch (k) {
  case CAPP_KEY_LEFT:
  case CAPP_KEY_UP:     moved(-1); return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_DOWN:
  case CAPP_KEY_ENTER:  moved(1); return 1;
  case ' ':             return do_action(ACT_SHOW);
  case 'p': case 'P':   return do_action(ACT_PRINT);
  case 'r': case 'R':   return do_action(ACT_SYNC);
  case 'd': case 'D':
  case 0x7F:           return do_action(ACT_DELETE);   /* Delete, on a Bluetooth keyboard */
  case CAPP_KEY_ESC:
    if (P.show) { slideshow(0); return status_changed(); }
    return 0;
  }
  return 0;
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int app_tick(void *st, uint32_t now) {
  (void)st;
  if (P.sync != SYNC_IDLE) {
    /* A step changes the status; only when it changes which pictures there
     * are, or which one is on screen, is the whole screen repainted. */
    char keep[NAMELEN];
    int before = P.count;
    keep[0] = 0;
    if (P.count) api->fmt(keep, sizeof keep, "%s", P.files[P.cur]);
    if (P.sync == SYNC_LIST) sync_list();
    else sync_get();
    if (P.count != before || (P.count && !str_same(P.files[P.cur], keep))) return 1;
    return status_changed();
  }
  if (P.show && P.count > 1 && (int32_t)(now - P.next_slide) >= 0) {
    P.cur = (P.cur + 1) % P.count;
    P.next_slide = now + SHOW_MS;
    return 1;
  }
  /* The caption goes when its time is up: one repaint, then nothing. */
  if (P.caption_until && (int32_t)(now - P.caption_until) >= 0) {
    P.caption_until = 0;
    if (!P.sync) P.status[0] = 0;
    return status_changed();
  }
  return 0;
}

/* A click on the right half advances, the left half goes back. */
static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)y; (void)button;
  moved(x < SCR_W / 2 ? -1 : 1);
  return 1;
}

static const CappAction ACTIONS[] = {
  { "print", "Print", 0, CAPP_KEY_PRINT, ACT_PRINT, 0, 0, 0, 0 },
  { "sync", "Sync", 0, 0, ACT_SYNC, 0, 0, 0, 0 },
  { "slideshow", "Slideshow", 0, 0, ACT_SHOW, 0, 0, 0, 0 },
  { "delete", "Delete", 0, 0, ACT_DELETE, 0, 0, 0, 0 },
};

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_PAINT_DIRECT,   /* paint reads the picture off the card */
  "Photos",
  /* 16x16: a framed landscape -- hill, sun. */
  { 0x00, 0x00, 0x7F, 0xFE, 0x40, 0x02, 0x41, 0x82,
    0x43, 0xC2, 0x41, 0x82, 0x40, 0x02, 0x40, 0x02,
    0x40, 0x82, 0x41, 0xC2, 0x43, 0xE2, 0x47, 0xF2,
    0x4F, 0xFA, 0x5F, 0xFE, 0x7F, 0xFE, 0x00, 0x00 },
  "arrows\tprevious and next\nenter\tnext\nspace\tslideshow on and off\np\tprint this one\n"
  "r\tsync with the dashboard\nd, del\tdelete (asks; from the dashboard too)\n"
  "esc\tstop the slideshow\n"
  "\n"
  "pictures come from the dashboard's Photos page.\n",
};

/* Static, not a local: the shell keeps calling into this long after
 * capp_main has returned. */
static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  P.at = rect(0, 0, SCR_W, SCR_H);   /* until the first paint says otherwise */
  api->fmt(P.dir, sizeof P.dir, "%s", DIR);
  index_load();
  /* An argument names a picture to start on -- in /pics, or anywhere: Files
   * opens an .img where it lies, and then that folder is the one shown and
   * nothing is synced into it. */
  if (argc > 1) {
    int i, slash = -1;
    const char *name = argv[1];
    for (i = 0; argv[1][i]; i++) if (argv[1][i] == '/') slash = i;
    if (slash > 0) {
      api->fmt(P.dir, sizeof P.dir, "%.*s", slash, argv[1]);
      name = argv[1] + slash + 1;
    } else if (slash == 0) {
      api->fmt(P.dir, sizeof P.dir, "/");
      name = argv[1] + 1;
    }
    rescan();
    for (i = 0; i < P.count; i++) if (str_same(name, P.files[i])) P.cur = i;
  } else {
    rescan();
  }
  P.sync = SYNC_IDLE;
  if (str_same(P.dir, DIR)) sync_begin();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.tick = app_tick;
  UI.actions = ACTIONS;
  UI.nactions = sizeof ACTIONS / sizeof ACTIONS[0];
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
