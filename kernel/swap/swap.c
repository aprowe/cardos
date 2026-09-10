/* CardOS swap. See swap.h for the contract. */

#include "swap/swap.h"

#include <string.h>

static const SwapDev *g_dev;
static SwapExtent     g_extents[SWAP_MAX_EXTENTS];
static int            g_n_extents;
static uint16_t       g_extent_first_page[SWAP_MAX_EXTENTS];

static unsigned char *g_bitmap;
static uint16_t       g_pages_total;
static uint16_t       g_pages_free;

static uint32_t       g_page_writes;
static uint32_t       g_page_reads;

/* One sector of scratch. A block is rarely a whole number of sectors, and
 * writing the tail straight from the caller's buffer would read past the end
 * of it -- so the tail goes through here. */
static unsigned char  g_bounce[SWAP_SECTOR_SIZE];

/* ------------------------------------------------------------- bitmap ---- */

static int bit_get(uint16_t page) {
  return (g_bitmap[page >> 3] >> (page & 7)) & 1;
}
static void bit_set(uint16_t page) {
  g_bitmap[page >> 3] |= (unsigned char)(1u << (page & 7));
}
static void bit_clear(uint16_t page) {
  g_bitmap[page >> 3] &= (unsigned char)~(1u << (page & 7));
}

/* ---------------------------------------------------------------- init --- */

int swap_init(const SwapDev *dev, const SwapExtent *extents, int n_extents,
              void *bitmap, size_t bitmap_bytes) {
  uint32_t total_sectors = 0;
  uint16_t page_cursor = 0;
  int i;

  if (!dev || !dev->read || !dev->write) return -1;
  if (!extents || n_extents <= 0 || n_extents > SWAP_MAX_EXTENTS) return -1;
  if (!bitmap) return -1;

  for (i = 0; i < n_extents; i++) {
    if (extents[i].sector_count == 0) return -1;
    if (extents[i].sector_count % SWAP_SECTORS_PER_PAGE != 0) return -1;
    total_sectors += extents[i].sector_count;
  }

  if (total_sectors / SWAP_SECTORS_PER_PAGE > 0xFFFEu) return -1;
  g_pages_total = (uint16_t)(total_sectors / SWAP_SECTORS_PER_PAGE);

  if (bitmap_bytes < (size_t)((g_pages_total + 7u) / 8u)) return -1;

  g_dev = dev;
  g_n_extents = n_extents;
  for (i = 0; i < n_extents; i++) {
    g_extents[i] = extents[i];
    g_extent_first_page[i] = page_cursor;
    page_cursor = (uint16_t)(page_cursor +
                             extents[i].sector_count / SWAP_SECTORS_PER_PAGE);
  }

  g_bitmap = (unsigned char *)bitmap;
  /* Zeroed here so pages left behind by a previous boot can never be mistaken
   * for live data. Swap is not meant to survive a reboot. */
  memset(g_bitmap, 0, (size_t)((g_pages_total + 7u) / 8u));
  g_pages_free = g_pages_total;
  g_page_writes = g_page_reads = 0;
  return 0;
}

uint32_t swap_page_to_sector(uint16_t page) {
  int i;
  if (page >= g_pages_total) return SWAP_BAD_SECTOR;
  for (i = g_n_extents - 1; i >= 0; i--) {
    if (page >= g_extent_first_page[i]) {
      uint32_t within = (uint32_t)(page - g_extent_first_page[i]);
      return g_extents[i].start_sector + within * SWAP_SECTORS_PER_PAGE;
    }
  }
  return SWAP_BAD_SECTOR;
}

/* ----------------------------------------------------------- allocator --- */

uint16_t swap_alloc_pages(uint16_t n) {
  uint16_t start = 0, run = 0, p;
  if (n == 0 || n > g_pages_free) return SWAP_INVALID_PAGE;

  for (p = 0; p < g_pages_total; p++) {
    if (bit_get(p)) {
      run = 0;                      /* the run is broken; never straddle */
      start = (uint16_t)(p + 1);
      continue;
    }
    if (run == 0) start = p;
    run++;
    if (run == n) {
      uint16_t k;
      for (k = 0; k < n; k++) bit_set((uint16_t)(start + k));
      g_pages_free = (uint16_t)(g_pages_free - n);
      return start;
    }
  }
  return SWAP_INVALID_PAGE;
}

