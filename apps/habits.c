/* Habit tracking: a list of habits, and which days each one was done.
 *
 * Storage is two kinds of file under /home/habits, both plain text so a card
 * reader or `cat` in Files reads them without this app:
 *
 *   habits.txt   one habit name per line, in the order shown on screen.
 *   <name>.log   one marked day per line, YYYYMMDD, for that habit alone.
 *
 * A log is named after its habit rather than an id, because there is no
 * second place that hands out ids -- habits.txt *is* the list of habits, and
 * a name typed once is what both files agree on. Renaming a habit means
 * renaming its log too (api->rename covers both in one call); that is a
 * later step's problem, not this one's.
 *
 * Everything here is whole-file rewrite, the same choice apps/todo.c makes
 * for its caches: a year of daily marks is a few kilobytes, well inside what
 * fits in a stack buffer, and a file that is only ever fully read then fully
 * written cannot be left half-updated by a toggle that touched one line.
 *
 * The day view (enter from the list, or ctrl-t) opens on today, lists the
 * habits with a [x]/[ ] beside each, and left/right step the viewed date a
 * day at a time. Toggling a habit there loads its log, adds or removes the
 * one date, and writes the log straight back; nothing is held in memory
 * past the call that changed it, so leaving the app mid-session loses
 * nothing.
 *
 * The calendar (c, or ctrl-c) is a month grid: left/right step a day and
 * up/down a week, both crossing a month boundary on their own, and each
 * day is shaded by how many habits were marked done on it. Enter, or
 * tapping a cell, opens the day view on that date -- which is also why the
 * day view remembers which of the list or the calendar sent it there
 * (day_back), so Escape returns to whichever one it was.
 *
 * Stats (s, or ctrl-s) lists every habit with its current streak and its
 * total marked days, both read straight off the log: the streak walks
 * backward from today counting consecutive marked days, capped at the
 * log's own length rather than assumed to be in order, since a log edited
 * by hand on a card reader might not be.
 *
 * There is no menu: switching views is the ctrl chords in ACTIONS
 * (ctrl-t/c/s), and the shell matches those against whichever view is on
 * screen before offering the key to the view's own handler -- see
 * kernel/app/capp.h's note on CappUi.actions -- so ctrl-c from the stats
 * view reaches the calendar exactly as it would from the list. Add and
 * delete are the exception: they only act while the list is the one on
 * screen, since they are list management rather than a view to switch to.
 * The list is still the app's top level -- the one Escape leaves the app
 * from -- and the day, calendar and stats views are each one step in from
 * it, or from each other.
 */

#include "kernel/app/capp.h"

#define DIR          CAPP_HOME "/habits"
#define HABITS_PATH  DIR "/habits.txt"
#define LOG_FMT      DIR "/%s.log"

#define MAX_HABITS      20
#define HABIT_NAME_MAX  24
#define MAX_LOG_DATES   400   /* over a year of daily marks */
#define ROW_H           11

#define CLR_BG      CAPP_RGB(28, 30, 36)
#define CLR_TEXT    CAPP_RGB(220, 224, 232)
#define CLR_DIM     CAPP_RGB(130, 138, 150)
#define CLR_SEL     CAPP_RGB(52, 80, 116)
#define CLR_BAR     CAPP_RGB(48, 82, 128)
#define CLR_BAR_FG  CAPP_RGB(232, 238, 248)
#define CLR_WARN    CAPP_RGB(220, 60, 60)
#define CLR_FIELD   CAPP_RGB(40, 44, 52)
#define CLR_CELL    CAPP_RGB(40, 44, 52)
#define CLR_PARTIAL CAPP_RGB(70, 100, 60)
#define CLR_DONE    CAPP_RGB(60, 150, 90)

typedef struct {
  char name[HABIT_NAME_MAX + 1];
} Habit;

static const CardApi *api;

enum { VIEW_LIST = 0, VIEW_ADD, VIEW_DAY, VIEW_CALENDAR, VIEW_STATS };
enum { ASK_NONE = 0, ASK_DELETE };

static struct {
  Habit habit[MAX_HABITS];
  int   n;

  int   view;
  int   sel;
  int   top;
  int   rows;
  int   ask;

  char  draft[HABIT_NAME_MAX + 1];
  int   draft_len;

