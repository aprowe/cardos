/* Notifications: anything can post one, and the OS shows it.
 *
 * A post is a banner across the top of whatever is on screen for a few
 * seconds and the notify chime (blip.c), and it goes in a list of the last
 * sixteen. fn-n opens the list -- the centre -- where Enter opens the app a
 * notification came from and d clears one. The launcher's bar has a dot
 * while any are unread.
 *
 * Posted without their apps open: Chat (a request to the server every half
 * minute while WiFi is up -- never bringing it up), the alarm (logged as it
 * rings; it keeps its own panel), and whatever an app scheduled with
 * notify_at -- Calendar its events' reminders, Timer its finish. Apps post
 * with api->notify. The list is kernel/sys/notifyq.c, host-tested.
 */
#ifndef CARDOS_NOTIFY_H
#define CARDOS_NOTIFY_H

#include <stdint.h>

void notify_init(void (*repaint)(void));

/* A banner and the chime, and into the list. */
void notify_post(const char *app, const char *title, const char *text);
/* Into the list only: for what has shown itself another way (an alarm). */
void notify_log(const char *app, const char *title, const char *text);

/* From the shell's loop: the banner's time, and the watchers. */
void notify_tick(uint32_t now_ms);

/* After a shell has painted: the banner goes back on top. */
void notify_paint_over(void);

/* ---- later -----------------------------------------------------------------
 *
 * A notification `seconds` from now, for `app`, named `key` so it can be
 * replaced or cancelled -- the Timer's finish, say. It fires whether the app
 * is open or not; if the app is on screen when it comes due, it is dropped,
 * since the app is showing it. Timed by the wall clock when there is one, and
 * then kept on the card through a restart; by uptime otherwise. `ring`: the
 * banner stays and the chime repeats, the screen awake, until a key (or a
 * minute). 0, or -1 if thirty-two are already waiting. key "*" to
 * notify_cancel is everything `app` set. */
int  notify_at(const char *app, const char *key, uint32_t seconds,
               const char *title, const char *text, int ring);
void notify_cancel(const char *app, const char *key);

/* Ringing: any key stops it, and is not passed on. */
int  notify_ringing(void);
void notify_dismiss(void);

/* `app` was opened: its notifications are read. */
void notify_opened(const char *app);

int  notify_unread(void);
/* The i-th unread one, newest first: 0 when there are not that many. For
 * the sleep clock, which lists them. */
int  notify_unread_at(int i, const char **app, const char **title, const char **text);

/* Rows at the top a banner covers right now (0 when there is none): a shell
 * leaves them alone, and repaints them when the banner goes. */
int  notify_covers(void);

/* The centre. While it is open it has every key. */
void notify_center_open(void);
int  notify_center_active(void);
void notify_center_key(uint8_t k);

/* Settings > Notifications. */
int  notify_chat_on(void);
void notify_set_chat(int on);


#endif /* CARDOS_NOTIFY_H */
