/* The todo list's offline cache, on the host.
 *
 * Between syncs the list lives in /cache/todo/<id>.cache, and what is written there has
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

/* Files on the host, by path. The cache is one file per list now, plus the
 * list of lists, so the fake has to tell paths apart: each becomes a file in
 * the working directory with the slashes turned into underscores. */
#define HOST_PREFIX "todo_test"

static FILE *s_file;

static void host_name(const char *path, char *out, size_t n) {
  size_t i = 0, j = 0;
  const char *pre = HOST_PREFIX;
  while (pre[i] && j + 1 < n) out[j++] = pre[i++];
  for (i = 0; path[i] && j + 1 < n; i++) out[j++] = path[i] == '/' ? '_' : path[i];
  out[j] = 0;
}
static int fake_open(const char *path, int flags) {
  char name[128];
  host_name(path, name, sizeof name);
  s_file = fopen(name, (flags & CAPP_O_WRITE) ? "wb" : "rb");
  return s_file ? 3 : -1;
}
static int fake_read(int fd, void *buf, size_t n)        { (void)fd; return (int)fread(buf, 1, n, s_file); }
static int fake_write(int fd, const void *buf, size_t n) { (void)fd; return (int)fwrite(buf, 1, n, s_file); }
static void fake_close(int fd)                           { (void)fd; fclose(s_file); s_file = NULL; }
static int fake_mkdir(const char *path)                  { (void)path; return 0; }
static int fake_remove(const char *path) {
  char name[128];
  host_name(path, name, sizeof name);
  return remove(name);
}
static int fake_rename(const char *from, const char *to) {
  char a[128], b[128];
  host_name(from, a, sizeof a);
  host_name(to, b, sizeof b);
  remove(b);
  return rename(a, b);
}
static void fake_log(const char *m) { (void)m; }

static int host_exists(const char *path) {
  char name[128];
  FILE *f;
  host_name(path, name, sizeof name);
  f = fopen(name, "rb");
  if (f) fclose(f);
  return f != NULL;
}
static void host_write(const char *path, const char *text) {
  char name[128];
  FILE *f;
  host_name(path, name, sizeof name);
  f = fopen(name, "wb");
  fputs(text, f);
  fclose(f);
}
static void host_clean(void) {
  static const char *paths[] = { LISTS_PATH, "/cache/todo/_.cache",
    "/cache/todo/listA.cache", "/cache/todo/listB.cache", "/cache/todo/listC.cache" };
  size_t i;
  for (i = 0; i < sizeof paths / sizeof paths[0]; i++) fake_remove(paths[i]);
}

static int fake_repeat;
static int fake_key_repeat(void) { return fake_repeat; }

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
  FAKE.mkdir = fake_mkdir;
  FAKE.remove = fake_remove;
  FAKE.rename = fake_rename;
  FAKE.log = fake_log;
  FAKE.key_repeat = fake_key_repeat;
  api = &FAKE;
  memset(&T, 0, sizeof T);
  host_clean();
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
  host_clean();
}

/* ---- a start the queue refused ------------------------------------------ */

/* One request at a time on the device, so http_start can say no: the Claude
 * terminal is mid-answer, or another window's sync is in flight. The app used
 * to return without a word, leaving the last status on screen and the next
 * attempt ten minutes away -- which is indistinguishable from "broken". */
static uint32_t    fake_ticks(void)        { return 1000; }
static int         fake_net_ready(void)    { return 1; }
static const char *fake_token(void)        { return "ya29.token"; }
static int fake_start_refused(const char *m, const char *u, const char *b,
                              const char *ct, const char *tok, int ms) {
  (void)m; (void)u; (void)b; (void)ct; (void)tok; (void)ms;
  return -1;
}

void test_todo_a_refused_start_says_so_and_retries_soon(void) {
  use_fake_api();
  FAKE.ticks_ms = fake_ticks;
  FAKE.net_ready = fake_net_ready;
  FAKE.google_token = fake_token;
  FAKE.http_start = fake_start_refused;
  snprintf(T.status, sizeof T.status, "%s", "2 tasks");

  sync_begin();

  CHECK_EQ(T.stage, SYNC_IDLE);
  CHECK(strstr(T.status, "busy") != NULL);
  CHECK_EQ(T.next_auto, 1000 + RETRY_MS);      /* not AUTO_EVERY_MS away */
}

/* ---- more than one list -------------------------------------------------- */

