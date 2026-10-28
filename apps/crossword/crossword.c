/* Crossword: the New York Times' old puzzles, on the device.
 *
 * The list is the library, newest first, a puzzle a row -- its weekday
 * (coloured from easy Monday to hard Saturday), its print date, its title if
 * it has one, and on the right what you have done with it: a check for a
 * puzzle solved (amber if it was helped), a half-filled circle for one begun,
 * with the time on the clock. Enter opens a puzzle. The grid is the puzzle:
 * numbered squares, the word the cursor is in tinted, the cursor square
 * brighter, and the clue for the word on a bar below it, wrapped to two lines
 * in ui13. Tab on the list is the statistics.
 *
 * Playing (the rules are apps/crossword/xwcore.h, host-tested in
 * test/test_crossword.c; the statistics are xwstats.h):
 *
 *   letters       go into the square, and the cursor moves on; backspace
 *                 clears a letter, or steps back and clears the one before
 *   arrows        move, and never turn (the owner's choice, 2026-10-27);
 *                 space or tab turns across or down; enter goes to the next
 *                 word, at its first empty square
 *   ctrl-l        the next view: side (the whole grid, the clue in a column
 *                 beside it -- the first), big (large squares, the clue under
 *                 them), zoom (edge to edge, scrolling); kept for next time
 *   ctrl-c        check -- letters are typed here, so c and h cannot be
 *   ctrl-r        reveal -- commands; each asks "letter, word or puzzle?" in
 *                 the footer, and revealing the puzzle asks y/n. The same
 *                 nine actions are in fn-space. A wrong letter turns red
 *                 until it is changed.
 *   ctrl-n        start over, after y/n
 *
 * A reveal means no best time (more than half revealed counts as giving up and
 * nothing at all). The clock runs while the grid is up and stops at the last
 * key after three idle minutes; leaving the grid stops it. Apps are not told
 * when they are closed, so progress is written every 15 s while it changes,
 * on leaving, and on a win -- what is lost to a switch-off is the last few
 * letters, never time that was not spent. A win stops the clock, counts the
 * solve in the statistics (per weekday: how many, the best time, the
 * average; helped ones apart; the streak of days) and shows the results.
 *
 * Where things are:
 *
 *   server        GET /crossword/list pages the library in 40s, newest first,
 *                 by weekday and date (server/crossword.py); the list asks for
 *                 the next page past the end of this one and the one before
 *                 past its start, so thousands of puzzles are a scroll away
 *   /crossword    the card: ID.xw is each puzzle opened once (the very text
 *                 the server sends, kept so it opens again with no network),
 *                 index.txt the list of them, in the server's own line format
 *                 and newest first, so the list works on the card alone
 *   /var/crossword  ID.sav, a puzzle's progress (xw_save: the list reads its
 *                 top for the marks), and stats.txt
 *
 * The list comes from the server when the OS found it at launch, and from the
 * card when it did not, or when a request fails, or when `o` says so. Either
 * way the same keys work: the card's index answers the same questions the
 * server does (weekday, date, which page).
 *
 * A puzzle that is not on the card is downloaded on the tick after Enter, so
 * the screen can say so first: a download blocks the machine for a moment.
 * It arrives as ID.xw.part, is parsed whole -- a cut-off file is refused, the
 * text ends in `end` for that -- and only then renamed into place.
 *
 * Painting: the OS composes a full repaint off the panel, so a cell is a
 * single blit built here, background, grid line, number, circle and letter in
 * one buffer, never a fill with something drawn over it. A cursor move marks
 * only the words it leaves and enters and the clue bar (api->damage); a
 * damaged repaint goes to the panel directly, which is why no region of
 * text is ever filled and then written over.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/footer.h"
#include "apps/safefile.h"
#include "apps/confirm.h"
#include "apps/crossword/xwcore.h"
#include "apps/crossword/xwstats.h"

static const CardApi *api;

#define PUZ_DIR     "/crossword"
#define INDEX_PATH  PUZ_DIR "/index.txt"
#define SAVE_DIR    CAPP_VAR "/crossword"
#define STATS_PATH  SAVE_DIR "/stats.txt"
#define VIEW_PATH   SAVE_DIR "/view.txt"     /* the view last chosen (side, big, zoom), then the tab */

enum { LAY_SIDE, LAY_BIG, LAY_ZOOM, LAY_N };

/* The list's tabs: which puzzles. The Midi is kept by the server as
 * DATE-midi; a puzzle whose id is not a date (traitors) is Themed, made for
 * this library and printed nowhere; everything else -- the dailies, old and
 * new -- is Full. server/crossword.py's kind_of says the same. */
enum { KIND_FULL, KIND_MIDI, KIND_THEMED, KIND_N };
static const char *const KIND_TAB[KIND_N] = { "Full", "Midi", "Themed" };
static const char *const KIND_ARG[KIND_N] = { "full", "midi", "themed" };
#define MIDI_SUFFIX "-midi"
static const char *const LAY_NAME[LAY_N] = { "side", "big", "zoom" };

#define SAVE_MS     15000                /* progress is written this often while it is changing */
#define IDLE_MS     180000               /* no key for this long: the clock stops, at the last key */
#define MSG_MS      4000                 /* a line in the footer after a check or a reveal */

#define PAGE_N      40                   /* puzzles in the window; asked of the server as limit= */
#define ID_MAX      16
#define TITLE_KEEP  28
#define REPLY_MAX   3400                 /* 40 lines of at most ~75 bytes */
#define NO_FILE     (-100)

#define TOP_H       16
#define ROW_H       15
#define MARGIN      6
#define CELL        14                   /* a square's pitch: 13 inside and the line */
#define CELL_MIN    8                    /* the side view's smallest square: a letter, no number */
#define CELL_NUM    13                   /* the smallest that has room for a number above the letter */
#define COL_MIN     100                  /* the side view's clue column is never narrower */
#define CELL_ZOOM   32                   /* the zoom view's biggest square */
#define CL_LINES    3                    /* the zoom view's clue bar grows to this many lines */
#define CELL_SIDE   24                   /* the side view's: a mini in big letters, the column still wide */
#define TIME_W      46

/* The list. */
#define L_BG        CAPP_RGB(250, 250, 246)
#define L_TEXT      CAPP_RGB(24, 26, 30)
#define L_DIM       CAPP_RGB(110, 116, 128)
#define L_BAR       CAPP_RGB(234, 236, 240)
#define L_SEL       CAPP_RGB(214, 226, 248)
#define L_RULE      CAPP_RGB(200, 204, 212)
#define L_BAD       CAPP_RGB(200, 50, 40)
#define K_PLAY      CAPP_RGB(36, 90, 180)
#define K_DONE      CAPP_RGB(30, 150, 70)
#define K_HELP      CAPP_RGB(214, 150, 20)
#define K_CARD      CAPP_RGB(150, 156, 168)

/* The grid and its bar. */
#define G_MARGIN    CAPP_RGB(44, 48, 58)
#define G_WHITE     CAPP_RGB(255, 255, 255)
#define G_BLOCK     CAPP_RGB(22, 22, 26)
#define G_LINE      CAPP_RGB(140, 146, 158)
#define G_WORD      CAPP_RGB(184, 212, 255)
#define G_CUR       CAPP_RGB(255, 212, 80)
#define G_INK       CAPP_RGB(20, 20, 32)
#define G_NUM       CAPP_RGB(96, 102, 118)
#define G_RING      CAPP_RGB(150, 156, 172)
#define G_WRONG     CAPP_RGB(210, 40, 40)
#define G_GIVEN     CAPP_RGB(36, 90, 200)
#define B_BG        CAPP_RGB(252, 248, 230)
#define B_TEXT      CAPP_RGB(20, 22, 30)
#define B_LABEL     CAPP_RGB(36, 90, 180)

enum { V_LIST = 0, V_GRID, V_DONE, V_STATS };
enum { ASK_NONE = 0, ASK_CHECK, ASK_REVEAL, ASK_REVEAL_ALL, ASK_RESET, ASK_FORGET };
enum { DK_CLEAN = 0, DK_HELPED, DK_REVEALED };        /* how a solved puzzle was solved */
enum { NET_IDLE = 0, NET_LIST };
enum { GO_FIRST = 0, GO_OLDER, GO_NEWER };           /* which way a page of the list was asked */
enum { M_NONE = 0, M_CARD, M_PLAY, M_DONE, M_HELP }; /* what a row says of its puzzle */

typedef struct {
  char     id[ID_MAX];
  char     date[11];                    /* YYYY-MM-DD, or "" */
  char     title[TITLE_KEEP];
  uint8_t  wd;                          /* 0 Monday .. 6 Sunday, 7 none */
  uint8_t  w, h;
  uint8_t  mark;                        /* M_* */
  uint8_t  looked;                      /* mark and secs have been looked up */
  uint32_t secs;                        /* on the clock, when begun or done */
} Entry;

static struct {
  int      view;
  CRect    at;                          /* the rect the last paint was given */
  int      have_at;
  int      f_ui, f_uib, f_num;          /* ui13, ui13b, num30; -1 is the 6x8 font */
  int      lh, lhb, lhn;                /* their line heights */
  uint8_t  cw[95], cwb[95];             /* each ASCII character's width in ui13, ui13b */
  char     status[64];
  int      bad;
  int      ask;                         /* a question in the footer: ASK_* */

  /* the list: one window of the library */
  Entry    page[PAGE_N];
  int      npage, sel, top;
  int      more_older, more_newer;      /* there may be puzzles past either end of the window */
  int      online;                      /* the server answered, so the list comes from it */
  int      local;                       /* the window is from the card's index */
  int      kind;                        /* KIND_*: the tab */
  int      qwd;                         /* only this weekday, 0 Monday .. 6; 7 for all */
  char     qto[11];                     /* nothing printed after this date; "" for no limit */
  int      net, net_go;                 /* a list request in flight, and which way it was asked */
  char     reply[REPLY_MAX];
  int      look;                        /* the next entry whose card and progress are looked up */
  int      prompting, plen;             /* typing a date to go to */
  char     pbuf[12];
  int      dl;                          /* a download waits for the next tick */
  char     open_id[ID_MAX];             /* ... of this puzzle */

  /* the puzzle */
  Xw       pz;
  int      have_pz;
  char     cur_id[ID_MAX];
  int      ox, oy;                      /* the first column and row on screen */
  int      lay;                         /* LAY_*: which view of the grid */
  int      cross_ci;                    /* the crossing clue the side column shows */
  const char *cl_text;                  /* the clue (in pz's pool), its label and its wrapping */
  char     cl_label[24];
  int      cl_lw;
  int      cl_off[CL_LINES], cl_len[CL_LINES], cl_cut, cl_cut3, cl_n;

  /* playing */
  int      dirty;                       /* the grid has changed since it was written */
  uint32_t last_save, last_key;         /* ticks_ms */
  int      paused;                      /* the clock stopped for want of a key */
  uint32_t shown_secs;                  /* the time the footer shows: set by tick, never by paint */
  char     msg[40];                     /* a footer line after a check or a reveal */
  int      msg_bad;
  uint32_t msg_until;

  /* the results and the statistics */
  XwStats  st;
  int      done_new;                    /* XWS_*: what the solve just done did to its weekday's best */
  int      stats_from;                  /* the view Tab was pressed in */
  int32_t  today;                       /* the day, when a screen that shows streaks was entered */
} G;

/* ---- the letters and marks, as pictures ---------------------------------------
 *
 * Squares are composed pixel by pixel, so the 5x7 capitals of the console font
 * (tools/make_font.py) are here as rows of five bits, the left column the high
 * bit; numbers are a 3x5 font, three bits a row. */

