/* apps/id3.h: an MP3's title and artist from its tag. */
#include <string.h>

#include "tinytest.h"
#include "apps/id3.h"

/* A v2.3 tag of frames, each "ID" then its body. */
static int tag(uint8_t *out, int ver, const char *const *frames, const uint8_t *const *bodies,
               const int *lens, int n) {
  int at = 10, i;
  memcpy(out, "ID3", 3);
  out[3] = (uint8_t)ver; out[4] = 0; out[5] = 0;
  for (i = 0; i < n; i++) {
    uint32_t len = (uint32_t)lens[i];
    memcpy(out + at, frames[i], 4);
    if (ver == 4) { out[at + 4] = 0; out[at + 5] = 0; out[at + 6] = (uint8_t)(len >> 7); out[at + 7] = len & 0x7F; }
    else { out[at + 4] = 0; out[at + 5] = 0; out[at + 6] = (uint8_t)(len >> 8); out[at + 7] = (uint8_t)len; }
    out[at + 8] = out[at + 9] = 0;
    memcpy(out + at + 10, bodies[i], len);
    at += 10 + (int)len;
  }
  {
    uint32_t size = (uint32_t)at - 10;
    out[6] = (uint8_t)(size >> 21 & 0x7F); out[7] = (uint8_t)(size >> 14 & 0x7F);
    out[8] = (uint8_t)(size >> 7 & 0x7F); out[9] = (uint8_t)(size & 0x7F);
  }
  return at;
}

void test_id3_finds_latin1_and_utf8_frames(void) {
  uint8_t b[256];
  char out[32];
  static const uint8_t title[] = "\0Holocene", artist[] = "\3Bon Iver";
  const char *const ids[] = { "TIT2", "TPE1" };
  const uint8_t *const bodies[] = { title, artist };
  const int lens[] = { 9, 9 };
  int n = tag(b, 3, ids, bodies, lens, 2);
  CHECK(id3_text(b, n, "TIT2", out, sizeof out));
  CHECK(!strcmp(out, "Holocene"));
  CHECK(id3_text(b, n, "TPE1", out, sizeof out));
  CHECK(!strcmp(out, "Bon Iver"));
  CHECK(!id3_text(b, n, "TALB", out, sizeof out));
  CHECK(!strcmp(out, ""));
}

void test_id3_reads_utf16_with_a_bom_and_v24_sizes(void) {
  uint8_t b[256];
  char out[32];
  static const uint8_t title[] = { 1, 0xFF, 0xFE, 'H', 0, 'i', 0, 0xE9, 0, 0, 0 };
  const char *const ids[] = { "TIT2" };
  const uint8_t *const bodies[] = { title };
  const int lens[] = { 11 };
  int n = tag(b, 4, ids, bodies, lens, 1);
  CHECK(id3_text(b, n, "TIT2", out, sizeof out));
  CHECK(!strcmp(out, "Hi?"));
}

void test_id3_cuts_to_the_buffer_and_refuses_what_is_not_a_tag(void) {
  uint8_t b[256];
  char out[5];
  static const uint8_t title[] = "\0A very long title";
  const char *const ids[] = { "TIT2" };
  const uint8_t *const bodies[] = { title };
  const int lens[] = { 18 };
  int n = tag(b, 3, ids, bodies, lens, 1);
  CHECK(id3_text(b, n, "TIT2", out, sizeof out));
  CHECK(!strcmp(out, "A ve"));
  CHECK(!id3_text(b, 20, "TIT2", out, sizeof out));          /* the frame runs past what was read */
  b[0] = 0xFF;
  CHECK(!id3_text(b, n, "TIT2", out, sizeof out));
}
