/* CardOS handle-based memory manager.
 *
 * Portable C11: no ESP-IDF, no FreeRTOS, no malloc. Compiles for the host
 * test suite and for Xtensa unchanged.
 *
 * Apps never hold raw pointers across a yield; they hold handles. The
 * indirection is what makes a block relocatable (so the heap compacts instead
 * of fragmenting) and evictable (so it can go to swap).
 */
#ifndef CARDOS_MEM_H
#define CARDOS_MEM_H

#include <stddef.h>
#include <stdint.h>

/* Low 8 bits are the descriptor index, high 8 bits a generation counter that
 * is never 0. A live handle is therefore never 0, and a handle left over from
 * a freed block fails mem_valid() instead of silently addressing whatever now
 * occupies its slot. */
typedef uint16_t Handle;

#define MEM_INVALID_HANDLE ((Handle)0)

#define MEM_ZERO   0x0001u   /* clear the block before returning it */
#define MEM_FIXED  0x0002u   /* never moved, never evicted: stacks, DMA buffers */

#define MEM_MAX_HANDLES 256
/* A block must be able to page back in after eviction, so it cannot be larger
 * than the space eviction can guarantee to free. This cap is what makes
 * page-in provably satisfiable rather than hopefully satisfiable. */
#define MEM_MAX_BLOCK   (32u * 1024u)

typedef struct {
  size_t   heap_size;
  size_t   movable_used;    /* bytes in movable blocks, including gaps */
  size_t   fixed_used;      /* bytes held by the fixed arena at the top */
  size_t   free_bytes;      /* the gap between the two arenas */
  size_t   largest_free;    /* biggest single run a movable block could take */
  size_t   resident_bytes;
  size_t   swapped_bytes;
  uint16_t handles_used;
  uint32_t compactions;
  uint32_t evictions;
  uint32_t page_ins;
} MemStats;

void   mem_init(void *heap, size_t bytes);

Handle mem_alloc(size_t bytes, uint16_t flags);
void   mem_free(Handle h);

/* Pin a block and return a writable pointer, paging it in if it was swapped.
 * Returns NULL if the handle is stale or if the page-in cannot be satisfied
 * because RAM is full of pinned blocks. Callers must check. Locks nest. */
void  *mem_lock(Handle h);

/* As mem_lock, but does not mark the block dirty. A clean block that already
 * has a swap page can be evicted again without writing it, which is what
 * makes read-mostly data such as console scrollback cheap to hold. */
void  *mem_lock_ro(Handle h);

void   mem_unlock(Handle h);

size_t mem_size(Handle h);
int    mem_valid(Handle h);
int    mem_resident(Handle h);

/* Slide unpinned movable blocks down to close gaps. Called automatically by
 * mem_alloc before it resorts to eviction; public because the shell's `mem`
 * command triggers it. */
void   mem_compact(void);

void   mem_stats(MemStats *out);

#endif /* CARDOS_MEM_H */
