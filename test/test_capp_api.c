#include "tinytest.h"
#include "kernel/app/elfload.h"

/* Which app API versions the loader takes: CAPP_API_MIN..CAPP_API_VERSION,
 * a newer one refused as needing a newer CardOS, an older one as too old. */

void test_capp_api_range_loads(void) {
  unsigned v;
  for (v = CAPP_API_MIN; v <= CAPP_API_VERSION; v++)
    CHECK_EQ(CAPP_OK, capp_api_check(v));
}

void test_capp_api_newer_needs_newer_cardos(void) {
  CHECK_EQ(CAPP_ERR_API_NEWER, capp_api_check(CAPP_API_VERSION + 1));
  CHECK_EQ(CAPP_ERR_API_NEWER, capp_api_check(0xFFFF));
}

void test_capp_api_older_refused(void) {
  CHECK_EQ(CAPP_ERR_API, capp_api_check(CAPP_API_MIN - 1));
  CHECK_EQ(CAPP_ERR_API, capp_api_check(0));
}

void test_capp_api_min_not_above_version(void) {
  CHECK(CAPP_API_MIN <= CAPP_API_VERSION);
  CHECK(CAPP_API_MIN >= 48);   /* CappUi.save: capprun copies CappUi whole */
}
