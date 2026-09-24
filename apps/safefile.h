/* Saving a small file so that a power cut cannot lose it.
 *
 * Rewriting a file in place -- open with TRUNC, write, close -- has a window
 * where the old contents are gone and the new ones are not all there yet. On
 * a device that is switched off by pulling it out of a pocket, that window
 * is where the data goes. So: write NAME.tmp, remove NAME, rename the temp
 * over it. The card's rename will not replace an existing file, hence the
 * remove -- and a cut between the remove and the rename leaves only the
 * .tmp, which safe_open_read notices and finishes the job with.
 *
 *   A cut while writing NAME.tmp       NAME is untouched; the half .tmp is
 *                                      ignored and overwritten next time.
 *   A cut after NAME was removed       only NAME.tmp is there, complete;
 *                                      the next read renames it into place.
 *
 * Used by apps/counter.c and apps/habits.c. Header-only, static helpers, the
 * same arrangement as apps/toolbar.h.
 */
#ifndef CARDOS_SAFEFILE_H
#define CARDOS_SAFEFILE_H

#include "kernel/app/capp.h"

#if defined(__GNUC__)
#define SF_OPT __attribute__((unused))
#else
#define SF_OPT
#endif

#define SF_PATH_MAX 80

typedef struct {
  const CardApi *api;
  int  fd;
  int  bad;                     /* a write came up short */
  char path[SF_PATH_MAX];
  char tmp[SF_PATH_MAX + 4];
} SafeFile;

/* Open NAME for reading, first putting back a NAME.tmp that a cut stranded.
 * The fd, or -1 if there is no such file. */
static SF_OPT int safe_open_read(const CardApi *api, const char *path) {
  char tmp[SF_PATH_MAX + 4];
  CappStat st;
  int fd = api->open(path, CAPP_O_READ);
  if (fd >= 0) return fd;
  api->fmt(tmp, sizeof tmp, "%s.tmp", path);
  if (api->stat(tmp, &st) != 0) return -1;
  if (api->rename(tmp, path) != 0) return -1;
  return api->open(path, CAPP_O_READ);
}

/* Start a save. 0, or -1 if the temp file cannot be made (no card). */
static SF_OPT int safe_begin(SafeFile *f, const CardApi *api, const char *path) {
  f->api = api;
  f->bad = 0;
  api->fmt(f->path, sizeof f->path, "%s", path);
  api->fmt(f->tmp, sizeof f->tmp, "%s.tmp", path);
  f->fd = api->open(f->tmp, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  return f->fd < 0 ? -1 : 0;
}

static SF_OPT void safe_write(SafeFile *f, const char *s, size_t n) {
  if (f->fd < 0 || f->bad) return;
  if (f->api->write(f->fd, s, n) != (int)n) f->bad = 1;
}

/* A formatted line, up to 160 bytes. */
static SF_OPT void safe_line(SafeFile *f, const char *s) {
  safe_write(f, s, f->api->str_len(s));
}

/* Finish: the temp becomes the file. 0, or -1 and the old file untouched if
 * any write came up short -- a full card must not replace a good file with
 * half of one. */
static SF_OPT int safe_commit(SafeFile *f) {
  const CardApi *api = f->api;
  if (f->fd < 0) return -1;
  api->close(f->fd);
  f->fd = -1;
  if (f->bad) { api->remove(f->tmp); return -1; }
  api->remove(f->path);
  return api->rename(f->tmp, f->path) == 0 ? 0 : -1;
}

#endif /* CARDOS_SAFEFILE_H */
