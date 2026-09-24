/* Calc, on the host.
 *
 * The app carries its own maths -- division included, since an app has no
 * libgcc -- so the first thing pinned here is that it agrees with the host's
 * libm to float precision. Then the language, the number formatting, the
 * plot, and the printed graph, which is fed back through the real printdoc
 * renderer to check every `%%` line it wrote is one the printer will take.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "kernel/sys/printdoc.h"

#define capp_info calc_capp_info
#define capp_main calc_capp_main
#include "apps/calc.c"
#undef capp_info
#undef capp_main

#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); tt_checks++; \
  if (strcmp(_a, _b)) { tt_fails++; printf("  FAIL %s:%d in %s: %s is \"%s\", not \"%s\"\n", \
  __FILE__, __LINE__, tt_current, #a, _a, _b); } } while (0)

#define SW 240
#define SH 135

static CappUi   INST;
static int      OUT_OF_BOUNDS;
static char     PRINTED[8192];
static int      PRINTS;

static void fk_fill(CRect r, uint16_t c) {
  (void)c;
  if (r.x < 0 || r.y < 0 || r.x + r.w > SW || r.y + r.h > SH) OUT_OF_BOUNDS++;
}
static void fk_text(int16_t x, int16_t y, const char *s, uint16_t f, uint16_t b) {
  (void)f; (void)b;
  if (x < 0 || y < 0 || y + 8 > SH || x + 6 * (int)strlen(s) > SW) OUT_OF_BOUNDS++;
}
static void fk_pixels(CRect r, const uint16_t *px) {
  (void)px;
  if (r.x < 0 || r.y < 0 || r.x + r.w > SW || r.y + r.h > SH) OUT_OF_BOUNDS++;
}
static void *fk_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *fk_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *fk_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static size_t fk_strlen(const char *s) { return strlen(s); }
static uint32_t fk_ticks(void) { return 1000; }
static int fk_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
static int fk_open(const char *p, int f) { (void)p; (void)f; return -1; }
static int fk_mkdir(const char *p) { (void)p; return 0; }
static int fk_stat(const char *p, CappStat *st) { (void)p; (void)st; return -1; }
static void fk_damage(CRect r) { (void)r; }
static CRect fk_area(void) { CRect r = { 0, 0, SW, SH }; return r; }
static void fk_ui(const CappUi *ui) { INST = *ui; }
static int fk_print_fonts(const char *doc, const char *a, const char *b, const char *c) {
  (void)a; (void)b; (void)c;
  snprintf(PRINTED, sizeof PRINTED, "%s", doc);
  PRINTS++;
  return 0;
}
static const char *fk_print_status(void) { return ""; }

static CardApi API;

static void boot(void) {
  char arg0[] = "calc";
  char *argv[1];
  argv[0] = arg0;
  memset(&API, 0, sizeof API);
  API.version = CAPP_API_VERSION;
  API.fill = fk_fill;
  API.text = fk_text;
  API.pixels = fk_pixels;
  API.mem_set = fk_memset;
  API.mem_cpy = fk_memcpy;
  API.mem_move = fk_memmove;
  API.str_len = fk_strlen;
  API.fmt = fk_fmt;
  API.ticks_ms = fk_ticks;
  API.open = fk_open;
  API.mkdir = fk_mkdir;
  API.stat = fk_stat;
  API.damage = fk_damage;
  API.paint_area = fk_area;
  API.ui = fk_ui;
  API.print_fonts = fk_print_fonts;
  API.print_status = fk_print_status;
  OUT_OF_BOUNDS = 0;
  PRINTS = 0;
  PRINTED[0] = 0;
  memset(&INST, 0, sizeof INST);
  calc_capp_main(&API, 1, argv);
}

static void paint(void) {
  CRect c = { 0, 0, SW, SH };
  INST.paint(INST.state, c);
}

static void type(const char *s) {
  for (; *s; s++) INST.key(INST.state, (uint8_t)*s);
}

/* The answer to a line, as the roll shows it. */
static const char *calc(const char *line) {
  static char out[40];
  submit(line, out, sizeof out);
  return out;
}

static int close_to(float got, double want) {
  double err = fabs((double)got - want);
  return err <= 3e-6 * fabs(want) || err <= 2e-7;
}

