/* Vim's editing core: modes, motions, operators, undo, the dot, search, ex
 * commands and completion, over the gap buffer in vimbuf.h.
 *
 * Everything that is not pixels is here, so the host suite can drive it a
 * key at a time (apps/vim/test/test_vim.c) exactly as the app does. The app
 * (apps/vim/vim.c) hands keys to vi_key, paints from what vi_layout leaves,
 * and answers the requests in V->req (a picker to open or save, :q).
 *
 * The files go through the CardApi table in `vapi`, which the includer sets:
 * the app's own, or test/fakeapi.h's in-memory card.
 *
 * The memory is handed in by vi_init: the text, the undo log and the one
 * register each a block of their own, so the app can put them wherever it
 * found room. Every limit is checked and said, never overrun: a file too big
 * for the text is not opened (and so cannot be saved over half of itself), a
 * change too big for the undo log empties the log and says so, a yank too
 * big for the register is refused.
 *
 * What it is: vi's grammar -- [count] operator [count] motion -- with the
 * motions and commands listed in capp_info's help, one unnamed register, a
 * literal (not regex) search, and the ex commands :w :q :wq :x :e :N :set
 * :s :d. What it is not: named registers, marks, macros, regex, windows.
 */
#ifndef CARDOS_VIMCORE_H
#define CARDOS_VIMCORE_H

#include "kernel/app/capp.h"
#include "apps/safefile.h"
#include "apps/vim/vimbuf.h"
#include "apps/vim/vimhl.h"

static const CardApi *vapi;

#define VK_ESC   0x1B
#define VK_ENTER 0x0D
#define VK_BS    0x08
#define VK_TAB   0x09
#define VK_UP    0x80
#define VK_DOWN  0x81
#define VK_LEFT  0x82
#define VK_RIGHT 0x83
#define VCTRL(c) ((c) - 'a' + 1)

#define VI_ROWS_MAX 24
#define VI_LB       512        /* the longest line coloured and drawn */
#define VI_CAND     8
#define VI_CANDLEN  32
#define VI_CL       96
#define VI_REC      160
#define VI_PAT      64
#define VI_PATH     96
#define VI_MSG      72
#define VI_EOL      0x7FFF     /* the wanted column after $ */
#define HL_STEP     32
#define HL_CHK      512        /* checkpoints: 16384 lines */
#define VI_UHDR     14
#define VI_UTRL     4

enum { VM_NORMAL = 0, VM_INSERT, VM_VISUAL, VM_VLINE, VM_CMD };
enum { VREQ_NONE = 0, VREQ_OPEN, VREQ_SAVEAS, VREQ_QUIT, VREQ_TREE, VREQ_RUN };

#define MF_LINE 1
#define MF_INCL 2
#define MF_WANT 4

/* Keys that mean a motion but are not one byte: gg. */
#define VKM_GG 0x100

typedef struct {
  VBuf t;

  /* undo: records [kind start pos n cursor text... size], oldest first */
  char *u;
  int   ucap, ulen, upos, usaved;
  int   unew;                  /* the next record starts a step */

  /* the unnamed register */
  char *r;
  int   rcap, rlen, rline;

  int   cur, want;
  int   mode, vanchor, vprev;  /* vprev: the mode : left from */
  int   vis_l1, vis_l2;        /* the last visual lines, for '<,'> */

  /* the command being typed */
  int   count, count1, op, pend, cmd_vis;
  int   ft_cmd, ft_ch;

  /* the dot: keys of the command now, and of the last change */
  uint8_t rec[VI_REC];
  int   recn, rec_over, recording;
  uint8_t dot[VI_REC];
  int   dotn;
  int   ins_cnt, ins_rec0, ins_kind;

  /* search */
  char  pat[VI_PAT];
  int   pat_back, pat_word;

  /* the command line */
  char  cl[VI_CL];
  int   cln, clk;
  char  cl_last[VI_CL];

  char  msg[VI_MSG];
  int   msg_err;

  /* options */
  int   nu, sw, ts, et, ic;
  int   lang;

  /* the view: set rows and width by the app, the rest by vi_view */
  int   rows, width, top, left;
  uint8_t row_st[VI_ROWS_MAX + 1];

  /* syntax states at every HL_STEP lines; the first hlnv are right */
  uint8_t hlchk[HL_CHK];
  int   hlnv;

  /* what changed since the app last looked: lines, or "everything" */
  int   chg_lo, chg_hi, chg_all;
  unsigned gen;

  /* completion */
  char  cand[VI_CAND][VI_CANDLEN];
  char  cpre[VI_CANDLEN];
  int   ncand, csel, cpop, cs, clen;

  /* the file */
  char  path[VI_PATH];
  int   dos, noeol;
  int   req, quit_after;

  /* scratch, for a line being coloured or drawn */
  char    lb[VI_LB];
  uint8_t lc[VI_LB];
} Vim;

/* ------------------------------------------------------------ helpers --- */

static void vi_say(Vim *V, int err, const char *m) {
  int i;
  for (i = 0; m[i] && i < VI_MSG - 1; i++) V->msg[i] = m[i];
  V->msg[i] = 0;
  V->msg_err = err;
}

static int vi_len(Vim *V) { return vb_len(&V->t); }
static int vi_at(Vim *V, int p) { return vb_at(&V->t, p); }
static int vi_last(Vim *V) { return V->t.nl; }
static int vi_ls(Vim *V, int L) { return vb_line_start(&V->t, L); }
static int vi_le(Vim *V, int L) { return vb_line_end(&V->t, L); }
static int vi_lof(Vim *V, int p) { return vb_line_of(&V->t, p); }
VI_FN int vi_line(Vim *V) { return vi_lof(V, V->cur); }
VI_FN int vi_col(Vim *V) { return V->cur - vi_ls(V, vi_line(V)); }

static int vi_space(int c) { return c == ' ' || c == '\t'; }
static int vi_cls(int c, int big) {
  if (c == ' ' || c == '\t' || c == '\n') return 0;
  if (big) return 1;
  return hl_wordch(c) ? 2 : 1;
}

static int vi_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void vi_strcpy(char *d, const char *s, int max) {
  int i;
  for (i = 0; s[i] && i < max - 1; i++) d[i] = s[i];
  d[i] = 0;
}
static int vi_streq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static int vi_fnb(Vim *V, int L) {           /* first non-blank of L */
  int p = vi_ls(V, L), e = vi_le(V, L);
  while (p < e && vi_space(vi_at(V, p))) p++;
  return p;
}

static int vi_vcol(Vim *V, int pos) {
  int L = vi_lof(V, pos), p = vi_ls(V, L), vc = 0;
  for (; p < pos; p++) {
    if (vi_at(V, p) == '\t') vc += V->ts - vc % V->ts;
    else vc++;
  }
  return vc;
}

/* The position on line L under virtual column vc; the last character when
 * the line is shorter (its end, with `past`, as insert mode may be). */
static int vi_at_vcol(Vim *V, int L, int vc, int past) {
  int p = vi_ls(V, L), e = vi_le(V, L), c = 0;
  while (p < e) {
    int w = vi_at(V, p) == '\t' ? V->ts - c % V->ts : 1;
    if (c + w > vc) return p;
    c += w;
    p++;
  }
  if (!past && e > vi_ls(V, L)) return e - 1;
  return e;
}

/* In normal mode the cursor sits on a character, never on a line's end. */
static void vi_clamp(Vim *V) {
  int L, s, e;
  if (V->cur > vi_len(V)) V->cur = vi_len(V);
  if (V->cur < 0) V->cur = 0;
  if (V->mode == VM_INSERT) return;
  L = vi_lof(V, V->cur);
  s = vi_ls(V, L);
  e = vi_le(V, L);
  if (V->cur >= e && e > s) V->cur = e - 1;
}

static void vi_setwant(Vim *V) { V->want = vi_vcol(V, V->cur); }

VI_FN int vi_modified(Vim *V) { return V->upos != V->usaved; }

/* --------------------------------------------------------- the changes --- */

static void vi_touch(Vim *V, int p, const char *s, int n, int del) {
  int L = vi_lof(V, p), i, nl = 0;
  if (del) { for (i = 0; i < n; i++) if (vi_at(V, p + i) == '\n') nl++; }
  else { for (i = 0; i < n; i++) if (s[i] == '\n') nl++; }
  if (nl) V->chg_all = 1;
  if (L < V->chg_lo) V->chg_lo = L;
  if (L > V->chg_hi) V->chg_hi = L;
  if (V->hlnv > L / HL_STEP + 1) V->hlnv = L / HL_STEP + 1;
  V->gen++;
}

static int bx_ins(Vim *V, int p, const char *s, int n) {
  vi_touch(V, p, s, n, 0);
  return vb_insert(&V->t, p, s, n);
}

static void bx_del(Vim *V, int p, int n) {
  vi_touch(V, p, 0, n, 1);
  vb_delete(&V->t, p, n);
}

/* ---------------------------------------------------------------- undo --- */

static void u_put32(char *b, int v) {
  b[0] = (char)(v & 255); b[1] = (char)((v >> 8) & 255);
  b[2] = (char)((v >> 16) & 255); b[3] = (char)((v >> 24) & 255);
}
static int u_get32(const char *b) {
  return (int)((uint32_t)(uint8_t)b[0] | (uint32_t)(uint8_t)b[1] << 8 |
               (uint32_t)(uint8_t)b[2] << 16 | (uint32_t)(uint8_t)b[3] << 24);
}

static void u_clear(Vim *V) { V->ulen = V->upos = 0; V->unew = 1; }

/* Forget the oldest step to make room. 0 when there was nothing to drop. */
static int u_drop_oldest(Vim *V) {
  int off, i;
  if (V->ulen == 0) return 0;
  off = u_get32(V->u + 6) + VI_UHDR + VI_UTRL;
  while (off < V->ulen && !V->u[off + 1]) off += u_get32(V->u + off + 6) + VI_UHDR + VI_UTRL;
  if (off > V->upos) return 0;           /* never what is not undone yet */
  for (i = off; i < V->ulen; i++) V->u[i - off] = V->u[i];
  V->ulen -= off;
  V->upos -= off;
  V->usaved = V->usaved >= off ? V->usaved - off : -1;
  return 1;
}

