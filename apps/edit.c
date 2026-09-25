/* A small code editor, as a loadable CardOS app.
 *
 * Dark, because that is what an editor looks like and because a 240x135 panel
 * held close is easier on the eyes dark than a sheet of white. Line numbers in
 * a gutter, the current line marked, a status bar that says what file and
 * whether it is saved.
 *
 * Opening and saving go through the OS file picker (api->pick), which is
 * also where folders get made and files renamed. This editor used to carry a
 * browser and a name prompt of its own; the picker replaced about a hundred
 * and fifty lines of them, and every other app gets the same dialog.
 *
 * The buffer is a flat array of fixed-width lines rather than a gap buffer: a
 * file this thing is for is a few kilobytes, and a flat array is the version
 * that is obviously correct at a glance.
 */

#include "kernel/app/capp.h"
#include "apps/toolbar.h"

#define MAXLINES  96
#define MAXCOL    64
#define ROWH      9
#define GUTTER    18      /* three digits and a separator */
#define CHARW     6

/* A dark palette that keeps syntax-free text readable at this size. Comment
 * green is used for the gutter, not for comments -- there is no parser here,
 * and pretending otherwise would highlight the wrong things. */
#define CLR_BG      CAPP_RGB(24, 26, 32)
#define CLR_GUTTER  CAPP_RGB(34, 37, 45)
#define CLR_LINENO  CAPP_RGB(92, 102, 120)
#define CLR_TEXT    CAPP_RGB(214, 220, 232)
#define CLR_CUR_BG  CAPP_RGB(38, 42, 52)
#define CLR_CARET   CAPP_RGB(120, 200, 255)
#define CLR_BAR     CAPP_RGB(48, 82, 128)
#define CLR_BAR_FG  CAPP_RGB(232, 238, 248)
#define CLR_DIRTY   CAPP_RGB(255, 190, 90)
#define CLR_SEL     CAPP_RGB(52, 80, 116)
#define CLR_DIM     CAPP_RGB(130, 140, 158)

/* Preview: a page, not a terminal. Lighter ground and darker ink, because
 * prose is read rather than scanned, and every markdown renderer anyone has
 * seen looks like paper. */
#define CLR_PG      CAPP_RGB(238, 238, 232)
#define CLR_PG_TX   CAPP_RGB(32, 34, 40)
#define CLR_PG_H    CAPP_RGB(16, 40, 96)
#define CLR_PG_DIM  CAPP_RGB(110, 116, 128)
#define CLR_PG_RULE CAPP_RGB(180, 182, 176)
#define CLR_PG_CODE CAPP_RGB(216, 218, 210)
#define CLR_PG_LINK CAPP_RGB(24, 82, 170)
#define CLR_PG_QUOT CAPP_RGB(150, 154, 160)

typedef enum { VIEW_EDIT = 0, VIEW_PREVIEW } View;

static const CardApi *api;

enum { PICK_NONE = 0, PICK_OPEN, PICK_SAVE };

static struct {
  View view;

  /* The OS file picker is up, and what for. The answer arrives in tick. */
  int  picking;                 /* 0, or PICK_* */

  /* buffer */
  char line[MAXLINES][MAXCOL + 1];
  short len[MAXLINES];
  int   nlines;
  int   cx, cy;
  int   top, leftcol;
  int   dirty;
  int   truncated;
  char  path[96];
  char  status[40];

  /* markdown preview */
  int   ptop;                 /* first rendered row on screen */
  int   prows;                /* how many rows the document rendered to */
  int   pmore;                /* rows are left below the screen */

  /* paper: the buffer joined with newlines, and whether the strip is
   * showing the job's progress */
  char page[MAXLINES * (MAXCOL + 1) + 1];
  int  printing;
} E;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *m) { api->fmt(E.status, sizeof E.status, "%s", m); }

/* ------------------------------------------------------------- buffer ---- */

static void blank(void) {
  api->mem_set(E.line, 0, sizeof E.line);
  api->mem_set(E.len, 0, sizeof E.len);
  E.nlines = 1;
  E.cx = E.cy = E.top = E.leftcol = 0;
  E.dirty = 0;
  E.truncated = 0;
}

static void load(const char *path) {
  char buf[512];
  int fd, n, i, col = 0;

  blank();
  api->fmt(E.path, sizeof E.path, "%s", path);

  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) { say("new file"); return; }

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c == 13) continue;
      if (c == 10) {
        E.len[E.nlines - 1] = (short)col;
        col = 0;
        if (E.nlines >= MAXLINES) { E.truncated = 1; goto out; }
        E.nlines++;
        continue;
      }
      if (c == 9) {                     /* tabs become two spaces */
        int t;
        for (t = 0; t < 2 && col < MAXCOL; t++) E.line[E.nlines - 1][col++] = ' ';
        continue;
      }
      /* A control byte means this is not text. Say so rather than drawing
       * 14 KB of ELF as characters. */
      if (c < 32 || (unsigned char)c > 126) { E.truncated = 2; goto out; }
      if (col < MAXCOL) E.line[E.nlines - 1][col++] = c;
      else E.truncated = 1;
    }
  }
  E.len[E.nlines - 1] = (short)col;
out:
  api->close(fd);
  if (E.truncated == 2) {
    blank();
    say("not a text file");
  } else if (E.truncated) {
    say("opened, truncated to fit");
  } else {
    api->fmt(E.status, sizeof E.status, "%d lines", E.nlines);
  }
}

