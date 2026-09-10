#include "tinytest.h"

int tt_checks = 0, tt_fails = 0;
const char *tt_current = "";

int tt_report(void) {
  printf("\n%d checks, %d failures\n", tt_checks, tt_fails);
  return tt_fails ? 1 : 0;
}

/* test_mem_handles.c */
void test_alloc_returns_distinct_nonzero_handles(void);
void test_handle_zero_is_always_invalid(void);
void test_freed_handle_is_detected_as_stale(void);
void test_generation_never_becomes_zero(void);
void test_handle_table_exhaustion_returns_zero(void);
void test_oversized_allocation_is_refused(void);

int main(void) {
  printf("-- handle table --\n");
  RUN(test_alloc_returns_distinct_nonzero_handles);
  RUN(test_handle_zero_is_always_invalid);
  RUN(test_freed_handle_is_detected_as_stale);
  RUN(test_generation_never_becomes_zero);
  RUN(test_handle_table_exhaustion_returns_zero);
  RUN(test_oversized_allocation_is_refused);
  return tt_report();
}