static void u_add(Vim *V, int kind, int pos, int n) {
  int total = n + VI_UHDR + VI_UTRL, at;
  if (V->ulen > V->upos) {               /* a change ends the redo */
    V->ulen = V->upos;
    if (V->usaved > V->upos) V->usaved = -1;
  }
  /* Typing extends the record it is typing into. */
  if (kind == 'i' && !V->unew && V->upos > 0) {
    int last = V->upos - VI_UTRL - u_get32(V->u + V->upos - VI_UTRL);
    int lp = u_get32(V->u + last + 2), ln = u_get32(V->u + last + 6);
    if (V->u[last] == 'i' && lp + ln == pos && V->ulen + n <= V->ucap) {
      if (V->usaved == V->upos) V->usaved = -1;
      vb_get(&V->t, pos, n, V->u + last + VI_UHDR + ln);
      ln += n;
      u_put32(V->u + last + 6, ln);
      u_put32(V->u + last + VI_UHDR + ln, ln);
      V->ulen = V->upos = last + VI_UHDR + ln + VI_UTRL;
      return;
    }
  }
  if (total > V->ucap) {
    u_clear(V);
    V->usaved = -1;
    vi_say(V, 1, "change too big to undo");
    return;
  }
  while (V->ulen + total > V->ucap) {
    if (!u_drop_oldest(V)) {             /* the step being made is all there is */
      u_clear(V);
      V->usaved = -1;
      break;
    }
  }
  at = V->ulen;
  V->u[at] = (char)kind;
  V->u[at + 1] = (char)(V->unew ? 1 : 0);
  u_put32(V->u + at + 2, pos);
  u_put32(V->u + at + 6, n);
  u_put32(V->u + at + 10, V->cur);
  vb_get(&V->t, pos, n, V->u + at + VI_UHDR);
  u_put32(V->u + at + VI_UHDR + n, n);
  V->ulen = V->upos = at + total;
  V->unew = 0;
}

/* Recorded changes: everything a command does to the text goes through
 * these two, and so into the undo log. */
static int ed_ins(Vim *V, int p, const char *s, int n) {
  if (n <= 0) return 0;
  if (bx_ins(V, p, s, n) != 0) {
    vi_say(V, 1, "buffer full");
    return -1;
  }
  u_add(V, 'i', p, n);                   /* reads the text back from the buffer */
  return 0;
}

static void ed_del(Vim *V, int p, int n) {
  if (p < 0) { n += p; p = 0; }
  if (p + n > vi_len(V)) n = vi_len(V) - p;
  if (n <= 0) return;
  u_add(V, 'd', p, n);
  bx_del(V, p, n);
}

VI_FN void vi_undo(Vim *V, int cnt) {
  int done = 0, pos = -1;
  while (cnt-- > 0) {
    if (V->upos == 0) break;
    for (;;) {
      int size = u_get32(V->u + V->upos - VI_UTRL);
      int at = V->upos - VI_UTRL - size - VI_UHDR;
      int kind = V->u[at], start = V->u[at + 1];
      int p = u_get32(V->u + at + 2), n = u_get32(V->u + at + 6);
      if (kind == 'i') bx_del(V, p, n);
      else bx_ins(V, p, V->u + at + VI_UHDR, n);
      pos = u_get32(V->u + at + 10);   /* where the cursor was before */
      V->upos = at;
      if (start || V->upos == 0) break;
    }
    done++;
  }
  if (!done) { vi_say(V, 0, "Already at oldest change"); return; }
  V->cur = pos;
  V->chg_all = 1;
  vi_clamp(V);
  vi_setwant(V);
  vi_say(V, 0, done > 1 ? "changes undone" : "1 change undone");
}

VI_FN void vi_redo(Vim *V, int cnt) {
  int done = 0, pos = -1;
  while (cnt-- > 0) {
    if (V->upos >= V->ulen) break;
    do {
      int at = V->upos;
      int kind = V->u[at];
      int p = u_get32(V->u + at + 2), n = u_get32(V->u + at + 6);
      if (kind == 'i') { bx_ins(V, p, V->u + at + VI_UHDR, n); pos = p; }
      else { bx_del(V, p, n); pos = p; }
      V->upos = at + VI_UHDR + n + VI_UTRL;
    } while (V->upos < V->ulen && !V->u[V->upos + 1]);
    done++;
  }
  if (!done) { vi_say(V, 0, "Already at newest change"); return; }
  V->cur = pos;
  V->chg_all = 1;
  vi_clamp(V);
  vi_setwant(V);
  vi_say(V, 0, done > 1 ? "changes redone" : "1 change redone");
}

/* ------------------------------------------------------------ register --- */

static int reg_set(Vim *V, int p, int n, int line) {
  if (n + line > V->rcap) {
    vi_say(V, 1, "too big for the register: not kept");
    return -1;
  }
  vb_get(&V->t, p, n, V->r);
  if (line) V->r[n++] = '\n';
  V->rlen = n;
  V->rline = line;
  return 0;
}

/* --------------------------------------------------------- view state --- */

VI_FN int vi_gutter(Vim *V) {
  int d = 1, n = vi_last(V) + 1;
  if (!V->nu) return 0;
  while (n >= 10) { n /= 10; d++; }
  if (d < 3) d = 3;
  return d + 1;
}

VI_FN int vi_cols(Vim *V) {
  int c = V->width - vi_gutter(V);
  return c < 1 ? 1 : c;
}

VI_FN int vi_rows(Vim *V) {
  int r = V->rows;
  if (r > VI_ROWS_MAX) r = VI_ROWS_MAX;
  return r < 1 ? 1 : r;
}

/* Keep the cursor on screen: rows by scrolling, columns by jumping half a
 * screen as vim's 'sidescroll' 0 does. */
VI_FN void vi_view(Vim *V) {
  int rows = vi_rows(V), cols = vi_cols(V), L = vi_line(V), vc;
  if (L < V->top) V->top = L;
  if (L >= V->top + rows) V->top = L - rows + 1;
  if (V->top > vi_last(V)) V->top = vi_last(V);
  if (V->top < 0) V->top = 0;
  vc = vi_vcol(V, V->cur);
  if (vc < V->left || vc >= V->left + cols) {
    V->left = vc - cols / 2;
    if (vc < cols - 1) V->left = 0;
    if (V->left < 0) V->left = 0;
  }
}

/* --------------------------------------------------------------- syntax --- */

/* Line L into V->lb (as much as fits) and its colours into V->lc, from the
 * state it starts in; the state it ends in. */
static int vi_hl_line(Vim *V, int L, int st, int *n) {
  int s = vi_ls(V, L), e = vi_le(V, L), k = e - s;
  if (k > VI_LB) k = VI_LB;
  vb_get(&V->t, s, k, V->lb);
  *n = k;
  return hl_line(V->lang, st, V->lb, k, V->lc);
}

/* The state at the start of line L: the nearest good checkpoint, then the
 * lines between, filling checkpoints on the way. */
VI_FN int vi_hl_state(Vim *V, int L) {
  int i, line, st, n;
  if (V->lang == LANG_PLAIN) return HS_NONE;
  if (V->hlnv < 1) { V->hlnv = 1; V->hlchk[0] = HS_NONE; }
  i = L / HL_STEP;
  if (i >= V->hlnv) i = V->hlnv - 1;
  st = V->hlchk[i];
  line = i * HL_STEP;
  while (line < L) {
    st = vi_hl_line(V, line, st, &n);
    line++;
    if (line % HL_STEP == 0 && line / HL_STEP == V->hlnv && V->hlnv < HL_CHK)
      V->hlchk[V->hlnv++] = (uint8_t)st;
  }
  return st;
}

/* After a key: the view in place and the state at each row's start. */
VI_FN void vi_layout(Vim *V) {
  int r, rows = vi_rows(V), st, n;
  vi_view(V);
  st = vi_hl_state(V, V->top);
  for (r = 0; r <= rows; r++) {
    V->row_st[r] = (uint8_t)st;
    if (V->top + r <= vi_last(V) && V->lang != LANG_PLAIN)
      st = vi_hl_line(V, V->top + r, st, &n);
  }
}

/* Row r of the screen for painting: its line (or -1 past the end), its text
 * in V->lb and colours in V->lc, n characters. */
VI_FN int vi_row(Vim *V, int r, int *n) {
  int L = V->top + r;
  if (L > vi_last(V) || r > vi_rows(V)) { *n = 0; return -1; }
  vi_hl_line(V, L, V->row_st[r], n);
  return L;
}

/* ------------------------------------------------------------- motions --- */

static int vi_fwd_word(Vim *V, int p, int big) {
  int len = vi_len(V), c;
  if (p >= len) return len;
  c = vi_cls(vi_at(V, p), big);
  if (c) while (p < len && vi_cls(vi_at(V, p), big) == c) p++;
  while (p < len) {
    int ch = vi_at(V, p);
    if (ch == '\n') {
      p++;
      if (p < len && vi_at(V, p) == '\n') return p;     /* an empty line is a word */
      continue;
    }
    if (vi_space(ch)) { p++; continue; }
    break;
  }
  return p;
}

static int vi_bwd_word(Vim *V, int p, int big) {
  int c;
  if (p <= 0) return 0;
  p--;
  while (p > 0) {
    int ch = vi_at(V, p);
    if (ch == '\n' && vi_at(V, p - 1) == '\n') return p;
    if (vi_cls(ch, big) == 0) p--;
    else break;
  }
  if (p == 0) return 0;
  c = vi_cls(vi_at(V, p), big);
  while (p > 0 && vi_cls(vi_at(V, p - 1), big) == c) p--;
  return p;
}

static int vi_fwd_end(Vim *V, int p, int big) {
  int len = vi_len(V), c;
  if (p + 1 >= len) return p;
  p++;
  while (p < len && vi_cls(vi_at(V, p), big) == 0) p++;
  if (p >= len) return len - 1;
  c = vi_cls(vi_at(V, p), big);
  while (p + 1 < len && vi_cls(vi_at(V, p + 1), big) == c) p++;
  return p;
}

static int vi_match_pair(Vim *V, int p) {
  static const char pairs[] = "()[]{}";
  int L = vi_lof(V, p), e = vi_le(V, L), i = 0, depth = 0, open, close, dir, len = vi_len(V);
  for (; p < e; p++) {
    for (i = 0; i < 6; i++) if (vi_at(V, p) == pairs[i]) break;
    if (i < 6) break;
  }
  if (p >= e) return -1;
  open = pairs[i & ~1];
  close = pairs[i | 1];
  dir = (i & 1) ? -1 : 1;
  for (; p >= 0 && p < len; p += dir) {
    int c = vi_at(V, p);
    if (c == open) depth += dir;
    else if (c == close) depth -= dir;
    if (depth == 0) return p;
  }
  return -1;
}

