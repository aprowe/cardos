/* The launcher's order. See iconorder.h. */

#include "kernel/ui/iconorder.h"

#include <string.h>

const char ICONORDER_DEFAULT[] =
  "# The launcher shows these first, in this order -- apps or folders, one a\n"
  "# line. A favourite app in a folder appears at the top level too.\n"
  "# Everything else follows: folders, then apps, A-Z.\n"
  "Todo\n"
  "Calendar\n"
  "Today\n"
  "Toggl\n"
  "Plan\n"
  "Make\n"
  "Net\n"
  "Games\n"
  "System\n";

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int cmp_ci(const char *a, const char *b) {
  while (*a && lower(*a) == lower(*b)) { a++; b++; }
  return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}

void iconorder_parse(const char *text, IconFavs *out) {
  const char *p = text ? text : "";
  memset(out, 0, sizeof *out);
  while (*p && out->n < ICONORDER_FAVS_MAX) {
    const char *e = p, *s = p;
    int k = 0;
    while (*e && *e != '\n') e++;
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    if (s < e && *s != '#') {
      char *d = out->name[out->n];
      while (s < e && k < ICONORDER_NAME_MAX - 1) d[k++] = *s++;
      d[k] = 0;
      out->n++;
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
}

int iconorder_rank(const IconFavs *f, const char *name) {
  int i;
  if (!f || !name) return -1;
  for (i = 0; i < f->n; i++)
    if (cmp_ci(f->name[i], name) == 0) return i;
  return -1;
}

/* Before b: favourites by rank, then folders, then the rest; A-Z within. */
static int before(int a, int b, const char *const *names, const int *is_folder,
                  const IconFavs *f) {
  int ra = iconorder_rank(f, names[a]), rb = iconorder_rank(f, names[b]);
  if (ra >= 0 || rb >= 0) {
    if (ra < 0) return 0;
    if (rb < 0) return 1;
    return ra < rb;
  }
  if (!is_folder[a] != !is_folder[b]) return is_folder[a] != 0;
  return cmp_ci(names[a], names[b]) < 0;
}

/* Insertion sort: tens of entries, stable, and no allocation. */
void iconorder_sort(int *idx, int n, const char *const *names, const int *is_folder,
                    const IconFavs *f) {
  int i, j;
  for (i = 1; i < n; i++) {
    int v = idx[i];
    for (j = i; j > 0 && before(v, idx[j - 1], names, is_folder, f); j--) idx[j] = idx[j - 1];
    idx[j] = v;
  }
}
