/* Apps in windows.
 *
 * An app is a name plus two callbacks: paint yourself into this rectangle, and
 * here is a keypress. No stack, no task.
 *
 * That is a deliberate departure from the kernel spec, which assumed windows
 * would need the cooperative scheduler underneath them. They do not: an
 * event-driven GUI runs on one stack by design, and measurement killed the
 * memory argument for a hand-written context switch anyway -- a FreeRTOS task
 * with a 1 KB stack costs 1132 bytes on this board, not the ~4 KB the spec
 * assumed, so owning the switch saves about 92 bytes per task.
 *
 * Per-task stacks become worth having when something needs to run *while*
 * something else runs -- a compile in the background, say. Nothing does yet.
 */
#ifndef CARDOS_APP_H
#define CARDOS_APP_H

#include <stdint.h>

#include "ui/rect.h"

typedef struct {
  const char *name;

  /* Draw into `content`. The clip is already set to the damaged part, so an
   * app can paint its whole contents and let the clip sort it out. */
  void (*paint)(void *state, Rect content);

  /* A keypress arrived while this app had focus. Return 1 if the window needs
   * repainting as a result. */
  int (*key)(void *state, uint8_t k);

  /* Called when a window opens, so a second instance starts clean. */
  void (*open)(void *state);

  void *state;
} AppDef;

/* The apps the Start menu offers. */
int            app_count(void);
const AppDef  *app_at(int i);

#endif /* CARDOS_APP_H */
