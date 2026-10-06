/* Songs as text, into MIDI messages with times. No screen, no API, no libc:
 * apps/midi.c plays what this makes, test/test_midiseq.c checks it, and the
 * server's prompt (server/midi.py) teaches Claude to write it.
 *
 *   # Fur Elise, opening       the first # line is the title
 *   tempo 72                   beats a minute (one only)
 *   ch 1                       the channel for what follows, 1..16
 *   prog 0                     program change now (or: prog 0 at 8)
 *   n 0 E5 0.25 80             at beat 0, E5, a quarter beat long, velocity 80
 *   n 0.25 D#5 1/4             a fraction is fine; velocity defaults to 90
 *   n 2 A3,E4,A4 1             a chord
 *   cc 0 7 100                 at beat 0, controller 7 to 100
 *   ramp 0 8 74 20 120         controller 74 from 20 to 120 across beats 0..8
 *   bend 4 2048                pitch bend at beat 4, -8192..8191
 *   loop                       play it round; or `loop 16` for 16 beats
 *   ; and # after a command are comments
 *
 * Notes are names (C4 is middle C, 60; # and b, octaves -1..9) or numbers.
 * Times are beats from the start, decimals or a/b. Inside, everything is in
 * ticks, 480 a beat, and integer -- an app has no float division.
 */
#ifndef CARDOS_MIDISEQ_H
#define CARDOS_MIDISEQ_H

#include <stdint.h>

#define MS_PPQ        480
/* An app sets these smaller before the include: a song lives in its data,
 * and an app's data has to come in one piece of a heap that breaks up. */
#ifndef MS_MAX_EV
#define MS_MAX_EV     2048
#endif
#ifndef MS_MAX_NOTES
#define MS_MAX_NOTES  768
#endif
#define MS_TITLE      40

typedef struct {
  uint32_t tick;
  uint8_t  order;                /* among equals: 0 off, 1 control, 2 on */
  uint8_t  n;
  uint8_t  b[3];
} MsEvent;

typedef struct {
  uint32_t start, len;           /* ticks */
  uint8_t  pitch, ch, vel;
} MsNote;

typedef struct {
  char     title[MS_TITLE];
  int      tempo;
  int      loop;                 /* play it round */
  uint32_t loop_ticks;           /* 0: to the end of the bar after the last thing */
  uint32_t end_ticks;            /* the last thing's end */
  MsEvent  ev[MS_MAX_EV];
  int      nev;
  MsNote   note[MS_MAX_NOTES];
  int      nnotes;
  int      err_line;             /* 0 when it parsed */
  char     err[48];
} MsSong;

/* ---- small text helpers --------------------------------------------------------------- */

static int ms_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int ms_digit(char c) { return c >= '0' && c <= '9'; }

static void ms_fail(MsSong *s, int line, const char *a, const char *b) {
  int i = 0, j = 0;
  if (s->err_line) return;
  s->err_line = line;
  while (a[j] && i < (int)sizeof s->err - 1) s->err[i++] = a[j++];
  j = 0;
  while (b && b[j] && i < (int)sizeof s->err - 1) s->err[i++] = b[j++];
  s->err[i] = 0;
}

/* The next word of `*p` into w; 0 if there is none. Stops at ; and # comments. */
static int ms_word(const char **p, char *w, int n) {
  const char *q = *p;
  int i = 0;
  while (ms_space(*q)) q++;
  if (!*q || *q == '\n' || *q == ';' || *q == '#') { *p = q; return 0; }
  while (*q && !ms_space(*q) && *q != '\n' && *q != ';') { if (i < n - 1) w[i++] = *q; q++; }
  w[i] = 0;
  *p = q;
  return 1;
}

/* An integer, maybe negative; -100000 on anything else. */
static long ms_int(const char *w) {
  long v = 0;
  int neg = 0, any = 0;
  if (*w == '-') { neg = 1; w++; } else if (*w == '+') w++;
  while (ms_digit(*w)) { v = v * 10 + (*w++ - '0'); any = 1; }
  if (*w || !any) return -100000;
  return neg ? -v : v;
}

