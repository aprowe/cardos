/* Pain: a log of when it hurts, how much, and where.
 *
 * THE TODAY SCREEN is the one the app opens on, and it is made to be used in
 * a bad moment: one hand, a few keys, nothing to read. A large level from 0
 * to 10, set in clock56 and coloured from green through amber to red, with a
 * word under it (none, mild, moderate, bad, worst). Up and down change the
 * level. Tab, or left and right, chooses where it hurts -- cramps, back, head,
 * other -- shown as a name with four dots under it. Enter logs it, with the
 * date and time from
 * the clock, and says so in the footer. h opens the history, t the trends.
 *
 * On the right, what has been logged today, newest first: a bar in the
 * entry's colour, the time, the level, the place. More than fit are counted
 * ("+3 earlier") rather than scrolled, because up and down are the level
 * here. Under it, the last entry ever logged, whenever it was, so a person
 * who opens the app on a fresh day can see where yesterday ended. The level
 * starts at the last one logged today, else 5.
 *
 * THE HISTORY is every entry, newest first, grouped under a heading for each
 * day (Today, Yesterday, Mon 20 Oct, and the year once it is not this one;
 * "No date" for what was logged with no clock). An entry row has its time, a
 * bar as long as its level in the level's colour, the level, the place and as
 * much of the note as fits; the strip under the list shows the selected
 * entry's whole note. Up and down move the selection an entry at a time,
 * left and right a day (so a month back is a handful of presses); e or Enter
 * edits the note, d or Del deletes after asking (y yes; n, Esc, Backspace
 * no), Esc goes back to Today. The note is typed on a screen of its own: a
 * field, a count of 31, Enter saves, Del clears it, Esc keeps the old one.
 * The list is not stored anywhere: a row is found by walking the log, which
 * is sorted by day (apps/pain_log.h), so a change to the log cannot leave the
 * list out of step with it. Each key marks only the rows it changed.
 *
 * THE TRENDS are the last 14 days as a bar chart: a column a day, as tall as
 * that day's worst level and in its colour, a faint line at 7 where a bad day
 * begins, the day of the month under each column (today's in teal, a day with
 * nothing logged dimmer). Above it three figures for the days on screen: the
 * average level of what was logged, the worst level and the day it was on,
 * and how many days reached 7 or more. Left and right (up and down too) page
 * a week back and forward, back as far as the oldest entry and forward to
 * today. Nothing logged at all is said in words instead; a stretch with
 * nothing in it says so over an empty chart. h and t move between the
 * history and the trends, Esc goes back to Today.
 *
 * p, or fn-p, prints a summary for a doctor on the thermal printer, from
 * Today, History or Trends: the days on the chart (average, worst, bad
 * days), the whole log in three lines, and each logged day with its worst
 * level and where it hurt. Plain words, no pictures; the numbers are the
 * same ones the screen shows, taken from the same place (trends_refresh).
 *
 * KEYS are the ones every app speaks (CLAUDE.md, "Every app speaks the same
 * keys"), and mean the same on every screen they can:
 *
 *   Enter   the screen's main action: log (Today), edit the note (History)
 *   Esc     back one level: Note to wherever it came from, History and Trends
 *           to Today; Today keeps it
 *   n       a new entry: logs the level on screen (Today), goes to Today to
 *           choose one (History, Trends)
 *   e       edit the note: the selected entry's (History), the last one
 *           logged today (Today)
 *   d, Del  delete, after asking: y yes; n, Esc, Backspace no
 *   p       print the doctor's summary (fn-p too)
 *   arrows  move: the level and the place (Today), an entry and a day
 *           (History), a week (Trends)
 *
 * and there is nothing else: h, t and Tab are Pain's own. Every action is in
 * one table (ACTIONS), which gives the OS the fn-p chord, the fn-space
 * palette and the help panel, and the menu bar (fn-b, or a mouse) its menus.
 * The bar is given only the actions that mean something on the screen it is
 * over (set_menu): Delete is not offered where there is nothing to delete.
 * Whatever reaches an action that does not apply, from the palette, is
 * ignored or said in the footer, never an error. The footer (hint) is key
 * and word pairs, as every app's is: what is needed in the moment, the rest
 * in the help (fn-h).
 *
 * NO CLOCK. A device that has never been on a network and was not told the
 * date cannot say what day it is. Pain still logs: the entry has no date, and
 * is known by a number instead -- #12, the twelfth thing ever logged. Today's
 * list is then headed "Latest" and shows the last entries of any day, the
 * header says "no clock", and when the clock arrives while the app is open
 * the screen moves to "Today" by itself (tick looks every five seconds). The
 * trends have no days to chart, so they chart the last 14 entries instead,
 * each column labelled with its level, and "a week back" is seven entries.
 *
 * THE CARD. The entries are /var/pain/log.txt, written whole through
 * apps/safefile.h after every change, so a device pulled from a pocket keeps
 * what was done a moment before. apps/pain_log.h is the log itself -- the
 * line format, the order, what happens to a log bigger than memory -- and
 * has nothing of the screen in it.
 *
 * REPAINTING. Every key marks only what it changed with api->damage; a change
 * of screen marks the whole thing (repaint_all). Every region paints its
 * whole rectangle exactly once (put() fills the margins round its text
 * instead of clearing first), so a repaint of only the number never shows the
 * number blanked. Paint reads the state and nothing else: the clock is read
 * by tick and by Enter, never by paint, which runs once per strip.
 *
 * Text is Atkinson Hyperlegible at 13 px (ui13, ui13b) and the level in
 * clock56, falling back to num30 if the column is too short and to the 6x8
 * font if the card has no fonts. The colours are the dark ones Counter and
 * Habits use.
 */

#include "kernel/app/capp.h"
#include "apps/datetime.h"
#include "apps/str.h"
#include "apps/toolbar.h"
#include "apps/footer.h"
#include "apps/confirm.h"
#include "apps/pain_log.h"

static const CardApi *api;

#define HEAD_H      18        /* the title strip */
#define LEFT_W      104       /* Today: the level's column */
#define ROW_H       17        /* a row of a list, Today's header */
#define CAP_H       18        /* Today: the last-logged line; History: the note under the list */
#define GAP_TOP     4         /* Today: least space above the number, and the others in the column */
#define GAP_NUM     2
#define GAP_WORD    6
#define DOT_BAND    8         /* the place's four dots: 2 above, 4 high, 2 below */
#define DOT         4
#define DOT_GAP     6
#define BAR_UNIT    5         /* History: pixels of bar a level is worth */
#define BAR_W       (PAIN_LEVEL_MAX * BAR_UNIT)
#define BAR_H       7
#define FIELD_H     24        /* the note's field */
#define TILE_H      40        /* Trends: a figure's label (16) and its number (24) */
#define COL_W       17        /* Trends: a column of the chart; 14 of them fill the width */
#define COL_PAD     3         /* ... and the space either side of its bar */
#define BARS_H      44        /* ... the chart at most: room for level 10, a baseline under it */
#define LAB_H       12        /* ... the day numbers under it */
#define SUMMARY_MAX 1536      /* the printed summary */

#define CLR_BG      CAPP_RGB(14, 16, 22)
#define CLR_HEAD    CAPP_RGB(26, 30, 40)
#define CLR_TEXT    CAPP_RGB(238, 241, 247)
#define CLR_DIM     CAPP_RGB(128, 136, 150)
#define CLR_FAINT   CAPP_RGB(70, 76, 90)
#define CLR_ACCENT  CAPP_RGB(96, 214, 198)
#define CLR_ROW     CAPP_RGB(20, 23, 31)
#define CLR_ROW_ALT CAPP_RGB(25, 29, 38)
#define CLR_SEL     CAPP_RGB(30, 44, 66)
#define CLR_TRACK   CAPP_RGB(36, 40, 52)
#define CLR_FIELD   CAPP_RGB(28, 32, 42)

enum { AL_LEFT = 0, AL_CENTER, AL_RIGHT };
enum { VIEW_TODAY = 0, VIEW_HISTORY, VIEW_NOTE, VIEW_TRENDS };

static struct {
  PainLog  log;

  int      view;
  int      level;            /* Today: what Enter would log */
  int      area;             /* PAIN_CRAMPS .. PAIN_OTHER */

  int32_t  today;            /* a day number; PAIN_NO_DAY with no clock */
  int      minute;           /* minutes since midnight now, -1 with no clock */
  uint32_t clock_at;         /* when tick last looked at the clock */

  int      show_first;       /* Today's list: log.e[show_first .. +show_n), shown newest first */
  int      show_n;

  int      sel;              /* History: the selected entry, an index into log.e; -1 for none */
  int      top;              /* History: the row at the top of the list */
  int      ask;              /* the delete question is up (for P.sel) */
  char     draft[PAIN_NOTE_MAX + 1];    /* the note being typed */
  int      draft_len;
  int      note_back;        /* the screen the note goes back to */

  struct {                   /* Trends: the figures for the columns on screen */
    int     off;             /* how far back: days, or with no clock entries; steps of 7 */
    int     counted;         /* the columns are entries, not days (no clock) */
    int32_t first, last;     /* the days of the first and last column */
    int8_t  v[PAIN_CHART_DAYS];       /* each column's worst level, -1 for nothing */
    uint8_t dom[PAIN_CHART_DAYS];     /* each column's day of the month */
    int     n;               /* entries in the columns */
    int     avg10;           /* their average level in tenths */
    int     worst;           /* the worst level, -1 for none */
    int32_t worst_day;       /* the day of it, or ... */
    uint32_t worst_seq;      /* ... with no clock, its number */
    int     days;            /* days with an entry (entries, with no clock) */
    int     bad;             /* of them, how many reached PAIN_BAD */
  } tr;

  const char *say;          /* a word in the footer until the next key */
  char     say_buf[40];

  int      f_clock, f_num, f_ui, f_uib;     /* font handles; -1 is the 6x8 font */
  int      f_shown;                         /* the one the level is set in */

