/* Stocks -- a watchlist with a week's graph and what a holding is worth.
 *
 * Leads with SPCX because that is the one this was built for. Each row can
 * carry a share count, and the app shows what that many shares come to at the
 * current price -- the number you actually want, rather than a price you then
 * do arithmetic on.
 *
 * Edit /config/stocks.txt to change the list. One per line:
 *
 *     SPCX 10800 SpaceX
 *     RKLB Rocket Lab
 *     ^GSPC 0 S&P 500
 *
 * The second field is a share count if it is a number, and part of the label
 * otherwise -- so a line with no holding needs nothing added to it.
 *
 * Quotes come from Yahoo's chart endpoint, which needs no key and returns both
 * the current price and the week's closes in one request. The reply is JSON
 * and this reads it by looking for field names: a real parser is several
 * kilobytes to extract two numbers and an array from a document this regular.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/toolbar.h"
#include "apps/footer.h"

#define MAX_SYMS   8
#define SYM_LEN    10
#define LABEL_LEN  18
#define HIST_MAX   8          /* a week of trading days, plus room */
#define BUF        2600       /* the 7-day reply measured 1962 bytes */

#define CLR_BG     CAPP_RGB(18, 20, 26)
#define CLR_PANEL  CAPP_RGB(28, 31, 39)
#define CLR_TEXT   CAPP_RGB(224, 230, 240)
#define CLR_DIM    CAPP_RGB(126, 136, 152)
#define CLR_UP     CAPP_RGB(104, 212, 132)
#define CLR_DOWN   CAPP_RGB(238, 100, 92)
#define CLR_LINE   CAPP_RGB(120, 190, 255)
#define CLR_GRID   CAPP_RGB(44, 48, 58)
#define CLR_SEL    CAPP_RGB(46, 72, 108)
#define CLR_HOLD   CAPP_RGB(255, 206, 106)

static const CardApi *api;

typedef struct {
  char sym[SYM_LEN];
  char label[LABEL_LEN];
  long shares;
  int  have;
  long price;               /* cents */
  long prev;                /* cents, the previous close */
  long hist[HIST_MAX];      /* cents, oldest first */
  int  nhist;
} Quote;

static struct {
  Quote q[MAX_SYMS];
  int   n;
  int   sel;
  int   busy;               /* a refresh is under way */
  int   fi;                 /* the symbol it is on */
  int   inflight;           /* its request is out */
  uint32_t fetched_ms;
  char  note[48];
  char  buf[BUF];
  /* Where the last paint put things, so a quote landing can mark its own
   * row rather than the screen. */
  CRect at;
  int   list_y, list_rows;
} S;

/* ---- the list ------------------------------------------------------------ */

static void add(const char *sym, long shares, const char *label) {
  Quote *q;
  if (S.n >= MAX_SYMS) return;
  q = &S.q[S.n++];
  api->mem_set(q, 0, sizeof *q);
  api->fmt(q->sym, sizeof q->sym, "%s", sym);
  api->fmt(q->label, sizeof q->label, "%s", label);
  q->shares = shares;
}

static int all_digits(const char *s) {
  int i = 0;
  if (!s[0]) return 0;
  for (; s[i]; i++) if (s[i] < '0' || s[i] > '9') return 0;
  return 1;
}

static long to_long(const char *s) {
  long v = 0;
  for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
  return v;
}

static void parse_line(char *line) {
  char sym[SYM_LEN], word[LABEL_LEN];
  long shares = 0;
  int i = 0, k = 0;

  if (!line[0] || line[0] == '#') return;

  while (line[i] && line[i] != ' ' && k < SYM_LEN - 1) sym[k++] = line[i++];
  sym[k] = 0;
  while (line[i] == ' ') i++;

  /* A number here is a share count; anything else is the start of the label. */
  k = 0;
  while (line[i + k] && line[i + k] != ' ' && k < LABEL_LEN - 1) { word[k] = line[i + k]; k++; }
  word[k] = 0;
  if (all_digits(word)) {
    shares = to_long(word);
    i += k;
    while (line[i] == ' ') i++;
  }

  if (sym[0]) add(sym, shares, line + i);
}

