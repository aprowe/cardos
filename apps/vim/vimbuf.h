/* Vim's text: a gap buffer of bytes, lines separated by '\n'.
 *
 * A gap buffer because editing is local -- a keystroke inserts or deletes
 * next to the last one -- and moving the gap costs only the distance from
 * the last edit. The memory is handed in (the arena's pool, or a big-mode
 * block: apps/vim/vim.c), so the same code runs over 14 KB or 60.
 *
 * Lines are not indexed. A line is found by scanning for '\n' from an
 * anchor -- the start of the last line asked for -- so the questions an
 * editor asks, which are always near the last one, cost a line or two of
 * scanning, and a jump to the end of the file costs one pass over it. The
 * anchor is kept true across inserts and deletes before it.
 *
 * Portable: no libc, no hardware. Host-tested in test/test_vim.c.
 */
#ifndef CARDOS_VIMBUF_H
#define CARDOS_VIMBUF_H

#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#define VI_FN static __attribute__((unused))
#else
#define VI_FN static
#endif

typedef struct {
  char *b;
  int   cap;
  int   gs, ge;          /* the gap is b[gs..ge) */
  int   nl;              /* how many '\n' */
  int   a_pos, a_line;   /* anchor: a_pos is the start of line a_line */
} VBuf;

VI_FN void vb_init(VBuf *v, char *mem, int cap) {
  v->b = mem;
  v->cap = cap;
  v->gs = 0;
  v->ge = cap;
  v->nl = 0;
  v->a_pos = 0;
  v->a_line = 0;
}

VI_FN int vb_len(const VBuf *v) { return v->cap - (v->ge - v->gs); }
VI_FN int vb_free(const VBuf *v) { return v->ge - v->gs; }
VI_FN int vb_lines(const VBuf *v) { return v->nl + 1; }

VI_FN int vb_at(const VBuf *v, int p) {
  if (p < 0 || p >= vb_len(v)) return '\n';    /* the edges read as line ends */
  return (unsigned char)(p < v->gs ? v->b[p] : v->b[p + (v->ge - v->gs)]);
}

/* Byte copies by hand: an app has no memcpy, and these are short. */
static void vb_copy_(char *d, const char *s, int n) {
  int i;
  if (d < s) for (i = 0; i < n; i++) d[i] = s[i];
  else for (i = n - 1; i >= 0; i--) d[i] = s[i];
}

VI_FN void vb_move_gap(VBuf *v, int p) {
  if (p < v->gs) {
    int n = v->gs - p;
    vb_copy_(v->b + v->ge - n, v->b + p, n);
    v->gs -= n;
    v->ge -= n;
  } else if (p > v->gs) {
    int n = p - v->gs;
    vb_copy_(v->b + v->gs, v->b + v->ge, n);
    v->gs += n;
    v->ge += n;
  }
}

/* Insert s[0..n) at p. 0, or -1 when there is no room (nothing changes). */
VI_FN int vb_insert(VBuf *v, int p, const char *s, int n) {
  int i, k = 0;
  if (n <= 0) return 0;
  if (n > vb_free(v)) return -1;
  if (p < 0) p = 0;
  if (p > vb_len(v)) p = vb_len(v);
  vb_move_gap(v, p);
  for (i = 0; i < n; i++) {
    v->b[v->gs + i] = s[i];
    if (s[i] == '\n') k++;
  }
  v->gs += n;
  v->nl += k;
  if (p < v->a_pos) { v->a_pos += n; v->a_line += k; }
  return 0;
}

/* Delete [p, p+n), clamped to the text. */
VI_FN void vb_delete(VBuf *v, int p, int n) {
  int i, k = 0;
  if (p < 0) { n += p; p = 0; }
  if (p + n > vb_len(v)) n = vb_len(v) - p;
  if (n <= 0) return;
  vb_move_gap(v, p);
  for (i = 0; i < n; i++) if (v->b[v->ge + i] == '\n') k++;
  v->ge += n;
  v->nl -= k;
  if (p + n < v->a_pos) { v->a_pos -= n; v->a_line -= k; }
  else if (p < v->a_pos) { v->a_pos = 0; v->a_line = 0; }
}

VI_FN void vb_get(const VBuf *v, int p, int n, char *out) {
  int i;
  for (i = 0; i < n; i++) out[i] = (char)vb_at(v, p + i);
}

/* The start of line L, clamped to the lines there are. */
VI_FN int vb_line_start(VBuf *v, int L) {
  int p = v->a_pos, line = v->a_line, len = vb_len(v);
  if (L < 0) L = 0;
  if (L > v->nl) L = v->nl;
  if (L - 0 < (line - L) && L < line) { p = 0; line = 0; }   /* nearer the top */
  while (line < L) {
    while (p < len && vb_at(v, p) != '\n') p++;
    if (p >= len) break;
    p++;
    line++;
  }
  while (line > L) {
    p--;                                       /* onto the previous line's '\n' */
    while (p > 0 && vb_at(v, p - 1) != '\n') p--;
    line--;
  }
  v->a_pos = p;
  v->a_line = line;
  return p;
}

/* Where line L ends: its '\n', or the end of the text. */
VI_FN int vb_line_end(VBuf *v, int L) {
  int p = vb_line_start(v, L), len = vb_len(v);
  while (p < len && vb_at(v, p) != '\n') p++;
  return p;
}

VI_FN int vb_line_len(VBuf *v, int L) { return vb_line_end(v, L) - vb_line_start(v, L); }

/* The line position p is on. */
VI_FN int vb_line_of(VBuf *v, int p) {
  int q, line, start;
  if (p < 0) p = 0;
  if (p > vb_len(v)) p = vb_len(v);
  if (p >= v->a_pos) {
    q = v->a_pos; line = v->a_line; start = q;
    for (; q < p; q++) if (vb_at(v, q) == '\n') { line++; start = q + 1; }
  } else {
    q = v->a_pos; line = v->a_line;
    if (p < v->a_pos - p) { q = 0; line = 0; }
    if (q <= p) {
      start = q;
      for (; q < p; q++) if (vb_at(v, q) == '\n') { line++; start = q + 1; }
    } else {
      for (q = q - 1; q >= p; q--) if (vb_at(v, q) == '\n') line--;
      start = p;
      while (start > 0 && vb_at(v, start - 1) != '\n') start--;
    }
  }
  v->a_pos = start;
  v->a_line = line;
  return line;
}

#endif /* CARDOS_VIMBUF_H */
