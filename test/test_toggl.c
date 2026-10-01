/* Toggl on the host: the server's lines read back, dates sent the way the
 * server parses them, and what the commands say. The server's half, against
 * a fake Toggl, is server/tests/test_toggl.py. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info toggl_capp_info
#define capp_main toggl_capp_main
#include "apps/toggl.c"
#undef capp_info
#undef capp_main

static int t_fmt(char *b, size_t n, const char *f, ...) {
  va_list ap; int r;
  va_start(ap, f); r = vsnprintf(b, n, f, ap); va_end(ap);
  return r;
}
static size_t t_strlen(const char *s) { return strlen(s); }
static const char *t_proxy(void) { return "http://srv"; }
static int t_ready(void) { return 1; }

/* 2026-10-01 17:30:00 UTC, and the zone is UTC-7 (local 10:30). */
static uint32_t t_epoch(void) { return 1790875800u; }
static void t_now(CappTime *t) {
  memset(t, 0, sizeof *t);
  t->synced = 2; t->year = 2026; t->month = 10; t->day = 1; t->hour = 10; t->min = 30;
}

static char s_url[200], s_body[200];
static const char *s_reply;
static int t_http(const char *m, const char *url, const char *body, const char *ct,
                  const char *bearer, char *out, size_t n, int ms) {
  (void)m; (void)ct; (void)bearer; (void)ms;
  snprintf(s_url, sizeof s_url, "%s", url);
  snprintf(s_body, sizeof s_body, "%s", body ? body : "");
  snprintf(out, n, "%s", s_reply);
  return (int)strlen(s_reply);
}

static uint32_t s_ticks;
static uint32_t t_ticks(void) { return s_ticks; }
static uint32_t t_no_clock(void) { return 0; }

static CardApi TF;

static void topen(void) {
  memset(&TF, 0, sizeof TF);
  TF.fmt = t_fmt; TF.str_len = t_strlen; TF.proxy = t_proxy; TF.net_ready = t_ready;
  TF.epoch = t_epoch; TF.now = t_now; TF.http = t_http; TF.ticks_ms = t_ticks;
  s_ticks = 5000;
  api = &TF;
  memset(&G, 0, sizeof G);
}

static const char STATUS[] =
  "running\t99\t1790874000\tWriting\tCardOS\n"
  "recent\t10\tWriting\tCardOS\n"
  "recent\t\tEmail\t\n"
  "project\t10\tCardOS\n"
  "project\t11\tHome\n";

void test_toggl_reads_the_status_lines(void) {
  topen();
  snprintf(G.reply, sizeof G.reply, "%s", STATUS);
  absorb_status();
  CHECK_EQ(G.running, 1);
  CHECK_EQ((long)G.start, 1790874000L);
  CHECK(!strcmp(G.desc, "Writing") && !strcmp(G.proj, "CardOS"));
  /* Projects first, then what was done lately. */
  CHECK_EQ(G.nrec, 4);
  CHECK(G.rec[0].kind == 'p' && !strcmp(G.rec[0].proj, "CardOS") && !G.rec[0].desc[0]);
  CHECK(G.rec[1].kind == 'p' && !strcmp(G.rec[1].proj_id, "11"));
  CHECK(G.rec[2].kind == 'r' && !strcmp(G.rec[2].proj_id, "10"));
  CHECK(!strcmp(G.rec[3].desc, "Email") && !G.rec[3].proj_id[0] && !G.rec[3].proj[0]);
  snprintf(G.reply, sizeof G.reply, "idle\n");
  absorb_status();
  CHECK_EQ(G.running, 0);
  CHECK_EQ(G.nrec, 0);
}

