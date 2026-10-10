/* Jar Factory's script machine on the host: apps/jarvm.h, its table
 * (apps/jarvm.def) against the enums the jar uses, the Python machine's
 * traces replayed (test/fixtures/jarvm_*, written by
 * tools/make_jarvm_fixtures.py), and the hook in apps/jarsim.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "apps/jarsim.h"

/* ---- a small world for the machine alone --------------------------------- */

typedef struct {
  uint32_t seed;
  int sense[16];
  char *out;                /* the trace, appended to */
  int n, cap;
  int acts, last_a, last_arg;
} World;

static void w_put(World *w, const char *s) {
  int k = (int)strlen(s);
  if (w->n + k + 1 > w->cap) return;
  memcpy(w->out + w->n, s, (size_t)k + 1);
  w->n += k;
}

static uint32_t w_rnd(void *c) {                 /* js_rnd's xorshift */
  World *w = (World *)c;
  uint32_t x = w->seed ? w->seed : 0x9E3779B9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  w->seed = x;
  return x;
}

static int w_sense(void *c, int s) {
  World *w = (World *)c;
  return s == JSN_RANDOM ? (int)(w_rnd(c) & 255) : w->sense[s];
}

static int w_act(void *c, int a, int arg) {
  World *w = (World *)c;
  char line[48];
  snprintf(line, sizeof line, "act %d %d\n", a, arg);
  w_put(w, line);
  w->acts++;
  w->last_a = a;
  w->last_arg = arg;
  return 0;
}

static char W_BUF[16384];

static void w_init(World *w, uint32_t seed) {
  memset(w, 0, sizeof *w);
  w->seed = seed;
  w->out = W_BUF;
  w->cap = (int)sizeof W_BUF;
  W_BUF[0] = 0;
}

static JvIo w_io(World *w) {
  JvIo io;
  io.ctx = w;
  io.sense = w_sense;
  io.act = w_act;
  io.rnd = w_rnd;
  return io;
}

/* A script by hand: one entry for `ev` (filter 0) and its code. */
static int one(uint8_t *s, int ev, const uint8_t *code, int n) {
  s[0] = JV_FORMAT; s[1] = 1;
  s[2] = (uint8_t)ev; s[3] = 0; s[4] = 5;
  memcpy(s + 5, code, (size_t)n);
  return 5 + n;
}

static int run1(const uint8_t *s, int n, int ev, int arg, int16_t *mem, World *w, int *steps) {
  JvIo io = w_io(w);
  return jv_run(s, n, ev, arg, mem, &io, steps);
}

/* ---- the table ------------------------------------------------------------ */

void test_jarvm_table_matches_the_jar(void) {
  int k = 0;
  /* opcodes are dense and in order */
#define JV_OP(c, n, o, p, q, d) CHECK_EQ(c, k); CHECK_EQ(JVO_##n, c); CHECK(o <= 2 && p <= 2 && q <= 1); k++;
#define JV_EVENT(v, n, e, f, d) CHECK_EQ(v, e);
#define JV_ACTION(v, n, e, a, d) CHECK_EQ(v, e);
#define JV_SENSE(v, n, e, d) CHECK_EQ(v, e);
#define JV_NAME(g, n, v, e) CHECK_EQ(v, e);
#include "apps/jarvm.def"
#undef JV_OP
#undef JV_EVENT
#undef JV_ACTION
#undef JV_SENSE
#undef JV_NAME
  CHECK_EQ(k, JV_NOPS);
  CHECK_EQ(JV_NACTIONS, JA_KINDS);
  CHECK_EQ(JV_NSENSES, JSN_KINDS);
  CHECK_EQ(JI_SCRIPT_MAX, 256);
}

/* ---- the machine ----------------------------------------------------------- */

