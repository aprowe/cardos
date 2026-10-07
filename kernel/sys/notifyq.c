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

/* ---- calendar ---- */

static int field_ul(const char **p, unsigned long *v) {
  const char *s = *p;
  unsigned long n = 0;
  int any = 0;
  while (*s == ' ') s++;
  while (*s >= '0' && *s <= '9') { n = n * 10ul + (unsigned long)(*s++ - '0'); any = 1; }
  *p = s;
  *v = n;
  return any;
}

int nq_cal_due(const char *line, uint32_t now, int lead_s, uint32_t *start,
               char *summary, size_t n) {
  unsigned long all_day, dirty, deleted, st, en;
  const char *p = line;
  if (!field_ul(&p, &all_day) || !field_ul(&p, &dirty) || !field_ul(&p, &deleted) ||
      !field_ul(&p, &st) || !field_ul(&p, &en))
    return 0;
  (void)dirty; (void)en;
  if (all_day || deleted) return 0;
  if (!(st > now && st <= (unsigned long)now + (unsigned long)lead_s)) return 0;
  while (*p == ' ') p++;
  while (*p && *p != ' ') p++;                         /* the id */
  while (*p == ' ') p++;
  {
    size_t i = 0;
    for (; p[i] && p[i] != '\n' && p[i] != '\r' && i < n - 1; i++) summary[i] = p[i];
    summary[i] = 0;
  }
  *start = (uint32_t)st;
  return 1;
}

uint32_t nq_key(uint32_t start, const char *summary) {
  uint32_t h = 2166136261u ^ start;
  while (summary && *summary) h = (h ^ (uint8_t)*summary++) * 16777619u;
  return h;
}

int nq_fired_new(NqFired *f, uint32_t key) {
  int i;
  for (i = 0; i < f->n; i++) if (f->key[i] == key) return 0;
  f->key[f->next] = key;
  f->next = (f->next + 1) % NQ_FIRED;
  if (f->n < NQ_FIRED) f->n++;
  return 1;
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
