#include "tinytest.h"
#include "swap/swap.h"
#include "fake_swapdev.h"

static unsigned char bitmap[128];
static FakeSwapDev *g_f;

static void setup(void) {
  SwapExtent ex[1] = { { 0, 2048 } };             /* 256 pages */
  if (g_f) fake_swapdev_destroy(g_f);
  g_f = fake_swapdev_create(2048);
  swap_init(fake_swapdev_dev(g_f), ex, 1, bitmap, sizeof bitmap);
}

void test_pages_are_allocated_contiguously_and_distinctly(void) {
  setup();
  uint16_t a = swap_alloc_pages(3), b = swap_alloc_pages(2);
  CHECK_EQ(a, 0);
  CHECK_EQ(b, 3);
  CHECK_EQ(swap_pages_free(), 256 - 5);
}

void test_freed_run_is_reused(void) {
  setup();
  uint16_t a = swap_alloc_pages(4);
  swap_alloc_pages(1);
  swap_free_pages(a, 4);
  CHECK_EQ(swap_pages_free(), 256 - 1);
  CHECK_EQ(swap_alloc_pages(4), a);               /* first fit reclaims the hole */
}

void test_a_run_never_straddles_an_allocated_page(void) {
  setup();
  uint16_t a = swap_alloc_pages(2);               /* pages 0-1 */
  uint16_t b = swap_alloc_pages(2);               /* pages 2-3 */
  uint16_t c = swap_alloc_pages(2);               /* pages 4-5 */
  CHECK_EQ(a, 0); CHECK_EQ(b, 2); CHECK_EQ(c, 4);
  swap_free_pages(a, 2);                          /* hole at 0-1, too small */
  swap_free_pages(c, 2);                          /* free from 4 upward */
  /* A run of 4 cannot fit in the 2-page hole at 0 and must not straddle the
   * still-allocated pages 2-3, so it has to start at 4. */
  CHECK_EQ(swap_alloc_pages(4), 4);
}

void test_exhaustion_returns_invalid_page(void) {
  setup();
  int n = 0;
  while (swap_alloc_pages(8) != SWAP_INVALID_PAGE) n++;
  CHECK_EQ(n, 32);                                /* 256 pages / 8 */
  CHECK_EQ(swap_pages_free(), 0);
  CHECK_EQ(swap_alloc_pages(1), SWAP_INVALID_PAGE);
}

void test_zero_length_run_is_refused(void) {
  setup();
  CHECK_EQ(swap_alloc_pages(0), SWAP_INVALID_PAGE);
  CHECK_EQ(swap_pages_free(), 256);
}

void test_bitmap_too_small_is_rejected_at_init(void) {
  FakeSwapDev *f = fake_swapdev_create(2048);
  SwapExtent ex[1] = { { 0, 2048 } };
  unsigned char tiny[4];                          /* 256 pages needs 32 bytes */
  CHECK(swap_init(fake_swapdev_dev(f), ex, 1, tiny, sizeof tiny) != 0);
  fake_swapdev_destroy(f);
}
