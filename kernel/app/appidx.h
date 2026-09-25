/* The app index, /cache/apps.idx: what the icon scan learned about each .capp.
 *
 * The scan used to load every app on the card -- read it, relocate it into
 * executable RAM, and throw it away -- only to read its name, icon, flags and
 * commands. It did that on every boot. This file remembers those, keyed by
 * the file's path, size and date, so the next boot loads only an app that
 * changed.
 *
 * Portable, so the host suite reads the same lines the device writes. Text,
 * one record per app:
 *
 *   appidx KEY
 *   A <tab> size <tab> mtime <tab> flags <tab> icon-hex <tab> path <tab> name
 *   C <tab> one line of /cache/commands.txt           (none or more)
 *   E
 *
 * The KEY is what the whole file depends on: the API version and the apps
 * this firmware carries. A different key and nothing in it is used. E closes
 * a record, so one cut short by a power cut is not found.
 */
#ifndef CARDOS_APPIDX_H
#define CARDOS_APPIDX_H

#include <stddef.h>
#include <stdint.h>

#define APPIDX_ICON_BYTES 32
#define APPIDX_PATH_MAX   80
#define APPIDX_NAME_MAX   16

typedef struct {
  uint32_t size;
  uint32_t mtime;
  uint16_t flags;
  uint8_t  icon[APPIDX_ICON_BYTES];
  char     path[APPIDX_PATH_MAX];
  char     name[APPIDX_NAME_MAX];     /* terminated */
} AppIdxRec;

/* "appidx KEY\n" into out. Its length, or -1 if it does not fit. */
int appidx_header(char *out, size_t size, const char *key);

/* One record, with `cmds` -- catalog lines, each ending in \n -- after it.
 * Its length, or -1 (and out[0] = 0) if the whole of it does not fit. */
int appidx_record(char *out, size_t size, const AppIdxRec *r, const char *cmds);

/* Past the header, if `text` starts with the header for `key`; else NULL. */
const char *appidx_body(const char *text, const char *key);

/* The record for `path`, if its size and date are still these. 1 with *out
 * and `cmds` (the catalog lines, \n after each, terminated) filled; 0 if
 * there is none, it changed, it was cut short, `mtime` is 0, or its commands
 * do not fit `cmds`. */
int appidx_find(const char *body, const char *path, uint32_t size, uint32_t mtime,
                AppIdxRec *out, char *cmds, size_t cmds_size);

#endif /* CARDOS_APPIDX_H */
