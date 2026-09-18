/* One HTTP request at a time, off the shell's loop. Device-only.
 *
 * The shell is a single cooperative loop, so a blocking request freezes the
 * machine for as long as it takes -- twenty seconds of dead keyboard for a
 * calendar sync. kernel/sys/busy.c made that visible; this makes it stop
 * happening. The caller starts a request and returns immediately, then polls
 * from its tick until the answer is there. apps/claude.c has always worked
 * this way and it is the pattern that survives on this hardware.
 *
 * A task of its own rather than a job on kernel/sys/bg.c: that task also
 * reconnects mice and rejoins WiFi, and a twenty-second download sitting in
 * front of those would trade one stall for another.
 *
 * ONE AT A TIME, deliberately. Two concurrent TLS sessions is 34 KB of
 * headroom this board does not have -- http.c already refuses a request when
 * memory is short, and the fix for that is not to allow two.
 *
 * WHO OWNS THE REPLY. This does, until it is collected. The caller passes no
 * buffer, because the obvious design -- fill the app's array -- writes into
 * an app that lazy loading may have unloaded while the request was in flight.
 * The buffer is allocated here, filled here, copied out on collection and
 * freed. An app that walks away leaks nothing -- and, since ownership, wedges
 * nothing either. Every request names its owner; the app loader calls
 * httpq_abandon for a slot it unloads, and a reply that owner never collected
 * is dropped instead of sitting here answering "busy" to everyone else until
 * the next reboot. That is what "Todo and Calendar keep failing" was:
 * leave either one within the two seconds its sync takes, and neither could
 * start another request. kernel/net/httpslot.c holds the bookkeeping.
 */
#ifndef CARDOS_HTTPQ_H
#define CARDOS_HTTPQ_H

#include <stddef.h>

/* Returned by httpq_poll while the request is still running. Chosen far from
 * any HTTP status negated into a return, so it cannot be mistaken for one. */
#define HTTPQ_PENDING (-1000)

void httpq_init(void);

/* Start one. Returns 0 if accepted, -1 if a request is already in flight,
 * -2 if there was no memory for the reply. The strings are copied, so the
 * caller may reuse or free them the moment this returns. `owner` is any
 * address that identifies the caller -- an app's slot, a module's static --
 * and is what httpq_abandon matches on. */
int httpq_start(const void *owner, const char *method, const char *url,
                const char *body, const char *content_type, const char *bearer,
                int timeout_ms);

/* The same, with the body read from a file on the card and the reply
 * written to one: for a request too large to hold, which is what a
 * conversation is. `auth` reads as http_request's `bearer` does. The poll
 * below then returns bytes written or a failure, and copies nothing. */
int httpq_start_files(const void *owner, const char *url,
                      const char *body_path, const char *content_type,
                      const char *auth, const char *reply_path,
                      int timeout_ms);

/* HTTPQ_PENDING while it runs. Otherwise whatever http_request would have
 * returned -- bytes on success, negative on failure, an HTTP status negated
 * into it -- and the body is copied into `out`, truncated to fit. Collecting
 * frees the reply and readies the next request, so poll until it is not
 * pending and then stop. */
int httpq_poll(char *out, size_t out_size);

/* Is a request in flight? For a shell that wants to show a spinner, and for
 * anything deciding whether to start another. */
int httpq_active(void);

/* `owner` is going away and will never poll again. Their reply, waiting or
 * still to come, is dropped, and the slot is free for the next caller. A
 * request that is not theirs is untouched. */
void httpq_abandon(const void *owner);

#endif /* CARDOS_HTTPQ_H */