static void load_list(void) {
  char line[80], rd[128];
  int fd, n, i, len = 0;

  S.n = 0;
  fd = api->open(CAPP_CONFIG "/stocks.txt", CAPP_O_READ);
  if (fd < 0) {
    add("SPCX", 10800, "SpaceX");
    add("RKLB", 0, "Rocket Lab");
    add("ASTS", 0, "AST SpaceMobile");
    add("^GSPC", 0, "S&P 500");
    return;
  }
  while ((n = api->read(fd, rd, sizeof rd)) > 0) {
    for (i = 0; i < n; i++) {
      char c = rd[i];
      if (c != 10 && c != 13) {
        if (len < (int)sizeof line - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      parse_line(line);
      len = 0;
    }
  }
  if (len) { line[len] = 0; parse_line(line); }
  api->close(fd);
  if (S.n == 0) add("SPCX", 10800, "SpaceX");
}

/* ---- reading the reply --------------------------------------------------- */

static const char *find(const char *hay, const char *needle) {
  int nlen = (int)api->str_len(needle), j;
  const char *p = hay;
  while (*p) {
    for (j = 0; j < nlen && p[j] == needle[j]; j++) { }
    if (j == nlen) return p;
    p++;
  }
  return 0;
}

/* A decimal number in cents. Two places is what a price has; the rest is noise
 * from a float that was never exact. */
static const char *read_cents(const char *p, long *out) {
  long whole = 0, frac = 0, scale = 1;
  int neg = 0, seen = 0;

  while (*p == ' ') p++;
  if (*p == '-') { neg = 1; p++; }
  while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p++ - '0'); seen = 1; }
  if (!seen) return 0;
  if (*p == '.') {
    p++;
    while (*p >= '0' && *p <= '9') {
      if (scale < 100) { frac = frac * 10 + (*p - '0'); scale *= 10; }
      p++;
    }
    while (scale < 100) { frac *= 10; scale *= 10; }
  }
  *out = (whole * 100 + frac) * (neg ? -1 : 1);
  return p;
}

static int field_cents(const char *hay, const char *name, long *out) {
  char pat[32];
  const char *p;
  api->fmt(pat, sizeof pat, "\"%s\":", name);
  p = find(hay, pat);
  if (!p) return 0;
  return read_cents(p + api->str_len(pat), out) != 0;
}

/* The close array. Nulls appear for days with no trade and are skipped, which
 * is why the count is returned rather than assumed. */
static int read_closes(const char *hay, long *out, int max) {
  const char *p = find(hay, "\"close\":[");
  int n = 0;

  if (!p) return 0;
  p += 9;
  while (*p && *p != ']' && n < max) {
    if (*p == 'n') {                       /* null */
      while (*p && *p != ',' && *p != ']') p++;
    } else {
      const char *q = read_cents(p, &out[n]);
      if (!q) break;
      n++;
      p = q;
    }
    while (*p == ',' || *p == ' ') p++;
  }
  return n;
}

static void quote_url(const Quote *q, char *url, size_t n) {
  /* One request for both: the meta block carries the current price and the
   * previous close, and the indicators carry the week. */
  api->fmt(url, n,
           "https://query1.finance.yahoo.com/v8/finance/chart/%s"
           "?interval=1d&range=7d", q->sym);
}

/* The reply in S.buf into `q`. 1 if it carried a price. */
static int take_reply(Quote *q) {
  q->have = field_cents(S.buf, "regularMarketPrice", &q->price) &&
            field_cents(S.buf, "chartPreviousClose", &q->prev);
  q->nhist = read_closes(S.buf, q->hist, HIST_MAX);
  return q->have ? 1 : 0;
}

