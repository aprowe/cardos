/* The fn-h panel's tabs and scrolling, and the fn-space palette:
 * kernel/ui/helpmodel.c. */
#include <string.h>

#include "tinytest.h"
#include "kernel/ui/helpmodel.h"
#include "kernel/drv/keyboard.h"

static const char *APPK = "space\ttoggle\nenter\topen\nn\tnew\nd\tdelete\ne\tedit\n";
static const char *OSK  = "arrows\tmove\nenter\topen\n";
static const char *SHORT = "Anywhere\nfn-n\tnotifications\nopt-t\tTodo\n";

static void model(HelpModel *m, const char *app) {
  memset(m, 0, sizeof *m);
  m->name[HELP_APP] = "Todo"; m->text[HELP_APP] = app;
  m->name[HELP_OS] = "Launcher"; m->text[HELP_OS] = OSK;
  m->name[HELP_SHORT] = "Shortcuts"; m->text[HELP_SHORT] = SHORT;
  m->rows = 3;
}

void test_helpmodel_lines_are_counted_and_found(void) {
  CHECK_EQ(helpm_count(APPK), 5);
  CHECK_EQ(helpm_count("a\tb"), 1);            /* no newline at the end */
  CHECK_EQ(helpm_count(""), 0);
  CHECK_EQ(helpm_count(NULL), 0);
  CHECK(!strncmp(helpm_line(APPK, 2), "n\tnew", 5));
  CHECK(helpm_line(APPK, 5) == NULL);
}

void test_helpmodel_tabs_skip_an_empty_one(void) {
  HelpModel m;
  model(&m, NULL);                              /* no app: the launcher's own */
  helpm_show(&m, HELP_APP);
  CHECK_EQ(m.tab, HELP_OS);
  CHECK_EQ(helpm_key(&m, KEY_RIGHT), 1);
  CHECK_EQ(m.tab, HELP_SHORT);
  helpm_key(&m, KEY_RIGHT);                     /* round, past the empty app tab */
  CHECK_EQ(m.tab, HELP_OS);
  helpm_key(&m, KEY_LEFT);
  CHECK_EQ(m.tab, HELP_SHORT);
  helpm_key(&m, '1');                           /* empty: stays */
  CHECK_EQ(m.tab, HELP_SHORT);
  helpm_key(&m, '2');
  CHECK_EQ(m.tab, HELP_OS);
}

void test_helpmodel_scrolls_within_the_text(void) {
  HelpModel m;
  model(&m, APPK);
  helpm_show(&m, HELP_APP);
  CHECK_EQ(m.tab, HELP_APP);
  helpm_key(&m, KEY_UP);
  CHECK_EQ(m.top, 0);
  helpm_key(&m, KEY_DOWN);
  helpm_key(&m, '.');                           /* the unshifted arrow too */
  CHECK_EQ(m.top, 2);                           /* five lines, three show: 2 is the end */
  helpm_key(&m, KEY_DOWN);
  CHECK_EQ(m.top, 2);
  helpm_key(&m, KEY_RIGHT);
  CHECK_EQ(m.top, 0);                           /* a new tab starts at its top */
}

void test_helpmodel_closes_on_the_keys_that_close(void) {
  HelpModel m;
  model(&m, APPK);
  helpm_show(&m, HELP_APP);
  CHECK_EQ(helpm_key(&m, 'x'), 1);              /* a stray key does not close it */
  CHECK_EQ(helpm_key(&m, KEY_ESC), 0);
  CHECK_EQ(helpm_key(&m, KEY_HELP), 0);
  CHECK_EQ(helpm_key(&m, KEY_QUIT), 0);
}

void test_palette_types_to_filter_and_enter_chooses(void) {
  Palette p;
  int r;
  pal_clear(&p);
  pal_add(&p, PAL_APP, "Add", "ctrl-a", "Task", "add", 0);
  pal_add(&p, PAL_APP, "Sync now", "ctrl-s", "List", "sync", 0);
  pal_add(&p, PAL_APP, "Print", "fn-p", "List", "print", 0);
  pal_add(&p, PAL_OS, "Notifications", "fn-n", "OS", "", KEY_FN_LETTER('n'));
  pal_filter(&p);
  CHECK_EQ(p.nhit, 4);                          /* nothing typed: all, in order */
  CHECK_EQ(p.hit[0], 0);
  pal_key(&p, 's', 5);
  CHECK_EQ(p.nhit, 2);                          /* Sync, then Notifications */
  CHECK_EQ(p.hit[0], 1);
  pal_key(&p, KEY_DOWN, 5);
  r = pal_key(&p, KEY_ENTER, 5);
  CHECK_EQ(r, PAL_CHOSEN);
  CHECK_EQ(p.e[p.hit[p.sel]].kind, PAL_OS);
  CHECK_EQ(p.e[p.hit[p.sel]].key, KEY_FN_LETTER('n'));
  pal_key(&p, KEY_BACKSPACE, 5);
  CHECK_EQ(p.nhit, 4);
  CHECK_EQ(pal_key(&p, KEY_BACKSPACE, 5), PAL_CLOSED);   /* past the start */
}

void test_palette_scrolls_and_wraps(void) {
  Palette p;
  int i;
  char l[8];
  pal_clear(&p);
  for (i = 0; i < 10; i++) { l[0] = 'A' + i; l[1] = 0; pal_add(&p, PAL_APP, l, "", "", l, 0); }
  pal_filter(&p);
  for (i = 0; i < 6; i++) pal_key(&p, KEY_DOWN, 4);
  CHECK_EQ(p.sel, 6);
  CHECK_EQ(p.top, 3);
  pal_key(&p, KEY_UP, 4);
  pal_key(&p, KEY_UP, 4);
  pal_key(&p, KEY_UP, 4);
  pal_key(&p, KEY_UP, 4);
  CHECK_EQ(p.top, 2);
  p.sel = 0; p.top = 0;
  pal_key(&p, KEY_UP, 4);                       /* above the first: the last */
  CHECK_EQ(p.sel, 9);
  CHECK_EQ(p.top, 6);
  CHECK_EQ(pal_key(&p, KEY_ESC, 4), PAL_CLOSED);
}

void test_palette_names_chords(void) {
  char c[10];
  pal_chord(0x13, c, sizeof c);
  CHECK(!strcmp(c, "ctrl-s"));
  pal_chord(KEY_FN_LETTER('p'), c, sizeof c);
  CHECK(!strcmp(c, "fn-p"));
  pal_chord(KEY_OPT_LETTER('t'), c, sizeof c);
  CHECK(!strcmp(c, "opt-t"));
  pal_chord(0, c, sizeof c);
  CHECK(!c[0]);
}
