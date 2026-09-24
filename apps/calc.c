/* Calc: a calculator that graphs.
 *
 * Two views over one set of functions. The calculator takes a line --
 * `2^10`, `sqrt(2)*3`, `a=5`, `ans/2` -- and answers under it, like a
 * paper roll. A line that is an equation, `y=x^2`, or that simply mentions
 * x, becomes a graph instead (up to four, y1..y4), and Tab shows them:
 * arrows pan, + and - zoom, f fits the height to the curves, t traces one
 * with a cursor that reads out x and y. fn-p prints whichever view is up;
 * a graph goes to paper as a picture (printdoc.h's `%%` lines), the
 * calculator as its roll.
 *
 * The arithmetic is single-precision float, because that is what the chip
 * has in hardware, and it is shown to seven significant digits, which is
 * what a float honestly holds. There is no libm and no libgcc here -- an
 * app links against nothing -- so the functions are written below from
 * scratch, and so is division: GCC turns `a / b` into a call to
 * __divsf3, which lives in libgcc, so this file provides one (a
 * reciprocal by Newton's method and a correction step).
 *
 * Functions persist in /var/calc.txt with the angle mode; the roll does
 * not -- it is a scratchpad.
 */

#include "kernel/app/capp.h"
#include "apps/safefile.h"

static const CardApi *api;

/* ==== float, without a library ========================================= */

typedef union { float f; uint32_t u; } FU;

static float fbits(uint32_t u) { FU v; v.u = u; return v.f; }
static uint32_t bitsf(float f) { FU v; v.f = f; return v.u; }
#define F_NAN  fbits(0x7FC00000u)
#define F_INF  fbits(0x7F800000u)
#define F_PI   3.14159265f
#define F_PIO2 1.57079633f
#define F_E    2.71828183f
#define F_LN2  0.693147181f
#define F_LN10 2.30258509f

static int f_isnan(float x) { return x != x; }
static int f_finite(float x) { return (bitsf(x) & 0x7F800000u) != 0x7F800000u; }
static float f_abs(float x) { return fbits(bitsf(x) & 0x7FFFFFFFu); }
static float f_signed(float mag, int neg) {
  return fbits((bitsf(mag) & 0x7FFFFFFFu) | (neg ? 0x80000000u : 0));
}

/* 2^k for k in the normal range. */
static float pow2i(int k) { return fbits((uint32_t)(k + 127) << 23); }

static float scalb(float x, int k) {
  while (k > 127)  { x *= pow2i(127);  k -= 127; }
  while (k < -126) { x *= pow2i(-126); k += 126; }
  return x * pow2i(k);
}

/* x = m * 2^e with m in [1, 2); x finite, positive, non-zero. */
static float split(float x, int *e) {
  uint32_t u = bitsf(x);
  int adj = 0;
  if (!(u & 0x7F800000u)) { x *= pow2i(24); u = bitsf(x); adj = 24; }  /* denormal */
  *e = (int)((u >> 23) & 0xFF) - 127 - adj;
  return fbits((u & 0x007FFFFFu) | 0x3F800000u);
}

/* 1/b by Newton's method from a linear first guess (24/17 - 8/17 m, good to
 * 1/17 over m in [1, 2)), which
 * three steps take past float precision. b finite and non-zero. */
static float f_recip(float b) {
  int e, neg = b < 0;
  float m = split(f_abs(b), &e), r = 1.41176471f - 0.470588235f * m;
  r = r * (2.0f - m * r);
  r = r * (2.0f - m * r);
  r = r * (2.0f - m * r);
  return f_signed(scalb(r, -e), neg);
}

static float f_div(float a, float b) {
  float r, q;
  int neg = (int)((bitsf(a) ^ bitsf(b)) >> 31);
  if (f_isnan(a) || f_isnan(b)) return F_NAN;
  if (!f_finite(b)) return f_finite(a) ? f_signed(0.0f, neg) : F_NAN;
  if (b == 0.0f) return a == 0.0f ? F_NAN : f_signed(F_INF, neg);
  if (!f_finite(a)) return f_signed(F_INF, neg);
  r = f_recip(b);
  q = a * r;
  if (f_finite(q) && q != 0.0f) q = q + (a - q * b) * r;   /* one correction */
  return q;
}

#ifdef __XTENSA__
/* What `/` compiles to. Everything in this file that divides lands here. */
float __divsf3(float a, float b);
float __divsf3(float a, float b) { return f_div(a, b); }
#endif

static int ifloor(float v) {         /* |v| < 2^31 */
  int i = (int)v;
  if ((float)i > v) i--;
  return i;
}

static float f_floor(float x) {
  if (!f_finite(x) || f_abs(x) >= 8388608.0f) return x;   /* already whole */
  return (float)ifloor(x);
}

/* a*b exactly, as the float returned plus *err (Dekker's product). The
 * volatiles keep the compiler from fusing a multiply into the add after
 * it, which would change what the split computes. */
static void split12(float x, float *hi, float *lo) {
  volatile float c = 4097.0f * x;
  float h = c - (c - x);
  *hi = h;
  *lo = x - h;
}
static float two_prod(float a, float b, float *err) {
  volatile float p = a * b;
  float ah, al, bh, bl;
  split12(a, &ah, &al);
  split12(b, &bh, &bl);
  *err = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
  return p;
}

static float f_sqrt(float x) {
  int e, i;
  float m, r;
  if (f_isnan(x) || x < 0.0f) return F_NAN;
  if (x == 0.0f || !f_finite(x)) return x;
  m = split(x, &e);
  if (e & 1) { m *= 2.0f; e -= 1; }            /* m in [1, 4), e even */
  r = 0.5f * (1.0f + m);
  for (i = 0; i < 5; i++) r = 0.5f * (r + m / r);
  /* Newton in float can stop a step either side of the root, and a step
   * is the seventh digit. One more with the residual m - r*r worked out
   * exactly lands on the nearest float. */
  {
    float lo, p = two_prod(r, r, &lo);
    r += ((m - p) - lo) * 0.5f / r;
  }
  return scalb(r, e / 2);
}

static float f_exp(float x) {
  int k;
  float r, p;
  if (f_isnan(x)) return x;
  if (x > 88.72f) return F_INF;
  if (x < -103.9f) return 0.0f;
  k = ifloor(x * 1.44269504f + 0.5f);
  r = x - (float)k * 0.693145752f - (float)k * 1.42860677e-6f;   /* ln 2, in two parts */
  p = 1.0f + r * (1.0f + r * (0.5f + r * (0.166666667f + r * (0.0416666667f +
      r * (0.00833333333f + r * (0.00138888889f + r * 0.000198412698f))))));
  return scalb(p, k);
}

static float f_ln(float x) {
  int e;
  float m, s, s2;
  if (f_isnan(x) || x < 0.0f) return F_NAN;
  if (x == 0.0f) return -F_INF;
  if (!f_finite(x)) return x;
  m = split(x, &e);
  if (m > 1.41421356f) { m *= 0.5f; e++; }
  s = (m - 1.0f) / (m + 1.0f);
  s2 = s * s;
  s = 2.0f * s * (1.0f + s2 * (0.333333333f + s2 * (0.2f + s2 * (0.142857143f +
      s2 * 0.111111111f))));
  return (float)e * 0.693145752f + (s + (float)e * 1.42860677e-6f);
}

static float sin_poly(float r) {
  float r2 = r * r;
  return r * (1.0f + r2 * (-0.166666667f + r2 * (0.00833333333f +
         r2 * (-0.000198412698f + r2 * 2.75573192e-6f))));
}
static float cos_poly(float r) {
  float r2 = r * r;
  return 1.0f + r2 * (-0.5f + r2 * (0.0416666667f + r2 * (-0.00138888889f +
         r2 * 2.48015873e-5f)));
}

/* sin (want_cos 0) or cos (1) of x radians -- or of x degrees, which are
 * reduced in degrees first so that sin(180) is exactly 0 rather than the
 * sine of the float nearest pi. */
static float f_sincos(float x, int want_cos, int deg) {
  int k, q;
  float r;
  if (!f_finite(x) || f_abs(x) > 1.0e7f) return F_NAN;
  if (deg) {
    k = ifloor(x / 90.0f + 0.5f);
    r = (x - (float)k * 90.0f) * 0.0174532925f;
  } else {
    k = ifloor(x * 0.636619772f + 0.5f);
    r = x - (float)k * 1.5703125f - (float)k * 4.83751297e-4f - (float)k * 7.54978995e-8f;
  }
  q = (k + want_cos) & 3;
  /* What a whole number of quarter turns leaves as float noise is zero. */
  if (k != 0 && !(q & 1) && f_abs(r) < 2.0e-7f * (f_abs(x) > 1.0f ? f_abs(x) : 1.0f))
    return 0.0f;
  switch (q) {
  case 0:  return sin_poly(r);
  case 1:  return cos_poly(r);
  case 2:  return -sin_poly(r);
  default: return -cos_poly(r);
  }
}