static const uint8_t LETTER[26][7] = {
  { 0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11 },   /* A */
  { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },   /* B */
  { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },   /* C */
  { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C },   /* D */
  { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F },   /* E */
  { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },   /* F */
  { 0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0F },   /* G */
  { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },   /* H */
  { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* I */
  { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C },   /* J */
  { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },   /* K */
  { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },   /* L */
  { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 },   /* M */
  { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 },   /* N */
  { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },   /* O */
  { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 },   /* P */
  { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D },   /* Q */
  { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },   /* R */
  { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E },   /* S */
  { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },   /* T */
  { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },   /* U */
  { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 },   /* V */
  { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11 },   /* W */
  { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },   /* X */
  { 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },   /* Y */
  { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },   /* Z */
};

static const uint8_t DIGIT[10][5] = {
  { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
  { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 2, 2 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
};

/* The ring round a circled square: 13 rows of 13 bits, the left column the high bit. */
static const uint16_t RING[13] = {
  0x1F0, 0x60C, 0x802, 0x802, 0x1001, 0x1001, 0x1001, 0x1001, 0x1001, 0x802, 0x802, 0x60C, 0x1F0,
};

/* The marks of a row, 9 rows of 9 bits. */
static const uint16_t MARK_CHECK[9] = { 0x000, 0x003, 0x006, 0x00C, 0x118, 0x1B0, 0x0E0, 0x040, 0x000 };
static const uint16_t MARK_PLAY[9]  = { 0x07C, 0x0F2, 0x1F1, 0x1F1, 0x1F1, 0x1F1, 0x1F1, 0x0F2, 0x07C };
static const uint16_t MARK_CARD[9]  = { 0x000, 0x000, 0x000, 0x038, 0x038, 0x038, 0x000, 0x000, 0x000 };

/* Monday to Sunday: the Times makes them harder through the week (Sunday is
 * big, not hard), and the colour says so before the date is read. */
static const uint16_t WD_COLOUR[8] = {
  CAPP_RGB(46, 140, 70), CAPP_RGB(92, 144, 50), CAPP_RGB(160, 140, 30), CAPP_RGB(198, 112, 36),
  CAPP_RGB(200, 74, 48), CAPP_RGB(166, 36, 70), CAPP_RGB(64, 84, 178), CAPP_RGB(110, 116, 128),
};

/* ---- small things ---------------------------------------------------------------- */

static int str_cmp(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return (unsigned char)*a - (unsigned char)*b;
}

static void damage_rect(int x, int y, int w, int h) {
  if (G.have_at && w > 0 && h > 0) api->damage(capp_rect(x, y, w, h));
}

static void damage_all(void) {
  if (G.have_at) api->damage(G.at);
}

/* Only the list shows these, so only the list is repainted for them. */
static void damage_top(void) {
  if (G.view == V_LIST && G.have_at) damage_rect(G.at.x, G.at.y, G.at.w, TOP_H);
}

static void damage_foot(void) {
  if (G.have_at) damage_rect(G.at.x, G.at.y + G.at.h - FOOT_H, G.at.w, FOOT_H);
}

static void say(const char *s, int bad) {
  api->fmt(G.status, sizeof G.status, "%s", s);
  G.bad = bad;
  damage_top();
}

/* A key puts the status line away: a message is for the moment after what
 * caused it. */
static void unsay(void) {
  if (!G.status[0]) return;
  G.status[0] = 0;
  damage_top();
}

static void fmt_time(uint32_t secs, char *out, int n) {
  int h = (int)(secs / 3600u), m = (int)((secs / 60u) % 60u), s = (int)(secs % 60u);
  if (h) api->fmt(out, (size_t)n, "%d:%02d:%02d", h, m, s);
  else api->fmt(out, (size_t)n, "%d:%02d", m, s);
}

/* ---- text widths: ui13 is proportional, so wrapping needs them ----------------------- */

static void measure_fonts(void) {
  int i;
  char s[2];
  s[1] = 0;
  for (i = 0; i < 95; i++) {
    s[0] = (char)(32 + i);
    G.cw[i]  = (uint8_t)api->text_width(G.f_ui, s);
    G.cwb[i] = (uint8_t)api->text_width(G.f_uib, s);
  }
  G.lh  = api->font_height(G.f_ui);
  G.lhb = api->font_height(G.f_uib);
  G.lhn = api->font_height(G.f_num);
  if (G.lh < 8) G.lh = 8;
  if (G.lhb < 8) G.lhb = 8;
  if (G.lhn < 8) G.lhn = 8;
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

/* As much of `s` as fits in `maxw` pixels, into `out`, with ".." when it is cut. */
static void fit(int bold, const char *s, int maxw, char *out, int n) {
  int i = 0, w = 0;
  if (str_w(bold, s, 1 << 14) <= maxw) { api->fmt(out, (size_t)n, "%s", s); return; }
  maxw -= str_w(bold, "..", 2);
  while (s[i] && i < n - 3 && w + char_w(bold, s[i]) <= maxw) w += char_w(bold, s[i++]);
  api->mem_cpy(out, s, (size_t)i);
  out[i] = '.'; out[i + 1] = '.'; out[i + 2] = 0;
}

/* A line of text in the box (x, y, w, h): the text, then fills for the rest of
 * the box and never a fill under it -- on a damaged repaint, which goes straight
 * to the panel, filling and then writing over would show. */
static void box_f(int f, int lh, int x, int y, int w, int h, int padl, int right,
                  const char *s, uint16_t fg, uint16_t bg) {
  int tw = s[0] ? api->text_width(f, s) : 0, tx, ty;
  if (lh > h) lh = h;
  ty = y + (h - lh) / 2;
  tx = right ? x + w - 3 - tw : x + padl;
  if (tx < x) tx = x;
  if (ty > y) api->fill(capp_rect(x, y, w, ty - y), bg);
  if (ty + lh < y + h) api->fill(capp_rect(x, ty + lh, w, y + h - ty - lh), bg);
  if (tx > x) api->fill(capp_rect(x, ty, tx - x, lh), bg);
  if (tx + tw < x + w) api->fill(capp_rect(tx + tw, ty, x + w - tx - tw, lh), bg);
  if (s[0]) api->text_font(f, (int16_t)tx, (int16_t)ty, s, fg, bg);
}

static void box_text(int bold, int x, int y, int w, int h, int padl, int right,
                     const char *s, uint16_t fg, uint16_t bg) {
  box_f(bold ? G.f_uib : G.f_ui, bold ? G.lhb : G.lh, x, y, w, h, padl, right, s, fg, bg);
}

/* The left padding that centres `s` in a box w wide. */
static int centre_pad(int f, int w, const char *s) {
  int pad = (w - api->text_width(f, s)) / 2;
  return pad > 0 ? pad : 0;
}

/* ---- reading lines from the card -------------------------------------------------------- */

typedef struct {
  int  fd;
  int  len, pos;
  char buf[256];
} Lines;

static int lines_open(Lines *l, const char *path) {
  l->len = l->pos = 0;
  l->fd = safe_open_read(api, path);
  return l->fd < 0 ? -1 : 0;
}

static void lines_close(Lines *l) {
  if (l->fd >= 0) api->close(l->fd);
  l->fd = -1;
}

/* The next line, without its newline or any CR, cut to fit `n`; 0 at the end. */
static int lines_next(Lines *l, char *line, int n) {
  int k = 0, got = 0;
  for (;;) {
    char c;
    if (l->pos >= l->len) {
      l->len = l->fd >= 0 ? api->read(l->fd, l->buf, sizeof l->buf) : 0;
      l->pos = 0;
      if (l->len <= 0) { l->len = 0; break; }
    }
    c = l->buf[l->pos++];
    got = 1;
    if (c == '\n') { line[k] = 0; return 1; }
    if (c != '\r' && k < n - 1) line[k++] = c;
  }
  line[k] = 0;
  return got;                                  /* a last line with no newline */
}

/* ---- the list's entries ---------------------------------------------------------------------- */

/* An id becomes a path: only the characters a date and its suffixes are made of. */
static int safe_id(const char *s) {
  int i;
  if (!s[0] || s[0] == '.') return 0;
  for (i = 0; s[i]; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          c == '-' || c == '_' || c == '.'))
      return 0;
  }
  return 1;
}

/* A line the server's list sends, which is also a line of index.txt:
 * id <tab> date <tab> Wkd <tab> WxH <tab> title. 0 if it is not one. */
static int parse_entry(const char *line, Entry *e) {
  char f[48];
  const char *t;
  int i;
  api->mem_set(e, 0, sizeof *e);
  tsv_field(line, 0, f, sizeof f);
  if (!safe_id(f) || (int)api->str_len(f) >= ID_MAX) return 0;
  api->fmt(e->id, sizeof e->id, "%s", f);
  tsv_field(line, 1, f, sizeof f);
  api->fmt(e->date, sizeof e->date, "%s", f);
  tsv_field(line, 2, f, sizeof f);
  e->wd = 7;
  for (i = 0; i < 7; i++) if (str_same(f, xw_day_name(i))) e->wd = (uint8_t)i;
  tsv_field(line, 3, f, sizeof f);
  t = f;
  e->w = (uint8_t)str_uint(&t);
  if (*t == 'x') { t++; e->h = (uint8_t)str_uint(&t); }
  tsv_field(line, 4, f, sizeof f);
  api->fmt(e->title, sizeof e->title, "%s", f);
  return 1;
}

static void puz_path(char *out, size_t n, const char *id, int part) {
  api->fmt(out, n, PUZ_DIR "/%s.xw%s", id, part ? ".part" : "");
}

static void save_path(char *out, size_t n, const char *id) {
  api->fmt(out, n, SAVE_DIR "/%s.sav", id);
}

/* The top of a puzzle's progress file: its state, time and how full it is.
 * 1 if there is one. */
static int peek_progress(const char *id, XwPeek *pk) {
  char path[48], buf[160];
  int fd, got = 0, n;
  save_path(path, sizeof path, id);
  fd = safe_open_read(api, path);
  if (fd < 0) return 0;
  while (got < (int)sizeof buf - 1 && (n = api->read(fd, buf + got, (size_t)((int)sizeof buf - 1 - got))) > 0)
    got += n;
  api->close(fd);
  buf[got] = 0;
  return xw_peek(buf, pk) == XW_OK;
}

/* What the card knows of an entry: is the puzzle there, and how far has it
 * been taken. 1 if that changed what the row shows. */
static int lookup_one(Entry *e) {
  char path[48];
  CappStat st;
  XwPeek pk;
  uint8_t mark = M_NONE, shown = e->looked ? e->mark : M_NONE;
  uint32_t secs = 0, shown_secs = e->looked ? e->secs : 0;
  int changed;
  api->mem_set(&pk, 0, sizeof pk);
  puz_path(path, sizeof path, e->id, 0);
  if (api->stat(path, &st) == 0) mark = M_CARD;
  if (peek_progress(e->id, &pk) && !(pk.state == 'P' && pk.filled == 0)) {
    mark = pk.state == 'S' ? M_DONE : pk.state == 'H' ? M_HELP : M_PLAY;
    secs = pk.secs;
  }
  changed = shown != mark || shown_secs != secs;     /* a row that looks the same is not repainted */
  e->looked = 1;
  e->mark = mark;
  e->secs = secs;
  return changed;
}

/* ---- the card's index of puzzles ----------------------------------------------------------------- */

static int kind_of(const char *id) {
  int n = (int)api->str_len(id), k = (int)sizeof MIDI_SUFFIX - 1;
  if (id[0] < '0' || id[0] > '9') return KIND_THEMED;
  return n >= k && !str_cmp(id + n - k, MIDI_SUFFIX) ? KIND_MIDI : KIND_FULL;
}

static int entry_ok(const Entry *e) {
  if (kind_of(e->id) != G.kind) return 0;
  if (G.qwd != 7 && e->wd != G.qwd) return 0;
  if (G.qto[0] && (!e->date[0] || str_cmp(e->date, G.qto) > 0)) return 0;
  return 1;
}

/* Walk index.txt for the entries one page of direction `go` holds: from the
 * top; past `after` (older) or before it (newer). With dst, copy them, up to
 * PAGE_N, leaving out the first `skip`. Returns how many matched in all --
 * counting stops once a page and one more have, unless the page is of newer
 * ones, which is the only way to know how many to skip. *more says the walk
 * met an entry past the page. */
static int local_walk(int go, const char *after, int skip, Entry *dst, int *more) {
  Lines l;
  char line[160];
  Entry e;
  int n = 0, put = 0;
  *more = 0;
  if (lines_open(&l, INDEX_PATH) != 0) return 0;
  while (lines_next(&l, line, sizeof line)) {
    if (!parse_entry(line, &e)) continue;
    if (go == GO_OLDER && str_cmp(e.id, after) >= 0) continue;
    if (go == GO_NEWER && str_cmp(e.id, after) <= 0) break;
    if (!entry_ok(&e)) continue;
    n++;
    if (dst) {
      if (n <= skip) continue;
      if (put >= PAGE_N) { *more = 1; break; }
      api->mem_cpy(&dst[put++], &e, sizeof e);
    } else if (go != GO_NEWER && n > PAGE_N) {
      *more = 1;
      break;
    }
  }
  lines_close(&l);
  return n;
}

/* The window is a new page: where the selection starts, and whether there may
 * be more either side. */
static void page_loaded(int go, int n, int more) {
  G.npage = n;
  if (go == GO_FIRST) { G.more_newer = 0; G.more_older = more; G.sel = 0; }
  else if (go == GO_OLDER) { G.more_newer = 1; G.more_older = more; G.sel = 0; }
  else { G.more_older = 1; G.more_newer = more; G.sel = n - 1; }
  G.top = 0;
  G.look = 0;
}

static int list_rows(void) {
  int h = G.have_at ? G.at.h : 135, r = (h - TOP_H - FOOT_H) / ROW_H;
  return r > 1 ? r : 1;
}

/* Scroll the window so the selection shows. 1 if it moved. */
static int fix_top(void) {
  int rows = list_rows(), was = G.top;
  if (G.sel < G.top) G.top = G.sel;
  if (G.sel >= G.top + rows) G.top = G.sel - rows + 1;
  if (G.top < 0) G.top = 0;
  return G.top != was;
}

/* A page of the list from the card. 1 if it found any (and replaced the
 * window), else 0 and the window stays. */
static int local_page(int go) {
  char after[ID_MAX];
  int n, more, skip = 0;
  after[0] = 0;
  if (go == GO_OLDER && G.npage) api->fmt(after, sizeof after, "%s", G.page[G.npage - 1].id);
  if (go == GO_NEWER && G.npage) api->fmt(after, sizeof after, "%s", G.page[0].id);
  n = local_walk(go, after, 0, 0, &more);
  if (n == 0) return 0;
  if (go == GO_NEWER && n > PAGE_N) skip = n - PAGE_N;
  local_walk(go, after, skip, G.page, &more);
  n = n - skip < PAGE_N ? n - skip : PAGE_N;
  G.local = 1;
  page_loaded(go, n, go == GO_NEWER ? skip > 0 : more);
  fix_top();
  return 1;
}

/* ---- the card's index, kept ---------------------------------------------------------------------- */

/* A puzzle now on the card goes into index.txt, which stays newest first: the
 * file is copied with its line in the place its id sorts to. */
static void index_put(const char *id) {
  Lines l;
  SafeFile f;
  char line[160], mine[160], other[ID_MAX];
  int placed = 0, wd = xw_puzzle_weekday(&G.pz), n;
  char date[12];
  if (G.pz.year) api->fmt(date, sizeof date, "%04d-%02d-%02d", G.pz.year, G.pz.month, G.pz.day);
  else date[0] = 0;
  n = api->fmt(mine, sizeof mine, "%s\t%s\t%s\t%dx%d\t%s\n", id, date, wd >= 0 ? xw_day_name(wd) : "",
               G.pz.w, G.pz.h, G.pz.title);
  if (n <= 0 || n >= (int)sizeof mine) return;
  api->mkdir(PUZ_DIR);
  l.fd = -1;
  lines_open(&l, INDEX_PATH);                  /* before safe_begin: it may finish a cut save */
  if (safe_begin(&f, api, INDEX_PATH) != 0) { lines_close(&l); return; }
  while (l.fd >= 0 && lines_next(&l, line, sizeof line)) {
    int c;
    tsv_field(line, 0, other, sizeof other);
    c = str_cmp(other, id);
    if (c == 0 || !other[0]) continue;         /* the old line for this puzzle, or a blank */
    if (!placed && c < 0) { safe_write(&f, mine, (size_t)n); placed = 1; }
    safe_write(&f, line, api->str_len(line));
    safe_write(&f, "\n", 1);
  }
  lines_close(&l);
  if (!placed) safe_write(&f, mine, (size_t)n);
  safe_commit(&f);
}

/* ---- the server's list ------------------------------------------------------------------------------ */

/* Ask for a page. From the card when the server is not what the list comes
 * from; otherwise a request that tick will collect. */
static void list_request(int go) {
  char url[220];
  int k;
  if (G.net != NET_IDLE) return;
  if (!G.online) {
    if (!local_page(go)) {
      if (go == GO_FIRST) {
        G.npage = G.sel = G.top = 0;
        G.more_older = G.more_newer = 0;
        G.local = 1;
      } else {
        say(go == GO_OLDER ? "that is the oldest on the card" : "that is the newest on the card", 0);
      }
    }
    damage_all();
    return;
  }
  k = api->fmt(url, sizeof url, "%s/crossword/list?limit=%d&kind=%s", api->proxy(), PAGE_N, KIND_ARG[G.kind]);
  if (G.qwd != 7) k += api->fmt(url + k, sizeof url - (size_t)k, "&weekday=%d", G.qwd);
  if (G.qto[0]) k += api->fmt(url + k, sizeof url - (size_t)k, "&to=%s", G.qto);
  if (go == GO_OLDER && G.npage)
    k += api->fmt(url + k, sizeof url - (size_t)k, "&after=%s", G.page[G.npage - 1].id);
  if (go == GO_NEWER && G.npage)
    k += api->fmt(url + k, sizeof url - (size_t)k, "&order=old&after=%s", G.page[0].id);
  if (api->http_start("GET", url, 0, 0, "", 15000) != 0) {
    say("busy: another request is out", 1);
    return;
  }
  G.net = NET_LIST;
  G.net_go = go;
  say("loading...", 0);
}

/* The reply's lines as the window. 1 if there were any. */
static int absorb_list(int go) {
  const char *p = G.reply;
  Entry e, t;
  int n = 0, i, cnt = 0;
  while (*p) {
    if (*p != '\n' && parse_entry(p, &e)) cnt++;
    p = tsv_next_line(p);
  }
  if (!cnt) return 0;
  p = G.reply;
  while (*p && n < PAGE_N) {
    if (*p != '\n' && parse_entry(p, &e)) api->mem_cpy(&G.page[n++], &e, sizeof e);
    p = tsv_next_line(p);
  }
  if (go == GO_NEWER)                           /* asked oldest-first: the nearest to the window first */
    for (i = 0; i < n / 2; i++) {
      api->mem_cpy(&t, &G.page[i], sizeof t);
      api->mem_cpy(&G.page[i], &G.page[n - 1 - i], sizeof t);
      api->mem_cpy(&G.page[n - 1 - i], &t, sizeof t);
    }
  G.local = 0;
  page_loaded(go, n, n >= PAGE_N);
  fix_top();
  return 1;
}

/* The one line of the server's "error why" reply, or a status as words. */
static void say_failure(int n) {
  int i;
  if (str_starts(G.reply, "error ")) api->fmt(G.status, sizeof G.status, "%s", G.reply + 6);
  else if (n == -403) api->fmt(G.status, sizeof G.status, "the server wants a token (claude.token)");
  else api->fmt(G.status, sizeof G.status, "cannot reach the server (%d)", n);
  for (i = 0; G.status[i]; i++) if (G.status[i] == '\n') { G.status[i] = 0; break; }
  G.bad = 1;
}

static int poll_list(void) {
  int n, go = G.net_go;
  n = api->http_poll(G.reply, sizeof G.reply);
  if (n == CAPP_HTTP_PENDING) return 0;
  G.net = NET_IDLE;
  if (n < 0) {
    G.reply[0] = 0;
    say_failure(n);
    G.online = 0;                               /* the card's list, until `r` asks again */
    if (local_page(go)) {
      api->fmt(G.status, sizeof G.status, "offline: the puzzles on the card");
    } else if (go == GO_FIRST) {
      G.npage = G.sel = G.top = 0;
      G.more_older = G.more_newer = 0;
      G.local = 1;
    }
    damage_all();
    return 1;
  }
  G.reply[n] = 0;
  if (CAPP_HTTP_FILLED(n, REPLY_MAX)) {         /* the last line may be cut short: leave it out */
    while (n > 0 && G.reply[n - 1] != '\n') n--;
    G.reply[n] = 0;
  }
  if (str_starts(G.reply, "error ")) {
    say_failure(n);
  } else if (absorb_list(go)) {
    G.status[0] = 0;
  } else if (go == GO_FIRST) {
    G.npage = G.sel = G.top = 0;
    G.more_older = G.more_newer = 0;
    G.local = 0;
    G.status[0] = 0;
  } else {
    say(go == GO_OLDER ? "that is the oldest puzzle" : "that is the newest puzzle", 0);
  }
  damage_all();
  return 1;
}

/* ---- filters --------------------------------------------------------------------------------------- */

/* "1993", "1993-11" or "1993-11-21" as the last day it names: out is
 * YYYY-MM-DD. 0 if it is none of those. */
static int normalize_to(const char *in, char *out) {
  const char *p = in;
  int y, m = 12, d = 0, digits;
  digits = 0;
  y = 0;
  while (*p >= '0' && *p <= '9') { y = y * 10 + (*p++ - '0'); digits++; }
  if (digits != 4) return 0;
  if (*p == '-') {
    int md = 0;
    p++;
    m = 0;
    while (*p >= '0' && *p <= '9' && md < 2) { m = m * 10 + (*p++ - '0'); md++; }
    if (!md || m < 1 || m > 12) return 0;
    if (*p == '-') {
      int dd = 0;
      p++;
      d = 0;
      while (*p >= '0' && *p <= '9' && dd < 2) { d = d * 10 + (*p++ - '0'); dd++; }
      if (!dd) return 0;
    }
  }
  if (*p) return 0;
  if (!d) d = xw_month_days(y, m);
  if (!xw_valid_date(y, m, d)) return 0;
  api->fmt(out, 11, "%04d-%02d-%02d", y, m, d);
  return 1;
}

/* The list from its newest puzzle again, under the filters as they are. A
 * request still out comes first: its reply is for the window as it was. */
static void reload_first(void) {
  if (G.net != NET_IDLE) {
    say("wait: the list is loading", 1);
    return;
  }
  G.npage = 0;
  list_request(GO_FIRST);
  damage_all();
}

static void describe_filter(char *out, int n) {
  out[0] = 0;
  if (G.qwd != 7 && G.qto[0]) api->fmt(out, (size_t)n, "%s only, to %s", xw_day_name(G.qwd), G.qto);
  else if (G.qwd != 7) api->fmt(out, (size_t)n, "%s only", xw_day_name(G.qwd));
  else if (G.qto[0]) api->fmt(out, (size_t)n, "to %s", G.qto);
}

/* ---- the puzzle ------------------------------------------------------------------------------------- */

/* A puzzle file, whole, into G.pz. XW_OK, an XW_ERR_*, or NO_FILE. */
static int load_puzzle(const char *path) {
  Lines l;
  char line[256];
  int r = XW_OK;
  xw_init(&G.pz);
  G.have_pz = 0;
  if (lines_open(&l, path) != 0) return NO_FILE;
  while (lines_next(&l, line, sizeof line)) {
    r = xw_load_line(&G.pz, line);
    if (r != XW_OK) break;
  }
  lines_close(&l);
  if (r == XW_DONE) r = XW_OK;
  else if (r == XW_OK) r = xw_load_end(&G.pz);
  if (r == XW_OK) G.have_pz = 1;
  return r;
}

/* What was saved of this puzzle, put back into it. */
static void restore_progress(const char *id) {
  char path[48], buf[XW_SAVE_MAX + 1];
  int fd, got = 0, n;
  save_path(path, sizeof path, id);
  fd = safe_open_read(api, path);
  if (fd < 0) return;
  while (got < XW_SAVE_MAX && (n = api->read(fd, buf + got, (size_t)(XW_SAVE_MAX - got))) > 0) got += n;
  api->close(fd);
  buf[got] = 0;
  xw_restore(&G.pz, buf);
}

/* ---- the statistics, on the card ------------------------------------------------------------------------- */

/* Today as a day number, or XWS_NO_DAY when the device has not been told the
 * date (a streak cannot be counted without one). */
static int32_t today_day(void) {
  CappTime t;
  api->now(&t);
  if (!t.synced || t.year < 2020) return XWS_NO_DAY;
  return xw_days(t.year, t.month, t.day);
}

static void stats_load(void) {
  char buf[512];
  int fd, got = 0, n;
  xws_init(&G.st);
  fd = safe_open_read(api, STATS_PATH);
  if (fd < 0) return;
  while (got < (int)sizeof buf - 1 && (n = api->read(fd, buf + got, (size_t)((int)sizeof buf - 1 - got))) > 0)
    got += n;
  api->close(fd);
  buf[got] = 0;
  xws_parse(&G.st, buf);
}

static void stats_save(void) {
  char buf[512];
  SafeFile f;
  int n = xws_format(&G.st, buf, sizeof buf);
  if (n < 0) return;
  api->mkdir(CAPP_VAR);
  api->mkdir(SAVE_DIR);
  if (safe_begin(&f, api, STATS_PATH) != 0) return;
  safe_write(&f, buf, (size_t)n);
  safe_commit(&f);
}

/* Which view, kept across launches: the side view unless another was chosen. */
static void view_load(void) {
  char buf[16];
  int fd, n;
  G.lay = LAY_SIDE;
  fd = safe_open_read(api, VIEW_PATH);
  if (fd < 0) return;
  n = api->read(fd, buf, sizeof buf - 1);
  api->close(fd);
  buf[n > 0 ? n : 0] = 0;
  if (buf[0] == 'b') G.lay = LAY_BIG;
  else if (buf[0] == 'z') G.lay = LAY_ZOOM;
  for (n = 0; buf[n] && buf[n] != '\n'; n++) {}
  G.kind = KIND_FULL;
  if (buf[n] == '\n' && buf[n + 1] == 'm') G.kind = KIND_MIDI;
  if (buf[n] == '\n' && buf[n + 1] == 't') G.kind = KIND_THEMED;
}

static void view_save(void) {
  SafeFile f;
  api->mkdir(CAPP_VAR);
  api->mkdir(SAVE_DIR);
  if (safe_begin(&f, api, VIEW_PATH) != 0) return;
  safe_line(&f, LAY_NAME[G.lay]);
  safe_line(&f, "\n");
  safe_line(&f, KIND_ARG[G.kind]);
  safe_line(&f, "\n");
  safe_commit(&f);
}

/* ---- the grid's geometry ------------------------------------------------------------------------------- */

typedef struct {
  CRect area;                    /* the grid's part of the screen */
  int   bar_y, bar_h;            /* the clue bar (the big view; 0 high in the side view) */
  CRect col;                     /* the clue column (the side view; empty in the big one) */
  int   cs;                      /* a square's pitch */
  int   vcols, vrows;            /* squares that fit */
  int   nc, nr;                  /* squares shown */
  int   gx0, gy0;                /* where the first shown square is */
} Geo;

/* Three views, ctrl-l from one to the next:
 *   side  the grid against the left edge in the biggest squares that show it
 *         whole (CELL_SIDE down to CELL_MIN, so a daily fits and only a Sunday
 *         scrolls), the clue and the one crossing it in a column down the right
 *   big   CELL squares, centred, the clue on a two-line bar under them
 *   zoom  squares as wide as the screen allows (up to CELL_ZOOM), so the grid
 *         runs edge to edge -- 240 is 15 sixteens and 10 twenty-fours -- and
 *         scrolls up and down; the clue on one line under it. The closing
 *         line on the right falls off the screen when the grid is exactly as
 *         wide as it. */
static void geometry(Geo *g) {
  CRect c = G.have_at ? G.at : capp_rect(0, 0, 240, 135);
  int gw, gh, h = c.h - FOOT_H, pw = G.pz.w > 0 ? G.pz.w : 1, ph = G.pz.h > 0 ? G.pz.h : 1;
  if (G.lay == LAY_SIDE) {
    int byh = (h - 1) / ph, byw = (c.w - COL_MIN - 1) / pw;
    g->cs = byh < byw ? byh : byw;
    if (g->cs > CELL_SIDE) g->cs = CELL_SIDE;
    if (g->cs < CELL_MIN) g->cs = CELL_MIN;
    g->bar_h = 0;
    g->bar_y = c.y + h;
    g->area = capp_rect(c.x, c.y, c.w - COL_MIN, h);
    g->vcols = (g->area.w - 1) / g->cs;
  } else {
    g->cs = CELL;
    if (G.lay == LAY_ZOOM) {
      g->cs = c.w / pw;
      if (g->cs > CELL_ZOOM) g->cs = CELL_ZOOM;
      if (g->cs < CELL) g->cs = CELL;
    }
    g->bar_h = (G.lay == LAY_ZOOM ? G.cl_n : 2) * G.lh + 4;     /* zoom: as tall as the clue */
    g->bar_y = c.y + h - g->bar_h;
    g->area = capp_rect(c.x, c.y, c.w, g->bar_y - c.y);
    g->vcols = (G.lay == LAY_ZOOM && pw * g->cs == c.w ? c.w : c.w - 1) / g->cs;
  }
  g->vrows = (g->area.h - 1) / g->cs;
  if (g->vcols < 1) g->vcols = 1;
  if (g->vrows < 1) g->vrows = 1;
  g->nc = G.pz.w - G.ox < g->vcols ? G.pz.w - G.ox : g->vcols;
  g->nr = G.pz.h - G.oy < g->vrows ? G.pz.h - G.oy : g->vrows;
  if (g->nc < 1) g->nc = 1;
  if (g->nr < 1) g->nr = 1;
  gw = g->nc * g->cs + 1;
  gh = g->nr * g->cs + 1;
  if (G.lay == LAY_SIDE) {
    g->area.w = gw;
    g->col = capp_rect(c.x + gw, c.y, c.w - gw, h);
    g->gx0 = c.x;
  } else {
    if (gw > g->area.w) gw = g->area.w;
    g->col = capp_rect(c.x + c.w, c.y, 0, 0);
    g->gx0 = g->area.x + (g->area.w - gw) / 2;
  }
  g->gy0 = g->area.y + (g->area.h - gh) / 2;
}

/* Keep the cursor on screen, a square in from the edge where the grid is
 * bigger than the screen. 1 if the view moved. */
static int scroll_to_cursor(void) {
  Geo g;
  int r = G.pz.cur / G.pz.w, c = G.pz.cur % G.pz.w, nx = G.ox, ny = G.oy;
  geometry(&g);
  if (G.pz.w <= g.vcols) nx = 0;
  else {
    if (c < nx + 1) nx = c - 1;
    if (c > nx + g.vcols - 2) nx = c - g.vcols + 2;
    if (nx > G.pz.w - g.vcols) nx = G.pz.w - g.vcols;
    if (nx < 0) nx = 0;
  }
  if (G.pz.h <= g.vrows) ny = 0;
  else {
    if (r < ny + 1) ny = r - 1;
    if (r > ny + g.vrows - 2) ny = r - g.vrows + 2;
    if (ny > G.pz.h - g.vrows) ny = G.pz.h - g.vrows;
    if (ny < 0) ny = 0;
  }
  if (nx == G.ox && ny == G.oy) return 0;
  G.ox = nx;
  G.oy = ny;
  return 1;
}

/* Mark the part of rect (x, y, w, h) inside the grid's area. */
static void damage_in_area(const Geo *g, int x, int y, int w, int h) {
  int x1 = x + w, y1 = y + h;
  if (x < g->area.x) x = g->area.x;
  if (y < g->area.y) y = g->area.y;
  if (x1 > g->area.x + g->area.w) x1 = g->area.x + g->area.w;
  if (y1 > g->area.y + g->area.h) y1 = g->area.y + g->area.h;
  damage_rect(x, y, x1 - x, y1 - y);
}

/* The squares of a word, as one rectangle. */
static void damage_word(int ci) {
  Geo g;
  const XwClue *cl;
  int r0, c0, nc, nr;
  if (ci < 0 || !G.have_at) return;
  geometry(&g);
  cl = &G.pz.clue[ci];
  r0 = cl->cell / G.pz.w;
  c0 = cl->cell % G.pz.w;
  nc = cl->dir == XW_ACROSS ? cl->len : 1;
  nr = cl->dir == XW_DOWN ? cl->len : 1;
  damage_in_area(&g, g.gx0 + (c0 - G.ox) * g.cs, g.gy0 + (r0 - G.oy) * g.cs, nc * g.cs + 1, nr * g.cs + 1);
}

/* The clue's place: the bar, or the side view's column. */
static void damage_bar(void) {
  Geo g;
  if (!G.have_at) return;
  geometry(&g);
  if (G.lay != LAY_SIDE) damage_rect(G.at.x, g.bar_y, G.at.w, g.bar_h);
  else damage_rect(g.col.x, g.col.y, g.col.w, g.col.h);
}

/* ---- the clue under the grid -------------------------------------------------------------------------- */

/* One line of `s` from `start` in at most `avail` pixels, broken at a space; a
 * word wider than the line breaks inside. Where the next line begins. */
static int wrap_one(const char *s, int start, int avail, int *off, int *len) {
  int p = start, px = 0, brk = -1, i;
  while (s[p] == ' ') p++;
  *off = p;
  for (i = p; s[i]; i++) {
    int w = char_w(0, s[i]);
    if (px + w > avail) break;
    if (s[i] == ' ') brk = i;
    px += w;
  }
  if (!s[i] || s[i] == ' ') { *len = i - p; return i; }
  if (brk > p) { *len = brk - p; return brk; }
  *len = i > p ? i - p : 1;
  return p + *len;
}

/* The current word's clue, wrapped to the bar: its label ("12-Across") in
 * bold, then the text over up to CL_LINES lines. The big view shows two, the
 * second cut with ".." if there is more (cl_cut); the zoom view's bar is as
 * many as the clue needs (cl_n), cut only past the third (cl_cut3). */
static void clue_layout(void) {
  int ci = xw_cur_clue(&G.pz), p, k, w = (G.have_at ? G.at.w : 240) - 8;
  G.cl_text = "";
  G.cl_label[0] = 0;
  G.cl_lw = 0;
  for (k = 0; k < CL_LINES; k++) G.cl_off[k] = G.cl_len[k] = 0;
  G.cl_cut = G.cl_cut3 = 0;
  G.cl_n = 1;
  if (ci < 0) return;
  api->fmt(G.cl_label, sizeof G.cl_label, "%d-%s", G.pz.clue[ci].num,
           G.pz.clue[ci].dir == XW_ACROSS ? "Across" : "Down");
  G.cl_lw = str_w(1, G.cl_label, 1 << 14) + 6;
  G.cl_text = xw_clue_text(&G.pz, ci);
  p = 0;
  for (k = 0; k < CL_LINES; k++) {
    p = wrap_one(G.cl_text, p, k ? w : w - G.cl_lw, &G.cl_off[k], &G.cl_len[k]);
    if (G.cl_len[k] > 0) G.cl_n = k + 1;
    if (k == 1) {
      int q = p;
      while (G.cl_text[q] == ' ') q++;
      G.cl_cut = G.cl_text[q] != 0;
    }
  }
  while (G.cl_text[p] == ' ') p++;
  G.cl_cut3 = G.cl_text[p] != 0;
}

/* ---- playing: the clock, the writing of progress, a win, and opening and leaving a puzzle ---------------------- */

/* The seconds on the puzzle's clock at `now`. xwcore's clock adds now - t0 as
 * unsigned, so a `now` a few ms before t0 would read as 49 days: clamped. */
static uint32_t elapsed_s(uint32_t now) {
  uint32_t ms = G.pz.ms;
  if (G.pz.running && (int32_t)(now - G.pz.t0) > 0) ms += now - G.pz.t0;
  return ms / 1000u;
}

/* Stop the clock as of ms `t`, which cannot be before it started. */
static void clock_stop_at(uint32_t t) {
  if (!G.pz.running) return;
  if ((int32_t)(t - G.pz.t0) < 0) t = G.pz.t0;
  xw_clock_stop(&G.pz, t);
}

/* A line for the footer, for a few seconds. */
static void say_msg(const char *s, int bad) {
  api->fmt(G.msg, sizeof G.msg, "%s", s);
  G.msg_bad = bad;
  G.msg_until = api->ticks_ms() + MSG_MS;
  damage_foot();
}

/* Write the puzzle's progress. The file keeps the time gone, so what is
 * written is the clock as it stands -- put into ms for xw_save to read and
 * put back, the clock itself undisturbed. */
static void save_progress(void) {
  char buf[XW_SAVE_MAX], path[48];
  SafeFile f;
  uint32_t now = api->ticks_ms(), keep = G.pz.ms;
  int n;
  if (!G.have_pz || !G.cur_id[0]) return;
  if (G.pz.running && (int32_t)(now - G.pz.t0) > 0) G.pz.ms += now - G.pz.t0;
  n = xw_save(&G.pz, buf, sizeof buf);
  G.pz.ms = keep;
  if (n <= 0) return;
  api->mkdir(CAPP_VAR);
  api->mkdir(SAVE_DIR);
  save_path(path, sizeof path, G.cur_id);
  if (safe_begin(&f, api, path) != 0) return;
  safe_write(&f, buf, (size_t)n);
  if (safe_commit(&f) == 0) {
    G.dirty = 0;
    G.last_save = now;
  }
}

static void enter_grid(const char *id) {
  uint32_t now = api->ticks_ms();
  api->fmt(G.cur_id, sizeof G.cur_id, "%s", id);
  restore_progress(id);
  G.view = V_GRID;
  G.ox = G.oy = 0;
  clue_layout();                                /* first: the zoom view's bar is as tall as the clue */
  scroll_to_cursor();
  G.status[0] = 0;
  G.ask = ASK_NONE;
  G.msg[0] = 0;
  G.dirty = 0;
  G.paused = 0;
  G.done_new = XWS_NOT_BEST;
  G.last_key = G.last_save = now;
  xw_clock_start(&G.pz, now);                   /* a solved puzzle's clock does not run */
  G.shown_secs = elapsed_s(now);
  damage_all();
}

/* Back to the list. What was done is written first unless it is a puzzle only
 * looked at, and the clock stops. */
static void leave_grid(void) {
  uint32_t now = api->ticks_ms();
  if (G.have_pz && !G.pz.solved && (G.dirty || elapsed_s(now) >= 10)) save_progress();
  clock_stop_at(now);
  G.view = V_LIST;
  G.ask = ASK_NONE;
  G.look = 0;                                   /* what was done in there shows in the row */
  damage_all();
}

/* How the solved puzzle was solved: by its own letters, with some given, or
 * mostly given. Worked out from the grid, so it holds after a restart too. */
static int done_kind(void) {
  int rev = xw_reveals(&G.pz), whites = 0;
  xw_count(&G.pz, 0, &whites);
  if (!rev) return DK_CLEAN;
  return rev * 2 >= whites ? DK_REVEALED : DK_HELPED;
}

/* The last square went in, or the last was revealed: the clock stops, the
 * solve is counted, everything is written, and the results are shown. */
static void on_won(void) {
  uint32_t now = api->ticks_ms(), secs;
  int kind;
  clock_stop_at(now);
  secs = G.pz.ms / 1000u;
  kind = done_kind();
  G.done_new = XWS_NOT_BEST;
  G.today = today_day();
  if (kind == DK_CLEAN) G.done_new = xws_solve(&G.st, xw_puzzle_weekday(&G.pz), secs, G.today);
  else if (kind == DK_HELPED) xws_help(&G.st);
  if (kind != DK_REVEALED) stats_save();
  G.shown_secs = secs;
  G.paused = 0;
  G.ask = ASK_NONE;
  G.msg[0] = 0;
  save_progress();
  G.view = V_DONE;
  damage_all();
}

/* Check or reveal a letter, the word, or the whole puzzle. */
static void do_scope(int reveal, int scope) {
  int old = xw_cur_clue(&G.pz), n, r = 0;
  char tmp[40];
  if (G.pz.solved) { say_msg("already solved", 0); return; }
  if (!reveal) {
    n = xw_check(&G.pz, scope);
    if (scope == XW_SCOPE_CELL) {
      if (!G.pz.cell[G.pz.cur].ent) say_msg("nothing there yet", 0);
      else say_msg(n ? "that letter is wrong" : "that letter is right", n != 0);
    } else if (n) {
      api->fmt(tmp, sizeof tmp, scope == XW_SCOPE_WORD ? "%d wrong in this word" : "%d wrong", n);
      say_msg(tmp, 1);
    } else {
      say_msg(scope == XW_SCOPE_WORD ? "no mistakes in this word yet" : "no mistakes so far", 0);
    }
  } else {
    r = xw_reveal(&G.pz, scope);
    if (!(r & XW_R_CELL)) { say_msg("already right", 0); return; }
    G.dirty = 1;
    say_msg("revealed: no best time now", 0);
  }
  if (scope == XW_SCOPE_ALL) damage_all();
  else damage_word(old);
  if (r & XW_R_WON) on_won();
}

/* The puzzle from the start, its saved progress gone. */
static void do_reset(void) {
  char path[48];
  uint32_t now = api->ticks_ms();
  xw_reset(&G.pz);
  save_path(path, sizeof path, G.cur_id);
  api->remove(path);
  G.dirty = 0;
  G.paused = 0;
  G.msg[0] = 0;
  G.done_new = XWS_NOT_BEST;
  G.last_key = G.last_save = now;
  xw_clock_start(&G.pz, now);
  G.shown_secs = 0;
  G.view = V_GRID;
  G.ox = G.oy = 0;
  clue_layout();                                /* first: the zoom view's bar is as tall as the clue */
  scroll_to_cursor();
  damage_all();
}

/* The puzzle is downloaded: the list's own call, on the tick after Enter. */
static void do_download(void) {
  char url[160], part[56], path[48];
  int n, r;
  G.dl = 0;
  if (!api->net_ready() && api->net_connect(15000) != 0) {
    say("offline: that puzzle is not on the card", 1);
    return;
  }
  api->mkdir(PUZ_DIR);
  puz_path(part, sizeof part, G.open_id, 1);
  puz_path(path, sizeof path, G.open_id, 0);
  api->fmt(url, sizeof url, "%s/crossword/puzzle?id=%s", api->proxy(), G.open_id);
  api->remove(part);
  n = api->http_download(url, part, 30000);
  if (n < 0) {
    api->remove(part);
    if (n == -404) api->fmt(G.status, sizeof G.status, "the server has no such puzzle");
    else api->fmt(G.status, sizeof G.status, "download failed (%d)", n);
    G.bad = 1;
    damage_top();
    return;
  }
  r = load_puzzle(part);
  if (r != XW_OK) {
    api->remove(part);
    api->fmt(G.status, sizeof G.status, "a bad puzzle: %s", r == NO_FILE ? "nothing arrived" : xw_err_text(r));
    G.bad = 1;
    damage_top();
    return;
  }
  api->remove(path);
  if (api->rename(part, path) != 0) {
    say("could not keep the puzzle: is the card full?", 1);
    return;
  }
  index_put(G.open_id);
  enter_grid(G.open_id);
}

/* Enter on a row: open the puzzle from the card, or fetch it first. */
static void open_entry(int i) {
  char path[48];
  CappStat st;
  int r;
  if (i < 0 || i >= G.npage || G.dl) return;
  api->fmt(G.open_id, sizeof G.open_id, "%s", G.page[i].id);
  puz_path(path, sizeof path, G.open_id, 0);
  if (api->stat(path, &st) == 0) {
    r = load_puzzle(path);
    if (r == XW_OK) { enter_grid(G.open_id); return; }
    api->fmt(G.status, sizeof G.status, "the saved puzzle is damaged (%s): fetching it again",
             r == NO_FILE ? "unreadable" : xw_err_text(r));
    G.bad = 1;
  } else {
    api->fmt(G.status, sizeof G.status, "downloading %s...", G.page[i].date[0] ? G.page[i].date : G.open_id);
    G.bad = 0;
  }
  G.dl = 1;
  damage_top();
}

/* ---- painting: the list ------------------------------------------------------------------------------------ */

/* The bar across the top: the name, which filters are on, and on the right
 * what is going on (a status) or where the list comes from. */
/* The tabs, where the name was: the one shown in bold with a blue line
 * under it. Each is drawn above its two rows of underline, so nothing is
 * filled and then written over. Their width. */
static int paint_tabs(CRect c, int h) {
  int i, x = c.x;
  for (i = 0; i < KIND_N; i++) {
    int on = i == G.kind, w = (i ? 6 : MARGIN) + str_w(on, KIND_TAB[i], 99) + 6;
    box_text(on, x, c.y, w, h - 2, i ? 6 : MARGIN, 0, KIND_TAB[i], on ? L_TEXT : L_DIM, L_BAR);
    api->fill(capp_rect(x, c.y + h - 2, w, 2), L_BAR);
    if (on) api->fill(capp_rect(x + (i ? 4 : MARGIN - 2), c.y + h - 2, w - (i ? 6 : MARGIN - 2) - 2, 2), K_PLAY);
    x += w;
  }
  return x - c.x;
}

static void paint_top(CRect c) {
  char filt[40], shown[40], right[48];
  int aw, rw, h = TOP_H - 1;
  describe_filter(filt, sizeof filt);
  if (G.status[0]) fit(0, G.status, c.w / 2, right, sizeof right);
  else api->fmt(right, sizeof right, "%s", G.local ? "on the card" : G.online ? "online" : "");
  rw = str_w(0, right, 99) + 12;
  aw = paint_tabs(c, h) + 8;
  api->fill(capp_rect(c.x + aw - 8, c.y, 8, h), L_BAR);
  fit(0, filt, c.w - aw - rw - 4, shown, sizeof shown);
  box_text(0, c.x + aw, c.y, c.w - aw - rw, h, 0, 0, shown, L_DIM, L_BAR);
  box_text(0, c.x + c.w - rw, c.y, rw, h, 0, 1, right, G.status[0] && G.bad ? L_BAD : L_DIM, L_BAR);
  api->fill(capp_rect(c.x, c.y + h, c.w, 1), L_RULE);
}

/* The 15-square box at a row's right end: its mark, composed with its background. */
static void paint_mark(int x, int y, const Entry *e, uint16_t bg) {
  uint16_t px[ROW_H * ROW_H];
  const uint16_t *bits = 0;
  uint16_t ink = K_CARD;
  int i, r, c;
  for (i = 0; i < ROW_H * ROW_H; i++) px[i] = bg;
  if (e->looked && e->mark == M_CARD) bits = MARK_CARD;
  else if (e->looked && e->mark == M_PLAY) { bits = MARK_PLAY; ink = K_PLAY; }
  else if (e->looked && e->mark == M_DONE) { bits = MARK_CHECK; ink = K_DONE; }
  else if (e->looked && e->mark == M_HELP) { bits = MARK_CHECK; ink = K_HELP; }
  if (bits)
    for (r = 0; r < 9; r++)
      for (c = 0; c < 9; c++)
        if ((bits[r] >> (8 - c)) & 1) px[(3 + r) * ROW_H + 3 + c] = ink;
  api->pixels(capp_rect(x, y, ROW_H, ROW_H), px);
}

static void paint_row(CRect c, int idx, int y) {
  const Entry *e = &G.page[idx];
  uint16_t bg = idx == G.sel ? L_SEL : L_BG, mark_ink = L_DIM;
  int wd_w = 34, date_w = 72, tx = c.x + wd_w + date_w, xr = c.x + c.w - ROW_H, title_w;
  char buf[TITLE_KEEP + 4], tbuf[16];
  int timed = e->looked && e->mark >= M_PLAY && (e->secs > 0 || e->mark != M_PLAY);

  box_text(1, c.x, y, wd_w, ROW_H, MARGIN, 0, e->wd < 7 ? xw_day_name(e->wd) : "", WD_COLOUR[e->wd < 7 ? e->wd : 7],
           bg);
  box_text(0, c.x + wd_w, y, date_w, ROW_H, 0, 0, e->date[0] ? e->date : e->id, L_TEXT, bg);
  title_w = xr - tx - (timed ? TIME_W : 0);
  fit(0, e->title, title_w - 8, buf, sizeof buf);
  box_text(0, tx, y, title_w, ROW_H, 0, 0, buf, L_DIM, bg);
  if (timed) {
    mark_ink = e->mark == M_PLAY ? K_PLAY : e->mark == M_DONE ? K_DONE : K_HELP;
    fmt_time(e->secs, tbuf, sizeof tbuf);
    box_text(0, xr - TIME_W, y, TIME_W, ROW_H, 0, 1, tbuf, mark_ink, bg);
  }
  paint_mark(xr, y, e, bg);
}

static void paint_empty(CRect c, int y) {
  const char *l1, *l2, *l3;
  int i, bottom = c.y + c.h - FOOT_H;
  if (G.net == NET_LIST) { l1 = "loading the list..."; l2 = ""; l3 = ""; }
  else if (G.local || !G.online) {
    l1 = "Nothing on the card yet.";
    l2 = "r asks the server; opening a puzzle";
    l3 = "keeps it here to play offline.";
  } else {
    l1 = "The server has no puzzles for this.";
    l2 = "a shows all; the library is filled by";
    l3 = "python -m server.crossword import";
  }
  for (i = 0; i < 3; i++, y += ROW_H)
    box_text(0, c.x, y, c.w, ROW_H, MARGIN, 0, i == 0 ? l1 : i == 1 ? l2 : l3, i == 0 ? L_TEXT : L_DIM, L_BG);
  if (bottom > y) api->fill(capp_rect(c.x, y, c.w, bottom - y), L_BG);
}

static void paint_list(CRect c) {
  int rows = list_rows(), i, y = c.y + TOP_H, bottom = c.y + c.h - FOOT_H;
  char foot[48];
  paint_top(c);
  if (!G.npage) {
    paint_empty(c, y);
  } else {
    for (i = 0; i < rows; i++, y += ROW_H) {
      int idx = G.top + i;
      if (idx < G.npage) paint_row(c, idx, y);
      else api->fill(capp_rect(c.x, y, c.w, ROW_H), L_BG);
    }
    if (bottom > y) api->fill(capp_rect(c.x, y, c.w, bottom - y), L_BG);
  }
  if (G.ask == ASK_FORGET) api->fmt(foot, sizeof foot, "forget your progress here? y/n");
  else if (G.prompting) api->fmt(foot, sizeof foot, "go to: %s_  enter go  esc", G.pbuf);
  else api->fmt(foot, sizeof foot, "enter open  tab kind  w day  s stats");
  footer_paint(api, c, foot);
}

/* ---- painting: the grid ----------------------------------------------------------------------------------------- */

/* One square, a cs by cs blit built here: its background (tinted if it is in
 * the word, brighter if it is the cursor's), the grid line along its top and
 * left, the ring if it is circled, its number where there is room for one
 * (cs >= CELL_NUM), and its letter, drawn bigger in the zoom view's big
 * squares. Static, since a zoomed square is 2 KB of pixels. */
static uint16_t s_px[CELL_ZOOM * CELL_ZOOM];

static void paint_cell(int idx, int x, int y, int cs) {
  uint16_t *px = s_px;
  const XwCell *q = &G.pz.cell[idx];
  int i, r, c, num, n = cs * cs;
  if (!q->sol) {
    for (i = 0; i < n; i++) px[i] = G_BLOCK;
  } else {
    uint16_t bg = idx == G.pz.cur ? G_CUR : xw_in_cur_word(&G.pz, idx) ? G_WORD : G_WHITE;
    int numbered = cs >= CELL_NUM, sc = cs >= 30 ? 3 : cs >= 22 ? 2 : 1, top, left;
    for (i = 0; i < n; i++) px[i] = bg;
    for (i = 0; i < cs; i++) { px[i] = G_LINE; px[i * cs] = G_LINE; }
    if (q->fl & XW_F_CIRCLE) {
      if (cs == CELL) {
        for (r = 0; r < 13; r++)
          for (c = 0; c < 13; c++)
            if ((RING[r] >> (12 - c)) & 1) px[(1 + r) * cs + 1 + c] = G_RING;
      } else {
        /* In doubled units, so the centre falls between pixels as the
         * square's inside does: a ring a pixel or so thick. */
        int ctr = cs + 1, ro = (cs - 1) * (cs - 1), ri = (cs - 4) * (cs - 4);
        for (r = 1; r < cs; r++)
          for (c = 1; c < cs; c++) {
            int dx = 2 * c + 1 - ctr, dy = 2 * r + 1 - ctr, d2 = dx * dx + dy * dy;
            if (d2 <= ro && d2 >= ri) px[r * cs + c] = G_RING;
          }
      }
    }
    num = numbered ? xw_number_at(&G.pz, idx) : 0;
    if (num) {
      char d[4];
      int nd = 0, k;
      if (num >= 100) d[nd++] = (char)(num / 100 % 10);
      if (num >= 10) d[nd++] = (char)(num / 10 % 10);
      d[nd++] = (char)(num % 10);
      for (k = 0; k < nd; k++)
        for (r = 0; r < 5; r++)
          for (c = 0; c < 3; c++)
            if ((DIGIT[(int)d[k]][r] >> (2 - c)) & 1 && 2 + k * 4 + c < cs) px[(1 + r) * cs + 2 + k * 4 + c] = G_NUM;
    }
    if (q->ent >= 'A' && q->ent <= 'Z') {
      uint16_t ink = (q->fl & XW_F_WRONG) ? G_WRONG : (q->fl & XW_F_REVEALED) ? G_GIVEN : G_INK;
      int lw = 5 * sc, lh = 7 * sc;
      top = numbered ? 6 + (cs - 6 - lh) / 2 : 1 + (cs - 1 - lh) / 2;
      left = (cs - lw + 1) / 2;
      for (r = 0; r < lh; r++)
        for (c = 0; c < lw; c++)
          if ((LETTER[q->ent - 'A'][r / sc] >> (4 - c / sc)) & 1) px[(top + r) * cs + left + c] = ink;
    }
  }
  api->pixels(capp_rect(x, y, cs, cs), px);
}

static void paint_cells(const Geo *g, CRect c) {
  CRect pa = api->paint_area();
  int r, col, cs = g->cs;
  if (pa.w <= 0 || pa.h <= 0) pa = c;
  for (r = 0; r < g->nr; r++) {
    int y = g->gy0 + r * cs;
    if (y + cs <= pa.y || y >= pa.y + pa.h) continue;
    for (col = 0; col < g->nc; col++) {
      int x = g->gx0 + col * cs;
      if (x + cs <= pa.x || x >= pa.x + pa.w) continue;
      paint_cell((G.oy + r) * G.pz.w + G.ox + col, x, y, cs);
    }
  }
}

/* A line of text cut to fit `w` with "..". */
static void cut_line(char *ln, int n, int w) {
  int maxw = w - str_w(0, "..", 2);
  while (n > 0 && str_w(0, ln, n) > maxw) n--;
  ln[n++] = '.';
  ln[n++] = '.';
  ln[n] = 0;
}

/* The clue bar: a rule, the label in bold and the clue's first line, then the
 * rest -- two lines in the big view, as many as it takes (up to CL_LINES) in
 * the zoom view; the last cut with ".." if there is still more. */
static void paint_bar(CRect c, const Geo *g) {
  char ln[220];
  int y = g->bar_y, k, n, lh = G.lh, w = c.w - 8, nl = G.lay == LAY_ZOOM ? G.cl_n : 2;
  int more = G.lay == LAY_ZOOM ? G.cl_cut3 : G.cl_cut;
  api->fill(capp_rect(c.x, y, c.w, 1), L_RULE);
  api->fill(capp_rect(c.x, y + 1, c.w, 1), B_BG);
  y += 2;
  box_text(1, c.x, y, 4 + G.cl_lw, lh, 4, 0, G.cl_label, B_LABEL, B_BG);
  for (k = 0; k < nl; k++) {
    n = G.cl_len[k] < (int)sizeof ln - 4 ? G.cl_len[k] : (int)sizeof ln - 4;
    api->mem_cpy(ln, G.cl_text + G.cl_off[k], (size_t)n);
    ln[n] = 0;
    if (k == nl - 1 && more)
      cut_line(ln, n, k == 0 ? w - G.cl_lw : w);
    if (k == 0) box_text(0, c.x + 4 + G.cl_lw, y, c.w - 4 - G.cl_lw, lh, 0, 0, ln, B_TEXT, B_BG);
    else box_text(0, c.x, y, c.w, lh, 4, 0, ln, B_TEXT, B_BG);
    y += lh;
  }
  api->fill(capp_rect(c.x, y, c.w, 2), B_BG);
}

/* Text wrapped down the side column from y, no lower than `bottom`; the last
 * line that fits is cut with ".." if there is more. Where the next line goes. */
static int column_text(const CRect *col, int y, int bottom, const char *s, uint16_t fg) {
  char ln[220];
  int p = 0, off, len, lh = G.lh, w = col->w - 8;
  while (s[p] && y + lh <= bottom) {
    int next = wrap_one(s, p, w, &off, &len), more;
    if (len > (int)sizeof ln - 4) len = (int)sizeof ln - 4;
    api->mem_cpy(ln, s + off, (size_t)len);
    ln[len] = 0;
    p = next;
    while (s[p] == ' ') p++;
    more = s[p] != 0;
    if (more && y + 2 * lh > bottom) cut_line(ln, len, w);
    box_text(0, col->x + 1, y, col->w - 1, lh, 4, 0, ln, fg, B_BG);
    y += lh;
  }
  return y;
}

/* The side view's column: a rule down its left, the clue's label in bold and
 * its words wrapped under it; then, dimmed, the clue that crosses at the
 * cursor, in what room is left. */
static void paint_column(const Geo *g) {
  const CRect *col = &g->col;
  int y = col->y + 3, bottom = col->y + col->h - 2, lh = G.lh, cross;
  char label[24];
  if (col->w <= 0) return;
  api->fill(capp_rect(col->x, col->y, 1, col->h), L_RULE);
  api->fill(capp_rect(col->x + 1, col->y, col->w - 1, 3), B_BG);
  if (G.cl_label[0]) {
    box_text(1, col->x + 1, y, col->w - 1, G.lhb, 4, 0, G.cl_label, B_LABEL, B_BG);
    y += G.lhb;
    y = column_text(col, y, bottom, G.cl_text, B_TEXT);
  }
  cross = xw_clue_at(&G.pz, G.pz.cur, 1 - G.pz.dir);
  if (cross >= 0 && y + 6 + G.lhb + lh <= bottom) {
    api->fill(capp_rect(col->x + 1, y, col->w - 1, 6), B_BG);
    y += 6;
    api->fmt(label, sizeof label, "%d-%s", G.pz.clue[cross].num,
             G.pz.clue[cross].dir == XW_ACROSS ? "Across" : "Down");
    box_text(1, col->x + 1, y, col->w - 1, G.lhb, 4, 0, label, L_DIM, B_BG);
    y += G.lhb;
    y = column_text(col, y, bottom, xw_clue_text(&G.pz, cross), L_DIM);
  }
  if (y < col->y + col->h) api->fill(capp_rect(col->x + 1, y, col->w - 1, col->y + col->h - y), B_BG);
}

/* The footer with something at each end: hints or a message on the left, the
 * time on the right. Like footer_paint, it fills round the text and not under
 * it, so a damaged repaint of just the time does not flash the hints. */
static void paint_foot(CRect c, const char *left, uint16_t lfg, const char *right) {
  int y = c.y + c.h - FOOT_H, rw = (int)api->str_len(right) * 6, lx = c.x + 4, rx = c.x + c.w - 4 - rw;
  int room = (rx - 6 - lx) / 6, lw;
  char tmp[48];
  if (room > (int)sizeof tmp - 1) room = (int)sizeof tmp - 1;
  if (room < 0) room = 0;
  api->fmt(tmp, sizeof tmp, "%s", left);
  if ((int)api->str_len(tmp) > room) tmp[room] = 0;
  lw = (int)api->str_len(tmp) * 6;
  api->fill(capp_rect(c.x, y, c.w, 2), FOOT_BG);
  api->fill(capp_rect(c.x, y + 10, c.w, FOOT_H - 10), FOOT_BG);
  api->fill(capp_rect(c.x, y + 2, 4, 8), FOOT_BG);
  api->fill(capp_rect(lx + lw, y + 2, rx - lx - lw, 8), FOOT_BG);
  api->fill(capp_rect(rx + rw, y + 2, c.x + c.w - rx - rw, 8), FOOT_BG);
  if (tmp[0]) api->text((int16_t)lx, (int16_t)(y + 2), tmp, lfg, FOOT_BG);
  if (right[0]) api->text((int16_t)rx, (int16_t)(y + 2), right, FOOT_FG, FOOT_BG);
}

/* What the grid's footer says: a question if one is up, else a message from a
 * check or reveal, else the keys; and the time. */
static void grid_foot(CRect c) {
  char t[16], right[24];
  const char *left = "^c check  ^r reveal  ^l view";
  uint16_t fg = FOOT_FG;
  if (G.ask == ASK_CHECK) left = "check: l letter  w word  p puzzle";
  else if (G.ask == ASK_REVEAL) left = "reveal: l letter  w word  p puzzle";
  else if (G.ask == ASK_REVEAL_ALL) left = "reveal the whole puzzle? y/n";
  else if (G.ask == ASK_RESET) left = "start this puzzle over? y/n";
  else if (G.msg[0]) { left = G.msg; fg = G.msg_bad ? CAPP_RGB(240, 150, 90) : CAPP_RGB(150, 200, 150); }
  else if (G.pz.solved) left = "enter results  esc list";
  right[0] = 0;
  if (!G.ask) {
    fmt_time(G.shown_secs, t, sizeof t);
    api->fmt(right, sizeof right, G.paused ? "|| %s" : "%s", t);
  }
  paint_foot(c, left, fg, right);
}

static void paint_grid(CRect c) {
  Geo g;
  int gw, gh;
  geometry(&g);
  gw = g.nc * g.cs + 1;
  gh = g.nr * g.cs + 1;
  if (g.gx0 + gw > g.area.x + g.area.w) gw = g.area.x + g.area.w - g.gx0;   /* zoom: the closing line is off the edge */
  /* The margins round the grid, in pieces: the squares are not painted over. */
  if (g.gy0 > g.area.y) api->fill(capp_rect(g.area.x, g.area.y, g.area.w, g.gy0 - g.area.y), G_MARGIN);
  if (g.gy0 + gh < g.area.y + g.area.h)
    api->fill(capp_rect(g.area.x, g.gy0 + gh, g.area.w, g.area.y + g.area.h - g.gy0 - gh), G_MARGIN);
  if (g.gx0 > g.area.x) api->fill(capp_rect(g.area.x, g.gy0, g.gx0 - g.area.x, gh), G_MARGIN);
  if (g.gx0 + gw < g.area.x + g.area.w)
    api->fill(capp_rect(g.gx0 + gw, g.gy0, g.area.x + g.area.w - g.gx0 - gw, gh), G_MARGIN);
  paint_cells(&g, c);
  if (g.gx0 + g.nc * g.cs < g.area.x + g.area.w) api->fill(capp_rect(g.gx0 + g.nc * g.cs, g.gy0, 1, gh), G_LINE);
  api->fill(capp_rect(g.gx0, g.gy0 + g.nr * g.cs, gw, 1), G_LINE);
  if (G.lay == LAY_SIDE) paint_column(&g);
  else paint_bar(c, &g);
  grid_foot(c);
}

/* ---- painting: the results and the statistics ------------------------------------------------------------------ */

static const char *const WD_FULL[7] = {
  "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday",
};

/* The bar across the top of a screen with two things on it. */
static void paint_top2(CRect c, const char *left, uint16_t lfg, const char *right, uint16_t rfg) {
  int h = TOP_H - 1, rw = str_w(0, right, 99) + 12;
  box_text(1, c.x, c.y, c.w - rw, h, MARGIN, 0, left, lfg, L_BAR);
  box_text(0, c.x + c.w - rw, c.y, rw, h, 0, 1, right, rfg, L_BAR);
  api->fill(capp_rect(c.x, c.y + h, c.w, 1), L_RULE);
}

/* A line of text across the screen, centred. */
static void centred_row(CRect c, int y, int h, const char *s, uint16_t fg) {
  box_text(0, c.x, y, c.w, h, centre_pad(G.f_ui, c.w, s), 0, s, fg, L_BG);
}

/* The results: how it ended, the time large, and what that did to the
 * weekday's record. Shown the moment a puzzle is finished, and again by Enter
 * on a finished one. */
static void paint_done(CRect c) {
  int kind = done_kind(), wd = xw_puzzle_weekday(&G.pz), wi, y = c.y + TOP_H, bh = G.lhn + 6, whites = 0;
  int rev = xw_reveals(&G.pz), bottom = c.y + c.h - FOOT_H;
  uint32_t secs = G.pz.ms / 1000u;
  uint16_t ink = kind == DK_CLEAN ? K_DONE : kind == DK_HELPED ? K_HELP : K_CARD;
  char date[12], top[32], t[16], a[16], b[48];
  xw_count(&G.pz, 0, &whites);
  wi = wd < 0 ? 0 : wd;
  if (G.pz.year) api->fmt(date, sizeof date, "%04d-%02d-%02d", G.pz.year, G.pz.month, G.pz.day);
  else date[0] = 0;
  api->fmt(top, sizeof top, "%s %s", wd >= 0 ? xw_day_name(wd) : "", date);
  paint_top2(c, kind == DK_CLEAN ? "Solved!" : kind == DK_HELPED ? "Solved with help" : "Revealed", ink, top, L_DIM);

  api->fill(capp_rect(c.x, y, c.w, 6), L_BG);
  y += 6;
  fmt_time(secs, t, sizeof t);
  box_f(G.f_num, G.lhn, c.x, y, c.w, bh, centre_pad(G.f_num, c.w, t), 0, t, ink, L_BG);
  y += bh;
  api->fill(capp_rect(c.x, y, c.w, 4), L_BG);
  y += 4;

  if (kind == DK_CLEAN) {
    uint32_t best = G.st.best[wi];
    fmt_time(best, a, sizeof a);
    if (G.done_new == XWS_FIRST) api->fmt(b, sizeof b, "your first %s solve!", WD_FULL[wi]);
    else if (G.done_new == XWS_BEST) api->fmt(b, sizeof b, "a new best for %ss!", WD_FULL[wi]);
    else api->fmt(b, sizeof b, "best %s: %s", WD_FULL[wi], best ? a : "-");
    centred_row(c, y, ROW_H, b, G.done_new != XWS_NOT_BEST ? K_DONE : L_TEXT);
    y += ROW_H;
    fmt_time(xws_avg(&G.st, wi), a, sizeof a);
    api->fmt(b, sizeof b, "average %s over %d", G.st.solved[wi] ? a : "-", G.st.solved[wi]);
    centred_row(c, y, ROW_H, b, L_DIM);
    y += ROW_H;
    {
      int streak = xws_streak_now(&G.st, G.today);
      if (streak > 1) api->fmt(b, sizeof b, "%d days in a row", streak);
      else api->fmt(b, sizeof b, "%d solved in all", xws_solved_all(&G.st));
    }
    centred_row(c, y, ROW_H, b, L_DIM);
    y += ROW_H;
  } else if (kind == DK_HELPED) {
    api->fmt(b, sizeof b, "%d letters were revealed", rev);
    centred_row(c, y, ROW_H, b, L_TEXT);
    y += ROW_H;
    centred_row(c, y, ROW_H, "so this one sets no best time", L_DIM);
    y += ROW_H;
    api->fmt(b, sizeof b, "%d solved with help", G.st.helped);
    centred_row(c, y, ROW_H, b, L_DIM);
    y += ROW_H;
  } else {
    api->fmt(b, sizeof b, "%d of %d letters were revealed", rev, whites);
    centred_row(c, y, ROW_H, b, L_TEXT);
    y += ROW_H;
    centred_row(c, y, ROW_H, "so nothing is counted", L_DIM);
    y += ROW_H;
    centred_row(c, y, ROW_H, "ctrl-n starts it over", L_DIM);
    y += ROW_H;
  }
  if (bottom > y) api->fill(capp_rect(c.x, y, c.w, bottom - y), L_BG);
  footer_paint(api, c, "enter grid  tab stats  esc list");
}

/* The statistics: a row a weekday, then the totals. */
static void paint_stats(CRect c) {
  int rh = (c.h - TOP_H - FOOT_H) / 8, i, y = c.y + TOP_H, bottom = c.y + c.h - FOOT_H;
  int streak = xws_streak_now(&G.st, G.today);
  char a[40], b[24], right[40];
  if (rh < 12) rh = 12;
  api->fmt(right, sizeof right, "streak %d (best %d)", streak, G.st.best_streak);
  paint_top2(c, "Statistics", L_TEXT, right, L_DIM);
  for (i = 0; i < 7; i++, y += rh) {
    box_text(1, c.x, y, 44, rh, MARGIN, 0, xw_day_name(i), WD_COLOUR[i], L_BG);
    api->fmt(a, sizeof a, "%d solved", G.st.solved[i]);
    box_text(0, c.x + 44, y, 62, rh, 2, 0, a, L_TEXT, L_BG);
    if (G.st.best[i]) { fmt_time(G.st.best[i], b, sizeof b); api->fmt(a, sizeof a, "best %s", b); }
    else api->fmt(a, sizeof a, "best -");
    box_text(0, c.x + 106, y, 66, rh, 2, 0, a, L_TEXT, L_BG);
    if (G.st.solved[i]) { fmt_time(xws_avg(&G.st, i), b, sizeof b); api->fmt(a, sizeof a, "avg %s", b); }
    else api->fmt(a, sizeof a, "avg -");
    box_text(0, c.x + 172, y, c.w - 172, rh, 2, 0, a, L_TEXT, L_BG);
  }
  box_text(1, c.x, y, 44, rh, MARGIN, 0, "All", L_TEXT, L_BG);
  api->fmt(a, sizeof a, "%d solved, %d with help", xws_solved_all(&G.st), G.st.helped);
  box_text(0, c.x + 44, y, c.w - 44, rh, 2, 0, a, L_DIM, L_BG);
  y += rh;
  if (bottom > y) api->fill(capp_rect(c.x, y, c.w, bottom - y), L_BG);
  footer_paint(api, c, "esc back");
}

static void app_paint(void *st, CRect c) {
  (void)st;
  G.at = c;
  G.have_at = 1;
  if (G.view == V_STATS) paint_stats(c);
  else if (G.view == V_DONE && G.have_pz) paint_done(c);
  else if (G.view == V_GRID && G.have_pz) paint_grid(c);
  else paint_list(c);
}

/* ---- keys ---------------------------------------------------------------------------------------------------------- */

/* Mark a list row for repair. */
static void damage_row(int idx) {
  if (!G.have_at || idx < G.top || idx >= G.top + list_rows()) return;
  damage_rect(G.at.x, G.at.y + TOP_H + (idx - G.top) * ROW_H, G.at.w, ROW_H);
}

/* The selection onto row `to` of the window. 1 if anything changed. */
static int select_row(int to) {
  int old = G.sel;
  if (to < 0) to = 0;
  if (to >= G.npage) to = G.npage - 1;
  if (to == old || to < 0) return 0;
  G.sel = to;
  if (fix_top()) damage_all();
  else { damage_row(old); damage_row(to); }
  return 1;
}

/* Up or down by `d` rows. Past the window's end, the next page of the list. */
static int move_sel(int d) {
  if (!G.npage) return 0;
  if (d > 0 && G.sel >= G.npage - 1) {
    if (G.more_older && G.net == NET_IDLE) { list_request(GO_OLDER); return 1; }
    return 0;
  }
  if (d < 0 && G.sel <= 0) {
    if (G.more_newer && G.net == NET_IDLE) { list_request(GO_NEWER); return 1; }
    return 0;
  }
  return select_row(G.sel + d);
}

/* d on the list: forget what was saved of the puzzle, after y/n. */
static void do_forget(void) {
  char path[48];
  Entry *e;
  if (G.sel < 0 || G.sel >= G.npage) return;
  e = &G.page[G.sel];
  save_path(path, sizeof path, e->id);
  api->remove(path);
  lookup_one(e);
  damage_row(G.sel);
  say("progress forgotten", 0);
}

static void ask_begin(int what) {
  G.ask = what;
  damage_foot();
}

static void end_ask(void) {
  G.ask = ASK_NONE;
  damage_foot();
}

/* A question is up in the footer. Check and reveal ask how much -- l, w or p,
 * Esc to cancel -- and the rest ask y/n (apps/confirm.h: y is yes; n, Esc and
 * Backspace are no; anything else waits). */
static int key_ask(uint8_t k) {
  if (G.ask == ASK_CHECK || G.ask == ASK_REVEAL) {
    int reveal = G.ask == ASK_REVEAL;
    if (k == CAPP_KEY_ESC) { end_ask(); return 1; }
    if (api->key_repeat()) return 1;
    if (k == 'l' || k == 'L') { end_ask(); do_scope(reveal, XW_SCOPE_CELL); return 1; }
    if (k == 'w' || k == 'W') { end_ask(); do_scope(reveal, XW_SCOPE_WORD); return 1; }
    if (k == 'p' || k == 'P') {
      if (reveal) ask_begin(ASK_REVEAL_ALL);            /* giving the puzzle away is asked twice */
      else { end_ask(); do_scope(0, XW_SCOPE_ALL); }
      return 1;
    }
    return 1;
  }
  {
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) {
      int what = G.ask;
      end_ask();
      if (what == ASK_REVEAL_ALL) do_scope(1, XW_SCOPE_ALL);
      else if (what == ASK_RESET) do_reset();
      else if (what == ASK_FORGET) do_forget();
    } else if (a != CONFIRM_WAIT) {
      end_ask();
    }
    return 1;
  }
}

static int key_prompt(uint8_t k) {
  if (k == CAPP_KEY_ESC) { G.prompting = 0; damage_foot(); return 1; }
  if (k == CAPP_KEY_ENTER) {
    char to[11];
    if (api->key_repeat()) return 1;
    to[0] = 0;
    if (G.plen && !normalize_to(G.pbuf, to)) {
      say("a date: 1993, 1993-11 or 1993-11-21", 1);
      return 1;
    }
    G.prompting = 0;
    api->fmt(G.qto, sizeof G.qto, "%s", to);
    reload_first();
    return 1;
  }
  if (k == CAPP_KEY_BACK) {
    if (G.plen) G.pbuf[--G.plen] = 0;
    damage_foot();
    return 1;
  }
  if (((k >= '0' && k <= '9') || k == '-') && G.plen < 10) {
    G.pbuf[G.plen++] = (char)k;
    G.pbuf[G.plen] = 0;
    damage_foot();
  }
  return 1;
}

static int key_list(uint8_t k) {
  if (G.ask) return key_ask(k);
  if (G.prompting) return key_prompt(k);
  unsay();
  switch (k) {
  case CAPP_KEY_UP:    return move_sel(-1);
  case CAPP_KEY_DOWN:  return move_sel(1);
  case CAPP_KEY_LEFT:  return move_sel(-list_rows());
  case CAPP_KEY_RIGHT: return move_sel(list_rows());
  case CAPP_KEY_ENTER:
    if (!api->key_repeat()) open_entry(G.sel);
    return 1;
  case '\t':                                    /* the next tab: Full, Midi, Themed */
    if (api->key_repeat()) return 1;
    G.kind = (G.kind + 1) % KIND_N;
    view_save();
    reload_first();
    return 1;
  case 's': case 'S':
    if (api->key_repeat()) return 1;
    G.stats_from = V_LIST;
    G.today = today_day();
    G.view = V_STATS;
    damage_all();
    return 1;
  case 'd': case 'D':
    if (api->key_repeat()) return 1;
    if (G.npage && G.page[G.sel].mark >= M_PLAY) ask_begin(ASK_FORGET);
    else say("nothing saved for that puzzle", 0);
    return 1;
  case 'g': case 'G':
    G.prompting = 1;
    G.plen = 0;
    G.pbuf[0] = 0;
    damage_foot();
    return 1;
  case 'w':                                     /* all, Monday, Tuesday ... Sunday, all */
    G.qwd = G.qwd == 7 ? 0 : G.qwd == 6 ? 7 : G.qwd + 1;
    reload_first();
    return 1;
  case 'W':                                     /* and the other way round */
    G.qwd = G.qwd == 7 ? 6 : G.qwd == 0 ? 7 : G.qwd - 1;
    reload_first();
    return 1;
  case 'a': case 'A':
    G.qwd = 7;
    G.qto[0] = 0;
    reload_first();
    return 1;
  case 'r': case 'R':
    if (api->key_repeat()) return 1;
    G.online = 1;
    reload_first();
    return 1;
  case 'o': case 'O':
    if (api->key_repeat()) return 1;
    G.online = !G.online;
    reload_first();
    return 1;
  }
  return 0;
}

/* The time in the footer's right end. */
static void damage_time(void) {
  if (G.have_at) damage_rect(G.at.x + G.at.w - 72, G.at.y + G.at.h - FOOT_H, 72, FOOT_H);
}

/* A key that was taken and changed nothing: a letter into a finished puzzle,
 * a turn with nowhere to turn. Handled -- declining would hand it on -- but
 * with no mark the whole screen would be repainted for it, so the corner of
 * it is. */
static int nothing(void) {
  damage_rect(G.at.x, G.at.y, 1, 1);
  return 1;
}

/* The cursor has moved, or turned, or typed on: mark the word it left, the
 * word it is in, and the clue bar if the clue changed. A scroll is the whole
 * grid. */
static void refresh_cursor(int old) {
  int ci = xw_cur_clue(&G.pz), cross = xw_clue_at(&G.pz, G.pz.cur, 1 - G.pz.dir), lines = G.cl_n;
  if (ci != old) clue_layout();
  /* Zoom's bar is as tall as the clue, so a clue of another height moves
   * the grid: that is a whole repaint, and the view may have to scroll. */
  if (G.lay == LAY_ZOOM && G.cl_n != lines) {
    scroll_to_cursor();
    damage_all();
  } else if (scroll_to_cursor()) {
    damage_all();
  } else {
    damage_word(old);
    damage_word(ci);
  }
  /* The side column shows the crossing clue too, which changes along a word. */
  if (ci != old || (G.lay == LAY_SIDE && cross != G.cross_ci)) damage_bar();
  G.cross_ci = cross;
}

/* ctrl-l: the next view. The view is kept for next time. */
static void next_layout(void) {
  G.lay = (G.lay + 1) % LAY_N;
  G.ox = G.oy = 0;
  clue_layout();                                /* first: the zoom view's bar is as tall as the clue */
  scroll_to_cursor();
  view_save();
  damage_all();
}

/* An arrow in the grid: the cursor moves or turns. */
static int move_cursor(int dr, int dc) {
  int old = xw_cur_clue(&G.pz);
  if (!xw_arrow(&G.pz, dr, dc)) return 0;
  refresh_cursor(old);
  return 1;
}

/* Tab, enter, space: what xwcore answered, shown. */
static int jump(int r, int old) {
  if (!r) return nothing();
  refresh_cursor(old);
  return 1;
}

/* A letter typed, or backspace: what xwcore answered, shown. A win is
 * handled last, since it replaces the screen. */
static int edited(int r, int old) {
  if (!r) return nothing();
  if (r & XW_R_CELL) G.dirty = 1;
  refresh_cursor(old);
  if (r & XW_R_WON) on_won();
  return 1;
}

/* Any key is someone there: the idle time starts again, and a clock stopped
 * for want of one runs on. */
static void note_activity(uint32_t now) {
  G.last_key = now;
  if (G.paused) {
    G.paused = 0;
    xw_clock_start(&G.pz, now);
    damage_foot();
  }
}

static int key_grid(uint8_t k) {
  int old;
  note_activity(api->ticks_ms());
  if (G.ask) return key_ask(k);
  old = xw_cur_clue(&G.pz);
  switch (k) {
  case CAPP_KEY_UP:    return move_cursor(-1, 0);
  case CAPP_KEY_DOWN:  return move_cursor(1, 0);
  case CAPP_KEY_LEFT:  return move_cursor(0, -1);
  case CAPP_KEY_RIGHT: return move_cursor(0, 1);
  case CAPP_KEY_ESC:   leave_grid(); return 1;
  case CAPP_KEY_ENTER:
    if (api->key_repeat()) return 1;
    if (G.pz.solved) { G.today = today_day(); G.view = V_DONE; damage_all(); return 1; }
    return jump(xw_next_word(&G.pz, 1), old);
  case '\t':                                        /* tab turns, as space does (the owner's choice) */
  case ' ':
    if (api->key_repeat()) return 1;
    return jump(xw_toggle(&G.pz), old);
  case CAPP_KEY_BACK:
    return edited(xw_backspace(&G.pz), old);
  }
  if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z')) {
    if (api->key_repeat()) return 1;                    /* a held letter is not a row of them */
    return edited(xw_type(&G.pz, k), old);
  }
  return 0;
}

