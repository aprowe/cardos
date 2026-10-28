/* JarScript 3's device machine on the host: apps/jar/jv3.h against its
 * table (apps/jar/jarvm3.def), scripts assembled by hand for each part of
 * the spec, and the Python machine's traces replayed (apps/jar/test/fixtures/jv3_*,
 * written by apps/jar/tools/make_jarvm3_fixtures.py) -- skipped quietly while there
 * are none. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

#include "tinytest.h"
#include "apps/jar/jv3.h"

/* ---- a tiny assembler ------------------------------------------------------ */

typedef struct {
  uint8_t s[JV3_MAX];
  int n, states, handlers;
} Asm;

/* The header and the tables; code starts at a->n after this. anims: bytes
 * as in the format ({fps, n, frames...} each); strings: NUL-separated in
 * `str` (0x01 is a value slot). */
static void a_begin(Asm *a, int nvars, const int16_t *init, int nstates, int nhandlers,
                    int nanims, const uint8_t *anims, int alen,
                    int nperches, const uint8_t *perches, int nstrings, const char *str) {
  int i;
  memset(a, 0, sizeof *a);
  a->s[0] = JV3_FORMAT;
  a->s[2] = (uint8_t)nvars;
  a->s[3] = (uint8_t)nstates;
  a->s[4] = (uint8_t)nhandlers;
  a->s[5] = (uint8_t)nanims;
  a->s[6] = (uint8_t)nperches;
  a->s[7] = (uint8_t)nstrings;
  a->n = JV3_HEAD;
  for (i = 0; i < nvars; i++) { a->s[a->n++] = (uint8_t)(init ? init[i] : 0); a->s[a->n++] = (uint8_t)((init ? init[i] : 0) >> 8); }
  a->states = a->n;
  a->n += 2 * nstates;
  a->handlers = a->n;
  a->n += 7 * nhandlers;
  if (alen) memcpy(a->s + a->n, anims, (size_t)alen);
  a->n += alen;
  if (nperches) memcpy(a->s + a->n, perches, (size_t)(2 * nperches));
  a->n += 2 * nperches;
  for (i = 0; i < nstrings; i++) {
    int k = (int)strlen(str);
    a->s[a->n++] = (uint8_t)k;
    memcpy(a->s + a->n, str, (size_t)k);
    a->n += k;
    str += k + 1;
  }
}
static int a_here(Asm *a) { return a->n; }
static void a_state(Asm *a, int k, int pc) { a->s[a->states + 2 * k] = (uint8_t)pc; a->s[a->states + 2 * k + 1] = (uint8_t)(pc >> 8); }
static void a_handler(Asm *a, int i, int ev, int arg, int state, int param, int pc) {
  uint8_t *h = a->s + a->handlers + 7 * i;
  h[0] = (uint8_t)ev; h[1] = (uint8_t)arg; h[2] = (uint8_t)state;
  h[3] = (uint8_t)param; h[4] = (uint8_t)(param >> 8);
  h[5] = (uint8_t)pc; h[6] = (uint8_t)(pc >> 8);
}
static void a_op(Asm *a, int op) { a->s[a->n++] = (uint8_t)op; }
static void a_op1(Asm *a, int op, int x) { a_op(a, op); a->s[a->n++] = (uint8_t)x; }
static void a_op2(Asm *a, int op, int x, int y) { a_op1(a, op, x); a->s[a->n++] = (uint8_t)y; }
static void a_push(Asm *a, int whole) { a_op1(a, J3O_PUSH8, whole & 255); }
static void a_raw(Asm *a, int raw) { a_op2(a, J3O_PUSH16, raw & 255, (raw >> 8) & 255); }
static int a_jump(Asm *a, int op) { int at = a->n + 1; a_op2(a, op, 0, 0); return at; }
static void a_jumpto(Asm *a, int op, int pc) { a_op2(a, op, pc & 255, pc >> 8); }
static void a_patch(Asm *a, int at, int pc) { a->s[at] = (uint8_t)pc; a->s[at + 1] = (uint8_t)(pc >> 8); }

/* ---- a world ----------------------------------------------------------------- */

static Jv3Jar J;
static Jv3Item ITEMS[JV3_SLOTS];
static int16_t MEM[JV3_SLOTS][8];
static uint8_t SCR[JV3_SLOTS][JV3_MAX];
static Jv3Box LEDGES[16];
static int16_t SENSES[J3_NSENSES];
static char OUT[1 << 21];
static int OUTN;
static int NFAULT, NACT, LASTACT, LASTARG, NSAY, NEMIT;
static char LASTSAY[64];

static int OUT_FULL;
static void out(const char *s) {
  int k = (int)strlen(s);
  if (OUTN + k + 1 < (int)sizeof OUT) { memcpy(OUT + OUTN, s, (size_t)k + 1); OUTN += k; }
  else OUT_FULL = 1;
}

static int16_t h_sense(void *c, int slot, int k) { (void)c; (void)slot; return SENSES[k]; }
static void h_act(void *c, int slot, int k, int16_t arg) {
  char l[64]; (void)c;
  snprintf(l, sizeof l, "act s=%d k=%d a=%d\n", slot, k, arg); out(l);
  NACT++; LASTACT = k; LASTARG = arg;
}
static void h_say(void *c, int slot, const char *text, int ticks) {
  char l[96]; (void)c;
  snprintf(l, sizeof l, "say s=%d ticks=%d \"%s\"\n", slot, ticks, text); out(l);
  NSAY++; snprintf(LASTSAY, sizeof LASTSAY, "%s", text);
}
static void h_emit(void *c, int slot, int kind, int n) {
  char l[64]; (void)c;
  snprintf(l, sizeof l, "emit s=%d k=%d n=%d\n", slot, kind, n); out(l);
  NEMIT++;
}
static void h_log(void *c, int slot, const char *text) {
  char l[96]; (void)c;
  snprintf(l, sizeof l, "log s=%d \"%s\"\n", slot, text); out(l);
}
static void h_fault(void *c, int slot, int pc) {
  char l[64]; (void)c;
  snprintf(l, sizeof l, "fault s=%d pc=%d\n", slot, pc); out(l);
  NFAULT++;
}
static const Jv3Host HOST = { 0, h_sense, h_act, h_say, h_emit, h_log, 0, h_fault };

static void w_reset(uint32_t seed) {
  memset(&J, 0, sizeof J);
  memset(ITEMS, 0, sizeof ITEMS);
  memset(MEM, 0, sizeof MEM);
  memset(SENSES, 0, sizeof SENSES);
  SENSES[J3S_hour] = -16;
  J.rng = seed ? seed : 1;
  J.max_bodies = JV3_SLOTS;
  J.host = &HOST;
  J.ledges = LEDGES;
  OUTN = 0; OUT[0] = 0; OUT_FULL = 0;
  NFAULT = NACT = NSAY = NEMIT = 0;
  LASTSAY[0] = 0;
}