/* Literal search for pat from `from` (exclusive), wrapping. -1 if absent. */
static int vi_match_at(Vim *V, int p, const char *pat, int m, int word) {
  int i;
  for (i = 0; i < m; i++) {
    int a = vi_at(V, p + i), b = (unsigned char)pat[i];
    if (p + i >= vi_len(V)) return 0;
    if (V->ic && !word) {
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
    }
    if (a != b) return 0;
  }
  if (word && ((p > 0 && hl_wordch(vi_at(V, p - 1))) || hl_wordch(vi_at(V, p + m)))) return 0;
  return 1;
}

static int vi_find(Vim *V, const char *pat, int back, int word, int from) {
  int len = vi_len(V), m = vi_strlen(pat), p, k;
  if (m == 0 || len == 0) return -1;
  p = from;
  for (k = 0; k <= len; k++) {
    p += back ? -1 : 1;
    if (p >= len) { p = 0; vi_say(V, 1, "search hit BOTTOM, continuing at TOP"); }
    if (p < 0) { p = len - 1; vi_say(V, 1, "search hit TOP, continuing at BOTTOM"); }
    if (vi_match_at(V, p, pat, m, word)) return p;
  }
  return -1;
}

static int vi_search_n(Vim *V, int back, int cnt) {
  int p = V->cur, q;
  char m[VI_MSG];
  if (!V->pat[0]) { vi_say(V, 1, "E35: No previous regular expression"); return -1; }
  V->msg[0] = 0;
  while (cnt-- > 0) {
    q = vi_find(V, V->pat, back, V->pat_word, p);
    if (q < 0) {
      vapi->fmt(m, sizeof m, "E486: Pattern not found: %s", V->pat);
      vi_say(V, 1, m);
      return -1;
    }
    p = q;
  }
  if (!V->msg[0]) {
    vapi->fmt(m, sizeof m, "%c%s", back ? '?' : '/', V->pat);
    vi_say(V, 0, m);
  }
  return p;
}

/* The word under or after the cursor, for *. */
static int vi_star(Vim *V, int back, int cnt) {
  int p = V->cur, L = vi_line(V), e = vi_le(V, L), s, n = 0;
  while (p < e && !hl_wordch(vi_at(V, p))) p++;
  if (p >= e) { vi_say(V, 1, "E348: No string under cursor"); return -1; }
  s = p;
  while (s > vi_ls(V, L) && hl_wordch(vi_at(V, s - 1))) s--;
  while (s + n < e && hl_wordch(vi_at(V, s + n)) && n < VI_PAT - 1) n++;
  vb_get(&V->t, s, n, V->pat);
  V->pat[n] = 0;
  V->pat_word = 1;
  V->pat_back = back;
  V->cur = s;
  p = vi_search_n(V, back, cnt);
  return p;
}

/* A motion from the cursor. 1 with the target in *to and MF_* in *fl, 0
 * when it cannot move (vim beeps), -1 when k is no motion at all. */
static int vi_motion(Vim *V, int k, int ch, int cnt, int has, int *to, int *fl) {
  int p = V->cur, L = vi_lof(V, p), s = vi_ls(V, L), e = vi_le(V, L), i, q;
  int last = vi_last(V), opp = V->op != 0;
  *fl = 0;
  switch (k) {
  case 'h': case VK_LEFT: case VK_BS:
    if (p <= s) return 0;
    p -= cnt;
    if (p < s) p = s;
    break;
  case 'l': case VK_RIGHT: case ' ':
    q = opp ? e : (e > s ? e - 1 : s);
    if (p >= q) return 0;
    p += cnt;
    if (p > q) p = q;
    break;
  case 'j': case VK_DOWN: case VCTRL('n'): case VCTRL('j'):
  case 'k': case VK_UP: case VCTRL('p'):
    if (k == 'k' || k == VK_UP || k == VCTRL('p')) {
      if (L == 0) return 0;
      L -= cnt;
      if (L < 0) L = 0;
    } else {
      if (L == last) return 0;
      L += cnt;
      if (L > last) L = last;
    }
    p = vi_at_vcol(V, L, V->want, 0);
    *fl = MF_LINE | MF_WANT;
    break;
  case VK_ENTER: case '+': case '-': case '_':
    if (k == '-') { if (L == 0) return 0; L -= cnt; if (L < 0) L = 0; }
    else if (k == '_') { L += cnt - 1; if (L > last) L = last; }
    else { if (L == last) return 0; L += cnt; if (L > last) L = last; }
    p = vi_fnb(V, L);
    *fl = MF_LINE;
    break;
  case '0': p = s; break;
  case '^': p = vi_fnb(V, L); break;
  case '|':
    p = vi_at_vcol(V, L, cnt - 1, 0);
    break;
  case '$':
    L += cnt - 1;
    if (L > last) L = last;
    e = vi_le(V, L);
    p = e > vi_ls(V, L) ? e - 1 : e;
    *fl = MF_INCL;
    break;
  case 'w': case 'W':
    for (i = 0; i < cnt; i++) {
      q = vi_fwd_word(V, p, k == 'W');
      if (q == p) { if (i == 0 && !opp) return 0; break; }
      p = q;
    }
    /* With an operator, a last word at the end of a line ends there:
     * dw on the last word of a line does not take the newline. */
    if (opp && vi_lof(V, p) > L && p <= vi_fnb(V, vi_lof(V, p)))
      p = vi_le(V, vi_lof(V, p) - 1);
    if (!opp && p >= vi_len(V)) p = vi_len(V) - 1;
    break;
  case 'b': case 'B':
    if (p == 0) return 0;
    for (i = 0; i < cnt; i++) p = vi_bwd_word(V, p, k == 'B');
    break;
  case 'e': case 'E':
    for (i = 0; i < cnt; i++) p = vi_fwd_end(V, p, k == 'E');
    if (p == V->cur) return 0;
    *fl = MF_INCL;
    break;
  case 'G': case VKM_GG:
    L = has ? cnt - 1 : (k == 'G' ? last : 0);
    if (L > last) L = last;
    p = vi_fnb(V, L);
    *fl = MF_LINE;
    break;
  case 'f': case 'F': case 't': case 'T': case ';': case ',': {
    int c = k, back, till, sk = 0;
    if (k == ';' || k == ',') {
      if (!V->ft_cmd) return 0;
      c = V->ft_cmd;
      ch = V->ft_ch;
      if (k == ',') c = (c == 'f') ? 'F' : (c == 'F') ? 'f' : (c == 't') ? 'T' : 't';
      sk = (c == 't' || c == 'T');       /* ; after t does not stick */
    } else {
      V->ft_cmd = k;
      V->ft_ch = ch;
    }
    back = (c == 'F' || c == 'T');
    till = (c == 't' || c == 'T');
    q = p;
    for (i = 0; i < cnt; i++) {
      int r = q + (back ? -1 : 1);
      if (till && sk && i == 0) r += back ? -1 : 1;
      while (r >= s && r < e && vi_at(V, r) != ch) r += back ? -1 : 1;
      if (r < s || r >= e) return 0;
      q = r;
    }
    p = till ? q + (back ? 1 : -1) : q;
    if (!back) *fl = MF_INCL;
    break;
  }
  case '%':
    if (has) {
      L = (cnt * (last + 1) + 99) / 100 - 1;
      if (L < 0) L = 0;
      if (L > last) L = last;
      p = vi_fnb(V, L);
      *fl = MF_LINE;
      break;
    }
    q = vi_match_pair(V, p);
    if (q < 0) return 0;
    p = q;
    *fl = MF_INCL;
    break;
  case 'H': case 'M': case 'L': {
    int vis = vi_last(V) - V->top + 1;
    if (vis > vi_rows(V)) vis = vi_rows(V);
    if (k == 'H') L = V->top + (cnt - 1 < vis ? cnt - 1 : vis - 1);
    else if (k == 'L') L = V->top + vis - 1 - (cnt - 1 < vis ? cnt - 1 : vis - 1);
    else L = V->top + (vis - 1) / 2;
    p = vi_fnb(V, L);
    *fl = MF_LINE;
    break;
  }
  case '}': case '{':
    for (i = 0; i < cnt; i++) {
      if (k == '}') {
        while (L < last && vi_le(V, L) == vi_ls(V, L)) L++;
        while (L < last && vi_le(V, L) != vi_ls(V, L)) L++;
      } else {
        while (L > 0 && vi_le(V, L) == vi_ls(V, L)) L--;
        while (L > 0 && vi_le(V, L) != vi_ls(V, L)) L--;
      }
    }
    p = vi_ls(V, L);
    if (k == '}' && L == last && vi_le(V, L) > p) {
      p = vi_le(V, L);
      if (!opp) p--;
      else *fl = 0;
    }
    break;
  case 'n': case 'N':
    q = vi_search_n(V, k == 'N' ? !V->pat_back : V->pat_back, cnt);
    if (q < 0) return 0;
    p = q;
    break;
  case '*': case '#':
    q = vi_star(V, k == '#', cnt);
    if (q < 0) return 0;
    p = q;
    break;
  default:
    return -1;
  }
  *to = p;
  return 1;
}

/* ----------------------------------------------------------- operators --- */

static void vi_insert_mode(Vim *V, int cnt, int kind);

static int vi_indent_of(Vim *V, int L, char *out, int max) {
  int p = vi_ls(V, L), e = vi_le(V, L), n = 0;
  while (p < e && vi_space(vi_at(V, p)) && n < max) out[n++] = (char)vi_at(V, p++);
  return n;
}

static void vi_shift_line(Vim *V, int L, int right) {
  int s = vi_ls(V, L), e = vi_le(V, L), i, n = 0, w = 0;
  if (right) {
    char sp[16];
    if (s == e) return;                  /* vim leaves empty lines alone */
    if (!V->et) { ed_ins(V, s, "\t", 1); return; }
    for (i = 0; i < V->sw && i < 16; i++) sp[i] = ' ';
    ed_ins(V, s, sp, i);
    return;
  }
  while (s + n < e && w < V->sw) {
    int c = vi_at(V, s + n);
    if (c == ' ') w++;
    else if (c == '\t') w += V->ts;
    else break;
    n++;
  }
  ed_del(V, s, n);
}

