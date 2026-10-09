/* apps/termlog.h: the line wrap Build, Claude and Chat share. */

#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define TL_LINES 6
#include "apps/termlog.h"

static CardApi TA;
static TermLog TLOG;

static void fresh(void) {
  fakeapi_init(&TA);
  memset(&TLOG, 0, sizeof TLOG);
}

void test_termlog_wraps_at_the_last_space_and_drops_it(void) {
  fresh();
  /* 40 columns: "...brown" ends at 39, so " fox" goes over. */
  tl_push_text(&TA, &TLOG, "the quick brown fox jumps over the lazy brown fox again", 1, 0);
  CHECK_EQ(TLOG.n, 2);
  CHECK(!strcmp(TLOG.line[0], "the quick brown fox jumps over the lazy"));
  CHECK(!strcmp(TLOG.line[1], "brown fox again"));
  CHECK_EQ(TLOG.tag[1], 1);
}

void test_termlog_breaks_a_long_word_where_it_must(void) {
  fresh();
  tl_push_text(&TA, &TLOG,
               "see https://example.org/a/very/long/path/that/does/not/fit", 0, 0);
  /* The only space is within the first third: not worth breaking at. */
  CHECK_EQ(TLOG.n, 2);
  CHECK_EQ((int)strlen(TLOG.line[0]), TL_COLS);
  CHECK(!strcmp(TLOG.line[1], "/that/does/not/fit"));
}

void test_termlog_newlines_and_empty_lines_are_lines(void) {
  const char *rest;
  fresh();
  tl_push_text(&TA, &TLOG, "one\n\nthree\r\n", 0, 0);
  CHECK_EQ(TLOG.n, 4);                     /* one, "", three, "" */
  CHECK(!strcmp(TLOG.line[1], ""));
  CHECK(!strcmp(TLOG.line[2], "three"));
  fresh();
  rest = tl_wrap_line(&TA, &TLOG, "a\tb\nc", 0, 0);
  CHECK(!strcmp(TLOG.line[0], "a b"));     /* a tab is a space */
  CHECK(rest && !strcmp(rest, "c"));
  CHECK(tl_wrap_line(&TA, &TLOG, rest, 0, 0) == NULL);
}

void test_termlog_continuations_indent_when_asked(void) {
  fresh();
  tl_push_text(&TA, &TLOG, "sam: the quick brown fox jumps over the lazy dog twice", 7, 2);
  CHECK_EQ(TLOG.n, 2);
  CHECK(!strcmp(TLOG.line[0], "sam: the quick brown fox jumps over the"));
  CHECK(!strcmp(TLOG.line[1], "  lazy dog twice"));
}

void test_termlog_the_oldest_line_falls_out(void) {
  int i;
  char s[8];
  fresh();
  for (i = 0; i < TL_LINES + 2; i++) {
    snprintf(s, sizeof s, "l%d", i);
    tl_push(&TA, &TLOG, s, i);
  }
  CHECK_EQ(TLOG.n, TL_LINES);
  CHECK(!strcmp(TLOG.line[0], "l2"));
  CHECK_EQ(TLOG.tag[0], 2);
  CHECK(!strcmp(TLOG.line[TL_LINES - 1], "l7"));
}
