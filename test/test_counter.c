/* Counter on the host: its state file, its laps, its commands.
 *
 * The state is what a user loses if this is wrong, so most of these are
 * about the card: that an older file still reads (laps without d= get their
 * deltas worked out), that a save stranded by a power cut is recovered, and
 * that holding space does not rewrite the card sixteen times a second.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info counter_capp_info
#define capp_main counter_capp_main
#include "apps/counter.c"
#undef capp_info
#undef capp_main

static int f_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
static void *f_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *f_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t f_strlen(const char *s) { return strlen(s); }

/* Files on the host: "counter_test" + the path with slashes as underscores. */
static FILE *s_f;
static int s_writes;              /* opens for writing: how often the card was touched */

static void hname(const char *path, char *out, size_t n) {
  size_t j = 0, i;
  const char *pre = "counter_test";
  for (i = 0; pre[i] && j + 1 < n; i++) out[j++] = pre[i];
  for (i = 0; path[i] && j + 1 < n; i++) out[j++] = path[i] == '/' ? '_' : path[i];
  out[j] = 0;
}
static int f_open(const char *path, int flags) {
  char nm[128];
  hname(path, nm, sizeof nm);
  if (flags & CAPP_O_WRITE) s_writes++;
  s_f = fopen(nm, (flags & CAPP_O_WRITE) ? "wb" : "rb");
  return s_f ? 3 : -1;
}
static int f_read(int fd, void *b, size_t n) { (void)fd; return (int)fread(b, 1, n, s_f); }
static int f_write(int fd, const void *b, size_t n) { (void)fd; return (int)fwrite(b, 1, n, s_f); }
static void f_close(int fd) { (void)fd; fclose(s_f); s_f = NULL; }
static int f_mkdir(const char *p) { (void)p; return 0; }
static int f_remove(const char *p) { char nm[128]; hname(p, nm, sizeof nm); return remove(nm); }
/* The card's rule: rename does not replace a file that is there. */
static int f_rename(const char *a, const char *b) {
  char x[128], y[128];
  FILE *t;
  hname(a, x, sizeof x);
  hname(b, y, sizeof y);
  if ((t = fopen(y, "rb")) != NULL) { fclose(t); return -1; }
  return rename(x, y) == 0 ? 0 : -1;
}
static int f_stat(const char *p, CappStat *st) {
  char nm[128];
  FILE *t;
  hname(p, nm, sizeof nm);
  if ((t = fopen(nm, "rb")) == NULL) return -1;
  fclose(t);
  if (st) memset(st, 0, sizeof *st);
  return 0;
}
static uint32_t s_ms;
static uint32_t f_ticks(void) { return s_ms; }
static void f_now(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->synced = 2; t->year = 2026; t->month = 9; t->day = 24;
  t->hour = 14; t->min = 32; t->sec = 7;
}
static int s_repeat;
static int f_key_repeat(void) { return s_repeat; }
static int f_headless(void) { return 0; }
static void f_damage(CRect r) { (void)r; }
static int f_font_height(int f) { (void)f; return 8; }
static int f_text_width(int f, const char *s) { (void)f; return (int)strlen(s) * 6; }

static CardApi FAKE;

static void host_write(const char *path, const char *text) {
  char nm[128];
  FILE *t;
  hname(path, nm, sizeof nm);
  t = fopen(nm, "wb");
  fputs(text, t);
  fclose(t);
}
static int host_read(const char *path, char *out, size_t n) {
  char nm[128];
  FILE *t;
  size_t got;
  hname(path, nm, sizeof nm);
  if ((t = fopen(nm, "rb")) == NULL) return -1;
  got = fread(out, 1, n - 1, t);
  out[got] = 0;
  fclose(t);
  return (int)got;
}
static void clean(void) {
  f_remove(STATE_PATH);
  f_remove(STATE_PATH ".tmp");
}

static void use_fake(void) {
  memset(&FAKE, 0, sizeof FAKE);
  FAKE.fmt = f_fmt; FAKE.mem_set = f_memset; FAKE.mem_cpy = f_memcpy; FAKE.str_len = f_strlen;
  FAKE.open = f_open; FAKE.read = f_read; FAKE.write = f_write; FAKE.close = f_close;
  FAKE.mkdir = f_mkdir; FAKE.remove = f_remove; FAKE.rename = f_rename; FAKE.stat = f_stat;
  FAKE.ticks_ms = f_ticks; FAKE.now = f_now; FAKE.key_repeat = f_key_repeat;
  FAKE.headless = f_headless; FAKE.damage = f_damage;
  FAKE.font_height = f_font_height; FAKE.text_width = f_text_width;
  api = &FAKE;
  memset(&C, 0, sizeof C);
  C.f_big = C.f_mid = C.f_ui = C.f_uib = -1;
  s_ms = 1000; s_repeat = 0; s_writes = 0;
  clean();
}

void test_counter_reads_the_old_file_and_works_out_the_deltas(void) {
  use_fake();
  host_write(STATE_PATH, "count=47\nlap=47 14:32:07\nlap=35 14:20:51\nlap=10 +3:41\n");
  state_load();
  CHECK_EQ(C.count, 47);
  CHECK_EQ(C.nlaps, 3);
  CHECK_EQ(C.laps[0].delta, 12);
  CHECK_EQ(C.laps[1].delta, 25);
  CHECK_EQ(C.laps[2].delta, 10);
  CHECK(!strcmp(C.laps[0].stamp, "14:32:07"));
  CHECK(!strcmp(C.laps[2].stamp, "+3:41"));
  clean();
}

