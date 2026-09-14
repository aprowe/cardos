/* WebDAV without the socket. See dav.h. */

#include "kernel/net/dav.h"

#include <stdio.h>
#include <string.h>

static const struct { const char *name; DavMethod m; } METHODS[] = {
  { "OPTIONS", DAV_OPTIONS }, { "GET", DAV_GET }, { "HEAD", DAV_HEAD },
  { "PUT", DAV_PUT }, { "DELETE", DAV_DELETE }, { "MKCOL", DAV_MKCOL },
  { "PROPFIND", DAV_PROPFIND }, { "PROPPATCH", DAV_PROPPATCH },
  { "MOVE", DAV_MOVE }, { "COPY", DAV_COPY }, { "LOCK", DAV_LOCK },
  { "UNLOCK", DAV_UNLOCK },
};
#define NMETHODS ((int)(sizeof METHODS / sizeof METHODS[0]))

const char *dav_method_name(DavMethod m) {
  int i;
  for (i = 0; i < NMETHODS; i++) if (METHODS[i].m == m) return METHODS[i].name;
  return "?";
}

int dav_headers_end(const char *buf, size_t n) {
  size_t i;
  for (i = 3; i < n; i++)
    if (buf[i - 3] == '\r' && buf[i - 2] == '\n' && buf[i - 1] == '\r' && buf[i] == '\n')
      return (int)i + 1;
  return -1;
}

static int ieq(const char *a, const char *b, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
    if (x != y) return 0;
  }
  return 1;
}

/* Does the header line at `line` (length n) carry `name`? If so, return the
 * value with surrounding spaces trimmed, else NULL. */
static const char *header(const char *line, size_t n, const char *name,
                          size_t *vlen) {
  size_t nl = strlen(name);
  const char *v, *end = line + n;
  if (n < nl + 1 || line[nl] != ':' || !ieq(line, name, nl)) return NULL;
  v = line + nl + 1;
  while (v < end && (*v == ' ' || *v == '\t')) v++;
  while (end > v && (end[-1] == ' ' || end[-1] == '\t')) end--;
  *vlen = (size_t)(end - v);
  return v;
}

/* dav_decode_path is defined in the next task. */
int dav_decode_path(const char *in, size_t n, char *out, size_t out_size);

int dav_parse(const char *hdr, size_t n, DavRequest *req) {
  const char *p = hdr, *end = hdr + n, *sp1, *sp2, *eol;
  size_t vlen;
  int i, rc;

  memset(req, 0, sizeof *req);
  req->depth = DAV_DEPTH_INFINITY;
  req->keep_alive = 1;
  req->overwrite = 1;

  /* METHOD SP target SP HTTP/x.y CRLF */
  eol = memchr(p, '\n', (size_t)(end - p));
  if (!eol || eol == p || eol[-1] != '\r') { req->bad = 400; return -1; }
  sp1 = memchr(p, ' ', (size_t)(eol - p));
  if (!sp1) { req->bad = 400; return -1; }
  sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
  if (!sp2 || sp2 == sp1 + 1) { req->bad = 400; return -1; }

  req->method = DAV_UNKNOWN;
  for (i = 0; i < NMETHODS; i++) {
    size_t ml = strlen(METHODS[i].name);
    if ((size_t)(sp1 - p) == ml && memcmp(p, METHODS[i].name, ml) == 0) {
      req->method = METHODS[i].m;
      break;
    }
  }

  rc = dav_decode_path(sp1 + 1, (size_t)(sp2 - sp1 - 1), req->path, sizeof req->path);
  if (rc != 0) { req->bad = rc == -2 ? 414 : 403; return -1; }

  /* Headers, one per line, until the blank one. */
  p = eol + 1;
  while (p < end) {
    size_t len;
    const char *v;
    eol = memchr(p, '\n', (size_t)(end - p));
    if (!eol) break;
    len = (size_t)(eol - p);
    if (len && p[len - 1] == '\r') len--;
    if (len == 0) break;

    if ((v = header(p, len, "Content-Length", &vlen)) != NULL) {
      uint32_t cl = 0;
      size_t k;
      for (k = 0; k < vlen && v[k] >= '0' && v[k] <= '9'; k++) {
        uint32_t d = (uint32_t)(v[k] - '0');
        if (cl > (UINT32_MAX - d) / 10) { req->bad = 400; return -1; }
        cl = cl * 10 + d;
      }
      /* Not a digit at all, or not every byte was one: not a number. */
      if (k == 0 || k != vlen) { req->bad = 400; return -1; }
      req->content_length = cl;
      req->has_content_length = 1;
    } else if ((v = header(p, len, "Connection", &vlen)) != NULL) {
      if (vlen == 5 && ieq(v, "close", 5)) req->keep_alive = 0;
    } else if ((v = header(p, len, "Transfer-Encoding", &vlen)) != NULL) {
      if (vlen >= 7 && ieq(v, "chunked", 7)) req->chunked = 1;
    } else if ((v = header(p, len, "Expect", &vlen)) != NULL) {
      if (vlen == 12 && ieq(v, "100-continue", 12)) req->expect_continue = 1;
    } else if ((v = header(p, len, "Depth", &vlen)) != NULL) {
      if (vlen == 1 && v[0] == '0') req->depth = 0;
      else if (vlen == 1 && v[0] == '1') req->depth = 1;
      else req->depth = DAV_DEPTH_INFINITY;
    } else if ((v = header(p, len, "Overwrite", &vlen)) != NULL) {
      if (vlen == 1 && (v[0] == 'F' || v[0] == 'f')) req->overwrite = 0;
    } else if ((v = header(p, len, "Destination", &vlen)) != NULL) {
      rc = dav_decode_path(v, vlen, req->dest, sizeof req->dest);
      if (rc != 0) { req->bad = rc == -2 ? 414 : 403; return -1; }
    }
    p = eol + 1;
  }
  return 0;
}

/* TEMPORARY: replaced in Task 3. */
int dav_decode_path(const char *in, size_t n, char *out, size_t out_size) {
  const char *s = in;
  size_t i, o = 0;
  if (n > 7 && memcmp(s, "http://", 7) == 0) {
    const char *slash = memchr(s + 7, '/', n - 7);
    if (!slash) return -1;
    n -= (size_t)(slash - s); s = slash;
  }
  for (i = 0; i < n && o + 1 < out_size; i++) {
    if (s[i] == '%' && i + 2 < n && s[i + 1] == '2' && s[i + 2] == '0') { out[o++] = ' '; i += 2; }
    else out[o++] = s[i];
  }
  out[o] = 0;
  return 0;
}
