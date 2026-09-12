/* Opt+letter shortcuts. Portable: the store is two callbacks so this file
 * has host tests and the NVS glue lives with the rest of it in src/main.c.
 *
 * Twenty-six slots, each an app name, saved as one blob. A name rather than
 * a path because that is what the launcher shows and what `run` takes, and
 * an app that moves into a folder keeps its name. */
#ifndef CARDOS_HOTKEYS_H
#define CARDOS_HOTKEYS_H

#define HOTKEY_NAME_MAX 16
#define HOTKEY_SLOTS    26
#define HOTKEY_BLOB     (HOTKEY_SLOTS * HOTKEY_NAME_MAX)

typedef struct {
  int  (*load)(char *buf, int size);   /* bytes read, or <= 0 for none */
  void (*save)(const char *buf, int len);
} HotkeyStore;

/* Loads the table, or seeds the defaults when there is nothing saved.
 * NULL for a store keeps the table in memory only. */
void hotkeys_init(const HotkeyStore *store);

/* The app bound to a letter, or NULL. Case does not matter. */
const char *hotkey_get(char letter);

/* Bind, or clear with NULL or "". 0 on success, -1 for a letter the system
 * owns, -2 for something that is not a letter. Saved at once. */
int hotkey_set(char letter, const char *name);

/* Letters the shell itself answers to: b (bluetooth), w (wifi), h (help). */
int hotkey_reserved(char letter);

/* How many letters are bound. */
int hotkey_count(void);

#endif /* CARDOS_HOTKEYS_H */
