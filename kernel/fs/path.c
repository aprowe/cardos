/* Path handling. See path.h. */

#include "kernel/fs/path.h"

#include <string.h>

int path_is_absolute(const char *p) {
  return (p && p[0] == '/') ? 1 : 0;
}

int path_normalize(const char *in, char *out, size_t out_size) {
  size_t len = 0;
  const char *p;

  if (!in || !out || out_size < 2) return -1;
  if (!path_is_absolute(in)) return -1;

  out[0] = '/';
  len = 1;

  p = in;
  while (*p) {
    const char *start;
    size_t n;

    while (*p == '/') p++;                 /* collapse separators */
    if (!*p) break;

    start = p;
    while (*p && *p != '/') p++;
    n = (size_t)(p - start);

    if (n == 1 && start[0] == '.') continue;

    if (n == 2 && start[0] == '.' && start[1] == '.') {
      /* Walk back one component. At the root there is nowhere to go, and
       * silently staying put is the only sensible answer. */
      while (len > 1 && out[len - 1] != '/') len--;
      if (len > 1) len--;                  /* drop the separator too */
      continue;
    }

    /* Need a separator (unless we are right after the root) plus the
     * component plus the NUL. */
    {
      size_t need = n + 1 + (len > 1 ? 1 : 0);
      if (len + need > out_size) return -1;
    }
    if (len > 1) out[len++] = '/';
    memcpy(out + len, start, n);
    len += n;
  }

  if (len == 0) { out[0] = '/'; len = 1; }
  out[len] = '\0';
  return 0;
}

int path_resolve(const char *cwd, const char *rel, char *out, size_t out_size) {
  char joined[FS_PATH_MAX * 2];
  size_t cl, rl;

  if (!cwd || !rel || !out) return -1;

  if (path_is_absolute(rel)) return path_normalize(rel, out, out_size);
  if (!path_is_absolute(cwd)) return -1;

  cl = strlen(cwd);
  rl = strlen(rel);
  if (rl == 0) return path_normalize(cwd, out, out_size);
  if (cl + 1 + rl + 1 > sizeof joined) return -1;

  memcpy(joined, cwd, cl);
  if (cl == 0 || joined[cl - 1] != '/') joined[cl++] = '/';
  memcpy(joined + cl, rel, rl);
  joined[cl + rl] = '\0';

  return path_normalize(joined, out, out_size);
}

const char *path_basename(const char *path) {
  const char *slash;
  if (!path) return "";
  slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

int path_dirname(const char *path, char *out, size_t out_size) {
  const char *slash;
  size_t n;

  if (!path || !out || out_size < 2) return -1;
  slash = strrchr(path, '/');
  if (!slash) return -1;

  n = (size_t)(slash - path);
  if (n == 0) {                            /* "/a" -> "/" */
    if (out_size < 2) return -1;
    out[0] = '/';
    out[1] = '\0';
    return 0;
  }
  if (n + 1 > out_size) return -1;
  memcpy(out, path, n);
  out[n] = '\0';
  return 0;
}
