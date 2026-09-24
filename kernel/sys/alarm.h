/* Alarms ringing, whatever is on the screen. Device-only.
 *
 * /config/alarms.txt is the list (kernel/sys/alarmfmt.h has the format and
 * the rules); the Clock app edits it, and anything else may -- a card
 * reader, a command. This is what rings them: once a minute it reads the
 * file, and an alarm due this minute takes over the top of the screen --
 * a panel over whatever app is open, which is left exactly as it was, not
 * closed and not told. The backlight comes on and stays on, a beep repeats,
 * and a key answers it: `s` snoozes for nine minutes, anything else stops
 * it. Unanswered, it stops itself after five minutes.
 *
 * In the kernel rather than the Clock app because an alarm that only rings
 * with its app open is a timer. Needs the time: with no clock (never synced,
 * nothing restored) nothing rings rather than something ringing at a guess.
 */
#ifndef CARDOS_ALARM_H
#define CARDOS_ALARM_H

#include <stdint.h>

/* Once a pass of the shell's loop. Cheap: the file is read when the minute
 * changes, and ringing redraws twice a second. */
void alarm_tick(void);

/* Is one ringing? The loop hands it every key while it is. */
int  alarm_ringing(void);
void alarm_key(uint8_t k);

/* Draw the panel again, if one is ringing: after anything repaints the whole
 * screen (main's repaint_all, a screenshot), so the panel stays on top. */
void alarm_paint_over(void);

/* What repaints whatever the panel covered, set once by main. */
void alarm_set_repaint(void (*repaint)(void));

/* "07:00 Work", or "" -- the soonest enabled alarm, for anything that wants
 * to say what is next. */
const char *alarm_next_text(void);

#endif /* CARDOS_ALARM_H */
