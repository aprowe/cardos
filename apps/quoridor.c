/* Quoridor -- the game wallz.gg plays. Against the computer at three
 * strengths, or two people passing the device between them.
 *
 * The rules and the computer are apps/quoridor.h; this is the title screen,
 * the board and the keys. Blue starts at the bottom going up, red at the top
 * coming down; each player's goal row is tinted in their colour, and each
 * wall is drawn in the colour of whoever laid it. A turn is a step or a wall.
 *
 *   shift+arrow  step that way at once (over the other pawn, too)
 *   arrows       move the cursor (reachable cells are dotted); enter steps
 *                there -- which is how to go sideways round the other pawn
 *   w, tab       wall mode: arrows move it, r turns it, enter lays it; grey
 *                means it cannot go there; escape goes back
 *   escape       to the title, where the game can be resumed
 *
 * Shift with the arrow keys -- ; , . / -- types : < > ?, which is what is
 * matched for the quick step.
 *
 * The board is 9 cells of 11 pixels with 3-pixel grooves: 123 pixels, the
 * height the screen has above the footer.
 */
#include "kernel/app/capp.h"
#include "apps/quoridor.h"
#include "apps/footer.h"

static const CardApi *api;

#define CELL   11
#define GAP    3
#define PITCH  (CELL + GAP)
#define BOARD  (Q_N * CELL + (Q_N - 1) * GAP)        /* 123 */
#define AI_MS  450                 /* long enough to see it was a reply */

#define CLR_BG     CAPP_RGB(15, 17, 23)
#define CLR_CELL   CAPP_RGB(46, 52, 66)
#define CLR_GROOVE CAPP_RGB(24, 27, 35)
#define CLR_TEXT   CAPP_RGB(232, 236, 244)
#define CLR_DIM    CAPP_RGB(128, 136, 152)
#define CLR_SEL    CAPP_RGB(36, 44, 62)
#define CLR_NO     CAPP_RGB(96, 98, 104)    /* a wall that cannot go there */
#define CLR_CURSOR CAPP_RGB(255, 255, 255)

/* Per player: the pawn, the walls, and the goal row's tint. */
static const uint16_t PAWN[2] = { CAPP_RGB(86, 158, 232), CAPP_RGB(228, 86, 76) };
static const uint16_t WALL[2] = { CAPP_RGB(130, 192, 255), CAPP_RGB(255, 140, 110) };
static const uint16_t GOAL[2] = { CAPP_RGB(34, 58, 98), CAPP_RGB(92, 38, 44) };
static const char *const NAME[2] = { "blue", "red" };

enum { SCREEN_TITLE, SCREEN_GAME };
enum { MODE_MOVE, MODE_WALL };
/* What the title offers, in order; RESUME only while a game is on. */
enum { PICK_RESUME, PICK_EASY, PICK_MEDIUM, PICK_HARD, PICK_TWO, PICKS };
static const char *const PICK_TEXT[PICKS] = {
  "Resume", "1 player - easy", "1 player - medium", "1 player - hard", "2 players",
};

static struct {
  int      screen;
  int      pick;                   /* the title's highlighted line */
  int      level;                  /* Q_EASY..Q_HARD, or -1 for two players */
  int      playing;                /* a game is on that can be resumed */
  Quoridor q;
  int      mode;
  int8_t   cr, cc;                 /* move cursor */
  int8_t   wr, wc, wv;             /* wall ghost */
  uint32_t ai_at;                  /* when the computer answers; 0 = not waiting */
  int      wins[2];                /* this run: you and the computer, or blue and red */
  uint32_t seed;
  int      f_big, f_ui;
  CRect    area;
} G;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int bx(void) { return G.area.x + 6; }
static int by(void) { return G.area.y + (G.area.h - FOOT_H - BOARD) / 2; }
static int cpu_game(void) { return G.level >= 0; }
static int humans_turn(void) { return !cpu_game() || G.q.turn == 0; }

static void cursor_to_pawn(void) {
  G.cr = G.q.r[G.q.turn];
  G.cc = G.q.c[G.q.turn];
}

static void new_game(void) {
  q_init(&G.q);
  G.mode = MODE_MOVE;
  cursor_to_pawn();
  G.wv = 0;
  G.ai_at = 0;
  G.playing = 1;
  G.screen = SCREEN_GAME;
}

/* ---- paint: the title ------------------------------------------------------------- */

