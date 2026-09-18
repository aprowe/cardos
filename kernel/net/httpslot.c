/* The request slot's bookkeeping. See httpslot.h. */

#include "kernel/net/httpslot.h"

void httpslot_init(HttpSlot *s) {
  s->state = HTTPSLOT_IDLE;
  s->owner = 0;
  s->abandoned = 0;
}

int httpslot_claim(HttpSlot *s, const void *owner) {
  if (s->state != HTTPSLOT_IDLE) return -1;
  s->state = HTTPSLOT_RUNNING;
  s->owner = owner;
  s->abandoned = 0;
  return 0;
}

int httpslot_finish(HttpSlot *s) {
  if (s->abandoned) {
    httpslot_init(s);
    return 1;
  }
  s->state = HTTPSLOT_DONE;
  return 0;
}

void httpslot_collect(HttpSlot *s) { httpslot_init(s); }

int httpslot_abandon(HttpSlot *s, const void *owner) {
  if (s->state == HTTPSLOT_IDLE || s->owner != owner) return 0;
  if (s->state == HTTPSLOT_RUNNING) { s->abandoned = 1; return 0; }
  httpslot_init(s);                    /* DONE: the reply is nobody's now */
  return 1;
}
