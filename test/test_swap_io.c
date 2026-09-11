#include "tinytest.h"
#include "kernel/swap/swap.h"
#include "fake_swapdev.h"
#include <string.h>

static unsigned char bitmap[128];
static unsigned char src[9000], dst[9000];

/* 2048 sectors of payload starting at 64, on a device with room for both. */
static FakeSwapDev *setup(void) {
  SwapExtent ex[1] = { { 64, 2048 } };
  FakeSwapDev *f = fake_swapdev_create(4096);
  swap_init(fake_swapdev_dev(f), ex, 1, bitmap, sizeof bitmap);
  return f;
}

void test_round_trip_of_a_page_aligned_block(void) {
  FakeSwapDev *f = setup();
  int i;
  for (i = 0; i < 8192; i++) src[i] = (unsigned char)(i * 31 + 7);
  uint16_t p = swap_alloc_pages(2);
  CHECK_EQ(swap_write(p, src, 8192), 0);
  memset(dst, 0, sizeof dst);
  CHECK_EQ(swap_read(p, dst, 8192), 0);
  CHECK_EQ(memcmp(src, dst, 8192), 0);
  CHECK_EQ(fake_swapdev_faults(f), 0);
  fake_swapdev_destroy(f);
}

void test_round_trip_of_a_ragged_block_stays_inside_the_buffer(void) {
  FakeSwapDev *f = setup();
  int i;
  for (i = 0; i < 4700; i++) src[i] = (unsigned char)(i ^ 0x5A);
  uint16_t p = swap_alloc_pages(2);
  CHECK_EQ(swap_write(p, src, 4700), 0);      /* 9 sectors plus 92 bytes */
  memset(dst, 0xEE, sizeof dst);
  CHECK_EQ(swap_read(p, dst, 4700), 0);
  CHECK_EQ(memcmp(src, dst, 4700), 0);
  CHECK_EQ(dst[4700], 0xEE);                  /* nothing written past the end */
  CHECK_EQ(dst[4800], 0xEE);
  fake_swapdev_destroy(f);
}

void test_a_single_byte_block_round_trips(void) {
  FakeSwapDev *f = setup();
  uint16_t p = swap_alloc_pages(1);
  src[0] = 0x42;
  CHECK_EQ(swap_write(p, src, 1), 0);
  memset(dst, 0, sizeof dst);
  CHECK_EQ(swap_read(p, dst, 1), 0);
  CHECK_EQ(dst[0], 0x42);
  fake_swapdev_destroy(f);
}

void test_two_blocks_in_adjacent_pages_do_not_bleed(void) {
  FakeSwapDev *f = setup();
  uint16_t a = swap_alloc_pages(1), b = swap_alloc_pages(1);
  memset(src, 0x11, 4096);
  CHECK_EQ(swap_write(a, src, 4096), 0);
  memset(src, 0x22, 4096);
  CHECK_EQ(swap_write(b, src, 4096), 0);
  memset(dst, 0, sizeof dst);
  CHECK_EQ(swap_read(a, dst, 4096), 0);
  CHECK_EQ(dst[0], 0x11);
  CHECK_EQ(dst[4095], 0x11);
  fake_swapdev_destroy(f);
}

void test_write_past_the_end_of_swap_is_refused(void) {
  FakeSwapDev *f = setup();
  CHECK(swap_write(255, src, 8192) != 0);     /* 2 pages starting at the last */
  CHECK(swap_read(255, dst, 8192) != 0);
  CHECK_EQ(fake_swapdev_faults(f), 0);        /* refused before touching the device */
  fake_swapdev_destroy(f);
}

void test_device_errors_are_propagated(void) {
  FakeSwapDev *f = setup();
  fake_swapdev_fail_next_write(f);
  CHECK(swap_write(0, src, 4096) != 0);
  fake_swapdev_destroy(f);
}
