/* apps/confirm.h: y is yes, n / Esc / Bksp no, a repeat or anything else
 * keeps asking -- Enter above all. */

#include "tinytest.h"
#include "fakeapi.h"
#include "apps/confirm.h"

static CardApi CA;

void test_confirm_only_y_is_yes(void) {
  fakeapi_init(&CA);
  CHECK_EQ(confirm_key(&CA, 'y'), CONFIRM_YES);
  CHECK_EQ(confirm_key(&CA, 'Y'), CONFIRM_YES);
  CHECK_EQ(confirm_key(&CA, CAPP_KEY_ENTER), CONFIRM_WAIT);
  CHECK_EQ(confirm_key(&CA, ' '), CONFIRM_WAIT);
  CHECK_EQ(confirm_key(&CA, 'd'), CONFIRM_WAIT);
}

void test_confirm_n_escape_and_backspace_are_no(void) {
  fakeapi_init(&CA);
  CHECK_EQ(confirm_key(&CA, 'n'), CONFIRM_NO);
  CHECK_EQ(confirm_key(&CA, 'N'), CONFIRM_NO);
  CHECK_EQ(confirm_key(&CA, CAPP_KEY_ESC), CONFIRM_NO);
  CHECK_EQ(confirm_key(&CA, CAPP_KEY_BACK), CONFIRM_NO);
}

void test_confirm_a_held_key_answers_nothing(void) {
  fakeapi_init(&CA);
  fakeapi_repeat = 1;                 /* the d that asked, still down */
  CHECK_EQ(confirm_key(&CA, 'y'), CONFIRM_WAIT);
  CHECK_EQ(confirm_key(&CA, 'n'), CONFIRM_WAIT);
  fakeapi_repeat = 0;
}