  int      laid;             /* paint has run, so the rectangles below are good */
  CRect    clip;             /* what this paint is asked to repair */
  struct {                   /* every screen */
    CRect full;              /* all of the app's rectangle, menu bar too */
    CRect all, head, htitle, hright, foot;   /* ... and the part under the bar */
    /* Today */
    CRect sep;
    CRect num, word, area;   /* the level's column ... */
    CRect rhead, list, cap;  /* ... and the list's */
    CRect gap[5];            /* what is left between them: 0-3 the column's, 4 the list's */
    int   rows;              /* rows of the list */
  } lay;
  struct {                   /* History */
    CRect list, gap, det;
    int   vrows;
  } hl;
  struct {                   /* the note */
    CRect g0, info, g1, field, g2, hint, count, g3;
  } nt;
  struct {                   /* Trends */
    CRect body, tile[3], g1, bars, labels, g2;
    int   x0;                /* where the first column starts */
    int   unit;              /* pixels of bar a level is worth */
  } tl;
} P;

/* The printed summary: static because it is more than the shell's stack
 * should be asked for, and the kernel copies it. */
static char s_page[SUMMARY_MAX];

/* ---- the day -------------------------------------------------------------- */

static const char *const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* The date and time to log: PAIN_NO_DAY and -1 when the clock is not set. A
 * clock that says it is before 2024 has not been set either, whatever it
 * claims. */
static void read_clock(void) {
  CappTime t;
  api->now(&t);
  if (t.year < 2024) t.synced = 0;
  pain_stamp(&t, &P.today, &P.minute);
}

/* Which entries Today's list shows: today's, or with no clock the log's whole
 * run, since none of it can be placed in a day. */
static void refresh_view(void) {
  if (P.today != PAIN_NO_DAY) {
    P.show_n = pain_day_count(&P.log, P.today, &P.show_first);
  } else {
    P.show_first = 0;
    P.show_n = P.log.n;
  }
}

/* Where the level starts: the last one logged in the list, else the middle. */
static void start_level(void) {
  P.level = P.show_n ? P.log.e[P.show_first + P.show_n - 1].level : 5;
}

/* "Tue 21 Oct". */
static void date_text(int32_t day, char *out, size_t n) {
  int y, m, d;
  dt_civil_from_days(day, &y, &m, &d);
  api->fmt(out, n, "%s %d %s", WDAY[dt_weekday(day)], d, MON[m - 1]);
}

/* A day as a heading: "Today", "Yesterday", "Mon 20 Oct", with the year when
 * it is not this one, "No date" for entries logged with no clock. */
static void day_label(int32_t day, char *out, size_t n) {
  int y, m, d, ty = 0, tm, td;
  if (day == PAIN_NO_DAY) { api->fmt(out, n, "No date"); return; }
  if (P.today != PAIN_NO_DAY) {
    if (day == P.today) { api->fmt(out, n, "Today"); return; }
    if (day == P.today - 1) { api->fmt(out, n, "Yesterday"); return; }
    dt_civil_from_days(P.today, &ty, &tm, &td);
  }
  dt_civil_from_days(day, &y, &m, &d);
  if (P.today != PAIN_NO_DAY && y != ty)
    api->fmt(out, n, "%s %d %s %d", WDAY[dt_weekday(day)], d, MON[m - 1], y);
  else
    api->fmt(out, n, "%s %d %s", WDAY[dt_weekday(day)], d, MON[m - 1]);
}

/* "14:05", or "--:--" for an entry with a date and no time. */
static void hm_text(const PainEntry *e, char *out, size_t n) {
  if (e->minute == PAIN_NO_TIME) api->fmt(out, n, "--:--");
  else api->fmt(out, n, "%02d:%02d", e->minute / 60, e->minute % 60);
}

/* When an entry was, in full: "Today 14:05", "Mon 20 Oct 18:30", "#12". */
static void entry_label(const PainEntry *e, char *out, size_t n) {
  char dl[24], hm[8];
  if (e->day == PAIN_NO_DAY) {
    api->fmt(out, n, "#%lu", (unsigned long)e->seq);
    return;
  }
  day_label(e->day, dl, sizeof dl);
  if (e->minute == PAIN_NO_TIME) {
    api->fmt(out, n, "%s", dl);
    return;
  }
  hm_text(e, hm, sizeof hm);
  api->fmt(out, n, "%s %s", dl, hm);
}

/* What names an entry in Today's list: its time if it is today's, "21 Oct"
 * if it is another day's, "#12" if it has no date at all. */
static void row_when(const PainEntry *e, char *out, size_t n) {
  int y, m, d;
  if (e->day == PAIN_NO_DAY) {
    api->fmt(out, n, "#%lu", (unsigned long)e->seq);
  } else if (P.today != PAIN_NO_DAY && e->day == P.today) {
    hm_text(e, out, n);
  } else {
    dt_civil_from_days(e->day, &y, &m, &d);
    api->fmt(out, n, "%d %s", d, MON[m - 1]);
  }
}

/* When the last entry was, short enough for the corner it goes in:
 * "today 14:05", "yest. 18:30", "Mon 20 Oct", "#12". */
static void last_text(const PainEntry *e, char *out, size_t n) {
  char hm[8];
  int timed = e->minute != PAIN_NO_TIME;
  hm_text(e, hm, sizeof hm);
  if (e->day == PAIN_NO_DAY)
    api->fmt(out, n, "#%lu", (unsigned long)e->seq);
  else if (P.today != PAIN_NO_DAY && e->day == P.today)
    api->fmt(out, n, timed ? "today %s" : "today", hm);
  else if (P.today != PAIN_NO_DAY && e->day == P.today - 1)
    api->fmt(out, n, timed ? "yest. %s" : "yest.", hm);
  else
    date_text(e->day, out, n);
}

/* ---- colour --------------------------------------------------------------- */

/* Green at 0, amber at 5, red at 10, blended between. */
static uint16_t level_colour(int level) {
  static const uint8_t R[3] = { 76, 244, 232 };
  static const uint8_t G[3] = { 198, 190, 70 };
  static const uint8_t B[3] = { 120, 70, 70 };
  int seg, t, r, g, b;
  if (level < 0) level = 0;
  if (level > PAIN_LEVEL_MAX) level = PAIN_LEVEL_MAX;
  seg = level >= 5;
  t = level - seg * 5;
  r = R[seg] + (R[seg + 1] - R[seg]) * t / 5;
  g = G[seg] + (G[seg + 1] - G[seg]) * t / 5;
  b = B[seg] + (B[seg + 1] - B[seg]) * t / 5;
  return CAPP_RGB(r, g, b);
}

/* ---- drawing -------------------------------------------------------------- */

/* A fill that is nothing when it has no size. */
static void blank(int x, int y, int w, int h, uint16_t col) {
  if (w > 0 && h > 0) api->fill(capp_rect(x, y, w, h), col);
}

static void blank_r(CRect r, uint16_t col) { blank(r.x, r.y, r.w, r.h, col); }

/* Is any of r being repaired? A region outside the clip is skipped whole. */
static int want(CRect r) { return capp_overlaps(P.clip, r); }

/* A line of text in a rectangle, and the rest of the rectangle in `bg`, each
 * pixel written once: the margins either side of the text, the bands above
 * and below it, and the text itself, which fills behind its own glyphs.
 * Nothing is cleared and then drawn over, so a repaint of only this
 * rectangle never shows it empty. `pad` is the gap from the edge it hugs;
 * the text is never put left of the rectangle. */
static void put(CRect r, int f, const char *s, uint16_t fg, uint16_t bg, int al, int pad) {
  int w = api->text_width(f, s), h = api->font_height(f);
  int right = r.x + r.w, bottom = r.y + r.h, x, y, fw;
  if (al == AL_LEFT) x = r.x + pad;
  else if (al == AL_RIGHT) x = right - pad - w;
  else x = r.x + (r.w - w) / 2;
  if (x < r.x) x = r.x;
  y = r.y + (r.h - h) / 2;
  if (y < r.y) y = r.y;
  fw = w;
  if (x + fw > right) fw = right - x;
  blank(r.x, r.y, x - r.x, r.h, bg);
  blank(x + fw, r.y, right - (x + fw), r.h, bg);
  blank(x, r.y, fw, y - r.y, bg);
  blank(x, y + h, fw, bottom - (y + h), bg);
  if (*s) api->text_font(f, (int16_t)x, (int16_t)y, s, fg, bg);
}

/* `s` cut to fit `maxw` pixels in font f, with ".." where it was cut. Into
 * `out`, which is 40 bytes at least. */
static void fit_text(int f, const char *s, int maxw, char *out) {
  size_t len = api->str_len(s), k, i;
  if (len > 37) len = 37;
  for (k = len; ; k--) {
    for (i = 0; i < k; i++) out[i] = s[i];
    out[k] = 0;
    if (k < len) { out[k] = '.'; out[k + 1] = '.'; out[k + 2] = 0; }
    if (k == 0 || api->text_width(f, out) <= maxw) return;
  }
}

static CRect row_rect(int i) {
  return capp_rect(P.lay.list.x, P.lay.list.y + i * ROW_H, P.lay.list.w, ROW_H);
}

/* The slot-th row of History's list on screen. */
static CRect hist_slot(int slot) {
  return capp_rect(P.hl.list.x, P.hl.list.y + slot * ROW_H, P.hl.list.w, ROW_H);
}

/* The rectangles of every screen, from the area paint was given. All of them
 * every time, so a key can mark what it changed on any screen. Today's level
 * is set as large as the column has room for: clock56 if it fits above the
 * word and the place, else num30. What is left over goes above the number and
 * below the place, half and half. */
