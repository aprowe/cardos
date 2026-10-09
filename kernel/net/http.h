/* HTTP and HTTPS, blocking. Device-only.
 *
 * A request into a caller's buffer, a file posted, a file exchanged for a
 * file, a download to the card, and a stream to a callback. Anything that
 * does not fit in a buffer is truncated rather than failing: a stock quote
 * is in the first line whatever else follows.
 *
 * Blocking, for as long as the timeout allows. Called from the shell, the
 * screen stops while this runs -- say what is happening before calling;
 * kernel/net/httpq.c runs a request on a task of its own instead.
 *
 * EVERY function here returns, on failure, one of:
 *
 *   -1    no network (and none could be joined)
 *   -2    bad arguments: no URL, a URL the client cannot parse, a method it
 *         does not speak, a local file that cannot be read or written
 *   -3    transport: could not connect, cut off sending, no answer, the
 *         answer cut off. http_last_error() always says which.
 *   -4    not enough memory: for TLS, for the client, or for the transfer
 *   -NNN  the server answered with HTTP status NNN (-404 is a 404)
 *
 * and on success a byte count. http_last_error() is reset at the start of
 * every call, so it never describes an earlier request. */
#ifndef CARDOS_HTTP_H
#define CARDOS_HTTP_H

#include <stddef.h>
#include <stdint.h>

/* Returns the number of bytes written into `buf` -- at most size - 1, then a
 * NUL; size - 1 means the reply filled it and may have been longer. */
int http_get(const char *url, char *buf, size_t size, int timeout_ms);

/* The general form. `method` is "GET", "POST", "PATCH" or "DELETE"; `body` and
 * `content_type` are NULL for a request without one; `bearer` is an OAuth
 * access token, or NULL -- or, with a colon in it, header lines sent as they
 * are ("x-api-key: K\nanthropic-version: V"), for an API that does not take
 * a bearer. Every function here that takes `bearer` reads it this way.
 *
 * One call rather than three because the difference between them here is two
 * strings, and an API with get/post/patch would have to grow again the first
 * time something needed DELETE. */
int http_request(const char *method, const char *url,
                 const char *body, const char *content_type,
                 const char *bearer,
                 char *out, size_t out_size, int timeout_ms);

/* The same, without arming the busy indicator. For kernel/net/httpq.c, whose
 * caller is not blocked and draws its own: the badge is painted by a task of
 * its own and is only safe while nothing else is drawing. */
int http_request_quiet(const char *method, const char *url,
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

/* The same with a bearer token and a progress callback: `done` bytes so far,
 * `total` from Content-Length or 0 if the server did not say. A firmware is
 * 1.5 MB and takes long enough that a screen showing nothing looks hung. */
typedef void (*HttpProgress)(void *ctx, uint32_t done, uint32_t total);
int http_download_ex(const char *url, const char *path, const char *bearer,
                     HttpProgress progress, void *ctx, int timeout_ms);

/* Body from a file, reply to a file: the request the on-device Claude agent
 * makes, whose body is a conversation the RAM cannot hold and whose reply is
 * scanned from the card. Returns bytes of reply written, or the codes above;
 * on a non-2xx status the reply file is still written, because an API's
 * error is a document worth reading. Does not arm the busy indicator: its
 * caller is kernel/net/httpq.c. */
int http_exchange_files(const char *url, const char *body_path,
                        const char *content_type, const char *auth,
                        const char *reply_path, int timeout_ms);

/* A response read as it arrives, for a body that does not end.
 *
 * `on_data` is called with each chunk as it comes off the socket; returning
 * non-zero stops the transfer, which is how a viewer quits. Nothing is
 * buffered beyond one chunk, so a stream can be longer than the heap -- which
 * for a screen share is the point: the frames never stop.
 *
 * Returns bytes received, or the codes above -- a non-2xx status is -NNN,
 * as everywhere else (it used to be -4). However the stream ends once it
 * has begun -- the caller stopping it, the server, the socket -- is not an
 * error. */
typedef int (*HttpSink)(void *ctx, const uint8_t *data, int n);
int http_stream(const char *url, HttpSink on_data, void *ctx, int timeout_ms);
/* The same with a token (set_auth's forms), for a stream from a server that
 * wants one -- Music's tracks from this device's own. */
int http_stream_ex(const char *url, const char *bearer, HttpSink on_data, void *ctx,
                   int timeout_ms);

/* Why the last request failed, as a sentence -- "not enough memory: 21 KB
 * free, TLS needs about 34" rather than -4. An app that prints a number has
 * told the person holding the device nothing they can do something about. */
const char *http_last_error(void);

/* POST a file as the body, and put the reply in `out`.
 *
 * Streamed from the card rather than read into memory: a fifteen-second
 * recording is 480 KB and the heap has 120. Returns the reply length, or
 * negative. */
int http_post_file(const char *url, const char *path, const char *content_type,
                   char *out, size_t out_size, int timeout_ms);

/* The same, reporting how much has gone. `progress` is called every kilobyte
 * with bytes sent and the total -- half a megabyte over WiFi takes a few
 * seconds, and a bar that means something beats a spinner that does not. */
int http_post_file_progress(const char *url, const char *path,
                            const char *content_type, const char *bearer,
                            char *out, size_t out_size, int timeout_ms,
                            void (*progress)(int sent, int total));

#endif /* CARDOS_HTTP_H */
