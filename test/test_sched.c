#include "tinytest.h"
#include "kernel/task/sched.h"
#include <string.h>

/* Time is injected, so these tests drive the clock directly rather than
 * sleeping -- a scheduler suite that actually waits is slow and flaky. */
static uint32_t g_now;
static uint32_t fake_clock(void *ctx) { (void)ctx; return g_now; }

static void setup(void) {
  g_now = 0;
  sched_init(fake_clock, NULL);
}

void test_created_tasks_get_distinct_live_tids(void) {
  Tid a, b;
  setup();
  a = sched_create("shell");
  b = sched_create("blink");
  CHECK(a != TID_INVALID);
  CHECK(b != TID_INVALID);
  CHECK(a != b);
  CHECK_EQ(sched_valid(a), 1);
  CHECK_EQ(sched_state(a), TASK_READY);
  CHECK_EQ(sched_count(), 2);
}

void test_task_table_exhaustion_is_reported(void) {
  int i, made = 0;
  setup();
  for (i = 0; i < SCHED_MAX_TASKS + 4; i++)
    if (sched_create("t") != TID_INVALID) made++;
  CHECK_EQ(made, SCHED_MAX_TASKS);
  CHECK_EQ(sched_count(), SCHED_MAX_TASKS);
}

void test_names_are_truncated_not_overflowed(void) {
  TaskInfo info[SCHED_MAX_TASKS];
  setup();
  sched_create("a-very-long-task-name-indeed");
  CHECK_EQ(sched_list(info, SCHED_MAX_TASKS), 1);
  CHECK_EQ(strlen(info[0].name), SCHED_NAME_MAX);
}

void test_round_robin_is_fair(void) {
  Tid a, b, c, seen[6];
  int i;
  setup();
  a = sched_create("a");
  b = sched_create("b");
  c = sched_create("c");
  for (i = 0; i < 6; i++) seen[i] = sched_next();
  CHECK_EQ(seen[0], a); CHECK_EQ(seen[1], b); CHECK_EQ(seen[2], c);
  CHECK_EQ(seen[3], a); CHECK_EQ(seen[4], b); CHECK_EQ(seen[5], c);
}

void test_running_task_is_tracked_and_counted(void) {
  Tid a;
  TaskInfo info[SCHED_MAX_TASKS];
  setup();
  a = sched_create("a");
  CHECK_EQ(sched_next(), a);
  CHECK_EQ(sched_current(), a);
  CHECK_EQ(sched_state(a), TASK_RUNNING);
  sched_next();
  sched_next();
  sched_list(info, SCHED_MAX_TASKS);
  CHECK_EQ(info[0].slices, 3);
}

void test_a_sleeping_task_is_skipped_until_its_deadline(void) {
  Tid a, b;
  setup();
  a = sched_create("a");
  b = sched_create("b");
  sched_sleep(a, 100);
  CHECK_EQ(sched_state(a), TASK_SLEEPING);

  CHECK_EQ(sched_next(), b);          /* a is asleep */
  CHECK_EQ(sched_next(), b);
  g_now = 99;
  CHECK_EQ(sched_next(), b);          /* not yet */
  g_now = 100;
  CHECK_EQ(sched_next(), a);          /* deadline reached */
  CHECK_EQ(sched_state(a), TASK_RUNNING);
}

void test_sleep_deadline_is_relative_to_now(void) {
  Tid a;
  TaskInfo info[SCHED_MAX_TASKS];
  setup();
  a = sched_create("a");
  g_now = 5000;
  sched_sleep(a, 250);
  sched_list(info, SCHED_MAX_TASKS);
  CHECK_EQ(info[0].wake_at, 5250);
}

void test_everything_asleep_yields_no_task(void) {
  Tid a, b;
  setup();
  a = sched_create("a");
  b = sched_create("b");
  sched_sleep(a, 10);
  sched_sleep(b, 10);
  CHECK_EQ(sched_next(), TID_INVALID);   /* caller should idle, not spin */
  g_now = 10;
  CHECK(sched_next() != TID_INVALID);
}

/* The spec lists a BLOCKED state but nothing that blocks. These are what make
 * it real: the shell will block on the keyboard rather than poll it. */
void test_a_blocked_task_runs_only_after_being_woken(void) {
  Tid a, b;
  setup();
  a = sched_create("a");
  b = sched_create("b");
  sched_block(a);
  CHECK_EQ(sched_state(a), TASK_BLOCKED);
  CHECK_EQ(sched_next(), b);
  CHECK_EQ(sched_next(), b);
  sched_wake(a);
  CHECK_EQ(sched_state(a), TASK_READY);
  CHECK_EQ(sched_next(), a);
}

void test_waking_a_task_that_is_not_blocked_is_harmless(void) {
  Tid a;
  setup();
  a = sched_create("a");
  sched_wake(a);                       /* it is merely READY */
  CHECK_EQ(sched_state(a), TASK_READY);
  sched_next();
  sched_wake(a);                       /* now it is RUNNING */
  CHECK_EQ(sched_state(a), TASK_RUNNING);
}

