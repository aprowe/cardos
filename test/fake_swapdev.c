#include "fake_swapdev.h"

#include <stdlib.h>
#include <string.h>

struct FakeSwapDev {
  unsigned char *store;
  uint32_t       sectors;
  unsigned long  writes, reads, faults;
  int            fail_next_write;
  SwapDev        dev;
};

static int fake_read(void *ctx, uint32_t sector, void *buf, uint32_t count) {
  FakeSwapDev *f = (FakeSwapDev *)ctx;
  if (sector + count > f->sectors) { f->faults++; return -1; }
  memcpy(buf, f->store + (size_t)sector * SWAP_SECTOR_SIZE,
         (size_t)count * SWAP_SECTOR_SIZE);
  f->reads += count;
  return 0;
}

static int fake_write(void *ctx, uint32_t sector, const void *buf, uint32_t count) {
  FakeSwapDev *f = (FakeSwapDev *)ctx;
  if (f->fail_next_write) { f->fail_next_write = 0; return -1; }
  if (sector + count > f->sectors) { f->faults++; return -1; }
  memcpy(f->store + (size_t)sector * SWAP_SECTOR_SIZE, buf,
         (size_t)count * SWAP_SECTOR_SIZE);
  f->writes += count;
  return 0;
}

FakeSwapDev *fake_swapdev_create(uint32_t sectors) {
  FakeSwapDev *f = (FakeSwapDev *)calloc(1, sizeof *f);
  f->store = (unsigned char *)malloc((size_t)sectors * SWAP_SECTOR_SIZE);
  /* Poison, so a read of a sector that was never written is visibly wrong
   * rather than conveniently zero. */
  memset(f->store, 0xA5, (size_t)sectors * SWAP_SECTOR_SIZE);
  f->sectors = sectors;
  f->dev.read = fake_read;
  f->dev.write = fake_write;
  f->dev.ctx = f;
  return f;
}

void fake_swapdev_destroy(FakeSwapDev *f) {
  if (!f) return;
  free(f->store);
  free(f);
}

const SwapDev *fake_swapdev_dev(FakeSwapDev *f) { return &f->dev; }

unsigned long fake_swapdev_writes(FakeSwapDev *f) { return f->writes; }
unsigned long fake_swapdev_reads(FakeSwapDev *f)  { return f->reads; }
unsigned long fake_swapdev_faults(FakeSwapDev *f) { return f->faults; }

void fake_swapdev_fail_next_write(FakeSwapDev *f) { f->fail_next_write = 1; }
