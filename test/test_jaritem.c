/* Jar Factory's item record on the host: apps/jaritem.h. The round trip, the
 * size limits, and how forgiving the reader is (and is not). */
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "apps/jaritem.h"

static void fill_item(JItem *it, int frames) {
  int i, k;
  memset(it, 0, sizeof *it);
  it->id = 4242;
  it->kind = JK_CRITTER;
  it->nframes = (uint8_t)frames;
  it->flags = JIF_GIFT;
  it->move = JM_HOPS;
  it->speed = JSP_FAST;
  it->zone = JZ_WATER;
  strcpy(it->name, "Pond Frog");
  strcpy(it->line, "Hums at dusk, mostly in tune.");
  for (i = 1; i < 8; i++) it->pal[i] = (uint16_t)(0x1111 * i);
  it->nhab = 2;
  it->hab[0].event = JE_NEAR; it->hab[0].earg = JN_MOSS; it->hab[0].action = JA_SAY; it->hab[0].aarg = 1;
  it->hab[1].event = JE_TIME; it->hab[1].earg = 3; it->hab[1].action = JA_GLOW; it->hab[1].aarg = 1;
  it->nbub = 2;
  strcpy(it->bub[0], "ribbit");
  strcpy(it->bub[1], "twelve chars");
  for (i = 0; i < JI_MEM; i++) it->mem[i] = (int16_t)(i * 1000 - 3000);
  strcpy(it->maker, "Maya");
  it->made = 1791504000u;
  strcpy(it->tags, "cosy,rainy,night");
  strcpy(it->gifted, "Alex");
  for (k = 0; k < frames; k++)
    for (i = 0; i < 256; i++) ji_set_px(it->frames[k], i, (i + k) % 8);
}

void test_jaritem_pixels_pack_three_bits(void) {
  uint8_t f[JI_FRAME_BYTES + 1];
  int i, ok = 1;
  memset(f, 0, sizeof f);
  for (i = 0; i < 256; i++) ji_set_px(f, i, (i * 5 + 3) & 7);
  for (i = 0; i < 256; i++) if (ji_px(f, i) != ((i * 5 + 3) & 7)) ok = 0;
  CHECK(ok);
  CHECK_EQ(f[JI_FRAME_BYTES], 0);            /* nothing written past the frame */
  /* one pixel changed leaves its neighbours alone */
  ji_set_px(f, 77, 0);
  CHECK_EQ(ji_px(f, 76), (76 * 5 + 3) & 7);
  CHECK_EQ(ji_px(f, 77), 0);
  CHECK_EQ(ji_px(f, 78), (78 * 5 + 3) & 7);
}

void test_jaritem_round_trip(void) {
  static JItem a, b;
  uint8_t buf[JI_MAX];
  int n, i;
  fill_item(&a, 3);
  a.sig_len = 5;
  memcpy(a.sig, "\x01\x02\x03\x04\x05", 5);
  a.script_len = 7;
  memcpy(a.script, "\x10\x20\x30\x40\x50\x60\x70", 7);
  n = jitem_encode(&a, buf, sizeof buf);
  CHECK_EQ(n, JI_HDR + 5 + 7 + 3 * JI_FRAME_BYTES);
  CHECK_EQ(buf[0], JI_VERSION);
  CHECK_EQ(buf[1], JI_HDR);
  CHECK_EQ(buf[2] | (buf[3] << 8), n);
  CHECK_EQ(jitem_decode(&b, buf, n), 0);
  CHECK_EQ(b.id, 4242);
  CHECK_EQ(b.kind, JK_CRITTER);
  CHECK_EQ(b.nframes, 3);
  CHECK_EQ(b.flags, JIF_GIFT);
  CHECK_EQ(b.move, JM_HOPS);
  CHECK_EQ(b.speed, JSP_FAST);
  CHECK_EQ(b.zone, JZ_WATER);
  CHECK(strcmp(b.name, "Pond Frog") == 0);
  CHECK(strcmp(b.line, "Hums at dusk, mostly in tune.") == 0);
  CHECK_EQ(b.pal[0], 0);
  CHECK_EQ(b.pal[7], 0x7777);
  CHECK_EQ(b.nhab, 2);
  CHECK_EQ(b.hab[1].event, JE_TIME);
  CHECK_EQ(b.hab[1].earg, 3);
  CHECK_EQ(b.hab[1].action, JA_GLOW);
  CHECK_EQ(b.nbub, 2);
  CHECK(strcmp(b.bub[1], "twelve chars") == 0);
  CHECK_EQ(b.mem[0], -3000);
  CHECK_EQ(b.mem[7], 4000);
  CHECK(strcmp(b.maker, "Maya") == 0);
  CHECK_EQ(b.made, 1791504000u);
  CHECK(strcmp(b.tags, "cosy,rainy,night") == 0);
  CHECK(strcmp(b.gifted, "Alex") == 0);
  CHECK_EQ(b.sig_len, 5);
  CHECK_EQ(b.sig[4], 5);
  CHECK_EQ(b.script_len, 7);
  CHECK_EQ(b.script[6], 0x70);
  for (i = 0; i < 3; i++) CHECK(memcmp(a.frames[i], b.frames[i], JI_FRAME_BYTES) == 0);
  /* and back out the same bytes */
  {
    uint8_t again[JI_MAX];
    CHECK_EQ(jitem_encode(&b, again, sizeof again), n);
    CHECK(memcmp(buf, again, (size_t)n) == 0);
  }
}

