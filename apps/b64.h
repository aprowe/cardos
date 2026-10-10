/* Base64, both ways (RFC 4648, the standard alphabet, with = padding).
 *
 * An app's HTTP is text, and a Jar Factory item record is bytes: the server
 * hands each one over as one line of base64, and a gift goes back the same
 * way. Header-only and libc-free like apps/str.h; host-tested in
 * test/test_b64.c.
 */
#ifndef CARDOS_B64_H
#define CARDOS_B64_H

#include <stdint.h>

#if defined(__GNUC__)
#define B64_OPT __attribute__((unused))
#else
#define B64_OPT
#endif

/* The characters `n` bytes encode to, not counting a NUL. */
#define B64_LEN(n) ((((n) + 2) / 3) * 4)

/* `n` bytes of `in` into `out` as base64, NUL-terminated. The length, or -1
 * if it does not fit in `cap` (which must have room for the NUL). */
static B64_OPT int b64_encode(const uint8_t *in, int n, char *out, int cap) {
  static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int i, k = 0;
  if (cap < B64_LEN(n) + 1) return -1;
  for (i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[k++] = A[(v >> 18) & 63];
    out[k++] = A[(v >> 12) & 63];
    out[k++] = i + 1 < n ? A[(v >> 6) & 63] : '=';
    out[k++] = i + 2 < n ? A[v & 63] : '=';
  }
  out[k] = 0;
  return k;
}

static B64_OPT int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;        /* the URL-safe spellings too */
  if (c == '/' || c == '_') return 63;
  return -1;
}

/* Base64 at `in` into bytes, stopping at the end of the line (a newline, a
 * tab or a NUL); a \r, spaces and padding are skipped. The bytes written, or
 * -1 if a character is not base64, the length is impossible, or the bytes do
 * not fit in `cap`. */
static B64_OPT int b64_decode(const char *in, uint8_t *out, int cap) {
  uint32_t acc = 0;
  int bits = 0, k = 0, seen = 0;
  for (; *in && *in != '\n' && *in != '\t'; in++) {
    int v;
    if (*in == '\r' || *in == ' ' || *in == '=') continue;
    v = b64_val(*in);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    seen++;
    if (bits >= 8) {
      bits -= 8;
      if (k >= cap) return -1;
      out[k++] = (uint8_t)(acc >> bits);
    }
  }
  if (seen % 4 == 1) return -1;               /* six bits cannot end a byte */
  return k;
}

#endif /* CARDOS_B64_H */
