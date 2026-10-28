/* Lamp's remotes: apps/lamp/lampmodel.h. */
#include <string.h>

#include "tinytest.h"
#include "apps/lamp/lampmodel.h"

static LampModel M[6];

void test_lamp_builtins_read_whole(void) {
  int n = 0, bad = -1;
  lm_parse(LM_BUILTIN, (int)sizeof LM_BUILTIN - 1, M, &n, 6, &bad);
  CHECK_EQ(bad, 0);
  CHECK_EQ(n, 2);
  CHECK(!strcmp(M[0].name, "24-key RGB remote"));
  CHECK_EQ(M[0].proto, LM_NEC);
  CHECK_EQ(M[0].addr, 0xEF00);
  CHECK_EQ(M[0].n, 24);
  CHECK_EQ(M[0].cols, 4);
  CHECK_EQ(M[0].key[3].cmd, 0x03);
  CHECK_EQ(M[0].key[3].role, LM_ROLE_ON);
  CHECK(!strcmp(M[0].key[3].label, "On"));
  CHECK_EQ(M[0].key[8].swatch, 1);
  CHECK_EQ(M[0].key[8].label[0], 0);            /* "." is no label */
  CHECK_EQ(M[0].key[8].r, 0xFF);
  CHECK_EQ(M[0].key[8].g, 0x50);
  CHECK(!strcmp(M[1].name, "44-key RGB remote"));
  CHECK_EQ(M[1].addr, 0x00);
  CHECK_EQ(M[1].n, 44);
  CHECK_EQ(M[1].key[3].cmd, 0x40);
  CHECK_EQ(lm_find_role(&M[1], LM_ROLE_POWER), 3);
  CHECK_EQ(lm_find_role(&M[1], LM_ROLE_ON), -1);
  CHECK_EQ(lm_find_role(&M[1], LM_ROLE_UP), 0);
  CHECK(!strcmp(M[1].key[43].label, "Fade7"));
}

void test_lamp_a_file_adds_models(void) {
  const char *t =
    "# my lamp\r\n"
    "model Corner lamp  \r\n"
    "necmsb\n"
    "cols 3\n"
    "key 0xF7C03F On on\n"
    "key E0E040BF Off off\n"
    "key FF00 Warm_wh #FFE0B0\n";
  int n = 1, bad = -1;
  memset(&M[0], 0, sizeof M[0]);
  lm_parse(t, (int)strlen(t), M, &n, 6, &bad);
  CHECK_EQ(bad, 0);
  CHECK_EQ(n, 2);
  CHECK(!strcmp(M[1].name, "Corner lamp"));
  CHECK_EQ(M[1].proto, LM_NEC_MSB);
  CHECK_EQ(M[1].cols, 3);
  CHECK_EQ(M[1].n, 3);
  CHECK(M[1].key[1].cmd == 0xE0E040BFu);
  CHECK(!strcmp(M[1].key[2].label, "Warm wh"));
  CHECK_EQ(M[1].key[2].b, 0xB0);
}

void test_lamp_bad_lines_are_skipped_and_named(void) {
  const char *t =
    "key 01 Before\n"                 /* 1: no model yet */
    "model A\n"
    "nec 0x00\n"
    "key 100 TooBig\n"                /* 4: NEC commands are a byte */
    "key 05 Fine\n"
    "key 06 Waytoolong\n"             /* 6 */
    "key 07 Col #12345\n"             /* 7 */
    "key 08 Bad sideways\n"           /* 8 */
    "nec 0x01\n"                      /* 9: after keys */
    "model Empty\n"                   /* dropped: no keys */
    "model B\n"
    "key zz X\n";                     /* 12 */
  int n = 0, bad = 0;
  lm_parse(t, (int)strlen(t), M, &n, 6, &bad);
  CHECK_EQ(bad, 1);
  CHECK_EQ(n, 1);
  CHECK(!strcmp(M[0].name, "A"));
  CHECK_EQ(M[0].n, 1);
  CHECK_EQ(M[0].key[0].cmd, 5);
}

void test_lamp_a_mapping_becomes_a_model(void) {
  static uint8_t pick[256];
  char out[2048];
  int n, count = 0, bad = -1;
  memset(pick, 0, sizeof pick);
  pick[0x10] = 8;                   /* Red */
  pick[0x02] = 2;                   /* Off */
  pick[0x03] = 1;                   /* On */
  pick[0x20] = LM_OPT_MODE + 1;
  pick[0x21] = LM_OPT_MODE + 1;
  pick[0x05] = LM_OPT_NONE + 1;     /* left out */
  pick[0x30] = 4;                   /* Brighter */
  n = lm_map_text("My lamp", 0xEF00, pick, out, sizeof out);
  CHECK(n > 0);
  CHECK(!strcmp(out,
    "model My lamp\nnec 0xEF00\ncols 4\n"
    "key 03 On on\nkey 02 Off off\nkey 30 Brt+ up\n"
    "key 20 Mode1\nkey 21 Mode2\nkey 10 . #FF0000\n"));
  lm_parse(out, n, M, &count, 6, &bad);
  CHECK_EQ(bad, 0);
  CHECK_EQ(count, 1);
  CHECK_EQ(M[0].n, 6);
  CHECK_EQ(M[0].key[5].swatch, 1);
  CHECK_EQ(lm_find_role(&M[0], LM_ROLE_UP), 2);
  CHECK_EQ(lm_map_text("My lamp", 0, pick, out, 40), -1);
  n = lm_map_text("X", 0x00, pick, out, sizeof out);
  CHECK(!strncmp(out, "model X\nnec 0x00\n", 17));
}

void test_lamp_replace_swaps_one_model(void) {
  const char *old = "# mine\nmodel Other\nkey 1 a\nmodel My lamp\nkey 2 b\nmodel Last\nkey 3 c";
  char out[512];
  int n = lm_replace(old, (int)strlen(old), "My lamp", "model My lamp\nkey 9 z\n", out, sizeof out);
  CHECK(n > 0);
  CHECK(!strcmp(out, "# mine\nmodel Other\nkey 1 a\nmodel Last\nkey 3 c\nmodel My lamp\nkey 9 z\n"));
  n = lm_replace("", 0, "My lamp", "model My lamp\n", out, sizeof out);
  CHECK(!strcmp(out, "model My lamp\n"));
  CHECK_EQ(lm_replace(old, (int)strlen(old), "My lamp", "model My lamp\n", out, 10), -1);
}

void test_lamp_stops_when_full(void) {
  const char *t = "model A\nkey 1 a\nmodel B\nkey 2 b\nmodel C\nkey 3 c\n";
  int n = 0, bad = 0;
  lm_parse(t, (int)strlen(t), M, &n, 2, &bad);
  CHECK_EQ(n, 2);
  CHECK_EQ(bad, 0);
  CHECK(!strcmp(M[1].name, "B"));
}
