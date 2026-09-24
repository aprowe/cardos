/* Habits on the host: the year of days it holds, the files it keeps, the
 * numbers it shows.
 *
 * The thing that must not happen is history lost: a log with two years in it
 * is loaded into a one-year window and then saved, and the older year has to
 * come back out exactly. Then the streak rules people notice when they are
 * wrong -- an unticked today does not break yesterday's streak -- and what
 * a name may be, since it is also a file name.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info habits_capp_info
#define capp_main habits_capp_main
#include "apps/habits.c"
#undef capp_info
#undef capp_main

static int h_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
static void *h_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *h_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t h_strlen(const char *s) { return strlen(s); }

static FILE *s_hf;

static void hn(const char *path, char *out, size_t n) {
  size_t j = 0, i;
  const char *pre = "habits_test";
  for (i = 0; pre[i] && j + 1 < n; i++) out[j++] = pre[i];
  for (i = 0; path[i] && j + 1 < n; i++) out[j++] = path[i] == '/' ? '_' : path[i];
  out[j] = 0;
}
static int h_open(const char *path, int flags) {
  char nm[128];
  hn(path, nm, sizeof nm);
  s_hf = fopen(nm, (flags & CAPP_O_WRITE) ? "wb" : "rb");
  return s_hf ? 3 : -1;
}
static int h_read(int fd, void *b, size_t n) { (void)fd; return (int)fread(b, 1, n, s_hf); }
static int h_write(int fd, const void *b, size_t n) { (void)fd; return (int)fwrite(b, 1, n, s_hf); }
static void h_close(int fd) { (void)fd; fclose(s_hf); s_hf = NULL; }
static int h_mkdir(const char *p) { (void)p; return 0; }
static int h_remove(const char *p) { char nm[128]; hn(p, nm, sizeof nm); return remove(nm); }
static int h_exists(const char *p) {
  char nm[128];
  FILE *t;
  hn(p, nm, sizeof nm);
  if ((t = fopen(nm, "rb")) == NULL) return 0;
  fclose(t);
  return 1;
}
static int h_rename(const char *a, const char *b) {
  char x[128], y[128];
  hn(a, x, sizeof x);
  hn(b, y, sizeof y);
  if (h_exists(b)) return -1;
  return rename(x, y) == 0 ? 0 : -1;
}
static int h_stat(const char *p, CappStat *st) {
  if (!h_exists(p)) return -1;
  if (st) memset(st, 0, sizeof *st);
  return 0;
}
static int s_y = 2026, s_m = 9, s_d = 24, s_synced = 2;
static void h_now(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->synced = (uint8_t)s_synced; t->year = (uint16_t)s_y;
  t->month = (uint8_t)s_m; t->day = (uint8_t)s_d;
}
static uint32_t h_ticks(void) { return 0; }
static int h_repeat(void) { return 0; }
static int h_headless(void) { return 1; }
static void h_damage(CRect r) { (void)r; }
static int h_font_height(int f) { (void)f; return 8; }

static CardApi HF;

static void put(const char *path, const char *text) {
  char nm[128];
  FILE *t;
  hn(path, nm, sizeof nm);
  t = fopen(nm, "wb");
  fputs(text, t);
  fclose(t);
}
static int get(const char *path, char *out, size_t n) {
  char nm[128];
  FILE *t;
  size_t got;
  hn(path, nm, sizeof nm);
  if ((t = fopen(nm, "rb")) == NULL) return -1;
  got = fread(out, 1, n - 1, t);
  out[got] = 0;
  fclose(t);
  return (int)got;
}

static const char *const FILES[] = {
  HABITS_PATH, DIR "/Read.log", DIR "/Stretch.log", DIR "/Water.log", DIR "/Run.log",
  DIR "/Jog.log", HABITS_PATH ".tmp", DIR "/Read.log.tmp",
};
static void wipe(void) {
  size_t i;
  for (i = 0; i < sizeof FILES / sizeof FILES[0]; i++) h_remove(FILES[i]);
}

/* A fresh app on 24 Sep 2026 over whatever files the test wrote. */
static void open_app(void) {
  memset(&HF, 0, sizeof HF);
  HF.fmt = h_fmt; HF.mem_set = h_memset; HF.mem_cpy = h_memcpy; HF.str_len = h_strlen;
  HF.open = h_open; HF.read = h_read; HF.write = h_write; HF.close = h_close;
  HF.mkdir = h_mkdir; HF.remove = h_remove; HF.rename = h_rename; HF.stat = h_stat;
  HF.now = h_now; HF.ticks_ms = h_ticks; HF.key_repeat = h_repeat;
  HF.headless = h_headless; HF.damage = h_damage; HF.font_height = h_font_height;
  api = &HF;
  {
    int32_t today;
    int ok;
    memset(&H, 0, sizeof H);
    H.f_ui = H.f_uib = H.f_num = -1;
    read_clock(&today, &ok);
    H.clock_ok = ok;
    anchor(today);
    load_all();
    H.day = H.today;
  }
}

