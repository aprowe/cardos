/* ; , . / on the built-in keyboard: where something takes text but is
 * mostly moved through -- the fn-space palette, the launcher's search, an
 * app that says CAPP_TEXT_ARROWS -- they are arrows, and fn makes them the
 * characters again (kernel/input/arrowkeys.c). */
#include "tinytest.h"

#include "kernel/input/arrowkeys.h"
#include "kernel/drv/keyboard.h"
#include "kernel/app/capp.h"

void test_arrowkeys_swap_both_ways(void) {
  CHECK_EQ(arrowkeys_swap(';'), KEY_UP);
  CHECK_EQ(arrowkeys_swap('.'), KEY_DOWN);
  CHECK_EQ(arrowkeys_swap(','), KEY_LEFT);
  CHECK_EQ(arrowkeys_swap('/'), KEY_RIGHT);
  /* fn with them made arrows; here it makes the characters */
  CHECK_EQ(arrowkeys_swap(KEY_UP), ';');
  CHECK_EQ(arrowkeys_swap(KEY_DOWN), '.');
  CHECK_EQ(arrowkeys_swap(KEY_LEFT), ',');
  CHECK_EQ(arrowkeys_swap(KEY_RIGHT), '/');
  CHECK_EQ(arrowkeys_swap('a'), 'a');
  CHECK_EQ(arrowkeys_swap(KEY_ENTER), KEY_ENTER);
  CHECK_EQ(arrowkeys_swap(':'), ':');               /* shift-; is not an arrow */
}

void test_arrowkeys_where_they_come_first(void) {
  /* the palette filters by typing, but is a list: arrows first */
  CHECK(arrowkeys_first(1, 0, 0));
  CHECK(arrowkeys_first(1, 0, 1));
  /* another panel (help, notifications) takes no text: fn-arrows stay arrows */
  CHECK(!arrowkeys_first(0, 1, CAPP_TEXT_ARROWS));
  /* otherwise whatever has the keys says */
  CHECK(arrowkeys_first(0, 0, CAPP_TEXT_ARROWS));
  CHECK(!arrowkeys_first(0, 0, 1));                 /* typing: ; is ; */
  CHECK(!arrowkeys_first(0, 0, 0));                 /* no text: the shells map them already */
}
