/* The Traitors: the rules, and the computer players. No screen, no API, no
 * libc -- apps/traitors/traitors.c draws it and apps/traitors/test/test_traitors.c plays
 * it on the host.
 *
 * An original hidden-role game in the manner of the television show: seven
 * at a castle, two of them Traitors who know each other, five Faithful who
 * know only themselves. A round is
 *
 *   mission     the leader (each round the next one alive) names a crew --
 *               three while six or more live, else two -- and everyone votes
 *               on it, the votes shown together. A majority of yes sends the
 *               crew; otherwise the next leader tries, and the third crew
 *               turned down in a row spoils the mission by itself.
 *   the vault   each of the crew drops a coin in secret: a Faithful gold, a
 *               Traitor gold or lead. The chest is opened and the coins
 *               counted, never told apart: one lead coin spoils the mission.
 *               A clean mission puts gold in the pot and gives its leader a
 *               shield to hand to someone else.
 *   the round   first the talk: people say what they think, the computers
 *   table       answer, and whoever is persuaded leans (tr_lean). When every
 *               person still in the game says they are ready, the computers
 *               decide, and everyone names someone; the most named is
 *               banished and their role shown (a tie banishes nobody).
 *   night       the Traitors murder a Faithful who holds no shield; the
 *               shield is spent either way. Breakfast says who.
 *
 * The Faithful win when no Traitor is left. The Traitors win when they are
 * as many as the Faithful, or when two missions have been spoiled.
 *
 * Seats 0..TR_N-1. human[s] is 0 for a computer, else which device plays it
 * (1 the host, 2 the guest). The host runs this; the guest is shown it.
 *
 * Driving it: tr_new, then each pass of the loop
 *   tr_need(t, seat)      what that seat must give now (TR_ASK_*), or 0
 *   tr_act(t, seat, v)    a person's answer
 *   tr_ready(t)           everything this phase wants is in
 *   tr_next(t)            settle the phase and enter the next one
 * The computer players answer as each phase is entered, so a person's
 * answer is the only thing ever waited for, and the app may pause between
 * phases as long as it likes for the show.
 *
 * The computer: every player keeps a suspicion of every other (sus[o][t],
 * 0 up), moved by what it saw -- lead in a crew it was not in counts against
 * the whole crew, lead in a crew it was in counts against the others; a vote
 * for a crew that came back spoiled, a vote to banish one who proved
 * Faithful. A small bias per pair, rolled at the start, is its character.
 * A Faithful acts on its own suspicion. A Traitor knows the truth and acts
 * on the herd's -- the mean of everyone's -- to blend in: it votes with the
 * table against the most suspected Faithful, and now and then throws a
 * fellow Traitor to the wolves when the herd has him anyway.
 */
#ifndef CARDOS_TRAITORS_H
#define CARDOS_TRAITORS_H

#include <stdint.h>

#ifndef TR_OPT
#define TR_OPT
#endif

#define TR_N          7
#define TR_TRAITORS   2
#define TR_SPOIL_WIN  2          /* spoiled missions that win it for the Traitors */
#define TR_REJECTS    3          /* crews turned down in a row that spoil the mission */
#define TR_GOLD       10         /* to the pot, a clean mission, each of the crew */

enum { TR_FAITHFUL = 0, TR_TRAITOR = 1 };

enum {
  TR_ROLES = 0,     /* each person sees their role */
  TR_PROPOSE,       /* the leader names a crew */
  TR_VOTE,          /* everyone votes on it */
  TR_VOTED,         /* the votes shown */
  TR_VAULT,         /* the crew drop their coins */
  TR_RESULT,        /* the chest opened */
  TR_SHIELD,        /* a clean mission's leader hands the shield on */
  TR_TABLE,         /* the round table: everyone names someone */
  TR_TABLED,        /* who was banished, and what they were */
  TR_NIGHT,         /* the Traitors choose */
  TR_BREAKFAST,     /* who was murdered */
  TR_OVER
};

