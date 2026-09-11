#include "tinytest.h"
#include "kernel/mem/mem.h"
#include <string.h>

static unsigned char heap[64 * 1024];

static void fill(Handle h, unsigned char v) {
  unsigned char *p = kmem_lock(h); memset(p, v, kmem_size(h)); kmem_unlock(h);
}
static int check_fill(Handle h, unsigned char v) {
  unsigned char *p = kmem_lock(h); if (!p) return 0;
  for (size_t i = 0; i < kmem_size(h); i++) if (p[i] != v) { kmem_unlock(h); return 0; }
  kmem_unlock(h); return 1;
}

void test_compaction_closes_a_gap_and_preserves_contents(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(4096, 0), b = kmem_alloc(4096, 0), c = kmem_alloc(4096, 0);
  fill(a, 0xA1); fill(b, 0xB2); fill(c, 0xC3);
  kmem_free(b);
  MemStats before; kmem_stats(&before);
  kmem_compact();
  MemStats after; kmem_stats(&after);
  CHECK(after.largest_free > before.largest_free);
  CHECK_EQ(after.compactions, before.compactions + 1);
  CHECK_EQ(check_fill(a, 0xA1), 1);
  CHECK_EQ(check_fill(c, 0xC3), 1);
}

void test_compaction_does_not_move_a_locked_block(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(4096, 0), b = kmem_alloc(4096, 0), c = kmem_alloc(4096, 0);
  fill(a, 0xA1);
  unsigned char *pc = kmem_lock(c);
  memset(pc, 0xC3, 4096);
  kmem_free(b);
  kmem_compact();
  CHECK_EQ(kmem_lock(c) == pc, 1);
  CHECK_EQ(pc[4095], 0xC3);
  kmem_unlock(c); kmem_unlock(c);
  CHECK_EQ(check_fill(a, 0xA1), 1);
}

void test_alloc_compacts_automatically_when_it_would_otherwise_fail(void) {
  kmem_init(heap, sizeof heap);
  Handle h[8];
  for (int i = 0; i < 8; i++) { h[i] = kmem_alloc(7000, 0); CHECK(h[i] != 0); }
  for (int i = 0; i < 8; i += 2) kmem_free(h[i]);
  MemStats before; kmem_stats(&before);
  Handle big = kmem_alloc(20000, 0);
  CHECK(big != 0);
  MemStats after; kmem_stats(&after);
  CHECK(after.compactions > before.compactions);
}

void test_compaction_is_a_noop_when_there_are_no_gaps(void) {
  kmem_init(heap, sizeof heap);
  Handle a = kmem_alloc(4096, 0);
  fill(a, 0x11);
  unsigned char *before = kmem_lock(a); kmem_unlock(a);
  kmem_compact();
  CHECK_EQ(kmem_lock(a) == before, 1); kmem_unlock(a);
  CHECK_EQ(check_fill(a, 0x11), 1);
}
