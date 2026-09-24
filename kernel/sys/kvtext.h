/* key=value text, one pair a line: /config/settings.txt.
 *
 * Portable, so the host suite reads the same lines the device does. A line
 * is `key=value`; blank lines and lines starting with # are skipped; spaces
 * around the key, and CR and trailing spaces after the value, are forgiven,
 * because the file is also meant to be edited by hand on a card reader.
 */
#ifndef CARDOS_KVTEXT_H
#define CARDOS_KVTEXT_H

#include <stddef.h>

/* The next pair at *pos, copied into key and value (truncated to fit, NUL
 * included; \n and \\ in a value become a newline and a backslash, the
 * escapes kv_put writes), and *pos moved past its line. 1, or 0 at the end. */
int kv_next(const char **pos, char *key, size_t ksize, char *value, size_t vsize);

/* "key=value\n" onto out at *len, if it fits. 0, or -1 (nothing written). */
int kv_put(char *out, size_t size, size_t *len, const char *key, const char *value);

#endif /* CARDOS_KVTEXT_H */
