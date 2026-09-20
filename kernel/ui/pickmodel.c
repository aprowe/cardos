/* The file picker's model. See pickmodel.h. Portable. */
#include "kernel/ui/pickmodel.h"

#include <stdio.h>
#include <string.h>

#define K_UP    0x80
#define K_DOWN  0x81
#define K_LEFT  0x82
#define K_RIGHT 0x83
#define K_ENTER 0x0D
#define K_BACK  0x08
#define K_TAB   0x09
#define K_ESC   0x1B
#define K_DEL   0x7F
#define CTRL(c) ((c) - 'a' + 1)

#define JUMP_WINDOW_MS 1000
#define DEFAULT_ROWS   9

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int name_cmp(const char *a, const char *b) {
  while (*a && lower(*a) == lower(*b)) { a++; b++; }
  return lower(*a) - lower(*b);
}

static void set_note(PickModel *m, const char *s) {
  snprintf(m->note, sizeof m->note, "%s", s);
  m->dirty = 1;
}

void pm_path_of(const PickModel *m, const char *name, char *out, size_t n) {
  if (!strcmp(m->dir, "/")) snprintf(out, n, "/%s", name);
  else snprintf(out, n, "%s/%s", m->dir, name);
}

/* ---- the listing --------------------------------------------------------- */

static int has_ext(const char *name, const char *filter) {
  const char *dot = strrchr(name, '.');
  const char *p = filter;
  if (!dot) return 0;
  dot++;
  while (*p) {
    const char *q = p;
    size_t n;
    while (*q && *q != ',') q++;
    n = (size_t)(q - p);
    if (n && strlen(dot) == n) {
      size_t i;
      for (i = 0; i < n && lower(dot[i]) == lower(p[i]); i++) ;
      if (i == n) return 1;
    }
    p = *q ? q + 1 : q;
  }
  return 0;
}

static int wanted(const PickModel *m, const PmEntry *e) {
  if (e->name[0] == '.' && !m->show_hidden) return 0;
  if (e->is_dir) return 1;
  if (m->mode == PM_FOLDER) return 0;
  if (m->filter[0] && !has_ext(e->name, m->filter)) return 0;
  return 1;
}

static void sort_entries(PmEntry *e, int n) {
  int i, j;
  for (i = 1; i < n; i++) {                 /* insertion sort: n <= 64 */
    PmEntry t = e[i];
    for (j = i; j > 0; j--) {
      const PmEntry *p = &e[j - 1];
      int before = p->synthetic ? 1
                 : t.synthetic ? 0
                 : (p->is_dir != t.is_dir) ? p->is_dir
                 : name_cmp(p->name, t.name) <= 0;
      if (before) break;
      e[j] = e[j - 1];
    }
    e[j] = t;
  }
}

static void keep_visible(PickModel *m) {
  int rows = m->rows > 0 ? m->rows : DEFAULT_ROWS;
  if (m->sel < 0) m->sel = 0;
  if (m->sel >= m->n) m->sel = m->n ? m->n - 1 : 0;
  if (m->sel < m->top) m->top = m->sel;
  if (m->sel >= m->top + rows) m->top = m->sel - rows + 1;
  if (m->top < 0) m->top = 0;
  m->dirty = 1;
}

