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

/* What was opened lately, newest first, and what is open now -- the
 * launcher's /config/recent.txt. An empty search lists these first; and
 * with Settings > Open last app on, boot reopens `running`. */
#define RECENT_MAX  8
#define RECENT_NAME 24

typedef struct {
  char name[RECENT_MAX][RECENT_NAME];
  int  n;
  char running[RECENT_NAME];      /* "" when the row is on screen */
} Recents;

void recents_opened(Recents *r, const char *name);   /* to the front, once; and running */
void recents_closed(Recents *r);                      /* nothing running */
/* The file: "@ NAME" for what is running, then a name a line, newest first. */
void recents_parse(Recents *r, const char *text);
int  recents_format(const Recents *r, char *out, int n);

/* appsearch_rank, but an empty query puts the recent ones first, newest
 * first, then the rest in their order. `r` may be NULL. */
int appsearch_rank_recent(const char *const *names, int n, const char *query,
                          const Recents *r, int *out, int max);

#endif /* CARDOS_APPSEARCH_H */