static const char LISTS_REPLY[] =
  "{\"kind\":\"tasks#taskLists\",\"items\":["
  "{\"kind\":\"tasks#taskList\",\"id\":\"listA\",\"title\":\"My Tasks\"},"
  "{\"kind\":\"tasks#taskList\",\"id\":\"listB\",\"title\":\"Groceries\"},"
  "{\"kind\":\"tasks#taskList\",\"id\":\"listC\",\"title\":\"Work\"}]}";

void test_todo_the_lists_reply_fills_the_table_and_keeps_the_current_one(void) {
  use_fake_api();
  snprintf(T.reply, sizeof T.reply, "%s", LISTS_REPLY);
  snprintf(T.list_id, sizeof T.list_id, "%s", "listB");   /* chosen last time */

  absorb_lists();

  CHECK_EQ(T.nlists, 3);
  CHECK(!strcmp(T.lists[0].id, "listA"));
  CHECK(!strcmp(T.lists[0].name, "My Tasks"));
  CHECK(!strcmp(T.lists[2].name, "Work"));
  CHECK_EQ(T.cur, 1);                          /* still Groceries */
  CHECK(!strcmp(T.list_id, "listB"));
  CHECK(host_exists(LISTS_PATH));              /* remembered for offline */
}

void test_todo_a_list_that_vanished_falls_back_to_the_first(void) {
  use_fake_api();
  snprintf(T.reply, sizeof T.reply, "%s", LISTS_REPLY);
  snprintf(T.list_id, sizeof T.list_id, "%s", "gone");
  absorb_lists();
  CHECK_EQ(T.cur, 0);
  CHECK(!strcmp(T.list_id, "listA"));
}

void test_todo_each_list_keeps_its_own_cache(void) {
  use_fake_api();
  snprintf(T.reply, sizeof T.reply, "%s", LISTS_REPLY);
  absorb_lists();                              /* on listA */
  add_item("a1", "Fix the bike", 0, 0, 0);
  cache_save();

  select_list(1);                              /* Groceries */
  CHECK_EQ(T.n, 0);
  add_item("", "Eggs", 0, 1, 0);
  add_item("", "Milk", 0, 1, 0);
  cache_save();

  select_list(0);
  CHECK_EQ(T.n, 1);
  CHECK(!strcmp(T.item[0].title, "Fix the bike"));

  select_list(1);
  CHECK_EQ(T.n, 2);
  CHECK(!strcmp(T.item[1].title, "Milk"));
  CHECK_EQ(T.item[1].dirty, 1);                /* still waiting to be pushed */
  host_clean();
}

void test_todo_the_lists_and_the_choice_survive_a_restart_offline(void) {
  use_fake_api();
  snprintf(T.reply, sizeof T.reply, "%s", LISTS_REPLY);
  absorb_lists();
  select_list(2);
  add_item("w1", "Ship it", 0, 0, 0);
  cache_save();

  memset(&T, 0, sizeof T);                     /* a fresh run, no network */
  lists_load();
  cache_load();
  CHECK_EQ(T.nlists, 3);
  CHECK_EQ(T.cur, 2);
  CHECK(!strcmp(T.list_id, "listC"));
  CHECK_EQ(T.n, 1);
  CHECK(!strcmp(T.item[0].title, "Ship it"));
  host_clean();
}

void test_todo_switching_waits_for_a_sync_in_flight(void) {
  use_fake_api();
  snprintf(T.reply, sizeof T.reply, "%s", LISTS_REPLY);
  absorb_lists();
  T.stage = SYNC_PUSH;                         /* a push for listA is out */
  select_list(1);
  CHECK_EQ(T.cur, 0);                          /* refused: that reply matters */
  CHECK(strstr(T.status, "wait") != NULL);
  host_clean();
}

void test_todo_left_and_right_cycle_the_lists(void) {
  use_fake_api();
  snprintf(T.reply, sizeof T.reply, "%s", LISTS_REPLY);
  absorb_lists();
  key_list(CAPP_KEY_RIGHT); CHECK_EQ(T.cur, 1);
  key_list(CAPP_KEY_RIGHT); CHECK_EQ(T.cur, 2);
  key_list(CAPP_KEY_RIGHT); CHECK_EQ(T.cur, 0);   /* wraps */
  key_list(CAPP_KEY_LEFT);  CHECK_EQ(T.cur, 2);
  host_clean();
}

