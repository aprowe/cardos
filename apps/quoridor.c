/* Quoridor, against the computer -- the game wallz.gg plays.
 *
 * The rules and the computer are apps/quoridor.h; this is the board and the
 * keys. You are blue, at the bottom, going up; the computer is red, coming
 * down. A turn is a step or a wall.
 *
 * Two modes, because a turn is two different kinds of choice:
 *   move   arrows put the cursor on a cell (the cells you can step to are
 *          dotted); enter or space steps there
 *   wall   w or tab: arrows move a wall along the grooves, space turns it,
 *          enter lays it; red means it cannot go there; escape goes back
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
#define CLR_WALL   CAPP_RGB(255, 196, 76)
#define CLR_GHOST  CAPP_RGB(150, 120, 60)
#define CLR_BAD    CAPP_RGB(220, 70, 60)
#define CLR_YOU    CAPP_RGB(86, 158, 232)
#define CLR_CPU    CAPP_RGB(228, 86, 76)
#define CLR_CURSOR CAPP_RGB(255, 255, 255)
#define CLR_HOME_Y CAPP_RGB(40, 58, 88)    /* the row you are heading for */

enum { MODE_MOVE, MODE_WALL };

static struct {
  Quoridor q;
  int      mode;
  int8_t   cr, cc;                 /* move cursor */
  int8_t   wr, wc, wv;             /* wall ghost */
  uint32_t ai_at;                  /* when the computer answers; 0 = not waiting */
  int      wins[2];
  uint32_t seed;
  CRect    area;
} G;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int bx(void) { return G.area.x + 6; }
static int by(void) { return G.area.y + (G.area.h - FOOT_H - BOARD) / 2; }

static void new_game(void) {
  q_init(&G.q);
  G.mode = MODE_MOVE;
  G.cr = G.q.r[0];
  G.cc = G.q.c[0];
  G.wr = 6; G.wc = 3; G.wv = 0;
  G.ai_at = 0;
}

/* ---- paint ------------------------------------------------------------------------ */

static void paint_wall(int r, int c, int vert, uint16_t colour) {
  if (vert) api->fill(rect(bx() + c * PITCH + CELL, by() + r * PITCH, GAP, CELL * 2 + GAP), colour);
  else api->fill(rect(bx() + c * PITCH, by() + r * PITCH + CELL, CELL * 2 + GAP, GAP), colour);
}

static void paint_board(void) {
  int r, c, i, n;
  int8_t sr[5], sc[5];
  api->fill(rect(bx() - 2, by() - 2, BOARD + 4, BOARD + 4), CLR_GROOVE);
  for (r = 0; r < Q_N; r++)
    for (c = 0; c < Q_N; c++)
      api->fill(rect(bx() + c * PITCH, by() + r * PITCH, CELL, CELL),
                r == 0 ? CLR_HOME_Y : CLR_CELL);
  for (r = 0; r < Q_G; r++)
    for (c = 0; c < Q_G; c++) {
      if (G.q.h[r][c]) paint_wall(r, c, 0, CLR_WALL);
      if (G.q.v[r][c]) paint_wall(r, c, 1, CLR_WALL);
    }
  /* Pawns, as rounded-off squares. */
  for (i = 0; i < 2; i++) {
    int x = bx() + G.q.c[i] * PITCH, y = by() + G.q.r[i] * PITCH;
    uint16_t col = i == 0 ? CLR_YOU : CLR_CPU;
    api->fill(rect(x + 2, y + 1, CELL - 4, CELL - 2), col);
    api->fill(rect(x + 1, y + 2, CELL - 2, CELL - 4), col);
  }
  if (G.q.winner >= 0 || G.q.turn != 0) return;
  if (G.mode == MODE_MOVE) {
    n = q_steps(&G.q, 0, sr, sc);
    for (i = 0; i < n; i++)
      api->fill(rect(bx() + sc[i] * PITCH + CELL / 2 - 1, by() + sr[i] * PITCH + CELL / 2 - 1, 3, 3),
                CLR_YOU);
    {
      int x = bx() + G.cc * PITCH - 1, y = by() + G.cr * PITCH - 1;
      api->fill(rect(x, y, CELL + 2, 1), CLR_CURSOR);
      api->fill(rect(x, y + CELL + 1, CELL + 2, 1), CLR_CURSOR);
      api->fill(rect(x, y, 1, CELL + 2), CLR_CURSOR);
      api->fill(rect(x + CELL + 1, y, 1, CELL + 2), CLR_CURSOR);
    }
  } else {
    paint_wall(G.wr, G.wc, G.wv, q_wall_ok(&G.q, 0, G.wr, G.wc, G.wv) ? CLR_GHOST : CLR_BAD);
  }
}

static void text(int x, int y, const char *s, uint16_t fg) {
  api->text((int16_t)x, (int16_t)y, s, fg, CLR_BG);
}