static float f_atan(float x) {
  int neg = x < 0, inv = 0;
  float a = f_abs(x), off = 0.0f, a2, r;
  if (f_isnan(x)) return x;
  if (a > 1.0f) { a = 1.0f / a; inv = 1; }
  if (a > 0.267949192f) { a = (a * 1.73205081f - 1.0f) / (a + 1.73205081f); off = 0.523598776f; }
  a2 = a * a;
  r = a * (1.0f - a2 * (0.333333333f - a2 * (0.2f - a2 * (0.142857143f -
      a2 * (0.111111111f - a2 * 0.0909090909f)))));
  r += off;
  if (inv) r = F_PIO2 - r;
  return neg ? -r : r;
}

static float f_asin(float x) {
  if (f_isnan(x) || f_abs(x) > 1.0f) return F_NAN;
  if (f_abs(x) == 1.0f) return x * F_PIO2;
  return f_atan(x / f_sqrt((1.0f - x) * (1.0f + x)));
}

static float f_acos(float x) {
  if (f_isnan(x) || f_abs(x) > 1.0f) return F_NAN;
  if (x == -1.0f) return F_PI;
  return 2.0f * f_atan(f_sqrt((1.0f - x) / (1.0f + x)));
}

static float f_pow(float a, float b) {
  if (f_isnan(a) || f_isnan(b)) return F_NAN;
  if (b == 0.0f) return 1.0f;
  if (f_abs(b) <= 1048576.0f && f_floor(b) == b) {     /* whole: exact-ish */
    int n = (int)f_abs(b);
    float r = 1.0f, s = a;
    while (n) { if (n & 1) r *= s; s *= s; n >>= 1; }
    if (b < 0.0f) return r == 0.0f ? F_NAN : 1.0f / r;
    return r;
  }
  if (a == 0.0f) return b > 0.0f ? 0.0f : F_NAN;
  if (a < 0.0f) {
    int odd;
    if (f_floor(b) != b) return F_NAN;                /* no complex numbers */
    odd = f_abs(b) < 16777216.0f && ((int)f_abs(b) & 1);
    return f_signed(f_exp(b * f_ln(-a)), odd);
  }
  return f_exp(b * f_ln(a));
}

static float f_fact(float x) {
  float r = 1.0f;
  int i, n;
  if (f_isnan(x) || x < 0.0f || f_floor(x) != x) return F_NAN;
  if (x > 34.0f) return F_INF;
  n = (int)x;
  for (i = 2; i <= n; i++) r *= (float)i;
  return r;
}

/* ==== numbers as text =================================================== */

static const float P10[39] = {
  1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f, 1e8f, 1e9f, 1e10f, 1e11f,
  1e12f, 1e13f, 1e14f, 1e15f, 1e16f, 1e17f, 1e18f, 1e19f, 1e20f, 1e21f, 1e22f,
  1e23f, 1e24f, 1e25f, 1e26f, 1e27f, 1e28f, 1e29f, 1e30f, 1e31f, 1e32f, 1e33f,
  1e34f, 1e35f, 1e36f, 1e37f, 1e38f,
};
static const float N10[39] = {
  1e-0f, 1e-1f, 1e-2f, 1e-3f, 1e-4f, 1e-5f, 1e-6f, 1e-7f, 1e-8f, 1e-9f,
  1e-10f, 1e-11f, 1e-12f, 1e-13f, 1e-14f, 1e-15f, 1e-16f, 1e-17f, 1e-18f,
  1e-19f, 1e-20f, 1e-21f, 1e-22f, 1e-23f, 1e-24f, 1e-25f, 1e-26f, 1e-27f,
  1e-28f, 1e-29f, 1e-30f, 1e-31f, 1e-32f, 1e-33f, 1e-34f, 1e-35f, 1e-36f,
  1e-37f, 1e-38f,
};

/* x * 10^k for any k a float can use. */
static float scale10(float x, int k) {
  while (k > 38)  { x *= P10[38]; k -= 38; }
  while (k < -38) { x /= P10[38]; k += 38; }
  return k >= 0 ? x * P10[k] : x / P10[-k];
}

/* The power of ten at or below a, a finite and positive. */
static int dec_exp(float a) {
  int e = 0;
  if (a >= 1.0f) { while (e < 38 && a >= P10[e + 1]) e++; return e; }
  e = -1;
  while (e > -38 && a < N10[-e]) e--;
  if (e == -38) while (e > -46 && a < scale10(1.0f, e)) e--;
  return e;
}

/* a * 10^k rounded to a whole number, a < 1e7 of them. The product has to
 * be exact first: 1/3 is 0.3333333433, but 0.3333333433 * 10^7 rounded to
 * a float is 3333333.5, and rounding that gives the wrong last digit. */
static int scaled_digits(float a, int k) {
  float hi, lo, f;
  int m;
  if (k >= 0 && k <= 38) {
    hi = two_prod(a, P10[k], &lo);
  } else if (k < 0 && k >= -38) {
    float p = P10[-k], qlo, qp;
    hi = a / p;
    qp = two_prod(hi, p, &qlo);
    lo = ((a - qp) - qlo) / p;
  } else {
    hi = scale10(a, k);
    lo = 0.0f;
  }
  m = ifloor(hi);
  f = (hi - (float)m) + lo;
  if (f >= 0.5f) m++;
  else if (f < -0.5f) m--;
  return m;
}

/* `sig` significant digits (at most 7), trailing zeros dropped; plain
 * notation from 0.00001 up to what the digits reach, 1.5e12 beyond. */
static void fmt_num(char *out, int size, float v, int sig) {
  char d[8], b[32];
  int e, n = 0, nd, i, m;
  float a;
  if (f_isnan(v)) { api->fmt(out, (size_t)size, "undefined"); return; }
  if (!f_finite(v)) { api->fmt(out, (size_t)size, v > 0 ? "overflow" : "-overflow"); return; }
  if (v == 0.0f) { api->fmt(out, (size_t)size, "0"); return; }
  if (sig > 7) sig = 7;
  a = f_abs(v);
  e = dec_exp(a);
  m = scaled_digits(a, sig - 1 - e);
  if (m >= (int)P10[sig]) { m /= 10; e++; }
  for (i = sig - 1; i >= 0; i--) { d[i] = (char)('0' + m % 10); m /= 10; }
  nd = sig;
  while (nd > 1 && d[nd - 1] == '0') nd--;

  if (v < 0) b[n++] = '-';
  if (e >= sig || e < -5) {
    b[n++] = d[0];
    if (nd > 1) { b[n++] = '.'; for (i = 1; i < nd; i++) b[n++] = d[i]; }
    b[n] = 0;
    api->fmt(out, (size_t)size, "%se%d", b, e);
    return;
  }
  if (e >= 0) {
    for (i = 0; i <= e; i++) b[n++] = i < nd ? d[i] : '0';
    if (nd > e + 1) { b[n++] = '.'; for (i = e + 1; i < nd; i++) b[n++] = d[i]; }
  } else {
    b[n++] = '0'; b[n++] = '.';
    for (i = 0; i < -e - 1; i++) b[n++] = '0';
    for (i = 0; i < nd; i++) b[n++] = d[i];
  }
  b[n] = 0;
  api->fmt(out, (size_t)size, "%s", b);
}

/* ==== the language ======================================================
 *
 * A line is compiled once into a little stack program -- which a graph then
 * runs a few hundred times per screen, one per column -- by a recursive
 * descent parser with the usual precedence: + - below * / below unary minus
 * below ^ (right to left) below !. A number, a name or a bracket straight
 * after something is multiplication: 2x, 3(1+2), (x+1)(x-1), 2pi, x sin(x).
 * Names run together are split greedily into known words, so `pix` is pi*x
 * and `xsin(x)` is x*sin(x).
 */

enum {
  FN_SIN, FN_COS, FN_TAN, FN_ASIN, FN_ACOS, FN_ATAN, FN_SQRT, FN_ABS,
  FN_LN, FN_LOG, FN_EXP, FN_FLOOR, FN_CEIL, FN_ROUND, FN_N
};
static const char *const FN_NAME[FN_N] = {
  "sin", "cos", "tan", "asin", "acos", "atan", "sqrt", "abs",
  "ln", "log", "exp", "floor", "ceil", "round",
};

enum {
  OP_NUM, OP_X, OP_VAR, OP_ANS, OP_NEG, OP_ADD, OP_SUB, OP_MUL, OP_DIV,
  OP_POW, OP_FACT, OP_FN, OP_Y
};

#define PROG_MAX  64
#define KONST_MAX 16
#define STACK_MAX 16
#define EXPR_MAX  80
#define NFUN      4

typedef struct {
  uint8_t n, nk;
  uint8_t op[PROG_MAX], arg[PROG_MAX];
  float   k[KONST_MAX];
} Prog;

typedef struct {
  const char *s;
  int   i;
  Prog *p;
  int   depth, maxd;
  const char *err;
  int   in_fn;        /* compiling a graph: y1(...) inside one is refused */
  int   uses_x;
} Parse;

static struct {
  float    var[26];
  uint32_t var_set;
  float    ans;
  int      deg;
  struct { int on, hidden; char src[EXPR_MAX + 1]; Prog prog; } fn[NFUN];
} M;

static char s_err[40];

static int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int peek(Parse *P) {
  while (P->s[P->i] == ' ') P->i++;
  return P->s[P->i];
}

