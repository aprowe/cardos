/* The file picker's model, over a fake file system held in memory. */
#include <stdio.h>
#include <string.h>
#include "tinytest.h"
#include "kernel/ui/pickmodel.h"

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_ENTER 0x0D
#define KEY_BACK 0x08
#define KEY_ESC 0x1B
#define KEY_TAB 0x09
#define KEY_DEL 0x7F
#define CTRL(c) ((c) - 'a' + 1)

/* ---- a fake card: flat list of absolute paths ---------------------------- */

#define FAKE_MAX 48
static struct { char path[PM_PATH_MAX]; int is_dir; uint32_t size; } fake[FAKE_MAX];
static int nfake;

static void fake_clear(void) { nfake = 0; }
static void fake_add(const char *path, int is_dir, uint32_t size) {
  snprintf(fake[nfake].path, PM_PATH_MAX, "%s", path);
  fake[nfake].is_dir = is_dir; fake[nfake].size = size; nfake++;
}
static int fake_find(const char *path) {
  int i;
  for (i = 0; i < nfake; i++) if (!strcmp(fake[i].path, path)) return i;
  return -1;
}

static int fake_list(const char *dir, PmEntry *out, int max) {
  int i, n = 0;
  size_t dl = strlen(dir);
  for (i = 0; i < nfake && n < max; i++) {
    const char *p = fake[i].path, *rest;
    if (strncmp(p, dir, dl) != 0) continue;
    rest = p + dl;
    if (dl > 1) { if (*rest != '/') continue; rest++; }
    else if (*rest == '/') rest++;
    if (!*rest || strchr(rest, '/')) continue;       /* not a direct child */
    snprintf(out[n].name, PM_NAME_MAX, "%s", rest);
    out[n].size = fake[i].size; out[n].is_dir = (uint8_t)fake[i].is_dir;
    out[n].synthetic = 0;
    n++;
  }
  return n;
}
static int fake_mkdir(const char *path) { if (fake_find(path) >= 0) return -1; fake_add(path, 1, 0); return 0; }
static int fake_remove(const char *path) {
  int i = fake_find(path), j;
  size_t dl = strlen(path);
  if (i < 0) return -1;
  if (fake[i].is_dir)
    for (j = 0; j < nfake; j++)
      if (!strncmp(fake[j].path, path, dl) && fake[j].path[dl] == '/') return -1;  /* not empty */
  fake[i] = fake[--nfake];
  return 0;
}
static int fake_rename(const char *from, const char *to) {
  int i = fake_find(from);
  if (i < 0 || fake_find(to) >= 0) return -1;
  snprintf(fake[i].path, PM_PATH_MAX, "%s", to);
  return 0;
}
static int fake_exists(const char *path, int *is_dir) {
  int i = fake_find(path);
  if (i < 0) return 0;
  if (is_dir) *is_dir = fake[i].is_dir;
  return 1;
}
static const PmFs FS = { fake_list, fake_mkdir, fake_remove, fake_rename, fake_exists };

static void home_card(void) {
  fake_clear();
  fake_add("/home", 1, 0);
  fake_add("/home/notes", 1, 0);
  fake_add("/home/zeta.txt", 0, 120);
  fake_add("/home/alpha.md", 0, 40);
  fake_add("/home/Beta.txt", 0, 7);
  fake_add("/home/pic.png", 0, 9000);
  fake_add("/home/.hidden", 0, 1);
  fake_add("/home/notes/todo.txt", 0, 3);
  fake_add("/apps", 1, 0);
}

static void type(PickModel *m, const char *s) { while (*s) pm_key(m, (uint8_t)*s++, 0); }

/* ---- listing ------------------------------------------------------------- */

