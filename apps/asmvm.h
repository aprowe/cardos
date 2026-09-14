/* A small machine, an assembler for it, and a compiler from it to Xtensa.
 *
 * See docs/superpowers/specs/2026-09-13-asm-vm-and-native-compiler-design.md.
 *
 * Everything here is portable integer C with no UI in it, so the whole thing
 * runs under the host test suite -- which matters more than usual, because
 * the output is machine code for a chip with no memory protection, and a
 * wrong instruction template is a reboot rather than a stack trace.
 *
 * THE ONE IDEA. The virtual machine's instruction set was invented to be a
 * near-subset of Xtensa, so compiling it is template expansion rather than
 * compilation. Twelve registers, r0-r11, map 1:1 onto a2-a13, which is
 * exactly what the call0 ABI leaves after a0 (return address), a1 (stack),
 * a14 (this compiler's scratch) and a15 (the data base). Because the mapping
 * is fixed, THERE IS NO REGISTER ALLOCATOR -- the hardest part of a compiler
 * does not exist here, and that is the whole reason this is tractable.
 *
 * Every encoding below was taken from xtensa-esp32s3-elf-as on this machine
 * and is pinned by a golden test, never transcribed from a manual.
 */
#ifndef CARDOS_ASMVM_H
#define CARDOS_ASMVM_H

#include <stdint.h>

#define ASM_REGS      12
#define ASM_MAXCODE   192           /* VM instructions in a program */
#define ASM_MAXLABEL  32
#define ASM_MAXLIT    16
#define ASM_LABELLEN  12
#define ASM_DATA      1024          /* bytes a program may address */
#define ASM_SLACK     256           /* see check_slack() -- do not shrink */
#define ASM_STEPS     2000000       /* runaway-loop cutoff for the interpreter */

/* ---- the instruction set --------------------------------------------------
 *
 * Each one is chosen because it is a single Xtensa instruction. Anything that
 * would need two is either absent or a sys call. */
typedef enum {
  OP_NOP = 0,
  OP_MOV, OP_MOVI, OP_LIT,
  OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR, OP_MUL, OP_DIV,
  OP_ADDI, OP_SHL, OP_SHR,
  OP_LD, OP_ST, OP_LDB, OP_STB,
  OP_BEQ, OP_BNE, OP_BLT, OP_BGE,
  OP_JMP, OP_CALL, OP_RET, OP_SYS, OP_HALT,
  OP__COUNT
} AsmOp;

/* What a sys call does. The interpreter switches on it; compiled code calls
 * one C trampoline with the same number, so both back ends reach identical
 * behaviour through the same function. */
enum { SYS_PUTC = 0, SYS_PUTI, SYS_TICKS, SYS_KEY, SYS__COUNT };

typedef struct {
  uint8_t  op;
  uint8_t  a, b;                    /* register operands */
  int32_t  imm;                     /* immediate, or a label's index */
} AsmIns;

/* Why a run stopped. */
typedef enum {
  RUN_OK = 0, RUN_HALT, RUN_FAULT, RUN_STEPS, RUN_NOCODE, RUN__COUNT
} AsmStop;

/* Registers and memory, shared by both back ends so their results can be
 * compared byte for byte. The compiled entry point is handed a pointer to
 * one of these and reads and writes it directly. */
typedef struct {
  int32_t  r[ASM_REGS];
  uint32_t pc;
  uint32_t fault_addr;
  uint8_t  mem[ASM_DATA + ASM_SLACK];
} AsmState;

typedef struct {
  AsmIns   code[ASM_MAXCODE];
  int      n;
  int32_t  lit[ASM_MAXLIT];         /* constants too wide for movi */
  int      nlit;
  int      checked;                 /* emit bounds checks when compiling */
  int      err_line;                /* 0 when the assembly succeeded */
  char     err[48];
} AsmProgram;

/* ---- text in, instructions out --------------------------------------------
 *
 * Two passes: labels are collected first, so a forward branch resolves
 * without backpatching at this level. Errors carry the line number, because
 * an assembler that says "syntax error" and not where is worse than none.
 */

typedef struct {
  char name[ASM_LABELLEN];
  int  at;
} AsmLabel;

static int asm_is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int asm_is_digit(char c) { return c >= '0' && c <= '9'; }

static int asm_is_word(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         asm_is_digit(c) || c == '_';
}

static char asm_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int asm_streq(const char *a, const char *b) {
  while (*a && *b) { if (asm_lower(*a) != asm_lower(*b)) return 0; a++; b++; }
  return !*a && !*b;
}

/* A word of [A-Za-z0-9_] into out, advancing p. Returns its length. */
static int asm_word(const char **p, char *out, int max) {
  int n = 0;
  while (asm_is_space(**p)) (*p)++;
  while (asm_is_word(**p) && n + 1 < max) out[n++] = *(*p)++;
  out[n] = 0;
  return n;
}

