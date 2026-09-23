/* Commands from outside an app, as words, checked against what it declared.
 *
 * The console's `do`, a voice line and an AI all arrive as text; an app's
 * handler only ever sees arguments that fit its CappAction table -- right
 * count, right types. Portable, so the host suite covers it. See
 * docs/superpowers/specs/2026-09-23-app-commands-design.md. */
#ifndef CARDOS_CMDLINE_H
#define CARDOS_CMDLINE_H

#include <stddef.h>
#include "kernel/app/capp.h"

/* Words into w[], "double quotes" keeping spaces, the text copied into buf.
 * The count, or -1 for an unclosed quote or too little room. */
int cmdline_split(const char *line, char *buf, size_t n, const char **w, int max);

/* Is this table entry a command (and not only a GUI action)? */
int cmdline_is_command(const CappAction *a);

/* The command called `id`, or NULL -- GUI-only entries are not commands. */
const CappAction *cmdline_find(const CappAction *t, int n, const char *id);

/* Bind nwords words to a's parameters into argv. A last TEXT parameter takes
 * the rest of the words, joined with spaces into `join`, so an unquoted
 * sentence is still one argument. Checks the count and each type. The
 * argument count, or -1 with the reason in `why`. */
int cmdline_bind(const CappAction *a, int nwords, const char *const *words,
                 const char **argv, char *join, size_t jn, char *why, size_t wn);

/* "todo add text:text # add a task" -- one line of /cache/commands.txt, and
 * of what the voice prompt and `do` show. A network command says "net". */
void cmdline_catalog_line(const char *app, const CappAction *a, char *out, size_t n);

#endif
