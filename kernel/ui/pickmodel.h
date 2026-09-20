/* The file picker's model: what it lists, where the cursor is, what it is
 * asking, and what it decided. Portable and host-tested; kernel/ui/picker.c
 * paints it and answers the file-system calls.
 *
 * One picker serves three requests -- open a file, save a file (a name
 * field appears), choose a folder (a "use this folder" row appears) -- and
 * in every mode it can make a folder, rename and delete, because a person
 * looking for somewhere to put a file is exactly the person who wants a new
 * folder, and a file picker that cannot tidy up is a file picker you leave
 * to go and tidy up.
 *
 * Keys, in the list:
 *   arrows        move            enter      open a folder / choose
 *   backspace     up a folder     tab        to the name field (save)
 *   letters       jump to a name  ctrl-n     new folder
 *   ctrl-r        rename          delete / ctrl-d   delete (asks)
 *   ctrl-a        show/hide dotfiles         escape   cancel
 * In a field: type, backspace, enter commits, escape cancels the field. */
#ifndef CARDOS_PICKMODEL_H
#define CARDOS_PICKMODEL_H

#include <stddef.h>
#include <stdint.h>

#define PM_NAME_MAX  64           /* a whole FAT long name (FS_NAME_MAX + 1) */
#define PM_PATH_MAX  128
#define PM_MAX       64           /* entries held for one folder */
#define PM_FILTER_MAX 24

enum { PM_OPEN = 0, PM_SAVE, PM_FOLDER };

/* What the picker is doing on top of the list. */
enum {
  PM_ASK_NONE = 0,
  PM_ASK_NAME,          /* the save-name field has the keys */
  PM_ASK_MKDIR,         /* typing a new folder's name */
  PM_ASK_RENAME,        /* typing the selected entry's new name */
  PM_ASK_DELETE,        /* "delete X? y/n" */
  PM_ASK_OVERWRITE      /* "X exists, replace? y/n" */
};

typedef struct {
  char     name[PM_NAME_MAX];
  uint32_t size;
  uint8_t  is_dir;
  uint8_t  synthetic;   /* the "use this folder" row: not on the card */
} PmEntry;

/* The file system, as the model needs it. Every path is absolute. `list`
 * fills up to `max` entries (unsorted; the model sorts) and returns the
 * count, or -1. `exists` says 0/1 and sets *is_dir. */
typedef struct {
  int (*list)(const char *dir, PmEntry *out, int max);
  int (*mkdir)(const char *path);
  int (*remove)(const char *path);
  int (*rename)(const char *from, const char *to);
  int (*exists)(const char *path, int *is_dir);
} PmFs;

typedef struct {
  PmFs  fs;
  int   mode;
  char  title[24];
  char  dir[PM_PATH_MAX];
  char  filter[PM_FILTER_MAX];      /* "txt,md" -- lowercase, no dots */
  int   show_hidden;

  PmEntry ent[PM_MAX];
  int   n;
  int   truncated;                  /* the folder held more than PM_MAX */
  int   sel;
  int   top;
  int   rows;                       /* how many the view shows; painter sets */

  int   ask;                        /* PM_ASK_* */
  char  field[PM_NAME_MAX];         /* the text being typed, whichever ask */
  int   field_len;
  char  name[PM_NAME_MAX];          /* save mode: the name in the field */

  char  jump[12];                   /* type-to-jump prefix */
  uint32_t jump_at;

  char  note[40];                   /* one line: an error, or a hint */
  int   done;                       /* 0 open, 1 chosen, -1 cancelled */
  char  result[PM_PATH_MAX];
  int   dirty;                      /* something to repaint */
} PickModel;

void pm_begin(PickModel *m, const PmFs *fs, int mode, const char *title,
              const char *dir, const char *filter, const char *name);

/* One key. Returns 1 if the picker finished (see m->done). */
int  pm_key(PickModel *m, uint8_t key, uint32_t now_ms);

/* A click on the row `row` of the view (0 = top visible), or a wheel. */
int  pm_click(PickModel *m, int row);
void pm_scroll(PickModel *m, int dy);

/* The full path of an entry, or of a name in the current folder. */
void pm_path_of(const PickModel *m, const char *name, char *out, size_t n);

/* Re-read the current folder (after the file system changed under it). */
void pm_refresh(PickModel *m);

/* The line for the bottom of the panel: the field being typed, the question
 * being asked, or the note. `label` gets the prompt ("Name", "Folder", ...)
 * or "" when the line is a plain note. */
const char *pm_prompt(const PickModel *m, const char **label);

#endif /* CARDOS_PICKMODEL_H */
