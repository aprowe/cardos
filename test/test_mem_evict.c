#include "tinytest.h"
#include "kernel/mem/mem.h"
#include "kernel/swap/swap.h"
#include "fake_swapdev.h"
#include <string.h>

static unsigned char heap[64 * 1024];
static unsigned char bitmap[128];
static FakeSwapDev *g_f;

static void setup_heap(size_t heap_bytes) {
  SwapExtent ex[1] = { { 0, 8192 } };            /* 1024 pages = 4 MB */
  if (g_f) fake_swapdev_destroy(g_f);
  g_f = fake_swapdev_create(8192);
  swap_init(fake_swapdev_dev(g_f), ex, 1, bitmap, sizeof bitmap);
  kmem_init(heap, heap_bytes);
}

static void setup(void) { setup_heap(sizeof heap); }

static void fill(Handle h, unsigned char v) {
  unsigned char *p = kmem_lock(h);
  if (p) { memset(p, v, kmem_size(h)); kmem_unlock(h); }
}

static int check_fill(Handle h, unsigned char v) {
  size_t i, n = kmem_size(h);
  unsigned char *p = kmem_lock_ro(h);      /* read-only: does not dirty */
  if (!p) return 0;
  for (i = 0; i < n; i++)
    if (p[i] != v) { kmem_unlock(h); return 0; }
  kmem_unlock(h);
  return 1;
}

void test_allocation_pressure_evicts_and_page_in_restores_bytes(void) {
  int i;
  Handle h[12];
  MemStats s;
  setup();
  for (i = 0; i < 12; i++) {          /* 12 x 8 KB = 96 KB into a 64 KB heap */
    h[i] = kmem_alloc(8192, 0);
    CHECK(h[i] != 0);
    fill(h[i], (unsigned char)(0x40 + i));
  }
  kmem_stats(&s);
  CHECK(s.evictions > 0);
  for (i = 0; i < 12; i++)            /* every byte survives the round trip */
    CHECK_EQ(check_fill(h[i], (unsigned char)(0x40 + i)), 1);
  kmem_stats(&s);
  CHECK(s.page_ins > 0);
}

void test_least_recently_used_block_is_evicted_first(void) {
  Handle a, b, c;
  setup_heap(40 * 1024);
  a = kmem_alloc(16384, 0);
  b = kmem_alloc(16384, 0);
  fill(a, 0xAA);
  fill(b, 0xBB);
  CHECK_EQ(check_fill(a, 0xAA), 1);   /* a becomes the most recently used */
  c = kmem_alloc(16384, 0);            /* forces one of them out */
  CHECK(c != 0);
  CHECK_EQ(kmem_resident(b), 0);       /* b was least recently used */
  CHECK_EQ(kmem_resident(a), 1);
  CHECK_EQ(check_fill(b, 0xBB), 1);   /* and it comes back intact */
}

void test_locked_blocks_are_never_evicted(void) {
  Handle pinned;
  unsigned char *p;
  int i;
  setup();
  pinned = kmem_alloc(16384, 0);
  p = kmem_lock(pinned);
  CHECK(p != NULL);
  memset(p, 0x77, 16384);
  for (i = 0; i < 8; i++) kmem_alloc(8192, 0);
  CHECK_EQ(kmem_resident(pinned), 1);
  CHECK_EQ(p[16383], 0x77);           /* the pointer is still good */
  CHECK_EQ(kmem_lock(pinned) == p, 1);
  kmem_unlock(pinned);
  kmem_unlock(pinned);
}

void test_fixed_blocks_are_never_evicted(void) {
  Handle fx;
  int i;
  setup();
  fx = kmem_alloc(8192, MEM_FIXED);
  fill(fx, 0x33);
  for (i = 0; i < 8; i++) kmem_alloc(8192, 0);
  CHECK_EQ(kmem_resident(fx), 1);
  CHECK_EQ(check_fill(fx, 0x33), 1);
}

/* A block paged in and only read keeps a valid copy in swap, so evicting it
 * again costs nothing. That is what makes read-mostly data -- the console
 * scrollback the spec uses as its demo -- cheap to hold. */
void test_read_only_sweeps_evict_without_rewriting_swap(void) {
  int i, pass;
  Handle h[12];
  MemStats before, after;
  setup();
  for (i = 0; i < 12; i++) {
    h[i] = kmem_alloc(8192, 0);
    fill(h[i], (unsigned char)(0x40 + i));      /* dirty: first eviction writes */
  }
  for (i = 0; i < 12; i++) CHECK_EQ(check_fill(h[i], (unsigned char)(0x40 + i)), 1);
  kmem_stats(&before);

  for (pass = 0; pass < 3; pass++)              /* read-only sweeps */
    for (i = 0; i < 12; i++)
      CHECK_EQ(check_fill(h[i], (unsigned char)(0x40 + i)), 1);

  kmem_stats(&after);
  CHECK(after.evictions > before.evictions);    /* the sweeps did evict ... */
  CHECK_EQ(after.evict_writes, before.evict_writes);  /* ... and wrote nothing */
}

