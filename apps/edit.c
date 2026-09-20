/* A small code editor, as a loadable CardOS app.
 *
 * Dark, because that is what an editor looks like and because a 240x135 panel
 * held close is easier on the eyes dark than a sheet of white. Line numbers in
 * a gutter, the current line marked, a status bar that says what file and
 * whether it is saved.
 *
 * Two views: a file browser and the editor. It opens in the browser, because an
 * editor with no file is an editor with nothing to do -- and because the
 * previous version was handed a path by the launcher and opened its own
 * binary, which is how you learn that "open a file" and "here is your icon's
 * path" are different questions.
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

#define DIRMAX    28
#define NAMELEN   32

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

typedef enum { VIEW_BROWSE = 0, VIEW_EDIT, VIEW_NAME, VIEW_PREVIEW } View;

/* What the filename prompt is for. One view serves both, because "what shall
 * it be called" is the same question either way -- only what happens after the
 * answer differs. */
typedef enum { NAME_NEW = 0, NAME_SAVE_AS } NameFor;

static const CardApi *api;

static struct {
  View view;

  /* browser */
  char dir[64];
  char names[DIRMAX][NAMELEN];
  int  ndir;
  int  bsel;

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

  /* the filename prompt */
  NameFor name_for;
  char    name[40];
  int     name_len;

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

static void begin_name(NameFor why, const char *initial) {
  E.name_for = why;
  api->fmt(E.name, sizeof E.name, "%s", initial ? initial : "");
  E.name_len = (int)api->str_len(E.name);
  E.view = VIEW_NAME;
}

static void save(void) {
  int fd, i;
  char nl = 10;

  /* Empty rather than pre-filled: a buffer with no name came from a pipe or a
   * new file, and the first thing typed should be the name rather than the
   * end of a name you have to delete first. */
  if (!E.path[0]) { begin_name(NAME_SAVE_AS, ""); return; }
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

/* ------------------------------------------------------------ browser ---- */

static void rescan(void) {
  static char raw[DIRMAX][NAMELEN];
  int n, i;

  E.ndir = 0;
  E.bsel = 0;
  n = api->list(E.dir, &raw[0][0], DIRMAX, NAMELEN);
  if (n < 0) { say("cannot read folder"); return; }
  for (i = 0; i < n && E.ndir < DIRMAX; i++) {
    api->fmt(E.names[E.ndir], NAMELEN, "%s", raw[i]);
    E.ndir++;
  }
  api->fmt(E.status, sizeof E.status, "%s  %d items", E.dir, E.ndir);
}

static void go_up(void) {
  int i, cut = 0;
  for (i = 0; E.dir[i]; i++) if (E.dir[i] == '/') cut = i;
  E.dir[cut ? cut : 1] = 0;
  rescan();
}

static void open_selected(void) {
  char path[96];
  int fd;

  if (E.bsel < 0 || E.bsel >= E.ndir) return;

  api->fmt(path, sizeof path, "%s%s%s", E.dir,
           E.dir[1] == 0 ? "" : "/", E.names[E.bsel]);

  /* A folder cannot be opened for reading, which is how this tells the two
   * apart -- there is no is_dir in the API an app is given. */
  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) {
    api->fmt(E.dir, sizeof E.dir, "%s", path);
    rescan();
    return;
  }
  api->close(fd);

  load(path);
  E.view = VIEW_EDIT;
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

/* The name is joined to the folder being browsed, so "notes.txt" lands where
 * you were looking rather than at the root. */
static void finish_name(void) {
  char path[96];

  if (E.name_len == 0) { E.view = VIEW_EDIT; return; }
  api->fmt(path, sizeof path, "%s%s%s", E.dir, E.dir[1] == 0 ? "" : "/", E.name);

  if (E.name_for == NAME_NEW) {
    blank();
    api->fmt(E.path, sizeof E.path, "%s", path);
    say("new file, ctrl-s saves");
  } else {
    api->fmt(E.path, sizeof E.path, "%s", path);
    E.view = VIEW_EDIT;
    save();
    return;
  }
  E.view = VIEW_EDIT;
}

/* ------------------------------------------------------------ painting --- */

static void paint_browse(CRect c) {
  int rows = (c.h - ROWH) / ROWH;
  int top = 0, i;

  api->fill(c, CLR_BG);
  if (E.bsel >= rows) top = E.bsel - rows + 1;

  for (i = 0; i < rows && top + i < E.ndir; i++) {
    int idx = top + i;
    short y = (short)(c.y + i * ROWH);
    int sel = (idx == E.bsel);
    api->fill(rect(c.x, y, c.w, ROWH), sel ? CLR_SEL : CLR_BG);
    api->text((short)(c.x + 3), y, E.names[idx],
              sel ? CLR_BAR_FG : CLR_TEXT, sel ? CLR_SEL : CLR_BG);
  }
  if (E.ndir == 0)
    api->text((short)(c.x + 3), c.y, "empty", CLR_DIM, CLR_BG);

  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  api->text((short)(c.x + 2), (short)(c.y + c.h - ROWH + 1), E.status,
            CLR_BAR_FG, CLR_BAR);
}

/* ------------------------------------------------------- markdown ---- */

/* A preview, not a parser.
 *
 * Markdown is a large specification and almost none of it earns its place on
 * a 240-pixel screen. What is here is what people actually write in notes:
 * headings, lists, quotes, code, rules, and bold or `code` inside a line.
 * Anything unrecognised is drawn as the text it is, which is markdown's whole
 * premise and a good fallback.
 *
 * It renders from the edit buffer rather than the file, so a preview shows
 * what you have typed and not what you last saved.
 *
 * There is no font but the 6x8 one and no way to scale it -- the API draws
 * text one way. Weight is therefore faked by drawing a glyph twice, one pixel
 * apart, which at this size reads convincingly as bold; headings get that
 * plus colour plus a rule under the big ones. Italics have no honest
 * equivalent, so emphasis is shown in colour instead of being invented.
 */

#define PG_LEFT   4          /* the page margin */
#define PG_ROW    9

/* One rendered row: the shape a line takes on the page. Built as the document
 * is walked so scrolling does not re-parse, and so the scrollbar knows how
 * long the document is before it has drawn it. */
typedef enum {
  MD_TEXT = 0, MD_H1, MD_H2, MD_H3, MD_BULLET, MD_NUMBER,
  MD_QUOTE, MD_CODE, MD_RULE, MD_BLANK
} MdKind;

static void md_text(int x, int y, const char *s, uint16_t fg, uint16_t bg,
                    int bold) {
  api->text((short)x, (short)y, s, fg, bg);
  /* The second pass is the whole of "bold" at six pixels: one to the right,
   * transparent background so it thickens rather than smears. */
  if (bold) api->text((short)(x + 1), (short)y, s, fg, bg);
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

/* Draw one run of text with the inline markers taken out: **bold**, *emph*,
 * `code`, and [label](url) reduced to its label.
 *
 * One pass, no nesting. Nested emphasis inside a note on a pocket computer is
 * a problem nobody has. */
static void md_inline(int x, int y, int maxw, const char *s, uint16_t fg,
                      uint16_t bg) {
  char word[64];
  int n = 0, bold = 0, code = 0;
  int cx = x;

  while (*s) {
    /* Flush what we have when a marker turns up, because the run either side
     * is drawn differently. */
    int flush = 0, next_bold = bold, next_code = code, skip = 0;

    if (s[0] == '*' && s[1] == '*') { flush = 1; next_bold = !bold; skip = 2; }
    else if (s[0] == '*' || s[0] == '_') { flush = 1; skip = 1; }
    else if (s[0] == '`') { flush = 1; next_code = !code; skip = 1; }
    else if (s[0] == '[') {
      /* [label](url): the label is what a reader wants; the URL will not fit
       * and could not be followed from here anyway. */
      const char *close = s + 1;
      while (*close && *close != ']') close++;
      if (*close == ']' && close[1] == '(') {
        const char *end = close + 2;
        while (*end && *end != ')') end++;
        if (*end == ')') {
          char label[48];
          int li = 0;
          const char *q = s + 1;
          while (q < close && li < (int)sizeof label - 1) label[li++] = *q++;
          label[li] = 0;
          if (n) { word[n] = 0; md_text(cx, y, word, fg, bg, bold);
                   cx += n * CHARW; n = 0; }
          md_text(cx, y, label, CLR_PG_LINK, bg, 0);
          cx += li * CHARW;
          s = end + 1;
          continue;
        }
      }
    }

    if (flush) {
      if (n) {
        word[n] = 0;
        md_text(cx, y, word, code ? CLR_PG_H : fg, code ? CLR_PG_CODE : bg, bold);
        cx += n * CHARW;
        n = 0;
      }
      bold = next_bold;
      code = next_code;
      s += skip;
      continue;
    }

    if (cx + (n + 1) * CHARW > x + maxw) break;      /* the wrap did its job */
    if (n < (int)sizeof word - 1) word[n++] = *s;
    s++;
  }
  if (n) {
    word[n] = 0;
    md_text(cx, y, word, code ? CLR_PG_H : fg, code ? CLR_PG_CODE : bg, bold);
  }
}

/* Walk the buffer, drawing rows from `from` for `rows` of them, and return
 * how many rows the whole document takes. Called twice: once to count (with
 * rows == 0), once to draw. Counting by walking is cheap here -- ninety-six
 * lines -- and it keeps one description of the layout rather than two. */
static int md_render(CRect c, int from, int rows) {
  int i, row = 0;
  int wrapcols = (c.w - PG_LEFT * 2) / CHARW;
  int fenced = 0;

  for (i = 0; i < E.nlines; i++) {
    const char *body;
    int indent = 0;
    MdKind k;
    int x, avail, y;
    uint16_t fg = CLR_PG_TX, bg = CLR_PG;
    int bold = 0;

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

    /* Where this row lands, and whether it is on screen at all. */
    y = c.y + (row - from) * PG_ROW;
    row++;
    if (rows == 0 || row - 1 < from || row - 1 >= from + rows) {
      /* Not drawn, but long lines still take more than one row. */
      if (k != MD_RULE && k != MD_BLANK) {
        int len = (int)api->str_len(body);
        int per = wrapcols - indent * 2 - (k == MD_BULLET || k == MD_NUMBER ? 2 : 0);
        while (per > 0 && len > per) { len -= per; row++; }
      }
      continue;
    }

    x = c.x + PG_LEFT + indent * 2 * CHARW;
    avail = c.w - (x - c.x) - PG_LEFT;

    switch (k) {
    case MD_BLANK:
      break;

    case MD_RULE:
      api->fill(rect(c.x + PG_LEFT, y + 4, c.w - PG_LEFT * 2, 1), CLR_PG_RULE);
      break;

    case MD_H1:
    case MD_H2:
      fg = CLR_PG_H;
      bold = 1;
      break;
    case MD_H3:
      fg = CLR_PG_H;
      break;

    case MD_QUOTE:
      /* The bar down the left is the whole visual idea of a quotation. */
      api->fill(rect(x, y, 2, PG_ROW), CLR_PG_QUOT);
      x += 6;
      avail -= 6;
      fg = CLR_PG_DIM;
      break;

    case MD_CODE:
      api->fill(rect(c.x + PG_LEFT, y, c.w - PG_LEFT * 2, PG_ROW), CLR_PG_CODE);
      bg = CLR_PG_CODE;
      break;

    case MD_BULLET:
      /* A square, because the font has no bullet and a hyphen reads as a
       * hyphen. */
      api->fill(rect(x + 1, y + 3, 3, 3), CLR_PG_TX);
      x += 8;
      avail -= 8;
      break;

    case MD_NUMBER:
    default:
      break;
    }

    if (k == MD_BLANK || k == MD_RULE) continue;

    if (k == MD_CODE) {
      /* Verbatim: markers are content inside a fence. */
      md_text(x, y, body, CLR_PG_TX, CLR_PG_CODE, 0);
    } else if (bold || k == MD_H1 || k == MD_H2 || k == MD_H3) {
      md_text(x, y, body, fg, bg, bold);
      if (k == MD_H1)
        api->fill(rect(c.x + PG_LEFT, y + PG_ROW - 1, c.w - PG_LEFT * 2, 1),
                  CLR_PG_RULE);
    } else {
      md_inline(x, y, avail, body, fg, bg);
    }
  }
  return row;
}

static void paint_preview(CRect c) {
  int rows = (c.h - ROWH) / PG_ROW;
  char bar[48];

  api->fill(rect(c.x, c.y, c.w, c.h - ROWH), CLR_PG);
  E.prows = md_render(rect(c.x, c.y + 2, c.w, c.h - ROWH - 2), E.ptop, rows);

  /* Clamp here rather than in the key handler: the length is only known once
   * it has been laid out, and laying it out is what this just did. */
  if (E.ptop > E.prows - rows) E.ptop = E.prows - rows;
  if (E.ptop < 0) E.ptop = 0;

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

static void paint_name(CRect c) {
  char shown[sizeof E.name + 2];
  short y = (short)(c.y + c.h / 2 - 18);

  api->fill(c, CLR_BG);
  api->text((short)(c.x + 8), y,
            E.name_for == NAME_NEW ? "New file" : "Save as", CLR_BAR_FG, CLR_BG);
  api->text((short)(c.x + 8), (short)(y + 11), E.dir, CLR_DIM, CLR_BG);

  api->fill(rect(c.x + 6, y + 24, c.w - 12, 13), CLR_CUR_BG);
  api->fill(rect(c.x + 6, y + 24, c.w - 12, 1), CLR_SEL);
  api->mem_cpy(shown, E.name, (size_t)E.name_len);
  shown[E.name_len] = '_';
  shown[E.name_len + 1] = 0;
  api->text((short)(c.x + 9), (short)(y + 27), shown, CLR_TEXT, CLR_CUR_BG);

  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  api->text((short)(c.x + 3), (short)(c.y + c.h - ROWH + 1),
            "enter confirms   backspace cancels when empty", CLR_BAR_FG, CLR_BAR);
}

/* What this editor can be asked to do. Keys map onto these and so do menu
 * items; see apps/toolbar.h for why a menu item is never a keystroke. */
enum { ACT_NEW = 1, ACT_SAVE, ACT_SAVEAS, ACT_OPEN, ACT_PREVIEW, ACT_PRINT };

static const CappAction EDIT_ACTIONS[] = {
  { "new",     "New",     "File", 0x0E, ACT_NEW },      /* ctrl-n */
  { "save",    "Save",    "File", 0x13, ACT_SAVE },     /* ctrl-s */
  { "saveas",  "Save as", "File", 0x12, ACT_SAVEAS },   /* ctrl-r */
  { "close",   "Close",   "File", 0x0F, ACT_OPEN },     /* ctrl-o */
  { "preview", "Preview", "View", 0x10, ACT_PREVIEW },  /* ctrl-p */
  { "print",   "Print",   "File", CAPP_KEY_PRINT, ACT_PRINT },  /* fn-p */
};
static const TbIcon EDIT_ICONS[] = { { "S", ACT_SAVE } };

#define NEDIT ((int)(sizeof EDIT_ACTIONS / sizeof EDIT_ACTIONS[0]))

/* The buffer, as the printer's markup: it is markdown-shaped already, so a
 * note with `# headings` and `[ ] tasks` prints as the preview shows it.
 * Joined here rather than sent line by line because print() copies once. */
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
  rc = api->print(E.page);
  if (rc == 0)       { E.printing = 1; say("printing..."); }
  else if (rc == -1) say("still printing the last one");
  else if (rc == -2) say("no printer: print scan in the console");
  else               say("could not print: no memory");
}

static int do_action(int a) {
  switch (a) {
  case ACT_NEW:     begin_name(NAME_NEW, ""); return 1;
  case ACT_SAVE:    save(); return 1;
  case ACT_SAVEAS:  begin_name(NAME_SAVE_AS, E.path[0] ? E.path : "untitled.txt"); return 1;
  case ACT_OPEN:    E.view = VIEW_BROWSE; rescan(); return 1;
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
  if (E.view == VIEW_BROWSE) { paint_browse(c); return; }
  if (E.view == VIEW_NAME) { paint_name(c); return; }
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

static int key_browse(unsigned char k) {
  switch (k) {
  case CAPP_KEY_UP:   if (E.bsel > 0) E.bsel--; return 1;
  case CAPP_KEY_DOWN: if (E.bsel + 1 < E.ndir) E.bsel++; return 1;
  case CAPP_KEY_LEFT:
  case CAPP_KEY_BACK: go_up(); return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_ENTER: open_selected(); return 1;
  case 'n': case 'N':
    begin_name(NAME_NEW, "");
    return 1;
  case 'r': case 'R': rescan(); return 1;
  default: return 0;
  }
}

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
  case CAPP_KEY_UP:    E.ptop -= 1; return 1;
  case CAPP_KEY_DOWN:  E.ptop += 1; return 1;
  case CAPP_KEY_LEFT:  E.ptop -= 12; return 1;
  case CAPP_KEY_RIGHT:
  case ' ':            E.ptop += 12; return 1;
  case 'g':            E.ptop = 0; return 1;
  case 0x0F:                                      /* ctrl-o, the file list */
    E.view = VIEW_BROWSE;
    rescan();
    return 1;
  case 0x13: save(); return 1;                    /* ctrl-s still saves */
  default: return 0;
  }
}

static int key_name(unsigned char k) {
  if (k == CAPP_KEY_ENTER) { finish_name(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (E.name_len > 0) E.name[--E.name_len] = 0;
    else E.view = (E.name_for == NAME_NEW) ? VIEW_BROWSE : VIEW_EDIT;
    return 1;
  }
  /* No slashes: this names a file in the folder being browsed, and a path
   * typed here would silently land somewhere else. */
  if (k >= 32 && k < 127 && k != '/' && E.name_len < (int)sizeof E.name - 1) {
    E.name[E.name_len++] = (char)k;
    E.name[E.name_len] = 0;
    return 1;
  }
  return 0;
}

/* What the shell calls for a chord out of the table, and what the menu bar
 * and any script reach through. */
static int app_action(void *st, int a) {
  (void)st;
  return do_action(a);
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
  if (E.view == VIEW_BROWSE) return key_browse(k);
  if (E.view == VIEW_NAME) return key_name(k);
  if (E.view == VIEW_PREVIEW) return key_preview(k);
  return key_edit(k);
}

static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)button;

  if (E.view == VIEW_BROWSE) {
    int i = y / ROWH;
    if (i < 0 || i >= E.ndir) return 0;
    if (i == E.bsel) open_selected();
    else E.bsel = i;
    return 1;
  }

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

  if (E.view == VIEW_BROWSE) {
    E.bsel -= wheel * 2;
    if (E.bsel < 0) E.bsel = 0;
    if (E.bsel >= E.ndir) E.bsel = E.ndir - 1;
    return 1;
  }
  if (E.view == VIEW_PREVIEW) { E.ptop -= wheel * 3; return 1; }
  if (E.view != VIEW_EDIT) return 0;

  E.top -= wheel * 3;
  if (E.top > E.nlines - 1) E.top = E.nlines - 1;
  if (E.top < 0) E.top = 0;
  return 1;
}

/* Only while editing. In the browser the arrows move a selection, and a
 * machine whose arrow keys are ; . , / needs those back. */
static int app_wants_text(void *st) {
  (void)st;
  /* Not while previewing: nothing there takes typing, and saying otherwise
   * would cost the arrow keys, which are how you scroll it. Nor while the
   * menu has the keyboard. */
  if (toolbar_has_keys()) return 0;
  return E.view == VIEW_EDIT || E.view == VIEW_NAME;
}

static void app_open(void *st) {
  (void)st;
  if (!E.dir[0]) api->fmt(E.dir, sizeof E.dir, "%s", CAPP_HOME);
  E.view = VIEW_BROWSE;
  rescan();
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
  "arrows\tmove\nenter\topen, or split the line\nbackspace\tup a folder, or delete\nn\tnew file\nctrl-n\tnew file\nctrl-s\tsave\nctrl-r\tsave as\nctrl-p\tmarkdown preview\nfn-p\tprint\nctrl-o\tback to the file list\nctrl-a\tstart of line\nctrl-e\tend of line\n",
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
  if (!E.dir[0]) api->fmt(E.dir, sizeof E.dir, "%s", CAPP_HOME);
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
     named file, or nothing -- and nothing means the browser, because an editor
     with no file has nothing to do. */
  if (api->has_input()) load_stdin();
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
  api->ui(&UI);
  return 0;
}
