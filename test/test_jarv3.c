/* The jar's side of JarScript 3 (apps/jar/jarv3.h): a format-3 item runs on
 * its own machine in the jar's block, moves where its script says, leaves
 * room behind it, falls back to its recipe when the block is full -- and a
 * v2 item beside it runs exactly as before. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "apps/jar/jarsim.h"

static Jar J;
static uint8_t BLOCK[8192];

static void fresh(int cap) {
  memset(&J, 0, sizeof J);
  js_init(&J, 11);
  js_v3_open(&J, cap ? BLOCK : 0, cap);
}

static void run_s(int secs) {
  int i;
  for (i = 0; i < secs * JS_HZ; i++) js_step(&J);
}

static void mk_item(JItem *it, uint32_t id, int kind, int move) {
  int i;
  memset(it, 0, sizeof *it);
  it->id = id;
  it->kind = (uint8_t)kind;
  it->move = (uint8_t)move;
  it->speed = JSP_MEDIUM;
  it->zone = JZ_ANYWHERE;
  it->nframes = 2;
  strcpy(it->name, "Thing");
  for (i = 1; i < 8; i++) it->pal[i] = (uint16_t)(i * 0x0841);
  for (i = 0; i < 256; i++) { ji_set_px(it->frames[0], i, i & 7); ji_set_px(it->frames[1], i, 1); }
}

/* A v3 script by hand (the layout in the spec): one state whose body walks
 * DX px at 10 px/s and parks; a poke handler that says nothing but counts in
 * var 0 when `count` is set. */
static int mk_walker(uint8_t *s, int dx, int count) {
  int n = 0, code;
  s[n++] = 3;                     /* format */
  s[n++] = 0;                     /* flags */
  s[n++] = count ? 1 : 0;         /* vars */
  s[n++] = 1;                     /* states */
  s[n++] = count ? 1 : 0;         /* handlers */
  s[n++] = 0; s[n++] = 0; s[n++] = 0;   /* anims, perches, strings */
  s[n++] = 1;                     /* layer */
  s[n++] = 0;                     /* body */
  s[n++] = 0; s[n++] = 0; s[n++] = 0; s[n++] = 0;   /* no solid */
  if (count) { s[n++] = 0; s[n++] = 0; }            /* var 0 starts at 0 */
  code = n + 2 + (count ? 7 : 0);
  s[n++] = (uint8_t)code; s[n++] = 0;               /* state 0's body */
  if (count) {
    int h = code + 7;                               /* after the body's 7 bytes */
    s[n++] = 0; s[n++] = 0; s[n++] = 255;           /* poke, any arg, any state */
    s[n++] = 0; s[n++] = 0;                         /* param */
    s[n++] = (uint8_t)h; s[n++] = 0;
  }
  s[n++] = 2; s[n++] = (uint8_t)dx;                 /* PUSH8 dx */
  s[n++] = 2; s[n++] = 10;                          /* PUSH8 10 */
  s[n++] = 45;                                      /* WALK */
  s[n++] = 30;                                      /* AWAIT */
  s[n++] = 0;                                       /* HALT */
  if (count) {
    s[n++] = 4; s[n++] = 0;                         /* LDV 0 */
    s[n++] = 2; s[n++] = 1;                         /* PUSH8 1 */
    s[n++] = 8;                                     /* ADD */
    s[n++] = 5; s[n++] = 0;                         /* STV 0 */
    s[n++] = 1;                                     /* RET */
  }
  return n;
}

void test_jarv3_an_item_walks_where_its_script_says(void) {
  static JItem it;
  int i;
  fresh(sizeof BLOCK);
  mk_item(&it, 900, JK_CRITTER, JM_SITS);
  it.script_len = (uint16_t)mk_walker(it.script, 20, 0);
  CHECK_EQ(jv3_check(it.script, it.script_len), 0);
  i = js_place(&J, &it, 100, 0);
  CHECK_EQ(i, 0);
  CHECK_EQ(J.placed[i].v3, 1);
  CHECK_EQ(J.placed[i].slen, 0);                   /* never in the v2 spool */
  CHECK_EQ(J.placed[i].nhab, 0);
  run_s(3);                                        /* 20 px at 10 px/s: 2 s */
  CHECK_EQ(J.placed[i].x / JS_FX, 120);
  CHECK_EQ(js_item_y(&J, i), JS_SOIL - 8);         /* still on the soil */
  CHECK_EQ(J.placed[i].flip, 0);                   /* facing right */
}

