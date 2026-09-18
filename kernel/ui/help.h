/* The fn-h key list. Device-only.
 *
 * Every shell shows the same panel over whatever is on screen: the app's keys
 * first, then the ones the shell itself owns. Shared rather than written twice
 * because a key list that differs between the launcher and the desktop is a
 * key list nobody trusts.
 */
#ifndef CARDOS_HELP_H
#define CARDOS_HELP_H

/* Draw the panel. Both strings are one entry per line, "key<tab>meaning";
 * either may be NULL. Paints over whatever is there, so the caller repaints
 * when it closes. */
void help_paint(const char *title, const char *app_keys, const char *shell_keys);

#endif /* CARDOS_HELP_H */
