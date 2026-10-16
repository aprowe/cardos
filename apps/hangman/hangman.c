/* Hangman for two, on one Cardputer passed between them.
 *
 * One player types a secret -- a word or a short phrase -- while the screen
 * shows only dots, passes the device over, and the other guesses a letter at
 * a time. Six misses finish the little figure on the gallows: head, body,
 * two arms, two legs. Find every letter first and the rope is cut and he
 * stands on the grass, arms up.
 *
 * A point to the guesser for a word found, to the setter for a figure
 * finished, and then the other player sets the next word. The score is for
 * the session; nothing is saved. `e` on the score screen names the players.
 *
 * Screens, each one Esc steps back from: the score (the top), names, the
 * secret, the hand-over, the game (Esc asks before giving up), the result.
 *
 * Every repaint is a whole one, composed off the panel by the OS (no damage
 * marks): a key changes the word, the figure, the letters tried and a line
 * of text, and the screen is a few dozen fills.
 *
 * The rules are apps/hangman/hangman.h, host-tested in test/test_hangman.c;
 * the screen in test/test_hangman_paint.c.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/footer.h"
#include "apps/confirm.h"
#include "apps/hangman/hangman.h"

static const CardApi *api;

#define C_BG      CAPP_RGB(26, 30, 44)
#define C_PANEL   CAPP_RGB(36, 42, 60)
#define C_TEXT    CAPP_RGB(236, 238, 244)
#define C_DIM     CAPP_RGB(140, 148, 168)
#define C_WARN    CAPP_RGB(240, 120, 100)
#define C_GOOD    CAPP_RGB(120, 210, 130)
#define C_ACCENT  CAPP_RGB(255, 204, 82)
#define C_WOOD    CAPP_RGB(160, 108, 62)
#define C_WOOD_D  CAPP_RGB(112, 72, 40)
#define C_ROPE    CAPP_RGB(214, 192, 140)
#define C_GRASS   CAPP_RGB(92, 168, 92)
#define C_GRASS_D CAPP_RGB(60, 120, 64)
#define C_SKIN    CAPP_RGB(252, 212, 172)
#define C_INK     CAPP_RGB(44, 32, 36)
#define C_CHEEK   CAPP_RGB(244, 146, 156)
#define C_SHIRT   CAPP_RGB(96, 164, 232)
#define C_LEGS    CAPP_RGB(70, 88, 150)
#define C_GHOST   CAPP_RGB(150, 150, 160)

/* The word on the right: one cell per character. */
#define WX     96
#define WW     140
#define CELL   10

enum { S_SCORE, S_NAMES, S_SECRET, S_HAND, S_PLAY, S_OVER };

static struct {
  int      screen;
  HmMatch  m;
  Hangman  h;
  char     secret[HM_MAX + 1];      /* as typed */
  int      slen;
  int      show;                    /* Tab: the secret in letters, not dots */
  char     edit[11];                /* a name being typed */
  int      elen, ewho;
  int      asking;                  /* "give up? y/n" */
  int      rsetter;                 /* who set this round's word: the match
                                     * has moved on by the time it is shown */
  char     msg[40];
  uint16_t msg_colour;
  uint32_t msg_until;               /* ticks_ms; 0 keeps it */
  int      f_big, f_ui, f_num;
} G;

static void say(const char *s, uint16_t colour, uint32_t ms) {
  api->fmt(G.msg, sizeof G.msg, "%s", s);
  G.msg_colour = colour;
  G.msg_until = ms ? api->ticks_ms() + ms : 0;
}

/* ---- the figure -------------------------------------------------------------- */

static void box(int x, int y, int w, int h, uint16_t c) { api->fill(capp_rect(x, y, w, h), c); }

