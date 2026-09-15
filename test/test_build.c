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
static void *fake_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *fake_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t fake_strlen(const char *s) { return strlen(s); }
static uint32_t fake_ticks(void) { return NOW; }
static int fake_open(const char *p, int f) { (void)p; (void)f; return -1; }
static void fake_ui(const CappUi *ui) { INST = *ui; }

static int fake_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

static CardApi API;
static CRect   WIN;

static void boot(void) {
  char arg0[] = "claude";
  char *argv[1];
  argv[0] = arg0;

  memset(&API, 0, sizeof API);
  API.version = CAPP_API_VERSION;
  API.fill = fake_fill;
  API.text = fake_text;
  API.mem_set = fake_memset;
  API.mem_cpy = fake_memcpy;
  API.str_len = fake_strlen;
  API.fmt = fake_fmt;
  API.ticks_ms = fake_ticks;
  API.open = fake_open;
  API.ui = fake_ui;

  NOW = 1000;
  FILLS = TEXTS = 0;
  memset(&INST, 0, sizeof INST);
  WIN.x = 0; WIN.y = 0; WIN.w = 240; WIN.h = 135;
  capp_main(&API, 1, argv);
  INST.paint(INST.state, WIN);            /* the shell's first paint */
}

/* One keypress, painted the way the shell does it: only if key() asks. */
static int press(unsigned char k) {
  FILLS = TEXTS = 0;
  if (INST.key(INST.state, k)) INST.paint(INST.state, WIN);
  return FILLS + TEXTS;
}

void test_build_installs_a_ui(void) {
  boot();
  CHECK(INST.paint != NULL);
  CHECK(INST.key != NULL);
  CHECK(INST.tick != NULL);
  CHECK(C.nlines > 0);                    /* the greeting */
}

/* Typing a character changes the input line and nothing else, so that is
 * all that should be drawn. Before: bar, every log line, and the input. */
void test_build_typing_repaints_only_the_input_line(void) {
  int full, typed;
  boot();
  FILLS = TEXTS = 0;
  INST.paint(INST.state, WIN);            /* unrequested: the whole window */
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
  INST.paint(INST.state, WIN);
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
  INST.paint(INST.state, WIN);
  first = FILLS + TEXTS;
  press('h');
  FILLS = TEXTS = 0;
  INST.paint(INST.state, WIN);
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
  INST.paint(INST.state, WIN);
  n = press(CAPP_KEY_UP);
  CHECK(C.scroll > 0);
  CHECK(n > 5);                           /* many log lines, not one input line */
}
