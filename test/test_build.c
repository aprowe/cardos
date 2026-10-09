/* The Claude terminal, on the host.
 *
 * No network here: the fake http() is never reached by these tests. What is
 * under test is the painting, which is the part that showed on the device --
 * every character typed redrew the whole window, log and all.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

/* Every app exports these two names; the pinball test already has them. */
#define capp_info claude_capp_info
#define capp_main claude_capp_main
#include "apps/build.c"

static CappUi   INST;
static uint32_t NOW;
static int      FILLS, TEXTS;

static void fake_fill(CRect r, uint16_t colour) { (void)r; (void)colour; FILLS++; }
static void fake_text(int16_t x, int16_t y, const char *s, uint16_t f, uint16_t b) {
  (void)x; (void)y; (void)s; (void)f; (void)b;
  TEXTS++;
}
static uint32_t fake_ticks(void) { return NOW; }
static int fake_open(const char *p, int f) { (void)p; (void)f; return -1; }
static void fake_ui(const CappUi *ui) { INST = *ui; }
/* Somewhere that is not CAPP_PROXY_DEFAULT, as `env PROXY` is on a device
 * pointed at the droplet. */
static const char *fake_proxy(void) { return "http://arowe.example:8080"; }

static CardApi API;
static CRect   WIN;

/* The shell's damage channel: marks union into the next paint's clip, and a
 * paint with nothing marked is the whole window -- which is how an app tells
 * its own repaint from one it did not ask for. */
static CRect PENDING;
static int   HAVE_PENDING;

static void fake_damage(CRect r) {
  int x0, y0, x1, y1;
  if (!HAVE_PENDING) { PENDING = r; HAVE_PENDING = 1; return; }
  x0 = r.x < PENDING.x ? r.x : PENDING.x;
  y0 = r.y < PENDING.y ? r.y : PENDING.y;
  x1 = r.x + r.w > PENDING.x + PENDING.w ? r.x + r.w : PENDING.x + PENDING.w;
  y1 = r.y + r.h > PENDING.y + PENDING.h ? r.y + r.h : PENDING.y + PENDING.h;
  PENDING.x = (int16_t)x0; PENDING.y = (int16_t)y0;
  PENDING.w = (int16_t)(x1 - x0); PENDING.h = (int16_t)(y1 - y0);
}

static CRect fake_paint_area(void) { return HAVE_PENDING ? PENDING : WIN; }

static void paint_now(void) {
  INST.paint(INST.state, WIN);
  HAVE_PENDING = 0;
}

static int fake_remove(const char *p) { (void)p; return 0; }
static int fake_write(int fd, const void *b, size_t n) { (void)fd; (void)b; return (int)n; }
static void fake_close(int fd) { (void)fd; }

static void boot(void) {
  char arg0[] = "claude";
  char *argv[1];
  argv[0] = arg0;

  fakeapi_init(&API);
  API.version = CAPP_API_VERSION;
  API.fill = fake_fill;
  API.text = fake_text;
  API.ticks_ms = fake_ticks;
  API.open = fake_open;
  API.remove = fake_remove;     /* job_clear: the saved job is gone */
  API.write = fake_write;
  API.close = fake_close;
  API.ui = fake_ui;
  API.proxy = fake_proxy;
  API.damage = fake_damage;
  API.paint_area = fake_paint_area;
  HAVE_PENDING = 0;

  NOW = 1000;
  FILLS = TEXTS = 0;
  memset(&INST, 0, sizeof INST);
  WIN.x = 0; WIN.y = 0; WIN.w = 240; WIN.h = 135;
  capp_main(&API, 1, argv);
  paint_now();                            /* the shell's first paint */
}

/* One keypress, painted the way the shell does it: only if key() asks. */
static int press(unsigned char k) {
  FILLS = TEXTS = 0;
  if (INST.key(INST.state, k)) paint_now();
  return FILLS + TEXTS;
}

/* Build posted to the compiled-in laptop address while the OS's pre-flight
 * went to `env PROXY` -- the droplet -- so the check passed and every message
 * went nowhere. It starts from what the kernel resolved. */
void test_build_talks_to_the_server_the_os_uses(void) {
  boot();
  CHECK(strcmp(C.base, "http://arowe.example:8080") == 0);
  CHECK(strcmp(C.base, CAPP_PROXY_DEFAULT) != 0);
}

/* While an answer is pending the server says what it is doing on the second
 * line, and that is what the bar shows -- cut to fit, and gone once the
 * answer lands. */
static const char *HTTP_REPLY;
static int fake_http(const char *m, const char *u, const char *b, const char *ct,
                     const char *bearer, char *out, size_t n, int t) {
  (void)m; (void)u; (void)b; (void)ct; (void)bearer; (void)t;
  snprintf(out, n, "%s", HTTP_REPLY);
  return (int)strlen(out);
}