/* An item in slot k at (px, py) px; script a (NULL: not scripted). */
static int w_item(int k, int px, int py, const Asm *a, uint32_t traits) {
  Jv3Slot *s = &J.slot[k];
  s->used = 1;
  s->x = (int16_t)(px * 16); s->y = (int16_t)(py * 16);
  s->w = 16; s->h = 16; s->scale = 1; s->nframes = 4;
  s->traits = traits;
  s->mem = MEM[k];
  if (k >= J.nslots) J.nslots = k + 1;
  if (!a) return 0;
  memcpy(SCR[k], a->s, (size_t)a->n);
  s->s = SCR[k];
  s->n = (uint16_t)a->n;
  s->it = &ITEMS[k];
  return jv3_place(&J, k);
}

static void w_ticks(int n) { while (n-- > 0) jv3_tick(&J); }

/* ---- the table ------------------------------------------------------------- */

void test_jv3_table(void) {
  int k = 0;
#define J3_OP(c, n, o, d) CHECK_EQ(c, k); CHECK_EQ(J3O_##n, c); CHECK(o <= 2); CHECK(strcmp(jv3_disasm_op(c), #n) == 0); k++;
#define J3_EVENT(v, w, f, d)
#define J3_SENSE(v, w, kk, d) CHECK_EQ(J3S_##w, v);
#define J3_FUNC(v, w, lo, hi, d) CHECK(lo <= hi && hi <= 8);
#define J3_FIELD(v, w, d)
#define J3_TARGET(v, w)
#define J3_ACTION(v, w, a, d)
#define J3_PARTICLE(v, w)
#define J3_NAME(g, w, v)
#include "apps/jar/jarvm3.def"
#undef J3_OP
#undef J3_EVENT
#undef J3_SENSE
#undef J3_FUNC
#undef J3_FIELD
#undef J3_TARGET
#undef J3_ACTION
#undef J3_PARTICLE
#undef J3_NAME
  CHECK_EQ(k, J3_NOPS);
  CHECK_EQ(J3_NEVENTS, 17);
  CHECK_EQ(J3_NSENSES, 38);
  CHECK_EQ(J3_NFUNCS, 18);
  CHECK_EQ(J3_NACTIONS, 10);
  CHECK_EQ(J3_NPARTICLES, 6);
  CHECK(strcmp(jv3_disasm_op(999), "?") == 0);
  CHECK(sizeof(Jv3Item) <= 256);
  printf("    sizeof(Jv3Item) = %d\n", (int)sizeof(Jv3Item));
}

/* ---- numbers ---------------------------------------------------------------- */

static void num(char *b, int raw) {
  J3Text t;
  t.o = b; t.at = 0; t.cap = 31;
  j3_putnum(&t, (int16_t)raw);
  b[t.at] = 0;
}

void test_jv3_numbers(void) {
  char b[32];
  CHECK_EQ(j3_sat(40000), 32767);
  CHECK_EQ(j3_sat(-40000), -32768);
  CHECK_EQ(j3_mul(24, 24), 36);               /* 1.5 * 1.5 = 2.25 */
  CHECK_EQ(j3_mul(-1, 1), -1);                /* floor(-1/16) */
  CHECK_EQ(j3_mul(32767, 32767), 32767);
  CHECK_EQ(j3_mul(-32768, 32767), -32768);
  CHECK_EQ(j3_div(16, 0), 0);
  CHECK_EQ(j3_div(16, 48), 5);                /* 1/3 -> 0.3125 trunc */
  CHECK_EQ(j3_div(-16, 48), -5);              /* toward zero */
  CHECK_EQ(j3_div(-32768, -1), 32767);
  CHECK_EQ(j3_mod(-112, 32), 16);             /* -7 mod 2 = 1, floored */
  CHECK_EQ(j3_mod(112, -32), -16);
  CHECK_EQ(j3_mod(5, 0), 0);
  CHECK_EQ(j3_round(8), 16);
  CHECK_EQ(j3_round(-8), -16);
  CHECK_EQ(j3_round(7), 0);
  CHECK_EQ(j3_round(32767), 32767);
  CHECK_EQ(j3_ticks(16), 20);
  CHECK_EQ(j3_ticks(1), 1);                   /* (20 + 8) >> 4 */
  CHECK_EQ(j3_ticks(-1), -1);                 /* floor */
  CHECK_EQ(j3_step(90 * 16), 64);
  CHECK_EQ(j3_step(-16), 255);                /* floored, then mod 256 */
  CHECK_EQ(j3_step(360 * 16), 0);
  CHECK_EQ(j3_deg(64), 1440);
  CHECK_EQ(j3_deg(1), 22);
  CHECK_EQ(J3_SIN[64], 16);
  CHECK_EQ(J3_SIN[192], -16);
  CHECK_EQ(j3_isqrt(0), 0);
  CHECK_EQ(j3_isqrt(15), 3);
  CHECK_EQ(j3_isqrt(0xFFFFFFFFu), 65535);
  /* numbers in text */
  num(b, 24); CHECK(strcmp(b, "1.5") == 0);
  num(b, 1); CHECK(strcmp(b, "0.06") == 0);
  num(b, -36); CHECK(strcmp(b, "-2.25") == 0);
  num(b, -48); CHECK(strcmp(b, "-3") == 0);
  num(b, 32767); CHECK(strcmp(b, "2047.94") == 0);
  num(b, -32768); CHECK(strcmp(b, "-2048") == 0);
  num(b, 2); CHECK(strcmp(b, "0.13") == 0);   /* 0.125 rounds up */
}