/* An operator over whole lines L1..L2. */
static void vi_op_lines(Vim *V, int op, int L1, int L2) {
  int s = vi_ls(V, L1), e = vi_le(V, L2), last = vi_last(V), L, col;
  char ind[48];
  int ni;
  if (op == '>' || op == '<') {
    for (L = L1; L <= L2; L++) vi_shift_line(V, L, op == '>');
    V->cur = vi_fnb(V, L1);
    return;
  }
  if (op == '~' || op == 'u' || op == 'U') return;
  if (reg_set(V, s, e - s, 1) != 0 && op == 'y') return;
  if (op == 'y') {
    col = vi_vcol(V, V->cur);
    if (vi_line(V) != L1) V->cur = vi_at_vcol(V, L1, col, 0);
    if (L2 > L1) {
      char m[40];
      vapi->fmt(m, sizeof m, "%d lines yanked", L2 - L1 + 1);
      vi_say(V, 0, m);
    }
    return;
  }
  if (op == 'c') {
    ni = vi_indent_of(V, L1, ind, sizeof ind);
    ed_del(V, s, e - s);
    ed_ins(V, s, ind, ni);
    V->cur = s + ni;
    vi_insert_mode(V, 1, 'c');
    return;
  }
  /* d */
  if (L2 < last) ed_del(V, s, e + 1 - s);
  else if (L1 > 0) ed_del(V, s - 1, e - s + 1);
  else ed_del(V, 0, vi_len(V));
  L = L1 > vi_last(V) ? vi_last(V) : L1;
  V->cur = vi_fnb(V, L);
  if (L2 - L1 >= 2 && !V->msg[0]) {
    char m[40];
    vapi->fmt(m, sizeof m, "%d fewer lines", L2 - L1 + 1);
    vi_say(V, 0, m);
  }
}

static void vi_case(Vim *V, int s, int e, int how) {
  int p;
  for (p = s; p < e; p++) {
    int c = vi_at(V, p), d = c;
    if (c >= 'a' && c <= 'z' && how != 'u') d = c - 32;
    else if (c >= 'A' && c <= 'Z' && how != 'U') d = c + 32;
    if (d != c) {
      char x = (char)d;
      ed_del(V, p, 1);
      ed_ins(V, p, &x, 1);
    }
  }
}

/* An operator over the characters [s, e). */
static void vi_op_chars(Vim *V, int op, int s, int e) {
  if (e < s) { int t = s; s = e; e = t; }
  if (op == '>' || op == '<') {
    vi_op_lines(V, op, vi_lof(V, s), vi_lof(V, e > s ? e - 1 : e));
    return;
  }
  if (op == '~' || op == 'u' || op == 'U') {
    vi_case(V, s, e, op);
    V->cur = s;
    return;
  }
  if (reg_set(V, s, e - s, 0) != 0 && op == 'y') return;
  if (op == 'y') { V->cur = s; return; }
  ed_del(V, s, e - s);
  V->cur = s;
  if (op == 'c') vi_insert_mode(V, 1, 'c');
}

/* Operator op from the cursor to a motion's target, by vim's rules. */
static void vi_apply(Vim *V, int op, int to, int fl) {
  int s = V->cur < to ? V->cur : to, e = V->cur < to ? to : V->cur;
  if (fl & MF_LINE) { vi_op_lines(V, op, vi_lof(V, s), vi_lof(V, e)); return; }
  if (fl & MF_INCL) {
    if (e < vi_len(V)) e++;
  } else if (e > s && vi_lof(V, e) > vi_lof(V, s) && e == vi_ls(V, vi_lof(V, e))) {
    /* An exclusive motion that ends at a line's start ends at the line
     * before instead -- linewise if it began at or before its first
     * non-blank (:help exclusive-linewise). */
    int Ls = vi_lof(V, s), Le = vi_lof(V, e);
    if (s <= vi_fnb(V, Ls)) { vi_op_lines(V, op, Ls, Le - 1); return; }
    e = vi_le(V, Le - 1);
  }
  vi_op_chars(V, op, s, e);
}

/* ------------------------------------------------------- simple edits --- */

static void vi_put(Vim *V, int after, int cnt) {
  int i, L = vi_line(V), p, n = V->rlen, first;
  if (!V->rlen) { vi_say(V, 1, "E353: Nothing in register"); return; }
  if (V->rline) {
    if (after) {
      p = vi_le(V, L);
      if (L == vi_last(V)) {             /* no newline after the last line */
        for (i = 0; i < cnt; i++) {
          if (ed_ins(V, p, "\n", 1) != 0) return;
          if (ed_ins(V, p + 1, V->r, n - 1) != 0) return;
          p += n;
        }
        V->cur = vi_fnb(V, L + 1);
        return;
      }
      p++;
      first = L + 1;
    } else {
      p = vi_ls(V, L);
      first = L;
    }
    for (i = 0; i < cnt; i++) if (ed_ins(V, p + i * n, V->r, n) != 0) break;
    V->cur = vi_fnb(V, first);
    return;
  }
  p = V->cur;
  if (after && vi_le(V, L) > vi_ls(V, L)) p++;
  for (i = 0; i < cnt; i++) if (ed_ins(V, p + i * n, V->r, n) != 0) return;
  for (i = 0; i < n && V->r[i] != '\n'; i++) { }
  V->cur = i < n ? p : p + cnt * n - 1;
}

static void vi_join(Vim *V, int cnt) {
  int i, L = vi_line(V);
  if (cnt < 2) cnt = 2;
  for (i = 1; i < cnt; i++) {
    int e, w, len = vi_len(V);
    if (L >= vi_last(V)) { if (i == 1) vi_say(V, 1, "nothing to join"); break; }
    e = vi_le(V, L);
    w = e + 1;
    while (w < len && vi_space(vi_at(V, w))) w++;
    ed_del(V, e, w - e);
    if (e > vi_ls(V, L) && !vi_space(vi_at(V, e - 1)) && e < vi_len(V) &&
        vi_at(V, e) != '\n' && vi_at(V, e) != ')')
      ed_ins(V, e, " ", 1);
    V->cur = e;
  }
}

static void vi_replace(Vim *V, int ch, int cnt) {
  int L = vi_line(V), e = vi_le(V, L), i;
  if (V->cur + cnt > e) return;
  if (ch == VK_ENTER) {
    ed_del(V, V->cur, cnt);
    ed_ins(V, V->cur, "\n", 1);
    V->cur++;
    return;
  }
  for (i = 0; i < cnt; i++) {
    char c = (char)ch;
    ed_del(V, V->cur + i, 1);
    ed_ins(V, V->cur + i, &c, 1);
  }
  V->cur += cnt - 1;
}

static void vi_open_line(Vim *V, int below) {
  int L = vi_line(V), n, p;
  char buf[49];
  n = vi_indent_of(V, L, buf + 1, 47);
  if (below) {
    p = vi_le(V, L);
    buf[0] = '\n';
    ed_ins(V, p, buf, n + 1);
    V->cur = p + 1 + n;
  } else {
    p = vi_ls(V, L);
    buf[n + 1] = '\n';
    ed_ins(V, p, buf + 1, n + 1);
    V->cur = p + n;
  }
}

/* --------------------------------------------------------- completion --- */

static int cmp_has(Vim *V, const char *w, int n) {
  int i, j;
  for (i = 0; i < V->ncand; i++) {
    for (j = 0; j < n && V->cand[i][j] == w[j]; j++) { }
    if (j == n && V->cand[i][n] == 0) return 1;
  }
  return 0;
}

static void cmp_add(Vim *V, const char *w, int n) {
  int i;
  if (V->ncand >= VI_CAND || n >= VI_CANDLEN || cmp_has(V, w, n)) return;
  for (i = 0; i < n; i++) V->cand[V->ncand][i] = w[i];
  V->cand[V->ncand][n] = 0;
  V->ncand++;
}

/* The word at q, if it starts with the prefix and is longer. */
static void cmp_try(Vim *V, int q, int plen) {
  char w[VI_CANDLEN];
  int n = 0;
  if (q == V->cs) return;
  while (n < VI_CANDLEN && hl_wordch(vi_at(V, q + n)) && q + n < vi_len(V)) {
    w[n] = (char)vi_at(V, q + n);
    n++;
  }
  if (n >= VI_CANDLEN || n <= plen) return;
  if (n > 0) {
    int i;
    for (i = 0; i < plen; i++) if (w[i] != V->cpre[i]) return;
    cmp_add(V, w, n);
  }
}

static void cmp_apply(Vim *V, int i) {
  const char *w = i < 0 ? V->cpre : V->cand[i];
  int n = vi_strlen(w);
  ed_del(V, V->cs, V->clen);
  ed_ins(V, V->cs, w, n);
  V->clen = n;
  V->cur = V->cs + n;
}

/* ctrl-n (dir 1), ctrl-p (-1), or tab: complete the word before the
 * cursor from the buffer -- nearest first, looking back and then on --
 * and the language's keywords. 0 when there is no word before it. */
static int cmp_start(Vim *V, int dir) {
  int p = V->cur, L = vi_line(V), s = p, q, len = vi_len(V), plen, w;
  const char *const *kw;
  while (s > vi_ls(V, L) && hl_wordch(vi_at(V, s - 1))) s--;
  plen = p - s;
  if (plen == 0 || plen >= VI_CANDLEN) return 0;
  vb_get(&V->t, s, plen, V->cpre);
  V->cpre[plen] = 0;
  V->cs = s;
  V->clen = plen;
  V->ncand = 0;
  for (q = s - 1; q >= 0 && V->ncand < VI_CAND; q--)
    if (hl_wordch(vi_at(V, q)) && (q == 0 || !hl_wordch(vi_at(V, q - 1)))) cmp_try(V, q, plen);
  for (q = p; q < len && V->ncand < VI_CAND; q++)
    if (hl_wordch(vi_at(V, q)) && (q == 0 || !hl_wordch(vi_at(V, q - 1)))) cmp_try(V, q, plen);
  for (w = 0; w < 2; w++) {
    int i, j;
    kw = hl_words(V->lang, w);
    for (i = 0; kw && kw[i]; i++) {
      for (j = 0; j < plen && kw[i][j] == V->cpre[j]; j++) { }
      if (j == plen && kw[i][plen]) cmp_add(V, kw[i], vi_strlen(kw[i]));
    }
  }
  if (V->ncand == 0) { vi_say(V, 1, "-- no match"); return 1; }
  V->csel = dir > 0 ? 0 : V->ncand - 1;
  cmp_apply(V, V->csel);
  V->cpop = V->ncand > 1;
  if (V->cpop) {
    char m[40];
    vapi->fmt(m, sizeof m, "-- match %d of %d", V->csel + 1, V->ncand);
    vi_say(V, 0, m);
  } else {
    V->msg[0] = 0;
  }
  return 1;
}

