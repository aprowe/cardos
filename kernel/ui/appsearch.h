/* Finding an app by typing part of its name: the launcher's search (Space).
 *
 * Ranked the way a person expects of a launcher: the start of the name
 * ("no" -> Notes), then the start of a word in it ("link" -> Dashboard
 * Link), then anywhere in it, then the letters in order with gaps ("dl" ->
 * Dashboard Link). Ties keep the order they came in, which is the
 * launcher's own -- favourites first. Case does not matter.
 *
 * Portable; the host suite tests it. kernel/ui/launchui.c draws it.
 */
#ifndef CARDOS_APPSEARCH_H
#define CARDOS_APPSEARCH_H

/* How well `query` finds `name`: higher is better, -1 not at all. An empty
 * query finds everything, equally. */
int appsearch_score(const char *name, const char *query);

/* The indices of `names` that match, best first, at most `max` of them.
 * Returns how many. */
int appsearch_rank(const char *const *names, int n, const char *query, int *out, int max);

#endif /* CARDOS_APPSEARCH_H */
