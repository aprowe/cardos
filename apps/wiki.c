/* Wikipedia -- search it, read it, print it.
 *
 * Wikipedia is HTTPS, JSON and wikitext; the server (server/wiki.py) does
 * all of that and answers in lines: a search is `title <tab> description`
 * a line, an article is a header line `page <tab> N <tab> TOTAL <tab> title`
 * and then plain ASCII, a paragraph a line, with `## ` and `### ` headings,
 * cut into pages of at most 5 KB so a page fits the buffer here.
 *
 * Three views, each Esc going back one:
 *   search   type (or speak: the box takes text), Enter searches; Enter
 *            with nothing typed opens a random article
 *   results  up/down, Enter opens; n or e goes back to the box
 *   article  word-wrapped in ui13 (headings in ui13b, 6x8 without the
 *            fonts); up/down a line, left/right or space a screen. Past
 *            the end of a page the next is fetched, and before the start
 *            the one before, landing at its end. r a random article, p or
 *            fn-p prints the page loaded.
 *
 * One request at a time, from tick (http_start / http_poll), as Toggl and
 * Roku do. The wrapped lines are offsets into the page's text, so a page
 * costs its 6 KB and a few bytes a line, not a copy.
 *
 * Commands: `summary TOPIC` (the first paragraph about it: for voice and
 * Today), `featured` (today's featured article), and `open TOPIC`, which
 * opens the app searching for it.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/footer.h"

static const CardApi *api;

#define MAX_RESULTS 10
#define TITLE_MAX   80
#define RDESC_MAX   60
#define QUERY_MAX   64
#define REPLY_MAX   6144                 /* a page: 5000 bytes of text and its header */
#define ART_MAX     6144
#define MAX_LINES   400
#define TOP_H       16
#define ROW_H       15
#define MARGIN      6
#define GAP_H       4                    /* between paragraphs */

#define CLR_BG     CAPP_RGB(250, 250, 246)
#define CLR_TEXT   CAPP_RGB(24, 26, 30)
#define CLR_DIM    CAPP_RGB(110, 116, 128)
#define CLR_HEAD   CAPP_RGB(0, 0, 0)
#define CLR_LINK   CAPP_RGB(36, 90, 180)        /* Wikipedia's blue */
#define CLR_BAR    CAPP_RGB(234, 236, 240)
#define CLR_SEL    CAPP_RGB(214, 226, 248)
#define CLR_BAD    CAPP_RGB(200, 50, 40)
#define CLR_RULE   CAPP_RGB(200, 204, 212)

enum { V_SEARCH = 0, V_RESULTS, V_ARTICLE };
enum { ST_IDLE = 0, ST_SEARCH, ST_PAGE };
enum { L_BODY = 0, L_H2, L_H3, L_GAP };

typedef struct {
  uint16_t off, len;              /* into G.art */
  uint8_t  kind;                  /* L_* */
} Line;

typedef struct {
  char title[TITLE_MAX];
  char desc[RDESC_MAX];
} Result;

static struct {
  int      view;
  int      stage;
  int      bad;
  char     status[48];

  char     query[QUERY_MAX];
  int      qlen;
  char     searched[QUERY_MAX];   /* what the results are for */
  Result   res[MAX_RESULTS];
  int      nres;
  int      rsel, rtop;
  int      from_results;          /* Esc from the article goes to the results */

  char     title[TITLE_MAX];      /* the article */
  int      page, pages;           /* from 1 */
  int      want_page;             /* the page asked for */
  int      to_end;                /* land at the end of the page that arrives */
  char     art[ART_MAX];          /* the page's text, after its header line */
  Line     line[MAX_LINES];
  int      nlines;
  int      top;                   /* the first line on screen */
  int      wrap_w;                /* the width the lines were wrapped to */
  int      printing;

  int      f_ui, f_uib;
  uint8_t  cw[95], cwb[95];       /* each character's width in ui13, ui13b */
  int      lh, lhb;               /* line heights */
  CRect    content;
  char     reply[REPLY_MAX];
} G;

/* ---- text widths ------------------------------------------------------------------ */