void swap_free_pages(uint16_t first, uint16_t n) {
  uint16_t k;
  if (first >= g_pages_total || n == 0) return;
  if ((uint32_t)first + n > g_pages_total) return;
  for (k = 0; k < n; k++) {
    if (bit_get((uint16_t)(first + k))) {
      bit_clear((uint16_t)(first + k));
      g_pages_free++;
    }
  }
}

/* ------------------------------------------------------------------ io --- */

static uint16_t pages_for(size_t bytes) {
  return (uint16_t)((bytes + SWAP_PAGE_SIZE - 1) / SWAP_PAGE_SIZE);
}

/* Bounds-check before touching the device, so a bad request is refused rather
 * than partially applied. */
static int io_range_ok(uint16_t first_page, size_t bytes) {
  uint32_t need = pages_for(bytes);
  if (bytes == 0) return 0;
  if ((uint32_t)first_page + need > g_pages_total) return 0;
  return 1;
}

int swap_write(uint16_t first_page, const void *src, size_t bytes) {
  const unsigned char *p = (const unsigned char *)src;
  size_t done = 0;

  if (!g_dev || !io_range_ok(first_page, bytes)) return -1;

  while (done < bytes) {
    uint16_t page = (uint16_t)(first_page + (uint16_t)(done / SWAP_PAGE_SIZE));
    uint32_t sector = swap_page_to_sector(page);
    uint32_t within = (uint32_t)(done % SWAP_PAGE_SIZE) / SWAP_SECTOR_SIZE;
    size_t   left = bytes - done;

    if (sector == SWAP_BAD_SECTOR) return -1;
    sector += within;

    if (left >= SWAP_SECTOR_SIZE) {
      /* Write as many whole sectors as remain inside this page in one go. */
      uint32_t sectors_left_in_page = SWAP_SECTORS_PER_PAGE - within;
      uint32_t count = (uint32_t)(left / SWAP_SECTOR_SIZE);
      if (count > sectors_left_in_page) count = sectors_left_in_page;
      if (g_dev->write(g_dev->ctx, sector, p + done, count) != 0) return -1;
      done += (size_t)count * SWAP_SECTOR_SIZE;
    } else {
      /* Ragged tail: pad through the bounce buffer rather than reading past
       * the end of the caller's block. */
      memset(g_bounce, 0, SWAP_SECTOR_SIZE);
      memcpy(g_bounce, p + done, left);
      if (g_dev->write(g_dev->ctx, sector, g_bounce, 1) != 0) return -1;
      done += left;
    }
  }
  g_page_writes += pages_for(bytes);
  return 0;
}

int swap_read(uint16_t first_page, void *dst, size_t bytes) {
  unsigned char *p = (unsigned char *)dst;
  size_t done = 0;

  if (!g_dev || !io_range_ok(first_page, bytes)) return -1;

  while (done < bytes) {
    uint16_t page = (uint16_t)(first_page + (uint16_t)(done / SWAP_PAGE_SIZE));
    uint32_t sector = swap_page_to_sector(page);
    uint32_t within = (uint32_t)(done % SWAP_PAGE_SIZE) / SWAP_SECTOR_SIZE;
    size_t   left = bytes - done;

    if (sector == SWAP_BAD_SECTOR) return -1;
    sector += within;

    if (left >= SWAP_SECTOR_SIZE) {
      uint32_t sectors_left_in_page = SWAP_SECTORS_PER_PAGE - within;
      uint32_t count = (uint32_t)(left / SWAP_SECTOR_SIZE);
      if (count > sectors_left_in_page) count = sectors_left_in_page;
      if (g_dev->read(g_dev->ctx, sector, p + done, count) != 0) return -1;
      done += (size_t)count * SWAP_SECTOR_SIZE;
    } else {
      if (g_dev->read(g_dev->ctx, sector, g_bounce, 1) != 0) return -1;
      memcpy(p + done, g_bounce, left);   /* only the bytes that are ours */
      done += left;
    }
  }
  g_page_reads += pages_for(bytes);
  return 0;
}

/* --------------------------------------------------------------- stats --- */

uint16_t swap_pages_total(void) { return g_pages_total; }
uint16_t swap_pages_free(void)  { return g_pages_free; }

void swap_stats(SwapStats *out) {
  out->pages_total = g_pages_total;
  out->pages_free  = g_pages_free;
  out->page_writes = g_page_writes;
  out->page_reads  = g_page_reads;
}