static void layout(CRect c) {
  int fb = api->font_height(P.f_uib), fu = api->font_height(P.f_ui);
  int body_y = c.y + HEAD_H, body_h = c.h - HEAD_H - FOOT_H;
  int wh = fb + 2, ah = fu + 2 + DOT_BAND;
  int fixed = GAP_TOP + GAP_NUM + wh + GAP_WORD + ah;
  int nh, extra, top, y, rx, rw, rows, list_end, hvr, tbars;

  P.lay.all    = c;
  P.lay.head   = capp_rect(c.x, c.y, c.w, HEAD_H);
  P.lay.htitle = capp_rect(c.x, c.y, 90, HEAD_H);
  P.lay.hright = capp_rect(c.x + 90, c.y, c.w - 90, HEAD_H);
  P.lay.foot   = capp_rect(c.x, c.y + c.h - FOOT_H, c.w, FOOT_H);

  /* Today */
  P.f_shown = P.f_clock;
  nh = api->font_height(P.f_clock);
  if (nh + fixed > body_h && P.f_num >= 0) {
    P.f_shown = P.f_num;
    nh = api->font_height(P.f_num);
  }
  extra = body_h - (nh + fixed);
  if (extra < 0) extra = 0;
  top = GAP_TOP + extra / 2;

  P.lay.sep  = capp_rect(c.x + LEFT_W, body_y, 1, body_h);

  y = body_y;
  P.lay.gap[0] = capp_rect(c.x, y, LEFT_W, top);        y += top;
  P.lay.num    = capp_rect(c.x, y, LEFT_W, nh);         y += nh;
  P.lay.gap[1] = capp_rect(c.x, y, LEFT_W, GAP_NUM);    y += GAP_NUM;
  P.lay.word   = capp_rect(c.x, y, LEFT_W, wh);         y += wh;
  P.lay.gap[2] = capp_rect(c.x, y, LEFT_W, GAP_WORD);   y += GAP_WORD;
  P.lay.area   = capp_rect(c.x, y, LEFT_W, ah);         y += ah;
  P.lay.gap[3] = capp_rect(c.x, y, LEFT_W, body_y + body_h - y);

  rx = c.x + LEFT_W + 1;
  rw = c.w - LEFT_W - 1;
  rows = (body_h - ROW_H - CAP_H) / ROW_H;
  if (rows < 1) rows = 1;
  P.lay.rows = rows;
  P.lay.rhead = capp_rect(rx, body_y, rw, ROW_H);
  P.lay.list  = capp_rect(rx, body_y + ROW_H, rw, rows * ROW_H);
  P.lay.cap   = capp_rect(rx, body_y + body_h - CAP_H, rw, CAP_H);
  list_end = body_y + ROW_H + rows * ROW_H;
  P.lay.gap[4] = capp_rect(rx, list_end, rw, P.lay.cap.y - list_end);

  /* History: rows of the list, then the selected note, then the footer */
  hvr = (body_h - CAP_H) / ROW_H;
  if (hvr < 1) hvr = 1;
  P.hl.vrows = hvr;
  P.hl.list = capp_rect(c.x, body_y, c.w, hvr * ROW_H);
  P.hl.det  = capp_rect(c.x, body_y + body_h - CAP_H, c.w, CAP_H);
  P.hl.gap  = capp_rect(c.x, body_y + hvr * ROW_H, c.w, P.hl.det.y - (body_y + hvr * ROW_H));

  /* The note: what it is a note of, the field, the count */
  y = body_y;
  P.nt.g0    = capp_rect(c.x, y, c.w, 10);                  y += 10;
  P.nt.info  = capp_rect(c.x, y, c.w, ROW_H);               y += ROW_H;
  P.nt.g1    = capp_rect(c.x, y, c.w, 6);                   y += 6;
  P.nt.field = capp_rect(c.x, y, c.w, FIELD_H);             y += FIELD_H;
  P.nt.g2    = capp_rect(c.x, y, c.w, 4);                   y += 4;
  P.nt.hint  = capp_rect(c.x, y, c.w - 60, ROW_H);
  P.nt.count = capp_rect(c.x + c.w - 60, y, 60, ROW_H);     y += ROW_H;
  P.nt.g3    = capp_rect(c.x, y, c.w, body_y + body_h - y);

  /* Trends: three figures, the chart with its baseline, the day numbers. The
   * chart takes what the figures and the numbers leave, up to BARS_H, and
   * its bars are as tall a level as it can afford: a menu bar over the app
   * takes 11 pixels, and 3 a level instead of 4 is what gives them. */
  P.tl.x0 = c.x + (c.w - PAIN_CHART_DAYS * COL_W) / 2;
  P.tl.body = capp_rect(c.x, body_y, c.w, body_h);
  tbars = body_h - TILE_H - 4 - LAB_H;
  if (tbars > BARS_H) tbars = BARS_H;
  if (tbars < 24) tbars = 24;
  P.tl.unit = (tbars - 4) / PAIN_LEVEL_MAX;
  if (P.tl.unit < 2) P.tl.unit = 2;
  y = body_y;
  P.tl.tile[0] = capp_rect(c.x, y, 76, TILE_H);
  P.tl.tile[1] = capp_rect(c.x + 76, y, 90, TILE_H);
  P.tl.tile[2] = capp_rect(c.x + 166, y, c.w - 166, TILE_H);   y += TILE_H;
  P.tl.g1     = capp_rect(c.x, y, c.w, 4);                     y += 4;
  P.tl.bars   = capp_rect(c.x, y, c.w, tbars);                 y += tbars;
  P.tl.labels = capp_rect(c.x, y, c.w, LAB_H);                 y += LAB_H;
  P.tl.g2     = capp_rect(c.x, y, c.w, body_y + body_h - y);

  P.laid = 1;
}

/* The title strip of every screen. */
static void paint_head(const char *title, const char *right) {
  if (!want(P.lay.head)) return;
  put(P.lay.htitle, P.f_uib, title, CLR_TEXT, CLR_HEAD, AL_LEFT, 8);
  put(P.lay.hright, P.f_ui, right, CLR_DIM, CLR_HEAD, AL_RIGHT, 8);
}

/* What the footer says: key and word pairs, at most FOOT_CHARS, the keys the
 * screen needs in the moment (the rest are in the help). The delete question
 * outranks a word an action left, which outranks the keys. */
static const char *hint(void) {
  if (P.ask) return P.view == VIEW_TODAY ? "delete the last entry? y/n" : "delete this entry? y/n";
  if (P.say) return P.say;
  switch (P.view) {
  case VIEW_HISTORY: return P.log.n ? "^v move  e note  d delete  esc back" : "esc back";
  case VIEW_NOTE:    return "enter save  esc cancel";
  case VIEW_TRENDS:  return P.log.n ? "<> week  p print  h hist  esc back" : "esc back";
  default:           return "^v level  enter log  h hist  t trends";
  }
}

static void paint_foot(void) {
  if (!want(P.lay.foot)) return;
  footer_paint(api, P.lay.foot, hint());
}

/* ---- Today ---------------------------------------------------------------- */

/* Four dots, the place's own lit: where in Tab's round the choice is. */
static void paint_dots(CRect b) {
  int total = PAIN_AREAS * DOT + (PAIN_AREAS - 1) * DOT_GAP;
  int x0 = b.x + (b.w - total) / 2, y = b.y + (b.h - DOT) / 2, x = b.x, i;
  blank(b.x, b.y, b.w, y - b.y, CLR_BG);
  for (i = 0; i < PAIN_AREAS; i++) {
    int dx = x0 + i * (DOT + DOT_GAP);
    blank(x, y, dx - x, DOT, CLR_BG);
    blank(dx, y, DOT, DOT, i == P.area ? CLR_ACCENT : CLR_FAINT);
    x = dx + DOT;
  }
  blank(x, y, b.x + b.w - x, DOT, CLR_BG);
  blank(b.x, y + DOT, b.w, b.y + b.h - (y + DOT), CLR_BG);
}

static void paint_area(void) {
  CRect a = P.lay.area;
  char s[24];
  api->fmt(s, sizeof s, "< %s >", pain_area_label(P.area));
  put(capp_rect(a.x, a.y, a.w, a.h - DOT_BAND), P.f_uib, s, CLR_TEXT, CLR_BG, AL_CENTER, 0);
  paint_dots(capp_rect(a.x, a.y + a.h - DOT_BAND, a.w, DOT_BAND));
}

/* The level's column: the number, its word, the place. */
static void paint_left(void) {
  uint16_t col = level_colour(P.level);
  char s[4];
  int i;
  for (i = 0; i < 4; i++)
    if (want(P.lay.gap[i])) blank_r(P.lay.gap[i], CLR_BG);
  if (want(P.lay.num)) {
    api->fmt(s, sizeof s, "%d", P.level);
    put(P.lay.num, P.f_shown, s, col, CLR_BG, AL_CENTER, 0);
  }
  if (want(P.lay.word))
    put(P.lay.word, P.f_uib, pain_level_word(P.level), col, CLR_BG, AL_CENTER, 0);
  if (want(P.lay.area)) paint_area();
}

static void paint_rhead(void) {
  CRect r = P.lay.rhead;
  char cnt[16];
  if (P.show_n) api->fmt(cnt, sizeof cnt, "%d logged", P.show_n);
  else api->fmt(cnt, sizeof cnt, "none");
  put(capp_rect(r.x, r.y, r.w - 70, r.h), P.f_uib, P.today != PAIN_NO_DAY ? "Today" : "Latest",
      CLR_TEXT, CLR_BG, AL_LEFT, 8);
  put(capp_rect(r.x + r.w - 70, r.y, 70, r.h), P.f_ui, cnt, CLR_DIM, CLR_BG, AL_RIGHT, 8);
}

/* Row i of Today's list: the i-th newest entry; or, if there are more than
 * fit, a last row that counts the rest; or, if there are none, the empty
 * state. */
static void paint_row(int i) {
  CRect r = row_rect(i);
  uint16_t bg = (i & 1) ? CLR_ROW_ALT : CLR_ROW;
  const PainEntry *e;
  uint16_t col;
  char when[16], lv[4];
  if (P.show_n == 0) {
    const char *msg = "";
    if (i == 1) msg = P.today != PAIN_NO_DAY ? "Nothing logged today" : "Nothing logged yet";
    else if (i == 2) msg = "enter logs the level";
    put(r, P.f_ui, msg, CLR_DIM, bg, AL_CENTER, 0);
    return;
  }
  if (i >= P.show_n) {
    blank_r(r, bg);
    return;
  }
  if (i == P.lay.rows - 1 && P.show_n > P.lay.rows) {
    api->fmt(when, sizeof when, "+%d earlier", P.show_n - (P.lay.rows - 1));
    put(r, P.f_ui, when, CLR_DIM, bg, AL_LEFT, 8);
    return;
  }
  e = &P.log.e[P.show_first + P.show_n - 1 - i];
  col = level_colour(e->level);
  row_when(e, when, sizeof when);
  api->fmt(lv, sizeof lv, "%d", e->level);
  blank(r.x, r.y, 4, r.h, col);
  put(capp_rect(r.x + 4, r.y, 48, r.h), P.f_ui, when, CLR_TEXT, bg, AL_LEFT, 4);
  put(capp_rect(r.x + 52, r.y, 26, r.h), P.f_uib, lv, col, bg, AL_LEFT, 0);
  put(capp_rect(r.x + 78, r.y, r.w - 78, r.h), P.f_ui, pain_area_label(e->area), CLR_DIM, bg,
      AL_LEFT, 0);
}

