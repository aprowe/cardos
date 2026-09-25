/* Forklang, the language Forklift's robots run: parsing, evaluating, and
 * saying clearly what is wrong -- the error line is most of the interface a
 * player has, so the messages are checked as much as the answers. */

#include <string.h>

#include "tinytest.h"
#include "apps/forklang.h"

static FlProg PROG;
static FlRun  RUN;
static FlWorld W;
static FlAction ACT;

/* A robot at (3, 2) holding nothing, an order for red then blue, the dock
 * at (9, 3), bays for red, blue and green down the left. */
static void world(void) {
  int k;
  memset(&W, 0, sizeof W);
  W.x = 3; W.y = 2; W.cap = 2;
  W.order[0] = 0; W.order[1] = 1; W.norder = 2;
  W.ship_x = 9; W.ship_y = 3;
  for (k = 0; k < FL_KINDS; k++) { W.src_x[k] = -1; W.src_y[k] = -1; }
  W.src_x[0] = 0; W.src_y[0] = 1;
  W.src_x[1] = 0; W.src_y[1] = 3;
  W.src_x[2] = 0; W.src_y[2] = 5;
}

static int run(const char *src) {
  world();
  if (fl_parse(&PROG, src) != 0) return -2;
  return fl_run(&RUN, &PROG, &W, &ACT);
}

/* bot = go (N, 0): the number, read back out of the action. */
static int number(const char *expr) {
  char src[256];
  strcpy(src, "bot = go (");
  strcat(src, expr);
  strcat(src, ", 0)");
  if (run(src) != 0) return -99999;
  return ACT.x;
}

static int truth(const char *expr) {
  char src[256];
  strcpy(src, "bot = (");
  strcat(src, expr);
  strcat(src, ") ? go (1, 0) : go (0, 0)");
  if (run(src) != 0) return -1;
  return ACT.x;
}

void test_forklang_the_default_program_fetches_and_ships(void) {
  const char *src =
    "# the bot calls this every step\n"
    "want = first order\n"
    "bot = empty holding\n"
    "  ? (at (src want) ? take want : go (src want))\n"
    "  : (at ship ? drop : go ship)\n";
  CHECK_EQ(0, run(src));
  CHECK_EQ(FL_ACT_GO, ACT.act);                 /* off to the red bay */
  CHECK_EQ(0, ACT.x);
  CHECK_EQ(1, ACT.y);

  W.x = 0; W.y = 1;                             /* at it */
  CHECK_EQ(0, fl_run(&RUN, &PROG, &W, &ACT));
  CHECK_EQ(FL_ACT_TAKE, ACT.act);
  CHECK_EQ(0, ACT.kind);

  W.held[0] = 0; W.nheld = 1;                   /* holding: to the dock */
  CHECK_EQ(0, fl_run(&RUN, &PROG, &W, &ACT));
  CHECK_EQ(FL_ACT_GO, ACT.act);
  CHECK_EQ(9, ACT.x);

  W.x = 9; W.y = 3;
  CHECK_EQ(0, fl_run(&RUN, &PROG, &W, &ACT));
  CHECK_EQ(FL_ACT_DROP, ACT.act);
}

void test_forklang_arithmetic_and_precedence(void) {
  CHECK_EQ(7, number("1 + 2 * 3"));
  CHECK_EQ(9, number("(1 + 2) * 3"));
  CHECK_EQ(2, number("7 / 3"));
  CHECK_EQ(1, number("7 % 3"));
  CHECK_EQ(-4, number("-4"));
  CHECK_EQ(3, number("abs (0 - 3)"));
  CHECK_EQ(2, number("min 2 5"));
  CHECK_EQ(5, number("max 2 5"));
}

void test_forklang_comparison_and_logic(void) {
  CHECK_EQ(1, truth("1 < 2"));
  CHECK_EQ(0, truth("2 <= 1"));
  CHECK_EQ(1, truth("3 >= 3"));
  CHECK_EQ(1, truth("1 != 2"));               /* != is not > */
  CHECK_EQ(0, truth("2 > 3"));
  CHECK_EQ(1, truth("red == red"));
  CHECK_EQ(0, truth("red == blue"));
  CHECK_EQ(1, truth("true && !false"));
  CHECK_EQ(1, truth("false || true"));
  CHECK_EQ(1, truth("[1, 2] == [1, 2]"));
  CHECK_EQ(0, truth("[1, 2] == [1]"));
  CHECK_EQ(1, truth("ship == (9, 3)"));
}

void test_forklang_and_or_short_circuit(void) {
  /* The right side would fail: first of an empty list. */
  CHECK_EQ(0, truth("false && first [] == 1"));
  CHECK_EQ(1, truth("true || first [] == 1"));
}

void test_forklang_functions_and_currying(void) {
  CHECK_EQ(0, run("twice x = x * 2\nbot = go (twice 5, 0)"));
  CHECK_EQ(10, ACT.x);
  CHECK_EQ(0, run("add a b = a + b\ninc = add 1\nbot = go (inc 4, 0)"));
  CHECK_EQ(5, ACT.x);
  CHECK_EQ(0, run("bot = go ((x -> x * x) 6, 0)"));
  CHECK_EQ(36, ACT.x);
}

void test_forklang_recursion(void) {
  CHECK_EQ(0, run("sum l = empty l ? 0 : first l + sum (rest l)\n"
                  "bot = go (sum [1, 2, 3, 4], 0)"));
  CHECK_EQ(10, ACT.x);
}