/* The results: Enter goes to the grid as it was left (a finished puzzle's,
 * locked), Tab to the statistics. */
static int key_done(uint8_t k) {
  if (api->key_repeat()) return 1;
  if (k == CAPP_KEY_ENTER) { G.view = V_GRID; damage_all(); return 1; }
  if (k == '\t') { G.stats_from = V_DONE; G.today = today_day(); G.view = V_STATS; damage_all(); return 1; }
  if (k == CAPP_KEY_ESC) { leave_grid(); return 1; }
  return 0;
}

static int key_stats(uint8_t k) {
  if (api->key_repeat()) return 1;
  if (k == CAPP_KEY_ESC || k == '\t') {
    G.view = G.stats_from;
    damage_all();
    return 1;
  }
  return 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (G.view == V_STATS) return key_stats(k);
  if (G.view == V_DONE && G.have_pz) return key_done(k);
  if (G.view == V_GRID && G.have_pz) return key_grid(k);
  return key_list(k);
}

/* Letters are typed in the grid, and ; . , / are arrows there without fn
 * (they are never needed as text): CAPP_TEXT_ARROWS, which a kernel that does
 * not know it reads as plain yes. The date box on the list takes text. */
static int app_wants_text(void *st) {
  (void)st;
  if (G.view == V_LIST) return G.prompting;
  if (G.view == V_GRID) return G.have_pz && !G.pz.solved && !G.ask ? CAPP_TEXT_ARROWS : 0;
  return 0;
}

