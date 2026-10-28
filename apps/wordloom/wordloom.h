/* Wordloom's rules: an 11x11 loom, racks of six, eighty tiles, and a game
 * that is only ever a seed and a list of moves.
 *
 * Portable C with no libc (the device app links none), host-tested in
 * apps/wordloom/test/test_wordloom.c. The app (wordloom.c) is the screen and
 * the network; everything that decides anything is here.
 *
 * THE GAME
 *   Two players. Each holds six tiles. On a turn a player does one of:
 *     weave  lay 1-6 tiles from the rack in one row or one column, with no
 *            empty cell between them (cells already holding tiles may sit
 *            between, and become part of the word). The first weave covers
 *            the centre and lays at least two tiles; every later one
 *            touches a tile already on the loom.
 *     swap   put 1-6 tiles back and draw as many (the bag must hold that
 *            many); scores nothing.
 *     pass   scores nothing.
 *     resign the other player wins; allowed on either player's turn.
 *   After a weave the rack is filled back up to six from the bag.
 *
 * SCORING A WEAVE
 *   Every word the weave makes counts: the word along its line, and each
 *   word across it through a new tile -- any run of two or more tiles.
 *   A word is the sum of its letters (WL_VALUE) times the premiums under
 *   the tiles laid THIS turn (a premium is used once):
 *     knot  k  letter x2        gold  g  letter x3
 *     warp  w  word x2          loom  L  word x3
 *   (the centre is a knot). Then two bonuses of Wordloom's own:
 *     weave  +5   the play makes three or more words at once
 *     bolt   +20  all six tiles laid in one play
 *
 * THE END
 *   - A player lays their last tile with the bag empty: they "tie off",
 *     +10 (WL_TIEOFF), and the other loses the value of the tiles they
 *     still hold.
 *   - Four turns in a row score nothing (swaps and passes, both players):
 *     each loses the value of their own rack.
 *   - A resignation: the other player wins, whatever the score.
 *   Otherwise the higher score wins; equal is a draw.
 *
 * WORDS ARE ON HONOUR
 *   There is no dictionary on the device -- a word list is megabytes, and
 *   flash is the constraint. wl_check takes a WlDict callback that is asked
 *   about every word a weave makes; the app passes none (0) and every word
 *   stands. A dictionary later is a function, not a change to the rules.
 *
 * WHAT A GAME IS
 *   A seed and the moves, in the order the server numbered them. The bag is
 *   the eighty tiles of WL_COUNT in letter order, shuffled by Fisher-Yates
 *   with xorshift32 (wl_rand) from the seed (0 means 0x9E3779B9); player 0
 *   draws six, then player 1 (tiles come off the END of the bag). A swap
 *   draws first, then puts the swapped tiles at the end of the bag and
 *   shuffles the whole bag again with the same generator. So every device
 *   that replays the same moves from the same seed has the same loom, racks,
 *   bag and score -- nothing else is ever stored or sent.
 *
 *   The room's state (wl_state_format): "wordloom 1\nseed N\n". The players
 *   are the room's members in order -- the creator first, and the server
 *   never reorders them (server/rooms.py) -- and player 0 moves first.
 *
 * THE MOVE FORMAT (a room move's body, one line, lower case)
 *   play R C a|d LETTERS   R, C the row and column (0-10) of the FIRST tile
 *                          laid; a is across (columns increase), d down;
 *                          LETTERS the tiles laid, in order -- cells already
 *                          holding tiles are stepped over, not written.
 *                          "play 5 4 a crow" lays c,r,o,w from (5,4).
 *   swap LETTERS           "swap qvx"
 *   pass
 *   resign
 *   A move from someone whose turn it is not, or one the rules refuse, is
 *   skipped by every device alike (wl_replay counts them), so a game can
 *   never fork.
 */
#ifndef CARDOS_WORDLOOM_H
#define CARDOS_WORDLOOM_H

#include <stdint.h>
#include "apps/str.h"

#ifdef __GNUC__
#define WL_OPT __attribute__((unused))
#else
#define WL_OPT
#endif

