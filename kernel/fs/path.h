/* Path handling for the CardOS filesystem.
 *
 * Portable C: no ESP-IDF, no FatFs. The shell's cd/ls/cat all resolve relative
 * paths against a working directory, and getting that wrong is how you end up
 * reading the wrong file or writing off the end of a buffer. It is pure string
 * logic, so it gets tested on the host rather than on a device with no
 * debugger.
 *
 * Paths are absolute, '/'-separated, with no trailing slash except for the
 * root itself, which is exactly "/".
 */
#ifndef CARDOS_PATH_H
#define CARDOS_PATH_H

#include <stddef.h>

#define FS_PATH_MAX 128        /* including the terminating NUL */

int  path_is_absolute(const char *p);

/* Collapse repeated slashes, resolve "." and "..", and strip any trailing
 * slash. `in` must be absolute. Returns 0 on success, -1 if `in` is relative,
 * empty, or the result would not fit. ".." at the root stays at the root
 * rather than escaping above it. */
int  path_normalize(const char *in, char *out, size_t out_size);

/* Resolve `rel` against `cwd`. An absolute `rel` replaces `cwd` entirely.
 * The result is normalized. Returns 0 on success, -1 on overflow or bad input. */
int  path_resolve(const char *cwd, const char *rel, char *out, size_t out_size);

/* The final component. For "/" this is the empty string. The pointer is into
 * `path`, so it lives exactly as long as `path` does. */
const char *path_basename(const char *path);

/* Everything but the final component. "/a/b" -> "/a", "/a" -> "/". */
int  path_dirname(const char *path, char *out, size_t out_size);

#endif /* CARDOS_PATH_H */
