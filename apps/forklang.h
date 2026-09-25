/* Forklang: the language Forklift's robots are programmed in.
 *
 * A program is a few definitions, one to a line (a line that starts with a
 * space carries on the one before). `bot` is the one the robot calls every
 * step, and it answers with an action:
 *
 *     want = first order
 *     bot = empty holding
 *       ? (at (src want) ? take want : go (src want))
 *       : (at ship ? drop : go ship)
 *
 * Pure and expression-only, because a pure function of what the robot can
 * see is short to write and easy to reason about, and short matters on a
 * keyboard this size. `c ? a : b`, `x -> e`, application by juxtaposition,
 * `(x, y)` for a place, `[a, b]` for a list, the usual arithmetic and
 * comparison, `&& || !`, `#` to the end of a line. No loops and no mutation;
 * recursion is allowed and paid for in fuel.
 *
 * A tree walker over a fixed pool of nodes. Nothing is allocated from the
 * heap: every run gets a fresh arena of cells (lists, closures' frames) and a
 * budget of fuel and depth, so a runaway program is an error on the screen
 * and not a crash -- the evaluator recurses on the C stack, and the shell's
 * stack is not large.
 *
 * Portable and libc-free, like the apps that include it: the game and the
 * host tests (test/test_forklang.c) compile the same text.
 */
#ifndef CARDOS_FORKLANG_H
#define CARDOS_FORKLANG_H

#include <stdint.h>
#include <stddef.h>

#if defined(__GNUC__)
#define FL_OPT __attribute__((unused))
#else
#define FL_OPT
#endif

#define FL_MAX_NODE  480
#define FL_MAX_SYM   96
#define FL_SYM_LEN   12        /* a name is at most 11 characters */
#define FL_MAX_DEF   32
#define FL_MAX_PARAM 6
#define FL_MAX_CELL  420
#define FL_FUEL      20000
/* Each level is a few C frames on the shell's stack, which is 8 KB for
 * everything; 28 leaves room and still recurses through a list of 8. */
#define FL_MAX_DEPTH 28
#define FL_ERR       64

#define FL_KINDS     5
#define FL_MAX_HOLD  4
#define FL_MAX_ORDER 8

/* ---- what the robot sees, and what it does -------------------------------- */

typedef struct {
  int me;                         /* which robot, from 0 */
  int x, y, cap;
  int held[FL_MAX_HOLD], nheld;
  int order[FL_MAX_ORDER], norder;
  int ship_x, ship_y;
  int src_x[FL_KINDS], src_y[FL_KINDS];   /* -1: no bay for it here */
} FlWorld;

enum { FL_ACT_WAIT = 0, FL_ACT_GO, FL_ACT_TAKE, FL_ACT_DROP };

typedef struct { int act, x, y, kind; } FlAction;

static const char *const FL_KIND_NAME[FL_KINDS] = {
  "red", "blue", "green", "yellow", "white"
};

/* ---- the program ------------------------------------------------------------ */

enum {
  N_INT = 0, N_LOCAL, N_NAME, N_DEF, N_PRIM, N_LAM, N_APP, N_IF, N_BIN,
  N_NOT, N_NEG, N_LIST, N_PAIR
};

typedef struct {
  uint8_t k, op;
  int16_t a, b, c;
  int16_t line;
  int32_t v;
} FlNode;

typedef struct {
  FlNode node[FL_MAX_NODE];
  int    nnode;
  char   sym[FL_MAX_SYM][FL_SYM_LEN];
  int    nsym;
  struct { int16_t sym, node, nparams, line; } def[FL_MAX_DEF];
  int    ndef;
  int    bot;                     /* the def the robot runs, or -1 */
  char   err[FL_ERR];
  int    err_line;                /* 1-based; 0 when there is no error */
} FlProg;

/* ---- the built-ins ------------------------------------------------------------ */

enum {
  P_HOLDING = 0, P_ORDER, P_CAP, P_ME, P_POS, P_SHIP, P_DROP, P_WAIT,
  P_TRUE, P_FALSE, P_RED, P_BLUE, P_GREEN, P_YELLOW, P_WHITE,
  P_SRC, P_AT, P_DIST, P_COL, P_ROW, P_GO, P_TAKE,
  P_FIRST, P_REST, P_LEN, P_EMPTY, P_ABS,
  P_HAS, P_COUNT, P_MIN, P_MAX, P_MAP, P_FILTER, P_NTH,
  P_FOLD,
  P_COUNT_OF_PRIMS
};

typedef struct { const char *name; uint8_t arity; const char *about; } FlPrim;

static const FlPrim FL_PRIMS[P_COUNT_OF_PRIMS] = {
  { "holding", 0, "list of kinds on the forks" },
  { "order",   0, "kinds the order still wants" },
  { "cap",     0, "how many the forks carry" },
  { "me",      0, "which robot this is, from 0" },
  { "pos",     0, "where this robot is" },
  { "ship",    0, "the shipping dock" },
  { "drop",    0, "action: ship what you hold" },
  { "wait",    0, "action: do nothing" },
  { "true",    0, "yes" },
  { "false",   0, "no" },
  { "red",     0, "a kind" },
  { "blue",    0, "a kind" },
  { "green",   0, "a kind" },
  { "yellow",  0, "a kind" },
  { "white",   0, "a kind" },
  { "src",     1, "src k: the bay for kind k" },
  { "at",      1, "at p: am I standing on p" },
  { "dist",    1, "dist p: steps from here, as the crow" },
  { "col",     1, "col p: its x" },
  { "row",     1, "row p: its y" },
  { "go",      1, "action: one step towards p" },
  { "take",    1, "action: take k from its bay" },
  { "first",   1, "first l" },
  { "rest",    1, "rest l: all but the first" },
  { "len",     1, "len l" },
  { "empty",   1, "empty l" },
  { "abs",     1, "abs n" },
  { "has",     2, "has l x: is x in l" },
  { "count",   2, "count l x: how many x in l" },
  { "min",     2, "min a b" },
  { "max",     2, "max a b" },
  { "map",     2, "map f l" },
  { "filter",  2, "filter f l: the x where f x" },
  { "nth",     2, "nth l i: from 0" },
  { "fold",    3, "fold f z l: f (f z a) b ..." },
};

