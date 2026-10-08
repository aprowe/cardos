/* Two devices, one game, no router: the protocol behind api->link.
 *
 * Portable C with no ESP-IDF, so the host suite can wire two of these
 * together through a fake radio and lose, repeat and reorder packets on
 * purpose. kernel/net/link.c is the ESP-NOW glue around it: it hands every
 * received frame to lp_rx, calls lp_tick from the shell's loop, and sends
 * whatever lp's `tx` asks for.
 *
 * Frames are at most LP_FRAME bytes (ESP-NOW's 250):
 *     'C' 'L' version type   game hash (4, LE)   session (2)   seq (2)   ack (2)
 *     payload
 *
 *   HELLO   broadcast while looking, a few times a second: flags, the name
 *   INVITE  to one peer: the inviter's name, a fresh session number
 *   ACCEPT / DECLINE  the answer, echoing the session
 *   DATA    one message of a session, numbered; resent until acknowledged
 *   ACK     "I have up to seq"
 *   PING    "still here" when there has been nothing else to say
 *   BYE     the other side left
 *
 * A session delivers messages whole, in order, once each (stop-and-wait:
 * games send a move at a time, not a stream). It ends when either side
 * leaves or nothing has been heard for LP_LOST_MS.
 */
#ifndef CARDOS_LINKPROTO_H
#define CARDOS_LINKPROTO_H

#include <stddef.h>
#include <stdint.h>

#define LP_FRAME      250
#define LP_HEAD       14
#define LP_MSG_MAX    200          /* one message's payload */
#define LP_NAME_MAX   16           /* with its NUL */
#define LP_PEERS      8
#define LP_QUEUE      6            /* messages each way */

#define LP_HELLO_MS   250
#define LP_PEER_MS    3000         /* a peer not heard this long is gone from the list */
#define LP_INVITE_MS  300          /* an invite is said again this often */
#define LP_INVITE_FOR 20000        /* and given up after this long */
#define LP_RESEND_MS  150
#define LP_PING_MS    1000
#define LP_LOST_MS    8000

enum { LP_OFF = 0, LP_LOOKING, LP_INVITING, LP_INVITED, LP_CONNECTED, LP_ENDED };

enum {
  LPT_HELLO = 1, LPT_INVITE, LPT_ACCEPT, LPT_DECLINE, LPT_DATA, LPT_ACK, LPT_PING, LPT_BYE
};

typedef struct {
  uint8_t  mac[6];
  char     name[LP_NAME_MAX];
  uint32_t heard;              /* ms */
  uint8_t  flags;              /* LP_F_* from their HELLO */
} LpPeer;

#define LP_F_FIXED 0x01        /* on a router's channel: the other side should come to it */

typedef struct {
  uint8_t len;
  uint8_t d[LP_MSG_MAX];
} LpMsg;

typedef void (*LpTx)(void *ctx, const uint8_t *mac /* NULL: everyone */,
                     const uint8_t *frame, int len);

typedef struct {
  int      state;
  uint32_t game;               /* hash of the game's name */
  char     me[LP_NAME_MAX];
  uint8_t  my_mac[6];
  uint8_t  flags;              /* sent in HELLO */
  LpTx     tx;
  void    *ctx;

  LpPeer   peers[LP_PEERS];
  int      npeers;

  /* the session */
  uint8_t  peer[6];
  char     peer_name[LP_NAME_MAX];
  uint16_t session;
  int      role;               /* 0 invited them (goes first), 1 was invited */
  uint32_t since;              /* when the invite went, or came */
  uint32_t heard;              /* last frame from the peer */
  uint32_t said;               /* last frame to the peer */
  uint32_t hello_at, resend_at;
  uint16_t tx_seq;             /* the number of out[0] when sent */
  uint16_t rx_next;            /* the number expected next */
  LpMsg    out[LP_QUEUE];
  int      nout;
  LpMsg    in[LP_QUEUE];
  int      nin;
  char     why[48];            /* why it ended, or was refused */
  uint32_t rand;
} LinkProto;

uint32_t lp_hash(const char *s);

void lp_init(LinkProto *lp, const uint8_t my_mac[6], LpTx tx, void *ctx, uint32_t seed);
/* Start looking for others playing `game`. */
void lp_open(LinkProto *lp, const char *game, const char *me, uint32_t now);
/* Leave: a session's peer is told. Back to OFF. */
void lp_close(LinkProto *lp);
/* From ENDED (or anywhere) back to LOOKING, the session forgotten. */
void lp_look(LinkProto *lp, uint32_t now);

void lp_rx(LinkProto *lp, const uint8_t mac[6], const uint8_t *frame, int len, uint32_t now);
void lp_tick(LinkProto *lp, uint32_t now);

int  lp_invite(LinkProto *lp, int peer, uint32_t now);   /* 0, or -1 */
int  lp_answer(LinkProto *lp, int yes, uint32_t now);    /* to an INVITED */

/* A session's messages. send: 0 queued, -1 not connected or full. recv:
 * the next one's length, copied (cut to max), or 0 if none. */
int  lp_send(LinkProto *lp, const void *buf, int len, uint32_t now);
int  lp_recv(LinkProto *lp, void *buf, int max);

#endif /* CARDOS_LINKPROTO_H */
