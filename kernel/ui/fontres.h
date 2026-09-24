/* Fonts an app asked for, loaded from the card. Device-only.
 *
 * A .cfnt is read whole into the heap -- a clock's eleven digits are 7 KB, a
 * print body 3 KB -- and parsed once (kernel/ui/cfont.c). Each load belongs
 * to an owner: the app that asked, which gets them all back when it is
 * released (capprun.c calls fontres_release_owner, the same way it disowns an
 * HTTP request), or a print job, which frees its own when the paper is out.
 * The same owner asking twice for the same font gets the same handle.
 *
 * A name with no slash is a font in /fonts: "clock56" is
 * /fonts/clock56.cfnt, which is how apps ask. A path is taken as it is.
 */
#ifndef CARDOS_FONTRES_H
#define CARDOS_FONTRES_H

#include "kernel/ui/cfont.h"

#define FONTRES_MAX 8
#define FONTS_DIR   "/fonts"

/* A handle, or -1: no such file, not a .cfnt, no memory, or all
 * FONTRES_MAX in use. The reason goes to the log. */
int  fontres_load(const char *name, const void *owner);
void fontres_free(int h);
void fontres_release_owner(const void *owner);

/* NULL for a handle that is not loaded -- including -1, so callers can pass
 * a failed load straight through and get the 6x8 font. */
const CFont *fontres_get(int h);

#endif /* CARDOS_FONTRES_H */
