/* Game Night's scoreboard (apps/gamenight.c): players across, rounds down, a
 * total under each column. For any game that is scored in rounds -- Hearts,
 * Oh Hell, Taboo, darts -- so it knows nothing about any of them: a cell is
 * a number someone typed, a total is a sum, and the leader is the highest
 * total, or the lowest when low wins.
 *
 * It does not follow whose turn it is. It does remember who goes first, set
 * by hand or by gs_pick_first's random choice, and marks them.
 *
 * Saved as text, a line a thing, so Files and Edit can read it:
 *     low 0                    1: the lowest total wins
 *     first 2                  who goes first, from 0; -1 nobody chosen
 *     player Alice             in column order
 *     round 5 -3 _ 12          one per round, a number per player, _ blank
 *
 * Portable and libc-free; host-tested in test/test_gamenight.c. */
#ifndef CARDOS_GN_SCORE_H
#define CARDOS_GN_SCORE_H

#include <stdint.h>

#if defined(__GNUC__)
#define GS_OPT __attribute__((unused))
#else
#define GS_OPT
#endif

#define GS_MAXP   8                     /* players or teams */
#define GS_MAXR   60                    /* rounds */
#define GS_NAME   10                    /* characters of a name */
#define GS_EMPTY  (-32768)              /* a cell nobody has filled */
#define GS_MAXV   9999                  /* a cell is -9999..9999 */

typedef struct {
  int     np, nr;
  char    name[GS_MAXP][GS_NAME + 1];
  int16_t cell[GS_MAXR][GS_MAXP];
  int     first;                        /* who goes first, -1 none */
  int     low;                          /* the lowest total wins */
} GScore;

static GS_OPT void gs_init(GScore *g) {
  int r, c;
  g->np = g->nr = 0;
  g->first = -1;
  g->low = 0;
  for (c = 0; c < GS_MAXP; c++) g->name[c][0] = 0;
  for (r = 0; r < GS_MAXR; r++)
    for (c = 0; c < GS_MAXP; c++) g->cell[r][c] = GS_EMPTY;
}

/* A new game: the same players, every score gone, nobody first. */
static GS_OPT void gs_new_game(GScore *g) {
  int r, c;
  for (r = 0; r < GS_MAXR; r++)
    for (c = 0; c < GS_MAXP; c++) g->cell[r][c] = GS_EMPTY;
  g->nr = 0;
  g->first = -1;
}

/* A name in, trimmed and cut to GS_NAME; its column, or -1 if full or blank. */
static GS_OPT int gs_add_player(GScore *g, const char *name) {
  int i = 0, k = 0;
  if (g->np >= GS_MAXP) return -1;
  while (name[i] == ' ') i++;
  while (name[i] && k < GS_NAME) g->name[g->np][k++] = name[i++];
  while (k && g->name[g->np][k - 1] == ' ') k--;
  g->name[g->np][k] = 0;
  if (!k) return -1;
  return g->np++;
}

static GS_OPT void gs_rename(GScore *g, int c, const char *name) {
  int i = 0, k = 0;
  if (c < 0 || c >= g->np) return;
  while (name[i] == ' ') i++;
  if (!name[i]) return;
  while (name[i] && k < GS_NAME) g->name[c][k++] = name[i++];
  while (k && g->name[c][k - 1] == ' ') k--;
  g->name[c][k] = 0;
}

/* Column c goes, and its scores; the columns after it move left. */
static GS_OPT void gs_del_player(GScore *g, int c) {
  int r, k, i;
  if (c < 0 || c >= g->np) return;
  for (k = c; k + 1 < g->np; k++) {
    for (i = 0; i <= GS_NAME; i++) g->name[k][i] = g->name[k + 1][i];
    for (r = 0; r < GS_MAXR; r++) g->cell[r][k] = g->cell[r][k + 1];
  }
  g->np--;
  for (r = 0; r < GS_MAXR; r++) g->cell[r][g->np] = GS_EMPTY;
  g->name[g->np][0] = 0;
  if (g->first == c) g->first = -1;
  else if (g->first > c) g->first--;
}

/* Swap columns a and b: the order players sit in. */
static GS_OPT void gs_swap(GScore *g, int a, int b) {
  int r, i;
  if (a < 0 || b < 0 || a >= g->np || b >= g->np || a == b) return;
  for (i = 0; i <= GS_NAME; i++) {
    char t = g->name[a][i];
    g->name[a][i] = g->name[b][i];
    g->name[b][i] = t;
  }
  for (r = 0; r < GS_MAXR; r++) {
    int16_t t = g->cell[r][a];
    g->cell[r][a] = g->cell[r][b];
    g->cell[r][b] = t;
  }
  if (g->first == a) g->first = b;
  else if (g->first == b) g->first = a;
}

/* Round r, column c := v. Writing in the row below the last starts a round.
 * 0, or -1 if r is beyond that, the board is full, or v is out of range. */
static GS_OPT int gs_set(GScore *g, int r, int c, int v) {
  if (c < 0 || c >= g->np || r < 0 || r > g->nr || r >= GS_MAXR) return -1;
  if (v < -GS_MAXV || v > GS_MAXV) return -1;
  if (r == g->nr) g->nr++;
  g->cell[r][c] = (int16_t)v;
  return 0;
}

static GS_OPT void gs_clear(GScore *g, int r, int c) {
  if (r >= 0 && r < g->nr && c >= 0 && c < g->np) g->cell[r][c] = GS_EMPTY;
}

