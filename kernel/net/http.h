/* One-shot HTTP GET into a caller's buffer. Device-only.
 *
 * Deliberately not a streaming client. An app on this board has a few
 * kilobytes to spare and no business holding a connection open across paint
 * calls, so the whole shape of the API is "ask for a small thing, block, get
 * it or don't". Anything that does not fit in the buffer is truncated rather
 * than failing: a stock quote is in the first line whatever else follows.
 *
 * Blocking, for as long as the timeout allows. The caller is the main loop, so
 * the screen stops while this runs -- say what is happening before calling.
 */
#ifndef CARDOS_HTTP_H
#define CARDOS_HTTP_H

#include <stddef.h>

/* Returns the number of bytes written (NUL-terminated), or negative:
 *   -1 no network        -2 bad URL or request refused
 *   -3 transport failure -4 HTTP status was not 2xx (the status is negated
 *                           into the return when it fits, so -404 is a 404) */
int http_get(const char *url, char *buf, size_t size, int timeout_ms);

/* The general form. `method` is "GET", "POST", "PATCH" or "DELETE"; `body` and
 * `content_type` are NULL for a request without one; `bearer` is an OAuth
 * access token, or NULL.
 *
 * One call rather than three because the difference between them here is two
 * strings, and an API with get/post/patch would have to grow again the first
 * time something needed DELETE. */
int http_request(const char *method, const char *url,
                 const char *body, const char *content_type,
                 const char *bearer,
                 char *out, size_t out_size, int timeout_ms);

/* Straight to a file, in chunks, for a body too large to hold. Returns the
 * number of bytes written, or the same negative codes as above.
 *
 * The alternative -- a buffer the caller sizes -- does not exist for a 200 KB
 * page on a board with 150 KB of heap, which is the whole reason this is
 * separate. */
int http_download(const char *url, const char *path, int timeout_ms);

#endif /* CARDOS_HTTP_H */
