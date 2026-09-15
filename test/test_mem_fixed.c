#include "tinytest.h"
#include "kernel/mem/mem.h"
#include <string.h>

static uint32_t heap_words[16 * 1024];
#define heap ((unsigned char *)heap_words)

void test_fixed_blocks_sit_above_movable_blocks(void) {
  kmem_init(heap, sizeof heap_words);
  Handle m = kmem_alloc(1024, 0);
  Handle f = kmem_alloc(1024, MEM_FIXED);
  unsigned char *pm = kmem_lock(m), *pf = kmem_lock(f);
  CHECK(pf > pm);
  kmem_unlock(m); kmem_unlock(f);
}

void test_fixed_block_never_moves_across_a_compaction(void) {
  kmem_init(heap, sizeof heap_words);
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
  kmem_init(heap, sizeof heap_words);
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
  kmem_init(heap, sizeof heap_words);
  while (kmem_alloc(MEM_MAX_BLOCK, MEM_FIXED)) { }
  MemStats s; kmem_stats(&s);
  CHECK(s.movable_used + s.fixed_used <= s.heap_size);
  CHECK_EQ(kmem_alloc(MEM_MAX_BLOCK, 0), 0);
}

/* Odd sizes, freed and reused. The bump path aligned a fresh fixed block
 * down, but a freed range kept the block's unrounded size, so carving the
 * next block out of it by first fit could land it on any byte: a task stack
 * or a DMA buffer two bytes off a word boundary. Every fixed block, however
 * it was found, sits on MEM_ALIGN. */
void test_fixed_blocks_reused_from_odd_sized_holes_stay_aligned(void) {
  Handle h[8];
  int i;
  kmem_init(heap, sizeof heap_words);
  h[0] = kmem_alloc(101, MEM_FIXED);
  h[1] = kmem_alloc(7, MEM_FIXED);
  h[2] = kmem_alloc(33, MEM_FIXED);
  kmem_free(h[0]); kmem_free(h[1]); kmem_free(h[2]);
  h[3] = kmem_alloc(8, MEM_FIXED);
  h[4] = kmem_alloc(50, MEM_FIXED);
  h[5] = kmem_alloc(40, MEM_FIXED);
  h[6] = kmem_alloc(9, MEM_FIXED);
  for (i = 3; i <= 6; i++) {
    unsigned char *p;
    CHECK(h[i] != 0);
    p = kmem_lock(h[i]);
    CHECK_EQ((unsigned)((uintptr_t)p & 3u), 0u);
    kmem_unlock(h[i]);
  }
}

/* And once everything odd-sized is freed again, the arena is empty: the
 * padding above each block used to be stranded forever. */
void test_freeing_every_odd_fixed_block_empties_the_arena(void) {
  Handle a, b, c;
  MemStats s;
  kmem_init(heap, sizeof heap_words);
  a = kmem_alloc(101, MEM_FIXED);
  b = kmem_alloc(7, MEM_FIXED);
  c = kmem_alloc(33, MEM_FIXED);
  kmem_free(b); kmem_free(a); kmem_free(c);
  kmem_stats(&s);
  CHECK_EQ(s.fixed_used, 0u);
}
