/* One value per line. See conf.h. */

#include "kernel/sys/conf.h"

#include <stdio.h>
#include <string.h>

int conf_split(const char *text, char *lines, int n, size_t width) {
  int i, filled = 0;
  const char *p = text ? text : "";

  for (i = 0; i < n; i++) {
    char *out = lines + (size_t)i * width;
    const char *end;
    size_t len;
    out[0] = 0;
    if (!*p) continue;
    end = strchr(p, '\n');
    len = end ? (size_t)(end - p) : strlen(p);
    while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
    if (len >= width) len = width - 1;
    memcpy(out, p, len);
    out[len] = 0;
    if (len) filled = i + 1;
    p = end ? end + 1 : p + strlen(p);
  }
  return filled;
}

int conf_join(const char *const *values, int n, char *out, size_t size) {
  int i;
  size_t used = 0;
  for (i = 0; i < n; i++) {
    const char *v = values[i] ? values[i] : "";
    size_t len = strlen(v);
    if (used + len + 2 > size) return -1;
    memcpy(out + used, v, len);
    used += len;
    out[used++] = '\n';
  }
  out[used] = 0;
  return (int)used;
}
