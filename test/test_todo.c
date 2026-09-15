/* The todo list's offline cache, on the host.
 *
 * Between syncs the list lives in /todo.cache, and what is written there has
 * to come back exactly: a tick made offline is a `dirty` flag and nothing
 * else, and a delete made offline is a `deleted` flag on an item that must
 * survive a restart so the delete can still be pushed. The saver and the
 * loader used to disagree about the columns -- the loader read flags from
 * positions that were always spaces, and the id from one character in --
 * so every restart quietly forgot what was pending and corrupted every id.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info todo_capp_info
#define capp_main todo_capp_main
#include "apps/todo.c"
#undef capp_info
#undef capp_main

/* ---- a CardApi that does just enough: files on the host ----------------- */

static int fake_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
static void *fake_memset(void *d, int c, size_t n)         { return memset(d, c, n); }
static void *fake_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t fake_strlen(const char *s)                   { return strlen(s); }

static FILE *s_file;
#define HOST_CACHE "todo_test.cache"

static int fake_open(const char *path, int flags) {
  (void)path;
  s_file = fopen(HOST_CACHE, (flags & CAPP_O_WRITE) ? "wb" : "rb");
  return s_file ? 3 : -1;
}
static int fake_read(int fd, void *buf, size_t n)        { (void)fd; return (int)fread(buf, 1, n, s_file); }
static int fake_write(int fd, const void *buf, size_t n) { (void)fd; return (int)fwrite(buf, 1, n, s_file); }
static void fake_close(int fd)                           { (void)fd; fclose(s_file); s_file = NULL; }

static CardApi FAKE;

static void use_fake_api(void) {
  memset(&FAKE, 0, sizeof FAKE);
  FAKE.fmt = fake_fmt;
  FAKE.mem_set = fake_memset;
  FAKE.mem_cpy = fake_memcpy;
  FAKE.str_len = fake_strlen;
  FAKE.open = fake_open;
  FAKE.read = fake_read;
  FAKE.write = fake_write;
  FAKE.close = fake_close;
  api = &FAKE;
  memset(&T, 0, sizeof T);
}

static void add_item(const char *id, const char *title, int done, int dirty, int deleted) {
  Item *it = &T.item[T.n++];
  memset(it, 0, sizeof *it);
  snprintf(it->id, sizeof it->id, "%s", id);
  snprintf(it->title, sizeof it->title, "%s", title);
  it->done = done; it->dirty = dirty; it->deleted = deleted;
}

void test_todo_cache_round_trips_every_field(void) {
  use_fake_api();
  add_item("MTIzNDU2Nzg5", "Buy milk and eggs", 1, 1, 0);
  add_item("",             "Never synced",      0, 1, 0);
  add_item("YWJj",         "Gone here",         0, 0, 1);
  cache_save();

  memset(&T, 0, sizeof T);
  cache_load();

  CHECK_EQ(T.n, 3);
  CHECK(!strcmp(T.item[0].id, "MTIzNDU2Nzg5"));
  CHECK(!strcmp(T.item[0].title, "Buy milk and eggs"));
  CHECK_EQ(T.item[0].done, 1);
  CHECK_EQ(T.item[0].dirty, 1);
  CHECK_EQ(T.item[0].deleted, 0);

  CHECK(!strcmp(T.item[1].id, ""));
  CHECK_EQ(T.item[1].dirty, 1);

  CHECK(!strcmp(T.item[2].id, "YWJj"));
  CHECK_EQ(T.item[2].deleted, 1);
  CHECK_EQ(T.item[2].dirty, 0);
  remove(HOST_CACHE);
}