static void measure_fonts(void) {
  int i;
  char s[2];
  s[1] = 0;
  for (i = 0; i < 95; i++) {
    s[0] = (char)(32 + i);
    G.cw[i]  = (uint8_t)(G.f_ui  >= 0 ? api->text_width(G.f_ui, s)  : 6);
    G.cwb[i] = (uint8_t)(G.f_uib >= 0 ? api->text_width(G.f_uib, s) : 6);
  }
  G.lh  = (G.f_ui  >= 0 ? api->font_height(G.f_ui)  : 8) + 2;
  G.lhb = (G.f_uib >= 0 ? api->font_height(G.f_uib) : 8) + 3;
}

static int char_w(int bold, char c) {
  unsigned char u = (unsigned char)c;
  if (u < 32 || u > 126) u = '?';
  return bold ? G.cwb[u - 32] : G.cw[u - 32];
}

static int str_w(int bold, const char *s, int n) {
  int w = 0, i;
  for (i = 0; i < n && s[i]; i++) w += char_w(bold, s[i]);
  return w;
}

/* As much of `s` as fits in `maxw` pixels, into `out`, with ".." when cut. */
static void fit(int bold, const char *s, int maxw, char *out, int n) {
  int i = 0, w = 0, full = str_w(bold, s, 1 << 14);
  if (full <= maxw) { api->fmt(out, (size_t)n, "%s", s); return; }
  maxw -= str_w(bold, "..", 2);
  while (s[i] && i < n - 3 && w + char_w(bold, s[i]) <= maxw) w += char_w(bold, s[i++]);
  api->mem_cpy(out, s, (size_t)i);
  out[i] = '.'; out[i + 1] = '.'; out[i + 2] = 0;
}

static int text_w(void) {
  int w = (G.content.w ? G.content.w : 240) - 2 * MARGIN;
  return w > 24 ? w : 24;
}

static int view_h(void) {
  int h = (G.content.h ? G.content.h : 135) - TOP_H - FOOT_H;
  return h > 10 ? h : 10;
}

/* ---- wrapping --------------------------------------------------------------------- */

static void add_line(int off, int len, int kind) {
  if (G.nlines >= MAX_LINES) return;
  G.line[G.nlines].off = (uint16_t)off;
  G.line[G.nlines].len = (uint16_t)len;
  G.line[G.nlines].kind = (uint8_t)kind;
  G.nlines++;
}

/* One paragraph, art[off, off+n), broken at spaces to `w` pixels; a word
 * wider than a line breaks inside. */
static void wrap_para(int off, int n, int kind, int w) {
  int bold = kind == L_H2 || kind == L_H3, pos = off, end = off + n;
  while (pos < end) {
    int i = pos, px = 0, brk = -1, stop;
    while (pos < end && G.art[pos] == ' ') pos++;
    if (pos >= end) break;
    for (i = pos; i < end; i++) {
      int cw = char_w(bold, G.art[i]);
      if (px + cw > w) break;
      if (G.art[i] == ' ') brk = i;
      px += cw;
    }
    if (i >= end) stop = end;
    else if (G.art[i] == ' ') stop = i;          /* a word that ends at the edge */
    else if (brk > pos) stop = brk;
    else stop = i > pos ? i : pos + 1;
    add_line(pos, stop - pos, kind);
    pos = stop;
  }
}

/* The page's text as lines on screen: each paragraph wrapped, a gap
 * between paragraphs, headings in bold with their marks taken off. */
static void reflow(void) {
  int p = 0, w = text_w();
  G.nlines = 0;
  G.wrap_w = w;
  while (G.art[p]) {
    int s = p, n, kind = L_BODY;
    while (G.art[p] && G.art[p] != '\n') p++;
    n = p - s;
    if (G.art[p]) p++;
    if (!n) continue;
    if (n > 4 && G.art[s] == '#' && G.art[s + 1] == '#' && G.art[s + 2] == '#' && G.art[s + 3] == ' ') {
      kind = L_H3; s += 4; n -= 4;
    } else if (n > 3 && G.art[s] == '#' && G.art[s + 1] == '#' && G.art[s + 2] == ' ') {
      kind = L_H2; s += 3; n -= 3;
    }
    if (G.nlines && kind != L_BODY) add_line(s, 0, L_GAP);
    wrap_para(s, n, kind, w);
    add_line(s, 0, L_GAP);
  }
  while (G.nlines && G.line[G.nlines - 1].kind == L_GAP) G.nlines--;
}