/* A body that does `code` then stores the top in var 0 and halts. */
static int calc(void (*emit)(Asm *a)) {
  Asm a;
  a_begin(&a, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  emit(&a);
  a_op1(&a, J3O_STV, 0);
  a_op(&a, J3O_HALT);
  w_reset(7);
  CHECK_EQ(w_item(0, 100, 112, &a, 0), 0);
  w_ticks(1);
  return MEM[0][0];
}
static void e_sat(Asm *a) { a_raw(a, 30000); a_raw(a, 30000); a_op(a, J3O_ADD); }
static void e_neg(Asm *a) { a_raw(a, -32768); a_op(a, J3O_NEG); }
static void e_div0(Asm *a) { a_push(a, 3); a_push(a, 0); a_op(a, J3O_DIV); }
static void e_and(Asm *a) { a_push(a, 2); a_push(a, 0); a_op(a, J3O_OR); a_push(a, 1); a_op(a, J3O_AND); }
static void e_rpt(Asm *a) {      /* v = 0; repeat 3: v += 2 */
  int top, out;
  a_push(a, 0); a_op1(a, J3O_STV, 0);
  a_push(a, 3);
  top = a_here(a);
  out = a_jump(a, J3O_RPT);
  a_op1(a, J3O_LDV, 0); a_push(a, 2); a_op(a, J3O_ADD); a_op1(a, J3O_STV, 0);
  a_jumpto(a, J3O_JMP, top);
  a_patch(a, out, a_here(a));
  a_op1(a, J3O_LDV, 0);
}
static void e_sqrt(Asm *a) { a_push(a, 2); a_op2(a, J3O_CALL, J3F_sqrt, 1); }
static void e_clamp(Asm *a) { a_push(a, 9); a_push(a, 1); a_push(a, 5); a_op2(a, J3O_CALL, J3F_clamp, 3); }
static void e_sin(Asm *a) { a_push(a, 30); a_op2(a, J3O_CALL, J3F_sin, 1); }
static void e_floor(Asm *a) { a_raw(a, -24); a_op2(a, J3O_CALL, J3F_floor, 1); }
static void e_sign(Asm *a) { a_raw(a, -3); a_op2(a, J3O_CALL, J3F_sign, 1); }
static void e_field0(Asm *a) { a_push(a, 0); a_op1(a, J3O_FIELD, J3D_x); }
static void e_me_w(Asm *a) { a_op1(a, J3O_LDS, J3S_me); a_op1(a, J3O_FIELD, J3D_w); }
static void e_rand(Asm *a) { a_push(a, 3); a_push(a, 1); a_op2(a, J3O_CALL, J3F_random, 2); }

void test_jv3_ops(void) {
  int v;
  CHECK_EQ(calc(e_sat), 32767);
  CHECK_EQ(calc(e_neg), 32767);
  CHECK_EQ(calc(e_div0), 0);
  CHECK_EQ(calc(e_and), 16);
  CHECK_EQ(calc(e_rpt), 96);
  CHECK_EQ(calc(e_sqrt), 22);                 /* isqrt(512): 1.375 */
  CHECK_EQ(calc(e_clamp), 80);
  CHECK_EQ(calc(e_sin), J3_SIN[21]);          /* 30 deg is step 21 */
  CHECK_EQ(calc(e_floor), -32);
  CHECK_EQ(calc(e_sign), -16);
  CHECK_EQ(calc(e_field0), 0);                /* nobody reads 0 */
  CHECK_EQ(calc(e_me_w), 256);
  v = calc(e_rand);
  CHECK(v == 16 || v == 32 || v == 48);
  {
    uint32_t r = 7;                            /* the same draw, by hand */
    j3_rnd(&r);
    CHECK_EQ(v, 16 + 16 * (int)(r % 3));
  }
}

void test_jv3_check_refuses(void) {
  Asm a;
  int at;
  /* good: a body that waits */
  a_begin(&a, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  at = a_here(&a);
  a_push(&a, 1); a_op(&a, J3O_WAIT); a_jumpto(&a, J3O_JMP, at);
  a_handler(&a, 0, J3E_poke, 0, 255, 0, a_here(&a));
  a_op(&a, J3O_RET);
  CHECK_EQ(jv3_check(a.s, a.n), 0);
  /* a handler that waits */
  a_handler(&a, 0, J3E_poke, 0, 255, 0, at);
  CHECK_EQ(jv3_check(a.s, a.n), -1);
  a_handler(&a, 0, J3E_poke, 0, 255, 0, a.n - 1);
  CHECK_EQ(jv3_check(a.s, a.n), 0);
  /* a var out of range */
  a.s[at + 3] = J3O_LDV;  /* WAIT -> LDV 0x19 (the JMP's opcode as operand) */
  CHECK_EQ(jv3_check(a.s, a.n), -1);
  /* falling off the end */
  a_begin(&a, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_push(&a, 1);
  CHECK_EQ(jv3_check(a.s, a.n), -1);
  a_op(&a, J3O_HALT);
  CHECK_EQ(jv3_check(a.s, a.n), 0);
  /* a jump into an operand */
  a_begin(&a, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_jumpto(&a, J3O_JMP, a_here(&a) + 1);
  a_op(&a, J3O_HALT);
  CHECK_EQ(jv3_check(a.s, a.n), -1);
  /* a wrong format, a short one */
  a.s[0] = 1;
  CHECK_EQ(jv3_check(a.s, a.n), -1);
  CHECK_EQ(jv3_check(a.s, 3), -1);
  /* a failed place leaves the item idle */
  w_reset(1);
  CHECK_EQ(w_item(0, 50, 112, &a, 0), -1);
  CHECK_EQ(ITEMS[0].stopped, 2);               /* 2: refused at placing, never ran */
}

/* ---- handlers, events, budget, faults ---------------------------------------- */

void test_jv3_state_handler_wins(void) {
  Asm a;
  a_begin(&a, 1, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a)); a_op(&a, J3O_HALT);
  a_state(&a, 1, a_here(&a)); a_op(&a, J3O_HALT);
  a_handler(&a, 0, J3E_poke, 0, 0, 0, a_here(&a));
  a_push(&a, 1); a_op1(&a, J3O_STV, 0); a_op1(&a, J3O_GO, 1);
  a_handler(&a, 1, J3E_poke, 0, 255, 0, a_here(&a));
  a_push(&a, 2); a_op1(&a, J3O_STV, 0); a_op(&a, J3O_RET);
  w_reset(1);
  CHECK_EQ(w_item(0, 50, 112, &a, 0), 0);
  jv3_event(&J, 0, J3E_poke, 0, 0);
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 16);
  CHECK_EQ(ITEMS[0].state, 1);
  jv3_event(&J, 0, J3E_poke, 0, 0);
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 32);                    /* state 1 has none: the top-level one */
}

/* Each of two items, on tick 1, signals the other; each counts signals. */
void test_jv3_events_wait_for_the_next_tick(void) {
  Asm a;
  int sig = 7;
  a_begin(&a, 1, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_op2(&a, J3O_SIGNAL, sig, 0);
  a_op(&a, J3O_HALT);
  a_handler(&a, 0, J3E_signal, sig, 255, 0, a_here(&a));
  a_op1(&a, J3O_LDV, 0); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 0);
  a_op(&a, J3O_RET);
  a_handler(&a, 1, J3E_signal, 0, 255, 0, a_here(&a));   /* another signal: never */
  a_push(&a, 99); a_op1(&a, J3O_STV, 0); a_op(&a, J3O_RET);
  w_reset(1);
  w_item(0, 50, 112, &a, 0);
  w_item(1, 150, 112, &a, 0);
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 0);                     /* slot 1 signalled after slot 0 ran */
  CHECK_EQ(MEM[1][0], 0);                     /* slot 0 signalled before slot 1 ran: still next tick */
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 16);
  CHECK_EQ(MEM[1][0], 16);
  CHECK_EQ(ITEMS[1].other, 1);                /* the sender's handle */
}

