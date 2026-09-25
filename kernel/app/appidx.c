/* The app index. See appidx.h. */

#include "kernel/app/appidx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char HEX[] = "0123456789abcdef";

int appidx_header(char *out, size_t size, const char *key) {
  int n = snprintf(out, size, "appidx %s\n", key);
  if (n < 0 || (size_t)n >= size) { if (size) out[0] = 0; return -1; }
  return n;
}

/* Append `s` to out at *len, if it fits. */
static int put(char *out, size_t size, size_t *len, const char *s, size_t n) {
  if (*len + n >= size) return -1;
  memcpy(out + *len, s, n);
  *len += n;
  out[*len] = 0;
  return 0;
}

int appidx_record(char *out, size_t size, const AppIdxRec *r, const char *cmds) {
  char head[64], icon[APPIDX_ICON_BYTES * 2], name[APPIDX_NAME_MAX];
  size_t len = 0;
  int i, n;
  const char *line;

  if (!size) return -1;
  out[0] = 0;
  n = snprintf(head, sizeof head, "A\t%lu\t%lu\t%u\t",
               (unsigned long)r->size, (unsigned long)r->mtime, (unsigned)r->flags);
  for (i = 0; i < APPIDX_ICON_BYTES; i++) {
    icon[i * 2]     = HEX[r->icon[i] >> 4];
    icon[i * 2 + 1] = HEX[r->icon[i] & 15];
  }
  /* The name is the app's to choose; it must not be able to end the line or
   * add a field to it. */
  for (i = 0; i < APPIDX_NAME_MAX - 1 && r->name[i]; i++)
    name[i] = (r->name[i] == '\t' || r->name[i] == '\n' || r->name[i] == '\r')
              ? ' ' : r->name[i];
  name[i] = 0;

  if (put(out, size, &len, head, (size_t)n) ||
      put(out, size, &len, icon, sizeof icon) ||
      put(out, size, &len, "\t", 1) ||
      put(out, size, &len, r->path, strlen(r->path)) ||
      put(out, size, &len, "\t", 1) ||
      put(out, size, &len, name, strlen(name)) ||
      put(out, size, &len, "\n", 1))
    goto full;

  for (line = cmds ? cmds : ""; *line; ) {
    size_t ln = strcspn(line, "\n");
    if (put(out, size, &len, "C\t", 2) ||
        put(out, size, &len, line, ln) ||
        put(out, size, &len, "\n", 1))
      goto full;
    line += ln;
    if (*line) line++;
  }
  if (put(out, size, &len, "E\n", 2)) goto full;
  return (int)len;

full:
  out[0] = 0;
  return -1;
}

const char *appidx_body(const char *text, const char *key) {
  size_t k = strlen(key);
  if (strncmp(text, "appidx ", 7) != 0) return NULL;
  if (strncmp(text + 7, key, k) != 0 || text[7 + k] != '\n') return NULL;
  return text + 7 + k + 1;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

/* One unsigned field ending in a tab. */
static int field_u(const char **p, unsigned long *out) {
  char *end;
  *out = strtoul(*p, &end, 10);
  if (end == *p || *end != '\t') return -1;
  *p = end + 1;
  return 0;
}

/* The A line at `p` (just past "A\t"), into *r. -1 if it is not one. */
static int parse_head(const char *p, AppIdxRec *r) {
  unsigned long size, mtime, flags;
  const char *tab, *eol;
  size_t n;
  int i;

  if (field_u(&p, &size) || field_u(&p, &mtime) || field_u(&p, &flags)) return -1;
  for (i = 0; i < APPIDX_ICON_BYTES; i++) {
    int hi = hexval(p[i * 2]), lo = hi < 0 ? -1 : hexval(p[i * 2 + 1]);
    if (lo < 0) return -1;
    r->icon[i] = (uint8_t)(hi << 4 | lo);
  }
  p += APPIDX_ICON_BYTES * 2;
  if (*p++ != '\t') return -1;

  tab = strchr(p, '\t');
  eol = strchr(p, '\n');
  if (!tab || !eol || tab > eol) return -1;
  n = (size_t)(tab - p);
  if (n >= sizeof r->path) return -1;
  memcpy(r->path, p, n);
  r->path[n] = 0;

  p = tab + 1;
  n = (size_t)(eol - p);
  if (n >= sizeof r->name) n = sizeof r->name - 1;
  memcpy(r->name, p, n);
  r->name[n] = 0;

  r->size = (uint32_t)size;
  r->mtime = (uint32_t)mtime;
  r->flags = (uint16_t)flags;
  return 0;
}

int appidx_find(const char *body, const char *path, uint32_t size, uint32_t mtime,
                AppIdxRec *out, char *cmds, size_t cmds_size) {
  const char *p = body;

  if (!body || !mtime || !cmds_size) return 0;
  while (*p) {
    const char *eol = strchr(p, '\n');
    if (!eol) return 0;                          /* torn: the last line has no end */
    if (p[0] == 'A' && p[1] == '\t' && parse_head(p + 2, out) == 0 &&
        !strcmp(out->path, path)) {
      size_t len = 0;
      if (out->size != size || out->mtime != mtime) return 0;
      cmds[0] = 0;
      for (p = eol + 1; ; p = eol + 1) {
        eol = strchr(p, '\n');
        if (!eol) return 0;                      /* no E: cut short */
        if (p[0] == 'E' && p + 1 == eol) return 1;
        if (p[0] != 'C' || p[1] != '\t') return 0;
        if (len + (size_t)(eol - p - 2) + 1 >= cmds_size) return 0;
        memcpy(cmds + len, p + 2, (size_t)(eol - p - 2));
        len += (size_t)(eol - p - 2);
        cmds[len++] = '\n';
        cmds[len] = 0;
      }
    }
    p = eol + 1;
  }
  return 0;
}