void test_exited_tasks_are_skipped_then_reaped(void) {
  Tid a, b;
  setup();
  a = sched_create("a");
  b = sched_create("b");
  sched_exit(a);
  CHECK_EQ(sched_state(a), TASK_DEAD);
  CHECK_EQ(sched_next(), b);
  CHECK_EQ(sched_count(), 2);          /* still occupying a slot */
  CHECK_EQ(sched_reap(), 1);
  CHECK_EQ(sched_count(), 1);
  CHECK_EQ(sched_valid(a), 0);
}

void test_a_reused_slot_does_not_answer_to_the_old_tid(void) {
  Tid a, b;
  setup();
  a = sched_create("a");
  sched_exit(a);
  sched_reap();
  b = sched_create("b");
  CHECK_EQ(sched_index(b), sched_index(a));   /* same slot ... */
  CHECK(b != a);                              /* ... different tid */
  CHECK_EQ(sched_valid(a), 0);
  CHECK_EQ(sched_valid(b), 1);

  /* The failure this prevents: killing a stale tid must not kill the
   * innocent task that inherited the slot. */
  sched_exit(a);
  CHECK_EQ(sched_state(b), TASK_READY);
}

void test_tid_generation_never_becomes_zero(void) {
  int i;
  setup();
  for (i = 0; i < 600; i++) {
    Tid t = sched_create("t");
    CHECK(t != TID_INVALID);
    sched_exit(t);
    sched_reap();
  }
}

void test_operations_on_an_invalid_tid_do_nothing(void) {
  Tid a;
  setup();
  a = sched_create("a");
  sched_sleep(TID_INVALID, 10);
  sched_block(TID_INVALID);
  sched_wake(TID_INVALID);
  sched_exit(TID_INVALID);
  CHECK_EQ(sched_state(TID_INVALID), TASK_FREE);
  CHECK_EQ(sched_valid(TID_INVALID), 0);
  CHECK_EQ(sched_state(a), TASK_READY);   /* untouched */
  CHECK_EQ(sched_count(), 1);
}

void test_the_clock_wrapping_does_not_strand_a_sleeper(void) {
  Tid a, b;
  setup();
  a = sched_create("a");
  b = sched_create("b");
  g_now = 0xFFFFFF00u;               /* ~49 days of uptime, then wrap */
  sched_sleep(a, 0x200);             /* deadline wraps past zero */
  CHECK_EQ(sched_next(), b);
  g_now = 0x100;                     /* wrapped; the deadline has passed */
  CHECK_EQ(sched_next(), a);
}

void test_listing_reports_every_live_task(void) {
  TaskInfo info[SCHED_MAX_TASKS];
  int n;
  setup();
  sched_create("shell");
  sched_create("blink");
  sched_create("reaper");
  n = sched_list(info, SCHED_MAX_TASKS);
  CHECK_EQ(n, 3);
  CHECK_EQ(strcmp(info[0].name, "shell"), 0);
  CHECK_EQ(strcmp(info[2].name, "reaper"), 0);
  CHECK_EQ(info[1].state, TASK_READY);
}

void test_listing_respects_the_caller_s_limit(void) {
  TaskInfo info[2];
  setup();
  sched_create("a"); sched_create("b"); sched_create("c");
  CHECK_EQ(sched_list(info, 2), 2);
}

/* --- integration with the memory manager --------------------------------- */

#include "kernel/mem/mem.h"
static unsigned char sched_heap[16 * 1024];

void test_a_dying_task_hands_back_the_locks_it_held(void) {
  Tid a, b;
  Handle h;
  setup();
  kmem_init(sched_heap, sizeof sched_heap);
  a = sched_create("a");
  b = sched_create("b");

  CHECK_EQ(sched_next(), a);          /* a is running, so a owns what it locks */
  h = kmem_alloc(1024, 0);
  CHECK(kmem_lock(h) != NULL);
  CHECK_EQ(kmem_locked(h), 1);

  sched_exit(a);
  CHECK_EQ(kmem_locked(h), 0);         /* released by the exit, not leaked */
  CHECK_EQ(kmem_valid(h), 1);          /* the block itself still exists */
  (void)b;
}

void test_one_task_exiting_leaves_another_s_locks_alone(void) {
  Tid a, b;
  Handle ha, hb;
  setup();
  kmem_init(sched_heap, sizeof sched_heap);
  a = sched_create("a");
  b = sched_create("b");

  CHECK_EQ(sched_next(), a);
  ha = kmem_alloc(1024, 0);
  kmem_lock(ha);
  CHECK_EQ(sched_next(), b);
  hb = kmem_alloc(1024, 0);
  kmem_lock(hb);

  sched_exit(b);
  CHECK_EQ(kmem_locked(hb), 0);
  CHECK_EQ(kmem_locked(ha), 1);        /* a is still very much alive */
}
