/* Dimming, and coming back. See power.h. */

#include "kernel/sys/power.h"

#include "kernel/sys/bg.h"
#include "kernel/sys/prefs.h"
#include "kernel/drv/display.h"
#include "kernel/sys/clock.h"

#define HOLDS_MAX 4

enum { LIT = 0, DIMMED, DARK, CLOCK };

static int s_state;
static int s_was;          /* the brightness the user chose, to come back to */
static int s_loaded;
static int s_dim_s = POWER_DIM_DEFAULT_S;
static int s_off_s = POWER_OFF_DEFAULT_S;
static int s_forced;       /* dark by request, not by the idle timeout */
static const void *s_hold[HOLDS_MAX];
static int s_sleep_clock = -1;   /* Settings > Display > Sleep: 1 clock, 0 black */
static void (*s_paint_clock)(void), (*s_repaint)(void);
static uint32_t s_clock_minute;

void power_set_painters(void (*paint_clock)(void), void (*repaint)(void)) {
  s_paint_clock = paint_clock;
  s_repaint = repaint;
}

int  power_sleep_clock(void) {
  if (s_sleep_clock < 0) s_sleep_clock = prefs_get_u16("sleep_clk", 1) != 0;
  return s_sleep_clock;
}
void power_set_sleep_clock(int on) { s_sleep_clock = on != 0; prefs_set_u16("sleep_clk", s_sleep_clock); }

int power_showing_clock(void) { return s_state == CLOCK; }
int power_asleep(void) { return s_state == DARK || s_state == CLOCK; }

static uint32_t minute_now(void) {
  uint32_t t = clock_epoch();
  return t ? t / 60 : bg_idle_ms() / 60000;
}

/* The dim clock: the backlight at its lowest, the face painted, and the panel
 * frozen against everything else until a key. */
static void to_clock(void) {
  if (!s_paint_clock) { display_backlight(0); s_state = DARK; return; }
  display_backlight(1);
  display_set_brightness_now(DISPLAY_BRIGHT_MIN);
  s_state = CLOCK;
  s_clock_minute = minute_now();
  s_paint_clock();
}

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
  int was_clock = s_state == CLOCK;
  if (s_state == LIT) return;
  display_freeze(0);            /* whatever put it to sleep, the panel is back */

  /* Back to what it was, not to full: waking a screen brighter than the
   * setting would be its own small insult. */
  display_backlight(1);
  display_set_brightness_now(s_was ? s_was : 100);
  s_state = LIT;
  s_forced = 0;
  if (was_clock && s_repaint) s_repaint();     /* what the clock covered */
}

void power_wake_now(void) {
  bg_note_activity();
  power_wake();
}

/* fn-o and fn-c, on purpose, are the same sequence -- remember the
 * brightness to come back to, mark it forced so the keypress that asked
 * for this is not undone by the very next tick -- and differ only in
 * whether the clock gets painted over the black. */
static void sleep_now(int clock) {
  if (s_state == LIT) s_was = display_brightness();
  s_forced = 1;
  if (clock) { to_clock(); return; }
  if (s_state == CLOCK) display_freeze(0);
  display_backlight(0);
  s_state = DARK;
}

void power_clock_now(void) { sleep_now(1); }
void power_off_now(void)   { sleep_now(0); }

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
  /* The clock face once a minute, and nothing between. */
  if (s_state == CLOCK && minute_now() != s_clock_minute) {
    s_clock_minute = minute_now();
    if (s_paint_clock) s_paint_clock();
  }
  /* Held on by an app, or never set to dim: lit, and that is all. */
  if (held() || !s_dim_s || idle < (uint32_t)s_dim_s * 1000u) {
    /* Not if forced dark: the keypress that asked for that just reset the
     * idle clock, and waking on the very next pass would make the chord
     * into a no-op. */
    if (s_state != LIT && !s_forced) power_wake();
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
     * keeps ticking -- only the panel stops drawing power. Or the dim clock,
     * if that is what sleep is set to show. */
    if (power_sleep_clock()) to_clock();
    else { display_backlight(0); s_state = DARK; }
  }
}
