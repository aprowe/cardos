/* Commands from outside an app -- the console's `do`, a voice line, an AI --
 * are words until they are checked against what the app declared. Nothing
 * reaches an app's handler that does not fit its table. */

#include <string.h>

#include "tinytest.h"
#include "kernel/app/cmdline.h"

static const CappParam TEXT[] = { { "text", CAPP_ARG_TEXT, "what the task says" } };
static const CappParam MIXED[] = {
  { "count", CAPP_ARG_INT,    "how many" },
  { "loud",  CAPP_ARG_BOOL,   "out loud" },
  { "view",  CAPP_ARG_CHOICE, "list|all|day" },
};
static const CappAction ADD  = { "add", "New", "Task", 0, 1, "add a task",
                                 TEXT, 1, CAPP_CMD_YES };
static const CappAction MIX  = { "mix", "Mix", 0, 0, 2, "three kinds",
                                 MIXED, 3, CAPP_CMD_YES };
static const CappAction SYNC = { "sync", "Sync", "Task", 0, 3, "sync now",
                                 0, 0, CAPP_CMD_YES | CAPP_CMD_NET };
static const CappAction GUI  = { "tick", "Tick", "Task", 0, 4, 0, 0, 0, 0 };

void test_cmdline_splits_words_and_quotes(void) {
  char buf[128];
  const char *w[8];
  int n = cmdline_split("todo add \"fix the car\" now", buf, sizeof buf, w, 8);
  CHECK_EQ(n, 4);
  CHECK(strcmp(w[0], "todo") == 0);
  CHECK(strcmp(w[2], "fix the car") == 0);
  CHECK(strcmp(w[3], "now") == 0);
  CHECK_EQ(cmdline_split("  ", buf, sizeof buf, w, 8), 0);
  CHECK(cmdline_split("a \"unclosed", buf, sizeof buf, w, 8) < 0);
}

void test_cmdline_binds_text_including_the_rest_of_the_line(void) {
  /* "carlos, make a todo to fix car" arrives as `do todo add fix car`: the
   * last text parameter takes the rest, quoted or not. */
  const char *words[] = { "fix", "car" };
  const char *argv[CAPP_CMD_ARGS_MAX];
  char join[64], why[80];
  CHECK_EQ(cmdline_bind(&ADD, 2, words, argv, join, sizeof join, why, sizeof why), 1);
  CHECK(strcmp(argv[0], "fix car") == 0);
}

void test_cmdline_checks_count_and_types(void) {
  const char *argv[CAPP_CMD_ARGS_MAX];
  char join[64], why[80];
  const char *ok[] = { "3", "yes", "all" };
  const char *not_int[] = { "three", "yes", "all" };
  const char *not_bool[] = { "3", "maybe", "all" };
  const char *not_choice[] = { "3", "no", "week" };
  const char *two[] = { "3", "no" };

  CHECK_EQ(cmdline_bind(&MIX, 3, ok, argv, join, sizeof join, why, sizeof why), 3);
  CHECK(cmdline_bind(&MIX, 3, not_int, argv, join, sizeof join, why, sizeof why) < 0);
  CHECK(strstr(why, "count") != 0);                 /* says which */
  CHECK(cmdline_bind(&MIX, 3, not_bool, argv, join, sizeof join, why, sizeof why) < 0);
  CHECK(cmdline_bind(&MIX, 3, not_choice, argv, join, sizeof join, why, sizeof why) < 0);
  CHECK(strstr(why, "list|all|day") != 0);          /* and what would do */
  CHECK(cmdline_bind(&MIX, 2, two, argv, join, sizeof join, why, sizeof why) < 0);
  CHECK(cmdline_bind(&SYNC, 0, 0, argv, join, sizeof join, why, sizeof why) == 0);
  CHECK(cmdline_bind(&SYNC, 1, ok, argv, join, sizeof join, why, sizeof why) < 0);
}

void test_cmdline_a_gui_action_is_not_a_command(void) {
  const char *argv[CAPP_CMD_ARGS_MAX];
  char join[64], why[80];
  CHECK(cmdline_bind(&GUI, 0, 0, argv, join, sizeof join, why, sizeof why) < 0);
  CHECK(!cmdline_is_command(&GUI));
  CHECK(cmdline_is_command(&ADD));
}

void test_cmdline_finds_a_command_by_id(void) {
  const CappAction table[] = { GUI, ADD, SYNC };
  CHECK(cmdline_find(table, 3, "add") == &table[1]);
  CHECK(cmdline_find(table, 3, "tick") == 0);       /* GUI-only: not found */
  CHECK(cmdline_find(table, 3, "nope") == 0);
}

void test_cmdline_catalog_line(void) {
  char line[160];
  cmdline_catalog_line("todo", &ADD, line, sizeof line);
  CHECK(strcmp(line, "todo add text:text # add a task") == 0);
  cmdline_catalog_line("x", &MIX, line, sizeof line);
  CHECK(strcmp(line, "x mix count:int loud:bool view:list|all|day # three kinds") == 0);
  cmdline_catalog_line("todo", &SYNC, line, sizeof line);
  CHECK(strcmp(line, "todo sync net # sync now") == 0);
}
