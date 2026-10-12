/* The fn-h panel and the fn-space palette, as state and keys -- no drawing.
 *
 * fn-h is one panel in three tabs: the app's keys (what it wrote, then its
 * action table's chords), the shell's own (the launcher's, the desktop's,
 * the console's), and the shortcuts that work anywhere (opt and fn chords,
 * and the owner's opt-letter hotkeys). Left/right (or tab, or 1-3) changes
 * tab, skipping an empty one; up/down scroll; Esc, fn-h, fn-` or Enter
 * close it. A text is one entry a line, "key<TAB>meaning"; a line with no
 * tab is a heading.
 *
 * fn-space is the palette: every action the app on screen offers in its
 * menus or by a chord, then the OS's, filtered by typing (ranked as the
 * launcher's search ranks names, kernel/ui/appsearch.c) and run by Enter.
 *
 * Portable; the host suite tests both. kernel/ui/help.c draws them and
 * src/main.c opens them.
 */
#ifndef CARDOS_HELPMODEL_H
#define CARDOS_HELPMODEL_H

#include <stdint.h>

/* ---- the help panel ---- */

#define HELP_TABS 3
enum { HELP_APP, HELP_OS, HELP_SHORT };

typedef struct {
  const char *name[HELP_TABS];   /* the tab's title */
  const char *text[HELP_TABS];   /* its lines; NULL or "" is an empty tab */
  int tab;                       /* the one showing */
  int top;                       /* its first line on screen */
  int rows;                      /* how many lines fit: the painter says */
} HelpModel;

int         helpm_count(const char *s);          /* lines in a text */
const char *helpm_line(const char *s, int n);    /* the start of line n, or NULL */
/* Show `tab`, or the next one with something in it. */
void        helpm_show(HelpModel *m, int tab);
/* A key: 1 still open, 0 closed. */
int         helpm_key(HelpModel *m, uint8_t key);

/* ---- the action palette ---- */

#define PAL_MAX   40
#define PAL_LABEL 28

enum { PAL_APP = 1, PAL_OS = 2 };               /* where an entry's action goes */
enum { PAL_OPEN, PAL_CLOSED, PAL_CHOSEN };      /* what a key did */

typedef struct {
  char    label[PAL_LABEL];
  char    chord[12];        /* "ctrl-s", "fn-p", "ctrl-opt-u", or "" */
  char    group[12];        /* the menu it is in, or "OS" */
  char    id[16];           /* an app action's id */
  uint8_t kind;             /* PAL_APP or PAL_OS */
  uint8_t key;              /* an OS entry's key, delivered as if pressed */
} PalEntry;

typedef struct {
  PalEntry e[PAL_MAX];
  int      n;
  char     q[24];           /* what has been typed */
  int      hit[PAL_MAX], nhit, sel, top;
} Palette;

void pal_clear(Palette *p);
int  pal_add(Palette *p, int kind, const char *label, const char *chord,
             const char *group, const char *id, uint8_t key);   /* -1 when full */
/* "ctrl-s" for 0x13, "fn-p" for the fn-p code; "" for none. */
void pal_chord(uint8_t key, char *out, int n);
void pal_filter(Palette *p);                     /* after q changes */
/* A key, with `rows` lines on screen. PAL_CHOSEN: the chosen entry is
 * e[hit[sel]], and the palette has closed. */
int  pal_key(Palette *p, uint8_t key, int rows);

#endif /* CARDOS_HELPMODEL_H */