/* ---- the maths ---- */

void test_calc_division_matches_the_hardware(void) {
  static const float a[] = { 1, 7, -3.5f, 1e30f, 1e-30f, 123456.7f, 0.1f, 2 };
  static const float b[] = { 3, -7, 0.25f, 3e-8f, 7e20f, 0.001f, 10, 1.0000001f };
  int i, j;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++) {
      float got = f_div(a[i], b[j]), want = a[i] / b[j];
      CHECK(fabsf(got - want) <= fabsf(want) * 2.5e-7f);
    }
  CHECK(f_isnan(f_div(0, 0)));
  CHECK(!f_finite(f_div(1, 0)) && f_div(1, 0) > 0);
  CHECK(!f_finite(f_div(-1, 0)) && f_div(-1, 0) < 0);
  CHECK(f_div(5, F_INF) == 0.0f);
}

void test_calc_functions_agree_with_libm(void) {
  float x;
  for (x = -20.0f; x <= 20.0f; x += 0.37f) {
    CHECK(close_to(f_sincos(x, 0, 0), sin((double)x)) || fabs(sin((double)x)) < 1e-6);
    CHECK(close_to(f_sincos(x, 1, 0), cos((double)x)) || fabs(cos((double)x)) < 1e-6);
    CHECK(close_to(f_atan(x), atan((double)x)));
    CHECK(close_to(f_exp(x), exp((double)x)));
  }
  for (x = 0.001f; x < 1e6f; x *= 1.7f) {
    CHECK(close_to(f_ln(x), log((double)x)));
    CHECK(close_to(f_sqrt(x), sqrt((double)x)));
    CHECK(close_to(f_pow(x, 0.7f), pow((double)x, 0.7)));
  }
  for (x = -1.0f; x <= 1.0f; x += 0.05f) {
    CHECK(close_to(f_asin(x), asin((double)x)));
    CHECK(close_to(f_acos(x), acos((double)x)));
  }
  CHECK(f_pow(2, 10) == 1024.0f);
  CHECK(f_pow(-2, 3) == -8.0f);
  CHECK(f_pow(2, -2) == 0.25f);
  CHECK(f_isnan(f_pow(-8, 0.5f)));
  CHECK(f_fact(5) == 120.0f);
  CHECK(f_isnan(f_fact(2.5f)));
  CHECK(f_isnan(f_sqrt(-1)));
  CHECK(f_isnan(f_ln(-1)));
}

void test_calc_whole_turns_are_exactly_zero(void) {
  CHECK(f_sincos(F_PI, 0, 0) == 0.0f);
  CHECK(f_sincos(180, 0, 1) == 0.0f);
  CHECK(f_sincos(90, 1, 1) == 0.0f);
  CHECK(f_sincos(90, 0, 1) == 1.0f);
  CHECK(f_sincos(1e-8f, 0, 0) > 0.0f);           /* small is not the same as noise */
}

/* ---- numbers as text ---- */

void test_calc_numbers_read_like_a_calculator(void) {
  char s[32];
  boot();
  fmt_num(s, sizeof s, 0.1f + 0.2f, 7);      CHECK_STR(s, "0.3");
  fmt_num(s, sizeof s, 1.0f / 3.0f, 7);      CHECK_STR(s, "0.3333333");
  fmt_num(s, sizeof s, 123456, 7);           CHECK_STR(s, "123456");
  fmt_num(s, sizeof s, 1234567, 7);          CHECK_STR(s, "1234567");
  fmt_num(s, sizeof s, 1e10f, 7);            CHECK_STR(s, "1e10");
  fmt_num(s, sizeof s, 2.5e-7f, 7);          CHECK_STR(s, "2.5e-7");
  fmt_num(s, sizeof s, -0.00012f, 7);        CHECK_STR(s, "-0.00012");
  fmt_num(s, sizeof s, 9.9999999f, 7);       CHECK_STR(s, "10");
  fmt_num(s, sizeof s, 0, 7);                CHECK_STR(s, "0");
  fmt_num(s, sizeof s, F_NAN, 7);            CHECK_STR(s, "undefined");
  fmt_num(s, sizeof s, -5.25f, 4);           CHECK_STR(s, "-5.25");
  fmt_num(s, sizeof s, 3.14159265f, 4);      CHECK_STR(s, "3.142");
}

