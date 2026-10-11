/* Jar Factory's script machine (phase 2 of item behaviour; docs/superpowers/
 * specs/2026-10-09-jar-factory-design.md, "Item behaviour" and "Scripts").
 *
 * A script is bytecode, at most JI_SCRIPT_MAX (256) bytes, carried in an
 * item's record. The server compiles it from the readable language
 * (server/jarvm.py) and runs the same machine to test it; the device only
 * ever sees the bytes. The instructions are apps/jarvm.def, which both sides
 * read -- this file builds its tables from it.
 *
 * THE LAYOUT
 *   byte 0      format (JV_FORMAT); anything else and the script is ignored
 *   byte 1      entries, 0..JV_MAX_ENTRIES
 *   then        entries x { event, filter, offset of its code }
 *   then        code, to the end
 * One entry runs per event: the first whose event matches and whose filter
 * is 0 (any) or equals the event's argument -- for tick, the filter is a
 * period instead: every Nth tick. The compiler lists a filtered entry before
 * an unfiltered one for the same event, so the narrower one wins.
 *
 * THE MACHINE runs one entry: a stack of JV_STACK 16-bit values, the item's
 * JI_MEM memory slots, and at most JV_STEPS instructions. It reaches the
 * world only through JvIo -- a sense, an action, a random number -- so it
 * cannot touch files, the network, coins or another item's memory: there
 * is no instruction that could.
 *
 * jv_run says how it went:
 *   JV_DONE   END reached: the event is handled
 *   JV_NONE   no entry for this event: the recipe takes it
 *   JV_LIMIT  JV_STEPS instructions without END: stopped
 *   JV_FAULT  a stack under- or overflow, a jump or a read outside the
 *             script, an unknown instruction, a slot, sense or action out
 *             of range: stopped, nothing more is done
 *   JV_IGNORE not a script this machine knows (format, entries): ignored
 * Limit and fault are where the spec's "? bubble, recipe until the next
 * event" comes from; that is the caller's (apps/jarsim.h, js_item_event).
 *
 * jv_check walks a whole script before it is used -- every entry on an
 * instruction, every instruction known, every operand in range, every jump
 * on an instruction -- so an unknown instruction means the script is
 * ignored outright, as the spec asks, rather than met halfway through.
 *
 * Portable and libc-free, no allocation, like the rest of the jar headers;
 * host-tested in test/test_jarvm.c, which also replays the Python machine's
 * traces (test/fixtures/jarvm_*) and needs them to agree byte for byte.
 */
#ifndef CARDOS_JARVM_H
#define CARDOS_JARVM_H

#include <stdint.h>
#include "apps/jaritem.h"

#if defined(__GNUC__)
#define JV_OPT __attribute__((unused))
#else
#define JV_OPT
#endif

#define JV_FORMAT       1
#define JV_STACK        8
#define JV_STEPS        64
#define JV_MAX_ENTRIES  16

enum { JV_DONE = 0, JV_NONE = 1, JV_LIMIT = -1, JV_FAULT = -2, JV_IGNORE = -3 };

/* The opcodes, JVO_END .. , and JV_NOPS. */
#define JV_OP(c, n, o, p, q, d) JVO_##n = c,
#define JV_EVENT(v, n, e, f, d)
#define JV_ACTION(v, n, e, a, d)
#define JV_SENSE(v, n, e, d)
#define JV_NAME(g, n, v, e)
enum {
#include "apps/jarvm.def"
  JV_NOPS
};
#undef JV_OP
#undef JV_ACTION
#undef JV_SENSE

/* How many actions and senses there are. */
#define JV_OP(c, n, o, p, q, d)
#define JV_ACTION(v, n, e, a, d) + 1
#define JV_SENSE(v, n, e, d)
enum { JV_NACTIONS = 0
#include "apps/jarvm.def"
};
#undef JV_ACTION
#undef JV_SENSE
#define JV_ACTION(v, n, e, a, d)
#define JV_SENSE(v, n, e, d) + 1
enum { JV_NSENSES = 0
#include "apps/jarvm.def"
};
#undef JV_OP
#undef JV_EVENT
#undef JV_ACTION
#undef JV_SENSE
#undef JV_NAME

/* Per opcode: operand bytes (bits 0-1), pops (2-3), pushes (4-5). */
#define JV_OP(c, n, o, p, q, d) (uint8_t)((o) | ((p) << 2) | ((q) << 4)),
#define JV_EVENT(v, n, e, f, d)
#define JV_ACTION(v, n, e, a, d)
#define JV_SENSE(v, n, e, d)
#define JV_NAME(g, n, v, e)
static JV_OPT const uint8_t JV_INFO[JV_NOPS] = {
#include "apps/jarvm.def"
};
#undef JV_OP
#undef JV_EVENT
#undef JV_ACTION
#undef JV_SENSE
#undef JV_NAME

/* What the machine may reach: the item's senses (JSN_*), its actions (JA_*,
 * a refusal is not the machine's business) and a random number. */
typedef struct {
  void *ctx;
  int (*sense)(void *ctx, int sense);
  int (*act)(void *ctx, int action, int arg);
  uint32_t (*rnd)(void *ctx);
} JvIo;

static JV_OPT int16_t jv_w(int32_t v) { return (int16_t)(uint16_t)(v & 0xFFFF); }

/* Where the code starts, or -1 if the header is not one this machine reads. */
static JV_OPT int jv_code0(const uint8_t *s, int n) {
  int c;
  if (n < 2 || n > JI_SCRIPT_MAX || s[0] != JV_FORMAT || s[1] > JV_MAX_ENTRIES) return -1;
  c = 2 + 3 * s[1];
  return c <= n ? c : -1;
}

