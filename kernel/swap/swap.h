/* CardOS swap: a bitmap page allocator over a raw sector device.
 *
 * On the device the backing store is /cardos/swap.img on the SD card, reached
 * with raw disk_read/disk_write on sectors so the filesystem is bypassed on
 * the hot path. Here that device is behind a SwapDev function-pointer
 * interface, so the host tests run the real allocator against RAM.
 *
 * Page to sector goes through an extent table rather than a single start
 * sector. A one-extent table is exactly the contiguous case the spec
 * describes, so nothing is lost -- but nothing depends on f_expand() being
 * compiled into ESP-IDF's FatFs, and a fragmented card still works.
 */
#ifndef CARDOS_SWAP_H
#define CARDOS_SWAP_H

#include <stddef.h>
#include <stdint.h>

#define SWAP_SECTOR_SIZE      512u
#define SWAP_PAGE_SIZE        4096u
#define SWAP_SECTORS_PER_PAGE (SWAP_PAGE_SIZE / SWAP_SECTOR_SIZE)  /* 8 */

#define SWAP_MAX_EXTENTS 8
#define SWAP_INVALID_PAGE ((uint16_t)0xFFFFu)
#define SWAP_BAD_SECTOR   ((uint32_t)0xFFFFFFFFu)

/* Returns 0 on success, non-zero on a device error. */
typedef struct {
  int (*read)(void *ctx, uint32_t sector, void *buf, uint32_t count);
  int (*write)(void *ctx, uint32_t sector, const void *buf, uint32_t count);
  void *ctx;
} SwapDev;

/* One unbroken run of sectors belonging to the swap image. */
typedef struct {
  uint32_t start_sector;
  uint32_t sector_count;   /* must be a whole number of pages */
} SwapExtent;

typedef struct {
  uint16_t pages_total;
  uint16_t pages_free;
  uint32_t page_writes;
  uint32_t page_reads;
} SwapStats;

/* The caller supplies the bitmap storage so swap allocates nothing itself.
 * Returns 0 on success. The bitmap is zeroed here, so pages left behind by a
 * previous boot can never be mistaken for live data. */
int swap_init(const SwapDev *dev, const SwapExtent *extents, int n_extents,
              void *bitmap, size_t bitmap_bytes);

uint32_t swap_page_to_sector(uint16_t page);

uint16_t swap_alloc_pages(uint16_t n);          /* SWAP_INVALID_PAGE if full */
void     swap_free_pages(uint16_t first, uint16_t n);

int swap_write(uint16_t first_page, const void *src, size_t bytes);
int swap_read(uint16_t first_page, void *dst, size_t bytes);

uint16_t swap_pages_total(void);
uint16_t swap_pages_free(void);
void     swap_stats(SwapStats *out);

#endif /* CARDOS_SWAP_H */