static void paint_side(void) {
  char line[24];
  int x = bx() + BOARD + 10, y = by() + 2;
  const char *say;
  text(x, y, "Quoridor", CLR_TEXT);
  y += 16;
  api->fmt(line, sizeof line, "%d walls", G.q.left[0]);
  api->fill(rect(x, y, 6, 7), CLR_YOU);
  text(x + 10, y, line, CLR_TEXT);
  y += 12;
  api->fmt(line, sizeof line, "%d walls", G.q.left[1]);
  api->fill(rect(x, y, 6, 7), CLR_CPU);
  text(x + 10, y, line, CLR_TEXT);
  y += 18;
  api->fmt(line, sizeof line, "%d to go", q_path(&G.q, 0));
  text(x, y, line, CLR_YOU);
  y += 10;
  api->fmt(line, sizeof line, "%d to go", q_path(&G.q, 1));
  text(x, y, line, CLR_CPU);
  y += 18;
  if (G.q.winner == 0) say = "you win!";
  else if (G.q.winner == 1) say = "computer wins";
  else if (G.q.turn == 1) say = "thinking...";
  else say = G.mode == MODE_WALL ? "lay a wall" : "your move";
  text(x, y, say, G.q.winner == 0 ? CLR_YOU : G.q.winner == 1 ? CLR_CPU : CLR_DIM);
  y += 18;
  api->fmt(line, sizeof line, "won %d  lost %d", G.wins[0], G.wins[1]);
  text(x, y, line, CLR_DIM);
}

static void app_paint(void *st, CRect c) {
  (void)st;
  G.area = c;
  api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_BG);
  paint_board();
  paint_side();
  footer_paint(api, c, G.q.winner >= 0 ? "n new game" :
               G.mode == MODE_WALL ? "enter lay  space turn  esc back" :
                                     "enter move  w wall  n new");
}

/* ---- keys ------------------------------------------------------------------------- */

static void after_turn(void) {
  if (G.q.winner >= 0) {
    G.wins[G.q.winner]++;
    return;
  }
  G.ai_at = api->ticks_ms() + AI_MS;
}

static int key_move(uint8_t k) {
  QMove m;
  switch (k) {
  case CAPP_KEY_UP:    if (G.cr > 0) G.cr--; return 1;
  case CAPP_KEY_DOWN:  if (G.cr < Q_N - 1) G.cr++; return 1;
  case CAPP_KEY_LEFT:  if (G.cc > 0) G.cc--; return 1;
  case CAPP_KEY_RIGHT: if (G.cc < Q_N - 1) G.cc++; return 1;
  case CAPP_KEY_ENTER:
  case ' ':
    m.wall = 0; m.vert = 0; m.r = G.cr; m.c = G.cc;
    if (q_play(&G.q, m) == 0) after_turn();
    return 1;
  case 'w': case 'W': case '\t':
    if (G.q.left[0] <= 0) return 1;
    G.mode = MODE_WALL;
    /* Start the wall just in front of the computer: where one is wanted. */
    G.wr = (int8_t)(G.q.r[1] < Q_G ? G.q.r[1] : Q_G - 1);
    G.wc = (int8_t)(G.q.c[1] > 0 ? G.q.c[1] - 1 : 0);
    if (G.wc > Q_G - 1) G.wc = Q_G - 1;
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
  case ' ':            G.wv = (int8_t)!G.wv; return 1;
  case CAPP_KEY_ENTER:
    m.wall = 1; m.vert = G.wv; m.r = G.wr; m.c = G.wc;
    if (q_play(&G.q, m) == 0) {
      G.mode = MODE_MOVE;
      after_turn();
    }
    return 1;
  case CAPP_KEY_ESC: case 'w': case 'W': case '\t':
    G.mode = MODE_MOVE;
    return 1;
  }
  return 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (k == 'n' || k == 'N') { new_game(); return 1; }
  if (G.q.winner >= 0 || G.q.turn != 0) return 0;
  return G.mode == MODE_WALL ? key_wall(k) : key_move(k);
}

/* The computer's reply, a moment after yours. */
static int app_tick(void *st, uint32_t now) {
  QMove m;
  (void)st;
  if (!G.ai_at || (int32_t)(now - G.ai_at) < 0) return 0;
  G.ai_at = 0;
  if (G.q.winner >= 0 || G.q.turn != 1) return 0;
  G.seed = G.seed * 1664525u + 1013904223u + now;
  m = q_ai(&G.q, 1, G.seed);
  if (q_play(&G.q, m) != 0) {               /* cannot happen; never hang on it */
    int8_t sr[5], sc[5];
    if (q_steps(&G.q, 1, sr, sc) > 0) { m.wall = 0; m.r = sr[0]; m.c = sc[0]; q_play(&G.q, m); }
  }
  if (G.q.winner >= 0) G.wins[G.q.winner]++;
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
  "arrows\tmove the cursor, or the wall\n"
  "enter, space\tstep to the cursor\n"
  "w, tab\twall mode, and back\n"
  "space\tturn the wall (in wall mode)\n"
  "enter\tlay the wall (in wall mode)\n"
  "esc\tback to moving\n"
  "n\tnew game\n"
  "\n"
  "reach the far row first. a wall blocks two\n"
  "cells and may not shut anyone in.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  G.seed = api->ticks_ms();
  new_game();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  api->ui(&UI);
  return 0;
}
