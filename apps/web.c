/* Web -- scroll a rendered web page.
 *
 * There is no HTML parser here and there never will be: layout is a constraint
 * solver over a box model and there is no small version of it. The layout
 * happens on a machine with a real browser, and this is sent pixels.
 *
 * The server (server/render/) renders the page with Chrome at a 240-pixel viewport and
 * re-typesets every run of text in this machine's own 6x8 font before taking
 * the shot -- which is the whole trick. A page rendered wide and scaled down
 * turns antialiased text into grey mush; a bitmap font laid out at its native
 * size on the pixel grid is exactly as crisp in the screenshot as it is on the
 * console, and a screenful is a paragraph rather than four words. The result is
 * encoded as a .cpx: RLE rows of RGB565, with an index so any row can be found
 * without reading the ones before it.
 *
 * So this app decodes one row at a time and blits it. It holds a single row --
 * 480 bytes -- rather than a page, which is what lets it scroll something
 * 4000 pixels tall on a board with no framebuffer.
 *
 * Two ways to render, because they are a taste decision rather than a right
 * answer, and f switches between them without retyping the address:
 *
 *   font   the page in this machine's 6x8 font, crisp, about 34 columns
 *   photo  the page in its own fonts, rendered wide and scaled to 240 -- text
 *          goes soft, but a page with photographs in it looks like itself
 *
 *     web                     the page already on the card
 *     web <url>               through the proxy in PROXY
 *     web -p <url>            photo mode
 *     web <proxy-url>         anything ending in .cpx is fetched directly
 */

#include "kernel/app/capp.h"

#define PAGE_PATH  "/cache/page.cpx"
#define MAXW       240
#define ROWBUF     700          /* a row is at most 240*2 plus its run bytes */
#define SCROLL_STEP 12
#define BAR_H      9
#define ENTRY_H    12

#define CLR_BG    CAPP_RGB(16, 18, 22)
#define CLR_BAR   CAPP_RGB(32, 48, 78)
#define CLR_FG    CAPP_RGB(226, 232, 242)
#define CLR_DIM   CAPP_RGB(132, 142, 158)
#define CLR_ENTRY CAPP_RGB(24, 36, 60)
#define CLR_LINK  CAPP_RGB(90, 150, 240)

static const CardApi *api;

static struct {
  int  fd;
  int  w, h;
  uint32_t rows_off, links_off;
  int  top;                     /* first visible row */
  int  loaded;
  int  photo;                   /* render mode: 0 the 6x8 font, 1 real fonts */
  char status[56];
  char url[160];
  char proxy[96];

  uint32_t nlinks;
  int      typing;              /* the address bar has the keyboard */
  char     entry[160];
  int      entry_len;

  uint8_t  raw[ROWBUF];
  uint16_t row[MAXW];
} W;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *s) { api->fmt(W.status, sizeof W.status, "%s", s); }

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- the file ------------------------------------------------------------ */

static void close_page(void) {
  if (W.fd >= 0) api->close(W.fd);
  W.fd = -1;
  W.loaded = 0;
}

static int open_page(void) {
  uint8_t head[20];

  close_page();
  W.fd = api->open(PAGE_PATH, CAPP_O_READ);
  if (W.fd < 0) { say("no page on the card -- web <url>"); return 0; }

  if (api->read(W.fd, head, 20) != 20 ||
      head[0] != 'C' || head[1] != 'P' || head[2] != 'X' || head[3] != '1') {
    say("not a rendered page");
    close_page();
    return 0;
  }

  W.w = rd16(head + 4);
  W.h = rd16(head + 6);
  W.nlinks = rd16(head + 8);
  W.rows_off = rd32(head + 12);
  W.links_off = rd32(head + 16);

  if (W.w != MAXW || W.h < 1) { say("wrong page size"); close_page(); return 0; }

  W.top = 0;
  W.loaded = 1;
  api->fmt(W.status, sizeof W.status, "%d rows", W.h);
  return 1;
}

/* Where row `i` starts, from the index. One four-byte read rather than a
 * resident table: 4000 rows would be 16 KB of index and the file is right
 * there. */
