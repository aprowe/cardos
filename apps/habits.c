/* Habits: what you mean to do every day, and whether you did.
 *
 * ONE SCREEN FOR THE DAY. Every habit, a box to tick for the day on screen,
 * its last seven days as dots and its streak. Space ticks; left and right
 * walk back through the days (not forward past today), t comes back. That
 * is the whole daily use, and it used to take a trip into a separate view.
 *
 * i opens one habit: its streak large, its best, its total, how many of the
 * last 30 days were done, and eighteen weeks as a grid -- a column a week,
 * Sunday at the top -- with a cursor that can mark a day that was missed.
 * c is the same grid for every habit at once, shaded by how many were done.
 * a adds, r renames, d deletes (after asking), < and > move a habit up and
 * down the list.
 *
 * THE CARD, read once. The files are what they were, so nothing is lost
 * moving to this version:
 *
 *   /home/habits/habits.txt   the names, one a line, in order
 *   /home/habits/NAME.log     that habit's done days, YYYYMMDD, one a line
 *
 * Every log is read when the app opens, into a bitmap of the last 371 days
 * (53 weeks) per habit -- 47 bytes each -- and never read again while it is
 * open: a keypress used to read every habit's log from the card, twenty
 * reads for an arrow key. Days outside that window are counted but not held;
 * when a log is saved they are read back from the file and written out
 * unchanged, so years of history survive being edited from here. Saves go
 * through apps/safefile.h: a power cut leaves the old file or the new one.
 *
 * A name is also a file name, so the characters the card cannot have in one
 * are refused when it is typed, and two names that differ only in case are
 * the same name -- the card thinks so.
 *
 * Marking needs a date. With no clock (api->now not synced) the screen still
 * shows everything but says so, and will not mark: a tick on the wrong day
 * is worse than none. If the app is left open past midnight the window moves
 * on by itself (tick checks the date).
 *
 * Text is Atkinson Hyperlegible at 13 px (ui13, ui13b) and the streak in the
 * detail view Space Mono (num30); all from fonts/fonts.txt, all falling back
 * to the 6x8 font if the card has not got them.
 *
 * Commands (the same data as the screen): add NAME, done HABIT, list.
 */

#include "kernel/app/capp.h"
#include "apps/toolbar.h"
#include "apps/safefile.h"

static const CardApi *api;

#define DIR          CAPP_HOME "/habits"
#define HABITS_PATH  DIR "/habits.txt"

#define MAX_HABITS   20
#define NAME_MAX     24
#define WIN          371                  /* days held in memory: 53 weeks */
#define WIN_BYTES    ((WIN + 7) / 8)
#define KEEP_MAX     512                  /* days outside the window a save carries over */
#define HEAT_WEEKS   18

#define ROW_H        18
#define HEAD_H       20
#define FOOT_H       11

#define CLR_BG       CAPP_RGB(15, 17, 23)
#define CLR_TEXT     CAPP_RGB(232, 236, 244)
#define CLR_DIM      CAPP_RGB(128, 136, 152)
#define CLR_FAINT    CAPP_RGB(60, 66, 80)
#define CLR_SEL      CAPP_RGB(30, 40, 58)
#define CLR_DONE     CAPP_RGB(76, 196, 128)
#define CLR_DONE_DIM CAPP_RGB(44, 110, 76)
#define CLR_CELL     CAPP_RGB(32, 36, 46)
#define CLR_STREAK   CAPP_RGB(255, 176, 76)
#define CLR_BARTRACK CAPP_RGB(34, 38, 48)
#define CLR_FOOT     CAPP_RGB(30, 34, 44)
#define CLR_WARN     CAPP_RGB(236, 104, 84)
#define CLR_FIELD    CAPP_RGB(28, 32, 42)
#define CLR_CURSOR   CAPP_RGB(240, 244, 250)

typedef struct {
  char     name[NAME_MAX + 1];
  uint8_t  bits[WIN_BYTES];     /* bit k: day (win_start + k) was done */
  uint16_t outside;             /* done days in the file outside the window */
} Habit;

enum { VIEW_TODAY = 0, VIEW_DETAIL, VIEW_PROMPT };
enum { ASK_NONE = 0, ASK_DELETE };
enum { PROMPT_ADD = 0, PROMPT_RENAME };

static struct {
  Habit habit[MAX_HABITS];
  int   n;

  int32_t today;                /* a day number (days since 1970-01-01) */
  int32_t win_start;            /* the day bit 0 is */
  int     clock_ok;             /* today is a real date */

  int     view;
  int     sel, top, rows;
  int32_t day;                  /* the day the main screen shows */
  int     ask;

  int     detail;               /* the habit the detail view shows, -1 all */
  int32_t cursor;               /* its grid's selected day */

  int     prompt;               /* PROMPT_* */
  char    draft[NAME_MAX + 1];
  int     draft_len;
  const char *err;              /* why the draft was refused, or NULL */

  uint32_t checked_at;          /* tick: last look at the date */
  int     f_ui, f_uib, f_num;
  CRect   content;
} H;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* ---- dates -------------------------------------------------------------------
 *
 * Howard Hinnant's civil-date algorithm, as apps/calendar.c uses: a date to
 * a day count from 1970-01-01 and back. Every date here is a day number;
 * YYYYMMDD exists only in the files. */

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

/* 0 = Sunday; 1970-01-01 was a Thursday. */
static int weekday(int32_t z) {
  int32_t w = (z + 4) % 7;
  return (int)(w < 0 ? w + 7 : w);
}

