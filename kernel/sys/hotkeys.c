/* Opt+letter shortcuts. See hotkeys.h. */

#include "kernel/sys/hotkeys.h"

#include <stdio.h>
#include <string.h>

static char s_table[HOTKEY_SLOTS][HOTKEY_NAME_MAX];
static HotkeyStore s_store;

/* The letters global_key answers to itself. Kept here rather than in main.c
 * so the launcher, the console and the tests refuse the same ones. */
static const char RESERVED[] = "bwh";

static int slot_of(char letter) {
  if (letter >= 'A' && letter <= 'Z') letter = (char)(letter + 32);
  if (letter < 'a' || letter > 'z') return -1;
  return letter - 'a';
}

/* What the fixed table in main.c used to say, so nobody loses a chord they
 * had. */
static void seed(void) {
  memset(s_table, 0, sizeof s_table);
  snprintf(s_table['t' - 'a'], HOTKEY_NAME_MAX, "%s", "Todo");
  snprintf(s_table['s' - 'a'], HOTKEY_NAME_MAX, "%s", "Stocks");
  snprintf(s_table['e' - 'a'], HOTKEY_NAME_MAX, "%s", "Edit");
  snprintf(s_table['m' - 'a'], HOTKEY_NAME_MAX, "%s", "Mines");
}

static void save(void) {
  if (s_store.save) s_store.save(&s_table[0][0], HOTKEY_BLOB);
}

void hotkeys_init(const HotkeyStore *store) {
  int i, n = 0;

  if (store) s_store = *store;
  else { s_store.load = NULL; s_store.save = NULL; }

  memset(s_table, 0, sizeof s_table);
  if (s_store.load) n = s_store.load(&s_table[0][0], HOTKEY_BLOB);
  if (n <= 0) { seed(); return; }

  /* Whatever was saved, every slot ends in a NUL: a blob from a build with a
   * different HOTKEY_NAME_MAX must not become an unterminated name. */
  for (i = 0; i < HOTKEY_SLOTS; i++) s_table[i][HOTKEY_NAME_MAX - 1] = 0;
}

int hotkey_reserved(char letter) {
  int s = slot_of(letter);
  return s >= 0 && strchr(RESERVED, (char)('a' + s)) != NULL;
}

const char *hotkey_get(char letter) {
  int s = slot_of(letter);
  if (s < 0 || !s_table[s][0]) return NULL;
  return s_table[s];
}

int hotkey_set(char letter, const char *name) {
  int s = slot_of(letter);
  if (s < 0) return -2;
  if (hotkey_reserved(letter)) return -1;
  snprintf(s_table[s], HOTKEY_NAME_MAX, "%s", name ? name : "");
  save();
  return 0;
}

int hotkey_count(void) {
  int i, n = 0;
  for (i = 0; i < HOTKEY_SLOTS; i++) n += s_table[i][0] != 0;
  return n;
}
