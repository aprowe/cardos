/* Frames on the serial line. See serframe.h. */
#include "kernel/sys/serframe.h"

#include <string.h>

void sf_reader_init(SfReader *r, char *buf, int cap) {
  r->buf = buf;
  r->cap = cap;
  r->len = r->in = r->over = 0;
  if (buf && cap) buf[0] = 0;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* The body ends in TAB and eight hex digits: check them and cut them off. */
static int check_crc(SfReader *r) {
  uint32_t want = 0;
  int i, v;
  if (r->len < 10 || r->buf[r->len - 9] != '\t') return 0;
  for (i = r->len - 8; i < r->len; i++) {
    if ((v = hexval(r->buf[i])) < 0) return 0;
    want = want << 4 | (uint32_t)v;
  }
  if (sf_crc32(r->buf, (size_t)(r->len - 9)) != want) return 0;
  r->len -= 9;
  r->buf[r->len] = 0;
  return 1;
}

int sf_feed(SfReader *r, uint8_t c) {
  if (c == SF_STX) {
    r->in = 1;
    r->len = r->over = 0;
    return SF_NONE;
  }
  if (!r->in) return SF_KEY;
  if (c == SF_ETX) {
    r->in = 0;
    r->buf[r->len < r->cap ? r->len : r->cap - 1] = 0;
    if (r->over || !check_crc(r)) { r->len = 0; r->buf[0] = 0; return SF_BAD; }
    return SF_FRAME;
  }
  if (r->len + 1 >= r->cap) { r->over = 1; return SF_NONE; }
  r->buf[r->len++] = (char)c;
  return SF_NONE;
}

uint32_t sf_crc32(const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t crc = 0xFFFFFFFFu;
  int k;
  while (n--) {
    crc ^= *p++;
    for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sf_b64_encode(const uint8_t *in, int n, char *out, int cap) {
  int i, o = 0;
  if (((n + 2) / 3) * 4 + 1 > cap) return -1;
  for (i = 0; i + 2 < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
    out[o++] = B64[v >> 18 & 63]; out[o++] = B64[v >> 12 & 63];
    out[o++] = B64[v >> 6 & 63];  out[o++] = B64[v & 63];
  }
  if (i < n) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
    out[o++] = B64[v >> 18 & 63]; out[o++] = B64[v >> 12 & 63];
    out[o++] = i + 1 < n ? B64[v >> 6 & 63] : '=';
    out[o++] = '=';
  }
  out[o] = 0;
  return o;
}

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int sf_b64_decode(const char *in, int n, uint8_t *out, int cap) {
  uint32_t acc = 0;
  int bits = 0, o = 0, i, v;
  for (i = 0; i < n; i++) {
    if (in[i] == '=') break;
    if ((v = b64val(in[i])) < 0) return -1;
    acc = acc << 6 | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  return o;
}

int sf_split(char *body, char **field, int max) {
  int n = 0;
  char *p = body;
  if (max <= 0) return 0;
  field[n++] = p;
  while (*p && n < max) {
    if (*p == '\t') { *p = 0; field[n++] = p + 1; }
    p++;
  }
  return n;
}

int sf_reply(char *out, int cap, const char *id, const char *status,
             const uint8_t *data, int n) {
  int o = 0, k, body;
  size_t idn = strlen(id), stn = strlen(status);
  if ((int)(idn + stn) + 4 + ((n + 2) / 3) * 4 + 13 > cap) return -1;
  out[o++] = SF_STX;
  body = o;
  out[o++] = '@';
  memcpy(out + o, id, idn); o += (int)idn;
  out[o++] = '\t';
  memcpy(out + o, status, stn); o += (int)stn;
  out[o++] = '\t';
  if ((k = sf_b64_encode(data, n, out + o, cap - o)) < 0) return -1;
  o += k;
  {
    uint32_t crc = sf_crc32(out + body, (size_t)(o - body));
    static const char HEX[] = "0123456789abcdef";
    out[o++] = '\t';
    for (k = 7; k >= 0; k--) out[o++] = HEX[crc >> (k * 4) & 15];
  }
  out[o++] = SF_ETX;
  out[o++] = '\n';
  out[o] = 0;
  return o;
}
