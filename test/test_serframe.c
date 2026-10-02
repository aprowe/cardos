/* Frames on the serial line: kernel/sys/serframe.c. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/serframe.h"

/* A request as tools/cardctl.py sends it. */
static int frame(char *out, const char *body) {
  return sprintf(out, "\x02%s\t%08x\x03", body, (unsigned)sf_crc32(body, strlen(body)));
}

static int feed_all(SfReader *r, const char *s, int n, char *keys) {
  int i, k = 0, got = SF_NONE, rc;
  for (i = 0; i < n; i++) {
    rc = sf_feed(r, (uint8_t)s[i]);
    if (rc == SF_KEY) keys[k++] = s[i];
    else if (rc != SF_NONE) got = rc;
  }
  keys[k] = 0;
  return got;
}

void test_serframe_crc32_is_the_standard_one(void) {
  CHECK_EQ(sf_crc32("123456789", 9), 0xCBF43926u);
  CHECK_EQ(sf_crc32("", 0), 0u);
}

void test_serframe_base64_round_trips_and_matches_python(void) {
  char enc[32];
  uint8_t dec[32], all[256];
  static char big[400];
  int i, n;
  CHECK_EQ(sf_b64_encode((const uint8_t *)"hi!", 3, enc, sizeof enc), 4);
  CHECK(!strcmp(enc, "aGkh"));
  sf_b64_encode((const uint8_t *)"hi", 2, enc, sizeof enc);
  CHECK(!strcmp(enc, "aGk="));
  sf_b64_encode((const uint8_t *)"h", 1, enc, sizeof enc);
  CHECK(!strcmp(enc, "aA=="));
  CHECK_EQ(sf_b64_decode("aGk=", 4, dec, sizeof dec), 2);
  CHECK(!memcmp(dec, "hi", 2));
  for (i = 0; i < 256; i++) all[i] = (uint8_t)i;
  n = sf_b64_encode(all, 256, big, sizeof big);
  CHECK_EQ(n, 344);
  {
    static uint8_t back[256];
    CHECK_EQ(sf_b64_decode(big, n, back, sizeof back), 256);
    CHECK(!memcmp(back, all, 256));
  }
  CHECK_EQ(sf_b64_decode("a*==", 4, dec, sizeof dec), -1);
  CHECK_EQ(sf_b64_encode(all, 3, enc, 4), -1);          /* no room for the NUL */
}

void test_serframe_bytes_outside_a_frame_are_keys(void) {
  char buf[128], keys[64], wire[128];
  SfReader r;
  int n;
  sf_reader_init(&r, buf, sizeof buf);
  n = sprintf(wire, "ab");
  n += frame(wire + n, "7\tls\t/config");
  n += sprintf(wire + n, "\rc");
  CHECK_EQ(feed_all(&r, wire, n, keys), SF_FRAME);
  CHECK(!strcmp(keys, "ab\rc"));
  CHECK(!strcmp(buf, "7\tls\t/config"));
}

void test_serframe_a_bad_crc_or_an_overlong_frame_is_dropped(void) {
  char buf[32], keys[64], wire[128];
  SfReader r;
  int n;
  sf_reader_init(&r, buf, sizeof buf);
  n = frame(wire, "1\tping");
  wire[3] = 'X';                                        /* "1\tXing" */
  CHECK_EQ(feed_all(&r, wire, n, keys), SF_BAD);
  n = frame(wire, "2\tput\t/a/very/long/path/that/does/not/fit");
  CHECK_EQ(feed_all(&r, wire, n, keys), SF_BAD);
  CHECK(!strcmp(keys, ""));
  n = frame(wire, "3\tping");                           /* and it recovers */
  CHECK_EQ(feed_all(&r, wire, n, keys), SF_FRAME);
  CHECK(!strcmp(buf, "3\tping"));
}

void test_serframe_a_new_stx_starts_over(void) {
  char buf[64], keys[64], wire[128];
  SfReader r;
  int n;
  sf_reader_init(&r, buf, sizeof buf);
  n = sprintf(wire, "\x02" "1\tput\t/half");            /* the sender gave up */
  n += frame(wire + n, "2\tstate");
  CHECK_EQ(feed_all(&r, wire, n, keys), SF_FRAME);
  CHECK(!strcmp(buf, "2\tstate"));
}

void test_serframe_split_keeps_the_rest_in_the_last_field(void) {
  char body[] = "4\tsh\techo a\tb";
  char *f[3];
  CHECK_EQ(sf_split(body, f, 3), 3);
  CHECK(!strcmp(f[0], "4"));
  CHECK(!strcmp(f[1], "sh"));
  CHECK(!strcmp(f[2], "echo a\tb"));
}

void test_serframe_a_reply_reads_back_through_the_reader(void) {
  char out[128], buf[128], keys[8];
  uint8_t data[8];
  char *f[3];
  SfReader r;
  int n = sf_reply(out, sizeof out, "9", "ok", (const uint8_t *)"hello", 5);
  CHECK(n > 0);
  CHECK_EQ(out[0], SF_STX);
  CHECK_EQ(out[n - 2], SF_ETX);
  CHECK_EQ(out[n - 1], '\n');
  sf_reader_init(&r, buf, sizeof buf);
  CHECK_EQ(feed_all(&r, out, n - 1, keys), SF_FRAME);
  CHECK_EQ(sf_split(buf, f, 3), 3);
  CHECK(!strcmp(f[0], "@9"));
  CHECK(!strcmp(f[1], "ok"));
  CHECK_EQ(sf_b64_decode(f[2], (int)strlen(f[2]), data, sizeof data), 5);
  CHECK(!memcmp(data, "hello", 5));
  CHECK_EQ(sf_reply(out, 20, "9", "ok", (const uint8_t *)"hello", 5), -1);
}