/* ---- the clock: what the grid does by itself, a request to collect, a download, a row to look up ------------------------ */

/* The grid, a tick: a message that has had its time goes; a clock with no key
 * for IDLE_MS stops as of the last key, and is written; the footer's time is
 * marked when the second changes; and a changed grid is written every SAVE_MS.
 * It takes its own time from ticks_ms, as every handler does, so the clock's
 * start and every later reading come from one source. */
static int grid_tick(void) {
  uint32_t t = api->ticks_ms();
  int changed = 0;
  if (G.msg[0] && (int32_t)(t - G.msg_until) >= 0) {
    G.msg[0] = 0;
    damage_foot();
    changed = 1;
  }
  if (G.pz.solved) return changed;
  if (!G.paused && G.pz.running && (int32_t)(t - G.last_key) >= IDLE_MS) {
    clock_stop_at(G.last_key);
    G.paused = 1;
    G.shown_secs = elapsed_s(t);
    save_progress();
    damage_foot();
    return 1;
  }
  if (!G.paused) {
    uint32_t s = elapsed_s(t);
    if (s != G.shown_secs) {
      G.shown_secs = s;
      damage_time();
      changed = 1;
    }
  }
  if (G.dirty && (int32_t)(t - G.last_save) >= SAVE_MS) save_progress();
  return changed;
}