void test_jaritem_size_limits(void) {
  static JItem a;
  uint8_t buf[JI_MAX];
  fill_item(&a, 4);
  a.sig_len = JI_SIG_MAX;
  a.script_len = JI_SCRIPT_MAX;
  /* the biggest a v1 record can be is under 1 KB */
  CHECK_EQ(jitem_size(&a), JI_HDR + 64 + 256 + 4 * 96);
  CHECK(jitem_size(&a) <= JI_MAX);
  CHECK_EQ(jitem_encode(&a, buf, sizeof buf), jitem_size(&a));
  CHECK_EQ(jitem_encode(&a, buf, jitem_size(&a) - 1), -1);   /* does not fit */
  a.nframes = 5;  CHECK_EQ(jitem_encode(&a, buf, sizeof buf), -1);
  a.nframes = 0;  CHECK_EQ(jitem_encode(&a, buf, sizeof buf), -1);
  a.nframes = 1;
  a.nbub = 5;     CHECK_EQ(jitem_encode(&a, buf, sizeof buf), -1);
  a.nbub = 0;
  a.nhab = 4;     CHECK_EQ(jitem_encode(&a, buf, sizeof buf), -1);
  a.nhab = 0;
  a.kind = 3;     CHECK_EQ(jitem_encode(&a, buf, sizeof buf), -1);
  a.kind = 0;
  a.script_len = JI_SCRIPT_MAX + 1; CHECK_EQ(jitem_encode(&a, buf, sizeof buf), -1);
  /* a name too long is cut, not refused, and comes back terminated */
  a.script_len = 0;
  strcpy(a.name, "Twelve chars");
  CHECK(jitem_encode(&a, buf, sizeof buf) > 0);
  {
    static JItem b;
    CHECK_EQ(jitem_decode(&b, buf, sizeof buf), 0);
    CHECK(strcmp(b.name, "Twelve chars") == 0);
  }
}

void test_jaritem_refuses_what_it_cannot_trust(void) {
  static JItem a, b;
  uint8_t buf[JI_MAX];
  int n;
  fill_item(&a, 2);
  n = jitem_encode(&a, buf, sizeof buf);
  CHECK_EQ(jitem_decode(&b, buf, 100), -1);          /* short */
  CHECK_EQ(jitem_decode(&b, buf, n - 1), -1);        /* the length says more */
  buf[9] = 0;                                        /* no frames */
  CHECK_EQ(jitem_decode(&b, buf, n), -3);
  buf[9] = 5;                                        /* too many */
  CHECK_EQ(jitem_decode(&b, buf, n), -3);
  buf[9] = 3;                                        /* more than the length holds */
  CHECK_EQ(jitem_decode(&b, buf, n), -2);
  buf[9] = 2;
  buf[8] = 7;                                        /* a kind that is not one */
  CHECK_EQ(jitem_decode(&b, buf, n), -3);
  buf[8] = JK_FLOOR;
  CHECK_EQ(jitem_decode(&b, buf, n), 0);
  /* a control code in a name comes out harmless */
  buf[12] = 7;
  CHECK_EQ(jitem_decode(&b, buf, n), 0);
  CHECK_EQ(b.name[0], '?');
}