/* The folder of the current file, for the picker to start in. */
static const char *own_dir(char *buf, size_t n) {
  int i, cut = 0;
  if (!E.path[0]) return NULL;
  for (i = 0; E.path[i]; i++) if (E.path[i] == '/') cut = i;
  if (cut == 0) return NULL;
  api->mem_cpy(buf, E.path, (size_t)cut);
  buf[cut < (int)n ? cut : (int)n - 1] = 0;
  return buf;
}

static const char *own_name(void) {
  const char *slash = NULL, *p;
  for (p = E.path; *p; p++) if (*p == '/') slash = p;
  return slash ? slash + 1 : E.path;
}

/* The OS picker, for a place to save. The answer comes back in tick. */
static void ask_save(void) {
  char dir[96];
  CappPick req;
  req.mode = CAPP_PICK_SAVE;
  req.title = "Save as";
  req.dir = own_dir(dir, sizeof dir);
  req.filter = NULL;
  req.name = E.path[0] ? own_name() : "untitled.txt";
  if (api->pick(&req) == 0) E.picking = PICK_SAVE;
  else say("a picker is already open");
}

static void ask_open(void) {
  char dir[96];
  CappPick req;
  req.mode = CAPP_PICK_OPEN;
  req.title = "Open";
  req.dir = own_dir(dir, sizeof dir);
  req.filter = NULL;
  req.name = NULL;
  if (api->pick(&req) == 0) E.picking = PICK_OPEN;
  else say("a picker is already open");
}

static void save(void) {
  int fd, i;
  char nl = 10;

  /* Empty rather than pre-filled: a buffer with no name came from a pipe or a
   * new file, and the first thing typed should be the name rather than the
   * end of a name you have to delete first. */
  if (!E.path[0]) { ask_save(); return; }
  fd = api->open(E.path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) { say("cannot write"); return; }
  for (i = 0; i < E.nlines; i++) {
    if (E.len[i]) api->write(fd, E.line[i], (size_t)E.len[i]);
    if (i + 1 < E.nlines) api->write(fd, &nl, 1);
  }
  api->close(fd);
  E.dirty = 0;
  say("saved");
}

/* ------------------------------------------------------------- editing --- */

static void scroll_to_cursor(int rows, int cols) {
  if (E.cy < E.top) E.top = E.cy;
  if (E.cy >= E.top + rows) E.top = E.cy - rows + 1;
  if (E.top < 0) E.top = 0;

  if (E.cx < E.leftcol) E.leftcol = E.cx;
  if (E.cx >= E.leftcol + cols) E.leftcol = E.cx - cols + 1;
  if (E.leftcol < 0) E.leftcol = 0;
}

static void insert_char(char c) {
  int i, n = E.len[E.cy];
  if (n >= MAXCOL) return;
  for (i = n; i > E.cx; i--) E.line[E.cy][i] = E.line[E.cy][i - 1];
  E.line[E.cy][E.cx] = c;
  E.len[E.cy] = (short)(n + 1);
  E.cx++;
  E.dirty = 1;
}

static void split_line(void) {
  int i, tail;
  if (E.nlines >= MAXLINES) { say("too many lines"); return; }
  for (i = E.nlines; i > E.cy + 1; i--) {
    api->mem_cpy(E.line[i], E.line[i - 1], MAXCOL + 1);
    E.len[i] = E.len[i - 1];
  }
  E.nlines++;
  tail = E.len[E.cy] - E.cx;
  api->mem_cpy(E.line[E.cy + 1], E.line[E.cy] + E.cx, (size_t)tail);
  E.len[E.cy + 1] = (short)tail;
  E.len[E.cy] = (short)E.cx;

  /* Keep the new line's indent. An editor that drops back to column zero on
   * every return is an editor you fight. */
  {
    int ind = 0;
    while (ind < E.len[E.cy] && E.line[E.cy][ind] == ' ') ind++;
    if (ind > 0 && E.len[E.cy + 1] + ind <= MAXCOL) {
      for (i = E.len[E.cy + 1]; i >= 0; i--)
        E.line[E.cy + 1][i + ind] = E.line[E.cy + 1][i];
      for (i = 0; i < ind; i++) E.line[E.cy + 1][i] = ' ';
      E.len[E.cy + 1] = (short)(E.len[E.cy + 1] + ind);
      E.cx = ind;
    } else {
      E.cx = 0;
    }
  }
  E.cy++;
  E.dirty = 1;
}

static void join_prev(void) {
  int i, prev;
  if (E.cy == 0) return;
  prev = E.len[E.cy - 1];
  if (prev + E.len[E.cy] > MAXCOL) { say("line would be too long"); return; }
  api->mem_cpy(E.line[E.cy - 1] + prev, E.line[E.cy], (size_t)E.len[E.cy]);
  E.len[E.cy - 1] = (short)(prev + E.len[E.cy]);
  for (i = E.cy; i + 1 < E.nlines; i++) {
    api->mem_cpy(E.line[i], E.line[i + 1], MAXCOL + 1);
    E.len[i] = E.len[i + 1];
  }
  E.nlines--;
  E.cy--;
  E.cx = prev;
  E.dirty = 1;
}

