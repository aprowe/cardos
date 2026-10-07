/* Notifications, portable part. See notifyq.h. */
#include "kernel/sys/notifyq.h"

#include <string.h>

static void copy(char *out, size_t n, const char *s) {
  size_t i = 0;
  if (!n) return;
  for (; s && s[i] && i < n - 1; i++) out[i] = s[i];
  out[i] = 0;
}

void nq_init(Nq *q) { memset(q, 0, sizeof *q); }

void nq_push(Nq *q, const char *app, const char *title, const char *text, uint32_t at) {
  NqItem *it;
  if (q->n == NQ_MAX) q->n--;                          /* the oldest goes */
  memmove(&q->it[1], &q->it[0], (size_t)q->n * sizeof q->it[0]);
  it = &q->it[0];
  copy(it->app, sizeof it->app, app);
  copy(it->title, sizeof it->title, title);
  copy(it->text, sizeof it->text, text);
  it->at = at;
  it->read = 0;
  q->n++;
}

void nq_remove(Nq *q, int i) {
  if (i < 0 || i >= q->n) return;
  memmove(&q->it[i], &q->it[i + 1], (size_t)(q->n - i - 1) * sizeof q->it[0]);
  q->n--;
}

int nq_unread(const Nq *q) {
  int i, u = 0;
  for (i = 0; i < q->n; i++) u += !q->it[i].read;
  return u;
}

void nq_read_all(Nq *q) {
  int i;
  for (i = 0; i < q->n; i++) q->it[i].read = 1;
}

void nq_read_app(Nq *q, const char *app) {
  int i;
  for (i = 0; i < q->n; i++)
    if (!strcmp(q->it[i].app, app)) q->it[i].read = 1;
}

/* ---- the server's answer ---- */

int nq_parse_poll(const char *reply,
                  void (*each)(void *ctx, const char *name, size_t name_len,
                               const char *text, size_t text_len),
                  void *ctx) {
  const char *p = reply;
  int last = 0, any = 0;
  if (!p || strncmp(p, "ok ", 3)) return -1;
  p += 3;
  while (*p >= '0' && *p <= '9') { last = last * 10 + (*p++ - '0'); any = 1; }
  if (!any) return -1;
  while (*p && *p != '\n') p++;
  while (*p == '\n') {
    const char *line = ++p, *tab1, *tab2, *end;
    end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    if (end - line > 5 && !strncmp(line, "chat\t", 5)) {
      tab1 = line + 5;
      tab2 = memchr(tab1, '\t', (size_t)(end - tab1));
      if (tab2 && each) each(ctx, tab1, (size_t)(tab2 - tab1), tab2 + 1, (size_t)(end - tab2 - 1));
    }
    p = end;
  }
  return last;
}