/* "20260924" -> day number, or 0 with *ok 0 for anything that is not a date. */
static int32_t parse_ymd(const char *s, int *ok) {
  int i, y = 0, m = 0, d = 0;
  *ok = 0;
  for (i = 0; i < 8; i++) if (s[i] < '0' || s[i] > '9') return 0;
  if (s[8] >= '0' && s[8] <= '9') return 0;
  for (i = 0; i < 4; i++) y = y * 10 + (s[i] - '0');
  m = (s[4] - '0') * 10 + (s[5] - '0');
  d = (s[6] - '0') * 10 + (s[7] - '0');
  if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1900) return 0;
  *ok = 1;
  return days_from_civil(y, m, d);
}

static void format_ymd(int32_t z, char *out, size_t n) {
  int y, m, d;
  civil_from_days(z, &y, &m, &d);
  api->fmt(out, n, "%04d%02d%02d", y, m, d);
}

static const char *const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/* "Today", "Yesterday", or "Mon 22 Sep". */
static void day_label(int32_t z, char *out, size_t n) {
  int y, m, d;
  if (z == H.today) { api->fmt(out, n, "Today"); return; }
  if (z == H.today - 1) { api->fmt(out, n, "Yesterday"); return; }
  civil_from_days(z, &y, &m, &d);
  api->fmt(out, n, "%s %d %s", WDAY[weekday(z)], d, MON[m - 1]);
}

/* ---- the window: one bit a day ----------------------------------------------- */

static int in_window(int32_t z) { return z >= H.win_start && z < H.win_start + WIN; }

static int is_done(int i, int32_t z) {
  int k;
  if (!in_window(z)) return 0;
  k = (int)(z - H.win_start);
  return (H.habit[i].bits[k >> 3] >> (k & 7)) & 1;
}

static void set_done(int i, int32_t z, int on) {
  int k;
  if (!in_window(z)) return;
  k = (int)(z - H.win_start);
  if (on) H.habit[i].bits[k >> 3] |= (uint8_t)(1u << (k & 7));
  else    H.habit[i].bits[k >> 3] &= (uint8_t)~(1u << (k & 7));
}

/* Put the window's last day on `today`: an open app at midnight. Days that
 * slide off the old end are still in the files; they become `outside`. */
static void anchor(int32_t today) {
  int32_t start = today - (WIN - 1), shift;
  int i, j;
  if (!H.n || H.today == 0) { H.today = today; H.win_start = start; return; }
  shift = start - H.win_start;
  if (shift == 0) { H.today = today; return; }
  for (i = 0; i < H.n; i++) {
    uint8_t old[WIN_BYTES];
    api->mem_cpy(old, H.habit[i].bits, sizeof old);
    api->mem_set(H.habit[i].bits, 0, sizeof old);
    for (j = 0; j < WIN; j++) {
      int32_t k = (int32_t)j - shift;           /* where old bit j lands */
      if (!((old[j >> 3] >> (j & 7)) & 1)) continue;
      if (k >= 0 && k < WIN) H.habit[i].bits[k >> 3] |= (uint8_t)(1u << (k & 7));
      else if (H.habit[i].outside < 0xFFFF) H.habit[i].outside++;
    }
  }
  H.today = today;
  H.win_start = start;
}

/* ---- numbers ------------------------------------------------------------------ */

/* Done days in a row ending at `z`. */
static int run_back(int i, int32_t z) {
  int n = 0;
  while (in_window(z - n) && is_done(i, z - n)) n++;
  return n;
}

/* The streak as a person counts it: today counts once it is done, and until
 * then yesterday's streak is still alive -- it is not broken at 00:01. */
static int streak(int i) {
  return is_done(i, H.today) ? run_back(i, H.today) : run_back(i, H.today - 1);
}

static int best_streak(int i) {
  int best = 0, cur = 0, k;
  for (k = 0; k < WIN; k++) {
    if ((H.habit[i].bits[k >> 3] >> (k & 7)) & 1) { if (++cur > best) best = cur; }
    else cur = 0;
  }
  return best;
}

static int total_done(int i) {
  int t = H.habit[i].outside, k;
  for (k = 0; k < WIN; k++) t += (H.habit[i].bits[k >> 3] >> (k & 7)) & 1;
  return t;
}

/* Of the `days` ending today, how many were done: 0..100. */
static int rate(int i, int days) {
  int d, done = 0;
  for (d = 0; d < days; d++) done += is_done(i, H.today - d);
  return days ? done * 100 / days : 0;
}

static int done_on(int32_t z) {
  int i, n = 0;
  for (i = 0; i < H.n; i++) n += is_done(i, z);
  return n;
}

/* ---- the card ------------------------------------------------------------------- */

static void log_path(const char *name, char *out, size_t n) {
  api->fmt(out, n, DIR "/%s.log", name);
}

/* One line at a time out of a file, for both kinds. */
typedef void (*LineFn)(const char *line, void *ctx);

static void read_lines(const char *path, LineFn fn, void *ctx) {
  char buf[256], line[40];
  int fd, n, i, len = 0;
  fd = safe_open_read(api, path);
  if (fd < 0) return;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      if (buf[i] == '\r') continue;
      if (buf[i] != '\n') { if (len < (int)sizeof line - 1) line[len++] = buf[i]; continue; }
      line[len] = 0;
      len = 0;
      fn(line, ctx);
    }
  }
  if (len) { line[len] = 0; fn(line, ctx); }
  api->close(fd);
}

static void name_line(const char *line, void *ctx) {
  (void)ctx;
  if (line[0] && H.n < MAX_HABITS) {
    api->mem_set(&H.habit[H.n], 0, sizeof H.habit[0]);
    api->fmt(H.habit[H.n].name, sizeof H.habit[0].name, "%s", line);
    H.n++;
  }
}