/* Blocking, for a command: the console and voice wait for the answer anyway,
 * and a headless instance has no tick to poll from. */
static int fetch_one(Quote *q) {
  char url[160];
  int n;
  quote_url(q, url, sizeof url);
  n = api->http_get(url, S.buf, sizeof S.buf, 15000);
  if (n <= 0) return n;
  return take_reply(q);
}

/* The screen's refresh is the same requests one at a time off the shell's
 * loop, started here and collected in tick. It used to be fetch_one in a
 * loop: up to fifteen seconds a symbol with the machine frozen, and eight
 * symbols on a slow network was two minutes of a dead keyboard. */
static void start_next(void) {
  char url[160];
  if (S.fi >= S.n) {
    S.busy = 0;
    S.fetched_ms = api->ticks_ms();
    S.note[0] = 0;
    return;
  }
  quote_url(&S.q[S.fi], url, sizeof url);
  api->fmt(S.note, sizeof S.note, "fetching %s (%d/%d)", S.q[S.fi].sym, S.fi + 1, S.n);
  /* Refused means someone else's request is out: tick asks again. */
  S.inflight = api->http_start("GET", url, 0, 0, 0, 15000) == 0;
}

static void refresh(void) {
  if (S.busy) return;

  /* Up front rather than on the first request. The connect takes seconds, and
   * doing it inside a fetch means the screen says "fetching SPCX" while it is
   * really waiting on a radio -- and reports a network error if the wait
   * fails, which is the wrong diagnosis. */
  if (!api->net_ready()) {
    api->fmt(S.note, sizeof S.note, "%s", "connecting to wifi...");
    if (api->net_connect(20000) != 0) {
      api->fmt(S.note, sizeof S.note, "%s", api->net_status());
      return;
    }
  }
  S.busy = 1;
  S.fi = 0;
  start_next();
}

/* Quote i landed (or none, -1: only the note changed). Mark its row, the
 * footer the note is in and the header the age is in -- each landing used
 * to clear and redraw the whole screen. The selected quote is the panel,
 * whose height depends on it, so that one is the whole screen. */
static int landed(int i) {
  CRect c = S.at;
  if (c.w <= 0 || i == S.sel) return 1;
  api->damage(capp_rect(c.x, c.y + c.h - FOOT_H, c.w, FOOT_H));
  api->damage(capp_rect(c.x, c.y, c.w, 13));
  if (i >= 0 && i < S.list_rows) api->damage(capp_rect(c.x, S.list_y + i * 9, c.w, 9));
  return 1;
}

static int app_tick(void *st, uint32_t now) {
  int n;
  (void)st; (void)now;
  toolbar_busy(S.busy);
  if (!S.busy) return 0;
  if (!S.inflight) {
    char url[160];
    quote_url(&S.q[S.fi], url, sizeof url);
    S.inflight = api->http_start("GET", url, 0, 0, 0, 15000) == 0;
    return 0;
  }
  n = api->http_poll(S.buf, sizeof S.buf - 1);
  if (n == CAPP_HTTP_PENDING) return 0;
  S.inflight = 0;
  if (n < 0) {
    api->fmt(S.note, sizeof S.note, "%s: network error %d", S.q[S.fi].sym, n);
    S.busy = 0;
    return landed(-1);
  }
  S.buf[n] = 0;
  take_reply(&S.q[S.fi]);
  S.fi++;
  start_next();
  return landed(S.fi - 1);
}

/* ---- painting ------------------------------------------------------------ */

/* Money with thousands separators. A holding worth 1620540 dollars printed
 * without them is a number you have to count digits on. */
