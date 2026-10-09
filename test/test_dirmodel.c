/* apps/dirmodel.h: what Files and Explorer know about a folder -- its
 * order, moving about, the operations, and what opens what -- over the
 * in-memory card. */

#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "apps/dirmodel.h"

static CardApi DA;
static DirList DL;

static void card(void) {
  fakeapi_init(&DA);
  fakefs_mem(&DA);
  memset(&DL, 0, sizeof DL);
  fakefs_put("/home/zeta.txt", "z");
  fakefs_put("/home/Alpha.md", "a");
  fakefs_put("/home/beta.img", "b");
  fakefs_put("/home/Docs/inner.txt", "i");
  DA.mkdir("/home/music");
}

void test_dirmodel_folders_first_then_names_ignoring_case(void) {
  card();
  CHECK_EQ(dir_go(&DA, &DL, "/home"), 0);
  CHECK_EQ(DL.n, 5);
  CHECK_EQ(DL.nfolders, 2);
  CHECK(!strcmp(dir_at(&DL, 0)->name, "Docs"));
  CHECK(!strcmp(dir_at(&DL, 1)->name, "music"));
  CHECK(!strcmp(dir_at(&DL, 2)->name, "Alpha.md"));
  CHECK(!strcmp(dir_at(&DL, 3)->name, "beta.img"));
  CHECK(!strcmp(dir_at(&DL, 4)->name, "zeta.txt"));
}

void test_dirmodel_in_and_up_to_the_root(void) {
  char path[DIR_PATH_MAX];
  card();
  dir_go(&DA, &DL, "/home");
  dir_path(&DA, &DL, 0, path, sizeof path);
  CHECK(!strcmp(path, "/home/Docs"));
  dir_go(&DA, &DL, path);
  CHECK_EQ(DL.n, 1);
  CHECK_EQ(dir_up(&DA, &DL), 1);
  CHECK(!strcmp(DL.cwd, "/home"));
  CHECK_EQ(dir_up(&DA, &DL), 1);
  CHECK(!strcmp(DL.cwd, "/"));
  CHECK_EQ(dir_up(&DA, &DL), 0);             /* nowhere above the root */
  dir_join(&DA, path, sizeof path, "/", "x");
  CHECK(!strcmp(path, "/x"));                /* no double slash */
  CHECK(!strcmp(dir_leaf("/home/Docs/inner.txt"), "inner.txt"));
  CHECK_EQ(dir_go(&DA, &DL, "/nowhere"), -1);
  CHECK_EQ(DL.n, 0);
}

void test_dirmodel_operations_say_what_happened_and_read_again(void) {
  card();
  dir_go(&DA, &DL, "/home");
  CHECK(!strcmp(dir_mkdir(&DA, &DL, "new"), "folder made"));
  CHECK_EQ(DL.nfolders, 3);
  CHECK(!strcmp(dir_rename(&DA, &DL, 5, "Zed.txt"), "renamed"));   /* zeta.txt */
  CHECK(fakefs_exists("/home/Zed.txt"));
  CHECK(!strcmp(dir_delete(&DA, &DL, 3), "deleted"));             /* Alpha.md */
  CHECK(!fakefs_exists("/home/Alpha.md"));
  CHECK(!strcmp(dir_delete(&DA, &DL, 0), "could not delete it"));  /* Docs, not empty */
  CHECK(!strcmp(dir_move_here(&DA, &DL, "/home/Docs/inner.txt"), "moved"));
  CHECK(fakefs_exists("/home/inner.txt"));
}

void test_dirmodel_what_opens_what(void) {
  int how;
  const char *app;
  CHECK(!strcmp(dir_kind("notes.TXT", &how, &app), "Text"));
  CHECK(how == DIR_OPEN_APP && !strcmp(app, "edit"));
  CHECK(!strcmp(dir_kind("p1.img", &how, &app), "Image"));
  CHECK(how == DIR_OPEN_APP && !strcmp(app, "photo"));         /* was Edit */
  dir_kind("raw.565", &how, &app);
  CHECK(how == DIR_OPEN_APP && !strcmp(app, "photo"));
  CHECK(!strcmp(dir_kind("PHOTO.JPG", &how, &app), "Picture"));
  CHECK_EQ(how, DIR_OPEN_NONE);                                /* was Photos */
  dir_kind("a.png", &how, &app);
  CHECK_EQ(how, DIR_OPEN_NONE);
  dir_kind("timer.capp", &how, &app);
  CHECK_EQ(how, DIR_OPEN_RUN);
  dir_kind("page.cpx", &how, &app);
  CHECK(how == DIR_OPEN_APP && !strcmp(app, "edit"));          /* not fetched by Web */
  dir_kind("README", &how, &app);
  CHECK(how == DIR_OPEN_APP && !strcmp(app, "edit"));
}