static void date_line(const char *line, void *ctx) {
  int i = *(int *)ctx, ok;
  int32_t z = parse_ymd(line, &ok);
  if (!ok) return;
  if (in_window(z)) {
    if (!is_done(i, z)) set_done(i, z, 1);       /* a date twice is one day */
  } else if (H.habit[i].outside < 0xFFFF) {
    H.habit[i].outside++;
  }
}

static void load_all(void) {
  int i;
  H.n = 0;
  read_lines(HABITS_PATH, name_line, 0);
  for (i = 0; i < H.n; i++) {
    char path[64];
    log_path(H.habit[i].name, path, sizeof path);
    read_lines(path, date_line, &i);
  }
}

static int save_names(void) {
  SafeFile f;
  int i;
  api->mkdir(DIR);
  if (safe_begin(&f, api, HABITS_PATH) != 0) return -1;
  for (i = 0; i < H.n; i++) {
    safe_line(&f, H.habit[i].name);
    safe_line(&f, "\n");
  }
  return safe_commit(&f);
}

/* The days outside the window, as the file has them, for a save to carry
 * over. Sorted on the way out; a file edited by hand need not be. */
static int32_t s_keep[KEEP_MAX];
static int     s_nkeep;

static void keep_line(const char *line, void *ctx) {
  int ok;
  int32_t z = parse_ymd(line, &ok);
  (void)ctx;
  if (!ok || in_window(z) || s_nkeep >= KEEP_MAX) return;
  s_keep[s_nkeep++] = z;
}

static void write_day(SafeFile *f, int32_t z) {
  char s[16];
  format_ymd(z, s, sizeof s);
  safe_line(f, s);
  safe_line(f, "\n");
}

/* Habit i's log: what was outside the window, as it was, and the window as
 * it is now, oldest first. 0, or -1 with the old file left alone. */
static int save_log(int i) {
  SafeFile f;
  char path[64];
  int a, b, k;
  log_path(H.habit[i].name, path, sizeof path);
  s_nkeep = 0;
  read_lines(path, keep_line, 0);
  for (a = 1; a < s_nkeep; a++)                     /* insertion sort: small, mostly sorted */
    for (b = a; b > 0 && s_keep[b - 1] > s_keep[b]; b--) {
      int32_t t = s_keep[b]; s_keep[b] = s_keep[b - 1]; s_keep[b - 1] = t;
    }
  api->mkdir(DIR);
  if (safe_begin(&f, api, path) != 0) return -1;
  for (a = 0; a < s_nkeep && s_keep[a] < H.win_start; a++) write_day(&f, s_keep[a]);
  for (k = 0; k < WIN; k++)
    if ((H.habit[i].bits[k >> 3] >> (k & 7)) & 1) write_day(&f, H.win_start + k);
  for (; a < s_nkeep; a++) write_day(&f, s_keep[a]);
  return safe_commit(&f);
}

/* ---- changing things --------------------------------------------------------------- */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int same_name(const char *a, const char *b) {
  while (*a && lower(*a) == lower(*b)) { a++; b++; }
  return !*a && !*b;
}

/* Why `name` cannot be a habit, or NULL if it can. `except` is the habit
 * being renamed, which may keep its own name in another case. */
static const char *name_problem(const char *name, int except) {
  int i, len = 0, blank = 1;
  for (i = 0; name[i]; i++) {
    char c = name[i];
    len++;
    if (c != ' ') blank = 0;
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
        c == '<' || c == '>' || c == '|' || (unsigned char)c < 32)
      return "no / \\ : * ? \" < > | -- it is a file name too";
  }
  if (!len || blank) return "a habit needs a name";
  if (len > NAME_MAX) return "that is longer than 24 characters";
  if (name[0] == ' ' || name[len - 1] == ' ' || name[len - 1] == '.')
    return "no space or dot at either end";
  for (i = 0; i < H.n; i++)
    if (i != except && same_name(H.habit[i].name, name)) return "there is one called that";
  return NULL;
}

/* The index of the new habit, or -1 with *why set. */
static int add_habit(const char *name, const char **why) {
  const char *p = name_problem(name, -1);
  Habit *h;
  if (!p && H.n >= MAX_HABITS) p = "that is 20 habits already";
  if (p) { if (why) *why = p; return -1; }
  h = &H.habit[H.n];
  api->mem_set(h, 0, sizeof *h);
  api->fmt(h->name, sizeof h->name, "%s", name);
  H.n++;
  if (save_names() != 0) {
    H.n--;
    if (why) *why = "could not write to the card";
    return -1;
  }
  return H.n - 1;
}

static int rename_habit(int i, const char *name, const char **why) {
  char from[64], to[64], old[NAME_MAX + 1];
  CappStat st;
  const char *p = name_problem(name, i);
  if (p) { if (why) *why = p; return -1; }
  log_path(H.habit[i].name, from, sizeof from);
  log_path(name, to, sizeof to);
  if (!same_name(H.habit[i].name, name) && api->stat(to, &st) == 0) {
    if (why) *why = "a log with that name is on the card";
    return -1;
  }
  if (api->stat(from, &st) == 0 && api->rename(from, to) != 0) {
    if (why) *why = "could not rename its log";
    return -1;
  }
  api->fmt(old, sizeof old, "%s", H.habit[i].name);
  api->fmt(H.habit[i].name, sizeof H.habit[i].name, "%s", name);
  if (save_names() != 0) {
    api->fmt(H.habit[i].name, sizeof H.habit[i].name, "%s", old);
    api->rename(to, from);
    if (why) *why = "could not write to the card";
    return -1;
  }
  return 0;
}

static void delete_habit(int i) {
  char path[64];
  int k;
  if (i < 0 || i >= H.n) return;
  log_path(H.habit[i].name, path, sizeof path);
  api->remove(path);
  for (k = i; k + 1 < H.n; k++) api->mem_cpy(&H.habit[k], &H.habit[k + 1], sizeof(Habit));
  H.n--;
  save_names();
}

