/* /cache/apps.idx: what the icon scan learned about each .capp, so the next
 * boot does not load every app to find out its name again. */
#include <string.h>
#include "tinytest.h"
#include "kernel/app/appidx.h"

static AppIdxRec rec(const char *path, const char *name, uint32_t size, uint32_t mtime) {
  AppIdxRec r;
  int i;
  memset(&r, 0, sizeof r);
  snprintf(r.path, sizeof r.path, "%s", path);
  snprintf(r.name, sizeof r.name, "%s", name);
  r.size = size;
  r.mtime = mtime;
  r.flags = 0x0102;
  for (i = 0; i < APPIDX_ICON_BYTES; i++) r.icon[i] = (uint8_t)(i * 7 + 1);
  return r;
}

/* Two records, the second with commands, behind the header for `key`. */
static size_t build(char *buf, size_t size, const char *key) {
  AppIdxRec a = rec("/apps/Games/mines.capp", "Mines", 5000, 1700000000u);
  AppIdxRec b = rec("/apps/todo.capp", "Todo", 14720, 1700000100u);
  int n = appidx_header(buf, size, key), m;
  CHECK(n > 0);
  m = appidx_record(buf + n, size - (size_t)n, &a, "");
  CHECK(m > 0); n += m;
  m = appidx_record(buf + n, size - (size_t)n, &b, "todo add TEXT\ntodo list\n");
  CHECK(m > 0); n += m;
  return (size_t)n;
}

void test_appidx_finds_what_it_wrote(void) {
  char buf[1024], cmds[128];
  AppIdxRec want = rec("/apps/todo.capp", "Todo", 14720, 1700000100u), got;
  const char *body;
  build(buf, sizeof buf, "32-deadbeef");
  body = appidx_body(buf, "32-deadbeef");
  CHECK(body != NULL);
  CHECK_EQ(appidx_find(body, "/apps/todo.capp", 14720, 1700000100u, &got, cmds, sizeof cmds), 1);
  CHECK(!strcmp(got.name, "Todo"));
  CHECK(!strcmp(got.path, "/apps/todo.capp"));
  CHECK_EQ(got.flags, 0x0102);
  CHECK(!memcmp(got.icon, want.icon, APPIDX_ICON_BYTES));
  CHECK(!strcmp(cmds, "todo add TEXT\ntodo list\n"));

  CHECK_EQ(appidx_find(body, "/apps/Games/mines.capp", 5000, 1700000000u, &got, cmds, sizeof cmds), 1);
  CHECK(!strcmp(got.name, "Mines"));
  CHECK(!strcmp(cmds, ""));
}

/* The key is the API version and the firmware's apps: either changing means
 * nothing in the file can be trusted. */
void test_appidx_refuses_another_key(void) {
  char buf[1024];
  build(buf, sizeof buf, "32-deadbeef");
  CHECK(appidx_body(buf, "33-deadbeef") == NULL);
  CHECK(appidx_body(buf, "32-deadbee") == NULL);
  CHECK(appidx_body("", "32-deadbeef") == NULL);
  CHECK(appidx_body("rubbish\n", "32-deadbeef") == NULL);
}

/* A file that changed size or date is a different app: `update apps`, a
 * card reader, Build. It must be loaded, not remembered. */
void test_appidx_misses_a_changed_file(void) {
  char buf[1024], cmds[64];
  AppIdxRec got;
  const char *body;
  build(buf, sizeof buf, "k");
  body = appidx_body(buf, "k");
  CHECK_EQ(appidx_find(body, "/apps/todo.capp", 14721, 1700000100u, &got, cmds, sizeof cmds), 0);
  CHECK_EQ(appidx_find(body, "/apps/todo.capp", 14720, 1700000102u, &got, cmds, sizeof cmds), 0);
  CHECK_EQ(appidx_find(body, "/apps/edit.capp", 14720, 1700000100u, &got, cmds, sizeof cmds), 0);
  /* A card that does not date its files cannot tell a rewrite from the
   * original, so it is never trusted. */
  CHECK_EQ(appidx_find(body, "/apps/todo.capp", 14720, 0, &got, cmds, sizeof cmds), 0);
}

/* A path that is a prefix of another must not match it. */
void test_appidx_matches_whole_paths(void) {
  char buf[1024], cmds[64];
  AppIdxRec got;
  const char *body;
  build(buf, sizeof buf, "k");
  body = appidx_body(buf, "k");
  CHECK_EQ(appidx_find(body, "/apps/todo.cap", 14720, 1700000100u, &got, cmds, sizeof cmds), 0);
  CHECK_EQ(appidx_find(body, "/apps/todo.capp2", 14720, 1700000100u, &got, cmds, sizeof cmds), 0);
}

/* The name is the app's own and could hold anything; a tab or newline in it
 * must not break the line it is written on. */
void test_appidx_name_cannot_break_the_line(void) {
  char buf[512], cmds[64];
  AppIdxRec r = rec("/apps/x.capp", "a\tb\nc", 10, 20), got;
  const char *body;
  int n = appidx_header(buf, sizeof buf, "k");
  n += appidx_record(buf + n, sizeof buf - (size_t)n, &r, "");
  body = appidx_body(buf, "k");
  CHECK_EQ(appidx_find(body, "/apps/x.capp", 10, 20, &got, cmds, sizeof cmds), 1);
  CHECK(!strcmp(got.name, "a b c"));
}

/* A record that does not fit is not written at all, rather than half. */
void test_appidx_record_that_does_not_fit_writes_nothing(void) {
  char buf[40];
  AppIdxRec r = rec("/apps/x.capp", "X", 10, 20);
  memset(buf, 'z', sizeof buf);
  CHECK_EQ(appidx_record(buf, sizeof buf, &r, ""), -1);
  CHECK_EQ(buf[0], 0);
}

/* Commands that do not fit the caller's buffer are a miss: an app listed
 * with half its commands would be worse than one loaded properly. */
void test_appidx_commands_too_long_is_a_miss(void) {
  char buf[1024], cmds[8];
  AppIdxRec got;
  const char *body;
  build(buf, sizeof buf, "k");
  body = appidx_body(buf, "k");
  CHECK_EQ(appidx_find(body, "/apps/todo.capp", 14720, 1700000100u, &got, cmds, sizeof cmds), 0);
}

/* A file cut short by a power cut: the torn last record is not found, and
 * the ones before it still are. */
void test_appidx_torn_file(void) {
  char buf[1024], cmds[128];
  AppIdxRec got;
  const char *body;
  size_t n = build(buf, sizeof buf, "k");
  buf[n - 30] = 0;
  body = appidx_body(buf, "k");
  CHECK(body != NULL);
  CHECK_EQ(appidx_find(body, "/apps/Games/mines.capp", 5000, 1700000000u, &got, cmds, sizeof cmds), 1);
  CHECK_EQ(appidx_find(body, "/apps/todo.capp", 14720, 1700000100u, &got, cmds, sizeof cmds), 0);
}
