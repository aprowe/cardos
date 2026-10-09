/* The Today app: its sections file, and the page it builds from other apps'
 * answers. run_command is faked -- the apps it would ask have tests of their
 * own; this is about what Today does with the answers. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define capp_info today_capp_info
#define capp_main today_capp_main
#include "apps/today.c"
#undef capp_info
#undef capp_main

static int s_day = 29, s_wday = 2;
static void d_now(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->synced = 2; t->year = 2026; t->month = 9; t->day = (uint8_t)s_day; t->wday = (uint8_t)s_wday;
  t->hour = 7; t->min = 5;
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
  fakeapi_init(&DF);
  DF.now = d_now; DF.run_command = d_run;
  api = &DF;
  memset(&D, 0, sizeof D);
  s_asked[0] = 0;
  s_day = 29; s_wday = 2;
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

/* Tomorrow's page: a section that asks for today asks for tomorrow, the
 * rest are asked as they are, and the heading is tomorrow's date. */
void test_today_tomorrow_asks_for_tomorrow(void) {
  dopen();
  add_section("Calendar", "calendar", "today");
  add_section("To do", "todo", "show Chores");
  add_section("Habits", "habits", "today");
  D.ahead = 1;
  gather_all();
  CHECK(!strcmp(s_asked, "calendar tomorrow;todo show Chores;habits tomorrow;"));
  CHECK(!strncmp(D.page, "# Wednesday 30 September\n", 25));
}

void test_today_tomorrow_crosses_the_month_and_the_year(void) {
  dopen();
  s_day = 30; s_wday = 3;
  add_section("Calendar", "calendar", "today");
  D.ahead = 1;
  gather_all();
  CHECK(!strncmp(D.page, "# Thursday 1 October\n", 21));
  {
    int y = 2026, m = 12, d = 31;
    next_day(&y, &m, &d);
    CHECK(y == 2027 && m == 1 && d == 1);
    y = 2028; m = 2; d = 28;
    next_day(&y, &m, &d);
    CHECK(m == 2 && d == 29);                 /* a leap year */
  }
}

/* The todo lists are in the script, to be chosen by typing one: Todo's
 * answer becomes one comment line, the open list marked. */
void test_today_the_lists_line_names_every_list(void) {
  char line[160];
  dopen();
  CHECK_EQ(lists_line(line, sizeof line), 0);
  CHECK(!strcmp(line, "# todo lists: Home, Chores (open in Todo)\n"));
}

/* The line goes in under the header, replacing the one before it, and
 * nothing the person wrote moves. */
void test_today_the_lists_line_replaces_the_old_one_and_keeps_the_rest(void) {
  const char *file =
    "# The Today app's page\n"
    "# todo lists: Old\n"
    "Calendar | calendar today\n"
    "# my own note\n"
    "To do | todo show Chores\n";
  char out[512];
  dopen();
  CHECK(with_lists_line(file, "# todo lists: Home, Chores\n", out, sizeof out) > 0);
  CHECK(!strcmp(out,
    "# The Today app's page\n"
    "# todo lists: Home, Chores\n"
    "Calendar | calendar today\n"
    "# my own note\n"
    "To do | todo show Chores\n"));
  /* A file with no header and no line gets it first. */
  CHECK(with_lists_line("Calendar | calendar today", "# todo lists: A\n", out, sizeof out) > 0);
  CHECK(!strcmp(out, "# todo lists: A\nCalendar | calendar today\n"));
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

/* e puts the todo lists in the sections file before Edit opens it, and does
 * that in the page's buffer. The page used to be left blank on the promise
 * of a regather when the file changed -- and back from Edit unchanged, it
 * stayed blank. Now it is gathered again at once. A card that refuses the
 * write still borrowed the buffer, so this one refuses it. */
static const char *s_cfg = "# The Today app's page\nCalendar | calendar today\n";
static size_t s_cfg_at;
static int d_open(const char *p, int flags) {
  (void)p;
  if (flags & CAPP_O_WRITE) return -1;
  s_cfg_at = 0;
  return 3;
}
static int d_read(int fd, void *b, size_t n) {
  size_t left = strlen(s_cfg) - s_cfg_at;
  (void)fd;
  if (n > left) n = left;
  memcpy(b, s_cfg + s_cfg_at, n);
  s_cfg_at += n;
  return (int)n;
}
static void d_close(int fd) { (void)fd; }
static int d_stat(const char *p, CappStat *st) { (void)p; (void)st; return -1; }
static int s_ran;
static int d_runapp(const char *name, const char *args) { (void)name; (void)args; s_ran = 1; return 0; }
static void d_font_free(int f) { (void)f; }

void test_today_editing_the_sections_does_not_leave_the_page_blank(void) {
  dopen();
  DF.open = d_open; DF.read = d_read; DF.close = d_close; DF.stat = d_stat;
  DF.run = d_runapp; DF.font_free = d_font_free;
  add_section("Calendar", "calendar", "today");
  gather_all();
  CHECK(D.next == -1 && D.len > 0);
  s_ran = 0;
  do_action(ACT_EDIT);
  CHECK_EQ(s_ran, 1);
  CHECK_EQ(D.next, 0);                    /* gathering again, a section a tick */
  CHECK(!strncmp(D.page, "# Tuesday 29 September\n", 23));
}

/* `daily focus` / `daily fact` are the server's, asked with the page's date
 * -- tomorrow's on tomorrow's page. */
static char s_url[200];
static int d_http(const char *m, const char *url, const char *body, const char *ct,
                  const char *bearer, char *out, size_t n, int ms) {
  (void)m; (void)body; (void)ct; (void)bearer; (void)ms;
  snprintf(s_url, sizeof s_url, "%s", url);
  return snprintf(out, n, "Breathe first.\n");
}
static const char *d_proxy(void) { return "http://srv"; }

void test_today_daily_sections_come_from_the_server(void) {
  dopen();
  DF.http = d_http;
  DF.proxy = d_proxy;
  add_section("Focus", "daily", "focus");
  gather_all();
  CHECK(strstr(s_url, "/daily?date=2026-09-29&kind=focus") != NULL);
  CHECK(strstr(D.page, "## Focus\nBreathe first.\n") != NULL);
  D.ahead = 1;
  gather_all();
  CHECK(strstr(s_url, "date=2026-09-30") != NULL);
}