void test_jv3_budget_and_faults(void) {
  Asm a;
  int top, i;
  /* a body that never waits: paused by the budget, carried on next tick */
  a_begin(&a, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  top = a_here(&a);
  a_op1(&a, J3O_LDV, 0); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 0);
  a_jumpto(&a, J3O_JMP, top);
  /* on poke: loop forever (a fault each time) */
  a_handler(&a, 0, J3E_poke, 0, 255, 0, a_here(&a));
  a_jumpto(&a, J3O_JMP, a_here(&a));
  w_reset(1);
  w_item(0, 50, 112, &a, 0);
  CHECK_EQ(jv3_budget(&J.slot[0]), 50);
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 10 * 16);               /* 5 steps a pass, 50 steps */
  CHECK_EQ(NFAULT, 0);
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 20 * 16);
  for (i = 0; i < 7; i++) { jv3_event(&J, 0, J3E_poke, 0, 0); w_ticks(1); }
  CHECK_EQ(NFAULT, 7);
  CHECK_EQ(ITEMS[0].faults, 7);
  CHECK_EQ(ITEMS[0].stopped, 0);
  CHECK(strstr(OUT, "fault s=0 pc=") != 0);
  jv3_event(&J, 0, J3E_poke, 0, 0);
  w_ticks(1);
  CHECK_EQ(ITEMS[0].stopped, 1);
  i = MEM[0][0];
  w_ticks(3);
  CHECK_EQ(MEM[0][0], i);                     /* nothing runs any more */
}

