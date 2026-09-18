/* The formatting and rotation policy behind kernel/sys/applog.c. Portable, so
 * the part with decisions in it runs under the host suite.
 *
 * A log on this device is a file on a card with no supervision: nothing
 * truncates it, nothing rotates it out of cron, and an app that logs once a
 * tick fills the card in an afternoon. So the policy is here, in one place,
 * and it is two questions -- what does a line look like, and when does the
 * file have to be turned over.
 */
#ifndef CARDOS_LOGRING_H
#define CARDOS_LOGRING_H

#include <stddef.h>
#include <stdint.h>

/* One line, terminated with a newline: "[    12.345] todo: syncing". The
 * timestamp is uptime, because there may be no clock -- a log whose first
 * lines are stamped 1970 is worse than one plainly counting from boot.
 * Returns the length written, always less than `n` and always ending in a
 * newline, however long the message was. */
int logring_line(char *out, size_t n, uint32_t ms, const char *tag,
                 const char *msg);

/* Would adding `add` bytes to a file of `size` take it past the cap? The
 * caller then renames the file aside and starts a new one, so the card holds
 * at most twice the cap. A line longer than the cap rotates and is then
 * written anyway: losing it would be worse than one oversized file. */
int logring_rotate_needed(uint32_t size, int add, uint32_t cap);

#endif /* CARDOS_LOGRING_H */