static void asm_skip_sep(const char **p) {
  while (asm_is_space(**p) || **p == ',') (*p)++;
}

/* A signed decimal or 0x hex literal. Returns 0 if there is no number. */
static int asm_number(const char **p, int32_t *out) {
  int neg = 0;
  int32_t v = 0;
  int any = 0;
  while (asm_is_space(**p)) (*p)++;
  if (**p == '-') { neg = 1; (*p)++; }
  else if (**p == '+') (*p)++;
  if (**p == '0' && (( *p)[1] == 'x' || (*p)[1] == 'X')) {
    (*p) += 2;
    for (;;) {
      char c = asm_lower(**p);
      int d;
      if (asm_is_digit(c)) d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else break;
      v = v * 16 + d;
      any = 1;
      (*p)++;
    }
  } else {
    while (asm_is_digit(**p)) { v = v * 10 + (**p - '0'); any = 1; (*p)++; }
  }
  if (!any) return 0;
  *out = neg ? -v : v;
  return 1;
}

/* "r7" -> 7. Returns -1 for anything else, including a register out of range,
 * which is a mistake worth naming rather than silently clamping. */
static int asm_reg(const char *w) {
  int v = 0, i = 1;
  if (asm_lower(w[0]) != 'r' || !w[1]) return -1;
  for (i = 1; w[i]; i++) {
    if (!asm_is_digit(w[i])) return -1;
    v = v * 10 + (w[i] - '0');
  }
  return v < ASM_REGS ? v : -1;
}

static const struct { const char *name; uint8_t op; uint8_t form; } ASM_OPS[] = {
  /* form: 0 none, 1 rd, 2 rd+imm, 3 rd+rs, 4 rd+rs+rt, 5 rd+[rs+imm],
   *       6 rs+rt+label, 7 label, 8 imm */
  { "nop",  OP_NOP,  0 },
  { "mov",  OP_MOV,  3 },
  { "movi", OP_MOVI, 2 },
  { "lit",  OP_LIT,  2 },
  { "add",  OP_ADD,  4 },
  { "sub",  OP_SUB,  4 },
  { "and",  OP_AND,  4 },
  { "or",   OP_OR,   4 },
  { "xor",  OP_XOR,  4 },
  { "mul",  OP_MUL,  4 },
  { "div",  OP_DIV,  4 },
  { "addi", OP_ADDI, 2 },
  { "shl",  OP_SHL,  2 },
  { "shr",  OP_SHR,  2 },
  { "ld",   OP_LD,   5 },
  { "st",   OP_ST,   5 },
  { "ldb",  OP_LDB,  5 },
  { "stb",  OP_STB,  5 },
  { "beq",  OP_BEQ,  6 },
  { "bne",  OP_BNE,  6 },
  { "blt",  OP_BLT,  6 },
  { "bge",  OP_BGE,  6 },
  { "jmp",  OP_JMP,  7 },
  { "call", OP_CALL, 7 },
  { "ret",  OP_RET,  0 },
  { "sys",  OP_SYS,  8 },
  { "halt", OP_HALT, 0 },
};
#define ASM_NOPS ((int)(sizeof ASM_OPS / sizeof ASM_OPS[0]))

static void asm_fail(AsmProgram *p, int line, const char *msg) {
  int i = 0;
  if (p->err_line) return;                  /* keep the first, not the last */
  p->err_line = line;
  while (msg[i] && i + 1 < (int)sizeof p->err) { p->err[i] = msg[i]; i++; }
  p->err[i] = 0;
}

/* Add a constant to the pool, reusing one that is already there. */
static int asm_intern(AsmProgram *p, int32_t v) {
  int i;
  for (i = 0; i < p->nlit; i++) if (p->lit[i] == v) return i;
  if (p->nlit >= ASM_MAXLIT) return -1;
  p->lit[p->nlit] = v;
  return p->nlit++;
}

/* `src` is the whole program, newline separated. Returns 1 on success; on
 * failure p->err_line and p->err say what and where. */
