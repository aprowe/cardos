/* Which apps sit on the desktop. Device-only.
 *
 * The desktop used to draw every app there was, which is the same list the
 * Start menu should show and a bad answer for a 240x135 screen once there are
 * fifteen of them. So the two are separated: the Start menu is everything,
 * the desktop is what you put there.
 *
 * Stored as names rather than paths, because a name is what the user sees and
 * what survives an app moving between folders -- `update apps` and the
 * folder seeding both move files around, and a pin that pointed at a path
 * would quietly stop matching.
 *
 * UNTIL SOMETHING IS UNPINNED, EVERYTHING IS PINNED. A fresh device should
 * look like it did before this existed rather than like an empty desk, and a
 * new app should turn up rather than hide. That is what pins_active
 * distinguishes: no list at all means "all of them", an empty list means
 * "none of them, and I meant it".
 */
#ifndef CARDOS_PINS_H
#define CARDOS_PINS_H

/* Load from NVS. Call once at boot, before the desktop paints. */
void pins_init(void);

/* Has the user ever pinned or unpinned anything? While this is 0 the desktop
 * shows every app. */
int  pins_active(void);

/* Should this app show on the desktop? True for everything until the list
 * exists. */
int  pins_has(const char *name);

void pins_add(const char *name);
void pins_remove(const char *name);

#endif /* CARDOS_PINS_H */
