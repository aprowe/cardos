/* The card as a network drive. Device-only.
 *
 * A WebDAV server on port 80, one connection at a time, on a task of its own
 * so a file being written does not freeze the shell. Everything that is not
 * a socket is in kernel/net/dav.c and is tested on the host; this file is
 * the glue and is deliberately thin.
 *
 * THE RULE, from bg.h: this task never touches the display. It says what it
 * did through share_take_log, and the shell draws that.
 *
 * On while you want it and off otherwise. There is no authentication -- the
 * boundary is the LAN, the same one webproxy.py trusts -- which is why this
 * is a thing you turn on rather than a service. */
#ifndef CARDOS_SHARE_H
#define CARDOS_SHARE_H

/* Start serving. `owned_by_app` records that an app started it, so that the
 * app going away stops it (share_app_closed). 0 on success; -1 and
 * share_error() says why: no wifi, no card, too little memory, already on. */
int  share_start(int owned_by_app);
void share_stop(void);
int  share_running(void);
const char *share_url(void);        /* "http://192.168.1.23/" or "" */
const char *share_error(void);      /* the last reason share_start refused */
/* Next log line -- "PUT /desktop/x.capp 201" -- or NULL. One per call; the
 * string is valid until the next call. */
const char *share_take_log(void);
/* capprun: an app was released. Stops the share if an app owned it. */
void share_app_closed(void);

#endif /* CARDOS_SHARE_H */
