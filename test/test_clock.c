/* Clock's commands and its one rule about the file: the kernel changes
 * /config/alarms.txt while the app is open (a `once` switched off after it
 * rings), and a change made here must not undo that. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info clock_capp_info
#define capp_main clock_capp_main
#include "apps/clock.c"
#undef capp_info
#undef capp_main

static int k_fmt(char *b, size_t n, const char *f, ...) {
  va_list ap; int r;
  va_start(ap, f); r = vsnprintf(b, n, f, ap); va_end(ap);
  return r;
}
static void *k_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *k_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t k_strlen(const char *s) { return strlen(s); }
static FILE *s_kf;
static void kn(const char *p, char *o, size_t n) {
  size_t j = 0, i;
  const char *pre = "clock_test";
  for (i = 0; pre[i] && j + 1 < n; i++) o[j++] = pre[i];
  for (i = 0; p[i] && j + 1 < n; i++) o[j++] = p[i] == '/' ? '_' : p[i];
  o[j] = 0;
}
static int k_exists(const char *p) { char nm[128]; FILE *t; kn(p, nm, sizeof nm); if (!(t = fopen(nm, "rb"))) return 0; fclose(t); return 1; }
static int k_open(const char *p, int fl) { char nm[128]; kn(p, nm, sizeof nm); s_kf = fopen(nm, (fl & CAPP_O_WRITE) ? "wb" : "rb"); return s_kf ? 3 : -1; }
static int k_read(int fd, void *b, size_t n) { (void)fd; return (int)fread(b, 1, n, s_kf); }
static int k_write(int fd, const void *b, size_t n) { (void)fd; return (int)fwrite(b, 1, n, s_kf); }
static void k_close(int fd) { (void)fd; fclose(s_kf); s_kf = NULL; }
static int k_mkdir(const char *p) { (void)p; return 0; }
static int k_remove(const char *p) { char nm[128]; kn(p, nm, sizeof nm); return remove(nm); }
static int k_rename(const char *a, const char *b) {
  char x[128], y[128];
  if (k_exists(b)) return -1;
  kn(a, x, sizeof x); kn(b, y, sizeof y);
  return rename(x, y) == 0 ? 0 : -1;
}
static int k_stat(const char *p, CappStat *st) { if (!k_exists(p)) return -1; if (st) memset(st, 0, sizeof *st); return 0; }
static void k_now(CappTime *t) {        /* Wednesday 24 Sep 2026, 06:00 */
  memset(t, 0, sizeof *t);
  t->synced = 2; t->year = 2026; t->month = 9; t->day = 24; t->wday = 3; t->hour = 6;
}
static int k_headless(void) { return 1; }

static CardApi KF;

static void kput(const char *text) {
  char nm[128]; FILE *t;
  kn(ALARM_FILE, nm, sizeof nm);
  t = fopen(nm, "wb"); fputs(text, t); fclose(t);
}
static void kget(char *out, size_t n) {
  char nm[128]; FILE *t; size_t got;
  kn(ALARM_FILE, nm, sizeof nm);
  out[0] = 0;
  if (!(t = fopen(nm, "rb"))) return;
  got = fread(out, 1, n - 1, t); out[got] = 0; fclose(t);
}

static void kopen(void) {
  memset(&KF, 0, sizeof KF);
  KF.fmt = k_fmt; KF.mem_set = k_memset; KF.mem_cpy = k_memcpy; KF.str_len = k_strlen;
  KF.open = k_open; KF.read = k_read; KF.write = k_write; KF.close = k_close;
  KF.mkdir = k_mkdir; KF.remove = k_remove; KF.rename = k_rename; KF.stat = k_stat;
  KF.now = k_now; KF.headless = k_headless;
  api = &KF;
  memset(&K, 0, sizeof K);
  K.f_big = K.f_mid = K.f_ui = K.f_uib = -1;
  load();
}
static void kclean(void) { k_remove(ALARM_FILE); k_remove(ALARM_FILE ".tmp"); }

void test_clock_add_reads_a_spoken_time(void) {
  char out[160], file[512];
  const char *a[] = { "7:30 wake up" }, *b[] = { "7 pm gym" }, *c[] = { "soon" };
  kclean();
  kopen();
  CHECK_EQ(app_command(0, ACT_ADD, 1, a, out, sizeof out), 0);
  CHECK(!strcmp(out, "alarm set for 07:30 wake up, in 1 h 30 min"));
  CHECK_EQ(app_command(0, ACT_ADD, 1, b, out, sizeof out), 0);
  CHECK(strstr(out, "19:00 gym") != NULL);
  CHECK_EQ(app_command(0, ACT_ADD, 1, c, out, sizeof out), -1);
  kget(file, sizeof file);
  CHECK(strstr(file, "on 07:30 once wake up\n") != NULL);
  CHECK(strstr(file, "on 19:00 once gym\n") != NULL);
  CHECK_EQ(app_command(0, ACT_NEXT, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(out, "07:30 wake up, in 1 h 30 min"));
  kclean();
}

void test_clock_off_and_list(void) {
  char out[256];
  const char *t[] = { "7am" };
  kclean();
  kput("on 07:00 -MTWTF- Work\non 09:00 once\n");
  kopen();
  CHECK_EQ(app_command(0, ACT_OFF, 1, t, out, sizeof out), 0);
  CHECK_EQ(app_command(0, ACT_LIST, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(out, "07:00 weekdays off Work\n09:00 once on"));
  kclean();
}

/* The kernel rang the 09:00 `once` and switched it off while the list was
 * open; toggling a different alarm here must leave that one off. */
void test_clock_a_change_does_not_undo_the_kernels(void) {
  char file[512];
  kclean();
  kput("on 07:00 -MTWTF- Work\non 09:00 once Dentist\n");
  kopen();
  K.sel = 0;
  kput("on 07:00 -MTWTF- Work\noff 09:00 once Dentist\n");     /* the kernel's edit */
  toggle_selected();                                             /* Work off */
  kget(file, sizeof file);
  CHECK(strstr(file, "off 07:00 -MTWTF- Work\n") != NULL);
  CHECK(strstr(file, "off 09:00 once Dentist\n") != NULL);       /* still off */
  kclean();
}