#define WL_VERSION 1
#define WL_N       11
#define WL_MID     5
#define WL_RACK    6
#define WL_TILES   80
#define WL_STALL   4          /* scoreless turns in a row that end the game */
#define WL_WEAVE   5          /* a play making three or more words */
#define WL_BOLT    20         /* all six tiles in one play */
#define WL_TIEOFF  10         /* laying your last tile with the bag empty */
#define WL_WORDS_MAX (1 + WL_RACK)
#define WL_SEED0   0x9E3779B9u

enum { WL_ACROSS = 0, WL_DOWN = 1 };
enum { WL_PLAIN = 0, WL_KNOT, WL_GOLD, WL_WARP, WL_LOOM };
enum { WL_PLAYING = 0, WL_ENDED, WL_RESIGNED };
enum { WL_K_NONE = 0, WL_K_PLAY, WL_K_SWAP, WL_K_PASS, WL_K_RESIGN };

/* What wl_check, wl_apply and friends return: 0 or one of these. */
#define WL_OK        0
#define WL_E_OVER   (-1)     /* the game has ended */
#define WL_E_TURN   (-2)     /* not this player's turn */
#define WL_E_RACK   (-3)     /* a letter the rack does not hold */
#define WL_E_OFF    (-4)     /* runs off the loom */
#define WL_E_TAKEN  (-5)     /* the first cell already has a tile */
#define WL_E_CENTRE (-6)     /* the first weave misses the centre */
#define WL_E_SHORT  (-7)     /* the first weave is one tile */
#define WL_E_ALONE  (-8)     /* touches nothing on the loom */
#define WL_E_LINE   (-9)     /* tiles not in one row or column */
#define WL_E_GAP    (-10)    /* an empty cell between the tiles */
#define WL_E_NONE   (-11)    /* nothing laid / nothing named */
#define WL_E_BAG    (-12)    /* a swap of more tiles than the bag holds */
#define WL_E_WORD   (-13)    /* the dictionary refused a word */
#define WL_E_PARSE  (-14)    /* not a move */

/* The loom's premiums: k knot, g gold, w warp, L loom, . plain. */
static const char WL_LAYOUT[WL_N][WL_N + 1] = {
  "g...w.w...g",
  ".L...k...L.",
  "..k.....k..",
  "...w...w...",
  "w...g.g...w",
  ".k...k...k.",
  "w...g.g...w",
  "...w...w...",
  "..k.....k..",
  ".L...k...L.",
  "g...w.w...g",
};

/*                                a  b  c  d  e   f  g  h  i  j  k  l  m */
static const uint8_t WL_VALUE[26] = { 1, 4, 3, 2, 1,  4, 3, 3, 1, 8, 5, 2, 3,
/*                                n  o  p  q  r   s  t  u  v  w  x  y  z */
                                      1, 1, 3, 9, 1,  1, 1, 2, 5, 4, 7, 4, 9 };
static const uint8_t WL_COUNT[26] = { 7, 2, 2, 3, 10, 1, 2, 2, 6, 1, 1, 4, 2,
                                      5, 6, 2, 1, 5,  4, 5, 3, 1, 1, 1, 2, 1 };

/* Asked about each word a weave makes (lower case); nonzero is a word. */
typedef int (*WlDict)(const char *word, void *ctx);

typedef struct {
  char     board[WL_N][WL_N];      /* 0, or 'a'..'z' */
  uint8_t  fresh[WL_N][WL_N];      /* the last weave's tiles */
  char     bag[WL_TILES];
  int      nbag;
  char     rack[2][WL_RACK + 1];   /* NUL-terminated */
  int      score[2];
  int      turn;                   /* whose move: 0 or 1 */
  int      turns;                  /* moves taken */
  int      stall;                  /* scoreless turns in a row */
  int      tiles;                  /* on the loom */
  int      over;                   /* WL_PLAYING, WL_ENDED, WL_RESIGNED */
  int      winner;                 /* when over: 0, 1, or -1 a draw */
  uint32_t rng;
  /* The last move, to say what happened. */
  int      last_player, last_kind, last_points, last_count;
  char     last_word[WL_N + 1];
} WlGame;

typedef struct {
  int8_t  r, c, dir;
  uint8_t len;
  int     points;
  char    text[WL_N + 1];
} WlWord;

