/* The app data arena.
 *
 * One block, reserved for the uptime, for the data of the app on screen.
 * It exists because an app's literals -- inside .code -- hold the absolute
 * addresses of its data, so code cached in flash is valid for one data
 * address only. A heap block lands somewhere new on every launch; this
 * does not. See docs/superpowers/specs/2026-10-09-xip-app-code-design.md.
 *
 * One holder: the app started on screen. Headless commands and the icon
 * scan load on the heap as before. */
#ifndef CARDOS_ARENA_H
#define CARDOS_ARENA_H

#include <stdint.h>

#define ARENA_SIZE  (28u * 1024u)   /* tools/build_apps.py DATA_BUDGET */
#define ARENA_ALIGN 16u

/* The arena, zeroed for `size` bytes, or NULL: held already, empty, too big,
 * or wanting more alignment than it has. `who` is for `mem`. */
void       *arena_claim(uint32_t size, uint32_t align, const char *who);
/* Give it back. Anything but the arena itself is ignored. */
void        arena_release(const void *p);
int         arena_held(void);
const char *arena_holder(void);     /* "" when free */
uintptr_t   arena_addr(void);
int         arena_owns(const void *p);

/* Where code goes once the flags are known: 1 the flash cache, 0 executable
 * RAM. Only data in the arena can have its code in flash. */
int arena_code_in_flash(int data_in_arena, int have_partition, uint16_t flags);

#endif /* CARDOS_ARENA_H */