static void fail(Parse *P, const char *why) { if (!P->err) P->err = why; }

static void emit(Parse *P, int op, int arg, int delta) {
  if (P->err) return;
  if (P->p->n >= PROG_MAX) { fail(P, "too long"); return; }
  P->p->op[P->p->n] = (uint8_t)op;
  P->p->arg[P->p->n] = (uint8_t)arg;
  P->p->n++;
  P->depth += delta;
  if (P->depth > P->maxd) P->maxd = P->depth;
  if (P->maxd > STACK_MAX) fail(P, "too deeply nested");
}

static void emit_num(Parse *P, float v) {
  if (P->p->nk >= KONST_MAX) { fail(P, "too many numbers"); return; }
  P->p->k[P->p->nk] = v;
  emit(P, OP_NUM, P->p->nk++, 1);
}

/* Up to nine significant digits are read exactly and the rest only
 * counted; `1e5` and `2.5e-3` are exponents, `2e` and `2ex` are 2*e. */
static void p_number(Parse *P) {
  const char *s = P->s;
  int i = P->i, mant = 0, nd = 0, dexp = 0, any = 0, dot = 0;
  for (;; i++) {
    if (is_digit(s[i])) {
      any = 1;
      if (nd < 9) { mant = mant * 10 + (s[i] - '0'); if (mant) nd++; if (dot) dexp--; }
      else if (!dot) dexp++;
    } else if (s[i] == '.' && !dot) {
      dot = 1;
    } else break;
  }
  if (!any) { fail(P, "a lone ."); return; }
  if (lower(s[i]) == 'e' && (is_digit(s[i + 1]) ||
      ((s[i + 1] == '-' || s[i + 1] == '+') && is_digit(s[i + 2])))) {
    int neg = s[i + 1] == '-', x = 0;
    i++;
    if (s[i] == '-' || s[i] == '+') i++;
    while (is_digit(s[i])) { if (x < 1000) x = x * 10 + (s[i] - '0'); i++; }
    dexp += neg ? -x : x;
  }
  P->i = i;
  emit_num(P, dexp > 60 ? F_INF : scale10((float)mant, dexp < -90 ? -90 : dexp));
}

static void p_expr(Parse *P);
static void p_unary(Parse *P);

static void p_group(Parse *P) {        /* after the ( */
  p_expr(P);
  if (P->err) return;
  if (peek(P) != ')') { fail(P, "missing )"); return; }
  P->i++;
}

static void p_name(Parse *P) {
  const char *s = P->s + P->i;
  int run = 0, best = 0, what = -1, i, c;
  while (is_alpha(s[run])) run++;
  for (i = 0; i < FN_N; i++) {
    int n = (int)api->str_len(FN_NAME[i]), j;
    if (n > run || n <= best) continue;
    for (j = 0; j < n && lower(s[j]) == FN_NAME[i][j]; j++) {}
    if (j == n) { best = n; what = i; }
  }
  if (best < 3 && run >= 3 && lower(s[0]) == 'a' && lower(s[1]) == 'n' && lower(s[2]) == 's') {
    best = 3; what = 100;
  }
  if (best < 2 && run >= 2 && lower(s[0]) == 'p' && lower(s[1]) == 'i') { best = 2; what = 101; }
  if (!best) { best = 1; what = 200; }
  P->i += best;

  if (what < FN_N) {
    if (peek(P) != '(') {
      api->fmt(s_err, sizeof s_err, "%s needs brackets: %s(...)", FN_NAME[what], FN_NAME[what]);
      fail(P, s_err);
      return;
    }
    P->i++;
    p_group(P);
    emit(P, OP_FN, what, 0);
    return;
  }
  if (what == 100) { emit(P, OP_ANS, 0, 1); return; }
  if (what == 101) { emit_num(P, F_PI); return; }

  c = lower(s[0]);
  if (c == 'e') { emit_num(P, F_E); return; }
  if (c == 'x') { P->uses_x = 1; emit(P, OP_X, 0, 1); return; }
  if (c == 'y') {
    int k = P->s[P->i] - '1';
    if (k >= 0 && k < NFUN && P->s[P->i + 1] == '(') {
      if (P->in_fn) { fail(P, "a graph can't use another"); return; }
      if (!M.fn[k].on) {
        api->fmt(s_err, sizeof s_err, "y%d is not defined", k + 1);
        fail(P, s_err);
        return;
      }
      P->i += 2;
      p_group(P);
      emit(P, OP_Y, k, 0);
      return;
    }
    fail(P, "y is for graphs: y=...");
    return;
  }
  if (!(M.var_set & (1u << (c - 'a')))) {
    api->fmt(s_err, sizeof s_err, "%c is not set (%c=5 sets it)", c, c);
    fail(P, s_err);
    return;
  }
  emit(P, OP_VAR, c - 'a', 1);
}

static void p_primary(Parse *P) {
  int c = peek(P);
  if (is_digit(c) || c == '.') p_number(P);
  else if (c == '(') { P->i++; p_group(P); }
  else if (is_alpha(c)) p_name(P);
  else if (c == 0) fail(P, "unfinished");
  else if (c == ')') fail(P, "unexpected )");
  else {
    api->fmt(s_err, sizeof s_err, "unexpected %c", c);
    fail(P, s_err);
  }
}

static void p_power(Parse *P) {
  p_primary(P);
  while (!P->err && peek(P) == '!') { P->i++; emit(P, OP_FACT, 0, 0); }
  if (!P->err && peek(P) == '^') {
    P->i++;
    p_unary(P);
    emit(P, OP_POW, 0, -1);
  }
}

static void p_unary(Parse *P) {
  int c = peek(P);
  if (c == '-') { P->i++; p_unary(P); emit(P, OP_NEG, 0, 0); }
  else if (c == '+') { P->i++; p_unary(P); }
  else p_power(P);
}

static void p_term(Parse *P) {
  p_unary(P);
  while (!P->err) {
    int c = peek(P);
    if (c == '*' || c == '/') {
      P->i++;
      p_unary(P);
      emit(P, c == '*' ? OP_MUL : OP_DIV, 0, -1);
    } else if (is_digit(c) || c == '.') {
      fail(P, "missing an operator");
    } else if (is_alpha(c) || c == '(') {
      p_power(P);                       /* 2x, 3(4), (x+1)(x-1) */
      emit(P, OP_MUL, 0, -1);
    } else break;
  }
}

static void p_expr(Parse *P) {
  p_term(P);
  while (!P->err) {
    int c = peek(P);
    if (c != '+' && c != '-') break;
    P->i++;
    p_term(P);
    emit(P, c == '+' ? OP_ADD : OP_SUB, 0, -1);
  }
}

/* Compile `s` into `p`. NULL on success, else why not. */
static const char *compile(const char *s, Prog *p, int in_fn, int *uses_x) {
  Parse P;
  api->mem_set(&P, 0, sizeof P);
  api->mem_set(p, 0, sizeof *p);
  P.s = s; P.p = p; P.in_fn = in_fn;
  if (!peek(&P)) return "nothing to work out";
  p_expr(&P);
  if (!P.err && peek(&P)) {
    int c = peek(&P);
    if (c == ')') P.err = "unexpected )";
    else if (c == '=') P.err = "only one = in a line";
    else { api->fmt(s_err, sizeof s_err, "unexpected %c", c); P.err = s_err; }
  }
  if (uses_x) *uses_x = P.uses_x;
  return P.err;
}

static float apply_fn(int f, float v) {
  int deg = M.deg;
  switch (f) {
  case FN_SIN:   return f_sincos(v, 0, deg);
  case FN_COS:   return f_sincos(v, 1, deg);
  case FN_TAN: {
    float c = f_sincos(v, 1, deg);
    return c == 0.0f ? F_NAN : f_sincos(v, 0, deg) / c;
  }
  case FN_ASIN:  v = f_asin(v); return deg ? v * 57.2957795f : v;
  case FN_ACOS:  v = f_acos(v); return deg ? v * 57.2957795f : v;
  case FN_ATAN:  v = f_atan(v); return deg ? v * 57.2957795f : v;
  case FN_SQRT:  return f_sqrt(v);
  case FN_ABS:   return f_abs(v);
  case FN_LN:    return f_ln(v);
  case FN_LOG:   return f_ln(v) * 0.434294482f;
  case FN_EXP:   return f_exp(v);
  case FN_FLOOR: return f_floor(v);
  case FN_CEIL:  return -f_floor(-v);
  case FN_ROUND: return f_floor(v + 0.5f);
  }
  return F_NAN;
}