void test_jv3_stack_faults_halt_the_body(void) {
  Asm a;
  int i;
  a_begin(&a, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  for (i = 0; i < 17; i++) a_push(&a, 1);
  a_op(&a, J3O_HALT);
  w_reset(1);
  w_item(0, 50, 112, &a, 0);
  w_ticks(1);
  CHECK_EQ(NFAULT, 1);
  CHECK_EQ(ITEMS[0].halted, 1);
  CHECK(strstr(OUT, "fault s=0 pc=") != 0);
  w_ticks(2);
  CHECK_EQ(NFAULT, 1);
  /* underflow */
  a_begin(&a, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_op(&a, J3O_ADD);
  a_op(&a, J3O_HALT);
  w_reset(1);
  w_item(0, 50, 112, &a, 0);
  w_ticks(1);
  CHECK_EQ(NFAULT, 1);
}

void test_jv3_wait_every_and_go(void) {
  Asm a;
  int top;
  /* state 0: wait 1 s; v0 += 1; loop.  every 5 ticks: v1 += 1.  v0 == 3 -> go 1 */
  a_begin(&a, 2, 0, 2, 1, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  top = a_here(&a);
  a_push(&a, 1); a_op(&a, J3O_WAIT);
  a_op1(&a, J3O_LDV, 0); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 0);
  a_jumpto(&a, J3O_JMP, top);
  a_state(&a, 1, a_here(&a));
  a_op(&a, J3O_HALT);
  a_handler(&a, 0, J3E_every, 0, 0, 5, a_here(&a));
  a_op1(&a, J3O_LDV, 1); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 1);
  a_op(&a, J3O_RET);
  w_reset(1);
  w_item(0, 50, 112, &a, 0);
  w_ticks(1);
  CHECK_EQ(ITEMS[0].wait, 20);
  w_ticks(19);
  CHECK_EQ(MEM[0][0], 0);
  CHECK_EQ(MEM[0][1], 4 * 16);                /* ticks 5, 10, 15, 20 */
  w_ticks(1);                                 /* tick 21: the wait is over */
  CHECK_EQ(MEM[0][0], 16);
  CHECK_EQ(ITEMS[0].wait, 20);
}

/* ---- text, anims, actions ---------------------------------------------------- */

void test_jv3_say_emit_act(void) {
  Asm a;
  const uint8_t anim[] = { 10, 3, 1, 2, 3 };
  a_begin(&a, 0, 0, 1, 0, 1, anim, 5, 0, 0, 2, "x=\001 y=\001!\0abcdefghijklmnopqrstuvwx");
  a_state(&a, 0, a_here(&a));
  a_raw(&a, 24); a_raw(&a, -1); a_push(&a, 3);
  a_op2(&a, J3O_SAY, 0, 2);
  a_raw(&a, 1); a_op2(&a, J3O_SAY, 1, 0);       /* 1/16 s: at least 6 ticks */
  a_push(&a, 3); a_op1(&a, J3O_EMIT, 4);
  a_push(&a, 99); a_op1(&a, J3O_EMIT, 0);
  a_push(&a, 7); a_op2(&a, J3O_ACT, 5, 1);
  a_op2(&a, J3O_ACT, 2, 0);
  a_push(&a, 5); a_raw(&a, 8); a_op2(&a, J3O_LOG, 255, 2);
  a_op2(&a, J3O_PLAY, 0, 1);
  a_op(&a, J3O_AWAIT);
  a_op1(&a, J3O_FRAME, 0);
  a_op(&a, J3O_HALT);
  w_reset(1);
  w_item(0, 50, 112, &a, 0);
  w_ticks(1);
  CHECK(strstr(OUT, "say s=0 ticks=60 \"x=1.5 y=-0.06!\"\n") != 0);
  CHECK(strstr(OUT, "say s=0 ticks=6 \"abcdefghijklmnopqrstuvwx\"\n") != 0);
  CHECK(strstr(OUT, "emit s=0 k=4 n=3\nemit s=0 k=0 n=12\n") != 0);
  CHECK(strstr(OUT, "act s=0 k=5 a=112\nact s=0 k=2 a=0\n") != 0);
  CHECK(strstr(OUT, "log s=0 \"5 0.5\"\n") != 0);
  CHECK_EQ(ITEMS[0].frame, 1);                /* the first frame of the anim */
  CHECK_EQ(ITEMS[0].say_ticks, 5);
  /* 10 fps: frame k = t * 10 / 20 */
  w_ticks(1); CHECK_EQ(ITEMS[0].frame, 2);    /* t 2 at the end of tick 2: k 1 */
  CHECK_EQ(ITEMS[0].anim_t, 2);
  w_ticks(2); CHECK_EQ(ITEMS[0].frame, 3);    /* t 4: k 2 */
  CHECK_EQ(ITEMS[0].anim_done, 0);
  w_ticks(2); CHECK_EQ(ITEMS[0].anim_done, 1);  /* t 6: k 3: done, last frame */
  CHECK_EQ(ITEMS[0].frame, 3);
  w_ticks(1);
  CHECK_EQ(ITEMS[0].frame, 0);                /* the body went on: frame 0 */
  CHECK_EQ(ITEMS[0].anim, 255);
  CHECK_EQ(ITEMS[0].say[0], 0);               /* 6 ticks are over */
}

/* ---- movers ------------------------------------------------------------------ */

void test_jv3_path_lands_exactly(void) {
  Asm a;
  int i, landed = -1;
  /* move to 100, 60 speed 40 arc 10; await; halt.  on land: v0 = other + 1 */
  a_begin(&a, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_push(&a, 100); a_push(&a, 60); a_push(&a, 40); a_push(&a, 10);
  a_op2(&a, J3O_MOVE, J3T_xy, 0);
  a_op(&a, J3O_AWAIT);
  a_op(&a, J3O_HALT);
  a_handler(&a, 0, J3E_land, 0, 255, 0, a_here(&a));
  a_op1(&a, J3O_LDS, J3S_other); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 0);
  a_op(&a, J3O_RET);
  w_reset(1);
  w_item(0, 40, 112, &a, 0);
  w_ticks(1);
  CHECK_EQ(ITEMS[0].mover, 1);
  CHECK(ITEMS[0].mdur >= 3);
  CHECK_EQ(ITEMS[0].facing, 1);
  for (i = 0; i < 200 && ITEMS[0].mover; i++) {
    int32_t px = ITEMS[0].x;
    w_ticks(1);
    CHECK(ITEMS[0].x >= px);                  /* eased, never backwards */
    CHECK(ITEMS[0].y < 112 * 16 || !ITEMS[0].mover);
  }
  CHECK_EQ(ITEMS[0].x, 1600);
  CHECK_EQ(ITEMS[0].y, 960);
  CHECK_EQ(J.slot[0].x, 1600);
  CHECK_EQ(MEM[0][0], 0);                     /* land waits for the next tick */
  w_ticks(1);
  landed = MEM[0][0];
  CHECK_EQ(landed, 16);
  /* the duration: len of a 60 x 52 ish arc / 40 px/s */
  CHECK(i > 20 && i < 60);
}

void test_jv3_walk_stops_at_the_glass(void) {
  Asm a;
  a_begin(&a, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_push(&a, -100); a_push(&a, 40);
  a_op(&a, J3O_WALK);
  a_op(&a, J3O_AWAIT);
  a_op(&a, J3O_HALT);
  w_reset(1);
  w_item(0, 30, 112, &a, 0);
  w_ticks(1);
  CHECK_EQ(ITEMS[0].facing, -1);
  w_ticks(40);
  CHECK_EQ(ITEMS[0].mover, 0);
  CHECK_EQ(ITEMS[0].blocked, 1);
  CHECK_EQ(ITEMS[0].x, 8 * 16 + 32);          /* half 8 px + 2 px */
  CHECK_EQ(ITEMS[0].y, 112 * 16);
  CHECK_EQ(ITEMS[0].halted, 1);
}

/* A perch on a branch; a bird goes to it, then follows it when the host moves it. */
void test_jv3_perch_and_follow(void) {
  Asm bird, branch;
  const uint8_t perch[] = { 4, 2 };
  a_begin(&branch, 0, 0, 1, 0, 0, 0, 0, 1, perch, 0, 0);
  a_state(&branch, 0, a_here(&branch)); a_op(&branch, J3O_HALT);
  a_begin(&bird, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&bird, 0, a_here(&bird));
  a_push(&bird, 60); a_push(&bird, 0);
  a_op2(&bird, J3O_MOVE, J3T_perch, 23 + 1);    /* perch roost */
  a_op(&bird, J3O_AWAIT);
  a_op(&bird, J3O_HALT);
  w_reset(3);
  w_item(0, 40, 112, &bird, 0);
  w_item(1, 150, 80, &branch, 1u << 23);
  w_ticks(1);
  CHECK_EQ(ITEMS[0].spot_owner, 2);
  CHECK_EQ(ITEMS[0].spot_idx, 0);
  w_ticks(100);
  CHECK_EQ(ITEMS[0].perch, 1);
  CHECK_EQ(ITEMS[0].perch_owner, 2);
  CHECK_EQ(j3_sense(&J, 0, J3S_perched), 32);
  CHECK_EQ(ITEMS[0].x, 150 * 16 - 128 + 64);
  CHECK_EQ(ITEMS[0].y, 80 * 16 - 256 + 32);
  CHECK_EQ(ITEMS[0].grounded, 0);
  /* the host moves the branch: the bird comes along */
  J.slot[1].x = (int16_t)(170 * 16);
  jv3_moved(&J, 1);
  w_ticks(1);
  CHECK_EQ(ITEMS[0].x, 170 * 16 - 128 + 64);
  /* nobody else can take it */
  CHECK_EQ(j3_spots(&J, 1, -1, -1, 0, 0), 4);   /* the rim only (the branch's own is not its) */
}

/* ---- physics ------------------------------------------------------------------- */

void test_jv3_physics_falls_and_bounces(void) {
  Asm a;
  int i, hits = 0, peak_after = 0;
  /* physics on; bounce 50; halt.  on hit: v0 = impact */
  a_begin(&a, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_push(&a, 50); a_op1(&a, J3O_STP, J3S_bounce);
  a_op2(&a, J3O_FLAG, 0, 1);
  a_op(&a, J3O_HALT);
  a_handler(&a, 0, J3E_hit, 0, 255, 0, a_here(&a));
  a_op1(&a, J3O_LDS, J3S_impact); a_op1(&a, J3O_STV, 0);
  a_op(&a, J3O_RET);
  w_reset(1);
  w_item(0, 120, 30, &a, 0);
  for (i = 0; i < 60; i++) {
    int vy0 = ITEMS[0].vy;
    w_ticks(1);
    if (vy0 > 0 && ITEMS[0].vy < 0) { hits++; if (!peak_after) peak_after = -ITEMS[0].vy; }
  }
  CHECK(hits >= 1);
  CHECK(MEM[0][0] > 240);                     /* an impact over 15 px/s */
  CHECK(peak_after > 0);
  CHECK(strstr(OUT, "fault") == 0);
  for (i = 0; i < 200; i++) w_ticks(1);
  CHECK_EQ(ITEMS[0].vy, 0);                   /* settled */
  CHECK_EQ(ITEMS[0].grounded, 1);
  CHECK_EQ(ITEMS[0].y, 113 * 16);             /* r = 7 px, half height 8: the feet 1 px into the soil */
  /* the cap: no more bodies */
  w_reset(1);
  J.max_bodies = 0;
  w_item(0, 120, 30, &a, 0);
  w_ticks(1);
  CHECK_EQ(ITEMS[0].physics, 0);
}

void test_jv3_physics_lands_on_a_ledge_and_bumps(void) {
  Asm a;
  int i;
  a_begin(&a, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_op2(&a, J3O_FLAG, 0, 1);
  a_op(&a, J3O_HALT);
  a_handler(&a, 0, J3E_bump, 0, 255, 0, a_here(&a));
  a_op1(&a, J3O_LDV, 0); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 0);
  a_op(&a, J3O_RET);
  w_reset(1);
  LEDGES[0].x = 100 * 16; LEDGES[0].y = 60 * 16; LEDGES[0].w = 40 * 16; LEDGES[0].h = 4 * 16;
  J.nledges = 1;
  w_item(0, 120, 20, &a, 0);
  for (i = 0; i < 120; i++) w_ticks(1);
  CHECK_EQ(MEM[0][0], 48);                    /* 113, 45, 18 px/s in; then 7: under 10, and touching */
  CHECK_EQ(ITEMS[0].grounded, 1);
  CHECK(ITEMS[0].y <= 60 * 16 + 16 && ITEMS[0].y >= 60 * 16 - 16);
}

/* ---- near, remove, moved ------------------------------------------------------- */

static void near_script(Asm *a) {
  /* on near bug within 30: v0 += 1, v1 = other */
  a_begin(a, 2, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0);
  a_state(a, 0, a_here(a)); a_op(a, J3O_HALT);
  a_handler(a, 0, J3E_near, 18 + 1, 255, 30, a_here(a));
  a_op1(a, J3O_LDV, 0); a_push(a, 1); a_op(a, J3O_ADD); a_op1(a, J3O_STV, 0);
  a_op1(a, J3O_LDS, J3S_other); a_op1(a, J3O_STV, 1);
  a_op(a, J3O_RET);
}

void test_jv3_near_fires_once_per_arrival(void) {
  Asm a;
  near_script(&a);
  w_reset(1);
  w_item(0, 100, 112, &a, 0);
  w_item(1, 200, 112, 0, 1u << 18);           /* an unscripted bug, far */
  w_ticks(3);
  CHECK_EQ(MEM[0][0], 0);
  J.slot[1].x = 120 * 16;
  w_ticks(3);
  CHECK_EQ(MEM[0][0], 16);
  CHECK_EQ(MEM[0][1], 32);
  J.slot[1].x = 200 * 16;
  w_ticks(1);
  J.slot[1].x = 110 * 16;
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 16);                    /* queued this tick, heard the next */
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 32);
}

/* One trait, two radii: a state's near within 22 (handler 0) and a top-level
 * near within 48 (handler 1). Each runs only for the arrivals its own radius
 * saw -- before, the far handler's arrival went to the first match, the
 * close one. v0 counts close, v1 far. */
void test_jv3_near_goes_to_the_handler_that_saw_it(void) {
  Asm a;
  a_begin(&a, 2, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a)); a_op(&a, J3O_HALT);
  a_handler(&a, 0, J3E_near, 18 + 1, 0, 22, a_here(&a));
  a_op1(&a, J3O_LDV, 0); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 0); a_op(&a, J3O_RET);
  a_handler(&a, 1, J3E_near, 18 + 1, 255, 48, a_here(&a));
  a_op1(&a, J3O_LDV, 1); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 1); a_op(&a, J3O_RET);
  w_reset(1);
  CHECK_EQ(w_item(0, 100, 112, &a, 0), 0);
  w_item(1, 200, 112, 0, 1u << 18);           /* an unscripted bug, far */
  w_ticks(2);
  J.slot[1].x = 140 * 16;                     /* 40 px: the far one only */
  w_ticks(2);
  CHECK_EQ(MEM[0][0], 0);
  CHECK_EQ(MEM[0][1], 16);
  CHECK_EQ(ITEMS[0].qn, 0);                   /* nothing left over */
  J.slot[1].x = 110 * 16;                     /* 10 px: the close one only (far still sees it) */
  w_ticks(2);
  CHECK_EQ(MEM[0][0], 16);
  CHECK_EQ(MEM[0][1], 16);
  J.slot[1].x = 200 * 16;
  w_ticks(1);
  J.slot[1].x = 105 * 16;                     /* both, one each */
  w_ticks(2);
  CHECK_EQ(MEM[0][0], 32);
  CHECK_EQ(MEM[0][1], 32);
  CHECK_EQ(ITEMS[0].other, 2);
  /* a near event naming a handler that is not a near one, or none: dropped */
  jv3_event(&J, 0, J3E_near, 0, 2);
  jv3_event(&J, 0, J3E_near, 3, 2);
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 32);
  CHECK_EQ(MEM[0][1], 32);
  CHECK_EQ(NFAULT, 0);
}

