/* A folder on the card, as Files and Explorer see it: the listing, its
 * order, where you are, the operations, and what opens what.
 *
 * The two apps carried this twice, nearly line for line -- join, leaf, the
 * sort, reload, up, and the table that says which app opens a file -- and
 * the table was wrong in both: JPGs went to Photos, which shows only its own
 * .img pictures and so started a sync instead; .img went to Edit as text;
 * .cpx went to Web, which fetches anything ending in .cpx over HTTP, so a
 * page on the card was asked of the network. They draw differently and keep
 * that; this is what they know.
 *
 * Header-only, static helpers, the same arrangement as apps/safefile.h.
 * Host-tested in test/test_dirmodel.c over the in-memory card.
 */
#ifndef CARDOS_DIRMODEL_H
#define CARDOS_DIRMODEL_H

#include "kernel/app/capp.h"
#include "apps/str.h"

#if defined(__GNUC__)
#define DIR_OPT __attribute__((unused))
#else
#define DIR_OPT
#endif

#define DIR_MAX_ENTRIES 96
#define DIR_PATH_MAX    128

typedef struct {
  char      cwd[DIR_PATH_MAX];
  CappEntry ent[DIR_MAX_ENTRIES];
  /* The display order, as indices into ent. Sorting the array itself would
   * mean assigning 72-byte structs, and a struct assignment compiles to a
   * memcpy an app has nothing to link against -- so the entries stay where
   * the listing put them and this says what order to read them in. */
  unsigned char order[DIR_MAX_ENTRIES];
  int       n;
  int       nfolders;
} DirList;

/* ---- paths ------------------------------------------------------------------ */

/* A directory and a name, without the double slash at the root. */
static DIR_OPT void dir_join(const CardApi *api, char *out, size_t n, const char *dir,
                             const char *name) {
  if (dir[0] == '/' && !dir[1]) api->fmt(out, n, "/%s", name);
  else api->fmt(out, n, "%s/%s", dir, name);
}

/* The last component of a path. */
static DIR_OPT const char *dir_leaf(const char *path) {
  const char *p = path, *last = path;
  for (; *p; p++) if (*p == '/' && p[1]) last = p + 1;
  return last;
}

/* ---- what a file is, and what opens it ---------------------------------------- */

#define DIR_OPEN_APP  0         /* hand the path to `app` */
#define DIR_OPEN_RUN  1         /* a program: run it */
#define DIR_OPEN_NONE 2         /* nothing here opens it */

/* Case-insensitive, because a camera writes PHOTO.JPG and a person writing
 * this table thinks in lower case. `want` is lower case. */
static DIR_OPT int dir_ext_is(const char *ext, const char *want) {
  for (;;) {
    if (str_lower(*ext) != *want) return 0;
    if (!*want) return 1;
    ext++; want++;
  }
}

/* The Type column and how to open it, from one table so the two cannot
 * disagree about what a file is. Pictures as cameras and the dashboard
 * take them (jpg, png, bmp) cannot be shown: there is no decoder here, and
 * the dashboard's Photos page is where they become the .img Photos shows. */
static DIR_OPT const char *dir_kind(const char *name, int *how, const char **app) {
  static const struct { const char *ext, *kind, *app; uint8_t how; } T[] = {
    { ".capp", "Program",  0,       DIR_OPEN_RUN },
    { ".txt",  "Text",     "edit",  DIR_OPEN_APP },
    { ".md",   "Text",     "edit",  DIR_OPEN_APP },
    { ".c",    "Text",     "edit",  DIR_OPEN_APP },
    { ".h",    "Text",     "edit",  DIR_OPEN_APP },
    { ".cfg",  "Text",     "edit",  DIR_OPEN_APP },
    { ".ini",  "Text",     "edit",  DIR_OPEN_APP },
    { ".img",  "Image",    "photo", DIR_OPEN_APP },
    { ".565",  "Image",    "photo", DIR_OPEN_APP },
    { ".jpg",  "Picture",  0,       DIR_OPEN_NONE },
    { ".jpeg", "Picture",  0,       DIR_OPEN_NONE },
    { ".png",  "Picture",  0,       DIR_OPEN_NONE },
    { ".bmp",  "Picture",  0,       DIR_OPEN_NONE },
    { ".wav",  "Sound",    0,       DIR_OPEN_NONE },
    { ".bin",  "Firmware", 0,       DIR_OPEN_NONE },
  };
  const char *ext = name;
  int i;
  while (*ext) ext++;
  while (ext > name && *ext != '.') ext--;
  if (ext != name)
    for (i = 0; i < (int)(sizeof T / sizeof T[0]); i++)
      if (dir_ext_is(ext, T[i].ext)) { *how = T[i].how; *app = T[i].app; return T[i].kind; }
  *how = DIR_OPEN_APP;                /* no extension, or one not above: text */
  *app = "edit";
  return "File";
}