static float run(const Prog *p, float x) {
  float st[STACK_MAX + 1], b;
  int i, sp = 0;
  for (i = 0; i < p->n; i++) {
    switch (p->op[i]) {
    case OP_NUM:  st[sp++] = p->k[p->arg[i]]; break;
    case OP_X:    st[sp++] = x; break;
    case OP_VAR:  st[sp++] = M.var[p->arg[i]]; break;
    case OP_ANS:  st[sp++] = M.ans; break;
    case OP_NEG:  st[sp - 1] = -st[sp - 1]; break;
    case OP_FACT: st[sp - 1] = f_fact(st[sp - 1]); break;
    case OP_FN:   st[sp - 1] = apply_fn(p->arg[i], st[sp - 1]); break;
    case OP_Y:
      st[sp - 1] = M.fn[p->arg[i]].on ? run(&M.fn[p->arg[i]].prog, st[sp - 1]) : F_NAN;
      break;
    default:
      b = st[--sp];
      switch (p->op[i]) {
      case OP_ADD: st[sp - 1] += b; break;
      case OP_SUB: st[sp - 1] -= b; break;
      case OP_MUL: st[sp - 1] *= b; break;
      case OP_DIV: st[sp - 1] = b == 0.0f ? F_NAN : st[sp - 1] / b; break;
      case OP_POW: st[sp - 1] = f_pow(st[sp - 1], b); break;
      }
    }
  }
  return sp ? st[0] : F_NAN;
}

/* ==== state ============================================================= */

#define HIST 20
enum { K_VALUE, K_GRAPH, K_SET, K_ERROR };
enum { V_CALC, V_GRAPH };

typedef struct {
  char    in[EXPR_MAX + 1];
  char    out[40];
  uint8_t kind;
} Entry;

static struct {
  int   view;
  Entry hist[HIST];
  int   nhist;
  int   recall;                    /* -1, or the entry Up has put in the line */
  char  line[EXPR_MAX + 1];
  int   len, cur;
  /* the graph window */
  float x0, x1, y0, y1;
  int   trace, tcol, tfn;
  char  msg[48];
  uint32_t msg_until;
  int   printing;
  char  shown_ps[48];
  CRect at;
} T;

#define CALC_FILE CAPP_VAR "/calc.txt"

static void say(const char *s) {
  api->fmt(T.msg, sizeof T.msg, "%s", s);
  T.msg_until = api->ticks_ms() + 2500;
}

static void copy(char *d, const char *s, int n) {
  int i;
  for (i = 0; i < n - 1 && s[i]; i++) d[i] = s[i];
  d[i] = 0;
}

/* Through safefile.h: a power cut mid-save must not lose the graphs. */
static void save(void) {
  SafeFile f;
  char l[EXPR_MAX + 8];
  int i;
  api->mkdir(CAPP_VAR);
  if (safe_begin(&f, api, CALC_FILE) != 0) return;
  safe_line(&f, M.deg ? "deg\n" : "rad\n");
  for (i = 0; i < NFUN; i++) {
    if (!M.fn[i].on) continue;
    api->fmt(l, sizeof l, "y%d=%s\n", i + 1, M.fn[i].src);
    safe_line(&f, l);
  }
  safe_commit(&f);
}

static int define_fn(int k, const char *src);

static void load(void) {
  char buf[16 + NFUN * (EXPR_MAX + 8)], *p, *nl;
  int fd = safe_open_read(api, CALC_FILE), n;
  if (fd < 0) return;
  n = api->read(fd, buf, sizeof buf - 1);
  api->close(fd);
  if (n <= 0) return;
  buf[n] = 0;
  for (p = buf; *p; p = nl) {
    for (nl = p; *nl && *nl != '\n'; nl++) {}
    if (*nl) *nl++ = 0;
    if (p[0] == 'd' && p[1] == 'e' && p[2] == 'g') M.deg = 1;
    else if (p[0] == 'y' && p[1] >= '1' && p[1] <= '0' + NFUN && p[2] == '=')
      define_fn(p[1] - '1', p + 3);
  }
}

/* ==== the graph window and the rasteriser ============================== */

#define MAXW  384
#define STATUS_H 9
#define STRIP 8

enum { CF_GRID = 1, CF_AXIS = 2 };

static struct {
  int   valid, w, h;
  /* rows lit in each column, plus one so -1 (above) fits a byte */
  uint8_t lo[NFUN][MAXW], hi[NFUN][MAXW];
  uint8_t colf[MAXW], rowf[256];
  int   ax, ay;                   /* the axes' column and row, or -1 */
  float xstep, ystep;
} G;

/* What a strip is painted from, and what a print is written into -- never
 * both at once: api->print copies the document before it returns. */
#ifndef CALC_DOC_MAX
#define CALC_DOC_MAX 11264
#endif
static union {
  uint16_t strip[240 * STRIP];
  char     doc[CALC_DOC_MAX];
} U;

static void invalidate(void) { G.valid = 0; }

static void view_standard(int w, int h) {
  T.x0 = -10.0f; T.x1 = 10.0f;
  T.y1 = 10.0f * (float)h / (float)(w ? w : 240);
  T.y0 = -T.y1;
  invalidate();
}

/* 1, 2 or 5 times a power of ten, near range/divisions. */
static float nice_step(float range, int divisions) {
  float t = range / (float)divisions, p;
  int e;
  if (!(t > 0.0f) || !f_finite(t)) return 1.0f;
  e = dec_exp(t);
  p = scale10(1.0f, e);
  if (t < 1.5f * p) return p;             /* the nearest, not the next up: */
  if (t < 3.5f * p) return 2.0f * p;      /* +-10 wants a line every 2, */
  if (t < 7.5f * p) return 5.0f * p;      /* not every 5 */
  return 10.0f * p;
}

static int col_of(float x, int w) {
  float c = (x - T.x0) * (float)w / (T.x1 - T.x0);
  if (!(c > -2.0f)) return -2;
  if (c > (float)(w + 2)) return w + 2;
  return ifloor(c);
}
static int row_of(float y, int h) {
  float r = (T.y1 - y) * (float)h / (T.y1 - T.y0);
  if (!(r > -2.0f)) return -2;
  if (r > (float)(h + 2)) return h + 2;
  return ifloor(r);
}

static float x_at(int col, int w) { return T.x0 + ((float)col + 0.5f) * (T.x1 - T.x0) / (float)w; }

static void mark_lines(uint8_t *flags, int n, float lo, float hi, float step, int is_col) {
  int k0, k1, k, at;
  float q0 = lo / step, q1 = hi / step;
  if (f_abs(q0) > 1.0e6f || f_abs(q1) > 1.0e6f) return;
  k0 = ifloor(q0); k1 = ifloor(q1) + 1;
  if (k1 - k0 > 200) return;
  for (k = k0; k <= k1; k++) {
    at = is_col ? col_of((float)k * step, n) : row_of((float)k * step, n);
    if (at >= 0 && at < n) flags[at] |= k == 0 ? CF_AXIS : CF_GRID;
  }
}

/* Where each function's curve crosses each column: the rows from its own
 * sample to halfway to each neighbour's, so a steep stretch is a solid
 * line rather than dots. Neighbours on opposite sides of the window and
 * more than two heights apart are not joined -- that is tan's asymptote,
 * not a line. */
static void plot_prepare(int w, int h) {
  static float py[MAXW + 2];
  int f, c;
  if (G.valid && G.w == w && G.h == h) return;
  if (w > MAXW) w = MAXW;
  if (h > 254) h = 254;
  G.w = w; G.h = h;
  api->mem_set(G.colf, 0, sizeof G.colf);
  api->mem_set(G.rowf, 0, sizeof G.rowf);
  G.xstep = nice_step(T.x1 - T.x0, 8);
  G.ystep = nice_step(T.y1 - T.y0, 5);
  mark_lines(G.colf, w, T.x0, T.x1, G.xstep, 1);
  mark_lines(G.rowf, h, T.y0, T.y1, G.ystep, 0);
  G.ax = G.ay = -1;
  for (c = 0; c < w; c++) if (G.colf[c] & CF_AXIS) { G.ax = c; break; }
  for (c = 0; c < h; c++) if (G.rowf[c] & CF_AXIS) { G.ay = c; break; }

  for (f = 0; f < NFUN; f++) {
    if (!M.fn[f].on) continue;
    for (c = -1; c <= w; c++) {
      float v = run(&M.fn[f].prog, x_at(c, w)), r;
      if (!f_finite(v)) { py[c + 1] = F_NAN; continue; }
      r = (T.y1 - v) * (float)h / (T.y1 - T.y0);
      if (r < -64.0f * (float)h) r = -64.0f * (float)h;
      if (r > 64.0f * (float)h) r = 64.0f * (float)h;
      py[c + 1] = r;
    }
    for (c = 0; c < w; c++) {
      float a = py[c + 1], lo = a, hi = a;
      int j, ilo, ihi;
      if (f_isnan(a)) { G.lo[f][c] = 2; G.hi[f][c] = 1; continue; }
      for (j = 0; j < 2; j++) {
        float b = py[j ? c + 2 : c], m;
        if (f_isnan(b)) continue;
        if (((a < 0 && b > (float)h) || (a > (float)h && b < 0)) &&
            f_abs(a - b) > 2.0f * (float)h) continue;
        m = 0.5f * (a + b);
        if (m < lo) lo = m;
        if (m > hi) hi = m;
      }
      /* Both ends into [-1, h]: -1 and h are the rows just off the window,
       * so a span wholly above or below it lights nothing -- and a byte
       * never sees a negative row (it did, and wrapped to a lit column). */
      ilo = lo < -1.0f ? -1 : lo > (float)h ? h : ifloor(lo);
      ihi = hi < -1.0f ? -1 : hi > (float)h ? h : ifloor(hi);
      G.lo[f][c] = (uint8_t)(ilo + 1);
      G.hi[f][c] = (uint8_t)(ihi + 1);
    }
  }
  G.valid = 1;
}

