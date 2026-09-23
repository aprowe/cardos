/* Claude on the device. See agent.h for why the loop is here and not in the
 * app, and kernel/sys/chatlog.h for why the conversation is on the card. */

#include "kernel/sys/agent.h"
#include "kernel/app/capp.h"   /* the card layout */

#include "kernel/app/capprun.h"
#include "kernel/app/cmdline.h"
#include "kernel/drv/display.h"
#include "kernel/fs/fs.h"
#include "kernel/net/httpq.h"
#include "kernel/net/wifi.h"
#include "kernel/sys/chatlog.h"
#include "kernel/sys/input.h"
#include "kernel/ui/overlay.h"
#include "kernel/ui/shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "agent";

#define KEY_PATH    CAPP_CONFIG "/claude.key"
#define DIR         CAPP_CACHE "/claude"
#define REQUEST     "request.json"
#define REPLY       "reply.json"
#define URL         "https://api.anthropic.com/v1/messages"
#define HISTORY_CAP 16384        /* bytes of conversation resent per request */
#define ROUNDS_MAX  10           /* tool rounds in one ask */
#define TIMEOUT_MS  60000
#define TOAST_MS    4000
#define TRANSCRIPT  2048

/* ---- the request, all but the conversation ---------------------------- */

/* In flash, and put together per request: HEAD_SYSTEM, then the command
 * catalog (/cache/commands.txt, which changes as apps come and go), then
 * HEAD_TOOLS. The conversation goes between that and TAIL. The tools are the
 * vocabulary in kernel/sys/rpc.h, described for a model; every one of them
 * is something a keyboard could have done, and `do` is what an app declared
 * it can do without one. */
static const char HEAD_SYSTEM[] =
  "{\"model\":\"claude-opus-5\",\"max_tokens\":1024,"
  "\"output_config\":{\"effort\":\"low\"},"
  "\"system\":\"You are Claude, running on CardOS: a pocket computer with a keyboard "
  "and a 40 by 16 character screen. Answer briefly, in plain text: no markdown, no "
  "lists, no headings. You can act on the device with the tools. When asked to do "
  "something, do it with as few calls as you can, then say in a few words what you "
  "did. Prefer the do tool: it runs an app's command directly, without opening the "
  "app, and its result is the answer. The commands, one per line as APP COMMAND "
  "then each argument as name:type, then what it does:\\n";
static const char HEAD_TOOLS[] =
  "\\nFor anything else, open the app with the open tool; its result lists the "
  "app's actions, run with the action tool, and text is typed with the type tool. "
  "The apps are Todo, Calendar, Edit (which is where notes go), IDE, Files, "
  "Explorer, Photos, Web, Stocks, Screen, Mines, Pinball, Memory and Settings.\","
  "\"tools\":["
  "{\"name\":\"do\",\"description\":\"Run one of the commands in the system prompt, "
  "as one line: APP COMMAND then the arguments, text in double quotes, e.g. todo "
  "add \\\"fix the car\\\". The result is the app's answer.\",\"input_schema\":{"
  "\"type\":\"object\",\"properties\":{\"line\":{\"type\":\"string\"}},\"required\":"
  "[\"line\"]}},"
  "{\"name\":\"open\",\"description\":\"Open an app by name. The result lists the "
  "actions it offers.\",\"input_schema\":{\"type\":\"object\",\"properties\":{\"app\":"
  "{\"type\":\"string\"}},\"required\":[\"app\"]}},"
  "{\"name\":\"action\",\"description\":\"Run one of the open app's actions by id, "
  "as listed when it was opened.\",\"input_schema\":{\"type\":\"object\",\"properties\":"
  "{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}},"
  "{\"name\":\"type\",\"description\":\"Type text into the open app, as keystrokes. "
  "Use a newline for enter.\",\"input_schema\":{\"type\":\"object\",\"properties\":"
  "{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}},"
  "{\"name\":\"key\",\"description\":\"Press one key.\",\"input_schema\":{\"type\":"
  "\"object\",\"properties\":{\"name\":{\"type\":\"string\",\"enum\":[\"escape\","
  "\"enter\",\"up\",\"down\",\"left\",\"right\"]}},\"required\":[\"name\"]}},"
  "{\"name\":\"shell\",\"description\":\"Switch to a shell.\",\"input_schema\":{\"type\":"
  "\"object\",\"properties\":{\"which\":{\"type\":\"string\",\"enum\":[\"launcher\","
  "\"desktop\",\"console\"]}},\"required\":[\"which\"]}},"
  "{\"name\":\"brightness\",\"description\":\"Set the screen brightness, 0 to 100.\","
  "\"input_schema\":{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":"
  "\"integer\"}},\"required\":[\"percent\"]}},"
  "{\"name\":\"wifi\",\"description\":\"Turn WiFi on or off.\",\"input_schema\":"
  "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}},\"required\":"
  "[\"on\"]}}"
  "],\"messages\":[";