  /* the day view: a date and, per habit, whether it is marked that day --
   * recomputed from the logs whenever the date changes rather than kept in
   * sync, since nothing else in this view can move a log out from under it.
   * day_back is which view opened it -- the list (the "today" shortcut) or
   * the calendar (a day tapped in the grid) -- so Escape returns there
   * rather than always to the list. */
  int     day_year, day_month, day_day;
  int     day_synced;
  int     day_sel, day_top, day_rows;
  int     day_back;
  uint8_t day_done[MAX_HABITS];

  /* the calendar: the month on screen, the selected day within it, and how
   * many habits were marked done on each day of that month -- one pass over
   * each habit's log rather than one log read per day, so opening the
   * calendar is O(habits) file reads and not O(habits x days). */
  int     cal_year, cal_month, cal_sel;
  uint8_t cal_count[32];   /* index 1..31; 0 unused */

  /* stats: per habit, the current streak (consecutive marked days counting
   * back from today) and the total marked days -- both read straight off
   * the log, so this view has no state of its own to go stale. */
  int stats_sel, stats_top, stats_rows;
  int stats_synced;
  int stats_streak[MAX_HABITS];
  int stats_total[MAX_HABITS];

  CRect content;
} H;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static int same(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return !*a && !*b;
}

/* ---- the habit list -------------------------------------------------- */

static void habits_load(void) {
  char buf[256], line[HABIT_NAME_MAX + 1];
  int fd, n, i, len = 0;

  H.n = 0;
  fd = api->open(HABITS_PATH, CAPP_O_READ);
  if (fd < 0) return;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      len = 0;
      if (line[0] && H.n < MAX_HABITS)
        api->fmt(H.habit[H.n++].name, sizeof H.habit[0].name, "%s", line);
    }
  }
  api->close(fd);
}

static void habits_save(void) {
  char line[HABIT_NAME_MAX + 2];
  int fd, i, n;

  api->mkdir(DIR);
  fd = api->open(HABITS_PATH, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  for (i = 0; i < H.n; i++) {
    n = api->fmt(line, sizeof line, "%s\n", H.habit[i].name);
    api->write(fd, line, (size_t)n);
  }
  api->close(fd);
}

/* ---- one habit's log of marked days ----------------------------------
 *
 * A date is YYYYMMDD packed into one uint32_t -- api->now() already hands
 * over year/month/day as separate fields, and packed decimal both sorts and
 * prints without unpacking it. */

static void log_path(const char *habit_name, char *out, size_t n) {
  api->fmt(out, n, LOG_FMT, habit_name);
}

static int log_load(const char *habit_name, uint32_t *dates, int max) {
  char path[64], buf[256], line[16];
  int fd, n, i, len = 0, count = 0;

  log_path(habit_name, path, sizeof path);
  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) return 0;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      len = 0;
      if (count < max) {
        const char *p = line;
        uint32_t v = 0;
        int digits = 0;
        for (; *p >= '0' && *p <= '9'; p++, digits++) v = v * 10 + (uint32_t)(*p - '0');
        if (digits == 8) dates[count++] = v;
      }
    }
  }
  api->close(fd);
  return count;
}

static void log_save(const char *habit_name, const uint32_t *dates, int count) {
  char path[64], line[16];
  int fd, i, n;

  api->mkdir(DIR);
  log_path(habit_name, path, sizeof path);
  fd = api->open(path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  for (i = 0; i < count; i++) {
    n = api->fmt(line, sizeof line, "%04u%02u%02u\n",
                 dates[i] / 10000u, (dates[i] / 100u) % 100u, dates[i] % 100u);
    api->write(fd, line, (size_t)n);
  }
  api->close(fd);
}

static uint32_t pack_date(int y, int m, int d) {
  return (uint32_t)y * 10000u + (uint32_t)m * 100u + (uint32_t)d;
}

static int is_leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

static int days_in_month(int y, int m) {
  static const uint8_t d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m == 2 && is_leap(y)) return 29;
  return d[m - 1];
}

/* Howard Hinnant's civil-date algorithm, the same one apps/calendar.c uses:
 * a date turned into a day count from 1970-01-01 and back, which is what
 * lets the calendar step by a week or jump months without a case for every
 * boundary. */
static int32_t days_from_civil(int y, int m, int d) {
  int era;
  unsigned yoe, doy, doe;
  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int32_t)era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int *yy, int *mm, int *dd) {
  int era, y;
  unsigned doe, yoe, doy, mp, d, m;
  z += 719468;
  era = (int)((z >= 0 ? z : z - 146096) / 146097);
  doe = (unsigned)(z - (int32_t)era * 146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : (unsigned)-9);
  *yy = y + (int)(m <= 2);
  *mm = (int)m;
  *dd = (int)d;
}