static int on_curve(int f, int c, int r) {
  return c >= 0 && c < G.w && r + 1 >= G.lo[f][c] && r + 1 <= G.hi[f][c];
}

#define CLR_BG      CAPP_RGB(20, 22, 28)
#define CLR_PANEL   CAPP_RGB(36, 40, 50)
#define CLR_GRID    CAPP_RGB(40, 44, 54)
#define CLR_AXIS    CAPP_RGB(130, 138, 150)
#define CLR_TEXT    CAPP_RGB(224, 228, 236)
#define CLR_DIM     CAPP_RGB(120, 128, 140)
#define CLR_ANSWER  CAPP_RGB(255, 204, 82)
#define CLR_ERROR   CAPP_RGB(228, 86, 76)
#define CLR_SEL     CAPP_RGB(52, 80, 116)

static const uint16_t FN_CLR[NFUN] = {
  CAPP_RGB(86, 158, 232), CAPP_RGB(228, 86, 76),
  CAPP_RGB(110, 196, 128), CAPP_RGB(186, 148, 232),
};
static const char *const FN_STYLE[NFUN] = { "solid", "dashed", "dotted", "thin" };

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* The trace cursor's row, or -100 when the curve is off the window here. */
static int trace_row(int h) {
  float v;
  int r;
  if (!T.trace || !M.fn[T.tfn].on) return -100;
  v = run(&M.fn[T.tfn].prog, x_at(T.tcol, G.w));
  if (!f_finite(v)) return -100;
  r = row_of(v, h);
  return r < 0 || r >= h ? -100 : r;
}

static CRect trace_box(void) {
  int r = trace_row(G.h);
  if (r < -50) return rect(T.at.x, T.at.y, 0, 0);
  return rect(T.at.x + T.tcol - 3, T.at.y + r - 3, 7, 7);
}

static void paint_strip(CRect c, int y0, int n, int w, int trow) {
  int r, col, f;
  for (r = 0; r < n; r++) {
    int row = y0 + r;
    uint16_t *px = U.strip + r * w;
    uint8_t rf = G.rowf[row];
    for (col = 0; col < w; col++) {
      uint8_t cf = G.colf[col];
      uint16_t k = CLR_BG;
      if ((cf | rf) & CF_GRID) k = CLR_GRID;
      if ((cf | rf) & CF_AXIS) k = CLR_AXIS;
      for (f = NFUN - 1; f >= 0; f--)
        if (M.fn[f].on && !M.fn[f].hidden && on_curve(f, col, row)) k = FN_CLR[f];
      if (trow > -50) {
        int dx = col - T.tcol, dy = row - trow;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if ((dx == 3 && dy <= 3) || (dy == 3 && dx <= 3)) k = CLR_TEXT;
      }
      px[col] = k;
    }
  }
  api->pixels(rect(c.x, c.y + y0, w, n), U.strip);
}

static void window_text(char *out, int n) {
  char a[16], b[16], cc[16], d[16];
  fmt_num(a, sizeof a, T.x0, 4); fmt_num(b, sizeof b, T.x1, 4);
  fmt_num(cc, sizeof cc, T.y0, 4); fmt_num(d, sizeof d, T.y1, 4);
  api->fmt(out, (size_t)n, "x %s..%s  y %s..%s", a, b, cc, d);
}

static void paint_graph(CRect c) {
  int w = c.w > 240 ? 240 : c.w, h = c.h - STATUS_H, y, f, any = 0;
  CRect area = api->paint_area();
  char s[64];

  plot_prepare(w, h);
  {
    int trow = trace_row(h);
    for (y = 0; y < h; y += STRIP) {
      int n = h - y < STRIP ? h - y : STRIP;
      if (c.y + y + n <= area.y || c.y + y >= area.y + area.h) continue;
      paint_strip(c, y, n, w, trow);
    }
  }

  for (f = 0; f < NFUN; f++) {
    char line[32];
    if (!M.fn[f].on) continue;
    api->fmt(line, sizeof line, "y%d=%s%s", f + 1, M.fn[f].src, M.fn[f].hidden ? " (off)" : "");
    if (api->str_len(line) > 30) { line[28] = '.'; line[29] = '.'; line[30] = 0; }
    api->text((int16_t)(c.x + 2), (int16_t)(c.y + 2 + any * 9), line,
              M.fn[f].hidden ? CLR_DIM : FN_CLR[f], CLR_BG);
    any++;
  }
  if (!any)
    api->text((int16_t)(c.x + 2), (int16_t)(c.y + 2), "no graphs: Tab, then y=x^2", CLR_DIM, CLR_BG);

  y = c.y + c.h - STATUS_H;
  api->fill(rect(c.x, y, c.w, STATUS_H), CLR_PANEL);
  if (T.msg[0]) {
    api->fmt(s, sizeof s, "%s", T.msg);
  } else if (T.trace && M.fn[T.tfn].on) {
    char xs[16], ys[16];
    float x = x_at(T.tcol, w);
    fmt_num(xs, sizeof xs, x, 6);
    fmt_num(ys, sizeof ys, run(&M.fn[T.tfn].prog, x), 6);
    api->fmt(s, sizeof s, "y%d  x=%s  y=%s", T.tfn + 1, xs, ys);
  } else {
    window_text(s, sizeof s);
  }
  api->text((int16_t)(c.x + 2), (int16_t)(y + 1), s,
            T.trace && !T.msg[0] ? FN_CLR[T.tfn] : CLR_TEXT, CLR_PANEL);
}

/* ==== the calculator's roll ============================================ */

#define LINE_H 9
#define HEAD_H 10
#define INPUT_H 12

static void text_clip(int x, int y, const char *s, int cols, uint16_t fg, uint16_t bg) {
  char b[64];
  int n = (int)api->str_len(s);
  if (cols > 63) cols = 63;
  if (n <= cols) { api->text((int16_t)x, (int16_t)y, s, fg, bg); return; }
  api->mem_cpy(b, s, (size_t)cols);
  b[cols - 1] = '~';
  b[cols] = 0;
  api->text((int16_t)x, (int16_t)y, b, fg, bg);
}

static CRect input_rect(void) {
  return rect(T.at.x, T.at.y + T.at.h - INPUT_H, T.at.w, INPUT_H);
}

static void paint_input(CRect c) {
  int y = c.y + c.h - INPUT_H, cols = (c.w - 14) / 6, first = 0, i, x;
  char ch[2];
  api->fill(rect(c.x, y, c.w, INPUT_H), CLR_PANEL);
  api->text((int16_t)(c.x + 2), (int16_t)(y + 2), ">", CLR_DIM, CLR_PANEL);
  if (T.cur >= cols) first = T.cur - cols + 1;
  x = c.x + 12;
  ch[1] = 0;
  for (i = first; i < T.len && i < first + cols; i++) {
    ch[0] = T.line[i];
    api->text((int16_t)(x + (i - first) * 6), (int16_t)(y + 2), ch,
              i == T.cur ? CLR_BG : CLR_TEXT, i == T.cur ? CLR_TEXT : CLR_PANEL);
  }
  if (T.cur == T.len && T.cur - first < cols)
    api->fill(rect(x + (T.cur - first) * 6, y + 2, 6, 8), CLR_TEXT);
}

static void paint_head(CRect c) {
  api->fill(rect(c.x, c.y, c.w, HEAD_H), CLR_PANEL);
  api->text((int16_t)(c.x + 2), (int16_t)(c.y + 1), T.msg[0] ? T.msg : "Calc",
            T.msg[0] ? CLR_ANSWER : CLR_TEXT, CLR_PANEL);
  api->text((int16_t)(c.x + c.w - 20), (int16_t)(c.y + 1), M.deg ? "DEG" : "RAD",
            CLR_DIM, CLR_PANEL);
}

static void paint_calc(CRect c) {
  int top = c.y + HEAD_H + 1, y = c.y + c.h - INPUT_H - LINE_H - 1;
  int cols = (c.w - 4) / 6, i;

  paint_head(c);
  api->fill(rect(c.x, c.y + HEAD_H, c.w, c.h - HEAD_H - INPUT_H), CLR_BG);
  if (!T.nhist) {
    static const char *const hint[] = {
      "type a sum and press Enter:", "  2^10   sqrt(2)*3   5!   a=7",
      "or an equation to graph it:", "  y=x^2-3   y=sin(x)   y=2x+1",
      "Tab shows the graphs", "fn-p prints   fn-h for the rest",
    };
    for (i = 0; i < 6; i++)
      api->text((int16_t)(c.x + 4), (int16_t)(top + 4 + i * 11), hint[i],
                i & 1 ? CLR_TEXT : CLR_DIM, CLR_BG);
  }
  for (i = T.nhist - 1; i >= 0 && y - LINE_H >= top; i--) {
    const Entry *e = &T.hist[i];
    uint16_t bg = i == T.recall ? CLR_SEL : CLR_BG;
    uint16_t fg = e->kind == K_ERROR ? CLR_ERROR : e->kind == K_VALUE ? CLR_ANSWER : FN_CLR[0];
    int ow = (int)api->str_len(e->out) * 6;
    if (bg != CLR_BG) api->fill(rect(c.x, y - LINE_H, c.w, 2 * LINE_H), bg);
    api->text((int16_t)(c.x + c.w - 2 - ow), (int16_t)y, e->out, fg, bg);
    text_clip(c.x + 2, y - LINE_H, e->in, cols, CLR_TEXT, bg);
    y -= 2 * LINE_H;
  }
  paint_input(c);
}