void test_pickmodel_lists_folders_first_then_names_case_blind(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  CHECK_EQ(m.n, 5);                                   /* .hidden is hidden */
  CHECK(!strcmp(m.ent[0].name, "notes")); CHECK(m.ent[0].is_dir);
  CHECK(!strcmp(m.ent[1].name, "alpha.md"));
  CHECK(!strcmp(m.ent[2].name, "Beta.txt"));
  CHECK(!strcmp(m.ent[3].name, "pic.png"));
  CHECK(!strcmp(m.ent[4].name, "zeta.txt"));
  CHECK_EQ(m.sel, 0);
  CHECK_EQ(m.done, 0);
}

void test_pickmodel_filter_keeps_folders_and_matching_extensions(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", "txt,md", NULL);
  CHECK_EQ(m.n, 4);
  CHECK(!strcmp(m.ent[0].name, "notes"));
  CHECK(!strcmp(m.ent[3].name, "zeta.txt"));         /* Beta.TXT would match too */
}

void test_pickmodel_ctrl_a_shows_hidden(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  pm_key(&m, CTRL('a'), 0);
  CHECK_EQ(m.n, 6);
  pm_key(&m, CTRL('a'), 0);
  CHECK_EQ(m.n, 5);
}

void test_pickmodel_starts_at_home_when_dir_is_missing(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/nowhere", NULL, NULL);
  CHECK(!strcmp(m.dir, "/home"));
  pm_begin(&m, &FS, PM_OPEN, "Open", NULL, NULL, NULL);
  CHECK(!strcmp(m.dir, "/home"));
}

/* ---- moving and choosing ------------------------------------------------- */

void test_pickmodel_enter_descends_backspace_ascends(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  CHECK_EQ(pm_key(&m, KEY_ENTER, 0), 0);
  CHECK(!strcmp(m.dir, "/home/notes"));
  CHECK_EQ(m.n, 1);
  CHECK_EQ(pm_key(&m, KEY_BACK, 0), 0);
  CHECK(!strcmp(m.dir, "/home"));
  CHECK_EQ(m.sel, 0);                                  /* back on the folder we left */
  pm_key(&m, KEY_BACK, 0);
  CHECK(!strcmp(m.dir, "/"));
  pm_key(&m, KEY_BACK, 0);
  CHECK(!strcmp(m.dir, "/"));                          /* the root has no parent */
}

void test_pickmodel_open_chooses_a_file_and_never_a_folder(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  pm_key(&m, KEY_DOWN, 0);
  CHECK_EQ(pm_key(&m, KEY_ENTER, 0), 1);
  CHECK_EQ(m.done, 1);
  CHECK(!strcmp(m.result, "/home/alpha.md"));
}

void test_pickmodel_escape_cancels(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  CHECK_EQ(pm_key(&m, KEY_ESC, 0), 1);
  CHECK_EQ(m.done, -1);
}

void test_pickmodel_arrows_clamp_and_scroll(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  m.rows = 2;
  pm_key(&m, KEY_UP, 0);   CHECK_EQ(m.sel, 0);
  pm_key(&m, KEY_DOWN, 0); pm_key(&m, KEY_DOWN, 0); pm_key(&m, KEY_DOWN, 0);
  CHECK_EQ(m.sel, 3); CHECK_EQ(m.top, 2);              /* scrolled to keep it visible */
  pm_key(&m, KEY_DOWN, 0); pm_key(&m, KEY_DOWN, 0);
  CHECK_EQ(m.sel, 4);
  pm_click(&m, 0);
  CHECK_EQ(m.sel, 3);                                  /* top visible row */
  pm_scroll(&m, -3);
  CHECK_EQ(m.top, 0);
}

void test_pickmodel_typing_jumps_to_a_name(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  pm_key(&m, 'z', 100);
  CHECK_EQ(m.sel, 4);
  pm_key(&m, 'b', 200);                                /* within the window: "zb" matches nothing, stays */
  CHECK_EQ(m.sel, 4);
  pm_key(&m, 'b', 3000);                               /* fresh prefix: "b" -> Beta */
  CHECK_EQ(m.sel, 2);
}

/* ---- save --------------------------------------------------------------- */

