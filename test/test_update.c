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

/* More apps than there were when the table was sized: 28 on 2026-10-01,
 * and with room for 24 the last four -- today, todo, toggl, web -- were
 * dropped without a word, so `update` called them current and never
 * installed them. Every app is kept now, and anything past the table is
 * counted so the device can say so. */
void test_manifest_keeps_every_app_and_counts_what_does_not_fit(void) {
  static char text[MANIFEST_MAX_APPS * 40 + 200];
  Manifest m;
  int i, o = 0;
  for (i = 0; i < 30; i++)
    o += snprintf(text + o, sizeof text - o, "app app%02d 0000%04x %d Tools\n", i, i, 1000 + i);
  manifest_parse(text, &m);
  CHECK_EQ(m.napps, 30);
  CHECK_EQ(m.dropped, 0);
  CHECK(!strcmp(m.app[29].name, "app29"));
  o = 0;
  for (i = 0; i < MANIFEST_MAX_APPS + 3; i++)
    o += snprintf(text + o, sizeof text - o, "app a%03d 0000%04x %d\n", i, i, 10 + i);
  manifest_parse(text, &m);
  CHECK_EQ(m.napps, MANIFEST_MAX_APPS);
  CHECK_EQ(m.dropped, 3);
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

/* The versions are labels for people (update version, the Update app):
 * kept, never compared, and not enough on their own to be a manifest. */
void test_manifest_keeps_the_versions(void) {
  Manifest m;
  char sha[65];
  int i;
  for (i = 0; i < 64; i++) sha[i] = 'a';
  sha[64] = 0;
  {
    char text[256];
    snprintf(text, sizeof text, "firmware %s 100\nversion firmware v0.15.1-2-gabc1234\n"
             "app cat 00000001 5\nversion server v0.15.2\n", sha);
    CHECK_EQ(manifest_parse(text, &m), 2);
  }
  CHECK(strcmp(m.fw_version, "v0.15.1-2-gabc1234") == 0);
  CHECK(strcmp(m.server_version, "v0.15.2") == 0);
  CHECK_EQ(manifest_parse("version server v1\n", &m), -1);
  CHECK_EQ(manifest_parse("app cat 00000001 5\n", &m), 1);
  CHECK_EQ(m.fw_version[0], 0);
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

/* ---- colour icons (2026-10-16) --------------------------------------------
 *
 * `update apps` carries the .cic files too. A server lists them only when
 * asked (/update?icons=1), and after the apps; an icon is named by the app's
 * own name, which can have a space in it ("Dashboard Link"), so the name is
 * the rest of the line. */

void test_manifest_reads_icons_named_to_the_end_of_the_line(void) {
  Manifest m;
  int n = manifest_parse("app dashlink 00000001 900 Net\n"
                         "icon 0000beef 520 Dashboard Link\r\n"
                         "icon DEADBEEF 520 folder-Games\n"
                         "icon 00000002 520 Edit   \n", &m);
  CHECK_EQ(n, 4);
  CHECK_EQ(m.napps, 1);
  CHECK_EQ(m.nicons, 3);
  CHECK(strcmp(m.icon[0].name, "Dashboard Link") == 0);
  CHECK_EQ(m.icon[0].hash, 0x0000beefu);
  CHECK_EQ((int)m.icon[0].size, 520);
  CHECK(strcmp(m.icon[1].name, "folder-Games") == 0);
  CHECK_EQ(m.icon[1].hash, 0xDEADBEEFu);
  CHECK(strcmp(m.icon[2].name, "Edit") == 0);     /* trailing spaces trimmed */
}

/* The name becomes /apps/icons/NAME.cic: no dot, no slash, nothing too long
 * to be what the launcher would look up. */
void test_manifest_an_icon_name_that_is_not_a_name_is_dropped(void) {
  Manifest m;
  int n = manifest_parse("icon 1 520 ../x\n"
                         "icon 1 520 a/b\n"
                         "icon 1 520 a.b\n"
                         "icon 1 520 abcdefghijklmnopqrstuvwxyz0123\n"
                         "icon zz 520 Bad Hash\n"
                         "icon 1 lots Bad Size\n"
                         "icon 1 520\n"
                         "icon 1 520 Ok_Name-2\n", &m);
  CHECK_EQ(n, 1);
  CHECK_EQ(m.nicons, 1);
  CHECK(strcmp(m.icon[0].name, "Ok_Name-2") == 0);
}

void test_manifest_counts_icons_that_do_not_fit(void) {
  static char text[(MANIFEST_MAX_ICONS + 5) * 32];
  Manifest m;
  int i, o = 0;
  for (i = 0; i < MANIFEST_MAX_ICONS + 5; i++)
    o += snprintf(text + o, sizeof text - o, "icon %08x 520 Icon %d\n", i, i);
  manifest_parse(text, &m);
  CHECK_EQ(m.nicons, MANIFEST_MAX_ICONS);
  CHECK_EQ(m.icons_dropped, 5);
  CHECK_EQ(m.dropped, 0);                         /* apps are counted apart */
}

/* What a server sends a device that did not ask for icons, and what every
 * server sent before them: parsed exactly as it was, with no icons. */
void test_manifest_an_old_manifest_has_no_icons(void) {
  Manifest m;
  CHECK_EQ(manifest_parse(SAMPLE, &m), 3);
  CHECK_EQ(m.napps, 2);
  CHECK_EQ(m.nicons, 0);
  CHECK_EQ(m.icons_dropped, 0);
}

/* The other direction: firmware from before icons skips any line whose
 * first word it does not know (the `font` line above), so an icon line
 * among the apps changes nothing about the apps. Checked here by parsing
 * with and without them -- the app half of the parser is the old one. */
void test_manifest_icon_lines_leave_the_apps_alone(void) {
  Manifest a, b;
  manifest_parse("app pinball 8c1e2b47 19672 Games\napp claude DEADBEEF 24312\n", &a);
  manifest_parse("icon 00000001 520 Pinball\napp pinball 8c1e2b47 19672 Games\n"
                 "icon 00000002 520 Claude\napp claude DEADBEEF 24312\n"
                 "icon 00000003 520 generic\n", &b);
  CHECK_EQ(a.napps, b.napps);
  CHECK(memcmp(a.app, b.app, sizeof a.app) == 0);
  CHECK_EQ(b.nicons, 3);
}

void test_manifest_diff_icons_finds_what_differs(void) {
  Manifest m;
  ManifestLocal l;
  uint8_t stale[MANIFEST_MAX_ICONS];
  manifest_parse("icon 00000001 520 Todo\nicon 00000002 520 Edit\n"
                 "icon 00000003 520 generic\n", &m);
  memset(&l, 0, sizeof l);
  l.have_icon[0] = 1; l.icon_hash[0] = 1;          /* current */
  l.have_icon[1] = 1; l.icon_hash[1] = 7;          /* differs */
  /* generic is not on the card at all */
  CHECK_EQ(manifest_diff_icons(&m, &l, stale), 2);
  CHECK_EQ(stale[0], 0);
  CHECK_EQ(stale[1], 1);
  CHECK_EQ(stale[2], 1);
}