/* ---- small things libc would give us ------------------------------------------ */

static int fl_streq(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static void fl_copy(char *d, const char *s, int n) {
  int i = 0;
  while (s[i] && i < n - 1) { d[i] = s[i]; i++; }
  d[i] = 0;
}

/* A message with one %s and one number at most: enough for every error
 * here, and no printf to lean on. */
static void fl_msg(char *out, const char *a, const char *b, const char *c) {
  int o = 0;
  const char *parts[3];
  int i;
  parts[0] = a; parts[1] = b; parts[2] = c;
  for (i = 0; i < 3; i++) {
    const char *s = parts[i];
    while (s && *s && o < FL_ERR - 1) out[o++] = *s++;
  }
  out[o] = 0;
}

static void fl_itoa(int v, char *out) {
  char tmp[12];
  int n = 0, o = 0;
  unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
  if (v < 0) out[o++] = '-';
  do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
  while (n) out[o++] = tmp[--n];
  out[o] = 0;
}

/* ---- the lexer ------------------------------------------------------------------ */

enum {
  T_EOF = 256, T_NL, T_INT, T_ID, T_ARROW, T_EQEQ, T_NE, T_LE, T_GE,
  T_AND, T_OR, T_BAD
};

typedef struct {
  const char *s;
  int pos, line;
  int tok, tline;
  int32_t ival;
  char id[FL_SYM_LEN];
  int long_id;
  int started;                   /* a token has been read: newlines now count */
} FlLex;

static int fl_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int fl_digit(char c) { return c >= '0' && c <= '9'; }

/* After a newline: another definition starts if the next line with anything
 * on it starts in column 0. Blank lines and comments do not count. */
static int fl_after_newline(FlLex *L) {
  for (;;) {
    char c = L->s[L->pos];
    if (c == 0) return 0;
    if (c == '\n') { L->pos++; L->line++; continue; }
    if (c == '#') {
      while (L->s[L->pos] && L->s[L->pos] != '\n') L->pos++;
      continue;
    }
    if (c == ' ' || c == '\t') {
      int p = L->pos;
      while (L->s[p] == ' ' || L->s[p] == '\t') p++;
      if (L->s[p] == '\n' || L->s[p] == 0 || L->s[p] == '#') {
        L->pos = p;
        continue;                /* an indented blank or comment line */
      }
      L->pos = p;
      return 0;                  /* a continuation */
    }
    return 1;                    /* column 0: a new definition */
  }
}

static void fl_next(FlLex *L) {
  const char *s = L->s;
  char c;
  for (;;) {
    c = s[L->pos];
    if (c == ' ' || c == '\t' || c == '\r') { L->pos++; continue; }
    if (c == '#') { while (s[L->pos] && s[L->pos] != '\n') L->pos++; continue; }
    if (c == '\n') {
      L->pos++;
      L->line++;
      if (fl_after_newline(L) && L->started) {
        L->tok = T_NL;
        L->tline = L->line;
        return;
      }
      continue;
    }
    break;
  }
  L->tline = L->line;
  L->started = 1;
  if (c == 0) { L->tok = T_EOF; return; }
  if (fl_digit(c)) {
    int32_t v = 0;
    while (fl_digit(s[L->pos])) {
      if (v < 100000000) v = v * 10 + (s[L->pos] - '0');
      L->pos++;
    }
    L->ival = v;
    L->tok = T_INT;
    return;
  }
  if (fl_alpha(c)) {
    int n = 0;
    L->long_id = 0;
    while (fl_alpha(s[L->pos]) || fl_digit(s[L->pos]) || s[L->pos] == '\'') {
      if (n < FL_SYM_LEN - 1) L->id[n++] = s[L->pos];
      else L->long_id = 1;
      L->pos++;
    }
    L->id[n] = 0;
    L->tok = T_ID;
    return;
  }
  L->pos++;
  {
    char d = s[L->pos];
    if (c == '-' && d == '>') { L->pos++; L->tok = T_ARROW; return; }
    if (c == '=' && d == '=') { L->pos++; L->tok = T_EQEQ; return; }
    if (c == '!' && d == '=') { L->pos++; L->tok = T_NE; return; }
    if (c == '<' && d == '=') { L->pos++; L->tok = T_LE; return; }
    if (c == '>' && d == '=') { L->pos++; L->tok = T_GE; return; }
    if (c == '&' && d == '&') { L->pos++; L->tok = T_AND; return; }
    if (c == '|' && d == '|') { L->pos++; L->tok = T_OR; return; }
  }
  switch (c) {
  case '(': case ')': case '[': case ']': case ',': case '?': case ':':
  case '=': case '<': case '>': case '+': case '-': case '*': case '/':
  case '%': case '!':
    L->tok = c;
    return;
  default:
    L->tok = T_BAD;
    L->ival = (uint8_t)c;
    return;
  }
}

static const char *fl_tok_name(const FlLex *L, char *buf) {
  switch (L->tok) {
  case T_EOF: return "the end";
  case T_NL: return "a new line";
  case T_INT: fl_itoa(L->ival, buf); return buf;
  case T_ID: return L->id;
  case T_ARROW: return "->";
  case T_EQEQ: return "==";
  case T_NE: return "!=";
  case T_LE: return "<=";
  case T_GE: return ">=";
  case T_AND: return "&&";
  case T_OR: return "||";
  case T_BAD: buf[0] = (char)L->ival; buf[1] = 0; return buf;
  default: buf[0] = (char)L->tok; buf[1] = 0; return buf;
  }
}

/* ---- the parser ------------------------------------------------------------------- */

typedef struct {
  FlProg *p;
  FlLex   L;
  int16_t scope[FL_MAX_PARAM * 8];
  int     nscope;
} FlParser;

static int fl_failed(FlParser *P) { return P->p->err[0] != 0; }

static void fl_fail(FlParser *P, int line, const char *a, const char *b, const char *c) {
  if (P->p->err[0]) return;
  fl_msg(P->p->err, a, b, c);
  P->p->err_line = line;
}

static void fl_fail_tok(FlParser *P, const char *want) {
  char buf[16];
  fl_fail(P, P->L.tline, want, ", not ", fl_tok_name(&P->L, buf));
}

static int fl_intern(FlProg *p, const char *name) {
  int i;
  for (i = 0; i < p->nsym; i++) if (fl_streq(p->sym[i], name)) return i;
  if (p->nsym >= FL_MAX_SYM) return -1;
  fl_copy(p->sym[p->nsym], name, FL_SYM_LEN);
  return p->nsym++;
}

static int fl_node(FlParser *P, int k, int line) {
  FlNode *n;
  if (P->p->nnode >= FL_MAX_NODE) {
    fl_fail(P, line, "the program is too big", 0, 0);
    return -1;
  }
  n = &P->p->node[P->p->nnode];
  n->k = (uint8_t)k;
  n->op = 0;
  n->a = n->b = n->c = -1;
  n->line = (int16_t)line;
  n->v = 0;
  return P->p->nnode++;
}

static int fl_in_scope(FlParser *P, int sym) {
  int i;
  for (i = P->nscope - 1; i >= 0; i--) if (P->scope[i] == sym) return 1;
  return 0;
}

static int fl_expr(FlParser *P);

static int fl_starts_atom(int tok) {
  return tok == T_INT || tok == T_ID || tok == '(' || tok == '[';
}

static int fl_atom(FlParser *P) {
  FlLex *L = &P->L;
  int n, line = L->tline;
  if (fl_failed(P)) return -1;
  if (L->tok == T_INT) {
    n = fl_node(P, N_INT, line);
    if (n >= 0) P->p->node[n].v = L->ival;
    fl_next(L);
    return n;
  }
  if (L->tok == T_ID) {
    int sym;
    if (L->long_id) { fl_fail(P, line, "a name is 11 letters at most: ", L->id, "..."); return -1; }
    sym = fl_intern(P->p, L->id);
    if (sym < 0) { fl_fail(P, line, "too many names", 0, 0); return -1; }
    n = fl_node(P, fl_in_scope(P, sym) ? N_LOCAL : N_NAME, line);
    if (n >= 0) P->p->node[n].v = sym;
    fl_next(L);
    return n;
  }
  if (L->tok == '(') {
    int a, b;
    fl_next(L);
    a = fl_expr(P);
    if (L->tok == ',') {
      fl_next(L);
      b = fl_expr(P);
      if (L->tok != ')') { fl_fail_tok(P, "expected )"); return -1; }
      fl_next(L);
      n = fl_node(P, N_PAIR, line);
      if (n >= 0) { P->p->node[n].a = (int16_t)a; P->p->node[n].b = (int16_t)b; }
      return n;
    }
    if (L->tok != ')') { fl_fail_tok(P, "expected )"); return -1; }
    fl_next(L);
    return a;
  }
  if (L->tok == '[') {
    int head = -1, tail = -1;
    fl_next(L);
    if (L->tok == ']') {
      fl_next(L);
      return fl_node(P, N_LIST, line);          /* a = -1: empty */
    }
    for (;;) {
      int e = fl_expr(P), cell;
      if (fl_failed(P)) return -1;
      cell = fl_node(P, N_LIST, line);
      if (cell < 0) return -1;
      P->p->node[cell].a = (int16_t)e;
      if (tail >= 0) P->p->node[tail].b = (int16_t)cell;
      else head = cell;
      tail = cell;
      if (L->tok == ',') { fl_next(L); continue; }
      if (L->tok == ']') { fl_next(L); break; }
      fl_fail_tok(P, "expected , or ]");
      return -1;
    }
    /* The last cell's b stays -1; an empty tail is a cell with a == -1. */
    {
      int end = fl_node(P, N_LIST, line);
      if (end < 0) return -1;
      P->p->node[tail].b = (int16_t)end;
    }
    return head;
  }
  fl_fail_tok(P, "expected a value");
  return -1;
}

static int fl_app(FlParser *P) {
  int f = fl_atom(P);
  while (!fl_failed(P) && fl_starts_atom(P->L.tok)) {
    int line = P->L.tline, x = fl_atom(P), n;
    if (fl_failed(P)) return -1;
    n = fl_node(P, N_APP, line);
    if (n < 0) return -1;
    P->p->node[n].a = (int16_t)f;
    P->p->node[n].b = (int16_t)x;
    f = n;
  }
  return f;
}

static int fl_unary(FlParser *P) {
  int line = P->L.tline;
  if (P->L.tok == '!' || P->L.tok == '-') {
    int k = P->L.tok == '!' ? N_NOT : N_NEG, a, n;
    fl_next(&P->L);
    a = fl_unary(P);
    n = fl_node(P, k, line);
    if (n >= 0) P->p->node[n].a = (int16_t)a;
    return n;
  }
  return fl_app(P);
}

/* Operators are stored in `op` as their character, or for the two-character
 * ones as a small code below any printable character. */
enum { OP_EQ = 1, OP_NE, OP_LE, OP_GE, OP_AND, OP_OR };

static int fl_bin(FlParser *P, int op, int a, int b, int line) {
  int n = fl_node(P, N_BIN, line);
  if (n < 0) return -1;
  switch (op) {
  case T_EQEQ: op = OP_EQ; break;
  case T_NE:   op = OP_NE; break;
  case T_LE:   op = OP_LE; break;
  case T_GE:   op = OP_GE; break;
  case T_AND:  op = OP_AND; break;
  case T_OR:   op = OP_OR; break;
  }
  P->p->node[n].op = (uint8_t)op;
  P->p->node[n].a = (int16_t)a;
  P->p->node[n].b = (int16_t)b;
  return n;
}

static int fl_mul(FlParser *P) {
  int a = fl_unary(P);
  while (!fl_failed(P) && (P->L.tok == '*' || P->L.tok == '/' || P->L.tok == '%')) {
    int op = P->L.tok, line = P->L.tline;
    fl_next(&P->L);
    a = fl_bin(P, op, a, fl_unary(P), line);
  }
  return a;
}

static int fl_add(FlParser *P) {
  int a = fl_mul(P);
  while (!fl_failed(P) && (P->L.tok == '+' || P->L.tok == '-')) {
    int op = P->L.tok, line = P->L.tline;
    fl_next(&P->L);
    a = fl_bin(P, op, a, fl_mul(P), line);
  }
  return a;
}

static int fl_cmp(FlParser *P) {
  int a = fl_add(P), t = P->L.tok;
  if (!fl_failed(P) && (t == T_EQEQ || t == T_NE || t == '<' || t == '>' ||
                        t == T_LE || t == T_GE)) {
    int line = P->L.tline;
    fl_next(&P->L);
    a = fl_bin(P, t, a, fl_add(P), line);
  }
  return a;
}

static int fl_and(FlParser *P) {
  int a = fl_cmp(P);
  while (!fl_failed(P) && P->L.tok == T_AND) {
    int line = P->L.tline;
    fl_next(&P->L);
    a = fl_bin(P, T_AND, a, fl_cmp(P), line);
  }
  return a;
}

static int fl_or(FlParser *P) {
  int a = fl_and(P);
  while (!fl_failed(P) && P->L.tok == T_OR) {
    int line = P->L.tline;
    fl_next(&P->L);
    a = fl_bin(P, T_OR, a, fl_and(P), line);
  }
  return a;
}

static int fl_lambda(FlParser *P, int sym, int body_line);

/* A lexer's state, copied by hand: a struct assignment this size compiles
 * to memcpy, and an app links no libc. */
static void fl_lex_copy(FlLex *d, const FlLex *s) {
  int i;
  d->s = s->s; d->pos = s->pos; d->line = s->line;
  d->tok = s->tok; d->tline = s->tline; d->ival = s->ival;
  d->long_id = s->long_id; d->started = s->started;
  for (i = 0; i < FL_SYM_LEN; i++) d->id[i] = s->id[i];
}

static int fl_expr(FlParser *P) {
  FlLex *L = &P->L;
  if (fl_failed(P)) return -1;
  if (L->tok == T_ID) {
    FlLex save;
    fl_lex_copy(&save, L);
    fl_next(L);
    if (L->tok == T_ARROW) {
      int sym;
      if (save.long_id) { fl_fail(P, save.tline, "a name is 11 letters at most", 0, 0); return -1; }
      sym = fl_intern(P->p, save.id);
      fl_next(L);
      return fl_lambda(P, sym, save.tline);
    }
    fl_lex_copy(L, &save);
  }
  {
    int c = fl_or(P);
    if (!fl_failed(P) && L->tok == '?') {
      int line = L->tline, a, b, n;
      fl_next(L);
      a = fl_expr(P);
      if (!fl_failed(P) && L->tok != ':') { fl_fail_tok(P, "expected : after ? ..."); return -1; }
      fl_next(L);
      b = fl_expr(P);
      n = fl_node(P, N_IF, line);
      if (n < 0) return -1;
      P->p->node[n].a = (int16_t)c;
      P->p->node[n].b = (int16_t)a;
      P->p->node[n].c = (int16_t)b;
      return n;
    }
    return c;
  }
}

/* x -> body, with x in scope for the body. */
static int fl_lambda(FlParser *P, int sym, int line) {
  int body, n;
  if (P->nscope >= (int)(sizeof P->scope / sizeof P->scope[0])) {
    fl_fail(P, line, "functions nested too deep", 0, 0);
    return -1;
  }
  P->scope[P->nscope++] = (int16_t)sym;
  body = fl_expr(P);
  P->nscope--;
  n = fl_node(P, N_LAM, line);
  if (n < 0) return -1;
  P->p->node[n].v = sym;
  P->p->node[n].a = (int16_t)body;
  return n;
}

static int fl_prim_index(const char *name) {
  int i;
  for (i = 0; i < P_COUNT_OF_PRIMS; i++) if (fl_streq(FL_PRIMS[i].name, name)) return i;
  return -1;
}

static int fl_def_index(const FlProg *p, int sym) {
  int i;
  for (i = 0; i < p->ndef; i++) if (p->def[i].sym == sym) return i;
  return -1;
}

/* One definition: name params = expr, the params becoming lambdas. */
static void fl_definition(FlParser *P) {
  FlLex *L = &P->L;
  int name, params[FL_MAX_PARAM], np = 0, line = L->tline, body, i;

  if (L->tok != T_ID) { fl_fail_tok(P, "expected a name to define"); return; }
  if (L->long_id) { fl_fail(P, line, "a name is 11 letters at most: ", L->id, "..."); return; }
  name = fl_intern(P->p, L->id);
  if (fl_prim_index(L->id) >= 0) { fl_fail(P, line, L->id, " is built in; pick another name", 0); return; }
  fl_next(L);
  while (L->tok == T_ID) {
    if (np >= FL_MAX_PARAM) { fl_fail(P, line, "six parameters at most", 0, 0); return; }
    params[np++] = fl_intern(P->p, L->id);
    fl_next(L);
  }
  if (L->tok != '=') { fl_fail_tok(P, "expected ="); return; }
  fl_next(L);

  for (i = 0; i < np; i++) P->scope[P->nscope++] = (int16_t)params[i];
  body = fl_expr(P);
  P->nscope -= np;
  if (fl_failed(P)) return;
  for (i = np - 1; i >= 0; i--) {
    int n = fl_node(P, N_LAM, line);
    if (n < 0) return;
    P->p->node[n].v = params[i];
    P->p->node[n].a = (int16_t)body;
    body = n;
  }
  if (L->tok != T_NL && L->tok != T_EOF) { fl_fail_tok(P, "expected the end of the line"); return; }

  if (fl_def_index(P->p, name) >= 0) { fl_fail(P, line, P->p->sym[name], " is defined twice", 0); return; }
  if (P->p->ndef >= FL_MAX_DEF) { fl_fail(P, line, "too many definitions", 0, 0); return; }
  P->p->def[P->p->ndef].sym = (int16_t)name;
  P->p->def[P->p->ndef].node = (int16_t)body;
  P->p->def[P->p->ndef].nparams = (int16_t)np;
  P->p->def[P->p->ndef].line = (int16_t)line;
  P->p->ndef++;
}

/* Parse a whole program. 0, or -1 with p->err and p->err_line saying why. */
static FL_OPT int fl_parse(FlProg *p, const char *src) {
  FlParser P;
  int i;

  p->nnode = p->nsym = p->ndef = 0;
  p->bot = -1;
  p->err[0] = 0;
  p->err_line = 0;
  P.p = p;
  P.nscope = 0;
  P.L.s = src;
  P.L.pos = 0;
  P.L.line = 1;
  P.L.started = 0;
  P.L.long_id = 0;
  fl_next(&P.L);
  while (P.L.tok != T_EOF && !fl_failed(&P)) {
    if (P.L.tok == T_NL) { fl_next(&P.L); continue; }
    fl_definition(&P);
  }
  if (fl_failed(&P)) return -1;

  /* Names are resolved once everything is defined, so a definition can use
   * one further down. */
  for (i = 0; i < p->nnode; i++) {
    FlNode *n = &p->node[i];
    int d, pr;
    if (n->k != N_NAME) continue;
    d = fl_def_index(p, (int)n->v);
    if (d >= 0) { n->k = N_DEF; n->v = d; continue; }
    pr = fl_prim_index(p->sym[n->v]);
    if (pr >= 0) { n->k = N_PRIM; n->v = pr; continue; }
    fl_fail(&P, n->line, "no such name: ", p->sym[n->v], 0);
    return -1;
  }

  for (i = 0; i < p->ndef; i++)
    if (fl_streq(p->sym[p->def[i].sym], "bot")) p->bot = i;
  if (p->bot < 0) { fl_fail(&P, 1, "no bot = ... to run", 0, 0); return -1; }
  if (p->def[p->bot].nparams) {
    fl_fail(&P, p->def[p->bot].line, "bot takes no parameters", 0, 0);
    return -1;
  }
  return 0;
}

/* ---- values and the evaluator ---------------------------------------------------- */

enum { V_INT = 0, V_BOOL, V_KIND, V_POS, V_NIL, V_CONS, V_ACT, V_CLO, V_PRIM };

typedef struct {
  uint8_t t, n;
  int16_t a, b;
  int32_t i;
} FlVal;

typedef struct {
  FlVal   v;
  int32_t next;                   /* the rest of a list, or a frame's parent */
  int16_t sym;                    /* a frame's name */
} FlCell;

typedef struct {
  const FlProg  *p;
  const FlWorld *w;
  FlCell  cell[FL_MAX_CELL];
  int     ncell;
  int     fuel, depth;
  FlVal   cache[FL_MAX_DEF];
  uint8_t cstate[FL_MAX_DEF];     /* 0 not yet, 1 being worked out, 2 known */
  char    err[FL_ERR];
  int     err_line;
  int     line;                   /* the node being evaluated, for errors */
} FlRun;

static const char *const FL_TYPE[] = {
  "a number", "true/false", "a kind", "a place", "a list", "a list",
  "an action", "a function", "a function"
};

static FlVal fl_v(int t) { FlVal v; v.t = (uint8_t)t; v.n = 0; v.a = v.b = 0; v.i = 0; return v; }
static FlVal fl_int(int i) { FlVal v = fl_v(V_INT); v.i = i; return v; }
static FlVal fl_bool(int b) { FlVal v = fl_v(V_BOOL); v.i = b ? 1 : 0; return v; }
static FlVal fl_kind(int k) { FlVal v = fl_v(V_KIND); v.i = k; return v; }
static FlVal fl_pos(int x, int y) { FlVal v = fl_v(V_POS); v.a = (int16_t)x; v.b = (int16_t)y; return v; }
static FlVal fl_act(int act) { FlVal v = fl_v(V_ACT); v.n = (uint8_t)act; return v; }

static int fl_bad(FlRun *R) { return R->err[0] != 0; }

static FlVal fl_err(FlRun *R, const char *a, const char *b, const char *c) {
  if (!R->err[0]) {
    fl_msg(R->err, a, b, c);
    R->err_line = R->line;
  }
  return fl_v(V_NIL);
}

static int fl_want(FlRun *R, FlVal v, int t, const char *who) {
  if (fl_bad(R)) return 0;
  if (v.t == t || (t == V_NIL && v.t == V_CONS)) return 1;
  fl_err(R, who, " wants ", FL_TYPE[t]);
  if (R->err[0]) {
    int o = 0;
    while (R->err[o]) o++;
    fl_msg(R->err + o, ", got ", FL_TYPE[v.t], 0);
  }
  return 0;
}

static int fl_alloc(FlRun *R) {
  if (R->ncell >= FL_MAX_CELL) {
    fl_err(R, "out of memory -- lists too long?", 0, 0);
    return -1;
  }
  return R->ncell++;
}

/* Appending to a list being built front to back: *head and *last start
 * as NIL and -1. No array of items on the stack, which matters in a
 * function that recurses. */
static void fl_append(FlRun *R, FlVal *head, int *last, FlVal x) {
  int c = fl_alloc(R);
  if (c < 0) return;
  R->cell[c].v = x;
  R->cell[c].next = -1;
  if (*last >= 0) R->cell[*last].next = c;
  else { *head = fl_v(V_CONS); head->i = c; }
  *last = c;
}

static FlVal fl_cons(FlRun *R, FlVal head, FlVal tail) {
  int c = fl_alloc(R);
  FlVal v;
  if (c < 0) return fl_v(V_NIL);
  R->cell[c].v = head;
  R->cell[c].next = tail.t == V_CONS ? tail.i : -1;
  v = fl_v(V_CONS);
  v.i = c;
  return v;
}

/* A list from an array of kinds, in order. */
static FlVal fl_kinds(FlRun *R, const int *k, int n) {
  FlVal l = fl_v(V_NIL);
  while (n-- > 0) l = fl_cons(R, fl_kind(k[n]), l);
  return l;
}

static FlVal fl_tail(const FlRun *R, FlVal l) {
  int nx = R->cell[l.i].next;
  FlVal v;
  if (nx < 0) return fl_v(V_NIL);
  v = fl_v(V_CONS);
  v.i = nx;
  return v;
}

static int fl_equal(const FlRun *R, FlVal a, FlVal b) {
  if (a.t == V_NIL || a.t == V_CONS) {
    if (b.t != V_NIL && b.t != V_CONS) return 0;
    while (a.t == V_CONS && b.t == V_CONS) {
      if (!fl_equal(R, R->cell[a.i].v, R->cell[b.i].v)) return 0;
      a = fl_tail(R, a);
      b = fl_tail(R, b);
    }
    return a.t == b.t;
  }
  if (a.t != b.t) return 0;
  if (a.t == V_POS) return a.a == b.a && a.b == b.b;
  if (a.t == V_ACT) return a.n == b.n && a.a == b.a && a.b == b.b && a.i == b.i;
  return a.i == b.i && a.a == b.a;
}

static FlVal fl_eval(FlRun *R, int node, int env);
static FlVal fl_apply(FlRun *R, FlVal f, FlVal x);

static FlVal fl_call(FlRun *R, int prim, FlVal *arg) {
  const FlWorld *w = R->w;
  FlVal v = fl_v(V_NIL), l;
  int n;
  switch (prim) {
  case P_HOLDING: return fl_kinds(R, w->held, w->nheld);
  case P_ORDER:   return fl_kinds(R, w->order, w->norder);
  case P_CAP:     return fl_int(w->cap);
  case P_ME:      return fl_int(w->me);
  case P_POS:     return fl_pos(w->x, w->y);
  case P_SHIP:    return fl_pos(w->ship_x, w->ship_y);
  case P_DROP:    return fl_act(FL_ACT_DROP);
  case P_WAIT:    return fl_act(FL_ACT_WAIT);
  case P_TRUE:    return fl_bool(1);
  case P_FALSE:   return fl_bool(0);
  case P_RED: case P_BLUE: case P_GREEN: case P_YELLOW: case P_WHITE:
    return fl_kind(prim - P_RED);
  case P_SRC:
    if (!fl_want(R, arg[0], V_KIND, "src")) return v;
    if (w->src_x[arg[0].i] < 0)
      return fl_err(R, "no ", FL_KIND_NAME[arg[0].i], " bay here yet");
    return fl_pos(w->src_x[arg[0].i], w->src_y[arg[0].i]);
  case P_AT:
    if (!fl_want(R, arg[0], V_POS, "at")) return v;
    return fl_bool(arg[0].a == w->x && arg[0].b == w->y);
  case P_DIST: {
    int dx, dy;
    if (!fl_want(R, arg[0], V_POS, "dist")) return v;
    dx = arg[0].a - w->x; dy = arg[0].b - w->y;
    return fl_int((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
  }
  case P_COL:
    if (!fl_want(R, arg[0], V_POS, "col")) return v;
    return fl_int(arg[0].a);
  case P_ROW:
    if (!fl_want(R, arg[0], V_POS, "row")) return v;
    return fl_int(arg[0].b);
  case P_GO:
    if (!fl_want(R, arg[0], V_POS, "go")) return v;
    v = fl_act(FL_ACT_GO);
    v.a = arg[0].a; v.b = arg[0].b;
    return v;
  case P_TAKE:
    if (!fl_want(R, arg[0], V_KIND, "take")) return v;
    v = fl_act(FL_ACT_TAKE);
    v.i = arg[0].i;
    return v;
  case P_FIRST:
    if (!fl_want(R, arg[0], V_NIL, "first")) return v;
    if (arg[0].t == V_NIL) return fl_err(R, "first of an empty list", 0, 0);
    return R->cell[arg[0].i].v;
  case P_REST:
    if (!fl_want(R, arg[0], V_NIL, "rest")) return v;
    if (arg[0].t == V_NIL) return fl_err(R, "rest of an empty list", 0, 0);
    return fl_tail(R, arg[0]);
  case P_LEN:
    if (!fl_want(R, arg[0], V_NIL, "len")) return v;
    for (n = 0, l = arg[0]; l.t == V_CONS; l = fl_tail(R, l)) n++;
    return fl_int(n);
  case P_EMPTY:
    if (!fl_want(R, arg[0], V_NIL, "empty")) return v;
    return fl_bool(arg[0].t == V_NIL);
  case P_ABS:
    if (!fl_want(R, arg[0], V_INT, "abs")) return v;
    return fl_int(arg[0].i < 0 ? -arg[0].i : arg[0].i);
  case P_HAS:
  case P_COUNT:
    if (!fl_want(R, arg[0], V_NIL, FL_PRIMS[prim].name)) return v;
    for (n = 0, l = arg[0]; l.t == V_CONS; l = fl_tail(R, l))
      if (fl_equal(R, R->cell[l.i].v, arg[1])) n++;
    return prim == P_HAS ? fl_bool(n > 0) : fl_int(n);
  case P_MIN:
  case P_MAX:
    if (!fl_want(R, arg[0], V_INT, FL_PRIMS[prim].name)) return v;
    if (!fl_want(R, arg[1], V_INT, FL_PRIMS[prim].name)) return v;
    return fl_int(prim == P_MIN ? (arg[0].i < arg[1].i ? arg[0].i : arg[1].i)
                                : (arg[0].i > arg[1].i ? arg[0].i : arg[1].i));
  case P_NTH:
    if (!fl_want(R, arg[0], V_NIL, "nth")) return v;
    if (!fl_want(R, arg[1], V_INT, "nth")) return v;
    for (n = arg[1].i, l = arg[0]; l.t == V_CONS && n > 0; l = fl_tail(R, l)) n--;
    if (l.t != V_CONS || n < 0) return fl_err(R, "nth: the list is too short", 0, 0);
    return R->cell[l.i].v;
  case P_MAP:
  case P_FILTER: {
    int last = -1;
    if (!fl_want(R, arg[1], V_NIL, FL_PRIMS[prim].name)) return v;
    for (l = arg[1]; l.t == V_CONS && !fl_bad(R); l = fl_tail(R, l)) {
      FlVal x = R->cell[l.i].v, y = fl_apply(R, arg[0], x);
      if (fl_bad(R)) return y;
      if (prim == P_FILTER) {
        if (!fl_want(R, y, V_BOOL, "filter's function")) return y;
        if (!y.i) continue;
        y = x;
      }
      fl_append(R, &v, &last, y);
    }
    return v;
  }
  case P_FOLD:
    if (!fl_want(R, arg[2], V_NIL, "fold")) return v;
    v = arg[1];
    for (l = arg[2]; l.t == V_CONS && !fl_bad(R); l = fl_tail(R, l))
      v = fl_apply(R, fl_apply(R, arg[0], v), R->cell[l.i].v);
    return v;
  }
  return fl_err(R, "unknown built-in", 0, 0);
}

static FlVal fl_apply(FlRun *R, FlVal f, FlVal x) {
  if (fl_bad(R)) return f;
  if (f.t == V_CLO) {
    const FlNode *lam = &R->p->node[f.a];
    int c = fl_alloc(R);
    if (c < 0) return fl_v(V_NIL);
    R->cell[c].v = x;
    R->cell[c].sym = (int16_t)lam->v;
    R->cell[c].next = f.i;
    return fl_eval(R, lam->a, c);
  }
  if (f.t == V_PRIM) {
    int arity = FL_PRIMS[f.a].arity, got = f.n + 1, c = fl_alloc(R);
    FlVal args[3], v;
    int i, at;
    if (c < 0) return fl_v(V_NIL);
    R->cell[c].v = x;
    R->cell[c].next = f.n ? f.i : -1;
    if (got < arity) {
      v = f;
      v.n = (uint8_t)got;
      v.i = c;
      return v;
    }
    /* The arguments were collected last first. */
    for (i = arity - 1, at = c; i >= 0; i--, at = R->cell[at].next) args[i] = R->cell[at].v;
    return fl_call(R, f.a, args);
  }
  return fl_err(R, FL_TYPE[f.t], " is not a function", 0);
}

static FlVal fl_def_value(FlRun *R, int d) {
  if (R->cstate[d] == 2) return R->cache[d];
  if (R->cstate[d] == 1)
    return fl_err(R, R->p->sym[R->p->def[d].sym], " needs itself to work out", 0);
  R->cstate[d] = 1;
  R->cache[d] = fl_eval(R, R->p->def[d].node, -1);
  R->cstate[d] = 2;
  return R->cache[d];
}

static FlVal fl_binop(FlRun *R, int op, FlVal a, FlVal b) {
  if (op == OP_EQ) return fl_bool(fl_equal(R, a, b));
  if (op == OP_NE) return fl_bool(!fl_equal(R, a, b));
  if (op == '+' && a.t == V_POS && b.t == V_POS) return fl_pos(a.a + b.a, a.b + b.b);
  if (op == '-' && a.t == V_POS && b.t == V_POS) return fl_pos(a.a - b.a, a.b - b.b);
  if (!fl_want(R, a, V_INT, "arithmetic") || !fl_want(R, b, V_INT, "arithmetic"))
    return a;
  switch (op) {
  case '+': return fl_int(a.i + b.i);
  case '-': return fl_int(a.i - b.i);
  case '*': return fl_int(a.i * b.i);
  case '/': if (!b.i) return fl_err(R, "divided by zero", 0, 0); return fl_int(a.i / b.i);
  case '%': if (!b.i) return fl_err(R, "divided by zero", 0, 0); return fl_int(a.i % b.i);
  case '<': return fl_bool(a.i < b.i);
  case '>': return fl_bool(a.i > b.i);
  case OP_LE: return fl_bool(a.i <= b.i);
  case OP_GE: return fl_bool(a.i >= b.i);
  }
  return fl_err(R, "unknown operator", 0, 0);
}

static FlVal fl_eval_in(FlRun *R, int node, int env) {
  const FlNode *n = &R->p->node[node];
  FlVal a, b;
  int e;

  R->line = n->line;
  switch (n->k) {
  case N_INT: return fl_int((int)n->v);
  case N_LOCAL:
    for (e = env; e >= 0; e = R->cell[e].next)
      if (R->cell[e].sym == n->v) return R->cell[e].v;
    return fl_err(R, "lost a name: ", R->p->sym[n->v], 0);
  case N_DEF: return fl_def_value(R, (int)n->v);
  case N_PRIM:
    if (FL_PRIMS[n->v].arity == 0) return fl_call(R, (int)n->v, 0);
    a = fl_v(V_PRIM);
    a.a = (int16_t)n->v;
    a.i = -1;
    return a;
  case N_LAM:
    a = fl_v(V_CLO);
    a.a = (int16_t)node;
    a.i = env;
    return a;
  case N_APP:
    a = fl_eval(R, n->a, env);
    b = fl_eval(R, n->b, env);
    R->line = n->line;
    return fl_apply(R, a, b);
  case N_IF:
    a = fl_eval(R, n->a, env);
    R->line = n->line;
    if (!fl_want(R, a, V_BOOL, "? ...")) return a;
    return fl_eval(R, a.i ? n->b : n->c, env);
  case N_BIN:
    a = fl_eval(R, n->a, env);
    if (n->op == OP_AND || n->op == OP_OR) {
      R->line = n->line;
      if (!fl_want(R, a, V_BOOL, n->op == OP_AND ? "&&" : "||")) return a;
      if (n->op == OP_AND && !a.i) return a;
      if (n->op == OP_OR && a.i) return a;
      b = fl_eval(R, n->b, env);
      if (!fl_want(R, b, V_BOOL, n->op == OP_AND ? "&&" : "||")) return b;
      return b;
    }
    b = fl_eval(R, n->b, env);
    R->line = n->line;
    return fl_binop(R, n->op, a, b);
  case N_NOT:
    a = fl_eval(R, n->a, env);
    if (!fl_want(R, a, V_BOOL, "!")) return a;
    return fl_bool(!a.i);
  case N_NEG:
    a = fl_eval(R, n->a, env);
    if (!fl_want(R, a, V_INT, "-")) return a;
    return fl_int(-a.i);
  case N_LIST: {
    int c = node, last = -1;
    a = fl_v(V_NIL);
    while (c >= 0 && R->p->node[c].a >= 0 && !fl_bad(R)) {
      b = fl_eval(R, R->p->node[c].a, env);
      fl_append(R, &a, &last, b);
      c = R->p->node[c].b;
    }
    return a;
  }
  case N_PAIR:
    a = fl_eval(R, n->a, env);
    b = fl_eval(R, n->b, env);
    R->line = n->line;
    if (!fl_want(R, a, V_INT, "a place") || !fl_want(R, b, V_INT, "a place")) return a;
    return fl_pos(a.i, b.i);
  }
  return fl_err(R, "bad node", 0, 0);
}

static FlVal fl_eval(FlRun *R, int node, int env) {
  FlVal v;
  if (fl_bad(R)) return fl_v(V_NIL);
  if (node < 0) return fl_err(R, "missing expression", 0, 0);
  if (--R->fuel < 0) return fl_err(R, "thinking too long (a loop?)", 0, 0);
  if (++R->depth > FL_MAX_DEPTH) {
    R->depth--;
    return fl_err(R, "too deep (recursion?)", 0, 0);
  }
  v = fl_eval_in(R, node, env);
  R->depth--;
  return v;
}

/* Run bot once against a world. 0 and *out, or -1 with R->err. */
static FL_OPT int fl_run(FlRun *R, const FlProg *p, const FlWorld *w, FlAction *out) {
  FlVal v;
  int i;
  R->p = p;
  R->w = w;
  R->ncell = 0;
  R->fuel = FL_FUEL;
  R->depth = 0;
  R->err[0] = 0;
  R->err_line = 0;
  R->line = 0;
  for (i = 0; i < p->ndef; i++) R->cstate[i] = 0;
  out->act = FL_ACT_WAIT;
  out->x = out->y = out->kind = 0;
  if (p->bot < 0) { fl_msg(R->err, "no program", 0, 0); return -1; }

  v = fl_def_value(R, p->bot);
  if (fl_bad(R)) return -1;
  if (v.t != V_ACT) {
    R->line = p->def[p->bot].line;
    fl_err(R, "bot gave ", FL_TYPE[v.t], ", not an action");
    return -1;
  }
  out->act = v.n;
  out->x = v.a;
  out->y = v.b;
  out->kind = v.i;
  return 0;
}

/* Every name the editor can complete, one at a time: built-ins first, then
 * the program's own definitions. NULL past the end. */
static FL_OPT const char *fl_name_at(const FlProg *p, int i) {
  if (i < P_COUNT_OF_PRIMS) return FL_PRIMS[i].name;
  i -= P_COUNT_OF_PRIMS;
  if (p && i < p->ndef) return p->sym[p->def[i].sym];
  return 0;
}

#endif /* CARDOS_FORKLANG_H */