/* What a seat is asked for. */
enum {
  TR_ASK_NONE = 0,
  TR_ASK_ACK,       /* look, and go on */
  TR_ASK_CREW,      /* a bit mask of tr_crew_size() seats, the leader among them */
  TR_ASK_YESNO,     /* 1 yes, 0 no */
  TR_ASK_COIN,      /* 0 gold, 1 lead (a Faithful's lead is gold) */
  TR_ASK_SEAT,      /* a seat: the shield's, the banished's, the murdered's */
  TR_ASK_TALK       /* the round table is talking: say something, or 1 when ready to vote */
};

typedef struct {
  uint32_t rng;
  int      phase, round;
  int8_t   role[TR_N];
  uint8_t  alive[TR_N], human[TR_N], shown[TR_N];   /* shown: role made public */
  uint8_t  acked[TR_N];
  uint8_t  ready[TR_N];                /* the table talk: this person is ready to vote */
  uint8_t  talk_open;                  /* the round table is still talking */
  int      leader, rejects, spoiled, clean, pot;
  uint8_t  crew;                       /* bit mask */
  int8_t   vote[TR_N];                 /* crew vote 0/1, table vote a seat; -1 none */
  int8_t   coin[TR_N];                 /* 0 gold, 1 lead; -1 none */
  int8_t   pick[TR_N];                 /* night: each Traitor's choice */
  int8_t   accused[TR_N];              /* the seat each last named at the table */
  int      lead;                       /* lead coins in the last chest */
  int      sent;                       /* the last vote sent the crew */
  int      shield;                     /* who holds it, or -1 */
  int      banished, murdered;         /* this round's, or -1 */
  int      winner;                     /* -1 playing, else TR_FAITHFUL or TR_TRAITOR */
  int16_t  sus[TR_N][TR_N];
} Tr;

/* ---- small things ---- */

static TR_OPT uint32_t tr_rand(Tr *t) {
  uint32_t x = t->rng ? t->rng : 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  t->rng = x;
  return x;
}
static TR_OPT int tr_roll(Tr *t, int n) { return n > 0 ? (int)(tr_rand(t) % (uint32_t)n) : 0; }

static TR_OPT int tr_count(const Tr *t, int role) {
  int s, n = 0;
  for (s = 0; s < TR_N; s++) if (t->alive[s] && (role < 0 || t->role[s] == role)) n++;
  return n;
}
static TR_OPT int tr_bits(uint8_t m) { int n = 0; while (m) { n += m & 1; m >>= 1; } return n; }
static TR_OPT int tr_in(uint8_t m, int s) { return (m >> s) & 1; }

/* The crew this round: three while six or more live, else two. */
static TR_OPT int tr_crew_size(const Tr *t) { return tr_count(t, -1) >= 6 ? 3 : 2; }

static TR_OPT int tr_next_alive(const Tr *t, int s) {
  int i;
  for (i = 1; i <= TR_N; i++) if (t->alive[(s + i) % TR_N]) return (s + i) % TR_N;
  return s;
}

/* The herd's suspicion of t: everyone else's, averaged. */
static TR_OPT int tr_herd(const Tr *t, int target) {
  int o, sum = 0, n = 0;
  for (o = 0; o < TR_N; o++)
    if (o != target && t->alive[o]) { sum += t->sus[o][target]; n++; }
  return n ? sum / n : 0;
}

/* What a computer thinks of `target`: a Faithful its suspicion; a Traitor
 * what the herd thinks, which is what it plays to. */
static TR_OPT int tr_view(const Tr *t, int o, int target) {
  return t->role[o] == TR_TRAITOR ? tr_herd(t, target) : t->sus[o][target];
}

static TR_OPT void tr_bump(Tr *t, int o, int target, int by) {
  int v;
  if (o == target) return;
  v = t->sus[o][target] + by;
  if (v < 0) v = 0;
  if (v > 2000) v = 2000;
  t->sus[o][target] = (int16_t)v;
}

/* ---- what is wanted ---- */