static void move_habit(int i, int dir) {
  Habit t;
  int j = i + dir;
  if (i < 0 || j < 0 || i >= H.n || j >= H.n) return;
  api->mem_cpy(&t, &H.habit[i], sizeof t);
  api->mem_cpy(&H.habit[i], &H.habit[j], sizeof t);
  api->mem_cpy(&H.habit[j], &t, sizeof t);
  save_names();
}

/* Flip day z for habit i and write its log. -1 if it cannot be: no clock, a
 * day in the future, or outside the year this holds. */
static int toggle(int i, int32_t z) {
  if (!H.clock_ok || i < 0 || i >= H.n || z > H.today || !in_window(z)) return -1;
  set_done(i, z, !is_done(i, z));
  if (save_log(i) != 0) {
    set_done(i, z, !is_done(i, z));            /* the card said no: undo it */
    return -1;
  }
  return 0;
}

/* ---- layout --------------------------------------------------------------------------- */

static CRect body_rect(void) {
  return rect(H.content.x, H.content.y + HEAD_H, H.content.w,
              H.content.h - HEAD_H - FOOT_H);
}

static CRect row_rect(int i) {
  CRect b = body_rect();
  return rect(b.x, b.y + (i - H.top) * ROW_H, b.w, ROW_H);
}

static void clamp_sel(void) {
  CRect b = body_rect();
  H.rows = b.h / ROW_H;
  if (H.rows < 1) H.rows = 1;
  if (H.sel >= H.n) H.sel = H.n - 1;
  if (H.sel < 0) H.sel = 0;
  if (H.sel < H.top) H.top = H.sel;
  if (H.sel >= H.top + H.rows) H.top = H.sel - H.rows + 1;
  if (H.top < 0) H.top = 0;
}

static int font_y(int f, int y, int h) { return y + (h - api->font_height(f)) / 2; }

/* ---- the day screen ---------------------------------------------------------------------- */

static void paint_check(int x, int y, int on, uint16_t bg) {
  api->fill(rect(x, y, 11, 11), on ? CLR_DONE : CLR_FAINT);
  if (!on) { api->fill(rect(x + 1, y + 1, 9, 9), bg); return; }
  /* a tick: two strokes, two pixels thick */
  api->fill(rect(x + 2, y + 5, 2, 2), CLR_BG);
  api->fill(rect(x + 3, y + 6, 2, 2), CLR_BG);
  api->fill(rect(x + 4, y + 7, 2, 2), CLR_BG);
  api->fill(rect(x + 5, y + 6, 2, 2), CLR_BG);
  api->fill(rect(x + 6, y + 5, 2, 2), CLR_BG);
  api->fill(rect(x + 7, y + 4, 2, 2), CLR_BG);
  api->fill(rect(x + 8, y + 3, 2, 2), CLR_BG);
}

static void paint_row(int i) {
  CRect r = row_rect(i);
  int sel = i == H.sel, k, x, s;
  uint16_t bg = sel ? CLR_SEL : CLR_BG;
  char num[8];

  api->fill(r, bg);
  paint_check(r.x + 8, r.y + (ROW_H - 11) / 2, is_done(i, H.day), bg);
  api->text_font(H.f_ui, (int16_t)(r.x + 26), (int16_t)font_y(H.f_ui, r.y, ROW_H),
                 H.habit[i].name, CLR_TEXT, bg);

  /* the streak, then seven days ending on the day shown, oldest left */
  s = streak(i);
  api->fmt(num, sizeof num, "%d", s);
  x = r.x + r.w - 8 - api->text_width(H.f_uib, num);
  api->text_font(H.f_uib, (int16_t)x, (int16_t)font_y(H.f_uib, r.y, ROW_H), num,
                 s ? CLR_STREAK : CLR_FAINT, bg);
  x -= 8 + 7 * 7;
  for (k = 6; k >= 0; k--) {
    int32_t z = H.day - k;
    int on = is_done(i, z);
    api->fill(rect(x, r.y + (ROW_H - 5) / 2, 5, 5),
              on ? (k ? CLR_DONE_DIM : CLR_DONE) : CLR_CELL);
    x += 7;
  }
}

static void paint_head(void) {
  CRect h = rect(H.content.x, H.content.y, H.content.w, HEAD_H);
  char label[24], frac[12];
  int done = done_on(H.day), w;
  api->fill(rect(h.x, h.y, h.w, h.h - 2), CLR_BG);
  day_label(H.day, label, sizeof label);
  api->text_font(H.f_uib, (int16_t)(h.x + 8), (int16_t)font_y(H.f_uib, h.y, h.h - 2), label,
                 CLR_TEXT, CLR_BG);
  if (!H.clock_ok)
    api->text_font(H.f_ui, (int16_t)(h.x + 8 + api->text_width(H.f_uib, label) + 8),
                   (int16_t)font_y(H.f_ui, h.y, h.h - 2), "clock not set", CLR_WARN, CLR_BG);
  api->fmt(frac, sizeof frac, "%d/%d", done, H.n);
  w = api->text_width(H.f_uib, frac);
  api->text_font(H.f_uib, (int16_t)(h.x + h.w - 8 - w), (int16_t)font_y(H.f_uib, h.y, h.h - 2),
                 frac, H.n && done == H.n ? CLR_DONE : CLR_DIM, CLR_BG);
  /* the day's progress, a line under the header */
  api->fill(rect(h.x, h.y + h.h - 2, h.w, 2), CLR_BARTRACK);
  if (H.n) api->fill(rect(h.x, h.y + h.h - 2, h.w * done / H.n, 2), CLR_DONE);
}

