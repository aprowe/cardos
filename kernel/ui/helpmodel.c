/* The fn-h panel and the fn-space palette as state. See helpmodel.h. */

#include "kernel/ui/helpmodel.h"
#include "kernel/ui/appsearch.h"
#include "kernel/drv/keyboard.h"

#include <stdio.h>
#include <string.h>

/* ---- the help panel -------------------------------------------------------- */

int helpm_count(const char *s) {
  int n = 0;
  if (!s || !*s) return 0;
  for (; *s; s++) if (*s == '\n') n++;
  return n + (s[-1] == '\n' ? 0 : 1);
}

const char *helpm_line(const char *s, int n) {
  if (!s || n < 0) return NULL;
  while (n > 0 && *s) { if (*s++ == '\n') n--; }
  return *s ? s : NULL;
}

static int has(const HelpModel *m, int t) { return helpm_count(m->text[t]) > 0; }

void helpm_show(HelpModel *m, int tab) {
  int i;
  for (i = 0; i < HELP_TABS; i++) {
    int t = (tab + i) % HELP_TABS;
    if (has(m, t)) { m->tab = t; m->top = 0; return; }
  }
  m->tab = tab;
  m->top = 0;
}

static void step_tab(HelpModel *m, int d) {
  int i;
  for (i = 1; i <= HELP_TABS; i++) {
    int t = ((m->tab + d * i) % HELP_TABS + HELP_TABS) % HELP_TABS;
    if (has(m, t)) { m->tab = t; m->top = 0; return; }
  }
}

static void scroll(HelpModel *m, int d) {
  int n = helpm_count(m->text[m->tab]), rows = m->rows > 0 ? m->rows : 1;
  int max = n - rows > 0 ? n - rows : 0;
  m->top += d;
  if (m->top > max) m->top = max;
  if (m->top < 0) m->top = 0;
}

int helpm_key(HelpModel *m, uint8_t k) {
  int page = m->rows > 1 ? m->rows - 1 : 1;
  switch (k) {
  case KEY_ESC: case KEY_HELP: case KEY_QUIT: case KEY_ENTER: case KEY_BACKSPACE:
    return 0;
  case KEY_LEFT: case ',': step_tab(m, -1); return 1;
  case KEY_RIGHT: case '/': case '\t': step_tab(m, 1); return 1;
  case '1': case '2': case '3':
    if (has(m, k - '1')) { m->tab = k - '1'; m->top = 0; }
    return 1;
  case KEY_UP: case ';': scroll(m, -1); return 1;
  case KEY_DOWN: case '.': scroll(m, 1); return 1;
  case ' ': scroll(m, page); return 1;
  default: return 1;              /* anything else does nothing, and it stays */
  }
}

/* ---- the action palette ---------------------------------------------------- */

static void copy(char *d, const char *s, int n) {
  int i = 0;
  if (s) while (s[i] && i < n - 1) { d[i] = s[i]; i++; }
  d[i] = 0;
}

void pal_clear(Palette *p) { memset(p, 0, sizeof *p); }

int pal_add(Palette *p, int kind, const char *label, const char *chord,
            const char *group, const char *id, uint8_t key) {
  PalEntry *e;
  if (p->n >= PAL_MAX || !label || !*label) return -1;
  e = &p->e[p->n++];
  copy(e->label, label, sizeof e->label);
  copy(e->chord, chord, sizeof e->chord);
  copy(e->group, group, sizeof e->group);
  copy(e->id, id, sizeof e->id);
  e->kind = (uint8_t)kind;
  e->key = key;
  return 0;
}

void pal_chord(uint8_t k, char *out, int n) {
  if (k >= 1 && k <= 26) snprintf(out, (size_t)n, "ctrl-%c", 'a' + k - 1);
  else if (k >= 0xE0 && k <= 0xF9) snprintf(out, (size_t)n, "fn-%c", 'a' + k - 0xE0);
  else if (k >= 0xC0 && k <= 0xD9) snprintf(out, (size_t)n, "opt-%c", 'a' + k - 0xC0);
  else if (n) out[0] = 0;
}

void pal_filter(Palette *p) {
  const char *names[PAL_MAX];
  int i;
  for (i = 0; i < p->n; i++) names[i] = p->e[i].label;
  p->nhit = appsearch_rank(names, p->n, p->q, p->hit, PAL_MAX);
  p->sel = 0;
  p->top = 0;
}

int pal_key(Palette *p, uint8_t k, int rows) {
  int n = (int)strlen(p->q);
  if (rows < 1) rows = 1;
  if (k == KEY_ESC || k == KEY_QUIT || k == KEY_ACTIONS) return PAL_CLOSED;
  if (k == KEY_ENTER) return p->nhit ? PAL_CHOSEN : PAL_OPEN;
  if (k == KEY_UP) { p->sel = p->sel > 0 ? p->sel - 1 : (p->nhit ? p->nhit - 1 : 0); }
  else if (k == KEY_DOWN) { p->sel = p->sel + 1 < p->nhit ? p->sel + 1 : 0; }
  else if (k == KEY_BACKSPACE) {
    if (!n) return PAL_CLOSED;
    p->q[n - 1] = 0;
    pal_filter(p);
  } else if (k >= 32 && k < 127 && n < (int)sizeof p->q - 1) {
    p->q[n] = (char)k;
    p->q[n + 1] = 0;
    pal_filter(p);
  }
  if (p->sel < p->top) p->top = p->sel;
  if (p->sel >= p->top + rows) p->top = p->sel - rows + 1;
  return PAL_OPEN;
}