static TR_OPT int tr_need(const Tr *t, int s) {
  if (s < 0 || s >= TR_N) return TR_ASK_NONE;
  switch (t->phase) {
  case TR_ROLES: case TR_VOTED: case TR_RESULT: case TR_TABLED: case TR_BREAKFAST:
    return t->human[s] && !t->acked[s] ? TR_ASK_ACK : TR_ASK_NONE;
  case TR_PROPOSE: return s == t->leader && t->crew == 0 ? TR_ASK_CREW : TR_ASK_NONE;
  case TR_VOTE:    return t->alive[s] && t->vote[s] < 0 ? TR_ASK_YESNO : TR_ASK_NONE;
  case TR_VAULT:   return tr_in(t->crew, s) && t->coin[s] < 0 ? TR_ASK_COIN : TR_ASK_NONE;
  case TR_SHIELD:  return s == t->leader && t->shield < 0 ? TR_ASK_SEAT : TR_ASK_NONE;
  case TR_TABLE:
    if (!t->alive[s]) return TR_ASK_NONE;
    if (t->talk_open) return t->human[s] && !t->ready[s] ? TR_ASK_TALK : TR_ASK_NONE;
    return t->vote[s] < 0 ? TR_ASK_SEAT : TR_ASK_NONE;
  case TR_NIGHT:
    /* A person who is a Traitor chooses; the computer Traitors defer to them. */
    if (!t->alive[s] || t->role[s] != TR_TRAITOR || t->pick[s] >= 0) return TR_ASK_NONE;
    return TR_ASK_SEAT;
  }
  return TR_ASK_NONE;
}

static TR_OPT int tr_ready(const Tr *t) {
  int s;
  if (t->phase == TR_OVER || (t->phase == TR_TABLE && t->talk_open)) return 0;
  for (s = 0; s < TR_N; s++) if (tr_need(t, s) != TR_ASK_NONE) return 0;
  return 1;
}

/* Is `target` one `s` may choose in this phase? */
static TR_OPT int tr_can_pick(const Tr *t, int s, int target) {
  if (target < 0 || target >= TR_N || !t->alive[target]) return 0;
  switch (t->phase) {
  case TR_SHIELD: return target != s;
  case TR_TABLE:  return target != s;
  case TR_NIGHT:  return t->role[target] == TR_FAITHFUL && target != t->shield;
  }
  return 0;
}

/* What the talk did: `o` now suspects `target` more (by > 0) or less. The
 * app calls it with what Claude says changed minds, or, offline, for a name
 * a person said. A computer Traitor's suspicion moves too: it plays to the
 * herd, and it is part of the herd. */
static TR_OPT void tr_lean(Tr *t, int o, int target, int by);

/* ---- the computer's choices ---- */

/* The seat among `cands` (a mask) that `o` scores highest (or lowest), ties
 * broken by the dice. -1 if none. */
static TR_OPT int tr_best(Tr *t, int o, uint8_t cands, int lowest, int herd) {
  int s, best = -1, bv = 0, ties = 0;
  for (s = 0; s < TR_N; s++) {
    int v;
    if (!tr_in(cands, s)) continue;
    v = herd ? tr_herd(t, s) : tr_view(t, o, s);
    if (lowest) v = -v;
    if (best < 0 || v > bv) { best = s; bv = v; ties = 1; }
    else if (v == bv && tr_roll(t, ++ties) == 0) best = s;
  }
  return best;
}

static TR_OPT uint8_t tr_alive_mask(const Tr *t) {
  uint8_t m = 0;
  int s;
  for (s = 0; s < TR_N; s++) if (t->alive[s]) m |= (uint8_t)(1 << s);
  return m;
}
static TR_OPT uint8_t tr_role_mask(const Tr *t, int role) {
  uint8_t m = 0;
  int s;
  for (s = 0; s < TR_N; s++) if (t->alive[s] && t->role[s] == role) m |= (uint8_t)(1 << s);
  return m;
}

static TR_OPT uint8_t tr_ai_crew(Tr *t, int o) {
  uint8_t crew = (uint8_t)(1 << o), pool = tr_alive_mask(t) & (uint8_t)~(1 << o);
  int k = tr_crew_size(t);
  /* A Traitor takes the trusted Faithful along: a Traitor beside it would
   * share the blame. */
  if (t->role[o] == TR_TRAITOR) pool &= tr_role_mask(t, TR_FAITHFUL);
  while (tr_bits(crew) < k && pool) {
    int s = tr_roll(t, 10) < 2 ? -1 : tr_best(t, o, pool, 1, 0);
    if (s < 0) {                                   /* now and then, a hunch */
      int n = tr_roll(t, tr_bits(pool));
      for (s = 0; s < TR_N; s++) if (tr_in(pool, s) && n-- == 0) break;
    }
    crew |= (uint8_t)(1 << s);
    pool &= (uint8_t)~(1 << s);
  }
  return crew;
}

