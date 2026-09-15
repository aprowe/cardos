/* The conversation the on-device Claude agent keeps on the card, exercised on
 * host files. What has to hold: a request is the model's own bytes plus what
 * we typed; a reply is scanned through a small window and never held whole;
 * a tool_use is never separated from the tool_result that answers it.
 */
#include "tinytest.h"

#include "kernel/sys/chatlog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

/* ---- host file ops: stdio behind the chatlog's table --------------------- */

static FILE *s_files[16];

static int h_open(const char *path, int flags) {
  const char *mode;
  int i;
  if (flags & CHATLOG_O_APPEND) mode = "ab";
  else if (flags & CHATLOG_O_TRUNC) mode = "wb";
  else if (flags & CHATLOG_O_WRITE) mode = "r+b";
  else mode = "rb";
  for (i = 0; i < 16; i++) if (!s_files[i]) break;
  if (i == 16) return -1;
  s_files[i] = fopen(path, mode);
  return s_files[i] ? i : -1;
}
static int h_read(int fd, void *buf, size_t n) { return (int)fread(buf, 1, n, s_files[fd]); }
static int h_write(int fd, const void *buf, size_t n) { return (int)fwrite(buf, 1, n, s_files[fd]); }
static int h_seek(int fd, int32_t off, int whence) {
  if (fseek(s_files[fd], off, whence == CHATLOG_SEEK_END ? SEEK_END : SEEK_SET) != 0) return -1;
  return (int)ftell(s_files[fd]);
}
static void h_close(int fd) { fclose(s_files[fd]); s_files[fd] = NULL; }
static int h_remove(const char *path) { return remove(path); }
static int h_rename(const char *a, const char *b) { remove(b); return rename(a, b); }

static const ChatlogOps OPS = { h_open, h_read, h_write, h_seek, h_close, h_remove, h_rename };

static char s_dir[260];

/* One directory, emptied before each test. A fresh numbered one per test
 * was fourteen directories in the repo root after every run, and nothing
 * ever removed them. */
static void fresh(void) {
  static const char *const LEFTOVERS[] = { "reply.json", "request.json" };
  char path[300];
  size_t i;
  snprintf(s_dir, sizeof s_dir, "chatlog_test");
#ifdef _WIN32
  _mkdir(s_dir);
#else
  mkdir(s_dir, 0700);
#endif
  chatlog_init(&OPS, s_dir);
  chatlog_clear();
  for (i = 0; i < sizeof LEFTOVERS / sizeof LEFTOVERS[0]; i++) {
    snprintf(path, sizeof path, "%s/%s", s_dir, LEFTOVERS[i]);
    remove(path);
  }
}

static char *slurp(const char *name) {
  static char buf[65536];
  char path[300];
  FILE *f;
  size_t n;
  snprintf(path, sizeof path, "%s/%s", s_dir, name);
  f = fopen(path, "rb");
  if (!f) { buf[0] = 0; return buf; }
  n = fread(buf, 1, sizeof buf - 1, f);
  buf[n] = 0;
  fclose(f);
  return buf;
}

static void spit(const char *name, const char *text) {
  char path[300];
  FILE *f;
  snprintf(path, sizeof path, "%s/%s", s_dir, name);
  f = fopen(path, "wb");
  fwrite(text, 1, strlen(text), f);
  fclose(f);
}

/* ---- the history ---------------------------------------------------------- */

void test_chatlog_a_user_turn_is_a_text_block_escaped(void) {
  fresh();
  CHECK_EQ(chatlog_add_user("say \"hi\"\nplease \\ now"), 0);
  CHECK(!strcmp(slurp("history.json"),
    "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"say \\\"hi\\\"\\nplease \\\\ now\"}]}"));
}

void test_chatlog_turns_are_comma_separated(void) {
  fresh();
  chatlog_add_user("one");
  chatlog_add_user("two");
  CHECK(strstr(slurp("history.json"), "\"one\"}]},{\"role\"") != NULL);
}

void test_chatlog_a_request_wraps_the_history_in_head_and_tail(void) {
  fresh();
  chatlog_add_user("hello");
  CHECK_EQ(chatlog_write_request("request.json", "{\"messages\":[", "]}"), 0);
  CHECK(!strcmp(slurp("request.json"),
    "{\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}]}"));
}

/* ---- the reply ------------------------------------------------------------ */

