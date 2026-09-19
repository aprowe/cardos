/* Small credential files under /config: one value per line.
 *
 * NVS is where WiFi and Google credentials live at runtime, and NVS is what
 * a full-table flash wipes. Each set of credentials is mirrored to a file on
 * the card -- /config/wifi.txt, /config/google.txt -- written whenever the
 * credentials are set and read back at boot when NVS has nothing, so a
 * reflash costs nothing and a hand-written file is a way to set up a fresh
 * device. The format is deliberately the simplest thing a person can type:
 * the values in a fixed order, one per line, CR or trailing spaces forgiven.
 *
 * The line handling (conf.c) is portable so the host suite can pin it; the
 * file calls (conf_file.c) are the device glue over fs. */
#ifndef CARDOS_CONF_H
#define CARDOS_CONF_H

#include <stddef.h>

/* Split `text` into up to `n` lines of at most `width` bytes each (NUL
 * included), trimming CR and trailing blanks. Blank lines are kept in place
 * -- the format is positional -- so a missing second value is an empty
 * string, not the third value shifted up. Returns how many lines were
 * filled; the rest are set to "". */
int conf_split(const char *text, char *lines, int n, size_t width);

/* Join `n` values as lines into `out`. Returns the length, or -1 if it
 * would not fit. */
int conf_join(const char *const *values, int n, char *out, size_t size);

/* The file forms. Read fills lines as conf_split does and returns the count
 * (0 for no file); write replaces the file, creating /config if needed;
 * remove deletes it. All three are no-ops without a card. */
int  conf_read(const char *path, char *lines, int n, size_t width);
int  conf_write(const char *path, const char *const *values, int n);
void conf_remove(const char *path);

#endif /* CARDOS_CONF_H */
