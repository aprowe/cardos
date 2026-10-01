/* The launcher's order: favourites first, then folders, then apps, A-Z.
 *
 * The card's directory order is the order files were written, which after a
 * few updates is no order at all. /config/favorites.txt names what should
 * come first -- apps or folders, one a line, in order:
 *
 *     Todo
 *     Calendar
 *     Plan
 *
 * A favourite app that lives in a folder is shown at the top level as well,
 * so the apps used most are one press from the launcher. Everything not
 * named follows, folders before apps, each A-Z ignoring case.
 *
 * Portable, for the host tests; icons.c gives it names and does the rest.
 */
#ifndef CARDOS_ICONORDER_H
#define CARDOS_ICONORDER_H

#define ICONORDER_FAVS_MAX 16
#define ICONORDER_NAME_MAX 20

typedef struct {
  char name[ICONORDER_FAVS_MAX][ICONORDER_NAME_MAX];
  int  n;
} IconFavs;

/* The file's text, one name a line; '#' starts a comment, blanks skipped. */
void iconorder_parse(const char *text, IconFavs *out);

/* Where `name` is in the favourites (ignoring case), or -1. */
int iconorder_rank(const IconFavs *f, const char *name);

/* Sort `idx[0..n)` -- indices into the caller's table, whose names and
 * folder flags are `names[idx[i]]` and `is_folder[idx[i]]` -- into launcher
 * order: favourites by rank, then folders, then the rest, each A-Z. */
void iconorder_sort(int *idx, int n, const char *const *names, const int *is_folder,
                    const IconFavs *f);

/* What /config/favorites.txt starts as. */
extern const char ICONORDER_DEFAULT[];

#endif /* CARDOS_ICONORDER_H */