static int line_h(int i) {
  switch (G.line[i].kind) {
  case L_GAP: return GAP_H;
  case L_H2:  return G.lhb + 2;
  case L_H3:  return G.lhb;
  default:    return G.lh;
  }
}

/* How many lines from `top` fit on the screen, whole. At least one. */
static int fits_from(int top) {
  int h = 0, i, room = view_h();
  for (i = top; i < G.nlines; i++) {
    if (h + line_h(i) > room) break;
    h += line_h(i);
  }
  return i - top > 0 ? i - top : 1;
}

/* The top that shows a screen ending just before `end`. */
static int top_before(int end) {
  int h = 0, i = end, room = view_h();
  while (i > 0 && h + line_h(i - 1) <= room) h += line_h(--i);
  return i;
}

/* ---- what the server says ----------------------------------------------------------- */

static void absorb_results(void) {
  const char *p = G.reply;
  G.nres = 0;
  while (*p && G.nres < MAX_RESULTS) {
    if (*p != '\n') {
      tsv_field(p, 0, G.res[G.nres].title, TITLE_MAX);
      tsv_field(p, 1, G.res[G.nres].desc, RDESC_MAX);
      if (G.res[G.nres].title[0]) G.nres++;
    }
    p = tsv_next_line(p);
  }
  G.rsel = G.rtop = 0;
}

/* A page: its header line, then its text into G.art. 0, or -1 if it is not
 * a page at all. */
static int absorb_page(void) {
  char num[12];
  const char *t;
  int n;
  if (!str_starts(G.reply, "page\t")) return -1;
  tsv_field(G.reply, 1, num, sizeof num);
  t = num;
  G.page = (int)str_uint(&t);
  tsv_field(G.reply, 2, num, sizeof num);
  t = num;
  G.pages = (int)str_uint(&t);
  if (G.page < 1) G.page = 1;
  if (G.pages < G.page) G.pages = G.page;
  tsv_field(G.reply, 3, G.title, TITLE_MAX);
  t = tsv_next_line(G.reply);
  n = (int)api->str_len(t);
  if (n > ART_MAX - 1) n = ART_MAX - 1;
  api->mem_cpy(G.art, t, (size_t)n);
  G.art[n] = 0;
  reflow();
  G.top = G.to_end ? top_before(G.nlines) : 0;
  G.to_end = 0;
  return 0;
}

static void say(const char *s, int bad) {
  api->fmt(G.status, sizeof G.status, "%s", s);
  G.bad = bad;
}

/* "error why" from the server, or a status, as one line. */
static void say_failure(int n) {
  int i;
  if (n != -1 && str_starts(G.reply, "error ")) api->fmt(G.status, sizeof G.status, "%s", G.reply + 6);
  else if (n == -1) api->fmt(G.status, sizeof G.status, "no network");
  else api->fmt(G.status, sizeof G.status, "cannot reach the server (%d)", n);
  for (i = 0; G.status[i]; i++) if (G.status[i] == '\n') { G.status[i] = 0; break; }
  G.bad = 1;
}

/* ---- asking -------------------------------------------------------------------------- */

static int ask(const char *rel, int stage) {
  char url[384];
  if (G.stage != ST_IDLE) { say("busy: waiting on Wikipedia", 1); return -1; }
  api->fmt(url, sizeof url, "%s%s", api->proxy(), rel);
  if (api->http_start("GET", url, 0, 0, "", 20000) != 0) {
    say("busy: another request is out", 1);
    return -1;
  }
  G.stage = stage;
  say(stage == ST_SEARCH ? "searching..." : "fetching...", 0);
  return 0;
}

