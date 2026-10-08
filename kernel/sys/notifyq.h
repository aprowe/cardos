/* Notifications, the part with no device in it: the list of recent ones,
 * when a calendar event is due to be announced, and what the server's
 * /notify/poll said. Portable, so the host suite runs it; kernel/sys/notify.c
 * is the banner, the centre, the sound and the watching.
 */
#ifndef CARDOS_NOTIFYQ_H
#define CARDOS_NOTIFYQ_H

#include <stddef.h>
#include <stdint.h>

#define NQ_MAX      16
#define NQ_APP      12
#define NQ_TITLE    32
#define NQ_TEXT     72

typedef struct {
  char     app[NQ_APP];        /* who posted it: "Chat", "Calendar", an app's name */
  char     title[NQ_TITLE];
  char     text[NQ_TEXT];
  uint32_t at;                 /* seconds since the epoch, or uptime if no clock */
  uint8_t  read;
} NqItem;

/* Newest first. */
typedef struct {
  NqItem it[NQ_MAX];
  int    n;
} Nq;

void nq_init(Nq *q);
void nq_push(Nq *q, const char *app, const char *title, const char *text, uint32_t at);
void nq_remove(Nq *q, int i);
int  nq_unread(const Nq *q);
void nq_read_all(Nq *q);
/* Everything from `app` is read: it has been opened. */
void nq_read_app(Nq *q, const char *app);

/* ---- the server's answer -------------------------------------------------
 *
 * GET /notify/poll?chat=ID&me=NAME&note=ID answers
 *   ok LAST_CHAT_ID [LAST_NOTE_ID]
 *   chat <tab> name <tab> text                (messages from others, oldest first)
 *   note <tab> app <tab> title <tab> text     (the server's own: a Build done)
 * Returns the last chat id (or -1 if the reply is not that), calling `each`
 * for every chat line and `note` for every note line; *note_last is the
 * second number, or -1 when there is none. Any callback may be NULL. The
 * reply is read, not changed. */
typedef struct { const char *s; size_t n; } NqStr;
int nq_parse_poll(const char *reply,
                  void (*each)(void *ctx, const char *name, size_t name_len,
                               const char *text, size_t text_len),
                  void (*note)(void *ctx, NqStr app, NqStr title, NqStr text),
                  int *note_last, void *ctx);

#endif /* CARDOS_NOTIFYQ_H */