static int32_t D(int y, int m, int d) { return days_from_civil(y, m, d); }

void test_habits_loads_the_old_files(void) {
  wipe();
  s_synced = 2; s_y = 2026; s_m = 9; s_d = 24;
  put(HABITS_PATH, "Read\nStretch\n");
  put(DIR "/Read.log", "20260922\n20260923\n20260924\n");
  put(DIR "/Stretch.log", "20260920\n");
  open_app();
  CHECK_EQ(H.n, 2);
  CHECK(!strcmp(H.habit[0].name, "Read"));
  CHECK(is_done(0, D(2026, 9, 24)));
  CHECK(is_done(0, D(2026, 9, 22)));
  CHECK(!is_done(0, D(2026, 9, 21)));
  CHECK(is_done(1, D(2026, 9, 20)));
  CHECK_EQ(streak(0), 3);
  CHECK_EQ(streak(1), 0);
  wipe();
}

/* Two years in a log, a one-year window: the year it does not hold goes back
 * out exactly, in order, around the days it does. */
void test_habits_a_save_keeps_the_history_outside_the_window(void) {
  char text[512];
  wipe();
  s_synced = 2; s_y = 2026; s_m = 9; s_d = 24;
  put(HABITS_PATH, "Read\n");
  /* out of order, as a hand edit might leave it, and one far older */
  put(DIR "/Read.log", "20260901\n20240101\n20250101\n20260923\n");
  open_app();
  CHECK_EQ(H.habit[0].outside, 2);             /* 2024 and 2025-01-01 */
  CHECK_EQ(total_done(0), 4);
  CHECK_EQ(toggle(0, D(2026, 9, 24)), 0);      /* tick today, which saves */
  CHECK(get(DIR "/Read.log", text, sizeof text) > 0);
  CHECK(!strcmp(text, "20240101\n20250101\n20260901\n20260923\n20260924\n"));
  wipe();
}

void test_habits_streak_is_not_broken_until_the_day_is_over(void) {
  wipe();
  s_synced = 2; s_y = 2026; s_m = 9; s_d = 24;
  put(HABITS_PATH, "Read\n");
  put(DIR "/Read.log", "20260920\n20260921\n20260922\n20260923\n");
  open_app();
  CHECK(!is_done(0, H.today));
  CHECK_EQ(streak(0), 4);                      /* today not ticked yet: still 4 */
  toggle(0, H.today);
  CHECK_EQ(streak(0), 5);
  CHECK_EQ(best_streak(0), 5);
  toggle(0, D(2026, 9, 22));                   /* a gap two days back */
  CHECK_EQ(streak(0), 2);
  CHECK_EQ(best_streak(0), 2);
  wipe();
}

void test_habits_rate_is_of_the_last_30_days(void) {
  int d;
  wipe();
  s_synced = 2; s_y = 2026; s_m = 9; s_d = 24;
  put(HABITS_PATH, "Read\n");
  open_app();
  for (d = 0; d < 15; d++) set_done(0, H.today - d, 1);
  set_done(0, H.today - 40, 1);                /* outside the 30 */
  CHECK_EQ(rate(0, 30), 50);
  wipe();
}

void test_habits_refuses_to_mark_the_future_or_without_a_clock(void) {
  wipe();
  s_synced = 2; s_y = 2026; s_m = 9; s_d = 24;
  put(HABITS_PATH, "Read\n");
  open_app();
  CHECK_EQ(toggle(0, H.today + 1), -1);
  CHECK_EQ(toggle(0, H.win_start - 1), -1);
  s_synced = 0;
  open_app();
  CHECK_EQ(H.clock_ok, 0);
  CHECK_EQ(toggle(0, H.today), -1);
  s_synced = 2;
  wipe();
}

