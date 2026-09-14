/* WebDAV, the part that is not a socket.
 *
 * Portable C so the host suite can run it: the request parser, the path
 * decoder that is the one thing the server must not get wrong, the two date
 * formats, and the XML a PROPFIND answer is made of. kernel/net/share.c is
 * the device-side glue that owns the socket and the task and calls these.
 *
 * Only the headers the server acts on are read; everything else is skipped.
 * Header names are matched without case, as RFC 9110 requires. */
#ifndef CARDOS_DAV_H
#define CARDOS_DAV_H

#include <stddef.h>
#include <stdint.h>

#include "kernel/fs/path.h"

typedef enum {
  DAV_UNKNOWN = 0, DAV_OPTIONS, DAV_GET, DAV_HEAD, DAV_PUT, DAV_DELETE,
  DAV_MKCOL, DAV_PROPFIND, DAV_PROPPATCH, DAV_MOVE, DAV_COPY, DAV_LOCK,
  DAV_UNLOCK
} DavMethod;

#define DAV_DEPTH_INFINITY (-1)

typedef struct {
  DavMethod method;
  char     path[FS_PATH_MAX];   /* decoded, normalised CardOS path */
  char     dest[FS_PATH_MAX];   /* Destination, same treatment, or "" */
  int      depth;               /* 0, 1, or DAV_DEPTH_INFINITY (the default) */
  uint32_t content_length;
  int      has_content_length;
  int      chunked;             /* Transfer-Encoding: chunked */
  int      expect_continue;     /* Expect: 100-continue */
  int      keep_alive;          /* 1 unless Connection: close */
  int      overwrite;           /* 1 unless Overwrite: F */
  int      bad;                 /* 0, or the status to answer with instead */
} DavRequest;

/* Index just past the blank line that ends the headers, or -1 if `buf` does
 * not contain one yet. */
int dav_headers_end(const char *buf, size_t n);

/* Parse the request line and headers in hdr[0..n). Returns 0, or -1 with
 * req->bad set to the status to send: 400 for a line that is not HTTP, 403
 * for a path that would leave the card, 414 for one that does not fit. */
int dav_parse(const char *hdr, size_t n, DavRequest *req);

const char *dav_method_name(DavMethod m);

/* Percent-decode in[0..n) -- a request target or a Destination, which may be
 * a full http://host/... URL -- into a normalised CardOS path. 0 on success;
 * -1 if it is not a path the card can hold (relative, traversal, a backslash,
 * a control character, a query string, a NUL); -2 if it does not fit. */
int dav_decode_path(const char *in, size_t n, char *out, size_t out_size);

/* The reverse, for an href: percent-encode everything but unreserved
 * characters and '/'. A directory gets a trailing '/' when is_dir. Returns
 * bytes written, or -1 if it did not fit. */
int dav_encode_path(const char *path, int is_dir, char *out, size_t out_size);

/* "Sun, 06 Nov 1994 08:49:37 GMT" -- for getlastmodified and Last-Modified. */
void dav_http_date(uint32_t epoch, char *out, size_t out_size);
/* "1994-11-06T08:49:37Z" -- for creationdate. */
void dav_iso_date(uint32_t epoch, char *out, size_t out_size);

#define DAV_ALLOW "OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND, PROPPATCH, MOVE, COPY, LOCK, UNLOCK"

const char *dav_status_text(int status);            /* "Not Found" */
const char *dav_content_type(const char *path);     /* by extension; "application/octet-stream" */

/* A complete status line + headers + blank line. content_length < 0 means
 * chunked. `extra` is zero or more complete "Name: value\r\n" lines. Returns
 * bytes written, or -1 if it did not fit. */
int dav_response_head(int status, int32_t content_length, const char *content_type,
                      const char *extra, int keep_alive, char *out, size_t out_size);

typedef struct {
  const char *path;       /* full CardOS path of the entry */
  uint32_t    size;
  int         is_dir;
  uint32_t    mtime;
} DavEntry;

extern const char DAV_MULTISTATUS_HEAD[];
extern const char DAV_MULTISTATUS_TAIL[];

/* One <D:response> for an entry. Returns bytes written, or -1. */
int dav_propfind_entry(const DavEntry *e, char *out, size_t out_size);

/* The 207 body for PROPPATCH: says every property was set. */
int dav_proppatch_body(const char *path, char *out, size_t out_size);

/* The 200 body for LOCK, and the token it names for the Lock-Token header. */
#define DAV_LOCK_TOKEN "opaquelocktoken:cardos-0000-0000-0001"
int dav_lock_body(const char *path, char *out, size_t out_size);

#endif /* CARDOS_DAV_H */
