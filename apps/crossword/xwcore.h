/* Crossword's puzzle: the grid, its numbering, the clues, the cursor, what
 * the solver has typed, and the file that remembers it -- the parts that can
 * be wrong in a way a person would argue about, kept apart from the screen so
 * the host suite can pin them (apps/crossword/test/test_crossword.c).
 *
 * Portable C with no libc: the app links none, and this compiles on the PC
 * for the tests as it is. One `Xw` is the whole state of one puzzle in play,
 * roughly 13 KB, so the app keeps exactly one, in its arena.
 *
 * ---- the puzzle text ------------------------------------------------------
 *
 * What the server answers to /crossword/puzzle?id= and what the card keeps in
 * /crossword/ID.xw. Lines, in this order, each ended by a newline (a CR before
 * it is ignored):
 *
 *   xw 1                  the format and its version; first
 *   title TEXT            optional: this and by, date, note run to the end
 *   by TEXT                 of the line, are cut to fit and may come in any
 *   date 1993-11-21         order after xw; a bad date is ignored
 *   note TEXT
 *   size W H              at most 21 by 21; once, before the rows
 *   row CELLS             H of them, each W characters:
 *                           .      a black square
 *                           A-Z    a white square, and the letter that is its answer
 *                           a-z    the same, in a circled square
 *   A N TEXT              the across clues, in number order; N is the clue's
 *   D N TEXT                number (see below) and the loader checks it, so a
 *                           converter that numbers differently is caught
 *   end                   last. A text without it was cut off -- the HTTP
 *                           layer cannot tell a body that ended from one
 *                           that was cut -- and is refused.
 *
 * Unknown lines are skipped, so a later version can add some. Every square is
 * one letter: a converter keeps a rebus square's first letter, and writes
 * clue text as ASCII (anything else is shown as '?' here, a tab as a space).
 *
 * ---- numbering ------------------------------------------------------------
 *
 * The usual rule. A white square starts an across entry if the square to its
 * left is black or the edge and the one to its right is white; a down entry
 * likewise. Entries are at least two squares: a lone white square between
 * blacks is not one. Squares that start an entry are numbered 1, 2, ... in
 * reading order. Clues are kept across first, then down, each in number
 * order -- the order Tab walks them.
 *
 * A square that is in only one entry (an "unchecked" one) is allowed. A white
 * square in none, or a grid with no white squares, is refused.
 *
 * ---- the cursor -----------------------------------------------------------
 *
 * Always on a white square and always in a word: `dir` is a direction that
 * square has an entry in, and anything that moves the cursor keeps it so.
 * Arrows are the NYT's: an arrow along the current direction moves, skipping
 * black squares (and stopping at the edge, no wrapping); an arrow across it
 * first turns to that direction if the square has an entry that way, and only
 * otherwise moves. Space turns it (`xw_toggle`), Tab goes to the next word
 * (`xw_next_word`), landing on its first empty square.
 *
 * The calls that move the cursor or edit a square answer a mask of XW_R_*
 * saying what changed, so the app can mark exactly that much damage;
 * `xw_check` answers how many wrong letters it marked instead.
 *
 * ---- progress -------------------------------------------------------------
 *
 * `xw_save` and `xw_restore` are a few lines of text (see xw_save). The app
 * keeps one file per puzzle through apps/safefile.h; `xw_peek` reads just
 * the top of one, for the list's solved and in-progress marks. Restoring
 * is all or nothing: a file that does not fit the puzzle (another size, other
 * black squares) changes nothing.
 */
#ifndef CARDOS_XWCORE_H
#define CARDOS_XWCORE_H

#include <stdint.h>

#if defined(__GNUC__)
#define XW_OPT __attribute__((unused))
#else
#define XW_OPT
#endif

#ifndef XW_MAX_W
#define XW_MAX_W     21        /* a Sunday puzzle is the biggest the NYT runs */
#endif
#ifndef XW_MAX_H
#define XW_MAX_H     21
#endif
#define XW_MAX_CELLS (XW_MAX_W * XW_MAX_H)
#define XW_MAX_CLUES 240       /* across and down together; indexes are bytes */
#define XW_POOL_MAX  8192      /* bytes of clue text, a NUL after each */
#define XW_CLUE_MAX  200       /* characters of one clue kept */
#define XW_TITLE_MAX 63
#define XW_BY_MAX    47
#define XW_NOTE_MAX  159
#define XW_SAVE_MAX  768       /* a buffer that holds any xw_save */
#define XW_NONE      0xFF      /* no clue, in a square's wa / wd */
#define XW_SECS_MAX  4000000   /* a clock stored beyond this is cut to it */

enum { XW_ACROSS = 0, XW_DOWN = 1 };

/* A square's flags. */
enum {
  XW_F_CIRCLE   = 1,           /* drawn in a circle */
  XW_F_WRONG    = 2,           /* marked by a check; any edit takes it off */
  XW_F_REVEALED = 4            /* filled in by reveal, so locked and not the solver's */
};

/* What a check or reveal covers. */
enum { XW_SCOPE_CELL = 0, XW_SCOPE_WORD = 1, XW_SCOPE_ALL = 2 };

/* What a call changed. */
enum {
  XW_R_CELL = 1,               /* a square's letter or flags */
  XW_R_MOVE = 2,               /* the cursor moved */
  XW_R_DIR  = 4,               /* the direction turned */
  XW_R_WON  = 8                /* the grid is now complete and right */
};