void test_build_shows_what_the_server_is_doing(void) {
  boot();
  API.http = fake_http;
  C.job = 7;
  HTTP_REPLY = "pending\nstep 2/3: writing timer.c";
  poll_now();
  CHECK(strcmp(C.progress, "step 2/3: writing timer.c") == 0);
  CHECK(C.job == 7);

  HTTP_REPLY = "pending\nstep 2/3: reading a-file-with-a-very-long-name.c";
  poll_now();
  CHECK(strlen(C.progress) == sizeof C.progress - 1);    /* cut, not overrun */

  HTTP_REPLY = "pending\n";                                /* an older server */
  poll_now();
  CHECK(C.progress[0] == 0);
  CHECK_EQ(C.log_seen, 0);

  {
    int before = C.log.n;
    HTTP_REPLY = "pending\nstep 1/2: reading\nread kernel/app/capp.h\n> using memo.c";
    poll_now();
    CHECK_EQ(C.log_seen, 2);                    /* both lines, counted */
    CHECK(C.log.n >= before + 2);              /* and in the window */
    CHECK(strcmp(C.progress, "step 1/2: reading") == 0);

    HTTP_REPLY = "pending\nstep 1/2: writing\nwrite apps/timer.c";
    poll_now();
    CHECK_EQ(C.log_seen, 3);                    /* the next poll asks from 3 */
  }

  HTTP_REPLY = "done\nall done";
  poll_now();
  CHECK(C.job == 0);
  CHECK(C.progress[0] == 0);

  /* A poll that brings nothing new asks for no repaint: every poll used to,
   * and each repaint redrew the bar. With a message queued the bar shows no
   * dots, so a repeated "pending" with the same step changes nothing. */
  C.job = 7;
  C.nqueued = 1;
  C.check_update = 0;                         /* no update_check here */
  HTTP_REPLY = "pending\nstep 1/2: reading";
  NOW = C.next_poll = 5000;
  CHECK(INST.tick(INST.state, NOW));          /* the step is new */
  HAVE_PENDING = 0;
  NOW = C.next_poll;
  CHECK(!INST.tick(INST.state, NOW));         /* the same step again */
  CHECK(!HAVE_PENDING);
  HTTP_REPLY = "pending\nstep 2/2: writing";
  NOW = C.next_poll;
  CHECK(INST.tick(INST.state, NOW));          /* the bar's words changed */
  C.nqueued = 0;
}

static void type_and_enter(const char *s) {
  while (*s) INST.key(INST.state, (unsigned char)*s++);
  INST.key(INST.state, CAPP_KEY_ENTER);
}

/* Enter while a request runs used to send at once and poll the new job
 * instead, so the first answer was never collected. Now it waits its turn. */
void test_build_queues_what_is_typed_while_a_request_runs(void) {
  int i;
  boot();
  API.http = fake_http;
  C.job = 7;                                  /* one is running */

  type_and_enter("add a bar");
  CHECK_EQ(C.nqueued, 1);
  CHECK(!C.sending);                          /* held, not sent */
  CHECK_EQ(C.job, 7);                         /* still waiting on the first */
  type_and_enter("make it blue");
  CHECK_EQ(C.nqueued, 2);

  HTTP_REPLY = "done\nfirst answer";          /* the first answer lands */
  poll_now();
  CHECK_EQ(C.job, 0);
  CHECK(C.sending);                           /* the next goes on the next tick */
  CHECK(strcmp(C.pending, "add a bar") == 0); /* in the order typed */
  CHECK_EQ(C.nqueued, 1);
  CHECK(strcmp(C.queued[0], "make it blue") == 0);

  for (i = 0; i < QUEUE_MAX + 2; i++) type_and_enter("more");
  CHECK_EQ(C.nqueued, QUEUE_MAX);             /* full is full */
  CHECK(C.in_len > 0);                        /* and the refused text is kept */

  C.in_len = 0; C.input[0] = 0;
  C.sending = 0;
  type_and_enter("/new");
  CHECK_EQ(C.nqueued, 0);                     /* nothing carries over */
}

void test_build_installs_a_ui(void) {
  boot();
  CHECK(INST.paint != NULL);
  CHECK(INST.key != NULL);
  CHECK(INST.tick != NULL);
  CHECK(C.log.n > 0);                    /* the greeting */
}

/* Typing a character changes the input line and nothing else, so that is
 * all that should be drawn. Before: bar, every log line, and the input. */
void test_build_typing_repaints_only_the_input_line(void) {
  int full, typed;
  boot();
  FILLS = TEXTS = 0;
  paint_now();                            /* unrequested: the whole window */
  full = FILLS + TEXTS;

  typed = press('h');
  CHECK(typed > 0);                       /* it did draw the character */
  CHECK(typed < full / 2);
  CHECK_EQ(C.in_len, 1);
}

/* And the same for backspace, which also only touches the input line. */
void test_build_backspace_repaints_only_the_input_line(void) {
  int full, n;
  boot();
  press('h');
  FILLS = TEXTS = 0;
  paint_now();
  full = FILLS + TEXTS;
  n = press(CAPP_KEY_BACK);
  CHECK(n > 0);
  CHECK(n < full / 2);
  CHECK_EQ(C.in_len, 0);
}

/* A paint the app did not ask for is the shell telling it something was
 * drawn over it; the only safe answer is everything. */
void test_build_an_unrequested_paint_draws_everything(void) {
  int first, again;
  boot();
  FILLS = TEXTS = 0;
  paint_now();
  first = FILLS + TEXTS;
  press('h');
  FILLS = TEXTS = 0;
  paint_now();
  again = FILLS + TEXTS;
  CHECK_EQ(again, first);
}

/* The prompt is always open, so the terminal is always taking text -- even
 * before the first character. Voice asks this before delivering a sentence,
 * and "no" here meant every spoken sentence was heard and then dropped. */
void test_build_wants_text_before_anything_is_typed(void) {
  boot();
  CHECK(INST.wants_text != NULL);
  CHECK_EQ(C.in_len, 0);
  CHECK(INST.wants_text(INST.state));
}

/* Scrolling with nothing typed moves the log, and the log is what repaints. */
void test_build_scrolling_repaints_the_log(void) {
  int i, n;
  boot();
  for (i = 0; i < 30; i++) note("a line of scrollback to move through");
  paint_now();
  n = press(CAPP_KEY_UP);
  CHECK(C.log.scroll > 0);
  CHECK(n > 5);                           /* many log lines, not one input line */
}