static int fake_poll_done(char *out, size_t n) {
  snprintf(out, n, "%s", "{\"items\":[{\"id\":\"x\",\"title\":\"From the old list\"}]}");
  return 40;
}

void test_todo_switching_during_a_pull_drops_that_reply(void) {
  use_fake_api();
  FAKE.ticks_ms = fake_ticks;
  FAKE.google_token = fake_token;
  FAKE.http_poll = fake_poll_done;
  snprintf(T.reply, sizeof T.reply, "%s", LISTS_REPLY);
  absorb_lists();
  T.stage = SYNC_PULL;                         /* listA's tasks are on their way */
  select_list(1);
  CHECK_EQ(T.cur, 1);                          /* allowed: nothing is lost */

  sync_tick();                                 /* listA's reply lands */
  CHECK_EQ(T.stage, SYNC_IDLE);
  CHECK_EQ(T.n, 0);                            /* and is not filed under listB */
  host_clean();
}

/* ---- the sync, driven end to end ----------------------------------------
 *
 * The user's complaint was two-sided: syncs that quietly did not happen, and
 * a sync that interrupted -- the app fetched, then fetched again, then came
 * back ten minutes later and did it once more while you were reading. So the
 * rule is now "everything at open, nothing after unless asked", and these
 * tests are what say it stays that way.
 *
 * The fake HTTP is a script: a queue of replies handed out in order, with a
 * record of every URL asked for, because "did it sync the other lists" is a
 * question about which requests were made and in what order.
 */
#define SCRIPT_MAX 12

static struct {
  const char *reply[SCRIPT_MAX];
  int    code[SCRIPT_MAX];
  int    n, at;
  char   url[SCRIPT_MAX][200];
  char   method[SCRIPT_MAX][8];
  int    asked;
  int    pending;            /* a request is out and has not been collected */
} S;

static void script_reset(void) { memset(&S, 0, sizeof S); }

static void script_add(const char *reply, int code) {
  if (S.n >= SCRIPT_MAX) return;
  S.reply[S.n] = reply;
  S.code[S.n] = code;
  S.n++;
}

static int script_start(const char *m, const char *u, const char *b,
                        const char *ct, const char *tok, int ms) {
  (void)b; (void)ct; (void)tok; (void)ms;
  if (S.pending) return -1;              /* one at a time, as on the device */
  if (S.asked < SCRIPT_MAX) {
    snprintf(S.url[S.asked], sizeof S.url[0], "%s", u ? u : "");
    snprintf(S.method[S.asked], sizeof S.method[0], "%s", m ? m : "");
  }
  S.asked++;
  S.pending = 1;
  return 0;
}

static int script_poll(char *out, size_t n) {
  const char *r;
  int code;
  if (!S.pending) return -1;
  if (S.at >= S.n) {                     /* nothing scripted: still waiting */
    return CAPP_HTTP_PENDING;
  }
  r = S.reply[S.at];
  code = S.code[S.at];
  S.at++;
  S.pending = 0;
  if (out && n) snprintf(out, n, "%s", r ? r : "");
  return code;
}

static uint32_t s_now;
static uint32_t script_ticks(void) { return s_now; }
static int script_net(void) { return 1; }
static const char *script_token(void) { return "ya29.token"; }

/* app_tick goes through the toolbar, which holds an api pointer of its own
 * and dereferences it -- so a fake that omits these crashes the suite rather
 * than failing a check, which is a much worse way to find out. */
static void   script_damage(CRect r) { (void)r; }
/* Painting on the host draws nowhere, but it still has to be callable: the
 * layout arithmetic is the thing under test and it runs inside paint. */
static void   script_fill(CRect r, uint16_t c) { (void)r; (void)c; }
static void   script_frame(CRect r, uint16_t c) { (void)r; (void)c; }
static void   script_text(short x, short y, const char *s, uint16_t f, uint16_t b) {
  (void)x; (void)y; (void)s; (void)f; (void)b;
}
static CRect  script_area(void) { CRect r; r.x = r.y = r.w = r.h = 0; return r; }
static void   script_log(const char *m) { (void)m; }
/* On screen, not started for a command: the tick's first sync depends on it. */
static int    script_headless(void) { return 0; }
/* No card behind it: every stat misses, so the menu bar setting is off. */
static int    script_stat(const char *path, CappStat *st) { (void)path; (void)st; return -1; }

