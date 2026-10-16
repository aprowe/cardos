/* Hangman's rules: the secret, the guesses, the figure's progress and how a
 * phrase is laid out on two lines. Portable, no libc, host-tested in
 * test/test_hangman.c; apps/hangman/hangman.c is the screen around it.
 *
 * A secret is letters and single spaces -- "BIG RED BUS" -- upper case,
 * at most HM_MAX characters, and it has to fit HM_LINES lines of HM_COLS
 * without breaking a word, because a word split over two lines reads as
 * two words to the person guessing.
 */
#ifndef CARDOS_HANGMAN_H
#define CARDOS_HANGMAN_H

#include <stdint.h>

#if defined(__GNUC__)
#define HM_OPT __attribute__((unused))
#else
#define HM_OPT
#endif

#define HM_MAX     28      /* characters in a secret, spaces included */
#define HM_COLS    14      /* characters a line of the shown word */
#define HM_LINES   2
#define HM_MISSES  6       /* head, body, two arms, two legs */

enum { HM_PLAYING = 0, HM_WON, HM_LOST };

/* What a guess did. */
#define HM_BAD    (-1)     /* not a letter, or the game is over */
#define HM_AGAIN  (-2)     /* that letter was tried already */
#define HM_MISS   0        /* otherwise: how many places it filled */

/* Why a secret was refused. */
#define HM_EMPTY   (-1)    /* no letters at all */
#define HM_LONG    (-2)    /* more than HM_MAX, or will not fit the lines */

typedef struct {
  char     word[HM_MAX + 1];      /* the secret, cleaned */
  int      len;
  uint32_t tried;                 /* a bit per letter, A = bit 0 */
  char     order[27];             /* the letters tried, in the order tried */
  int      ntried;
  int      misses;
  int      state;                 /* HM_PLAYING, HM_WON, HM_LOST */
} Hangman;

static HM_OPT int hm_upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static HM_OPT int hm_letter(int c) { c = hm_upper(c); return c >= 'A' && c <= 'Z'; }

/* `in` as a secret into `out` (HM_MAX + 1): letters upper-cased, anything
 * else a single space between words, none at either end. Letters in it, or
 * HM_LONG when the cleaned text would be longer than HM_MAX. */
static HM_OPT int hm_clean(const char *in, char *out) {
  int n = 0, letters = 0, gap = 0;
  for (; *in; in++) {
    if (hm_letter(*in)) {
      if (gap && n) { if (n >= HM_MAX) return HM_LONG; out[n++] = ' '; }
      gap = 0;
      if (n >= HM_MAX) return HM_LONG;
      out[n++] = (char)hm_upper(*in);
      letters++;
    } else {
      gap = 1;
    }
  }
  out[n] = 0;
  return letters;
}

/* The secret broken into lines at spaces: starts[i], lens[i] for each.
 * The number of lines, or -1 if a word is longer than `cols` or it needs
 * more than `maxl` lines. */
static HM_OPT int hm_wrap(const char *w, int len, int cols, int *starts, int *lens, int maxl) {
  int at = 0, lines = 0;
  while (at < len) {
    int end = at, last_fit = -1;
    /* The longest run of whole words from `at` that fits `cols`. */
    while (end <= len) {
      if (end == len || w[end] == ' ') {
        if (end - at <= cols) last_fit = end;
        else break;
      }
      end++;
    }
    if (last_fit < 0 || lines >= maxl) return -1;
    starts[lines] = at;
    lens[lines] = last_fit - at;
    lines++;
    at = last_fit;
    while (at < len && w[at] == ' ') at++;
  }
  return lines;
}

/* Start a round with `secret`, as typed. 0, HM_EMPTY or HM_LONG. */
static HM_OPT int hm_start(Hangman *h, const char *secret) {
  int starts[HM_LINES + 1], lens[HM_LINES + 1], letters, i;
  letters = hm_clean(secret, h->word);
  if (letters == HM_LONG) { h->word[0] = 0; return HM_LONG; }
  if (letters <= 0) { h->word[0] = 0; return HM_EMPTY; }
  for (h->len = 0; h->word[h->len]; h->len++) {}
  if (hm_wrap(h->word, h->len, HM_COLS, starts, lens, HM_LINES) < 0) return HM_LONG;
  h->tried = 0;
  for (i = 0; i < 27; i++) h->order[i] = 0;
  h->ntried = 0;
  h->misses = 0;
  h->state = HM_PLAYING;
  return 0;
}

static HM_OPT int hm_was_tried(const Hangman *h, int c) {
  c = hm_upper(c);
  return c >= 'A' && c <= 'Z' && (h->tried >> (c - 'A')) & 1u;
}

/* Every letter of the secret has been tried. */
static HM_OPT int hm_solved(const Hangman *h) {
  int i;
  for (i = 0; i < h->len; i++)
    if (h->word[i] != ' ' && !hm_was_tried(h, h->word[i])) return 0;
  return 1;
}

/* A letter guessed: how many places it filled (0, a miss), HM_AGAIN or HM_BAD. */
static HM_OPT int hm_guess(Hangman *h, int c) {
  int i, hits = 0;
  if (h->state != HM_PLAYING || !hm_letter(c)) return HM_BAD;
  c = hm_upper(c);
  if (hm_was_tried(h, c)) return HM_AGAIN;
  h->tried |= 1u << (c - 'A');
  h->order[h->ntried++] = (char)c;
  h->order[h->ntried] = 0;
  for (i = 0; i < h->len; i++) hits += h->word[i] == c;
  if (!hits && ++h->misses >= HM_MISSES) h->state = HM_LOST;
  else if (hits && hm_solved(h)) h->state = HM_WON;
  return hits;
}

/* The secret as the guesser sees it: letters not yet found are '_', spaces
 * stay spaces. With `all`, every letter -- the reveal. */
static HM_OPT void hm_shown(const Hangman *h, char *out, int all) {
  int i;
  for (i = 0; i < h->len; i++) {
    char c = h->word[i];
    out[i] = (c == ' ' || all || hm_was_tried(h, c)) ? c : '_';
  }
  out[h->len] = 0;
}

/* The letters tried that are not in the secret, in the order tried. */
static HM_OPT int hm_wrong(const Hangman *h, char *out) {
  int i, j, n = 0;
  for (i = 0; i < h->ntried; i++) {
    int in = 0;
    for (j = 0; j < h->len; j++) in |= h->word[j] == h->order[i];
    if (!in) out[n++] = h->order[i];
  }
  out[n] = 0;
  return n;
}

/* ---- the session: two players, taking turns to set the word ---------------- */

typedef struct {
  char name[2][11];
  int  score[2];
  int  round;              /* from 1 */
  int  setter;             /* 0 or 1: who sets this round's word */
} HmMatch;

static HM_OPT void hm_match_init(HmMatch *m) {
  m->name[0][0] = 'P'; m->name[0][1] = '1'; m->name[0][2] = 0;
  m->name[1][0] = 'P'; m->name[1][1] = '2'; m->name[1][2] = 0;
  m->score[0] = m->score[1] = 0;
  m->round = 1;
  m->setter = 0;
}

/* A round over: a point to the guesser for a word found, to the setter for
 * a figure finished; then the other player sets the next word. */
static HM_OPT void hm_match_round(HmMatch *m, const Hangman *h) {
  int guesser = 1 - m->setter;
  if (h->state == HM_WON) m->score[guesser]++;
  else if (h->state == HM_LOST) m->score[m->setter]++;
  else return;
  m->round++;
  m->setter = guesser;
}

#endif /* CARDOS_HANGMAN_H */
