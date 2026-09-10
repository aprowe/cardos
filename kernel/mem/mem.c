/* CardOS handle-based memory manager. See mem.h for the contract. */

#include "mem/mem.h"
#include "mem/mem_internal.h"

#include <string.h>

/* ---------------------------------------------------------------- state -- */

static MemDesc  g_table[MEM_MAX_HANDLES];
static uint8_t *g_base;
static uint32_t g_size;

/* Movable blocks bump up from offset 0; fixed blocks allocate down from the
 * top. Keeping them apart is what lets compaction stay fully effective: a
 * MEM_FIXED block can never be moved, so one sitting in the middle of the
 * movable region would pin a hole nothing could ever close. */
static uint32_t g_movable_top;
static uint32_t g_fixed_bottom;

static uint16_t g_handles_used;

static uint32_t g_compactions;
static uint32_t g_evictions;
static uint32_t g_page_ins;

/* Free ranges inside the fixed arena, kept sorted by offset and coalesced. */
#define FIXED_FREE_MAX 32
typedef struct { uint32_t off, size; } Range;
static Range   g_fixed_free[FIXED_FREE_MAX];
static int     g_fixed_free_n;

/* ------------------------------------------------------------ handles ---- */

MemDesc *mem_desc_at(uint8_t index) { return &g_table[index]; }

MemDesc *mem_desc(Handle h) {
  uint8_t gen = mem_generation(h);
  MemDesc *d;
  if (gen == 0) return NULL;             /* handle 0, or a corrupt handle */
  d = &g_table[mem_index(h)];
  if (!(d->flags & D_INUSE) || d->gen != gen) return NULL;
  return d;
}

int mem_valid(Handle h) { return mem_desc(h) != NULL; }

static Handle handle_of(const MemDesc *d) {
  uint8_t idx = (uint8_t)(d - g_table);
  return (Handle)(((uint16_t)d->gen << 8) | idx);
}

/* First fit from slot 0, so a freed slot is reused immediately. That is
 * deliberate: it exercises the generation guard on almost every free/alloc
 * pair, rather than hiding stale-handle bugs behind 256 allocations of
 * breathing room. */
static MemDesc *claim_slot(void) {
  int i;
  for (i = 0; i < MEM_MAX_HANDLES; i++)
    if (!(g_table[i].flags & D_INUSE)) return &g_table[i];
  return NULL;
}

static void bump_generation(MemDesc *d) {
  d->gen = (uint8_t)(d->gen + 1);
  if (d->gen == 0) d->gen = 1;           /* 0 is reserved so no handle is 0 */
}

/* ------------------------------------------------------------ LRU list --- */
/* Intrusive doubly-linked list of blocks that are eviction candidates:
 * unlocked, resident and not MEM_FIXED. Head is least recently used. An
 * intrusive list means no clock interface is needed at all. */

static uint8_t g_lru_head = MEM_NO_LRU;
static uint8_t g_lru_tail = MEM_NO_LRU;

static void lru_remove(MemDesc *d) {
  uint8_t idx = (uint8_t)(d - g_table);
  if (!(d->flags & D_ONLRU)) return;
  if (d->lru_prev != MEM_NO_LRU) g_table[d->lru_prev].lru_next = d->lru_next;
  else                           g_lru_head = d->lru_next;
  if (d->lru_next != MEM_NO_LRU) g_table[d->lru_next].lru_prev = d->lru_prev;
  else                           g_lru_tail = d->lru_prev;
  d->lru_prev = d->lru_next = MEM_NO_LRU;
  d->flags = (uint8_t)(d->flags & ~D_ONLRU);
  (void)idx;
}

/* Push to the tail: the tail is most recently used, the head is the victim. */
static void lru_touch(MemDesc *d) {
  uint8_t idx = (uint8_t)(d - g_table);
  if (d->flags & D_ONLRU) lru_remove(d);
  if ((d->flags & D_FIXED) || d->lock > 0 || !(d->flags & D_RESIDENT)) return;
  d->lru_prev = g_lru_tail;
  d->lru_next = MEM_NO_LRU;
  if (g_lru_tail != MEM_NO_LRU) g_table[g_lru_tail].lru_next = idx;
  else                          g_lru_head = idx;
  g_lru_tail = idx;
  d->flags |= D_ONLRU;
}

/* -------------------------------------------------------- fixed arena ---- */

