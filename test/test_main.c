#include "tinytest.h"
int tt_checks = 0, tt_fails = 0;
const char *tt_current = "";
int tt_report(void) {
  printf("\n%d checks, %d failures\n", tt_checks, tt_fails);
  return tt_fails ? 1 : 0;
}
static void test_harness_reports_failure(void) { CHECK_EQ(1, 1); }
int main(void) { RUN(test_harness_reports_failure); return tt_report(); }
