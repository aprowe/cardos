#include "tinytest.h"
#include "mem/mem.h"
#include <string.h>

static unsigned char heap[64 * 1024];

static void fill(Handle h, unsigned char v) {
  unsigned char *p = mem_lock(h); memset(p, v, mem_size(h)); mem_unlock(h);
}
static int check_fill(Handle h, unsigned char v) {
  unsigned char *p = mem_lock(h); if (!p) return 0;
  for (size_t i = 0; i < mem_size(h); i++) if (p[i] != v) { mem_unlock(h); return 0; }
  mem_unlock(h); return 1;
}

void test_compaction_closes_a_gap_and_preserves_contents(void) {
  mem_init(heap, sizeof heap);
  Handle a = mem_alloc(4096, 0), b = mem_alloc(4096, 0), c = mem_alloc(4096, 0);
  fill(a, 0xA1); fill(b, 0xB2); fill(c, 0xC3);
  mem_free(b);
  MemStats before; mem_stats(&before);
  mem_compact();
  MemStats after; mem_stats(&after);
  CHECK(after.largest_free > before.largest_free);
  CHECK_EQ(after.compactions, before.compactions + 1);
  CHECK_EQ(check_fill(a, 0xA1), 1);
  CHECK_EQ(check_fill(c, 0xC3), 1);
}

void test_compaction_does_not_move_a_locked_block(void) {
  mem_init(heap, sizeof heap);
  Handle a = mem_alloc(4096, 0), b = mem_alloc(4096, 0), c = mem_alloc(4096, 0);
  fill(a, 0xA1);
  unsigned char *pc = mem_lock(c);
  memset(pc, 0xC3, 4096);
  mem_free(b);
  mem_compact();
  CHECK_EQ(mem_lock(c) == pc, 1);
  CHECK_EQ(pc[4095], 0xC3);
  mem_unlock(c); mem_unlock(c);
  CHECK_EQ(check_fill(a, 0xA1), 1);
}

void test_alloc_compacts_automatically_when_it_would_otherwise_fail(void) {
  mem_init(heap, sizeof heap);
  Handle h[8];
  for (int i = 0; i < 8; i++) { h[i] = mem_alloc(7000, 0); CHECK(h[i] != 0); }
  for (int i = 0; i < 8; i += 2) mem_free(h[i]);
  MemStats before; mem_stats(&before);
  Handle big = mem_alloc(20000, 0);
  CHECK(big != 0);
  MemStats after; mem_stats(&after);
  CHECK(after.compactions > before.compactions);
}

void test_compaction_is_a_noop_when_there_are_no_gaps(void) {
  mem_init(heap, sizeof heap);
  Handle a = mem_alloc(4096, 0);
  fill(a, 0x11);
  unsigned char *before = mem_lock(a); mem_unlock(a);
  mem_compact();
  CHECK_EQ(mem_lock(a) == before, 1); mem_unlock(a);
  CHECK_EQ(check_fill(a, 0x11), 1);
}
