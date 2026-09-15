#include "tinytest.h"
#include "kernel/mem/mem.h"
#include "kernel/mem/mem_internal.h"

static unsigned char heap[64 * 1024];

void test_alloc_returns_distinct_nonzero_handles(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(100, 0), b = kmem_alloc(100, 0);
  CHECK(a != 0); CHECK(b != 0); CHECK(a != b);
  CHECK_EQ(kmem_size(a), 100);
}

void test_handle_zero_is_always_invalid(void) {
  kmem_init(heap, sizeof heap);
  CHECK_EQ(kmem_valid(0), 0);
  CHECK_EQ(kmem_size(0), 0);
}

void test_freed_handle_is_detected_as_stale(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(100, 0);
  kmem_free(a);
  CHECK_EQ(kmem_valid(a), 0);
  Handle b = kmem_alloc(100, 0);
  CHECK_EQ(mem_index(b), mem_index(a));
  CHECK(b != a);
  CHECK_EQ(kmem_valid(a), 0);
  CHECK_EQ(kmem_valid(b), 1);
}

void test_generation_never_becomes_zero(void) {
  kmem_init(heap, sizeof heap);
  for (int i = 0; i < 600; i++) {
    Handle h = kmem_alloc(16, 0);
    CHECK(h != 0);
    CHECK(mem_generation(h) != 0);
    kmem_free(h);
  }
}

void test_handle_table_exhaustion_returns_zero(void) {
  kmem_init(heap, sizeof heap);
  int got = 0;
  for (int i = 0; i < MEM_MAX_HANDLES + 10; i++)
    if (kmem_alloc(16, 0)) got++;
  CHECK_EQ(got, MEM_MAX_HANDLES);
}

void test_oversized_allocation_is_refused(void) {
  kmem_init(heap, sizeof heap);
  CHECK_EQ(kmem_alloc(MEM_MAX_BLOCK + 1, 0), 0);
  CHECK_EQ(kmem_alloc(0, 0), 0);
}

/* The lock count is a byte. Nesting past 255 used to wrap it to zero, and a
 * block with 256 live pointers into it became movable. The 256th lock is
 * refused instead. */
void test_lock_nesting_has_a_ceiling(void) {
  static unsigned char heap[8192];
  Handle h;
  int i, got = 0;
  kmem_init(heap, sizeof heap);
  h = kmem_alloc(64, 0);
  for (i = 0; i < 300; i++) if (kmem_lock(h)) got++;
  CHECK_EQ(got, 255);
  for (i = 0; i < got; i++) kmem_unlock(h);
  CHECK(kmem_lock(h) != NULL);            /* fully unlocked: usable again */
  kmem_unlock(h);
}