static void fixed_free_insert(uint32_t off, uint32_t size) {
  int i, at = 0;
  while (at < g_fixed_free_n && g_fixed_free[at].off < off) at++;
  if (g_fixed_free_n < FIXED_FREE_MAX) {
    for (i = g_fixed_free_n; i > at; i--) g_fixed_free[i] = g_fixed_free[i - 1];
    g_fixed_free[at].off = off;
    g_fixed_free[at].size = size;
    g_fixed_free_n++;
  }
  /* Coalesce with both neighbours. */
  for (i = 0; i < g_fixed_free_n - 1; ) {
    if (g_fixed_free[i].off + g_fixed_free[i].size == g_fixed_free[i + 1].off) {
      int j;
      g_fixed_free[i].size += g_fixed_free[i + 1].size;
      for (j = i + 1; j < g_fixed_free_n - 1; j++) g_fixed_free[j] = g_fixed_free[j + 1];
      g_fixed_free_n--;
    } else {
      i++;
    }
  }
  /* If the lowest free range now reaches the arena floor, give it back so the
   * movable arena can grow into it again. */
  if (g_fixed_free_n > 0 && g_fixed_free[0].off == g_fixed_bottom) {
    int j;
    g_fixed_bottom += g_fixed_free[0].size;
    for (j = 0; j < g_fixed_free_n - 1; j++) g_fixed_free[j] = g_fixed_free[j + 1];
    g_fixed_free_n--;
  }
}

static int fixed_alloc(uint32_t size, uint32_t *out_off) {
  int i;
  for (i = 0; i < g_fixed_free_n; i++) {      /* first fit in a freed range */
    if (g_fixed_free[i].size >= size) {
      *out_off = g_fixed_free[i].off;
      g_fixed_free[i].off  += size;
      g_fixed_free[i].size -= size;
      if (g_fixed_free[i].size == 0) {
        int j;
        for (j = i; j < g_fixed_free_n - 1; j++) g_fixed_free[j] = g_fixed_free[j + 1];
        g_fixed_free_n--;
      }
      return 1;
    }
  }
  if (g_fixed_bottom < size || g_fixed_bottom - size < g_movable_top) return 0;
  g_fixed_bottom -= size;                     /* grow the arena downward */
  *out_off = g_fixed_bottom;
  return 1;
}

/* ------------------------------------------------------------- lifetime -- */

void mem_init(void *heap, size_t bytes) {
  int i;
  memset(g_table, 0, sizeof g_table);
  for (i = 0; i < MEM_MAX_HANDLES; i++) {
    g_table[i].gen = 1;
    g_table[i].off = MEM_OFF_NONE;
    g_table[i].lru_prev = g_table[i].lru_next = MEM_NO_LRU;
  }
  g_base = (uint8_t *)heap;
  g_size = (uint32_t)bytes;
  g_movable_top = 0;
  g_fixed_bottom = g_size;
  g_handles_used = 0;
  g_compactions = g_evictions = g_page_ins = 0;
  g_fixed_free_n = 0;
  g_lru_head = g_lru_tail = MEM_NO_LRU;
}

size_t mem_size(Handle h) {
  MemDesc *d = mem_desc(h);
  return d ? d->size : 0;
}

int mem_resident(Handle h) {
  MemDesc *d = mem_desc(h);
  return d && (d->flags & D_RESIDENT) ? 1 : 0;
}

void mem_stats(MemStats *out) {
  int i;
  memset(out, 0, sizeof *out);
  out->heap_size    = g_size;
  out->movable_used = g_movable_top;
  out->fixed_used   = g_size - g_fixed_bottom;
  out->free_bytes   = g_fixed_bottom - g_movable_top;
  out->largest_free = out->free_bytes;
  out->handles_used = g_handles_used;
  out->compactions  = g_compactions;
  out->evictions    = g_evictions;
  out->page_ins     = g_page_ins;
  for (i = 0; i < MEM_MAX_HANDLES; i++) {
    MemDesc *d = &g_table[i];
    if (!(d->flags & D_INUSE)) continue;
    if (d->flags & D_RESIDENT) out->resident_bytes += d->size;
    else                       out->swapped_bytes  += d->size;
  }
}

/* ------------------------------------------------------ movable arena ---- */

/* Place a movable block. For now this is a pure bump allocation at the top of
 * the movable region; compaction and eviction hook in here in later tasks. */
