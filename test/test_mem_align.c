/* Block alignment.
 *
 * Offsets used to advance by the raw requested size, so a block allocated
 * after an odd-sized one started on an odd address. On x86 that is invisible;
 * on Xtensa a 32-bit load from a misaligned address is an alignment exception,
 * and the first thing a caller does with a block is usually to put a struct in
 * it. The host suite cannot feel the fault, so it asserts the property
 * instead.
 */
#include "tinytest.h"
#include "kernel/mem/mem.h"
#include <stdint.h>
#include <string.h>

/* uint32_t so the heap itself is aligned; otherwise this proves nothing. */
static uint32_t heap_words[16 * 1024 / 4];
#define HEAP ((unsigned char *)heap_words)

#define IS_ALIGNED(p) ((((uintptr_t)(p)) & 3u) == 0)

void test_every_block_is_four_byte_aligned(void) {
  /* Sizes chosen to be maximally awkward: each leaves a different remainder. */
  static const uint32_t sizes[] = { 1, 2, 3, 5, 7, 13, 31, 33, 61, 100, 127 };
  Handle h[sizeof sizes / sizeof sizes[0]];
  size_t i;

  mem_init(HEAP, sizeof heap_words);
  for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    void *p;
    h[i] = mem_alloc(sizes[i], 0);
    CHECK(h[i] != 0);
    p = mem_lock(h[i]);
    CHECK(p != NULL);
    CHECK(IS_ALIGNED(p));
    if (!IS_ALIGNED(p))
      printf("      size %u landed at offset %u\n", (unsigned)sizes[i],
             (unsigned)((unsigned char *)p - HEAP));
    mem_unlock(h[i]);
  }
}

void test_alignment_survives_compaction(void) {
  Handle a, b, c;
  void *p;
  mem_init(HEAP, sizeof heap_words);
  a = mem_alloc(37, 0);
  b = mem_alloc(41, 0);
  c = mem_alloc(53, 0);
  mem_free(b);
  mem_compact();
  p = mem_lock(a); CHECK(IS_ALIGNED(p)); mem_unlock(a);
  p = mem_lock(c); CHECK(IS_ALIGNED(p)); mem_unlock(c);
}

void test_fixed_blocks_are_aligned_too(void) {
  /* Task stacks are MEM_FIXED, and a misaligned stack pointer is not a bug
   * that gets diagnosed politely. */
  Handle a, b;
  void *p;
  mem_init(HEAP, sizeof heap_words);
  a = mem_alloc(101, MEM_FIXED);
  b = mem_alloc(7, MEM_FIXED);
  p = mem_lock(a); CHECK(IS_ALIGNED(p)); mem_unlock(a);
  p = mem_lock(b); CHECK(IS_ALIGNED(p)); mem_unlock(b);
}

void test_alignment_survives_eviction_and_page_in(void) {
  Handle h[10];
  int i;
  mem_init(HEAP, sizeof heap_words);
  for (i = 0; i < 10; i++) {
    h[i] = mem_alloc(1000 + (uint32_t)i * 7, 0);   /* deliberately ragged */
    if (h[i]) { void *p = mem_lock(h[i]); if (p) { CHECK(IS_ALIGNED(p)); mem_unlock(h[i]); } }
  }
  for (i = 0; i < 10; i++) {
    if (!h[i]) continue;
    {
      void *p = mem_lock_ro(h[i]);
      if (p) { CHECK(IS_ALIGNED(p)); mem_unlock(h[i]); }
    }
  }
}

/* Padding is the cost of alignment; make sure it is bounded and not
 * accidentally rounding a whole block up. */
void test_reported_size_is_the_requested_size(void) {
  Handle a;
  mem_init(HEAP, sizeof heap_words);
  a = mem_alloc(37, 0);
  CHECK_EQ(mem_size(a), 37);      /* not 40 */
}