static void money(char *buf, size_t n, long cents, int with_cents) {
  char digits[24];
  long whole = cents / 100;
  int i = 0, k = 0, groups;

  if (whole == 0) { api->fmt(buf, n, with_cents ? "0.%02ld" : "0", cents % 100); return; }
  while (whole > 0) { digits[i++] = (char)('0' + (whole % 10)); whole /= 10; }

  groups = i;
  while (i > 0 && (size_t)k < n - 8) {
    buf[k++] = digits[--i];
    if (i > 0 && (i % 3) == 0) buf[k++] = ',';
  }
  (void)groups;
  buf[k] = 0;
  if (with_cents) {
    char tail[8];
    api->fmt(tail, sizeof tail, ".%02ld", cents % 100 < 0 ? -(cents % 100) : cents % 100);
    api->fmt(buf + k, n - k, "%s", tail);
  }
}

/* The week, as a line. Scaled to its own range rather than to zero: a 5%
 * move on a 150 dollar share is invisible against an axis that starts at
 * nothing, and the shape is the entire reason to draw it. */
/* Nothing on this screen is filled and then drawn over: that is a blink of
 * whatever is drawn second, on a panel with no framebuffer. Text paints its
 * own 6x8 background, so a line of it is padded to the width it owns and
 * only the pixels it leaves are filled. */

/* s in [x0, x1) at y: padded with spaces (or cut) to the whole characters
 * that fit, and the odd pixels after them filled. */
static void seg(int x0, int x1, int y, const char *s, uint16_t fg, uint16_t bg) {
  char line[48];
  int n = (x1 - x0) / 6, i = 0;
  if (n <= 0) { if (x1 > x0) api->fill(capp_rect(x0, y, x1 - x0, 8), bg); return; }
  if (n > (int)sizeof line - 1) n = sizeof line - 1;
  while (s[i] && i < n) { line[i] = s[i]; i++; }
  while (i < n) line[i++] = ' ';
  line[n] = 0;
  api->text((short)x0, (short)y, line, fg, bg);
  if (x0 + n * 6 < x1) api->fill(capp_rect(x0 + n * 6, y, x1 - x0 - n * 6, 8), bg);
}

/* The week, as a line, blitted a row at a time: each row's pixels are
 * worked out -- panel, the dotted previous close, the line -- and sent
 * once, rather than the panel filled and the line drawn over it.
 *
 * Scaled to its own range rather than to zero: a 5% move on a 150 dollar
 * share is invisible against an axis that starts at nothing, and the shape
 * is the entire reason to draw it. */
#define GRAPH_W_MAX 240
static uint8_t s_top[GRAPH_W_MAX], s_bot[GRAPH_W_MAX];   /* the line in each column */
static uint16_t s_row[GRAPH_W_MAX];

