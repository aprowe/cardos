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
 * freed. An app that walks away leaks nothing.
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
 * caller may reuse or free them the moment this returns. */
int httpq_start(const char *method, const char *url, const char *body,
                const char *content_type, const char *bearer, int timeout_ms);

/* HTTPQ_PENDING while it runs. Otherwise whatever http_request would have
 * returned -- bytes on success, negative on failure, an HTTP status negated
 * into it -- and the body is copied into `out`, truncated to fit. Collecting
 * frees the reply and readies the next request, so poll until it is not
 * pending and then stop. */
int httpq_poll(char *out, size_t out_size);

/* Is a request in flight? For a shell that wants to show a spinner, and for
 * anything deciding whether to start another. */
int httpq_active(void);

#endif /* CARDOS_HTTPQ_H */