static void use_script_api(void) {
  use_fake_api();
  script_reset();
  s_now = 1000;
  FAKE.ticks_ms = script_ticks;
  FAKE.net_ready = script_net;
  FAKE.google_token = script_token;
  FAKE.http_start = script_start;
  FAKE.http_poll = script_poll;
  FAKE.damage = script_damage;
  FAKE.paint_area = script_area;
  FAKE.fill = script_fill;
  FAKE.frame = script_frame;
  FAKE.text = script_text;
  FAKE.log = script_log;
  FAKE.headless = script_headless;
  FAKE.stat = script_stat;
  toolbar_init(&FAKE, LIST_ACTIONS, NLIST, LIST_ICONS, 1);
}

/* Run the app's tick until the sync settles or we give up, the way the
 * shell's loop would. */
static int settle(int max_ticks) {
  int i;
  for (i = 0; i < max_ticks; i++) {
    s_now += 5;
    app_tick(0, s_now);
    if (T.stage == SYNC_IDLE && S.at >= S.n) return i;
  }
  return -1;
}

static int url_has(int i, const char *needle) {
  return i < S.asked && strstr(S.url[i], needle) != NULL;
}

static const char THREE_LISTS[] =
  "{\"items\":["
  "{\"id\":\"listA\",\"title\":\"My Tasks\"},"
  "{\"id\":\"listB\",\"title\":\"Groceries\"},"
  "{\"id\":\"listC\",\"title\":\"Work\"}]}";

static const char TASKS_A[] =
  "{\"items\":[{\"id\":\"a1\",\"title\":\"Fix the bike\",\"status\":\"needsAction\"}]}";
static const char TASKS_B[] =
  "{\"items\":[{\"id\":\"b1\",\"title\":\"Eggs\",\"status\":\"needsAction\"},"
  "{\"id\":\"b2\",\"title\":\"Milk\",\"status\":\"completed\"}]}";
static const char TASKS_C[] =
  "{\"items\":[{\"id\":\"c1\",\"title\":\"Ship it\",\"status\":\"needsAction\"}]}";

/* One open, one sweep: the lists, then the list on screen, then the rest.
 * Every list ends up fetched, which is what makes the overview real. */
void test_todo_one_open_syncs_every_list_once(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);

  sync_begin();
  CHECK(settle(400) >= 0);

  CHECK_EQ(S.asked, 4);
  CHECK(url_has(0, "users/@me/lists"));
  CHECK(url_has(1, "lists/listA/tasks"));
  CHECK(url_has(2, "lists/listB/tasks"));
  CHECK(url_has(3, "lists/listC/tasks"));
  CHECK_EQ(T.stage, SYNC_IDLE);
  CHECK_EQ(T.online, 1);
  host_clean();
}

/* The interruption the user actually felt: it came back and did it again
 * while they were reading. Nothing may start on its own after the sweep. */
void test_todo_nothing_syncs_again_until_it_is_asked_to(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);
  CHECK_EQ(S.asked, 4);

  /* Twenty minutes of ticks, well past any old ten-minute timer. */
  {
    int i;
    for (i = 0; i < 4000; i++) { s_now += 300; app_tick(0, s_now); }
  }
  CHECK_EQ(S.asked, 4);

  /* Asking still works. */
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  script_add(THREE_LISTS, 200);
  do_action(ACT_SYNC);
  CHECK(S.asked > 4);
  host_clean();
}

/* A list you are not looking at is still fetched, and lands in its own file
 * rather than on screen. */
void test_todo_a_list_off_screen_is_fetched_into_its_own_cache(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);

  /* On screen is still listA, untouched by the other two replies. */
  CHECK(!strcmp(T.list_id, "listA"));
  CHECK_EQ(T.n, 1);
  CHECK(!strcmp(T.item[0].title, "Fix the bike"));

  /* And the others are on the card. */
  CHECK(host_exists("/cache/todo/listB.cache"));
  CHECK(host_exists("/cache/todo/listC.cache"));
  select_list(1);
  CHECK_EQ(T.n, 2);
  CHECK(!strcmp(T.item[0].title, "Eggs"));
  CHECK_EQ(T.item[1].done, 1);
  host_clean();
}

/* A failure on one list must not stop the others: the sweep is the only
 * chance they get until the user asks again. */
void test_todo_one_list_failing_does_not_abandon_the_rest(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add("", -403);                  /* listB refused */
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);

  CHECK_EQ(S.asked, 4);
  CHECK(url_has(3, "lists/listC/tasks"));
  CHECK(host_exists("/cache/todo/listC.cache"));
  host_clean();
}