static uint32_t row_offset(int i) {
  uint8_t buf[4];
  if (api->seek(W.fd, (int32_t)(20 + 4 * i), 0) < 0) return 0;
  if (api->read(W.fd, buf, 4) != 4) return 0;
  return rd32(buf);
}

/* Expand one RLE row into W.row. Returns 0 if the row is malformed, which for
 * a file off a removable card is a thing that happens. */
static int decode_row(int len) {
  int i = 0, out = 0;

  while (i < len && out < MAXW) {
    uint8_t c = W.raw[i++];
    if (c < 128) {
      int n = c + 1;
      uint16_t px;
      if (i + 2 > len) return 0;
      px = rd16(W.raw + i);
      i += 2;
      while (n-- > 0 && out < MAXW) W.row[out++] = px;
    } else {
      int n = c - 127;
      if (i + 2 * n > len) return 0;
      while (n-- > 0 && out < MAXW) { W.row[out++] = rd16(W.raw + i); i += 2; }
    }
  }
  while (out < MAXW) W.row[out++] = 0;
  return 1;
}

/* Which link, if any, covers a point on the page.
 *
 * The table is read off the card rather than held: 400 links with their URLs
 * is 30 KB and this app is meant to run in what is left after a radio. Each
 * entry is ten bytes plus the URL, laid out in order, so finding one is a walk
 * -- and a walk over a few hundred entries between clicks costs nothing.
 *
 * Returns the URL in `out`, or 0 if the point is not on a link. */
static int link_at(int px, int py, char *out, size_t out_size) {
  uint8_t rec[10];
  uint32_t i;
  uint32_t off = W.links_off;

  if (!W.nlinks || W.fd < 0) return 0;
  if (api->seek(W.fd, (int32_t)off, 0) < 0) return 0;

  for (i = 0; i < W.nlinks; i++) {
    int lx, ly, lw, lh, n;
    if (api->read(W.fd, rec, 10) != 10) return 0;
    lx = rd16(rec); ly = rd16(rec + 2);
    lw = rd16(rec + 4); lh = rd16(rec + 6);
    n = rd16(rec + 8);
    if (n <= 0 || n > 250) return 0;

    if (px >= lx && px < lx + lw && py >= ly && py < ly + lh) {
      size_t take = (size_t)n < out_size - 1 ? (size_t)n : out_size - 1;
      if (api->read(W.fd, out, take) != (int)take) return 0;
      out[take] = 0;
      return 1;
    }
    if (api->seek(W.fd, (int32_t)n, 1) < 0) return 0;   /* skip the URL */
  }
  return 0;
}

/* ---- painting ------------------------------------------------------------ */

static void paint_bar(CRect c) {
  char buf[72];
  int pct = W.h > 1 ? (W.top * 100) / (W.h - 1) : 0;

  api->fill(rect(c.x, c.y + c.h - BAR_H, c.w, BAR_H), CLR_BAR);
  api->fmt(buf, sizeof buf, "%3d%%  %s  [u]rl [f]%s", pct, W.status,
           W.photo ? "ont" : "oto");
  api->text((short)(c.x + 2), (short)(c.y + c.h - BAR_H + 1), buf, CLR_FG, CLR_BAR);
}

/* The address bar. It is only on screen while it has the keyboard, because a
 * permanent one would cost eleven of the panel's hundred and thirty-five rows
 * to show something that changes once a page. */
static void paint_entry(CRect c) {
  int cols = (c.w - 8) / 6;
  int from = W.entry_len > cols - 1 ? W.entry_len - cols + 1 : 0;

  api->fill(rect(c.x, c.y, c.w, ENTRY_H), CLR_ENTRY);
  api->fill(rect(c.x, c.y + ENTRY_H - 1, c.w, 1), CLR_BAR);
  api->text((short)(c.x + 3), (short)(c.y + 2), W.entry + from, CLR_FG, CLR_ENTRY);
  /* A block caret: the address bar is the one place in this app that takes
   * typing, and it should look like it. */
  api->fill(rect(c.x + 3 + (W.entry_len - from) * 6, c.y + 2, 6, 8), CLR_FG);
}