/* A key while the list is up. 1 when it was the list's. */
static int cmp_key(Vim *V, int k) {
  char m[40];
  if (k == VCTRL('n') || k == VK_TAB || k == VK_DOWN || k == VCTRL('p') || k == VK_UP) {
    int d = (k == VCTRL('p') || k == VK_UP) ? -1 : 1;
    V->csel = (V->csel + d + V->ncand) % V->ncand;
    cmp_apply(V, V->csel);
    vapi->fmt(m, sizeof m, "-- match %d of %d", V->csel + 1, V->ncand);
    vi_say(V, 0, m);
    return 1;
  }
  V->cpop = 0;
  V->chg_all = 1;
  V->msg[0] = 0;
  if (k == VK_ENTER || k == VCTRL('y')) return 1;
  if (k == VCTRL('e')) { cmp_apply(V, -1); return 1; }
  return 0;
}

/* --------------------------------------------------------- insert mode --- */

static void rec_put(Vim *V, int k) {
  if (!V->recording) return;
  if (V->recn < VI_REC) V->rec[V->recn++] = (uint8_t)k;
  else V->rec_over = 1;
}

static void vi_insert_mode(Vim *V, int cnt, int kind) {
  V->mode = VM_INSERT;
  V->ins_cnt = cnt < 1 ? 1 : cnt;
  V->ins_rec0 = V->recn;
  V->ins_kind = kind;
  V->cpop = 0;
}

static void ins_newline(Vim *V) {
  char buf[64];
  int L = vi_line(V), n, q = V->cur, i;
  buf[0] = '\n';
  n = vi_indent_of(V, L, buf + 1, 40);
  if (n > V->cur - vi_ls(V, L)) n = V->cur - vi_ls(V, L);
  while (q > vi_ls(V, L) && vi_space(vi_at(V, q - 1))) q--;
  if (q > vi_ls(V, L) && ((V->lang == LANG_C && vi_at(V, q - 1) == '{') ||
                          (V->lang == LANG_PY && vi_at(V, q - 1) == ':'))) {
    if (V->et) for (i = 0; i < V->sw && n < 56; i++) buf[1 + n++] = ' ';
    else buf[1 + n++] = '\t';
  }
  if (ed_ins(V, V->cur, buf, n + 1) == 0) V->cur += n + 1;
}

static void ins_char(Vim *V, int k) {
  char c = (char)k;
  int L = vi_line(V), s = vi_ls(V, L);
  /* A closing brace typed first on a line steps its indent back. */
  if (k == '}' && V->lang == LANG_C && V->cur > s && vi_fnb(V, L) == V->cur &&
      vi_at(V, V->cur) != '}') {
    int n = 0, w = 0;
    while (s + n < V->cur && w < V->sw) {
      w += vi_at(V, s + n + 0) == '\t' ? V->ts : 1;
      n++;
    }
    if (n > 0 && V->cur - s >= n) { ed_del(V, V->cur - n, n); V->cur -= n; }
  }
  if (ed_ins(V, V->cur, &c, 1) == 0) V->cur++;
}

static void ins_key(Vim *V, int k) {
  int L = vi_line(V), s = vi_ls(V, L), q;
  if (V->cpop && cmp_key(V, k)) return;
  switch (k) {
  case VK_ENTER: ins_newline(V); return;
  case VK_BS:
    if (V->cur > 0) { ed_del(V, V->cur - 1, 1); V->cur--; }
    return;
  case VK_TAB:
    if (V->cur > s && hl_wordch(vi_at(V, V->cur - 1))) { cmp_start(V, 1); return; }
    if (!V->et) { ins_char(V, '\t'); return; }
    {
      char sp[16];
      int n = V->sw - vi_vcol(V, V->cur) % V->sw, i;
      for (i = 0; i < n && i < 16; i++) sp[i] = ' ';
      if (ed_ins(V, V->cur, sp, i) == 0) V->cur += i;
    }
    return;
  case VCTRL('n'): case VCTRL('p'):
    if (!cmp_start(V, k == VCTRL('n') ? 1 : -1)) vi_say(V, 1, "-- no word to complete");
    return;
  case VCTRL('w'):
    q = V->cur;
    while (q > s && vi_space(vi_at(V, q - 1))) q--;
    if (q > s) {
      int c = vi_cls(vi_at(V, q - 1), 0);
      while (q > s && vi_cls(vi_at(V, q - 1), 0) == c) q--;
    } else if (q == s && q > 0 && q == V->cur) q--;
    ed_del(V, q, V->cur - q);
    V->cur = q;
    return;
  case VCTRL('u'):
    q = vi_fnb(V, L);
    if (q >= V->cur) q = s;
    ed_del(V, q, V->cur - q);
    V->cur = q;
    return;
  case VK_LEFT: if (V->cur > s) V->cur--; V->unew = 1; vi_setwant(V); return;
  case VK_RIGHT: if (V->cur < vi_le(V, L)) V->cur++; V->unew = 1; vi_setwant(V); return;
  case VK_UP: case VK_DOWN:
    if (k == VK_UP && L > 0) V->cur = vi_at_vcol(V, L - 1, V->want, 1);
    if (k == VK_DOWN && L < vi_last(V)) V->cur = vi_at_vcol(V, L + 1, V->want, 1);
    V->unew = 1;
    return;
  default:
    if ((k >= 32 && k < 127) || k == VK_TAB) ins_char(V, k);
  }
}

static void vi_commit_dot(Vim *V) {
  int i;
  if (V->rec_over || V->cmd_vis) { V->recording = 0; return; }
  for (i = 0; i < V->recn; i++) V->dot[i] = V->rec[i];
  V->dotn = V->recn;
  V->recording = 0;
}

static void ins_esc(Vim *V) {
  int i, j;
  V->cpop = 0;
  if (V->ins_cnt > 1 && !V->rec_over) {
    uint8_t keys[VI_REC];
    int n = V->recn - 1 - V->ins_rec0, rec = V->recording;
    for (i = 0; i < n; i++) keys[i] = V->rec[V->ins_rec0 + i];
    V->recording = 0;
    for (j = 1; j < V->ins_cnt; j++) {
      if (V->ins_kind == 'o' || V->ins_kind == 'O') ins_newline(V);
      for (i = 0; i < n; i++) ins_key(V, keys[i]);
    }
    V->recording = rec;
  }
  V->ins_cnt = 1;
  V->mode = VM_NORMAL;
  if (V->cur > vi_ls(V, vi_line(V))) V->cur--;
  vi_clamp(V);
  vi_setwant(V);
  if (V->recording) vi_commit_dot(V);
  V->msg[0] = 0;
}

/* --------------------------------------------------------------- files --- */

static void vi_reset(Vim *V) {
  vb_init(&V->t, V->t.b, V->t.cap);
  u_clear(V);
  V->usaved = 0;
  V->cur = V->want = V->top = V->left = 0;
  V->mode = VM_NORMAL;
  V->count = V->count1 = V->op = V->pend = 0;
  V->hlnv = 1;
  V->hlchk[0] = HS_NONE;
  V->chg_all = 1;
  V->cpop = 0;
  V->dos = 0;
  V->noeol = 0;
  V->gen++;
}

static const char *vi_name(const char *path) {
  const char *s = path, *p;
  for (p = path; *p; p++) if (*p == '/') s = p + 1;
  return s;
}

/* A path as typed, made whole: relative to the file's own folder, or /home. */
static void vi_resolve(Vim *V, const char *arg, char *out, int n) {
  int cut = 0, i;
  if (arg[0] == '/') { vi_strcpy(out, arg, n); return; }
  for (i = 0; V->path[i]; i++) if (V->path[i] == '/') cut = i;
  if (cut > 0 && cut < n - 2) {
    for (i = 0; i < cut; i++) out[i] = V->path[i];
    out[cut] = 0;
    vapi->fmt(out + cut, (size_t)(n - cut), "/%s", arg);
  } else {
    vapi->fmt(out, (size_t)n, CAPP_HOME "/%s", arg);
  }
}

/* Open a file into the buffer. 0, or -1 (and an empty, unnamed buffer: a
 * file that did not fit is not opened at all, so it cannot be saved over
 * with part of itself). A file that is not there is a new one. */
VI_FN int vi_load(Vim *V, const char *path) {
  char chunk[256], m[VI_MSG];
  int fd, n, i, cr = 0, bad = 0, total = 0;
  vi_reset(V);
  vi_strcpy(V->path, path, VI_PATH);
  V->lang = hl_lang_for(path);
  fd = safe_open_read(vapi, path);
  if (fd < 0) {
    vapi->fmt(m, sizeof m, "\"%s\" [New]", vi_name(path));
    vi_say(V, 0, m);
    return 0;
  }
  while (!bad && (n = vapi->read(fd, chunk, sizeof chunk)) > 0) {
    int k = 0;
    for (i = 0; i < n; i++) {
      if (chunk[i] == 0) { bad = 2; break; }
      if (chunk[i] == '\r') { cr = 1; continue; }
      chunk[k++] = chunk[i];
    }
    total += n;
    if (!bad && vb_insert(&V->t, vi_len(V), chunk, k) != 0) bad = 1;
  }
  vapi->close(fd);
  if (bad) {
    vi_reset(V);
    V->path[0] = 0;
    if (bad == 2) vapi->fmt(m, sizeof m, "\"%s\" is not text", vi_name(path));
    else vapi->fmt(m, sizeof m, "\"%s\" is too big for the %d KB buffer",
                   vi_name(path), V->t.cap / 1024);
    vi_say(V, 1, m);
    return -1;
  }
  V->dos = cr;
  if (vi_len(V) > 0 && vi_at(V, vi_len(V) - 1) == '\n') vb_delete(&V->t, vi_len(V) - 1, 1);
  else if (vi_len(V) > 0) V->noeol = 1;
  vapi->fmt(m, sizeof m, "\"%s\" %dL, %dB%s", vi_name(path), vi_last(V) + 1, total,
            V->dos ? " [dos]" : "");
  vi_say(V, 0, m);
  return 0;
}

static void vi_wr(Vim *V, SafeFile *f, int fd, const char *s, int n) {
  if (f) safe_write(f, s, (size_t)n);
  else vapi->write(fd, s, (size_t)n);
  (void)V;
}

/* Write the buffer to path: NAME.tmp and a rename (apps/safefile.h), or in
 * place for a path too long for it. 0, or -1 with the reason said. */