static void paint_graph(CRect g, const Quote *q) {
  long lo, hi, span;
  int i, r, ref = -1, tw_hi = 0, tw_lo = 0, w = g.w < GRAPH_W_MAX ? g.w : GRAPH_W_MAX;
  char hs[16], ls[16];

  if (q->nhist < 2) {
    int ty = g.h / 2 - 4;
    api->fill(capp_rect(g.x, g.y, g.w, ty), CLR_PANEL);
    api->fill(capp_rect(g.x, g.y + ty + 8, g.w, g.h - ty - 8), CLR_PANEL);
    seg(g.x, g.x + 6, g.y + ty, "", CLR_DIM, CLR_PANEL);
    seg(g.x + 6, g.x + g.w, g.y + ty, "no history", CLR_DIM, CLR_PANEL);
    return;
  }

  lo = hi = q->hist[0];
  for (i = 1; i < q->nhist; i++) {
    if (q->hist[i] < lo) lo = q->hist[i];
    if (q->hist[i] > hi) hi = q->hist[i];
  }
  span = hi - lo;
  if (span < 1) span = 1;

  /* The previous close as a reference line, so up and down have a meaning
   * beyond the shape of the curve. */
  if (q->prev >= lo && q->prev <= hi) ref = g.h - 1 - (int)((q->prev - lo) * (g.h - 2) / span);

  for (i = 0; i < w; i++) { s_top[i] = 255; s_bot[i] = 0; }
  for (i = 1; i < q->nhist; i++) {
    int x0 = (i - 1) * (g.w - 1) / (q->nhist - 1);
    int x1 = i * (g.w - 1) / (q->nhist - 1);
    int y0 = g.h - 1 - (int)((q->hist[i - 1] - lo) * (g.h - 2) / span);
    int y1 = g.h - 1 - (int)((q->hist[i] - lo) * (g.h - 2) / span);
    int x, steps = x1 - x0;
    if (steps < 1) steps = 1;

    /* A column per x: no diagonal rasteriser needed when the graph is only
     * ever as wide as the screen. */
    for (x = 0; x <= steps; x++) {
      int y = y0 + (y1 - y0) * x / steps;
      int yn = y0 + (y1 - y0) * (x + 1 > steps ? steps : x + 1) / steps;
      int top = y < yn ? y : yn, bot = y < yn ? yn : y, px = x0 + x;
      if (px < 0 || px >= w) continue;
      if (top < s_top[px]) s_top[px] = (uint8_t)top;
      if (bot > s_bot[px]) s_bot[px] = (uint8_t)bot;
    }
  }

  /* The range, so the shape has numbers attached to it -- drawn last, and
   * its rows of the graph sent only to the right of it. */
  money(hs, sizeof hs, hi, 0);
  money(ls, sizeof ls, lo, 0);
  tw_hi = 2 + (int)api->str_len(hs) * 6;
  tw_lo = 2 + (int)api->str_len(ls) * 6;
  for (r = 0; r < g.h; r++) {
    int from = 0;
    if (r >= 1 && r < 9) from = tw_hi;
    else if (r >= g.h - 9 && r < g.h - 1) from = tw_lo;
    if (from >= w) continue;
    for (i = from; i < w; i++) {
      if (s_top[i] <= r && r <= s_bot[i]) s_row[i] = CLR_LINE;
      else if (r == ref && (i % 4) < 2) s_row[i] = CLR_GRID;
      else s_row[i] = CLR_PANEL;
    }
    if (from > 0) api->fill(capp_rect(g.x, g.y + r, 2, 1), CLR_PANEL);
    api->pixels(capp_rect(g.x + from, g.y + r, w - from, 1), s_row + from);
  }
  api->text((short)(g.x + 2), (short)(g.y + 1), hs, CLR_DIM, CLR_PANEL);
  api->text((short)(g.x + 2), (short)(g.y + g.h - 9), ls, CLR_DIM, CLR_PANEL);
}