/* 0 = Sunday. Day 0 (1970-01-01) was a Thursday, hence the 4. */
static int weekday_of_day(int32_t z) {
  int32_t w = (z + 4) % 7;
  return (int)(w < 0 ? w + 7 : w);
}

/* delta is always +-1: one key press is one day. */
static void date_add_day(int *y, int *m, int *d, int delta) {
  *d += delta;
  if (*d < 1) {
    if (--*m < 1) { *m = 12; (*y)--; }
    *d = days_in_month(*y, *m);
  } else if (*d > days_in_month(*y, *m)) {
    *d = 1;
    if (++*m > 12) { *m = 1; (*y)++; }
  }
}

/* One habit's log, read whole, changed by one date, written back whole --
 * the same whole-file rewrite log_load/log_save were built for. Kept as a
 * single static scratch array rather than a stack buffer: 400 dates is
 * 1.6 KB, and apps run on the shell's own stack, not one of their own. */
static uint32_t log_scratch[MAX_LOG_DATES];

static void toggle_log_date(const char *habit_name, uint32_t date, int *out_marked) {
  int count = log_load(habit_name, log_scratch, MAX_LOG_DATES);
  int i, pos = -1, ins;

  for (i = 0; i < count; i++) if (log_scratch[i] == date) { pos = i; break; }
  if (pos >= 0) {
    for (i = pos; i + 1 < count; i++) log_scratch[i] = log_scratch[i + 1];
    count--;
    *out_marked = 0;
  } else if (count < MAX_LOG_DATES) {
    ins = count;
    for (i = 0; i < count; i++) if (log_scratch[i] > date) { ins = i; break; }
    for (i = count; i > ins; i--) log_scratch[i] = log_scratch[i - 1];
    log_scratch[ins] = date;
    count++;
    *out_marked = 1;
  } else {
    *out_marked = 0;   /* log is full; the day stays whatever it was */
  }
  log_save(habit_name, log_scratch, count);
}

static int day_marked(const char *habit_name, uint32_t date) {
  int count = log_load(habit_name, log_scratch, MAX_LOG_DATES);
  int i;
  for (i = 0; i < count; i++) if (log_scratch[i] == date) return 1;
  return 0;
}

/* ---- adding and deleting ----------------------------------------------
 *
 * A habit's name is also its log's filename, so a '/' in it would write
 * outside /home/habits; typing one is refused rather than escaped, the same
 * choice apps/todo.c makes about quotes in a JSON title. */

static void clamp_scroll(void) {
  if (H.sel < 0) H.sel = 0;
  if (H.sel >= H.n) H.sel = H.n ? H.n - 1 : 0;
  if (H.sel < H.top) H.top = H.sel;
  if (H.rows && H.sel >= H.top + H.rows) H.top = H.sel - H.rows + 1;
  if (H.top < 0) H.top = 0;
}

static void add_habit(void) {
  int i;
  if (!H.draft_len || H.n >= MAX_HABITS) { H.view = VIEW_LIST; return; }
  for (i = 0; i < H.n; i++)
    if (same(H.habit[i].name, H.draft)) { H.view = VIEW_LIST; H.sel = i; clamp_scroll(); return; }
  api->fmt(H.habit[H.n].name, sizeof H.habit[0].name, "%s", H.draft);
  H.sel = H.n;
  H.n++;
  habits_save();
  H.view = VIEW_LIST;
  clamp_scroll();
}

static void delete_habit(void) {
  char path[64];
  int i;
  if (!H.n) return;
  log_path(H.habit[H.sel].name, path, sizeof path);
  api->remove(path);
  for (i = H.sel; i + 1 < H.n; i++)
    api->mem_cpy(&H.habit[i], &H.habit[i + 1], sizeof H.habit[0]);
  H.n--;
  habits_save();
  clamp_scroll();
}

/* ---- the day view -------------------------------------------------------
 *
 * Opens on today; left/right step the viewed date a day at a time, up/down
 * move the highlighted habit, enter or a click toggles it. */

static void day_recompute(void) {
  uint32_t d = pack_date(H.day_year, H.day_month, H.day_day);
  int i;
  for (i = 0; i < H.n; i++) H.day_done[i] = (uint8_t)day_marked(H.habit[i].name, d);
}

/* `back` is which view Escape returns to: the list, for the "today"
 * shortcut, or the calendar, for a day tapped in the grid. */
