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

/* ---- calendar ----------------------------------------------------------
 *
 * One line of the Calendar app's cache (apps/calendar.c, cache_save):
 *   all_day dirty deleted start end id summary
 * 1 if it is a timed, not-deleted event starting in (now, now + lead_s],
 * with its start and summary. */
int nq_cal_due(const char *line, uint32_t now, int lead_s, uint32_t *start,
               char *summary, size_t n);

/* Events already announced, so a minute's check does not announce one
 * twice. 1 if `key` is new (and now remembered). */
#define NQ_FIRED 24
typedef struct { uint32_t key[NQ_FIRED]; int n, next; } NqFired;
int nq_fired_new(NqFired *f, uint32_t key);
uint32_t nq_key(uint32_t start, const char *summary);

/* ---- the server's answer -------------------------------------------------
 *
 * GET /notify/poll?chat=ID&me=NAME answers
 *   ok LAST_CHAT_ID
 *   chat <tab> name <tab> text        (newest messages from others, oldest first)
 * Returns the last chat id (or -1 if the reply is not that), calling `each`
 * for every chat line. The reply is read, not changed. */
int nq_parse_poll(const char *reply,
                  void (*each)(void *ctx, const char *name, size_t name_len,
                               const char *text, size_t text_len),
                  void *ctx);

#endif /* CARDOS_NOTIFYQ_H */