/* Read the folder, and put the cursor on `select` if it is there. */
static void load(PickModel *m, const char *select) {
  PmEntry raw[PM_MAX];
  int got, i;

  m->n = 0;
  m->truncated = 0;
  if (m->mode == PM_FOLDER) {
    PmEntry *s = &m->ent[m->n++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", "[ use this folder ]");
    s->is_dir = 1;
    s->synthetic = 1;
  }
  got = m->fs.list ? m->fs.list(m->dir, raw, PM_MAX) : -1;
  if (got < 0) { got = 0; set_note(m, "cannot read this folder"); }
  if (got >= PM_MAX) m->truncated = 1;
  for (i = 0; i < got && m->n < PM_MAX; i++)
    if (wanted(m, &raw[i])) m->ent[m->n++] = raw[i];
  sort_entries(m->ent, m->n);

  m->sel = 0;
  m->top = 0;
  if (select)
    for (i = 0; i < m->n; i++)
      if (!strcmp(m->ent[i].name, select)) { m->sel = i; break; }
  keep_visible(m);
}

void pm_refresh(PickModel *m) {
  char keep[PM_NAME_MAX];
  snprintf(keep, sizeof keep, "%s", m->n ? m->ent[m->sel].name : "");
  load(m, keep);
}

static void descend(PickModel *m, const char *name) {
  char path[PM_PATH_MAX];
  pm_path_of(m, name, path, sizeof path);
  snprintf(m->dir, sizeof m->dir, "%s", path);
  m->note[0] = 0;
  load(m, NULL);
}

static void ascend(PickModel *m) {
  char *slash = strrchr(m->dir, '/');
  char child[PM_NAME_MAX];
  if (!slash || !strcmp(m->dir, "/")) return;
  snprintf(child, sizeof child, "%s", slash + 1);
  if (slash == m->dir) m->dir[1] = 0; else *slash = 0;
  m->note[0] = 0;
  load(m, child);
}

/* ---- begin --------------------------------------------------------------- */

void pm_begin(PickModel *m, const PmFs *fs, int mode, const char *title,
              const char *dir, const char *filter, const char *name) {
  int is_dir = 0;
  const char *p;
  size_t i = 0;

  memset(m, 0, sizeof *m);
  m->fs = *fs;
  m->mode = mode;
  m->rows = DEFAULT_ROWS;
  snprintf(m->title, sizeof m->title, "%s", title ? title : "Choose");
  for (p = filter ? filter : ""; *p && i < sizeof m->filter - 1; p++)
    if (*p != '.' && *p != ' ') m->filter[i++] = (char)lower(*p);
  m->filter[i] = 0;

  if (dir && *dir && fs->exists && fs->exists(dir, &is_dir) && is_dir)
    snprintf(m->dir, sizeof m->dir, "%s", dir);
  else if (fs->exists && fs->exists("/home", &is_dir) && is_dir)
    snprintf(m->dir, sizeof m->dir, "%s", "/home");
  else
    snprintf(m->dir, sizeof m->dir, "%s", "/");
  {
    size_t n = strlen(m->dir);
    if (n > 1 && m->dir[n - 1] == '/') m->dir[n - 1] = 0;
  }

  if (mode == PM_SAVE) {
    snprintf(m->name, sizeof m->name, "%s", name ? name : "");
    snprintf(m->field, sizeof m->field, "%s", m->name);
    m->field_len = (int)strlen(m->field);
    m->ask = PM_ASK_NAME;
  }
  load(m, NULL);
}

/* ---- finishing ----------------------------------------------------------- */

static int finish(PickModel *m, const char *path) {
  snprintf(m->result, sizeof m->result, "%s", path);
  m->done = 1;
  m->dirty = 1;
  return 1;
}

static int cancel(PickModel *m) {
  m->done = -1;
  m->dirty = 1;
  return 1;
}

static int valid_name(PickModel *m, const char *name) {
  if (!name[0]) { set_note(m, "a name is needed"); return 0; }
  if (strchr(name, '/') || strchr(name, '\\')) { set_note(m, "no slashes in a name"); return 0; }
  if (!strcmp(name, ".") || !strcmp(name, "..")) { set_note(m, "not a name"); return 0; }
  return 1;
}

/* Save: the name in the field, or ask before replacing. */
static int try_save(PickModel *m) {
  char path[PM_PATH_MAX];
  int is_dir = 0;
  if (!valid_name(m, m->field)) return 0;
  snprintf(m->name, sizeof m->name, "%s", m->field);
  pm_path_of(m, m->field, path, sizeof path);
  if (m->fs.exists && m->fs.exists(path, &is_dir)) {
    if (is_dir) { set_note(m, "that is a folder"); return 0; }
    m->ask = PM_ASK_OVERWRITE;
    m->dirty = 1;
    return 0;
  }
  return finish(m, path);
}

/* ---- the field ----------------------------------------------------------- */

static void field_set(PickModel *m, const char *s) {
  snprintf(m->field, sizeof m->field, "%s", s ? s : "");
  m->field_len = (int)strlen(m->field);
  m->dirty = 1;
}

static int field_key(PickModel *m, uint8_t k) {
  if (k == K_BACK) {
    if (m->field_len) m->field[--m->field_len] = 0;
    m->dirty = 1;
    return 1;
  }
  if (k >= 0x20 && k < 0x7F) {
    if (m->field_len < (int)sizeof m->field - 1) {
      m->field[m->field_len++] = (char)k;
      m->field[m->field_len] = 0;
    }
    m->dirty = 1;
    return 1;
  }
  return 0;
}

static void commit_mkdir(PickModel *m) {
  char path[PM_PATH_MAX];
  if (!valid_name(m, m->field)) return;
  pm_path_of(m, m->field, path, sizeof path);
  if (!m->fs.mkdir || m->fs.mkdir(path) != 0) { set_note(m, "could not make that folder"); return; }
  m->ask = PM_ASK_NONE;
  m->note[0] = 0;
  load(m, m->field);
}

static void commit_rename(PickModel *m) {
  char from[PM_PATH_MAX], to[PM_PATH_MAX];
  if (!valid_name(m, m->field)) return;
  pm_path_of(m, m->ent[m->sel].name, from, sizeof from);
  pm_path_of(m, m->field, to, sizeof to);
  if (!strcmp(from, to)) { m->ask = PM_ASK_NONE; m->dirty = 1; return; }
  if (m->fs.exists && m->fs.exists(to, NULL)) { set_note(m, "that name is taken"); return; }
  if (!m->fs.rename || m->fs.rename(from, to) != 0) { set_note(m, "could not rename"); return; }
  m->ask = PM_ASK_NONE;
  m->note[0] = 0;
  load(m, m->field);
}

static void commit_delete(PickModel *m) {
  char path[PM_PATH_MAX];
  int sel = m->sel;
  pm_path_of(m, m->ent[m->sel].name, path, sizeof path);
  m->ask = PM_ASK_NONE;
  if (!m->fs.remove || m->fs.remove(path) != 0) {
    set_note(m, m->ent[m->sel].is_dir ? "not deleted: folder not empty"
                                      : "could not delete");
    return;
  }
  m->note[0] = 0;
  load(m, NULL);
  m->sel = sel;
  keep_visible(m);
}

/* ---- the list ------------------------------------------------------------ */

static int choose(PickModel *m) {
  PmEntry *e;
  char path[PM_PATH_MAX];
  if (!m->n) return 0;
  e = &m->ent[m->sel];
  if (e->synthetic) return finish(m, m->dir);
  if (e->is_dir) { descend(m, e->name); return 0; }
  pm_path_of(m, e->name, path, sizeof path);
  if (m->mode == PM_SAVE) {
    field_set(m, e->name);
    snprintf(m->name, sizeof m->name, "%s", e->name);
    m->ask = PM_ASK_OVERWRITE;
    return 0;
  }
  return finish(m, path);
}

static void jump(PickModel *m, uint8_t k, uint32_t now_ms) {
  size_t n = strlen(m->jump);
  int i;
  if ((uint32_t)(now_ms - m->jump_at) > JUMP_WINDOW_MS) { n = 0; m->jump[0] = 0; }
  m->jump_at = now_ms;
  if (n < sizeof m->jump - 1) { m->jump[n++] = (char)lower(k); m->jump[n] = 0; }
  for (i = 0; i < m->n; i++) {
    size_t j;
    for (j = 0; j < n && lower(m->ent[i].name[j]) == m->jump[j]; j++) ;
    if (j == n) { m->sel = i; keep_visible(m); return; }
  }
}

static int list_key(PickModel *m, uint8_t k, uint32_t now_ms) {
  switch (k) {
  case K_UP:    m->sel--; keep_visible(m); return 0;
  case K_DOWN:  m->sel++; keep_visible(m); return 0;
  case K_ENTER: case K_RIGHT: return choose(m);
  case K_BACK:  case K_LEFT:  ascend(m); return 0;
  case K_ESC:   return cancel(m);
  case K_TAB:
    if (m->mode == PM_SAVE) { field_set(m, m->name); m->ask = PM_ASK_NAME; }
    return 0;
  case CTRL('n'):
    field_set(m, "");
    m->ask = PM_ASK_MKDIR;
    return 0;
  case CTRL('r'):
    if (!m->n || m->ent[m->sel].synthetic) return 0;
    field_set(m, m->ent[m->sel].name);
    m->ask = PM_ASK_RENAME;
    return 0;
  case K_DEL: case CTRL('d'):
    if (!m->n || m->ent[m->sel].synthetic) return 0;
    field_set(m, m->ent[m->sel].name);
    m->ask = PM_ASK_DELETE;
    return 0;
  case CTRL('a'):
    m->show_hidden = !m->show_hidden;
    pm_refresh(m);
    return 0;
  default:
    if (k >= 0x20 && k < 0x7F) jump(m, k, now_ms);
    return 0;
  }
}

int pm_key(PickModel *m, uint8_t k, uint32_t now_ms) {
  if (m->done) return 1;
  if (m->note[0] && k != K_ENTER) { m->note[0] = 0; m->dirty = 1; }

  switch (m->ask) {
  case PM_ASK_NAME:
    if (k == K_ENTER) return try_save(m);
    if (k == K_ESC)   return cancel(m);
    if (k == K_TAB || k == K_UP || k == K_DOWN) {
      snprintf(m->name, sizeof m->name, "%s", m->field);
      m->ask = PM_ASK_NONE;
      m->dirty = 1;
      if (k != K_TAB) return list_key(m, k, now_ms);
      return 0;
    }
    field_key(m, k);
    return 0;

  case PM_ASK_MKDIR:
  case PM_ASK_RENAME:
    if (k == K_ENTER) {
      if (m->ask == PM_ASK_MKDIR) commit_mkdir(m); else commit_rename(m);
      return 0;
    }
    if (k == K_ESC) { m->ask = PM_ASK_NONE; m->dirty = 1; return 0; }
    field_key(m, k);
    return 0;

  case PM_ASK_DELETE:
    if (k == 'y' || k == 'Y' || k == K_ENTER) commit_delete(m);
    else if (k == 'n' || k == 'N' || k == K_ESC) { m->ask = PM_ASK_NONE; m->dirty = 1; }
    return 0;

  case PM_ASK_OVERWRITE:
    if (k == 'y' || k == 'Y' || k == K_ENTER) {
      char path[PM_PATH_MAX];
      pm_path_of(m, m->field, path, sizeof path);
      return finish(m, path);
    }
    if (k == 'n' || k == 'N' || k == K_ESC) {
      m->ask = m->mode == PM_SAVE ? PM_ASK_NAME : PM_ASK_NONE;
      m->dirty = 1;
    }
    return 0;

  default:
    return list_key(m, k, now_ms);
  }
}

int pm_click(PickModel *m, int row) {
  int idx = m->top + row;
  if (m->ask == PM_ASK_DELETE || m->ask == PM_ASK_OVERWRITE) return 0;
  if (idx < 0 || idx >= m->n) return 0;
  if (m->ask == PM_ASK_NAME) { snprintf(m->name, sizeof m->name, "%s", m->field); }
  m->ask = PM_ASK_NONE;
  if (idx == m->sel) return choose(m);
  m->sel = idx;
  keep_visible(m);
  return 0;
}

void pm_scroll(PickModel *m, int dy) {
  int rows = m->rows > 0 ? m->rows : DEFAULT_ROWS;
  int max_top = m->n > rows ? m->n - rows : 0;
  m->top += dy;
  if (m->top > max_top) m->top = max_top;
  if (m->top < 0) m->top = 0;
  m->dirty = 1;
}

const char *pm_prompt(const PickModel *m, const char **label) {
  static char line[PM_NAME_MAX + 24];
  switch (m->ask) {
  case PM_ASK_NAME:      *label = "Name";       return m->field;
  case PM_ASK_MKDIR:     *label = "New folder"; return m->field;
  case PM_ASK_RENAME:    *label = "Rename";     return m->field;
  case PM_ASK_DELETE:
    *label = "Delete?";
    snprintf(line, sizeof line, "%s  y/n", m->field);
    return line;
  case PM_ASK_OVERWRITE:
    *label = "Replace?";
    snprintf(line, sizeof line, "%s exists  y/n", m->field);
    return line;
  default:
    *label = "";
    if (m->note[0]) return m->note;
    if (m->truncated) return "showing the first 64";
    return "";
  }
}