static int asm_assemble(AsmProgram *p, const char *src) {
  AsmLabel lab[ASM_MAXLABEL];
  int nlab = 0;
  int pass, i;

  for (i = 0; i < (int)sizeof p->err; i++) p->err[i] = 0;
  p->err_line = 0;
  p->n = 0;
  p->nlit = 0;
  p->checked = 1;

  for (pass = 0; pass < 2; pass++) {
    const char *s = src;
    int line = 1;
    p->n = 0;
    if (pass == 1) p->nlit = 0;

    while (*s) {
      char w[24];
      const char *ls = s;
      const char *cur;

      /* Take the line. */
      while (*s && *s != '\n') s++;
      cur = ls;

      /* Strip a comment. Handled by scanning rather than by cutting the
       * string, because src is the editor's buffer and must not be written. */
      {
        const char *c = ls;
        while (c < s && *c != ';' && *c != '#') c++;
        /* `c` is now the end of the code part of this line. */

        while (cur < c && asm_is_space(*cur)) cur++;

        /* A label, alone or in front of an instruction. */
        {
          const char *save = cur;
          int n = 0;
          char nm[ASM_LABELLEN];
          while (cur < c && asm_is_word(*cur) && n + 1 < (int)sizeof nm) nm[n++] = *cur++;
          nm[n] = 0;
          if (n && cur < c && *cur == ':') {
            cur++;
            if (pass == 0) {
              int j, dup = 0;
              for (j = 0; j < nlab; j++) if (asm_streq(lab[j].name, nm)) dup = 1;
              if (dup) asm_fail(p, line, "label defined twice");
              else if (nlab >= ASM_MAXLABEL) asm_fail(p, line, "too many labels");
              else {
                int k = 0;
                for (k = 0; nm[k]; k++) lab[nlab].name[k] = nm[k];
                lab[nlab].name[k] = 0;
                lab[nlab].at = p->n;
                nlab++;
              }
            }
          } else {
            cur = save;                    /* not a label after all */
          }
        }

        while (cur < c && asm_is_space(*cur)) cur++;
        if (cur >= c) goto next_line;      /* blank, or a label on its own */

        /* A directive. */
        if (*cur == '.') {
          cur++;
          asm_word(&cur, w, sizeof w);
          if (asm_streq(w, "unchecked")) p->checked = 0;
          else if (asm_streq(w, "checked")) p->checked = 1;
          else asm_fail(p, line, "unknown directive");
          goto next_line;
        }

        asm_word(&cur, w, sizeof w);
        {
          int oi = -1, k;
          AsmIns ins;
          for (k = 0; k < ASM_NOPS; k++)
            if (asm_streq(ASM_OPS[k].name, w)) { oi = k; break; }
          if (oi < 0) { asm_fail(p, line, "unknown instruction"); goto next_line; }

          if (p->n >= ASM_MAXCODE) { asm_fail(p, line, "program too long"); goto next_line; }

          ins.op = ASM_OPS[oi].op;
          ins.a = ins.b = 0;
          ins.imm = 0;

          switch (ASM_OPS[oi].form) {
          case 0:
            break;

          case 2: {                        /* rd, imm */
            int rd;
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rd = asm_reg(w);
            if (rd < 0) { asm_fail(p, line, "expected a register r0-r11"); break; }
            asm_skip_sep(&cur);
            if (!asm_number(&cur, &ins.imm)) { asm_fail(p, line, "expected a number"); break; }
            ins.a = (uint8_t)rd;
            if (ins.op == OP_MOVI && (ins.imm < -2048 || ins.imm > 2047)) {
              /* Too wide for movi. Promote to lit, which is what the pool is
               * for -- rather than making the programmer know the limit. */
              ins.op = OP_LIT;
            }
            if (ins.op == OP_LIT) {
              int slot = asm_intern(p, ins.imm);
              if (slot < 0) { asm_fail(p, line, "too many constants"); break; }
            }
            if (ins.op == OP_ADDI && (ins.imm < -128 || ins.imm > 127))
              asm_fail(p, line, "addi takes -128..127");
            if ((ins.op == OP_SHL || ins.op == OP_SHR) &&
                (ins.imm < 0 || ins.imm > 31))
              asm_fail(p, line, "shift takes 0..31");
            break;
          }

          case 3: {                        /* rd, rs */
            int rd, rs;
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rd = asm_reg(w);
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rs = asm_reg(w);
            if (rd < 0 || rs < 0) { asm_fail(p, line, "expected two registers"); break; }
            ins.a = (uint8_t)rd; ins.b = (uint8_t)rs;
            break;
          }

          case 4: {                        /* rd, rs, rt */
            int rd, rs, rt;
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rd = asm_reg(w);
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rs = asm_reg(w);
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rt = asm_reg(w);
            if (rd < 0 || rs < 0 || rt < 0) {
              asm_fail(p, line, "expected three registers"); break;
            }
            ins.a = (uint8_t)rd; ins.b = (uint8_t)rs; ins.imm = rt;
            break;
          }

          case 5: {                        /* rd, [rs+imm]  or  rd, [rs] */
            int rd, rs;
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rd = asm_reg(w);
            asm_skip_sep(&cur);
            if (*cur != '[') { asm_fail(p, line, "expected [rN] or [rN+off]"); break; }
            cur++;
            asm_word(&cur, w, sizeof w);
            rs = asm_reg(w);
            if (rd < 0 || rs < 0) { asm_fail(p, line, "expected a register r0-r11"); break; }
            while (asm_is_space(*cur)) cur++;
            if (*cur == '+' || *cur == '-') {
              if (!asm_number(&cur, &ins.imm)) { asm_fail(p, line, "bad offset"); break; }
            }
            while (asm_is_space(*cur)) cur++;
            if (*cur != ']') { asm_fail(p, line, "missing ]"); break; }
            cur++;
            if (ins.imm < 0 || ins.imm > 255) {
              asm_fail(p, line, "offset takes 0..255"); break;
            }
            if ((ins.op == OP_LD || ins.op == OP_ST) && (ins.imm & 3)) {
              /* l32i scales its displacement by four, so an unaligned one
               * cannot be encoded at all. Better said here than silently
               * rounded. */
              asm_fail(p, line, "word offset must be a multiple of 4"); break;
            }
            ins.a = (uint8_t)rd; ins.b = (uint8_t)rs;
            break;
          }

          case 6: {                        /* rs, rt, label */
            int rs, rt;
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rs = asm_reg(w);
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            rt = asm_reg(w);
            if (rs < 0 || rt < 0) { asm_fail(p, line, "expected two registers"); break; }
            ins.a = (uint8_t)rs; ins.b = (uint8_t)rt;
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            if (!w[0]) { asm_fail(p, line, "expected a label"); break; }
            if (pass == 1) {
              int j, at = -1;
              for (j = 0; j < nlab; j++) if (asm_streq(lab[j].name, w)) at = lab[j].at;
              if (at < 0) { asm_fail(p, line, "no such label"); break; }
              ins.imm = at;
            }
            break;
          }

          case 7: {                        /* label */
            asm_skip_sep(&cur);
            asm_word(&cur, w, sizeof w);
            if (!w[0]) { asm_fail(p, line, "expected a label"); break; }
            if (pass == 1) {
              int j, at = -1;
              for (j = 0; j < nlab; j++) if (asm_streq(lab[j].name, w)) at = lab[j].at;
              if (at < 0) { asm_fail(p, line, "no such label"); break; }
              ins.imm = at;
            }
            break;
          }

          case 8:                          /* imm */
            asm_skip_sep(&cur);
            if (!asm_number(&cur, &ins.imm)) { asm_fail(p, line, "expected a number"); break; }
            if (ins.imm < 0 || ins.imm >= SYS__COUNT) asm_fail(p, line, "no such sys call");
            break;

          default:
            break;
          }

          p->code[p->n++] = ins;
        }
      }

    next_line:
      if (*s == '\n') { s++; line++; }
      if (p->err_line) return 0;
    }
  }
  return p->err_line == 0;
}

