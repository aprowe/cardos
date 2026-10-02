/* Quoridor's rules and computer player: apps/quoridor.h. */
#include "tinytest.h"
#include "apps/quoridor.h"

static QMove step(int r, int c) { QMove m; m.wall = 0; m.vert = 0; m.r = (int8_t)r; m.c = (int8_t)c; return m; }
static QMove wall(int r, int c, int vert) { QMove m; m.wall = 1; m.vert = (int8_t)vert; m.r = (int8_t)r; m.c = (int8_t)c; return m; }

void test_quoridor_starts_in_the_middle_eight_steps_from_home(void) {
  Quoridor q;
  q_init(&q);
  CHECK_EQ(q.r[0], 8); CHECK_EQ(q.c[0], 4);
  CHECK_EQ(q.r[1], 0); CHECK_EQ(q.c[1], 4);
  CHECK_EQ(q_path(&q, 0), 8);
  CHECK_EQ(q_path(&q, 1), 8);
  CHECK_EQ(q.turn, 0);
}

void test_quoridor_a_step_is_one_cell_and_turns_alternate(void) {
  Quoridor q;
  q_init(&q);
  CHECK_EQ(q_play(&q, step(6, 4)), -1);          /* two cells */
  CHECK_EQ(q_play(&q, step(7, 5)), -1);          /* diagonal */
  CHECK_EQ(q_play(&q, step(7, 4)), 0);
  CHECK_EQ(q.turn, 1);
  CHECK_EQ(q_play(&q, step(7, 4)), -1);          /* not player 1's pawn */
  CHECK_EQ(q_play(&q, step(1, 4)), 0);
}

void test_quoridor_a_wall_blocks_two_cells_and_costs_one(void) {
  Quoridor q;
  q_init(&q);
  CHECK_EQ(q_play(&q, wall(7, 3, 0)), 0);        /* under row 7, columns 3 and 4 */
  CHECK_EQ(q.left[0], 9);
  q.turn = 0;
  CHECK_EQ(q_play(&q, step(7, 4)), -1);          /* straight up is shut */
  CHECK(q_blocked(&q, 8, 3, 0));
  CHECK(!q_blocked(&q, 8, 5, 0));
  CHECK_EQ(q_path(&q, 0), 9);                    /* round it */
}

void test_quoridor_walls_cannot_cross_or_overlap(void) {
  Quoridor q;
  q_init(&q);
  CHECK(q_wall_ok(&q, 0, 4, 4, 0));
  q.h[4][4] = 1;
  CHECK(!q_wall_ok(&q, 0, 4, 4, 1));             /* crossing it */
  CHECK(!q_wall_ok(&q, 0, 4, 3, 0));             /* half over it */
  CHECK(!q_wall_ok(&q, 0, 4, 5, 0));
  CHECK(q_wall_ok(&q, 0, 4, 6, 0));              /* end to end is fine */
  CHECK(q_wall_ok(&q, 0, 3, 4, 1));              /* touching at a corner is fine */
  CHECK(!q_wall_ok(&q, 0, 8, 0, 0));             /* off the board */
}

void test_quoridor_no_wall_may_shut_a_pawn_in(void) {
  Quoridor q;
  int c;
  q_init(&q);
  /* A fence across the board above row 1 with one gap at column 8... */
  for (c = 0; c < 8; c += 2) q.h[0][c] = 1;      /* covers columns 0..7 */
  CHECK_EQ(q_path(&q, 0) < Q_FAR, 1);
  /* ...and nothing may close the gap. */
  q.v[0][7] = 1;                                 /* beside the gap, rows 0..1 */
  CHECK(!q_wall_ok(&q, 0, 1, 7, 0));             /* under it: shuts column 8 */
}

void test_quoridor_face_to_face_jumps_over_or_round(void) {
  Quoridor q;
  int8_t r[5], c[5];
  int n, i, over = 0, left = 0, right = 0;
  q_init(&q);
  q.r[0] = 5; q.c[0] = 4;
  q.r[1] = 4; q.c[1] = 4;
  n = q_steps(&q, 0, r, c);
  for (i = 0; i < n; i++) if (r[i] == 3 && c[i] == 4) over = 1;
  CHECK(over);
  /* A wall behind the other pawn: round it, either side. */
  q.h[3][4] = 1;
  n = q_steps(&q, 0, r, c);
  over = 0;
  for (i = 0; i < n; i++) {
    if (r[i] == 3 && c[i] == 4) over = 1;
    if (r[i] == 4 && c[i] == 3) left = 1;
    if (r[i] == 4 && c[i] == 5) right = 1;
  }
  CHECK(!over);
  CHECK(left && right);
}

void test_quoridor_reaching_the_far_row_wins(void) {
  Quoridor q;
  q_init(&q);
  q.r[0] = 1; q.c[0] = 0;
  CHECK_EQ(q_play(&q, step(0, 0)), 0);
  CHECK_EQ(q.winner, 0);
  CHECK_EQ(q_play(&q, step(1, 4)), -1);          /* over */
}

void test_quoridor_the_computer_walls_a_runner_and_walks_when_ahead(void) {
  Quoridor q;
  QMove m;
  q_init(&q);
  /* The human two from home, the computer far, in another column -- in the
   * same one, every wall in the human's way is in its own way too: it must
   * wall. */
  q.r[0] = 2; q.c[0] = 4;
  q.c[1] = 0;
  q.turn = 1;
  m = q_ai(&q, 1, 12345);
  CHECK_EQ(m.wall, 1);
  CHECK_EQ(q_play(&q, m), 0);
  CHECK(q_path(&q, 0) > 2);
  /* Itself two from home and well ahead: it walks. */
  q_init(&q);
  q.r[1] = 6;
  q.turn = 1;
  m = q_ai(&q, 1, 1);
  CHECK_EQ(m.wall, 0);
  CHECK_EQ(m.r, 7);
}

void test_quoridor_two_computers_finish_a_game_by_the_rules(void) {
  int game;
  for (game = 0; game < 20; game++) {
    Quoridor q;
    int turns = 0;
    q_init(&q);
    while (q.winner < 0 && turns < 400) {
      QMove m = q_ai(&q, q.turn, (uint32_t)(game * 2654435761u + turns * 40503u));
      CHECK_EQ(q_play(&q, m), 0);                /* never an illegal move */
      if (q.winner >= 0) break;
      turns++;
    }
    CHECK(q.winner >= 0);
    CHECK(q.left[0] >= 0 && q.left[1] >= 0);
  }
}