static void paint_foot(const char *hint, uint16_t fg) {
  CRect f = rect(H.content.x, H.content.y + H.content.h - FOOT_H, H.content.w, FOOT_H);
  api->fill(f, CLR_FOOT);
  api->text((int16_t)(f.x + 4), (int16_t)(f.y + 2), hint, fg, CLR_FOOT);
}

static void paint_today(void) {
  CRect b = body_rect();
  int i, y;
  clamp_sel();
  paint_head();
  if (!H.n) {
    api->fill(b, CLR_BG);
    api->text_font(H.f_uib, (int16_t)(b.x + 8), (int16_t)(b.y + 8), "No habits yet",
                   CLR_TEXT, CLR_BG);
    api->text_font(H.f_ui, (int16_t)(b.x + 8), (int16_t)(b.y + 26),
                   "a adds one: read, stretch, water...", CLR_DIM, CLR_BG);
  } else {
    for (i = H.top; i < H.n && i < H.top + H.rows; i++) paint_row(i);
    y = b.y + (i - H.top) * ROW_H;
    if (y < b.y + b.h) api->fill(rect(b.x, y, b.w, b.y + b.h - y), CLR_BG);
  }
  if (H.ask == ASK_DELETE) {
    char q[48];
    api->fmt(q, sizeof q, "delete %s and its log?  y/n", H.habit[H.sel].name);
    paint_foot(q, CLR_WARN);
  } else {
    paint_foot("spc done </> day i info c all a add", CLR_DIM);
  }
}

/* ---- the grid: one habit, or all of them ----------------------------------------------- */

/* The first day (a Sunday) of the leftmost column: the grid ends with the
 * week holding today. */
static int32_t grid_start(void) {
  return H.today - weekday(H.today) - 7 * (HEAT_WEEKS - 1);
}

#define CELL 6
#define GAP  1

static uint16_t lerp(int r0, int g0, int b0, int r1, int g1, int b1, int t, int tmax) {
  return CAPP_RGB(r0 + (r1 - r0) * t / tmax, g0 + (g1 - g0) * t / tmax,
                  b0 + (b1 - b0) * t / tmax);
}

static uint16_t cell_colour(int32_t z) {
  int n;
  if (z > H.today) return CLR_BG;
  if (H.detail >= 0) return is_done(H.detail, z) ? CLR_DONE : CLR_CELL;
  if (!H.n) return CLR_CELL;
  n = done_on(z);
  if (!n) return CLR_CELL;
  return lerp(36, 70, 52, 76, 196, 128, n, H.n);
}

static CRect grid_rect(void) {
  int w = HEAT_WEEKS * (CELL + GAP) - GAP, h = 7 * (CELL + GAP) - GAP;
  return rect(H.content.x + H.content.w - 8 - w, H.content.y + HEAD_H + 4, w, h);
}

static void paint_cell(int32_t z) {
  CRect g = grid_rect();
  int col = (int)((z - grid_start()) / 7), row = weekday(z);
  int x = g.x + col * (CELL + GAP), y = g.y + row * (CELL + GAP);
  if (col < 0 || col >= HEAT_WEEKS) return;
  if (z == H.cursor) {
    api->fill(rect(x - 1, y - 1, CELL + 2, CELL + 2), CLR_CURSOR);
    api->fill(rect(x + 1, y + 1, CELL - 2, CELL - 2), cell_colour(z));
  } else {
    api->fill(rect(x - 1, y - 1, CELL + 2, CELL + 2), CLR_BG);
    api->fill(rect(x, y, CELL, CELL), cell_colour(z));
  }
}

static void paint_detail(void) {
  CRect c = H.content, g = grid_rect();
  int32_t z, start = grid_start();
  char s[48];
  int x = c.x + 8, y, one = H.detail >= 0;
  int lh = api->font_height(H.f_ui);

  api->fill(c, CLR_BG);
  api->text_font(H.f_uib, (int16_t)x, (int16_t)font_y(H.f_uib, c.y, HEAD_H),
                 one ? H.habit[H.detail].name : "All habits", CLR_TEXT, CLR_BG);
  api->fill(rect(c.x, c.y + HEAD_H - 2, c.w, 2), CLR_BARTRACK);

  y = c.y + HEAD_H + 4;
  if (one) {
    int st = streak(H.detail);
    api->fmt(s, sizeof s, "%d", st);
    api->text_font(H.f_num, (int16_t)x, (int16_t)y, s, st ? CLR_STREAK : CLR_FAINT, CLR_BG);
    api->text_font(H.f_ui, (int16_t)(x + api->text_width(H.f_num, s) + 6),
                   (int16_t)(y + api->font_height(H.f_num) - lh), st == 1 ? "day" : "days",
                   CLR_DIM, CLR_BG);
    y += api->font_height(H.f_num) + 4;
    api->fmt(s, sizeof s, "best %d", best_streak(H.detail));
    api->text_font(H.f_ui, (int16_t)x, (int16_t)y, s, CLR_TEXT, CLR_BG); y += lh;
    api->fmt(s, sizeof s, "total %d", total_done(H.detail));
    api->text_font(H.f_ui, (int16_t)x, (int16_t)y, s, CLR_TEXT, CLR_BG); y += lh;
    api->fmt(s, sizeof s, "30 days %d%%", rate(H.detail, 30));
    api->text_font(H.f_ui, (int16_t)x, (int16_t)y, s, CLR_TEXT, CLR_BG);
  } else {
    int perfect = 0, d, sum = 0;
    for (d = 0; d < 30; d++) {
      int n = done_on(H.today - d);
      sum += n;
      if (H.n && n == H.n) perfect++;
    }
    api->fmt(s, sizeof s, "%d/%d", done_on(H.today), H.n);
    api->text_font(H.f_num, (int16_t)x, (int16_t)y, s, CLR_DONE, CLR_BG);
    api->text_font(H.f_ui, (int16_t)(x + api->text_width(H.f_num, s) + 6),
                   (int16_t)(y + api->font_height(H.f_num) - lh), "today", CLR_DIM, CLR_BG);
    y += api->font_height(H.f_num) + 4;
    api->fmt(s, sizeof s, "all done %d of 30", perfect);
    api->text_font(H.f_ui, (int16_t)x, (int16_t)y, s, CLR_TEXT, CLR_BG); y += lh;
    api->fmt(s, sizeof s, "30 days %d%%", H.n ? sum * 100 / (30 * H.n) : 0);
    api->text_font(H.f_ui, (int16_t)x, (int16_t)y, s, CLR_TEXT, CLR_BG);
  }

  for (z = start; z <= H.today; z++) paint_cell(z);

  /* the cursor's day, under the grid */
  {
    char label[24];
    day_label(H.cursor, label, sizeof label);
    if (one) api->fmt(s, sizeof s, "%s  %s", label, is_done(H.detail, H.cursor) ? "done" : "--");
    else api->fmt(s, sizeof s, "%s  %d/%d", label, done_on(H.cursor), H.n);
    api->text_font(H.f_ui, (int16_t)g.x, (int16_t)(g.y + g.h + 4), s, CLR_DIM, CLR_BG);
  }
  paint_foot(one ? "arrows move  spc mark  n/p habit  esc" : "arrows move  esc back", CLR_DIM);
}

