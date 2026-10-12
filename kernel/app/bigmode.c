/* Big-app mode. See bigmode.h. */

#include "kernel/app/bigmode.h"

#include <stddef.h>

#define ON_ENTER_MAX 4

static const BigHeap *s_heap;
static const void *s_owner;
static void *s_block[BIG_BLOCKS];
static int s_nblock;
static void (*s_on_enter[ON_ENTER_MAX])(void);
static int s_n_on_enter;

void bigmode_set_heap(const BigHeap *h) { s_heap = h; }

static void free_blocks(void) {
  int i;
  for (i = 0; i < s_nblock; i++)
    if (s_heap && s_block[i]) s_heap->free(s_block[i]);
  for (i = 0; i < BIG_BLOCKS; i++) s_block[i] = NULL;
  s_nblock = 0;
}

void bigmode_reset(void) {
  free_blocks();
  s_owner = NULL;
  s_n_on_enter = 0;
}

int bigmode_on_enter(void (*fn)(void)) {
  int i;
  if (!fn) return -1;
  for (i = 0; i < s_n_on_enter; i++) if (s_on_enter[i] == fn) return 0;
  if (s_n_on_enter == ON_ENTER_MAX) return -1;
  s_on_enter[s_n_on_enter++] = fn;
  return 0;
}

void bigmode_enter(const void *owner) {
  int i;
  if (!owner || owner == s_owner) return;
  free_blocks();                       /* one that went without a release */
  s_owner = owner;
  for (i = 0; i < s_n_on_enter; i++) s_on_enter[i]();
}

void bigmode_release(const void *owner) {
  if (!owner || owner != s_owner) return;
  free_blocks();
  s_owner = NULL;
}

int bigmode_active(void) { return s_owner != NULL; }
const void *bigmode_owner(void) { return s_owner; }
int bigmode_blocks(void) { return s_nblock; }

size_t bigmode_room(const void *caller) {
  size_t l, t;
  if (!s_heap || !caller || caller != s_owner || s_nblock == BIG_BLOCKS) return 0;
  l = s_heap->largest();
  t = s_heap->total();
  t = t > BIG_RESERVE ? t - BIG_RESERVE : 0;  /* the reserve, out of all that is free */
  return l < t ? l : t;
}

void *bigmode_alloc(const void *caller, size_t n) {
  void *p;
  if (!n || n > bigmode_room(caller)) return NULL;
  p = s_heap->alloc(n);
  if (p) s_block[s_nblock++] = p;
  return p;
}