/* ---- the language ---- */

void test_calc_precedence_and_implicit_multiplication(void) {
  boot();
  CHECK_STR(calc("2+3*4"), "= 14");
  CHECK_STR(calc("2^3^2"), "= 512");
  CHECK_STR(calc("-2^2"), "= -4");
  CHECK_STR(calc("2^-1"), "= 0.5");
  CHECK_STR(calc("(1+2)(3+4)"), "= 21");
  CHECK_STR(calc("2pi"), "= 6.283185");
  CHECK_STR(calc("3!+1"), "= 7");
  CHECK_STR(calc("sqrt(16)+abs(-2)"), "= 6");
  CHECK_STR(calc("2e3"), "= 2000");
  CHECK_STR(calc("2e"), "= 5.436563");       /* float e is 2.71828175 */
  CHECK_STR(calc("10/4"), "= 2.5");
  CHECK_STR(calc("log(1000)"), "= 3");
  CHECK_STR(calc("ln(e)"), "= 1");
  CHECK_STR(calc("SIN(0)"), "= 0");
}

void test_calc_variables_and_ans(void) {
  boot();
  CHECK_STR(calc("a=5"), "a = 5");
  CHECK_STR(calc("2a+1"), "= 11");
  CHECK_STR(calc("ans*2"), "= 22");
  CHECK_STR(calc("b"), "b is not set (b=5 sets it)");
  CHECK_STR(calc("x=3"), "x is the graph's variable");
}

void test_calc_errors_say_what_is_wrong(void) {
  boot();
  CHECK_STR(calc("(1+2"), "missing )");
  CHECK_STR(calc("1+"), "unfinished");
  CHECK_STR(calc("sin 3"), "sin needs brackets: sin(...)");
  CHECK_STR(calc("1/0"), "undefined");
  CHECK_STR(calc("2 3"), "missing an operator");
  CHECK_STR(calc("1)"), "unexpected )");
  CHECK_STR(calc("x^2=4"), "can't solve for that: try y=...");
}

void test_calc_equations_become_graphs(void) {
  boot();
  CHECK_STR(calc("y=x^2"), "graphed as y1");
  CHECK_STR(calc("2x+1"), "graphed as y2");         /* mentions x: a graph too */
  CHECK_STR(calc("y4=sin(x)"), "graphed as y4");
  CHECK_STR(calc("y1(3)"), "= 9");                   /* and they can be called */
  CHECK_STR(calc("y=y1(x)"), "a graph can't use another");
  CHECK_STR(calc("y=x"), "graphed as y3");
  CHECK_STR(calc("y=x+1"), "4 graphs already: y2=... replaces one");
  CHECK_STR(calc("y2=x-1"), "graphed as y2");
  CHECK_STR(calc("y2(1)"), "= 0");
}

void test_calc_typing_and_the_operator_shortcut(void) {
  boot();
  type("2+3\r");
  CHECK_EQ(T.nhist, 1);
  CHECK_STR(T.hist[0].out, "= 5");
  type("*4\r");                                      /* ans*4 */
  CHECK_STR(T.hist[1].in, "ans*4");
  CHECK_STR(T.hist[1].out, "= 20");
  INST.key(INST.state, CAPP_KEY_UP);
  CHECK_STR(T.line, "ans*4");
  CHECK_EQ(INST.key(INST.state, CAPP_KEY_ESC), 1);  /* clears the line... */
  CHECK_EQ(T.len, 0);
  CHECK_EQ(INST.key(INST.state, CAPP_KEY_ESC), 0);  /* ...then is not ours */
  CHECK(INST.wants_text(INST.state));
  INST.key(INST.state, 0x09);
  CHECK_EQ(T.view, V_GRAPH);
  CHECK(!INST.wants_text(INST.state));               /* ; , . / are arrows here */
}

/* ---- the plot ---- */