/* Pending edits go first, or a pull would overwrite them with the server's
 * older answer -- the one way an offline edit is silently lost. */
void test_todo_pending_edits_are_pushed_before_the_sweep_pulls(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add("{\"id\":\"new1\"}", 200);   /* the push */
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);

  add_item("", "Written on a train", 0, 1, 0);
  sync_begin();
  CHECK(settle(400) >= 0);

  CHECK(!strcmp(S.method[1], "POST"));
  CHECK(url_has(1, "lists/listA/tasks"));
  CHECK(!strcmp(S.method[2], "GET"));
  host_clean();
}


/* ---- the overview, and the keys around it -------------------------------- */

/* Everything open, under the list it belongs to. The list on screen comes
 * from memory (it may hold edits not yet in its file); the rest from their
 * cache files, which is why the sweep has to have filled them. */
void test_todo_the_overview_groups_every_lists_open_tasks(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);

  over_build();
  /* listA: Fix the bike. listB: Eggs (Milk is done). listC: Ship it. */
  CHECK_EQ(T.nover, 3);
  CHECK(!strcmp(T.over[0].title, "Fix the bike"));
  CHECK_EQ(T.over[0].list, 0);
  CHECK_EQ(T.over[0].head, 1);            /* first of its list: a heading */
  CHECK(!strcmp(T.over[1].title, "Eggs"));
  CHECK_EQ(T.over[1].list, 1);
  CHECK_EQ(T.over[1].head, 1);
  CHECK(!strcmp(T.over[2].title, "Ship it"));
  CHECK_EQ(T.over[2].list, 2);
  host_clean();
}

/* "What is there to do" is the question, so a finished task is not an
 * answer -- and neither is one on its way out. */
void test_todo_the_overview_leaves_out_what_is_done_or_going(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);

  T.item[0].done = 1;                     /* the only task on listA */
  over_build();
  CHECK_EQ(T.nover, 2);
  CHECK(!strcmp(T.over[0].title, "Eggs"));
  CHECK_EQ(T.over[0].head, 1);            /* listB now leads, so it heads */

  T.item[0].done = 0;
  T.item[0].deleted = 1;
  over_build();
  CHECK_EQ(T.nover, 2);
  host_clean();
}

/* Enter on a row goes to that row's list. */
void test_todo_the_overview_opens_the_list_a_row_belongs_to(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);

  do_action(ACT_ALL);
  CHECK_EQ(T.view, VIEW_ALL);
  T.osel = 2;                             /* "Ship it", on listC */
  key_all(CAPP_KEY_ENTER);
  CHECK_EQ(T.view, VIEW_LIST);
  CHECK_EQ(T.cur, 2);
  CHECK(!strcmp(T.list_id, "listC"));
  host_clean();
}

/* A mis-hit `d` used to mean retyping the task: the only way out of a
 * pending delete was to add it again. Until the sweep sends it, a delete is
 * a local opinion and the same key takes it back. */
void test_todo_d_toggles_a_pending_delete_rather_than_only_setting_it(void) {
  use_fake_api();
  add_item("g1", "Delete me by accident", 0, 0, 0);
  T.sel = 0;

  delete_selected();
  CHECK_EQ(T.item[0].deleted, 1);
  CHECK_EQ(T.item[0].dirty, 1);
  CHECK_EQ(T.n, 1);                       /* still there, struck through */

  delete_selected();
  CHECK_EQ(T.item[0].deleted, 0);
  CHECK_EQ(T.item[0].dirty, 1);           /* and still needs telling */
  CHECK_EQ(T.n, 1);
  host_clean();
}

/* One that never reached Google has nothing at the other end to delete, so
 * it simply goes -- and there is nothing to toggle back to. */
void test_todo_deleting_something_never_synced_removes_it_outright(void) {
  use_fake_api();
  add_item("", "Never sent", 0, 1, 0);
  T.sel = 0;
  delete_selected();
  CHECK_EQ(T.n, 0);
  host_clean();
}

/* Escape now reaches the app first: every view above the list uses it to go
 * back a step, and the list declines it so the shell leaves the app. That
 * decline is the contract -- if the list ever returned 1 for Escape, the
 * only way out would be fn-`. */