static int movable_bump(uint32_t size, uint32_t *out_off) {
  if (g_fixed_bottom - g_movable_top < size) return 0;
  *out_off = g_movable_top;
  g_movable_top += size;
  return 1;
}

/* Slide unpinned movable blocks down to close gaps left by frees and
 * evictions. A locked block cannot move -- a caller is holding its pointer --
 * so the destination cursor jumps past it and packing resumes above. That is
 * why the lock discipline matters: every block held locked across an
 * allocation is a place compaction has to give up on. */
void mem_compact(void) {
  uint8_t order[MEM_MAX_HANDLES];
  int n = 0, i, k;
  uint32_t dst = 0;

  for (i = 0; i < MEM_MAX_HANDLES; i++) {
    MemDesc *d = &g_table[i];
    if ((d->flags & (D_INUSE | D_RESIDENT | D_FIXED)) == (D_INUSE | D_RESIDENT))
      order[n++] = (uint8_t)i;
  }
  for (i = 1; i < n; i++) {            /* insertion sort by heap offset */
    uint8_t v = order[i];
    for (k = i - 1; k >= 0 && g_table[order[k]].off > g_table[v].off; k--)
      order[k + 1] = order[k];
    order[k + 1] = v;
  }

  for (i = 0; i < n; i++) {
    MemDesc *d = &g_table[order[i]];
    if (d->lock > 0) {                 /* pinned: cannot move, skip past it */
      dst = d->off + d->size;
      continue;
    }
    if (d->off != dst) {
      memmove(g_base + dst, g_base + d->off, d->size);
      d->off = dst;
    }
    dst += d->size;
  }
  g_movable_top = dst;
  g_compactions++;
}

/* Place a movable block, compacting first if a plain bump will not fit.
 * Eviction to swap is added on top of this in Task 9. */
static int movable_alloc(uint32_t size, uint32_t *out_off) {
  if (movable_bump(size, out_off)) return 1;
  mem_compact();
  return movable_bump(size, out_off);
}

/* ----------------------------------------------------------- public API -- */

Handle mem_alloc(size_t bytes, uint16_t flags) {
  MemDesc *d;
  uint32_t off, size = (uint32_t)bytes;
  int ok;

  if (bytes == 0 || bytes > MEM_MAX_BLOCK) return MEM_INVALID_HANDLE;

  d = claim_slot();
  if (!d) return MEM_INVALID_HANDLE;

  ok = (flags & MEM_FIXED) ? fixed_alloc(size, &off)
                           : movable_alloc(size, &off);
  if (!ok) return MEM_INVALID_HANDLE;

  d->off       = off;
  d->size      = size;
  d->swap_page = 0;
  d->lock      = 0;
  d->flags     = (uint8_t)(D_INUSE | D_RESIDENT |
                           ((flags & MEM_FIXED) ? D_FIXED : 0));
  d->lru_prev  = d->lru_next = MEM_NO_LRU;
  g_handles_used++;

  if (flags & MEM_ZERO) memset(g_base + off, 0, size);
  lru_touch(d);
  return handle_of(d);
}

void mem_free(Handle h) {
  MemDesc *d = mem_desc(h);
  if (!d) return;
  lru_remove(d);
  if (d->flags & D_FIXED) fixed_free_insert(d->off, d->size);
  d->flags = 0;                 /* clears D_INUSE */
  d->off   = MEM_OFF_NONE;
  d->size  = 0;
  d->lock  = 0;
  bump_generation(d);
  g_handles_used--;
}

static void *lock_common(Handle h, int mark_dirty) {
  MemDesc *d = mem_desc(h);
  if (!d) return NULL;
  if (!(d->flags & D_RESIDENT)) return NULL;   /* page-in arrives in Task 9 */
  lru_remove(d);                               /* pinned blocks are not victims */
  d->lock++;
  if (mark_dirty) d->flags |= D_DIRTY;
  return g_base + d->off;
}

void *mem_lock(Handle h)    { return lock_common(h, 1); }
void *mem_lock_ro(Handle h) { return lock_common(h, 0); }

void mem_unlock(Handle h) {
  MemDesc *d = mem_desc(h);
  if (!d || d->lock == 0) return;
  d->lock--;
  if (d->lock == 0) lru_touch(d);              /* relocatable and evictable again */
}