/* What a weave would do, from wl_check. */
typedef struct {
  int     n;                       /* tiles laid */
  int8_t  pr[WL_RACK], pc[WL_RACK];
  char    pl[WL_RACK];
  int     nwords;
  WlWord  w[WL_WORDS_MAX];         /* w[0] the word along the line, if any */
  int     weave, bolt;             /* the bonuses it earned */
  int     points;                  /* everything */
  int     bad;                     /* WL_E_WORD: the word refused */
} WlPlay;

/* ---- little helpers -------------------------------------------------------- */

static WL_OPT int wl_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }

static WL_OPT uint32_t wl_rand(uint32_t *s) {
  uint32_t x = *s ? *s : WL_SEED0;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x;
  return x;
}

static WL_OPT int wl_value(char ch) {
  return ch >= 'a' && ch <= 'z' ? WL_VALUE[ch - 'a'] : 0;
}

static WL_OPT int wl_premium(int r, int c) {
  char p;
  if (r < 0 || c < 0 || r >= WL_N || c >= WL_N) return WL_PLAIN;
  p = WL_LAYOUT[r][c];
  return p == 'k' ? WL_KNOT : p == 'g' ? WL_GOLD : p == 'w' ? WL_WARP : p == 'L' ? WL_LOOM : WL_PLAIN;
}

static WL_OPT int wl_rack_value(const char *rack) {
  int v = 0;
  while (*rack) v += wl_value(*rack++);
  return v;
}

/* Take one `ch` out of `rack`; 0 when it has none. */
static WL_OPT int wl_rack_take(char *rack, char ch) {
  int i, n = wl_len(rack);
  for (i = 0; i < n; i++)
    if (rack[i] == ch) {
      for (; i < n; i++) rack[i] = rack[i + 1];
      return 1;
    }
  return 0;
}

static WL_OPT void wl_shuffle(WlGame *g) {
  int i;
  for (i = g->nbag - 1; i > 0; i--) {
    int j = (int)(wl_rand(&g->rng) % (uint32_t)(i + 1));
    char t = g->bag[i];
    g->bag[i] = g->bag[j];
    g->bag[j] = t;
  }
}

static WL_OPT void wl_draw(WlGame *g, int p) {
  int n = wl_len(g->rack[p]);
  while (n < WL_RACK && g->nbag > 0) g->rack[p][n++] = g->bag[--g->nbag];
  g->rack[p][n] = 0;
}

static WL_OPT void wl_zero(void *p, int n) {
  char *b = (char *)p;
  while (n-- > 0) *b++ = 0;
}

/* ---- a new game ------------------------------------------------------------- */

static WL_OPT void wl_new(WlGame *g, uint32_t seed) {
  int i, k;
  wl_zero(g, (int)sizeof *g);
  g->rng = seed ? seed : WL_SEED0;
  for (i = 0; i < 26; i++)
    for (k = 0; k < WL_COUNT[i]; k++) g->bag[g->nbag++] = (char)('a' + i);
  wl_shuffle(g);
  wl_draw(g, 0);
  wl_draw(g, 1);
  g->winner = -1;
  g->last_player = -1;
}

/* ---- checking a weave ----------------------------------------------------- */

/* The run of tiles through (r, c) along `dir` on `b`, into *w; its length. */
static WL_OPT int wl_word_at(char b[WL_N][WL_N], int r, int c, int dir, WlWord *w) {
  int dr = dir == WL_DOWN, dc = dir == WL_ACROSS, n = 0;
  while (r - dr >= 0 && c - dc >= 0 && b[r - dr][c - dc]) { r -= dr; c -= dc; }
  w->r = (int8_t)r;
  w->c = (int8_t)c;
  w->dir = (int8_t)dir;
  while (r < WL_N && c < WL_N && b[r][c]) {
    w->text[n++] = b[r][c];
    r += dr;
    c += dc;
  }
  w->text[n] = 0;
  w->len = (uint8_t)n;
  return n;
}

static WL_OPT int wl_score_word(char b[WL_N][WL_N], uint8_t nw[WL_N][WL_N], WlWord *w) {
  int i, sum = 0, mult = 1;
  int dr = w->dir == WL_DOWN, dc = w->dir == WL_ACROSS;
  for (i = 0; i < w->len; i++) {
    int r = w->r + dr * i, c = w->c + dc * i, v = wl_value(b[r][c]);
    if (nw[r][c]) {
      int p = wl_premium(r, c);
      if (p == WL_KNOT) v *= 2;
      else if (p == WL_GOLD) v *= 3;
      else if (p == WL_WARP) mult *= 2;
      else if (p == WL_LOOM) mult *= 3;
    }
    sum += v;
  }
  w->points = sum * mult;
  return w->points;
}

