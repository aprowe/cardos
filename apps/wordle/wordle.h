/* Wordle's rules, its word list, the daily puzzle and the stats file -- the
 * parts that can be wrong in a way a person would argue about, kept apart
 * from the screen so the host suite can pin them (test/test_wordle.c).
 *
 * Portable C with no libc: the app links none, and this compiles on the PC
 * for the tests as it is.
 *
 * The word list (apps/wordle/wordle_words.h, made by tools/make_wordle.py)
 * is one sorted list: ENABLE2K's five-letter words, which is what a guess
 * may be, with a bit on each that is also an answer. A word's first two
 * letters pick a bucket; its last three are 15 bits of a uint16_t. Two
 * bytes a word, about 18 KB for 8,600, all constants.
 *
 * Scoring follows the game everyone knows, duplicates included: greens
 * first, then left to right a letter is yellow only while the answer has
 * one of it not yet matched, so guessing two Es at an answer with one E
 * shows one coloured E, never two.
 */
#ifndef CARDOS_WORDLE_H
#define CARDOS_WORDLE_H

#include <stdint.h>
#include "apps/wordle/wordle_words.h"

#if defined(__GNUC__)
#define WD_OPT __attribute__((unused))
#else
#define WD_OPT
#endif

#define WD_LEN    5
#define WD_TRIES  6

/* A tile's or a key's state; the order matters -- a key keeps the highest. */
enum { WD_EMPTY = 0, WD_ABSENT, WD_PRESENT, WD_CORRECT };

/* ---- scoring ------------------------------------------------------------- */

/* `guess` and `answer`: five lowercase letters. */
static WD_OPT void wd_score(const char *guess, const char *answer, uint8_t out[WD_LEN]) {
  uint8_t left[26];
  int i;
  for (i = 0; i < 26; i++) left[i] = 0;
  for (i = 0; i < WD_LEN; i++) {
    if (guess[i] == answer[i]) out[i] = WD_CORRECT;
    else { out[i] = WD_ABSENT; left[answer[i] - 'a']++; }
  }
  for (i = 0; i < WD_LEN; i++) {
    int c = guess[i] - 'a';
    if (out[i] == WD_CORRECT) continue;
    if (left[c]) { out[i] = WD_PRESENT; left[c]--; }
  }
}

/* The on-screen keyboard: each letter keeps the best it has been shown as. */
static WD_OPT void wd_keys(uint8_t keys[26], const char *guess, const uint8_t sc[WD_LEN]) {
  int i;
  for (i = 0; i < WD_LEN; i++) {
    int c = guess[i] - 'a';
    if (sc[i] > keys[c]) keys[c] = sc[i];
  }
}

/* ---- the word list --------------------------------------------------------- */

static WD_OPT int wd_is_word_shape(const char *w) {
  int i;
  for (i = 0; i < WD_LEN; i++) if (w[i] < 'a' || w[i] > 'z') return 0;
  return w[WD_LEN] == 0;
}

/* Where `w` is in the list, or -1. */
static WD_OPT int wd_find(const char *w) {
  int b, lo, hi;
  uint16_t t;
  if (!wd_is_word_shape(w)) return -1;
  b = (w[0] - 'a') * 26 + (w[1] - 'a');
  t = (uint16_t)(((w[2] - 'a') << 10) | ((w[3] - 'a') << 5) | (w[4] - 'a'));
  lo = WD_BUCKET[b];
  hi = WD_BUCKET[b + 1] - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint16_t v = (uint16_t)(WD_TAIL[mid] & 0x7FFF);
    if (v == t) return mid;
    if (v < t) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

static WD_OPT int wd_valid(const char *w) { return wd_find(w) >= 0; }

static WD_OPT int wd_is_answer(const char *w) {
  int i = wd_find(w);
  return i >= 0 && (WD_TAIL[i] & 0x8000) != 0;
}

/* The word at index `i` of the list into `out` (six bytes). */
static WD_OPT void wd_word(int i, char out[WD_LEN + 1]) {
  int lo = 0, hi = 26 * 26 - 1;
  uint16_t t = WD_TAIL[i];
  /* The last bucket starting at or before i. */
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    if (WD_BUCKET[mid] <= i) lo = mid;
    else hi = mid - 1;
  }
  /* Empty buckets share a start with the next; the one i is in is the last
   * of those, which is where the search lands. */
  out[0] = (char)('a' + lo / 26);
  out[1] = (char)('a' + lo % 26);
  out[2] = (char)('a' + ((t >> 10) & 31));
  out[3] = (char)('a' + ((t >> 5) & 31));
  out[4] = (char)('a' + (t & 31));
  out[5] = 0;
}

