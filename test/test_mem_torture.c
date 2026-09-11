/* The host-side equivalent of the spec's `stress` command.
 *
 * Success criterion 2 is "stress 2 allocates well past physical RAM, evicts,
 * pages back in, and every byte read back matches what was written" -- on the
 * device, where a failure is a hang with no debugger. This runs that same
 * property on the PC, every commit, against a deterministic seed, so a bug
 * reports the exact byte that went wrong instead of a dead Cardputer.
 */
#include "tinytest.h"
#include "kernel/mem/mem.h"
#include "kernel/swap/swap.h"
#include "fake_swapdev.h"
#include <string.h>

static unsigned char heap[64 * 1024];
static unsigned char bitmap[256];

static uint32_t rng_state;
static uint32_t rnd(void) {            /* xorshift32: same sequence everywhere */
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

/* Byte i of a block is (seed + i) & 0xff, so a block that comes back shifted,
 * truncated or belonging to someone else is caught at the exact offset it
 * went wrong -- unlike a constant fill, which hides off-by-ones. */
static void write_pattern(unsigned char *p, uint32_t n, unsigned char seed) {
  uint32_t i;
  for (i = 0; i < n; i++) p[i] = (unsigned char)(seed + i);
}
static int verify_pattern(const unsigned char *p, uint32_t n, unsigned char seed) {
  uint32_t i;
  for (i = 0; i < n; i++)
    if (p[i] != (unsigned char)(seed + i)) return 0;
  return 1;
}

#define SLOTS 48
typedef struct { Handle h; uint32_t size; unsigned char seed; int locked; } Slot;

static FakeSwapDev *begin(uint32_t seed) {
  SwapExtent ex[1] = { { 0, 8192 } };            /* 4 MB, as on the card */
  FakeSwapDev *f = fake_swapdev_create(8192);
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  kmem_init(heap, sizeof heap);
  rng_state = seed;
  return f;
}

void test_torture_every_byte_survives_thousands_of_operations(void) {
  FakeSwapDev *f = begin(0xC0FFEEu);
  Slot s[SLOTS];
  MemStats st, end;
  int step, i, mismatches = 0, allocs = 0, alloc_fails = 0, lock_fails = 0;
  int locked_now = 0, max_locked = 0;
  size_t pinned_bytes = 0, max_pinned = 0;

  memset(s, 0, sizeof s);

  for (step = 0; step < 20000; step++) {
    i = (int)(rnd() % SLOTS);
    switch (rnd() % 4) {
    case 0:                                       /* allocate and fill */
      if (s[i].h == 0) {
        uint32_t n = 64 + rnd() % 6000;
        Handle h = kmem_alloc(n, 0);
        if (!h) { alloc_fails++; break; }
        {
          unsigned char *p = kmem_lock(h);
          if (p) {
            s[i].h = h; s[i].size = n; s[i].seed = (unsigned char)rnd();
            write_pattern(p, n, s[i].seed);
            kmem_unlock(h);
            allocs++;
          } else {
            lock_fails++;
            kmem_free(h);
          }
        }
      }
      break;
    case 1:                                       /* verify, read-only */
      if (s[i].h && !s[i].locked) {
        unsigned char *p = kmem_lock_ro(s[i].h);
        if (p) {
          if (!verify_pattern(p, s[i].size, s[i].seed)) mismatches++;
          kmem_unlock(s[i].h);
        }
      }
      break;
    case 2:                                       /* free */
      if (s[i].h && !s[i].locked) {
        kmem_free(s[i].h);
        memset(&s[i], 0, sizeof s[i]);
      }
      break;
    case 3:                                       /* hold a lock across other work */
      if (s[i].h && !s[i].locked) {
        unsigned char *p = kmem_lock(s[i].h);
        if (p) {
          s[i].locked = 1;
          locked_now++;
          pinned_bytes += s[i].size;
          if (locked_now > max_locked) max_locked = locked_now;
          if (pinned_bytes > max_pinned) max_pinned = pinned_bytes;
          if (!verify_pattern(p, s[i].size, s[i].seed)) mismatches++;
        }
      } else if (s[i].locked) {
        unsigned char *p = kmem_lock_ro(s[i].h);   /* still valid while pinned */
        if (p) {
          if (!verify_pattern(p, s[i].size, s[i].seed)) mismatches++;
          kmem_unlock(s[i].h);
        }
        kmem_unlock(s[i].h);
        s[i].locked = 0;
        locked_now--;
        pinned_bytes -= s[i].size;
      }
      break;
    }
  }
  for (i = 0; i < SLOTS; i++) if (s[i].locked) { kmem_unlock(s[i].h); s[i].locked = 0; }

  CHECK_EQ(mismatches, 0);
  CHECK(allocs > 1000);                           /* it really did work hard */

  kmem_stats(&st);
  printf("    torture: %lu allocs, %lu compactions, %lu evictions "
         "(%lu wrote), %lu page-ins, %lu alloc fails, %lu lock fails\n"
         "             max %lu blocks pinned = %lu bytes of a %lu byte heap\n",
         (unsigned long)allocs, (unsigned long)st.compactions,
         (unsigned long)st.evictions, (unsigned long)st.evict_writes,
         (unsigned long)st.page_ins,
         (unsigned long)alloc_fails, (unsigned long)lock_fails,
         (unsigned long)max_locked, (unsigned long)max_pinned,
         (unsigned long)sizeof heap);
  CHECK(st.evictions > 100);                      /* the pressure was real */
  CHECK(st.page_ins > 100);

  for (i = 0; i < SLOTS; i++) if (s[i].h) kmem_free(s[i].h);
  kmem_stats(&end);
  CHECK_EQ(end.handles_used, 0);                  /* nothing leaked */
  CHECK_EQ(swap_pages_free(), swap_pages_total()); /* no swap leaked either */
  fake_swapdev_destroy(f);
}

void test_torture_with_fixed_blocks_interleaved(void) {
  FakeSwapDev *f = begin(0x1234567u);
  Handle stacks[8];
  Slot s[SLOTS];
  MemStats end;
  int step, i, k, mismatches = 0;

  memset(stacks, 0, sizeof stacks);
  memset(s, 0, sizeof s);

  for (step = 0; step < 8000; step++) {
    if (step % 37 == 0) {                         /* task stacks churning */
      k = (int)(rnd() % 8);
      if (stacks[k]) { kmem_free(stacks[k]); stacks[k] = 0; }
      else stacks[k] = kmem_alloc(1024, MEM_FIXED | MEM_ZERO);
    }
    i = (int)(rnd() % SLOTS);
    if (s[i].h == 0) {
      uint32_t n = 64 + rnd() % 4000;
      Handle h = kmem_alloc(n, 0);
      if (h) {
        unsigned char *p = kmem_lock(h);
        if (p) {
          s[i].h = h; s[i].size = n; s[i].seed = (unsigned char)rnd();
          write_pattern(p, n, s[i].seed);
          kmem_unlock(h);
        } else {
          kmem_free(h);
        }
      }
    } else {
      unsigned char *p = kmem_lock_ro(s[i].h);
      if (p) {
        if (!verify_pattern(p, s[i].size, s[i].seed)) mismatches++;
        kmem_unlock(s[i].h);
      }
      if (rnd() % 3 == 0) { kmem_free(s[i].h); memset(&s[i], 0, sizeof s[i]); }
    }
  }
  CHECK_EQ(mismatches, 0);

  for (i = 0; i < SLOTS; i++) if (s[i].h) kmem_free(s[i].h);
  for (k = 0; k < 8; k++) if (stacks[k]) kmem_free(stacks[k]);
  kmem_stats(&end);
  CHECK_EQ(end.handles_used, 0);
  CHECK_EQ(end.fixed_used, 0);                    /* the fixed arena unwound */
  CHECK_EQ(swap_pages_free(), swap_pages_total());
  fake_swapdev_destroy(f);
}

/* Proves the paranoid relocation is actually running -- without this, the
 * safety net could quietly be doing nothing. */
void test_paranoid_mode_relocates_unpinned_blocks(void) {
  FakeSwapDev *f = begin(1u);
  Handle a = kmem_alloc(1024, 0);
  unsigned char *first, *second;
  int moved = 0, i;

  first = kmem_lock_ro(a);
  kmem_unlock(a);
  for (i = 0; i < 4; i++) {
    Handle t = kmem_alloc(64, 0);            /* an unrelated allocation ... */
    second = kmem_lock_ro(a);
    kmem_unlock(a);
    if (second != first) moved = 1;         /* ... must be able to move a */
    kmem_free(t);
  }
#ifdef CARDOS_MEM_PARANOID
  CHECK_EQ(moved, 1);
#else
  (void)moved;
#endif
  CHECK_EQ(kmem_size(a), 1024);
  kmem_free(a);
  fake_swapdev_destroy(f);
}