static void day_open_at(int y, int m, int d, int synced, int back) {
  H.day_year = y; H.day_month = m; H.day_day = d;
  H.day_synced = synced;
  H.day_back = back;
  H.day_sel = 0; H.day_top = 0;
  H.view = VIEW_DAY;
  day_recompute();
}

static void day_open(void) {
  CappTime t;
  api->now(&t);
  day_open_at(t.year, t.month, t.day, t.synced, VIEW_LIST);
}

static void clamp_day_scroll(void) {
  if (H.day_sel < 0) H.day_sel = 0;
  if (H.day_sel >= H.n) H.day_sel = H.n ? H.n - 1 : 0;
  if (H.day_sel < H.day_top) H.day_top = H.day_sel;
  if (H.day_rows && H.day_sel >= H.day_top + H.day_rows) H.day_top = H.day_sel - H.day_rows + 1;
  if (H.day_top < 0) H.day_top = 0;
}

static void day_toggle(int idx) {
  uint32_t d;
  int marked;
  if (idx < 0 || idx >= H.n) return;
  d = pack_date(H.day_year, H.day_month, H.day_day);
  toggle_log_date(H.habit[idx].name, d, &marked);
  H.day_done[idx] = (uint8_t)marked;
}

/* ---- the calendar ---------------------------------------------------------
 *
 * A month grid. Left/right step a day, up/down a week -- both cross a month
 * boundary on their own, via the same day-count Hinnant's algorithm gives
 * the day view's arithmetic no reason to use (it only ever moves by one day,
 * where the simpler carry-the-month version already worked). Enter, or a
 * tap on a cell, opens that day. */

static void cal_recompute(void) {
  int i, j, count;
  for (i = 0; i <= 31; i++) H.cal_count[i] = 0;
  for (i = 0; i < H.n; i++) {
    count = log_load(H.habit[i].name, log_scratch, MAX_LOG_DATES);
    for (j = 0; j < count; j++) {
      uint32_t dt = log_scratch[j];
      int yy = (int)(dt / 10000u);
      int mm = (int)((dt / 100u) % 100u);
      int dd = (int)(dt % 100u);
      if (yy == H.cal_year && mm == H.cal_month && dd >= 1 && dd <= 31) H.cal_count[dd]++;
    }
  }
}

static void cal_open(void) {
  CappTime t;
  api->now(&t);
  H.cal_year = t.year; H.cal_month = t.month; H.cal_sel = t.day;
  H.view = VIEW_CALENDAR;
  cal_recompute();
}

static void cal_move(int delta_days) {
  int32_t z = days_from_civil(H.cal_year, H.cal_month, H.cal_sel) + delta_days;
  int y = H.cal_year, m = H.cal_month;
  civil_from_days(z, &H.cal_year, &H.cal_month, &H.cal_sel);
  if (H.cal_year != y || H.cal_month != m) cal_recompute();
}

/* ---- stats ----------------------------------------------------------------
 *
 * The current streak: walk backward from today one day at a time, counting
 * while each is in the log, stopping at the first gap or once every entry
 * has been accounted for -- a log with `count` dates cannot support a
 * streak longer than `count`, so that bounds the walk without assuming the
 * dates are in order (a hand-edited log on a card reader might not be). */

static int date_in_array(const uint32_t *dates, int count, uint32_t d) {
  int i;
  for (i = 0; i < count; i++) if (dates[i] == d) return 1;
  return 0;
}

static int compute_streak(const uint32_t *dates, int count, int y, int m, int d) {
  int32_t z = days_from_civil(y, m, d);
  int streak = 0, yy, mm, dd;
  while (streak < count) {
    civil_from_days(z - streak, &yy, &mm, &dd);
    if (!date_in_array(dates, count, pack_date(yy, mm, dd))) break;
    streak++;
  }
  return streak;
}

static void clamp_stats_scroll(void) {
  if (H.stats_sel < 0) H.stats_sel = 0;
  if (H.stats_sel >= H.n) H.stats_sel = H.n ? H.n - 1 : 0;
  if (H.stats_sel < H.stats_top) H.stats_top = H.stats_sel;
  if (H.stats_rows && H.stats_sel >= H.stats_top + H.stats_rows)
    H.stats_top = H.stats_sel - H.stats_rows + 1;
  if (H.stats_top < 0) H.stats_top = 0;
}