void test_jarvm_runs_and_returns(void) {
  uint8_t s[64];
  int16_t mem[JI_MEM] = { 0 };
  World w;
  int steps, n;
  const uint8_t code[] = { JVO_PUSH8, 5, JVO_PUSH16, 0x10, 0x27, JVO_ADD, JVO_STORE, 2,
                           JVO_ACTK, JA_SAY, 1, JVO_LOAD, 2, JVO_ACT, JA_HOP, JVO_END };
  w_init(&w, 1);
  n = one(s, JE_POKE, code, (int)sizeof code);
  CHECK_EQ(jv_check(s, n), 0);
  CHECK_EQ(run1(s, n, JE_POKE, 0, mem, &w, &steps), JV_DONE);
  CHECK_EQ(steps, 8);
  CHECK_EQ(mem[2], 10005);
  CHECK(strcmp(W_BUF, "act 10 1\nact 1 10005\n") == 0);
  /* no entry for this event: nothing runs */
  CHECK_EQ(run1(s, n, JE_TICK, 0, mem, &w, &steps), JV_NONE);
  CHECK_EQ(steps, 0);
  CHECK_EQ(w.acts, 2);
}

void test_jarvm_filters_pick_one_entry(void) {
  /* on near snail -> say 0; on near -> say 1; on tick every 3 -> hop */
  const uint8_t s[] = { JV_FORMAT, 3,
                        JE_NEAR, JN_SNAIL, 11, JE_NEAR, 0, 15, JE_TICK, 3, 19,
                        JVO_ACTK, JA_SAY, 0, JVO_END,
                        JVO_ACTK, JA_SAY, 1, JVO_END,
                        JVO_ACTK, JA_HOP, 0, JVO_END };
  int16_t mem[JI_MEM] = { 0 };
  World w;
  int t, hops = 0;
  w_init(&w, 1);
  CHECK_EQ(jv_check(s, (int)sizeof s), 0);
  CHECK_EQ(run1(s, (int)sizeof s, JE_NEAR, JN_SNAIL, mem, &w, 0), JV_DONE);
  CHECK_EQ(w.last_arg, 0);
  CHECK_EQ(run1(s, (int)sizeof s, JE_NEAR, JN_MOSS, mem, &w, 0), JV_DONE);
  CHECK_EQ(w.last_arg, 1);
  for (t = 0; t < 9; t++) {
    int r = run1(s, (int)sizeof s, JE_TICK, t, mem, &w, 0);
    if (r == JV_DONE) hops++;
    CHECK_EQ(r, t % 3 ? JV_NONE : JV_DONE);
  }
  CHECK_EQ(hops, 3);
}

void test_jarvm_step_limit_stops(void) {
  uint8_t s[64];
  int16_t mem[JI_MEM] = { 0 };
  World w;
  int steps, n;
  const uint8_t loop[] = { JVO_ACTK, JA_FLIP, 0, JVO_JMP, 5 };
  w_init(&w, 1);
  n = one(s, JE_POKE, loop, (int)sizeof loop);
  CHECK_EQ(jv_check(s, n), 0);                     /* a loop is legal; it just stops */
  CHECK_EQ(run1(s, n, JE_POKE, 0, mem, &w, &steps), JV_LIMIT);
  CHECK_EQ(steps, JV_STEPS);
  CHECK_EQ(w.acts, JV_STEPS / 2);
}

