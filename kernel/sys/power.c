/* Dimming, and coming back. See power.h. */

#include "kernel/sys/power.h"

#include "kernel/sys/bg.h"
#include "kernel/sys/prefs.h"
#include "kernel/drv/display.h"

#define HOLDS_MAX 4

enum { LIT = 0, DIMMED, DARK };

static int s_state;
static int s_was;          /* the brightness the user chose, to come back to */
static int s_loaded;
static int s_dim_s = POWER_DIM_DEFAULT_S;
static int s_off_s = POWER_OFF_DEFAULT_S;
static const void *s_hold[HOLDS_MAX];

/* From NVS once, on first use: after boot has restored /config/settings.txt,
 * which is why it is not done at init. */
static void load(void) {
  if (s_loaded) return;
  s_loaded = 1;
  s_dim_s = prefs_get_u16("dim_s", POWER_DIM_DEFAULT_S);
  s_off_s = prefs_get_u16("off_s", POWER_OFF_DEFAULT_S);
}

int power_dim_s(void) { load(); return s_dim_s; }
int power_off_s(void) { load(); return s_off_s; }

void power_set_timeouts(int dim_s, int off_s) {
  load();
  s_dim_s = dim_s < 0 ? 0 : dim_s;
  s_off_s = off_s < 0 ? 0 : off_s;
  prefs_set_u16("dim_s", s_dim_s);
  prefs_set_u16("off_s", s_off_s);
  power_wake();                 /* whatever the new rule, start it lit */
  bg_note_activity();
}

int power_dimmed(void) { return s_state != LIT; }

void power_wake(void) {
  if (s_state == LIT) return;

  /* Back to what it was, not to full: waking a screen brighter than the
   * setting would be its own small insult. */
  display_backlight(1);
  display_set_brightness_now(s_was ? s_was : 100);
  s_state = LIT;
}

void power_wake_now(void) {
  bg_note_activity();
  power_wake();
}

void power_hold(const void *owner, int on) {
  int i, free_slot = -1;
  if (!owner) return;
  for (i = 0; i < HOLDS_MAX; i++) {
    if (s_hold[i] == owner) { if (!on) s_hold[i] = 0; return; }
    if (!s_hold[i] && free_slot < 0) free_slot = i;
  }
  if (on && free_slot >= 0) { s_hold[free_slot] = owner; power_wake(); }
}

void power_release_owner(const void *owner) { power_hold(owner, 0); }

static int held(void) {
  int i;
  for (i = 0; i < HOLDS_MAX; i++) if (s_hold[i]) return 1;
  return 0;
}

void power_tick(void) {
  uint32_t idle = bg_idle_ms();

  load();
  /* Held on by an app, or never set to dim: lit, and that is all. */
  if (held() || !s_dim_s || idle < (uint32_t)s_dim_s * 1000u) {
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

  if (s_state == DIMMED && s_off_s && idle >= (uint32_t)s_off_s * 1000u) {
    /* Off, not asleep. The shell keeps running, the radios stay up, a script
     * keeps ticking -- only the panel stops drawing power. */
    display_backlight(0);
    s_state = DARK;
  }
}
