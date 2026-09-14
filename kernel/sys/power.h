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
 *   30 seconds idle    dim to the setting's floor
 *   2 minutes idle     backlight off; the machine keeps running
 *   any key or click   back to the brightness you chose, immediately
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

/* Called every pass of the shell's loop. Cheap, and does nothing at all until
 * a threshold is crossed. */
void power_tick(void);

/* Somebody did something: undim now, before the next tick. Called from the
 * same place that resets the idle clock. */
void power_wake(void);

/* Is the screen currently dimmed or dark? For a shell that wants to swallow
 * the keypress that woke it rather than acting on it. */
int  power_dimmed(void);

#endif /* CARDOS_POWER_H */
