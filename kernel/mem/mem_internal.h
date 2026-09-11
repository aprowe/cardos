/* Internals of the memory manager, exposed so the host test suite can assert
 * on them. Nothing outside kernel/mem and test/ should include this. */
#ifndef CARDOS_MEM_INTERNAL_H
#define CARDOS_MEM_INTERNAL_H

#include "kernel/mem/mem.h"

#define MEM_OFF_NONE 0xFFFFFFFFu
#define MEM_NO_LRU   0xFFu

/* Descriptor flags. The two low bits mirror the public MEM_* flags. */
#define D_INUSE    0x01u
#define D_FIXED    0x02u
#define D_RESIDENT 0x04u
#define D_DIRTY    0x08u
#define D_ONLRU    0x10u
#define D_BACKED   0x20u  /* swap_page holds a valid copy */

/* Offset from the heap base, not a pointer, so the descriptor has the same
 * layout on a 64-bit host as on the 32-bit device and the host tests measure
 * the real footprint. */
typedef struct {
  uint32_t off;
  uint32_t size;
  uint16_t swap_page;
  uint8_t  lock;
  uint8_t  flags;
  uint8_t  gen;
  uint8_t  lru_prev;
  uint8_t  lru_next;
  uint8_t  owner;   /* task index holding the lock; 0 = unowned */
} MemDesc;

_Static_assert(sizeof(MemDesc) == 16,
               "MemDesc must stay 16 bytes: 256 of them is the 4 KB handle table");

static inline uint8_t mem_index(Handle h)      { return (uint8_t)(h & 0xFFu); }
static inline uint8_t mem_generation(Handle h) { return (uint8_t)(h >> 8); }

MemDesc *mem_desc(Handle h);          /* NULL unless the handle is live */
MemDesc *mem_desc_at(uint8_t index);  /* raw slot access, for tests */

#endif /* CARDOS_MEM_INTERNAL_H */