void test_jaritem_newer_versions_fall_back_to_the_recipe(void) {
  static JItem a, b;
  uint8_t buf[JI_MAX], newer[JI_MAX];
  int n, k;
  fill_item(&a, 1);
  a.script_len = 4;
  memcpy(a.script, "\x01\x02\x03\x04", 4);
  a.nhab = 3;
  a.hab[2].event = 99;                     /* an event this device has never heard of */
  a.hab[2].action = JA_SAY;
  a.move = JM_FLOATS;
  n = jitem_encode(&a, buf, sizeof buf);
  CHECK(n > 0);
  /* A version 2 record with a header eight bytes longer: v1's fields where
   * they were, something new after them, then the rest. */
  memcpy(newer, buf, JI_HDR);
  newer[0] = 2;
  newer[1] = JI_HDR + 8;
  for (k = 0; k < 8; k++) newer[JI_HDR + k] = 0xEE;
  memcpy(newer + JI_HDR + 8, buf + JI_HDR, (size_t)(n - JI_HDR));
  newer[2] = (uint8_t)(n + 8);
  newer[3] = (uint8_t)((n + 8) >> 8);
  newer[72] = 9;                           /* a movement added later */
  CHECK_EQ(jitem_decode(&b, newer, n + 8), 0);
  CHECK_EQ(b.version, 2);
  CHECK(b.flags & JIF_NEWER);
  CHECK_EQ(b.script_len, 0);               /* the script is ignored */
  CHECK_EQ(b.move, JM_SITS);               /* an unknown movement sits */
  CHECK_EQ(b.nhab, 2);                     /* the unknown habit is dropped */
  CHECK(strcmp(b.name, "Pond Frog") == 0);
  CHECK(memcmp(a.frames[0], b.frames[0], JI_FRAME_BYTES) == 0);
}

void test_jaritem_number(void) {
  char s[16];
  jitem_number(42, s);
  CHECK(strcmp(s, "No. 0042") == 0);
  jitem_number(123456, s);
  CHECK(strcmp(s, "No. 123456") == 0);
}

/* The signed message is the record as if it had no signature and empty
 * memory: the same as one encoded afresh with neither. Memory a script
 * wrote, or a different signature, does not change it. */
void test_jaritem_signed_message_is_the_record_without_its_signature(void) {
  static JItem a;
  uint8_t with[JI_MAX], without[JI_MAX], msg[JI_MAX];
  int n1, n0, m, i;
  fill_item(&a, 2);
  a.script_len = 3;
  memcpy(a.script, "\x01\x02\x03", 3);
  memset(a.mem, 0, sizeof a.mem);
  n0 = jitem_encode(&a, without, sizeof without);
  for (i = 0; i < JI_MEM; i++) a.mem[i] = (int16_t)(i * 77 - 100);   /* a script was here */
  a.sig_len = 64;
  for (i = 0; i < 64; i++) a.sig[i] = (uint8_t)(0xA0 + i);
  n1 = jitem_encode(&a, with, sizeof with);
  CHECK_EQ(n1, n0 + 64);
  CHECK(memcmp(with + 136, without + 136, 16) != 0);
  m = ji_signed_message(with, n1, msg, sizeof msg);
  CHECK_EQ(m, n0);
  CHECK(memcmp(msg, without, (size_t)n0) == 0);
  CHECK_EQ(msg[204], 0);
  CHECK_EQ((int)ji_get16(msg + 2), n0);
  /* unsigned and memory-less: the message is the record */
  CHECK_EQ(ji_signed_message(without, n0, msg, sizeof msg), n0);
  CHECK(memcmp(msg, without, (size_t)n0) == 0);
  /* lengths that disagree, or no room */
  CHECK_EQ(ji_signed_message(with, n1 - 1, msg, sizeof msg), -1);
  CHECK_EQ(ji_signed_message(with, n1, msg, n0 - 1), -1);
}

/* The server's own example, when server/jar.py's tests have written it:
 * test/fixtures/jar_item_signed.bin and the message it signed. Skipped
 * (with a line saying so) when the fixtures are not in this tree. */
void test_jaritem_signed_message_matches_the_servers_fixture(void) {
  static uint8_t rec[JI_MAX + 64], want[JI_MAX], got[JI_MAX];
  FILE *f = fopen("test/fixtures/jar_item_signed.bin", "rb");
  FILE *g = fopen("test/fixtures/jar_item_message.bin", "rb");
  int n, w, m;
  if (!f || !g) {
    if (f) fclose(f);
    if (g) fclose(g);
    printf("  (skipped: test/fixtures/jar_item_*.bin not here)\n");
    return;
  }
  n = (int)fread(rec, 1, sizeof rec, f);
  w = (int)fread(want, 1, sizeof want, g);
  fclose(f);
  fclose(g);
  m = ji_signed_message(rec, n, got, sizeof got);
  CHECK_EQ(m, w);
  CHECK(m == w && memcmp(got, want, (size_t)w) == 0);
  {
    static JItem it;
    CHECK_EQ(jitem_decode(&it, rec, n), 0);
    CHECK_EQ(it.sig_len, 64);
  }
}