/* ---- the interpreter -------------------------------------------------------
 *
 * The reference implementation, and the reason the VM exists: it can be
 * stepped, and a bad program stops here instead of rebooting the machine.
 * `budget` bounds a runaway loop; pass 1 to single-step. */

typedef void (*AsmSys)(void *ctx, int call, AsmState *st);

static int asm_addr_ok(uint32_t a, int width) {
  return a + (uint32_t)width <= (uint32_t)ASM_DATA;
}

static AsmStop asm_run(const AsmProgram *p, AsmState *st, int32_t budget,
                       AsmSys sys, void *ctx) {
  int32_t steps = 0;
  /* One level of call, which is all call0 gives without a frame. */
  uint32_t link = 0;

  if (!p->n) return RUN_NOCODE;

  while (steps++ < budget) {
    const AsmIns *in;
    int32_t *r = st->r;

    if (st->pc >= (uint32_t)p->n) return RUN_HALT;
    in = &p->code[st->pc];
    st->pc++;

    switch (in->op) {
    case OP_NOP:  break;
    case OP_MOV:  r[in->a] = r[in->b]; break;
    case OP_MOVI: case OP_LIT: r[in->a] = in->imm; break;
    case OP_ADD:  r[in->a] = r[in->b] + r[in->imm]; break;
    case OP_SUB:  r[in->a] = r[in->b] - r[in->imm]; break;
    case OP_AND:  r[in->a] = r[in->b] & r[in->imm]; break;
    case OP_OR:   r[in->a] = r[in->b] | r[in->imm]; break;
    case OP_XOR:  r[in->a] = r[in->b] ^ r[in->imm]; break;
    case OP_MUL:  r[in->a] = r[in->b] * r[in->imm]; break;
    case OP_DIV:
      /* Divide by zero is a fault, not a silent zero and not a reset: the
       * hardware instruction is undefined here and the interpreter must not
       * disagree with it about whether the program survives. */
      if (r[in->imm] == 0) { st->fault_addr = 0; return RUN_FAULT; }
      r[in->a] = r[in->b] / r[in->imm];
      break;
    case OP_ADDI: r[in->a] = r[in->a] + in->imm; break;
    case OP_SHL:  r[in->a] = (int32_t)((uint32_t)r[in->a] << in->imm); break;
    case OP_SHR:  r[in->a] = (int32_t)((uint32_t)r[in->a] >> in->imm); break;

    case OP_LD: case OP_LDB: case OP_ST: case OP_STB: {
      uint32_t base = (uint32_t)r[in->b];
      uint32_t a = base + (uint32_t)in->imm;
      int wide = (in->op == OP_LD || in->op == OP_ST);
      /* Compiled code checks the base register, not the sum -- see
       * asm_emit_check. The interpreter agrees with it deliberately, so that
       * a program cannot behave one way stepped and another way compiled. */
      if (base >= (uint32_t)ASM_DATA || !asm_addr_ok(base, 1)) {
        st->fault_addr = base;
        return RUN_FAULT;
      }
      if (wide) {
        uint8_t *m = st->mem + a;
        if (in->op == OP_LD)
          r[in->a] = (int32_t)((uint32_t)m[0] | ((uint32_t)m[1] << 8) |
                               ((uint32_t)m[2] << 16) | ((uint32_t)m[3] << 24));
        else {
          uint32_t v = (uint32_t)r[in->a];
          m[0] = (uint8_t)v; m[1] = (uint8_t)(v >> 8);
          m[2] = (uint8_t)(v >> 16); m[3] = (uint8_t)(v >> 24);
        }
      } else {
        if (in->op == OP_LDB) r[in->a] = st->mem[a];
        else st->mem[a] = (uint8_t)r[in->a];
      }
      break;
    }

    case OP_BEQ: if (r[in->a] == r[in->b]) st->pc = (uint32_t)in->imm; break;
    case OP_BNE: if (r[in->a] != r[in->b]) st->pc = (uint32_t)in->imm; break;
    case OP_BLT: if (r[in->a] <  r[in->b]) st->pc = (uint32_t)in->imm; break;
    case OP_BGE: if (r[in->a] >= r[in->b]) st->pc = (uint32_t)in->imm; break;

    case OP_JMP:  st->pc = (uint32_t)in->imm; break;
    case OP_CALL: link = st->pc; st->pc = (uint32_t)in->imm; break;
    case OP_RET:  st->pc = link; break;
    case OP_SYS:  if (sys) sys(ctx, (int)in->imm, st); break;
    case OP_HALT: return RUN_HALT;
    default: return RUN_FAULT;
    }
  }
  return RUN_STEPS;
}