/* 20 pokes (8, 8, 4) to a handler costing 25 steps with a 50-step budget:
 * two run a tick, the rest cannot start and are dropped -- never a fault. */
void test_jv3_unstarted_handlers_are_not_faults(void) {
  Asm a;
  int i, t;
  a_begin(&a, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a)); a_op(&a, J3O_HALT);
  a_handler(&a, 0, J3E_poke, 0, 255, 0, a_here(&a));
  for (i = 0; i < 6; i++) { a_op1(&a, J3O_LDV, 0); a_push(&a, 1); a_op(&a, J3O_ADD); a_op1(&a, J3O_STV, 0); }
  a_op(&a, J3O_RET);                          /* 6 * 4 + 1 = 25 steps */
  w_reset(1);
  w_item(0, 50, 112, &a, 0);
  w_ticks(1);
  for (t = 0; t < 3; t++) {
    for (i = 0; i < (t < 2 ? 8 : 4); i++) jv3_event(&J, 0, J3E_poke, 0, 0);
    w_ticks(1);
  }
  w_ticks(2);
  CHECK_EQ(NFAULT, 0);
  CHECK_EQ(ITEMS[0].faults, 0);
  CHECK_EQ(ITEMS[0].stopped, 0);
  CHECK_EQ(MEM[0][0], 3 * 2 * 6 * 16);
  CHECK(strstr(OUT, "fault") == 0);
  /* a handler that starts and runs out mid-way is still a fault: 26 steps */
  a.n--;                                      /* ...a push, then RET: one more step */
  a_push(&a, 0);
  a_op(&a, J3O_RET);
  w_reset(1);
  w_item(0, 50, 112, &a, 0);
  w_ticks(1);
  jv3_event(&J, 0, J3E_poke, 0, 0);
  jv3_event(&J, 0, J3E_poke, 0, 0);
  w_ticks(1);
  CHECK_EQ(NFAULT, 1);
}

