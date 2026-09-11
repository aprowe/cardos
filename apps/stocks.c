/* Stocks -- a watchlist with a week's graph and what a holding is worth.
 *
 * Leads with SPCX because that is the one this was built for. Each row can
 * carry a share count, and the app shows what that many shares come to at the
 * current price -- the number you actually want, rather than a price you then
 * do arithmetic on.
 *
 * Edit /desktop/stocks.txt to change the list. One per line:
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
  int   busy;
  uint32_t fetched_ms;
  char  note[48];
  char  buf[BUF];
} S;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

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
  while (line[i] && line[i] != ' ' && k < LABEL_LEN - 1) word[k++] = line[i];
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
  fd = api->open("/desktop/stocks.txt", CAPP_O_READ);
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

static int fetch_one(Quote *q) {
  char url[160];
  int n;

  /* One request for both: the meta block carries the current price and the
   * previous close, and the indicators carry the week. */
  api->fmt(url, sizeof url,
           "https://query1.finance.yahoo.com/v8/finance/chart/%s"
           "?interval=1d&range=7d", q->sym);
  n = api->http_get(url, S.buf, sizeof S.buf, 15000);
  if (n <= 0) return n;

  q->have = field_cents(S.buf, "regularMarketPrice", &q->price) &&
            field_cents(S.buf, "chartPreviousClose", &q->prev);
  q->nhist = read_closes(S.buf, q->hist, HIST_MAX);
  return q->have ? 1 : 0;
}