/* ---- the name prompt ------------------------------------------------------------------------ */

static void paint_prompt(void) {
  CRect c = H.content;
  char shown[NAME_MAX + 2];
  int y = c.y + 10;
  api->fill(c, CLR_BG);
  api->text_font(H.f_uib, (int16_t)(c.x + 8), (int16_t)y,
                 H.prompt == PROMPT_ADD ? "New habit" : "Rename", CLR_TEXT, CLR_BG);
  y += api->font_height(H.f_uib) + 6;
  api->fill(rect(c.x + 6, y, c.w - 12, 22), CLR_FIELD);
  api->fmt(shown, sizeof shown, "%s_", H.draft);
  api->text_font(H.f_ui, (int16_t)(c.x + 12), (int16_t)font_y(H.f_ui, y, 22), shown,
                 CLR_TEXT, CLR_FIELD);
  y += 28;
  if (H.err) api->text_font(H.f_ui, (int16_t)(c.x + 8), (int16_t)y, H.err, CLR_WARN, CLR_BG);
  paint_foot("enter saves  esc cancels", CLR_DIM);
}

static void app_paint(void *st, CRect full) {
  (void)st;
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  H.content = toolbar_rest(full);
  if (H.view == VIEW_DETAIL) paint_detail();
  else if (H.view == VIEW_PROMPT) paint_prompt();
  else paint_today();
  toolbar_paint_menu(full);
}

/* ---- actions ----------------------------------------------------------------------------------- */

enum { ACT_ADD = 1, ACT_DONE, ACT_LIST, ACT_RENAME, ACT_DELETE, ACT_INFO, ACT_ALL,
       ACT_TODAY, ACT_UP, ACT_DOWN };

static const CappParam P_HABIT[] = { { "habit", CAPP_ARG_TEXT, "the habit, or enough of its name" } };
static const CappParam P_NEW[]   = { { "name",  CAPP_ARG_TEXT, "what the habit is called" } };