void test_todo_escape_goes_back_from_a_subview_and_is_declined_by_the_list(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);

  do_action(ACT_ALL);
  CHECK_EQ(T.view, VIEW_ALL);
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);
  CHECK_EQ(T.view, VIEW_LIST);

  do_action(ACT_LISTS);
  CHECK_EQ(T.view, VIEW_LISTS);
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);
  CHECK_EQ(T.view, VIEW_LIST);

  do_action(ACT_ADD);
  CHECK_EQ(T.view, VIEW_ADD);
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);
  CHECK_EQ(T.view, VIEW_LIST);

  /* Top level: not the app's key. The shell takes this as "leave". */
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 0);
  host_clean();
}

/* Escape while typing must not throw away the app along with the draft. */
void test_todo_escape_while_typing_abandons_the_draft_only(void) {
  use_fake_api();
  do_action(ACT_ADD);
  key_add('h');
  key_add('i');
  CHECK_EQ(T.draft_len, 2);
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);
  CHECK_EQ(T.view, VIEW_LIST);
  CHECK_EQ(T.n, 0);                       /* not added */
  host_clean();
}


/* A task object from Google is three hundred bytes of etag, selfLink,
 * position and updated, and forty of them do not fit in the reply buffer --
 * nor in the kernel's, which fills up and cannot tell a body that ended from
 * one that was cut off. The list came back short with nothing saying so.
 * Asking only for the fields that are read is what makes the arithmetic
 * work, so the mask is the thing worth pinning down. */
void test_todo_the_urls_ask_google_for_only_the_fields_that_are_read(void) {
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);

  CHECK(url_has(0, "fields=items(id,title)"));
  CHECK(url_has(1, "fields=items(id,title,status)"));
  CHECK(url_has(2, "fields=items(id,title,status)"));

  /* Forty trimmed tasks have to leave room in the buffer they land in. */
  CHECK(40 * 64 < REPLY_MAX);
  host_clean();
}

/* A click in the overview cannot be divided back into a row: a heading makes
 * the row under it twice as tall. It is matched against what paint drew. */
void test_todo_a_click_in_the_overview_finds_the_row_that_was_drawn(void) {
  CRect c;
  use_script_api();
  script_add(THREE_LISTS, 200);
  script_add(TASKS_A, 200);
  script_add(TASKS_B, 200);
  script_add(TASKS_C, 200);
  sync_begin();
  CHECK(settle(400) >= 0);

  do_action(ACT_ALL);
  c.x = 0; c.y = 0; c.w = 240; c.h = 135;
  paint_all(c);

  /* Three rows, each under its own heading, so each sits a row lower than a
   * flat list would put it. */
  CHECK(T.oy[0] >= ROW_H);
  CHECK(T.oy[1] > T.oy[0]);
  CHECK(T.oy[2] > T.oy[1]);

  T.osel = 0;
  CHECK_EQ(app_click(0, 100, (short)T.oy[2], 0), 1);
  CHECK_EQ(T.osel, 2);                   /* first click selects */
  CHECK_EQ(T.view, VIEW_ALL);
  CHECK_EQ(app_click(0, 100, (short)T.oy[2], 0), 1);
  CHECK_EQ(T.view, VIEW_LIST);           /* the second opens */
  CHECK_EQ(T.cur, 2);
  host_clean();
}

/* The shell matches an action's chord before the app's key handler runs, so
 * a chord that is also a real key's byte takes that key away from the whole
 * app. Calendar shipped exactly that -- "ctrl-m" is the byte Enter sends --
 * and it cost the day view its Enter. Todo's chords are checked here so the
 * next one added cannot repeat it. */
void test_todo_no_action_chord_collides_with_a_real_key(void) {
  int i;
  for (i = 0; i < NLIST; i++) {
    unsigned char k = (unsigned char)LIST_ACTIONS[i].key;
    CHECK(k != CAPP_KEY_ENTER);        /* 0x0D, ctrl-m */
    CHECK(k != CAPP_KEY_BACK);         /* 0x08, ctrl-h */
    CHECK(k != 0x09);                  /* tab, ctrl-i */
    CHECK(k != CAPP_KEY_ESC);          /* 0x1B */
  }
}

/* ---- the menu bar, from the keyboard ------------------------------------- */

/* fn-b is the whole entry point: it shows the bar and hands it the keyboard.
 * Before, the bar existed only if a mouse had moved, so on a device with no
 * mouse the menus -- and the chords printed beside them -- could not be
 * reached or even seen. */