static void backspace(void) {
  int i;
  if (E.cx == 0) { join_prev(); return; }
  for (i = E.cx - 1; i + 1 < E.len[E.cy]; i++) E.line[E.cy][i] = E.line[E.cy][i + 1];
  E.len[E.cy]--;
  E.cx--;
  E.dirty = 1;
}

/* ------------------------------------------------------------ painting --- */


/* ------------------------------------------------------- markdown ---- */

/* A preview, not a parser.
 *
 * Markdown is a large specification and almost none of it earns its place on
 * a 240-pixel screen. What is here is what people actually write in notes:
 * headings, lists, quotes, code, rules, and bold, emphasis, `code` and links
 * inside a line. Anything unrecognised is drawn as the text it is, which is
 * markdown's whole premise and a good fallback.
 *
 * It renders from the edit buffer rather than the file, so a preview shows
 * what you have typed and not what you last saved.
 *
 * Set in Atkinson Hyperlegible (ui13, ui13b from /fonts), a sans-serif, so a
 * note reads as a page and not as a terminal; code, fenced or `inline`, stays
 * in the 6x8 console font, where monospace is the point. Without the fonts on
 * the card everything falls back to 6x8 and still wraps.
 *
 * Lines are word-wrapped to the width of the screen, measured in the font
 * they are drawn in: a line breaks at the last space that fits, a word too
 * long for a whole row is broken inside, and nothing is cut off. Wrapped rows
 * of a list item or a quote hang under the text, not under the marker. The
 * layout is walked for every paint -- ninety-six lines at most -- and draws
 * only the rows on screen, so there is one description of it, not two.
 */

#define PG_LEFT   4          /* the page margin */
#define PG_CODE_H 10         /* a row of 6x8 code */

typedef enum {
  MD_TEXT = 0, MD_H1, MD_H2, MD_H3, MD_BULLET, MD_NUMBER,
  MD_QUOTE, MD_CODE, MD_RULE, MD_BLANK
} MdKind;

/* The style of each character once the markers are out of the line. */
enum { ST_N = 0, ST_B, ST_E, ST_C, ST_L };   /* normal bold emph code link */

static struct {
  int     loaded;
  int     body, bold;           /* font handles; -1 is the 6x8 font */
  int     line_h;               /* a row of prose */
  char    ch[MAXCOL + 1];       /* the line being laid out, markers removed */
  uint8_t st[MAXCOL + 1];
  int     n;
} M;

static void md_fonts(void) {
  int hb, hbb;
  if (M.loaded) return;
  M.loaded = 1;
  M.body = api->font_load ? api->font_load("ui13") : -1;
  M.bold = api->font_load ? api->font_load("ui13b") : -1;
  hb = api->font_height ? api->font_height(M.body) : 8;
  hbb = api->font_height ? api->font_height(M.bold) : 8;
  M.line_h = (hb > hbb ? hb : hbb) + 2;
}

/* Strip the markers a line begins with, and say what it was. */
static MdKind md_kind(const char *in, const char **body, int *indent) {
  const char *p = in;
  int spaces = 0;

  while (*p == ' ') { p++; spaces++; }
  *indent = spaces / 2;
  *body = p;

  if (!*p) return MD_BLANK;

  if (p[0] == '#' && p[1] == '#' && p[2] == '#' && p[3] == ' ') { *body = p + 4; return MD_H3; }
  if (p[0] == '#' && p[1] == '#' && p[2] == ' ')                { *body = p + 3; return MD_H2; }
  if (p[0] == '#' && p[1] == ' ')                               { *body = p + 2; return MD_H1; }

  /* Three or more of - _ * alone on a line. */
  if (p[0] == '-' || p[0] == '_' || p[0] == '*') {
    const char *q = p;
    int n = 0;
    while (*q == *p) { q++; n++; }
    while (*q == ' ') q++;
    if (n >= 3 && !*q) return MD_RULE;
  }

  if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') {
    *body = p + 2;
    return MD_BULLET;
  }
  if (p[0] >= '0' && p[0] <= '9') {
    const char *q = p;
    while (*q >= '0' && *q <= '9') q++;
    if ((q[0] == '.' || q[0] == ')') && q[1] == ' ') { *body = p; return MD_NUMBER; }
  }
  if (p[0] == '>' ) { *body = (p[1] == ' ') ? p + 2 : p + 1; return MD_QUOTE; }

  return MD_TEXT;
}

static void md_put(char c, int st) {
  if (M.n >= MAXCOL) return;
  M.ch[M.n] = c;
  M.st[M.n] = (uint8_t)st;
  M.n++;
  M.ch[M.n] = 0;
}

static int md_space(char c) { return c == 0 || c == ' '; }

/* The line into M.ch and M.st with the inline markers taken out: **bold**,
 * *emph* and _emph_, `code`, and [label](url) reduced to its label -- the URL
 * will not fit and could not be followed from here anyway. `verbatim` for a
 * fenced block, where markers are content. One pass, no nesting. An
 * underscore inside a word (snake_case) is a character, not emphasis. */