/* angleto's own table: the eight compass points come out exact. */
void test_jv3_angleto_compass(void) {
  Asm a;
  static const int D[8][2] = { { 40, 0 }, { 40, 40 }, { 0, 40 }, { -40, 40 }, { -40, 0 }, { -40, -40 }, { 0, -40 }, { 40, -40 } };
  int i;
  int16_t arg;
  a_begin(&a, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a)); a_op(&a, J3O_HALT);
  w_reset(1);
  w_item(0, 120, 80, &a, 0);
  for (i = 0; i < 8; i++) w_item(1 + i, 120 + D[i][0], 80 + D[i][1], 0, 0);
  for (i = 0; i < 8; i++) {
    arg = (int16_t)((2 + i) * 16);
    CHECK_EQ(j3_call(&J, 0, J3F_angleto, &arg, 1), (i * 32 * 45) >> 1);
    CHECK_EQ(j3_call(&J, 0, J3F_angleto, &arg, 1), i * 45 * 16);
  }
  CHECK_EQ(J3_ANG[64], 16384);
  CHECK_EQ(J3_ANG[32], 11585);
  CHECK_EQ(J3_ANG[192], -16384);
  /* as far apart as int16 allows: no overflow in int32 */
  w_reset(1);
  w_item(0, 0, 0, &a, 0);
  J.slot[0].x = -32768; J.slot[0].y = 32767; ITEMS[0].x = -32768; ITEMS[0].y = 32767;
  w_item(1, 0, 0, 0, 0);
  J.slot[1].x = 32767; J.slot[1].y = -32768;
  arg = 2 * 16;
  CHECK_EQ(j3_call(&J, 0, J3F_angleto, &arg, 1), 315 * 16);
}

void test_jv3_remove_fixes_references(void) {
  Asm a, br;
  const uint8_t perch[] = { 4, 2 };
  near_script(&a);
  a_begin(&br, 0, 0, 1, 0, 0, 0, 0, 1, perch, 0, 0);
  a_state(&br, 0, a_here(&br)); a_op(&br, J3O_HALT);
  w_reset(1);
  w_item(0, 20, 112, 0, 0);                   /* a pebble, removed below */
  w_item(1, 100, 112, &a, 0);
  w_item(2, 110, 112, 0, 1u << 18);           /* a bug, near */
  w_item(3, 150, 80, &br, 0);
  w_ticks(2);                                 /* near is heard the tick after */
  CHECK_EQ(MEM[1][0], 16);
  CHECK_EQ(ITEMS[1].nearm[0], 1u << 2);
  /* something perched on slot 3, something queued from slot 2 */
  ITEMS[1].perch = 1; ITEMS[1].perch_owner = 4;
  jv3_event(&J, 1, J3E_poke, 0, 3);
  jv3_event(&J, 1, J3E_poke, 0, 1);
  /* the host moves the storage with the slots, as the jar does */
  jv3_remove(&J, 0);
  memmove(&ITEMS[0], &ITEMS[1], sizeof ITEMS[0] * 3);
  memmove(MEM[0], MEM[1], sizeof MEM[0] * 3);
  memmove(SCR[0], SCR[1], sizeof SCR[0] * 3);
  { int k; for (k = 0; k < J.nslots; k++) { if (J.slot[k].it) J.slot[k].it = &ITEMS[k]; J.slot[k].mem = MEM[k]; if (J.slot[k].s) J.slot[k].s = SCR[k]; } }
  CHECK_EQ(J.nslots, 3);
  CHECK_EQ(J.slot[3].used, 0);
  CHECK_EQ(ITEMS[0].nearm[0], 1u << 1);
  CHECK_EQ(ITEMS[0].perch_owner, 3);
  CHECK_EQ(ITEMS[0].q[0].other, 2);
  CHECK_EQ(ITEMS[0].q[1].other, 0);
  w_ticks(1);
  CHECK_EQ(MEM[0][0], 16);                    /* the bug is the same bug: no new arrival */
  CHECK_EQ(ITEMS[0].perch, 1);
  /* removing the branch leaves the perch */
  jv3_remove(&J, 2);
  CHECK_EQ(ITEMS[0].perch, 0);
  CHECK_EQ(J.nslots, 2);
  /* removing the bug drops its bit */
  jv3_remove(&J, 1);
  CHECK_EQ(ITEMS[0].nearm[0], 0u);
}