static const char REPLY_TEXT[] =
  "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"model\":\"claude-opus-5\","
  "\"content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"abc\"},"
  "{\"type\":\"text\",\"text\":\"Four.\\nThat is 2+2 \\u00e9\"}],"
  "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":10,\"output_tokens\":3}}";

static const char REPLY_TOOLS[] =
  "{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\","
  "\"content\":[{\"type\":\"text\",\"text\":\"Opening it.\"},"
  "{\"type\":\"tool_use\",\"id\":\"toolu_01\",\"name\":\"open\",\"input\":{\"app\":\"todo\"}},"
  "{\"type\":\"tool_use\",\"id\":\"toolu_02\",\"name\":\"brightness\",\"input\":{\"percent\":40}},"
  "{\"type\":\"tool_use\",\"id\":\"toolu_03\",\"name\":\"wifi\",\"input\":{\"on\":false}}],"
  "\"stop_reason\":\"tool_use\"}";

static const char REPLY_ERROR[] =
  "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
  "\"message\":\"invalid x-api-key\"}}";

void test_chatlog_scans_text_and_stop_reason(void) {
  ChatReply r;
  fresh();
  spit("reply.json", REPLY_TEXT);
  CHECK_EQ(chatlog_scan_reply("reply.json", &r), 0);
  CHECK_EQ(r.stop, CHAT_STOP_END);
  CHECK(!strcmp(r.text, "Four.\nThat is 2+2 \xc3\xa9"));
  CHECK_EQ(r.ntools, 0);
  CHECK(!r.error);
}

void test_chatlog_scans_tool_calls_with_string_number_and_bool(void) {
  ChatReply r;
  fresh();
  spit("reply.json", REPLY_TOOLS);
  CHECK_EQ(chatlog_scan_reply("reply.json", &r), 0);
  CHECK_EQ(r.stop, CHAT_STOP_TOOL);
  CHECK(!strcmp(r.text, "Opening it."));
  CHECK_EQ(r.ntools, 3);
  CHECK(!strcmp(r.tool[0].id, "toolu_01"));
  CHECK(!strcmp(r.tool[0].name, "open"));
  CHECK(!strcmp(r.tool[0].arg, "todo"));
  CHECK(!strcmp(r.tool[1].name, "brightness"));
  CHECK_EQ(r.tool[1].num, 40);
  CHECK(!strcmp(r.tool[1].arg, "40"));
  CHECK(!strcmp(r.tool[2].name, "wifi"));
  CHECK_EQ(r.tool[2].num, 0);
  CHECK(!strcmp(r.tool[2].arg, "false"));
}

/* The model's note recipe is six calls in one turn. A turn with more calls
 * than the reply struct holds used to drop the surplus on the floor, while
 * the history kept the whole content array: every later request was refused
 * for tool_use ids with no tool_result, until the conversation was reset.
 * The surplus ids come back too, so they can be answered. */
void test_chatlog_surplus_tool_calls_keep_their_ids(void) {
  ChatReply r;
  char reply[4096];
  int i, n;
  fresh();
  n = snprintf(reply, sizeof reply,
    "{\"id\":\"msg_3\",\"type\":\"message\",\"role\":\"assistant\",\"content\":[");
  for (i = 0; i < CHAT_TOOLS_MAX + 2; i++)
    n += snprintf(reply + n, sizeof reply - (size_t)n,
      "%s{\"type\":\"tool_use\",\"id\":\"toolu_%02d\",\"name\":\"key\",\"input\":{\"name\":\"enter\"}}",
      i ? "," : "", i);
  snprintf(reply + n, sizeof reply - (size_t)n, "],\"stop_reason\":\"tool_use\"}");
  spit("reply.json", reply);
  CHECK_EQ(chatlog_scan_reply("reply.json", &r), 0);
  CHECK_EQ(r.ntools, CHAT_TOOLS_MAX);
  CHECK_EQ(r.nextra, 2);
  CHECK(!strcmp(r.extra_id[0], "toolu_08"));
  CHECK(!strcmp(r.extra_id[1], "toolu_09"));
}

void test_chatlog_an_error_reply_is_its_message(void) {
  ChatReply r;
  fresh();
  spit("reply.json", REPLY_ERROR);
  CHECK_EQ(chatlog_scan_reply("reply.json", &r), 0);
  CHECK(r.error);
  CHECK(!strcmp(r.text, "authentication_error: invalid x-api-key"));
}

