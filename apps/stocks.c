/* A quote tracker, as a loadable CardOS app.
 *
 * Built because the ask was to track SpaceX. SpaceX is privately held: it has
 * no ticker, and no public feed can quote it. What it has are public proxies,
 * so the default watchlist leads with DXYZ -- Destiny Tech100, a NYSE-listed
 * closed-end fund whose largest holding is SpaceX -- and the app says as much
 * on screen rather than quietly implying it is quoting SpaceX itself.
 *
 * Edit /desktop/stocks.txt to change the list: one symbol per line, with
 * anything after a space treated as a label.
 *
 * Quotes come from Yahoo's chart endpoint, which needs no key. The reply is
 * JSON and this reads it by looking for two field names -- a real parser is
 * several kilobytes of code to extract two numbers that sit in the first 700
 * bytes of every response.
 */

#include "kernel/app/capp.h"

#define MAX_SYMS   8
#define SYM_LEN    10
#define LABEL_LEN  18
#define ROW_H      11
#define BUF        1600

static const CardApi *api;

typedef struct {
  char  sym[SYM_LEN];
  char  label[LABEL_LEN];
  int   have;
  long  price_cents;
  long  prev_cents;
} Quote;

static struct {
  Quote q[MAX_SYMS];
  int   n;
  int   sel;
  int   busy;
  uint32_t fetched_ms;
  char  note[44];
  char  buf[BUF];
} S;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void add(const char *sym, const char *label) {
  Quote *q;
  if (S.n >= MAX_SYMS) return;
  q = &S.q[S.n++];
  api->mem_set(q, 0, sizeof *q);
  api->fmt(q->sym, sizeof q->sym, "%s", sym);
  api->fmt(q->label, sizeof q->label, "%s", label);
}

/* One symbol per line: "SYM label words". A line starting with # is a note. */
static void load_list(void) {
  char line[64];
  int fd, n, i, len = 0;
  char rd[128];

  S.n = 0;
  fd = api->open("/desktop/stocks.txt", CAPP_O_READ);
  if (fd < 0) {
    /* The defaults say what they are. A ticker on its own would look like a
     * claim that this is SpaceX. */
    add("DXYZ", "SpaceX proxy");
    add("RKLB", "Rocket Lab");
    add("ASTS", "AST SpaceMob");
    add("^GSPC", "S&P 500");
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
      if (len && line[0] != '#') {
        char sym[SYM_LEN], label[LABEL_LEN];
        int j = 0, k = 0;
        while (line[j] && line[j] != ' ' && j < SYM_LEN - 1) { sym[j] = line[j]; j++; }
        sym[j] = 0;
        while (line[j] == ' ') j++;
        while (line[j] && k < LABEL_LEN - 1) label[k++] = line[j++];
        label[k] = 0;
        if (sym[0]) add(sym, label);
      }
      len = 0;
    }
  }
  api->close(fd);
  if (S.n == 0) add("DXYZ", "SpaceX proxy");
}

/* ---- the smallest JSON reader that answers the question ------------------
 *
 * Finds "name": and reads the number after it. No nesting, no escapes, no
 * objects -- and none needed, because both fields are scalars at a known
 * depth. Returns 0 if the field is absent, which is also what a truncated
 * reply looks like. */
static int find_number(const char *hay, const char *name, long *out_cents) {
  size_t nl = api->str_len(name);
  size_t hl = api->str_len(hay);
  size_t i;

  for (i = 0; i + nl + 2 < hl; i++) {
    const char *p;
    long whole = 0, frac = 0, scale = 1;
    int neg = 0, seen = 0;

    if (hay[i] != '"') continue;
    {
      size_t j;
      for (j = 0; j < nl; j++) if (hay[i + 1 + j] != name[j]) break;
      if (j < nl) continue;
      if (hay[i + 1 + nl] != '"' || hay[i + 2 + nl] != ':') continue;
    }

    p = hay + i + 3 + nl;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); p++; seen = 1; }
    if (!seen) return 0;
    if (*p == '.') {
      p++;
      /* Two decimal places is what a price has; the rest is noise from a
       * float that was never exact. */
      while (*p >= '0' && *p <= '9' && scale < 100) {
        frac = frac * 10 + (*p - '0');
        scale *= 10;
        p++;
      }
      while (scale < 100) { frac *= 10; scale *= 10; }
    }
    *out_cents = (whole * 100 + frac) * (neg ? -1 : 1);
    return 1;
  }
  return 0;
}

static int fetch_one(Quote *q) {
  char url[128];
  int n;

  api->fmt(url, sizeof url,
           "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1d",
           q->sym);
  n = api->http_get(url, S.buf, sizeof S.buf, 12000);
  if (n <= 0) return n;

  q->have = find_number(S.buf, "regularMarketPrice", &q->price_cents) &&
            find_number(S.buf, "chartPreviousClose", &q->prev_cents);
  return q->have ? 1 : 0;
}

static void refresh(void) {
  int i;

  S.busy = 1;
  if (!api->net_ready()) {
    api->fmt(S.note, sizeof S.note, "joining the network...");
    if (api->net_connect(20000) != 0) {
      api->fmt(S.note, sizeof S.note, "no network -- join one in Settings");
      S.busy = 0;
      return;
    }
  }

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
  api->fmt(S.note, sizeof S.note, "r refreshes");
  S.busy = 0;
}