/* Results. */
enum {
  XW_OK          = 0,
  XW_DONE        = 1,          /* xw_load_line: the `end` line, nothing more is wanted */
  XW_ERR_HEADER  = -1,         /* not a puzzle (or progress) file, or another version */
  XW_ERR_SIZE    = -2,         /* a bad or missing size */
  XW_ERR_GRID    = -3,         /* a row that is the wrong length or has a bad character, rows missing or extra */
  XW_ERR_CLUE    = -4,         /* a clue line that is not the next one the grid needs */
  XW_ERR_MISSING = -5,         /* an entry with no clue */
  XW_ERR_TRUNC   = -6,         /* the text ended before `end` */
  XW_ERR_BIG     = -7,         /* more entries or clue text than there is room for */
  XW_ERR_ORPHAN  = -8,         /* a white square in no entry */
  XW_ERR_EMPTY   = -9,         /* no white squares at all */
  XW_ERR_STATE   = -10         /* no puzzle is loaded */
};

/* The loader's stages. */
enum { XW_S_NEW = 0, XW_S_HEAD, XW_S_ROWS, XW_S_CLUES, XW_S_END };

typedef struct {
  uint8_t sol;                 /* the answer's letter, 'A'..'Z'; 0 for a black square */
  uint8_t ent;                 /* what is in it, 'A'..'Z'; 0 for empty */
  uint8_t fl;                  /* XW_F_* */
  uint8_t wa, wd;              /* the across and down clue through it, or XW_NONE */
} XwCell;

typedef struct {
  uint16_t num;                /* the clue's number */
  uint16_t cell;               /* its first square, row * w + col */
  uint16_t text;               /* offset of its text in the pool */
  uint8_t  len;                /* squares in the entry */
  uint8_t  dir;                /* XW_ACROSS or XW_DOWN */
} XwClue;

typedef struct {
  /* the puzzle */
  int      w, h;
  char     title[XW_TITLE_MAX + 1];
  char     by[XW_BY_MAX + 1];
  char     note[XW_NOTE_MAX + 1];
  int      year, month, day;   /* the print date; 0 when there is none */
  XwCell   cell[XW_MAX_CELLS];
  XwClue   clue[XW_MAX_CLUES]; /* across first, then down */
  int      nclues, nacross;
  char     pool[XW_POOL_MAX];
  int      pool_n;
  /* the loader */
  int      stage, rows_in, got_a, got_d;
  int      err;                /* the first error, which sticks */
  int      ready;              /* loaded whole and checked: everything below is meaningful */
  /* play */
  int      cur;                /* the square the cursor is on */
  int      dir;                /* XW_ACROSS or XW_DOWN */
  int      solved;
  uint32_t ms, t0;             /* clock: ms gone before the run that began at t0 */
  int      running;
} Xw;

/* ---- small helpers --------------------------------------------------------- */