static void paint_body(CRect c) {
  const Quote *q;
  char buf[48], buf2[32];
  long d;
  uint16_t tone;
  int i, rows, y, x1 = c.x + c.w, end = c.y + c.h - FOOT_H;

  S.at = c;
  S.list_rows = 0;
  /* What it is doing, or what the keys are when it is doing nothing. */
  footer_paint(api, c, S.note[0] ? S.note : "r refresh  e edit list  arrows symbol");
  if (S.n == 0) { api->fill(capp_rect(c.x, c.y, c.w, end - c.y), CLR_BG); return; }
  q = &S.q[S.sel];
  d = q->price - q->prev;
  tone = d < 0 ? CLR_DOWN : CLR_UP;

  /* The symbol and what it is, and how long ago: rows 2..9 of a band of 13. */
  api->fill(capp_rect(c.x, c.y, c.w, 2), CLR_BG);
  api->fill(capp_rect(c.x, c.y + 10, c.w, 3), CLR_BG);
  api->fill(capp_rect(c.x, c.y + 2, 3, 8), CLR_BG);
  api->fmt(buf, sizeof buf, "%s ", q->sym);
  seg(c.x + 3, c.x + 3 + (int)api->str_len(buf) * 6, c.y + 2, buf, CLR_TEXT, CLR_BG);
  seg(c.x + 3 + (int)api->str_len(buf) * 6, x1 - 30, c.y + 2, q->label, CLR_DIM, CLR_BG);
  buf[0] = 0;
  if (S.fetched_ms)
    api->fmt(buf, sizeof buf, "%us", (unsigned)((api->ticks_ms() - S.fetched_ms) / 1000u));
  seg(x1 - 30, x1, c.y + 2, buf, CLR_DIM, CLR_BG);

  /* The price, large, with the change beside it: rows 13..20 of 13..24. */
  api->fill(capp_rect(c.x, c.y + 13, 3, 8), CLR_BG);
  api->fill(capp_rect(c.x, c.y + 21, c.w, 3), CLR_BG);
  if (!q->have) {
    seg(c.x + 3, x1, c.y + 13, "--", CLR_DIM, CLR_BG);
  } else {
    money(buf, sizeof buf, q->price, 1);
    seg(c.x + 3, c.x + 63, c.y + 13, buf, CLR_TEXT, CLR_BG);
    {
      long pct = q->prev ? (d * 1000) / q->prev : 0;
      long ap = pct < 0 ? -pct : pct;
      long ad = d < 0 ? -d : d;
      money(buf2, sizeof buf2, ad, 1);
      api->fmt(buf, sizeof buf, "%c%s  %c%ld.%ld%%",
               d < 0 ? '-' : '+', buf2, d < 0 ? '-' : '+', ap / 10, ap % 10);
      seg(c.x + 63, x1, c.y + 13, buf, tone, CLR_BG);
    }
  }

  /* The graph, its margins, and the three rows under it. */
  api->fill(capp_rect(c.x, c.y + 24, 2, 46), CLR_BG);
  api->fill(capp_rect(x1 - 2, c.y + 24, 2, 46), CLR_BG);
  api->fill(capp_rect(c.x, c.y + 70, c.w, 3), CLR_BG);
  paint_graph(capp_rect(c.x + 2, c.y + 24, c.w - 4, 46), q);

  /* What the holding is worth. The number the price is a means to. */
  y = c.y + 73;
  if (q->shares > 0 && q->have) {
    api->fill(capp_rect(c.x, y, 3, 8), CLR_BG);
    money(buf2, sizeof buf2, q->shares * 100, 0);
    api->fmt(buf, sizeof buf, "%s sh", buf2);
    seg(c.x + 3, c.x + 60, y, buf, CLR_DIM, CLR_BG);

    money(buf2, sizeof buf2, q->shares * q->price, 0);
    api->fmt(buf, sizeof buf, "$%s", buf2);
    seg(c.x + 60, x1, y, buf, CLR_HOLD, CLR_BG);

    api->fill(capp_rect(c.x, y + 8, c.w, 2), CLR_BG);
    api->fill(capp_rect(c.x, y + 10, 3, 8), CLR_BG);
    money(buf2, sizeof buf2, (q->shares * d < 0) ? -(q->shares * d) : q->shares * d, 0);
    api->fmt(buf, sizeof buf, "%c$%s today", d < 0 ? '-' : '+', buf2);
    seg(c.x + 3, x1, y + 10, buf, tone, CLR_BG);
    api->fill(capp_rect(c.x, y + 18, c.w, 2), CLR_BG);
    y += 20;
  } else {
    api->fill(capp_rect(c.x, y, c.w, 2), CLR_BG);
    y += 2;
  }

  /* The rest of the watchlist, compact. */
  rows = (c.y + c.h - FOOT_H - 1 - y) / 9;
  S.list_y = y;
  S.list_rows = rows;
  for (i = 0; i < S.n && i < rows; i++) {
    int ry = y + i * 9;
    int sel = (i == S.sel);
    uint16_t bg = sel ? CLR_SEL : CLR_BG;
    long dd = S.q[i].price - S.q[i].prev;

    api->fill(capp_rect(c.x, ry, 3, 8), bg);
    api->fill(capp_rect(c.x, ry + 8, c.w, 1), bg);
    seg(c.x + 3, c.x + 48, ry, S.q[i].sym, CLR_TEXT, bg);
    if (!S.q[i].have) {
      seg(c.x + 48, x1, ry, "--", CLR_DIM, bg);
      continue;
    }
    money(buf, sizeof buf, S.q[i].price, 1);
    seg(c.x + 48, c.x + 110, ry, buf, CLR_TEXT, bg);
    {
      long pct = S.q[i].prev ? (dd * 1000) / S.q[i].prev : 0;
      long ap = pct < 0 ? -pct : pct;
      api->fmt(buf, sizeof buf, "%c%ld.%ld%%", dd < 0 ? '-' : '+', ap / 10, ap % 10);
      seg(c.x + 110, c.x + 160, ry, buf, dd < 0 ? CLR_DOWN : CLR_UP, bg);
    }
    buf2[0] = 0;
    if (S.q[i].shares > 0) {
      money(buf, sizeof buf, S.q[i].shares * S.q[i].price, 0);
      api->fmt(buf2, sizeof buf2, "$%s", buf);
    }
    seg(c.x + 160, x1, ry, buf2, CLR_HOLD, bg);
  }
  y += i * 9;
  if (y < end) api->fill(capp_rect(c.x, y, c.w, end - y), CLR_BG);
}