static const char TAIL[] = "]}";

/* ---- state ------------------------------------------------------------ */

static int      s_inited;
static int      s_running;              /* a request is in flight */
static int      s_rounds;               /* tool rounds so far in this ask */
static char     s_auth[400];            /* the headers, built from the key */
static char     s_transcript[TRANSCRIPT];
static unsigned s_generation;
static char     s_status[48];
static int64_t  s_seen_us;              /* the terminal was on screen at */
static int64_t  s_toast_until_us;

static int64_t now_us(void) { return esp_timer_get_time(); }

/* ---- the chatlog over the card ---------------------------------------- */

/* The flag values are the same on both sides, on purpose. */
static int  c_open(const char *p, int f)              { return fs_open(p, f); }
static int  c_read(int fd, void *b, size_t n)         { return fs_read(fd, b, n); }
static int  c_write(int fd, const void *b, size_t n)  { return fs_write(fd, b, n); }
static int  c_seek(int fd, int32_t off, int whence)   { return fs_seek(fd, off, whence); }
static void c_close(int fd)                           { fs_close(fd); }
static int  c_remove(const char *p)                   { return fs_remove(p); }
static int  c_rename(const char *a, const char *b)    { return fs_rename(a, b); }
static const ChatlogOps OPS = { c_open, c_read, c_write, c_seek, c_close, c_remove, c_rename };

void agent_init(void) {
  if (s_inited) return;
  s_inited = 1;
  chatlog_init(&OPS, DIR);
}

static void ensure_dir(void) {
  if (fs_mkdir(CAPP_CACHE) != 0) { /* already there */ }
  if (fs_mkdir(DIR) != 0) { /* already there */ }
}

/* ---- the transcript ---------------------------------------------------- */

static void transcript_add(const char *prefix, const char *text) {
  size_t have = strlen(s_transcript);
  size_t need = strlen(prefix) + strlen(text) + 2;
  if (need >= sizeof s_transcript) need = sizeof s_transcript - 1;
  /* Older lines fall off the front, whole. */
  while (have + need >= sizeof s_transcript && have > 0) {
    char *nl = strchr(s_transcript, '\n');
    if (!nl) { s_transcript[0] = 0; have = 0; break; }
    memmove(s_transcript, nl + 1, strlen(nl + 1) + 1);
    have = strlen(s_transcript);
  }
  snprintf(s_transcript + have, sizeof s_transcript - have, "%s%s\n", prefix, text);
  s_generation++;
}

unsigned    agent_generation(void) { return s_generation; }
const char *agent_transcript(void) { return s_transcript; }
const char *agent_status(void)     { return s_status; }
void        agent_seen(void)       { s_seen_us = now_us(); }
int         agent_busy(void)       { return s_running; }

static void status(const char *s) {
  snprintf(s_status, sizeof s_status, "%s", s);
  s_generation++;
}

/* ---- the key ------------------------------------------------------------ */

static int read_key(void) {
  char key[240];
  int fd, n, i;
  fd = fs_open(KEY_PATH, FS_O_READ);
  if (fd < 0) return 0;
  n = fs_read(fd, key, sizeof key - 1);
  fs_close(fd);
  if (n <= 0) return 0;
  key[n] = 0;
  for (i = 0; key[i]; i++) if (key[i] == '\r' || key[i] == '\n' || key[i] == ' ') { key[i] = 0; break; }
  if (!key[0]) return 0;
  /* Two kinds of key. An API key goes in x-api-key. A Claude login's OAuth
   * token -- what Claude Code itself uses, which is what `sk-ant-oat` means
   * -- is a bearer, and /v1/messages wants the oauth beta header with it. */
  if (!strncmp(key, "sk-ant-oat", 10))
    snprintf(s_auth, sizeof s_auth,
             "Authorization: Bearer %.240s\nanthropic-beta: oauth-2025-04-20\n"
             "anthropic-version: 2023-06-01", key);
  else
    snprintf(s_auth, sizeof s_auth, "x-api-key: %s\nanthropic-version: 2023-06-01", key);
  return 1;
}

