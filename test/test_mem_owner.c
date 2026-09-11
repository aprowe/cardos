/* Lock ownership.
 *
 * The approved spec has task_kill with no account of the locks the dead task
 * held. Those lock counts would stay raised forever, pinning blocks that can
 * then never be moved or evicted -- a leak that only shows up as a heap that
 * mysteriously stops compacting. Recording an owner on each lock lets the
 * scheduler hand them back when the task dies.
 */
#include "tinytest.h"
#include "kernel/mem/mem.h"
#include <string.h>

static unsigned char heap[32 * 1024];

#define OWNER_SHELL 1
#define OWNER_BLINK 2

void test_locks_are_released_when_their_owner_dies(void) {
  Handle h;
  mem_init(heap, sizeof heap);
  mem_set_owner(OWNER_SHELL);
  h = mem_alloc(1024, 0);
  CHECK(mem_lock(h) != NULL);
  CHECK_EQ(mem_locked(h), 1);

  CHECK_EQ(mem_release_owner(OWNER_SHELL), 1);
  CHECK_EQ(mem_locked(h), 0);
  CHECK_EQ(mem_valid(h), 1);          /* released, not freed */
}

void test_nested_locks_are_released_completely(void) {
  Handle h;
  mem_init(heap, sizeof heap);
  mem_set_owner(OWNER_SHELL);
  h = mem_alloc(1024, 0);
  mem_lock(h); mem_lock(h); mem_lock(h);
  CHECK_EQ(mem_release_owner(OWNER_SHELL), 1);
  CHECK_EQ(mem_locked(h), 0);         /* a partial release would still pin it */
}

void test_one_owner_dying_does_not_release_another_s_locks(void) {
  Handle a, b;
  mem_init(heap, sizeof heap);
  mem_set_owner(OWNER_SHELL);
  a = mem_alloc(1024, 0);
  mem_lock(a);
  mem_set_owner(OWNER_BLINK);
  b = mem_alloc(1024, 0);
  mem_lock(b);

  CHECK_EQ(mem_release_owner(OWNER_BLINK), 1);
  CHECK_EQ(mem_locked(b), 0);
  CHECK_EQ(mem_locked(a), 1);         /* the shell is still running */
}

void test_a_released_block_becomes_relocatable_again(void) {
  Handle pad, a;
  unsigned char *before, *after;
  mem_init(heap, sizeof heap);
  mem_set_owner(OWNER_SHELL);
  /* pad sits *below* a, so freeing it leaves a gap that compaction can only
   * close by moving a downward. Freeing a block above a would prove nothing. */
  pad = mem_alloc(4096, 0);
  a = mem_alloc(4096, 0);
  before = mem_lock(a);
  memset(before, 0x5C, 4096);

  mem_free(pad);
  mem_compact();
  CHECK_EQ(mem_lock(a) == before, 1); /* pinned: compaction could not move it */
  mem_unlock(a);

  mem_release_owner(OWNER_SHELL);     /* the owner dies holding it */
  mem_compact();
  after = mem_lock_ro(a);
  CHECK(after != NULL);
  CHECK(after != before);             /* now it moves, so it really is free */
  CHECK_EQ(after[4095], 0x5C);        /* and the contents came with it */
  mem_unlock(a);
}

void test_releasing_an_owner_with_nothing_locked_is_harmless(void) {
  Handle h;
  mem_init(heap, sizeof heap);
  mem_set_owner(OWNER_SHELL);
  h = mem_alloc(1024, 0);
  CHECK_EQ(mem_release_owner(OWNER_SHELL), 0);
  CHECK_EQ(mem_release_owner(OWNER_BLINK), 0);
  CHECK_EQ(mem_valid(h), 1);
}

/* Owner 0 means "no owner" -- kernel allocations made before any task exists.
 * Releasing it must not be a way to unpin the whole heap. */
void test_owner_zero_is_never_released(void) {
  Handle h;
  mem_init(heap, sizeof heap);
  mem_set_owner(0);
  h = mem_alloc(1024, 0);
  mem_lock(h);
  CHECK_EQ(mem_release_owner(0), 0);
  CHECK_EQ(mem_locked(h), 1);
}
