/* The Traitors' rules and computer players: apps/traitors/traitors.h. */
#include <string.h>

#include "tinytest.h"
#include "apps/traitors/traitors.h"

static Tr T;

static void tr_seats(const char *roles) {     /* "TFFTFFF": who is what, all computers */
  int s;
  for (s = 0; s < TR_N; s++) T.role[s] = roles[s] == 'T' ? TR_TRAITOR : TR_FAITHFUL;
}

/* Play on until it is over or a person is asked; the steps it took. */
static int tr_run(int max) {
  int n = 0;
  while (T.phase != TR_OVER && tr_ready(&T) && n < max) { tr_next(&T); n++; }
  return n;
}

void test_traitors_a_new_castle_has_two_traitors_and_waits_for_the_people(void) {
  uint8_t human[TR_N] = { 1, 2, 0, 0, 0, 0, 0 };
  int s, traitors = 0;
  tr_new(&T, 7, human);
  for (s = 0; s < TR_N; s++) traitors += T.role[s] == TR_TRAITOR;
  CHECK_EQ(traitors, TR_TRAITORS);
  CHECK_EQ(T.phase, TR_ROLES);
  CHECK_EQ(tr_count(&T, -1), 7);
  CHECK_EQ(tr_need(&T, 0), TR_ASK_ACK);
  CHECK_EQ(tr_need(&T, 1), TR_ASK_ACK);
  CHECK_EQ(tr_need(&T, 2), TR_ASK_NONE);          /* computers do not need telling */
  CHECK(!tr_ready(&T));
  tr_act(&T, 0, 0);
  CHECK(!tr_ready(&T));
  tr_act(&T, 1, 0);
  CHECK(tr_ready(&T));
  tr_next(&T);
  CHECK_EQ(T.phase, TR_PROPOSE);
}

void test_traitors_computers_alone_always_finish_and_both_sides_win_some(void) {
  int seed, wins[2] = { 0, 0 }, rounds = 0;
  for (seed = 1; seed <= 400; seed++) {
    tr_new(&T, (uint32_t)seed, 0);
    CHECK(tr_run(400) < 400);
    CHECK_EQ(T.phase, TR_OVER);
    CHECK(T.winner == TR_FAITHFUL || T.winner == TR_TRAITOR);
    if (T.winner >= 0) wins[T.winner]++;
    rounds += T.round;
  }
  /* Neither side should be hopeless: each wins at least a fifth. */
  CHECK(wins[TR_FAITHFUL] > 80);
  CHECK(wins[TR_TRAITOR] > 80);
  CHECK(rounds / 400 >= 2);
}

void test_traitors_a_crew_is_the_right_size_with_its_leader_in_it(void) {
  uint8_t human[TR_N] = { 1, 0, 0, 0, 0, 0, 0 };
  tr_new(&T, 3, human);
  tr_act(&T, 0, 0);
  tr_next(&T);
  T.leader = 0;
  tr_enter(&T, TR_PROPOSE);
  CHECK_EQ(tr_need(&T, 0), TR_ASK_CREW);
  CHECK_EQ(tr_crew_size(&T), 3);
  CHECK_EQ(tr_act(&T, 0, 0x06), 0);               /* the leader not in it */
  CHECK_EQ(tr_act(&T, 0, 0x03), 0);               /* two, not three */
  CHECK_EQ(tr_act(&T, 0, 0x0F), 0);               /* four */
  T.alive[2] = 0;
  CHECK_EQ(tr_act(&T, 0, 0x07), 0);               /* someone no longer here */
  T.alive[2] = 1;
  CHECK_EQ(tr_act(&T, 0, 0x07), 1);
  CHECK(tr_ready(&T));
  tr_next(&T);
  CHECK_EQ(T.phase, TR_VOTE);
  CHECK_EQ(tr_need(&T, 0), TR_ASK_YESNO);
}