/* Open over midnight: the window moves a day and nothing already ticked is
 * lost or shifted onto the wrong date. */
void test_habits_the_window_follows_midnight(void) {
  wipe();
  s_synced = 2; s_y = 2026; s_m = 9; s_d = 24;
  put(HABITS_PATH, "Read\n");
  open_app();
  toggle(0, H.today);
  set_done(0, H.win_start, 1);                  /* the oldest day held */
  anchor(H.today + 1);
  CHECK(is_done(0, D(2026, 9, 24)));
  CHECK(!is_done(0, D(2026, 9, 25)));
  CHECK_EQ(H.habit[0].outside, 1);              /* the oldest slid out, counted */
  CHECK_EQ(streak(0), 1);                       /* yesterday's, still alive */
  wipe();
}

void test_habits_names_that_cannot_be_files_are_refused(void) {
  const char *why = NULL;
  wipe();
  s_synced = 2;
  open_app();
  CHECK(add_habit("Read", &why) == 0);
  CHECK(add_habit("read", &why) < 0);           /* the card is case-blind */
  CHECK(why && strstr(why, "called that"));
  CHECK(add_habit("a/b", &why) < 0);
  CHECK(add_habit("what?", &why) < 0);
  CHECK(add_habit("C:x", &why) < 0);
  CHECK(add_habit("   ", &why) < 0);
  CHECK(add_habit(" padded", &why) < 0);
  CHECK(add_habit("dots.", &why) < 0);
  CHECK(add_habit("0123456789012345678901234", &why) < 0);   /* 25 */
  CHECK(add_habit("Drink 2L of water", &why) == 1);
  CHECK_EQ(H.n, 2);
  wipe();
}

void test_habits_rename_moves_the_log_and_delete_removes_it(void) {
  const char *why = NULL;
  wipe();
  s_synced = 2; s_y = 2026; s_m = 9; s_d = 24;
  put(HABITS_PATH, "Run\nRead\n");
  put(DIR "/Run.log", "20260924\n");
  open_app();
  CHECK_EQ(rename_habit(0, "Read", &why), -1);  /* taken */
  CHECK_EQ(rename_habit(0, "Jog", &why), 0);
  CHECK(!h_exists(DIR "/Run.log"));
  CHECK(h_exists(DIR "/Jog.log"));
  open_app();
  CHECK(!strcmp(H.habit[0].name, "Jog"));
  CHECK(is_done(0, H.today));                    /* its history came with it */
  delete_habit(0);
  CHECK(!h_exists(DIR "/Jog.log"));
  open_app();
  CHECK_EQ(H.n, 1);
  CHECK(!strcmp(H.habit[0].name, "Read"));
  wipe();
}

void test_habits_move_reorders_and_saves(void) {
  wipe();
  put(HABITS_PATH, "A\nB\nC\n");
  open_app();
  move_habit(2, -1);
  open_app();
  CHECK(!strcmp(H.habit[1].name, "C"));
  CHECK(!strcmp(H.habit[2].name, "B"));
  wipe();
}

void test_habits_commands(void) {
  char out[256];
  const char *read_[] = { "read" }, *zz[] = { "zz" }, *new_[] = { "Water" };
  wipe();
  s_synced = 2; s_y = 2026; s_m = 9; s_d = 24;
  put(HABITS_PATH, "Read\nStretch\n");
  put(DIR "/Read.log", "20260923\n");
  open_app();
  CHECK_EQ(app_command(0, ACT_DONE, 1, read_, out, sizeof out), 0);
  CHECK(!strcmp(out, "Read: done today, 2 day streak"));
  CHECK_EQ(app_command(0, ACT_DONE, 1, read_, out, sizeof out), 0);
  CHECK(strstr(out, "already done today") != NULL);
  CHECK_EQ(app_command(0, ACT_DONE, 1, zz, out, sizeof out), -1);
  CHECK_EQ(app_command(0, ACT_ADD, 1, new_, out, sizeof out), 0);
  CHECK(!strcmp(out, "tracking Water"));
  CHECK_EQ(app_command(0, ACT_LIST, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(out, "- Read: done today, 2 day streak\n"
                     "- Stretch: not yet today, 0 day streak\n"
                     "- Water: not yet today, 0 day streak"));
  wipe();
}
