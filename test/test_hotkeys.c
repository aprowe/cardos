/* Opt+letter bindings. The store is a fake so the tests can see what was
 * saved and hand it back, which is the whole round trip the card provides. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/sys/hotkeys.h"

static char s_text[HOTKEY_TEXT_MAX];
static int  s_saves;

static int fake_load(char *buf, int size) {
  int n = (int)strlen(s_text);
  if (n == 0) return 0;
  if (n >= size) n = size - 1;
  memcpy(buf, s_text, (size_t)n);
  buf[n] = 0;
  return n;
}
static void fake_save(const char *text) {
  snprintf(s_text, sizeof s_text, "%s", text);
  s_saves++;
}
static const HotkeyStore FAKE = { fake_load, fake_save };

static void fresh(void) { s_text[0] = 0; s_saves = 0; hotkeys_init(&FAKE); }

void test_hotkeys_first_boot_is_empty(void) {
  fresh();
  CHECK(hotkey_get('t') == NULL);
  CHECK(hotkey_get('s') == NULL);
  CHECK_EQ(hotkey_count(), 0);
  CHECK_EQ(s_saves, 0);                           /* nothing to write yet */
}

void test_hotkeys_set_get_clear_and_save(void) {
  fresh();
  CHECK_EQ(hotkey_set('p', "Pinball"), 0);
  CHECK(!strcmp(hotkey_get('p'), "Pinball"));
  CHECK(!strcmp(hotkey_get('P'), "Pinball"));   /* case does not matter */
  CHECK_EQ(s_saves, 1);
  CHECK(!strcmp(s_text, "p=Pinball\n"));          /* the file is one line per binding */
  CHECK_EQ(hotkey_set('p', NULL), 0);
  CHECK(hotkey_get('p') == NULL);
  CHECK(!strcmp(s_text, ""));                     /* unbinding the last leaves it empty */
  CHECK_EQ(hotkey_set('p', ""), 0);
  CHECK_EQ(s_saves, 3);
}

void test_hotkeys_survive_a_reload(void) {
  fresh();
  hotkey_set('p', "Pinball");
  hotkey_set('a', "Todo");
  hotkeys_init(&FAKE);                            /* "reboot" */
  CHECK(!strcmp(hotkey_get('p'), "Pinball"));
  CHECK(!strcmp(hotkey_get('a'), "Todo"));
  CHECK_EQ(hotkey_count(), 2);
  CHECK(!strcmp(s_text, "a=Todo\np=Pinball\n"));  /* written in letter order */
}

void test_hotkeys_load_ignores_what_is_not_a_binding(void) {
  fresh();
  snprintf(s_text, sizeof s_text, "%s",
           "# hand-edited\r\n"
           "\n"
           "x=Web\n"
           "b=Mines\n"             /* reserved: the shell owns b */
           "=Nothing\n"
           "9=Digit\n"
           "junk line\n"
           "T = Todo \n"           /* spaces and case are forgiven */
           "z=ANameLongerThanFifteenCharacters\n");
  hotkeys_init(&FAKE);
  CHECK(!strcmp(hotkey_get('x'), "Web"));
  CHECK(hotkey_get('b') == NULL);
  CHECK(!strcmp(hotkey_get('t'), "Todo"));
  CHECK_EQ((int)strlen(hotkey_get('z')), HOTKEY_NAME_MAX - 1);
  CHECK_EQ(hotkey_count(), 3);
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
  CHECK(hotkey_get('t') == NULL);
  CHECK_EQ(hotkey_set('x', "Web"), 0);
  CHECK(!strcmp(hotkey_get('x'), "Web"));
}