/* Beats -- "2", "0.25", "1/3", "1.5/2" -- into ticks; -1 on nonsense. */
static long ms_ticks(const char *w) {
  long whole = 0, frac = 0, scale = 1, num, den = 0;
  int any = 0;
  while (ms_digit(*w)) { whole = whole * 10 + (*w++ - '0'); any = 1; if (whole > 4000) return -1; }
  if (*w == '.') {
    w++;
    while (ms_digit(*w)) { if (scale < 1000) { frac = frac * 10 + (*w - '0'); scale *= 10; } w++; any = 1; }
  }
  if (!any) return -1;
  num = (whole * scale + frac) * MS_PPQ;          /* ticks, times scale */
  if (*w == '/') {
    w++;
    while (ms_digit(*w)) den = den * 10 + (*w++ - '0');
    if (!den) return -1;
  } else den = 1;
  if (*w || den > 64) return -1;
  /* Rounded: 1/3 of a beat is 160 ticks, 0.333 is 160 too. 4000 beats at
   * three decimals is under 2^31. */
  return (num + scale * den / 2) / (scale * den);
}

/* A note name or number into 0..127; -1 if it is neither. */
static int ms_pitch(const char *w) {
  static const int PC[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* A B C D E F G */
  int pc, oct = 0, neg = 0, any = 0;
  long v;
  if (ms_digit(*w)) { v = ms_int(w); return v >= 0 && v <= 127 ? (int)v : -1; }
  if (*w >= 'a' && *w <= 'g') pc = PC[*w - 'a'];
  else if (*w >= 'A' && *w <= 'G') pc = PC[*w - 'A'];
  else return -1;
  w++;
  if (*w == '#') { pc++; w++; }
  else if (*w == 'b') { pc--; w++; }
  if (*w == '-') { neg = 1; w++; }
  while (ms_digit(*w)) { oct = oct * 10 + (*w++ - '0'); any = 1; }
  if (*w || !any) return -1;
  if (neg) oct = -oct;
  v = 12L * (oct + 1) + pc;
  return v >= 0 && v <= 127 ? (int)v : -1;
}

/* ---- building the song ------------------------------------------------------------------- */

static void ms_add(MsSong *s, int line, uint32_t tick, int order, int n, int b0, int b1, int b2) {
  MsEvent *e;
  if (s->nev >= MS_MAX_EV) { ms_fail(s, line, "too many events for one song", 0); return; }
  e = &s->ev[s->nev++];
  e->tick = tick;
  e->order = (uint8_t)order;
  e->n = (uint8_t)n;
  e->b[0] = (uint8_t)b0; e->b[1] = (uint8_t)b1; e->b[2] = (uint8_t)b2;
}

static void ms_reach(MsSong *s, uint32_t t) { if (t > s->end_ticks) s->end_ticks = t; }

/* By time, and at the same time off before control before on -- a note
 * repeated on the beat is ended before it starts again. Insertion sort:
 * the lines are mostly in order already. */
static void ms_sort(MsSong *s) {
  int i, j;
  /* Field by field: a struct assignment can become a call to memcpy,
   * which an app does not have. */
  for (i = 1; i < s->nev; i++) {
    uint32_t tick = s->ev[i].tick;
    uint8_t order = s->ev[i].order, n = s->ev[i].n;
    uint8_t b0 = s->ev[i].b[0], b1 = s->ev[i].b[1], b2 = s->ev[i].b[2];
    for (j = i - 1; j >= 0 && (s->ev[j].tick > tick ||
                               (s->ev[j].tick == tick && s->ev[j].order > order)); j--) {
      MsEvent *d = &s->ev[j + 1], *f = &s->ev[j];
      d->tick = f->tick; d->order = f->order; d->n = f->n;
      d->b[0] = f->b[0]; d->b[1] = f->b[1]; d->b[2] = f->b[2];
    }
    s->ev[j + 1].tick = tick; s->ev[j + 1].order = order; s->ev[j + 1].n = n;
    s->ev[j + 1].b[0] = b0; s->ev[j + 1].b[1] = b1; s->ev[j + 1].b[2] = b2;
  }
}

static int ms_parse(MsSong *s, const char *text) {
  const char *p = text;
  int line = 1, ch = 0;
  s->title[0] = 0;
  s->tempo = 0;
  s->loop = 0;
  s->loop_ticks = 0;
  s->end_ticks = 0;
  s->nev = s->nnotes = 0;
  s->err_line = 0;
  s->err[0] = 0;
  while (*p && !s->err_line) {
    char cmd[16], a[64], b[16], c[16], d[16], e[16];
    const char *q;
    while (ms_space(*p)) p++;
    if (*p == '#' && !s->title[0]) {                  /* the title */
      int i = 0;
      p++;
      while (ms_space(*p)) p++;
      while (*p && *p != '\n' && i < MS_TITLE - 1) s->title[i++] = *p++;
      while (i && ms_space(s->title[i - 1])) i--;
      s->title[i] = 0;
    }
    q = p;
    if (ms_word(&q, cmd, sizeof cmd)) {
      long t, len, v, x, y;
      if (cmd[0] == 'n' && !cmd[1]) {
        const char *k;
        int vel = 90;
        if (!ms_word(&q, a, sizeof a) || (t = ms_ticks(a)) < 0) { ms_fail(s, line, "n: when? a beat, like 0 or 1.5", 0); break; }
        if (!ms_word(&q, a, sizeof a)) { ms_fail(s, line, "n: which note?", 0); break; }
        if (!ms_word(&q, b, sizeof b) || (len = ms_ticks(b)) <= 0) { ms_fail(s, line, "n: how long, in beats?", 0); break; }
        if (ms_word(&q, c, sizeof c)) {
          v = ms_int(c);
          if (v < 1 || v > 127) { ms_fail(s, line, "n: velocity is 1..127", 0); break; }
          vel = (int)v;
        }
        for (k = a; *k; ) {                           /* each note of a chord */
          char one[8];
          int i = 0, pitch;
          while (*k && *k != ',' && i < 7) one[i++] = *k++;
          one[i] = 0;
          if (*k == ',') k++;
          if ((pitch = ms_pitch(one)) < 0) { ms_fail(s, line, "what note is ", one); break; }
          ms_add(s, line, (uint32_t)t, 2, 3, 0x90 | ch, pitch, vel);
          ms_add(s, line, (uint32_t)(t + len), 0, 3, 0x80 | ch, pitch, 0);
          if (s->nnotes < MS_MAX_NOTES) {
            MsNote *nt = &s->note[s->nnotes++];
            nt->start = (uint32_t)t; nt->len = (uint32_t)len;
            nt->pitch = (uint8_t)pitch; nt->ch = (uint8_t)ch; nt->vel = (uint8_t)vel;
          }
        }
        ms_reach(s, (uint32_t)(t + len));
      } else if (!cmd[2] && cmd[0] == 'c' && cmd[1] == 'c') {
        if (!ms_word(&q, a, sizeof a) || (t = ms_ticks(a)) < 0 || !ms_word(&q, b, sizeof b) ||
            !ms_word(&q, c, sizeof c)) { ms_fail(s, line, "cc: beat, controller, value", 0); break; }
        x = ms_int(b); v = ms_int(c);
        if (x < 0 || x > 127 || v < 0 || v > 127) { ms_fail(s, line, "cc: controller and value are 0..127", 0); break; }
        ms_add(s, line, (uint32_t)t, 1, 3, 0xB0 | ch, (int)x, (int)v);
        ms_reach(s, (uint32_t)t);
      } else if (cmd[0] == 'r' && cmd[1] == 'a' && cmd[2] == 'm' && cmd[3] == 'p' && !cmd[4]) {
        long t2, from, to, step, at;
        if (!ms_word(&q, a, sizeof a) || (t = ms_ticks(a)) < 0 || !ms_word(&q, b, sizeof b) ||
            (t2 = ms_ticks(b)) <= t || !ms_word(&q, c, sizeof c) || !ms_word(&q, d, sizeof d) ||
            !ms_word(&q, e, sizeof e)) { ms_fail(s, line, "ramp: from, to, controller, start, end", 0); break; }
        x = ms_int(c); from = ms_int(d); to = ms_int(e);
        if (x < 0 || x > 127 || from < 0 || from > 127 || to < 0 || to > 127) { ms_fail(s, line, "ramp: 0..127", 0); break; }
        /* A step every eighth of a beat, and only when the value changes. */
        step = MS_PPQ / 8;
        y = -1;
        for (at = t; at <= t2; at += step) {
          long val = from + (to - from) * (at - t) / (t2 - t);
          if (val != y) { ms_add(s, line, (uint32_t)at, 1, 3, 0xB0 | ch, (int)x, (int)val); y = val; }
        }
        if (y != to) ms_add(s, line, (uint32_t)t2, 1, 3, 0xB0 | ch, (int)x, (int)to);
        ms_reach(s, (uint32_t)t2);
      } else if (cmd[0] == 'b' && cmd[1] == 'e' && cmd[2] == 'n' && cmd[3] == 'd' && !cmd[4]) {
        if (!ms_word(&q, a, sizeof a) || (t = ms_ticks(a)) < 0 || !ms_word(&q, b, sizeof b) ||
            (v = ms_int(b)) < -8192 || v > 8191) { ms_fail(s, line, "bend: beat, -8192..8191", 0); break; }
        v += 8192;
        ms_add(s, line, (uint32_t)t, 1, 3, 0xE0 | ch, (int)(v & 0x7F), (int)(v >> 7));
        ms_reach(s, (uint32_t)t);
      } else if (cmd[0] == 'p' && cmd[1] == 'r' && cmd[2] == 'o' && cmd[3] == 'g' && !cmd[4]) {
        t = 0;
        if (!ms_word(&q, a, sizeof a) || (v = ms_int(a)) < 0 || v > 127) { ms_fail(s, line, "prog: 0..127", 0); break; }
        if (ms_word(&q, b, sizeof b)) {
          if (!(b[0] == 'a' && b[1] == 't' && !b[2]) || !ms_word(&q, c, sizeof c) || (t = ms_ticks(c)) < 0) {
            ms_fail(s, line, "prog: N, or N at BEAT", 0); break;
          }
        }
        ms_add(s, line, (uint32_t)t, 1, 2, 0xC0 | ch, (int)v, 0);
      } else if (cmd[0] == 'c' && cmd[1] == 'h' && !cmd[2]) {
        if (!ms_word(&q, a, sizeof a) || (v = ms_int(a)) < 1 || v > 16) { ms_fail(s, line, "ch: 1..16", 0); break; }
        ch = (int)v - 1;
      } else if (cmd[0] == 't' && cmd[1] == 'e' && cmd[2] == 'm' && cmd[3] == 'p' && cmd[4] == 'o' && !cmd[5]) {
        /* A decimal is rounded: Claude has written 42.5 when asked to halve 85. */
        if (!ms_word(&q, a, sizeof a) || (v = ms_ticks(a)) < 0 || (v = (v + MS_PPQ / 2) / MS_PPQ) < 20 || v > 400) {
          ms_fail(s, line, "tempo: 20..400 beats a minute", 0);
          break;
        }
        if (!s->tempo) s->tempo = (int)v;
      } else if (cmd[0] == 'l' && cmd[1] == 'o' && cmd[2] == 'o' && cmd[3] == 'p' && !cmd[4]) {
        s->loop = 1;
        if (ms_word(&q, a, sizeof a)) {
          if ((t = ms_ticks(a)) <= 0) { ms_fail(s, line, "loop: how many beats?", 0); break; }
          s->loop_ticks = (uint32_t)t;
        }
      } else {
        ms_fail(s, line, "what is ", cmd);
        break;
      }
    }
    while (*p && *p != '\n') p++;
    if (*p == '\n') { p++; line++; }
  }
  if (!s->tempo) s->tempo = 120;
  if (s->loop && !s->loop_ticks) {                     /* the end of the bar after */
    uint32_t bar = 4 * MS_PPQ;
    s->loop_ticks = (s->end_ticks + bar - 1) / bar * bar;
    if (!s->loop_ticks) s->loop_ticks = bar;
  }
  ms_sort(s);
  return s->err_line ? -1 : 0;
}

/* Ticks to milliseconds at the song's tempo: 60000 / (tempo * 480) = 125 / tempo. */
static uint32_t ms_ms(const MsSong *s, uint32_t ticks) {
  return ticks / (uint32_t)s->tempo * 125 + (ticks % (uint32_t)s->tempo) * 125 / (uint32_t)s->tempo;
}

#endif /* CARDOS_MIDISEQ_H */