static void app_paint(void *st, CRect c) {
  (void)st;
  T.at = c;
  if (T.view == V_GRAPH) paint_graph(c);
  else paint_calc(c);
}

/* ==== doing things ===================================================== */

static void push_hist(const char *in, const char *out, int kind) {
  Entry *e;
  if (T.nhist == HIST) {
    api->mem_move(&T.hist[0], &T.hist[1], sizeof T.hist[0] * (HIST - 1));
    T.nhist--;
  }
  e = &T.hist[T.nhist++];
  copy(e->in, in, sizeof e->in);
  copy(e->out, out, sizeof e->out);
  e->kind = (uint8_t)kind;
}

/* 0 on success, or -1 with s_err saying why. */
static int define_fn(int k, const char *src) {
  Prog p;
  const char *err;
  while (*src == ' ') src++;
  err = compile(src, &p, 1, 0);
  if (err) { copy(s_err, err, sizeof s_err); return -1; }
  M.fn[k].on = 1;
  M.fn[k].hidden = 0;
  api->mem_cpy(&M.fn[k].prog, &p, sizeof p);
  copy(M.fn[k].src, src, sizeof M.fn[k].src);
  invalidate();
  return 0;
}

static int free_slot(void) {
  int i;
  for (i = 0; i < NFUN; i++) if (!M.fn[i].on) return i;
  return -1;
}

/* Work out one line: a value, a variable, or a graph. The answer, or the
 * reason there is none, goes in `out`; the return is what kind it was. */
static int submit(const char *src, char *out, int n) {
  char lhs[8];
  const char *eq = 0, *s = src;
  int i, uses_x = 0, k;
  Prog p;
  const char *err;
  float v;

  while (*s == ' ') s++;
  for (i = 0; s[i]; i++) if (s[i] == '=') { eq = s + i; break; }

  if (eq) {
    int ln = 0;
    for (i = 0; s + i < eq && ln < 7; i++) if (s[i] != ' ') lhs[ln++] = (char)lower(s[i]);
    lhs[ln] = 0;
    if (lhs[0] == 'y' && (ln == 1 || (ln == 2 && lhs[1] >= '1' && lhs[1] <= '0' + NFUN))) {
      k = ln == 2 ? lhs[1] - '1' : free_slot();
      if (k < 0) { api->fmt(out, (size_t)n, "4 graphs already: y2=... replaces one"); return K_ERROR; }
      if (define_fn(k, eq + 1) != 0) { api->fmt(out, (size_t)n, "%s", s_err); return K_ERROR; }
      save();
      api->fmt(out, (size_t)n, "graphed as y%d", k + 1);
      return K_GRAPH;
    }
    if (ln == 1 && is_alpha(lhs[0]) && lhs[0] != 'x' && lhs[0] != 'e') {
      err = compile(eq + 1, &p, 0, &uses_x);
      if (!err && uses_x) err = "x only means something in a graph";
      if (err) { api->fmt(out, (size_t)n, "%s", err); return K_ERROR; }
      v = run(&p, 0.0f);
      if (!f_finite(v)) { fmt_num(out, n, v, 7); return K_ERROR; }
      M.var[lhs[0] - 'a'] = v;
      M.var_set |= 1u << (lhs[0] - 'a');
      invalidate();                  /* a graph may use it */
      {
        char num[24];
        fmt_num(num, sizeof num, v, 7);
        api->fmt(out, (size_t)n, "%c = %s", lhs[0], num);
      }
      return K_SET;
    }
    if (ln == 1 && lhs[0] == 'x') { api->fmt(out, (size_t)n, "x is the graph's variable"); return K_ERROR; }
    api->fmt(out, (size_t)n, "can't solve for that: try y=...");
    return K_ERROR;
  }

  err = compile(s, &p, 0, &uses_x);
  if (err) { api->fmt(out, (size_t)n, "%s", err); return K_ERROR; }
  if (uses_x) {
    k = free_slot();
    if (k < 0) { api->fmt(out, (size_t)n, "4 graphs already: y2=... replaces one"); return K_ERROR; }
    define_fn(k, s);
    save();
    api->fmt(out, (size_t)n, "graphed as y%d", k + 1);
    return K_GRAPH;
  }
  v = run(&p, 0.0f);
  if (!f_finite(v)) { fmt_num(out, n, v, 7); return K_ERROR; }
  M.ans = v;
  {
    char num[24];
    fmt_num(num, sizeof num, v, 7);
    api->fmt(out, (size_t)n, "= %s", num);
  }
  return K_VALUE;
}

static void enter(void) {
  char out[40];
  int kind;
  if (!T.len) return;
  T.line[T.len] = 0;
  kind = submit(T.line, out, sizeof out);
  push_hist(T.line, out, kind);
  T.recall = -1;
  if (kind != K_ERROR) { T.len = T.cur = 0; T.line[0] = 0; }
  if (kind == K_GRAPH && T.trace == 0) T.tfn = 0;
}

/* ---- print ---- */

/* A roll line that the print markup would take for something else --
 * `---5` for a rule -- gets a leading space. Nothing here makes # [ or %. */
static int doc_line(int at, const char *pre, const char *s) {
  int n = (int)sizeof U.doc - at;
  if (n <= 2) return at;
  if (!pre[0] && s[0] == '-' && s[1] == '-' && s[2] == '-') pre = " ";
  at += api->fmt(U.doc + at, (size_t)n, "%s%s\n", pre, s);
  return at < (int)sizeof U.doc ? at : (int)sizeof U.doc - 1;
}

#define PW   352               /* the plot on paper, inside the margins */
#define PH   160
#define PX0  16

/* Ink for one pixel of the printed plot. Axes are two pixels, ticks six
 * pixels across the axis at each grid step (a grid of dotted lines would
 * be a hundred runs a row), curves two pixels, told apart by pattern
 * since paper has one colour. */
static int print_ink(int c, int r) {
  int f, ax = G.ax, ay = G.ay;
  if (c == 0 || c == G.w - 1 || r == 0 || r == G.h - 1) return 1;      /* frame */
  if (ax >= 0 && (c == ax || c == ax + 1)) return 1;
  if (ay >= 0 && (r == ay || r == ay + 1)) return 1;
  if (ay >= 0 && (G.colf[c] & CF_GRID) && r >= ay - 3 && r <= ay + 4) return 1;
  if (ax >= 0 && (G.rowf[r] & CF_GRID) && c >= ax - 3 && c <= ax + 4) return 1;
  for (f = 0; f < NFUN; f++) {
    if (!M.fn[f].on || M.fn[f].hidden) continue;
    if (f == 1 && c % 16 >= 10) continue;
    if (f == 2 && c % 6 >= 2) continue;
    if (f == 3 ? on_curve(f, c, r)
               : on_curve(f, c, r) || on_curve(f, c - 1, r) ||
                 on_curve(f, c, r + 1) || on_curve(f, c - 1, r + 1))
      return 1;
  }
  return 0;
}

/* One printed row as a `%%` line (printdoc.h): runs from the paper's left
 * edge, white first and the margin included -- or the margin as a run and
 * then the pixels raw, six to a base64 digit, when that is shorter. A
 * smooth curve is a few runs a row; sin(5x) crosses a row sixteen times,
 * and in runs alone one graph of it was 20 KB. Raw caps a row at 64. */
static int row_line(char *out, int size, int r) {
  static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static uint8_t ink[MAXW];
  char raw[80];
  int len, c, cur = 0, run = PX0, nums = 0, n = 0;

  for (c = 0; c < G.w; c++) ink[c] = (uint8_t)print_ink(c, r);

  n = api->fmt(raw, sizeof raw, "%%%%%d=", PX0);
  for (c = 0; c < G.w && n < (int)sizeof raw - 1; c += 6) {
    int d = 0, b;
    for (b = 0; b < 6; b++) d = d << 1 | (c + b < G.w ? ink[c + b] : 0);
    raw[n++] = B64[d];
  }
  raw[n] = 0;

  len = api->fmt(out, (size_t)size, "%%%%");
  for (c = 0; c < G.w && len < n; c++) {
    if (ink[c] == cur) { run++; continue; }
    len += api->fmt(out + len, (size_t)(size - len), nums++ ? ",%d" : "%d", run);
    cur = ink[c];
    run = 1;
  }
  if (cur && len < n) len += api->fmt(out + len, (size_t)(size - len), nums ? ",%d" : "%d", run);
  if (len < n) return len;
  copy(out, raw, size);
  return n;
}

/* The plot as `%%` lines, identical rows merged into one with a count.
 * Returns the new end of the document, or -1 if it ran out of room. */