static TR_OPT int tr_ai_vote(Tr *t, int o) {
  int s, worst = 0;
  if (t->rejects == TR_REJECTS - 1) return 1;      /* a third no spoils it for everyone */
  if (t->role[o] == TR_TRAITOR) {
    if (t->crew & tr_role_mask(t, TR_TRAITOR)) return tr_roll(t, 10) < 9;
    return tr_roll(t, 2);
  }
  if (tr_in(t->crew, o)) return 1;
  for (s = 0; s < TR_N; s++)
    if (tr_in(t->crew, s) && t->sus[o][s] > worst) worst = t->sus[o][s];
  return worst < 350;
}

static TR_OPT int tr_ai_coin(Tr *t, int o) {
  int s;
  if (t->role[o] != TR_TRAITOR) return 0;
  if (t->spoiled == TR_SPOIL_WIN - 1) return 1;    /* this one wins it */
  /* Two Traitors in a crew: the first spoils, the other keeps its hands clean. */
  for (s = 0; s < o; s++)
    if (tr_in(t->crew, s) && t->role[s] == TR_TRAITOR && !t->human[s]) return 0;
  return tr_roll(t, 100) < 65;
}

static TR_OPT int tr_ai_shield(Tr *t, int o) {
  uint8_t pool = tr_alive_mask(t) & (uint8_t)~(1 << o);
  if (t->role[o] == TR_TRAITOR) {
    uint8_t mates = tr_role_mask(t, TR_TRAITOR) & pool;
    if (mates && tr_roll(t, 2)) return tr_best(t, o, mates, 1, 1);   /* wasted on a Traitor */
    return tr_best(t, o, pool & tr_role_mask(t, TR_FAITHFUL), 0, 1);  /* on one it would not kill */
  }
  return tr_best(t, o, pool, 1, 0);
}

static TR_OPT int tr_ai_table(Tr *t, int o) {
  uint8_t pool = tr_alive_mask(t) & (uint8_t)~(1 << o);
  if (t->role[o] == TR_TRAITOR) {
    int herd = tr_best(t, o, pool, 0, 1);
    /* The herd has a fellow Traitor anyway: join in, and be the one who was right. */
    if (herd >= 0 && t->role[herd] == TR_TRAITOR && tr_roll(t, 2)) return herd;
    return tr_best(t, o, pool & tr_role_mask(t, TR_FAITHFUL), 0, 1);
  }
  return tr_best(t, o, pool, 0, 0);
}

/* The Traitors' victim: the Faithful closest to the truth -- the one who
 * named a Traitor last, or suspects one most. */
static TR_OPT int tr_ai_murder(Tr *t) {
  int s, x, best = -1, bv = -1;
  for (s = 0; s < TR_N; s++) {
    int v = 0, most = 0;
    if (t->phase != TR_NIGHT ? !(t->alive[s] && t->role[s] == TR_FAITHFUL && s != t->shield)
                             : !tr_can_pick(t, -1, s)) continue;
    for (x = 0; x < TR_N; x++)
      if (t->alive[x] && t->role[x] == TR_TRAITOR && t->sus[s][x] > most) most = t->sus[s][x];
    v = most;
    if (t->accused[s] >= 0 && t->role[(int)t->accused[s]] == TR_TRAITOR) v += 2000;
    v = v * 4 + tr_roll(t, 4);
    if (v > bv) { bv = v; best = s; }
  }
  return best;
}

/* ---- the phases ---- */

static TR_OPT void tr_clear_votes(Tr *t) {
  int s;
  for (s = 0; s < TR_N; s++) { t->vote[s] = -1; t->coin[s] = -1; t->pick[s] = -1; t->acked[s] = 0; }
}

