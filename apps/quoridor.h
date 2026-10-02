/* Quoridor: the rules, and a computer player. No screen, no API, no libc --
 * apps/quoridor.c draws it and test/test_quoridor.c plays it on the host.
 *
 * A 9x9 board. Each side has a pawn and ten walls; you win by reaching the
 * far row. A turn is a step (up, down, left, right) or a wall: two cells
 * long, laid in a groove between rows or columns, never across another wall,
 * and never so as to leave either pawn with no way through. A pawn facing
 * the other jumps over it, or -- with a wall or the edge behind it --
 * sideways round it.
 *
 * Player 0 starts at the bottom (row 8) and goes up; player 1 starts at the
 * top and goes down. Walls are named by the groove crossing at their middle:
 * (r, c) with r and c in 0..7, between rows r and r+1 and columns c and c+1.
 * A horizontal wall there blocks moving between rows r and r+1 in columns c
 * and c+1; a vertical one, between columns c and c+1 in rows r and r+1.
 *
 * The computer is simple and honest: it measures both shortest paths. When
 * it is ahead it walks its own. When it is not, it looks at every wall it
 * could lay and lays the one that most lengthens the other path against its
 * own -- if that beats a step. "Ahead" counts the turn: it is the one moving.
 */
#ifndef CARDOS_QUORIDOR_H
#define CARDOS_QUORIDOR_H

#include <stdint.h>

#define Q_N     9
#define Q_G     8                 /* grooves a side */
#define Q_WALLS 10
#define Q_FAR   99                /* no way through */

typedef struct {
  int8_t  r[2], c[2];             /* the pawns */
  int8_t  left[2];                /* walls in hand */
  uint8_t h[Q_G][Q_G], v[Q_G][Q_G];
  int8_t  turn;                   /* whose: 0 or 1 */
  int8_t  winner;                 /* -1 none yet */
} Quoridor;

/* A turn: a step to (r, c), or a wall at (r, c), vertical or not. */
typedef struct {
  int8_t wall;                    /* 0 step, 1 wall */
  int8_t vert;
  int8_t r, c;
} QMove;

static const int8_t Q_DR[4] = { -1, 1, 0, 0 };   /* up, down, left, right */
static const int8_t Q_DC[4] = { 0, 0, -1, 1 };

static void q_init(Quoridor *q) {
  int i, j;
  for (i = 0; i < Q_G; i++)
    for (j = 0; j < Q_G; j++) q->h[i][j] = q->v[i][j] = 0;
  q->r[0] = Q_N - 1; q->c[0] = Q_N / 2;
  q->r[1] = 0;       q->c[1] = Q_N / 2;
  q->left[0] = q->left[1] = Q_WALLS;
  q->turn = 0;
  q->winner = -1;
}

static int q_goal(int who) { return who == 0 ? 0 : Q_N - 1; }

/* Is the way from (r, c) one cell in direction d shut, by a wall or the edge? */
static int q_blocked(const Quoridor *q, int r, int c, int d) {
  int nr = r + Q_DR[d], nc = c + Q_DC[d];
  if (nr < 0 || nr >= Q_N || nc < 0 || nc >= Q_N) return 1;
  if (d < 2) {                                /* across a row line */
    int gr = d == 0 ? r - 1 : r;              /* the groove between gr, gr+1 */
    if (c < Q_G && q->h[gr][c]) return 1;
    if (c > 0 && q->h[gr][c - 1]) return 1;
  } else {                                    /* across a column line */
    int gc = d == 2 ? c - 1 : c;
    if (r < Q_G && q->v[r][gc]) return 1;
    if (r > 0 && q->v[r - 1][gc]) return 1;
  }
  return 0;
}

/* Steps from (r, c) to the goal row, ignoring pawns -- or Q_FAR. */
static int q_path_from(const Quoridor *q, int r, int c, int goal) {
  int8_t dist[Q_N][Q_N];
  uint8_t qr[Q_N * Q_N], qc[Q_N * Q_N];
  int head = 0, tail = 0, i, j, d;
  for (i = 0; i < Q_N; i++)
    for (j = 0; j < Q_N; j++) dist[i][j] = -1;
  dist[r][c] = 0;
  qr[tail] = (uint8_t)r; qc[tail++] = (uint8_t)c;
  while (head < tail) {
    int cr = qr[head], cc = qc[head++];
    if (cr == goal) return dist[cr][cc];
    for (d = 0; d < 4; d++) {
      int nr = cr + Q_DR[d], nc = cc + Q_DC[d];
      if (q_blocked(q, cr, cc, d) || dist[nr][nc] >= 0) continue;
      dist[nr][nc] = (int8_t)(dist[cr][cc] + 1);
      qr[tail] = (uint8_t)nr; qc[tail++] = (uint8_t)nc;
    }
  }
  return Q_FAR;
}

static int q_path(const Quoridor *q, int who) {
  return q_path_from(q, q->r[who], q->c[who], q_goal(who));
}

