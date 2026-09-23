/* The server's /tz answer, read. Portable so the host suite can reach it;
 * kernel/sys/tzlookup.c is the device glue that asks. */
#ifndef CARDOS_TZREPLY_H
#define CARDOS_TZREPLY_H

#include <stddef.h>

/* "tz <POSIX rule>\nzone <IANA name>\n" -> 0, the rule in `rule` and the
 * name (or "") in `zone`. Anything else -- an error line, an IANA name where
 * the rule should be, a character no rule has, a rule too long for `rule` --
 * is -1: this goes into env TZ, which every clock on the machine reads. */
int tzreply_parse(const char *reply, char *rule, size_t rn, char *zone, size_t zn);

#endif