/* Enter a phase: clear what it asks for, and answer for the computers. */
static TR_OPT void tr_enter(Tr *t, int phase) {
  int s;
  t->phase = phase;
  tr_clear_votes(t);
  switch (phase) {
  case TR_PROPOSE:
    t->crew = 0;
    if (!t->human[t->leader]) t->crew = tr_ai_crew(t, t->leader);
    break;
  case TR_VOTE:
    for (s = 0; s < TR_N; s++) if (t->alive[s] && !t->human[s]) t->vote[s] = (int8_t)tr_ai_vote(t, s);
    break;
  case TR_VAULT:
    for (s = 0; s < TR_N; s++) if (tr_in(t->crew, s) && !t->human[s]) t->coin[s] = (int8_t)tr_ai_coin(t, s);
    break;
  case TR_SHIELD:
    if (!t->human[t->leader]) t->shield = tr_ai_shield(t, t->leader);
    break;
  case TR_TABLE:
    /* The talk first, while a person is there to talk; the computers decide
     * when it closes (tr_close_talk). */
    t->banished = -1;
    t->talk_open = 0;
    for (s = 0; s < TR_N; s++) {
      t->ready[s] = 0;
      if (t->alive[s] && t->human[s]) t->talk_open = 1;
    }
    if (!t->talk_open)
      for (s = 0; s < TR_N; s++) if (t->alive[s] && !t->human[s]) t->vote[s] = (int8_t)tr_ai_table(t, s);
    break;
  case TR_NIGHT: {
    int people = 0, v;
    t->murdered = -1;
    for (s = 0; s < TR_N; s++)
      if (t->alive[s] && t->role[s] == TR_TRAITOR && t->human[s]) people++;
    v = tr_ai_murder(t);
    for (s = 0; s < TR_N; s++)
      if (t->alive[s] && t->role[s] == TR_TRAITOR && !t->human[s])
        t->pick[s] = (int8_t)(people ? TR_N : v);  /* TR_N: deferring to a person */
    if (v < 0)                                      /* nobody to kill: the night passes */
      for (s = 0; s < TR_N; s++) if (t->role[s] == TR_TRAITOR) t->pick[s] = TR_N;
    break;
  }
  }
}

static TR_OPT void tr_new(Tr *t, uint32_t seed, const uint8_t human[TR_N]) {
  int s, o, n;
  uint8_t *p = (uint8_t *)t;
  for (s = 0; s < (int)sizeof *t; s++) p[s] = 0;
  t->rng = seed ? seed : 1;
  for (s = 0; s < TR_N; s++) {
    t->alive[s] = 1;
    t->human[s] = human ? human[s] : 0;
    t->role[s] = TR_FAITHFUL;
    t->accused[s] = -1;
  }
  for (n = 0; n < TR_TRAITORS;) {
    s = tr_roll(t, TR_N);
    if (t->role[s] != TR_TRAITOR) { t->role[s] = TR_TRAITOR; n++; }
  }
  for (o = 0; o < TR_N; o++)
    for (s = 0; s < TR_N; s++) t->sus[o][s] = (int16_t)(o == s ? 0 : 100 + tr_roll(t, 120));
  t->leader = tr_roll(t, TR_N);
  t->shield = t->banished = t->murdered = -1;
  t->winner = -1;
  t->round = 1;
  tr_enter(t, TR_ROLES);
}

static TR_OPT void tr_lean(Tr *t, int o, int target, int by) {
  if (o < 0 || o >= TR_N || target < 0 || target >= TR_N || t->human[o]) return;
  tr_bump(t, o, target, by);
}

/* The talk is over: the computers make up their minds, and the vote is on. */
static TR_OPT void tr_close_talk(Tr *t) {
  int s;
  if (t->phase != TR_TABLE || !t->talk_open) return;
  t->talk_open = 0;
  for (s = 0; s < TR_N; s++) if (t->alive[s] && !t->human[s]) t->vote[s] = (int8_t)tr_ai_table(t, s);
}

