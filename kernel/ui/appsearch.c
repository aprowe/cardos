/* Finding an app by name. See appsearch.h. */
#include "kernel/ui/appsearch.h"

#include <string.h>

static char low(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int starts_ci(const char *s, const char *q) {
  while (*q) if (low(*s++) != low(*q++)) return 0;
  return 1;
}

int appsearch_score(const char *name, const char *query) {
  int n = (int)strlen(name), i, gaps = 0, last = -1;
  const char *q;
  if (!*query) return 0;
  if (starts_ci(name, query)) return 1000 - n;               /* shorter first */
  for (i = 1; i < n; i++)
    if ((name[i - 1] == ' ' || name[i - 1] == '-' || name[i - 1] == '_') &&
        starts_ci(name + i, query))
      return 800 - i;
  for (i = 1; i < n; i++)
    if (starts_ci(name + i, query)) return 600 - i;
  /* The letters in order, anywhere: the fewer letters skipped, the better. */
  for (q = query, i = 0; *q && i < n; i++) {
    if (low(name[i]) != low(*q)) continue;
    if (last >= 0) gaps += i - last - 1;
    last = i;
    q++;
  }
  if (*q) return -1;
  return 300 - gaps;
}

int appsearch_rank(const char *const *names, int n, const char *query, int *out, int max) {
  int i, k, j, count = 0, sc[64];
  if (max > 64) max = 64;
  for (i = 0; i < n; i++) {
    int s = appsearch_score(names[i], query);
    if (s < 0) continue;
    /* Insertion into a short sorted list: stable, so ties keep the
     * launcher's order. */
    for (k = count; k > 0 && sc[k - 1] < s; k--) ;
    if (k >= max) continue;
    if (count < max) count++;
    for (j = count - 1; j > k; j--) { out[j] = out[j - 1]; sc[j] = sc[j - 1]; }
    out[k] = i;
    sc[k] = s;
  }
  return count;
}