/* ---- the code generator ----------------------------------------------------
 *
 * VM register n is Xtensa a(n+2). a14 is scratch, a15 is the data base.
 *
 * Only the wide three-byte encodings are emitted, never the narrow ones, so
 * every instruction is the same size and a branch target is arithmetic rather
 * than a fixed point of shrinking encodings. The literal pool goes first,
 * because l32r can only read backwards.
 */

#define XA(n)   ((uint32_t)((n) + 2))     /* VM register -> Xtensa register */
#define XSCRATCH 14u
#define XBASE    15u

/* RRR: op2<<20 | op1<<16 | r<<12 | s<<8 | t<<4 | op0 */
static uint32_t x_rrr(uint32_t base, uint32_t r, uint32_t s, uint32_t t) {
  return base | (r << 12) | (s << 8) | (t << 4);
}
/* RRI8: imm8<<16 | r<<12 | s<<8 | t<<4 | op0 */
static uint32_t x_rri8(uint32_t imm8, uint32_t r, uint32_t s, uint32_t t,
                       uint32_t op0) {
  return ((imm8 & 0xFFu) << 16) | (r << 12) | (s << 8) | (t << 4) | op0;
}

#define X_ADD  0x800000u
#define X_SUB  0xC00000u
#define X_AND  0x100000u
#define X_OR   0x200000u
#define X_XOR  0x300000u
#define X_MULL 0x820000u
#define X_QUOS 0xD20000u

