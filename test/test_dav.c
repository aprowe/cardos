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

static int dec(const char *in, char *out, size_t n) {
  return dav_decode_path(in, strlen(in), out, n);
}

void test_dav_decodes_percent_escapes_and_normalises(void) {
  char out[FS_PATH_MAX];
  CHECK_EQ(dec("/desktop/a%20b.txt", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop/a b.txt") == 0);
  CHECK_EQ(dec("/desktop/", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop") == 0);
  CHECK_EQ(dec("//desktop///x", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop/x") == 0);
  CHECK_EQ(dec("/", out, sizeof out), 0);
  CHECK(strcmp(out, "/") == 0);
  CHECK_EQ(dec("/caf%C3%A9", out, sizeof out), 0);
  CHECK(strcmp(out, "/caf\xC3\xA9") == 0);
}

void test_dav_strips_the_host_from_a_full_url(void) {
  char out[FS_PATH_MAX];
  CHECK_EQ(dec("http://192.168.1.23/desktop/x", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop/x") == 0);
  CHECK_EQ(dec("http://cardos:80/", out, sizeof out), 0);
  CHECK(strcmp(out, "/") == 0);
}

void test_dav_refuses_every_shape_of_traversal(void) {
  char out[FS_PATH_MAX];
  CHECK_EQ(dec("/../x", out, sizeof out), -1);
  CHECK_EQ(dec("/desktop/../../x", out, sizeof out), -1);
  CHECK_EQ(dec("/%2e%2e/x", out, sizeof out), -1);
  CHECK_EQ(dec("/%2E%2E/x", out, sizeof out), -1);
  CHECK_EQ(dec("/desktop/..%2fx", out, sizeof out), -1);
  CHECK_EQ(dec("/desktop\\x", out, sizeof out), -1);
  CHECK_EQ(dec("/desktop%5cx", out, sizeof out), -1);
  CHECK_EQ(dec("/x%00y", out, sizeof out), -1);
  CHECK_EQ(dec("/x?y=1", out, sizeof out), -1);
  CHECK_EQ(dec("desktop/x", out, sizeof out), -1);
  CHECK_EQ(dec("", out, sizeof out), -1);
  CHECK_EQ(dec("/x%zz", out, sizeof out), -1);
  CHECK_EQ(dec("/x\x01", out, sizeof out), -1);
}

void test_dav_a_single_dot_segment_is_harmless(void) {
  char out[FS_PATH_MAX];
  CHECK_EQ(dec("/desktop/./x", out, sizeof out), 0);
  CHECK(strcmp(out, "/desktop/x") == 0);
}

void test_dav_a_path_that_does_not_fit_is_414_not_truncated(void) {
  char in[300], out[FS_PATH_MAX];
  memset(in, 'a', sizeof in);
  in[0] = '/';
  in[sizeof in - 1] = 0;
  CHECK_EQ(dav_decode_path(in, strlen(in), out, sizeof out), -2);
}

void test_dav_encodes_an_href(void) {
  char out[256];
  CHECK(dav_encode_path("/desktop/a b.txt", 0, out, sizeof out) > 0);
  CHECK(strcmp(out, "/desktop/a%20b.txt") == 0);
  CHECK(dav_encode_path("/desktop", 1, out, sizeof out) > 0);
  CHECK(strcmp(out, "/desktop/") == 0);
  CHECK(dav_encode_path("/", 1, out, sizeof out) > 0);
  CHECK(strcmp(out, "/") == 0);
  CHECK(dav_encode_path("/caf\xC3\xA9&<>", 0, out, sizeof out) > 0);
  CHECK(strcmp(out, "/caf%C3%A9%26%3C%3E") == 0);
  CHECK_EQ(dav_encode_path("/desktop/a b", 0, out, 12), -1);
}

void test_dav_formats_the_rfc_example_date(void) {
  char out[40];
  dav_http_date(784111777u, out, sizeof out);     /* the RFC 7231 example */
  CHECK(strcmp(out, "Sun, 06 Nov 1994 08:49:37 GMT") == 0);
  dav_iso_date(784111777u, out, sizeof out);
  CHECK(strcmp(out, "1994-11-06T08:49:37Z") == 0);
}

void test_dav_formats_the_epoch_and_a_leap_day(void) {
  char out[40];
  dav_http_date(0, out, sizeof out);
  CHECK(strcmp(out, "Thu, 01 Jan 1970 00:00:00 GMT") == 0);
  dav_http_date(1709164800u, out, sizeof out);    /* 2024-02-29 00:00:00 */
  CHECK(strcmp(out, "Thu, 29 Feb 2024 00:00:00 GMT") == 0);
  dav_http_date(1789603199u, out, sizeof out);    /* 2026-09-16 23:59:59 */
  CHECK(strcmp(out, "Wed, 16 Sep 2026 23:59:59 GMT") == 0);
}

void test_dav_response_head_has_the_headers_windows_needs(void) {
  char out[512];
  int n = dav_response_head(200, 0, NULL,
    "DAV: 1,2\r\nMS-Author-Via: DAV\r\nAllow: " DAV_ALLOW "\r\n", 1, out, sizeof out);
  CHECK(n > 0);
  CHECK(strncmp(out, "HTTP/1.1 200 OK\r\n", 17) == 0);
  CHECK(strstr(out, "Content-Length: 0\r\n") != NULL);
  CHECK(strstr(out, "DAV: 1,2\r\n") != NULL);
  CHECK(strstr(out, "MS-Author-Via: DAV\r\n") != NULL);
  CHECK(strstr(out, "Connection: keep-alive\r\n") != NULL);
  CHECK(strstr(out, "Server: CardOS\r\n") != NULL);
  CHECK(strcmp(out + n - 4, "\r\n\r\n") == 0);

  n = dav_response_head(207, -1, "text/xml; charset=\"utf-8\"", NULL, 0, out, sizeof out);
  CHECK(n > 0);
  CHECK(strncmp(out, "HTTP/1.1 207 Multi-Status\r\n", 27) == 0);
  CHECK(strstr(out, "Transfer-Encoding: chunked\r\n") != NULL);
  CHECK(strstr(out, "Content-Length:") == NULL);
  CHECK(strstr(out, "Connection: close\r\n") != NULL);
  CHECK(strstr(out, "Content-Type: text/xml; charset=\"utf-8\"\r\n") != NULL);

  CHECK_EQ(dav_response_head(200, 0, NULL, NULL, 1, out, 20), -1);
}

void test_dav_knows_its_status_texts(void) {
  CHECK(strcmp(dav_status_text(100), "Continue") == 0);
  CHECK(strcmp(dav_status_text(201), "Created") == 0);
  CHECK(strcmp(dav_status_text(204), "No Content") == 0);
  CHECK(strcmp(dav_status_text(207), "Multi-Status") == 0);
  CHECK(strcmp(dav_status_text(403), "Forbidden") == 0);
  CHECK(strcmp(dav_status_text(404), "Not Found") == 0);
  CHECK(strcmp(dav_status_text(405), "Method Not Allowed") == 0);
  CHECK(strcmp(dav_status_text(409), "Conflict") == 0);
  CHECK(strcmp(dav_status_text(411), "Length Required") == 0);
  CHECK(strcmp(dav_status_text(412), "Precondition Failed") == 0);
  CHECK(strcmp(dav_status_text(414), "URI Too Long") == 0);
  CHECK(strcmp(dav_status_text(415), "Unsupported Media Type") == 0);
  CHECK(strcmp(dav_status_text(431), "Request Header Fields Too Large") == 0);
  CHECK(strcmp(dav_status_text(507), "Insufficient Storage") == 0);
  CHECK(strcmp(dav_status_text(599), "Unknown") == 0);
}

void test_dav_guesses_content_types(void) {
  CHECK(strcmp(dav_content_type("/a.txt"), "text/plain") == 0);
  CHECK(strcmp(dav_content_type("/a.md"), "text/plain") == 0);
  CHECK(strcmp(dav_content_type("/a.html"), "text/html") == 0);
  CHECK(strcmp(dav_content_type("/a.png"), "image/png") == 0);
  CHECK(strcmp(dav_content_type("/a.wav"), "audio/wav") == 0);
  CHECK(strcmp(dav_content_type("/a.capp"), "application/octet-stream") == 0);
  CHECK(strcmp(dav_content_type("/noext"), "application/octet-stream") == 0);
  CHECK(strcmp(dav_content_type("/a.TXT"), "text/plain") == 0);
}

void test_dav_propfind_entry_for_a_file(void) {
  DavEntry e = { "/desktop/a b.txt", 1234, 0, 784111777u };
  char out[1024];
  int n = dav_propfind_entry(&e, out, sizeof out);
  CHECK(n > 0);
  CHECK(strcmp(out,
    "<D:response>"
    "<D:href>/desktop/a%20b.txt</D:href>"
    "<D:propstat><D:prop>"
    "<D:resourcetype/>"
    "<D:getcontentlength>1234</D:getcontentlength>"
    "<D:getlastmodified>Sun, 06 Nov 1994 08:49:37 GMT</D:getlastmodified>"
    "<D:creationdate>1994-11-06T08:49:37Z</D:creationdate>"
    "<D:displayname>a b.txt</D:displayname>"
    "<D:getcontenttype>text/plain</D:getcontenttype>"
    "<D:getetag>\"4d2-2ebc98a1\"</D:getetag>"
    "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
    "</D:response>") == 0);
}

void test_dav_propfind_entry_for_a_folder_and_the_root(void) {
  DavEntry d = { "/desktop", 0, 1, 0 };
  DavEntry r = { "/", 0, 1, 0 };
  char out[1024];
  CHECK(dav_propfind_entry(&d, out, sizeof out) > 0);
  CHECK(strstr(out, "<D:href>/desktop/</D:href>") != NULL);
  CHECK(strstr(out, "<D:resourcetype><D:collection/></D:resourcetype>") != NULL);
  CHECK(strstr(out, "<D:displayname>desktop</D:displayname>") != NULL);
  CHECK(strstr(out, "getcontentlength") == NULL);
  CHECK(strstr(out, "getcontenttype") == NULL);
  CHECK(dav_propfind_entry(&r, out, sizeof out) > 0);
  CHECK(strstr(out, "<D:href>/</D:href>") != NULL);
  CHECK(strstr(out, "<D:displayname></D:displayname>") != NULL);
  CHECK_EQ(dav_propfind_entry(&d, out, 64), -1);
}

void test_dav_escapes_xml_in_names(void) {
  DavEntry e = { "/a&b<c>.txt", 1, 0, 0 };
  char out[1024];
  CHECK(dav_propfind_entry(&e, out, sizeof out) > 0);
  CHECK(strstr(out, "<D:displayname>a&amp;b&lt;c&gt;.txt</D:displayname>") != NULL);
  CHECK(strstr(out, "<D:href>/a%26b%3Cc%3E.txt</D:href>") != NULL);
}

void test_dav_multistatus_wrapper_and_the_yes_bodies(void) {
  char out[1024];
  CHECK(strstr(DAV_MULTISTATUS_HEAD, "<D:multistatus xmlns:D=\"DAV:\">") != NULL);
  CHECK(strcmp(DAV_MULTISTATUS_TAIL, "</D:multistatus>") == 0);

  CHECK(dav_proppatch_body("/x y", out, sizeof out) > 0);
  CHECK(strstr(out, "<D:href>/x%20y</D:href>") != NULL);
  CHECK(strstr(out, "<D:status>HTTP/1.1 200 OK</D:status>") != NULL);

  CHECK(dav_lock_body("/x", out, sizeof out) > 0);
  CHECK(strstr(out, "<D:locktoken><D:href>" DAV_LOCK_TOKEN "</D:href></D:locktoken>") != NULL);
  CHECK(strstr(out, "<D:lockscope><D:exclusive/></D:lockscope>") != NULL);
  CHECK(strstr(out, "<D:href>/x</D:href>") != NULL);
}