static void app_paint(void *st, CRect full) {
  (void)st;
  /* The toolbar's two fast paths: a dropdown whose highlight moved, and the
   * busy dots, each without redrawing the graph underneath. */
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  paint_body(toolbar_rest(full));
  toolbar_paint_menu(full);
}

/* ---- actions and commands --------------------------------------------------
 *
 * Refresh is the GUI's `r`. The commands fetch one request per symbol through
 * fetch_one, and answer in words: a price read aloud or shown to an AI wants
 * "AAPL 187.23, up 1.2% today", not a graph. */
enum { ACT_REFRESH = 1, ACT_QUOTE, ACT_PORTFOLIO, ACT_EDIT };

static const CappParam P_SYM[] = { { "symbol", CAPP_ARG_TEXT, "a ticker, like AAPL" } };

static const CappAction ACTIONS[] = {
  { "refresh",   "Refresh",   "Stocks", 0x12, ACT_REFRESH },   /* ctrl-r */
  { "edit",      "Edit list", "Stocks", 0,    ACT_EDIT },
  { "quote",     "Quote",     0,        0,    ACT_QUOTE,
    "the price of any ticker and its move today", P_SYM, 1, CAPP_CMD_YES | CAPP_CMD_NET },
  { "portfolio", "Portfolio", 0,        0,    ACT_PORTFOLIO,
    "every holding in stocks.txt, priced, and the total", 0, 0, CAPP_CMD_YES | CAPP_CMD_NET },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_NET,
  "Stocks",
  /* 16x16: a rising line over an axis. */
  { 0x00, 0x00, 0x40, 0x00, 0x40, 0x3E, 0x40, 0x0E,
    0x40, 0x1A, 0x40, 0x30, 0x40, 0x60, 0x41, 0x80,
    0x43, 0x00, 0x4C, 0x00, 0x58, 0x00, 0x60, 0x00,
    0x40, 0x00, 0x7F, 0xFE, 0x00, 0x00, 0x00, 0x00 },
  "arrows\tchange symbol\nr\tre-read stocks.txt and fetch quotes\n"
  "enter\tfetch quotes too\ne\tedit stocks.txt in Edit\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

/* r re-reads the list as well as the prices: "refresh" is everything the
 * screen shows, and an edited stocks.txt is part of that. It was `e`, which
 * everywhere else means edit -- so now it does. */
static int do_action(int a) {
  switch (a) {
  case ACT_REFRESH:
    if (S.busy) return 0;
    load_list();
    if (S.sel >= S.n) S.sel = 0;
    refresh();
    return 1;
  case ACT_EDIT:
    if (api->run("edit", CAPP_CONFIG "/stocks.txt") != 0)
      api->fmt(S.note, sizeof S.note, "%s", "could not open Edit");
    return 1;
  }
  return 0;
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int app_key(void *st, unsigned char k) {
  int a = toolbar_key(k);
  (void)st;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  switch (k) {
  /* Moving is fine during a refresh: the rows fill in as they arrive. */
  case CAPP_KEY_UP:
  case CAPP_KEY_LEFT:  if (S.sel > 0) S.sel--; return 1;
  case CAPP_KEY_DOWN:
  case CAPP_KEY_RIGHT: if (S.sel + 1 < S.n) S.sel++; return 1;
  case 'r': case 'R':
  case CAPP_KEY_ENTER: return do_action(ACT_REFRESH);
  case 'e': case 'E':  return do_action(ACT_EDIT);
  default: return 0;
  }
}

static int app_click(void *st, short x, short y, int button) {
  int a = toolbar_click(x, y);
  (void)st; (void)button;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  y = (short)(y - toolbar_h());
  if (y < 24) return do_action(ACT_REFRESH);
  return 0;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int changed;
  (void)st; (void)buttons; (void)wheel;
  changed = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) changed = 1;
  return changed;
}

/* "187.23, up 1.20% today", or why not. */
static void describe(const Quote *q, char *out, size_t n) {
  char price[24];
  long move = q->prev ? (q->price - q->prev) * 10000 / q->prev : 0;   /* hundredths of % */
  long mag = move < 0 ? -move : move;
  money(price, sizeof price, q->price, 1);
  api->fmt(out, n, "%s %s, %s %ld.%02ld%% today", q->sym, price,
           move < 0 ? "down" : "up", mag / 100, mag % 100);
}

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  Quote q;
  size_t o = 0;
  long total = 0;
  int i, r;
  char line[64], money_s[24];
  (void)st;
  (void)argc;

  if (!api->net_ready() && api->net_connect(20000) != 0) {
    api->fmt(out, n, "%s", api->net_status());
    return -1;
  }
  switch (action) {
  case ACT_QUOTE:
    api->mem_set(&q, 0, sizeof q);
    for (i = 0; argv[0][i] && i < SYM_LEN - 1; i++)
      q.sym[i] = (argv[0][i] >= 'a' && argv[0][i] <= 'z') ? (char)(argv[0][i] - 32)
                                                           : argv[0][i];
    q.sym[i] = 0;
    r = fetch_one(&q);
    if (r < 0) { api->fmt(out, n, "network error %d", r); return -1; }
    if (!r) { api->fmt(out, n, "no price for %s", q.sym); return -1; }
    describe(&q, out, n);
    return 0;

  case ACT_PORTFOLIO:
    if (!S.n) { api->fmt(out, n, "no holdings: stocks.txt is empty"); return 0; }
    for (i = 0; i < S.n; i++) {
      r = fetch_one(&S.q[i]);
      if (r <= 0) {
        o += (size_t)api->fmt(out + o, n - o, "%s: no price\n", S.q[i].sym);
        continue;
      }
      describe(&S.q[i], line, sizeof line);
      o += (size_t)api->fmt(out + o, n - o, "%s\n", line);
      total += S.q[i].price * S.q[i].shares;
    }
    money(money_s, sizeof money_s, total, 0);
    api->fmt(out + o, n - o, "total %s", money_s);
    return 0;
  }
  api->fmt(out, n, "stocks has no command %d", action);
  return -1;
}

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  (void)argc; (void)argv;

  api->mem_set(&S, 0, sizeof S);
  load_list();
  toolbar_init(api, ACTIONS, NACT, 0, 0);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