void test_jv3_moved_cancels_the_mover(void) {
  Asm a;
  a_begin(&a, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  a_state(&a, 0, a_here(&a));
  a_push(&a, 100); a_push(&a, 60); a_push(&a, 20); a_push(&a, 0);
  a_op2(&a, J3O_MOVE, J3T_xy, 0);
  a_op(&a, J3O_AWAIT);
  a_op(&a, J3O_HALT);
  w_reset(1);
  w_item(0, 40, 112, &a, 0);
  w_ticks(2);
  CHECK_EQ(ITEMS[0].mover, 1);
  ITEMS[0].perch = 1;
  ITEMS[0].vx = 99;
  J.slot[0].x = 200 * 16; J.slot[0].y = 112 * 16;
  jv3_moved(&J, 0);
  CHECK_EQ(ITEMS[0].mover, 0);
  CHECK_EQ(ITEMS[0].perch, 0);
  CHECK_EQ(ITEMS[0].vx, 0);
  CHECK_EQ(ITEMS[0].x, 200 * 16);
  w_ticks(2);
  CHECK_EQ(ITEMS[0].x, 200 * 16);
  CHECK_EQ(ITEMS[0].halted, 1);               /* the await saw nothing under way */
}

/* ---- the Python machine's traces, replayed ------------------------------------ */

static void fx_dir(char *out_, int cap) {
  const char *f = __FILE__, *cut = 0, *p;
  for (p = f; *p; p++) if (*p == '/' || *p == '\\') cut = p + 1;
  if (cut) snprintf(out_, (size_t)cap, "%.*sfixtures/", (int)(cut - f), f);
  else snprintf(out_, (size_t)cap, "apps/jar/test/fixtures/");
}

static int fx_read(const char *dir, const char *name, char *buf, int cap) {
  char path[600];
  FILE *f;
  int n;
  snprintf(path, sizeof path, "%s%s", dir, name);
  f = fopen(path, "rb");
  if (!f) return -1;
  n = (int)fread(buf, 1, (size_t)cap - 1, f);
  fclose(f);
  buf[n] = 0;
  return n;
}

static void no_cr(char *s) {
  char *d = s;
  for (; *s; s++) if (*s != '\r') *d++ = *s;
  *d = 0;
}

#define FX_EVENTS 256
static char FX_WORLD[16384];
static char FX_WANT[1 << 21];
static struct { int tick, slot, ev, arg, other; } FX_EV[FX_EVENTS];

static void fx_state_lines(void) {
  int k;
  for (k = 0; k < J.nslots; k++) {
    const Jv3Item *it = J.slot[k].it;
    const int16_t *v = J.slot[k].mem;
    char l[256];
    if (!J.slot[k].used || !it) continue;
    snprintf(l, sizeof l, "t=%u s=%d x=%d y=%d vx=%d vy=%d a=%d st=%d fr=%d fc=%d w=%d h=%d f=%d v=%d,%d,%d,%d,%d,%d,%d,%d\n",
             (unsigned)J.tick, k, it->x, it->y, it->vx, it->vy, it->angle, it->state, it->frame, it->facing,
             it->wait, it->halted, it->faults, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    out(l);
  }
}

/* Run one world; 1 if its trace is the Python one. */
static int fx_replay(const char *dir, const char *world) {
  char name[300], *line, *next, *a, *b;
  int ticks = 0, nev = 0, t, i, ln;
  w_reset(1);
  if (fx_read(dir, world, FX_WORLD, (int)sizeof FX_WORLD) < 0) return 0;
  no_cr(FX_WORLD);
  for (line = FX_WORLD; line && *line; line = next) {
    unsigned seed, tr;
    int k, x, y, w, h, sc, nf, m[8];
    char bin[200];
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    if (sscanf(line, "seed %u", &seed) == 1) J.rng = seed ? seed : 1;
    else if (sscanf(line, "ticks %d", &ticks) == 1) {}
    else if (sscanf(line, "bodies %d", &k) == 1) J.max_bodies = k;
    else if (sscanf(line, "sense %d %d", &k, &x) == 2) { if (k >= 0 && k < J3_NSENSES) SENSES[k] = (int16_t)x; }
    else if (sscanf(line, "ledge %d %d %d %d", &x, &y, &w, &h) == 4) {
      if (J.nledges < 16) { Jv3Box *L = &LEDGES[J.nledges++]; L->x = (int16_t)x; L->y = (int16_t)y; L->w = (int16_t)w; L->h = (int16_t)h; }
    } else if (sscanf(line, "event %d %d %d %d %d", &t, &k, &x, &y, &w) == 5) {
      if (nev < FX_EVENTS) { FX_EV[nev].tick = t; FX_EV[nev].slot = k; FX_EV[nev].ev = x; FX_EV[nev].arg = y; FX_EV[nev].other = w; nev++; }
    } else if (sscanf(line, "item %d %d %d %d %d %d %x %d %199s %d %d %d %d %d %d %d %d",
                      &k, &x, &y, &w, &h, &sc, &tr, &nf, bin, &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7]) == 17) {
      Jv3Slot *s;
      if (k < 0 || k >= JV3_SLOTS) continue;
      s = &J.slot[k];
      s->used = 1; s->x = (int16_t)x; s->y = (int16_t)y; s->w = (uint8_t)w; s->h = (uint8_t)h;
      s->scale = (uint8_t)sc; s->traits = tr; s->nframes = (uint8_t)nf; s->mem = MEM[k];
      for (i = 0; i < 8; i++) MEM[k][i] = (int16_t)m[i];
      if (k >= J.nslots) J.nslots = k + 1;
      if (strcmp(bin, "-") != 0) {
        int n = fx_read(dir, bin, (char *)SCR[k], JV3_MAX + 1);
        if (n < 0) { printf("    %s: no %s\n", world, bin); return 0; }
        s->s = SCR[k]; s->n = (uint16_t)n; s->it = &ITEMS[k];
      }
    }
  }
  for (i = 0; i < J.nslots; i++)          /* a script the machine refuses: the host treats the item as unscripted */
    if (J.slot[i].it && jv3_place(&J, i) < 0) { J.slot[i].it = 0; J.slot[i].s = 0; J.slot[i].n = 0; }
  for (t = 1; t <= ticks; t++) {
    for (i = 0; i < nev; i++) if (FX_EV[i].tick == t) jv3_event(&J, FX_EV[i].slot, FX_EV[i].ev, FX_EV[i].arg, FX_EV[i].other);
    jv3_tick(&J);
    fx_state_lines();
  }
  if (OUT_FULL) { printf("    %s: the trace outgrew the buffer\n", world); return 0; }
  snprintf(name, sizeof name, "%s", world);
  strcpy(name + strlen(name) - 6, ".trace");
  if (fx_read(dir, name, FX_WANT, (int)sizeof FX_WANT) < 0) { printf("    %s: no trace\n", world); return 0; }
  no_cr(FX_WANT);
  /* line by line, the first difference */
  for (a = OUT, b = FX_WANT, ln = 1; *a || *b; ln++) {
    char *ea = strchr(a, '\n'), *eb = strchr(b, '\n');
    int la = ea ? (int)(ea - a) : (int)strlen(a), lb = eb ? (int)(eb - b) : (int)strlen(b);
    if (la != lb || strncmp(a, b, (size_t)la) != 0) {
      printf("    %s line %d differs\n      C:      %.*s\n      Python: %.*s\n", world, ln, la, a, lb, b);
      return 0;
    }
    a = ea ? ea + 1 : a + la;
    b = eb ? eb + 1 : b + lb;
  }
  return 1;
}

void test_jv3_agrees_with_the_python_machine(void) {
  char dir[512], names[64][128];
  int n = 0, i;
  fx_dir(dir, (int)sizeof dir);
  if (getenv("JV3_FIXTURES")) snprintf(dir, sizeof dir, "%s/", getenv("JV3_FIXTURES"));
#ifdef _WIN32
  {
    char pat[600];
    struct _finddata_t fd;
    intptr_t h;
    snprintf(pat, sizeof pat, "%sjv3_*.world", dir);
    h = _findfirst(pat, &fd);
    if (h != -1) {
      do { if (n < 64) snprintf(names[n++], sizeof names[0], "%s", fd.name); } while (_findnext(h, &fd) == 0);
      _findclose(h);
    }
  }
#else
  {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d) {
      while ((e = readdir(d)) != 0) {
        size_t k = strlen(e->d_name);
        if (strncmp(e->d_name, "jv3_", 4) == 0 && k > 6 && strcmp(e->d_name + k - 6, ".world") == 0 && n < 64)
          snprintf(names[n++], sizeof names[0], "%s", e->d_name);
      }
      closedir(d);
    }
  }
#endif
  for (i = 0; i < n; i++) CHECK(fx_replay(dir, names[i]));
  if (n) printf("    %d jv3 fixtures replayed\n", n);
}