VI_FN int vi_save(Vim *V, const char *path) {
  SafeFile sf, *f = 0;
  int fd = -1, seg, rc;
  char m[VI_MSG];
  if (vapi->str_len(path) < SF_PATH_MAX) {
    if (safe_begin(&sf, vapi, path) != 0) { vi_say(V, 1, "E212: Can't open file for writing"); return -1; }
    f = &sf;
  } else {
    fd = vapi->open(path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
    if (fd < 0) { vi_say(V, 1, "E212: Can't open file for writing"); return -1; }
  }
  /* The two halves either side of the gap, a line at a time when the file
   * wants its carriage returns back. */
  for (seg = 0; seg < 2; seg++) {
    const char *s = seg ? V->t.b + V->t.ge : V->t.b;
    int n = seg ? V->t.cap - V->t.ge : V->t.gs, a = 0, i;
    if (!V->dos) { if (n) vi_wr(V, f, fd, s, n); continue; }
    for (i = 0; i < n; i++)
      if (s[i] == '\n') { vi_wr(V, f, fd, s + a, i - a); vi_wr(V, f, fd, "\r\n", 2); a = i + 1; }
    if (i > a) vi_wr(V, f, fd, s + a, i - a);
  }
  if (vi_len(V) > 0 && !V->noeol) vi_wr(V, f, fd, V->dos ? "\r\n" : "\n", V->dos ? 2 : 1);
  if (f) rc = safe_commit(f);
  else { vapi->close(fd); rc = 0; }
  if (rc != 0) { vi_say(V, 1, "E514: write error (card full?)"); return -1; }
  if (vi_streq(path, V->path) || !V->path[0]) {
    vi_strcpy(V->path, path, VI_PATH);
    V->usaved = V->upos;
    V->lang = hl_lang_for(path);
    V->hlnv = 1;
    V->chg_all = 1;
  }
  vapi->fmt(m, sizeof m, "\"%s\" %dL, %dB written", vi_name(path), vi_last(V) + 1,
            vi_len(V) + (V->noeol ? 0 : 1));
  vi_say(V, 0, m);
  return 0;
}

/* ------------------------------------------------------------ ex lines --- */

static const char *ex_skip(const char *p) { while (*p == ' ') p++; return p; }

/* One address: a number, ., $, '<, '>, with +N or -N. -2 for none. */
static int ex_addr(Vim *V, const char **pp) {
  const char *p = *pp;
  int L = -2;
  if (*p >= '0' && *p <= '9') { L = 0; while (*p >= '0' && *p <= '9') L = L * 10 + (*p++ - '0'); L--; }
  else if (*p == '.') { L = vi_line(V); p++; }
  else if (*p == '$') { L = vi_last(V); p++; }
  else if (p[0] == '\'' && p[1] == '<') { L = V->vis_l1; p += 2; }
  else if (p[0] == '\'' && p[1] == '>') { L = V->vis_l2; p += 2; }
  while (*p == '+' || *p == '-') {
    int sign = *p++ == '-' ? -1 : 1, n = 0, any = 0;
    if (L == -2) L = vi_line(V);
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p++ - '0'); any = 1; }
    L += sign * (any ? n : 1);
  }
  *pp = p;
  return L;
}

/* Undo the escapes in a :s piece, up to the delimiter. Where it stopped. */
static const char *ex_piece(const char *p, int delim, char *out, int max) {
  int n = 0;
  while (*p && *p != delim) {
    char c = *p++;
    if (c == '\\' && *p) {
      c = *p++;
      if (c == 't') c = '\t';
    }
    if (n < max - 1) out[n++] = c;
  }
  out[n] = 0;
  return p;
}

static void ex_subst(Vim *V, const char *p, int L1, int L2) {
  char pat[VI_PAT], rep[VI_PAT], m[VI_MSG];
  int delim, g = 0, ic = V->ic, L, subs = 0, lines = 0, lastL = -1, pl, rl, save_ic;
  delim = *p;
  if (!delim || delim == ' ' || hl_wordch(delim)) { vi_say(V, 1, "E486: :s/pattern/replacement/"); return; }
  p = ex_piece(p + 1, delim, pat, sizeof pat);
  if (*p == delim) p++;
  p = ex_piece(p, delim, rep, sizeof rep);
  if (*p == delim) p++;
  for (; *p; p++) {
    if (*p == 'g') g = 1;
    else if (*p == 'i') ic = 1;
    else if (*p == 'I') ic = 0;
  }
  if (!pat[0]) vi_strcpy(pat, V->pat, sizeof pat);
  else vi_strcpy(V->pat, pat, sizeof V->pat);
  V->pat_word = 0;
  if (!pat[0]) { vi_say(V, 1, "E35: No previous regular expression"); return; }
  pl = vi_strlen(pat);
  rl = vi_strlen(rep);
  save_ic = V->ic;
  V->ic = ic;
  for (L = L1; L <= L2 && L <= vi_last(V); L++) {
    int q = vi_ls(V, L), hit = 0;
    while (q + pl <= vi_le(V, L)) {
      if (vi_match_at(V, q, pat, pl, 0)) {
        ed_del(V, q, pl);
        if (ed_ins(V, q, rep, rl) != 0) { L = L2 + 1; break; }
        q += rl;
        subs++;
        hit = 1;
        if (!g) break;
      } else {
        q++;
      }
    }
    if (hit) { lines++; lastL = L; }
  }
  V->ic = save_ic;
  if (!subs) {
    vapi->fmt(m, sizeof m, "E486: Pattern not found: %s", pat);
    vi_say(V, 1, m);
    return;
  }
  V->cur = vi_fnb(V, lastL);
  vapi->fmt(m, sizeof m, "%d substitution%s on %d line%s", subs, subs == 1 ? "" : "s",
            lines, lines == 1 ? "" : "s");
  vi_say(V, 0, m);
}

static void ex_set(Vim *V, const char *p) {
  char m[VI_MSG], w[16];
  while (*(p = ex_skip(p))) {
    int n = 0, val = -1, on = 1;
    while (*p && *p != ' ' && *p != '=' && n < 15) w[n++] = *p++;
    w[n] = 0;
    if (*p == '=') {
      p++;
      val = 0;
      while (*p >= '0' && *p <= '9') val = val * 10 + (*p++ - '0');
    }
    while (*p && *p != ' ') p++;
    if (w[0] == 'n' && w[1] == 'o') { on = 0; vi_strcpy(w, w + 2, sizeof w); }
    if (vi_streq(w, "nu") || vi_streq(w, "number")) V->nu = on;
    else if (vi_streq(w, "et") || vi_streq(w, "expandtab")) V->et = on;
    else if (vi_streq(w, "ic") || vi_streq(w, "ignorecase")) V->ic = on;
    else if ((vi_streq(w, "sw") || vi_streq(w, "shiftwidth")) && val > 0 && val <= 16) V->sw = val;
    else if ((vi_streq(w, "ts") || vi_streq(w, "tabstop")) && val > 0 && val <= 16) V->ts = val;
    else if (vi_streq(w, "list") || vi_streq(w, "wrap")) { }
    else {
      vapi->fmt(m, sizeof m, "E518: Unknown option: %s", w);
      vi_say(V, 1, m);
      return;
    }
  }
  V->chg_all = 1;
  vapi->fmt(m, sizeof m, "%snu sw=%d ts=%d %set %sic", V->nu ? "" : "no", V->sw, V->ts,
            V->et ? "" : "no", V->ic ? "" : "no");
  vi_say(V, 0, m);
}

static int ex_write(Vim *V, const char *arg) {
  char path[VI_PATH];
  if (*arg) { vi_resolve(V, arg, path, sizeof path); return vi_save(V, path); }
  if (!V->path[0]) { V->req = VREQ_SAVEAS; return 1; }
  return vi_save(V, V->path);
}

VI_FN void vi_ex(Vim *V, const char *line) {
  const char *p = ex_skip(line);
  char cmd[12], m[VI_MSG];
  int L1 = -2, L2 = -2, n = 0, bang = 0;
  while (*p == ':') p = ex_skip(p + 1);
  if (*p == '%') { L1 = 0; L2 = vi_last(V); p++; }
  else {
    L1 = ex_addr(V, &p);
    if (*p == ',') { p++; L2 = ex_addr(V, &p); }
    else L2 = L1;
    if (L1 != -2 && L2 == -2) L2 = vi_last(V);
  }
  p = ex_skip(p);
  while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) && n < 11) cmd[n++] = *p++;
  cmd[n] = 0;
  if (*p == '!') { bang = 1; p++; }
  p = ex_skip(p);
  if (L1 != -2) {
    if (L1 < 0) L1 = 0;
    if (L2 > vi_last(V)) L2 = vi_last(V);
    if (L2 < L1) { int t = L1; L1 = L2; L2 = t; }
  }

  if (!n) {
    if (L1 == -2) return;
    V->cur = vi_fnb(V, L2 < 0 ? 0 : L2);
    vi_setwant(V);
    return;
  }
  if (vi_streq(cmd, "w") || vi_streq(cmd, "write")) { ex_write(V, p); return; }
  if (vi_streq(cmd, "wq") || vi_streq(cmd, "x") || vi_streq(cmd, "xit") || vi_streq(cmd, "wqa") ||
      vi_streq(cmd, "xa")) {
    int rc = 0;
    if (cmd[0] == 'w' || vi_modified(V) || *p) rc = ex_write(V, p);
    if (rc == 0) V->req = VREQ_QUIT;
    else if (rc == 1) V->quit_after = 1;
    return;
  }
  if (vi_streq(cmd, "q") || vi_streq(cmd, "quit") || vi_streq(cmd, "qa") || vi_streq(cmd, "qall")) {
    if (vi_modified(V) && !bang) { vi_say(V, 1, "E37: No write since last change (add ! to override)"); return; }
    V->req = VREQ_QUIT;
    return;
  }
  if (vi_streq(cmd, "Ex") || vi_streq(cmd, "Explore") || vi_streq(cmd, "Ve") || vi_streq(cmd, "Sex")) {
    if (vi_modified(V) && !bang) { vi_say(V, 1, "E37: No write since last change (add ! to override)"); return; }
    V->req = VREQ_TREE;
    return;
  }
  /* :run -- the file, written first, in the Python app (apps/vim/vim.c) */
  if (vi_streq(cmd, "run") || vi_streq(cmd, "py") || vi_streq(cmd, "python")) {
    if (!V->path[0]) { vi_say(V, 1, "E32: No file name"); return; }
    if (hl_lang_for(V->path) != LANG_PY) { vi_say(V, 1, "not a Python file: :run is for .py"); return; }
    if (vi_modified(V) && vi_save(V, V->path) != 0) return;
    V->req = VREQ_RUN;
    return;
  }
  if (vi_streq(cmd, "e") || vi_streq(cmd, "edit")) {
    char path[VI_PATH];
    if (vi_modified(V) && !bang) { vi_say(V, 1, "E37: No write since last change (add ! to override)"); return; }
    if (!*p) {
      if (bang && V->path[0]) { vi_strcpy(path, V->path, sizeof path); vi_load(V, path); }
      else V->req = VREQ_OPEN;
      return;
    }
    vi_resolve(V, p, path, sizeof path);
    vi_load(V, path);
    return;
  }
  /* :mkdir NAME -- a folder beside the file (or where it is written) */
  if (vi_streq(cmd, "mkdir") || vi_streq(cmd, "md")) {
    char path[VI_PATH];
    int len;
    if (!*p) { vi_say(V, 1, "E471: Argument required: :mkdir NAME"); return; }
    vi_resolve(V, p, path, sizeof path);
    len = (int)vapi->str_len(path);
    while (len > 1 && path[len - 1] == '/') path[--len] = 0;
    vapi->fmt(m, sizeof m, vapi->mkdir(path) == 0 ? "made %s/" : "could not make %s", path);
    vi_say(V, m[0] == 'c', m);
    return;
  }
  if (vi_streq(cmd, "set") || vi_streq(cmd, "se")) { ex_set(V, p); return; }
  if (vi_streq(cmd, "s") || vi_streq(cmd, "substitute")) {
    if (L1 == -2) L1 = L2 = vi_line(V);
    ex_subst(V, p, L1, L2);
    return;
  }
  if (vi_streq(cmd, "d") || vi_streq(cmd, "delete")) {
    if (L1 == -2) L1 = L2 = vi_line(V);
    vi_op_lines(V, 'd', L1, L2);
    return;
  }
  if (vi_streq(cmd, "noh") || vi_streq(cmd, "nohlsearch")) return;
  vapi->fmt(m, sizeof m, "E492: Not an editor command: %s", line);
  vi_say(V, 1, m);
}