static void md_inline(const char *s, int verbatim) {
  int bold = 0, emph = 0, code = 0;
  const char *start = s;

  M.n = 0;
  M.ch[0] = 0;
  if (verbatim) {
    while (*s) md_put(*s++, ST_C);
    return;
  }
  while (*s) {
    if (code) {
      if (*s == '`') { code = 0; s++; continue; }
      md_put(*s++, ST_C);
      continue;
    }
    if (s[0] == '*' && s[1] == '*') { bold = !bold; s += 2; continue; }
    if (s[0] == '*') { emph = !emph; s++; continue; }
    if (s[0] == '_' && (s == start || md_space(s[-1]) || md_space(s[1]) ||
                        s[1] == '.' || s[1] == ',')) {
      emph = !emph; s++; continue;
    }
    if (s[0] == '`') { code = 1; s++; continue; }
    if (s[0] == '[') {
      const char *close = s + 1, *end;
      while (*close && *close != ']') close++;
      if (*close == ']' && close[1] == '(') {
        end = close + 2;
        while (*end && *end != ')') end++;
        if (*end == ')') {
          for (s++; s < close; s++) md_put(*s, ST_L);
          s = end + 1;
          continue;
        }
      }
    }
    md_put(*s++, bold ? ST_B : emph ? ST_E : ST_N);
  }
}

/* The font a character is drawn in. A heading is bold throughout. */
static int md_font(int st, int heading) {
  if (st == ST_C) return -1;
  if (heading || st == ST_B) return M.bold;
  return M.body;
}

/* How wide M.ch[a..b) is, run by run, each run in its own font. */
static int md_width(int a, int b, int heading) {
  char run[MAXCOL + 1];
  int w = 0;
  while (a < b) {
    int st = M.st[a], n = 0, f = md_font(st, heading);
    while (a < b && md_font(M.st[a], heading) == f) run[n++] = M.ch[a++];
    run[n] = 0;
    w += api->text_width(f, run);
  }
  return w;
}

/* Where the row that starts at `from` ends, to fit `avail` pixels: after the
 * last word that fits, or -- a word too wide for a row on its own -- inside
 * it, one character at least, so the walk always moves on. */
static int md_break(int from, int avail, int heading) {
  int fit = -1, i = from, k;
  while (i < M.n) {
    int j = i;
    while (j < M.n && M.ch[j] != ' ') j++;
    if (md_width(from, j, heading) > avail) break;
    fit = j;
    i = j;
    while (i < M.n && M.ch[i] == ' ') i++;
  }
  if (fit >= 0) return fit;
  for (k = from + 1; k < M.n && md_width(from, k + 1, heading) <= avail; k++) { }
  return k;
}

/* Draw M.ch[a..b) at x, y in a row `h` tall, run by run. */
static void md_draw_run(int x, int y, int h, int a, int b, int heading,
                        uint16_t fg, uint16_t bg) {
  char run[MAXCOL + 1];
  while (a < b) {
    int st = M.st[a], n = 0, f = md_font(st, heading), fh;
    uint16_t rf = fg, rb = bg;
    while (a < b && M.st[a] == st) run[n++] = M.ch[a++];
    run[n] = 0;
    if (st == ST_C) { rf = CLR_PG_H; rb = CLR_PG_CODE; }
    else if (st == ST_L) rf = CLR_PG_LINK;
    else if (st == ST_E) rf = CLR_PG_H;
    fh = api->font_height(f);
    api->text_font(f, (int16_t)x, (int16_t)(y + (h - fh) / 2), run, rf, rb);
    x += api->text_width(f, run);
  }
}

/* A row the layout produced: which source line, and which of its characters.
 * The host tests read these to check the wrap; drawing does not need them. */
typedef void (*MdRowFn)(int line, int from, int to);
static MdRowFn md_row_hook;

/* Walk the buffer. Rows before `from` are laid out and skipped; rows from it
 * are drawn into `c` for as long as they fit. Returns how many rows the
 * document takes; E.pmore says whether any were left below the screen. */