static void paint_title(CRect c) {
  int i, y = c.y + 34;
  api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_BG);
  api->text_font(G.f_big, (int16_t)(c.x + 12), (int16_t)(c.y + 8), "Quoridor", CLR_TEXT, CLR_BG);
  /* A pawn each, by the title, and a wall. */
  api->fill(rect(c.x + c.w - 40, c.y + 10, 9, 9), PAWN[0]);
  api->fill(rect(c.x + c.w - 26, c.y + 10, 9, 9), PAWN[1]);
  api->fill(rect(c.x + c.w - 41, c.y + 22, 33, 3), WALL[0]);
  for (i = 0; i < PICKS; i++) {
    uint16_t bg = i == G.pick ? CLR_SEL : CLR_BG;
    if (i == PICK_RESUME && !G.playing) continue;
    api->fill(rect(c.x + 8, y, c.w - 16, 16), bg);
    api->text_font(G.f_ui, (int16_t)(c.x + 14), (int16_t)(y + 1), PICK_TEXT[i],
                   i == G.pick ? CLR_TEXT : CLR_DIM, bg);
    y += 17;
  }
  footer_paint(api, c, "enter start  up/down choose");
}

/* ---- paint: the game -------------------------------------------------------------- */

static void paint_wall(int r, int c, int vert, uint16_t colour) {
  if (vert) api->fill(rect(bx() + c * PITCH + CELL, by() + r * PITCH, GAP, CELL * 2 + GAP), colour);
  else api->fill(rect(bx() + c * PITCH, by() + r * PITCH + CELL, CELL * 2 + GAP, GAP), colour);
}

static void paint_board(void) {
  int r, c, i, n, who = G.q.turn;
  int8_t sr[5], sc[5];
  api->fill(rect(bx() - 2, by() - 2, BOARD + 4, BOARD + 4), CLR_GROOVE);
  for (r = 0; r < Q_N; r++)
    for (c = 0; c < Q_N; c++)
      api->fill(rect(bx() + c * PITCH, by() + r * PITCH, CELL, CELL),
                r == q_goal(0) ? GOAL[0] : r == q_goal(1) ? GOAL[1] : CLR_CELL);
  for (r = 0; r < Q_G; r++)
    for (c = 0; c < Q_G; c++) {
      if (G.q.h[r][c]) paint_wall(r, c, 0, WALL[(G.q.h[r][c] - 1) & 1]);
      if (G.q.v[r][c]) paint_wall(r, c, 1, WALL[(G.q.v[r][c] - 1) & 1]);
    }
  for (i = 0; i < 2; i++) {                  /* pawns, corners cut */
    int x = bx() + G.q.c[i] * PITCH, y = by() + G.q.r[i] * PITCH;
    api->fill(rect(x + 2, y + 1, CELL - 4, CELL - 2), PAWN[i]);
    api->fill(rect(x + 1, y + 2, CELL - 2, CELL - 4), PAWN[i]);
  }
  if (G.q.winner >= 0 || !humans_turn()) return;
  if (G.mode == MODE_MOVE) {
    n = q_steps(&G.q, who, sr, sc);
    for (i = 0; i < n; i++)
      api->fill(rect(bx() + sc[i] * PITCH + CELL / 2 - 1, by() + sr[i] * PITCH + CELL / 2 - 1, 3, 3),
                PAWN[who]);
    {
      int x = bx() + G.cc * PITCH - 1, y = by() + G.cr * PITCH - 1;
      api->fill(rect(x, y, CELL + 2, 1), CLR_CURSOR);
      api->fill(rect(x, y + CELL + 1, CELL + 2, 1), CLR_CURSOR);
      api->fill(rect(x, y, 1, CELL + 2), CLR_CURSOR);
      api->fill(rect(x + CELL + 1, y, 1, CELL + 2), CLR_CURSOR);
    }
  } else {
    paint_wall(G.wr, G.wc, G.wv, q_wall_ok(&G.q, who, G.wr, G.wc, G.wv) ? WALL[who] : CLR_NO);
  }
}

static void text(int x, int y, const char *s, uint16_t fg) {
  api->text((int16_t)x, (int16_t)y, s, fg, CLR_BG);
}