void test_read_write_sweeps_do_rewrite_swap(void) {
  int i, pass;
  Handle h[12];
  MemStats before, after;
  setup();
  for (i = 0; i < 12; i++) {
    h[i] = kmem_alloc(8192, 0);
    fill(h[i], (unsigned char)(0x40 + i));
  }
  for (i = 0; i < 12; i++) CHECK_EQ(check_fill(h[i], (unsigned char)(0x40 + i)), 1);
  kmem_stats(&before);

  for (pass = 0; pass < 3; pass++)              /* writable sweeps */
    for (i = 0; i < 12; i++)
      fill(h[i], (unsigned char)(0x80 + i));

  kmem_stats(&after);
  CHECK(after.evict_writes > before.evict_writes);
  for (i = 0; i < 12; i++)                      /* the new contents survive */
    CHECK_EQ(check_fill(h[i], (unsigned char)(0x80 + i)), 1);
}

void test_lock_returns_null_when_page_in_cannot_be_satisfied(void) {
  Handle victim, hogs[2];
  unsigned char *p0, *p1;
  int i;
  setup();
  victim = kmem_alloc(MEM_MAX_BLOCK, 0);
  fill(victim, 0xD1);
  for (i = 0; i < 2; i++) {
    hogs[i] = kmem_alloc(MEM_MAX_BLOCK, 0);
    CHECK(hogs[i] != 0);
  }
  CHECK_EQ(kmem_resident(victim), 0);  /* pushed out by the two hogs */
  p0 = kmem_lock(hogs[0]);             /* pin the entire heap */
  p1 = kmem_lock(hogs[1]);
  CHECK(p0 != NULL);
  CHECK(p1 != NULL);
  CHECK(kmem_lock(victim) == NULL);    /* no room, and it says so */
  kmem_unlock(hogs[0]);
  kmem_unlock(hogs[1]);
  CHECK(kmem_lock(victim) != NULL);    /* room again once unpinned */
  kmem_unlock(victim);
  CHECK_EQ(check_fill(victim, 0xD1), 1);
}

void test_freeing_a_swapped_block_releases_its_swap_pages(void) {
  Handle a, b, c;
  uint16_t free_before;
  setup();
  a = kmem_alloc(16384, 0);
  fill(a, 0x5E);
  b = kmem_alloc(MEM_MAX_BLOCK, 0);
  c = kmem_alloc(MEM_MAX_BLOCK, 0);
  CHECK(b != 0);
  CHECK(c != 0);
  CHECK_EQ(kmem_resident(a), 0);
  free_before = swap_pages_free();
  kmem_free(a);
  CHECK(swap_pages_free() > free_before);
}

void test_swapped_and_resident_bytes_are_reported(void) {
  Handle a, b, c;
  MemStats s;
  setup();
  a = kmem_alloc(16384, 0);
  fill(a, 0x01);
  b = kmem_alloc(MEM_MAX_BLOCK, 0);
  c = kmem_alloc(MEM_MAX_BLOCK, 0);
  CHECK(b != 0);
  CHECK(c != 0);
  kmem_stats(&s);
  CHECK_EQ(s.swapped_bytes, 16384);
  CHECK_EQ(s.resident_bytes, 2 * MEM_MAX_BLOCK);
}

/* A write that fails should cost that one block its turn, not end eviction:
 * the loop used to stop at the first block it could not push out, and the
 * allocation failed with candidates still on the list. Worse, the same block
 * stayed at the head, so every later allocation under pressure hit the same
 * failure. Here the least recently used block's write fails once and the
 * allocation succeeds by evicting the next one. */
void test_a_failed_swap_write_moves_on_to_the_next_victim(void) {
  Handle a, b, c;
  setup_heap(40 * 1024);
  a = kmem_alloc(16384, 0);
  b = kmem_alloc(16384, 0);
  fill(a, 0xAA);
  fill(b, 0xBB);
  CHECK_EQ(check_fill(a, 0xAA), 1);         /* b is the first victim */
  fake_swapdev_fail_next_write(g_f);
  c = kmem_alloc(16384, 0);
  CHECK(c != 0);
  CHECK_EQ(kmem_resident(b), 1);            /* b's write failed: it stayed */
  CHECK_EQ(kmem_resident(a), 0);            /* a went instead */
  CHECK_EQ(check_fill(a, 0xAA), 1);
  CHECK_EQ(check_fill(b, 0xBB), 1);
}