/* ---- the listing ------------------------------------------------------------ */

/* Folders first, then names, both case-insensitively. */
static DIR_OPT int dir_before(const CappEntry *a, const CappEntry *b) {
  const char *p = a->name, *q = b->name;
  if (a->is_dir != b->is_dir) return a->is_dir;
  for (;;) {
    char x = str_lower(*p), y = str_lower(*q);
    if (x != y) return x < y;
    if (!x) return 0;
    p++; q++;
  }
}

/* The entry at a display position. */
static DIR_OPT const CappEntry *dir_at(const DirList *d, int i) { return &d->ent[d->order[i]]; }

/* Read cwd again and sort it: an insertion sort, ninety-six entries at most,
 * run when a folder is entered rather than per frame. 0, or -1 when the
 * folder cannot be read (and then it is empty). */
static DIR_OPT int dir_reload(const CardApi *api, DirList *d) {
  int i, j, bad = 0;
  d->n = api->list_ex(d->cwd, d->ent, DIR_MAX_ENTRIES);
  if (d->n < 0) { d->n = 0; bad = -1; }
  for (i = 0; i < d->n; i++) d->order[i] = (unsigned char)i;
  for (i = 1; i < d->n; i++) {
    unsigned char tmp = d->order[i];
    for (j = i; j > 0 && dir_before(&d->ent[tmp], &d->ent[d->order[j - 1]]); j--)
      d->order[j] = d->order[j - 1];
    d->order[j] = tmp;
  }
  d->nfolders = 0;
  for (i = 0; i < d->n; i++) if (dir_at(d, i)->is_dir) d->nfolders++;
  return bad;
}

static DIR_OPT int dir_go(const CardApi *api, DirList *d, const char *path) {
  api->fmt(d->cwd, sizeof d->cwd, "%s", path);
  return dir_reload(api, d);
}

/* Up a folder. 0 when already at the root, else 1 (and it is read). */
static DIR_OPT int dir_up(const CardApi *api, DirList *d) {
  int i, cut = 0;
  if (str_same(d->cwd, "/")) return 0;
  for (i = 0; d->cwd[i]; i++) if (d->cwd[i] == '/') cut = i;
  d->cwd[cut ? cut : 1] = 0;
  dir_reload(api, d);
  return 1;
}

/* The path of the entry at display position i. */
static DIR_OPT void dir_path(const CardApi *api, const DirList *d, int i, char *out, size_t n) {
  dir_join(api, out, n, d->cwd, dir_at(d, i)->name);
}

/* ---- the operations: each reads the folder again and says what happened ---- */

static DIR_OPT const char *dir_mkdir(const CardApi *api, DirList *d, const char *name) {
  char path[DIR_PATH_MAX];
  int r;
  dir_join(api, path, sizeof path, d->cwd, name);
  r = api->mkdir(path);
  dir_reload(api, d);
  return r == 0 ? "folder made" : "could not make it";
}

static DIR_OPT const char *dir_rename(const CardApi *api, DirList *d, int i, const char *to_name) {
  char path[DIR_PATH_MAX], to[DIR_PATH_MAX];
  int r;
  dir_path(api, d, i, path, sizeof path);
  dir_join(api, to, sizeof to, d->cwd, to_name);
  r = api->rename(path, to);
  dir_reload(api, d);
  return r == 0 ? "renamed" : "could not rename it";
}

static DIR_OPT const char *dir_delete(const CardApi *api, DirList *d, int i) {
  char path[DIR_PATH_MAX];
  int r;
  dir_path(api, d, i, path, sizeof path);
  r = api->remove(path);
  dir_reload(api, d);
  return r == 0 ? "deleted" : "could not delete it";
}

/* `from` (a whole path) into this folder. A rename across folders is a move
 * on FAT, and it costs nothing: no bytes are copied, only the entry. */
static DIR_OPT const char *dir_move_here(const CardApi *api, DirList *d, const char *from) {
  char to[DIR_PATH_MAX];
  int r;
  dir_join(api, to, sizeof to, d->cwd, dir_leaf(from));
  r = api->rename(from, to);
  dir_reload(api, d);
  return r == 0 ? "moved" : "could not move it";
}

#endif /* CARDOS_DIRMODEL_H */
