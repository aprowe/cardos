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

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int dav_decode_path(const char *in, size_t n, char *out, size_t out_size) {
  char raw[FS_PATH_MAX];
  size_t i, o = 0;

  /* A Destination may be absolute: skip scheme and host. */
  if (n > 7 && memcmp(in, "http://", 7) == 0) {
    const char *slash = memchr(in + 7, '/', n - 7);
    if (!slash) return -1;
    n -= (size_t)(slash - in);
    in = slash;
  }
  if (n == 0 || in[0] != '/') return -1;

  for (i = 0; i < n; i++) {
    char c = in[i];
    if (c == '%') {
      int hi, lo;
      if (i + 2 >= n) return -1;
      hi = hexval(in[i + 1]);
      lo = hexval(in[i + 2]);
      if (hi < 0 || lo < 0) return -1;
      c = (char)(hi * 16 + lo);
      i += 2;
    } else if (c == '?' || c == '#') {
      return -1;                       /* a file has no query string */
    }
    if (c == '\0' || c == '\\' || (unsigned char)c < 0x20 || c == 0x7f) return -1;
    if (o + 1 >= sizeof raw) return -2;
    raw[o++] = c;
  }
  raw[o] = '\0';

  /* Traversal: refuse rather than resolve. path_normalize would clamp ".."
   * at the root, and a client that sends ".." is not one to be helpful to. */
  {
    const char *s = raw;
    while (*s) {
      const char *seg = s, *e = strchr(s, '/');
      size_t len = e ? (size_t)(e - seg) : strlen(seg);
      if (len == 2 && seg[0] == '.' && seg[1] == '.') return -1;
      s = e ? e + 1 : seg + len;
    }
  }
  if (path_normalize(raw, out, out_size) != 0) return -2;
  return 0;
}

static int unreserved(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}

int dav_encode_path(const char *path, int is_dir, char *out, size_t out_size) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t o = 0;
  const unsigned char *p = (const unsigned char *)path;

  for (; *p; p++) {
    if (*p == '/' || unreserved(*p)) {
      if (o + 1 >= out_size) return -1;
      out[o++] = (char)*p;
    } else {
      if (o + 3 >= out_size) return -1;
      out[o++] = '%';
      out[o++] = HEX[*p >> 4];
      out[o++] = HEX[*p & 15];
    }
  }
  if (is_dir && !(o == 1 && out[0] == '/')) {
    if (o + 1 >= out_size) return -1;
    out[o++] = '/';
  }
  out[o] = '\0';
  return (int)o;
}

/* Civil date from days since 1970-01-01 (Howard Hinnant's algorithm), so
 * there is no dependence on libc's gmtime -- the device's newlib is built
 * with a zone, and the host's is not. */
static void civil(uint32_t days, int *y, int *m, int *d) {
  int64_t z = (int64_t)days + 719468;
  int64_t era = z / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2));
}

static const char *const DAYS[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
static const char *const MONS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

void dav_http_date(uint32_t epoch, char *out, size_t out_size) {
  uint32_t days = epoch / 86400u, secs = epoch % 86400u;
  int y, m, d;
  civil(days, &y, &m, &d);
  snprintf(out, out_size, "%s, %02d %s %04d %02u:%02u:%02u GMT",
           DAYS[days % 7], d, MONS[m - 1], y,
           (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}

void dav_iso_date(uint32_t epoch, char *out, size_t out_size) {
  uint32_t days = epoch / 86400u, secs = epoch % 86400u;
  int y, m, d;
  civil(days, &y, &m, &d);
  snprintf(out, out_size, "%04d-%02d-%02dT%02u:%02u:%02uZ", y, m, d,
           (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}
