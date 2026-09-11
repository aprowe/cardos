#include "tinytest.h"
#include "kernel/mem/mem.h"
#include <string.h>

static unsigned char heap[64 * 1024];

void test_locked_pointer_is_writable_and_stable(void) {
  mem_init(heap, sizeof heap);
  Handle h = mem_alloc(256, 0);
  unsigned char *p = mem_lock(h);
  CHECK(p != NULL);
  memset(p, 0xAB, 256);
  unsigned char *q = mem_lock(h);
  CHECK_EQ(p == q, 1);
  CHECK_EQ(p[255], 0xAB);
  mem_unlock(h); mem_unlock(h);
}

void test_mem_zero_clears_the_block(void) {
  mem_init(heap, sizeof heap);
  memset(heap, 0xFF, sizeof heap);
  Handle h = mem_alloc(64, MEM_ZERO);
  unsigned char *p = mem_lock(h);
  for (int i = 0; i < 64; i++) CHECK_EQ(p[i], 0);
  mem_unlock(h);
}

void test_blocks_do_not_overlap(void) {
  mem_init(heap, sizeof heap);
  Handle a = mem_alloc(1000, 0), b = mem_alloc(1000, 0);
  unsigned char *pa = mem_lock(a), *pb = mem_lock(b);
  memset(pa, 1, 1000); memset(pb, 2, 1000);
  for (int i = 0; i < 1000; i++) { CHECK_EQ(pa[i], 1); CHECK_EQ(pb[i], 2); }
  mem_unlock(a); mem_unlock(b);
}

void test_lock_of_stale_handle_returns_null(void) {
  mem_init(heap, sizeof heap);
  Handle h = mem_alloc(64, 0);
  mem_free(h);
  CHECK(mem_lock(h) == NULL);
}

void test_alloc_fails_cleanly_when_heap_is_full(void) {
  mem_init(heap, sizeof heap);
  int n = 0;
  while (mem_alloc(MEM_MAX_BLOCK, 0)) n++;
  CHECK(n >= 1);
  MemStats s; mem_stats(&s);
  CHECK(s.free_bytes < MEM_MAX_BLOCK);
}