/* "Last  7  yest. 18:30": the latest entry in the log, whenever it was. */
static void paint_cap(void) {
  CRect r = P.lay.cap;
  const PainEntry *e;
  char when[16], lv[4];
  if (!P.log.n) {
    put(r, P.f_ui, "No entries yet", CLR_DIM, CLR_HEAD, AL_CENTER, 0);
    return;
  }
  e = &P.log.e[P.log.n - 1];
  last_text(e, when, sizeof when);
  api->fmt(lv, sizeof lv, "%d", e->level);
  put(capp_rect(r.x, r.y, 36, r.h), P.f_ui, "Last", CLR_DIM, CLR_HEAD, AL_LEFT, 8);
  put(capp_rect(r.x + 36, r.y, 18, r.h), P.f_uib, lv, level_colour(e->level), CLR_HEAD, AL_LEFT, 0);
  put(capp_rect(r.x + 54, r.y, r.w - 54, r.h), P.f_ui, when, CLR_DIM, CLR_HEAD, AL_LEFT, 0);
}

static void paint_right(void) {
  int i;
  if (want(P.lay.sep)) api->fill(P.lay.sep, CLR_FAINT);
  if (want(P.lay.rhead)) paint_rhead();
  for (i = 0; i < P.lay.rows; i++)
    if (want(row_rect(i))) paint_row(i);
  if (want(P.lay.gap[4])) blank_r(P.lay.gap[4], CLR_BG);
  if (want(P.lay.cap)) paint_cap();
}

static void paint_today(void) {
  char s[24];
  if (P.today != PAIN_NO_DAY) date_text(P.today, s, sizeof s);
  else api->fmt(s, sizeof s, "no clock");
  paint_head("Pain", s);
  paint_left();
  paint_right();
  paint_foot();
}

/* ---- History: rows found by walking the log -------------------------------
 *
 * Newest first, and a heading before the newest entry of each day. Row r of
 * the list is that many steps from the top of this walk; nothing about the
 * list is kept but which entry is selected and which row is at the top. */

/* Row r: 1 and the newest entry of the day it heads, if it is a heading; 0
 * and the entry, if it is an entry; 0 and -1 past the end. */
static int hist_row(int r, int *idx) {
  int i = P.log.n - 1, row = 0;
  while (i >= 0) {
    int day = P.log.e[i].day;
    if (row == r) { *idx = i; return 1; }
    row++;
    while (i >= 0 && P.log.e[i].day == day) {
      if (row == r) { *idx = i; return 0; }
      row++;
      i--;
    }
  }
  *idx = -1;
  return 0;
}

static int hist_is_header(int r) {
  int idx;
  return hist_row(r, &idx);
}

/* How many rows the whole list is: an entry each and a heading a day. */
static int hist_total(void) {
  int i, rows = 0;
  for (i = P.log.n - 1; i >= 0; i--) {
    if (i == P.log.n - 1 || P.log.e[i].day != P.log.e[i + 1].day) rows++;
    rows++;
  }
  return rows;
}

/* The row entry k is on. */
static int hist_entry_row(int k) {
  int i = P.log.n - 1, row = 0;
  while (i >= 0) {
    int day = P.log.e[i].day;
    row++;
    while (i >= 0 && P.log.e[i].day == day) {
      if (i == k) return row;
      row++;
      i--;
    }
  }
  return 0;
}

/* How many entries share the day of entry idx and the worst of them. */
static void hist_group(int idx, int *count, int *worst) {
  int i, day = P.log.e[idx].day;
  *count = 0;
  *worst = 0;
  for (i = idx; i >= 0 && P.log.e[i].day == day; i--) {
    (*count)++;
    if (P.log.e[i].level > *worst) *worst = P.log.e[i].level;
  }
}

/* Scroll just enough that the selected entry is on screen, with its day's
 * heading above it if it is the day's newest and there is room. */
static void hist_fit(void) {
  int vr = P.hl.vrows, total, sr, lo, max;
  if (vr < 1) vr = 1;
  if (P.log.n == 0) { P.sel = -1; P.top = 0; return; }
  if (P.sel < 0) P.sel = 0;
  if (P.sel >= P.log.n) P.sel = P.log.n - 1;
  total = hist_total();
  sr = hist_entry_row(P.sel);
  lo = sr;
  if (sr > 0 && hist_is_header(sr - 1)) lo = sr - 1;
  if (P.top > lo) P.top = lo;
  if (sr >= P.top + vr) P.top = sr - vr + 1;
  max = total - vr;
  if (max < 0) max = 0;
  if (P.top > max) P.top = max;
  if (P.top < 0) P.top = 0;
}

/* A day's heading: its name on the left, how many and the worst on the right. */
static void paint_hist_header(CRect rr, int idx) {
  char lbl[24], sum[32];
  int cnt, worst;
  day_label(P.log.e[idx].day, lbl, sizeof lbl);
  hist_group(idx, &cnt, &worst);
  if (cnt == 1) api->fmt(sum, sizeof sum, "1 logged");
  else api->fmt(sum, sizeof sum, "%d logged, worst %d", cnt, worst);
  put(capp_rect(rr.x, rr.y, 120, rr.h), P.f_uib, lbl, CLR_TEXT, CLR_HEAD, AL_LEFT, 8);
  put(capp_rect(rr.x + 120, rr.y, rr.w - 120, rr.h), P.f_ui, sum, CLR_DIM, CLR_HEAD, AL_RIGHT, 8);
}

/* An entry: time, a bar as long as the level in the level's colour on a
 * track as long as ten, the level, the place, what fits of the note. */
static void paint_hist_entry(CRect rr, int idx) {
  const PainEntry *e = &P.log.e[idx];
  int sel = idx == P.sel;
  uint16_t bg = sel ? CLR_SEL : CLR_ROW, col = level_colour(e->level);
  int bx = rr.x + 54, by = rr.y + (rr.h - BAR_H) / 2, len = e->level * BAR_UNIT;
  char when[16], lv[4], note[40];
  if (e->day == PAIN_NO_DAY) api->fmt(when, sizeof when, "#%lu", (unsigned long)e->seq);
  else hm_text(e, when, sizeof when);
  api->fmt(lv, sizeof lv, "%d", e->level);
  fit_text(P.f_ui, e->note, rr.w - 176 - 4, note);

  blank(rr.x, rr.y, 3, rr.h, sel ? CLR_ACCENT : bg);
  put(capp_rect(rr.x + 3, rr.y, 49, rr.h), P.f_ui, when, CLR_TEXT, bg, AL_LEFT, 5);
  blank(rr.x + 52, rr.y, 2, rr.h, bg);
  blank(bx, rr.y, BAR_W, by - rr.y, bg);
  blank(bx, by, len, BAR_H, col);
  blank(bx + len, by, BAR_W - len, BAR_H, CLR_TRACK);
  blank(bx, by + BAR_H, BAR_W, rr.y + rr.h - (by + BAR_H), bg);
  blank(bx + BAR_W, rr.y, 2, rr.h, bg);
  put(capp_rect(rr.x + 106, rr.y, 22, rr.h), P.f_uib, lv, col, bg, AL_LEFT, 0);
  put(capp_rect(rr.x + 128, rr.y, 48, rr.h), P.f_ui, pain_area_label(e->area), CLR_DIM, bg, AL_LEFT, 0);
  put(capp_rect(rr.x + 176, rr.y, rr.w - 176, rr.h), P.f_ui, note, CLR_DIM, bg, AL_LEFT, 0);
}

/* Row r of the whole list, drawn in the slot rr. */
static void paint_hist_row(int r, CRect rr) {
  int idx, header = hist_row(r, &idx);
  if (idx < 0) blank_r(rr, CLR_BG);
  else if (header) paint_hist_header(rr, idx);
  else paint_hist_entry(rr, idx);
}

/* Under the list: the selected entry's whole note. */
static void paint_hist_note(void) {
  CRect r = P.hl.det;
  char note[40];
  if (P.sel < 0 || P.sel >= P.log.n) {
    put(r, P.f_ui, "", CLR_DIM, CLR_HEAD, AL_LEFT, 8);
  } else if (P.log.e[P.sel].note[0]) {
    fit_text(P.f_ui, P.log.e[P.sel].note, r.w - 16, note);
    put(r, P.f_ui, note, CLR_TEXT, CLR_HEAD, AL_LEFT, 8);
  } else {
    put(r, P.f_ui, "no note - e adds one", CLR_DIM, CLR_HEAD, AL_LEFT, 8);
  }
}

static void paint_history(void) {
  char pos[24];
  int i, total = hist_total(), n = P.log.n;
  if (n) api->fmt(pos, sizeof pos, "%d of %d", n - P.sel, n);
  else api->fmt(pos, sizeof pos, "empty");
  paint_head("History", pos);
  for (i = 0; i < P.hl.vrows; i++) {
    CRect rr = hist_slot(i);
    if (!want(rr)) continue;
    if (n == 0) {
      const char *msg = "";
      if (i == 1) msg = "Nothing logged yet";
      else if (i == 2) msg = "log one on the Today screen";
      put(rr, P.f_ui, msg, CLR_DIM, CLR_BG, AL_CENTER, 0);
    } else if (P.top + i >= total) {
      blank_r(rr, CLR_BG);
    } else {
      paint_hist_row(P.top + i, rr);
    }
  }
  if (want(P.hl.gap)) blank_r(P.hl.gap, CLR_BG);
  if (want(P.hl.det)) paint_hist_note();
  paint_foot();
}

/* ---- the note ------------------------------------------------------------- */

/* A field with its margins, the text so far and a cursor. */
static void paint_field(void) {
  CRect f = P.nt.field;
  char shown[PAIN_NOTE_MAX + 2];
  api->fmt(shown, sizeof shown, "%s_", P.draft);
  blank(f.x, f.y, 8, f.h, CLR_BG);
  put(capp_rect(f.x + 8, f.y, f.w - 16, f.h), P.f_ui, shown, CLR_TEXT, CLR_FIELD, AL_LEFT, 6);
  blank(f.x + f.w - 8, f.y, 8, f.h, CLR_BG);
}

