/* Opt+letter bindings. The store is a fake so the tests can see what was
 * saved and hand it back, which is the whole round trip NVS provides. */
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/hotkeys.h"

static char s_blob[26 * HOTKEY_NAME_MAX];
static int  s_blob_len;
static int  s_saves;

static int fake_load(char *buf, int size) {
  if (s_blob_len <= 0) return 0;
  if (size > s_blob_len) size = s_blob_len;
  memcpy(buf, s_blob, (size_t)size);
  return size;
}
static void fake_save(const char *buf, int len) {
  if (len > (int)sizeof s_blob) len = (int)sizeof s_blob;
  memcpy(s_blob, buf, (size_t)len);
  s_blob_len = len;
  s_saves++;
}
static const HotkeyStore FAKE = { fake_load, fake_save };

static void fresh(void) { s_blob_len = 0; s_saves = 0; hotkeys_init(&FAKE); }

void test_hotkeys_first_boot_seeds_the_old_table(void) {
  fresh();
  CHECK(!strcmp(hotkey_get('t'), "Todo"));
  CHECK(!strcmp(hotkey_get('s'), "Stocks"));
  CHECK(!strcmp(hotkey_get('e'), "Edit"));
  CHECK(!strcmp(hotkey_get('m'), "Mines"));
  CHECK(hotkey_get('q') == NULL);
  CHECK_EQ(hotkey_count(), 4);
}

void test_hotkeys_set_get_clear_and_save(void) {
  fresh();
  CHECK_EQ(hotkey_set('p', "Pinball"), 0);
  CHECK(!strcmp(hotkey_get('p'), "Pinball"));
  CHECK(!strcmp(hotkey_get('P'), "Pinball"));   /* case does not matter */
  CHECK_EQ(s_saves, 1);
  CHECK_EQ(hotkey_set('p', NULL), 0);
  CHECK(hotkey_get('p') == NULL);
  CHECK_EQ(hotkey_set('p', ""), 0);
  CHECK_EQ(s_saves, 3);
}

void test_hotkeys_survive_a_reload(void) {
  fresh();
  hotkey_set('p', "Pinball");
  hotkey_set('t', NULL);
  hotkeys_init(&FAKE);                            /* "reboot" */
  CHECK(!strcmp(hotkey_get('p'), "Pinball"));
  CHECK(hotkey_get('t') == NULL);                 /* the seed did not come back */
  CHECK(!strcmp(hotkey_get('s'), "Stocks"));
}

void test_hotkeys_refuse_reserved_and_bad_letters(void) {
  fresh();
  CHECK(hotkey_reserved('b'));
  CHECK(hotkey_reserved('W'));
  CHECK(hotkey_reserved('h'));
  CHECK(!hotkey_reserved('x'));
  CHECK_EQ(hotkey_set('b', "Mines"), -1);
  CHECK_EQ(hotkey_set('1', "Mines"), -2);
  CHECK_EQ(hotkey_set(0, "Mines"), -2);
  CHECK(hotkey_get('1') == NULL);
  CHECK(hotkey_get('b') == NULL);
}

void test_hotkeys_truncate_a_long_name(void) {
  fresh();
  CHECK_EQ(hotkey_set('x', "ANameLongerThanFifteenCharacters"), 0);
  CHECK_EQ((int)strlen(hotkey_get('x')), HOTKEY_NAME_MAX - 1);
}

void test_hotkeys_work_without_a_store(void) {
  hotkeys_init(NULL);
  CHECK(!strcmp(hotkey_get('t'), "Todo"));
  CHECK_EQ(hotkey_set('x', "Web"), 0);
  CHECK(!strcmp(hotkey_get('x'), "Web"));
}