void test_toggl_status_says_what_and_how_long(void) {
  char out[128];
  topen();
  s_reply = STATUS;
  CHECK_EQ(app_command(0, ACT_STATUS, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(out, "running: Writing (CardOS), 0:30"));       /* 1800 s */
}

void test_toggl_start_brings_a_recent_entrys_project(void) {
  const char *argv[1] = { "writ" };
  char out[128];
  topen();
  s_reply = STATUS;                     /* the status it reads first */
  app_command(0, ACT_START, 1, argv, out, sizeof out);
  CHECK(strstr(s_url, "/toggl/start") != NULL);
  CHECK(!strcmp(s_body, "description=writ\nproject=10"));
}

/* A project with no description, found by the start of its name. */
void test_toggl_project_starts_with_no_description(void) {
  const char *argv[1] = { "ho" };
  char out[128];
  topen();
  s_reply = STATUS;
  app_command(0, ACT_PROJECT, 1, argv, out, sizeof out);
  CHECK(strstr(s_url, "/toggl/start") != NULL);
  CHECK(!strcmp(s_body, "description=\nproject=11"));
  argv[0] = "nothing like it";
  s_reply = STATUS;
  CHECK_EQ(app_command(0, ACT_PROJECT, 1, argv, out, sizeof out), -1);
  CHECK(!strcmp(out, "no project called nothing like it"));
}

void test_toggl_describe_names_the_running_entry(void) {
  const char *argv[1] = { "Gutters" };
  char out[128];
  topen();
  s_reply = "running\t99\t1790874000\tGutters\tHome\n";
  CHECK_EQ(app_command(0, ACT_DESCRIBE, 1, argv, out, sizeof out), 0);
  CHECK(strstr(s_url, "/toggl/describe") != NULL);
  CHECK(!strcmp(s_body, "description=Gutters"));
  CHECK(!strcmp(out, "Home: Gutters"));
  s_reply = "idle\n";
  CHECK_EQ(app_command(0, ACT_DESCRIBE, 1, argv, out, sizeof out), -1);
}

/* Local midnight to local midnight, sent in UTC: the zone is UTC-7, so the
 * day starts at 07:00 Z. */
void test_toggl_today_asks_for_the_local_day_and_reads_local_times(void) {
  char out[256];
  topen();
  s_reply = "1790874000\t1800\tWriting\tCardOS\n1790875000\t300\t\t\ntotal\t2100\n";
  CHECK_EQ(app_command(0, ACT_TODAY, 0, NULL, out, sizeof out), 0);
  CHECK(strstr(s_url, "from=2026-10-01T07:00:00Z&to=2026-10-02T07:00:00Z") != NULL);
  CHECK(!strcmp(out, "10:00 Writing (CardOS) 0:30\n10:16 (no description) 0:05\ntotal 0:35"));
  s_reply = "total\t0\n";
  app_command(0, ACT_TODAY, 0, NULL, out, sizeof out);
  CHECK(!strcmp(out, "nothing tracked\ntotal 0:00"));
}

void test_toggl_dates_are_rfc3339(void) {
  char s[24];
  topen();
  rfc3339(0, s, sizeof s);
  CHECK(!strcmp(s, "1970-01-01T00:00:00Z"));
  rfc3339(1709164800u, s, sizeof s);                  /* a leap day */
  CHECK(!strcmp(s, "2024-02-29T00:00:00Z"));
  CHECK_EQ(utc_offset(), -7 * 3600L);
}

void test_toggl_a_refusal_is_said_in_the_servers_words(void) {
  char out[128];
  topen();
  TF.http = NULL;
  snprintf(G.reply, sizeof G.reply, "error Toggl refused the token: paste a new one at /dash\n");
  failed(-401, out, sizeof out);
  CHECK(!strncmp(out, "Toggl refused the token", 23));
}

/* Each project in its own colour, as Toggl has it -- lifted when it is too
 * dark for this screen; no colour is 0, and the text colour is used. */
void test_toggl_projects_carry_their_colours(void) {
  topen();
  snprintf(G.reply, sizeof G.reply, "%s",
           "running\t99\t1790874000\tWriting\tCardOS\t#0b83d9\n"
           "project\t10\tCardOS\t#0b83d9\n"
           "project\t11\tHome\t\n");
  absorb_status();
  CHECK(G.colour != 0);
  CHECK_EQ(G.rec[0].colour, G.colour);
  CHECK_EQ(G.rec[1].colour, 0);
  CHECK_EQ(parse_colour("#ffffff"), CAPP_RGB(255, 255, 255));
  CHECK_EQ(parse_colour("#000000"), CAPP_RGB(127, 127, 127));   /* lifted */
  CHECK_EQ(parse_colour("nope"), 0);
  CHECK_EQ(parse_colour("#12345"), 0);
}

/* The device's clock can be unset or minutes behind after a reboot, and the
 * timer showed 0:00:00 (2026-10-01). It counts from what the server says has
 * run, on the device's uptime, so the device's clock does not matter. */
void test_toggl_counts_from_the_servers_seconds_not_the_clock(void) {
  char out[128];
  topen();
  TF.epoch = t_no_clock;
  snprintf(G.reply, sizeof G.reply, "%s",
           "running\t99\t1790874000\tWriting\tCardOS\t#0b83d9\t1800\n");
  absorb_status();
  CHECK_EQ((long)elapsed(), 1800L);
  s_ticks += 65000;                        /* a minute and five seconds on */
  CHECK_EQ((long)elapsed(), 1865L);
  s_reply = "running\t99\t1790874000\tWriting\tCardOS\t#0b83d9\t2700\n";
  CHECK_EQ(app_command(0, ACT_STATUS, 0, NULL, out, sizeof out), 0);
  CHECK(!strcmp(out, "running: Writing (CardOS), 0:45"));
}