/* Would `player` laying `letters` from (r, c) along `dir` be a weave? WL_OK
 * and *out filled in, or why not. `dict` may be 0: every word stands. */
static WL_OPT int wl_check(const WlGame *g, int player, int r, int c, int dir, const char *letters,
                           WlDict dict, void *ctx, WlPlay *out) {
  char b[WL_N][WL_N], rack[WL_RACK + 1];
  uint8_t nw[WL_N][WL_N];
  int n = wl_len(letters), i, rr = r, cc = c, dr, dc, touch = 0, mid = 0;
  wl_zero(out, (int)sizeof *out);
  if (g->over) return WL_E_OVER;
  if (player != g->turn) return WL_E_TURN;
  if (n < 1) return WL_E_NONE;
  if (n > WL_RACK) return WL_E_RACK;
  if (dir != WL_ACROSS && dir != WL_DOWN) return WL_E_PARSE;
  if (r < 0 || c < 0 || r >= WL_N || c >= WL_N) return WL_E_OFF;
  if (g->board[r][c]) return WL_E_TAKEN;
  for (i = 0; i <= WL_RACK; i++) rack[i] = g->rack[player][i];
  for (i = 0; i < n; i++) {
    if (letters[i] < 'a' || letters[i] > 'z') return WL_E_PARSE;
    if (!wl_rack_take(rack, letters[i])) return WL_E_RACK;
  }
  for (rr = 0; rr < WL_N; rr++)
    for (cc = 0; cc < WL_N; cc++) { b[rr][cc] = g->board[rr][cc]; nw[rr][cc] = 0; }
  dr = dir == WL_DOWN;
  dc = dir == WL_ACROSS;
  rr = r;
  cc = c;
  for (i = 0; i < n; i++) {
    while (rr < WL_N && cc < WL_N && b[rr][cc]) { rr += dr; cc += dc; }
    if (rr >= WL_N || cc >= WL_N) return WL_E_OFF;
    b[rr][cc] = letters[i];
    nw[rr][cc] = 1;
    out->pr[i] = (int8_t)rr;
    out->pc[i] = (int8_t)cc;
    out->pl[i] = letters[i];
    if (rr == WL_MID && cc == WL_MID) mid = 1;
    rr += dr;
    cc += dc;
  }
  out->n = n;
  if (g->tiles == 0) {
    if (!mid) return WL_E_CENTRE;
    if (n < 2) return WL_E_SHORT;
  } else {
    for (i = 0; i < n && !touch; i++) {
      int pr = out->pr[i], pc = out->pc[i];
      touch = (pr > 0 && g->board[pr - 1][pc]) || (pr < WL_N - 1 && g->board[pr + 1][pc]) ||
              (pc > 0 && g->board[pr][pc - 1]) || (pc < WL_N - 1 && g->board[pr][pc + 1]);
    }
    if (!touch) return WL_E_ALONE;
  }
  /* The word along the line, then one across it through each new tile. */
  if (wl_word_at(b, out->pr[0], out->pc[0], dir, &out->w[0]) >= 2) out->nwords = 1;
  for (i = 0; i < n; i++)
    if (wl_word_at(b, out->pr[i], out->pc[i], 1 - dir, &out->w[out->nwords]) >= 2) out->nwords++;
  if (out->nwords == 0) return WL_E_ALONE;
  for (i = 0; i < out->nwords; i++) {
    out->points += wl_score_word(b, nw, &out->w[i]);
    if (dict && !dict(out->w[i].text, ctx)) { out->bad = i; return WL_E_WORD; }
  }
  if (out->nwords >= 3) { out->weave = WL_WEAVE; out->points += WL_WEAVE; }
  if (n == WL_RACK) { out->bolt = WL_BOLT; out->points += WL_BOLT; }
  return WL_OK;
}

/* ---- moves ------------------------------------------------------------------ */