int agent_has_key(void) { return read_key(); }

/* ---- executing the vocabulary ----------------------------------------- */

/* The focused app's action ids, comma-separated, after `prefix`. */
static void list_actions(const AppDef *a, char *out, size_t cap) {
  int n = 0, i;
  size_t len;
  const CappAction *act = a ? capprun_actions(a, &n) : NULL;
  if (!act || !n) { snprintf(out, cap, "%s offers no actions", a ? a->name : "it"); return; }
  snprintf(out, cap, "actions: ");
  for (i = 0; i < n; i++) {
    len = strlen(out);
    snprintf(out + len, cap - len, "%s%s", i ? ", " : "", act[i].id);
  }
}

void agent_execute(const RpcCmd *c, char *result, size_t cap) {
  switch (c->verb) {
  case RPC_OPEN: {
    char name[RPC_ARG_MAX];
    const AppDef *a;
    /* "notes" is what a person calls the editor. */
    snprintf(name, sizeof name, "%s", !strcmp(c->arg, "notes") ? "edit" : c->arg);
    if (shell_open_app(name) != 0) {
      snprintf(result, cap, "no app called %.40s", c->arg);
      return;
    }
    a = shell_running_app();
    snprintf(result, cap, "opened %s. ", a ? a->name : name);
    list_actions(a, result + strlen(result), cap - strlen(result));
    return;
  }
  case RPC_ACTION: {
    const AppDef *a = shell_running_app();
    if (!a) { snprintf(result, cap, "no app is open"); return; }
    if (capprun_action_invoke(a, c->arg) == 0) {
      snprintf(result, cap, "done. ");
      list_actions(a, result + strlen(result), cap - strlen(result));
    } else {
      snprintf(result, cap, "%s has no action %.30s. ", a->name, c->arg);
      list_actions(a, result + strlen(result), cap - strlen(result));
    }
    return;
  }
  case RPC_SHELL:
    shell_switch(c->arg);
    snprintf(result, cap, "%s", c->arg);
    return;
  case RPC_BRIGHT:
    display_set_brightness(c->num);
    snprintf(result, cap, "brightness %d%%", display_brightness());
    return;
  case RPC_WIFI:
    if (c->num) { wifi_connect_saved(20000); snprintf(result, cap, "%s", wifi_status()); }
    else { wifi_stop(); snprintf(result, cap, "wifi off"); }
    return;
  case RPC_SAY:
    if (input_text(c->arg) > 0) snprintf(result, cap, "typed");
    else snprintf(result, cap, "nothing here is taking text");
    return;
  case RPC_KEY: {
    uint8_t k = 0;
    if (!strcmp(c->arg, "escape")) k = 0x1B;
    else if (!strcmp(c->arg, "enter")) k = 0x0D;
    else if (!strcmp(c->arg, "up")) k = 0x80;
    else if (!strcmp(c->arg, "down")) k = 0x81;
    else if (!strcmp(c->arg, "left")) k = 0x82;
    else if (!strcmp(c->arg, "right")) k = 0x83;
    if (k) { shell_feed_key(k); snprintf(result, cap, "pressed %s", c->arg); }
    else snprintf(result, cap, "no key called %.20s", c->arg);
    return;
  }
  case RPC_NONE:
    snprintf(result, cap, "%.60s", c->arg[0] ? c->arg : "not understood");
    return;
  case RPC_DO: {
    /* An app's command, open or not: the same path the console's `do`
     * takes, checked against what the app declared. */
    char buf[RPC_ARG_MAX];
    const char *w[2 + CAPP_CMD_ARGS_MAX + 8];
    int n = cmdline_split(c->arg, buf, sizeof buf, w, (int)(sizeof w / sizeof w[0]));
    if (n < 2) { snprintf(result, cap, "do needs an app and a command"); return; }
    if (capprun_command(w[0], w[1], n - 2, w + 2, result, cap) == 0 && !result[0])
      snprintf(result, cap, "done");
    return;
  }
  default:
    snprintf(result, cap, "not a command");
    return;
  }
}