static void stats_recompute(void) {
  CappTime t;
  int i, count;
  api->now(&t);
  H.stats_synced = t.synced;
  for (i = 0; i < H.n; i++) {
    count = log_load(H.habit[i].name, log_scratch, MAX_LOG_DATES);
    H.stats_total[i] = count;
    H.stats_streak[i] = compute_streak(log_scratch, count, t.year, t.month, t.day);
  }
}

static void stats_open(void) {
  H.stats_sel = 0; H.stats_top = 0;
  H.view = VIEW_STATS;
  stats_recompute();
}

/* ---- painting ----------------------------------------------------------- */

static void paint_list(CRect c) {
  int i, y;

  H.rows = (c.h - ROW_H) / ROW_H;
  if (!H.n) {
    api->text((short)(c.x + 6), (short)(c.y + 6), "no habits yet", CLR_DIM, CLR_BG);
    api->text((short)(c.x + 6), (short)(c.y + 18), "a adds one", CLR_DIM, CLR_BG);
  } else {
    for (i = H.top, y = c.y; i < H.n && i < H.top + H.rows; i++, y += ROW_H) {
      int sel = i == H.sel;
      uint16_t bg = sel ? CLR_SEL : CLR_BG;
      api->fill(rect(c.x, y, c.w, ROW_H), bg);
      api->text((short)(c.x + 6), (short)(y + 1), H.habit[i].name,
                sel ? CLR_BAR_FG : CLR_TEXT, bg);
    }
  }

  {
    CRect s = rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H);
    api->fill(s, CLR_BAR);
    if (H.ask == ASK_DELETE) {
      char q[40];
      api->fmt(q, sizeof q, "delete %s?  y/n", H.n ? H.habit[H.sel].name : "");
      api->text((short)(s.x + 3), (short)(s.y + 2), q, CLR_WARN, CLR_BAR);
    } else {
      api->text((short)(s.x + 3), (short)(s.y + 2), "a add   d delete", CLR_BAR_FG, CLR_BAR);
    }
  }
}

static void paint_add(CRect c) {
  char shown[HABIT_NAME_MAX + 2];
  int i;

  api->fill(c, CLR_BG);
  api->text((short)(c.x + 6), (short)(c.y + 8), "New habit", CLR_BAR_FG, CLR_BG);

  api->fill(rect(c.x + 5, c.y + 22, c.w - 10, 14), CLR_FIELD);
  for (i = 0; i < H.draft_len; i++) shown[i] = H.draft[i];
  shown[H.draft_len] = '_';
  shown[H.draft_len + 1] = 0;
  api->text((short)(c.x + 8), (short)(c.y + 25), shown, CLR_TEXT, CLR_FIELD);

  api->text((short)(c.x + 6), (short)(c.y + 44), "enter adds   esc cancels", CLR_DIM, CLR_BG);
}

static void paint_day(CRect c) {
  char hdr[40], line[HABIT_NAME_MAX + 6];
  int i, y;

  api->fmt(hdr, sizeof hdr, "%04d-%02d-%02d%s", H.day_year, H.day_month, H.day_day,
           H.day_synced ? "" : "  (clock not set)");
  api->text((short)(c.x + 6), (short)(c.y + 2), hdr, CLR_BAR_FG, CLR_BG);

  H.day_rows = (c.h - 2 * ROW_H) / ROW_H;
  if (!H.n) {
    api->text((short)(c.x + 6), (short)(c.y + ROW_H + 6), "no habits yet", CLR_DIM, CLR_BG);
  } else {
    for (i = H.day_top, y = c.y + ROW_H; i < H.n && i < H.day_top + H.day_rows; i++, y += ROW_H) {
      int sel = i == H.day_sel;
      uint16_t bg = sel ? CLR_SEL : CLR_BG;
      api->fill(rect(c.x, y, c.w, ROW_H), bg);
      api->fmt(line, sizeof line, "%s %s", H.day_done[i] ? "[x]" : "[ ]", H.habit[i].name);
      api->text((short)(c.x + 6), (short)(y + 1), line, sel ? CLR_BAR_FG : CLR_TEXT, bg);
    }
  }

  {
    CRect s = rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H);
    api->fill(s, CLR_BAR);
    api->text((short)(s.x + 3), (short)(s.y + 2), "</> day   enter mark   esc back",
              CLR_BAR_FG, CLR_BAR);
  }
}

static const char *const MON[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};
static const char WDAY[7] = { 'S', 'M', 'T', 'W', 'T', 'F', 'S' };

