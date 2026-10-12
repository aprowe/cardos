/* Big-app mode (2026-10-11).
 *
 * An app's data lives in the 28 KB arena (arena.h), fixed because flash
 * code is relocated for one data address. An app that needs more -- Jar
 * Factory's world -- sets CAPP_BIG in its capp_info flags. While it is the
 * app on screen it may take blocks of the general heap for its lifetime
 * (api->big_alloc, api->big_room), reached through pointers in its data, so
 * the arena's fixed address is untouched. Entering lets go of what the OS
 * can rebuild later -- the code block Today keeps, the Bluetooth radio
 * unless something holds it -- registered with bigmode_on_enter; the app's
 * release frees what it took (register bigmode_release with
 * capprun_on_release).
 *
 * Portable: the heap is a table set at boot (bigmode_set_heap), so the
 * host tests run it against a counting fake (test/test_bigmode.c).
 */
#ifndef CARDOS_BIGMODE_H
#define CARDOS_BIGMODE_H

#include <stddef.h>

#define BIG_BLOCKS  8                  /* blocks one big app may hold */
#define BIG_RESERVE (24u * 1024u)      /* left for WiFi, requests and the OS */

typedef struct {
  void  *(*alloc)(size_t n);
  void   (*free)(void *p);
  size_t (*largest)(void);             /* the largest free block */
  size_t (*total)(void);               /* all that is free */
} BigHeap;

void        bigmode_set_heap(const BigHeap *h);
void        bigmode_reset(void);                  /* for the host tests */

/* Things to let go of when a big app comes on screen. 0, or -1 when full. */
int         bigmode_on_enter(void (*fn)(void));

/* `owner` (capprun_executing's identity) is the big app on screen now. */
void        bigmode_enter(const void *owner);
/* capprun_on_release's hook: the big app's blocks freed, the mode left. */
void        bigmode_release(const void *owner);

int         bigmode_active(void);
const void *bigmode_owner(void);
int         bigmode_blocks(void);

/* A block of n bytes for `caller`, only if it is the big app on screen, it
 * fits the largest free piece, and BIG_RESERVE stays free in all after it;
 * NULL otherwise. */
void       *bigmode_alloc(const void *caller, size_t n);
/* How much bigmode_alloc would give `caller` in one piece now; 0 if none. */
size_t      bigmode_room(const void *caller);

#endif /* CARDOS_BIGMODE_H */