static WL_OPT void wl_finish(WlGame *g, int out_player) {
  int p;
  if (out_player >= 0) {
    g->score[out_player] += WL_TIEOFF;
    g->score[1 - out_player] -= wl_rack_value(g->rack[1 - out_player]);
  } else {
    for (p = 0; p < 2; p++) g->score[p] -= wl_rack_value(g->rack[p]);
  }
  g->over = WL_ENDED;
  g->winner = g->score[0] > g->score[1] ? 0 : g->score[1] > g->score[0] ? 1 : -1;
}

static WL_OPT void wl_note(WlGame *g, int player, int kind, int points, int count, const char *word) {
  int i = 0;
  g->last_player = player;
  g->last_kind = kind;
  g->last_points = points;
  g->last_count = count;
  if (word) for (; word[i] && i < WL_N; i++) g->last_word[i] = word[i];
  g->last_word[i] = 0;
}

static WL_OPT void wl_scoreless(WlGame *g) {
  g->stall++;
  g->turns++;
  g->turn = 1 - g->turn;
  if (g->stall >= WL_STALL) wl_finish(g, -1);
}

/* A weave: its points, or an error (nothing changes). */
static WL_OPT int wl_play(WlGame *g, int player, int r, int c, int dir, const char *letters,
                          WlDict dict, void *ctx) {
  WlPlay pl;
  int i, rr, cc, e = wl_check(g, player, r, c, dir, letters, dict, ctx, &pl);
  if (e != WL_OK) return e;
  for (rr = 0; rr < WL_N; rr++) for (cc = 0; cc < WL_N; cc++) g->fresh[rr][cc] = 0;
  for (i = 0; i < pl.n; i++) {
    g->board[pl.pr[i]][pl.pc[i]] = pl.pl[i];
    g->fresh[pl.pr[i]][pl.pc[i]] = 1;
    wl_rack_take(g->rack[player], pl.pl[i]);
  }
  g->tiles += pl.n;
  g->score[player] += pl.points;
  g->stall = 0;
  g->turns++;
  wl_note(g, player, WL_K_PLAY, pl.points, pl.n, pl.nwords ? pl.w[0].text : "");
  wl_draw(g, player);
  g->turn = 1 - player;
  if (g->rack[player][0] == 0 && g->nbag == 0) wl_finish(g, player);
  return pl.points;
}

static WL_OPT int wl_swap(WlGame *g, int player, const char *letters) {
  char rack[WL_RACK + 1];
  int n = wl_len(letters), i;
  if (g->over) return WL_E_OVER;
  if (player != g->turn) return WL_E_TURN;
  if (n < 1) return WL_E_NONE;
  if (n > WL_RACK) return WL_E_RACK;
  if (g->nbag < n) return WL_E_BAG;
  for (i = 0; i <= WL_RACK; i++) rack[i] = g->rack[player][i];
  for (i = 0; i < n; i++) if (!wl_rack_take(rack, letters[i])) return WL_E_RACK;
  for (i = 0; i <= WL_RACK; i++) g->rack[player][i] = rack[i];
  wl_draw(g, player);
  for (i = 0; i < n; i++) g->bag[g->nbag++] = letters[i];
  wl_shuffle(g);
  wl_note(g, player, WL_K_SWAP, 0, n, 0);
  wl_scoreless(g);
  return WL_OK;
}

static WL_OPT int wl_pass(WlGame *g, int player) {
  if (g->over) return WL_E_OVER;
  if (player != g->turn) return WL_E_TURN;
  wl_note(g, player, WL_K_PASS, 0, 0, 0);
  wl_scoreless(g);
  return WL_OK;
}

static WL_OPT int wl_resign(WlGame *g, int player) {
  if (g->over) return WL_E_OVER;
  if (player != 0 && player != 1) return WL_E_TURN;
  g->over = WL_RESIGNED;
  g->winner = 1 - player;
  g->turns++;
  wl_note(g, player, WL_K_RESIGN, 0, 0, 0);
  return WL_OK;
}

/* ---- the wire --------------------------------------------------------------- */