/* Round r goes; the rounds after it move up. */
static GS_OPT void gs_del_round(GScore *g, int r) {
  int k, c;
  if (r < 0 || r >= g->nr) return;
  for (k = r; k + 1 < g->nr; k++)
    for (c = 0; c < GS_MAXP; c++) g->cell[k][c] = g->cell[k + 1][c];
  g->nr--;
  for (c = 0; c < GS_MAXP; c++) g->cell[g->nr][c] = GS_EMPTY;
}

static GS_OPT int32_t gs_total(const GScore *g, int c) {
  int32_t t = 0;
  int r;
  for (r = 0; r < g->nr; r++)
    if (g->cell[r][c] != GS_EMPTY) t += g->cell[r][c];
  return t;
}

/* Has anyone a score yet? (No leader on an empty board.) */
static GS_OPT int gs_any(const GScore *g) {
  int r, c;
  for (r = 0; r < g->nr; r++)
    for (c = 0; c < g->np; c++)
      if (g->cell[r][c] != GS_EMPTY) return 1;
  return 0;
}

/* The leaders, a bit per column: the best total (highest, or lowest when low
 * wins); a tie is all of them. 0 with no scores. */
static GS_OPT unsigned gs_leaders(const GScore *g) {
  int c;
  int32_t best = 0;
  unsigned m = 0;
  if (!g->np || !gs_any(g)) return 0;
  for (c = 0; c < g->np; c++) {
    int32_t t = gs_total(g, c);
    if (!m || (g->low ? t < best : t > best)) { best = t; m = 1u << c; }
    else if (t == best) m |= 1u << c;
  }
  return m;
}

/* Who goes first, at random: `rnd` is any random number. */
static GS_OPT int gs_pick_first(GScore *g, uint32_t rnd) {
  if (!g->np) return -1;
  g->first = (int)(rnd % (uint32_t)g->np);
  return g->first;
}

/* ---- as text ----------------------------------------------------------------- */

static GS_OPT int gs_put(char *b, int n, int cap, const char *s) {
  while (*s && n < cap - 1) b[n++] = *s++;
  b[n] = 0;
  return n;
}

static GS_OPT int gs_put_int(char *b, int n, int cap, int32_t v) {
  char d[12];
  int k = 0;
  uint32_t u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
  if (v < 0 && n < cap - 1) b[n++] = '-';
  do { d[k++] = (char)('0' + u % 10); u /= 10; } while (u);
  while (k && n < cap - 1) b[n++] = d[--k];
  b[n] = 0;
  return n;
}

/* The board as text into buf (cap bytes, 4 KB is plenty): its length. */
static GS_OPT int gs_save(const GScore *g, char *buf, int cap) {
  int n = 0, r, c;
  n = gs_put(buf, n, cap, "low ");
  n = gs_put_int(buf, n, cap, g->low);
  n = gs_put(buf, n, cap, "\nfirst ");
  n = gs_put_int(buf, n, cap, g->first);
  n = gs_put(buf, n, cap, "\n");
  for (c = 0; c < g->np; c++) {
    n = gs_put(buf, n, cap, "player ");
    n = gs_put(buf, n, cap, g->name[c]);
    n = gs_put(buf, n, cap, "\n");
  }
  for (r = 0; r < g->nr; r++) {
    n = gs_put(buf, n, cap, "round");
    for (c = 0; c < g->np; c++) {
      n = gs_put(buf, n, cap, " ");
      if (g->cell[r][c] == GS_EMPTY) n = gs_put(buf, n, cap, "_");
      else n = gs_put_int(buf, n, cap, g->cell[r][c]);
    }
    n = gs_put(buf, n, cap, "\n");
  }
  return n;
}

static GS_OPT int gs_starts(const char *s, const char *w) {
  while (*w) if (*s++ != *w++) return 0;
  return 1;
}

/* A number at *p: 0 and *ok = 0 if there is none. */
static GS_OPT int32_t gs_num(const char **p, int *ok) {
  const char *s = *p;
  int neg = 0, any = 0;
  int32_t v = 0;
  while (*s == ' ') s++;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') { if (v < 100000) v = v * 10 + (*s - '0'); s++; any = 1; }
  *p = s;
  *ok = any;
  return neg ? -v : v;
}

/* Text back into the board; what it does not understand it skips, so a file
 * edited by hand keeps what it can. */
static GS_OPT void gs_load(GScore *g, const char *t) {
  char line[96];
  gs_init(g);
  while (*t) {
    int k = 0, ok;
    const char *p;
    while (*t && *t != '\n') { if (*t != '\r' && k < (int)sizeof line - 1) line[k++] = *t; t++; }
    if (*t) t++;
    line[k] = 0;
    p = line;
    if (gs_starts(line, "low ")) { p += 4; g->low = gs_num(&p, &ok) != 0; }
    else if (gs_starts(line, "first ")) {
      int32_t v;
      p += 6;
      v = gs_num(&p, &ok);
      g->first = ok ? (int)v : -1;
    } else if (gs_starts(line, "player ")) gs_add_player(g, line + 7);
    else if (gs_starts(line, "round") && g->nr < GS_MAXR) {
      int c = 0, r = g->nr;
      p += 5;
      g->nr++;
      while (c < g->np) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '_') { p++; c++; continue; }
        {
          int32_t v = gs_num(&p, &ok);
          if (!ok) break;
          if (v >= -GS_MAXV && v <= GS_MAXV) g->cell[r][c] = (int16_t)v;
          c++;
        }
      }
    }
  }
  if (g->first >= g->np) g->first = -1;
}

#endif