void test_todo_the_menu_opens_with_the_chord_and_takes_the_keyboard(void) {
  use_script_api();
  CHECK_EQ(toolbar_h(), 0);                 /* no mouse, no bar */
  CHECK_EQ(toolbar_has_keys(), 0);

  CHECK_EQ(app_key(0, CAPP_KEY_MENU), 1);
  CHECK(toolbar_h() > 0);                   /* on screen without a mouse */
  CHECK_EQ(toolbar_has_keys(), 1);

  CHECK_EQ(app_key(0, CAPP_KEY_MENU), 1);   /* and away again */
  CHECK_EQ(toolbar_h(), 0);
  CHECK_EQ(toolbar_has_keys(), 0);
  host_clean();
}

/* Left and right walk the names, down opens one, up and down walk its items. */
void test_todo_the_arrows_walk_the_menus_and_their_items(void) {
  use_script_api();
  app_key(0, CAPP_KEY_MENU);
  CHECK_EQ(TB.hot_menu, 0);
  CHECK_EQ(TB.open, -1);

  app_key(0, CAPP_KEY_RIGHT);
  CHECK_EQ(TB.hot_menu, 1);
  app_key(0, CAPP_KEY_LEFT);
  CHECK_EQ(TB.hot_menu, 0);

  app_key(0, CAPP_KEY_DOWN);                /* opens it */
  CHECK_EQ(TB.open, 0);
  CHECK_EQ(TB.hot_item, 0);
  app_key(0, CAPP_KEY_DOWN);
  CHECK_EQ(TB.hot_item, 1);
  app_key(0, CAPP_KEY_UP);
  CHECK_EQ(TB.hot_item, 0);

  /* Up off the top closes it rather than wrapping: the way back to the bar
   * has to be the way you came in. */
  app_key(0, CAPP_KEY_UP);
  CHECK_EQ(TB.open, -1);
  CHECK_EQ(toolbar_has_keys(), 1);          /* still in the bar */
  host_clean();
}

/* An open dropdown follows the highlight across the bar, which is the only
 * way to read what is in each menu without reopening every one. */
void test_todo_an_open_menu_follows_the_arrows_across_the_bar(void) {
  use_script_api();
  app_key(0, CAPP_KEY_MENU);
  app_key(0, CAPP_KEY_DOWN);
  CHECK_EQ(TB.open, 0);
  app_key(0, CAPP_KEY_RIGHT);
  CHECK_EQ(TB.open, 1);
  CHECK_EQ(TB.hot_item, 0);
  host_clean();
}

/* Enter runs the highlighted item as an action -- the same action a click
 * would have produced -- and gives the keyboard back to the app. */
void test_todo_enter_runs_the_item_and_returns_the_keyboard(void) {
  use_script_api();
  app_key(0, CAPP_KEY_MENU);
  app_key(0, CAPP_KEY_DOWN);                /* Task menu, first item: Add */
  CHECK(tb_menu_item(TB.open, TB.hot_item) != 0);
  CHECK_EQ(tb_menu_item(TB.open, TB.hot_item)->action, ACT_ADD);

  app_key(0, CAPP_KEY_ENTER);
  CHECK_EQ(T.view, VIEW_ADD);               /* the action actually ran */
  CHECK_EQ(toolbar_has_keys(), 0);
  CHECK_EQ(TB.open, -1);
  host_clean();
}

/* Escape steps out one level at a time, as it does everywhere else: the
 * dropdown, then the bar, and only then does the app see it. */
void test_todo_escape_steps_out_of_the_menu_before_the_app_sees_it(void) {
  use_script_api();
  app_key(0, CAPP_KEY_MENU);
  app_key(0, CAPP_KEY_DOWN);
  CHECK_EQ(TB.open, 0);

  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);
  CHECK_EQ(TB.open, -1);
  CHECK_EQ(toolbar_has_keys(), 1);

  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 1);
  CHECK_EQ(toolbar_has_keys(), 0);

  /* Now it is the app's again, and the list declines it so the shell leaves. */
  CHECK_EQ(app_key(0, CAPP_KEY_ESC), 0);
  host_clean();
}

/* While the menu has the keyboard the app must get nothing -- not arrows,
 * not letters. A key that fell through typed into the app underneath an
 * open menu. */
void test_todo_the_app_gets_no_keys_while_the_menu_is_up(void) {
  use_script_api();
  add_item("g1", "Do not tick me", 0, 0, 0);
  T.sel = 0;

  app_key(0, CAPP_KEY_MENU);
  CHECK_EQ(app_key(0, 'a'), 1);             /* would have opened the draft */
  CHECK_EQ(T.view, VIEW_LIST);
  CHECK_EQ(app_key(0, ' '), 1);             /* would have ticked it off */
  CHECK_EQ(T.item[0].done, 0);
  CHECK_EQ(app_wants_text(0), 0);           /* and voice must not type */
  host_clean();
}