void test_traitors_a_faithfuls_lead_is_gold_and_one_lead_spoils_it(void) {
  uint8_t human[TR_N] = { 1, 0, 0, 0, 0, 0, 0 };
  tr_new(&T, 5, human);
  tr_seats("FTFFTFF");
  T.crew = 0x07;                                   /* 0, 1 (a Traitor), 2 */
  tr_enter(&T, TR_VAULT);
  T.coin[1] = 0; T.coin[2] = 0;                    /* the Traitor plays it straight */
  CHECK_EQ(tr_need(&T, 0), TR_ASK_COIN);
  tr_act(&T, 0, 1);                                /* a Faithful cannot */
  CHECK_EQ(T.coin[0], 0);
  tr_next(&T);
  CHECK_EQ(T.phase, TR_RESULT);
  CHECK_EQ(T.lead, 0);
  CHECK_EQ(T.clean, 1);
  CHECK_EQ(T.pot, 3 * TR_GOLD);

  tr_enter(&T, TR_VAULT);
  T.coin[1] = 1; T.coin[2] = 0;
  tr_act(&T, 0, 0);
  {
    int before_out = T.sus[5][1], before_in = T.sus[2][1], before_mate = T.sus[2][0];
    tr_next(&T);
    CHECK_EQ(T.lead, 1);
    CHECK_EQ(T.spoiled, 1);
    /* Outside the crew, one lead coin in three is a third on each of them;
     * inside, an honest member knows its own was gold, so it is a half on
     * each of the other two. */
    CHECK_EQ(T.sus[5][1] - before_out, 420 / 3);
    CHECK_EQ(T.sus[2][1] - before_in, 420 / 2);
    CHECK_EQ(T.sus[2][0] - before_mate, 420 / 2);
  }
}

void test_traitors_the_third_crew_turned_down_spoils_the_mission(void) {
  tr_new(&T, 9, 0);
  T.phase = TR_PROPOSE;
  T.rejects = 2;
  T.crew = 0x07;
  tr_enter(&T, TR_VOTE);
  /* On the last try every computer says yes, so it goes; force a no to see the rule. */
  CHECK_EQ(tr_ai_vote(&T, 3), 1);
  {
    int s;
    for (s = 0; s < TR_N; s++) T.vote[s] = 0;
  }
  tr_next(&T);
  CHECK_EQ(T.phase, TR_VOTED);
  CHECK(!T.sent);
  tr_next(&T);
  CHECK_EQ(T.phase, TR_RESULT);
  CHECK_EQ(T.lead, -1);
  CHECK_EQ(T.spoiled, 1);
  CHECK_EQ(T.rejects, 0);
}

void test_traitors_a_tie_at_the_table_banishes_nobody(void) {
  int s;
  tr_new(&T, 11, 0);
  tr_seats("TTFFFFF");
  tr_enter(&T, TR_TABLE);
  /* 3 on seat 0, 3 on seat 2, seat 0 itself on 2: a tie. */
  T.vote[0] = 2; T.vote[1] = 2; T.vote[2] = 0; T.vote[3] = 0; T.vote[4] = 0; T.vote[5] = 2; T.vote[6] = 1;
  tr_next(&T);
  CHECK_EQ(T.phase, TR_TABLED);
  CHECK_EQ(T.banished, -1);
  CHECK_EQ(tr_count(&T, -1), 7);
  /* A clear one goes, and is shown. */
  tr_enter(&T, TR_TABLE);
  for (s = 0; s < TR_N; s++) T.vote[s] = 1;
  T.vote[1] = 0;
  tr_next(&T);
  CHECK_EQ(T.banished, 1);
  CHECK(!T.alive[1]);
  CHECK(T.shown[1]);
  CHECK_EQ(T.accused[3], 1);
}

void test_traitors_the_night_spares_the_shielded_and_a_person_chooses(void) {
  uint8_t human[TR_N] = { 1, 0, 0, 0, 0, 0, 0 };
  tr_new(&T, 13, human);
  tr_seats("TTFFFFF");                              /* the person is a Traitor */
  T.shield = 2;
  tr_enter(&T, TR_NIGHT);
  CHECK_EQ(tr_need(&T, 0), TR_ASK_SEAT);
  CHECK_EQ(tr_need(&T, 1), TR_ASK_NONE);          /* the computer Traitor defers */
  CHECK_EQ(tr_act(&T, 0, 2), 0);                  /* shielded */
  CHECK_EQ(tr_act(&T, 0, 1), 0);                  /* a Traitor */
  CHECK_EQ(tr_act(&T, 0, 4), 1);
  tr_next(&T);
  CHECK_EQ(T.phase, TR_BREAKFAST);
  CHECK_EQ(T.murdered, 4);
  CHECK(!T.alive[4]);
  CHECK_EQ(T.shield, -1);                          /* spent */
}