/* The k-th answer (from 0, in list order) into `out`; 0, or -1 out of range.
 * A walk of the list, a few thousand steps: once a game, never a frame. */
static WD_OPT int wd_answer(int k, char out[WD_LEN + 1]) {
  int i;
  if (k < 0 || k >= WD_NANSWERS) return -1;
  for (i = 0; i < WD_NWORDS; i++)
    if ((WD_TAIL[i] & 0x8000) && k-- == 0) { wd_word(i, out); return 0; }
  return -1;
}

/* ---- the daily puzzle ------------------------------------------------------ */

/* Days from 2026-01-01 (day 0, puzzle #1) to a date; negative before it. */
static WD_OPT int32_t wd_day(int y, int m, int d) {
  /* Days from the civil date, after Howard Hinnant's days_from_civil. */
  int32_t era, yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468 - 20454;   /* 20454: 2026-01-01 since 1970 */
}

/* Which answer a day's puzzle is: every answer once, scrambled, then again. */
static WD_OPT int wd_daily_index(int32_t day) {
  uint32_t d = (uint32_t)(((day % WD_NANSWERS) + WD_NANSWERS) % WD_NANSWERS);
  return (int)((d * WD_DAILY_MUL + WD_DAILY_ADD) % WD_NANSWERS);
}

/* ---- a game ---------------------------------------------------------------- */

enum { WD_PLAYING = 0, WD_WON, WD_LOST };
enum { WD_OK = 0, WD_SHORT = -1, WD_UNKNOWN = -2, WD_OVER = -3 };

typedef struct {
  char    answer[WD_LEN + 1];
  char    guess[WD_TRIES][WD_LEN + 1];   /* the guesses made */
  uint8_t score[WD_TRIES][WD_LEN];
  int     n;                             /* how many */
  char    cur[WD_LEN + 1];               /* the one being typed */
  int     len;
  uint8_t keys[26];
  int     over;                          /* WD_PLAYING, WD_WON, WD_LOST */
} WdGame;

static WD_OPT void wd_start(WdGame *g, const char *answer) {
  int i, j;
  for (i = 0; i < WD_TRIES; i++) {
    for (j = 0; j <= WD_LEN; j++) g->guess[i][j] = 0;
    for (j = 0; j < WD_LEN; j++) g->score[i][j] = WD_EMPTY;
  }
  for (i = 0; i < 26; i++) g->keys[i] = WD_EMPTY;
  for (i = 0; i <= WD_LEN; i++) { g->answer[i] = answer[i]; g->cur[i] = 0; }
  g->n = g->len = 0;
  g->over = WD_PLAYING;
}

/* A typed key: a letter, either case. 1 if it went in. */
static WD_OPT int wd_type(WdGame *g, int c) {
  if (c >= 'A' && c <= 'Z') c += 32;
  if (c < 'a' || c > 'z' || g->over != WD_PLAYING || g->len >= WD_LEN) return 0;
  g->cur[g->len++] = (char)c;
  g->cur[g->len] = 0;
  return 1;
}

static WD_OPT int wd_back(WdGame *g) {
  if (g->over != WD_PLAYING || g->len == 0) return 0;
  g->cur[--g->len] = 0;
  return 1;
}

