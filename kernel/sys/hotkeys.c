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

/* The file, one binding per line in letter order. Empty when nothing is
 * bound, which is also what a fresh card has. */
static void save(void) {
  char text[HOTKEY_TEXT_MAX];
  int i, n = 0;
  if (!s_store.save) return;
  for (i = 0; i < HOTKEY_SLOTS; i++) {
    if (!s_table[i][0]) continue;
    n += snprintf(text + n, sizeof text - (size_t)n, "%c=%s\n", 'a' + i, s_table[i]);
  }
  text[n] = 0;
  s_store.save(text);
}

/* One line of the file: `letter=name`. Spaces around either are forgiven,
 * as is a CR; anything else -- a comment, a digit, a reserved letter, an
 * empty name -- is not a binding and is skipped rather than refused, since a
 * hand-edited file with one bad line should still give you the rest. */
static void load_line(const char *line, int len) {
  const char *eq, *name, *end;
  int s;
  while (len > 0 && (*line == ' ' || *line == '\t')) { line++; len--; }
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\r')) len--;
  if (len < 3) return;
  eq = memchr(line, '=', (size_t)len);
  if (!eq) return;
  end = eq;
  while (end > line && (end[-1] == ' ' || end[-1] == '\t')) end--;
  if (end - line != 1) return;                     /* one letter before the = */
  s = slot_of(*line);
  if (s < 0 || hotkey_reserved(*line)) return;
  name = eq + 1;
  while (name < line + len && (*name == ' ' || *name == '\t')) name++;
  if (name >= line + len) return;
  snprintf(s_table[s], HOTKEY_NAME_MAX, "%.*s", (int)(line + len - name), name);
}

void hotkeys_init(const HotkeyStore *store) {
  char text[HOTKEY_TEXT_MAX];
  const char *p, *nl;
  int n = 0;

  if (store) s_store = *store;
  else { s_store.load = NULL; s_store.save = NULL; }

  memset(s_table, 0, sizeof s_table);
  if (s_store.load) n = s_store.load(text, (int)sizeof text);
  if (n <= 0) return;
  text[n < (int)sizeof text ? n : (int)sizeof text - 1] = 0;

  for (p = text; *p; p = nl + 1) {
    nl = strchr(p, '\n');
    if (!nl) { load_line(p, (int)strlen(p)); break; }
    load_line(p, (int)(nl - p));
  }
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
