#include "tinytest.h"
#include "kernel/mem/mem.h"
#include <string.h>

static unsigned char heap[64 * 1024];

void test_fixed_blocks_sit_above_movable_blocks(void) {
  mem_init(heap, sizeof heap);
  Handle m = mem_alloc(1024, 0);
  Handle f = mem_alloc(1024, MEM_FIXED);
  unsigned char *pm = mem_lock(m), *pf = mem_lock(f);
  CHECK(pf > pm);
  mem_unlock(m); mem_unlock(f);
}

void test_fixed_block_never_moves_across_a_compaction(void) {
  mem_init(heap, sizeof heap);
  Handle a = mem_alloc(2048, 0);
  Handle f = mem_alloc(1024, MEM_FIXED);
  unsigned char *pf = mem_lock(f);
  memset(pf, 0x5A, 1024);
  mem_unlock(f);
  mem_free(a);
  mem_compact();
  unsigned char *pf2 = mem_lock(f);
  CHECK_EQ(pf == pf2, 1);
  CHECK_EQ(pf2[1023], 0x5A);
  mem_unlock(f);
}

void test_freed_fixed_blocks_are_coalesced_and_reused(void) {
  mem_init(heap, sizeof heap);
  Handle a = mem_alloc(1024, MEM_FIXED);
  Handle b = mem_alloc(1024, MEM_FIXED);
  MemStats before; mem_stats(&before);
  mem_free(a); mem_free(b);
  Handle c = mem_alloc(2048, MEM_FIXED);
  CHECK(c != 0);
  MemStats after; mem_stats(&after);
  CHECK_EQ(before.fixed_used, after.fixed_used);
}

void test_fixed_and_movable_arenas_cannot_collide(void) {
  mem_init(heap, sizeof heap);
  while (mem_alloc(MEM_MAX_BLOCK, MEM_FIXED)) { }
  MemStats s; mem_stats(&s);
  CHECK(s.movable_used + s.fixed_used <= s.heap_size);
  CHECK_EQ(mem_alloc(MEM_MAX_BLOCK, 0), 0);
}
