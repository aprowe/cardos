/* The Today app: its sections file, and the page it builds from other apps'
 * answers. run_command is faked -- the apps it would ask have tests of their
 * own; this is about what Today does with the answers. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info today_capp_info
#define capp_main today_capp_main
#include "apps/today.c"
#undef capp_info
#undef capp_main

static int d_fmt(char *b, size_t n, const char *f, ...) {
  va_list ap; int r;
  va_start(ap, f); r = vsnprintf(b, n, f, ap); va_end(ap);
  return r;
}
static void *d_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *d_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t d_strlen(const char *s) { return strlen(s); }
static void d_now(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->synced = 2; t->year = 2026; t->month = 9; t->day = 29; t->wday = 2; t->hour = 7; t->min = 5;
}

static char s_asked[256];
static int d_run(const char *app, const char *line, char *out, size_t n) {
  strcat(s_asked, app); strcat(s_asked, " "); strcat(s_asked, line); strcat(s_asked, ";");
  if (!strcmp(app, "calendar")) { snprintf(out, n, "09:00 Standup\n18:00 Dinner"); return 0; }
  if (!strcmp(app, "todo") && !strcmp(line, "lists")) { snprintf(out, n, "Home\n* Chores"); return 0; }
  if (!strcmp(app, "todo")) { snprintf(out, n, "[ ] Bins\n[ ] Gutters"); return 0; }
  if (!strcmp(app, "stocks")) { snprintf(out, n, "offline"); return -1; }
  snprintf(out, n, "no app called %s", app);
  return -1;
}

static CardApi DF;

static void dopen(void) {
  memset(&DF, 0, sizeof DF);
  DF.fmt = d_fmt; DF.mem_set = d_memset; DF.mem_cpy = d_memcpy; DF.str_len = d_strlen;
  DF.now = d_now; DF.run_command = d_run;
  api = &DF;
  memset(&D, 0, sizeof D);
  s_asked[0] = 0;
}

void test_today_reads_a_section_line(void) {
  Section s;
  dopen();
  CHECK(parse_section("  To do | todo show Chores ", &s));
  CHECK(!strcmp(s.head, "To do"));
  CHECK(!strcmp(s.app, "todo"));
  CHECK(!strcmp(s.line, "show Chores"));
  CHECK(!parse_section("# Calendar | calendar today", &s));
  CHECK(!parse_section("no bar here", &s));
  CHECK(!parse_section("Heading | calendar", &s));      /* an app and no command */
}

void test_today_builds_the_page_from_the_answers(void) {
  dopen();
  add_section("Calendar", "calendar", "today");
  add_section("To do", "todo", "show Chores");
  add_section("Stocks", "stocks", "portfolio");
  gather_all();
  CHECK(!strcmp(s_asked, "calendar today;todo show Chores;stocks portfolio;"));
  CHECK(!strcmp(D.page,
    "# Tuesday 29 September\n"
    "\n## Calendar\n09:00 Standup\n18:00 Dinner\n"
    "\n## To do\n[ ] Bins\n[ ] Gutters\n"
    "\n## Stocks\n(offline)\n"          /* one section failing is said in its place */
    "\n---\nprinted 29 Sep 07:05\n"));
}

/* The first time, with no file: the four sections asked for, and the todo
 * one on the list Todo has open. */
void test_today_first_page_uses_todos_current_list(void) {
  int i;
  dopen();
  for (i = 0; i < MAX_SECT; i++) memset(&D.sect[i], 0, sizeof D.sect[i]);
  D.nsect = 0;
  add_section("Calendar", "calendar", "today");
  {
    char list[40];
    CHECK_EQ(current_list(list, sizeof list), 0);
    CHECK(!strcmp(list, "Chores"));
  }
}