static void paint_note(void) {
  const PainEntry *e = (P.sel >= 0 && P.sel < P.log.n) ? &P.log.e[P.sel] : NULL;
  char lbl[40], kind[32], lv[4], cnt[12];
  CRect r = P.nt.info;
  if (e) entry_label(e, lbl, sizeof lbl);
  else lbl[0] = 0;
  paint_head("Note", lbl);
  if (want(P.nt.g0)) blank_r(P.nt.g0, CLR_BG);
  if (want(r)) {
    if (e) {
      uint16_t col = level_colour(e->level);
      api->fmt(lv, sizeof lv, "%d", e->level);
      api->fmt(kind, sizeof kind, "%s, %s", pain_area_label(e->area), pain_level_word(e->level));
      blank(r.x, r.y, 4, r.h, col);
      put(capp_rect(r.x + 4, r.y, 30, r.h), P.f_uib, lv, col, CLR_BG, AL_LEFT, 4);
      put(capp_rect(r.x + 34, r.y, r.w - 34, r.h), P.f_ui, kind, CLR_DIM, CLR_BG, AL_LEFT, 0);
    } else {
      blank_r(r, CLR_BG);
    }
  }
  if (want(P.nt.g1)) blank_r(P.nt.g1, CLR_BG);
  if (want(P.nt.field)) paint_field();
  if (want(P.nt.g2)) blank_r(P.nt.g2, CLR_BG);
  api->fmt(cnt, sizeof cnt, "%d/%d", P.draft_len, PAIN_NOTE_MAX);
  if (want(P.nt.hint))
    put(P.nt.hint, P.f_ui, "enter saves, del clears", CLR_DIM, CLR_BG, AL_LEFT, 8);
  if (want(P.nt.count))
    put(P.nt.count, P.f_ui, cnt, P.draft_len >= PAIN_NOTE_MAX ? level_colour(8) : CLR_DIM,
        CLR_BG, AL_RIGHT, 8);
  if (want(P.nt.g3)) blank_r(P.nt.g3, CLR_BG);
  paint_foot();
}

/* ---- Trends: the numbers and the chart ------------------------------------ */

/* The first day anything was logged on, PAIN_NO_DAY if nothing was (undated
 * entries sort before every dated one). */
static int32_t oldest_day(void) {
  int i;
  for (i = 0; i < P.log.n; i++)
    if (P.log.e[i].day != PAIN_NO_DAY) return P.log.e[i].day;
  return PAIN_NO_DAY;
}

/* Work out the columns and the three figures for the window P.tr.off puts on
 * the log: the 14 days ending `off` days before today; with no clock, the 14
 * entries ending `off` entries before the last. Called whenever the log, the
 * date or the page changes, so paint only reads. */
static void trends_refresh(void) {
  int counted = P.today == PAIN_NO_DAY, k;
  if (counted != P.tr.counted) P.tr.off = 0;      /* the clock came or went: back to the end */
  P.tr.counted = counted;
  P.tr.worst_day = PAIN_NO_DAY;
  P.tr.worst_seq = 0;
  if (!counted) {
    PainStats st;
    P.tr.last = P.today - P.tr.off;
    P.tr.first = P.tr.last - (PAIN_CHART_DAYS - 1);
    pain_daily_max(&P.log, P.tr.last, PAIN_CHART_DAYS, P.tr.v);
    pain_stats_range(&P.log, P.tr.first, P.tr.last, &st);
    for (k = 0; k < PAIN_CHART_DAYS; k++) {
      int y, m, d;
      dt_civil_from_days(P.tr.first + k, &y, &m, &d);
      P.tr.dom[k] = (uint8_t)d;
    }
    P.tr.n = st.n;
    P.tr.avg10 = st.avg10;
    P.tr.worst = st.worst;
    P.tr.worst_day = st.worst_day;
    P.tr.days = st.days;
    P.tr.bad = st.bad_days;
  } else {
    int end = P.log.n - P.tr.off, start = end - PAIN_CHART_DAYS;
    long sum = 0;
    P.tr.first = PAIN_NO_DAY;
    P.tr.last = PAIN_NO_DAY;
    P.tr.n = 0;
    P.tr.avg10 = 0;
    P.tr.bad = 0;
    P.tr.worst = -1;
    for (k = 0; k < PAIN_CHART_DAYS; k++) {
      int idx = start + k;
      P.tr.dom[k] = 0;
      P.tr.v[k] = -1;
      if (idx >= 0 && idx < end) {
        const PainEntry *e = &P.log.e[idx];
        P.tr.v[k] = (int8_t)e->level;
        sum += e->level;
        P.tr.n++;
        if ((int)e->level >= P.tr.worst) {
          P.tr.worst = e->level;
          P.tr.worst_seq = e->seq;
        }
        if (e->level >= PAIN_BAD) P.tr.bad++;
      }
    }
    P.tr.days = P.tr.n;
    if (P.tr.n) P.tr.avg10 = (int)((sum * 10 + P.tr.n / 2) / P.tr.n);
  }
}

/* The days on the chart for the title: "8 - 21 Oct", "28 Sep - 11 Oct"; with
 * no clock, which entries. */
static void range_text(char *out, size_t n) {
  int y1, m1, d1, y2, m2, d2;
  if (P.tr.counted) {
    if (P.tr.off == 0) api->fmt(out, n, "last %d entries", PAIN_CHART_DAYS);
    else api->fmt(out, n, "%d entries, %d back", PAIN_CHART_DAYS, P.tr.off);
    return;
  }
  dt_civil_from_days(P.tr.first, &y1, &m1, &d1);
  dt_civil_from_days(P.tr.last, &y2, &m2, &d2);
  if (m1 == m2) api->fmt(out, n, "%d - %d %s", d1, d2, MON[m2 - 1]);
  else api->fmt(out, n, "%d %s - %d %s", d1, MON[m1 - 1], d2, MON[m2 - 1]);
}

/* Where the worst of the columns was: "14 Oct", or with no clock "#12". */
static void worst_text(char *out, size_t n) {
  int y, m, d;
  if (P.tr.counted) {
    api->fmt(out, n, "#%lu", (unsigned long)P.tr.worst_seq);
    return;
  }
  if (P.tr.worst_day == PAIN_NO_DAY) {
    out[0] = 0;
    return;
  }
  dt_civil_from_days(P.tr.worst_day, &y, &m, &d);
  api->fmt(out, n, "%d %s", d, MON[m - 1]);
}

/* A figure and what to read it against: the number in num30, then a few
 * words. */
static void paint_value(CRect r, const char *num, uint16_t col, const char *cap) {
  int nw = 8 + api->text_width(P.f_num, num) + 3;
  if (nw > r.w) nw = r.w;
  put(capp_rect(r.x, r.y, nw, r.h), P.f_num, num, col, CLR_BG, AL_LEFT, 8);
  if (r.w > nw) put(capp_rect(r.x + nw, r.y, r.w - nw, r.h), P.f_ui, cap, CLR_DIM, CLR_BG, AL_LEFT, 0);
}

/* The average: num30 has no full stop, so the dot is drawn, a 3-pixel square
 * on the digits' baseline, between the whole number and the tenth. */
static void paint_avg(CRect r, int avg10, uint16_t col) {
  char ip[4], fr[4];
  int wi, wf, fh = api->font_height(P.f_num), ty, dy, x;
  api->fmt(ip, sizeof ip, "%d", avg10 / 10);
  api->fmt(fr, sizeof fr, "%d", avg10 % 10);
  wi = api->text_width(P.f_num, ip);
  wf = api->text_width(P.f_num, fr);
  ty = r.y + (r.h - fh) / 2;
  if (ty < r.y) ty = r.y;
  dy = ty + fh - 3;
  x = r.x;
  put(capp_rect(x, r.y, 8 + wi, r.h), P.f_num, ip, col, CLR_BG, AL_LEFT, 8);
  x += 8 + wi;
  blank(x, r.y, 6, dy - r.y, CLR_BG);
  blank(x, dy, 1, 3, CLR_BG);
  blank(x + 1, dy, 3, 3, col);
  blank(x + 4, dy, 2, 3, CLR_BG);
  blank(x, dy + 3, 6, r.y + r.h - (dy + 3), CLR_BG);
  x += 6;
  put(capp_rect(x, r.y, wf, r.h), P.f_num, fr, col, CLR_BG, AL_LEFT, 0);
  x += wf;
  blank(x, r.y, r.x + r.w - x, r.h, CLR_BG);
}

/* Average, worst and bad days of the columns on screen. */
static void paint_tiles(void) {
  static const char *const LABEL[2][3] = {
    { "Average", "Worst", "Bad days" },
    { "Average", "Worst", "Bad (7+)" },
  };
  int i;
  for (i = 0; i < 3; i++) {
    CRect t = P.tl.tile[i], v;
    char num[8], cap[16];
    if (!want(t)) continue;
    v = capp_rect(t.x, t.y + 16, t.w, TILE_H - 16);
    put(capp_rect(t.x, t.y, t.w, 16), P.f_ui, LABEL[P.tr.counted][i], CLR_DIM, CLR_BG, AL_LEFT, 8);
    if (P.tr.n == 0) {
      put(v, P.f_num, "-", CLR_FAINT, CLR_BG, AL_LEFT, 8);
    } else if (i == 0) {
      paint_avg(v, P.tr.avg10, level_colour((P.tr.avg10 + 5) / 10));
    } else if (i == 1) {
      api->fmt(num, sizeof num, "%d", P.tr.worst);
      worst_text(cap, sizeof cap);
      paint_value(v, num, level_colour(P.tr.worst), cap);
    } else {
      api->fmt(num, sizeof num, "%d", P.tr.bad);
      api->fmt(cap, sizeof cap, "/%d", P.tr.days);
      paint_value(v, num, level_colour(P.tr.bad ? 8 : 0), cap);
    }
  }
}

/* Rows y0 to y1 of one column: background, except the bar where it reaches
 * (rows from `top` down), which is `col`. A column's bands share a bar and
 * differ only in what is behind it, so the faint line at 7 can run under the
 * bars and over the gaps without anything being drawn twice. */
