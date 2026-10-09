/* apps/str.h: the helpers every app used to carry a copy of. */

#include <string.h>

#include "tinytest.h"
#include "apps/str.h"

void test_str_url_enc_keeps_a_query_inside_its_value(void) {
  char out[64];
  url_enc(out, (int)sizeof out, "https://x.org/s?q=a b&page=2");
  CHECK(!strcmp(out, "https%3A%2F%2Fx.org%2Fs%3Fq%3Da%20b%26page%3D2"));
  url_enc(out, (int)sizeof out, "A-z_0.9~");
  CHECK(!strcmp(out, "A-z_0.9~"));             /* the unreserved set stays */
}

void test_str_url_enc_cuts_at_a_whole_character(void) {
  char out[5];
  CHECK_EQ(url_enc(out, (int)sizeof out, "ab&cd"), 2);   /* "ab%26" would not fit */
  CHECK(!strcmp(out, "ab"));
  CHECK_EQ(url_enc(out, (int)sizeof out, "abcdefg"), 4);
  CHECK(!strcmp(out, "abcd"));
  CHECK_EQ(url_enc(out, (int)sizeof out, "a&"), 4);
  CHECK(!strcmp(out, "a%26"));
}

void test_str_tsv_field_and_next_line(void) {
  const char *text = "t1\tFirst\t1000\nt2\tSecond\n";
  char f[8];
  tsv_field(text, 0, f, sizeof f); CHECK(!strcmp(f, "t1"));
  tsv_field(text, 1, f, sizeof f); CHECK(!strcmp(f, "First"));
  tsv_field(text, 3, f, sizeof f); CHECK(!strcmp(f, ""));    /* not on this line */
  tsv_field(tsv_next_line(text), 1, f, 4); CHECK(!strcmp(f, "Sec"));
  CHECK(*tsv_next_line(tsv_next_line(text)) == 0);
}

void test_str_contains_with_and_without_case(void) {
  CHECK(str_contains("Buy Milk", "milk", 1));
  CHECK(!str_contains("Buy Milk", "milk", 0));
  CHECK(str_contains("abc", "", 0));
  CHECK(!str_contains("ab", "abc", 1));
  CHECK(str_contains("aab", "ab", 0));
}

void test_str_small_ones(void) {
  const char *p = "4096 rest";
  CHECK(str_same("a", "a") && !str_same("a", "ab") && !str_same("ab", "a"));
  CHECK(str_starts("count=4", "count=") && !str_starts("cou", "count"));
  CHECK_EQ(str_lower('Q'), 'q');
  CHECK_EQ(str_uint(&p), 4096);
  CHECK(!strcmp(p, " rest"));
  CHECK_EQ(capp_rect(1, 2, -5, 3).w, 0);
  CHECK_EQ(capp_rect(1, 2, 5, -3).h, 0);
  CHECK(capp_overlaps(capp_rect(0, 0, 10, 10), capp_rect(9, 9, 5, 5)));
  CHECK(!capp_overlaps(capp_rect(0, 0, 10, 10), capp_rect(10, 0, 5, 5)));
}
