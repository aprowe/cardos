/* Finding an app by name. See appsearch.h. */
#include "kernel/ui/appsearch.h"

#include <stdio.h>
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

static int same_ci(const char *a, const char *b) {
  while (*a && *b) if (low(*a++) != low(*b++)) return 0;
  return *a == *b;
}

/* With nothing typed, how recently it was opened: newest highest. */
static int score_for(const char *name, const char *query, const Recents *r) {
  int i;
  if (*query || !r) return appsearch_score(name, query);
  for (i = 0; i < r->n; i++) if (same_ci(r->name[i], name)) return 100 - i;
  return 0;
}

int appsearch_rank(const char *const *names, int n, const char *query, int *out, int max) {
  return appsearch_rank_recent(names, n, query, NULL, out, max);
}

int appsearch_rank_recent(const char *const *names, int n, const char *query,
                          const Recents *r, int *out, int max) {
  int i, k, j, count = 0, sc[64];
  if (max > 64) max = 64;
  for (i = 0; i < n; i++) {
    int s = score_for(names[i], query, r);
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

static void copy_name(char *d, const char *s) {
  int i = 0;
  while (s[i] && s[i] != '\n' && s[i] != '\r' && i < RECENT_NAME - 1) { d[i] = s[i]; i++; }
  d[i] = 0;
}

void recents_opened(Recents *r, const char *name) {
  int i, at = r->n;
  char one[RECENT_NAME];
  if (!name || !*name) return;
  copy_name(one, name);
  for (i = 0; i < r->n; i++) if (same_ci(r->name[i], one)) { at = i; break; }
  if (at == r->n && r->n < RECENT_MAX) r->n++;
  if (at >= RECENT_MAX) at = RECENT_MAX - 1;          /* full: the oldest goes */
  for (i = at; i > 0; i--) memcpy(r->name[i], r->name[i - 1], RECENT_NAME);
  memcpy(r->name[0], one, RECENT_NAME);
  memcpy(r->running, one, RECENT_NAME);
}

void recents_closed(Recents *r) { r->running[0] = 0; }

void recents_parse(Recents *r, const char *t) {
  memset(r, 0, sizeof *r);
  while (*t) {
    if (t[0] == '@' && t[1] == ' ') copy_name(r->running, t + 2);
    else if (*t != '\n' && *t != '\r' && *t != '#' && r->n < RECENT_MAX) copy_name(r->name[r->n++], t);
    while (*t && *t != '\n') t++;
    if (*t) t++;
  }
}

int recents_format(const Recents *r, char *out, int n) {
  int o = 0, i;
  out[0] = 0;
  if (r->running[0]) o += snprintf(out + o, (size_t)(n - o), "@ %s\n", r->running);
  for (i = 0; i < r->n && o < n; i++) o += snprintf(out + o, (size_t)(n - o), "%s\n", r->name[i]);
  return o < n ? o : n - 1;
}