static const CappAction ACTIONS[] = {
  { "add",    "Add habit",    "Habit", 0, ACT_ADD,
    "start tracking a new habit", P_NEW, 1, CAPP_CMD_YES },
  { "done",   "Done today",   0,       0, ACT_DONE,
    "mark a habit done today", P_HABIT, 1, CAPP_CMD_YES },
  { "list",   "List",         0,       0, ACT_LIST,
    "every habit, whether it is done today, and its streak", 0, 0, CAPP_CMD_YES },
  { "rename", "Rename",       "Habit", 0, ACT_RENAME },
  { "delete", "Delete...",    "Habit", 0, ACT_DELETE },
  { "up",     "Move up",      "Habit", 0, ACT_UP },
  { "down",   "Move down",    "Habit", 0, ACT_DOWN },
  { "info",   "Details",      "View",  0, ACT_INFO },
  { "all",    "All habits",   "View",  0, ACT_ALL },
  { "today",  "Today",        "View",  0, ACT_TODAY },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static void open_prompt(int kind) {
  H.prompt = kind;
  H.err = NULL;
  if (kind == PROMPT_RENAME && H.n) {
    api->fmt(H.draft, sizeof H.draft, "%s", H.habit[H.sel].name);
    H.draft_len = (int)api->str_len(H.draft);
  } else {
    H.draft[0] = 0;
    H.draft_len = 0;
  }
  H.view = VIEW_PROMPT;
}

static void open_detail(int which) {
  H.detail = which;
  H.cursor = H.day;
  H.view = VIEW_DETAIL;
}

static int do_action(int a) {
  switch (a) {
  case ACT_ADD:    open_prompt(PROMPT_ADD); return 1;
  case ACT_RENAME: if (H.n) open_prompt(PROMPT_RENAME); return 1;
  case ACT_DELETE: if (H.n) { H.view = VIEW_TODAY; H.ask = ASK_DELETE; } return 1;
  case ACT_UP:     if (H.sel > 0) { move_habit(H.sel, -1); H.sel--; } return 1;
  case ACT_DOWN:   if (H.sel + 1 < H.n) { move_habit(H.sel, 1); H.sel++; } return 1;
  case ACT_INFO:   if (H.n) open_detail(H.sel); return 1;
  case ACT_ALL:    open_detail(-1); return 1;
  case ACT_TODAY:  H.view = VIEW_TODAY; H.day = H.today; return 1;
  default:         return 0;
  }
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

/* Case-blind: is `needle` in `hay`? */
static int contains(const char *hay, const char *needle) {
  int i, j;
  for (i = 0; hay[i]; i++) {
    for (j = 0; needle[j] && lower(hay[i + j]) == lower(needle[j]); j++) {}
    if (!needle[j]) return 1;
  }
  return 0;
}

/* The one habit `word` picks out: an exact name first, else the only one
 * containing it. -1 with a reason. */
static int find_habit(const char *word, char *out, size_t n) {
  int i, hit = -1, hits = 0;
  for (i = 0; i < H.n; i++) if (same_name(H.habit[i].name, word)) return i;
  for (i = 0; i < H.n; i++) if (contains(H.habit[i].name, word)) { hit = i; hits++; }
  if (hits == 1) return hit;
  if (!hits) api->fmt(out, n, "no habit matches \"%s\"", word);
  else api->fmt(out, n, "%d habits match \"%s\"; say more", hits, word);
  return -1;
}

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  const char *why = NULL;
  size_t o = 0;
  int i;
  (void)st; (void)argc;
  switch (action) {
  case ACT_ADD:
    if ((i = add_habit(argv[0], &why)) < 0) { api->fmt(out, n, "%s", why); return -1; }
    api->fmt(out, n, "tracking %s", H.habit[i].name);
    return 0;
  case ACT_DONE:
    if (!H.clock_ok) { api->fmt(out, n, "the clock is not set, so which day is today?"); return -1; }
    if ((i = find_habit(argv[0], out, n)) < 0) return -1;
    if (is_done(i, H.today)) {
      api->fmt(out, n, "%s was already done today (%d day streak)", H.habit[i].name, streak(i));
      return 0;
    }
    if (toggle(i, H.today) != 0) { api->fmt(out, n, "could not write to the card"); return -1; }
    api->fmt(out, n, "%s: done today, %d day streak", H.habit[i].name, streak(i));
    return 0;
  case ACT_LIST:
    if (!H.n) { api->fmt(out, n, "no habits yet"); return 0; }
    for (i = 0; i < H.n && o + 8 < n; i++)
      o += (size_t)api->fmt(out + o, n - o, "%s- %s: %s, %d day streak", i ? "\n" : "",
                            H.habit[i].name,
                            is_done(i, H.today) ? "done today" : "not yet today", streak(i));
    return 0;
  }
  api->fmt(out, n, "habits has no command %d", action);
  return -1;
}

/* ---- keys ------------------------------------------------------------------------------------------ */

static void damage_row(int i) {
  if (i >= H.top && i < H.top + H.rows) api->damage(row_rect(i));
}

static void damage_head(void) {
  api->damage(rect(H.content.x, H.content.y, H.content.w, HEAD_H));
}

static int key_today(uint8_t k) {
  if (H.ask == ASK_DELETE) {
    if (api->key_repeat()) return 1;
    if (k == 'y' || k == 'Y' || k == CAPP_KEY_ENTER) delete_habit(H.sel);
    else if (!(k == 'n' || k == 'N' || k == CAPP_KEY_ESC || k == CAPP_KEY_BACK)) return 1;
    H.ask = ASK_NONE;
    return 1;
  }
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN &&
      k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return 1;

  switch (k) {
  case CAPP_KEY_UP:
  case CAPP_KEY_DOWN: {
    int was = H.sel, top = H.top;
    H.sel += k == CAPP_KEY_UP ? -1 : 1;
    clamp_sel();
    if (H.top == top) { damage_row(was); damage_row(H.sel); }
    return 1;
  }
  case CAPP_KEY_LEFT:
    if (H.day - 1 >= H.win_start) H.day--;
    return 1;
  case CAPP_KEY_RIGHT:
    if (H.day < H.today) H.day++;
    return 1;
  case ' ':
  case CAPP_KEY_ENTER:
    if (toggle(H.sel, H.day) == 0) { damage_row(H.sel); damage_head(); }
    return 1;
  case 't': case 'T': H.day = H.today; return 1;
  case 'a': case 'A': return do_action(ACT_ADD);
  case 'r': case 'R': return do_action(ACT_RENAME);
  case 'd': case 'D': case 0x7F: return do_action(ACT_DELETE);
  case 'i': case 'I': return do_action(ACT_INFO);
  case 'c': case 'C': return do_action(ACT_ALL);
  case '<':           return do_action(ACT_UP);
  case '>':           return do_action(ACT_DOWN);
  default:            return 0;
  }
}

/* The cell a day is drawn in, with the ring the cursor draws round it. */
static CRect cell_rect(int32_t z) {
  CRect g = grid_rect();
  int col = (int)((z - grid_start()) / 7), row = weekday(z);
  return rect(g.x + col * (CELL + GAP) - 1, g.y + row * (CELL + GAP) - 1, CELL + 2, CELL + 2);
}

static void move_cursor(int32_t by) {
  int32_t z = H.cursor + by, was = H.cursor;
  if (z < grid_start() || z > H.today) return;
  H.cursor = z;
  api->damage(cell_rect(was));             /* two cells, not a screen */
  api->damage(cell_rect(z));
}

static int key_detail(uint8_t k) {
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN &&
      k != CAPP_KEY_LEFT && k != CAPP_KEY_RIGHT) return 1;
  switch (k) {
  case CAPP_KEY_UP:    move_cursor(-1); break;
  case CAPP_KEY_DOWN:  move_cursor(1); break;
  case CAPP_KEY_LEFT:  move_cursor(-7); break;
  case CAPP_KEY_RIGHT: move_cursor(7); break;
  case ' ':
  case CAPP_KEY_ENTER:
    if (H.detail >= 0) toggle(H.detail, H.cursor);
    return 1;
  case 'n': case 'N':
    if (H.detail >= 0 && H.detail + 1 < H.n) { H.detail++; H.sel = H.detail; }
    return 1;
  case 'p': case 'P':
    if (H.detail > 0) { H.detail--; H.sel = H.detail; }
    return 1;
  case CAPP_KEY_ESC:
  case CAPP_KEY_BACK:
    H.view = VIEW_TODAY;
    return 1;
  default:
    return 0;
  }
  /* The cells were drawn in move_cursor; the line under the grid says which
   * day, and is all that is left to repaint. */
  {
    CRect g = grid_rect();
    api->damage(rect(g.x, g.y + g.h + 2, H.content.x + H.content.w - g.x,
                     api->font_height(H.f_ui) + 4));
  }
  return 1;
}

