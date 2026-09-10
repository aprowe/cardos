#include "tinytest.h"
#include "swap/swap.h"
#include "fake_swapdev.h"

static unsigned char bitmap[128];

void test_single_extent_maps_pages_linearly(void) {
  FakeSwapDev *f = fake_swapdev_create(4096);
  SwapExtent ex[1] = { { 1000, 2048 } };          /* 2048 sectors = 256 pages */
  CHECK_EQ(swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap), 0);
  CHECK_EQ(swap_pages_total(), 256);
  CHECK_EQ(swap_page_to_sector(0), 1000);
  CHECK_EQ(swap_page_to_sector(1), 1008);
  CHECK_EQ(swap_page_to_sector(255), 1000 + 255 * 8);
  fake_swapdev_destroy(f);
}

void test_split_extents_map_across_the_boundary(void) {
  FakeSwapDev *f = fake_swapdev_create(8192);
  SwapExtent ex[2] = { { 100, 800 }, { 5000, 1248 } };   /* 800 + 1248 = 2048 */
  CHECK_EQ(swap_init(fake_swapdev_dev(f), ex, 2, bitmap, sizeof bitmap), 0);
  CHECK_EQ(swap_pages_total(), 256);
  CHECK_EQ(swap_page_to_sector(99), 100 + 99 * 8);       /* last page of ex[0] */
  CHECK_EQ(swap_page_to_sector(100), 5000);              /* first page of ex[1] */
  CHECK_EQ(swap_page_to_sector(101), 5008);
  fake_swapdev_destroy(f);
}

void test_extent_not_a_whole_number_of_pages_is_rejected(void) {
  FakeSwapDev *f = fake_swapdev_create(4096);
  SwapExtent ex[1] = { { 100, 803 } };            /* 803 is not divisible by 8 */
  CHECK(swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap) != 0);
  fake_swapdev_destroy(f);
}

void test_out_of_range_page_is_rejected(void) {
  FakeSwapDev *f = fake_swapdev_create(4096);
  SwapExtent ex[1] = { { 1000, 2048 } };
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  CHECK_EQ(swap_page_to_sector(256), SWAP_BAD_SECTOR);
  CHECK_EQ(swap_page_to_sector(SWAP_INVALID_PAGE), SWAP_BAD_SECTOR);
  fake_swapdev_destroy(f);
}

void test_too_many_extents_is_rejected(void) {
  FakeSwapDev *f = fake_swapdev_create(65536);
  SwapExtent ex[SWAP_MAX_EXTENTS + 1];
  int i;
  for (i = 0; i < SWAP_MAX_EXTENTS + 1; i++) {
    ex[i].start_sector = (uint32_t)(i * 512);
    ex[i].sector_count = 64;
  }
  CHECK(swap_init(fake_swapdev_dev(f), ex, SWAP_MAX_EXTENTS + 1,
                  bitmap, sizeof bitmap) != 0);
  fake_swapdev_destroy(f);
}