static void paint_calendar(CRect c) {
  char hdr[24];
  int first, len, d, col, row, cell_w, cell_h, grid_y, i;

  api->fmt(hdr, sizeof hdr, "%s %d", MON[H.cal_month - 1], H.cal_year);
  api->text((short)(c.x + 6), (short)(c.y + 2), hdr, CLR_BAR_FG, CLR_BG);

  grid_y = c.y + ROW_H;
  for (i = 0; i < 7; i++) {
    char wd[2];
    wd[0] = WDAY[i]; wd[1] = 0;
    api->text((short)(c.x + 4 + i * (c.w / 7)), (short)grid_y, wd, CLR_DIM, CLR_BG);
  }
  grid_y += 9;

  cell_w = c.w / 7;
  cell_h = (c.y + c.h - ROW_H - grid_y) / 6;
  first = weekday_of_day(days_from_civil(H.cal_year, H.cal_month, 1));
  len = days_in_month(H.cal_year, H.cal_month);

  for (d = 1; d <= len; d++) {
    int idx = d - 1 + first;
    uint16_t bg;
    char label[4];
    CRect cell;

    col = idx % 7;
    row = idx / 7;
    cell = rect(c.x + col * cell_w, grid_y + row * cell_h, cell_w - 1, cell_h - 1);

    if (H.n && H.cal_count[d] >= H.n) bg = CLR_DONE;
    else if (H.cal_count[d] > 0) bg = CLR_PARTIAL;
    else bg = CLR_CELL;
    api->fill(cell, bg);
    if (d == H.cal_sel) api->frame(cell, CLR_BAR_FG);

    api->fmt(label, sizeof label, "%d", d);
    api->text((short)(cell.x + 2), (short)(cell.y + 1), label, CLR_TEXT, bg);
  }

  {
    CRect s = rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H);
    api->fill(s, CLR_BAR);
    api->text((short)(s.x + 3), (short)(s.y + 2), "arrows move   enter opens   esc back",
              CLR_BAR_FG, CLR_BAR);
  }
}

static void paint_stats(CRect c) {
  int i, y;

  api->text((short)(c.x + 6), (short)(c.y + 2),
            H.stats_synced ? "Stats" : "Stats  (clock not set)", CLR_BAR_FG, CLR_BG);

  H.stats_rows = (c.h - 2 * ROW_H) / ROW_H;
  if (!H.n) {
    api->text((short)(c.x + 6), (short)(c.y + ROW_H + 6), "no habits yet", CLR_DIM, CLR_BG);
  } else {
    for (i = H.stats_top, y = c.y + ROW_H; i < H.n && i < H.stats_top + H.stats_rows; i++, y += ROW_H) {
      int sel = i == H.stats_sel;
      uint16_t bg = sel ? CLR_SEL : CLR_BG;
      char num[16];
      int nw;
      api->fill(rect(c.x, y, c.w, ROW_H), bg);
      api->text((short)(c.x + 6), (short)(y + 1), H.habit[i].name,
                sel ? CLR_BAR_FG : CLR_TEXT, bg);
      api->fmt(num, sizeof num, "%dd  %dx", H.stats_streak[i], H.stats_total[i]);
      nw = (int)api->str_len(num);
      api->text((short)(c.x + c.w - 6 - 6 * nw), (short)(y + 1), num,
                sel ? CLR_BAR_FG : CLR_DIM, bg);
    }
  }

  {
    CRect s = rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H);
    api->fill(s, CLR_BAR);
    api->text((short)(s.x + 3), (short)(s.y + 2), "streak / total marked   esc back",
              CLR_BAR_FG, CLR_BAR);
  }
}

static void app_paint(void *st, CRect c) {
  (void)st;
  H.content = c;
  api->fill(c, CLR_BG);
  if (H.view == VIEW_ADD) paint_add(c);
  else if (H.view == VIEW_DAY) paint_day(c);
  else if (H.view == VIEW_CALENDAR) paint_calendar(c);
  else if (H.view == VIEW_STATS) paint_stats(c);
  else paint_list(c);
}

/* ---- input --------------------------------------------------------------- */

enum { ACT_ADD = 1, ACT_DELETE, ACT_CANCEL, ACT_SAVE, ACT_TODAY, ACT_TOGGLE, ACT_CALENDAR,
       ACT_STATS };