/* A tool call as the model made it, into the vocabulary. 0 if it is one. */
static int tool_to_cmd(const ChatToolCall *t, RpcCmd *c) {
  memset(c, 0, sizeof *c);
  snprintf(c->arg, sizeof c->arg, "%s", t->arg);
  c->num = t->num;
  if (!strcmp(t->name, "open"))       c->verb = RPC_OPEN;
  else if (!strcmp(t->name, "action")) c->verb = RPC_ACTION;
  else if (!strcmp(t->name, "type")) { c->verb = RPC_SAY; rpc_one_line(c->arg); }
  else if (!strcmp(t->name, "key"))    c->verb = RPC_KEY;
  else if (!strcmp(t->name, "shell"))  c->verb = RPC_SHELL;
  else if (!strcmp(t->name, "brightness")) {
    c->verb = RPC_BRIGHT;
    if (c->num > 100) c->num = 100;
    if (c->num < 0) c->num = 0;
  }
  else if (!strcmp(t->name, "wifi"))   c->verb = RPC_WIFI;
  else if (!strcmp(t->name, "do"))     c->verb = RPC_DO;
  else return -1;
  return 0;
}

/* HEAD_SYSTEM + the catalog, JSON-escaped + HEAD_TOOLS, into one buffer.
 * Static: a few kilobytes, and only one request is ever being written. A
 * catalog too long for the room left is cut at a line, not mid-line. */
/* The catalog was 1.8 KB at 25 commands in 11 apps (2026-09-23), and 1.5 KB
 * of room cut it off: room for about twice that. Allocated for the moment
 * the request is written and freed after, not static -- as static buffers
 * these held 12 KB of heap for good, on a machine where a TLS handshake is
 * short of heap already. The caller frees what this returns. */
#define CATALOG_MAX 4096

static char *build_head(void) {
  size_t cap = sizeof HEAD_SYSTEM + sizeof HEAD_TOOLS + 2 * CATALOG_MAX;
  char *head = malloc(cap), *cat = malloc(CATALOG_MAX);
  size_t o = 0, i;
  int fd, n = 0;

  if (!head || !cat) { free(head); free(cat); return NULL; }
  memcpy(head, HEAD_SYSTEM, sizeof HEAD_SYSTEM - 1);
  o = sizeof HEAD_SYSTEM - 1;

  fd = fs_open(CAPPRUN_CATALOG, FS_O_READ);
  if (fd >= 0) { n = fs_read(fd, cat, CATALOG_MAX - 1); fs_close(fd); }
  if (n < 0) n = 0;
  cat[n] = 0;
  if (n == CATALOG_MAX - 1) {                      /* cut at the last line */
    char *nl = strrchr(cat, '\n');
    if (nl) nl[1] = 0;
  }
  for (i = 0; cat[i] && o + 8 < cap - sizeof HEAD_TOOLS; i++) {
    char ch = cat[i];
    if (ch == '"' || ch == '\\') { head[o++] = '\\'; head[o++] = ch; }
    else if (ch == '\n') { head[o++] = '\\'; head[o++] = 'n'; }
    else if ((unsigned char)ch >= 0x20) head[o++] = ch;
  }
  if (!cat[0]) {
    static const char NONE[] = "(no app has commands yet)";
    memcpy(head + o, NONE, sizeof NONE - 1);
    o += sizeof NONE - 1;
  }
  free(cat);
  memcpy(head + o, HEAD_TOOLS, sizeof HEAD_TOOLS);  /* with its terminator */
  return head;
}

/* ---- the loop --------------------------------------------------------- */

static int start_request(void) {
  ensure_dir();
  /* The last reply goes before the next request is made. The download only
   * opens the file once headers have arrived, so a proxy that is down or a
   * network that has gone leaves the old file in place -- and on_reply used
   * to scan it, believe it, run its tool calls again and append its turn to
   * the history a second time, round after round, until ROUNDS_MAX. */
  fs_remove(DIR "/" REPLY);
  if (chatlog_trim(HISTORY_CAP) != 0) ESP_LOGW(TAG, "could not trim the history");
  {
    char *head = build_head();
    int rc = head ? chatlog_write_request(REQUEST, head, TAIL) : -1;
    free(head);
    if (rc != 0) return -4;
  }
  if (httpq_start_files(&s_running, URL, DIR "/" REQUEST, "application/json",
                        s_auth, DIR "/" REPLY, TIMEOUT_MS) != 0)
    return -4;
  s_running = 1;
  status("thinking");
  return 0;
}

