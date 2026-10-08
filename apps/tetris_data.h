/* Tetris's tables: the seven pieces' four rotations, their wall kicks, where
 * they spawn, and how scoring and speed move with level. No screen, no API,
 * no libc, no logic -- apps/tetris.c is the game.
 *
 * A piece's four rotation states are each a 4x4 grid, bit (row*4+col), row 0
 * at the top, bit set where the piece fills that cell -- the same box every
 * rotation lives in, so a rotation is just "try this mask at this (row,col)
 * offset from the one before", with the kick tables below saying which
 * offsets to try when the straight rotation would overlap the stack or a
 * wall. J, L, S, T and Z use a 3x3 shape inside the box (column 3 and, for
 * the horizontal states, row 3 are always empty); I uses the full 4x4; O is
 * drawn the same in all four states, since a player pressing rotate on a
 * square expects nothing to move, which is what every real Tetris shows
 * them even though the strict spec shifts it a cell internally.
 *
 * The rotation state is just 0..3, clockwise from spawn (TT_SPAWN is state
 * 0; the others are never named, only ever reached by +1/-1 mod 4, which is
 * all apps/tetris.c ever does with one). The kick tables are the standard
 * rotation system's, converted from its y-up convention to this file's: row
 * grows downward, so a kick's dy here is the negative of the spec's. Each
 * rotation attempt is tried as (dx, dy) from the table in order, indexed by
 * the state rotated FROM and the direction; the first that does not collide
 * wins, and TT_KICK_TESTS is how many there are to try (test 0 is always
 * (0, 0), the un-kicked rotation).
 */
#ifndef CARDOS_TETRIS_DATA_H
#define CARDOS_TETRIS_DATA_H

#include <stdint.h>

#define TT_I 0
#define TT_O 1
#define TT_T 2
#define TT_S 3
#define TT_Z 4
#define TT_J 5
#define TT_L 6
#define TT_PIECES 7

#define TT_SPAWN 0   /* rotation state 0; 1..3 follow clockwise, never named */
#define TT_ROTS  4

#define TT_BOX   4   /* every shape lives in a TT_BOX x TT_BOX grid */

/* Shapes, one uint16_t per rotation, bit (row*4+col) set where filled. */
static const uint16_t TT_SHAPE[TT_PIECES][TT_ROTS] = {
  /* I */ { 0x00F0, 0x4444, 0x0F00, 0x2222 },
  /* O */ { 0x0660, 0x0660, 0x0660, 0x0660 },
  /* T */ { 0x0072, 0x0262, 0x0270, 0x0232 },
  /* S */ { 0x0036, 0x0462, 0x0360, 0x0231 },
  /* Z */ { 0x0063, 0x0264, 0x0630, 0x0132 },
  /* J */ { 0x0071, 0x0226, 0x0470, 0x0322 },
  /* L */ { 0x0074, 0x0622, 0x0170, 0x0223 },
};

#define TT_KICK_TESTS 5    /* tests tried, in order, for one rotation */
#define TT_CW  0           /* the two rotation directions, for the dir index */
#define TT_CCW 1

/* J, L, S, T and Z share one kick table, indexed by the state rotated FROM
 * and the direction. O never moves (see above) and needs none; I has its
 * own table below, since its longer box kicks further. */
static const int8_t TT_KICK_JLSTZ[TT_ROTS][2][TT_KICK_TESTS][2] = {
  /* from state 0 (spawn) */
  { { {0,0}, {-1,0}, {-1,-1}, {0,2}, {-1,2} },     /* CW  -> state 1 */
    { {0,0}, {1,0},  {1,-1},  {0,2}, {1,2}  } },   /* CCW -> state 3 */
  /* from state 1 */
  { { {0,0}, {1,0},  {1,1},   {0,-2}, {1,-2} },    /* CW  -> state 2 */
    { {0,0}, {1,0},  {1,1},   {0,-2}, {1,-2} } },  /* CCW -> state 0 */
  /* from state 2 */
  { { {0,0}, {1,0},  {1,-1},  {0,2}, {1,2}   },    /* CW  -> state 3 */
    { {0,0}, {-1,0}, {-1,-1}, {0,2}, {-1,2}  } },  /* CCW -> state 1 */
  /* from state 3 */
  { { {0,0}, {-1,0}, {-1,1},  {0,-2}, {-1,-2} },   /* CW  -> state 0 */
    { {0,0}, {-1,0}, {-1,1},  {0,-2}, {-1,-2} } }, /* CCW -> state 2 */
};

static const int8_t TT_KICK_I[TT_ROTS][2][TT_KICK_TESTS][2] = {
  /* from state 0 (spawn) */
  { { {0,0}, {-2,0}, {1,0},  {-2,1},  {1,-2}  },   /* CW  -> state 1 */
    { {0,0}, {-1,0}, {2,0},  {-1,-2}, {-2,1}  } }, /* CCW -> state 3 */
  /* from state 1 */
  { { {0,0}, {-1,0}, {2,0},  {1,-2},  {2,1}   },   /* CW  -> state 2 */
    { {0,0}, {2,0},  {-1,0}, {2,-1},  {-1,2}  } }, /* CCW -> state 0 */
  /* from state 2 */
  { { {0,0}, {2,0},  {-1,0}, {2,1},   {-1,-2} },   /* CW  -> state 3 */
    { {0,0}, {1,0},  {-2,0}, {-1,2},  {-2,-1} } }, /* CCW -> state 1 */
  /* from state 3 */
  { { {0,0}, {1,0},  {-2,0}, {1,2},   {2,-1}  },   /* CW  -> state 0 */
    { {0,0}, {-2,0}, {1,0},  {-2,-1}, {1,2}   } }, /* CCW -> state 2 */
};

/* Where a piece appears: its box's top-left cell, board row/col, assuming
 * the common 10-column board TT_BOARD_W_STD. On another width use
 * col = (width - TT_BOX) / 2 instead of TT_SPAWN_COL. Row 0 is the top of
 * the playfield; a piece spawning here that already overlaps the stack is
 * the game-over condition. */
#define TT_BOARD_W_STD 10
#define TT_SPAWN_COL   3
#define TT_SPAWN_ROW   0

/* Scoring: lines cleared at once (1..4) times TT_SCORE_LINES[n-1] times the
 * level (1-based, so level 1 scores the base amount); dropping costs points
 * per row moved, TT_SCORE_SOFT for soft drop, TT_SCORE_HARD for hard. */
static const uint16_t TT_SCORE_LINES[4] = { 100, 300, 500, 800 };
#define TT_SCORE_SOFT 1
#define TT_SCORE_HARD 2

/* A level up every this many lines cleared, in total. */
#define TT_LINES_PER_LEVEL 10

/* Milliseconds between a falling piece's rows, by level, 0-based -- the
 * classic curve, its NES frame counts (at 60 Hz) turned into milliseconds.
 * A level past the end of this table runs at the last entry's speed. */
#define TT_LEVEL_MAX 29
static const uint16_t TT_LEVEL_MS[TT_LEVEL_MAX + 1] = {
  800, 717, 633, 550, 467, 383, 300, 217, 133, 100,
   83,  83,  83,  67,  67,  67,  50,  50,  50,  33,
   33,  33,  33,  33,  33,  33,  33,  33,  33,  17,
};

#endif
