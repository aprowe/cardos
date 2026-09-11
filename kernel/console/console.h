/* Text console on the ST7789: 240x135 at 6x8 per cell = exactly 40x16.
 *
 * The con_ prefix is deliberate: ESP-IDF's esp_stdio component exports its own
 * console_write, and a console_-prefixed API collides with it at link time. */
#ifndef CARDOS_CONSOLE_H
#define CARDOS_CONSOLE_H

#include <stdint.h>

#define CON_COLS 40
#define CON_ROWS 16

int  con_init(void);
void con_putc(char c);
void con_write(const char *s);
void con_printf(const char *fmt, ...);
void con_clear(void);

/* Draw the cursor block, or erase it. The main loop blinks it. */
void con_cursor(int visible);

void con_set_color(uint16_t fg);

/* Mirror everything the console prints to stdout as well as the panel, and
 * let characters arriving on stdin act as keypresses. The Cardputer's screen
 * is 40x16 and cannot be photographed into a bug report, so having the same
 * text on the wire is the difference between debugging and guessing. Costs
 * nothing when nothing is listening. */
void con_set_serial(int on);
int  con_serial_key(void);   /* next character from stdin, or 0 */

#endif /* CARDOS_CONSOLE_H */