static void refresh(void) {
  int i;

  S.busy = 1;
  for (i = 0; i < S.n; i++) {
    int r;
    api->fmt(S.note, sizeof S.note, "fetching %s (%d/%d)", S.q[i].sym, i + 1, S.n);
    r = fetch_one(&S.q[i]);
    if (r < 0) {
      api->fmt(S.note, sizeof S.note, "%s: network error %d", S.q[i].sym, r);
      S.busy = 0;
      return;
    }
  }
  S.fetched_ms = api->ticks_ms();
  api->fmt(S.note, sizeof S.note, "r refreshes   arrows change symbol");
  S.busy = 0;
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
static void paint_graph(CRect g, const Quote *q) {
  long lo, hi, span;
  int i;

  api->fill(g, CLR_PANEL);
  if (q->nhist < 2) {
    api->text((short)(g.x + 6), (short)(g.y + g.h / 2 - 4), "no history",
              CLR_DIM, CLR_PANEL);
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
  if (q->prev >= lo && q->prev <= hi) {
    short y = (short)(g.y + g.h - 1 - (short)((q->prev - lo) * (g.h - 2) / span));
    for (i = 0; i < g.w; i += 4)
      api->fill(rect(g.x + i, y, 2, 1), CLR_GRID);
  }

  for (i = 1; i < q->nhist; i++) {
    short x0 = (short)(g.x + (i - 1) * (g.w - 1) / (q->nhist - 1));
    short x1 = (short)(g.x + i * (g.w - 1) / (q->nhist - 1));
    short y0 = (short)(g.y + g.h - 1 - (short)((q->hist[i - 1] - lo) * (g.h - 2) / span));
    short y1 = (short)(g.y + g.h - 1 - (short)((q->hist[i] - lo) * (g.h - 2) / span));
    short x, steps = (short)(x1 - x0);
    if (steps < 1) steps = 1;

    /* A line drawn as a column per x: no diagonal rasteriser needed when the
     * graph is only ever as wide as the screen. */
    for (x = 0; x <= steps; x++) {
      short y = (short)(y0 + (y1 - y0) * x / steps);
      short yn = (short)(y0 + (y1 - y0) * (x + 1 > steps ? steps : x + 1) / steps);
      short top = y < yn ? y : yn;
      short h = (short)((y < yn ? yn - y : y - yn) + 1);
      api->fill(rect(x0 + x, top, 1, h), CLR_LINE);
    }
  }

  /* The range, so the shape has numbers attached to it. */
  {
    char buf[16];
    money(buf, sizeof buf, hi, 0);
    api->text((short)(g.x + 2), (short)(g.y + 1), buf, CLR_DIM, CLR_PANEL);
    money(buf, sizeof buf, lo, 0);
    api->text((short)(g.x + 2), (short)(g.y + g.h - 9), buf, CLR_DIM, CLR_PANEL);
  }
}

static void app_paint(void *st, CRect c) {
  const Quote *q;
  char buf[48], buf2[32];
  long d;
  uint16_t tone;
  int i, rows, y;
  (void)st;

  api->fill(c, CLR_BG);
  if (S.n == 0) return;
  q = &S.q[S.sel];
  d = q->price - q->prev;
  tone = d < 0 ? CLR_DOWN : CLR_UP;

  /* The symbol and what it is. */
  api->text((short)(c.x + 3), (short)(c.y + 2), q->sym, CLR_TEXT, CLR_BG);
  api->text((short)(c.x + 3 + (short)api->str_len(q->sym) * 6 + 6), (short)(c.y + 2),
            q->label, CLR_DIM, CLR_BG);
  if (S.fetched_ms) {
    api->fmt(buf, sizeof buf, "%us", (unsigned)((api->ticks_ms() - S.fetched_ms) / 1000u));
    api->text((short)(c.x + c.w - 30), (short)(c.y + 2), buf, CLR_DIM, CLR_BG);
  }

  /* The price, large, with the change beside it. */
  if (!q->have) {
    api->text((short)(c.x + 3), (short)(c.y + 13), "--", CLR_DIM, CLR_BG);
  } else {
    money(buf, sizeof buf, q->price, 1);
    api->text((short)(c.x + 3), (short)(c.y + 13), buf, CLR_TEXT, CLR_BG);
    {
      long pct = q->prev ? (d * 1000) / q->prev : 0;
      long ap = pct < 0 ? -pct : pct;
      long ad = d < 0 ? -d : d;
      money(buf2, sizeof buf2, ad, 1);
      api->fmt(buf, sizeof buf, "%c%s  %c%ld.%ld%%",
               d < 0 ? '-' : '+', buf2, d < 0 ? '-' : '+', ap / 10, ap % 10);
      api->text((short)(c.x + 3 + 60), (short)(c.y + 13), buf, tone, CLR_BG);
    }
  }

  paint_graph(rect(c.x + 2, c.y + 24, c.w - 4, 46), q);

  /* What the holding is worth. The number the price is a means to. */
  y = c.y + 73;
  if (q->shares > 0 && q->have) {
    money(buf2, sizeof buf2, q->shares * 100, 0);
    api->fmt(buf, sizeof buf, "%s sh", buf2);
    api->text((short)(c.x + 3), (short)y, buf, CLR_DIM, CLR_BG);

    money(buf2, sizeof buf2, q->shares * q->price, 0);
    api->fmt(buf, sizeof buf, "$%s", buf2);
    api->text((short)(c.x + 60), (short)y, buf, CLR_HOLD, CLR_BG);

    money(buf2, sizeof buf2, (q->shares * d < 0) ? -(q->shares * d) : q->shares * d, 0);
    api->fmt(buf, sizeof buf, "%c$%s today", d < 0 ? '-' : '+', buf2);
    api->text((short)(c.x + 3), (short)(y + 10), buf, tone, CLR_BG);
    y += 20;
  } else {
    y += 2;
  }

  /* The rest of the watchlist, compact. */
  rows = (c.y + c.h - 10 - y) / 9;
  for (i = 0; i < S.n && i < rows; i++) {
    short ry = (short)(y + i * 9);
    int sel = (i == S.sel);
    uint16_t bg = sel ? CLR_SEL : CLR_BG;
    long dd = S.q[i].price - S.q[i].prev;

    api->fill(rect(c.x, ry, c.w, 9), bg);
    api->text((short)(c.x + 3), ry, S.q[i].sym, CLR_TEXT, bg);
    if (!S.q[i].have) {
      api->text((short)(c.x + 48), ry, "--", CLR_DIM, bg);
      continue;
    }
    money(buf, sizeof buf, S.q[i].price, 1);
    api->text((short)(c.x + 48), ry, buf, CLR_TEXT, bg);
    {
      long pct = S.q[i].prev ? (dd * 1000) / S.q[i].prev : 0;
      long ap = pct < 0 ? -pct : pct;
      api->fmt(buf, sizeof buf, "%c%ld.%ld%%", dd < 0 ? '-' : '+', ap / 10, ap % 10);
      api->text((short)(c.x + 110), ry, buf, dd < 0 ? CLR_DOWN : CLR_UP, bg);
    }
    if (S.q[i].shares > 0) {
      money(buf, sizeof buf, S.q[i].shares * S.q[i].price, 0);
      api->fmt(buf2, sizeof buf2, "$%s", buf);
      api->text((short)(c.x + 160), ry, buf2, CLR_HOLD, bg);
    }
  }

  api->text((short)(c.x + 3), (short)(c.y + c.h - 9), S.note, CLR_DIM, CLR_BG);
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (S.busy) return 0;
  switch (k) {
  case CAPP_KEY_UP:
  case CAPP_KEY_LEFT:  if (S.sel > 0) S.sel--; return 1;
  case CAPP_KEY_DOWN:
  case CAPP_KEY_RIGHT: if (S.sel + 1 < S.n) S.sel++; return 1;
  case 'r': case 'R':
  case CAPP_KEY_ENTER: refresh(); return 1;
  case 'e': case 'E':
    load_list();
    api->fmt(S.note, sizeof S.note, "%d symbols from stocks.txt", S.n);
    return 1;
  default: return 0;
  }
}

static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)x; (void)button;
  if (S.busy) return 0;
  if (y < 24) { refresh(); return 1; }
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Stocks",
  /* 16x16: a rising line over an axis. */
  { 0x00, 0x00, 0x40, 0x00, 0x40, 0x3E, 0x40, 0x0E,
    0x40, 0x1A, 0x40, 0x30, 0x40, 0x60, 0x41, 0x80,
    0x43, 0x00, 0x4C, 0x00, 0x58, 0x00, 0x60, 0x00,
    0x40, 0x00, 0x7F, 0xFE, 0x00, 0x00, 0x00, 0x00 },
  "arrows\tchange symbol\nr\tfetch quotes\ne\tre-read stocks.txt\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  (void)argc; (void)argv;

  api->mem_set(&S, 0, sizeof S);
  load_list();
  api->fmt(S.note, sizeof S.note, "r fetches quotes");

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  api->ui(&UI);
  return 0;
}