static void app_paint(void *st, CRect c) {
  int view = c.h - BAR_H;
  int y;
  (void)st;

  if (!W.loaded) {
    api->fill(c, CLR_BG);
    api->text((short)(c.x + 6), (short)(c.y + 8), "Web", CLR_FG, CLR_BG);
    api->text((short)(c.x + 6), (short)(c.y + 24), W.status, CLR_DIM, CLR_BG);
    api->text((short)(c.x + 6), (short)(c.y + 40), "start the proxy, then", CLR_DIM, CLR_BG);
    api->text((short)(c.x + 6), (short)(c.y + 52), "u types an address", CLR_DIM, CLR_BG);
    paint_bar(c);
    if (W.typing) paint_entry(c);
    return;
  }

  /* One seek, then straight through: the rows are stored in order, so reading
   * a screenful is sequential after the first jump. That is the difference
   * between scrolling and waiting. */
  if (api->seek(W.fd, (int32_t)row_offset(W.top), 0) < 0) {
    say("seek failed");
    return;
  }

  for (y = 0; y < view; y++) {
    uint8_t len16[2];
    int len;

    if (W.top + y >= W.h) {
      api->fill(rect(c.x, c.y + y, c.w, 1), CLR_BG);
      continue;
    }
    if (api->read(W.fd, len16, 2) != 2) break;
    len = rd16(len16);
    if (len <= 0 || len > ROWBUF) break;
    if (api->read(W.fd, W.raw, (size_t)len) != len) break;
    if (!decode_row(len)) break;

    api->pixels(rect(c.x, c.y + y, MAXW, 1), W.row);
  }

  paint_bar(c);
  if (W.typing) paint_entry(c);
}

/* ---- fetching ------------------------------------------------------------ */

static void fetch(const char *what) {
  char url[280];
  int n;

  if (!api->net_ready()) {
    say("connecting to wifi...");
    if (api->net_connect(20000) != 0) { say(api->net_status()); return; }
  }

  /* A .cpx is already rendered, so it is fetched as given. Anything else is a
   * page, and the proxy is what turns one into the other. */
  {
    size_t l = api->str_len(what);
    if (l > 4 && what[l-4] == '.' && what[l-3] == 'c' &&
        what[l-2] == 'p' && what[l-1] == 'x')
      api->fmt(url, sizeof url, "%s", what);
    else if (W.photo)
      /* 480: a desktop page at half size. 720 fits more of the page but the
       * text stops being readable, and a page you cannot read is a picture. */
      api->fmt(url, sizeof url, "%s/render?px=0&w=480&url=%s", W.proxy, what);
    else
      api->fmt(url, sizeof url, "%s/render?url=%s", W.proxy, what);
  }

  close_page();
  say("rendering...");
  /* Long: the proxy is driving a real browser, and a page that needs its
   * JavaScript to settle takes several seconds before the screenshot. */
  n = api->http_download(url, PAGE_PATH, 45000);
  if (n < 0) {
    api->fmt(W.status, sizeof W.status, "fetch failed (%d)", n);
    return;
  }
  api->fmt(W.url, sizeof W.url, "%s", what);
  if (open_page())
    api->fmt(W.status, sizeof W.status, "%s  %d rows, %dK",
             W.photo ? "photo" : "font", W.h, n / 1024);
}

/* ---- input --------------------------------------------------------------- */

static void scroll_by(int rows, int view) {
  int max = W.h - view;
  if (max < 0) max = 0;
  W.top += rows;
  if (W.top < 0) W.top = 0;
  if (W.top > max) W.top = max;
}

/* While the address bar has the keyboard it gets every printable key, and the
 * app says so through wants_text -- otherwise the shell would turn ; . , / into
 * arrows and an address could not contain a full stop. */
static int key_entry(unsigned char k) {
  if (k == CAPP_KEY_ENTER) {
    W.typing = 0;
    if (W.entry_len) fetch(W.entry);
    return 1;
  }
  if (k == CAPP_KEY_BACK) {
    if (W.entry_len) W.entry[--W.entry_len] = 0;
    else W.typing = 0;              /* backspacing off the end puts it away */
    return 1;
  }
  if (k >= ' ' && k < 0x7F && W.entry_len < (int)sizeof W.entry - 1) {
    W.entry[W.entry_len++] = (char)k;
    W.entry[W.entry_len] = 0;
    return 1;
  }
  return 1;                          /* swallow everything: this is a field */
}

