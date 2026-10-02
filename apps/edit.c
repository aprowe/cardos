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
#include "apps/footer.h"
#include "apps/safefile.h"

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
  /* The line goes on in the next one: no newline between them in the file.
   * A paragraph longer than MAXCOL is held as a run of these, so a note
   * written on the dashboard as one long line opens whole and saves back as
   * one line. Before, everything past the 64th character was dropped at
   * load -- words cut off mid-sentence, and gone for good at the next save. */
  char  cont[MAXLINES];
  int   nlines;
  int   cx, cy;
  int   top, leftcol;
  int   dirty;
  int   truncated;
  int   wrap;                 /* long lines fold onto the next row (ctrl-w) */
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
  api->mem_set(E.cont, 0, sizeof E.cont);
  E.nlines = 1;
  E.cx = E.cy = E.top = E.leftcol = 0;
  E.dirty = 0;
  E.truncated = 0;
}

/* Make room for a line at y, moving the rest down. 0 when the buffer is full. */
static int open_line(int y) {
  int i;
  if (E.nlines >= MAXLINES) return 0;
  for (i = E.nlines; i > y; i--) {
    api->mem_cpy(E.line[i], E.line[i - 1], MAXCOL + 1);
    E.len[i] = E.len[i - 1];
    E.cont[i] = E.cont[i - 1];
  }
  api->mem_set(E.line[y], 0, MAXCOL + 1);
  E.len[y] = 0;
  E.cont[y] = 0;
  E.nlines++;
  return 1;
}

static void close_line(int y) {
  int i;
  for (i = y; i + 1 < E.nlines; i++) {
    api->mem_cpy(E.line[i], E.line[i + 1], MAXCOL + 1);
    E.len[i] = E.len[i + 1];
    E.cont[i] = E.cont[i + 1];
  }
  E.nlines--;
}

/* Line y is full: carry what follows its last space (or, with no space, its
 * last character) onto a new continuation line, so a word is not split
 * across two lines when it need not be. Where the cut fell, or -1 when there
 * is no room for another line. */
static int soft_split(int y) {
  int at = MAXCOL - 1, s, tail;
  for (s = E.len[y] - 2; s >= 1; s--)
    if (E.line[y][s] == ' ') { at = s + 1; break; }
  if (!open_line(y + 1)) return -1;
  tail = E.len[y] - at;
  api->mem_cpy(E.line[y + 1], E.line[y] + at, (size_t)tail);
  E.len[y + 1] = (short)tail;
  E.len[y] = (short)at;
  E.cont[y + 1] = E.cont[y];
  E.cont[y] = 1;
  return at;
}

/* One character into the last line, as the file is read. 0 when the buffer
 * has no more lines to give. */
static int load_put(char c) {
  int y = E.nlines - 1, at;
  if (E.len[y] >= MAXCOL) {
    if ((at = soft_split(y)) < 0) return 0;
    y++;
  }
  E.line[y][E.len[y]++] = c;
  return 1;
}

