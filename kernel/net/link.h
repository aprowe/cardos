/* Two devices, one game, over ESP-NOW. The protocol is linkproto.c; this is
 * the radio. Apps reach it as api->link() (CappLink in capp.h). */
#ifndef CARDOS_LINK_H
#define CARDOS_LINK_H

/* Start looking for others playing `game`; `me` NULL is the Chat name, or
 * "Cardputer XXXX". 0, or -1 with link_why() saying why. */
int  link_open(const char *game, const char *me);
void link_close(void);
void link_release_owner(const void *owner);   /* capprun, when the app goes */
int  link_active(void);

/* From the shell's loop: frames in, timers, the channel. */
void link_tick(void);

int  link_state(void);                 /* LP_* in linkproto.h, same as CAPP_LINK_* */
int  link_peer_count(void);
const char *link_peer_name(int i);     /* -1: the one in the game */
int  link_invite(int i);
int  link_answer(int yes);
int  link_role(void);
int  link_send(const void *buf, int len);
int  link_recv(void *buf, int max);
void link_look(void);
const char *link_why(void);
int  link_channel(void);

#endif /* CARDOS_LINK_H */