static int app_key(void *st, unsigned char k) {
  int view = 135 - BAR_H;
  (void)st;

  if (W.typing) return key_entry(k);

  if (k == 'u' || k == 'U') {
    /* Opens on the page you are looking at, so a small edit to an address is
     * a small edit rather than retyping it. */
    api->fmt(W.entry, sizeof W.entry, "%s", W.url);
    W.entry_len = (int)api->str_len(W.entry);
    W.typing = 1;
    return 1;
  }

  if (!W.loaded) return 0;

  switch (k) {
  case CAPP_KEY_DOWN:  scroll_by(SCROLL_STEP, view); return 1;
  case CAPP_KEY_UP:    scroll_by(-SCROLL_STEP, view); return 1;
  case CAPP_KEY_RIGHT:
  case ' ':            scroll_by(view, view); return 1;
  case CAPP_KEY_LEFT:  scroll_by(-view, view); return 1;
  case 'g':            W.top = 0; return 1;
  case 'G':            scroll_by(W.h, view); return 1;
  case 'r': case 'R':
    if (W.url[0]) { fetch(W.url); return 1; }
    return 0;
  case 'f': case 'F':
    /* The same page the other way round. It has to be fetched again: the
     * choice is made by the renderer, and what arrived is pixels. */
    W.photo = !W.photo;
    if (W.url[0]) fetch(W.url);
    else say(W.photo ? "photo mode -- web <url>" : "font mode -- web <url>");
    return 1;
  default: return 0;
  }
}

static int app_click(void *st, short x, short y, int button) {
  int view = 135 - BAR_H;
  char url[200];
  (void)st; (void)button;
  if (!W.loaded) return 0;

  if (W.typing) { W.typing = 0; return 1; }

  /* A link first: the page knows where its own links are, because the proxy
   * wrote them down while it still had a DOM to ask. */
  if (y < view && link_at(x, W.top + y, url, sizeof url)) {
    fetch(url);
    return 1;
  }

  /* Otherwise the tap scrolls -- top half up, bottom half down. */
  scroll_by(y < view / 2 ? -view / 2 : view / 2, view);
  return 1;
}

static int app_wants_text(void *st) {
  (void)st;
  return W.typing;
}

/* The wheel is the natural way to read a page, and the one gesture this app
 * wanted a mouse for. Three notches to a text line at the sizes the proxy
 * renders. */
static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  int view = 135 - BAR_H;
  (void)st; (void)x; (void)y; (void)buttons;
  if (!wheel || !W.loaded) return 0;
  scroll_by(-wheel * SCROLL_STEP * 2, view);
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_NEEDS_PROXY,   /* a window by default; the title bar's box fills the screen */
  "Web",
  /* 16x16: a globe. */
  { 0x07, 0xE0, 0x18, 0x18, 0x24, 0x24, 0x4A, 0x52,
    0x4A, 0x52, 0x9A, 0x59, 0xFF, 0xFF, 0x9A, 0x59,
    0x9A, 0x59, 0xFF, 0xFF, 0x4A, 0x52, 0x4A, 0x52,
    0x24, 0x24, 0x18, 0x18, 0x07, 0xE0, 0x00, 0x00 },
  "u\ttype an address\nclick\tfollow a link\narrows\tscroll, left/right a page\nspace\tpage down\ng G\ttop, bottom\nr\treload\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;

  api->mem_set(&W, 0, sizeof W);
  W.fd = -1;
  /* Overridable by argument so the proxy can move without a rebuild. */
  api->fmt(W.proxy, sizeof W.proxy, "%s", api->proxy());

  {
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
      if (argv[i][1] == 'p') W.photo = 1;
      else if (argv[i][1] == 'f') W.photo = 0;
    }
    if (i < argc && argv[i][0]) fetch(argv[i]);
    else if (!open_page()) say("no page yet -- web <url>");
  }

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