void test_jarvm_faults_stop_safely(void) {
  uint8_t s[64];
  int16_t mem[JI_MEM] = { 0 };
  World w;
  int n, i;
  const uint8_t under[] = { JVO_ADD, JVO_END };
  const uint8_t over[] = { JVO_PUSH8, 1, JVO_PUSH8, 1, JVO_PUSH8, 1, JVO_PUSH8, 1, JVO_PUSH8, 1,
                           JVO_PUSH8, 1, JVO_PUSH8, 1, JVO_PUSH8, 1, JVO_PUSH8, 1, JVO_END };
  const uint8_t far[] = { JVO_JMP, 250, JVO_END };
  const uint8_t back[] = { JVO_JMP, 1, JVO_END };   /* into the entry table */
  const uint8_t unk[] = { 0xEE, JVO_END };
  const uint8_t slot[] = { JVO_STORE, 8, JVO_END };
  const uint8_t sense[] = { JVO_SENSE, 99, JVO_END };
  const uint8_t act[] = { JVO_ACTK, 99, 0, JVO_END };
  const uint8_t cut[] = { JVO_PUSH16, 1 };
  const uint8_t tail[] = { JVO_ACTK, JA_HOP, 0 };   /* runs off the end */
  struct { const uint8_t *c; int n; } bad[] = {
    { under, 2 }, { over, 19 }, { far, 3 }, { back, 3 }, { unk, 2 }, { slot, 3 },
    { sense, 3 }, { act, 4 }, { cut, 2 }, { tail, 3 },
  };
  for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
    w_init(&w, 1);
    n = one(s, JE_POKE, bad[i].c, bad[i].n);
    CHECK_EQ(run1(s, n, JE_POKE, 0, mem, &w, 0), JV_FAULT);
  }
  for (i = 0; i < JI_MEM; i++) CHECK_EQ(mem[i], 0);
  /* jv_check refuses all but the ones that only go wrong while running */
  for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
    int ok;
    n = one(s, JE_POKE, bad[i].c, bad[i].n);
    ok = jv_check(s, n) == 0;
    CHECK_EQ(ok, i == 0 || i == 1 || i == 9);     /* under, over, tail */
  }
}

void test_jarvm_ignores_what_it_does_not_know(void) {
  uint8_t s[64];
  int16_t mem[JI_MEM] = { 0 };
  World w;
  int n;
  const uint8_t code[] = { JVO_ACTK, JA_HOP, 0, JVO_END };
  w_init(&w, 1);
  n = one(s, JE_POKE, code, (int)sizeof code);
  s[0] = JV_FORMAT + 1;                            /* a newer format */
  CHECK_EQ(jv_check(s, n), JV_IGNORE);
  CHECK_EQ(run1(s, n, JE_POKE, 0, mem, &w, 0), JV_IGNORE);
  s[0] = JV_FORMAT;
  s[1] = 40;                                       /* entries past the end */
  CHECK_EQ(jv_check(s, n), JV_IGNORE);
  CHECK_EQ(run1(s, n, JE_POKE, 0, mem, &w, 0), JV_IGNORE);
  s[1] = 1;
  s[4] = 6;                                        /* an entry inside an instruction */
  CHECK_EQ(jv_check(s, n), JV_IGNORE);
  CHECK_EQ(run1(s, 1, JE_POKE, 0, mem, &w, 0), JV_IGNORE);
  CHECK_EQ(w.acts, 0);
}

void test_jarvm_arithmetic_wraps_like_c(void) {
  uint8_t s[64];
  int16_t mem[JI_MEM] = { 0 };
  World w;
  int n;
  /* 30000 + 30000; -32768 / -1; 7 / 0; -7 % 2; -7 / 2 */
  {
    const uint8_t c2[] = {
      JVO_PUSH16, 0x30, 0x75, JVO_PUSH16, 0x30, 0x75, JVO_ADD, JVO_STORE, 0,
      JVO_PUSH16, 0x00, 0x80, JVO_PUSH8, 0xFF, JVO_DIV, JVO_STORE, 1,
      JVO_PUSH8, 7, JVO_PUSH8, 0, JVO_DIV, JVO_STORE, 2,
      JVO_PUSH8, 0xF9, JVO_PUSH8, 2, JVO_MOD, JVO_STORE, 3,
      JVO_PUSH8, 0xF9, JVO_PUSH8, 2, JVO_DIV, JVO_STORE, 4,
      JVO_END };
    w_init(&w, 1);
    n = one(s, JE_POKE, c2, (int)sizeof c2);
    CHECK_EQ(jv_check(s, n), 0);
    CHECK_EQ(run1(s, n, JE_POKE, 0, mem, &w, 0), JV_DONE);
    CHECK_EQ(mem[0], -5536);
    CHECK_EQ(mem[1], -32768);
    CHECK_EQ(mem[2], 0);
    CHECK_EQ(mem[3], -1);
    CHECK_EQ(mem[4], -3);
  }
}

/* ---- the Python machine's traces, replayed --------------------------------- */