static int key_prompt(uint8_t k) {
  const char *why = NULL;
  if (k == CAPP_KEY_ESC) { H.view = VIEW_TODAY; return 1; }
  if (k == CAPP_KEY_ENTER) {
    if (H.prompt == PROMPT_ADD) {
      int i = add_habit(H.draft, &why);
      if (i >= 0) { H.sel = i; H.view = VIEW_TODAY; }
    } else if (rename_habit(H.sel, H.draft, &why) == 0) {
      H.view = VIEW_TODAY;
    }
    H.err = why;
    return 1;
  }
  if (k == CAPP_KEY_BACK) {
    if (H.draft_len) H.draft[--H.draft_len] = 0;
    else H.view = VIEW_TODAY;
    H.err = NULL;
    return 1;
  }
  if (k >= 32 && k < 127 && H.draft_len < NAME_MAX) {
    H.draft[H.draft_len++] = (char)k;
    H.draft[H.draft_len] = 0;
    H.err = NULL;
    return 1;
  }
  return 1;               /* a text field: nothing else typed should leak out */
}

static int app_key(void *st, uint8_t k) {
  int a = toolbar_key(k);
  (void)st;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  if (H.view == VIEW_PROMPT) return key_prompt(k);
  if (H.view == VIEW_DETAIL) return key_detail(k);
  return key_today(k);
}

static int app_click(void *st, int16_t x, int16_t y, int button) {
  int a, row;
  (void)st; (void)button;
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  if (H.view != VIEW_TODAY || H.ask) return 0;
  y = (int16_t)(y - toolbar_h() - HEAD_H);
  if (y < 0) return 0;
  row = H.top + y / ROW_H;
  if (row >= H.n || row >= H.top + H.rows) return 0;
  H.sel = row;
  if (x < 26) toggle(row, H.day);           /* on the box: tick it */
  return 1;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int ch;
  (void)st; (void)buttons; (void)wheel;
  ch = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) ch = 1;
  return ch;
}

static int app_wants_text(void *st) {
  (void)st;
  return !toolbar_has_keys() && H.view == VIEW_PROMPT;
}

/* ---- the date --------------------------------------------------------------------------------------- */

/* What day it is, and whether that is worth anything. */
static void read_clock(int32_t *today, int *ok) {
  CappTime t;
  api->now(&t);
  *ok = t.synced && t.year >= 2024;
  *today = *ok ? days_from_civil(t.year, t.month, t.day) : days_from_civil(2026, 1, 1);
}

/* Past midnight with the app open, or a clock that arrived after it opened:
 * move the window to the new today, once a few seconds. */
static int app_tick(void *st, uint32_t now) {
  int32_t today;
  int ok;
  (void)st;
  if (now - H.checked_at < 5000) return 0;
  H.checked_at = now;
  read_clock(&today, &ok);
  if (today == H.today && ok == H.clock_ok) return 0;
  if (ok && !H.clock_ok) {
    /* The window was placed on a guess; place it on the truth and read the
     * logs again into it. */
    H.today = 0;
    anchor(today);
    load_all();
  } else {
    int on_today = H.day == H.today;
    anchor(today);
    if (on_today || H.day < H.win_start) H.day = H.today;
  }
  H.clock_ok = ok;
  if (H.cursor > H.today || H.cursor < grid_start()) H.cursor = H.today;
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Habits",
  /* 16x16: a 3x3 grid of squares, a habit-tracker calendar in miniature. */
  { 0x00, 0x00, 0x00, 0x00, 0x3B, 0xB8, 0x3B, 0xB8,
    0x3B, 0xB8, 0x00, 0x00, 0x3B, 0xB8, 0x3B, 0xB8,
    0x3B, 0xB8, 0x00, 0x00, 0x3B, 0xB8, 0x3B, 0xB8,
    0x3B, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "space\tdone / not done\nleft/right\tthe day before / after\nt\tback to today\n"
  "i\ta habit's details and grid\nc\tevery habit's grid\na\tadd\nr\trename\n"
  "d\tdelete, asks first\n< >\tmove up / down\nn/p\t(details) next / previous habit\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  int32_t today;
  int ok;
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&H, 0, sizeof H);
  H.f_ui = H.f_uib = H.f_num = -1;
  H.detail = -1;
  read_clock(&today, &ok);
  H.clock_ok = ok;
  anchor(today);
  api->mkdir(DIR);
  load_all();
  H.day = H.today;
  H.cursor = H.today;
  H.checked_at = api->ticks_ms();
  if (!api->headless()) {
    H.f_ui  = api->font_load("ui13");
    H.f_uib = api->font_load("ui13b");
    H.f_num = api->font_load("num30");
    toolbar_init(api, ACTIONS, NACT, 0, 0);
  }

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
