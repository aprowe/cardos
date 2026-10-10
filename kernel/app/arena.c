/* The app data arena. See arena.h. */

#include "kernel/app/arena.h"
#include "kernel/app/capp.h"

#include <stdio.h>
#include <string.h>

/* Static rather than allocated at boot: same address on every boot of a
 * build, and no chance of a radio getting there first. */
#if defined(_MSC_VER)
__declspec(align(16)) static uint8_t s_arena[ARENA_SIZE];
#else
static uint8_t s_arena[ARENA_SIZE] __attribute__((aligned(16)));
#endif
static int  s_held;
static char s_who[24];

void *arena_claim(uint32_t size, uint32_t align, const char *who) {
  if (s_held || size == 0 || size > ARENA_SIZE || align > ARENA_ALIGN) return NULL;
  s_held = 1;
  snprintf(s_who, sizeof s_who, "%s", who ? who : "?");
  memset(s_arena, 0, size);
  return s_arena;
}

void arena_release(const void *p) {
  if (!s_held || p != (const void *)s_arena) return;
  s_held = 0;
  s_who[0] = 0;
}

int arena_held(void) { return s_held; }
const char *arena_holder(void) { return s_held ? s_who : ""; }
uintptr_t arena_addr(void) { return (uintptr_t)s_arena; }
int arena_owns(const void *p) { return p == (const void *)s_arena; }

int arena_code_in_flash(int data_in_arena, int have_partition, uint16_t flags) {
  return data_in_arena && have_partition && !(flags & CAPP_CODE_IN_RAM);
}
