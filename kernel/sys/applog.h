/* The app log, on the card. Device-only.
 *
 * `api->log()` used to go to ESP_LOGI and nowhere else, which means it went
 * to a USB serial port that drops out constantly and is not attached at all
 * when the device is in a pocket. An app whose sync fails once an hour cannot
 * be debugged that way: by the time you plug in, the evidence is gone.
 *
 * So it goes to /cache/app.log as well, tagged with the app that wrote it,
 * stamped with uptime, and rotated at 32 KB so the card cannot fill. `log` in
 * the console prints the tail of it; `log clear` empties it.
 *
 * The policy -- what a line looks like, when to rotate -- is in
 * kernel/sys/logring.c, where the host suite can reach it.
 */
#ifndef CARDOS_APPLOG_H
#define CARDOS_APPLOG_H

#define APPLOG_PATH "/cache/app.log"
#define APPLOG_PREV "/cache/app.log.1"

/* One entry. `tag` is usually the running app's name. Safe to call from any
 * task; safe before the card is mounted, when it does nothing. */
void applog(const char *tag, const char *msg);

/* printf-style, for callers that would otherwise build the string themselves
 * and get the buffer size wrong. */
void applogf(const char *tag, const char *fmt, ...);

/* Both files, gone. */
void applog_clear(void);

/* The last `max_lines` of the log, written out through `emit` one line at a
 * time (the console's writer, typically). Returns the number printed. */
int  applog_tail(int max_lines, void (*emit)(const char *line));

#endif /* CARDOS_APPLOG_H */