static XW_OPT int xw_len(const char *s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

/* Bytes by hand, through a volatile pointer: an app links no memset, and a
 * plain loop like this one is what the compiler likes to turn into a call
 * to it (apps/jar/jaritem.h learned that). */
static XW_OPT void xw_zero(void *v, int n) {
  volatile uint8_t *b = (volatile uint8_t *)v;
  while (n-- > 0) *b++ = 0;
}

/* s[0..len) into out (cap bytes, the NUL included), made safe to draw:
 * control characters become spaces, anything past ASCII a '?'. Its length. */
static XW_OPT int xw_copy_text(char *out, int cap, const char *s, int len) {
  int i, k = 0;
  for (i = 0; i < len && k < cap - 1; i++) {
    unsigned char c = (unsigned char)s[i];
    out[k++] = (char)(c < 0x20 ? ' ' : c > 0x7E ? '?' : c);
  }
  out[k] = 0;
  return k;
}

/* If s[0..n) is `key` alone or `key` and a space, the text after that one
 * space (the end, for the first); else NULL. */
static XW_OPT const char *xw_key(const char *s, int n, const char *key) {
  int i = 0;
  while (key[i]) {
    if (i >= n || s[i] != key[i]) return 0;
    i++;
  }
  if (i == n) return s + n;
  if (s[i] != ' ') return 0;
  return s + i + 1;
}

/* A number (up to nine digits) at *pp, past any spaces; *pp moves past it. */
static XW_OPT int xw_int(const char **pp, const char *end, int *v) {
  const char *s = *pp;
  int n = 0, digits = 0;
  while (s < end && *s == ' ') s++;
  while (s < end && *s >= '0' && *s <= '9' && digits < 9) {
    n = n * 10 + (*s - '0');
    s++;
    digits++;
  }
  if (!digits) return 0;
  *pp = s;
  *v = n;
  return 1;
}

/* Append to buf (n bytes); the new length, or -1 once it does not fit. */
static XW_OPT int xw_cat(char *buf, int n, int at, const char *s) {
  if (at < 0) return -1;
  while (*s) {
    if (at >= n - 1) return -1;
    buf[at++] = *s++;
  }
  if (at < n) buf[at] = 0;
  return at;
}

static XW_OPT int xw_cat_ch(char *buf, int n, int at, char c) {
  if (at < 0 || at >= n - 1) return -1;
  buf[at++] = c;
  buf[at] = 0;
  return at;
}

static XW_OPT int xw_cat_num(char *buf, int n, int at, int32_t v) {
  char t[16];
  int i = 15;
  uint32_t u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
  t[i] = 0;
  do { t[--i] = (char)('0' + u % 10); u /= 10; } while (u);
  if (v < 0) t[--i] = '-';
  return xw_cat(buf, n, at, t + i);
}

/* ---- dates ----------------------------------------------------------------- */

static XW_OPT int xw_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static XW_OPT int xw_month_days(int y, int m) {
  switch (m) {
    case 2: return xw_leap(y) ? 29 : 28;
    case 4: case 6: case 9: case 11: return 30;
    default: return 31;
  }
}

static XW_OPT int xw_valid_date(int y, int m, int d) {
  return y >= 1 && y <= 9999 && m >= 1 && m <= 12 && d >= 1 && d <= xw_month_days(y, m);
}

/* Days since 1970-01-01, after Howard Hinnant's days_from_civil. */
static XW_OPT int32_t xw_days(int y, int m, int d) {
  int32_t era, yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/* 0 Monday .. 6 Sunday. */
static XW_OPT int xw_weekday(int y, int m, int d) {
  int32_t n = xw_days(y, m, d) + 3;      /* 1970-01-01 was a Thursday */
  return (int)(((n % 7) + 7) % 7);
}

static XW_OPT const char *xw_day_name(int wd) {
  switch (wd) {
    case 0: return "Mon";
    case 1: return "Tue";
    case 2: return "Wed";
    case 3: return "Thu";
    case 4: return "Fri";
    case 5: return "Sat";
    case 6: return "Sun";
  }
  return "???";
}

/* The puzzle's weekday, or -1 when it carries no date. */
static XW_OPT int xw_puzzle_weekday(const Xw *p) {
  return p->year ? xw_weekday(p->year, p->month, p->day) : -1;
}

/* ---- squares and words ----------------------------------------------------- */

static XW_OPT void xw_init(Xw *p) { xw_zero(p, (int)sizeof(Xw)); }

/* A black square -- or off the grid, which blocks a word just the same. */
static XW_OPT int xw_is_black(const Xw *p, int r, int c) {
  return r < 0 || c < 0 || r >= p->h || c >= p->w || p->cell[r * p->w + c].sol == 0;
}

/* The clue of the entry through `cell` going `dir`, or -1. */
static XW_OPT int xw_clue_at(const Xw *p, int cell, int dir) {
  int k = dir == XW_ACROSS ? p->cell[cell].wa : p->cell[cell].wd;
  return k == XW_NONE ? -1 : k;
}

/* The clue the cursor is in. */
static XW_OPT int xw_cur_clue(const Xw *p) { return xw_clue_at(p, p->cur, p->dir); }

/* The k-th square of clue `ci`, from 0. */
static XW_OPT int xw_clue_cell(const Xw *p, int ci, int k) {
  const XwClue *c = &p->clue[ci];
  return c->dir == XW_ACROSS ? c->cell + k : c->cell + k * p->w;
}

/* Where `cell` falls in clue `ci` (from 0), or -1 if it is not in it. */
static XW_OPT int xw_clue_pos(const Xw *p, int ci, int cell) {
  const XwClue *c = &p->clue[ci];
  int d = cell - c->cell;
  if (d < 0) return -1;
  if (c->dir == XW_ACROSS) return d < c->len ? d : -1;
  if (d % p->w) return -1;
  d /= p->w;
  return d < c->len ? d : -1;
}

/* Is `cell` in the word the cursor is in? */
static XW_OPT int xw_in_cur_word(const Xw *p, int cell) {
  int ci = xw_cur_clue(p);
  return ci >= 0 && xw_clue_pos(p, ci, cell) >= 0;
}

static XW_OPT const char *xw_clue_text(const Xw *p, int ci) { return p->pool + p->clue[ci].text; }

/* The number printed in `cell`, or 0 if no entry starts there. */
static XW_OPT int xw_number_at(const Xw *p, int cell) {
  const XwCell *q = &p->cell[cell];
  if (q->wa != XW_NONE && p->clue[q->wa].cell == cell) return p->clue[q->wa].num;
  if (q->wd != XW_NONE && p->clue[q->wd].cell == cell) return p->clue[q->wd].num;
  return 0;
}

/* ---- loading --------------------------------------------------------------- */

/* Find the entries from the black squares, number them, and say which clue
 * each square is in. XW_OK, or why the grid cannot be a crossword. */
static XW_OPT int xw_number(Xw *p) {
  int d, r, c, i, whites = 0;
  p->nclues = 0;
  p->nacross = 0;
  for (i = 0; i < p->w * p->h; i++) {
    p->cell[i].wa = XW_NONE;
    p->cell[i].wd = XW_NONE;
    whites += p->cell[i].sol != 0;
  }
  if (!whites) return XW_ERR_EMPTY;
  for (d = 0; d < 2; d++) {
    int num = 0;
    for (r = 0; r < p->h; r++) {
      for (c = 0; c < p->w; c++) {
        int sa, sd, k, len;
        XwClue *cl;
        if (xw_is_black(p, r, c)) continue;
        sa = xw_is_black(p, r, c - 1) && !xw_is_black(p, r, c + 1);
        sd = xw_is_black(p, r - 1, c) && !xw_is_black(p, r + 1, c);
        if (sa || sd) num++;          /* the same count in both passes */
        if (!(d == XW_ACROSS ? sa : sd)) continue;
        if (p->nclues >= XW_MAX_CLUES) return XW_ERR_BIG;
        len = 0;
        while (d == XW_ACROSS ? !xw_is_black(p, r, c + len) : !xw_is_black(p, r + len, c)) len++;
        cl = &p->clue[p->nclues];
        cl->num = (uint16_t)num;
        cl->cell = (uint16_t)(r * p->w + c);
        cl->text = 0;
        cl->len = (uint8_t)len;
        cl->dir = (uint8_t)d;
        for (k = 0; k < len; k++) {
          XwCell *q = &p->cell[d == XW_ACROSS ? r * p->w + c + k : (r + k) * p->w + c];
          if (d == XW_ACROSS) q->wa = (uint8_t)p->nclues;
          else q->wd = (uint8_t)p->nclues;
        }
        p->nclues++;
      }
    }
    if (d == XW_ACROSS) p->nacross = p->nclues;
  }
  for (i = 0; i < p->w * p->h; i++)
    if (p->cell[i].sol && p->cell[i].wa == XW_NONE && p->cell[i].wd == XW_NONE) return XW_ERR_ORPHAN;
  return XW_OK;
}

/* The cursor at the start: 1-Across, or 1-Down in a grid with no across. */
static XW_OPT void xw_start(Xw *p) {
  p->dir = p->nacross > 0 ? XW_ACROSS : XW_DOWN;
  p->cur = p->clue[0].cell;
}

static XW_OPT int xw_fail(Xw *p, int err) {
  p->err = err;
  p->ready = 0;
  return err;
}

static XW_OPT void xw_set_date(Xw *p, const char *s, const char *end) {
  int y = 0, m = 0, d = 0;
  p->year = p->month = p->day = 0;
  if (!xw_int(&s, end, &y) || s >= end || *s != '-') return;
  s++;
  if (!xw_int(&s, end, &m) || s >= end || *s != '-') return;
  s++;
  if (!xw_int(&s, end, &d)) return;
  if (!xw_valid_date(y, m, d)) return;
  p->year = y;
  p->month = m;
  p->day = d;
}

static XW_OPT int xw_row(Xw *p, const char *s, const char *end) {
  int c;
  if (p->stage == XW_S_HEAD) return xw_fail(p, XW_ERR_SIZE);
  if (p->stage != XW_S_ROWS || p->rows_in >= p->h || (int)(end - s) != p->w)
    return xw_fail(p, XW_ERR_GRID);
  for (c = 0; c < p->w; c++) {
    XwCell *q = &p->cell[p->rows_in * p->w + c];
    char ch = s[c];
    q->wa = XW_NONE;
    q->wd = XW_NONE;
    q->ent = 0;
    if (ch == '.') { q->sol = 0; q->fl = 0; }
    else if (ch >= 'A' && ch <= 'Z') { q->sol = (uint8_t)ch; q->fl = 0; }
    else if (ch >= 'a' && ch <= 'z') { q->sol = (uint8_t)(ch - 32); q->fl = XW_F_CIRCLE; }
    else return xw_fail(p, XW_ERR_GRID);
  }
  p->rows_in++;
  return XW_OK;
}

/* The grid is whole: number it, and from now on only clues are wanted. */
static XW_OPT int xw_enter_clues(Xw *p) {
  int r;
  if (p->stage == XW_S_CLUES) return XW_OK;
  if (p->stage < XW_S_ROWS) return XW_ERR_SIZE;
  if (p->stage != XW_S_ROWS || p->rows_in != p->h) return XW_ERR_GRID;
  r = xw_number(p);
  if (r != XW_OK) return r;
  p->stage = XW_S_CLUES;
  return XW_OK;
}

static XW_OPT int xw_clue_line(Xw *p, int dir, const char *s, const char *end) {
  int num = 0, r, count, *got;
  const char *text;
  XwClue *cl;
  int tl;
  r = xw_enter_clues(p);
  if (r != XW_OK) return xw_fail(p, r);
  if (!xw_int(&s, end, &num)) return xw_fail(p, XW_ERR_CLUE);
  if (s < end && *s != ' ') return xw_fail(p, XW_ERR_CLUE);
  text = s < end ? s + 1 : s;
  count = dir == XW_ACROSS ? p->nacross : p->nclues - p->nacross;
  got = dir == XW_ACROSS ? &p->got_a : &p->got_d;
  if (*got >= count) return xw_fail(p, XW_ERR_CLUE);
  cl = &p->clue[(dir == XW_ACROSS ? 0 : p->nacross) + *got];
  if (cl->num != num) return xw_fail(p, XW_ERR_CLUE);
  tl = (int)(end - text);
  if (tl > XW_CLUE_MAX) tl = XW_CLUE_MAX;
  if (p->pool_n + tl + 1 > XW_POOL_MAX) return xw_fail(p, XW_ERR_BIG);
  cl->text = (uint16_t)p->pool_n;
  p->pool_n += xw_copy_text(p->pool + p->pool_n, tl + 1, text, tl) + 1;
  (*got)++;
  return XW_OK;
}

/* The solver's work is over when every white square holds its answer. 1 the
 * moment it becomes so, 0 otherwise (including already). */
static XW_OPT int xw_update(Xw *p) {
  int i, ok = 1;
  for (i = 0; i < p->w * p->h; i++)
    if (p->cell[i].sol && p->cell[i].ent != p->cell[i].sol) { ok = 0; break; }
  if (ok && !p->solved) { p->solved = 1; return 1; }
  p->solved = ok;
  return 0;
}

/* One line of the puzzle text, without its newline, `n` bytes: XW_OK, XW_DONE
 * on `end` (the puzzle is ready; lines after it are ignored), or an error,
 * which every later call repeats. Start with xw_init. */
static XW_OPT int xw_feed(Xw *p, const char *line, int n) {
  const char *rest, *end;
  int v = 0;
  if (p->err) return p->err;
  if (p->stage == XW_S_END) return XW_DONE;
  while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) n--;
  if (n == 0) return XW_OK;
  end = line + n;
  if (p->stage == XW_S_NEW) {
    rest = xw_key(line, n, "xw");
    if (!rest || !xw_int(&rest, end, &v) || v != 1) return xw_fail(p, XW_ERR_HEADER);
    p->stage = XW_S_HEAD;
    return XW_OK;
  }
  if ((rest = xw_key(line, n, "title")) != 0) {
    xw_copy_text(p->title, (int)sizeof p->title, rest, (int)(end - rest));
    return XW_OK;
  }
  if ((rest = xw_key(line, n, "by")) != 0) {
    xw_copy_text(p->by, (int)sizeof p->by, rest, (int)(end - rest));
    return XW_OK;
  }
  if ((rest = xw_key(line, n, "note")) != 0) {
    xw_copy_text(p->note, (int)sizeof p->note, rest, (int)(end - rest));
    return XW_OK;
  }
  if ((rest = xw_key(line, n, "date")) != 0) {
    xw_set_date(p, rest, end);
    return XW_OK;
  }
  if ((rest = xw_key(line, n, "size")) != 0) {
    int w = 0, h = 0;
    if (p->stage != XW_S_HEAD || !xw_int(&rest, end, &w) || !xw_int(&rest, end, &h) ||
        w < 1 || h < 1 || w > XW_MAX_W || h > XW_MAX_H)
      return xw_fail(p, XW_ERR_SIZE);
    p->w = w;
    p->h = h;
    p->stage = XW_S_ROWS;
    p->rows_in = 0;
    return XW_OK;
  }
  if ((rest = xw_key(line, n, "row")) != 0) return xw_row(p, rest, end);
  if ((rest = xw_key(line, n, "A")) != 0) return xw_clue_line(p, XW_ACROSS, rest, end);
  if ((rest = xw_key(line, n, "D")) != 0) return xw_clue_line(p, XW_DOWN, rest, end);
  if (xw_key(line, n, "end") != 0) {
    int r = xw_enter_clues(p);
    if (r != XW_OK) return xw_fail(p, r);
    if (p->got_a != p->nacross || p->got_d != p->nclues - p->nacross)
      return xw_fail(p, XW_ERR_MISSING);
    p->stage = XW_S_END;
    p->ready = 1;
    xw_start(p);
    return XW_DONE;
  }
  return XW_OK;                     /* a line a later version added */
}

/* The same for a NUL-terminated line, for a loader reading the card a line
 * at a time. */
static XW_OPT int xw_load_line(Xw *p, const char *line) { return xw_feed(p, line, xw_len(line)); }

/* The text has run out: XW_OK if the puzzle came whole, else why not. */
static XW_OPT int xw_load_end(Xw *p) {
  if (p->err) return p->err;
  return p->stage == XW_S_END ? XW_OK : XW_ERR_TRUNC;
}

/* A whole NUL-terminated text, from a fresh start. XW_OK or an error; on an
 * error the Xw is not a puzzle (p->ready is 0). */
static XW_OPT int xw_parse(Xw *p, const char *text) {
  const char *s = text;
  xw_init(p);
  while (*s) {
    const char *e = s;
    int r;
    while (*e && *e != '\n') e++;
    r = xw_feed(p, s, (int)(e - s));
    if (r < 0) return r;
    if (r == XW_DONE) return XW_OK;
    s = *e ? e + 1 : e;
  }
  return xw_load_end(p);
}

static XW_OPT const char *xw_err_text(int err) {
  switch (err) {
    case XW_OK: case XW_DONE: return "ok";
    case XW_ERR_HEADER:  return "not a puzzle file";
    case XW_ERR_SIZE:    return "bad grid size";
    case XW_ERR_GRID:    return "bad grid";
    case XW_ERR_CLUE:    return "clues do not fit the grid";
    case XW_ERR_MISSING: return "clues missing";
    case XW_ERR_TRUNC:   return "puzzle cut short";
    case XW_ERR_BIG:     return "puzzle too big";
    case XW_ERR_ORPHAN:  return "square in no word";
    case XW_ERR_EMPTY:   return "grid has no squares";
    case XW_ERR_STATE:   return "no puzzle loaded";
  }
  return "error";
}

/* ---- the cursor ------------------------------------------------------------ */

/* After the cursor lands: turn to a direction the square has a word in. */
static XW_OPT int xw_fix_dir(Xw *p) {
  if (xw_clue_at(p, p->cur, p->dir) >= 0) return 0;
  if (xw_clue_at(p, p->cur, 1 - p->dir) >= 0) { p->dir = 1 - p->dir; return XW_R_DIR; }
  return 0;
}

/* One step of the arrow (dr, dc) -- (0,1) right, (1,0) down -- to the next
 * white square that way, over black ones; none at the edge. No turning. */
static XW_OPT int xw_step(Xw *p, int dr, int dc) {
  int r, c;
  if (!p->ready || (dr != 0) == (dc != 0)) return 0;
  r = p->cur / p->w;
  c = p->cur % p->w;
  for (;;) {
    r += dr;
    c += dc;
    if (r < 0 || c < 0 || r >= p->h || c >= p->w) return 0;
    if (!xw_is_black(p, r, c)) break;
  }
  p->cur = r * p->w + c;
  return XW_R_MOVE | xw_fix_dir(p);
}

/* An arrow key: it always moves, never turns first -- space turns (the
 * owner's choice, 2026-10-27). The direction changes only where the square
 * moved to has no word the old way. */
static XW_OPT int xw_arrow(Xw *p, int dr, int dc) {
  return xw_step(p, dr, dc);
}

/* Space: across becomes down and back, where the square has both. */
static XW_OPT int xw_toggle(Xw *p) {
  if (!p->ready || xw_clue_at(p, p->cur, 1 - p->dir) < 0) return 0;
  p->dir = 1 - p->dir;
  return XW_R_DIR;
}

/* The cursor onto a white square, keeping the direction if it has a word
 * that way. */
static XW_OPT int xw_goto(Xw *p, int cell) {
  if (!p->ready || cell < 0 || cell >= p->w * p->h || p->cell[cell].sol == 0) return 0;
  if (cell == p->cur) return 0;
  p->cur = cell;
  return XW_R_MOVE | xw_fix_dir(p);
}

/* A click on a square: the one the cursor is on turns the direction; another
 * moves to it. */
static XW_OPT int xw_click(Xw *p, int cell) {
  if (!p->ready || cell < 0 || cell >= p->w * p->h || p->cell[cell].sol == 0) return 0;
  return cell == p->cur ? xw_toggle(p) : xw_goto(p, cell);
}

/* Into clue `ci`: its first empty square, or its first if none is empty. */
static XW_OPT int xw_goto_clue(Xw *p, int ci) {
  int k, to, r = 0;
  if (!p->ready || ci < 0 || ci >= p->nclues) return 0;
  to = xw_clue_cell(p, ci, 0);
  for (k = 0; k < p->clue[ci].len; k++) {
    int cell = xw_clue_cell(p, ci, k);
    if (p->cell[cell].ent == 0) { to = cell; break; }
  }
  if (p->clue[ci].dir != p->dir) { p->dir = p->clue[ci].dir; r |= XW_R_DIR; }
  if (to != p->cur) { p->cur = to; r |= XW_R_MOVE; }
  return r;
}

/* Tab and shift-Tab: the next or the previous clue, across ones then down
 * ones, round again at the end. */
static XW_OPT int xw_next_word(Xw *p, int forward) {
  int ci;
  if (!p->ready) return 0;
  ci = xw_cur_clue(p);
  ci = (ci + (forward ? 1 : p->nclues - 1)) % p->nclues;
  return xw_goto_clue(p, ci);
}

/* ---- typing ---------------------------------------------------------------- */

/* A letter into the square, either case; then on to the next square of the
 * word, which stays put at the word's end. A revealed square keeps its
 * letter, and a finished puzzle takes no more. */
static XW_OPT int xw_type(Xw *p, int ch) {
  XwCell *q;
  int ci, k, r = 0;
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  if (!p->ready || p->solved || ch < 'A' || ch > 'Z') return 0;
  q = &p->cell[p->cur];
  if (!(q->fl & XW_F_REVEALED) && q->ent != ch) {
    q->ent = (uint8_t)ch;
    q->fl = (uint8_t)(q->fl & ~XW_F_WRONG);
    r |= XW_R_CELL;
  }
  ci = xw_cur_clue(p);
  k = xw_clue_pos(p, ci, p->cur);
  if (k + 1 < p->clue[ci].len) {
    p->cur = xw_clue_cell(p, ci, k + 1);
    r |= XW_R_MOVE;
  }
  if ((r & XW_R_CELL) && xw_update(p)) r |= XW_R_WON;
  return r;
}

/* Empty the square the cursor is on, if it holds the solver's letter. */
static XW_OPT int xw_clear(Xw *p) {
  XwCell *q;
  if (!p->ready || p->solved) return 0;
  q = &p->cell[p->cur];
  if (!q->ent || (q->fl & XW_F_REVEALED)) return 0;
  q->ent = 0;
  q->fl = (uint8_t)(q->fl & ~XW_F_WRONG);
  return XW_R_CELL;
}

/* Backspace: empty the square; if it was already empty, step back along the
 * word and empty that one. At the word's start, nothing. */
static XW_OPT int xw_backspace(Xw *p) {
  int r, ci, k;
  if (!p->ready || p->solved) return 0;
  r = xw_clear(p);
  if (r) return r;
  ci = xw_cur_clue(p);
  k = xw_clue_pos(p, ci, p->cur);
  if (k <= 0) return 0;
  p->cur = xw_clue_cell(p, ci, k - 1);
  return XW_R_MOVE | xw_clear(p);
}

/* ---- checking and revealing ------------------------------------------------ */

/* The k-th square of a scope, from 0; -1 past its end. XW_SCOPE_ALL gives
 * black squares too, which callers skip (they have no answer). */
static XW_OPT int xw_scope_cell(const Xw *p, int scope, int k) {
  if (scope == XW_SCOPE_CELL) return k == 0 ? p->cur : -1;
  if (scope == XW_SCOPE_WORD) {
    int ci = xw_cur_clue(p);
    return (ci >= 0 && k < p->clue[ci].len) ? xw_clue_cell(p, ci, k) : -1;
  }
  return k < p->w * p->h ? k : -1;
}

/* Mark the wrong letters in the scope (empty squares are not wrong yet). How
 * many it found. The marks stay until the square is edited. */
static XW_OPT int xw_check(Xw *p, int scope) {
  int k, i, n = 0;
  if (!p->ready) return 0;
  for (k = 0; (i = xw_scope_cell(p, scope, k)) >= 0; k++) {
    XwCell *q = &p->cell[i];
    if (q->sol && q->ent && q->ent != q->sol) {
      q->fl |= XW_F_WRONG;
      n++;
    }
  }
  return n;
}

/* Fill in the answers in the scope: XW_R_CELL if any square changed, with
 * XW_R_WON if that finished the grid. A square already right is left as the
 * solver's own. */
static XW_OPT int xw_reveal(Xw *p, int scope) {
  int k, i, r = 0;
  if (!p->ready || p->solved) return 0;
  for (k = 0; (i = xw_scope_cell(p, scope, k)) >= 0; k++) {
    XwCell *q = &p->cell[i];
    if (q->sol && q->ent != q->sol) {
      q->ent = q->sol;
      q->fl = (uint8_t)((q->fl | XW_F_REVEALED) & ~XW_F_WRONG);
      r |= XW_R_CELL;
    }
  }
  if (r && xw_update(p)) r |= XW_R_WON;
  return r;
}

/* Take every check mark off; how many there were. */
static XW_OPT int xw_unmark(Xw *p) {
  int i, n = 0;
  for (i = 0; i < p->w * p->h; i++)
    if (p->cell[i].fl & XW_F_WRONG) {
      p->cell[i].fl = (uint8_t)(p->cell[i].fl & ~XW_F_WRONG);
      n++;
    }
  return n;
}

static XW_OPT int xw_solved(const Xw *p) { return p->solved; }

/* White squares, and how many hold a letter. */
static XW_OPT void xw_count(const Xw *p, int *filled, int *total) {
  int i, f = 0, t = 0;
  for (i = 0; i < p->w * p->h; i++)
    if (p->cell[i].sol) { t++; f += p->cell[i].ent != 0; }
  if (filled) *filled = f;
  if (total) *total = t;
}

/* Squares filled in by reveal: a solve with none is the solver's own. */
static XW_OPT int xw_reveals(const Xw *p) {
  int i, n = 0;
  for (i = 0; i < p->w * p->h; i++) n += (p->cell[i].fl & XW_F_REVEALED) != 0;
  return n;
}

static XW_OPT int xw_wrongs(const Xw *p) {
  int i, n = 0;
  for (i = 0; i < p->w * p->h; i++) n += (p->cell[i].fl & XW_F_WRONG) != 0;
  return n;
}

/* Start over: empty grid, clock at nothing, cursor at the start. */
static XW_OPT void xw_reset(Xw *p) {
  int i;
  for (i = 0; i < p->w * p->h; i++) {
    p->cell[i].ent = 0;
    p->cell[i].fl = (uint8_t)(p->cell[i].fl & XW_F_CIRCLE);
  }
  p->ms = 0;
  p->t0 = 0;
  p->running = 0;
  p->solved = 0;
  if (p->ready) xw_start(p);
}

/* ---- the clock -------------------------------------------------------------- *
 *
 * The app passes its own millisecond counter (it may wrap; the sums are
 * unsigned). It starts the clock when the puzzle comes up, stops it when the
 * app is left or a call answers XW_R_WON, and stops it before xw_save, which
 * writes only the time gone.
 */
static XW_OPT void xw_clock_start(Xw *p, uint32_t now_ms) {
  if (p->running || p->solved) return;
  p->t0 = now_ms;
  p->running = 1;
}

static XW_OPT void xw_clock_stop(Xw *p, uint32_t now_ms) {
  if (!p->running) return;
  p->ms += now_ms - p->t0;
  p->running = 0;
}

static XW_OPT uint32_t xw_elapsed_ms(const Xw *p, uint32_t now_ms) {
  return p->ms + (p->running ? now_ms - p->t0 : 0);
}

static XW_OPT uint32_t xw_elapsed_s(const Xw *p, uint32_t now_ms) {
  return xw_elapsed_ms(p, now_ms) / 1000u;
}

/* ---- progress -------------------------------------------------------------- *
 *
 * A few lines of text per puzzle (/crossword/ID.sav), the top of which tells
 * a list what it needs without reading the grid:
 *
 *   xwp 1
 *   state P          P playing, S solved, H solved with reveals
 *   time 125         seconds on the clock
 *   fill 6 23        squares holding a letter, and white squares
 *   reveals 1        squares filled in by reveal
 *   size 5 5
 *   cur 6 D          the cursor's square, and A or D
 *   g HEAR.          a row a line: . black, - empty, A-Z the solver's letter,
 *   g X----            a-z a revealed one (a letter that is the answer)
 *   ...
 *   end
 *
 * state, fill and reveals are for xw_peek; restoring derives them from the
 * grid. Check marks are not kept.
 */

static XW_OPT int xw_save(const Xw *p, char *buf, int n) {
  int at = 0, r, c, filled, total, helped;
  if (!p->ready) return -1;
  if (n > 0) buf[0] = 0;
  xw_count(p, &filled, &total);
  helped = xw_reveals(p);
  at = xw_cat(buf, n, at, "xwp 1\nstate ");
  at = xw_cat(buf, n, at, p->solved ? (helped ? "H" : "S") : "P");
  at = xw_cat(buf, n, at, "\ntime ");
  at = xw_cat_num(buf, n, at, (int32_t)(p->ms / 1000u));
  at = xw_cat(buf, n, at, "\nfill ");
  at = xw_cat_num(buf, n, at, filled);
  at = xw_cat(buf, n, at, " ");
  at = xw_cat_num(buf, n, at, total);
  at = xw_cat(buf, n, at, "\nreveals ");
  at = xw_cat_num(buf, n, at, helped);
  at = xw_cat(buf, n, at, "\nsize ");
  at = xw_cat_num(buf, n, at, p->w);
  at = xw_cat(buf, n, at, " ");
  at = xw_cat_num(buf, n, at, p->h);
  at = xw_cat(buf, n, at, "\ncur ");
  at = xw_cat_num(buf, n, at, p->cur);
  at = xw_cat(buf, n, at, p->dir == XW_DOWN ? " D\n" : " A\n");
  for (r = 0; r < p->h; r++) {
    at = xw_cat(buf, n, at, "g ");
    for (c = 0; c < p->w; c++) {
      const XwCell *q = &p->cell[r * p->w + c];
      char ch = !q->sol ? '.' : !q->ent ? '-' : (char)((q->fl & XW_F_REVEALED) ? q->ent + 32 : q->ent);
      at = xw_cat_ch(buf, n, at, ch);
    }
    at = xw_cat(buf, n, at, "\n");
  }
  at = xw_cat(buf, n, at, "end\n");
  return at;
}

/* Walk a progress text against the loaded puzzle; with `apply`, write it in.
 * Run once without to know it fits, then once with: an error never leaves
 * the puzzle half changed. */
static XW_OPT int xw_prog_read(Xw *p, const char *text, int apply) {
  const char *s = text;
  int hdr = 0, size_seen = 0, rows = 0, ended = 0, cur = -1, dir = XW_ACROSS;
  int32_t secs = 0;
  while (*s && !ended) {
    const char *e = s, *rest, *lend;
    int n, v = 0, w = 0, h = 0;
    while (*e && *e != '\n') e++;
    n = (int)(e - s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
    lend = s + n;
    if (n > 0) {
      if (!hdr) {
        rest = xw_key(s, n, "xwp");
        if (!rest || !xw_int(&rest, lend, &v) || v != 1) return XW_ERR_HEADER;
        hdr = 1;
      } else if ((rest = xw_key(s, n, "size")) != 0) {
        if (!xw_int(&rest, lend, &w) || !xw_int(&rest, lend, &h) || w != p->w || h != p->h)
          return XW_ERR_SIZE;
        size_seen = 1;
      } else if ((rest = xw_key(s, n, "time")) != 0) {
        if (xw_int(&rest, lend, &v)) secs = v;
      } else if ((rest = xw_key(s, n, "cur")) != 0) {
        if (xw_int(&rest, lend, &v)) {
          cur = v;
          while (rest < lend && *rest == ' ') rest++;
          dir = (rest < lend && *rest == 'D') ? XW_DOWN : XW_ACROSS;
        }
      } else if ((rest = xw_key(s, n, "g")) != 0) {
        int c;
        if (!size_seen || rows >= p->h || (int)(lend - rest) != p->w) return XW_ERR_GRID;
        for (c = 0; c < p->w; c++) {
          XwCell *q = &p->cell[rows * p->w + c];
          char ch = rest[c];
          if (ch == '.') {
            if (q->sol) return XW_ERR_GRID;
          } else if (!q->sol) {
            return XW_ERR_GRID;
          } else if (ch == '-') {
            if (apply) { q->ent = 0; q->fl = (uint8_t)(q->fl & XW_F_CIRCLE); }
          } else if (ch >= 'A' && ch <= 'Z') {
            if (apply) { q->ent = (uint8_t)ch; q->fl = (uint8_t)(q->fl & XW_F_CIRCLE); }
          } else if (ch >= 'a' && ch <= 'z') {
            if (ch - 32 != q->sol) return XW_ERR_GRID;     /* revealed means it is the answer */
            if (apply) { q->ent = q->sol; q->fl = (uint8_t)((q->fl & XW_F_CIRCLE) | XW_F_REVEALED); }
          } else {
            return XW_ERR_GRID;
          }
        }
        rows++;
      } else if (xw_key(s, n, "end") != 0) {
        ended = 1;
      }
    }
    s = *e ? e + 1 : e;
  }
  if (!hdr) return XW_ERR_HEADER;
  if (!ended) return XW_ERR_TRUNC;
  if (!size_seen || rows != p->h) return XW_ERR_GRID;
  if (apply) {
    xw_start(p);
    if (cur >= 0 && cur < p->w * p->h && p->cell[cur].sol) {
      p->cur = cur;
      p->dir = dir;
      xw_fix_dir(p);
    }
    p->ms = (uint32_t)(secs > XW_SECS_MAX ? XW_SECS_MAX : secs) * 1000u;
    p->t0 = 0;
    p->running = 0;
    p->solved = 0;
    xw_update(p);
  }
  return XW_OK;
}

/* Put saved progress into the loaded puzzle. XW_OK, or why the text is not
 * progress for this puzzle -- and then nothing has changed. */
static XW_OPT int xw_restore(Xw *p, const char *text) {
  int r;
  if (!p->ready) return XW_ERR_STATE;
  r = xw_prog_read(p, text, 0);
  if (r != XW_OK) return r;
  return xw_prog_read(p, text, 1);
}

/* What the top of a progress file says, for a list. */
typedef struct {
  int      ok;                 /* a header and a state were read */
  int      state;              /* 'P', 'S' or 'H' */
  uint32_t secs;
  int      filled, total, reveals;
} XwPeek;

/* Read only the lines before `size`, and only whole ones, so the first
 * hundred bytes of a file will do. XW_OK, or XW_ERR_HEADER. */
static XW_OPT int xw_peek(const char *text, XwPeek *pk) {
  const char *s = text;
  int hdr = 0, done = 0;
  pk->ok = 0;
  pk->state = 0;
  pk->secs = 0;
  pk->filled = pk->total = pk->reveals = 0;
  while (!done) {
    const char *e = s, *rest, *lend;
    int n, v = 0, v2 = 0;
    while (*e && *e != '\n') e++;
    if (*e != '\n') break;                   /* an unfinished line is not read */
    n = (int)(e - s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == ' ')) n--;
    lend = s + n;
    if (n > 0) {
      if (!hdr) {
        rest = xw_key(s, n, "xwp");
        if (!rest || !xw_int(&rest, lend, &v) || v != 1) return XW_ERR_HEADER;
        hdr = 1;
      } else if ((rest = xw_key(s, n, "state")) != 0) {
        char c = rest < lend ? *rest : 0;
        if (c == 'P' || c == 'S' || c == 'H') pk->state = c;
      } else if ((rest = xw_key(s, n, "time")) != 0) {
        if (xw_int(&rest, lend, &v)) pk->secs = (uint32_t)v;
      } else if ((rest = xw_key(s, n, "fill")) != 0) {
        if (xw_int(&rest, lend, &v) && xw_int(&rest, lend, &v2)) { pk->filled = v; pk->total = v2; }
      } else if ((rest = xw_key(s, n, "reveals")) != 0) {
        if (xw_int(&rest, lend, &v)) pk->reveals = v;
      } else if (xw_key(s, n, "size") != 0 || xw_key(s, n, "cur") != 0 || xw_key(s, n, "g") != 0) {
        done = 1;
      }
    }
    s = e + 1;
  }
  pk->ok = hdr && pk->state != 0;
  return pk->ok ? XW_OK : XW_ERR_HEADER;
}

#endif /* CARDOS_XWCORE_H */
