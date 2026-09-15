/* The update manifest. See manifest.h. */

#include "kernel/net/manifest.h"

#include <string.h>

uint32_t manifest_fnv1a(uint32_t h, const uint8_t *data, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

/* ---- a tiny tokenizer, because strtok writes to its input --------------- */

static const char *skip_space(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

/* Copies the next word into `out`; returns where it ended, or NULL if there
 * was no word before the end of line. A word too long for `out` is a NULL
 * too, which is what keeps a 30-character app name from being truncated into
 * a 15-character one that happens to match something. */
static const char *word(const char *p, char *out, size_t cap) {
  size_t n = 0;
  p = skip_space(p);
  if (!*p || *p == '\n' || *p == '\r') return NULL;
  while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
    if (n + 1 >= cap) return NULL;
    out[n++] = *p++;
  }
  out[n] = 0;
  return p;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_hex32(const char *s, uint32_t *out) {
  uint32_t v = 0;
  int n = 0;
  for (; *s; s++, n++) {
    int d = hexval(*s);
    if (d < 0 || n >= 8) return -1;
    v = (v << 4) | (uint32_t)d;
  }
  if (n == 0) return -1;
  *out = v;
  return 0;
}

static int parse_sha(const char *s, uint8_t *out) {
  int i;
  if (strlen(s) != 64) return -1;
  for (i = 0; i < 32; i++) {
    int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

static int parse_dec(const char *s, uint32_t *out) {
  uint32_t v = 0;
  if (!*s) return -1;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return -1;
    v = v * 10 + (uint32_t)(*s - '0');
  }
  *out = v;
  return 0;
}

static const char *next_line(const char *p) {
  while (*p && *p != '\n') p++;
  return *p ? p + 1 : p;
}

/* A name becomes NAME.capp under /desktop, so it is letters, digits, `-`
 * and `_` and nothing that could be a path: not a slash, not a dot. */
static int name_ok(const char *n) {
  if (!*n) return 0;
  for (; *n; n++) {
    char c = *n;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_') continue;
    return 0;
  }
  return 1;
}

int manifest_parse(const char *text, Manifest *out) {
  const char *p = text;
  int understood = 0;

  memset(out, 0, sizeof *out);

  while (*p) {
    char kind[12], a[72], b[24], c[24];
    const char *q;

    q = word(p, kind, sizeof kind);
    if (!q) { p = next_line(p); continue; }

    if (!strcmp(kind, "firmware")) {
      uint32_t size;
      if ((q = word(q, a, sizeof a)) && (q = word(q, b, sizeof b)) &&
          parse_sha(a, out->firmware_sha) == 0 && parse_dec(b, &size) == 0) {
        out->has_firmware = 1;
        out->firmware_size = size;
        understood++;
      }
    } else if (!strcmp(kind, "app") && out->napps < MANIFEST_MAX_APPS) {
      ManifestApp *ap = &out->app[out->napps];
      uint32_t hash, size;
      if ((q = word(q, ap->name, sizeof ap->name)) && name_ok(ap->name) &&
          (q = word(q, b, sizeof b)) && (q = word(q, c, sizeof c)) &&
          parse_hex32(b, &hash) == 0 && parse_dec(c, &size) == 0) {
        ap->hash = hash;
        ap->size = size;
        out->napps++;
        understood++;
      } else {
        ap->name[0] = 0;
      }
    }
    p = next_line(p);
  }
  return understood ? understood : -1;
}

int manifest_diff(const Manifest *m, const ManifestLocal *local,
                  int *stale_app) {
  int i;
  for (i = 0; i < m->napps; i++)
    stale_app[i] = !local->have_app[i] || local->app_hash[i] != m->app[i].hash;
  if (!m->has_firmware) return 0;
  if (!local->own_sha) return 1;
  return memcmp(local->own_sha, m->firmware_sha, 32) != 0;
}