static const CappAction ACTIONS[] = {
  { "add",      "Add habit",    "Habit", 0x01, ACT_ADD },      /* ctrl-a */
  { "delete",   "Delete habit", "Habit", 0x04, ACT_DELETE },   /* ctrl-d */
  { "today",    "Today",        "Habit", 0x14, ACT_TODAY },    /* ctrl-t */
  { "toggle",   "Toggle",       "Habit", 0x18, ACT_TOGGLE },   /* ctrl-x */
  { "calendar", "Calendar",     "Habit", 0x03, ACT_CALENDAR }, /* ctrl-c */
  { "stats",    "Stats",        "Habit", 0x13, ACT_STATS },    /* ctrl-s */
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

/* The shell matches every ctrl chord in ACTIONS against whatever view is on
 * screen, not just the list -- that is what makes ctrl-t/c/s a way to jump
 * straight to the day, calendar or stats view from any of the others. Add
 * and delete are list management, not view switches, so unlike the rest of
 * this table they only fire from the list: ctrl-d reached from the day view
 * used to set the ask-a-question flag with no list on screen to show it on,
 * so the question was still pending, unseen, the next time the list came
 * back into view. */
static int do_action(int a) {
  switch (a) {
  case ACT_ADD:
    if (H.view != VIEW_LIST || H.n >= MAX_HABITS) return 1;
    H.draft[0] = 0;
    H.draft_len = 0;
    H.view = VIEW_ADD;
    return 1;
  case ACT_DELETE:
    if (H.view == VIEW_LIST && H.n) H.ask = ASK_DELETE;
    return 1;
  case ACT_SAVE:     add_habit(); return 1;
  case ACT_CANCEL:   H.view = VIEW_LIST; return 1;
  case ACT_TODAY:    day_open(); return 1;
  case ACT_TOGGLE:
    if (H.view == VIEW_DAY && H.n) day_toggle(H.day_sel);
    return 1;
  case ACT_CALENDAR: cal_open(); return 1;
  case ACT_STATS:    stats_open(); return 1;
  default: return 0;
  }
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int key_list(uint8_t k) {
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 0;

  if (H.ask == ASK_DELETE) {
    if (k == 'y' || k == 'Y' || k == CAPP_KEY_ENTER) { H.ask = ASK_NONE; delete_habit(); }
    else if (k == 'n' || k == 'N' || k == CAPP_KEY_ESC) H.ask = ASK_NONE;
    return 1;
  }
  switch (k) {
  case CAPP_KEY_UP:   if (H.sel > 0) H.sel--; clamp_scroll(); return 1;
  case CAPP_KEY_DOWN: if (H.sel + 1 < H.n) H.sel++; clamp_scroll(); return 1;
  case CAPP_KEY_ENTER: return do_action(ACT_TODAY);
  case 'a': case 'A': return do_action(ACT_ADD);
  case 'd': case 'D':
  case 0x7F:          return do_action(ACT_DELETE);
  case 'c': case 'C': return do_action(ACT_CALENDAR);
  case 's': case 'S': return do_action(ACT_STATS);
  default: return 0;
  }
}

static int key_stats(uint8_t k) {
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 0;
  switch (k) {
  case CAPP_KEY_UP:   if (H.stats_sel > 0) H.stats_sel--; clamp_stats_scroll(); return 1;
  case CAPP_KEY_DOWN: if (H.stats_sel + 1 < H.n) H.stats_sel++; clamp_stats_scroll(); return 1;
  case CAPP_KEY_ESC:  H.view = VIEW_LIST; return 1;
  default: return 0;
  }
}

static int key_day(uint8_t k) {
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN &&
      k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return 0;
  switch (k) {
  case CAPP_KEY_UP:   if (H.day_sel > 0) H.day_sel--; clamp_day_scroll(); return 1;
  case CAPP_KEY_DOWN: if (H.day_sel + 1 < H.n) H.day_sel++; clamp_day_scroll(); return 1;
  case CAPP_KEY_LEFT:
    date_add_day(&H.day_year, &H.day_month, &H.day_day, -1);
    day_recompute();
    return 1;
  case CAPP_KEY_RIGHT:
    date_add_day(&H.day_year, &H.day_month, &H.day_day, 1);
    day_recompute();
    return 1;
  case CAPP_KEY_ENTER:
  case ' ':           return do_action(ACT_TOGGLE);
  case CAPP_KEY_ESC:   H.view = H.day_back; return 1;
  default: return 0;
  }
}

static int key_cal(uint8_t k) {
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN &&
      k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return 0;
  switch (k) {
  case CAPP_KEY_LEFT:  cal_move(-1); return 1;
  case CAPP_KEY_RIGHT: cal_move(1); return 1;
  case CAPP_KEY_UP:    cal_move(-7); return 1;
  case CAPP_KEY_DOWN:  cal_move(7); return 1;
  case CAPP_KEY_ENTER:
    day_open_at(H.cal_year, H.cal_month, H.cal_sel, 1, VIEW_CALENDAR);
    return 1;
  case CAPP_KEY_ESC:   H.view = VIEW_LIST; return 1;
  default: return 0;
  }
}

static int key_add(uint8_t k) {
  if (k == CAPP_KEY_ENTER) return do_action(ACT_SAVE);
  if (k == CAPP_KEY_ESC) return do_action(ACT_CANCEL);
  if (k == CAPP_KEY_BACK) {
    if (H.draft_len > 0) { H.draft[--H.draft_len] = 0; return 1; }
    return do_action(ACT_CANCEL);
  }
  if (k >= 32 && k < 127 && k != '/' && H.draft_len < HABIT_NAME_MAX) {
    H.draft[H.draft_len++] = (char)k;
    H.draft[H.draft_len] = 0;
    return 1;
  }
  return 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (H.view == VIEW_ADD) return key_add(k);
  if (H.view == VIEW_DAY) return key_day(k);
  if (H.view == VIEW_CALENDAR) return key_cal(k);
  if (H.view == VIEW_STATS) return key_stats(k);
  return key_list(k);
}

/* Which day of the month a tap in the grid landed on, or 0 for none: the
 * inverse of paint_calendar's cell layout. */
static int cal_day_at(int16_t x, int16_t y) {
  int cell_w = H.content.w / 7;
  int grid_y = H.content.y + ROW_H + 9;
  int cell_h = (H.content.y + H.content.h - ROW_H - grid_y) / 6;
  int first = weekday_of_day(days_from_civil(H.cal_year, H.cal_month, 1));
  int len = days_in_month(H.cal_year, H.cal_month);
  int col, row, idx, d;

  if (y < grid_y || y >= H.content.y + H.content.h - ROW_H || cell_w <= 0 || cell_h <= 0) return 0;
  col = (x - H.content.x) / cell_w;
  row = (y - grid_y) / cell_h;
  if (col < 0 || col > 6 || row < 0 || row > 5) return 0;
  idx = row * 7 + col;
  d = idx - first + 1;
  return (d >= 1 && d <= len) ? d : 0;
}

static int app_click(void *st, int16_t x, int16_t y, int button) {
  int row;
  (void)st; (void)button;
  if (H.view == VIEW_CALENDAR) {
    int d = cal_day_at(x, y);
    if (!d) return 0;
    H.cal_sel = d;
    day_open_at(H.cal_year, H.cal_month, d, 1, VIEW_CALENDAR);
    return 1;
  }
  if (H.view == VIEW_DAY) {
    row = (y - H.content.y - ROW_H) / ROW_H;
    if (row < 0 || H.day_top + row >= H.n || y >= H.content.y + H.content.h - ROW_H) return 0;
    H.day_sel = H.day_top + row;
    day_toggle(H.day_sel);
    return 1;
  }
  if (H.view != VIEW_LIST) return 0;
  row = (y - H.content.y) / ROW_H;
  if (row < 0 || H.top + row >= H.n || y >= H.content.y + H.content.h - ROW_H) return 0;
  H.sel = H.top + row;
  clamp_scroll();
  return 1;
}

static int app_wants_text(void *st) { (void)st; return H.view == VIEW_ADD; }

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Habits",
  /* 16x16: a 3x3 grid of squares, a habit-tracker calendar in miniature. */
  { 0x00, 0x00, 0x00, 0x00, 0x3B, 0xB8, 0x3B, 0xB8,
    0x3B, 0xB8, 0x00, 0x00, 0x3B, 0xB8, 0x3B, 0xB8,
    0x3B, 0xB8, 0x00, 0x00, 0x3B, 0xB8, 0x3B, 0xB8,
    0x3B, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "arrows\tmove\na\tadd a habit\nd\tdelete\nenter\topen today / mark done / open day\n"
  "left/right\t(day view) previous/next day; (calendar) same, a week with up/down\n"
  "c\tcalendar\ns\tstats\nescape\tback\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  (void)argc; (void)argv;
  api->mem_set(&H, 0, sizeof H);
  api->mkdir(DIR);
  habits_load();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