static int doc_plot(int at) {
  static char row[200], prev[200];
  int r, reps = 0;
  float sx0 = T.x0, sx1 = T.x1, sy0 = T.y0, sy1 = T.y1;
  /* The screen's x range; the height keeps its centre and the screen's
   * units per pixel, so a circle on screen is a circle on paper. */
  {
    int sw = T.at.w ? (T.at.w > 240 ? 240 : T.at.w) : 240;
    int sh = T.at.h ? T.at.h - STATUS_H : 126;
    float mid = 0.5f * (T.y0 + T.y1);
    float half = 0.5f * (T.y1 - T.y0) * (float)PH * (float)sw / ((float)sh * (float)PW);
    T.y0 = mid - half; T.y1 = mid + half;
  }
  invalidate();
  plot_prepare(PW, PH);
  for (r = 0; r <= PH; r++) {
    if (r < PH) {
      row_line(row, sizeof row, r);
      if (reps && reps < 64) {
        int i, same = 1;
        for (i = 0; row[i] || prev[i]; i++) if (row[i] != prev[i]) { same = 0; break; }
        if (same) { reps++; continue; }
      }
    }
    if (reps) {
      int room = (int)sizeof U.doc - at;
      if ((int)api->str_len(prev) + 8 > room) { at = -1; break; }
      if (reps > 1) at += api->fmt(U.doc + at, (size_t)room, "%%%%%d*%s\n", reps, prev + 2);
      else at += api->fmt(U.doc + at, (size_t)room, "%s\n", prev);
    }
    if (r < PH) { copy(prev, row, sizeof prev); reps = 1; }
  }
  T.x0 = sx0; T.x1 = sx1; T.y0 = sy0; T.y1 = sy1;
  invalidate();
  return at;
}

static void print_now(void) {
  int at = 0, i, rc;
  char a[48];
  if (T.view == V_GRAPH) {
    int any = 0;
    for (i = 0; i < NFUN; i++) any |= M.fn[i].on && !M.fn[i].hidden;
    if (!any) { say("no graphs to print"); return; }
    at = doc_line(at, "# ", "Graph");
    at = doc_plot(at);
    if (at < 0) { say("too much to print"); return; }
    at = doc_line(at, "", "");
    for (i = 0; i < NFUN; i++) {
      char l[EXPR_MAX + 32];
      if (!M.fn[i].on || M.fn[i].hidden) continue;
      api->fmt(l, sizeof l, "y%d = %s  (%s)", i + 1, M.fn[i].src, FN_STYLE[i]);
      at = doc_line(at, "", l);
    }
    {
      char w[64], xs[16], ys[16];
      window_text(w, sizeof w);
      at = doc_line(at, "", w);
      fmt_num(xs, sizeof xs, G.xstep, 4);
      fmt_num(ys, sizeof ys, G.ystep, 4);
      api->fmt(a, sizeof a, "ticks every %s across, %s up", xs, ys);
      at = doc_line(at, "", a);
    }
  } else {
    if (!T.nhist) { say("nothing to print yet"); return; }
    at = doc_line(at, "# ", "Calc");
    for (i = 0; i < T.nhist; i++) {
      at = doc_line(at, "", T.hist[i].in);
      at = doc_line(at, "## ", T.hist[i].out);
    }
  }
  rc = api->print_fonts(U.doc, "print24", "print24b", "print34b");
  if (rc == 0)       { T.printing = 1; T.shown_ps[0] = 0; say("printing..."); }
  else if (rc == -1) say("still printing the last one");
  else if (rc == -2) say("no printer: print scan in the console");
  else               say("could not print: no memory");
}

/* ---- the graph's controls ---- */

static void zoom(float by) {
  float cx = T.trace ? x_at(T.tcol, G.w ? G.w : 240) : 0.5f * (T.x0 + T.x1);
  float cy = 0.5f * (T.y0 + T.y1);
  float hw = 0.5f * (T.x1 - T.x0) * by, hh = 0.5f * (T.y1 - T.y0) * by;
  if (hw < 1.0e-4f || hw > 1.0e6f) return;
  T.x0 = cx - hw; T.x1 = cx + hw; T.y0 = cy - hh; T.y1 = cy + hh;
  if (T.trace) T.tcol = (G.w ? G.w : 240) / 2;
  invalidate();
}

static void pan(int dx, int dy) {
  float sx = (T.x1 - T.x0) * 0.125f * (float)dx, sy = (T.y1 - T.y0) * 0.125f * (float)dy;
  T.x0 += sx; T.x1 += sx; T.y0 += sy; T.y1 += sy;
  invalidate();
}

/* The height that shows every visible curve across this width. */
static void fit(void) {
  int w = G.w ? G.w : 240, c, f, any = 0;
  float lo = 0, hi = 0, pad;
  for (f = 0; f < NFUN; f++) {
    if (!M.fn[f].on || M.fn[f].hidden) continue;
    for (c = 0; c < w; c += 2) {
      float v = run(&M.fn[f].prog, x_at(c, w));
      if (!f_finite(v) || f_abs(v) > 1.0e6f) continue;
      if (!any || v < lo) lo = v;
      if (!any || v > hi) hi = v;
      any = 1;
    }
  }
  if (!any) { say("nothing to fit"); return; }
  if (hi - lo < 1.0e-3f) { lo -= 1.0f; hi += 1.0f; }
  pad = (hi - lo) * 0.08f;
  T.y0 = lo - pad; T.y1 = hi + pad;
  invalidate();
}

static int first_fn(int from, int dir) {
  int i, k;
  for (i = 1; i <= NFUN; i++) {
    k = (from + dir * i + NFUN * 2) % NFUN;
    if (M.fn[k].on && !M.fn[k].hidden) return k;
  }
  return M.fn[from].on && !M.fn[from].hidden ? from : -1;
}

static void trace_toggle(void) {
  int k;
  if (T.trace) { T.trace = 0; return; }
  k = first_fn(T.tfn, 0);
  if (M.fn[T.tfn].on && !M.fn[T.tfn].hidden) k = T.tfn;
  if (k < 0) { say("no graph to trace"); return; }
  T.trace = 1; T.tfn = k;
  T.tcol = (G.w ? G.w : 240) / 2;
}

/* Only what moved: the old box, the new one and the status line. */
static void trace_move(int dcol) {
  int w = G.w ? G.w : 240;
  api->damage(trace_box());
  T.tcol += dcol;
  if (T.tcol < 0) { T.tcol = w / 8; pan(-1, 0); return; }
  if (T.tcol >= w) { T.tcol = w - 1 - w / 8; pan(1, 0); return; }
  api->damage(trace_box());
  api->damage(rect(T.at.x, T.at.y + T.at.h - STATUS_H, T.at.w, STATUS_H));
  /* the legend is drawn over the plot: a box under it takes it along */
  api->damage(rect(T.at.x, T.at.y, T.at.w, 2 + 9 * NFUN));
}

static void clear_graphs(void) {
  int i;
  for (i = 0; i < NFUN; i++) M.fn[i].on = 0;
  T.trace = 0;
  invalidate();
  save();
  say("graphs cleared");
}

static int graph_key(uint8_t k) {
  if (T.trace) {
    switch (k) {
    case CAPP_KEY_LEFT:  trace_move(-1); return 1;
    case CAPP_KEY_RIGHT: trace_move(1); return 1;
    case CAPP_KEY_UP: case CAPP_KEY_DOWN: {
      int n = first_fn(T.tfn, k == CAPP_KEY_UP ? -1 : 1);
      if (n >= 0) T.tfn = n;
      return 1;
    }
    case CAPP_KEY_BACK: case 0x7F:
      M.fn[T.tfn].on = 0;
      save();
      invalidate();
      say("graph deleted");
      if (first_fn(T.tfn, 1) < 0) T.trace = 0; else T.tfn = first_fn(T.tfn, 1);
      return 1;
    case CAPP_KEY_ESC: case 't': case 'T': case CAPP_KEY_ENTER:
      T.trace = 0; return 1;
    }
  } else {
    switch (k) {
    case CAPP_KEY_LEFT:  pan(-1, 0); return 1;
    case CAPP_KEY_RIGHT: pan(1, 0); return 1;
    case CAPP_KEY_UP:    pan(0, 1); return 1;
    case CAPP_KEY_DOWN:  pan(0, -1); return 1;
    case 't': case 'T': case CAPP_KEY_ENTER: trace_toggle(); return 1;
    case CAPP_KEY_ESC: T.view = V_CALC; return 1;
    }
  }
  switch (k) {
  case '+': case '=': zoom(0.5f); return 1;
  case '-': case '_': zoom(2.0f); return 1;
  case '0': case 'z': case 'Z': view_standard(G.w ? G.w : 240, G.h ? G.h : 126); return 1;
  case 'f': case 'F': fit(); return 1;
  case '1': case '2': case '3': case '4': {
    int i = k - '1';
    if (!M.fn[i].on) { say("no such graph"); return 1; }
    M.fn[i].hidden = !M.fn[i].hidden;
    if (T.trace && T.tfn == i && M.fn[i].hidden) trace_toggle();
    invalidate();
    return 1;
  }
  case 0x09: case 'g': case 'G': T.view = V_CALC; T.trace = 0; return 1;
  case 'y': case 'Y':
    T.view = V_CALC; T.trace = 0;
    T.len = T.cur = 0;
    T.line[T.len++] = 'y'; T.line[T.len++] = '=';
    T.cur = T.len;
    return 1;
  }
  return 0;
}