void test_pickmodel_save_types_a_name_and_returns_its_path(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_SAVE, "Save as", "/home", "txt", "untitled.txt");
  CHECK_EQ(m.ask, PM_ASK_NAME);                        /* the field has the keys first */
  CHECK(!strcmp(m.field, "untitled.txt"));
  pm_key(&m, KEY_BACK, 0); pm_key(&m, KEY_BACK, 0); pm_key(&m, KEY_BACK, 0);   /* txt */
  type(&m, "md");
  CHECK_EQ(pm_key(&m, KEY_ENTER, 0), 1);
  CHECK_EQ(m.done, 1);
  CHECK(!strcmp(m.result, "/home/untitled.md"));
}

void test_pickmodel_save_tab_moves_between_list_and_field(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_SAVE, "Save as", "/home", NULL, "new.txt");
  pm_key(&m, KEY_TAB, 0);
  CHECK_EQ(m.ask, PM_ASK_NONE);
  pm_key(&m, KEY_ENTER, 0);                            /* into notes/ */
  CHECK(!strcmp(m.dir, "/home/notes"));
  pm_key(&m, KEY_TAB, 0);
  CHECK_EQ(m.ask, PM_ASK_NAME);
  CHECK(!strcmp(m.field, "new.txt"));                 /* the name survived the trip */
  pm_key(&m, KEY_ENTER, 0);
  CHECK(!strcmp(m.result, "/home/notes/new.txt"));
}

void test_pickmodel_save_over_an_existing_file_asks(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_SAVE, "Save as", "/home", NULL, "zeta.txt");
  CHECK_EQ(pm_key(&m, KEY_ENTER, 0), 0);
  CHECK_EQ(m.ask, PM_ASK_OVERWRITE);
  pm_key(&m, 'n', 0);
  CHECK_EQ(m.ask, PM_ASK_NAME);
  CHECK_EQ(m.done, 0);
  pm_key(&m, KEY_ENTER, 0);
  CHECK_EQ(pm_key(&m, 'y', 0), 1);
  CHECK(!strcmp(m.result, "/home/zeta.txt"));
}

void test_pickmodel_save_choosing_a_listed_file_takes_its_name(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_SAVE, "Save as", "/home", NULL, "x.txt");
  pm_key(&m, KEY_TAB, 0);
  pm_key(&m, KEY_DOWN, 0);                             /* alpha.md */
  pm_key(&m, KEY_ENTER, 0);
  CHECK_EQ(m.ask, PM_ASK_OVERWRITE);
  CHECK(!strcmp(m.field, "alpha.md"));
}

void test_pickmodel_save_refuses_an_empty_or_slashed_name(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_SAVE, "Save as", "/home", NULL, "");
  CHECK_EQ(pm_key(&m, KEY_ENTER, 0), 0);
  CHECK_EQ(m.done, 0);
  type(&m, "a/b");
  CHECK_EQ(pm_key(&m, KEY_ENTER, 0), 0);
  CHECK(m.note[0] != 0);
}

/* ---- folder mode --------------------------------------------------------- */

void test_pickmodel_folder_mode_has_a_use_this_folder_row(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_FOLDER, "Choose a folder", "/home", NULL, NULL);
  CHECK(m.ent[0].synthetic);
  CHECK_EQ(m.n, 2);                                    /* the row, and notes/ */
  pm_key(&m, KEY_DOWN, 0);
  pm_key(&m, KEY_ENTER, 0);
  CHECK(!strcmp(m.dir, "/home/notes"));
  CHECK_EQ(m.sel, 0);
  CHECK_EQ(pm_key(&m, KEY_ENTER, 0), 1);
  CHECK(!strcmp(m.result, "/home/notes"));
}

/* ---- managing ------------------------------------------------------------ */