/* A person's answer. 1 taken, 0 not wanted or not allowed. */
static TR_OPT int tr_act(Tr *t, int s, int v) {
  switch (tr_need(t, s)) {
  case TR_ASK_ACK:
    t->acked[s] = 1;
    return 1;
  case TR_ASK_CREW:
    if (v < 0 || v >= (1 << TR_N) || !tr_in((uint8_t)v, s)) return 0;
    if (tr_bits((uint8_t)v) != tr_crew_size(t) || ((uint8_t)v & ~tr_alive_mask(t))) return 0;
    t->crew = (uint8_t)v;
    return 1;
  case TR_ASK_YESNO:
    t->vote[s] = (int8_t)(v ? 1 : 0);
    return 1;
  case TR_ASK_COIN:
    t->coin[s] = (int8_t)(v && t->role[s] == TR_TRAITOR ? 1 : 0);
    return 1;
  case TR_ASK_TALK: {
    int k, all = 1;
    if (v != 1) return 0;
    t->ready[s] = 1;
    for (k = 0; k < TR_N; k++) if (t->alive[k] && t->human[k] && !t->ready[k]) all = 0;
    if (all) tr_close_talk(t);
    return 1;
  }
  case TR_ASK_SEAT:
    if (!tr_can_pick(t, s, v)) return 0;
    if (t->phase == TR_SHIELD) t->shield = v;
    else if (t->phase == TR_TABLE) t->vote[s] = (int8_t)v;
    else t->pick[s] = (int8_t)v;
    return 1;
  }
  return 0;
}

/* Who wins now, if anyone: set winner and say so. */
static TR_OPT int tr_check(Tr *t) {
  int tr = tr_count(t, TR_TRAITOR), fa = tr_count(t, TR_FAITHFUL);
  if (tr == 0) t->winner = TR_FAITHFUL;
  else if (tr >= fa || t->spoiled >= TR_SPOIL_WIN) t->winner = TR_TRAITOR;
  return t->winner >= 0;
}

/* What everyone learnt from a chest with `lead` lead coins. */
static TR_OPT void tr_learn_chest(Tr *t) {
  int o, s, k = tr_bits(t->crew);
  for (o = 0; o < TR_N; o++) {
    int mine = tr_in(t->crew, o) && t->coin[o] == 0;   /* knows its own was gold */
    int others = mine ? k - 1 : k;
    for (s = 0; s < TR_N; s++) {
      if (s == o || !tr_in(t->crew, s)) continue;
      if (t->lead) tr_bump(t, o, s, others ? 420 * t->lead / others : 0);
      else tr_bump(t, o, s, -80);
    }
    if (t->lead) {
      if (t->leader != o) tr_bump(t, o, t->leader, 90);
      for (s = 0; s < TR_N; s++) if (t->vote[s] == 1 && s != o) tr_bump(t, o, s, 50);
    }
  }
}

static TR_OPT void tr_learn_banish(Tr *t) {
  int o, s, b = t->banished;
  if (b < 0) return;
  for (o = 0; o < TR_N; o++)
    for (s = 0; s < TR_N; s++)
      if (t->accused[s] == b) tr_bump(t, o, s, t->role[b] == TR_TRAITOR ? -150 : 110);
}