/* The whole script, before it is used. 0, or JV_IGNORE. */
#define JV_AT(t) ((t) < n && (at[(t) >> 3] >> ((t) & 7) & 1))
static JV_OPT int jv_check(const uint8_t *s, int n) {
  uint8_t at[JI_SCRIPT_MAX / 8];           /* a bit where each instruction starts */
  int c = jv_code0(s, n), pc, e;
  if (c < 0) return JV_IGNORE;
  ji_zero(at, (int)sizeof at);
  for (pc = c; pc < n; ) {
    int op = s[pc], k;
    if (op >= JV_NOPS) return JV_IGNORE;
    k = JV_INFO[op] & 3;
    if (pc + k >= n) return JV_IGNORE;      /* its operands run off the end */
    if ((op == JVO_LOAD || op == JVO_STORE) && s[pc + 1] >= JI_MEM) return JV_IGNORE;
    if (op == JVO_SENSE && s[pc + 1] >= JV_NSENSES) return JV_IGNORE;
    if ((op == JVO_ACT || op == JVO_ACTK) && s[pc + 1] >= JV_NACTIONS) return JV_IGNORE;
    at[pc >> 3] |= (uint8_t)(1 << (pc & 7));
    pc += 1 + k;
  }
  for (pc = c; pc < n; pc += 1 + (JV_INFO[s[pc]] & 3))
    if ((s[pc] == JVO_JMP || s[pc] == JVO_JZ) && !JV_AT(s[pc + 1]))
      return JV_IGNORE;
  for (e = 0; e < s[1]; e++)
    if (!JV_AT(s[4 + 3 * e])) return JV_IGNORE;
  return 0;
}

/* The entry for event `ev` with argument `arg`: its code offset, or -1. */
static JV_OPT int jv_entry(const uint8_t *s, int ev, int arg) {
  int e;
  for (e = 0; e < s[1]; e++) {
    const uint8_t *t = s + 2 + 3 * e;
    if (t[0] != ev) continue;
    if (ev == JE_TICK ? (t[1] > 1 && arg % t[1]) : (t[1] && t[1] != arg)) continue;
    return t[2];
  }
  return -1;
}

/* Run the entry for (ev, arg). `mem` is the item's JI_MEM slots, changed in
 * place; *steps, if given, is how many instructions ran. */
static JV_OPT int jv_run(const uint8_t *s, int n, int ev, int arg, int16_t *mem,
                         const JvIo *io, int *steps) {
  int16_t st[JV_STACK];
  int sp = 0, c = jv_code0(s, n), pc, i;
  if (steps) *steps = 0;
  if (c < 0) return JV_IGNORE;
  pc = jv_entry(s, ev, arg);
  if (pc < 0) return JV_NONE;
  for (i = 0; i < JV_STEPS; i++) {
    int op, k, nk, pops, a = 0, b = 0, x = 0, y = 0;
    if (pc < c || pc >= n || (op = s[pc]) >= JV_NOPS) return JV_FAULT;
    k = JV_INFO[op];
    nk = k & 3;
    pops = (k >> 2) & 3;
    if (pc + nk >= n || sp < pops || sp - pops + (k >> 4) > JV_STACK) return JV_FAULT;
    if (nk) a = s[pc + 1];
    if (nk == 2) b = s[pc + 2];
    pc += 1 + nk;
    if (pops == 2) y = st[--sp];
    if (pops) x = st[--sp];
    if (steps) *steps = i + 1;
    switch (op) {
    case JVO_END:    return JV_DONE;
    case JVO_PUSH8:  x = (int8_t)a; break;
    case JVO_PUSH16: x = (int16_t)(uint16_t)(a | (b << 8)); break;
    case JVO_LOAD:   if (a >= JI_MEM) return JV_FAULT; x = mem[a]; break;
    case JVO_STORE:  if (a >= JI_MEM) return JV_FAULT; mem[a] = (int16_t)x; break;
    case JVO_SENSE:
      if (a >= JV_NSENSES) return JV_FAULT;
      x = io->sense(io->ctx, a);
      x = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
      break;
    case JVO_RAND:   x = x > 0 ? (int)(io->rnd(io->ctx) % (uint32_t)x) : 0; break;
    case JVO_ADD:    x = x + y; break;
    case JVO_SUB:    x = x - y; break;
    case JVO_MUL:    x = x * y; break;
    case JVO_DIV:    x = y ? x / y : 0; break;
    case JVO_MOD:    x = y ? x % y : 0; break;
    case JVO_NEG:    x = -x; break;
    case JVO_EQ:     x = x == y; break;
    case JVO_NE:     x = x != y; break;
    case JVO_LT:     x = x < y; break;
    case JVO_LE:     x = x <= y; break;
    case JVO_GT:     x = x > y; break;
    case JVO_GE:     x = x >= y; break;
    case JVO_NOT:    x = !x; break;
    case JVO_AND:    x = x && y; break;
    case JVO_OR:     x = x || y; break;
    case JVO_BIT:    x = (y >= 0 && y <= 15) ? (x >> y) & 1 : 0; break;
    case JVO_JMP:    pc = a; break;
    case JVO_JZ:     if (!x) pc = a; break;
    default:                                   /* ACT, ACTK */
      if (a >= JV_NACTIONS) return JV_FAULT;
      io->act(io->ctx, a, op == JVO_ACTK ? b : x);
      break;
    }
    if (k >> 4) st[sp++] = jv_w(x);
  }
  return JV_LIMIT;
}

#endif /* CARDOS_JARVM_H */