void test_calc_a_line_is_drawn_where_it_should_be(void) {
  int c, gaps = 0;
  boot();
  calc("y=x");
  T.x0 = -10; T.x1 = 10; T.y0 = -10; T.y1 = 10;
  invalidate();
  plot_prepare(200, 200);                            /* square: the diagonal */
  for (c = 0; c < 200; c++) {
    int want = 199 - c;
    CHECK(G.lo[0][c] - 1 <= want && G.hi[0][c] - 1 >= want);
    if (c && G.lo[0][c] - 1 > G.hi[0][c - 1]) gaps++;
  }
  CHECK_EQ(gaps, 0);
  CHECK_EQ(G.ax, 100);
  CHECK_EQ(G.ay, 100);
}

void test_calc_tan_is_not_joined_across_its_asymptote(void) {
  int c, tall = 0;
  boot();
  calc("y=tan(x)");
  invalidate();
  plot_prepare(240, 126);
  for (c = 0; c < 240; c++)
    if (G.hi[0][c] - G.lo[0][c] > 100) tall++;
  CHECK_EQ(tall, 0);
}

void test_calc_paints_inside_its_rectangle(void) {
  boot();
  paint();
  calc("y=x^2");
  calc("2+2");
  paint();
  INST.key(INST.state, 0x09);
  paint();
  INST.key(INST.state, 't');
  CHECK(T.trace);
  INST.key(INST.state, CAPP_KEY_RIGHT);
  paint();
  INST.key(INST.state, '+');
  INST.key(INST.state, 'f');
  paint();
  CHECK_EQ(OUT_OF_BOUNDS, 0);
}

/* ---- paper ---- */

void test_calc_prints_the_roll(void) {
  boot();
  type("6*7\r");
  INST.action(INST.state, ACT_PRINT);
  CHECK_EQ(PRINTS, 1);
  CHECK(strstr(PRINTED, "# Calc\n6*7\n## = 42\n") != NULL);
}

void test_calc_prints_a_graph_as_a_picture(void) {
  const char *p;
  int bits = 0, rows = 0;
  PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  boot();
  calc("y=x^2-3");
  calc("y=sin(x)");
  paint();                                           /* so it knows the screen */
  INST.key(INST.state, 0x09);
  INST.action(INST.state, ACT_PRINT);
  CHECK_EQ(PRINTS, 1);
  CHECK(strstr(PRINTED, "# Graph\n%%") != NULL);
  CHECK(strstr(PRINTED, "y1 = x^2-3  (solid)") != NULL);
  CHECK(strstr(PRINTED, "y2 = sin(x)  (dashed)") != NULL);
  CHECK((int)strlen(PRINTED) < (int)sizeof U.doc);

  /* Every picture line is one the renderer takes as a picture: the rows
   * they add up to are exactly the plot's height. */
  for (p = PRINTED; *p; ) {
    const char *nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    if (len >= 2 && p[0] == '%' && p[1] == '%') {
      char one[256];
      int n = len < 255 ? len : 255;
      memcpy(one, p, (size_t)n);
      one[n] = 0;
      rows += printdoc_count_rows(one);
      bits++;
    }
    p += len + (nl ? 1 : 0);
  }
  CHECK_EQ(rows, PH);
  CHECK(bits < PH);                                  /* some rows were merged */

  /* and it has ink, inside the paper */
  printdoc_begin(&d, PRINTED);
  rows = 0;
  while (printdoc_next_row(&d, row)) {
    int x, n = 0;
    for (x = 0; x < PRINT_WIDTH; x++) n += (row[x >> 3] >> (x & 7)) & 1;
    if (n) rows++;
  }
  CHECK(rows > PH);
}

void test_calc_the_window_survives_a_print(void) {
  boot();
  calc("y=x");
  paint();
  INST.key(INST.state, 0x09);
  T.x0 = -3; T.x1 = 7; T.y0 = -1; T.y1 = 2;
  INST.action(INST.state, ACT_PRINT);
  CHECK(T.x0 == -3.0f); CHECK(T.x1 == 7.0f);
  CHECK(T.y0 == -1.0f); CHECK(T.y1 == 2.0f);
}

void test_calc_eval_command(void) {
  char out[64];
  const char *args[1];
  boot();
  args[0] = "2^10";
  CHECK_EQ(INST.command(INST.state, ACT_EVAL, 1, args, out, sizeof out), 0);
  CHECK_STR(out, "= 1024");
  args[0] = "(";
  CHECK(INST.command(INST.state, ACT_EVAL, 1, args, out, sizeof out) < 0);
}
