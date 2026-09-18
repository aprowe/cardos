/* The one request slot behind kernel/net/httpq.c, as a state machine with no
 * operating system in it. Portable, so the host suite can pin down the one
 * sequence that wedged the device:
 *
 *   an app starts a request, the user leaves the app, the reply lands, and
 *   nobody ever collects it -- so the slot says "busy" to every caller from
 *   then until the next reboot, and Todo and Calendar quietly stop syncing.
 *
 * The cure is ownership. Whoever starts a request names themselves, and
 * whoever unloads them tells the slot (httpslot_abandon). A reply that was
 * still running is dropped when it finishes; one already waiting is dropped
 * on the spot. The lock and the task live in httpq.c; this is just the
 * bookkeeping, and every function here is called with the lock held.
 */
#ifndef CARDOS_HTTPSLOT_H
#define CARDOS_HTTPSLOT_H

enum { HTTPSLOT_IDLE = 0, HTTPSLOT_RUNNING, HTTPSLOT_DONE };

typedef struct {
  int         state;
  const void *owner;      /* who asked; NULL means nobody in particular */
  int         abandoned;  /* the owner went away while it was running */
} HttpSlot;

void httpslot_init(HttpSlot *s);

/* Claim it for `owner`. 0 if it is now RUNNING for them, -1 if it was busy. */
int  httpslot_claim(HttpSlot *s, const void *owner);

/* The worker finished. Returns 1 if the reply should be thrown away (nobody
 * is coming for it) and the slot is idle again, 0 if it is now DONE and
 * waiting to be collected. */
int  httpslot_finish(HttpSlot *s);

/* The owner collected the reply: idle again. */
void httpslot_collect(HttpSlot *s);

/* `owner` is going away. Returns 1 if a waiting reply should be freed now,
 * 0 otherwise (nothing of theirs here, or it will be dropped on finish). */
int  httpslot_abandon(HttpSlot *s, const void *owner);

#endif /* CARDOS_HTTPSLOT_H */
