/* The launcher's order: favourites first, then folders, then apps, A-Z. */

#include <string.h>

#include "tinytest.h"
#include "kernel/ui/iconorder.h"

static const char *NAMES[] = { "Stocks", "Games", "calendar", "About", "Plan", "Todo",
                               "Make", "Edit" };
static const int FOLDER[] = { 0, 1, 0, 0, 1, 0, 1, 0 };

static void order(const char *favs, int *idx, int n) {
  IconFavs f;
  int i;
  iconorder_parse(favs, &f);
  for (i = 0; i < n; i++) idx[i] = i;
  iconorder_sort(idx, n, NAMES, FOLDER, &f);
}

void test_iconorder_favourites_then_folders_then_apps(void) {
  int idx[8];
  order("Todo\n  Calendar \n# not this\n\nPlan\n", idx, 8);
  /* Todo, Calendar (any case), Plan; then the other folders A-Z; then apps. */
  CHECK(!strcmp(NAMES[idx[0]], "Todo"));
  CHECK(!strcmp(NAMES[idx[1]], "calendar"));
  CHECK(!strcmp(NAMES[idx[2]], "Plan"));
  CHECK(!strcmp(NAMES[idx[3]], "Games"));
  CHECK(!strcmp(NAMES[idx[4]], "Make"));
  CHECK(!strcmp(NAMES[idx[5]], "About"));
  CHECK(!strcmp(NAMES[idx[6]], "Edit"));
  CHECK(!strcmp(NAMES[idx[7]], "Stocks"));
}

void test_iconorder_no_favourites_is_folders_then_a_to_z(void) {
  int idx[8];
  order("", idx, 8);
  CHECK(!strcmp(NAMES[idx[0]], "Games"));
  CHECK(!strcmp(NAMES[idx[2]], "Plan"));
  CHECK(!strcmp(NAMES[idx[3]], "About"));
  CHECK(!strcmp(NAMES[idx[4]], "calendar"));
}

void test_iconorder_reads_the_default_file(void) {
  IconFavs f;
  iconorder_parse(ICONORDER_DEFAULT, &f);
  CHECK_EQ(f.n, 9);
  CHECK_EQ(iconorder_rank(&f, "todo"), 0);
  CHECK_EQ(iconorder_rank(&f, "System"), 8);
  CHECK_EQ(iconorder_rank(&f, "Pinball"), -1);
}

void test_iconorder_a_long_list_is_cut_not_overrun(void) {
  IconFavs f;
  char text[30 * 32 + 1];          /* 30 lines of 31 characters */
  int i, o = 0;
  for (i = 0; i < 30; i++) o += (int)strlen(strcpy(text + o, "AVeryLongApplicationNameIndeed\n"));
  iconorder_parse(text, &f);
  CHECK_EQ(f.n, ICONORDER_FAVS_MAX);
  CHECK_EQ((int)strlen(f.name[0]), ICONORDER_NAME_MAX - 1);
}