/* Play what has been typed. WD_OK, or why not -- and then it stays typed. */
static WD_OPT int wd_submit(WdGame *g) {
  int i, greens = 0;
  if (g->over != WD_PLAYING) return WD_OVER;
  if (g->len < WD_LEN) return WD_SHORT;
  if (!wd_valid(g->cur)) return WD_UNKNOWN;
  for (i = 0; i <= WD_LEN; i++) g->guess[g->n][i] = g->cur[i];
  wd_score(g->cur, g->answer, g->score[g->n]);
  wd_keys(g->keys, g->cur, g->score[g->n]);
  for (i = 0; i < WD_LEN; i++) greens += g->score[g->n][i] == WD_CORRECT;
  g->n++;
  g->len = 0;
  g->cur[0] = 0;
  if (greens == WD_LEN) g->over = WD_WON;
  else if (g->n >= WD_TRIES) g->over = WD_LOST;
  return WD_OK;
}

/* Type a whole word and play it: a saved game coming back, and the tests. */
static WD_OPT int wd_guess(WdGame *g, const char *w) {
  int i;
  if (g->over != WD_PLAYING) return WD_OVER;
  while (g->len) wd_back(g);
  for (i = 0; w[i] && i < WD_LEN + 1; i++)
    if (!wd_type(g, w[i])) return WD_UNKNOWN;
  return wd_submit(g);
}

/* ---- the stats ------------------------------------------------------------- *
 *
 * Kept for the daily puzzle only -- practice is practice -- in a few lines of
 * text (/var/wordle/stats.txt):
 *
 *   played 12          won 10          streak 3          best 5
 *   dist 0 1 4 3 2 0   guesses taken, for the games won
 *   lastwin 288        the day of the last daily won, which a streak needs
 *   lastdone 288       the last daily finished, so it is counted once
 *   day 289 crane ...  today's daily so far, so leaving the app keeps it
 */
#define WD_NO_DAY (-100000)

typedef struct {
  int     played, won, streak, best;
  int     dist[WD_TRIES];
  int32_t last_win, last_done;
  int32_t day;                           /* the daily these guesses are for */
  int     nguess;
  char    guess[WD_TRIES][WD_LEN + 1];
} WdStats;

static WD_OPT void wd_stats_init(WdStats *s) {
  int i, j;
  s->played = s->won = s->streak = s->best = 0;
  for (i = 0; i < WD_TRIES; i++) {
    s->dist[i] = 0;
    for (j = 0; j <= WD_LEN; j++) s->guess[i][j] = 0;
  }
  s->last_win = s->last_done = s->day = WD_NO_DAY;
  s->nguess = 0;
}

/* A daily finished: won in `tries`, or lost. Twice for one day counts once. */
static WD_OPT void wd_stats_finish(WdStats *s, int32_t day, int won, int tries) {
  if (s->last_done == day) return;
  s->last_done = day;
  s->played++;
  if (won) {
    s->won++;
    if (tries >= 1 && tries <= WD_TRIES) s->dist[tries - 1]++;
    s->streak = s->last_win == day - 1 ? s->streak + 1 : 1;
    if (s->streak > s->best) s->best = s->streak;
    s->last_win = day;
  } else {
    s->streak = 0;
  }
}

/* The streak as it stands today: one not won yesterday or today is over. */
static WD_OPT int wd_stats_streak(const WdStats *s, int32_t today) {
  return (s->last_win == today || s->last_win == today - 1) ? s->streak : 0;
}

/* Numbers and words off a line; *p moves past them. */
static WD_OPT int wd_num(const char **p, int32_t *v) {
  const char *s = *p;
  int neg = 0;
  int32_t n = 0;
  while (*s == ' ') s++;
  if (*s == '-') { neg = 1; s++; }
  if (*s < '0' || *s > '9') { *p = s; return 0; }
  while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
  *v = neg ? -n : n;
  *p = s;
  return 1;
}

static WD_OPT int wd_is(const char *line, const char *key, const char **rest) {
  while (*key) if (*line++ != *key++) return 0;
  if (*line != ' ') return 0;
  *rest = line;
  return 1;
}

