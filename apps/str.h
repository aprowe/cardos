/* The small string and rectangle helpers every app wants.
 *
 * An app links no libc, and the API offers only mem_set, mem_cpy, mem_move,
 * str_len and fmt. So every app grew its own: same() nine times, rect()
 * thirty-seven, three url_enc that disagreed on what to escape (and Web,
 * which needed one most, had none). These are those, once.
 *
 * Header-only, static helpers, the same arrangement as apps/safefile.h;
 * an app that does not call one does not carry it.
 */
#ifndef CARDOS_STR_H
#define CARDOS_STR_H

#include "kernel/app/capp.h"

#if defined(__GNUC__)
#define STR_OPT __attribute__((unused))
#else
#define STR_OPT
#endif

/* ---- strings ------------------------------------------------------------- */

static STR_OPT int str_same(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

/* Does `s` begin with `p`, exactly? */
static STR_OPT int str_starts(const char *s, const char *p) {
  while (*p) { if (*s != *p) return 0; s++; p++; }
  return 1;
}

static STR_OPT char str_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* Is `needle` somewhere in `hay`? With `fold`, ignoring ASCII case. An empty
 * needle is in everything. */
static STR_OPT int str_contains(const char *hay, const char *needle, int fold) {
  int i, j;
  if (!*needle) return 1;
  for (i = 0; hay[i]; i++) {
    for (j = 0; needle[j]; j++) {
      char a = hay[i + j], b = needle[j];
      if (fold) { a = str_lower(a); b = str_lower(b); }
      if (a != b) break;
    }
    if (!needle[j]) return 1;
    if (!hay[i + j]) return 0;          /* the rest of hay is too short */
  }
  return 0;
}

/* The decimal digits at *p, and *p moved past them. 0 when there are none. */
static STR_OPT uint32_t str_uint(const char **p) {
  uint32_t v = 0;
  const char *s = *p;
  while (*s >= '0' && *s <= '9') v = v * 10u + (uint32_t)(*s++ - '0');
  *p = s;
  return v;
}

/* ---- the server's replies: tab-separated lines ---------------------------- */

/* Field `k` (from 0) of the tab-separated line at `line` into `out`: up to
 * the next tab or the end of the line, cut to fit. */
static STR_OPT void tsv_field(const char *line, int k, char *out, int n) {
  int i = 0;
  while (k > 0 && *line && *line != '\n') { if (*line++ == '\t') k--; }
  while (*line && *line != '\t' && *line != '\n' && i < n - 1) out[i++] = *line++;
  out[i] = 0;
}

/* The start of the line after the one at `p`, or the end of the text. */
static STR_OPT const char *tsv_next_line(const char *p) {
  while (*p && *p != '\n') p++;
  return *p ? p + 1 : p;
}

/* ---- URLs ---------------------------------------------------------------- */

/* `s` as a query value: everything but RFC 3986's unreserved characters
 * (A-Z a-z 0-9 - . _ ~) as %XX, so an & or a space in it stays in it. Cut
 * at a whole character to fit `n`; the length written. */
static STR_OPT int url_enc(char *out, int n, const char *s) {
  static const char HEX[] = "0123456789ABCDEF";
  int k = 0;
  if (n <= 0) return 0;
  for (; *s; s++) {
    unsigned char ch = (unsigned char)*s;
    int plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '.' || ch == '_' || ch == '~';
    if (k + (plain ? 1 : 3) > n - 1) break;
    if (plain) out[k++] = (char)ch;
    else { out[k++] = '%'; out[k++] = HEX[ch >> 4]; out[k++] = HEX[ch & 15]; }
  }
  out[k] = 0;
  return k;
}

/* ---- rectangles ---------------------------------------------------------- */

/* A CRect, with a negative width or height taken as nothing. */
static STR_OPT CRect capp_rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y;
  r.w = (int16_t)(w > 0 ? w : 0); r.h = (int16_t)(h > 0 ? h : 0);
  return r;
}

static STR_OPT int capp_overlaps(CRect a, CRect b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

#endif /* CARDOS_STR_H */
