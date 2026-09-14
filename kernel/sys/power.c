/* Dimming, and coming back. See power.h. */

#include "kernel/sys/power.h"

#include "kernel/sys/bg.h"
#include "kernel/drv/display.h"

#define DIM_AFTER_MS   30000
#define OFF_AFTER_MS  120000

enum { LIT = 0, DIMMED, DARK };

static int s_state;
static int s_was;          /* the brightness the user chose, to come back to */

int power_dimmed(void) { return s_state != LIT; }

void power_wake(void) {
  if (s_state == LIT) return;

  /* Back to what it was, not to full: waking a screen brighter than the
   * setting would be its own small insult. */
  display_backlight(1);
  display_set_brightness_now(s_was ? s_was : 100);
  s_state = LIT;
}

void power_tick(void) {
  uint32_t idle = bg_idle_ms();

  if (idle < DIM_AFTER_MS) {
    if (s_state != LIT) power_wake();
    return;
  }

  if (s_state == LIT) {
    /* Remember before changing it, or the way back is lost. */
    s_was = display_brightness();
    display_set_brightness_now(DISPLAY_BRIGHT_MIN);
    s_state = DIMMED;
    return;
  }

  if (s_state == DIMMED && idle >= OFF_AFTER_MS) {
    /* Off, not asleep. The shell keeps running, the radios stay up, a script
     * keeps ticking -- only the panel stops drawing power. */
    display_backlight(0);
    s_state = DARK;
  }
}