/* The next space-separated word of *p into out (at most n-1); its length. */
static WL_OPT int wl_word(const char **p, char *out, int n) {
  const char *s = *p;
  int k = 0;
  while (*s == ' ') s++;
  while (*s && *s != ' ' && *s != '\n' && *s != '\r' && *s != '\t') {
    if (k < n - 1) out[k] = *s;
    k++;
    s++;
  }
  out[k < n - 1 ? k : n - 1] = 0;
  *p = s;
  return k;
}

static WL_OPT int wl_num(const char *s) {
  int v = 0;
  if (!*s) return -1;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || v > 1000) return -1;
    v = v * 10 + (*s - '0');
  }
  return v;
}

/* One move of the wire format, for `player`. WL_OK, or why it is refused
 * (and nothing changed). */
static WL_OPT int wl_apply(WlGame *g, int player, const char *line, WlDict dict, void *ctx) {
  char verb[8], a[8], b[8], d[8], letters[16];
  const char *p = line;
  if (wl_word(&p, verb, sizeof verb) >= (int)sizeof verb) return WL_E_PARSE;
  if (str_same(verb, "pass")) return wl_pass(g, player);
  if (str_same(verb, "resign")) return wl_resign(g, player);
  if (str_same(verb, "swap")) {
    if (wl_word(&p, letters, sizeof letters) >= (int)sizeof letters) return WL_E_RACK;
    return wl_swap(g, player, letters);
  }
  if (str_same(verb, "play")) {
    int r, c, e;
    wl_word(&p, a, sizeof a);
    wl_word(&p, b, sizeof b);
    wl_word(&p, d, sizeof d);
    if (wl_word(&p, letters, sizeof letters) >= (int)sizeof letters) return WL_E_RACK;
    r = wl_num(a);
    c = wl_num(b);
    if (r < 0 || c < 0 || (!str_same(d, "a") && !str_same(d, "d"))) return WL_E_PARSE;
    e = wl_play(g, player, r, c, d[0] == 'd' ? WL_DOWN : WL_ACROSS, letters, dict, ctx);
    return e < 0 ? e : WL_OK;
  }
  return WL_E_PARSE;
}

static WL_OPT int wl_put_uint(char *out, int at, int n, uint32_t v) {
  char t[12];
  int k = 0;
  do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (k > 0 && at < n - 1) out[at++] = t[--k];
  return at;
}

static WL_OPT int wl_put_str(char *out, int at, int n, const char *s) {
  while (*s && at < n - 1) out[at++] = *s++;
  return at;
}

/* "play R C a|d LETTERS" into out; its length. */
static WL_OPT int wl_fmt_play(char *out, int n, int r, int c, int dir, const char *letters) {
  int at = wl_put_str(out, 0, n, "play ");
  at = wl_put_uint(out, at, n, (uint32_t)r);
  at = wl_put_str(out, at, n, " ");
  at = wl_put_uint(out, at, n, (uint32_t)c);
  at = wl_put_str(out, at, n, dir == WL_DOWN ? " d " : " a ");
  at = wl_put_str(out, at, n, letters);
  out[at] = 0;
  return at;
}

/* The room's state for a new game. */
static WL_OPT int wl_state_format(char *out, int n, uint32_t seed) {
  int at = wl_put_str(out, 0, n, "wordloom ");
  at = wl_put_uint(out, at, n, WL_VERSION);
  at = wl_put_str(out, at, n, "\nseed ");
  at = wl_put_uint(out, at, n, seed);
  at = wl_put_str(out, at, n, "\n");
  out[at] = 0;
  return at;
}

/* The version a state names (and its seed), or -1 when it is not ours. */
static WL_OPT int wl_state_parse(const char *s, uint32_t *seed) {
  int version = -1;
  *seed = 0;
  while (*s) {
    if (str_starts(s, "wordloom ")) { const char *q = s + 9; version = (int)str_uint(&q); }
    else if (str_starts(s, "seed ")) { const char *q = s + 5; *seed = str_uint(&q); }
    while (*s && *s != '\n') s++;
    if (*s) s++;
  }
  return version;
}

/* A game from its seed and a log of "SEQ <tab> FROM <tab> MOVE" lines, the
 * players named p0 (moves first) and p1. How many moves were skipped: from
 * neither player, out of turn, or refused by the rules. */