static void paint_side(void) {
  char line[28];
  int x = bx() + BOARD + 10, y = by() + 2, i;
  const char *label[2];
  label[0] = cpu_game() ? "you" : "blue";
  label[1] = cpu_game() ? "cpu" : "red";
  text(x, y, cpu_game() ? (G.level == Q_EASY ? "vs cpu: easy" : G.level == Q_MEDIUM ?
                           "vs cpu: medium" : "vs cpu: hard") : "2 players", CLR_DIM);
  y += 14;
  for (i = 0; i < 2; i++, y += 12) {
    api->fill(rect(x, y, 7, 7), PAWN[i]);
    api->fmt(line, sizeof line, "%-4s %d walls", label[i], G.q.left[i]);
    text(x + 11, y, line, G.q.winner < 0 && G.q.turn == i ? CLR_TEXT : CLR_DIM);
  }
  y += 6;
  for (i = 0; i < 2; i++, y += 10) {
    api->fmt(line, sizeof line, "%s %d to go", label[i], q_path(&G.q, i));
    text(x, y, line, PAWN[i]);
  }
  y += 8;
  if (G.q.winner >= 0) {
    if (cpu_game()) api->fmt(line, sizeof line, "%s", G.q.winner == 0 ? "you win!" : "the cpu wins");
    else api->fmt(line, sizeof line, "%s wins!", NAME[G.q.winner]);
  } else if (!humans_turn()) {
    api->fmt(line, sizeof line, "thinking...");
  } else if (cpu_game()) {
    api->fmt(line, sizeof line, "%s", G.mode == MODE_WALL ? "lay a wall" : "your move");
  } else {
    api->fmt(line, sizeof line, "%s to move", NAME[G.q.turn]);
  }
  text(x, y, line, G.q.winner >= 0 ? PAWN[G.q.winner] : humans_turn() ? PAWN[G.q.turn] : CLR_DIM);
  y += 16;
  api->fmt(line, sizeof line, "%s %d  %s %d", label[0], G.wins[0], label[1], G.wins[1]);
  text(x, y, line, CLR_DIM);
}

static void app_paint(void *st, CRect c) {
  (void)st;
  G.area = c;
  if (G.screen == SCREEN_TITLE) { paint_title(c); return; }
  api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_BG);
  paint_board();
  paint_side();
  footer_paint(api, c, G.q.winner >= 0 ? "n again  esc title" :
               G.mode == MODE_WALL ? "enter lay  r turn  esc back" :
                                     "shift+arrow step  w wall  esc menu");
}

/* ---- keys ------------------------------------------------------------------------- */

static void after_turn(void) {
  G.mode = MODE_MOVE;
  if (G.q.winner >= 0) {
    G.wins[G.q.winner]++;
    G.playing = 0;
    return;
  }
  if (cpu_game()) G.ai_at = api->ticks_ms() + AI_MS;
  else cursor_to_pawn();                     /* hand it over */
}

static void play_step(int r, int c) {
  QMove m;
  m.wall = 0; m.vert = 0; m.r = (int8_t)r; m.c = (int8_t)c;
  if (q_play(&G.q, m) == 0) {
    G.cr = (int8_t)r;
    G.cc = (int8_t)c;
    after_turn();
  }
}

static void to_title(void) {
  G.screen = SCREEN_TITLE;
  G.pick = G.playing ? PICK_RESUME : G.level < 0 ? PICK_TWO : PICK_EASY + G.level;
}

static int key_move(uint8_t k) {
  int o = 1 - G.q.turn;
  switch (k) {
  case CAPP_KEY_UP:    if (G.cr > 0) G.cr--; return 1;
  case CAPP_KEY_DOWN:  if (G.cr < Q_N - 1) G.cr++; return 1;
  case CAPP_KEY_LEFT:  if (G.cc > 0) G.cc--; return 1;
  case CAPP_KEY_RIGHT: if (G.cc < Q_N - 1) G.cc++; return 1;
  case CAPP_KEY_ENTER:
  case ' ':
    play_step(G.cr, G.cc);
    return 1;
  case 'w': case 'W': case '\t':
    if (G.q.left[G.q.turn] <= 0) return 1;
    G.mode = MODE_WALL;
    /* Start the wall just in front of the other pawn: where one is wanted. */
    G.wr = (int8_t)(G.q.r[o] - (o == 0 ? 1 : 0));
    if (G.wr < 0) G.wr = 0;
    if (G.wr > Q_G - 1) G.wr = Q_G - 1;
    G.wc = (int8_t)(G.q.c[o] > 0 ? G.q.c[o] - 1 : 0);
    if (G.wc > Q_G - 1) G.wc = Q_G - 1;
    return 1;
  case CAPP_KEY_ESC:
    to_title();
    return 1;
  }
  return 0;
}

static int key_wall(uint8_t k) {
  QMove m;
  switch (k) {
  case CAPP_KEY_UP:    if (G.wr > 0) G.wr--; return 1;
  case CAPP_KEY_DOWN:  if (G.wr < Q_G - 1) G.wr++; return 1;
  case CAPP_KEY_LEFT:  if (G.wc > 0) G.wc--; return 1;
  case CAPP_KEY_RIGHT: if (G.wc < Q_G - 1) G.wc++; return 1;
  case 'r': case 'R': case ' ': G.wv = (int8_t)!G.wv; return 1;
  case CAPP_KEY_ENTER:
    m.wall = 1; m.vert = G.wv; m.r = G.wr; m.c = G.wc;
    if (q_play(&G.q, m) == 0) after_turn();
    return 1;
  case CAPP_KEY_ESC: case 'w': case 'W': case '\t':
    G.mode = MODE_MOVE;
    return 1;
  }
  return 0;
}