void test_forklang_list_builtins(void) {
  CHECK_EQ(3, number("len [4, 5, 6]"));
  CHECK_EQ(5, number("nth [4, 5, 6] 1"));
  CHECK_EQ(2, number("count [red, blue, red] red"));
  CHECK_EQ(1, truth("has order blue"));
  CHECK_EQ(0, truth("has order green"));
  CHECK_EQ(12, number("fold (a -> b -> a + b) 0 (map (x -> x * 2) [1, 2, 3])"));
  CHECK_EQ(2, number("len (filter (x -> x > 1) [1, 2, 3])"));
  CHECK_EQ(1, truth("empty holding"));
}

void test_forklang_world_values(void) {
  CHECK_EQ(3, number("col pos"));
  CHECK_EQ(2, number("row pos"));
  CHECK_EQ(2, number("cap"));
  CHECK_EQ(0, number("me"));
  CHECK_EQ(7, number("dist ship"));            /* 6 across, 1 down */
  CHECK_EQ(1, truth("at (3, 2)"));
  CHECK_EQ(1, truth("src blue == (0, 3)"));
}

void test_forklang_continuation_lines_and_comments(void) {
  CHECK_EQ(0, run("# a comment\n"
                  "\n"
                  "bot =\n"
                  "  # a comment inside\n"
                  "  go (1,\n"
                  "      2)   # and after\n"
                  "\n"));
  CHECK_EQ(1, ACT.x);
  CHECK_EQ(2, ACT.y);
}

void test_forklang_a_definition_can_use_a_later_one(void) {
  CHECK_EQ(0, run("bot = go target\ntarget = ship"));
  CHECK_EQ(9, ACT.x);
}

/* ---- errors: what they say, and on which line ---------------------------- */

static const char *parse_error(const char *src) {
  if (fl_parse(&PROG, src) == 0) return "";
  return PROG.err;
}

void test_forklang_parse_errors_name_the_problem_and_line(void) {
  CHECK(strstr(parse_error("bot = go ship\nwant = frist order"), "no such name: frist"));
  CHECK_EQ(2, PROG.err_line);
  CHECK(strstr(parse_error("bot = (1 + 2"), "expected )"));
  CHECK(strstr(parse_error("bot = a ? b"), "expected :"));
  CHECK(strstr(parse_error("want = 1"), "no bot"));
  CHECK(strstr(parse_error("bot = 1\nbot = 2"), "defined twice"));
  CHECK(strstr(parse_error("bot x = 1"), "no parameters"));
  CHECK(strstr(parse_error("ship = 1\nbot = 1"), "built in"));
  CHECK(strstr(parse_error("bot = go ship $"), "$"));
  CHECK(strstr(parse_error("bot = averyveryverylongname"), "11 letters"));
}

static const char *run_error(const char *src) {
  int r = run(src);
  if (r == -2) return PROG.err;
  if (r == 0) return "";
  return RUN.err;
}

void test_forklang_run_errors_are_errors_not_crashes(void) {
  CHECK(strstr(run_error("bot = 3"), "not an action"));
  CHECK(strstr(run_error("bot = go (1 / 0, 0)"), "divided by zero"));
  CHECK(strstr(run_error("bot = go (first [], 0)"), "empty list"));
  CHECK(strstr(run_error("bot = go red"), "go wants a place, got a kind"));
  CHECK(strstr(run_error("bot = take (1, 2)"), "take wants a kind, got a place"));
  CHECK(strstr(run_error("bot = go (src yellow)"), "no yellow bay"));
  CHECK(strstr(run_error("bot = 3 4"), "not a function"));
  CHECK(strstr(run_error("x = x + 1\nbot = go (x, 0)"), "needs itself"));
}

void test_forklang_runaway_programs_stop(void) {
  /* Infinite recursion: stopped by depth, not by the C stack. */
  CHECK(strstr(run_error("f x = f (x + 1)\nbot = go (f 0, 0)"), "too deep"));
  /* Wide rather than deep: stopped by fuel or memory. */
  {
    const char *e = run_error(
      "grow l = len l > 400 ? l : grow (map (x -> x) (l))\n"
      "bot = go (len (grow [1]), 0)");
    CHECK(e[0] != 0);
  }
}

void test_forklang_every_run_starts_fresh(void) {
  int i;
  CHECK_EQ(0, run("big = map (x -> x + 1) [1, 2, 3, 4, 5, 6, 7, 8]\n"
                  "bot = go (len big, 0)"));
  /* The arena is reset each step: a thousand steps do not run it out. */
  for (i = 0; i < 1000; i++) CHECK_EQ(0, fl_run(&RUN, &PROG, &W, &ACT));
  CHECK_EQ(8, ACT.x);
}

void test_forklang_the_error_line_is_where_it_went_wrong(void) {
  CHECK(run_error("bot = go (x, 0)\nx =\n  first\n    []")[0] != 0);
  CHECK(RUN.err_line >= 3);
}

void test_forklang_completion_lists_builtins_and_definitions(void) {
  int i, found_bot = 0, found_holding = 0;
  const char *n;
  fl_parse(&PROG, "want = first order\nbot = go ship");
  for (i = 0; (n = fl_name_at(&PROG, i)) != 0; i++) {
    if (!strcmp(n, "want")) found_bot = 1;
    if (!strcmp(n, "holding")) found_holding = 1;
  }
  CHECK(found_bot);
  CHECK(found_holding);
}