/* Where `who` may step: up to five cells into out_r/out_c. */
static int q_steps(const Quoridor *q, int who, int8_t *out_r, int8_t *out_c) {
  int r = q->r[who], c = q->c[who], o = 1 - who, n = 0, d, s;
  for (d = 0; d < 4; d++) {
    int nr = r + Q_DR[d], nc = c + Q_DC[d];
    if (q_blocked(q, r, c, d)) continue;
    if (nr != q->r[o] || nc != q->c[o]) { out_r[n] = (int8_t)nr; out_c[n++] = (int8_t)nc; continue; }
    /* Face to face: over, if nothing is behind; otherwise to either side. */
    if (!q_blocked(q, nr, nc, d)) {
      out_r[n] = (int8_t)(nr + Q_DR[d]); out_c[n++] = (int8_t)(nc + Q_DC[d]);
      continue;
    }
    for (s = 0; s < 4; s++) {
      if ((s < 2) == (d < 2)) continue;       /* only the perpendicular ones */
      if (q_blocked(q, nr, nc, s)) continue;
      out_r[n] = (int8_t)(nr + Q_DR[s]); out_c[n++] = (int8_t)(nc + Q_DC[s]);
    }
  }
  return n;
}

/* Does a wall here touch one already laid? */
static int q_wall_clashes(const Quoridor *q, int r, int c, int vert) {
  if (r < 0 || r >= Q_G || c < 0 || c >= Q_G) return 1;
  if (q->h[r][c] || q->v[r][c]) return 1;               /* the same crossing */
  if (vert) return (r > 0 && q->v[r - 1][c]) || (r < Q_G - 1 && q->v[r + 1][c]);
  return (c > 0 && q->h[r][c - 1]) || (c < Q_G - 1 && q->h[r][c + 1]);
}

/* Could `who` lay this wall? Clashes, walls in hand, and both ways through. */
static int q_wall_ok(Quoridor *q, int who, int r, int c, int vert) {
  int ok;
  if (q->left[who] <= 0 || q_wall_clashes(q, r, c, vert)) return 0;
  if (vert) q->v[r][c] = 1; else q->h[r][c] = 1;
  ok = q_path(q, 0) < Q_FAR && q_path(q, 1) < Q_FAR;
  if (vert) q->v[r][c] = 0; else q->h[r][c] = 0;
  return ok;
}

static int q_step_ok(const Quoridor *q, int who, int r, int c) {
  int8_t sr[5], sc[5];
  int n = q_steps(q, who, sr, sc), i;
  for (i = 0; i < n; i++) if (sr[i] == r && sc[i] == c) return 1;
  return 0;
}

/* Play a turn for whoever's turn it is. 0 if done, -1 if not allowed. */
static int q_play(Quoridor *q, QMove m) {
  int who = q->turn;
  if (q->winner >= 0) return -1;
  if (m.wall) {
    if (!q_wall_ok(q, who, m.r, m.c, m.vert)) return -1;
    if (m.vert) q->v[m.r][m.c] = 1; else q->h[m.r][m.c] = 1;
    q->left[who]--;
  } else {
    if (!q_step_ok(q, who, m.r, m.c)) return -1;
    q->r[who] = m.r;
    q->c[who] = m.c;
    if (m.r == q_goal(who)) q->winner = (int8_t)who;
  }
  q->turn = (int8_t)(1 - who);
  return 0;
}

/* The computer's turn for `who`. `seed` varies the choice between equals. */
static QMove q_ai(Quoridor *q, int who, uint32_t seed) {
  int o = 1 - who, mine = q_path(q, who), theirs = q_path(q, o);
  int8_t sr[5], sc[5];
  int n = q_steps(q, who, sr, sc), i, r, c, vert, best = Q_FAR, ties = 0;
  QMove step = { 0, 0, q->r[who], q->c[who] }, wall = { 1, 0, 0, 0 };
  int wall_gain = 0;

  /* The step that most shortens the way home; between equals, by the seed. */
  for (i = 0; i < n; i++) {
    int d = q_path_from(q, sr[i], sc[i], q_goal(who));
    if (d < best) { best = d; ties = 1; step.r = sr[i]; step.c = sc[i]; }
    else if (d == best && (seed >> (ties++ % 16) & 1)) { step.r = sr[i]; step.c = sc[i]; }
  }
  if (best == 0 || q->left[who] == 0) return step;     /* home, or nothing else */

  /* Behind or level -- moving first counts for one -- and not about to win:
   * the wall that does the most damage, if it does more than a step would. */
  if (theirs <= mine || theirs <= 2) {
    for (vert = 0; vert < 2; vert++)
      for (r = 0; r < Q_G; r++)
        for (c = 0; c < Q_G; c++) {
          int t, m2, gain;
          if (!q_wall_ok(q, who, r, c, vert)) continue;
          if (vert) q->v[r][c] = 1; else q->h[r][c] = 1;
          t = q_path(q, o);
          m2 = q_path(q, who);
          if (vert) q->v[r][c] = 0; else q->h[r][c] = 0;
          gain = (t - theirs) - (m2 - mine);
          if (gain > wall_gain || (gain == wall_gain && gain > 0 && ((seed >> ((r * 8 + c) % 32)) & 1))) {
            wall_gain = gain;
            wall.vert = (int8_t)vert; wall.r = (int8_t)r; wall.c = (int8_t)c;
          }
        }
    if (wall_gain >= 2 || (wall_gain >= 1 && theirs <= mine - 1) || (wall_gain >= 1 && theirs <= 2))
      return wall;
  }
  return step;
}

#endif /* CARDOS_QUORIDOR_H */