/* A line two pixels thick. */
static void line(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    box(x0, y0, 2, 2, c);
    if (x0 == x1 && y0 == y1) break;
    {
      int e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
}

static void disc(int cx, int cy, int r, uint16_t c) {
  int dy;
  for (dy = -r; dy <= r; dy++) {
    int dx = 0;
    while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
    box(cx - dx, cy + dy, 2 * dx + 1, 1, c);
  }
}

/* The gallows, the ground under it and the rope. */
static void paint_gallows(void) {
  int x;
  box(0, 112, 92, 12, C_GRASS_D);
  box(0, 112, 92, 2, C_GRASS);
  for (x = 3; x < 92; x += 7) box(x, 110, 1, 2, C_GRASS);         /* tufts */
  box(14, 8, 5, 104, C_WOOD);                                      /* the post */
  box(18, 8, 1, 104, C_WOOD_D);
  box(14, 6, 52, 5, C_WOOD);                                       /* the beam */
  box(14, 10, 52, 1, C_WOOD_D);
  line(19, 26, 33, 11, C_WOOD_D);                                  /* the brace */
  box(6, 108, 22, 4, C_WOOD_D);                                    /* the foot */
  box(60, 11, 2, 12, C_ROPE);
}

/* The figure with `parts` of its six, hanging from the rope at dy = 0 or
 * standing on the grass, arms up, at dy = 34. `dead` crosses the eyes and
 * greys him. */
static void paint_figure(int parts, int dy, int dead) {
  int cx = 61, hy = 30 + dy;
  uint16_t shirt = dead ? C_GHOST : C_SHIRT, legs = dead ? C_GHOST : C_LEGS;
  int up = dy != 0;                                 /* arms raised */
  if (parts >= 2) {                                 /* the body */
    box(cx - 4, hy + 8, 9, 18, C_INK);
    box(cx - 3, hy + 9, 7, 16, shirt);
  }
  if (parts >= 3) line(cx - 4, hy + 11, cx - 13, up ? hy - 1 : hy + 22, C_INK);
  if (parts >= 4) line(cx + 4, hy + 11, cx + 13, up ? hy - 1 : hy + 22, C_INK);
  if (parts >= 5) {
    line(cx - 2, hy + 25, cx - 8, hy + 43, legs);
    box(cx - 11, hy + 43, 4, 2, C_INK);
  }
  if (parts >= 6) {
    line(cx + 1, hy + 25, cx + 7, hy + 43, legs);
    box(cx + 7, hy + 43, 4, 2, C_INK);
  }
  if (parts >= 1) {                                 /* the head, over the neck */
    disc(cx, hy, 8, C_INK);
    disc(cx, hy, 7, dead ? C_GHOST : C_SKIN);
    if (dead) {
      line(cx - 5, hy - 3, cx - 2, hy, C_INK); line(cx - 5, hy, cx - 2, hy - 3, C_INK);
      line(cx + 2, hy - 3, cx + 5, hy, C_INK); line(cx + 2, hy, cx + 5, hy - 3, C_INK);
      box(cx - 3, hy + 4, 7, 1, C_INK);
    } else {
      box(cx - 4, hy - 2, 2, 2, C_INK);
      box(cx + 3, hy - 2, 2, 2, C_INK);
      box(cx - 6, hy + 1, 2, 2, C_CHEEK);
      box(cx + 5, hy + 1, 2, 2, C_CHEEK);
      if (up || parts < 4) {                        /* a smile */
        box(cx - 2, hy + 4, 5, 1, C_INK);
        box(cx - 3, hy + 3, 1, 1, C_INK);
        box(cx + 3, hy + 3, 1, 1, C_INK);
      } else {                                      /* worried */
        box(cx - 1, hy + 3, 3, 3, C_INK);
      }
    }
  }
}

/* ---- the word ------------------------------------------------------------- */

static void paint_word(int reveal) {
  char shown[HM_MAX + 1];
  int starts[HM_LINES + 1], lens[HM_LINES + 1], n, l, i;
  hm_shown(&G.h, shown, reveal);
  n = hm_wrap(G.h.word, G.h.len, HM_COLS, starts, lens, HM_LINES);
  for (l = 0; l < n; l++) {
    int y = (n == 1 ? 48 : 38) + l * 26;
    int x0 = WX + (WW - lens[l] * CELL) / 2;
    for (i = 0; i < lens[l]; i++) {
      char c = shown[starts[l] + i], s[2];
      int x = x0 + i * CELL;
      if (c == ' ') continue;
      box(x + 1, y + 17, CELL - 2, 2, C_DIM);
      if (c == '_') continue;
      s[0] = c;
      s[1] = 0;
      api->text_font(G.f_big, (int16_t)(x + (CELL - api->text_width(G.f_big, s)) / 2), (int16_t)y, s,
                     hm_was_tried(&G.h, c) ? C_TEXT : C_WARN, C_BG);
    }
  }
}

static void paint_play(CRect c) {
  char b[48], wrong[27];
  int i, over = G.screen == S_OVER, setter = G.rsetter, guesser = 1 - setter;
  int x;

  paint_gallows();
  if (over && G.h.state == HM_WON) paint_figure(6, 34, 0);
  else paint_figure(over ? 6 : G.h.misses, 0, over);

  /* The players, the guesser first. */
  api->fmt(b, sizeof b, "%s %d", G.m.name[guesser], G.m.score[guesser]);
  api->text_font(G.f_big, WX, 2, b, C_ACCENT, C_BG);
  api->fmt(b, sizeof b, "%s %d", G.m.name[setter], G.m.score[setter]);
  api->text_font(G.f_ui, (int16_t)(c.w - 4 - api->text_width(G.f_ui, b)), 2, b, C_DIM, C_BG);

  if (!over) {
    api->fmt(b, sizeof b, "%s is guessing", G.m.name[guesser]);
    api->text_font(G.f_ui, WX, 18, b, C_DIM, C_BG);
  } else if (G.h.state == HM_WON) {
    api->fmt(b, sizeof b, "%s got it!", G.m.name[guesser]);
    api->text_font(G.f_ui, WX, 18, b, C_GOOD, C_BG);
  } else {
    api->fmt(b, sizeof b, "%s wins this one", G.m.name[setter]);
    api->text_font(G.f_ui, WX, 18, b, C_WARN, C_BG);
  }

  paint_word(over);

  /* What is left: a pip a miss, then the wrong letters. */
  api->text_font(G.f_ui, WX, 88, "misses", C_DIM, C_BG);
  x = WX + api->text_width(G.f_ui, "misses") + 6;
  for (i = 0; i < HM_MISSES; i++)
    box(x + i * 9, 93, 7, 7, i < G.h.misses ? C_WARN : C_PANEL);
  if (G.msg[0]) api->text_font(G.f_ui, WX, 104, G.msg, G.msg_colour, C_BG);
  else if (hm_wrong(&G.h, wrong)) {
    char spaced[60];
    int n = 0;
    for (i = 0; wrong[i] && n < (int)sizeof spaced - 3; i++) {
      spaced[n++] = wrong[i];
      spaced[n++] = ' ';
    }
    spaced[n] = 0;
    api->text_font(G.f_big, WX, 104, spaced, C_WARN, C_BG);
  }
  if (over) footer_paint(api, c, "enter next round");
  else if (G.asking) footer_paint(api, c, "y give up  n keep going");
  else footer_paint(api, c, "type a letter  esc give up");
}

/* ---- the other screens --------------------------------------------------------- */

static void centred(int f, int y, const char *s, uint16_t fg, CRect c) {
  api->text_font(f, (int16_t)(c.x + (c.w - api->text_width(f, s)) / 2), (int16_t)y, s, fg, C_BG);
}

static void paint_score(CRect c) {
  char b[32];
  int i;
  paint_gallows();
  paint_figure(6, 34, 0);
  centred(G.f_big, 6, "Hangman", C_ACCENT, capp_rect(92, 0, c.w - 92, 20));
  for (i = 0; i < 2; i++) {
    CRect col = capp_rect(96 + i * 72, 0, 70, 0);
    centred(G.f_ui, 28, G.m.name[i], C_DIM, col);
    api->fmt(b, sizeof b, "%d", G.m.score[i]);
    centred(G.f_num, 46, b, C_TEXT, col);
  }
  centred(G.f_num, 46, ":", C_DIM, capp_rect(96, 0, 142, 0));
  api->fmt(b, sizeof b, "round %d", G.m.round);
  centred(G.f_ui, 82, b, C_DIM, capp_rect(92, 0, c.w - 92, 0));
  api->fmt(b, sizeof b, "%s sets the word", G.m.name[G.m.setter]);
  centred(G.f_ui, 99, b, C_TEXT, capp_rect(92, 0, c.w - 92, 0));
  footer_paint(api, c, "enter start  e names");
}

/* A text box with the cursor at its end. */
static void field(int y, const char *s, int dots, CRect c) {
  int i, x = 14, w;
  box(8, y, c.w - 16, 22, C_PANEL);
  if (dots) {
    for (i = 0; s[i]; i++, x += 7)
      if (s[i] != ' ') disc(x + 2, y + 11, 2, C_TEXT);
    w = x - 14;
  } else {
    api->text_font(G.f_big, 14, (int16_t)(y + 3), s, C_TEXT, C_PANEL);
    w = api->text_width(G.f_big, s);
  }
  box(14 + w + 1, y + 4, 2, 14, C_ACCENT);
}

static void paint_names(CRect c) {
  char b[40];
  api->fmt(b, sizeof b, "Player %d's name", G.ewho + 1);
  api->text_font(G.f_big, 8, 14, b, C_ACCENT, C_BG);
  api->text_font(G.f_ui, 8, 32, "up to ten letters", C_DIM, C_BG);
  field(56, G.edit, 0, c);
  footer_paint(api, c, G.ewho == 0 ? "enter next  esc back" : "enter done  esc back");
}

static void paint_secret(CRect c) {
  char b[48];
  int letters = 0, i;
  api->fmt(b, sizeof b, "%s, type a secret", G.m.name[G.m.setter]);
  api->text_font(G.f_big, 8, 6, b, C_ACCENT, C_BG);
  api->fmt(b, sizeof b, "%s, look away!", G.m.name[1 - G.m.setter]);
  api->text_font(G.f_ui, 8, 24, b, C_WARN, C_BG);
  field(48, G.secret, !G.show, c);
  for (i = 0; i < G.slen; i++) letters += G.secret[i] != ' ';
  api->fmt(b, sizeof b, "%d letter%s, a word or a few", letters, letters == 1 ? "" : "s");
  api->text_font(G.f_ui, 8, 76, b, C_DIM, C_BG);
  if (G.msg[0]) api->text_font(G.f_ui, 8, 96, G.msg, G.msg_colour, C_BG);
  footer_paint(api, c, G.show ? "enter done  tab hide  esc back" : "enter done  tab show  esc back");
}

static void paint_hand(CRect c) {
  char b[48];
  paint_gallows();
  paint_figure(0, 0, 0);
  api->fmt(b, sizeof b, "Pass it to %s", G.m.name[1 - G.m.setter]);
  centred(G.f_big, 40, b, C_ACCENT, capp_rect(92, 0, c.w - 92, 0));
  centred(G.f_ui, 62, "then press enter", C_DIM, capp_rect(92, 0, c.w - 92, 0));
  footer_paint(api, c, "enter ready  esc change it");
}

static void app_paint(void *st, CRect c) {
  (void)st;
  api->fill(capp_rect(c.x, c.y, c.w, c.h - FOOT_H), C_BG);
  switch (G.screen) {
  case S_NAMES:  paint_names(c); break;
  case S_SECRET: paint_secret(c); break;
  case S_HAND:   paint_hand(c); break;
  case S_PLAY:
  case S_OVER:   paint_play(c); break;
  default:       paint_score(c); break;
  }
}

/* ---- keys ------------------------------------------------------------------- */

static void to_secret(void) {
  G.screen = S_SECRET;
  G.rsetter = G.m.setter;
  G.secret[0] = 0;
  G.slen = 0;
  G.show = 0;
  say("", C_TEXT, 0);
}

static void edit_name(int who) {
  int i;
  G.screen = S_NAMES;
  G.ewho = who;
  for (i = 0; G.m.name[who][i]; i++) G.edit[i] = G.m.name[who][i];
  G.edit[i] = 0;
  G.elen = i;
}

static int key_names(uint8_t k) {
  if (k == CAPP_KEY_ESC) { G.screen = S_SCORE; return 1; }
  if (k == CAPP_KEY_BACK) { if (G.elen) G.edit[--G.elen] = 0; return 1; }
  if (k == CAPP_KEY_ENTER) {
    if (api->key_repeat()) return 1;
    if (G.elen) api->mem_cpy(G.m.name[G.ewho], G.edit, (size_t)G.elen + 1);
    if (G.ewho == 0) edit_name(1);
    else G.screen = S_SCORE;
    return 1;
  }
  if (k >= ' ' && k < 0x7F && G.elen < (int)sizeof G.edit - 1 && !(k == ' ' && !G.elen)) {
    G.edit[G.elen++] = (char)k;
    G.edit[G.elen] = 0;
    return 1;
  }
  return 0;
}

static int key_secret(uint8_t k) {
  if (k == CAPP_KEY_ESC) { G.screen = S_SCORE; return 1; }
  if (k == '\t') { if (!api->key_repeat()) G.show = !G.show; return 1; }
  if (k == CAPP_KEY_BACK) { if (G.slen) G.secret[--G.slen] = 0; say("", C_TEXT, 0); return 1; }
  if (k == CAPP_KEY_ENTER) {
    int r;
    if (api->key_repeat()) return 1;
    r = hm_start(&G.h, G.secret);
    if (r == HM_EMPTY) say("type a word first", C_WARN, 2000);
    else if (r == HM_LONG) say("too long: two lines of 14", C_WARN, 2500);
    else { G.screen = S_HAND; say("", C_TEXT, 0); }
    return 1;
  }
  if (hm_letter(k) || (k == ' ' && G.slen && G.secret[G.slen - 1] != ' ')) {
    if (G.slen >= HM_MAX) { say("that is as long as it goes", C_WARN, 1500); return 1; }
    G.secret[G.slen++] = (char)hm_upper(k);
    G.secret[G.slen] = 0;
    say("", C_TEXT, 0);
    return 1;
  }
  return 0;
}

static int key_play(uint8_t k) {
  int r;
  char b[40];
  if (api->key_repeat()) return 1;
  if (G.asking) {
    int a = confirm_key(api, k);
    if (a == CONFIRM_YES) {
      G.asking = 0;
      say("", C_TEXT, 0);
      G.h.state = HM_LOST;                 /* a point to the setter */
      G.screen = S_OVER;
      hm_match_round(&G.m, &G.h);
    } else if (a == CONFIRM_NO) {
      G.asking = 0;
      say("", C_TEXT, 0);
    }
    return 1;
  }
  if (k == CAPP_KEY_ESC) { G.asking = 1; say("give up this round?", C_WARN, 0); return 1; }
  r = hm_guess(&G.h, k);
  if (r == HM_BAD) return 0;
  if (r == HM_AGAIN) {
    api->fmt(b, sizeof b, "%c was tried already", hm_upper(k));
    say(b, C_DIM, 1500);
    return 1;
  }
  say("", C_TEXT, 0);
  if (G.h.state != HM_PLAYING) {
    G.screen = S_OVER;
    hm_match_round(&G.m, &G.h);
  }
  return 1;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  switch (G.screen) {
  case S_NAMES:  return key_names(k);
  case S_SECRET: return key_secret(k);
  case S_PLAY:   return key_play(k);
  case S_HAND:
    if (k == CAPP_KEY_ESC) { G.screen = S_SECRET; return 1; }
    if (k == CAPP_KEY_ENTER && !api->key_repeat()) { G.screen = S_PLAY; return 1; }
    return k == CAPP_KEY_ENTER;
  case S_OVER:
    if (k == CAPP_KEY_ENTER && !api->key_repeat()) { G.screen = S_SCORE; return 1; }
    return k == CAPP_KEY_ENTER;
  default:
    if (api->key_repeat()) return 1;
    if (k == CAPP_KEY_ENTER) { to_secret(); return 1; }
    if (k == 'e' || k == 'E') { edit_name(0); return 1; }
    return 0;
  }
}

/* Only a message timing out moves by itself. */
static int app_tick(void *st, uint32_t now) {
  (void)st;
  if (G.msg_until && (int32_t)(now - G.msg_until) >= 0) {
    G.msg[0] = 0;
    G.msg_until = 0;
    return 1;
  }
  return 0;
}

static int app_wants_text(void *st) {
  (void)st;
  return G.screen == S_NAMES || G.screen == S_SECRET || (G.screen == S_PLAY && !G.asking);
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Hangman",
  /* 16x16: the gallows with a figure on the rope. */
  { 0x3F, 0xF0, 0x30, 0x40, 0x30, 0x40, 0x30, 0xE0,
    0x30, 0xA0, 0x30, 0xE0, 0x30, 0x40, 0x30, 0xE0,
    0x31, 0x50, 0x30, 0x40, 0x30, 0xA0, 0x30, 0xA0,
    0x30, 0x00, 0x30, 0x00, 0xFF, 0xF0, 0xFF, 0xF0 },
  "enter\tstart a round, go on\ne\tname the players (score screen)\n"
  "letters\tthe secret, then the guesses\ntab\tshow or hide the secret as it is typed\n"
  "bksp\tdelete a letter\nesc\tback; in a round, give up\n"
  "six misses\tthe setter scores\nthe word\tthe guesser scores\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  hm_match_init(&G.m);
  G.screen = S_SCORE;
  G.f_big = api->font_load("ui13b");
  G.f_ui = api->font_load("ui13");
  G.f_num = api->font_load("num30");

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
