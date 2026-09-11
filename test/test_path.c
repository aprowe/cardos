#include "tinytest.h"
#include "kernel/fs/path.h"
#include <string.h>

static char out[FS_PATH_MAX];

#define NORM_IS(in, expect) do {                      \
    CHECK_EQ(path_normalize((in), out, sizeof out), 0); \
    CHECK_EQ(strcmp(out, (expect)), 0);                 \
    if (strcmp(out, (expect)))                          \
      printf("      %s -> %s, wanted %s\n", (in), out, (expect)); \
  } while (0)

#define RESOLVE_IS(cwd, rel, expect) do {                        \
    CHECK_EQ(path_resolve((cwd), (rel), out, sizeof out), 0);      \
    CHECK_EQ(strcmp(out, (expect)), 0);                            \
    if (strcmp(out, (expect)))                                     \
      printf("      %s + %s -> %s, wanted %s\n", (cwd), (rel), out, (expect)); \
  } while (0)

void test_absolute_paths_are_recognised(void) {
  CHECK_EQ(path_is_absolute("/a"), 1);
  CHECK_EQ(path_is_absolute("/"), 1);
  CHECK_EQ(path_is_absolute("a"), 0);
  CHECK_EQ(path_is_absolute(""), 0);
}

void test_normalize_leaves_a_clean_path_alone(void) {
  NORM_IS("/", "/");
  NORM_IS("/a", "/a");
  NORM_IS("/cardos/apps", "/cardos/apps");
}

void test_normalize_collapses_slashes_and_trailing_separators(void) {
  NORM_IS("/a//b", "/a/b");
  NORM_IS("/a/b/", "/a/b");
  NORM_IS("///", "/");
  NORM_IS("/a///b////c/", "/a/b/c");
}

void test_normalize_resolves_dot_and_dotdot(void) {
  NORM_IS("/a/./b", "/a/b");
  NORM_IS("/a/b/..", "/a");
  NORM_IS("/a/b/../c", "/a/c");
  NORM_IS("/./a", "/a");
  NORM_IS("/a/..", "/");
}

/* ".." at the root must not escape upward: on a device where the root is the
 * SD card, escaping it has no meaning and the result would be nonsense. */
void test_dotdot_cannot_escape_the_root(void) {
  NORM_IS("/..", "/");
  NORM_IS("/../..", "/");
  NORM_IS("/a/../../..", "/");
  NORM_IS("/../a", "/a");
}

void test_normalize_rejects_relative_and_empty_input(void) {
  CHECK_EQ(path_normalize("a/b", out, sizeof out), -1);
  CHECK_EQ(path_normalize("", out, sizeof out), -1);
  CHECK_EQ(path_normalize(".", out, sizeof out), -1);
}

void test_normalize_refuses_to_overflow(void) {
  char small[8];
  CHECK_EQ(path_normalize("/aaaaaaaaaaaaaaaa", small, sizeof small), -1);
  /* Exactly fitting is fine: "/abcdef" plus NUL is 8. */
  CHECK_EQ(path_normalize("/abcdef", small, sizeof small), 0);
  CHECK_EQ(strcmp(small, "/abcdef"), 0);
}

void test_a_component_longer_than_the_buffer_is_rejected(void) {
  char big[FS_PATH_MAX + 40];
  memset(big, 'x', sizeof big);
  big[0] = '/';
  big[sizeof big - 1] = 0;
  CHECK_EQ(path_normalize(big, out, sizeof out), -1);
}

void test_resolve_joins_a_relative_path_onto_the_cwd(void) {
  RESOLVE_IS("/home", "notes.txt", "/home/notes.txt");
  RESOLVE_IS("/", "cardos", "/cardos");
  RESOLVE_IS("/cardos", "apps/bruce.bin", "/cardos/apps/bruce.bin");
}

void test_resolve_lets_an_absolute_path_win(void) {
  RESOLVE_IS("/home", "/cardos/apps", "/cardos/apps");
  RESOLVE_IS("/home", "/", "/");
}

void test_resolve_handles_dot_and_dotdot(void) {
  RESOLVE_IS("/cardos/apps", "..", "/cardos");
  RESOLVE_IS("/cardos/apps", "../src", "/cardos/src");
  RESOLVE_IS("/cardos", ".", "/cardos");
  RESOLVE_IS("/", "..", "/");
  RESOLVE_IS("/a/b/c", "../../..", "/");
}

void test_resolve_of_an_empty_relative_path_is_the_cwd(void) {
  RESOLVE_IS("/home", "", "/home");
}

void test_resolve_refuses_to_overflow(void) {
  char small[10];
  CHECK_EQ(path_resolve("/home", "averylongname", small, sizeof small), -1);
}

void test_basename_returns_the_last_component(void) {
  CHECK_EQ(strcmp(path_basename("/cardos/apps/bruce.bin"), "bruce.bin"), 0);
  CHECK_EQ(strcmp(path_basename("/a"), "a"), 0);
  CHECK_EQ(strcmp(path_basename("/"), ""), 0);
}

void test_dirname_returns_everything_above(void) {
  CHECK_EQ(path_dirname("/cardos/apps/bruce.bin", out, sizeof out), 0);
  CHECK_EQ(strcmp(out, "/cardos/apps"), 0);
  CHECK_EQ(path_dirname("/a", out, sizeof out), 0);
  CHECK_EQ(strcmp(out, "/"), 0);
  CHECK_EQ(path_dirname("/", out, sizeof out), 0);
  CHECK_EQ(strcmp(out, "/"), 0);
}

/* The layout the spec fixes. If these ever stop resolving the launcher and
 * swap are looking in the wrong places. */
void test_the_documented_layout_resolves(void) {
  RESOLVE_IS("/", "cardos/swap.img", "/cardos/swap.img");
  RESOLVE_IS("/cardos", "apps", "/cardos/apps");
  RESOLVE_IS("/cardos/apps", "../src", "/cardos/src");
  RESOLVE_IS("/home", "../cardos/apps", "/cardos/apps");
}