static int md_render(CRect c, int from) {
  int i, row = 0, y = c.y;
  int fenced = 0;
  int bottom = c.y + c.h;

  md_fonts();
  E.pmore = 0;

  for (i = 0; i < E.nlines; i++) {
    const char *body = E.line[i];
    int indent = 0, heading, h, x0, xtext, avail, pos, first = 1;
    MdKind k;
    uint16_t fg = CLR_PG_TX, bg = CLR_PG;

    /* Editing keeps E.len and leaves old bytes past it; the preview reads
     * lines as strings, so it terminates each one first, or a line that was
     * backspaced or split showed its old tail. */
    E.line[i][E.len[i]] = 0;

    /* A fenced block is verbatim: no headings, no bullets, no emphasis. */
    if (E.line[i][0] == '`' && E.line[i][1] == '`' && E.line[i][2] == '`') {
      fenced = !fenced;
      continue;
    }
    k = fenced ? MD_CODE : md_kind(E.line[i], &body, &indent);
    if (fenced) body = E.line[i];

    heading = (k == MD_H1 || k == MD_H2);
    h = (k == MD_CODE) ? PG_CODE_H : M.line_h;
    x0 = c.x + PG_LEFT + indent * 12;
    xtext = x0;
    if (k == MD_BULLET) xtext += 8;
    if (k == MD_QUOTE) xtext += 6;
    avail = c.x + c.w - PG_LEFT - xtext;
    if (avail < 24) avail = 24;
    if (k == MD_H1 || k == MD_H2 || k == MD_H3) fg = CLR_PG_H;
    if (k == MD_QUOTE) fg = CLR_PG_DIM;
    if (k == MD_CODE) bg = CLR_PG_CODE;

    /* Blank lines and rules are rows of their own, shorter than text. */
    if (k == MD_BLANK || k == MD_RULE) {
      h = (k == MD_BLANK) ? M.line_h / 2 : 7;
      if (row++ < from) continue;
      if (y + h > bottom) { E.pmore = 1; continue; }
      if (k == MD_RULE)
        api->fill(rect(c.x + PG_LEFT, y + 3, c.w - PG_LEFT * 2, 1), CLR_PG_RULE);
      y += h;
      continue;
    }

    md_inline(body, k == MD_CODE);
    pos = 0;
    do {
      int end = M.n == 0 ? 0 : md_break(pos, avail, heading);
      if (md_row_hook) md_row_hook(i, pos, end);
      if (row++ >= from) {
        if (y + h > bottom) E.pmore = 1;
        else {
          if (k == MD_CODE)
            api->fill(rect(c.x + PG_LEFT, y, c.w - PG_LEFT * 2, h), CLR_PG_CODE);
          if (k == MD_QUOTE)
            api->fill(rect(x0, y, 2, h), CLR_PG_QUOT);
          /* A square, because the font has no bullet and a hyphen reads as a
           * hyphen. On the first row only: the rest hang under the text. */
          if (k == MD_BULLET && first)
            api->fill(rect(x0 + 1, y + h / 2 - 1, 3, 3), CLR_PG_TX);
          md_draw_run(xtext, y, h, pos, end, heading, fg, bg);
          y += h;
          if (k == MD_H1 && end >= M.n)
            api->fill(rect(c.x + PG_LEFT, y - 1, c.w - PG_LEFT * 2, 1), CLR_PG_RULE);
        }
      }
      first = 0;
      pos = end;
      while (pos < M.n && M.ch[pos] == ' ') pos++;
    } while (pos < M.n);
  }
  return row;
}

static void paint_preview(CRect c) {
  char bar[48];

  if (E.ptop < 0) E.ptop = 0;
  api->fill(rect(c.x, c.y, c.w, c.h - ROWH), CLR_PG);
  E.prows = md_render(rect(c.x, c.y + 2, c.w, c.h - ROWH - 2), E.ptop);

  /* Clamp here rather than in the key handler: the length is only known once
   * it has been laid out, and laying it out is what this just did. A scroll
   * past the end draws nothing, so it steps back and draws again. */
  if (E.ptop >= E.prows && E.prows > 0) {
    E.ptop = E.prows - 1;
    api->fill(rect(c.x, c.y, c.w, c.h - ROWH), CLR_PG);
    E.prows = md_render(rect(c.x, c.y + 2, c.w, c.h - ROWH - 2), E.ptop);
  }

  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  api->fmt(bar, sizeof bar, "preview  %s  ctrl-p edits",
           E.path[0] ? E.path : "(unsaved)");
  api->text((short)(c.x + 2), (short)(c.y + c.h - ROWH + 1), bar,
            CLR_BAR_FG, CLR_BAR);
}

static void paint_edit(CRect c) {
  int rows = (c.h - ROWH) / ROWH;
  int cols = (c.w - GUTTER) / CHARW;
  char buf[MAXCOL + 8];
  int r;

  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  scroll_to_cursor(rows, cols);

  /* No full-screen clear. Each row paints its own background as it goes, so a
   * keystroke redraws rows rather than wiping 240x135 to one colour and
   * drawing over it -- which at 40MHz is 12ms of flat background on every
   * character typed, and reads as a flash. */
  for (r = 0; r < rows; r++) {
    int i = E.top + r;
    short y = (short)(c.y + r * ROWH);
    int on_cursor = (i == E.cy);
    uint16_t bg = on_cursor ? CLR_CUR_BG : CLR_BG;
    int n;

    api->fill(rect(c.x, y, GUTTER, ROWH), CLR_GUTTER);
    api->fill(rect(c.x + GUTTER, y, c.w - GUTTER, ROWH), bg);

    if (i >= E.nlines) continue;      /* cleared, so deleted lines disappear */

    api->fmt(buf, sizeof buf, "%3d", i + 1);
    api->text((short)(c.x + 1), y, buf,
              on_cursor ? CLR_TEXT : CLR_LINENO, CLR_GUTTER);

    n = E.len[i] - E.leftcol;
    if (n > cols) n = cols;
    if (n > 0) {
      api->mem_cpy(buf, E.line[i] + E.leftcol, (size_t)n);
      buf[n] = 0;
      api->text((short)(c.x + GUTTER), y, buf, CLR_TEXT, bg);
    }

    if (on_cursor) {
      short cxp = (short)(c.x + GUTTER + (E.cx - E.leftcol) * CHARW);
      api->fill(rect(cxp, y, 1, 8), CLR_CARET);
    }
  }

  /* Status bar: the two things you look down for are which file and whether it
   * is saved. The dot is the unsaved marker, coloured rather than lettered so
   * it reads without being parsed. */
  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  if (E.dirty) api->fill(rect(c.x + 2, c.y + c.h - ROWH + 3, 3, 3), CLR_DIRTY);
  api->fmt(buf, sizeof buf, "%s  %d:%d  %s", E.path, E.cy + 1, E.cx + 1, E.status);
  api->text((short)(c.x + 7), (short)(c.y + c.h - ROWH + 1), buf,
            CLR_BAR_FG, CLR_BAR);
}

