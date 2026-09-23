#include "kernel/app/cmdline.h"

#include <stdio.h>
#include <string.h>

int cmdline_split(const char *line, char *buf, size_t n, const char **w, int max) {
  size_t o = 0;
  int count = 0;
  const char *p = line ? line : "";

  for (;;) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return count;
    if (count >= max) return -1;
    w[count++] = buf + o;
    if (*p == '"') {
      p++;
      while (*p && *p != '"') {
        if (o + 1 >= n) return -1;
        buf[o++] = *p++;
      }
      if (*p != '"') return -1;                  /* never closed */
      p++;
    } else {
      while (*p && *p != ' ' && *p != '\t') {
        if (o + 1 >= n) return -1;
        buf[o++] = *p++;
      }
    }
    if (o + 1 > n) return -1;
    buf[o++] = 0;
  }
}

int cmdline_is_command(const CappAction *a) {
  return a && (a->cmd & CAPP_CMD_YES) && a->id;
}

const CappAction *cmdline_find(const CappAction *t, int n, const char *id) {
  int i;
  if (!t || !id) return NULL;
  for (i = 0; i < n; i++)
    if (cmdline_is_command(&t[i]) && !strcmp(t[i].id, id)) return &t[i];
  return NULL;
}

static int is_int(const char *s) {
  if (*s == '-') s++;
  if (!*s) return 0;
  for (; *s; s++) if (*s < '0' || *s > '9') return 0;
  return 1;
}

static int is_bool(const char *s) {
  static const char *const WORDS[] = { "yes", "no", "true", "false", "on",
                                       "off", "1", "0" };
  size_t i;
  for (i = 0; i < sizeof WORDS / sizeof WORDS[0]; i++)
    if (!strcmp(s, WORDS[i])) return 1;
  return 0;
}

/* Is `s` one of the |-separated words in `choices`? */
static int is_choice(const char *s, const char *choices) {
  size_t len = strlen(s);
  const char *p = choices ? choices : "";
  while (*p) {
    const char *end = strchr(p, '|');
    size_t wl = end ? (size_t)(end - p) : strlen(p);
    if (wl == len && !strncmp(p, s, len)) return 1;
    if (!end) break;
    p = end + 1;
  }
  return 0;
}

int cmdline_bind(const CappAction *a, int nwords, const char *const *words,
                 const char **argv, char *join, size_t jn, char *why, size_t wn) {
  int np, i;

  if (!cmdline_is_command(a)) {
    snprintf(why, wn, "not a command");
    return -1;
  }
  np = a->nparams;
  if (np > CAPP_CMD_ARGS_MAX) np = CAPP_CMD_ARGS_MAX;

  /* A trailing TEXT parameter takes the rest of the sentence. */
  if (np > 0 && nwords > np && a->params[np - 1].type == CAPP_ARG_TEXT) {
    size_t o = 0;
    join[0] = 0;
    for (i = np - 1; i < nwords; i++) {
      size_t l = strlen(words[i]);
      if (o + l + 2 > jn) { snprintf(why, wn, "too long"); return -1; }
      if (o) join[o++] = ' ';
      memcpy(join + o, words[i], l);
      o += l;
      join[o] = 0;
    }
    for (i = 0; i < np - 1; i++) argv[i] = words[i];
    argv[np - 1] = join;
    nwords = np;
  } else {
    for (i = 0; i < nwords && i < CAPP_CMD_ARGS_MAX; i++) argv[i] = words[i];
  }

  if (nwords != np) {
    snprintf(why, wn, "%s takes %d argument%s, not %d", a->id, np,
             np == 1 ? "" : "s", nwords);
    return -1;
  }
  for (i = 0; i < np; i++) {
    const CappParam *p = &a->params[i];
    int ok = p->type == CAPP_ARG_TEXT   ? argv[i][0] != 0
           : p->type == CAPP_ARG_INT    ? is_int(argv[i])
           : p->type == CAPP_ARG_BOOL   ? is_bool(argv[i])
           : p->type == CAPP_ARG_CHOICE ? is_choice(argv[i], p->about)
           : 0;
    if (!ok) {
      if (p->type == CAPP_ARG_CHOICE)
        snprintf(why, wn, "%s must be one of %s", p->name, p->about ? p->about : "");
      else
        snprintf(why, wn, "%s must be %s", p->name,
                 p->type == CAPP_ARG_INT ? "a number" :
                 p->type == CAPP_ARG_BOOL ? "yes or no" : "some text");
      return -1;
    }
  }
  return np;
}

void cmdline_catalog_line(const char *app, const CappAction *a, char *out, size_t n) {
  size_t o;
  int i;
  o = (size_t)snprintf(out, n, "%s %s", app, a->id);
  for (i = 0; i < a->nparams && o < n; i++) {
    const CappParam *p = &a->params[i];
    const char *t = p->type == CAPP_ARG_TEXT ? "text" : p->type == CAPP_ARG_INT ? "int"
                  : p->type == CAPP_ARG_BOOL ? "bool" : (p->about ? p->about : "choice");
    o += (size_t)snprintf(out + o, n - o, " %s:%s", p->name, t);
  }
  if (o < n && (a->cmd & CAPP_CMD_NET)) o += (size_t)snprintf(out + o, n - o, " net");
  if (o < n && a->about) snprintf(out + o, n - o, " # %s", a->about);
}
