/* Vim's file tree: a netrw / nvim-tree view of the card, rooted at /.
 *
 * The owner: "an easy way to navigate code. View a file tree, open a file,
 * view it, then close back to the file tree at the same spot." Folders
 * open and close in place (Enter, l and h, o), j k gg G ctrl-d ctrl-u
 * move, / searches the names on screen, Enter on a file opens it in the
 * editor; :q in the editor comes back here as it was.
 *
 * What is kept, and what is only shown. The tree's state is small and
 * lives in the arena with the editor: which folders are open (their paths,
 * NUL-separated in `open`), the selected row's path, the scroll, whether
 * dotfiles show. The rows themselves -- every visible name -- are rebuilt
 * from the card whenever the tree is shown, into memory the app lends
 * (vt_attach): Vim lends its text buffer, which is empty whenever the tree
 * is up, so the tree costs the editor no room at all. Only open folders are
 * listed, each at most VT_DIRMAX entries (and a row saying the rest were
 * cut); rows that do not fit in the lent memory are left out and said.
 *
 * The state round-trips through a few lines of text (vt_state_write,
 * vt_state_read): the way back's CappUi.save, and /cache/vimtree, which
 * keeps the tree from one launch to the next. A blob that is truncated or
 * not ours is refused whole, and nothing is changed.
 *
 * Portable, like vimcore.h: the card is the CardApi in `vapi`, and
 * test/test_vim.c drives it a key at a time.
 */
#ifndef CARDOS_VIMTREE_H
#define CARDOS_VIMTREE_H

#include "kernel/app/capp.h"
#include "apps/vim/vimcore.h"       /* vapi, the key codes, hl_lang_for */

#define VT_PATH    96          /* == VI_PATH */
#define VT_OPENSZ  448         /* the open folders' paths, NUL-separated */
#define VT_DIRMAX  256         /* entries read from one folder at most */
#define VT_DEPTH   12          /* folders opened inside folders, at most */
#define VT_LINE    96          /* what is typed after / or : (a path, after rename) */
#define VT_MSG     64
#define VT_FRONT_MAX 0xF000    /* rows and names: offsets are 16-bit */

#define VTF_DIR  1
#define VTF_CUT  2             /* "...": the folder had more than was read */

/* What vt_key asks of the app. */
enum { VT_NONE = 0, VT_OPEN, VT_QUIT, VT_RUN };
enum { VTM_LIST = 0, VTM_SEARCH, VTM_CMD };

typedef struct {
  uint16_t name;               /* offset of its NUL-terminated name in mem */
  uint8_t  depth, flags;
} VtRow;

typedef struct {
  /* the lent memory: rows up from the front, names down from `front`,
   * and the listing scratch after it */
  char      *mem;
  int        front;
  CappEntry *ent;
  int        maxent;
  VtRow     *rows;
  int        nrows, nlo, built, full;

  /* the state that is kept */
  char  open[VT_OPENSZ];
  int   openlen;
  char  selpath[VT_PATH];
  int   sel, top, dots;

  /* the view */
  int   vis;                   /* rows on screen, set by the app */
  int   mode, count, pend;
  char  line[VT_LINE];
  int   linen, sel0;
  char  pat[VT_LINE];
  char  msg[VT_MSG];
  int   msg_err;
  char  out[VT_PATH];          /* the path for VT_OPEN and VT_RUN */
  char  tmp[VT_PATH];
} VTree;

/* ------------------------------------------------------------ helpers --- */

