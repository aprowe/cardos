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

#include "kernel/ui/rect.h"

typedef struct {
  const char *name;

  /* Draw into `content`. The clip is already set to the damaged part, so an
   * app can paint its whole contents and let the clip sort it out. */
  void (*paint)(void *state, Rect content);

  /* A keypress arrived while this app had focus. Return 1 if the window needs
   * repainting as a result. */
  int (*key)(void *state, uint8_t k);

  /* A click landed in the content area, in content-relative pixels. Return 1
   * if the window needs repainting. */
  int (*click)(void *state, int16_t x, int16_t y, int button);

  /* Called when a window opens, so a second instance starts clean. */
  void (*open)(void *state);

  void *state;

  /* Natural height in pixels at this width, for apps whose contents do not
   * fit. NULL means "whatever the window is" -- the common case. The window
   * system scrolls the difference and draws a scrollbar; an app that answers
   * this never has to know it is being scrolled. */
  int16_t (*height)(void *state, int16_t width);

  /* Preferred content size. Zero means "whatever the desktop hands out".
   * A game with a fixed board has a right answer here and nothing else
   * does. */
  int16_t pref_w, pref_h;

  /* Does this app want typed characters right now?
   *
   * When it does not, the shell turns ; . , / into arrows, so moving a
   * selection does not need the Fn key. An app that is taking text -- an
   * editor, a password field -- returns 1 and gets those keys as themselves.
   * NULL means it never takes text, which is the common case. */
  int (*wants_text)(void *state);

  /* The app's keys, one per line as "key	meaning". Shown by ctrl-h over
   * whatever the app is doing. NULL means it has none worth listing, and the
   * shell still shows its own. A plain string rather than a callback because
   * an app's keys do not change while it runs, and one that did would be a
   * worse app. */
  const char *help;

  /* The arguments the app was started with, as one string. Built-ins only:
   * a loaded program gets argv through capp_main instead, which is why it has
   * no equivalent here. */
  void (*set_args)(void *state, const char *args);

  /* The pointer moved, a button is held, or the wheel turned; content-relative
   * coordinates, a held-button mask, and wheel notches. Return 1 to repaint.
   * See CappUi.mouse -- the rules are the same, including that returning 0 for
   * a wheel notch leaves the scrolling to the window. */
  int (*mouse)(void *state, int16_t x, int16_t y, int buttons, int wheel);

  /* Called every pass of the shell's loop with the clock in milliseconds.
   * Return 1 if the window needs repainting. This is the only way anything on
   * screen moves without being pushed; see CappUi.tick for the rules, which
   * are the same ones. NULL for an app that changes only on a keypress. */
  int (*tick)(void *state, uint32_t now_ms);

  /* What this app says changed since its last paint, in content coordinates.
   * Returns 1 with a rectangle, or 0 meaning "repaint all of it" -- which is
   * what an app that never marks anything always returns, so this costs the
   * apps written before it nothing.
   *
   * The shell consumes it when deciding what to clip; painting clears it,
   * because the accumulator belongs to the frame being drawn. */
  int (*take_damage)(void *state, Rect *out);
} AppDef;

/* The apps the Start menu offers. */
int            app_count(void);
const AppDef  *app_at(int i);
int            app_index_by_name(const char *name);

#endif /* CARDOS_APP_H */