static void load(const char *path) {
  char buf[512];
  int fd, n, i;

  blank();
  api->fmt(E.path, sizeof E.path, "%s", path);

  /* Through safefile: a save cut off by a power cut leaves only NAME.tmp,
   * and this is where it is put back. */
  fd = safe_open_read(api, path);
  if (fd < 0) { say("new file"); return; }

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c == 13) continue;
      if (c == 10) {
        if (E.nlines >= MAXLINES) { E.truncated = 1; goto out; }
        E.nlines++;
        continue;
      }
      if (c == 9) {                     /* tabs become two spaces */
        if (!load_put(' ') || !load_put(' ')) { E.truncated = 1; goto out; }
        continue;
      }
      /* A control byte means this is not text. Say so rather than drawing
       * 14 KB of ELF as characters. */
      if (c < 32 || (unsigned char)c > 126) { E.truncated = 2; goto out; }
      if (!load_put(c)) { E.truncated = 1; goto out; }
    }
  }
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

  /* NAME.tmp, then remove and rename (apps/safefile.h): writing the file in
   * place has a moment where the old text is gone and the new is not all
   * there, and a device pulled from a pocket mid-save lost the file in it.
   * safefile holds paths of SF_PATH_MAX; a longer one is rare enough here
   * that it is written in place as before rather than not at all. */
  if (api->str_len(E.path) < SF_PATH_MAX) {
    SafeFile f;
    if (safe_begin(&f, api, E.path) != 0) { say("cannot write"); return; }
    for (i = 0; i < E.nlines; i++) {
      if (E.len[i]) safe_write(&f, E.line[i], (size_t)E.len[i]);
      if (i + 1 < E.nlines && !E.cont[i]) safe_write(&f, &nl, 1);
    }
    if (safe_commit(&f) != 0) { say("not saved: card full?"); return; }
  } else {
    fd = api->open(E.path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
    if (fd < 0) { say("cannot write"); return; }
    for (i = 0; i < E.nlines; i++) {
      if (E.len[i]) api->write(fd, E.line[i], (size_t)E.len[i]);
      if (i + 1 < E.nlines && !E.cont[i]) api->write(fd, &nl, 1);
    }
    api->close(fd);
  }
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
  /* A full line carries on into a continuation line rather than refusing the
   * key: the file still has one line there, however long it gets. */
  if (n >= MAXCOL) {
    int at = soft_split(E.cy);
    if (at < 0) { say("too many lines"); return; }
    if (E.cx > at) { E.cy++; E.cx -= at; }
    n = E.len[E.cy];
  }
  for (i = n; i > E.cx; i--) E.line[E.cy][i] = E.line[E.cy][i - 1];
  E.line[E.cy][E.cx] = c;
  E.len[E.cy] = (short)(n + 1);
  E.cx++;
  E.dirty = 1;
}

