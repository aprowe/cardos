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

#endif /* CARDOS_DAV_H */