static int app_tick(void *st, uint32_t now) {
  int changed = 0;
  (void)st; (void)now;
  if (G.view == V_GRID && G.have_pz && grid_tick()) changed = 1;
  if (G.net == NET_LIST && poll_list()) changed = 1;
  if (G.dl && G.net == NET_IDLE) {
    do_download();
    changed = 1;
  }
  if (G.view == V_LIST && G.look < G.npage && !G.dl) {
    int i = G.look++;
    if (lookup_one(&G.page[i])) {
      damage_row(i);
      changed = 1;
    }
  }
  return changed;
}

/* ---- actions: the same things from a chord or from fn-space -------------------------------------------------------------------- */

enum { ACT_CHECK = 1, ACT_CHECK_LETTER, ACT_CHECK_WORD, ACT_CHECK_ALL,
       ACT_REVEAL, ACT_REVEAL_LETTER, ACT_REVEAL_WORD, ACT_REVEAL_ALL, ACT_RESET, ACT_LAYOUT };

/* Letters are typed in the grid, so check and reveal are ctrl chords -- c and
 * h cannot be both -- that ask how much; fn-space has every size outright.
 * ctrl-h is Backspace's byte, so reveal is ctrl-r. */
static const CappAction ACTIONS[] = {
  { "check",         "Check...",      "Puzzle", 0x03, ACT_CHECK },           /* ctrl-c */
  { "check.letter",  "Check letter",  "Puzzle", 0, ACT_CHECK_LETTER },
  { "check.word",    "Check word",    "Puzzle", 0, ACT_CHECK_WORD },
  { "check.puzzle",  "Check puzzle",  "Puzzle", 0, ACT_CHECK_ALL },
  { "reveal",        "Reveal...",     "Puzzle", 0x12, ACT_REVEAL },          /* ctrl-r */
  { "reveal.letter", "Reveal letter", "Puzzle", 0, ACT_REVEAL_LETTER },
  { "reveal.word",   "Reveal word",   "Puzzle", 0, ACT_REVEAL_WORD },
  { "reveal.puzzle", "Reveal puzzle", "Puzzle", 0, ACT_REVEAL_ALL },
  { "reset",         "Start over",    "Puzzle", 0x0E, ACT_RESET },           /* ctrl-n */
  { "layout",        "Next view",     "View",   0x0C, ACT_LAYOUT },          /* ctrl-l: side, big, zoom */
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int app_action(void *st, int a) {
  (void)st;
  if (G.view == V_DONE && a == ACT_RESET) { G.view = V_GRID; damage_all(); }   /* from the results */
  if (G.view != V_GRID || !G.have_pz) return 0;      /* only a puzzle on screen has these */
  note_activity(api->ticks_ms());
  if (G.ask) end_ask();
  if (a == ACT_LAYOUT) { next_layout(); return 1; }  /* a solved grid has views too */
  if (a == ACT_RESET) { ask_begin(ASK_RESET); return 1; }
  if (G.pz.solved) { say_msg("already solved", 0); return 1; }
  switch (a) {
  case ACT_CHECK:         ask_begin(ASK_CHECK); break;
  case ACT_REVEAL:        ask_begin(ASK_REVEAL); break;
  case ACT_CHECK_LETTER:  do_scope(0, XW_SCOPE_CELL); break;
  case ACT_CHECK_WORD:    do_scope(0, XW_SCOPE_WORD); break;
  case ACT_CHECK_ALL:     do_scope(0, XW_SCOPE_ALL); break;
  case ACT_REVEAL_LETTER: do_scope(1, XW_SCOPE_CELL); break;
  case ACT_REVEAL_WORD:   do_scope(1, XW_SCOPE_WORD); break;
  case ACT_REVEAL_ALL:    ask_begin(ASK_REVEAL_ALL); break;
  default: return 0;
  }
  return 1;
}

/* ---- the app ------------------------------------------------------------------------------------------------------------- */

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Crossword",
  /* 16x16: a five-by-five grid with a few black squares. */
  { 0xFF, 0xFF, 0x93, 0xC9, 0x93, 0xC9, 0xFF, 0xFF,
    0xF2, 0x49, 0xF2, 0x49, 0xFF, 0xFF, 0x93, 0xC9,
    0x93, 0xC9, 0xFF, 0xFF, 0x92, 0x4F, 0x92, 0x4F,
    0xFF, 0xFF, 0x93, 0xC9, 0x93, 0xC9, 0xFF, 0xFF },
  "enter\topen a puzzle (fetched if not on the card)\n"
  "up/down\tchoose; past the end, the next page\n"
  "left/right\ta screenful up or down\n"
  "g\tgo to a date: 1993, 1993-11, 1993-11-21\n"
  "w\tone weekday, then the next, round to all\n"
  "a\tall puzzles again\n"
  "r\task the server again\n"
  "o\tonly the card's puzzles, or the server's\n"
  "d\tforget what you did on a puzzle\n"
  "tab\tthe next tab: Full, Midi, Themed\n"
  "s\tyour statistics\n"
  "\tin a puzzle:\n"
  "letters\ttype; bksp clears or steps back\n"
  "arrows\tmove; they never turn\n"
  "space\tturn across or down (tab too)\n"
  "enter\tthe next word\n"
  "ctrl-l\tthe next view: side, big, zoom\n"
  "ctrl-c\tcheck a letter, word or puzzle\n"
  "ctrl-r\treveal one: no best time then\n"
  "ctrl-n\tstart the puzzle over\n"
  "esc\tback to the list\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  G.f_ui = api->font_load("ui13");
  G.f_uib = api->font_load("ui13b");
  G.f_num = api->font_load("num30");
  measure_fonts();
  stats_load();
  view_load();
  G.view = V_LIST;
  G.qwd = 7;
  G.online = (api->caps_ok() & CAPP_CAP_PROXY) != 0;
  list_request(GO_FIRST);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