/* Settle the phase that is ready and enter the next. */
static TR_OPT void tr_next(Tr *t) {
  int s, yes = 0, no = 0;
  if (!tr_ready(t)) return;
  switch (t->phase) {
  case TR_ROLES:
    tr_enter(t, TR_PROPOSE);
    break;
  case TR_PROPOSE:
    tr_enter(t, TR_VOTE);
    break;
  case TR_VOTE: {
    /* Kept through VOTED, for the screen to show who voted how. */
    int8_t keep[TR_N];
    for (s = 0; s < TR_N; s++) {
      keep[s] = t->vote[s];
      if (t->vote[s] == 1) yes++; else if (t->vote[s] == 0) no++;
    }
    t->sent = yes > no;
    tr_enter(t, TR_VOTED);
    for (s = 0; s < TR_N; s++) t->vote[s] = keep[s];
    break;
  }
  case TR_VOTED:
    if (t->sent) { t->rejects = 0; tr_enter(t, TR_VAULT); break; }
    t->leader = tr_next_alive(t, t->leader);
    if (++t->rejects >= TR_REJECTS) {            /* turned down once too often */
      t->rejects = 0;
      t->lead = -1;                              /* -1: no chest, it never left */
      t->spoiled++;
      t->crew = 0;
      tr_enter(t, TR_RESULT);
      tr_check(t);
      if (t->winner >= 0) t->phase = TR_OVER;
      break;
    }
    tr_enter(t, TR_PROPOSE);
    break;
  case TR_VAULT: {
    int8_t coins[TR_N], votes[TR_N];
    t->lead = 0;
    for (s = 0; s < TR_N; s++) if (t->coin[s] == 1) t->lead++;
    if (t->lead) t->spoiled++;
    else { t->clean++; t->pot += TR_GOLD * tr_bits(t->crew); }
    /* What was in the chest stays until the next phase has used it. */
    for (s = 0; s < TR_N; s++) { coins[s] = t->coin[s]; votes[s] = t->vote[s]; }
    tr_enter(t, TR_RESULT);
    for (s = 0; s < TR_N; s++) { t->coin[s] = coins[s]; t->vote[s] = votes[s]; }
    tr_learn_chest(t);
    if (tr_check(t)) t->phase = TR_OVER;
    break;
  }
  case TR_RESULT:
    if (t->lead == 0) { t->shield = -1; tr_enter(t, TR_SHIELD); }
    else tr_enter(t, TR_TABLE);
    break;
  case TR_SHIELD:
    tr_enter(t, TR_TABLE);
    break;
  case TR_TABLE: {
    int count[TR_N], best = -1, bv = 0, tie = 0;
    int8_t votes[TR_N];
    for (s = 0; s < TR_N; s++) count[s] = 0;
    for (s = 0; s < TR_N; s++)
      if (t->alive[s] && t->vote[s] >= 0) { count[(int)t->vote[s]]++; t->accused[s] = t->vote[s]; }
    for (s = 0; s < TR_N; s++) {
      if (count[s] > bv) { bv = count[s]; best = s; tie = 0; }
      else if (count[s] == bv && bv > 0) tie = 1;
    }
    for (s = 0; s < TR_N; s++) votes[s] = t->vote[s];
    tr_enter(t, TR_TABLED);
    for (s = 0; s < TR_N; s++) t->vote[s] = votes[s];
    t->banished = tie ? -1 : best;
    if (t->banished >= 0) {
      t->alive[t->banished] = 0;
      t->shown[t->banished] = 1;
      if (t->shield == t->banished) t->shield = -1;
      tr_learn_banish(t);
    }
    if (tr_check(t)) t->phase = TR_OVER;
    break;
  }
  case TR_TABLED:
    tr_enter(t, TR_NIGHT);
    break;
  case TR_NIGHT: {
    int v = -1;
    /* A person's choice first, then the computers'. */
    for (s = 0; s < TR_N; s++)
      if (t->alive[s] && t->role[s] == TR_TRAITOR && t->human[s] && t->pick[s] >= 0 && t->pick[s] < TR_N) {
        v = t->pick[s];
        break;
      }
    if (v < 0)
      for (s = 0; s < TR_N; s++)
        if (t->alive[s] && t->role[s] == TR_TRAITOR && t->pick[s] >= 0 && t->pick[s] < TR_N) {
          v = t->pick[s];
          break;
        }
    tr_enter(t, TR_BREAKFAST);
    t->murdered = v;
    if (v >= 0) t->alive[v] = 0;              /* the murdered were Faithful: no need to show it */
    t->shield = -1;                           /* spent, used or not */
    if (tr_check(t)) t->phase = TR_OVER;
    break;
  }
  case TR_BREAKFAST:
    t->round++;
    t->banished = t->murdered = -1;
    t->leader = tr_next_alive(t, t->leader);
    t->rejects = 0;
    tr_enter(t, TR_PROPOSE);
    break;
  }
}

/* ---- one seat's view ----
 *
 * What a seat may know, and all the screen draws from: on the host from its
 * own Tr, on the guest from what the host sent. A vote is shown once the
 * votes are in (VOTED, TABLED) and only your own before; a coin never, only
 * how many were lead; a role once banished, or a fellow Traitor's to a
 * Traitor, and everyone's at the end. Packed, it is one link message. */

#define TV_WIRE 32

typedef struct {
  uint8_t phase, round, me, my_role, need;
  uint8_t leader, crew, crew_size, rejects, spoiled, clean;
  uint8_t pot;                        /* in tens */
  int8_t  lead, shield, banished, murdered, winner, sent;
  uint8_t alive, shown, traitors;     /* traitors: the ones this seat knows */
  uint8_t voted, yes;                 /* masks: who has voted; who said yes, once shown */
  int8_t  target[TR_N];               /* table votes, once shown (and your own) */
} TrView;