void test_chatlog_an_error_with_no_message_shows_its_type(void) {
  ChatReply r;
  fresh();
  spit("reply.json", "{\"type\":\"error\",\"error\":{\"type\":\"rate_limit_error\",\"message\":\"Error\"}}");
  CHECK_EQ(chatlog_scan_reply("reply.json", &r), 0);
  CHECK(r.error);
  /* "Error" says nothing; the type is the only clue there is. */
  CHECK(!strcmp(r.text, "rate_limit_error: Error"));
}

void test_chatlog_a_string_straddling_the_window_survives(void) {
  /* Padding pushes the text block across the 512-byte reader window. */
  static char big[4096];
  char pad[700];
  ChatReply r;
  memset(pad, 'p', sizeof pad - 1); pad[sizeof pad - 1] = 0;
  fresh();
  snprintf(big, sizeof big,
    "{\"id\":\"%s\",\"content\":[{\"type\":\"text\",\"text\":\"across the edge\"}],"
    "\"stop_reason\":\"end_turn\"}", pad);
  spit("reply.json", big);
  CHECK_EQ(chatlog_scan_reply("reply.json", &r), 0);
  CHECK(!strcmp(r.text, "across the edge"));
}

/* ---- the reply into the history ------------------------------------------ */

void test_chatlog_the_reply_content_is_copied_byte_for_byte(void) {
  const char *h;
  fresh();
  chatlog_add_user("2+2?");
  spit("reply.json", REPLY_TEXT);
  CHECK_EQ(chatlog_add_reply("reply.json"), 0);
  h = slurp("history.json");
  /* The thinking block, signature and all, went in untouched. */
  CHECK(strstr(h, ",{\"role\":\"assistant\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"abc\"},"
                  "{\"type\":\"text\",\"text\":\"Four.\\nThat is 2+2 \\u00e9\"}]}") != NULL);
}

void test_chatlog_tool_results_are_one_user_turn(void) {
  ChatToolResult res[2] = { { "toolu_01", "opened Todo. actions: add, tick" },
                            { "toolu_02", "brightness 40%" } };
  fresh();
  CHECK_EQ(chatlog_add_tool_results(res, 2), 0);
  CHECK(!strcmp(slurp("history.json"),
    "{\"role\":\"user\",\"content\":["
    "{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_01\",\"content\":\"opened Todo. actions: add, tick\"},"
    "{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_02\",\"content\":\"brightness 40%\"}]}"));
}

/* ---- trimming ------------------------------------------------------------- */

void test_chatlog_trim_drops_the_oldest_turn_whole(void) {
  const char *h;
  fresh();
  chatlog_add_user("first question, which is longish");
  spit("reply.json", REPLY_TEXT);
  chatlog_add_reply("reply.json");
  chatlog_add_user("second");
  /* Cap below the total: the first user turn and its answer must both go,
   * leaving a history that starts with a user text turn. */
  CHECK_EQ(chatlog_trim(120), 0);
  h = slurp("history.json");
  CHECK(!strcmp(h, "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"second\"}]}"));
}

void test_chatlog_trim_never_leaves_a_tool_result_first(void) {
  ChatToolResult res[1] = { { "toolu_01", "opened Todo" } };
  const char *h;
  fresh();
  chatlog_add_user("open todo");                 /* user text */
  spit("reply.json", REPLY_TOOLS);
  chatlog_add_reply("reply.json");               /* assistant tool_use */
  chatlog_add_tool_results(res, 1);              /* user tool_result */
  chatlog_add_user("thanks");                    /* user text */
  CHECK_EQ(chatlog_trim(200), 0);
  h = slurp("history.json");
  CHECK(strncmp(h, "{\"role\":\"user\",\"content\":[{\"type\":\"text\"", 40) == 0);
  CHECK(strstr(h, "tool_result") == NULL);
  CHECK(strstr(h, "thanks") != NULL);
}

void test_chatlog_trim_under_cap_changes_nothing(void) {
  fresh();
  chatlog_add_user("small");
  CHECK_EQ(chatlog_trim(4096), 0);
  CHECK(!strcmp(slurp("history.json"), "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"small\"}]}"));
}

void test_chatlog_size_reports_the_history(void) {
  fresh();
  CHECK_EQ(chatlog_size(), 0);
  chatlog_add_user("abc");
  CHECK_EQ(chatlog_size(), (int)strlen("{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"abc\"}]}"));
}
