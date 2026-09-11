#include "tinytest.h"
#include "kernel/mem/mem.h"
#include "kernel/mem/mem_internal.h"

static unsigned char heap[64 * 1024];

void test_alloc_returns_distinct_nonzero_handles(void) {
  mem_init(heap, sizeof heap);
  Handle a = mem_alloc(100, 0), b = mem_alloc(100, 0);
  CHECK(a != 0); CHECK(b != 0); CHECK(a != b);
  CHECK_EQ(mem_size(a), 100);
}

void test_handle_zero_is_always_invalid(void) {
  mem_init(heap, sizeof heap);
  CHECK_EQ(mem_valid(0), 0);
  CHECK_EQ(mem_size(0), 0);
}

void test_freed_handle_is_detected_as_stale(void) {
  mem_init(heap, sizeof heap);
  Handle a = mem_alloc(100, 0);
  mem_free(a);
  CHECK_EQ(mem_valid(a), 0);
  Handle b = mem_alloc(100, 0);
  CHECK_EQ(mem_index(b), mem_index(a));
  CHECK(b != a);
  CHECK_EQ(mem_valid(a), 0);
  CHECK_EQ(mem_valid(b), 1);
}

void test_generation_never_becomes_zero(void) {
  mem_init(heap, sizeof heap);
  for (int i = 0; i < 600; i++) {
    Handle h = mem_alloc(16, 0);
    CHECK(h != 0);
    CHECK(mem_generation(h) != 0);
    mem_free(h);
  }
}

void test_handle_table_exhaustion_returns_zero(void) {
  mem_init(heap, sizeof heap);
  int got = 0;
  for (int i = 0; i < MEM_MAX_HANDLES + 10; i++)
    if (mem_alloc(16, 0)) got++;
  CHECK_EQ(got, MEM_MAX_HANDLES);
}

void test_oversized_allocation_is_refused(void) {
  mem_init(heap, sizeof heap);
  CHECK_EQ(mem_alloc(MEM_MAX_BLOCK + 1, 0), 0);
  CHECK_EQ(mem_alloc(0, 0), 0);
}
