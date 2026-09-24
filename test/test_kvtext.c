/* /config/settings.txt's lines: what a hand edit may do to them, and that
 * writing them back is what reading expects. */
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/kvtext.h"

void test_kv_reads_pairs_and_forgives_a_hand_edit(void) {
  const char *text = "# settings\r\nbright=75\r\n\n  volume = 40  \nbad line\npins=Todo,Edit\n=x\n";
  const char *p = text;
  char k[16], v[32];
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 1);
  CHECK(!strcmp(k, "bright")); CHECK(!strcmp(v, "75"));
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 1);
  CHECK(!strcmp(k, "volume")); CHECK(!strcmp(v, "40"));
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 1);          /* no '=': skipped */
  CHECK(!strcmp(k, "pins")); CHECK(!strcmp(v, "Todo,Edit"));
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 0);          /* "=x": no key */
}

void test_kv_truncates_rather_than_overruns(void) {
  const char *p = "averyveryverylongkey=averyveryverylongvalue\n";
  char k[6], v[6];
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 1);
  CHECK(!strcmp(k, "avery"));
  CHECK(!strcmp(v, "avery"));
}

void test_kv_put_writes_what_next_reads(void) {
  char out[32], k[8], v[8];
  size_t len = 0;
  const char *p;
  CHECK_EQ(kv_put(out, sizeof out, &len, "a", "1"), 0);
  CHECK_EQ(kv_put(out, sizeof out, &len, "shell", "2"), 0);
  CHECK(!strcmp(out, "a=1\nshell=2\n"));
  CHECK_EQ(kv_put(out, sizeof out, &len, "toolong", "0123456789abcdef"), -1);
  CHECK(!strcmp(out, "a=1\nshell=2\n"));                       /* untouched */
  p = out;
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 1);
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 1);
  CHECK(!strcmp(k, "shell")); CHECK(!strcmp(v, "2"));
}

/* A value may hold a newline -- the launcher's pins are names one a line --
 * and the file is one pair a line, so newlines and backslashes are escaped
 * on the way out and back on the way in. Unescaped, "pins=Settings\ngrep"
 * restored as one pin and lost the rest. */
void test_kv_escapes_newlines_in_values(void) {
  char out[64], k[8], v[32];
  size_t len = 0;
  const char *p;
  CHECK_EQ(kv_put(out, sizeof out, &len, "pins", "Settings\ngrep\\x"), 0);
  CHECK(!strcmp(out, "pins=Settings\\ngrep\\\\x\n"));
  p = out;
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 1);
  CHECK(!strcmp(v, "Settings\ngrep\\x"));
  CHECK_EQ(kv_next(&p, k, sizeof k, v, sizeof v), 0);
}
