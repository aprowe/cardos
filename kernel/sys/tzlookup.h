/* Asking the server what time zone this is, once, when TZ has never been set.
 *
 * The request runs on a task of its own, started for it and gone after --
 * not on cardos-bg, whose 4 KB stack a network request overflowed on every
 * boot (2026-09-23) -- and the answer is applied by the shell, the one task
 * that reads env and the clock, via tzlookup_take(). The server does the
 * work: server/tz.py. */
#ifndef CARDOS_TZLOOKUP_H
#define CARDOS_TZLOOKUP_H

#include <stddef.h>

/* Start a lookup if TZ is unset and none is running. Cheap to call. */
void tzlookup_start(void);

/* The POSIX rule a finished lookup found, once, or NULL; the zone's name in
 * `zone`. For the shell's loop. */
const char *tzlookup_take(char *zone, size_t n);

#endif