void test_pickmodel_new_folder_is_made_and_selected(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  pm_key(&m, CTRL('n'), 0);
  CHECK_EQ(m.ask, PM_ASK_MKDIR);
  type(&m, "drafts");
  pm_key(&m, KEY_ENTER, 0);
  CHECK_EQ(m.ask, PM_ASK_NONE);
  CHECK(fake_exists("/home/drafts", NULL));
  CHECK(!strcmp(m.ent[m.sel].name, "drafts"));
}

void test_pickmodel_rename_keeps_the_selection_on_the_renamed_entry(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  pm_key(&m, KEY_DOWN, 0);                             /* alpha.md */
  pm_key(&m, CTRL('r'), 0);
  CHECK_EQ(m.ask, PM_ASK_RENAME);
  CHECK(!strcmp(m.field, "alpha.md"));
  pm_key(&m, KEY_BACK, 0); pm_key(&m, KEY_BACK, 0);
  type(&m, "txt");
  pm_key(&m, KEY_ENTER, 0);
  CHECK(fake_exists("/home/alpha.txt", NULL));
  CHECK(!fake_exists("/home/alpha.md", NULL));
  CHECK(!strcmp(m.ent[m.sel].name, "alpha.txt"));
}

void test_pickmodel_rename_onto_an_existing_name_is_refused(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  pm_key(&m, KEY_DOWN, 0);
  pm_key(&m, CTRL('r'), 0);
  m.field_len = 0; m.field[0] = 0;
  type(&m, "zeta.txt");
  pm_key(&m, KEY_ENTER, 0);
  CHECK(fake_exists("/home/alpha.md", NULL));
  CHECK(m.note[0] != 0);
}

void test_pickmodel_delete_asks_and_refuses_a_full_folder(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  pm_key(&m, KEY_DOWN, 0);                             /* alpha.md */
  pm_key(&m, KEY_DEL, 0);
  CHECK_EQ(m.ask, PM_ASK_DELETE);
  pm_key(&m, 'n', 0);
  CHECK(fake_exists("/home/alpha.md", NULL));
  pm_key(&m, CTRL('d'), 0);
  pm_key(&m, 'y', 0);
  CHECK(!fake_exists("/home/alpha.md", NULL));
  CHECK_EQ(m.n, 4);
  CHECK_EQ(m.sel, 1);                                  /* the next entry */
  pm_key(&m, KEY_UP, 0);                               /* notes/, not empty */
  pm_key(&m, KEY_DEL, 0);
  pm_key(&m, 'y', 0);
  CHECK(fake_exists("/home/notes", NULL));
  CHECK(strstr(m.note, "empty") != NULL);
}

void test_pickmodel_escape_in_a_field_only_closes_the_field(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_OPEN, "Open", "/home", NULL, NULL);
  pm_key(&m, CTRL('n'), 0);
  type(&m, "x");
  CHECK_EQ(pm_key(&m, KEY_ESC, 0), 0);
  CHECK_EQ(m.ask, PM_ASK_NONE);
  CHECK_EQ(m.done, 0);
}

void test_pickmodel_the_synthetic_row_cannot_be_renamed_or_deleted(void) {
  PickModel m;
  home_card();
  pm_begin(&m, &FS, PM_FOLDER, "Choose", "/home", NULL, NULL);
  pm_key(&m, CTRL('r'), 0);
  CHECK_EQ(m.ask, PM_ASK_NONE);
  pm_key(&m, KEY_DEL, 0);
  CHECK_EQ(m.ask, PM_ASK_NONE);
}

void test_pickmodel_prompt_names_what_is_being_asked(void) {
  PickModel m;
  const char *label = NULL;
  home_card();
  pm_begin(&m, &FS, PM_SAVE, "Save as", "/home", NULL, "n.txt");
  CHECK(!strcmp(pm_prompt(&m, &label), "n.txt"));
  CHECK(!strcmp(label, "Name"));
  pm_key(&m, KEY_TAB, 0);
  pm_key(&m, CTRL('n'), 0);
  pm_prompt(&m, &label);
  CHECK(!strcmp(label, "New folder"));
}