static uint32_t x_movi(uint32_t t, int32_t imm) {
  uint32_t u = (uint32_t)imm & 0xFFFu;
  return x_rri8(u & 0xFFu, 0xAu, (u >> 8) & 0xFu, t, 2u);
}
static uint32_t x_addi(uint32_t t, uint32_t s, int32_t imm) {
  return x_rri8((uint32_t)imm & 0xFFu, 0xCu, s, t, 2u);
}
static uint32_t x_l32i(uint32_t t, uint32_t s, uint32_t off) {
  return x_rri8(off / 4u, 0x2u, s, t, 2u);
}
static uint32_t x_s32i(uint32_t t, uint32_t s, uint32_t off) {
  return x_rri8(off / 4u, 0x6u, s, t, 2u);
}
static uint32_t x_l8ui(uint32_t t, uint32_t s, uint32_t off) {
  return x_rri8(off, 0x0u, s, t, 2u);
}
static uint32_t x_s8i(uint32_t t, uint32_t s, uint32_t off) {
  return x_rri8(off, 0x4u, s, t, 2u);
}
/* slli encodes 32-n, split across op1's low bit and t. */
static uint32_t x_slli(uint32_t r, uint32_t s, uint32_t n) {
  uint32_t sa = 32u - n;
  return 0x100000u | ((sa >> 4) << 16) | (r << 12) | (s << 8) | ((sa & 0xFu) << 4);
}
static uint32_t x_srli(uint32_t r, uint32_t t, uint32_t n) {
  return 0x410000u | (r << 12) | (n << 8) | (t << 4);
}
/* Conditional branches. r selects which: beq 1, bne 9, blt 2, bge A, bgeu B. */
static uint32_t x_bcc(uint32_t which, uint32_t s, uint32_t t, int32_t off) {
  return x_rri8((uint32_t)off, which, s, t, 7u);
}
static uint32_t x_j(int32_t off) {
  return (((uint32_t)off & 0x3FFFFu) << 6) | 6u;
}
static uint32_t x_call0(int32_t off) {
  return (((uint32_t)off & 0x3FFFFu) << 6) | 5u;
}
static uint32_t x_callx0(uint32_t s) { return 0x0000C0u | (s << 8); }
static uint32_t x_ret(void)          { return 0x000080u; }
static uint32_t x_nop(void)          { return 0x0020F0u; }
static uint32_t x_l32r(uint32_t t, int32_t words_back) {
  /* imm16 is negative and scaled by four. */
  return (((uint32_t)words_back & 0xFFFFu) << 8) | (t << 4) | 1u;
}
static uint32_t x_mov(uint32_t r, uint32_t s) { return x_rrr(X_OR, r, s, s); }

/* Where the generated code goes. `buf` is the writable alias of an executable
 * allocation; the caller supplies both and this never allocates. */
typedef struct {
  uint8_t *buf;
  int      cap;
  int      at;                      /* bytes emitted */
  int      code0;                   /* byte offset where instructions start */
  int      ins_at[ASM_MAXCODE + 1]; /* byte offset of each VM instruction */
  int      overflow;
} AsmEmit;

static void x_put(AsmEmit *e, uint32_t word) {
  if (e->at + 3 > e->cap) { e->overflow = 1; return; }
  e->buf[e->at + 0] = (uint8_t)(word & 0xFFu);
  e->buf[e->at + 1] = (uint8_t)((word >> 8) & 0xFFu);
  e->buf[e->at + 2] = (uint8_t)((word >> 16) & 0xFFu);
  e->at += 3;
}

/* The bounds check, and why it looks like this.
 *
 * It tests the BASE REGISTER against the limit, not base+displacement -- so
 * it needs one scratch register rather than two, which is what leaves twelve
 * registers for the program instead of eleven. The displacement is then
 * unchecked, and cannot do any harm because the data area is allocated with
 * ASM_SLACK bytes beyond the limit: an offset is at most 255, so the worst a
 * program can reach is still inside its own allocation. Shrinking ASM_SLACK
 * below 256 breaks that argument and hands a program the heap. */
static void asm_emit_check(AsmEmit *e, uint32_t base_reg, int fault_at) {
  int32_t off;
  x_put(e, x_movi(XSCRATCH, ASM_DATA));
  /* bgeu base, scratch, fault -- unsigned, so a negative index is caught as
   * an enormous positive one rather than sliding under the test. */
  off = (int32_t)(fault_at - (e->at + 4));
  x_put(e, x_bcc(0xBu, base_reg, XSCRATCH, off));
}

/* Emit the whole program. Two passes: the first learns where every VM
 * instruction lands so branches can be resolved, the second emits for real.
 * The layout is fixed between passes because every instruction is three
 * bytes, so one repeat is enough -- there is no iteration to convergence. */