static void app_open(void *st) {
  (void)st;
  load_list();
  S.sel = 0;
  S.fetched_ms = 0;
  api->fmt(S.note, sizeof S.note, "r fetches quotes");
}

static void money(char *buf, size_t n, long cents) {
  long a = cents < 0 ? -cents : cents;
  api->fmt(buf, n, "%s%ld.%02ld", cents < 0 ? "-" : "", a / 100, a % 100);
}

static void app_paint(void *st, CRect c) {
  int i;
  char buf[32];
  (void)st;

  api->fill(c, CAPP_WHITE);
  api->fill(rect(c.x, c.y, c.w, ROW_H), CAPP_NAVY);
  api->text((short)(c.x + 2), (short)(c.y + 2), "Watchlist", CAPP_WHITE, CAPP_NAVY);
  if (S.fetched_ms) {
    api->fmt(buf, sizeof buf, "%us ago",
             (unsigned)((api->ticks_ms() - S.fetched_ms) / 1000u));
    api->text((short)(c.x + c.w - 60), (short)(c.y + 2), buf, CAPP_WHITE, CAPP_NAVY);
  }

  for (i = 0; i < S.n; i++) {
    short y = (short)(c.y + ROW_H + 2 + i * ROW_H);
    int sel = (i == S.sel);
    uint16_t bg = sel ? CAPP_FACE : CAPP_WHITE;
    long d = S.q[i].price_cents - S.q[i].prev_cents;

    api->fill(rect(c.x, y, c.w, ROW_H), bg);
    api->text((short)(c.x + 2), (short)(y + 1), S.q[i].sym, CAPP_BLACK, bg);

    if (!S.q[i].have) {
      api->text((short)(c.x + 62), (short)(y + 1), "--", CAPP_GREY, bg);
    } else {
      money(buf, sizeof buf, S.q[i].price_cents);
      api->text((short)(c.x + 62), (short)(y + 1), buf, CAPP_BLACK, bg);

      /* Up and down carry the sign as well as the colour: red and green on a
       * 135-pixel panel are not always distinguishable in daylight. */
      {
        long pct = S.q[i].prev_cents ? (d * 1000) / S.q[i].prev_cents : 0;
        long ap = pct < 0 ? -pct : pct;
        api->fmt(buf, sizeof buf, "%c%ld.%ld%%", d < 0 ? '-' : '+',
                 ap / 10, ap % 10);
        api->text((short)(c.x + 118), (short)(y + 1), buf,
                  d < 0 ? CAPP_RED : CAPP_GREEN, bg);
      }
    }

    if (sel && S.q[i].label[0])
      api->text((short)(c.x + 2), (short)(y + 1), S.q[i].label, CAPP_BLACK, bg);
  }

  api->text(c.x, (short)(c.y + c.h - 8), S.note, CAPP_GREY, CAPP_WHITE);
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (S.busy) return 0;
  switch (k) {
  case CAPP_KEY_UP:   if (S.sel > 0) S.sel--; return 1;
  case CAPP_KEY_DOWN: if (S.sel + 1 < S.n) S.sel++; return 1;
  case 'r': case 'R':
  case CAPP_KEY_ENTER:
    refresh();
    return 1;
  case 'e': case 'E':
    load_list();
    api->fmt(S.note, sizeof S.note, "%d symbols from stocks.txt", S.n);
    return 1;
  default: return 0;
  }
}

static int app_click(void *st, short x, short y, int button) {
  int i = (y - ROW_H - 2) / ROW_H;
  (void)st; (void)x; (void)button;
  if (S.busy) return 0;
  if (y < ROW_H) { refresh(); return 1; }     /* the title bar refreshes */
  if (i < 0 || i >= S.n) return 0;
  S.sel = i;
  return 1;
}

/* 16x16: a rising line over an axis. */
static const unsigned char ICON[CAPP_ICON_BYTES] = {
  0x00, 0x00, 0x40, 0x00, 0x40, 0x3E, 0x40, 0x0E,
  0x40, 0x1A, 0x40, 0x30, 0x40, 0x60, 0x41, 0x80,
  0x43, 0x00, 0x4C, 0x00, 0x58, 0x00, 0x60, 0x00,
  0x40, 0x00, 0x7F, 0xFE, 0x00, 0x00, 0x00, 0x00,
};

static CappApp APP;

const CappApp *capp_register(const CardApi *a) {
  api = a;
  APP.api_version = CAPP_API_VERSION;
  api->mem_cpy(APP.name, "Stocks", 7);
  api->mem_cpy(APP.icon, ICON, CAPP_ICON_BYTES);
  APP.fullscreen = 0;
  APP.paint = app_paint;
  APP.key = app_key;
  APP.click = app_click;
  APP.open = app_open;
  APP.set_file = 0;
  APP.height = 0;
  APP.pref_w = 0;
  APP.pref_h = 0;
  APP.wants_text = 0;      /* a list or a board, never a text field */
  APP.state = 0;
  return &APP;
}
