/* The backlight, when nobody is looking.
 *
 * This screen is static almost all of the time, and it was being lit at full
 * brightness for all of it. The backlight is the largest single draw on the
 * board -- larger than either radio -- so a device left on a desk was burning
 * its cell to display a clock nobody was reading.
 *
 * The policy is deliberately dull, because a clever one is a device that goes
 * dark while you are reading it:
 *
 *   idle for "dim after"         dim to the setting's floor (30 s by default)
 *   idle for "screen off after"  backlight off; the machine keeps running (2 min)
 *   any key or click             back to the brightness you chose, immediately
 *
 * Both are Settings > Display, in seconds, 0 for never; kept in NVS and in
 * /config/settings.txt (prefs.h) as dim_s and off_s.
 *
 * An app can hold the screen on (power_hold -- Timer while it counts, an
 * alarm while it rings) and wake it (power_wake_now -- the alarm going off).
 * A hold belongs to the app that took it and goes when the app does
 * (capprun.c calls power_release_owner), so a crashed or closed app cannot
 * leave the backlight burning.
 *
 * "Idle" is the same clock the background task uses -- see bg.h -- so the same
 * quiet that lets a radio scan happen is the quiet that dims the screen.
 *
 * What this deliberately does not do: sleep. Light sleep would stop the shell
 * loop, drop the USB console and complicate every timeout in the system, for a
 * saving that matters far less than the backlight. The panel is the power
 * budget; the CPU at 240 MHz doing nothing is not.
 */
#ifndef CARDOS_POWER_H
#define CARDOS_POWER_H

#define POWER_DIM_DEFAULT_S  30
#define POWER_OFF_DEFAULT_S  120

/* Called every pass of the shell's loop. Cheap, and does nothing at all until
 * a threshold is crossed. */
void power_tick(void);

/* Somebody did something: undim now, before the next tick. Called from the
 * same place that resets the idle clock. */
void power_wake(void);

/* Is the screen currently dimmed or dark? For a shell that wants to swallow
 * the keypress that woke it rather than acting on it. */
int  power_dimmed(void);

/* The timeouts, in seconds, 0 for never. Set saves and applies at once. */
int  power_dim_s(void);
int  power_off_s(void);
void power_set_timeouts(int dim_s, int off_s);

/* An app keeping the screen on, or letting it go. Up to a few owners at once;
 * the screen stays lit while any holds it. */
void power_hold(const void *owner, int on);
void power_release_owner(const void *owner);

/* Light it now and start the idle clock again, as a key would -- without
 * being a key, so nothing is typed. */
void power_wake_now(void);

#endif /* CARDOS_POWER_H */
