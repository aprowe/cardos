/* Small settings, kept in NVS and mirrored to /config/settings.txt.
 *
 * NVS is what a full-table flash wipes, and one day it cost the network, the
 * hotkeys and the Google login in a row. WiFi, Google and the environment
 * have mirrors of their own (/config/wifi.txt, google.txt, env.txt); this is
 * the one for everything else small -- brightness, volume, the shell at
 * boot, Bluetooth at boot, the desktop's autostart, the launcher's pins, the
 * screen timeouts. The rule, for anything new: a value in NVS that a person
 * chose gets a line here; only a true cache (the last known time) does not.
 *
 * The modules keep their own NVS reads and writes. After a write they call
 * prefs_mirror(), which rewrites the file from NVS for every key in the table
 * in prefs.c -- so the file is always what NVS says, never a second truth.
 * At boot, after the card mounts, prefs_restore_from_card() puts back any
 * key NVS has lost; where both have one, NVS wins, as for WiFi. A key must be
 * in the table to be mirrored: that table is the list of what survives.
 */
#ifndef CARDOS_PREFS_H
#define CARDOS_PREFS_H

#include <stdint.h>

#define PREFS_FILE "/config/settings.txt"
#define PREFS_NS   "cardos"

/* Rewrite /config/settings.txt from NVS. Quietly nothing without a card. */
void prefs_mirror(void);

/* Keys NVS lacks, from the file. Returns how many were put back. */
int  prefs_restore_from_card(void);

/* Forget the file too -- the console's `defaults`, and Settings' Forget all,
 * would otherwise be undone by the next boot's restore. */
void prefs_forget_file(void);

/* For settings with no module of their own to keep them (the screen
 * timeouts): read with a default, write and mirror. */
int  prefs_get_u16(const char *key, int def);
void prefs_set_u16(const char *key, int value);

#endif /* CARDOS_PREFS_H */