static void search(const char *q) {
  char rel[40 + QUERY_MAX * 3];
  int k = api->fmt(rel, sizeof rel, "/wiki/search?q=");
  url_enc(rel + k, (int)sizeof rel - k, q);
  if (ask(rel, ST_SEARCH) == 0) api->fmt(G.searched, sizeof G.searched, "%s", q);
}

static void fetch_page(const char *title, int page, int to_end) {
  char rel[40 + TITLE_MAX * 3];
  int k = api->fmt(rel, sizeof rel, "/wiki/page?page=%d&title=", page);
  url_enc(rel + k, (int)sizeof rel - k, title);
  if (ask(rel, ST_PAGE) == 0) { G.want_page = page; G.to_end = to_end; }
}

static void random_article(void) {
  if (ask("/wiki/random", ST_PAGE) == 0) { G.want_page = 1; G.to_end = 0; G.from_results = 0; }
}

static int poll(void) {
  int n;
  if (G.stage == ST_IDLE) return 0;
  n = api->http_poll(G.reply, sizeof G.reply);
  if (n == CAPP_HTTP_PENDING) return 0;
  if (n < 0) {
    G.stage = ST_IDLE;
    G.to_end = 0;
    say_failure(n);
    return 1;
  }
  G.reply[n] = 0;
  if (G.stage == ST_SEARCH) {
    absorb_results();
    G.view = V_RESULTS;
    if (G.nres) G.status[0] = 0;
    else say("nothing found", 0);
  } else if (absorb_page() == 0) {
    G.view = V_ARTICLE;
    G.status[0] = 0;
  } else {
    say("the server sent no page", 1);
  }
  G.stage = ST_IDLE;
  return 1;
}

/* ---- printing -------------------------------------------------------------------------- */

/* The page loaded, as the printer's markup: the title as `# `, headings as
 * `## ` (### too: the printer has two sizes), and which page it is when
 * there are more. Built in the reply buffer, which is idle between requests;
 * print copies it. */
static int build_print(char *out, int n) {
  int o = 0, p = 0;
  o += api->fmt(out + o, (size_t)(n - o), "# %s\n", G.title);
  if (G.pages > 1 && o < n) o += api->fmt(out + o, (size_t)(n - o), "(page %d of %d)\n", G.page, G.pages);
  while (G.art[p] && o < n - 2) {
    if ((p == 0 || G.art[p - 1] == '\n') && G.art[p] == '#' && G.art[p + 1] == '#' &&
        G.art[p + 2] == '#' && G.art[p + 3] == ' ') {
      p++;                          /* ### to ## */
      continue;
    }
    out[o++] = G.art[p++];
  }
  if (o > n - 1) o = n - 1;
  out[o] = 0;
  return o;
}

static void print_page(void) {
  int rc;
  if (G.view != V_ARTICLE || !G.title[0]) { say("open an article to print it", 1); return; }
  if (G.stage != ST_IDLE) { say("busy: waiting on Wikipedia", 1); return; }
  build_print(G.reply, sizeof G.reply);
  rc = api->print_fonts(G.reply, "print24", "print24b", "print34b");
  G.reply[0] = 0;
  if (rc == 0)       { G.printing = 1; say("printing...", 0); }
  else if (rc == -1) say("still printing the last one", 1);
  else if (rc == -2) say("no printer: print scan in the console", 1);
  else               say("could not print: no memory", 1);
}

/* ---- the screen ------------------------------------------------------------------------ */

static void draw(int bold, int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  int f = bold ? G.f_uib : G.f_ui;
  if (f >= 0) api->text_font(f, (int16_t)x, (int16_t)y, s, fg, bg);
  else api->text((int16_t)x, (int16_t)y, s, fg, bg);
}