void test_counter_state_round_trips_with_deltas(void) {
  char text[256];
  use_fake();
  C.count = 5;
  add_lap();                                   /* lap 5, +5 */
  count_sub(1); count_sub(1);                  /* 3 */
  add_lap();                                   /* lap 3, -2 */
  CHECK(host_read(STATE_PATH, text, sizeof text) > 0);
  CHECK(!strcmp(text, "count=3\nlap=3 d=-2 14:32:07\nlap=5 d=5 14:32:07\n"));
  memset(&C, 0, sizeof C);
  state_load();
  CHECK_EQ(C.count, 3);
  CHECK_EQ(C.nlaps, 2);
  CHECK_EQ(C.laps[0].delta, -2);
  CHECK_EQ(C.laps[1].delta, 5);
  clean();
}

/* A cut after the old file was removed and before the rename: only the
 * .tmp is there, whole, and the next open puts it back. */
void test_counter_recovers_a_save_stranded_by_a_power_cut(void) {
  use_fake();
  host_write(STATE_PATH ".tmp", "count=9\n");
  state_load();
  CHECK_EQ(C.count, 9);
  CHECK(f_stat(STATE_PATH, NULL) == 0);
  CHECK(f_stat(STATE_PATH ".tmp", NULL) != 0);
  clean();
}

/* A cut while the .tmp was being written: the real file wins. */
void test_counter_ignores_a_half_written_temp(void) {
  use_fake();
  host_write(STATE_PATH, "count=20\n");
  host_write(STATE_PATH ".tmp", "count=2");
  state_load();
  CHECK_EQ(C.count, 20);
  C.count = 21;
  CHECK_EQ(state_save(), 0);                   /* and a save still succeeds */
  memset(&C, 0, sizeof C);
  state_load();
  CHECK_EQ(C.count, 21);
  clean();
}

void test_counter_does_not_go_below_zero(void) {
  use_fake();
  CHECK_EQ(count_sub(1), 0);
  CHECK_EQ(C.count, 0);
  count_add(1);
  CHECK_EQ(count_sub(1), 1);
  CHECK_EQ(C.count, 0);
  clean();
}

/* Held space: every repeat counts, and the card is written once, when the
 * key has been still for a moment -- not once per 60 ms repeat. */
void test_counter_held_space_saves_once_when_it_stops(void) {
  int i;
  use_fake();
  s_repeat = 0;
  app_key(0, ' ');                             /* the press: saved at once */
  CHECK_EQ(s_writes, 1);
  s_repeat = 1;
  for (i = 0; i < 20; i++) { s_ms += 60; app_key(0, ' '); app_tick(0, s_ms); }
  CHECK_EQ(C.count, 21);
  CHECK_EQ(s_writes, 1);                       /* nothing while it repeats */
  s_ms += SAVE_IDLE_MS + 10;
  app_tick(0, s_ms);
  CHECK_EQ(s_writes, 2);
  CHECK_EQ(C.dirty, 0);
  memset(&C, 0, sizeof C);
  state_load();
  CHECK_EQ(C.count, 21);
  clean();
}

void test_counter_reset_asks_first(void) {
  use_fake();
  C.count = 4;
  add_lap();
  app_key(0, 0x7F);
  CHECK_EQ(C.ask, ASK_RESET);
  CHECK_EQ(C.count, 4);
  app_key(0, 'n');
  CHECK_EQ(C.ask, ASK_NONE);
  CHECK_EQ(C.count, 4);
  app_key(0, 0x7F);
  app_key(0, 'y');
  CHECK_EQ(C.count, 0);
  CHECK_EQ(C.nlaps, 0);
  clean();
}

void test_counter_commands(void) {
  char out[160];
  const char *five[] = { "5" }, *bad[] = { "5x" }, *neg[] = { "-1" };
  use_fake();
  CHECK_EQ(app_command(0, ACT_ADD, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(out, "count 1"));
  CHECK_EQ(app_command(0, ACT_SET, 1, five, out, sizeof out), 0);
  CHECK_EQ(C.count, 5);
  CHECK_EQ(app_command(0, ACT_SET, 1, bad, out, sizeof out), -1);
  CHECK_EQ(app_command(0, ACT_SET, 1, neg, out, sizeof out), -1);
  CHECK_EQ(C.count, 5);
  CHECK_EQ(app_command(0, ACT_LAP, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(out, "lap 1: 5 (+5)"));
  CHECK_EQ(app_command(0, ACT_SHOW, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(out, "count 5\nlap 1: 5 (+5) at 14:32:07"));
  C.count = 0;
  CHECK_EQ(app_command(0, ACT_SUB, 0, NULL, out, sizeof out), -1);
  clean();
}

void test_counter_keeps_the_newest_laps(void) {
  int i;
  use_fake();
  for (i = 1; i <= MAX_LAPS + 5; i++) { C.count = (uint32_t)i; add_lap(); }
  CHECK_EQ(C.nlaps, MAX_LAPS);
  CHECK_EQ(C.laps[0].count, MAX_LAPS + 5);
  CHECK_EQ(C.laps[MAX_LAPS - 1].count, 6);
  clean();
}