/* -------------------------------------------------------- command line --- */

static void cl_open(Vim *V, int kind, const char *pre) {
  V->vprev = V->mode;
  V->mode = VM_CMD;
  V->clk = kind;
  vi_strcpy(V->cl, pre, VI_CL);
  V->cln = vi_strlen(V->cl);
  V->msg[0] = 0;
}

static void cl_key(Vim *V, int k) {
  if (k == VK_ESC) { V->mode = VM_NORMAL; vi_clamp(V); return; }
  if (k == VK_BS) {
    if (V->cln == 0) { V->mode = VM_NORMAL; vi_clamp(V); return; }
    V->cl[--V->cln] = 0;
    return;
  }
  if (k == VK_UP && V->clk == ':' && V->cl_last[0]) {
    vi_strcpy(V->cl, V->cl_last, VI_CL);
    V->cln = vi_strlen(V->cl);
    return;
  }
  if (k == VK_ENTER) {
    V->mode = VM_NORMAL;
    vi_clamp(V);
    if (V->clk == ':') {
      if (V->cl[0]) vi_strcpy(V->cl_last, V->cl, VI_CL);
      V->unew = 1;
      vi_ex(V, V->cl);
    } else {
      int p;
      if (V->cl[0]) vi_strcpy(V->pat, V->cl, VI_PAT);
      V->pat_back = V->clk == '?';
      V->pat_word = 0;
      p = vi_search_n(V, V->pat_back, 1);
      if (p >= 0) V->cur = p;
    }
    vi_clamp(V);
    vi_setwant(V);
    return;
  }
  if (k == VK_TAB) k = ' ';
  if (k >= 32 && k < 127 && V->cln < VI_CL - 1) {
    V->cl[V->cln++] = (char)k;
    V->cl[V->cln] = 0;
  }
}

/* --------------------------------------------------------- normal mode --- */

static void nm_reset(Vim *V) { V->count = V->count1 = V->op = V->pend = 0; }

/* A command finished: forget its keys, unless it changed the text and
 * finished in normal mode, when they become the dot. */
static void nm_done(Vim *V, int changed) {
  nm_reset(V);
  if (V->mode == VM_INSERT) return;
  if (changed) vi_commit_dot(V);
  V->recording = 0;
}

static void vi_key_(Vim *V, int k);

static void nm_dot(Vim *V, int cnt, int has) {
  uint8_t keys[VI_REC + 8];
  int n = 0, i = 0, j;
  if (!V->dotn) return;
  if (has) {
    char d[8];
    int m = vapi->fmt(d, sizeof d, "%d", cnt);
    while (i < V->dotn && V->dot[i] >= '0' && V->dot[i] <= '9') i++;
    for (j = 0; j < m; j++) keys[n++] = (uint8_t)d[j];
  }
  for (; i < V->dotn && n < VI_REC + 8; i++) keys[n++] = V->dot[i];
  nm_reset(V);
  V->recording = 0;
  for (i = 0; i < n; i++) vi_key_(V, keys[i]);
  if (V->mode == VM_INSERT) vi_key_(V, VK_ESC);   /* a dot ends what it starts */
}

static void vis_range(Vim *V, int *s, int *e) {
  *s = V->vanchor < V->cur ? V->vanchor : V->cur;
  *e = V->vanchor < V->cur ? V->cur : V->vanchor;
}

static void vis_leave(Vim *V) {
  int s, e;
  vis_range(V, &s, &e);
  V->vis_l1 = vi_lof(V, s);
  V->vis_l2 = vi_lof(V, e);
  V->mode = VM_NORMAL;
  V->chg_all = 1;
}

/* An operator on the selection. */
static void vis_op(Vim *V, int op) {
  int s, e, line = V->mode == VM_VLINE;
  vis_range(V, &s, &e);
  vis_leave(V);
  if (op == 'D' || op == 'X' || op == 'Y' || op == 'C' || op == 'S' || op == 'R') {
    line = 1;
    op = op == 'D' || op == 'X' ? 'd' : op == 'Y' ? 'y' : 'c';
  }
  if (line) vi_op_lines(V, op, vi_lof(V, s), vi_lof(V, e));
  else vi_op_chars(V, op, s, e < vi_len(V) ? e + 1 : e);
}

static void nm_key(Vim *V, int k);

static void nm_pending(Vim *V, int pend, int k, int cnt, int has) {
  int to, fl, r;
  if (k == VK_ESC) { nm_done(V, 0); return; }
  switch (pend) {
  case 'g':
    if (k == 'g') {
      r = vi_motion(V, VKM_GG, 0, cnt, has, &to, &fl);
      if (r > 0) {
        if (V->op) { int op = V->op; vi_apply(V, op, to, fl); nm_done(V, op != 'y'); return; }
        V->cur = to;
        vi_setwant(V);
      }
      nm_done(V, 0);
      return;
    }
    if (!V->op && (k == '~' || k == 'u' || k == 'U')) {
      V->op = k;
      V->count1 = V->count;
      V->count = 0;
      return;
    }
    if (!V->op && k == 'J') { nm_done(V, 0); return; }
    nm_done(V, 0);
    return;
  case 'f': case 'F': case 't': case 'T':
    r = vi_motion(V, pend, k, cnt, has, &to, &fl);
    if (r > 0) {
      if (V->op) { int op = V->op; vi_apply(V, op, to, fl); nm_done(V, op != 'y'); return; }
      V->cur = to;
      vi_setwant(V);
    }
    nm_done(V, 0);
    return;
  case 'r':
    if (V->mode == VM_VISUAL || V->mode == VM_VLINE) {
      int s, e, p;
      char c = (char)k;
      vis_range(V, &s, &e);
      vis_leave(V);
      if (k < 32 || k > 126) { nm_done(V, 0); return; }
      if (e < vi_len(V)) e++;
      for (p = s; p < e; p++)
        if (vi_at(V, p) != '\n') { ed_del(V, p, 1); ed_ins(V, p, &c, 1); }
      V->cur = s;
      nm_done(V, 0);
      return;
    }
    if ((k >= 32 && k < 127) || k == VK_ENTER) {
      int L = vi_line(V);
      if (V->cur + cnt <= vi_le(V, L) && vi_le(V, L) > vi_ls(V, L)) {
        vi_replace(V, k, cnt);
        nm_done(V, 1);
        return;
      }
    }
    nm_done(V, 0);
    return;
  case 'z': {
    int L = vi_line(V), rows = vi_rows(V);
    if (k == 'z' || k == '.') V->top = L - (rows - 1) / 2;
    else if (k == 't' || k == VK_ENTER) V->top = L;
    else if (k == 'b' || k == '-') V->top = L - rows + 1;
    if (V->top < 0) V->top = 0;
    V->chg_all = 1;
    nm_done(V, 0);
    return;
  }
  case ' ':                       /* the leader: space r runs */
    if (k == 'r') { vi_ex(V, "run"); nm_done(V, 0); return; }
    {                              /* anything else: space was l */
      int to2, fl2;
      if (vi_motion(V, ' ', 0, 1, 0, &to2, &fl2) > 0) { V->cur = to2; vi_setwant(V); }
    }
    nm_done(V, 0);
    nm_key(V, k);
    return;
  case 'Z':
    if (k == 'Z') vi_ex(V, "x");
    else if (k == 'Q') vi_ex(V, "q!");
    nm_done(V, 0);
    return;
  }
  nm_done(V, 0);
}

static void vi_scroll(Vim *V, int n) {
  int last = vi_last(V), L = vi_line(V);
  if (n > 0 && L == last) return;
  if (n < 0 && L == 0) return;
  V->top += n;
  if (V->top > last) V->top = last;
  if (V->top < 0) V->top = 0;
  L += n;
  if (L > last) L = last;
  if (L < 0) L = 0;
  V->cur = vi_at_vcol(V, L, V->want, 0);
  V->chg_all = 1;
}

