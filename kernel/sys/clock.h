/* What time it is -- which this machine has never known.
 *
 * The status bar has shown `s_now_ms / 60000 : s_now_ms / 1000 % 60` since the
 * launcher was written: minutes and seconds since boot, formatted as a clock
 * and put where a person reads the time of day. It looks right for the first
 * hour of uptime and is wrong forever after.
 *
 * There is no RTC on this board -- that is in the hardware notes, and it is
 * why this is harder than calling a function. Time comes from the network and
 * is lost at every power cut, so:
 *
 *   - nothing is displayed as a time until it has been synchronised, because
 *     a confident wrong clock is worse than an obvious missing one;
 *   - the sync runs on the background task, since it waits on a UDP round
 *     trip and the shell should not;
 *   - after a reboot the device is honestly ignorant again until WiFi is up.
 *
 * The zone comes from `env TZ`, in the POSIX form -- "GMT0BST,M3.5.0/1,M10.5.0"
 * for the UK, "EST5EDT,M3.2.0,M11.1.0" for New York. UTC if unset, which is at
 * least a defensible wrong answer rather than an arbitrary one.
 */
#ifndef CARDOS_CLOCK_H
#define CARDOS_CLOCK_H

#include <stdint.h>

/* Apply the zone from the environment and start SNTP if the network is up.
 * Cheap and idempotent; the actual waiting happens on the background task. */
void clock_init(void);

/* Re-read env TZ and apply it. Called at boot, at each sync, and by `set`
 * -- changing TZ has to take effect now, not at the next reboot. */
void clock_apply_zone(void);

/* Was a zone chosen at all, and what is it? Without this, a device quietly
 * running on UTC looks exactly like one whose clock is wrong. */
int         clock_zone_set(void);
const char *clock_zone(void);

/* Ask the network. Blocks for up to timeout_ms -- call it from the background
 * task, not from a shell. Returns 0 once the clock is set. */
int  clock_sync(int timeout_ms);

/* Did the NETWORK set it, this boot? The authoritative answer, and what to
 * check before trusting a time to the minute. */
int  clock_synced(void);

/* Is there a time at all -- either the network's, or the one restored from
 * the last save? True far more often than clock_synced, and the right question
 * for anything that wants today's date rather than the exact minute. */
int  clock_have_time(void);

/* Write the time down if the interval is up. Cheap; call it from the shell's
 * housekeeping every pass. */
void clock_persist_tick(void);

/* "14:32" into buf, or "--:--" if the clock has never been set. Local time,
 * per TZ. */
void clock_hm(char *buf, size_t n);

/* "Mon 12 Sep 14:32", or "not set". For anything with room for it. */
void clock_full(char *buf, size_t n);

/* Seconds since the epoch, or 0 if unknown. For a file timestamp or an "as
 * of" line -- and 0 is the value to check for before writing either. */
uint32_t clock_epoch(void);

#endif /* CARDOS_CLOCK_H */