/* ---- the printed page ---------------------------------------------------- */

static char fake_printed[PAGE_MAX];
static int fake_print(const char *doc) { snprintf(fake_printed, sizeof fake_printed, "%s", doc); return 0; }
static void fake_now(CappTime *t) { memset(t, 0, sizeof *t); t->synced = 2; t->year = 2026; t->month = 9; t->day = 20; t->hour = 10; t->min = 5; }
static const char *fake_print_status(void) { return "printed"; }

void test_todo_prints_open_tasks_as_boxes_and_done_ones_ticked(void) {
  use_fake_api();
  FAKE.print = fake_print;
  FAKE.now = fake_now;
  FAKE.print_status = fake_print_status;
  T.nlists = 1; T.cur = 0;
  snprintf(T.lists[0].name, sizeof T.lists[0].name, "%s", "Groceries");
  add_item("a", "Milk", 0, 0, 0);
  add_item("b", "Eggs", 1, 0, 0);
  add_item("c", "Gone", 0, 0, 1);
  add_item("d", "Bread", 0, 0, 0);
  T.view = VIEW_LIST;
  print_page();
  CHECK(!strcmp(fake_printed,
    "# Groceries\n[ ] Milk\n[ ] Bread\n---\n[x] Eggs\n---\nprinted 20 Sep 10:05\n"));
  CHECK_EQ(T.printing, 1);
  CHECK(!strcmp(T.status, "printing..."));
}

void test_todo_prints_the_overview_with_a_heading_per_list(void) {
  use_fake_api();
  FAKE.print = fake_print;
  FAKE.now = fake_now;
  T.nlists = 2;
  snprintf(T.lists[0].name, sizeof T.lists[0].name, "%s", "Home");
  snprintf(T.lists[1].name, sizeof T.lists[1].name, "%s", "Work");
  T.nover = 3;
  snprintf(T.over[0].title, sizeof T.over[0].title, "%s", "Bins"); T.over[0].list = 0; T.over[0].head = 1;
  snprintf(T.over[1].title, sizeof T.over[1].title, "%s", "Report"); T.over[1].list = 1; T.over[1].head = 1;
  snprintf(T.over[2].title, sizeof T.over[2].title, "%s", "Slides"); T.over[2].list = 1; T.over[2].head = 0;
  T.view = VIEW_ALL;
  print_page();
  CHECK(!strcmp(fake_printed,
    "# All lists\n## Home\n[ ] Bins\n## Work\n[ ] Report\n[ ] Slides\n---\nprinted 20 Sep 10:05\n"));
}

static int fake_print_busy(const char *doc) { (void)doc; return -1; }

void test_todo_says_so_when_the_printer_is_busy_or_missing(void) {
  use_fake_api();
  FAKE.print = fake_print_busy;
  FAKE.now = fake_now;
  T.view = VIEW_LIST;
  print_page();
  CHECK_EQ(T.printing, 0);
  CHECK(strstr(T.status, "still printing") != NULL);
}

void test_todo_page_stops_short_rather_than_overflowing(void) {
  int i;
  use_fake_api();
  FAKE.print = fake_print;
  FAKE.now = fake_now;
  T.nlists = 1; T.cur = 0;
  for (i = 0; i < MAX_ITEMS; i++)
    add_item("x", "A task title that is as long as the app allows", 0, 0, 0);
  T.view = VIEW_LIST;
  print_page();
  CHECK((int)strlen(fake_printed) < PAGE_MAX);
  CHECK(strstr(fake_printed, "[ ] A task") != NULL);
}

void test_todo_held_arrow_moves_but_held_space_does_not_toggle(void) {
  use_fake_api();
  add_item("a", "One", 0, 0, 0);
  add_item("b", "Two", 0, 0, 0);
  T.view = VIEW_LIST;
  fake_repeat = 1;
  key_list(CAPP_KEY_DOWN);
  CHECK_EQ(T.sel, 1);
  key_list(' ');
  CHECK_EQ(T.item[1].done, 0);          /* a repeat of space is ignored */
  fake_repeat = 0;
  key_list(' ');
  CHECK_EQ(T.item[1].done, 1);          /* a real press toggles */
}