int agent_ask(const char *text) {
  int rc;
  if (!s_inited) agent_init();
  if (!text || !*text) return -4;
  if (s_running || httpq_active()) return -1;
  if (!fs_mounted()) return -3;
  if (!read_key()) return -2;
  ensure_dir();
  if (chatlog_add_user(text) != 0) return -4;
  transcript_add("> ", text);
  s_rounds = 0;
  rc = start_request();
  if (rc != 0) status("could not start the request");
  return rc;
}

void agent_new(void) {
  if (!s_inited) agent_init();
  chatlog_clear();
  s_transcript[0] = 0;
  s_status[0] = 0;
  s_generation++;
}

/* Show the answer where the user is: the terminal, if it is on screen, or
 * an overlay over whatever is, taken down a few seconds later. */
static void show_answer(const char *text) {
  if (now_us() - s_seen_us < 500 * 1000) return;
  overlay_result(text);
  s_toast_until_us = now_us() + (int64_t)TOAST_MS * 1000;
}

static void finish_with(const char *text) {
  s_running = 0;
  status("");
  if (text && *text) { transcript_add("", text); show_answer(text); }
}

/* The reply is in. Read it, act on it, and either start the next round or
 * stop. */
static void on_reply(int rc) {
  static ChatReply r;              /* 3 KB: static rather than on the stack */
  ChatToolResult results[CHAT_TOOLS_MAX + CHAT_EXTRA_MAX];
  static char result_text[CHAT_TOOLS_MAX][160];
  int i;

  if (rc < 0 && chatlog_scan_reply(REPLY, &r) != 0) {
    char line[64];
    snprintf(line, sizeof line, "request failed (%d)", rc);
    finish_with(line);
    return;
  }
  if (rc >= 0 && chatlog_scan_reply(REPLY, &r) != 0) {
    finish_with("could not read the reply");
    return;
  }
  if (r.error) {
    finish_with(r.text[0] ? r.text : "the API returned an error");
    return;
  }

  /* The model's turn goes into the history as its own bytes, tool calls,
   * thinking and all, before anything is done about it. */
  if (chatlog_add_reply(REPLY) != 0) {
    finish_with("could not keep the reply");
    return;
  }
  if (r.text[0]) transcript_add("", r.text);

  if (r.stop != CHAT_STOP_TOOL || r.ntools == 0) {
    s_running = 0;
    status(r.stop == CHAT_STOP_MAX ? "answer cut short" : "");
    if (r.text[0]) show_answer(r.text);
    return;
  }

  if (++s_rounds > ROUNDS_MAX) {
    finish_with("stopped: too many steps");
    return;
  }

  for (i = 0; i < r.ntools; i++) {
    RpcCmd c;
    char line[220];
    if (tool_to_cmd(&r.tool[i], &c) != 0)
      snprintf(result_text[i], sizeof result_text[i], "no tool called %.20s", r.tool[i].name);
    else
      agent_execute(&c, result_text[i], sizeof result_text[i]);
    results[i].id = r.tool[i].id;
    results[i].text = result_text[i];
    snprintf(line, sizeof line, "%s %.40s: %.140s", r.tool[i].name, r.tool[i].arg, result_text[i]);
    transcript_add("-> ", line);
    ESP_LOGI(TAG, "%s", line);
  }
  /* Every tool_use gets a tool_result, including the ones past the cap that
   * were not run: an unanswered id makes the API refuse every later request
   * in the conversation. */
  for (i = 0; i < r.nextra; i++) {
    results[r.ntools + i].id = r.extra_id[i];
    results[r.ntools + i].text = "not run: too many tool calls in one turn";
  }
  if (chatlog_add_tool_results(results, r.ntools + r.nextra) != 0) {
    finish_with("could not keep the tool results");
    return;
  }
  if (start_request() != 0) finish_with("could not continue");
}

void agent_tick(void) {
  if (s_toast_until_us && now_us() > s_toast_until_us) {
    s_toast_until_us = 0;
    overlay_close();
  }
  if (!s_running) return;
  {
    int rc = httpq_poll(NULL, 0);
    if (rc == HTTPQ_PENDING) return;
    on_reply(rc);
  }
}