static void nm_key(Vim *V, int k) {
  int fresh = !V->count && !V->op && !V->pend, cnt, has, to, fl, r, vis;
  vis = V->mode == VM_VISUAL || V->mode == VM_VLINE;
  if (fresh) {
    V->recn = 0;
    V->rec_over = 0;
    V->recording = 1;
    V->unew = 1;
    V->msg[0] = 0;
    V->cmd_vis = vis;
  }
  rec_put(V, k);
  cnt = (V->count1 ? V->count1 : 1) * (V->count ? V->count : 1);
  has = V->count1 || V->count;

  if (V->pend) { int p = V->pend; V->pend = 0; nm_pending(V, p, k, cnt, has); return; }
  /* Two of the file tree's own (apps/vim/vimtree.h): - opens it on this
   * file, as vim-vinegar does; space r runs the file. Only as a command of
   * their own: d- and 3- are still the motion. */
  if (fresh && !vis && k == '-') { vi_ex(V, "Ex"); nm_done(V, 0); return; }
  if (fresh && !vis && k == ' ') { V->pend = ' '; return; }
  if ((k >= '1' && k <= '9') || (k == '0' && V->count)) {
    V->count = V->count * 10 + (k - '0');
    if (V->count > 9999) V->count = 9999;
    return;
  }
  if (k == VK_ESC) {
    if (vis) vis_leave(V);
    nm_done(V, 0);
    return;
  }

  if (V->op) {
    int op = V->op;
    if (k == op || (op == '~' && k == '~') || (op == 'u' && k == 'u') || (op == 'U' && k == 'U')) {
      int L = vi_line(V), L2 = L + cnt - 1;
      if (L2 > vi_last(V)) L2 = vi_last(V);
      if (op == '~' || op == 'u' || op == 'U') vi_case(V, vi_ls(V, L), vi_le(V, L2), op);
      else vi_op_lines(V, op, L, L2);
      nm_done(V, op != 'y');
      return;
    }
    if (k == 'g' || k == 'f' || k == 'F' || k == 't' || k == 'T') { V->pend = k; return; }
    if (op == 'c' && (k == 'w' || k == 'W') && vi_cls(vi_at(V, V->cur), 0) != 0) {
      /* cw on a word changes to its end, not to the next word */
      int i, p = V->cur, big = k == 'W';
      for (i = 0; i < cnt; i++) {
        int q;
        if (i == 0 && p + 1 < vi_len(V) && vi_cls(vi_at(V, p + 1), big) == vi_cls(vi_at(V, p), big)) {
          q = vi_fwd_end(V, p, big);
        } else if (i == 0) {
          q = p;
        } else {
          q = vi_fwd_end(V, p, big);
        }
        p = q;
      }
      vi_apply(V, 'c', p, MF_INCL);
      nm_done(V, 1);
      return;
    }
    r = vi_motion(V, k, 0, cnt, has, &to, &fl);
    if (r > 0) { vi_apply(V, op, to, fl); nm_done(V, op != 'y'); return; }
    nm_done(V, 0);
    return;
  }

  /* motions, in either normal or visual mode */
  if (k != 'f' && k != 'F' && k != 't' && k != 'T') {
    r = vi_motion(V, k, 0, cnt, has, &to, &fl);
    if (r >= 0) {
      if (r > 0) {
        V->cur = to;
        vi_clamp(V);
        if (k == '$') V->want = VI_EOL;
        else if (!(fl & MF_WANT)) vi_setwant(V);
      }
      nm_done(V, 0);
      return;
    }
  }

  if (vis) {
    switch (k) {
    case 'o': { int t = V->cur; V->cur = V->vanchor; V->vanchor = t; vi_setwant(V); nm_done(V, 0); return; }
    case 'v': case 'V': {
      int want = k == 'v' ? VM_VISUAL : VM_VLINE;
      if (V->mode == want) vis_leave(V);
      else V->mode = want;
      V->chg_all = 1;
      nm_done(V, 0);
      return;
    }
    case 'd': case 'x': case 'y': case 'c': case 's': case '>': case '<': case '~': case 'u': case 'U':
    case 'D': case 'X': case 'Y': case 'C': case 'S': case 'R':
      vis_op(V, k == 'x' ? 'd' : k == 's' ? 'c' : k);
      nm_done(V, 0);
      return;
    case 'J': {
      int s, e, n;
      vis_range(V, &s, &e);
      vis_leave(V);
      n = vi_lof(V, e) - vi_lof(V, s) + 1;
      V->cur = s;
      vi_join(V, n);
      nm_done(V, 0);
      return;
    }
    case 'r': case 'f': case 'F': case 't': case 'T': case 'g': case 'z':
      V->pend = k;
      return;
    case ':':
      vis_leave(V);
      nm_done(V, 0);
      cl_open(V, ':', "'<,'>");
      return;
    }
    nm_done(V, 0);
    return;
  }

  switch (k) {
  case 'd': case 'c': case 'y': case '>': case '<':
    V->op = k;
    V->count1 = V->count;
    V->count = 0;
    return;
  case 'g': case 'z': case 'Z': case 'r': case 'f': case 'F': case 't': case 'T':
    V->pend = k;
    return;
  case 'x': case 'X': case 's': {
    int L = vi_line(V), s = vi_ls(V, L), e = vi_le(V, L), a = V->cur, b;
    if (k == 'X') { b = a; a -= cnt; if (a < s) a = s; }
    else { b = a + cnt; if (b > e) b = e; }
    if (a == b) {
      if (k == 's') { vi_insert_mode(V, 1, 's'); nm_done(V, 1); return; }
      nm_done(V, 0);
      return;
    }
    vi_op_chars(V, k == 's' ? 'c' : 'd', a, b);
    vi_clamp(V);
    nm_done(V, 1);
    return;
  }
  case 'D': case 'C': case 'Y': case 'S':
    if (k == 'Y' || k == 'S') {
      int L = vi_line(V), L2 = L + cnt - 1;
      if (L2 > vi_last(V)) L2 = vi_last(V);
      vi_op_lines(V, k == 'Y' ? 'y' : 'c', L, L2);
      nm_done(V, k == 'S');
      return;
    }
    V->op = k == 'D' ? 'd' : 'c';
    r = vi_motion(V, '$', 0, cnt, has, &to, &fl);
    if (vi_le(V, vi_line(V)) == vi_ls(V, vi_line(V)) && cnt == 1) {
      if (k == 'C') { vi_insert_mode(V, 1, 'C'); nm_done(V, 1); return; }
      nm_done(V, 0);
      return;
    }
    if (r > 0) vi_apply(V, V->op, to, fl);
    vi_clamp(V);
    nm_done(V, 1);
    return;
  case 'p': case 'P':
    vi_put(V, k == 'p', cnt);
    vi_clamp(V);
    vi_setwant(V);
    nm_done(V, 1);
    return;
  case 'J':
    vi_join(V, cnt);
    nm_done(V, 1);
    return;
  case '~': {
    int L = vi_line(V), e = vi_le(V, L), b = V->cur + cnt;
    if (b > e) b = e;
    if (b > V->cur) {
      vi_case(V, V->cur, b, '~');
      V->cur = b;
      vi_clamp(V);
      vi_setwant(V);
    }
    nm_done(V, 1);
    return;
  }
  case 'i': case 'a': case 'I': case 'A': case 'o': case 'O': {
    int L = vi_line(V);
    if (k == 'a' && vi_le(V, L) > vi_ls(V, L)) V->cur++;
    else if (k == 'A') V->cur = vi_le(V, L);
    else if (k == 'I') V->cur = vi_fnb(V, L);
    else if (k == 'o' || k == 'O') vi_open_line(V, k == 'o');
    vi_insert_mode(V, cnt, k);
    nm_done(V, 1);
    return;
  }
  case 'u': vi_undo(V, cnt); nm_done(V, 0); return;
  case VCTRL('r'): vi_redo(V, cnt); nm_done(V, 0); return;
  case '.': nm_dot(V, cnt, has); return;
  case 'v': case 'V':
    V->vanchor = V->cur;
    V->mode = k == 'v' ? VM_VISUAL : VM_VLINE;
    V->chg_all = 1;
    nm_done(V, 0);
    return;
  case ':': nm_done(V, 0); cl_open(V, ':', has ? "." : ""); if (has) {
      char d[12]; vapi->fmt(d, sizeof d, ".,.+%d", cnt - 1); vi_strcpy(V->cl, d, VI_CL); V->cln = vi_strlen(V->cl);
    }
    return;
  case '/': case '?': nm_done(V, 0); cl_open(V, k, ""); return;
  case VCTRL('d'): case VCTRL('u'): {
    int n = has ? cnt : vi_rows(V) / 2;
    vi_scroll(V, k == VCTRL('d') ? n : -n);
    nm_done(V, 0);
    return;
  }
  case VCTRL('f'): case VCTRL('b'):
    vi_scroll(V, (k == VCTRL('f') ? 1 : -1) * cnt * (vi_rows(V) - 2));
    nm_done(V, 0);
    return;
  case VCTRL('e'): case VCTRL('y'): {
    int L = vi_line(V), rows = vi_rows(V);
    V->top += k == VCTRL('e') ? cnt : -cnt;
    if (V->top > vi_last(V)) V->top = vi_last(V);
    if (V->top < 0) V->top = 0;
    if (L < V->top) V->cur = vi_at_vcol(V, V->top, V->want, 0);
    if (L >= V->top + rows) V->cur = vi_at_vcol(V, V->top + rows - 1, V->want, 0);
    V->chg_all = 1;
    nm_done(V, 0);
    return;
  }
  case VCTRL('g'): {
    char m[VI_MSG];
    vapi->fmt(m, sizeof m, "\"%s\"%s %d lines --%d%%--", V->path[0] ? vi_name(V->path) : "[No Name]",
              vi_modified(V) ? " [Modified]" : "", vi_last(V) + 1,
              (vi_line(V) + 1) * 100 / (vi_last(V) + 1));
    vi_say(V, 0, m);
    nm_done(V, 0);
    return;
  }
  }
  nm_done(V, 0);
}

static void vi_key_(Vim *V, int k) {
  if (V->mode == VM_CMD) { cl_key(V, k); return; }
  if (V->mode == VM_INSERT) {
    rec_put(V, k);
    if (k == VK_ESC) ins_esc(V);
    else ins_key(V, k);
    return;
  }
  nm_key(V, k);
}

/* A key, from the keyboard or a test. */
VI_FN void vi_key(Vim *V, int k) {
  vi_key_(V, k);
  if (V->mode != VM_INSERT) vi_clamp(V);
  vi_layout(V);
}

VI_FN void vi_keys(Vim *V, const char *s) {
  while (*s) vi_key(V, (uint8_t)*s++);
}

/* The memory, the options' defaults, an empty buffer. */
VI_FN void vi_init(Vim *V, char *text, int tcap, char *undo, int ucap, char *reg, int rcap) {
  vb_init(&V->t, text, tcap);
  V->u = undo;
  V->ucap = ucap;
  V->r = reg;
  V->rcap = rcap;
  V->rlen = 0;
  V->nu = 1;
  V->sw = 2;
  V->ts = 4;
  V->et = 1;
  V->ic = 0;
  V->rows = 13;
  V->width = 40;
  V->dotn = 0;
  V->pat[0] = 0;
  V->path[0] = 0;
  V->lang = LANG_PLAIN;
  V->req = VREQ_NONE;
  vi_reset(V);
  vi_layout(V);
}

VI_FN const char *vi_mode_name(Vim *V) {
  switch (V->mode) {
  case VM_INSERT: return "INSERT";
  case VM_VISUAL: return "VISUAL";
  case VM_VLINE:  return "V-LINE";
  case VM_CMD:    return "COMMAND";
  }
  return "NORMAL";
}

#endif /* CARDOS_VIMCORE_H */