static void fx_path(char *out, int cap, const char *name) {
  const char *f = __FILE__, *cut = 0, *p;
  for (p = f; *p; p++) if (*p == '/' || *p == '\\') cut = p + 1;
  if (cut) snprintf(out, (size_t)cap, "%.*sfixtures/%s", (int)(cut - f), f, name);
  else snprintf(out, (size_t)cap, "test/fixtures/%s", name);
}

static int fx_read(const char *name, char *buf, int cap) {
  char path[512];
  FILE *f;
  int n;
  fx_path(path, (int)sizeof path, name);
  f = fopen(path, "rb");
  if (!f) {
    snprintf(path, sizeof path, "test/fixtures/%s", name);
    f = fopen(path, "rb");
  }
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

static char FX_TXT[16384], FX_LIST[2048];

/* One fixture: 1 if the C machine's trace is the Python one. */
static int fx_replay(const char *name) {
  char file[96], *line, *want, *next;
  uint8_t s[512];
  int n, i;
  int16_t mem[JI_MEM] = { 0 };
  World w;
  JvIo io;
  snprintf(file, sizeof file, "jarvm_%s.bin", name);
  n = fx_read(file, (char *)s, (int)sizeof s);
  snprintf(file, sizeof file, "jarvm_%s.txt", name);
  if (n < 0 || fx_read(file, FX_TXT, (int)sizeof FX_TXT) < 0) {
    printf("    fixture %s is missing\n", name);
    return 0;
  }
  no_cr(FX_TXT);
  want = strstr(FX_TXT, "\n--\n");
  if (!want) return 0;
  *want = 0;
  want += 4;
  w_init(&w, 0);
  io = w_io(&w);
  {
    char head[32];
    snprintf(head, sizeof head, "check %d\n", jv_check(s, n));
    w_put(&w, head);
  }
  for (line = FX_TXT; line && *line; line = next) {
    int a, b;
    next = strchr(line, '\n');
    if (next) *next++ = 0;
    if (sscanf(line, "seed %d", &a) == 1) w.seed = (uint32_t)a;
    else if (sscanf(line, "sense %d %d", &a, &b) == 2 && a >= 0 && a < 16) w.sense[a] = b;
    else if (strncmp(line, "mem ", 4) == 0) {
      char *p = line + 4;
      for (i = 0; i < JI_MEM; i++) mem[i] = (int16_t)strtol(p, &p, 10);
    } else if (sscanf(line, "ev %d %d", &a, &b) == 2) {
      char out[96];
      int steps, r = jv_run(s, n, a, b, mem, &io, &steps);
      snprintf(out, sizeof out, "ret %d %d\nmem %d %d %d %d %d %d %d %d\n", r, steps,
               mem[0], mem[1], mem[2], mem[3], mem[4], mem[5], mem[6], mem[7]);
      w_put(&w, out);
    }
  }
  if (strcmp(W_BUF, want) != 0) {
    printf("    fixture %s: the C machine says\n%s    and the Python one\n%s", name, W_BUF, want);
    return 0;
  }
  return 1;
}

void test_jarvm_agrees_with_the_python_machine(void) {
  char *name, *next;
  int count = 0;
  CHECK(fx_read("jarvm_list.txt", FX_LIST, (int)sizeof FX_LIST) > 0);
  no_cr(FX_LIST);
  for (name = FX_LIST; name && *name; name = next) {
    next = strchr(name, '\n');
    if (next) *next++ = 0;
    if (!*name) continue;
    CHECK(fx_replay(name));
    count++;
  }
  CHECK(count >= 15);
}

/* ---- the hook in apps/jarsim.h ------------------------------------------- */

static Jar J;

static void mk_item(JItem *it, uint32_t id, const uint8_t *script, int n) {
  int i;
  memset(it, 0, sizeof *it);
  it->id = id;
  it->kind = JK_CRITTER;
  it->move = JM_SITS;
  it->speed = JSP_MEDIUM;
  it->zone = JZ_ANYWHERE;
  it->nframes = 1;
  it->nbub = 2;
  strcpy(it->bub[0], "hi");
  strcpy(it->bub[1], "bye");
  strcpy(it->name, "Scripty");
  for (i = 1; i < 8; i++) it->pal[i] = (uint16_t)(i * 0x0841);
  for (i = 0; i < 256; i++) ji_set_px(it->frames[0], i, 1 + (i & 3));
  /* the recipe: poke glows, tick flips, gift gives a heart */
  it->nhab = 3;
  it->hab[0].event = JE_POKE;  it->hab[0].action = JA_GLOW;     it->hab[0].aarg = 1;
  it->hab[1].event = JE_TICK;  it->hab[1].action = JA_FLIP;
  it->hab[2].event = JE_GIFT;  it->hab[2].action = JA_PARTICLE; it->hab[2].aarg = JP_HEART;
  memcpy(it->script, script, (size_t)n);
  it->script_len = (uint16_t)n;
}

static void fresh_jar(void) {
  memset(&J, 0, sizeof J);
  js_init(&J, 5);
}

static int hearts(void) {
  int i, n = 0;
  for (i = 0; i < JS_PARTS; i++) if (J.part[i].life && J.part[i].type == JP_HEART) n++;
  return n;
}

void test_jarvm_script_takes_the_event_from_the_recipe(void) {
  /* on poke: say 2 */
  const uint8_t s[] = { JV_FORMAT, 1, JE_POKE, 0, 5, JVO_ACTK, JA_SAY, 1, JVO_END };
  static JItem it;
  int i;
  fresh_jar();
  mk_item(&it, 900, s, (int)sizeof s);
  i = js_place(&J, &it, 60, 0);
  CHECK(i >= 0);
  CHECK_EQ(J.placed[i].slen, (int)sizeof s);
  CHECK_EQ(js_item_event(&J, i, JE_POKE, 0), 1);
  CHECK_EQ(J.placed[i].say, 1);                    /* the script spoke */
  CHECK(J.placed[i].say_t > 0);
  CHECK_EQ(J.placed[i].glow, 0);                   /* the recipe did not glow */
  /* an event the script has no handler for is the recipe's */
  CHECK_EQ(J.placed[i].flip, 0);
  js_item_event(&J, i, JE_TICK, 0);
  CHECK_EQ(J.placed[i].flip, 1);
}

void test_jarvm_a_stopped_script_shows_a_question_and_falls_back(void) {
  /* on gift: flip forever; on poke: the stack runs dry */
  const uint8_t s[] = { JV_FORMAT, 2, JE_GIFT, 0, 8, JE_POKE, 0, 13,
                        JVO_ACTK, JA_FLIP, 0, JVO_JMP, 8,
                        JVO_ADD, JVO_END };
  static JItem it;
  int i;
  fresh_jar();
  mk_item(&it, 901, s, (int)sizeof s);
  i = js_place(&J, &it, 60, 0);
  CHECK(i >= 0 && J.placed[i].slen > 0);
  CHECK_EQ(hearts(), 0);
  js_item_event(&J, i, JE_GIFT, 0);                /* the step limit */
  CHECK_EQ(J.placed[i].say, JI_MAX_BUB);
  CHECK(strcmp(J.placed[i].bub[JI_MAX_BUB], "?") == 0);
  CHECK(J.placed[i].say_t > 0);
  CHECK_EQ(hearts(), 1);                           /* and the recipe had the gift */
  J.placed[i].say = -1;
  J.placed[i].say_t = 0;
  js_item_event(&J, i, JE_POKE, 0);                /* a fault */
  CHECK_EQ(J.placed[i].say, JI_MAX_BUB);
  CHECK_EQ(J.placed[i].glow, 1);                   /* the recipe's poke */
  /* the next event asks the script again, and it stops again */
  J.placed[i].glow = 0;
  js_item_event(&J, i, JE_POKE, 0);
  CHECK_EQ(J.placed[i].glow, 1);
}

void test_jarvm_a_script_it_cannot_read_is_left_out(void) {
  const uint8_t newer[] = { JV_FORMAT + 1, 1, JE_POKE, 0, 5, JVO_ACTK, JA_SAY, 1, JVO_END };
  const uint8_t unknown[] = { JV_FORMAT, 1, JE_POKE, 0, 5, 0xEE, JVO_END };
  static JItem it;
  int i;
  fresh_jar();
  mk_item(&it, 902, newer, (int)sizeof newer);
  i = js_place(&J, &it, 60, 0);
  CHECK_EQ(J.placed[i].slen, 0);
  js_item_event(&J, i, JE_POKE, 0);
  CHECK_EQ(J.placed[i].glow, 1);                   /* the recipe, as in phase 1 */
  CHECK_EQ(J.placed[i].say_t, 0);                  /* and no ? */
  mk_item(&it, 903, unknown, (int)sizeof unknown);
  i = js_place(&J, &it, 90, 0);
  CHECK_EQ(J.placed[i].slen, 0);
  CHECK_EQ(J.sused, 0);
}

void test_jarvm_memory_persists_through_the_record(void) {
  /* on poke: mem[3] = mem[3] + 1; say 1 */
  const uint8_t s[] = { JV_FORMAT, 1, JE_POKE, 0, 5,
                        JVO_LOAD, 3, JVO_PUSH8, 1, JVO_ADD, JVO_STORE, 3,
                        JVO_ACTK, JA_SAY, 0, JVO_END };
  static JItem it, back;
  static uint8_t rec[JI_MAX];
  int i, n, k;
  fresh_jar();
  mk_item(&it, 904, s, (int)sizeof s);
  it.mem[3] = 40;
  i = js_place(&J, &it, 60, 0);
  CHECK_EQ(J.placed[i].mem_dirty, 0);
  for (k = 0; k < 3; k++) js_item_event(&J, i, JE_POKE, 0);
  CHECK_EQ(J.placed[i].mem[3], 43);
  CHECK_EQ(J.placed[i].mem_dirty, 1);
  /* the app, saving: the record gets the slots, once */
  CHECK_EQ(js_mem_sync(&J, i, &it), 1);
  CHECK_EQ(js_mem_sync(&J, i, &it), 0);
  CHECK_EQ(it.mem[3], 43);
  n = jitem_encode(&it, rec, (int)sizeof rec);
  CHECK(n > 0);
  CHECK_EQ(jitem_decode(&back, rec, n), 0);
  CHECK_EQ(back.mem[3], 43);
  CHECK_EQ(back.script_len, (int)sizeof s);
  /* a later session goes on counting from there */
  fresh_jar();
  i = js_place(&J, &back, 60, 0);
  js_item_event(&J, i, JE_POKE, 0);
  CHECK_EQ(J.placed[i].mem[3], 44);
  /* the wrong item's record is never written */
  back.id = 1;
  CHECK_EQ(js_mem_sync(&J, i, &back), 0);
}

void test_jarvm_scripts_share_a_pool_and_close_up(void) {
  /* three items: 910 and 912 say their first bubble on poke, 911 its second */
  uint8_t s[3][9];
  static JItem it;
  int k, i;
  fresh_jar();
  for (k = 0; k < 3; k++) {
    const uint8_t one_[] = { JV_FORMAT, 1, JE_POKE, 0, 5, JVO_ACTK, JA_SAY, (uint8_t)(k & 1), JVO_END };
    memcpy(s[k], one_, sizeof one_);
    mk_item(&it, 910 + (uint32_t)k, s[k], 9);
    CHECK_EQ(js_place(&J, &it, 40 + 50 * k, 0), k);
  }
  CHECK_EQ(J.sused, 27);
  js_unplace(&J, 0);
  CHECK_EQ(J.sused, 18);
  CHECK_EQ(J.placed[0].soff, 0);
  CHECK_EQ(J.placed[1].soff, 9);
  for (i = 0; i < 2; i++) {
    js_item_event(&J, i, JE_POKE, 0);
    CHECK_EQ(J.placed[i].say, (i + 1) & 1);
  }
  /* a full pool: the item goes in, its script does not */
  J.sused = JS_SPOOL - 4;
  mk_item(&it, 920, s[0], 9);
  i = js_place(&J, &it, 200, 0);
  CHECK(i >= 0);
  CHECK_EQ(J.placed[i].slen, 0);
}
