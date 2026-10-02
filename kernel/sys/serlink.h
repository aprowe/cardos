/* The serial link: a PC working on the device over USB without typing at it.
 * Device-only; the frame format is serframe.h, the PC end tools/cardctl.py.
 *
 * Requests, one field per tab, each answered `ok` or `err` with data:
 *   ping                     "serlink 1"
 *   state                    which shell and app are up, heap, uptime
 *   sh LINE                  a console line, its output captured (nothing drawn)
 *   ls PATH                  "name TAB size TAB d|f TAB mtime" a line
 *   stat PATH                "size TAB d|f TAB mtime"
 *   get PATH OFF LEN         the bytes, at most SF_CHUNK
 *   put PATH OFF BASE64      into PATH.part; OFF 0 starts it. A retry of a
 *                            chunk that already landed is answered ok
 *   commit PATH SIZE         PATH.part becomes PATH, if it is SIZE bytes
 *   rm PATH / mv A B / mkdir PATH
 *   open NAME [ARGS]         an app, as a hotkey would open it
 *   key HEX                  keystrokes, delivered as if typed
 *   shot                     the screen into /shots/link.565; its path
 *   re                       the last reply again (one lost to a log line)
 *
 * Everything runs on the shell's loop, between events, which is where the
 * card and the apps are safe to touch -- whatever is on screen.
 */
#ifndef CARDOS_SERLINK_H
#define CARDOS_SERLINK_H

#include <stddef.h>

typedef struct {
  /* A console line with output captured; nonzero if refused. */
  int  (*shell)(const char *line, char *out, size_t n);
  /* Open an app by name; 0 if it opened. */
  int  (*open)(const char *name, const char *args);
  /* One line about the device as it is. */
  void (*state)(char *out, size_t n);
  /* Paint all of the current shell, for a screenshot. */
  void (*repaint)(void);
} SerlinkHooks;

void serlink_init(const SerlinkHooks *hooks);

/* In place of con_serial_key in the shell's loop: the next keystroke from
 * the serial line, or 0. A frame is read whole and handled here, and sets
 * *activity, since a PC at work is not an idle machine. */
int serlink_key(int *activity);

#endif /* CARDOS_SERLINK_H */
