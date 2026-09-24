/* key=value lines. See kvtext.h. */

#include "kernel/sys/kvtext.h"

#include <string.h>

#define BS '\\'

static void copy(char *out, size_t size, const char *s, size_t n) {
  if (!size) return;
  if (n > size - 1) n = size - 1;
  memcpy(out, s, n);
  out[n] = 0;
}

/* The value, with a backslash-n and a doubled backslash turned back into a
 * newline and a backslash -- the two escapes kv_put writes. */
static void unescape(char *out, size_t size, const char *s, size_t n) {
  size_t i, o = 0;
  if (!size) return;
  for (i = 0; i < n && o + 1 < size; i++) {
    if (s[i] == BS && i + 1 < n && (s[i + 1] == 'n' || s[i + 1] == BS)) {
      out[o++] = s[i + 1] == 'n' ? '\n' : BS;
      i++;
    } else {
      out[o++] = s[i];
    }
  }
  out[o] = 0;
}

int kv_next(const char **pos, char *key, size_t ksize, char *value, size_t vsize) {
  const char *p = *pos;
  while (*p) {
    const char *line = p, *end, *eq, *ks, *ke, *vs, *ve;
    while (*p && *p != '\n') p++;
    end = p;
    if (*p == '\n') p++;
    *pos = p;

    ks = line;
    while (ks < end && (*ks == ' ' || *ks == '\t')) ks++;
    if (ks == end || *ks == '#' || *ks == '\r') continue;
    for (eq = ks; eq < end && *eq != '='; eq++) {}
    if (eq == end) continue;                         /* no '=': not a pair */
    ke = eq;
    while (ke > ks && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
    if (ke == ks) continue;                          /* no key */
    vs = eq + 1;
    while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;
    ve = end;
    while (ve > vs && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) ve--;
    copy(key, ksize, ks, (size_t)(ke - ks));
    unescape(value, vsize, vs, (size_t)(ve - vs));
    return 1;
  }
  *pos = p;
  return 0;
}

/* A newline in a value would end its line, so it goes out as backslash-n,
 * and a backslash as two -- the launcher's pins are names one a line. */
int kv_put(char *out, size_t size, size_t *len, const char *key, const char *value) {
  size_t k = strlen(key), v = 0, i, at;
  for (i = 0; value[i]; i++) v += (value[i] == '\n' || value[i] == BS) ? 2 : 1;
  if (*len + k + 1 + v + 1 + 1 > size) return -1;
  at = *len;
  memcpy(out + at, key, k);
  at += k;
  out[at++] = '=';
  for (i = 0; value[i]; i++) {
    if (value[i] == '\n')    { out[at++] = BS; out[at++] = 'n'; }
    else if (value[i] == BS) { out[at++] = BS; out[at++] = BS; }
    else out[at++] = value[i];
  }
  out[at++] = '\n';
  out[at] = 0;
  *len = at;
  return 0;
}
