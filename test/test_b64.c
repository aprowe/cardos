/* apps/b64.h: base64 both ways, against RFC 4648's own examples. */
#include <string.h>

#include "tinytest.h"
#include "apps/b64.h"

void test_b64_rfc4648_vectors(void) {
  static const char *const PLAIN[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
  static const char *const ENC[] = { "", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy" };
  char out[32];
  uint8_t back[32];
  int i, n;
  for (i = 0; i < 7; i++) {
    n = b64_encode((const uint8_t *)PLAIN[i], (int)strlen(PLAIN[i]), out, sizeof out);
    CHECK_EQ(n, (int)strlen(ENC[i]));
    CHECK(strcmp(out, ENC[i]) == 0);
    n = b64_decode(ENC[i], back, sizeof back);
    CHECK_EQ(n, (int)strlen(PLAIN[i]));
    CHECK(memcmp(back, PLAIN[i], (size_t)n) == 0);
  }
}

void test_b64_round_trips_every_byte(void) {
  static uint8_t in[300], back[300];
  static char enc[420];
  int i, n;
  for (i = 0; i < 300; i++) in[i] = (uint8_t)(i * 7 + 3);
  n = b64_encode(in, 300, enc, sizeof enc);
  CHECK_EQ(n, B64_LEN(300));
  CHECK_EQ(b64_decode(enc, back, sizeof back), 300);
  CHECK(memcmp(in, back, 300) == 0);
}

void test_b64_stops_at_the_end_of_the_line_and_refuses_junk(void) {
  uint8_t out[8];
  CHECK_EQ(b64_decode("Zm9v\r\nZm9v", out, sizeof out), 3);   /* one line */
  CHECK_EQ(b64_decode("Zm9v\tnext", out, sizeof out), 3);
  CHECK_EQ(b64_decode("Zm*v", out, sizeof out), -1);
  CHECK_EQ(b64_decode("Z", out, sizeof out), -1);
  CHECK_EQ(b64_decode("Zm9vYmFy", out, 5), -1);            /* does not fit */
  {
    char small[4];
    CHECK_EQ(b64_encode((const uint8_t *)"foo", 3, small, sizeof small), -1);
  }
}