/* What this editor can be asked to do. Keys map onto these and so do menu
 * items; see apps/toolbar.h for why a menu item is never a keystroke. */
enum { ACT_NEW = 1, ACT_SAVE, ACT_SAVEAS, ACT_OPEN, ACT_PREVIEW, ACT_PRINT,
       ACT_NOTE, ACT_NOTES };

/* Commands (CAPP_CMD_YES): a note straight to the card, dated, without the
 * buffer -- the one thing people ask a notes app for by voice. They used to
 * take five steps from the Claude app: open, new, type, save as, a name. */
#define NOTES_DIR CAPP_HOME "/notes"
static const CappParam P_NOTE[] = { { "text", CAPP_ARG_TEXT, "what the note says" } };

static const CappAction EDIT_ACTIONS[] = {
  { "note",    "Note",    0,      0,    ACT_NOTE,
    "save a new note, named by the time", P_NOTE, 1, CAPP_CMD_YES },
  { "notes",   "Notes",   0,      0,    ACT_NOTES,
    "the saved notes, newest first", 0, 0, CAPP_CMD_YES },
  { "new",     "New",     "File", 0x0E, ACT_NEW },      /* ctrl-n */
  { "save",    "Save",    "File", 0x13, ACT_SAVE },     /* ctrl-s */
  { "saveas",  "Save as", "File", 0x12, ACT_SAVEAS },   /* ctrl-r */
  { "open",    "Open...", "File", 0x0F, ACT_OPEN },     /* ctrl-o */
  { "preview", "Preview", "View", 0x10, ACT_PREVIEW },  /* ctrl-p */
  { "print",   "Print",   "File", CAPP_KEY_PRINT, ACT_PRINT },  /* fn-p */
};
static const TbIcon EDIT_ICONS[] = { { "S", ACT_SAVE } };

#define NEDIT ((int)(sizeof EDIT_ACTIONS / sizeof EDIT_ACTIONS[0]))

/* The buffer, as the printer's markup: it is markdown-shaped already, so a
 * note with `# headings` and `[ ] tasks` prints as the preview shows it.
 * Joined here rather than sent line by line because print() copies once.
 *
 * Set in Atkinson Hyperlegible (fonts/fonts.txt): a 24 px body, its bold
 * for `##` and 34 px bold for `#`. The job loads them, not this app, so
 * closing Edit mid-print is fine; a card without them prints in 6x8. */
static void print_buffer(void) {
  int i, at = 0, rc;
  for (i = 0; i < E.nlines; i++) {
    int n = E.len[i];
    if (at + n + 1 >= (int)sizeof E.page) break;
    api->mem_cpy(E.page + at, E.line[i], (size_t)n);
    at += n;
    E.page[at++] = 10;
  }
  E.page[at] = 0;
  rc = api->print_fonts(E.page, "print24", "print24b", "print34b");
  if (rc == 0)       { E.printing = 1; say("printing..."); }
  else if (rc == -1) say("still printing the last one");
  else if (rc == -2) say("no printer: print scan in the console");
  else               say("could not print: no memory");
}

static int do_action(int a) {
  switch (a) {
  case ACT_NEW:
    /* An empty, unnamed buffer: the name is asked for at the first save,
     * by the picker, in the folder you choose. */
    blank();
    E.path[0] = 0;
    E.view = VIEW_EDIT;
    say("new file, ctrl-s saves");
    return 1;
  case ACT_SAVE:    save(); return 1;
  case ACT_SAVEAS:  ask_save(); return 1;
  case ACT_OPEN:    ask_open(); return 1;
  case ACT_PREVIEW: E.view = VIEW_PREVIEW; E.ptop = 0; return 1;
  case ACT_PRINT:   print_buffer(); return 1;
  default: return 0;
  }
}

/* Only while a page is printing: its progress is the strip, and the last
 * word -- "printed", or why not -- stays until something else is said. */
static int app_tick(void *st, uint32_t now_ms) {
  const char *ps;
  (void)st; (void)now_ms;

  if (E.picking) {
    char path[96];
    int r = api->pick_poll(path, sizeof path);
    if (r != CAPP_PICK_PENDING) {
      int what = E.picking;
      E.picking = PICK_NONE;
      if (r == 1) {
        if (what == PICK_OPEN) { load(path); E.view = VIEW_EDIT; }
        else { api->fmt(E.path, sizeof E.path, "%s", path); save(); }
      } else if (what == PICK_OPEN && !E.path[0]) {
        say("new file, ctrl-s saves");
      }
      return 1;
    }
  }

  if (!E.printing) return 0;
  ps = api->print_status();
  if (!(ps[0] == 's' || ps[0] == 'c' || (ps[0] == 'p' && ps[5] == 'i')))
    E.printing = 0;                   /* not starting/connecting/printing */
  {
    const char *a = ps, *b = E.status;
    while (*a && *a == *b) { a++; b++; }
    if (!*a && !*b) return 0;         /* the strip already says this */
  }
  say(ps);
  return 1;
}