static int asm_compile(const AsmProgram *p, AsmEmit *e, uint32_t sys_fn) {
  int pass, i;
  int npool = p->nlit + 1;              /* the trampoline's address, last */
  int fault_at = 0, exit_at = 0;

  if (!p->n) return 0;

  for (pass = 0; pass < 2; pass++) {
    int lit_bytes = npool * 4;
    e->at = 0;
    e->overflow = 0;

    /* The literal pool, first, because l32r reads backwards. */
    for (i = 0; i < npool; i++) {
      uint32_t v = (i < p->nlit) ? (uint32_t)p->lit[i] : sys_fn;
      if (e->at + 4 > e->cap) { e->overflow = 1; break; }
      e->buf[e->at + 0] = (uint8_t)(v & 0xFFu);
      e->buf[e->at + 1] = (uint8_t)((v >> 8) & 0xFFu);
      e->buf[e->at + 2] = (uint8_t)((v >> 16) & 0xFFu);
      e->buf[e->at + 3] = (uint8_t)((v >> 24) & 0xFFu);
      e->at += 4;
    }
    e->code0 = e->at = lit_bytes;

    /* Prologue. a2 arrives holding the AsmState pointer; it is also r0, so it
     * is stashed on the stack before anything overwrites it. */
    x_put(e, x_addi(1u, 1u, -32));            /* addi a1, a1, -32 */
    x_put(e, x_s32i(0u, 1u, 0u));             /* s32i a0, a1, 0  -- return */
    x_put(e, x_s32i(12u, 1u, 4u));            /* callee-saved a12..a15 */
    x_put(e, x_s32i(13u, 1u, 8u));
    x_put(e, x_s32i(14u, 1u, 12u));
    x_put(e, x_s32i(15u, 1u, 16u));
    x_put(e, x_s32i(2u, 1u, 20u));            /* the state pointer */
    x_put(e, x_mov(XSCRATCH, 2u));            /* scratch = state */
    /* a15 = &state->mem, which follows r[12], pc and fault_addr. */
    x_put(e, x_addi(XBASE, XSCRATCH, (int32_t)(ASM_REGS * 4 + 8)));
    for (i = 0; i < ASM_REGS; i++)
      x_put(e, x_l32i(XA(i), XSCRATCH, (uint32_t)(i * 4)));

    /* The body. */
    for (i = 0; i < p->n; i++) {
      const AsmIns *in = &p->code[i];
      e->ins_at[i] = e->at;

      switch (in->op) {
      case OP_NOP:  x_put(e, x_nop()); break;
      case OP_MOV:  x_put(e, x_mov(XA(in->a), XA(in->b))); break;
      case OP_MOVI: x_put(e, x_movi(XA(in->a), in->imm)); break;

      case OP_LIT: {
        int slot = 0, k;
        int32_t words_back;
        for (k = 0; k < p->nlit; k++) if (p->lit[k] == in->imm) { slot = k; break; }
        /* l32r reads from ((pc + 3) & ~3) + imm16*4, and imm16 is negative. */
        words_back = (int32_t)((slot * 4) - (int32_t)(((e->at + 3) & ~3))) / 4;
        x_put(e, x_l32r(XA(in->a), words_back));
        break;
      }

      case OP_ADD: x_put(e, x_rrr(X_ADD,  XA(in->a), XA(in->b), XA(in->imm))); break;
      case OP_SUB: x_put(e, x_rrr(X_SUB,  XA(in->a), XA(in->b), XA(in->imm))); break;
      case OP_AND: x_put(e, x_rrr(X_AND,  XA(in->a), XA(in->b), XA(in->imm))); break;
      case OP_OR:  x_put(e, x_rrr(X_OR,   XA(in->a), XA(in->b), XA(in->imm))); break;
      case OP_XOR: x_put(e, x_rrr(X_XOR,  XA(in->a), XA(in->b), XA(in->imm))); break;
      case OP_MUL: x_put(e, x_rrr(X_MULL, XA(in->a), XA(in->b), XA(in->imm))); break;
      case OP_DIV: x_put(e, x_rrr(X_QUOS, XA(in->a), XA(in->b), XA(in->imm))); break;

      case OP_ADDI: x_put(e, x_addi(XA(in->a), XA(in->a), in->imm)); break;
      case OP_SHL:  x_put(e, x_slli(XA(in->a), XA(in->a), (uint32_t)in->imm)); break;
      case OP_SHR:  x_put(e, x_srli(XA(in->a), XA(in->a), (uint32_t)in->imm)); break;

      case OP_LD: case OP_LDB: case OP_ST: case OP_STB: {
        if (p->checked) asm_emit_check(e, XA(in->b), fault_at);
        /* scratch = base + index, then the access off scratch. */
        x_put(e, x_rrr(X_ADD, XSCRATCH, XBASE, XA(in->b)));
        if (in->op == OP_LD)
          x_put(e, x_l32i(XA(in->a), XSCRATCH, (uint32_t)in->imm));
        else if (in->op == OP_ST)
          x_put(e, x_s32i(XA(in->a), XSCRATCH, (uint32_t)in->imm));
        else if (in->op == OP_LDB)
          x_put(e, x_l8ui(XA(in->a), XSCRATCH, (uint32_t)in->imm));
        else
          x_put(e, x_s8i(XA(in->a), XSCRATCH, (uint32_t)in->imm));
        break;
      }

      case OP_BEQ: case OP_BNE: case OP_BLT: case OP_BGE: {
        static const uint32_t WHICH[4] = { 1u, 9u, 2u, 0xAu };
        uint32_t which = WHICH[in->op - OP_BEQ];
        int target = e->ins_at[0];
        int32_t off;
        if (in->imm >= 0 && in->imm < p->n) target = e->ins_at[in->imm];
        else if (in->imm == p->n) target = exit_at;
        off = (int32_t)(target - (e->at + 4));
        if (off >= -128 && off <= 127) {
          x_put(e, x_bcc(which, XA(in->a), XA(in->b), off));
        } else {
          /* Out of the 8-bit reach: invert the test, hop the jump. The
           * inverse of beq is bne and of blt is bge, which is why the table
           * is ordered in pairs. */
          static const uint32_t INV[4] = { 9u, 1u, 0xAu, 2u };
          x_put(e, x_bcc(INV[in->op - OP_BEQ], XA(in->a), XA(in->b), 3));
          x_put(e, x_j((int32_t)(target - (e->at + 4))));
        }
        break;
      }

      case OP_JMP: {
        int target = (in->imm >= 0 && in->imm < p->n) ? e->ins_at[in->imm] : exit_at;
        x_put(e, x_j((int32_t)(target - (e->at + 4))));
        break;
      }

      case OP_CALL: {
        int target = (in->imm >= 0 && in->imm < p->n) ? e->ins_at[in->imm] : exit_at;
        /* call0's displacement is relative to the *aligned* next word. */
        x_put(e, x_call0((int32_t)(target - (int32_t)((e->at + 4) & ~3)) / 4));
        break;
      }

      case OP_RET:  x_put(e, x_ret()); break;

      case OP_SYS: {
        /* Both back ends go through one C function, so their behaviour cannot
         * drift apart. It costs about thirty instructions, and the reason is
         * the ABI rather than the call: under call0 a2-a11 are CALLER-saved,
         * which is exactly where r0-r9 live, so every register has to go out
         * to the state and come back. a14 and a15 are callee-saved and
         * survive on their own, which is why the data base does not move.
         *
         * sys is a handful of instructions in a program that prints, so
         * paying thirty for it is the right trade against giving the compiler
         * a register allocator to avoid the spill. */
        int k;
        int32_t words_back;
        x_put(e, x_s32i(0u, 1u, 24u));                /* a0 survives callx0 */
        x_put(e, x_l32i(XSCRATCH, 1u, 20u));          /* the state */
        for (k = 0; k < ASM_REGS; k++)
          x_put(e, x_s32i(XA(k), XSCRATCH, (uint32_t)(k * 4)));
        x_put(e, x_movi(2u, in->imm));                /* arg 1: which call */
        x_put(e, x_mov(3u, XSCRATCH));                /* arg 2: the state */
        words_back = (int32_t)((p->nlit * 4) - (int32_t)((e->at + 3) & ~3)) / 4;
        x_put(e, x_l32r(XSCRATCH, words_back));
        x_put(e, x_callx0(XSCRATCH));
        x_put(e, x_l32i(XSCRATCH, 1u, 20u));
        for (k = 0; k < ASM_REGS; k++)
          x_put(e, x_l32i(XA(k), XSCRATCH, (uint32_t)(k * 4)));
        x_put(e, x_l32i(0u, 1u, 24u));
        break;
      }

      case OP_HALT:
        x_put(e, x_j((int32_t)(exit_at - (e->at + 4))));
        break;

      default:
        x_put(e, x_nop());
        break;
      }
    }
    e->ins_at[p->n] = e->at;

    /* The fault stub: record nothing here, just fall into the exit with a
     * marker. Reached only by a failed bounds check. */
    fault_at = e->at;
    x_put(e, x_movi(XSCRATCH, 1));
    x_put(e, x_j(3));

    /* Epilogue: registers back into the state, then restore and return. */
    exit_at = e->at;
    {
      x_put(e, x_l32i(XSCRATCH, 1u, 20u));   /* the state pointer */
      for (i = 0; i < ASM_REGS; i++)
        x_put(e, x_s32i(XA(i), XSCRATCH, (uint32_t)(i * 4)));
      x_put(e, x_l32i(0u, 1u, 0u));
      x_put(e, x_l32i(12u, 1u, 4u));
      x_put(e, x_l32i(13u, 1u, 8u));
      x_put(e, x_l32i(14u, 1u, 12u));
      x_put(e, x_l32i(15u, 1u, 16u));
      x_put(e, x_addi(1u, 1u, 32));
      x_put(e, x_ret());
    }
  }

  return !e->overflow;
}

#endif /* CARDOS_ASMVM_H */