static void split_line(void) {
  int i, tail;
  if (!open_line(E.cy + 1)) { say("too many lines"); return; }
  tail = E.len[E.cy] - E.cx;
  api->mem_cpy(E.line[E.cy + 1], E.line[E.cy] + E.cx, (size_t)tail);
  E.len[E.cy + 1] = (short)tail;
  E.len[E.cy] = (short)E.cx;
  /* A real newline now ends this line; whatever it ran on into, the new
   * one runs on into instead. */
  E.cont[E.cy + 1] = E.cont[E.cy];
  E.cont[E.cy] = 0;

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
  int prev;
  if (E.cy == 0) return;
  /* Between a line and its continuation there is no newline to delete, so
   * Backspace takes the character before the cursor, which is the previous
   * line's last -- then the two are joined if they now fit. */
  if (E.cont[E.cy - 1] && E.len[E.cy - 1] > 0) {
    E.len[E.cy - 1]--;
    E.dirty = 1;
    if (E.len[E.cy - 1] + E.len[E.cy] > MAXCOL) return;
  }
  prev = E.len[E.cy - 1];
  if (prev + E.len[E.cy] > MAXCOL) { say("line would be too long"); return; }
  api->mem_cpy(E.line[E.cy - 1] + prev, E.line[E.cy], (size_t)E.len[E.cy]);
  E.len[E.cy - 1] = (short)(prev + E.len[E.cy]);
  E.cont[E.cy - 1] = E.cont[E.cy];
  close_line(E.cy);
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

/* The longest line the preview lays out: a paragraph is the run of lines
 * E.cont joins, up to this. Sixteen full lines -- well past a paragraph. */
#define PARA_MAX  1024

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
  char    ch[PARA_MAX + 1];     /* the line being laid out, markers removed */
  uint8_t st[PARA_MAX + 1];
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
  if (M.n >= PARA_MAX) return;
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

/* How wide M.ch[a..b) is, run by run, each run in its own font. Measured
 * where it lies, by ending the string at the run for a moment, so a long
 * paragraph costs no copy on the shell's stack. */
static int md_width(int a, int b, int heading) {
  int w = 0;
  while (a < b) {
    int f = md_font(M.st[a], heading), e = a;
    char keep;
    while (e < b && md_font(M.st[e], heading) == f) e++;
    keep = M.ch[e];
    M.ch[e] = 0;
    w += api->text_width(f, M.ch + a);
    M.ch[e] = keep;
    a = e;
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
  while (a < b) {
    int st = M.st[a], e = a, f = md_font(st, heading), fh;
    uint16_t rf = fg, rb = bg;
    char keep;
    while (e < b && M.st[e] == st) e++;
    if (st == ST_C) { rf = CLR_PG_H; rb = CLR_PG_CODE; }
    else if (st == ST_L) rf = CLR_PG_LINK;
    else if (st == ST_E) rf = CLR_PG_H;
    keep = M.ch[e];
    M.ch[e] = 0;
    fh = api->font_height(f);
    api->text_font(f, (int16_t)x, (int16_t)(y + (h - fh) / 2), M.ch + a, rf, rb);
    x += api->text_width(f, M.ch + a);
    M.ch[e] = keep;
    a = e;
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
    static char para[PARA_MAX + 1];
    const char *body = para;
    int indent = 0, heading, h, x0, xtext, avail, pos, first = 1, n = 0;
    int line0 = i;
    MdKind k;
    uint16_t fg = CLR_PG_TX, bg = CLR_PG;

    /* The paragraph: this line and the continuations E.cont joins to it,
     * copied by length -- editing leaves old bytes past E.len, and reading
     * the lines as strings showed a backspaced line's old tail. */
    for (;;) {
      int len = E.len[i];
      if (n + len > PARA_MAX) len = PARA_MAX - n;
      api->mem_cpy(para + n, E.line[i], (size_t)len);
      n += len;
      if (!E.cont[i] || i + 1 >= E.nlines) break;
      i++;
    }
    para[n] = 0;

    /* A fenced block is verbatim: no headings, no bullets, no emphasis. */
    if (para[0] == '`' && para[1] == '`' && para[2] == '`') {
      fenced = !fenced;
      continue;
    }
    k = fenced ? MD_CODE : md_kind(para, &body, &indent);
    if (fenced) body = para;

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
      if (md_row_hook) md_row_hook(line0, pos, end);
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
  if (E.ptop < 0) E.ptop = 0;
  api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_PG);
  E.prows = md_render(rect(c.x, c.y + 2, c.w, c.h - FOOT_H - 2), E.ptop);

  /* Clamp here rather than in the key handler: the length is only known once
   * it has been laid out, and laying it out is what this just did. A scroll
   * past the end draws nothing, so it steps back and draws again. */
  if (E.ptop >= E.prows && E.prows > 0) {
    E.ptop = E.prows - 1;
    api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_PG);
    E.prows = md_render(rect(c.x, c.y + 2, c.w, c.h - FOOT_H - 2), E.ptop);
  }

  footer_paint(api, c, "esc edit  arrows scroll  p print");
}

/* ---------------------------------------------------------- word wrap --- */

/* With wrap on, a line wider than the screen folds onto the rows under it
 * instead of scrolling sideways. A row breaks after the last space that
 * fits, or at the width when one word fills it. The last row of a line is
 * kept under `cols` characters so the caret at its end is still on screen;
 * a line that exactly fills its rows gets an empty one for the caret. */
static int wrap_end(int i, int p, int cols) {
  int s;
  if (E.len[i] - p < cols) return E.len[i];
  for (s = p + cols - 1; s > p; s--)
    if (E.line[i][s] == ' ') return s + 1;
  return p + cols;
}

static int wrap_last(int i, int p, int e, int cols) {
  return e >= E.len[i] && e - p < cols;
}

/* How many rows line i takes; or, with cx >= 0, which of them has column cx. */
static int wrap_rows(int i, int cols, int cx) {
  int p = 0, n = 0, e;
  for (;;) {
    e = wrap_end(i, p, cols);
    if (cx >= 0 && (cx < e || wrap_last(i, p, e, cols))) return n;
    n++;
    if (wrap_last(i, p, e, cols)) return n;
    p = e;
  }
}

/* The row `k` rows below the top of the screen: its line and columns. 0 when
 * it is past the end of the buffer. */
static int wrap_row(int k, int cols, int *line, int *from, int *to) {
  int i = E.top, p = 0, e;
  while (i < E.nlines) {
    e = wrap_end(i, p, cols);
    if (k-- == 0) { *line = i; *from = p; *to = e; return 1; }
    if (wrap_last(i, p, e, cols)) { i++; p = 0; }
    else p = e;
  }
  return 0;
}

/* The width the last paint folded at, for a click to find the same rows. */
static int shown_cols = (240 - GUTTER) / CHARW;
/* And the rows, and where the editor was: a key marks rows of that paint. */
static int shown_rows = 1;
static CRect shown_edit;

static void scroll_wrapped(int rows, int cols) {
  int i, below;
  E.leftcol = 0;
  if (E.cy < E.top) E.top = E.cy;
  for (;;) {
    below = wrap_rows(E.cy, cols, E.cx);
    for (i = E.top; i < E.cy; i++) below += wrap_rows(i, cols, -1);
    if (below < rows || E.top >= E.cy) break;
    E.top++;
  }
}

static void paint_edit(CRect c) {
  int rows = (c.h - FOOT_H) / ROWH;
  int cols = (c.w - GUTTER) / CHARW;
  char buf[MAXCOL + 8];
  int r;
  CRect area;

  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  shown_cols = cols;
  if (E.wrap) scroll_wrapped(rows, cols);
  else scroll_to_cursor(rows, cols);
  shown_rows = rows;
  shown_edit = c;
  area = api->paint_area ? api->paint_area() : c;

  /* No clear, not even of a row. The panel has no framebuffer, so a fill
   * that text then writes over is a blink you can see -- and every visible
   * row filled its gutter and its background on every keystroke. text
   * paints its own 6x8 background, so the number and the text are drawn
   * first and only what they leave is filled: the gutter round the digits,
   * the row right of the text, and the ninth pixel row, which ROWH has and
   * the font does not. Background over background does not show. */
  for (r = 0; r < rows; r++) {
    int i = E.top + r, from = E.leftcol, to = 0, last = 1;
    short y = (short)(c.y + r * ROWH);
    int on_cursor, numbered, tx;
    uint16_t bg;
    int n;

    /* Outside what this paint repairs: the shell clips it away anyway. */
    if (y + ROWH <= area.y || y >= area.y + area.h) continue;

    /* Wrapped, a screen row is a piece of a line rather than a line. */
    if (E.wrap && !wrap_row(r, cols, &i, &from, &to)) i = E.nlines;
    else if (E.wrap) last = wrap_last(i, from, to, cols);
    else to = E.len[i < E.nlines ? i : 0];

    on_cursor = (i == E.cy);
    bg = on_cursor ? CLR_CUR_BG : CLR_BG;

    if (i >= E.nlines) {              /* past the end: nothing to draw over */
      api->fill(rect(c.x, y, GUTTER, ROWH), CLR_GUTTER);
      api->fill(rect(c.x + GUTTER, y, c.w - GUTTER, ROWH), bg);
      continue;
    }

    /* The number on a line's first row only, so a folded line reads as one.
     * Its three cells are exactly the gutter. They were drawn a pixel to the
     * right, so the last cell's blank column was written and then written
     * again by the text: a one-pixel stripe blinking down every row. */
    numbered = from == 0 || !E.wrap;
    if (numbered) {
      api->fmt(buf, sizeof buf, "%3d", i + 1);
      api->text((short)c.x, y, buf, on_cursor ? CLR_TEXT : CLR_LINENO, CLR_GUTTER);
    } else {
      api->fill(rect(c.x, y, GUTTER, 8), CLR_GUTTER);
    }
    api->fill(rect(c.x, y + 8, GUTTER, ROWH - 8), CLR_GUTTER);

    n = to - from;
    if (n > cols) n = cols;
    if (n < 0) n = 0;
    if (n > 0) {
      api->mem_cpy(buf, E.line[i] + from, (size_t)n);
      buf[n] = 0;
      api->text((short)(c.x + GUTTER), y, buf, CLR_TEXT, bg);
    }
    tx = GUTTER + n * CHARW;          /* right of the text */
    if (c.w > tx) api->fill(rect(c.x + tx, y, c.w - tx, 8), bg);
    api->fill(rect(c.x + GUTTER, y + 8, c.w - GUTTER, ROWH - 8), bg);

    if (on_cursor && E.cx >= from && (!E.wrap || E.cx < to || last)) {
      short cxp = (short)(c.x + GUTTER + (E.cx - from) * CHARW);
      api->fill(rect(cxp, y, 1, 8), CLR_CARET);
    }
  }

  /* Status bar: the two things you look down for are which file and whether it
   * is saved. The dot is the unsaved marker, coloured rather than lettered so
   * it reads without being parsed. It is the shared footer's strip, holding a
   * status rather than keys: the keys are in the help. Drawn here rather than
   * by footer_paint, which would fill the bar the status is then written
   * over: the same blink, at the bottom, on every key. */
  {
    int fy = c.y + c.h - FOOT_H, fw, fcols = (c.w - 7) / CHARW;
    if (fy + FOOT_H > area.y && fy < area.y + area.h) {
      api->fmt(buf, sizeof buf, "%s  %d:%d  %s", E.path, E.cy + 1, E.cx + 1, E.status);
      if (fcols < 0) fcols = 0;
      if ((int)api->str_len(buf) > fcols) buf[fcols] = 0;
      fw = (int)api->str_len(buf) * CHARW;
      api->fill(rect(c.x, fy, c.w, 2), FOOT_BG);
      api->fill(rect(c.x, fy + 10, c.w, FOOT_H - 10), FOOT_BG);
      if (E.dirty) {                  /* round the dot, then the dot */
        api->fill(rect(c.x, fy + 2, 2, 8), FOOT_BG);
        api->fill(rect(c.x + 5, fy + 2, 2, 8), FOOT_BG);
        api->fill(rect(c.x + 2, fy + 2, 3, 2), FOOT_BG);
        api->fill(rect(c.x + 2, fy + 7, 3, 3), FOOT_BG);
        api->fill(rect(c.x + 2, fy + 4, 3, 3), CLR_DIRTY);
      } else {
        api->fill(rect(c.x, fy + 2, 7, 8), FOOT_BG);
      }
      api->text((short)(c.x + 7), (short)(fy + 2), buf, FOOT_FG, FOOT_BG);
      if (c.w > 7 + fw) api->fill(rect(c.x + 7 + fw, fy + 2, c.w - 7 - fw, 8), FOOT_BG);
    }
  }
}

/* After a key that did not scroll, add or remove lines, or fold anything,
 * only the rows it touched changed: the cursor's old and new rows, and the
 * one above (Backspace at a continuation takes that line's last character).
 * Mark those and the status bar, so the paint is a few rows, not the whole
 * editor. Anything else marks nothing and gets the whole of it. */
static void damage_after_key(int top0, int left0, int cy0, int n0, int view0) {
  CRect c = shown_edit;
  int a, b;
  if (!api->damage || c.w == 0 || E.wrap || view0 != VIEW_EDIT || E.view != VIEW_EDIT)
    return;
  scroll_to_cursor(shown_rows, shown_cols);   /* what the paint would do */
  if (E.top != top0 || E.leftcol != left0 || E.nlines != n0) return;
  a = (cy0 < E.cy ? cy0 : E.cy) - 1 - E.top;
  b = (cy0 > E.cy ? cy0 : E.cy) - E.top;
  if (a < 0) a = 0;
  if (b >= shown_rows) b = shown_rows - 1;
  if (b >= a) api->damage(rect(c.x, c.y + a * ROWH, c.w, (b - a + 1) * ROWH));
  api->damage(rect(c.x, c.y + c.h - FOOT_H, c.w, FOOT_H));
}

/* What this editor can be asked to do. Keys map onto these and so do menu
 * items; see apps/toolbar.h for why a menu item is never a keystroke. */
enum { ACT_NEW = 1, ACT_SAVE, ACT_SAVEAS, ACT_OPEN, ACT_PREVIEW, ACT_PRINT,
       ACT_NOTE, ACT_NOTES, ACT_WRAP };

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
  { "wrap",    "Wrap lines", "View", 0x17, ACT_WRAP },  /* ctrl-w */
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
    if (!E.cont[i]) E.page[at++] = 10;   /* a continuation is the same line */
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
  /* The table's chords reach here before the key handler, in either view,
   * so ctrl-p is a toggle here: in the preview it goes back to editing. */
  case ACT_PREVIEW:
    if (E.view == VIEW_PREVIEW) E.view = VIEW_EDIT;
    else { E.view = VIEW_PREVIEW; E.ptop = 0; }
    return 1;
  case ACT_PRINT:   print_buffer(); return 1;
  /* The editor's wrap, so asked for from the preview it goes back to the
   * editor to show what it did. */
  case ACT_WRAP:
    E.wrap = !E.wrap;
    E.view = VIEW_EDIT;
    say(E.wrap ? "wrap on" : "wrap off");
    return 1;
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
  /* The bar over both views. The preview used to skip it, and fn-b there
   * put the keyboard into a menu nobody could see: every key went to it. */
  {
    CRect full = c;
    /* Only the dropdown moved: draw it and nothing else, or the content
     * under the menu is repainted on every mouse move and flickers. */
    if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
    toolbar_paint_bar(full);
    if (E.view == VIEW_PREVIEW) paint_preview(toolbar_rest(full));
    else paint_edit(toolbar_rest(full));
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

/* The preview reads; it does not edit. Escape is back a level, to the
 * editor, and so is Enter. ctrl-p, ctrl-s and ctrl-o are the action table's
 * and never reach here: do_action answers them in either view. */
static int key_preview(unsigned char k) {
  switch (k) {
  case CAPP_KEY_ESC:
  case CAPP_KEY_ENTER:
    E.view = VIEW_EDIT;
    return 1;
  case 'p':            print_buffer(); return 1;
  /* Down only while there is more below: the rows are not all one height,
   * so "the last screenful" is whatever the last paint found. */
  case CAPP_KEY_UP:    E.ptop -= 1; return 1;
  case CAPP_KEY_DOWN:  if (E.pmore) E.ptop += 1; return 1;
  case CAPP_KEY_LEFT:  E.ptop -= 6; return 1;
  case CAPP_KEY_RIGHT:
  case ' ':            if (E.pmore) E.ptop += 6; return 1;
  case 'g':            E.ptop = 0; return 1;
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
  int i, j, cnt;
  (void)st;
  (void)argc;

  api->mkdir(NOTES_DIR);
  switch (action) {
  /* .md, because the Notes app lists only NAME.md in /home/notes and syncs
   * those to the server: a .txt note was saved and then never seen again.
   * Notes titles a note by its first line, which here is what was said. */
  case ACT_NOTE: {
    SafeFile f;
    api->now(&t);
    if (t.synced)
      api->fmt(path, sizeof path, "%s/%02u%02u-%02u%02u%02u.md", NOTES_DIR,
               t.month, t.day, t.hour, t.min, t.sec);
    else {
      cnt = api->list_ex(NOTES_DIR, ent, 40);
      api->fmt(path, sizeof path, "%s/note%03d.md", NOTES_DIR, cnt > 0 ? cnt + 1 : 1);
    }
    if (safe_begin(&f, api, path) != 0) {
      api->fmt(out, n, "cannot write %s", path);
      return -1;
    }
    safe_line(&f, argv[0]);
    safe_write(&f, "\n", 1);
    if (safe_commit(&f) != 0) { api->fmt(out, n, "cannot write %s", path); return -1; }
    api->fmt(out, n, "saved %s", path);
    return 0;
  }

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
  {
    int top0 = E.top, left0 = E.leftcol, cy0 = E.cy, n0 = E.nlines;
    r = key_edit(k);
    if (r) damage_after_key(top0, left0, cy0, n0, VIEW_EDIT);
    return r;
  }
}

static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)button;

  {
    int a = toolbar_click(x, y);
    if (a == TB_CONSUMED) return 1;            /* opened or closed a menu */
    if (a != TB_NONE) return do_action(a);
  }

  /* The preview has no caret to put anywhere. */
  if (E.view != VIEW_EDIT) return 0;

  {
    int r, i, from = E.leftcol, to;
    y = (short)(y - toolbar_h());
    r = y / ROWH;
    if (y < 0) return 0;
    if (E.wrap) {
      if (!wrap_row(r, shown_cols, &i, &from, &to)) return 0;
    } else {
      i = E.top + r;
      if (i < 0 || i >= E.nlines) return 0;
      to = E.len[i];
    }
    E.cy = i;
    E.cx = from + (x - GUTTER) / CHARW;
    if (E.cx < from) E.cx = from;
    if (E.cx > to) E.cx = to;
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
  "arrows\tmove\nenter\tsplit the line\nbksp\tdelete\nctrl-a\tstart of line\nctrl-e\tend of line\nctrl-w\twrap long lines, or not\nctrl-p\tpreview, and back\nctrl-s\tsave\nctrl-r\tsave as\nctrl-o\topen a file\nctrl-n\tnew file\nfn-p\tprint\nIn the preview\narrows\tscroll\nspace\ta screen down\ng\tthe top\np\tprint\nesc\tback to editing\nenter\tback to editing\n",
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