static void app_paint(void *st, CRect c) {
  (void)st;
  /* The bar belongs to the editor. The browser and the name prompt are
   * whole screens of their own and have nothing to put on it. */
  if (E.view == VIEW_PREVIEW) { paint_preview(c); return; }
  {
    CRect full = c;
    /* Only the dropdown moved: draw it and nothing else, or the content
     * under the menu is repainted on every mouse move and flickers. */
    if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
    toolbar_paint_bar(full);
    paint_edit(toolbar_rest(full));
    toolbar_paint_menu(full);
  }
}


/* --------------------------------------------------------------- input --- */

static int key_edit(unsigned char k) {
  switch (k) {
  case CAPP_KEY_LEFT:
    if (E.cx > 0) E.cx--;
    else if (E.cy > 0) { E.cy--; E.cx = E.len[E.cy]; }
    return 1;
  case CAPP_KEY_RIGHT:
    if (E.cx < E.len[E.cy]) E.cx++;
    else if (E.cy + 1 < E.nlines) { E.cy++; E.cx = 0; }
    return 1;
  case CAPP_KEY_UP:
    if (E.cy > 0) { E.cy--; if (E.cx > E.len[E.cy]) E.cx = E.len[E.cy]; }
    return 1;
  case CAPP_KEY_DOWN:
    if (E.cy + 1 < E.nlines) { E.cy++; if (E.cx > E.len[E.cy]) E.cx = E.len[E.cy]; }
    return 1;

  case CAPP_KEY_BACK:  backspace(); return 1;
  case CAPP_KEY_ENTER: split_line(); return 1;

  case 0x01: E.cx = 0; return 1;                  /* ctrl-a */
  case 0x05: E.cx = E.len[E.cy]; return 1;        /* ctrl-e */

  default:
    if (k >= 32 && k < 127) { insert_char((char)k); return 1; }
    return 0;
  }
}

/* The preview reads; it does not edit. Anything that would change the text
 * puts you back in the editor first, rather than being quietly ignored. */
static int key_preview(unsigned char k) {
  switch (k) {
  case 0x10:                                      /* ctrl-p toggles back */
  case CAPP_KEY_ENTER:
    E.view = VIEW_EDIT;
    return 1;
  /* Down only while there is more below: the rows are not all one height,
   * so "the last screenful" is whatever the last paint found. */
  case CAPP_KEY_UP:    E.ptop -= 1; return 1;
  case CAPP_KEY_DOWN:  if (E.pmore) E.ptop += 1; return 1;
  case CAPP_KEY_LEFT:  E.ptop -= 6; return 1;
  case CAPP_KEY_RIGHT:
  case ' ':            if (E.pmore) E.ptop += 6; return 1;
  case 'g':            E.ptop = 0; return 1;
  case 0x0F: ask_open(); return 1;                /* ctrl-o, the file list */
  case 0x13: save(); return 1;                    /* ctrl-s still saves */
  default: return 0;
  }
}

/* What the shell calls for a chord out of the table, and what the menu bar
 * and any script reach through. */
static int app_action(void *st, int a) {
  (void)st;
  return do_action(a);
}

/* The commands: files in /home/notes, named by the clock so they sort by when
 * (by a count when there is no clock), and left alone by the open buffer. */
static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  static CappEntry ent[40];
  char path[64];
  CappTime t;
  size_t o = 0;
  int fd, i, j, cnt;
  (void)st;
  (void)argc;

  api->mkdir(NOTES_DIR);
  switch (action) {
  case ACT_NOTE:
    api->now(&t);
    if (t.synced)
      api->fmt(path, sizeof path, "%s/%02u%02u-%02u%02u%02u.txt", NOTES_DIR,
               t.month, t.day, t.hour, t.min, t.sec);
    else {
      cnt = api->list_ex(NOTES_DIR, ent, 40);
      api->fmt(path, sizeof path, "%s/note%03d.txt", NOTES_DIR, cnt > 0 ? cnt + 1 : 1);
    }
    fd = api->open(path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
    if (fd < 0) { api->fmt(out, n, "cannot write %s", path); return -1; }
    api->write(fd, argv[0], api->str_len(argv[0]));
    api->write(fd, "\n", 1);
    api->close(fd);
    api->fmt(out, n, "saved %s", path);
    return 0;

  case ACT_NOTES:
    cnt = api->list_ex(NOTES_DIR, ent, 40);
    if (cnt <= 0) { api->fmt(out, n, "no notes yet"); return 0; }
    /* Newest first: the names sort by time, so the highest name first. */
    for (i = 0; i < cnt && o + 4 < n; i++) {
      int best = -1;
      for (j = 0; j < cnt; j++) {
        const char *a, *b;
        if (ent[j].is_dir || !ent[j].name[0]) continue;
        if (best < 0) { best = j; continue; }
        a = ent[j].name; b = ent[best].name;
        while (*a && *a == *b) { a++; b++; }
        if ((unsigned char)*a > (unsigned char)*b) best = j;
      }
      if (best < 0) break;
      o += (size_t)api->fmt(out + o, n - o, "- %s\n", ent[best].name);
      ent[best].name[0] = 0;               /* taken */
    }
    return 0;
  }
  api->fmt(out, n, "edit has no command %d", action);
  return -1;
}

/* The bar first, and it answers for every key while it has them. fn-b puts
 * the keyboard in it; an item chosen there becomes an action, the same one a
 * click would have produced. */
static int menu_key(unsigned char k, int *handled) {
  int a = toolbar_key(k);
  *handled = 1;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  *handled = 0;
  return 0;
}