static int key_title(uint8_t k) {
  int step = 0;
  if (k == CAPP_KEY_UP) step = -1;
  else if (k == CAPP_KEY_DOWN) step = 1;
  if (step) {
    do G.pick = (G.pick + step + PICKS) % PICKS;
    while (G.pick == PICK_RESUME && !G.playing);
    return 1;
  }
  if (k != CAPP_KEY_ENTER && k != ' ') return 0;
  if (G.pick == PICK_RESUME) { G.screen = SCREEN_GAME; return 1; }
  {
    int level = G.pick == PICK_TWO ? -1 : G.pick - PICK_EASY;
    /* A different opponent is a different score. */
    if (level != G.level) G.wins[0] = G.wins[1] = 0;
    G.level = level;
  }
  new_game();
  return 1;
}

/* Shift+arrow: the direction (Q_DR/Q_DC order), or -1. */
static int shifted_arrow(uint8_t k) {
  switch (k) {
  case ':': return 0;              /* shift ; -- up */
  case '>': return 1;              /* shift . -- down */
  case '<': return 2;              /* shift , -- left */
  case '?': return 3;              /* shift / -- right */
  }
  return -1;
}

static int app_key(void *st, uint8_t k) {
  int d, r, c;
  (void)st;
  if (G.screen == SCREEN_TITLE) return key_title(k);
  if (G.q.winner >= 0) {
    if (k == 'n' || k == 'N' || k == CAPP_KEY_ENTER) { new_game(); return 1; }
    if (k == CAPP_KEY_ESC) { to_title(); return 1; }
    return 0;
  }
  if (!humans_turn()) {
    if (k == CAPP_KEY_ESC) { to_title(); return 1; }
    return 0;
  }
  if ((d = shifted_arrow(k)) >= 0) {
    if (q_step_toward(&G.q, G.q.turn, d, &r, &c)) play_step(r, c);
    return 1;
  }
  return G.mode == MODE_WALL ? key_wall(k) : key_move(k);
}

/* The computer's reply, a moment after yours -- and only while the board is
 * on screen: it does not move behind the title. */
static int app_tick(void *st, uint32_t now) {
  QMove m;
  (void)st;
  if (!G.ai_at || G.screen != SCREEN_GAME || (int32_t)(now - G.ai_at) < 0) return 0;
  G.ai_at = 0;
  if (G.q.winner >= 0 || G.q.turn != 1 || !cpu_game()) return 0;
  G.seed = G.seed * 1664525u + 1013904223u + now;
  m = q_ai_level(&G.q, 1, G.level, G.seed);
  if (q_play(&G.q, m) != 0) {               /* cannot happen; never hang on it */
    int8_t sr[5], sc[5];
    if (q_steps(&G.q, 1, sr, sc) > 0) { m.wall = 0; m.r = sr[0]; m.c = sc[0]; q_play(&G.q, m); }
  }
  if (G.q.winner >= 0) { G.wins[G.q.winner]++; G.playing = 0; }
  else cursor_to_pawn();
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Quoridor",
  /* 16x16: a board of cells with a wall laid across it. */
  { 0x00, 0x00, 0x77, 0x70, 0x77, 0x70, 0x77, 0x70,
    0x00, 0x00, 0x7F, 0xF0, 0x00, 0x00, 0x77, 0x70,
    0x77, 0x70, 0x77, 0x70, 0x00, 0x00, 0x77, 0x70,
    0x77, 0x70, 0x77, 0x70, 0x00, 0x00, 0x00, 0x00 },
  "shift+arrow\tstep that way now\n"
  "arrows\tmove the cursor, or the wall\n"
  "enter, space\tstep to the cursor\n"
  "w, tab\twall mode, and back\n"
  "r, space\tturn the wall (in wall mode)\n"
  "enter\tlay the wall (in wall mode)\n"
  "esc\tback to moving; from moving, the title\n"
  "n\tanother game, once one is won\n"
  "\n"
  "reach the far row -- your colour -- first.\n"
  "a wall blocks two cells and may not shut\n"
  "anyone in; grey means it cannot go there.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  G.seed = api->ticks_ms();
  G.f_big = api->font_load("ui13b");
  G.f_ui = api->font_load("ui13");
  G.screen = SCREEN_TITLE;
  G.pick = PICK_MEDIUM;
  G.level = Q_MEDIUM;
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  api->ui(&UI);
  return 0;
}