static void insert(const char *s) {
  int n = (int)api->str_len(s);
  if (T.len + n > EXPR_MAX) return;
  api->mem_move(T.line + T.cur + n, T.line + T.cur, (size_t)(T.len - T.cur));
  api->mem_cpy(T.line + T.cur, s, (size_t)n);
  T.len += n; T.cur += n;
  T.line[T.len] = 0;
}

static void recall(int dir) {
  int i = T.recall < 0 ? T.nhist : T.recall;
  i += dir;
  if (i < 0 || !T.nhist) return;
  if (i >= T.nhist) { T.recall = -1; T.len = T.cur = 0; T.line[0] = 0; return; }
  T.recall = i;
  copy(T.line, T.hist[i].in, sizeof T.line);
  T.len = T.cur = (int)api->str_len(T.line);
}

static int calc_key(uint8_t k) {
  CRect in = input_rect();
  switch (k) {
  case CAPP_KEY_ENTER: enter(); return 1;
  case 0x09: T.view = V_GRAPH; return 1;
  case CAPP_KEY_ESC:
    if (!T.len && T.recall < 0) return 0;          /* top level: not ours */
    T.len = T.cur = 0; T.line[0] = 0; T.recall = -1;
    return 1;
  case CAPP_KEY_UP:   recall(-1); return 1;
  case CAPP_KEY_DOWN: recall(1); return 1;
  case CAPP_KEY_LEFT:  if (T.cur > 0) T.cur--; api->damage(in); return 1;
  case CAPP_KEY_RIGHT: if (T.cur < T.len) T.cur++; api->damage(in); return 1;
  case CAPP_KEY_BACK:
    if (T.cur > 0) {
      api->mem_move(T.line + T.cur - 1, T.line + T.cur, (size_t)(T.len - T.cur + 1));
      T.len--; T.cur--;
    }
    api->damage(in);
    return 1;
  case 0x7F:
    if (T.cur < T.len) {
      api->mem_move(T.line + T.cur, T.line + T.cur + 1, (size_t)(T.len - T.cur));
      T.len--;
    }
    api->damage(in);
    return 1;
  }
  if (k >= 0x20 && k < 0x7F) {
    char s[2];
    /* An operator first continues the last answer, as on any calculator. */
    if (!T.len && T.nhist && (k == '+' || k == '*' || k == '/' || k == '^' || k == '!'))
      insert("ans");
    s[0] = (char)k; s[1] = 0;
    insert(s);
    api->damage(in);
    return 1;
  }
  return 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  return T.view == V_GRAPH ? graph_key(k) : calc_key(k);
}

/* Typing only in the calculator: in the graph ; , . / are the arrows. */
static int app_wants_text(void *st) { (void)st; return T.view == V_CALC; }

static int app_tick(void *st, uint32_t now) {
  int dirty = 0;
  (void)st;
  if (T.printing) {
    const char *ps = api->print_status();
    int i, same = 1;
    for (i = 0; ps[i] || T.shown_ps[i]; i++) if (ps[i] != T.shown_ps[i]) { same = 0; break; }
    if (!same) {
      copy(T.shown_ps, ps, sizeof T.shown_ps);
      say(ps);
      dirty = 1;
      if (!(ps[0] == 's' || ps[0] == 'c' || ps[0] == 'w' || (ps[0] == 'p' && ps[5] == 'i')))
        T.printing = 0;               /* not starting/connecting/waiting/printing */
    }
  }
  if (T.msg[0] && (int32_t)(now - T.msg_until) >= 0) { T.msg[0] = 0; dirty = 1; }
  if (dirty && T.at.w) {
    if (T.view == V_GRAPH)
      api->damage(rect(T.at.x, T.at.y + T.at.h - STATUS_H, T.at.w, STATUS_H));
    else
      api->damage(rect(T.at.x, T.at.y, T.at.w, HEAD_H));
  }
  return dirty;
}

enum { ACT_VIEW = 1, ACT_TRACE, ACT_FIT, ACT_STD, ACT_ANGLE, ACT_CLEAR,
       ACT_CLEARG, ACT_PRINT, ACT_EVAL, ACT_GRAPH };

static const CappParam P_EXPR[] = { { "sum", CAPP_ARG_TEXT, "what to work out: 2^10, sqrt(2)*3" } };
static const CappParam P_EQ[]   = { { "equation", CAPP_ARG_TEXT, "what to graph: y=x^2" } };

static const CappAction ACTIONS[] = {
  { "view",   "Calc / Graph",      "View",  0x07, ACT_VIEW },     /* ctrl-g, and Tab */
  { "trace",  "Trace",             "Graph", 0x14, ACT_TRACE },    /* ctrl-t */
  { "fit",    "Fit height",        "Graph", 0x06, ACT_FIT },      /* ctrl-f */
  { "zoom.standard", "Standard zoom", "Graph", 0x12, ACT_STD },   /* ctrl-r */
  { "graphs.clear",  "Clear graphs",  "Graph", 0x0B, ACT_CLEARG },/* ctrl-k */
  { "angle",  "Degrees / radians", "Calc",  0x04, ACT_ANGLE },    /* ctrl-d */
  { "clear",  "Clear the roll",    "Calc",  0x0C, ACT_CLEAR },    /* ctrl-l */
  { "print",  "Print",             "Calc",  CAPP_KEY_PRINT, ACT_PRINT },   /* fn-p */
  { "eval",   "Work out",          0, 0, ACT_EVAL,
    "work out a sum: 2^10, sqrt(2)*3", P_EXPR, 1, CAPP_CMD_YES },
  { "graph",  "Graph",             0, 0, ACT_GRAPH,
    "open Calc graphing an equation: y=x^2", P_EQ, 1, CAPP_CMD_YES | CAPP_CMD_OPEN },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Calc",
  /* 16x16: a calculator, a screen over three rows of keys. */
  { 0x3F, 0xFC, 0x20, 0x04, 0x2F, 0xF4, 0x28, 0x14,
    0x2F, 0xF4, 0x20, 0x04, 0x2D, 0xB4, 0x2D, 0xB4,
    0x20, 0x04, 0x2D, 0xB4, 0x2D, 0xB4, 0x20, 0x04,
    0x2D, 0xB4, 0x2D, 0xB4, 0x20, 0x04, 0x3F, 0xFC },
  "Enter\twork it out\nup/down\tearlier lines\ny=...\tgraph it (y1..y4)\n"
  "a=5\tset a variable\nans\tthe last answer\nTab\tcalculator / graph\n"
  "arrows\tpan the graph\n+ -\tzoom\nf\tfit the height\n0\tstandard zoom\n"
  "t\ttrace: arrows move\n1-4\tshow/hide a graph\ndel\tdelete traced graph\n"
  "fns\tsin cos tan asin acos atan\n\tsqrt abs ln log exp\n\tfloor ceil round  n!\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static int app_action(void *st, int a) {
  (void)st;
  switch (a) {
  case ACT_VIEW:   T.view = T.view == V_CALC ? V_GRAPH : V_CALC; T.trace = 0; return 1;
  case ACT_TRACE:  T.view = V_GRAPH; trace_toggle(); return 1;
  case ACT_FIT:    T.view = V_GRAPH; fit(); return 1;
  case ACT_STD:    view_standard(G.w ? G.w : 240, G.h ? G.h : 126); return 1;
  case ACT_CLEARG: clear_graphs(); return 1;
  case ACT_ANGLE:
    M.deg = !M.deg;
    invalidate();
    save();
    say(M.deg ? "angles in degrees" : "angles in radians");
    return 1;
  case ACT_CLEAR:  T.nhist = 0; T.recall = -1; return 1;
  case ACT_PRINT:  print_now(); return 1;
  }
  return 0;
}

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  int kind;
  (void)st;
  if (argc < 1) return -1;
  if (action != ACT_EVAL && action != ACT_GRAPH) return -1;
  kind = submit(argv[0], out, (int)n);
  return kind == K_ERROR ? -1 : 0;
}

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  api->mem_set(&T, 0, sizeof T);
  api->mem_set(&M, 0, sizeof M);
  api->mem_set(&G, 0, sizeof G);
  T.recall = -1;
  view_standard(240, 135 - STATUS_H);
  load();

  /* `run calc y=x^2`, and what the `graph` command opens: the words are
   * one line, worked out at once, and a graph is shown. */
  if (argc > 1) {
    char line[EXPR_MAX + 1], out[40];
    int i, at = 0, kind;
    for (i = 1; i < argc; i++) {
      int n = (int)api->str_len(argv[i]);
      if (at + n + 1 > EXPR_MAX) break;
      if (at) line[at++] = ' ';
      api->mem_cpy(line + at, argv[i], (size_t)n);
      at += n;
    }
    line[at] = 0;
    kind = submit(line, out, sizeof out);
    push_hist(line, out, kind);
    if (kind == K_GRAPH) T.view = V_GRAPH;
  }

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