static int app_key(void *st, unsigned char k) {
  int handled, r;
  (void)st;
  r = menu_key(k, &handled);
  if (handled) return r;
  if (E.view == VIEW_PREVIEW) return key_preview(k);
  return key_edit(k);
}

static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)button;

  {
    int a = toolbar_click(x, y);
    if (a == TB_CONSUMED) return 1;            /* opened or closed a menu */
    if (a != TB_NONE) return do_action(a);
  }

  {
    int r, i;
    y = (short)(y - toolbar_h());
    r = y / ROWH;
    i = E.top + r;
    if (i < 0 || i >= E.nlines) return 0;
    E.cy = i;
    E.cx = E.leftcol + (x - GUTTER) / CHARW;
    if (E.cx < 0) E.cx = 0;
    if (E.cx > E.len[i]) E.cx = E.len[i];
    return 1;
  }
}

/* The wheel scrolls the view without moving the cursor -- looking somewhere
 * else is not the same as typing there, and a wheel that dragged the caret
 * around would lose your place. */
static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  int changed;
  (void)st; (void)buttons;

  changed = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) changed = 1;
  if (!wheel) return changed;

  if (E.view == VIEW_PREVIEW) {
    if (wheel > 0 || E.pmore) E.ptop -= wheel * 3;
    return 1;
  }
  if (E.view != VIEW_EDIT) return 0;

  E.top -= wheel * 3;
  if (E.top > E.nlines - 1) E.top = E.nlines - 1;
  if (E.top < 0) E.top = 0;
  return 1;
}

/* Only while editing: the preview scrolls with the arrows, and a machine
 * whose arrow keys are ; . , / needs those back. */
static int app_wants_text(void *st) {
  (void)st;
  /* Not while previewing: nothing there takes typing, and saying otherwise
   * would cost the arrow keys, which are how you scroll it. Nor while the
   * menu has the keyboard. */
  if (toolbar_has_keys()) return 0;
  return E.view == VIEW_EDIT;
}

/* Opened with nothing: an empty buffer, and the picker straight away --
 * an editor with no file is an editor with nothing to do. Cancel it and
 * the empty buffer is yours. */
static void app_open(void *st) {
  (void)st;
  blank();
  E.path[0] = 0;
  E.view = VIEW_EDIT;
  ask_open();
}

static void app_set_args(void *st, const char *path) {
  (void)st;
  load(path);
  E.view = VIEW_EDIT;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Edit",
  /* 16x16: a document with a folded corner and ruled lines. */
  { 0x00, 0x00, 0x1F, 0xF0, 0x10, 0x18, 0x10, 0x14,
    0x10, 0x12, 0x10, 0x1F, 0x13, 0xC1, 0x10, 0x01,
    0x13, 0xE1, 0x10, 0x01, 0x13, 0xC1, 0x10, 0x01,
    0x11, 0xE1, 0x10, 0x01, 0x1F, 0xFF, 0x00, 0x00 },
  "arrows\tmove\nenter\tsplit the line\nbackspace\tdelete\nctrl-n\tnew file\nctrl-s\tsave\nctrl-r\tsave as\nctrl-o\topen a file\nctrl-p\tmarkdown preview\nfn-p\tprint\nctrl-a\tstart of line\nctrl-e\tend of line\n",
  EDIT_ACTIONS,
  sizeof EDIT_ACTIONS / sizeof EDIT_ACTIONS[0],
};

/* Static, not a local: the shell keeps calling into this long after
 * capp_main has returned. */
static CappUi UI;

/* Pulls whatever was piped in into the buffer. "cat notes | grep TODO | edit"
 * is the case that matters: the left-hand side produced text and this is where
 * you want to look at it. It stays an unnamed buffer until saved, which is
 * what an editor should do with something that arrived without a file. */
static void load_stdin(void) {
  char line[MAXCOL + 64];
  int n;

  blank();
  E.path[0] = 0;
  while (E.nlines < MAXLINES && (n = api->in_line(line, sizeof line)) >= 0) {
    if (n > MAXCOL) n = MAXCOL;
    api->mem_cpy(E.line[E.nlines - 1], line, (size_t)n);
    E.len[E.nlines - 1] = (short)n;
    E.nlines++;
  }
  if (E.nlines > 1) E.nlines--;        /* the last increment had no line */
  E.dirty = 1;                         /* nothing on the card holds this yet */
  E.view = VIEW_EDIT;
  api->fmt(E.status, sizeof E.status, "%d lines in, ctrl-s names it", E.nlines);
}

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  blank();
  /* Three ways in, in the order a shell would expect: something piped in, a
     named file, or nothing -- and nothing means the picker, because an editor
     with no file has nothing to do. */
  /* None of them when started only for a command: there is no screen for
   * the picker to be on, and a note does not need the buffer. */
  if (api->headless()) { /* the command works on files, not the buffer */ }
  else if (api->has_input()) load_stdin();
  else if (argc > 1) app_set_args(0, argv[1]);
  else app_open(0);
  toolbar_init(api, EDIT_ACTIONS, NEDIT, EDIT_ICONS, 1);

  UI.paint = app_paint;
  UI.tick = app_tick;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  UI.actions = EDIT_ACTIONS;
  UI.nactions = NEDIT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