static void col_band(int x, int y0, int y1, int top, uint16_t col, uint16_t bg) {
  int by0 = top > y0 ? top : y0;
  if (by0 > y1) by0 = y1;
  blank(x, y0, COL_W, by0 - y0, bg);
  blank(x, by0, COL_PAD, y1 - by0, bg);
  blank(x + COL_PAD, by0, COL_W - 2 * COL_PAD, y1 - by0, col);
  blank(x + COL_W - COL_PAD, by0, COL_PAD, y1 - by0, bg);
}

/* The chart: a bar a column, a level's colour and P.tl.unit pixels tall for
 * each point of it (a 0 is a stub, so a day with no pain is not a day with
 * nothing logged), a line across at 7, a baseline under. */
static void paint_cols(void) {
  CRect b = P.tl.bars;
  int base = b.y + b.h - 1, yt = base - PAIN_BAD * P.tl.unit;
  int end = P.tl.x0 + PAIN_CHART_DAYS * COL_W, k;
  blank(b.x, b.y, P.tl.x0 - b.x, b.h, CLR_BG);
  for (k = 0; k < PAIN_CHART_DAYS; k++) {
    int x = P.tl.x0 + k * COL_W, v = P.tr.v[k];
    int bh = v < 0 ? 0 : v == 0 ? 2 : v * P.tl.unit;
    int top = base - bh;
    uint16_t col = v < 0 ? CLR_BG : level_colour(v);
    col_band(x, b.y, yt, top, col, CLR_BG);
    col_band(x, yt, yt + 1, top, col, CLR_TRACK);
    col_band(x, yt + 1, base, top, col, CLR_BG);
    blank(x, base, COL_W, 1, CLR_FAINT);
  }
  blank(end, b.y, b.x + b.w - end, b.h, CLR_BG);
}

/* Under each column: its day of the month (today's in teal, the first of a
 * month in white, a day with nothing logged dim); with no clock, its level. */
static void paint_labels(void) {
  CRect l = P.tl.labels;
  int end = P.tl.x0 + PAIN_CHART_DAYS * COL_W, k;
  blank(l.x, l.y, P.tl.x0 - l.x, l.h, CLR_BG);
  for (k = 0; k < PAIN_CHART_DAYS; k++) {
    int v = P.tr.v[k];
    uint16_t col;
    char s[8];
    if (P.tr.counted) {
      if (v >= 0) api->fmt(s, sizeof s, "%d", v);
      else s[0] = 0;
      col = v >= 0 ? level_colour(v) : CLR_FAINT;
    } else {
      api->fmt(s, sizeof s, "%d", P.tr.dom[k]);
      if (P.tr.first + k == P.today) col = CLR_ACCENT;
      else if (P.tr.dom[k] == 1) col = CLR_TEXT;
      else col = v >= 0 ? CLR_DIM : CLR_FAINT;
    }
    put(capp_rect(P.tl.x0 + k * COL_W, l.y, COL_W, l.h), -1, s, col, CLR_BG, AL_CENTER, 0);
  }
  blank(end, l.y, l.x + l.w - end, l.h, CLR_BG);
}

/* Nothing logged at all: the body says what to do instead of an empty chart. */
static void paint_trends_empty(void) {
  static const char *const MSG[3] = { "No pain logged yet", "log one on the Today screen",
                                      "and the trends show up here" };
  CRect b = P.tl.body;
  int top = (b.h - 3 * ROW_H) / 2, i;
  if (top < 0) top = 0;
  blank(b.x, b.y, b.w, top, CLR_BG);
  for (i = 0; i < 3; i++)
    put(capp_rect(b.x, b.y + top + i * ROW_H, b.w, ROW_H), P.f_ui, MSG[i],
        i ? CLR_DIM : CLR_TEXT, CLR_BG, AL_CENTER, 0);
  blank(b.x, b.y + top + 3 * ROW_H, b.w, b.h - top - 3 * ROW_H, CLR_BG);
}

static void paint_trends(void) {
  char rng[32];
  range_text(rng, sizeof rng);
  paint_head("Trends", rng);
  if (P.log.n == 0) {
    if (want(P.tl.body)) paint_trends_empty();
  } else {
    paint_tiles();
    if (want(P.tl.g1)) blank_r(P.tl.g1, CLR_BG);
    if (want(P.tl.bars)) {
      /* a stretch with nothing in it: an empty chart would look like a broken one */
      if (P.tr.n == 0) put(P.tl.bars, P.f_ui, "Nothing logged in these days", CLR_DIM, CLR_BG, AL_CENTER, 0);
      else paint_cols();
    }
    if (want(P.tl.labels)) paint_labels();
    if (want(P.tl.g2)) blank_r(P.tl.g2, CLR_BG);
  }
  paint_foot();
}

/* The menu bar goes above everything and its open menu over it (apps/toolbar.h);
 * the screens are laid out in what the bar leaves, which is 11 pixels less
 * when it is showing. */
static void app_paint(void *st, CRect full) {
  CRect c;
  (void)st;
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  P.lay.full = full;
  toolbar_paint_bar(full);
  c = toolbar_rest(full);
  layout(c);
  P.clip = api->paint_area();
  if (P.view == VIEW_HISTORY) paint_history();
  else if (P.view == VIEW_NOTE) paint_note();
  else if (P.view == VIEW_TRENDS) paint_trends();
  else paint_today();
  toolbar_paint_menu(full);
}

/* ---- saying what changed -------------------------------------------------- */

/* Say what changed, if a paint has told us where things are; before the
 * first paint there is nothing to mark and the first paint is complete. */
static void mark(CRect r) {
  if (P.laid) api->damage(r);
}

/* A new screen: everything, the menu bar included, since its menus change
 * with the screen. Marking nothing would do the same, but a key that also
 * cleared the footer's word has marked the footer already, and that alone
 * would be all the repaint the new screen got. */
static void repaint_all(void) {
  mark(P.lay.full);
}

/* A word in the footer until the next key. */
static void say(const char *s) {
  P.say = s;
  mark(P.lay.foot);
}

static void mark_list(void) {
  mark(P.lay.rhead);
  mark(P.lay.list);
  mark(P.lay.cap);
}

/* History's row r, if it is on screen. */
static void mark_hist_row(int r) {
  if (r >= P.top && r < P.top + P.hl.vrows) mark(hist_slot(r - P.top));
}

static void mark_note(void) {
  mark(P.nt.field);
  mark(P.nt.count);
}

/* ---- the actions and the menu bar ------------------------------------------
 *
 * Everything the app can be asked to do, once. The OS reads this table for the
 * fn-p chord, the fn-space palette and the help panel; the menu bar (apps/
 * toolbar.h) for its menus, from the `menu` names; and do_action carries each
 * out, whichever way it was asked. Plain letters are not chords and are not in
 * the table's `key`: they are the key handlers', and call do_action. */

enum { ACT_NEW = 1, ACT_UP, ACT_DOWN, ACT_AREA, ACT_EDIT, ACT_DELETE, ACT_PRINT,
       ACT_TODAY, ACT_HISTORY, ACT_TRENDS, ACT_EARLIER, ACT_LATER, ACT_SAVE, ACT_CANCEL };