static WD_OPT void wd_stats_parse(WdStats *s, const char *text) {
  const char *p = text;
  wd_stats_init(s);
  while (*p) {
    const char *r;
    int32_t v;
    if (wd_is(p, "played", &r) && wd_num(&r, &v)) s->played = (int)v;
    else if (wd_is(p, "won", &r) && wd_num(&r, &v)) s->won = (int)v;
    else if (wd_is(p, "streak", &r) && wd_num(&r, &v)) s->streak = (int)v;
    else if (wd_is(p, "best", &r) && wd_num(&r, &v)) s->best = (int)v;
    else if (wd_is(p, "lastwin", &r) && wd_num(&r, &v)) s->last_win = v;
    else if (wd_is(p, "lastdone", &r) && wd_num(&r, &v)) s->last_done = v;
    else if (wd_is(p, "dist", &r)) {
      int i;
      for (i = 0; i < WD_TRIES && wd_num(&r, &v); i++) s->dist[i] = (int)v;
    } else if (wd_is(p, "day", &r) && wd_num(&r, &v)) {
      s->day = v;
      s->nguess = 0;
      while (s->nguess < WD_TRIES) {
        char w[WD_LEN + 1];
        int i = 0;
        while (*r == ' ') r++;
        while (i < WD_LEN && *r >= 'a' && *r <= 'z') w[i++] = *r++;
        w[i] = 0;
        if (i != WD_LEN || (*r != ' ' && *r != '\r' && *r != '\n' && *r)) break;
        for (i = 0; i <= WD_LEN; i++) s->guess[s->nguess][i] = w[i];
        s->nguess++;
      }
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
}

/* Append to buf; -1 once it does not fit. */
static WD_OPT int wd_cat(char *buf, int n, int at, const char *s) {
  if (at < 0) return -1;
  while (*s) { if (at >= n - 1) return -1; buf[at++] = *s++; }
  buf[at] = 0;
  return at;
}

static WD_OPT int wd_cat_num(char *buf, int n, int at, int32_t v) {
  char t[16];
  int i = 15;
  uint32_t u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
  t[i] = 0;
  do { t[--i] = (char)('0' + u % 10); u /= 10; } while (u);
  if (v < 0) t[--i] = '-';
  return wd_cat(buf, n, at, t + i);
}

/* The file's text into buf; its length, or -1 if buf is too small. */
static WD_OPT int wd_stats_format(const WdStats *s, char *buf, int n) {
  int at = 0, i;
  if (n > 0) buf[0] = 0;
  at = wd_cat(buf, n, at, "played ");   at = wd_cat_num(buf, n, at, s->played);
  at = wd_cat(buf, n, at, "\nwon ");    at = wd_cat_num(buf, n, at, s->won);
  at = wd_cat(buf, n, at, "\nstreak "); at = wd_cat_num(buf, n, at, s->streak);
  at = wd_cat(buf, n, at, "\nbest ");   at = wd_cat_num(buf, n, at, s->best);
  at = wd_cat(buf, n, at, "\ndist");
  for (i = 0; i < WD_TRIES; i++) { at = wd_cat(buf, n, at, " "); at = wd_cat_num(buf, n, at, s->dist[i]); }
  at = wd_cat(buf, n, at, "\nlastwin ");  at = wd_cat_num(buf, n, at, s->last_win);
  at = wd_cat(buf, n, at, "\nlastdone "); at = wd_cat_num(buf, n, at, s->last_done);
  at = wd_cat(buf, n, at, "\nday ");      at = wd_cat_num(buf, n, at, s->day);
  for (i = 0; i < s->nguess && i < WD_TRIES; i++) {
    at = wd_cat(buf, n, at, " ");
    at = wd_cat(buf, n, at, s->guess[i]);
  }
  at = wd_cat(buf, n, at, "\n");
  return at;
}

#endif /* CARDOS_WORDLE_H */
