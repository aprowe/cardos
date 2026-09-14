/* Work that takes seconds, done where it cannot be felt.
 *
 * Bringing a radio up is a multi-second scan. Until now those ran on the
 * shell's own loop: `launchui.c` retried a lost mouse every fifteen seconds
 * with a four-second blocking scan, so a paired mouse out of range froze the
 * launcher for four seconds in every fifteen, forever. The comment above that
 * call already said "each attempt is a multi-second scan". It was written
 * knowing, and put on the drawing path anyway, because there was nowhere else
 * to put it.
 *
 * This is somewhere else. One FreeRTOS task below the shell's priority, a
 * queue of jobs in and a queue of results out. The shell posts a job and
 * carries on; the task blocks on the queue and costs nothing when there is
 * nothing to do; the shell drains results on its next pass and draws them.
 *
 * CardOS's own scheduler cannot do this yet -- sched.c is policy with no
 * context switch, so it cannot park a task mid-scan -- and this is the
 * pragmatic hook underneath it rather than a replacement for it.
 *
 * THE RULE: a job runs on another task and MUST NOT TOUCH THE DISPLAY. Not
 * the panel, not the draw layer, not the console. Neither is thread-safe, and
 * the failure looks like a corrupted screen rather than like a race. A job
 * produces a sentence; the shell decides what to do with it.
 */
#ifndef CARDOS_BG_H
#define CARDOS_BG_H

#include <stdint.h>

typedef enum {
  BG_BT_RECONNECT = 1,   /* look for a bonded mouse or keyboard */
  BG_WIFI_RECONNECT,     /* rejoin the saved network */
  BG_TIME_SYNC,          /* ask the network what time it is */
  BG_BT_PAIR_MOUSE,      /* scan for a new mouse and bond it */
  BG_BT_PAIR_KBD,        /* the same for a keyboard */
  BG_RECONNECT_ALL       /* the saved network and whatever was paired */
} BgJob;

/* Start the task. Called once at boot, after the radios' own init. */
void bg_init(void);

/* Ask for a job. Returns 0 if it was queued, -1 if the queue is full or the
 * same job is already running -- a scan asked for twice is one scan. */
int  bg_submit(BgJob job);

/* Is anything running right now? For a shell that wants to say so. */
int  bg_busy(void);

/* The next finished job's message, or NULL. Drained by the shell's loop, one
 * per pass; the string is valid until the next call. */
const char *bg_take_result(void);

/* ---- idle ----------------------------------------------------------------
 *
 * Outside the moment somebody is pressing a key, this screen is static: the
 * clock ticks once a second and nothing else moves. That is the budget this
 * module spends, and it is also the reason the shell can afford to poll less
 * often when nothing is happening.
 *
 * The shell reports activity; everything else asks how long it has been
 * quiet. Jobs are only started when quiet, so a scan never competes with
 * typing. */
void     bg_note_activity(void);
uint32_t bg_idle_ms(void);

#endif /* CARDOS_BG_H */
