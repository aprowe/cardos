/* Wordle with friends: what a finished daily publishes for them to see,
 * and reading theirs back (apps/social.h, server/friends.py).
 *
 * The owner's rule: not social media. Nothing is posted anywhere -- a
 * result is kept as your pub/wordle value of `day` in the server's store,
 * where only friends can read it, and Wordle shows theirs on its own
 * screens. What reaches Chat is only what `s` sends.
 *
 * Your value of `day`, a few lines of text -- the pattern and never the
 * letters, so a friend who has not played yet sees no spoiler:
 *
 *   day 299          the daily's day number (wd_day; puzzle #300)
 *   result 3         guesses it took, or X for six misses
 *   rows yy... .g.yg ggggg      each guess as its colours: g right place,
 *                               y elsewhere in the word, . not in it
 *   streak 5         the daily streak after it
 *
 * Portable C, host-tested in apps/wordle/test/test_wordle_friends.c. */
#ifndef CARDOS_WDFRIENDS_H
#define CARDOS_WDFRIENDS_H

#include "apps/wordle/wordle.h"

#define WDF_MAX 8                 /* friends shown */
#define WDF_X   7                 /* `result` of six misses */

typedef struct {
  char    name[25];               /* the account name */
  char    display[17];            /* the name they chose */
  int32_t day;
  int     result;                 /* 1-6, WDF_X, or 0 for none read */
  int     nrows;
  uint8_t rows[WD_TRIES][WD_LEN]; /* WD_CORRECT, WD_PRESENT, WD_ABSENT */
  int     streak;
} WdFriend;

/* A finished daily, as the text above; its length, -1 if buf is too small
 * or the game is not over. */
static WD_OPT int wdf_format(const WdGame *g, int32_t day, int streak, char *buf, int n) {
  int at = 0, i, j;
  if (n > 0) buf[0] = 0;
  if (g->over == WD_PLAYING) return -1;
  at = wd_cat(buf, n, at, "day ");      at = wd_cat_num(buf, n, at, day);
  at = wd_cat(buf, n, at, "\nresult ");
  at = g->over == WD_WON ? wd_cat_num(buf, n, at, g->n) : wd_cat(buf, n, at, "X");
  at = wd_cat(buf, n, at, "\nrows");
  for (i = 0; i < g->n && i < WD_TRIES; i++) {
    char r[WD_LEN + 2];
    r[0] = ' ';
    for (j = 0; j < WD_LEN; j++)
      r[j + 1] = g->score[i][j] == WD_CORRECT ? 'g' : g->score[i][j] == WD_PRESENT ? 'y' : '.';
    r[WD_LEN + 1] = 0;
    at = wd_cat(buf, n, at, r);
  }
  at = wd_cat(buf, n, at, "\nstreak "); at = wd_cat_num(buf, n, at, streak);
  at = wd_cat(buf, n, at, "\n");
  return at;
}

/* One friend's value (`len` bytes, not terminated) into f; 1 if it said a
 * day and a result. */
static WD_OPT int wdf_parse(const char *body, int len, WdFriend *f) {
  char t[160];
  const char *p = t;
  int i;
  if (len < 0) len = 0;
  if (len > (int)sizeof t - 1) len = (int)sizeof t - 1;
  for (i = 0; i < len; i++) t[i] = body[i];
  t[len] = 0;
  f->day = WD_NO_DAY;
  f->result = 0;
  f->nrows = 0;
  f->streak = 0;
  while (*p) {
    const char *r;
    int32_t v;
    if (wd_is(p, "day", &r) && wd_num(&r, &v)) f->day = v;
    else if (wd_is(p, "result", &r)) {
      while (*r == ' ') r++;
      if (*r == 'X') f->result = WDF_X;
      else if (wd_num(&r, &v) && v >= 1 && v <= WD_TRIES) f->result = (int)v;
    } else if (wd_is(p, "streak", &r) && wd_num(&r, &v)) f->streak = (int)v;
    else if (wd_is(p, "rows", &r)) {
      while (f->nrows < WD_TRIES) {
        int j;
        while (*r == ' ') r++;
        for (j = 0; j < WD_LEN && (r[j] == 'g' || r[j] == 'y' || r[j] == '.'); j++)
          f->rows[f->nrows][j] = r[j] == 'g' ? WD_CORRECT : r[j] == 'y' ? WD_PRESENT : WD_ABSENT;
        if (j != WD_LEN) break;
        r += WD_LEN;
        f->nrows++;
      }
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
  return f->day != WD_NO_DAY && f->result != 0;
}

/* "3/6" or "X/6". */
static WD_OPT void wdf_score(const WdFriend *f, char out[4]) {
  out[0] = f->result == WDF_X ? 'X' : (char)('0' + f->result);
  out[1] = '/';
  out[2] = (char)('0' + WD_TRIES);
  out[3] = 0;
}

#endif /* CARDOS_WDFRIENDS_H */
