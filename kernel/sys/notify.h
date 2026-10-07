/* Notifications: anything can post one, and the OS shows it.
 *
 * A post is a banner across the top of whatever is on screen for a few
 * seconds and the notify chime (blip.c), and it goes in a list of the last
 * sixteen. fn-n opens the list -- the centre -- where Enter opens the app a
 * notification came from and d clears one. The launcher's bar has a dot
 * while any are unread.
 *
 * Three watchers post without their apps being open: Chat (a request to the
 * server every half minute while WiFi is up -- never bringing it up), the
 * Calendar app's cache (an event a few minutes before it starts), and the
 * alarm (logged as it rings; it keeps its own panel). Apps post with
 * api->notify. The list and the rules are kernel/sys/notifyq.c, host-tested.
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

/* `app` was opened: its notifications are read. */
void notify_opened(const char *app);

int  notify_unread(void);

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
int  notify_cal_on(void);
void notify_set_cal(int on);
int  notify_lead_min(void);           /* 5, 10, 15 or 30 */
void notify_set_lead_min(int m);

#endif /* CARDOS_NOTIFY_H */
