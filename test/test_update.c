/* The update manifest: parsing what the proxy sends, and deciding what is
 * stale. The device side is fetch-and-hash glue around these two. */

#include <string.h>

#include "tinytest.h"
#include "kernel/net/manifest.h"

static const char SAMPLE[] =
  "firmware 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef 1590576\n"
  "app pinball 8c1e2b47 19672\n"
  "app claude  DEADBEEF 24312\n";

void test_manifest_parses_firmware_and_apps(void) {
  Manifest m;
  int n = manifest_parse(SAMPLE, &m);
  CHECK_EQ(n, 3);
  CHECK(m.has_firmware);
  CHECK_EQ((int)m.firmware_size, 1590576);
  CHECK_EQ(m.firmware_sha[0], 0x01);
  CHECK_EQ(m.firmware_sha[1], 0x23);
  CHECK_EQ(m.firmware_sha[31], 0xef);
  CHECK_EQ(m.napps, 2);
  CHECK(strcmp(m.app[0].name, "pinball") == 0);
  CHECK_EQ(m.app[0].hash, 0x8c1e2b47u);
  CHECK_EQ((int)m.app[0].size, 19672);
  CHECK(strcmp(m.app[1].name, "claude") == 0);
  CHECK_EQ(m.app[1].hash, 0xDEADBEEFu);      /* either case */
}

/* The folder rides at the end of an app line, so firmware from before it
 * reads the same line and ignores the extra word. Without one, the app
 * belongs at the top level -- and an app new to the card used to land there
 * whatever its folder, because only the seed blobs knew it. */
void test_manifest_reads_an_apps_folder(void) {
  Manifest m;
  CHECK_EQ(manifest_parse("app timer 0000beef 12924 Tools\n"
                          "app grep 00000001 2000\n", &m), 2);
  CHECK(strcmp(m.app[0].folder, "Tools") == 0);
  CHECK(m.app[1].folder[0] == 0);
}

void test_manifest_a_folder_that_is_not_a_name_is_ignored(void) {
  Manifest m;
  CHECK_EQ(manifest_parse("app timer 0000beef 12924 ../sys\n", &m), 1);
  CHECK_EQ(m.napps, 1);                      /* the app is still an app */
  CHECK(m.app[0].folder[0] == 0);            /* but lands at the top level */
}

void test_manifest_skips_what_it_does_not_understand(void) {
  Manifest m;
  int n = manifest_parse("# a comment\nfont 6x8 abcd 100\napp cat 00000001 5\n\n", &m);
  CHECK_EQ(n, 1);
  CHECK(!m.has_firmware);
  CHECK_EQ(m.napps, 1);
}

void test_manifest_rejects_a_page_that_is_not_one(void) {
  Manifest m;
  CHECK_EQ(manifest_parse("<html><body>404</body></html>", &m), -1);
  CHECK_EQ(manifest_parse("", &m), -1);
}

void test_manifest_refuses_a_malformed_line_but_keeps_the_rest(void) {
  Manifest m;
  int n = manifest_parse("app pinball zz 5\napp ok 1 2\nfirmware short 9\n", &m);
  CHECK_EQ(n, 1);                            /* only "app ok" made sense */
  CHECK(!m.has_firmware);
  CHECK_EQ(m.napps, 1);
  CHECK(strcmp(m.app[0].name, "ok") == 0);
}

/* The name becomes a path under /desktop. A proxy is trusted, but a name
 * with a slash or a dot in it is not a name, and `../x` would have landed
 * outside the folder. */
void test_manifest_name_that_is_not_a_name_is_dropped(void) {
  Manifest m;
  int n = manifest_parse("app ../x 1 2\napp a/b 1 2\napp a.b 1 2\napp ok-1_2 1 2\n", &m);
  CHECK_EQ(n, 1);
  CHECK_EQ(m.napps, 1);
  CHECK(strcmp(m.app[0].name, "ok-1_2") == 0);
}

void test_manifest_name_too_long_is_dropped(void) {
  Manifest m;
  int n = manifest_parse("app abcdefghijklmnopqrstuvwxyz 1 2\napp b 1 2\n", &m);
  CHECK_EQ(n, 1);
  CHECK(strcmp(m.app[0].name, "b") == 0);
}

void test_manifest_fnv_matches_the_reference(void) {
  /* FNV-1a 32 test vectors from the reference: "" and "a". */
  CHECK_EQ(manifest_fnv1a(MANIFEST_FNV_INIT, (const uint8_t *)"", 0), 0x811c9dc5u);
  CHECK_EQ(manifest_fnv1a(MANIFEST_FNV_INIT, (const uint8_t *)"a", 1), 0xe40c292cu);
  /* And in pieces gives the same as in one go. */
  {
    uint32_t whole = manifest_fnv1a(MANIFEST_FNV_INIT, (const uint8_t *)"foobar", 6);
    uint32_t h = manifest_fnv1a(MANIFEST_FNV_INIT, (const uint8_t *)"foo", 3);
    h = manifest_fnv1a(h, (const uint8_t *)"bar", 3);
    CHECK_EQ(h, whole);
    CHECK_EQ(whole, 0xbf9cf968u);
  }
}

void test_manifest_diff_finds_what_differs(void) {
  Manifest m;
  ManifestLocal l;
  int stale[MANIFEST_MAX_APPS];
  uint8_t same[32], other[32];
  int fw;

  manifest_parse(SAMPLE, &m);
  memcpy(same, m.firmware_sha, 32);
  memcpy(other, m.firmware_sha, 32);
  other[5] ^= 1;

  memset(&l, 0, sizeof l);
  l.own_sha = same;
  l.have_app[0] = 1; l.app_hash[0] = 0x8c1e2b47u;   /* pinball current */
  l.have_app[1] = 1; l.app_hash[1] = 0x00000000u;   /* claude differs */
  fw = manifest_diff(&m, &l, stale);
  CHECK_EQ(fw, 0);
  CHECK_EQ(stale[0], 0);
  CHECK_EQ(stale[1], 1);

  l.own_sha = other;
  l.have_app[1] = 0;                                /* claude missing */
  fw = manifest_diff(&m, &l, stale);
  CHECK_EQ(fw, 1);
  CHECK_EQ(stale[1], 1);

  l.own_sha = NULL;                                 /* no idea what we run */
  fw = manifest_diff(&m, &l, stale);
  CHECK_EQ(fw, 1);
}

void test_manifest_diff_without_firmware_never_says_stale(void) {
  Manifest m;
  ManifestLocal l;
  int stale[MANIFEST_MAX_APPS];
  manifest_parse("app cat 1 2\n", &m);
  memset(&l, 0, sizeof l);
  CHECK_EQ(manifest_diff(&m, &l, stale), 0);
  CHECK_EQ(stale[0], 1);
}