static void paint_top(const char *left) {
  CRect c = G.content;
  char buf[TITLE_MAX + 8];
  int sw = 0;
  api->fill(capp_rect(c.x, c.y, c.w, TOP_H), CLR_BAR);
  if (G.status[0]) {
    sw = str_w(0, G.status, 1 << 14);
    if (sw > c.w - 60) sw = c.w - 60;
    fit(0, G.status, sw, buf, sizeof buf);
    sw = str_w(0, buf, 1 << 14);
    draw(0, c.x + c.w - MARGIN - sw, c.y + (TOP_H - G.lh + 2) / 2, buf,
         G.bad ? CLR_BAD : CLR_DIM, CLR_BAR);
  }
  fit(1, left, c.w - 2 * MARGIN - sw - (sw ? 6 : 0), buf, sizeof buf);
  draw(1, c.x + MARGIN, c.y + (TOP_H - G.lhb + 3) / 2, buf, CLR_HEAD, CLR_BAR);
  api->fill(capp_rect(c.x, c.y + TOP_H - 1, c.w, 1), CLR_RULE);
}

static void paint_search(void) {
  CRect c = G.content;
  int y = c.y + TOP_H + 10, bh = G.lh + 8;
  char box[QUERY_MAX + 2];
  api->fill(capp_rect(c.x, c.y + TOP_H, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  api->fill(capp_rect(c.x + MARGIN, y, c.w - 2 * MARGIN, bh), CLR_RULE);
  api->fill(capp_rect(c.x + MARGIN + 1, y + 1, c.w - 2 * MARGIN - 2, bh - 2), CAPP_RGB(255, 255, 255));
  api->fmt(box, sizeof box, "%s_", G.query);
  {
    /* The end of what is typed, when it is longer than the box. */
    const char *s = box;
    int room = c.w - 2 * MARGIN - 10;
    while (*s && str_w(0, s, 1 << 14) > room) s++;
    draw(0, c.x + MARGIN + 5, y + 4, s, G.qlen ? CLR_TEXT : CLR_DIM, CAPP_RGB(255, 255, 255));
  }
  y += bh + 10;
  draw(0, c.x + MARGIN, y, "Type a topic and press Enter.", CLR_DIM, CLR_BG);
  y += G.lh + 2;
  draw(0, c.x + MARGIN, y, "Enter with nothing typed: a random", CLR_DIM, CLR_BG);
  y += G.lh;
  draw(0, c.x + MARGIN, y, "article.", CLR_DIM, CLR_BG);
  if (G.nres) {
    y += G.lh + 4;
    draw(0, c.x + MARGIN, y, "Esc: back to the results", CLR_DIM, CLR_BG);
  }
  footer_paint(api, c, "enter search  esc clear");
}

static int result_rows(void) {
  int r = view_h() / ROW_H;
  return r > 1 ? r : 1;
}

static void paint_results(void) {
  CRect c = G.content;
  int y = c.y + TOP_H, i, rows = result_rows();
  char buf[TITLE_MAX + RDESC_MAX];
  api->fill(capp_rect(c.x, y, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  if (!G.nres)
    draw(0, c.x + MARGIN, y + 6, "nothing found: Esc to search again", CLR_DIM, CLR_BG);
  for (i = G.rtop; i < G.nres && i - G.rtop < rows; i++, y += ROW_H) {
    uint16_t bg = i == G.rsel ? CLR_SEL : CLR_BG;
    int tw, x = c.x + MARGIN, ty = y + (ROW_H - G.lh + 2) / 2;
    api->fill(capp_rect(c.x, y, c.w, ROW_H), bg);
    fit(1, G.res[i].title, c.w - 2 * MARGIN, buf, sizeof buf);
    draw(1, x, ty, buf, CLR_LINK, bg);
    tw = str_w(1, buf, 1 << 14) + 8;
    if (G.res[i].desc[0] && tw < c.w - 2 * MARGIN - 30) {
      fit(0, G.res[i].desc, c.w - 2 * MARGIN - tw, buf, sizeof buf);
      draw(0, x + tw, ty, buf, CLR_DIM, bg);
    }
  }
  footer_paint(api, c, "enter open  n search  r random");
}

static void paint_article(void) {
  CRect c = G.content;
  int y = c.y + TOP_H, i, bottom = c.y + c.h - FOOT_H;
  char buf[160], foot[40];
  api->fill(capp_rect(c.x, y, c.w, bottom - y), CLR_BG);
  y += 1;
  for (i = G.top; i < G.nlines && y + line_h(i) <= bottom; i++) {
    const Line *l = &G.line[i];
    int n = l->len < (int)sizeof buf - 1 ? l->len : (int)sizeof buf - 1;
    if (l->kind != L_GAP) {
      api->mem_cpy(buf, G.art + l->off, (size_t)n);
      buf[n] = 0;
      if (l->kind == L_BODY) draw(0, c.x + MARGIN, y, buf, CLR_TEXT, CLR_BG);
      else draw(1, c.x + MARGIN, y + (l->kind == L_H2 ? 2 : 0), buf,
                l->kind == L_H2 ? CLR_HEAD : CLR_LINK, CLR_BG);
      if (l->kind == L_H2)
        api->fill(capp_rect(c.x + MARGIN, y + G.lhb + 1, c.w - 2 * MARGIN, 1), CLR_RULE);
    }
    y += line_h(i);
  }
  /* Where we are: a thin bar down the right edge for this page. */
  if (G.nlines > fits_from(G.top)) {
    int h = bottom - c.y - TOP_H, bh = h * fits_from(G.top) / G.nlines, by;
    if (bh < 4) bh = 4;
    by = c.y + TOP_H + (h - bh) * G.top / (G.nlines - fits_from(G.top) > 0 ? G.nlines - fits_from(G.top) : 1);
    if (by + bh > bottom) by = bottom - bh;
    api->fill(capp_rect(c.x + c.w - 2, by, 2, bh), CLR_RULE);
  }
  if (G.pages > 1) api->fmt(foot, sizeof foot, "p%d/%d  space more  r random", G.page, G.pages);
  else api->fmt(foot, sizeof foot, "space more  r random  p print");
  footer_paint(api, c, foot);
}

static void app_paint(void *st, CRect c) {
  char top[TITLE_MAX + 16];
  (void)st;
  G.content = c;
  /* The lines were wrapped before the first paint said how wide the screen
   * is, at the default. The same answer every strip: paint runs per strip. */
  if (G.view == V_ARTICLE && G.wrap_w != text_w()) {
    reflow();
    if (G.top >= G.nlines) G.top = G.nlines ? G.nlines - 1 : 0;
  }
  if (G.view == V_ARTICLE) {
    paint_top(G.title);
    paint_article();
  } else if (G.view == V_RESULTS) {
    api->fmt(top, sizeof top, "\"%s\"", G.searched);
    paint_top(top);
    paint_results();
  } else {
    paint_top("Wikipedia");
    paint_search();
  }
}

/* ---- keys --------------------------------------------------------------------------- */

static void line_down(void) {
  if (G.top + fits_from(G.top) < G.nlines) G.top++;
  else if (G.page < G.pages) fetch_page(G.title, G.page + 1, 0);
}

static void line_up(void) {
  if (G.top > 0) G.top--;
  else if (G.page > 1) fetch_page(G.title, G.page - 1, 1);
}

static void screen_down(void) {
  int next = G.top + fits_from(G.top);
  if (next < G.nlines) G.top = next;
  else if (G.page < G.pages) fetch_page(G.title, G.page + 1, 0);
  else say("the end", 0);
}

static void screen_up(void) {
  if (G.top > 0) G.top = top_before(G.top);
  else if (G.page > 1) fetch_page(G.title, G.page - 1, 1);
}

static int key_search(uint8_t k) {
  if (k == CAPP_KEY_ENTER) {
    if (G.qlen) search(G.query);
    else random_article();
    return 1;
  }
  if (k == CAPP_KEY_ESC) {
    if (G.qlen) { G.qlen = 0; G.query[0] = 0; return 1; }
    if (G.nres) { G.view = V_RESULTS; return 1; }
    return 0;                          /* the top: Esc is declined */
  }
  if (k == CAPP_KEY_BACK) { if (G.qlen) G.query[--G.qlen] = 0; return 1; }
  if (k >= 32 && k < 127 && G.qlen < QUERY_MAX - 1) {
    G.query[G.qlen++] = (char)k;
    G.query[G.qlen] = 0;
    return 1;
  }
  return 1;
}

static int key_results(uint8_t k) {
  int rows = result_rows();
  switch (k) {
  case CAPP_KEY_UP:
    if (G.rsel > 0) G.rsel--;
    if (G.rsel < G.rtop) G.rtop = G.rsel;
    return 1;
  case CAPP_KEY_DOWN:
    if (G.rsel < G.nres - 1) G.rsel++;
    if (G.rsel >= G.rtop + rows) G.rtop = G.rsel - rows + 1;
    return 1;
  case CAPP_KEY_ENTER:
    if (G.rsel < G.nres) { G.from_results = 1; fetch_page(G.res[G.rsel].title, 1, 0); }
    return 1;
  case CAPP_KEY_ESC: case CAPP_KEY_BACK: case 'n': case 'N': case 'e': case 'E':
    G.view = V_SEARCH;
    return 1;
  case 'r': case 'R': random_article(); return 1;
  }
  return 0;
}

static int key_article(uint8_t k) {
  switch (k) {
  case CAPP_KEY_DOWN:  line_down(); return 1;
  case CAPP_KEY_UP:    line_up(); return 1;
  case CAPP_KEY_RIGHT: case ' ': screen_down(); return 1;
  case CAPP_KEY_LEFT:  screen_up(); return 1;
  case CAPP_KEY_ESC: case CAPP_KEY_BACK:
    G.view = G.from_results && G.nres ? V_RESULTS : V_SEARCH;
    return 1;
  case 'r': case 'R': random_article(); return 1;
  case 'n': case 'N': G.view = V_SEARCH; return 1;
  case 'p': case 'P': print_page(); return 1;
  }
  return 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (G.view == V_SEARCH) return key_search(k);
  if (G.view == V_RESULTS) return key_results(k);
  return key_article(k);
}

static int app_wants_text(void *st) { (void)st; return G.view == V_SEARCH; }

static int app_tick(void *st, uint32_t now_ms) {
  int changed;
  (void)st; (void)now_ms;
  changed = poll();
  if (G.printing) {
    const char *ps = api->print_status();
    if (!(ps[0] == 's' || ps[0] == 'c' || (ps[0] == 'p' && ps[5] == 'i')))
      G.printing = 0;                 /* not starting/connecting/printing */
    if (!str_same(ps, G.status)) { say(ps, 0); changed = 1; }
  }
  return changed;
}

/* ---- commands --------------------------------------------------------------------------- */

enum { ACT_PRINT = 1, ACT_RANDOM, ACT_SUMMARY, ACT_FEATURED, ACT_OPEN };

static const CappParam P_TOPIC[] = {
  { "topic", CAPP_ARG_TEXT, "what to look up: the moon, Ada Lovelace" },
};

static const CappAction ACTIONS[] = {
  { "print",  "Print",          "Article", CAPP_KEY_PRINT, ACT_PRINT },   /* fn-p */
  { "random", "Random article", "Article", 0, ACT_RANDOM },
  { "summary", "Summary", 0, 0, ACT_SUMMARY,
    "the first paragraph of Wikipedia's article on a topic", P_TOPIC, 1,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "featured", "Featured", 0, 0, ACT_FEATURED,
    "today's featured Wikipedia article: its title and first paragraph", 0, 0,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "open", "Look up", 0, 0, ACT_OPEN, "open Wikipedia searching for a topic", P_TOPIC, 1,
    CAPP_CMD_YES | CAPP_CMD_OPEN },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int app_action(void *st, int a) {
  (void)st;
  if (a == ACT_PRINT) { print_page(); return 1; }
  if (a == ACT_RANDOM) { random_article(); return 1; }
  return 0;
}

/* A blocking request for a command: the console and voice wait anyway. */
static int fetch(const char *rel) {
  char url[384];
  int n;
  if (!api->net_ready() && api->net_connect(20000) != 0) return -2;
  api->fmt(url, sizeof url, "%s%s", api->proxy(), rel);
  n = api->http("GET", url, 0, 0, "", G.reply, sizeof G.reply, 20000);
  if (n >= 0) G.reply[n < (int)sizeof G.reply ? n : (int)sizeof G.reply - 1] = 0;
  return n;
}

static int failed(int n, char *out, size_t sz) {
  if (n == -2) api->fmt(out, sz, "%s", api->net_status());
  else if (str_starts(G.reply, "error ")) {
    api->fmt(out, sz, "%s", G.reply + 6);
    if (sz && out[0]) { size_t i; for (i = 0; out[i]; i++) if (out[i] == '\n') out[i] = 0; }
  } else api->fmt(out, sz, "cannot reach the server (%d)", n);
  return -1;
}

/* `title <tab> description`, then a paragraph: the paragraph, or with
 * `with_title` the title on a line of its own first. Cut at a word, with
 * "...", when it is longer than `out`. */
static void summary_out(int with_title, char *out, size_t n) {
  char title[TITLE_MAX];
  const char *para = tsv_next_line(G.reply);
  size_t o = 0, plen;
  tsv_field(G.reply, 0, title, sizeof title);
  if (with_title) o = (size_t)api->fmt(out, n, "%s\n", title);
  if (o >= n) o = n ? n - 1 : 0;
  plen = 0;
  while (para[plen] && para[plen] != '\n') plen++;
  if (o + plen + 1 > n) {
    size_t room = n - o, k;
    if (room < 5) { out[o] = 0; return; }
    k = room - 4;                           /* "..." and the NUL */
    while (k > 0 && para[k] != ' ') k--;
    if (!k) k = room - 4;
    api->mem_cpy(out + o, para, k);
    api->mem_cpy(out + o + k, "...", 4);
    return;
  }
  api->mem_cpy(out + o, para, plen);
  out[o + plen] = 0;
}

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  char rel[40 + QUERY_MAX * 3];
  int r, k;
  (void)st;
  switch (action) {
  case ACT_SUMMARY:
    if (argc < 1 || !argv[0][0]) { api->fmt(out, n, "summary of what?"); return -1; }
    k = api->fmt(rel, sizeof rel, "/wiki/summary?q=");
    url_enc(rel + k, (int)sizeof rel - k, argv[0]);
    if ((r = fetch(rel)) < 0) return failed(r, out, n);
    summary_out(0, out, n);
    return 0;
  case ACT_FEATURED:
    if ((r = fetch("/wiki/featured")) < 0) return failed(r, out, n);
    summary_out(1, out, n);
    return 0;
  }
  api->fmt(out, n, "wiki has no command %d", action);
  return -1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Wikipedia",
  /* 16x16: a round globe with a W on it. */
  { 0x07, 0xE0, 0x18, 0x18, 0x20, 0x04, 0x40, 0x02,
    0x40, 0x02, 0x90, 0x09, 0x91, 0x89, 0x89, 0x91,
    0x8A, 0x51, 0x8A, 0x51, 0x44, 0x22, 0x44, 0x22,
    0x20, 0x04, 0x18, 0x18, 0x07, 0xE0, 0x00, 0x00 },
  "enter\tsearch; with nothing typed, a random article\n"
  "\tin the results: open the highlighted one\n"
  "up/down\tchoose; in an article, a line\n"
  "left/right\ta screen back or on (space: on)\n"
  "\tpast the end, the next page is fetched\n"
  "n\ta new search\n"
  "r\ta random article\n"
  "p\tprint the page loaded\n"
  "esc\tback: article, results, search\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  int i;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  G.f_ui = G.f_uib = -1;
  if (!api->headless()) {
    G.f_ui = api->font_load("ui13");
    G.f_uib = api->font_load("ui13b");
  }
  measure_fonts();
  /* `open TOPIC`, or `run wiki the moon`: the words are the search. */
  for (i = 1; i < argc; i++) {
    int n = (int)api->str_len(argv[i]);
    if (G.qlen && G.qlen < QUERY_MAX - 1) G.query[G.qlen++] = ' ';
    if (G.qlen + n > QUERY_MAX - 1) n = QUERY_MAX - 1 - G.qlen;
    if (n > 0) { api->mem_cpy(G.query + G.qlen, argv[i], (size_t)n); G.qlen += n; }
    G.query[G.qlen] = 0;
  }
  if (G.qlen && !api->headless()) search(G.query);
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