void test_jarv3_a_poke_reaches_the_script_and_its_memory_persists(void) {
  static JItem it;
  int i;
  fresh(sizeof BLOCK);
  mk_item(&it, 901, JK_FLOOR, JM_SITS);
  it.script_len = (uint16_t)mk_walker(it.script, 0, 1);
  i = js_place(&J, &it, 60, 0);
  CHECK_EQ(J.placed[i].v3, 1);
  run_s(1);
  J.placed[i].mem_dirty = 0;
  js_item_event(&J, i, JE_POKE, 0);
  js_item_event(&J, i, JE_POKE, 0);
  run_s(1);
  CHECK_EQ(J.placed[i].mem[0], 2 * 16);            /* raw: two pokes */
  CHECK_EQ(J.placed[i].mem_dirty, 1);              /* so the save writes it */
}

void test_jarv3_leaving_closes_the_block_up(void) {
  static JItem a, b, c;
  int used0, n;
  fresh(sizeof BLOCK);
  used0 = J.v3used;
  mk_item(&a, 910, JK_CRITTER, JM_SITS);
  mk_item(&b, 911, JK_CRITTER, JM_SITS);
  mk_item(&c, 912, JK_CRITTER, JM_SITS);
  n = mk_walker(a.script, 30, 0);
  a.script_len = b.script_len = c.script_len = (uint16_t)n;
  memcpy(b.script, a.script, (size_t)n);
  memcpy(c.script, a.script, (size_t)n);
  js_place(&J, &a, 40, 0);
  js_place(&J, &b, 100, 0);
  js_place(&J, &c, 160, 0);
  CHECK_EQ(J.v3used, used0 + 3 * js_v3_chunk(n));
  js_unplace(&J, 0);
  CHECK_EQ(J.v3used, used0 + 2 * js_v3_chunk(n));
  CHECK_EQ(J.placed[0].id, 911);
  CHECK_EQ(J.placed[0].v3off, used0);              /* moved down */
  run_s(4);                                        /* both still walk, from where they were */
  CHECK_EQ(J.placed[0].x / JS_FX, 130);
  CHECK_EQ(J.placed[1].x / JS_FX, 190);
}

void test_jarv3_no_room_places_it_on_its_recipe(void) {
  static JItem it;
  int i;
  fresh(0);                                        /* no block at all */
  mk_item(&it, 920, JK_CRITTER, JM_SITS);
  it.nhab = 1;
  it.hab[0].event = JE_POKE;
  it.hab[0].action = JA_FLIP;
  it.script_len = (uint16_t)mk_walker(it.script, 20, 0);
  i = js_place(&J, &it, 100, 0);
  CHECK(i >= 0);
  CHECK_EQ(J.placed[i].v3, 0);
  CHECK_EQ(J.placed[i].nhab, 1);                   /* its recipe still works */
  CHECK_EQ(J.v3short, 1);
  js_item_event(&J, i, JE_POKE, 0);
  CHECK_EQ(J.placed[i].flip, 1);
}

void test_jarv3_a_v2_item_beside_it_is_untouched(void) {
  /* a v2 item keeps its recipe and its wandering with a v3 item in the jar */
  static JItem v2, v3;
  int a, b, moved = 0, k;
  int32_t x0;
  fresh(sizeof BLOCK);
  mk_item(&v2, 930, JK_CRITTER, JM_WANDERS);
  v2.nhab = 1;
  v2.hab[0].event = JE_POKE;
  v2.hab[0].action = JA_FLIP;
  a = js_place(&J, &v2, 60, 0);
  mk_item(&v3, 931, JK_FLOOR, JM_SITS);
  v3.script_len = (uint16_t)mk_walker(v3.script, 0, 0);
  b = js_place(&J, &v3, 200, 0);
  CHECK_EQ(J.placed[a].v3, 0);
  CHECK_EQ(J.placed[b].v3, 1);
  x0 = J.placed[a].x;
  for (k = 0; k < 60 * JS_HZ && !moved; k++) { js_step(&J); moved = J.placed[a].x != x0; }
  CHECK(moved);
  J.placed[a].flip = 0;
  CHECK_EQ(js_item_event(&J, a, JE_POKE, 0), 1);
  CHECK_EQ(J.placed[a].flip, 1);
}

static int WANTS;
static void want_block(void *jar) { WANTS++; js_v3_open((Jar *)jar, BLOCK, (int)sizeof BLOCK); }

void test_jarv3_the_block_is_asked_for_only_by_a_v3_item(void) {
  static JItem v2, v3;
  fresh(0);
  WANTS = 0;
  J.v3_want = want_block;
  mk_item(&v2, 940, JK_CRITTER, JM_WANDERS);
  js_place(&J, &v2, 60, 0);
  run_s(1);
  CHECK_EQ(WANTS, 0);                              /* a v2 jar costs nothing */
  mk_item(&v3, 941, JK_CRITTER, JM_SITS);
  v3.script_len = (uint16_t)mk_walker(v3.script, 20, 0);
  CHECK_EQ(J.placed[js_place(&J, &v3, 100, 0)].v3, 1);
  CHECK_EQ(WANTS, 1);
  run_s(3);
  CHECK_EQ(J.placed[1].x / JS_FX, 120);
}