static TR_OPT void tv_from(const Tr *t, int me, TrView *v) {
  int s, shown_votes = t->phase == TR_VOTED || t->phase == TR_TABLED;
  uint8_t *p = (uint8_t *)v;
  for (s = 0; s < (int)sizeof *v; s++) p[s] = 0;
  v->phase = (uint8_t)t->phase;
  v->round = (uint8_t)t->round;
  v->me = (uint8_t)me;
  v->my_role = (uint8_t)t->role[me];
  v->need = (uint8_t)tr_need(t, me);
  v->leader = (uint8_t)t->leader;
  v->crew = t->crew;
  v->crew_size = (uint8_t)tr_crew_size(t);
  v->rejects = (uint8_t)t->rejects;
  v->spoiled = (uint8_t)t->spoiled;
  v->clean = (uint8_t)t->clean;
  v->pot = (uint8_t)(t->pot / 10);
  v->lead = (int8_t)t->lead;
  v->shield = (int8_t)t->shield;
  v->banished = (int8_t)t->banished;
  v->murdered = (int8_t)t->murdered;
  v->winner = (int8_t)t->winner;
  v->sent = (int8_t)t->sent;
  for (s = 0; s < TR_N; s++) {
    uint8_t bit = (uint8_t)(1 << s);
    int is_t = t->role[s] == TR_TRAITOR;
    if (t->alive[s]) v->alive |= bit;
    if (t->shown[s]) v->shown |= bit;
    if (is_t && (t->shown[s] || t->phase == TR_OVER || t->role[me] == TR_TRAITOR)) v->traitors |= bit;
    v->target[s] = -1;
    if (t->phase == TR_VOTE || t->phase == TR_VOTED || t->phase == TR_TABLE || t->phase == TR_TABLED)
      if (t->vote[s] >= 0) v->voted |= bit;
    if (t->phase == TR_VOTED && t->vote[s] == 1) v->yes |= bit;
    if ((t->phase == TR_TABLE && s == me) || t->phase == TR_TABLED) v->target[s] = t->vote[s];
  }
  if (t->phase == TR_VOTE && t->vote[me] == 1) v->yes = (uint8_t)(1 << me);
  (void)shown_votes;
}

static TR_OPT int tv_pack(const TrView *v, uint8_t out[TV_WIRE]) {
  int s, n = 0;
  out[n++] = 'S';
  out[n++] = v->phase; out[n++] = v->round; out[n++] = v->me; out[n++] = v->my_role; out[n++] = v->need;
  out[n++] = v->leader; out[n++] = v->crew; out[n++] = v->crew_size; out[n++] = v->rejects;
  out[n++] = v->spoiled; out[n++] = v->clean; out[n++] = v->pot;
  out[n++] = (uint8_t)v->lead; out[n++] = (uint8_t)v->shield; out[n++] = (uint8_t)v->banished;
  out[n++] = (uint8_t)v->murdered; out[n++] = (uint8_t)v->winner; out[n++] = (uint8_t)v->sent;
  out[n++] = v->alive; out[n++] = v->shown; out[n++] = v->traitors; out[n++] = v->voted; out[n++] = v->yes;
  for (s = 0; s < TR_N; s++) out[n++] = (uint8_t)v->target[s];
  return n;
}

static TR_OPT int tv_unpack(const uint8_t *in, int n, TrView *v) {
  int s, k = 1;
  if (n < 31 || in[0] != 'S') return -1;
  v->phase = in[k++]; v->round = in[k++]; v->me = in[k++]; v->my_role = in[k++]; v->need = in[k++];
  v->leader = in[k++]; v->crew = in[k++]; v->crew_size = in[k++]; v->rejects = in[k++];
  v->spoiled = in[k++]; v->clean = in[k++]; v->pot = in[k++];
  v->lead = (int8_t)in[k++]; v->shield = (int8_t)in[k++]; v->banished = (int8_t)in[k++];
  v->murdered = (int8_t)in[k++]; v->winner = (int8_t)in[k++]; v->sent = (int8_t)in[k++];
  v->alive = in[k++]; v->shown = in[k++]; v->traitors = in[k++]; v->voted = in[k++]; v->yes = in[k++];
  for (s = 0; s < TR_N; s++) v->target[s] = (int8_t)in[k++];
  if (v->phase > TR_OVER || v->me >= TR_N) return -1;
  return 0;
}

#endif
