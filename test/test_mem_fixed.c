#include "tinytest.h"
#include "kernel/mem/mem.h"
#include <string.h>

static unsigned char heap[64 * 1024];

void test_fixed_blocks_sit_above_movable_blocks(void) {
  kmem_init(heap, sizeof heap);
  Handle m = kmem_alloc(1024, 0);
  Handle f = kmem_alloc(1024, MEM_FIXED);
  unsigned char *pm = kmem_lock(m), *pf = kmem_lock(f);
  CHECK(pf > pm);
  kmem_unlock(m); kmem_unlock(f);
}

void test_fixed_block_never_moves_across_a_compaction(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(2048, 0);
  Handle f = kmem_alloc(1024, MEM_FIXED);
  unsigned char *pf = kmem_lock(f);
  memset(pf, 0x5A, 1024);
  kmem_unlock(f);
  kmem_free(a);
  kmem_compact();
  unsigned char *pf2 = kmem_lock(f);
  CHECK_EQ(pf == pf2, 1);
  CHECK_EQ(pf2[1023], 0x5A);
  kmem_unlock(f);
}

void test_freed_fixed_blocks_are_coalesced_and_reused(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(1024, MEM_FIXED);
  Handle b = kmem_alloc(1024, MEM_FIXED);
  MemStats before; kmem_stats(&before);
  kmem_free(a); kmem_free(b);
  Handle c = kmem_alloc(2048, MEM_FIXED);
  CHECK(c != 0);
  MemStats after; kmem_stats(&after);
  CHECK_EQ(before.fixed_used, after.fixed_used);
}

void test_fixed_and_movable_arenas_cannot_collide(void) {
  kmem_init(heap, sizeof heap);
  while (kmem_alloc(MEM_MAX_BLOCK, MEM_FIXED)) { }
  MemStats s; kmem_stats(&s);
  CHECK(s.movable_used + s.fixed_used <= s.heap_size);
  CHECK_EQ(kmem_alloc(MEM_MAX_BLOCK, 0), 0);
}
