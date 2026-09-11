/* CardOS cooperative scheduler -- portable policy. See sched.h. */

#include "kernel/task/sched.h"
#include "kernel/mem/mem.h"

#include <string.h>

/* Task index i owns memory locks as owner i+1, keeping owner 0 to mean
 * "kernel, before any task existed". */
#define OWNER_OF(index) ((uint8_t)((index) + 1))

typedef struct {
  char      name[SCHED_NAME_MAX + 1];
  TaskState state;
  uint8_t   gen;
  uint32_t  slices;
  uint32_t  wake_at;
} Slot;

static Slot       g_slot[SCHED_MAX_TASKS];
static SchedClock g_clock;
static void      *g_clock_ctx;
static uint8_t    g_cursor;        /* index of the task that ran last */
static Tid        g_current;

static uint32_t now_ms(void) { return g_clock ? g_clock(g_clock_ctx) : 0; }

static uint8_t idx_of(Tid t)  { return (uint8_t)(t & 0xFFu); }
static uint8_t gen_of(Tid t)  { return (uint8_t)(t >> 8); }

static Slot *slot_of(Tid t) {
  Slot *s;
  if (gen_of(t) == 0) return NULL;            /* TID_INVALID, or corrupt */
  s = &g_slot[idx_of(t)];
  if (s->state == TASK_FREE || s->gen != gen_of(t)) return NULL;
  return s;
}

static Tid tid_of(const Slot *s) {
  uint8_t i = (uint8_t)(s - g_slot);
  return (Tid)(((uint16_t)s->gen << 8) | i);
}

static void bump_generation(Slot *s) {
  s->gen = (uint8_t)(s->gen + 1);
  if (s->gen == 0) s->gen = 1;                /* 0 is reserved */
}

void sched_init(SchedClock clock, void *ctx) {
  int i;
  memset(g_slot, 0, sizeof g_slot);
  for (i = 0; i < SCHED_MAX_TASKS; i++) g_slot[i].gen = 1;
  g_clock = clock;
  g_clock_ctx = ctx;
  g_cursor = SCHED_MAX_TASKS - 1;             /* so the first pick is slot 0 */
  g_current = TID_INVALID;
}

Tid sched_create(const char *name) {
  int i;
  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    Slot *s = &g_slot[i];
    if (s->state != TASK_FREE) continue;
    memset(s->name, 0, sizeof s->name);
    if (name) strncpy(s->name, name, SCHED_NAME_MAX);
    s->name[SCHED_NAME_MAX] = '\0';
    s->state = TASK_READY;
    s->slices = 0;
    s->wake_at = 0;
    return tid_of(s);
  }
  return TID_INVALID;
}

int       sched_valid(Tid t) { return slot_of(t) != NULL; }
uint8_t   sched_index(Tid t) { return idx_of(t); }
Tid       sched_current(void) { return g_current; }

TaskState sched_state(Tid t) {
  Slot *s = slot_of(t);
  return s ? s->state : TASK_FREE;
}

/* Signed difference, so a deadline that wrapped past 2^32 is still recognised
 * as passed. After ~49 days of uptime the millisecond clock wraps, and a
 * naive `now >= wake_at` would strand the sleeper for another 49 days. */
static int deadline_passed(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

Tid sched_next(void) {
  uint32_t now = now_ms();
  int i;

  /* Wake anything whose deadline has come. */
  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    Slot *s = &g_slot[i];
    if (s->state == TASK_SLEEPING && deadline_passed(now, s->wake_at))
      s->state = TASK_READY;
  }

  /* The outgoing task goes to the back of the queue. */
  {
    Slot *cur = slot_of(g_current);
    if (cur && cur->state == TASK_RUNNING) cur->state = TASK_READY;
  }

  /* Round robin: start just past whoever ran last. */
  for (i = 1; i <= SCHED_MAX_TASKS; i++) {
    uint8_t k = (uint8_t)((g_cursor + i) % SCHED_MAX_TASKS);
    Slot *s = &g_slot[k];
    if (s->state != TASK_READY) continue;
    s->state = TASK_RUNNING;
    s->slices++;
    g_cursor = k;
    g_current = tid_of(s);
    kmem_set_owner(OWNER_OF(k));      /* locks taken from here belong to it */
    return g_current;
  }

  g_current = TID_INVALID;         /* nothing runnable: the caller idles */
  kmem_set_owner(0);
  return TID_INVALID;
}

void sched_sleep(Tid t, uint32_t ms) {
  Slot *s = slot_of(t);
  if (!s || s->state == TASK_DEAD) return;
  s->wake_at = now_ms() + ms;
  s->state = TASK_SLEEPING;
}

void sched_block(Tid t) {
  Slot *s = slot_of(t);
  if (!s || s->state == TASK_DEAD) return;
  s->state = TASK_BLOCKED;
}

void sched_wake(Tid t) {
  Slot *s = slot_of(t);
  if (!s) return;
  if (s->state == TASK_BLOCKED || s->state == TASK_SLEEPING) s->state = TASK_READY;
}

void sched_exit(Tid t) {
  Slot *s = slot_of(t);
  if (!s) return;
  s->state = TASK_DEAD;
  /* Hand back anything it had locked. Without this the lock counts stay
   * raised forever and those blocks can never be moved or evicted again --
   * a leak the approved spec does not account for. */
  kmem_release_owner(OWNER_OF(idx_of(t)));
  if (t == g_current) g_current = TID_INVALID;
}

int sched_reap(void) {
  int i, n = 0;
  for (i = 0; i < SCHED_MAX_TASKS; i++) {
    Slot *s = &g_slot[i];
    if (s->state != TASK_DEAD) continue;
    s->state = TASK_FREE;
    bump_generation(s);            /* the slot may be reused; the tid may not */
    n++;
  }
  return n;
}

int sched_list(TaskInfo *out, int max) {
  int i, n = 0;
  for (i = 0; i < SCHED_MAX_TASKS && n < max; i++) {
    Slot *s = &g_slot[i];
    if (s->state == TASK_FREE) continue;
    out[n].tid = tid_of(s);
    memcpy(out[n].name, s->name, sizeof out[n].name);
    out[n].state = s->state;
    out[n].slices = s->slices;
    out[n].wake_at = s->wake_at;
    n++;
  }
  return n;
}

int sched_count(void) {
  int i, n = 0;
  for (i = 0; i < SCHED_MAX_TASKS; i++)
    if (g_slot[i].state != TASK_FREE) n++;
  return n;
}