static WL_OPT int wl_replay(WlGame *g, uint32_t seed, const char *p0, const char *p1,
                            const char *log) {
  char from[32], move[64];
  int skipped = 0;
  wl_new(g, seed);
  while (log && *log) {
    int who;
    tsv_field(log, 1, from, sizeof from);
    tsv_field(log, 2, move, sizeof move);
    who = str_same(from, p0) ? 0 : str_same(from, p1) ? 1 : -1;
    if (who < 0 || wl_apply(g, who, move, 0, 0) != WL_OK) skipped++;
    log = tsv_next_line(log);
  }
  return skipped;
}

/* ---- from tiles on a screen to a move -------------------------------------- */

/* `n` tiles a person put down at (r[i], c[i]) -- any order -- as the wire's
 * first cell, direction and letters. `pref` is the direction for a single
 * tile with neighbours both ways (either scores the same). WL_OK, or
 * WL_E_NONE, WL_E_LINE, WL_E_GAP. */
static WL_OPT int wl_from_cells(const WlGame *g, const int8_t *r, const int8_t *c, const char *l,
                                int n, int pref, int *R, int *C, int *D, char *letters) {
  int i, j, dir, lo = 99, hi = -1, k = 0, same_r = 1, same_c = 1;
  int ord[WL_RACK];
  if (n < 1) return WL_E_NONE;
  if (n > WL_RACK) return WL_E_RACK;
  for (i = 1; i < n; i++) {
    if (r[i] != r[0]) same_r = 0;
    if (c[i] != c[0]) same_c = 0;
  }
  if (n == 1) {
    int h = (c[0] > 0 && g->board[r[0]][c[0] - 1]) || (c[0] < WL_N - 1 && g->board[r[0]][c[0] + 1]);
    int v = (r[0] > 0 && g->board[r[0] - 1][c[0]]) || (r[0] < WL_N - 1 && g->board[r[0] + 1][c[0]]);
    dir = h && v ? pref : h ? WL_ACROSS : v ? WL_DOWN : pref;
  } else if (same_r) dir = WL_ACROSS;
  else if (same_c) dir = WL_DOWN;
  else return WL_E_LINE;
  for (i = 0; i < n; i++) ord[i] = i;
  for (i = 1; i < n; i++)                 /* along the line */
    for (j = i; j > 0; j--) {
      int a = ord[j - 1], b = ord[j];
      int pa = dir == WL_ACROSS ? c[a] : r[a], pb = dir == WL_ACROSS ? c[b] : r[b];
      if (pa <= pb) break;
      ord[j - 1] = b;
      ord[j] = a;
    }
  lo = dir == WL_ACROSS ? c[ord[0]] : r[ord[0]];
  hi = dir == WL_ACROSS ? c[ord[n - 1]] : r[ord[n - 1]];
  for (i = lo; i <= hi; i++) {           /* every cell between is a tile */
    int rr = dir == WL_ACROSS ? r[0] : i, cc = dir == WL_ACROSS ? i : c[0], mine = 0;
    for (j = 0; j < n; j++) if (r[j] == rr && c[j] == cc) mine = 1;
    if (!mine && !g->board[rr][cc]) return WL_E_GAP;
  }
  for (i = 0; i < n; i++) letters[k++] = l[ord[i]];
  letters[k] = 0;
  *R = r[ord[0]];
  *C = c[ord[0]];
  *D = dir;
  return WL_OK;
}

/* Why a move was refused, in a few words. */
static WL_OPT const char *wl_why(int e) {
  switch (e) {
  case WL_E_OVER: return "the game is over";
  case WL_E_TURN: return "not your turn";
  case WL_E_RACK: return "not in your rack";
  case WL_E_OFF: return "runs off the loom";
  case WL_E_TAKEN: return "that cell is taken";
  case WL_E_CENTRE: return "first word: centre";
  case WL_E_SHORT: return "first word: 2+ tiles";
  case WL_E_ALONE: return "must touch a tile";
  case WL_E_LINE: return "one row or column";
  case WL_E_GAP: return "no gaps";
  case WL_E_NONE: return "lay some tiles";
  case WL_E_BAG: return "the bag is too low";
  case WL_E_WORD: return "not a word";
  case WL_E_PARSE: return "not a move";
  default: return "";
  }
}

#endif /* CARDOS_WORDLOOM_H */
