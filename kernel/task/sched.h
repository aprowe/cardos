/* CardOS cooperative scheduler -- the portable policy half.
 *
 * This file knows which task should run next and why. It does not know how to
 * switch to it: the Xtensa context switch is machine code and lives in
 * kernel/task/switch_xtensa.S, behind kernel/task/task.h. Splitting them is
 * what lets the ready queue, sleep accounting and reaping be tested on the PC,
 * which is where scheduling bugs are actually findable.
 *
 * Cooperative round-robin. No preemption, no priorities.
 */
#ifndef CARDOS_SCHED_H
#define CARDOS_SCHED_H

#include <stdint.h>

/* Low 8 bits index, high 8 bits a generation that is never 0 -- so a Tid is
 * never 0 while live, and a Tid left over from a task that has since exited
 * cannot kill whatever task later reuses its slot. Exactly the reasoning
 * behind Handle in mem.h, and the failure it prevents here is worse. */
typedef uint16_t Tid;

#define TID_INVALID      ((Tid)0)
#define SCHED_MAX_TASKS  16
#define SCHED_NAME_MAX   15

typedef enum {
  TASK_FREE = 0,
  TASK_READY,
  TASK_RUNNING,
  TASK_SLEEPING,
  TASK_BLOCKED,
  TASK_DEAD
} TaskState;

typedef struct {
  Tid       tid;
  char      name[SCHED_NAME_MAX + 1];
  TaskState state;
  uint32_t  slices;     /* times scheduled -- `ps` shows this */
  uint32_t  wake_at;    /* meaningful while SLEEPING */
} TaskInfo;

/* Monotonic milliseconds. Injected so the host tests can drive time directly
 * instead of sleeping, which would make the suite slow and flaky. */
typedef uint32_t (*SchedClock)(void *ctx);

void      sched_init(SchedClock clock, void *ctx);

Tid       sched_create(const char *name);
int       sched_valid(Tid t);
TaskState sched_state(Tid t);
uint8_t   sched_index(Tid t);
Tid       sched_current(void);

/* Pick the next runnable task and mark it RUNNING. Any task that was RUNNING
 * is put back to READY first. Sleepers whose deadline has passed become READY
 * as a side effect. Returns TID_INVALID when nothing can run -- every task is
 * sleeping, blocked or dead -- and the caller should idle rather than spin. */
Tid       sched_next(void);

void      sched_sleep(Tid t, uint32_t ms);
void      sched_block(Tid t);
void      sched_wake(Tid t);
void      sched_exit(Tid t);

/* Free the slots of DEAD tasks. Returns how many were reaped. Separate from
 * sched_exit because a task cannot free the stack it is still standing on. */
int       sched_reap(void);

int       sched_list(TaskInfo *out, int max);
int       sched_count(void);      /* tasks that are not FREE */

#endif /* CARDOS_SCHED_H */