static const CappAction ACTIONS[] = {
  { "new",     "Log entry",     "Entry", 0, ACT_NEW },
  { "up",      "Level up",      "Entry", 0, ACT_UP },
  { "down",    "Level down",    "Entry", 0, ACT_DOWN },
  { "area",    "Next place",    "Entry", 0, ACT_AREA },
  { "edit",    "Edit note...",  "Entry", 0, ACT_EDIT },
  { "delete",  "Delete...",     "Entry", 0, ACT_DELETE },
  { "print",   "Print summary", "Entry", CAPP_KEY_PRINT, ACT_PRINT },   /* fn-p */
  { "today",   "Today",         "View",  0, ACT_TODAY },
  { "history", "History",       "View",  0, ACT_HISTORY },
  { "trends",  "Trends",        "View",  0, ACT_TRENDS },
  { "earlier", "Week back",     "View",  0, ACT_EARLIER },
  { "later",   "Week forward",  "View",  0, ACT_LATER },
  { "save",    "Save note",     "Note",  0, ACT_SAVE },
  { "cancel",  "Cancel",        "Note",  0, ACT_CANCEL },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

/* Which of them the menu bar offers on each screen, in the order of its
 * menus: a menu item that cannot apply is worse than none. */
static const uint8_t MENU_TODAY[]  = { ACT_NEW, ACT_UP, ACT_DOWN, ACT_AREA, ACT_EDIT, ACT_DELETE,
                                       ACT_PRINT, ACT_HISTORY, ACT_TRENDS };
static const uint8_t MENU_HIST[]   = { ACT_NEW, ACT_EDIT, ACT_DELETE, ACT_PRINT, ACT_TODAY, ACT_TRENDS };
static const uint8_t MENU_TRENDS[] = { ACT_NEW, ACT_PRINT, ACT_TODAY, ACT_HISTORY, ACT_EARLIER,
                                       ACT_LATER };
static const uint8_t MENU_NOTE[]   = { ACT_SAVE, ACT_CANCEL };

/* The bar reads its table by pointer, so the screen's own is built here, from
 * ACTIONS, copying each entry with api->mem_cpy (a struct assignment would
 * compile to a memcpy an app does not have). */
static CappAction s_menu[10];

static void set_menu(void) {
  const uint8_t *ids;
  int n, i, j, k = 0;
  switch (P.view) {
  case VIEW_HISTORY: ids = MENU_HIST;   n = (int)sizeof MENU_HIST;   break;
  case VIEW_TRENDS:  ids = MENU_TRENDS; n = (int)sizeof MENU_TRENDS; break;
  case VIEW_NOTE:    ids = MENU_NOTE;   n = (int)sizeof MENU_NOTE;   break;
  default:           ids = MENU_TODAY;  n = (int)sizeof MENU_TODAY;  break;
  }
  for (i = 0; i < n; i++)
    for (j = 0; j < NACT; j++)
      if (ACTIONS[j].action == ids[i] && k < (int)(sizeof s_menu / sizeof s_menu[0])) {
        api->mem_cpy(&s_menu[k++], &ACTIONS[j], sizeof s_menu[0]);
        break;
      }
  toolbar_set(s_menu, k, 0, 0);
}

/* Change screen: everything repaints, the bar's menus follow, and what one
 * screen carries -- a question being asked, a page of the trends -- does not
 * come along to the next. */
static void enter_view(int v) {
  P.view = v;
  P.ask = 0;
  if (v != VIEW_TRENDS) P.tr.off = 0;
  set_menu();
  repaint_all();
}

/* ---- Trends: opening, paging, printing ------------------------------------ */

/* To the trends, on the latest stretch. */
static void open_trends(void) {
  P.tr.off = 0;
  trends_refresh();
  enter_view(VIEW_TRENDS);
}

/* A week back (dir < 0) or forward (dir > 0): back as far as the oldest entry
 * (the new last day may not be before it; with no clock, the new last entry
 * must exist), forward as far as today. At either end the page stays put;
 * what is marked is the same, because a key that changed nothing should still
 * cost a small repaint and not the whole screen. */
static void trends_page(int dir) {
  if (dir < 0) {
    if (P.tr.counted) {
      if (P.tr.off + 7 < P.log.n) P.tr.off += 7;
    } else {
      int32_t old = oldest_day();
      if (old != PAIN_NO_DAY && P.tr.last - 7 >= old) P.tr.off += 7;
    }
  } else if (P.tr.off >= 7) {
    P.tr.off -= 7;
  }
  trends_refresh();
  mark(P.tl.body);
  mark(P.lay.hright);
}

/* Where on a day it hurt: "cramps, back". */
static void day_areas(int32_t day, char *out, size_t n) {
  int first, cnt = pain_day_count(&P.log, day, &first), i, a, mask = 0;
  size_t o = 0;
  for (i = 0; i < cnt; i++) mask |= 1 << P.log.e[first + i].area;
  out[0] = 0;
  for (a = 0; a < PAIN_AREAS; a++)
    if (mask & (1 << a))
      o += (size_t)api->fmt(out + o, n - o, "%s%s", o ? ", " : "", pain_area_name(a));
}

/* Room for another line of the summary. */
#define PAGE_ROOM(o) ((o) + 80 < sizeof s_page)

/* The summary for a doctor, in the printer's markup: the days on the chart,
 * the whole log, and each logged day. The figures are P.tr's, so they are the
 * ones on screen; the caller has just refreshed them. */
static void build_summary(void) {
  PainStats all;
  CappTime t;
  char when[32], rng[32], worst[16], areas[48], ds[16];
  size_t o = 0;
  int k;

  pain_stats(&P.log, &all);
  api->now(&t);
  if (t.synced && t.year >= 2024 && t.month >= 1 && t.month <= 12)
    api->fmt(when, sizeof when, "%d %s %d %02d:%02d", t.day, MON[t.month - 1], t.year, t.hour, t.min);
  else
    api->fmt(when, sizeof when, "undated");
  range_text(rng, sizeof rng);
  worst_text(worst, sizeof worst);

  o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "# Pain summary\nprinted %s\n---\n## %s\n",
                        when, rng);
  if (P.tr.n == 0) {
    o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "nothing logged\n");
  } else {
    o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "average %d.%d of 10\n",
                          P.tr.avg10 / 10, P.tr.avg10 % 10);
    o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "worst %d, %s\n", P.tr.worst, worst);
    o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "bad (%d+): %d of %d %s\n", PAIN_BAD,
                          P.tr.bad, P.tr.days, P.tr.counted ? "entries" : "days");
  }

  o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "---\n## whole log\n%d entries\n", all.n);
  o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "average %d.%d of 10\nworst %d\n",
                        all.avg10 / 10, all.avg10 % 10, all.worst);
  o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "bad (%d+): %d of %d %s\n", PAIN_BAD,
                        all.bad_days, all.days, P.tr.counted ? "entries" : "days");

  if (P.tr.n == 0) return;
  o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "---\n## %s\n",
                        P.tr.counted ? "entry by entry" : "day by day");
  for (k = 0; k < PAIN_CHART_DAYS && PAGE_ROOM(o); k++) {
    if (P.tr.v[k] < 0) continue;
    if (P.tr.counted) {
      const PainEntry *e = &P.log.e[P.log.n - P.tr.off - PAIN_CHART_DAYS + k];
      o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "#%lu: %d %s\n", (unsigned long)e->seq,
                            e->level, pain_area_name(e->area));
    } else {
      date_text(P.tr.first + k, ds, sizeof ds);
      day_areas(P.tr.first + k, areas, sizeof areas);
      o += (size_t)api->fmt(s_page + o, sizeof s_page - o, "%s: %d %s\n", ds, P.tr.v[k], areas);
    }
  }
}

/* Print the summary, and say in the footer how that started. */
static void print_summary(void) {
  int rc;
  if (P.log.n == 0) {
    P.say = "nothing logged to print yet";
    mark(P.lay.foot);
    return;
  }
  trends_refresh();
  build_summary();
  rc = api->print_fonts(s_page, "print24", "print24b", "print34b");
  if (rc == 0)       P.say = "printing...";
  else if (rc == -1) P.say = "still printing the last one";
  else if (rc == -2) P.say = "no printer: print scan in the console";
  else               P.say = "could not print: no memory";
  mark(P.lay.foot);
}

/* ---- Today: what the keys do ---------------------------------------------- */

static void set_level(int v) {
  if (v < 0) v = 0;
  if (v > PAIN_LEVEL_MAX) v = PAIN_LEVEL_MAX;
  P.level = v;
  mark(P.lay.num);
  mark(P.lay.word);
}

/* The place one step on (d > 0) or back (d < 0), round the four. */
static void step_area(int d) {
  P.area = (P.area + PAIN_AREAS + d) % PAIN_AREAS;
  mark(P.lay.area);
}

/* Log what is on screen, now, and save. The footer says how it went: a log
 * that did not reach the card is still in memory, and the next one tries
 * again with it. */
static void log_now(void) {
  int was = P.today, at;
  read_clock();
  at = pain_add(&P.log, P.today, P.minute, P.level, P.area, "");
  if (at < 0) {
    P.say = "older than the log keeps";
    mark(P.lay.foot);
    return;
  }
  refresh_view();
  if (pain_save(&P.log, api) == 0) {
    api->fmt(P.say_buf, sizeof P.say_buf, "logged %d, %s", P.level, pain_area_name(P.area));
    P.say = P.say_buf;
  } else {
    P.say = "logged, but the card would not save";
  }
  mark_list();
  mark(P.lay.foot);
  if (P.today != was) mark(P.lay.head);          /* midnight, or the clock arrived */
}

/* To the history, on the newest entry. */
static void open_history(void) {
  P.sel = P.log.n ? P.log.n - 1 : -1;
  P.top = 0;
  hist_fit();
  enter_view(VIEW_HISTORY);
}

/* The entry just logged: the newest in Today's list, or -1 for none. Today's
 * edit and delete are about it, since nothing else on that screen is selected. */
static int last_today(void) {
  return P.show_n ? P.show_first + P.show_n - 1 : -1;
}

/* ---- History: what the keys do -------------------------------------------- */

/* Select entry s (kept in range) and scroll to it. A move inside the screen
 * redraws the row it left and the row it reached; one that scrolls redraws
 * the list. The note under it and the place in the title change either way,
 * and are marked even when the selection did not move, so a key that does
 * nothing still asks for something small and not for the whole screen. */
static void hist_select(int s) {
  int old = P.sel, old_top = P.top;
  if (P.log.n == 0) return;
  if (s < 0) s = 0;
  if (s >= P.log.n) s = P.log.n - 1;
  P.sel = s;
  hist_fit();
  if (P.top != old_top) {
    mark(P.hl.list);
  } else if (s != old) {
    mark_hist_row(hist_entry_row(old));
    mark_hist_row(hist_entry_row(s));
  }
  mark(P.hl.det);
  mark(P.lay.hright);
}

/* To the newest entry of the next day older (dir < 0) or newer (dir > 0). */
static void hist_day(int dir) {
  int i = P.sel, n = P.log.n, day;
  if (i < 0 || i >= n) return;
  day = P.log.e[i].day;
  if (dir < 0) {
    while (i >= 0 && P.log.e[i].day == day) i--;
    if (i < 0) { hist_select(P.sel); return; }       /* already in the oldest day */
  } else {
    while (i < n && P.log.e[i].day == day) i++;
    if (i >= n) { hist_select(P.sel); return; }      /* already in the newest */
    day = P.log.e[i].day;
    while (i + 1 < n && P.log.e[i + 1].day == day) i++;
  }
  hist_select(i);
}

/* The note of entry P.sel on a screen of its own, going back to `back` when
 * it is saved or given up. */
static void begin_note(int back) {
  if (P.sel < 0 || P.sel >= P.log.n) return;
  api->fmt(P.draft, sizeof P.draft, "%s", P.log.e[P.sel].note);
  P.draft_len = (int)api->str_len(P.draft);
  P.note_back = back;
  enter_view(VIEW_NOTE);
}

/* Leave the field for the screen it came from, keeping what was typed (save)
 * or not. */
static void end_note(int save) {
  if (save && P.sel >= 0 && P.sel < P.log.n) {
    pain_set_note(&P.log, P.sel, P.draft);
    P.say = pain_save(&P.log, api) == 0 ? "note saved" : "note kept, but the card would not save";
  }
  if (P.note_back == VIEW_HISTORY) hist_fit();
  enter_view(P.note_back);
}

/* Remove the selected entry and save. In the history the selection falls to
 * the next older, which is where the row under the deleted one lands; on
 * Today the list and the last line change. */
static void delete_selected(void) {
  int s = P.sel;
  if (s < 0 || s >= P.log.n) return;
  pain_delete(&P.log, s);
  P.sel = s > 0 ? s - 1 : 0;
  if (P.log.n == 0) P.sel = -1;
  refresh_view();
  P.say = pain_save(&P.log, api) == 0 ? "deleted" : "deleted, but the card would not save";
  if (P.view == VIEW_TODAY) {
    mark_list();
  } else {
    hist_fit();
    mark(P.hl.list);
    mark(P.hl.det);
    mark(P.lay.hright);
  }
  mark(P.lay.foot);
}

/* Ask whether to delete entry `idx`; key_ask takes the answer. */
static void ask_delete(int idx) {
  P.sel = idx;
  P.ask = 1;
  mark(P.lay.foot);
}

/* The answer to the question: y deletes; n, Escape and Backspace do not;
 * anything else leaves it up (apps/confirm.h). */