void test_traitors_the_computer_murders_whoever_named_a_traitor(void) {
  tr_new(&T, 17, 0);
  tr_seats("TFFTFFF");
  T.accused[5] = 3;
  tr_enter(&T, TR_NIGHT);
  tr_next(&T);
  CHECK_EQ(T.murdered, 5);
}

void test_traitors_who_wins(void) {
  tr_new(&T, 19, 0);
  tr_seats("TTFFFFF");
  CHECK(!tr_check(&T));
  T.alive[0] = T.alive[1] = 0;
  CHECK(tr_check(&T));
  CHECK_EQ(T.winner, TR_FAITHFUL);

  tr_new(&T, 19, 0);
  tr_seats("TTFFFFF");
  T.alive[2] = T.alive[3] = T.alive[4] = 0;        /* two and two */
  CHECK(tr_check(&T));
  CHECK_EQ(T.winner, TR_TRAITOR);

  tr_new(&T, 19, 0);
  tr_seats("TTFFFFF");
  T.spoiled = TR_SPOIL_WIN;
  CHECK(tr_check(&T));
  CHECK_EQ(T.winner, TR_TRAITOR);
}

void test_traitors_a_view_keeps_secrets_and_survives_the_wire(void) {
  uint8_t human[TR_N] = { 1, 2, 0, 0, 0, 0, 0 };
  uint8_t wire[TV_WIRE];
  TrView v, back;
  int n;
  tr_new(&T, 29, human);
  tr_seats("FFTTFFF");
  /* A Faithful knows nobody; a Traitor knows both. */
  tv_from(&T, 1, &v);
  CHECK_EQ(v.traitors, 0);
  CHECK_EQ(v.my_role, TR_FAITHFUL);
  tv_from(&T, 2, &v);
  CHECK_EQ(v.traitors, 0x0C);
  /* While voting: who has voted, never how -- but your own. */
  T.crew = 0x07;
  tr_enter(&T, TR_VOTE);
  tr_act(&T, 1, 1);
  tv_from(&T, 0, &v);
  CHECK_EQ(v.need, TR_ASK_YESNO);
  CHECK(v.voted & 0x02);
  CHECK_EQ(v.yes, 0);
  tv_from(&T, 1, &v);
  CHECK_EQ(v.yes, 0x02);
  CHECK_EQ(v.need, TR_ASK_NONE);
  tr_act(&T, 0, 0);
  tr_next(&T);
  tv_from(&T, 1, &v);                               /* shown together */
  CHECK_EQ(v.phase, TR_VOTED);
  CHECK(v.yes & 0x02);
  CHECK(!(v.yes & 0x01));
  /* Through the wire and back, the same. */
  n = tv_pack(&v, wire);
  CHECK(n <= TV_WIRE);
  CHECK_EQ(tv_unpack(wire, n, &back), 0);
  CHECK(!memcmp(&v, &back, sizeof v));
  wire[0] = 'X';
  CHECK_EQ(tv_unpack(wire, n, &back), -1);
}

void test_traitors_a_traitor_votes_with_the_herd_against_a_faithful(void) {
  int s;
  tr_new(&T, 23, 0);
  tr_seats("TFFTFFF");
  for (s = 0; s < TR_N; s++) { T.sus[s][5] = 900; T.sus[s][6] = 100; }
  T.sus[5][5] = 0;
  CHECK_EQ(tr_ai_table(&T, 0), 5);
  /* A Faithful with its own suspicion goes its own way. */
  T.sus[2][6] = 1500;
  CHECK_EQ(tr_ai_table(&T, 2), 6);
}
