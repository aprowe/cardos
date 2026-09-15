/* The stocks list file, on the host.
 *
 * /desktop/stocks.txt is one line per symbol: the ticker, an optional share
 * count, then a label. The share-count scan used to copy the same character
 * over and over without advancing, so any line with a count produced a run
 * of that digit, a nonsense count, and a label read from past the end of the
 * line. Only the built-in fallback list ever worked.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info stocks_capp_info
#define capp_main stocks_capp_main
#include "apps/stocks.c"
#undef capp_info
#undef capp_main

static int fake_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
static void *fake_memset(void *d, int c, size_t n) { return memset(d, c, n); }

static CardApi FAKE;

static void use_fake_api(void) {
  memset(&FAKE, 0, sizeof FAKE);
  FAKE.fmt = fake_fmt;
  FAKE.mem_set = fake_memset;
  api = &FAKE;
  memset(&S, 0, sizeof S);
}

void test_stocks_a_line_with_a_share_count(void) {
  char line[] = "SPCX 10800 SpaceX";
  use_fake_api();
  parse_line(line);
  CHECK_EQ(S.n, 1);
  CHECK(!strcmp(S.q[0].sym, "SPCX"));
  CHECK_EQ(S.q[0].shares, 10800);
  CHECK(!strcmp(S.q[0].label, "SpaceX"));
}

void test_stocks_a_line_without_a_share_count(void) {
  char line[] = "^GSPC S&P 500";
  use_fake_api();
  parse_line(line);
  CHECK_EQ(S.n, 1);
  CHECK(!strcmp(S.q[0].sym, "^GSPC"));
  CHECK_EQ(S.q[0].shares, 0);
  CHECK(!strcmp(S.q[0].label, "S&P 500"));
}

void test_stocks_comments_and_blank_lines_are_skipped(void) {
  char c[] = "# mine", b[] = "";
  use_fake_api();
  parse_line(c);
  parse_line(b);
  CHECK_EQ(S.n, 0);
}