static int key_ask(uint8_t k) {
  int a = confirm_key(api, k);
  if (a == CONFIRM_WAIT) return 1;
  P.ask = 0;
  mark(P.lay.foot);
  if (a == CONFIRM_YES) delete_selected();
  return 1;
}

/* One action, from a key, the menu bar or the palette. Where it means nothing
 * on the screen it is on, nothing happens, or the footer says why: the palette
 * lists every action whatever is showing, and a mouse can click an item the
 * bar still shows. On the note's screen only its own two apply. */
static int do_action(int a) {
  if (P.view == VIEW_NOTE) {
    if (a == ACT_SAVE) end_note(1);
    else if (a == ACT_CANCEL) end_note(0);
    return 1;
  }
  if (P.ask) {                                   /* any other action answers the question: no */
    P.ask = 0;
    mark(P.lay.foot);
  }
  switch (a) {
  case ACT_NEW:
    if (P.view == VIEW_TODAY) log_now();         /* a new entry is the level and place on screen */
    else enter_view(VIEW_TODAY);                 /* ... which is chosen there */
    return 1;
  case ACT_UP:
    if (P.view == VIEW_TODAY) set_level(P.level + 1);
    return 1;
  case ACT_DOWN:
    if (P.view == VIEW_TODAY) set_level(P.level - 1);
    return 1;
  case ACT_AREA:
    if (P.view == VIEW_TODAY) step_area(1);
    return 1;
  case ACT_EDIT:
    if (P.view == VIEW_HISTORY) {
      begin_note(VIEW_HISTORY);
    } else if (P.view == VIEW_TODAY) {
      if (last_today() < 0) {
        say("log an entry first");
      } else {
        P.sel = last_today();
        begin_note(VIEW_TODAY);
      }
    }
    return 1;
  case ACT_DELETE:
    if (P.view == VIEW_HISTORY) {
      if (P.sel >= 0) ask_delete(P.sel);
    } else if (P.view == VIEW_TODAY) {
      if (last_today() < 0) say("nothing logged to delete");
      else ask_delete(last_today());
    }
    return 1;
  case ACT_PRINT:
    print_summary();
    return 1;
  case ACT_TODAY:
    if (P.view != VIEW_TODAY) enter_view(VIEW_TODAY);
    return 1;
  case ACT_HISTORY:
    if (P.view != VIEW_HISTORY) open_history();
    return 1;
  case ACT_TRENDS:
    if (P.view != VIEW_TRENDS) open_trends();
    return 1;
  case ACT_EARLIER:
    if (P.view == VIEW_TRENDS) trends_page(-1);
    return 1;
  case ACT_LATER:
    if (P.view == VIEW_TRENDS) trends_page(1);
    return 1;
  case ACT_SAVE:
  case ACT_CANCEL:
    return 1;
  default:
    return 0;
  }
}

/* Today's keys. Up and down are the level, left, right and Tab the place; the
 * rest are the shared ones, each through its action. Only moving is for
 * holding down. */
static int key_today(uint8_t k) {
  if (P.ask) return key_ask(k);
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 1;
  switch (k) {
  case CAPP_KEY_UP:    return do_action(ACT_UP);
  case CAPP_KEY_DOWN:  return do_action(ACT_DOWN);
  case CAPP_KEY_LEFT:  step_area(-1); return 1;
  case CAPP_KEY_RIGHT:
  case '\t':           step_area(1); return 1;
  case CAPP_KEY_ENTER:
  case 'n': case 'N':  return do_action(ACT_NEW);
  case 'e': case 'E':  return do_action(ACT_EDIT);
  case 'd': case 'D': case 0x7F: return do_action(ACT_DELETE);
  case 'p': case 'P':  return do_action(ACT_PRINT);
  case 'h': case 'H':  return do_action(ACT_HISTORY);
  case 't': case 'T':  return do_action(ACT_TRENDS);
  default:             return 0;
  }
}

static int key_hist(uint8_t k) {
  if (P.ask) return key_ask(k);
  /* Moving is for holding down; nothing else should fire twice. */
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN &&
      k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return 1;
  switch (k) {
  case CAPP_KEY_UP:    hist_select(P.sel + 1); return 1;        /* newer is up the list */
  case CAPP_KEY_DOWN:  hist_select(P.sel - 1); return 1;
  case CAPP_KEY_LEFT:  hist_day(-1); return 1;
  case CAPP_KEY_RIGHT: hist_day(1); return 1;
  case CAPP_KEY_ENTER:
  case 'e': case 'E':  return do_action(ACT_EDIT);
  case 'n': case 'N':  return do_action(ACT_NEW);
  case 'd': case 'D': case 0x7F: return do_action(ACT_DELETE);
  case 'p': case 'P':  return do_action(ACT_PRINT);
  case 't': case 'T':  return do_action(ACT_TRENDS);
  case CAPP_KEY_ESC:
  case CAPP_KEY_BACK:  return do_action(ACT_TODAY);
  default:             return 0;
  }
}

/* ---- Trends: what the keys do --------------------------------------------- */

static int key_trends(uint8_t k) {
  /* Paging is for holding down; nothing else should fire twice. */
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN &&
      k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return 1;
  switch (k) {
  case CAPP_KEY_LEFT:
  case CAPP_KEY_UP:    return do_action(ACT_EARLIER);
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_DOWN:  return do_action(ACT_LATER);
  case 'n': case 'N':  return do_action(ACT_NEW);
  case 'p': case 'P':  return do_action(ACT_PRINT);
  case 'h': case 'H':  return do_action(ACT_HISTORY);
  case CAPP_KEY_ESC:
  case CAPP_KEY_BACK:  return do_action(ACT_TODAY);
  default:             return 0;
  }
}

/* ---- the note: what the keys do ------------------------------------------- */

/* A text field: whatever is typed is for it, and nothing leaks out. */
static int key_note(uint8_t k) {
  if (k == CAPP_KEY_ESC) { end_note(0); return 1; }
  if (k == CAPP_KEY_ENTER) { end_note(1); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (P.draft_len) {
      P.draft[--P.draft_len] = 0;
      mark_note();
    }
    return 1;
  }
  if (k == 0x7F) {
    P.draft_len = 0;
    P.draft[0] = 0;
    mark_note();
    return 1;
  }
  if (k >= 32 && k < 127 && P.draft_len < PAIN_NOTE_MAX) {
    P.draft[P.draft_len++] = (char)k;
    P.draft[P.draft_len] = 0;
    mark_note();
  }
  return 1;
}

/* ---- the app -------------------------------------------------------------- */

/* The menu bar first, as apps/toolbar.h asks: while it has the keyboard (fn-b)
 * every key is its, and fn-b itself changes the layout, so nothing may be
 * marked before it -- the repaint that follows is the whole screen's. */
static int app_key(void *st, uint8_t k) {
  int a = toolbar_key(k);
  (void)st;
  if (a == TB_CONSUMED) return 1;
  if (P.say) {
    P.say = NULL;
    mark(P.lay.foot);
  }
  if (a != TB_NONE) return do_action(a);
  if (P.view == VIEW_NOTE) return key_note(k);
  if (P.view == VIEW_HISTORY) return key_hist(k);
  if (P.view == VIEW_TRENDS) return key_trends(k);
  return key_today(k);
}

/* The OS's way in for the fn-p chord and the palette's entries. */
static int app_action(void *st, int a) {
  (void)st;
  return do_action(a);
}

static int app_click(void *st, int16_t x, int16_t y, int button) {
  int a;
  (void)st; (void)button;
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  return 0;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int ch;
  (void)st; (void)buttons; (void)wheel;
  ch = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) ch = 1;
  return ch;
}

/* Typed characters are wanted in the note's field and nowhere else, so
 * everywhere else ; . , / are the arrows -- and not while the menu bar has
 * the keyboard, or a spoken sentence would type into a field that is not
 * listening. */
static int app_wants_text(void *st) {
  (void)st;
  return !toolbar_has_keys() && P.view == VIEW_NOTE;
}

/* The date moving on under an open app -- midnight, or the clock arriving
 * late -- changes the header, which entries are today's and the last line, so
 * it is a whole repaint. The minute alone is not shown and is not asked
 * about. */
static int app_tick(void *st, uint32_t now) {
  int32_t was = P.today;
  (void)st;
  if (now - P.clock_at < 5000) return 0;
  P.clock_at = now;
  read_clock();
  if (P.today == was) return 0;
  refresh_view();
  trends_refresh();
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Pain",
  /* 16x16: a drop, with a highlight. */
  { 0x01, 0x80, 0x01, 0x80, 0x03, 0xC0, 0x03, 0xC0,
    0x07, 0xE0, 0x0F, 0xF0, 0x1F, 0xF8, 0x1F, 0xF8,
    0x3F, 0xFC, 0x3B, 0xFC, 0x3B, 0xFC, 0x3F, 0xFC,
    0x1F, 0xF8, 0x1F, 0xF8, 0x07, 0xE0, 0x00, 0x00 },
  "up/down\tthe level, 0 to 10\n"
  "left/right\twhere it hurts: cramps, back, head, other (tab too)\n"
  "enter n\tlog an entry now, with the date and time\n"
  "e\tadd or change the note of the last entry logged\n"
  "d del\tdelete the last entry logged: y yes, n esc bksp no\n"
  "h\tHistory: every entry, a day at a time\n"
  "t\tTrends: 14 days as bars, with the average, the worst and the bad days (7+)\n"
  "p\tprint a summary for a doctor visit\n"
  "history\tup/down an entry, left/right a day, e or enter the note, d delete, n new, esc back\n"
  "trends\tleft/right (or up/down) a week back / forward, h History, esc back\n"
  "note\ttype it, enter saves, del clears, esc keeps the old one\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&P, 0, sizeof P);
  P.f_clock = api->font_load("clock56");
  P.f_num   = api->font_load("num30");
  P.f_ui    = api->font_load("ui13");
  P.f_uib   = api->font_load("ui13b");
  P.sel = -1;
  read_clock();
  pain_load(&P.log, api);
  refresh_view();
  start_level();
  if (P.log.skipped) {
    api->fmt(P.say_buf, sizeof P.say_buf, "%d unreadable line%s skipped", P.log.skipped,
             P.log.skipped == 1 ? "" : "s");
    P.say = P.say_buf;
  }
  P.clock_at = api->ticks_ms();
  toolbar_init(api, ACTIONS, NACT, 0, 0);
  set_menu();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