static int vt_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void vt_cpy(char *d, const char *s, int max) {
  int i = 0;
  if (max <= 0) return;
  while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
  d[i] = 0;
}
static int vt_eq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}
static int vt_low(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static void vt_say(VTree *T, int err, const char *m) {
  vt_cpy(T->msg, m, VT_MSG);
  T->msg_err = err;
}

/* `p` starts with folder `dir` and goes on inside it (or is it). */
static int vt_under(const char *p, const char *dir, int or_same) {
  int n = vt_len(dir);
  int i;
  if (n == 1 && dir[0] == '/') return or_same || p[1] != 0;
  for (i = 0; i < n; i++) if (p[i] != dir[i]) return 0;
  return p[n] == '/' || (or_same && p[n] == 0);
}

VI_FN const char *vt_name(VTree *T, int i) { return T->mem + T->rows[i].name; }
VI_FN int vt_is_dir(VTree *T, int i) { return (T->rows[i].flags & VTF_DIR) != 0; }

/* -------------------------------------------------------- open folders --- */

VI_FN int vt_is_open(VTree *T, const char *path) {
  int i = 0;
  if (path[0] == '/' && !path[1]) return 1;      /* the root always is */
  while (i < T->openlen) {
    if (vt_eq(T->open + i, path)) return 1;
    i += vt_len(T->open + i) + 1;
  }
  return 0;
}

/* Forget `path`, and what is open inside it. */
static void vt_forget(VTree *T, const char *path) {
  int i = 0;
  while (i < T->openlen) {
    int n = vt_len(T->open + i) + 1, k;
    if (vt_under(T->open + i, path, 1)) {
      for (k = i; k + n < T->openlen; k++) T->open[k] = T->open[k + n];
      T->openlen -= n;
    } else {
      i += n;
    }
  }
}

/* 0, or -1 when the set is full. */
VI_FN int vt_set_open(VTree *T, const char *path, int on) {
  int n;
  if (path[0] == '/' && !path[1]) return 0;
  if (!on) { vt_forget(T, path); return 0; }
  if (vt_is_open(T, path)) return 0;
  n = vt_len(path) + 1;
  if (T->openlen + n > VT_OPENSZ) return -1;
  vt_cpy(T->open + T->openlen, path, n);
  T->openlen += n;
  return 0;
}

/* --------------------------------------------------------------- rows --- */

/* The parent row of row i (i > 0). */
VI_FN int vt_parent(VTree *T, int i) {
  int d = T->rows[i].depth;
  while (i > 0 && T->rows[i].depth >= d) i--;
  return i;
}

/* The whole path of row i. */
VI_FN void vt_path(VTree *T, int i, char *out, int n) {
  int chain[VT_DEPTH + 2], k = 0, len = 0;
  while (i > 0 && k < VT_DEPTH + 2) { chain[k++] = i; i = vt_parent(T, i); }
  if (!k) { vt_cpy(out, "/", n); return; }
  while (k-- > 0) {
    const char *s = vt_name(T, chain[k]);
    if (len < n - 1) out[len++] = '/';
    while (*s && len < n - 1) out[len++] = *s++;
  }
  out[len] = 0;
}

/* Folders first, then A-Z ignoring case, then by case. */
static int vt_before(const CappEntry *a, const CappEntry *b) {
  const char *p = a->name, *q = b->name;
  int i;
  if (a->is_dir != b->is_dir) return a->is_dir;
  for (i = 0;; i++) {
    int x = vt_low((unsigned char)p[i]), y = vt_low((unsigned char)q[i]);
    if (x != y) return x < y;
    if (!x) break;
  }
  for (i = 0;; i++) {
    if (p[i] != q[i]) return (unsigned char)p[i] < (unsigned char)q[i];
    if (!p[i]) return 0;
  }
}

/* A name into the pool: its offset, or -1 when rows and names have met. */
static int vt_put_name(VTree *T, const char *s, int extra_rows) {
  int n = vt_len(s) + 1, at = T->nlo - n, i;
  if (at < (T->nrows + extra_rows) * (int)sizeof(VtRow)) return -1;
  for (i = 0; i < n; i++) T->mem[at + i] = s[i];
  T->nlo = at;
  return at;
}

/* Swap two entries a byte at a time: no 72-byte temporary on a stack that
 * vt_expand recurses on. */
static void vt_swap(CappEntry *a, CappEntry *b) {
  char *p = (char *)a, *q = (char *)b;
  int i;
  for (i = 0; i < (int)sizeof *a; i++) { char t = p[i]; p[i] = q[i]; q[i] = t; }
}

/* Read folder row i and put what is in it under it, then the same for
 * every folder in it that is open. 0, or -1 when the rows ran out. The
 * frame is a few ints: this recurses once a level. */
static int vt_expand(VTree *T, int i) {
  int n, k, kept = 0, got = 0, add, d = T->rows[i].depth + 1, cut, cutname = 0, rc = 0;
  CappEntry *e = T->ent;
  if (d > VT_DEPTH) return 0;
  vt_path(T, i, T->tmp, VT_PATH);
  n = vapi->list_ex(T->tmp, e, T->maxent);
  if (n < 0) {
    vapi->fmt(T->msg, VT_MSG, "cannot read %s", T->tmp);
    T->msg_err = 1;
    return 0;
  }
  cut = n >= T->maxent;
  /* the hidden go, the rest in order (insertion: a folder is small) */
  for (k = 0; k < n; k++) {
    if (!T->dots && e[k].name[0] == '.') continue;
    if (kept != k) vapi->mem_cpy(&e[kept], &e[k], sizeof e[0]);
    kept++;
  }
  for (k = 1; k < kept; k++) {
    int j;
    for (j = k; j > 0 && vt_before(&e[j], &e[j - 1]); j--) vt_swap(&e[j], &e[j - 1]);
  }
  /* Names into the pool first -- each leaving room for every row to come;
   * an entry's size is spent, so it keeps its name's offset -- then the
   * rows into place below row i. */
  add = kept + (cut ? 1 : 0);
  for (k = 0; k < kept; k++) {
    int o = vt_put_name(T, e[k].name, add);
    if (o < 0) break;
    e[k].size = (uint32_t)o;
    got++;
  }
  if (got < kept) { rc = -1; T->full = 1; cut = 0; }
  if (cut) {
    cutname = vt_put_name(T, "...", add);
    if (cutname < 0) cut = 0;
  }
  add = got + (cut ? 1 : 0);
  if (!add) return rc;
  vapi->mem_move(&T->rows[i + 1 + add], &T->rows[i + 1], (size_t)(T->nrows - i - 1) * sizeof(VtRow));
  for (k = 0; k < got; k++) {
    T->rows[i + 1 + k].name = (uint16_t)e[k].size;
    T->rows[i + 1 + k].depth = (uint8_t)d;
    T->rows[i + 1 + k].flags = (uint8_t)(e[k].is_dir ? VTF_DIR : 0);
  }
  if (cut) {
    T->rows[i + 1 + got].name = (uint16_t)cutname;
    T->rows[i + 1 + got].depth = (uint8_t)d;
    T->rows[i + 1 + got].flags = VTF_CUT;
  }
  T->nrows += add;
  /* the open ones, last first, so the rows above stay where they are */
  for (k = got - 1; k >= 0; k--) {
    int r = i + 1 + k;
    if (!vt_is_dir(T, r)) continue;
    vt_path(T, r, T->tmp, VT_PATH);
    if (vt_is_open(T, T->tmp) && vt_expand(T, r) < 0) rc = -1;
  }
  return rc;
}

/* The memory the rows go in: Vim's text buffer while the tree is up. */
VI_FN void vt_attach(VTree *T, char *mem, int size) {
  int pad = (int)((uintptr_t)mem & 3u), scratch;
  if (pad) { mem += 4 - pad; size -= 4 - pad; }
  size &= ~3;
  T->maxent = size / 2 / (int)sizeof(CappEntry);
  if (T->maxent > VT_DIRMAX) T->maxent = VT_DIRMAX;
  scratch = T->maxent * (int)sizeof(CappEntry);
  T->mem = mem;
  T->front = (size - scratch) & ~3;
  if (T->front > VT_FRONT_MAX) T->front = VT_FRONT_MAX;
  T->ent = (CappEntry *)(void *)(mem + size - scratch);
  T->rows = (VtRow *)(void *)mem;
  T->nrows = 0;
  T->built = 0;
}

/* The rows went (the memory was lent to the editor again): remember the
 * selection by its path, to find it after the next build. */
VI_FN void vt_remember(VTree *T) {
  if (T->built && T->sel >= 0 && T->sel < T->nrows) vt_path(T, T->sel, T->selpath, VT_PATH);
}

static void vt_show_sel(VTree *T) {
  int vis = T->vis > 0 ? T->vis : 1;
  if (T->sel >= T->nrows) T->sel = T->nrows - 1;
  if (T->sel < 0) T->sel = 0;
  if (T->top > T->sel) T->top = T->sel;
  if (T->sel >= T->top + vis) T->top = T->sel - vis + 1;
  if (T->top > T->nrows - vis) T->top = T->nrows - vis;
  if (T->top < 0) T->top = 0;
}

/* The row whose path is `p`, or -1. Rows are in order, so the search walks
 * down a level at a time. */
VI_FN int vt_find(VTree *T, const char *p) {
  int i;
  for (i = 0; i < T->nrows; i++) {
    if (T->rows[i].flags & VTF_CUT) continue;
    vt_path(T, i, T->tmp, VT_PATH);
    if (vt_eq(T->tmp, p)) return i;
  }
  return -1;
}

/* Read every open folder again, and find the selection: the row it was,
 * or the nearest folder above it that is still there. */
VI_FN void vt_build(VTree *T) {
  char want[VT_PATH];
  int s, i;
  vt_remember(T);
  vt_cpy(want, T->selpath, VT_PATH);
  T->nrows = 0;
  T->nlo = T->front;
  T->full = 0;
  T->built = 1;
  s = vt_put_name(T, "/", 1);
  if (s < 0) { vt_say(T, 1, "no room for the tree"); return; }
  T->rows[0].name = (uint16_t)s;
  T->rows[0].depth = 0;
  T->rows[0].flags = VTF_DIR;
  T->nrows = 1;
  vt_expand(T, 0);
  if (T->full) vt_say(T, 1, "too many rows: close a folder");
  s = -1;
  while (want[0] && s < 0) {
    s = vt_find(T, want);
    if (s >= 0) break;
    for (i = vt_len(want) - 1; i > 0 && want[i] != '/'; i--) {}
    if (i <= 0) break;
    want[i] = 0;
  }
  T->sel = s >= 0 ? s : 0;
  vt_show_sel(T);
  vt_path(T, T->sel, T->selpath, VT_PATH);
}

/* Open every folder on the way to `path` and select it: `-` from a file. */
VI_FN void vt_reveal(VTree *T, const char *path) {
  char p[VT_PATH];
  int i;
  vt_cpy(p, path, VT_PATH);
  for (i = 1; p[i]; i++) {
    if (p[i] != '/') continue;
    p[i] = 0;
    vt_set_open(T, p, 1);
    p[i] = '/';
  }
  vt_cpy(T->selpath, path, VT_PATH);
  T->built = 0;                                   /* nothing to remember */
}

VI_FN void vt_init(VTree *T) {
  T->openlen = 0;
  T->selpath[0] = 0;
  T->sel = T->top = T->dots = 0;
  T->vis = 13;
  T->mode = VTM_LIST;
  T->count = T->pend = 0;
  T->linen = 0;
  T->line[0] = T->pat[0] = T->msg[0] = 0;
  T->built = 0;
  T->nrows = 0;
}

/* --------------------------------------------------------- open, close --- */

/* Open folder row i (and say so), or close it. */
static void vt_toggle(VTree *T, int i, int open) {
  vt_path(T, i, T->tmp, VT_PATH);
  if (open) {
    if (vt_is_open(T, T->tmp)) return;
    if (T->rows[i].depth >= VT_DEPTH) { vt_say(T, 1, "too deep"); return; }
    if (vt_set_open(T, T->tmp, 1) < 0) { vt_say(T, 1, "too many folders open: close one"); return; }
    if (vt_expand(T, i) < 0) {
      /* out of rows: names of closed folders may be lying about; again,
       * from nothing (the open set already has this one) */
      vt_path(T, i, T->selpath, VT_PATH);
      T->built = 0;
      vt_build(T);
    }
  } else {
    int d = T->rows[i].depth, e = i + 1;
    if (i == 0) return;                           /* the root stays open */
    vt_set_open(T, T->tmp, 0);
    while (e < T->nrows && T->rows[e].depth > d) e++;
    if (e > i + 1) {
      vapi->mem_move(&T->rows[i + 1], &T->rows[e], (size_t)(T->nrows - e) * sizeof(VtRow));
      T->nrows -= e - i - 1;
      if (T->sel >= e) T->sel -= e - i - 1;
      else if (T->sel > i) T->sel = i;
    }
  }
  vt_show_sel(T);
}

VI_FN int vt_row_open(VTree *T, int i) {
  return vt_is_dir(T, i) && i + 1 < T->nrows && T->rows[i + 1].depth > T->rows[i].depth;
}

/* ------------------------------------------------------------- search --- */

static int vt_has(const char *s, const char *pat) {
  int i, j;
  if (!pat[0]) return 1;
  for (i = 0; s[i]; i++) {
    for (j = 0; pat[j] && s[i + j] && vt_low((unsigned char)s[i + j]) == vt_low((unsigned char)pat[j]); j++) {}
    if (!pat[j]) return 1;
  }
  return 0;
}

/* The next row whose name has `pat`, from `from` on (or back), wrapping. */
VI_FN int vt_search(VTree *T, const char *pat, int from, int back) {
  int k, i;
  if (!pat[0] || T->nrows <= 1) return -1;
  for (k = 0; k < T->nrows; k++) {
    i = back ? from - k : from + k;
    i = ((i % T->nrows) + T->nrows) % T->nrows;
    if (i == 0 || (T->rows[i].flags & VTF_CUT)) continue;
    if (vt_has(vt_name(T, i), pat)) return i;
  }
  return -1;
}

/* ---------------------------------------------------------------- keys --- */

static int vt_select_file(VTree *T, int i, int act) {
  vt_path(T, i, T->out, VT_PATH);
  vt_cpy(T->selpath, T->out, VT_PATH);
  if (act == VT_RUN && hl_lang_for(T->out) != LANG_PY) {
    vt_say(T, 1, "not a Python file: :run is for .py");
    return VT_NONE;
  }
  return act;
}

/* A name typed at the tree's command line, as a path: absolute as it is,
 * else in the folder selected, or the one the selection is in. Into out. */
static void vt_resolve(VTree *T, const char *p, char *out) {
  int i;
  if (*p == '/') { vt_cpy(out, p, VT_PATH); return; }
  i = vt_is_dir(T, T->sel) ? T->sel : vt_parent(T, T->sel);
  vt_path(T, i, T->tmp, VT_PATH);
  vapi->fmt(out, VT_PATH, "%s/%s", (T->tmp[0] == '/' && !T->tmp[1]) ? "" : T->tmp, p);
}

/* `word` then a space or the end: the command, and *arg what follows it. */
static int vt_cmd(const char *l, const char *word, const char **arg) {
  int n = vt_len(word), i;
  for (i = 0; i < n; i++) if (l[i] != word[i]) return 0;
  if (l[n] != ' ' && l[n]) return 0;
  l += n;
  while (*l == ' ') l++;
  *arg = l;
  return 1;
}

/* `name` beside row i (in the folder it is in), or as it is if absolute. */
static void vt_sibling(VTree *T, int i, const char *name, char *out) {
  if (*name == '/') { vt_cpy(out, name, VT_PATH); return; }
  vt_path(T, vt_parent(T, i), T->tmp, VT_PATH);
  vapi->fmt(out, VT_PATH, "%s/%s", (T->tmp[0] == '/' && !T->tmp[1]) ? "" : T->tmp, name);
}

/* Built again with `path` shown and selected, and a word about it. */
static void vt_show_made(VTree *T, const char *path, const char *what) {
  char m[VT_MSG];
  vt_reveal(T, path);
  vt_build(T);
  vapi->fmt(m, sizeof m, "%s %s", what, path);
  vt_say(T, 0, m);
}

/* The keys nvim-tree has, as commands the keys pre-fill (a, r, d): the line
 * shows what will happen, Enter does it and Esc does not -- the confirm. */
static int vt_ex_files(VTree *T, const char *l) {
  char m[VT_MSG], to[VT_PATH];
  const char *arg;
  CappStat st;
  int i = T->sel, n;
  if (vt_cmd(l, "add", &arg) || vt_cmd(l, "touch", &arg)) {
    int dir;
    if (!*arg) { vt_say(T, 1, "E471: Argument required: :add NAME (NAME/ for a folder)"); return 1; }
    vt_resolve(T, arg, T->out);
    n = vt_len(T->out);
    dir = T->out[n - 1] == '/';
    while (n > 1 && T->out[n - 1] == '/') T->out[--n] = 0;
    if (vapi->stat(T->out, &st) == 0) {
      vapi->fmt(m, sizeof m, "already there: %s", T->out);
      vt_say(T, 1, m);
      return 1;
    }
    if (dir) n = vapi->mkdir(T->out);
    else {
      int fd = vapi->open(T->out, CAPP_O_WRITE | CAPP_O_CREATE);
      if (fd >= 0) vapi->close(fd);
      n = fd >= 0 ? 0 : -1;
    }
    if (n != 0) {
      vapi->fmt(m, sizeof m, "could not make %s", T->out);
      vt_say(T, 1, m);
      return 1;
    }
    vt_show_made(T, T->out, dir ? "made folder" : "made");
    return 1;
  }
  if (vt_cmd(l, "rename", &arg) || vt_cmd(l, "mv", &arg)) {
    if (i <= 0 || (T->rows[i].flags & VTF_CUT)) { vt_say(T, 1, "nothing to rename here"); return 1; }
    if (!*arg) { vt_say(T, 1, "E471: Argument required: :rename NEWNAME"); return 1; }
    vt_path(T, i, T->out, VT_PATH);
    vt_sibling(T, i, arg, to);
    if (vt_eq(to, T->out)) return 1;
    if (vapi->stat(to, &st) == 0) {
      vapi->fmt(m, sizeof m, "already there: %s", to);
      vt_say(T, 1, m);
      return 1;
    }
    if (vapi->rename(T->out, to) != 0) {
      vapi->fmt(m, sizeof m, "could not rename %s", T->out);
      vt_say(T, 1, m);
      return 1;
    }
    if (vt_is_dir(T, i) && vt_is_open(T, T->out)) {   /* still open under its new name */
      vt_set_open(T, T->out, 0);
      vt_set_open(T, to, 1);
    }
    vt_show_made(T, to, "renamed to");
    return 1;
  }
  if (vt_cmd(l, "delete", &arg) || vt_cmd(l, "rm", &arg)) {
    if (i <= 0 || (T->rows[i].flags & VTF_CUT)) { vt_say(T, 1, "nothing to delete here"); return 1; }
    if (*arg) vt_sibling(T, i, arg, T->out);
    else vt_path(T, i, T->out, VT_PATH);
    if (vapi->remove(T->out) != 0) {
      vapi->fmt(m, sizeof m, vapi->stat(T->out, &st) == 0 && st.is_dir ?
                "not deleted: %s is not empty" : "could not delete %s", T->out);
      vt_say(T, 1, m);
      return 1;
    }
    vt_set_open(T, T->out, 0);
    vapi->fmt(m, sizeof m, "deleted %s", T->out);
    T->sel = i + 1 < T->nrows && T->rows[i + 1].depth == T->rows[i].depth ? i + 1 : i - 1;
    vt_path(T, T->sel, T->selpath, VT_PATH);
    T->built = 0;
    vt_build(T);
    vt_say(T, 0, m);
    return 1;
  }
  return 0;
}

/* The command line pre-filled by a key: `pre` then the selection's name. */
static void vt_prefill(VTree *T, const char *pre, int with_name) {
  T->mode = VTM_CMD;
  if (with_name) vapi->fmt(T->line, VT_LINE, "%s%s", pre, vt_name(T, T->sel));
  else vt_cpy(T->line, pre, VT_LINE);
  T->linen = vt_len(T->line);
}

static int vt_ex(VTree *T, const char *l) {
  char m[VT_MSG];
  const char *arg;
  while (*l == ' ' || *l == ':') l++;
  if (vt_ex_files(T, l)) return VT_NONE;
  /* :mkdir NAME -- a folder where :e would put a file, then shown selected */
  if (vt_cmd(l, "mkdir", &arg) || vt_cmd(l, "md", &arg)) {
    int n;
    if (!*arg) { vt_say(T, 1, "E471: Argument required: :mkdir NAME"); return VT_NONE; }
    vt_resolve(T, arg, T->out);
    n = vt_len(T->out);
    while (n > 1 && T->out[n - 1] == '/') T->out[--n] = 0;      /* "lib/" is "lib" */
    if (vapi->mkdir(T->out) != 0) {
      vapi->fmt(m, sizeof m, "could not make %s", T->out);
      vt_say(T, 1, m);
      return VT_NONE;
    }
    vt_reveal(T, T->out);
    vt_build(T);
    vapi->fmt(m, sizeof m, "made %s/", T->out);
    vt_say(T, 0, m);
    return VT_NONE;
  }
  if (vt_eq(l, "q") || vt_eq(l, "q!") || vt_eq(l, "qa") || vt_eq(l, "qa!") || vt_eq(l, "quit") ||
      vt_eq(l, "x") || vt_eq(l, "wq"))
    return VT_QUIT;
  if (vt_eq(l, "run") || vt_eq(l, "py") || vt_eq(l, "python")) {
    if (vt_is_dir(T, T->sel) || (T->rows[T->sel].flags & VTF_CUT)) { vt_say(T, 1, "not a Python file: :run is for .py"); return VT_NONE; }
    return vt_select_file(T, T->sel, VT_RUN);
  }
  if (vt_eq(l, "Ex") || vt_eq(l, "Explore") || vt_eq(l, "noh") || !l[0]) return VT_NONE;
  if (l[0] == 'e' && (l[1] == ' ' || !l[1])) {
    const char *p = l + 1;
    while (*p == ' ') p++;
    if (!*p) { vt_say(T, 1, "E32: No file name"); return VT_NONE; }
    vt_resolve(T, p, T->out);
    vt_cpy(T->selpath, T->out, VT_PATH);
    return VT_OPEN;
  }
  if (vt_eq(l, "set hidden") || vt_eq(l, "set nohidden")) {
    T->dots = l[4] == 'h';
    T->built = 0;
    vt_build(T);
    return VT_NONE;
  }
  if (l[0] >= '0' && l[0] <= '9') {
    int n = 0;
    while (*l >= '0' && *l <= '9') n = n * 10 + (*l++ - '0');
    T->sel = n > 0 ? n - 1 : 0;
    vt_show_sel(T);
    return VT_NONE;
  }
  vapi->fmt(m, sizeof m, "E492: Not an editor command: %s", l);
  vt_say(T, 1, m);
  return VT_NONE;
}

static int vt_line_key(VTree *T, int k) {
  if (k == VK_ESC || (k == VK_BS && T->linen == 0)) {
    if (T->mode == VTM_SEARCH) T->sel = T->sel0;
    T->mode = VTM_LIST;
    vt_show_sel(T);
    return VT_NONE;
  }
  if (k == VK_BS) T->line[--T->linen] = 0;
  else if (k == VK_ENTER) {
    int m = T->mode;
    T->mode = VTM_LIST;
    if (m == VTM_CMD) return vt_ex(T, T->line);
    if (T->linen) vt_cpy(T->pat, T->line, VT_LINE);
    {
      int f = vt_search(T, T->pat, T->sel0 + 1, 0);
      if (f >= 0) T->sel = f;
      else if (T->pat[0]) {
        T->sel = T->sel0;
        vapi->fmt(T->msg, VT_MSG, "E486: Pattern not found: %s", T->pat);
        T->msg_err = 1;
      }
    }
    vt_show_sel(T);
    vt_path(T, T->sel, T->selpath, VT_PATH);
    return VT_NONE;
  } else if (k >= 32 && k < 127 && T->linen < VT_LINE - 1) {
    T->line[T->linen++] = (char)k;
    T->line[T->linen] = 0;
  }
  if (T->mode == VTM_SEARCH) {                    /* as it is typed */
    int i = vt_search(T, T->line, T->sel0 + 1, 0);
    T->sel = i >= 0 ? i : T->sel0;
    vt_show_sel(T);
  }
  return VT_NONE;
}

/* A key. What the app should do next: VT_NONE, or VT_OPEN / VT_RUN with
 * the file in T->out, or VT_QUIT. */
VI_FN int vt_key(VTree *T, int k) {
  int cnt, half = T->vis / 2 > 0 ? T->vis / 2 : 1, i = T->sel, last = T->nrows - 1;
  if (!T->built || T->nrows < 1) return VT_NONE;
  if (T->mode != VTM_LIST) return vt_line_key(T, k);
  T->msg[0] = 0;
  if (T->pend) {
    int p = T->pend;
    T->pend = 0;
    cnt = T->count;
    T->count = 0;
    if (p == 'g' && k == 'g') T->sel = cnt ? cnt - 1 : 0;
    else if (p == 'g' && k == 'h') { T->dots = !T->dots; vt_build(T); vt_say(T, 0, T->dots ? "dotfiles shown" : "dotfiles hidden"); }
    else if (p == ' ' && k == 'r') {
      if (vt_is_dir(T, i) || (T->rows[i].flags & VTF_CUT)) { vt_say(T, 1, "not a Python file: :run is for .py"); return VT_NONE; }
      return vt_select_file(T, i, VT_RUN);
    }
    vt_show_sel(T);
    return VT_NONE;
  }
  if ((k >= '1' && k <= '9') || (k == '0' && T->count)) {
    T->count = T->count * 10 + (k - '0');
    if (T->count > 9999) T->count = 9999;
    return VT_NONE;
  }
  cnt = T->count ? T->count : 1;
  if (k == 'g' || k == ' ') { T->pend = k; return VT_NONE; }
  switch (k) {
  case 'j': case VK_DOWN: case VCTRL('n'): T->sel += cnt; break;
  case 'k': case VK_UP: case VCTRL('p'): T->sel -= cnt; break;
  case 'G': T->sel = T->count ? T->count - 1 : last; break;
  case VCTRL('d'): T->sel += half; T->top += half; break;
  case VCTRL('u'): T->sel -= half; T->top -= half; if (T->top < 0) T->top = 0; break;
  case VCTRL('f'): T->sel += T->vis; T->top += T->vis; break;
  case VCTRL('b'): T->sel -= T->vis; T->top -= T->vis; if (T->top < 0) T->top = 0; break;
  case 'H': T->sel = T->top; break;
  case 'L': T->sel = T->top + T->vis - 1; break;
  case 'M': T->sel = T->top + (T->vis - 1) / 2; break;
  case VK_ENTER: case 'o': case 'l': case VK_RIGHT:
    T->count = 0;
    if (T->rows[i].flags & VTF_CUT) { vt_say(T, 0, "the folder has more than can be listed"); return VT_NONE; }
    if (!vt_is_dir(T, i)) return vt_select_file(T, i, VT_OPEN);
    vt_path(T, i, T->tmp, VT_PATH);
    if (vt_row_open(T, i) || vt_is_open(T, T->tmp)) {
      if (k == 'l' || k == VK_RIGHT) { if (vt_row_open(T, i)) T->sel = i + 1; }
      else vt_toggle(T, i, 0);
    } else {
      vt_toggle(T, i, 1);
    }
    break;
  case 'h': case VK_LEFT: case '-':
    if (vt_is_dir(T, i) && vt_row_open(T, i) && i > 0 && k != '-') vt_toggle(T, i, 0);
    else if (i > 0) {
      int p = vt_parent(T, i);
      T->sel = p;
      if (k == '-' && p > 0) vt_toggle(T, p, 0);
    }
    break;
  case '.': T->dots = !T->dots; vt_build(T); vt_say(T, 0, T->dots ? "dotfiles shown" : "dotfiles hidden"); break;
  case 'R': vt_build(T); vt_say(T, 0, "read again"); break;
  /* nvim-tree's: each opens the command line filled in, Enter does it */
  case 'a': T->count = 0; vt_prefill(T, "add ", 0); return VT_NONE;
  case 'r':
    T->count = 0;
    if (i > 0) vt_prefill(T, "rename ", 1);
    return VT_NONE;
  case 'd':
    T->count = 0;
    if (i > 0) vt_prefill(T, "delete ", 1);
    return VT_NONE;
  case '/':
    T->mode = VTM_SEARCH;
    T->linen = 0; T->line[0] = 0;
    T->sel0 = T->sel;
    break;
  case ':':
    T->mode = VTM_CMD;
    T->linen = 0; T->line[0] = 0;
    break;
  case 'n': case 'N': {
    int r = 0, j = T->sel;
    for (; r < cnt; r++) {
      int f = vt_search(T, T->pat, k == 'n' ? j + 1 : j - 1, k == 'N');
      if (f < 0) break;
      j = f;
    }
    if (j == T->sel && T->pat[0] && r == 0) {
      char m[VT_MSG];
      vapi->fmt(m, sizeof m, "E486: Pattern not found: %s", T->pat);
      vt_say(T, 1, m);
    }
    T->sel = j;
    break;
  }
  case 'q': T->count = 0; return VT_QUIT;
  default: break;
  }
  T->count = 0;
  vt_show_sel(T);
  if (T->sel >= 0 && T->sel < T->nrows) vt_path(T, T->sel, T->selpath, VT_PATH);
  return VT_NONE;
}

/* --------------------------------------------------- the state as text ---
 *
 *   vim1
 *   m t            t the tree; b a file opened from it; e a file, :q leaves
 *   f /home/a.py   the file
 *   c 12 4 3       its line, column and top line
 *   s /home/a.py   the tree's selection
 *   t 0            the tree's top row
 *   d 0            dotfiles shown
 *   o /home        an open folder, a line each
 *   .              the end: a blob without it was cut short
 */

typedef struct {
  char mode;                   /* 't', 'b' or 'e' */
  char path[VT_PATH];
  int  line, col, top;
} VtHead;

static int vt_put(char *b, int n, int at, const char *s) {
  while (*s) {
    if (at >= n) return n + 1;
    b[at++] = *s++;
  }
  return at;
}

/* The state into buf: its length, or 0 when it does not fit. */
VI_FN int vt_state_write(VTree *T, const VtHead *h, char *buf, int n) {
  char l[VT_PATH + 32];
  int at = 0, i = 0;
  vt_remember(T);
  vapi->fmt(l, sizeof l, "vim1\nm %c\n", h->mode);
  at = vt_put(buf, n, at, l);
  if (h->path[0]) {
    vapi->fmt(l, sizeof l, "f %s\nc %d %d %d\n", h->path, h->line, h->col, h->top);
    at = vt_put(buf, n, at, l);
  }
  if (T->selpath[0]) {
    vapi->fmt(l, sizeof l, "s %s\n", T->selpath);
    at = vt_put(buf, n, at, l);
  }
  vapi->fmt(l, sizeof l, "t %d\nd %d\n", T->top, T->dots);
  at = vt_put(buf, n, at, l);
  while (i < T->openlen) {
    vapi->fmt(l, sizeof l, "o %s\n", T->open + i);
    at = vt_put(buf, n, at, l);
    i += vt_len(T->open + i) + 1;
  }
  at = vt_put(buf, n, at, ".\n");
  return at > n ? 0 : at;
}

static int vt_num(const char **pp, int *out) {
  const char *p = *pp;
  int v = 0, any = 0;
  while (*p == ' ') p++;
  while (*p >= '0' && *p <= '9' && v < 1000000) { v = v * 10 + (*p++ - '0'); any = 1; }
  *pp = p;
  *out = v;
  return any;
}

/* Read what vt_state_write wrote. 0, with h and the tree's kept state set;
 * or -1 for anything else -- short, garbled, too long, another version --
 * and then nothing has changed. Two passes: check it all, then take it. */
VI_FN int vt_state_read(VTree *T, VtHead *h, const char *buf, int n) {
  int pass, end = 0;
  for (pass = 0; pass < 2; pass++) {
    int at = 5, openlen = 0, seen_m = 0;
    if (n < 5 || buf[0] != 'v' || buf[1] != 'i' || buf[2] != 'm' || buf[3] != '1' || buf[4] != '\n') return -1;
    if (pass) {
      h->mode = 't';
      h->path[0] = 0;
      h->line = h->col = h->top = 0;
      T->openlen = 0;
      T->selpath[0] = 0;
      T->top = 0;
      T->dots = 0;
      T->built = 0;
    }
    end = 0;
    while (at < n && !end) {
      char v[VT_PATH];
      int s = at, len = 0, key;
      while (at < n && buf[at] != '\n') at++;
      if (at >= n) return -1;                     /* a line cut off */
      key = buf[s];
      if (key == '.') {
        if (at != s + 1 || at + 1 != n) return -1;
        end = 1;
        break;
      }
      if (at - s < 3 || buf[s + 1] != ' ' || at - s - 2 >= VT_PATH) return -1;
      for (len = 0; len < at - s - 2; len++) {
        char c = buf[s + 2 + len];
        if (c < 32 || c > 126) return -1;
        v[len] = c;
      }
      v[len] = 0;
      at++;
      switch (key) {
      case 'm':
        if (len != 1 || (v[0] != 't' && v[0] != 'b' && v[0] != 'e')) return -1;
        seen_m = 1;
        if (pass) h->mode = v[0];
        break;
      case 'f': case 's':
        if (v[0] != '/') return -1;
        if (pass) vt_cpy(key == 'f' ? h->path : T->selpath, v, VT_PATH);
        break;
      case 'c': {
        const char *p = v;
        int a, b, c;
        if (!vt_num(&p, &a) || !vt_num(&p, &b) || !vt_num(&p, &c) || *p) return -1;
        if (pass) { h->line = a; h->col = b; h->top = c; }
        break;
      }
      case 't': case 'd': {
        const char *p = v;
        int a;
        if (!vt_num(&p, &a) || *p) return -1;
        if (pass) { if (key == 't') T->top = a; else T->dots = a != 0; }
        break;
      }
      case 'o':
        if (v[0] != '/' || openlen + len + 1 > VT_OPENSZ) return -1;
        openlen += len + 1;
        if (pass) vt_set_open(T, v, 1);
        break;
      default:
        return -1;
      }
    }
    if (!end || !seen_m) return -1;
  }
  return 0;
}

#endif /* CARDOS_VIMTREE_H */
