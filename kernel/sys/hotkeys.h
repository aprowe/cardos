/* Opt+letter shortcuts. Portable: the store is two callbacks so this file
 * has host tests and the card glue lives with the rest of it in src/main.c.
 *
 * Twenty-six slots, each an app name, saved as text: one `t=Todo` line per
 * binding, in letter order, so /settings/hotkeys.txt can be read and edited
 * by hand. A name rather than a path because that is what the launcher
 * shows and what `run` takes, and an app that moves into a folder keeps its
 * name. There are no defaults: a fresh card has no hotkeys until you bind
 * one. */
#ifndef CARDOS_HOTKEYS_H
#define CARDOS_HOTKEYS_H

#define HOTKEY_NAME_MAX 16
#define HOTKEY_SLOTS    26
/* Room for every slot as "x=<name>\n", plus a NUL. */
#define HOTKEY_TEXT_MAX (HOTKEY_SLOTS * (HOTKEY_NAME_MAX + 3) + 1)

typedef struct {
  int  (*load)(char *buf, int size);   /* NUL-terminated text; bytes, or <= 0 for none */
  void (*save)(const char *text);      /* the whole file, NUL-terminated */
} HotkeyStore;

/* Loads the table; nothing saved means nothing bound. NULL for a store keeps
 * the table in memory only. */
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
