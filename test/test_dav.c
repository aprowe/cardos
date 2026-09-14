/* WebDAV on the host: everything in kernel/net/dav.c that is not a socket. */

#include <string.h>

#include "tinytest.h"
#include "kernel/net/dav.h"

void test_dav_finds_the_end_of_the_headers(void) {
  const char *r = "GET / HTTP/1.1\r\nHost: x\r\n\r\nbody";
  CHECK_EQ(dav_headers_end(r, strlen(r)), 27);
  CHECK_EQ(dav_headers_end(r, 10), -1);
  CHECK_EQ(dav_headers_end("", 0), -1);
}

void test_dav_parses_a_plain_get(void) {
  const char *r = "GET /desktop/a.txt HTTP/1.1\r\nHost: 1.2.3.4\r\n\r\n";
  DavRequest q;
  CHECK_EQ(dav_parse(r, strlen(r), &q), 0);
  CHECK_EQ(q.method, DAV_GET);
  CHECK(strcmp(q.path, "/desktop/a.txt") == 0);
  CHECK_EQ(q.keep_alive, 1);
  CHECK_EQ(q.depth, DAV_DEPTH_INFINITY);
  CHECK_EQ(q.has_content_length, 0);
  CHECK_EQ(q.overwrite, 1);
  CHECK_EQ(q.bad, 0);
}

void test_dav_reads_the_headers_it_cares_about(void) {
  const char *r =
    "PUT /x.bin HTTP/1.1\r\n"
    "content-length: 4096\r\n"
    "Connection: close\r\n"
    "Expect: 100-continue\r\n"
    "Depth: 1\r\n"
    "Overwrite: F\r\n"
    "Destination: http://1.2.3.4/y%20z.bin\r\n"
    "\r\n";
  DavRequest q;
  CHECK_EQ(dav_parse(r, strlen(r), &q), 0);
  CHECK_EQ(q.method, DAV_PUT);
  CHECK_EQ((int)q.content_length, 4096);
  CHECK_EQ(q.has_content_length, 1);
  CHECK_EQ(q.keep_alive, 0);
  CHECK_EQ(q.expect_continue, 1);
  CHECK_EQ(q.depth, 1);
  CHECK_EQ(q.overwrite, 0);
  CHECK(strcmp(q.dest, "/y z.bin") == 0);
}

void test_dav_knows_every_method_and_the_rest_is_unknown(void) {
  static const char *names[] = { "OPTIONS", "GET", "HEAD", "PUT", "DELETE",
    "MKCOL", "PROPFIND", "PROPPATCH", "MOVE", "COPY", "LOCK", "UNLOCK" };
  static const DavMethod ms[] = { DAV_OPTIONS, DAV_GET, DAV_HEAD, DAV_PUT,
    DAV_DELETE, DAV_MKCOL, DAV_PROPFIND, DAV_PROPPATCH, DAV_MOVE, DAV_COPY,
    DAV_LOCK, DAV_UNLOCK };
  char buf[64];
  DavRequest q;
  size_t i;
  for (i = 0; i < sizeof names / sizeof names[0]; i++) {
    sprintf(buf, "%s / HTTP/1.1\r\n\r\n", names[i]);
    CHECK_EQ(dav_parse(buf, strlen(buf), &q), 0);
    CHECK_EQ(q.method, ms[i]);
    CHECK(strcmp(dav_method_name(ms[i]), names[i]) == 0);
  }
  strcpy(buf, "BREW / HTTP/1.1\r\n\r\n");
  CHECK_EQ(dav_parse(buf, strlen(buf), &q), 0);
  CHECK_EQ(q.method, DAV_UNKNOWN);
}

void test_dav_rejects_a_malformed_request_line(void) {
  DavRequest q;
  const char *r = "GET\r\n\r\n";
  CHECK_EQ(dav_parse(r, strlen(r), &q), -1);
  CHECK_EQ(q.bad, 400);
  r = "GET /a HTTP/1.1";                     /* no terminator at all */
  CHECK_EQ(dav_parse(r, strlen(r), &q), -1);
  CHECK_EQ(q.bad, 400);
}

void test_dav_infinity_and_chunked_are_recorded_not_refused(void) {
  const char *r = "PROPFIND / HTTP/1.1\r\nDepth: infinity\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n";
  DavRequest q;
  CHECK_EQ(dav_parse(r, strlen(r), &q), 0);
  CHECK_EQ(q.depth, DAV_DEPTH_INFINITY);
  CHECK_EQ(q.chunked, 1);
}

/* A Content-Length that would overflow uint32_t, or is not a number at all,
 * is malformed -- not silently wrapped or ignored. The value that exactly
 * fills uint32_t is still good. */
void test_dav_a_content_length_that_overflows_is_400(void) {
  const char *r = "PUT /x HTTP/1.1\r\nContent-Length: 999999999999\r\n\r\n";
  DavRequest q;
  CHECK_EQ(dav_parse(r, strlen(r), &q), -1);
  CHECK_EQ(q.bad, 400);

  r = "PUT /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n";
  CHECK_EQ(dav_parse(r, strlen(r), &q), -1);
  CHECK_EQ(q.bad, 400);

  r = "PUT /x HTTP/1.1\r\nContent-Length: 4294967295\r\n\r\n";
  CHECK_EQ(dav_parse(r, strlen(r), &q), 0);
  CHECK_EQ(q.has_content_length, 1);
  CHECK_EQ(q.content_length, 4294967295u);
}
