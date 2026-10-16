/* The Python app's REPL on the PC: apps/python/python.c and MicroPython's
 * core built for the host with gcc, driven by keystrokes through the fake
 * CardApi (test/fakeapi.c). Not in the MSVC host suite -- MicroPython wants
 * gcc -- so tools/python_host_test.sh builds and runs it on Linux (the
 * droplet, from Windows).
 */
#include <stdio.h>
#include <string.h>

#include "test/fakeapi.h"
#include "apps/python/python.c"

static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
  printf(__VA_ARGS__); printf("\n"); } } while (0)

static void type(const char *s) {
  for (; *s; s++) app_key(0, (uint8_t)(*s == '\n' ? CAPP_KEY_ENTER : *s));
}

/* The log's last line, and whether any of the last n lines has `want`. */
static const char *last(void) { return L.n ? L.line[L.n - 1] : ""; }
static int seen(const char *want, int n) {
  int i;
  for (i = L.n - 1; i >= 0 && i >= L.n - n; i--)
    if (strstr(L.line[i], want)) return L.tag[i] + 1;
  return 0;
}

/* key_pending: up for a while, then held down -- a key pressed mid-run. */
static int pending_calls, pending_after;
static int held_later(void) { return ++pending_calls > pending_after; }
static int never(void) { return 0; }

int main(void) {
  static CardApi A;
  fakeapi_init(&A);
  fakeapi_big_room = 96 * 1024;
  capp_main(&A, 1, 0);

  CHECK(seen("MicroPython 1.24.1", 3), "banner: %s", L.line[0]);
  CHECK(!strcmp(last(), "2"), "print(1 + 1) -> %s", last());

  type("x = 6 * 7\nx\n");
  CHECK(!strcmp(last(), "42"), "x -> %s", last());
  CHECK(seen(">>> x = 6 * 7", 3) == T_IN + 1, "the line is echoed as typed");

  type("def f(n):\n");
  CHECK(src_n > 0, "a def waits for its block");
  type("\treturn n * 2\n\nf(21)\n");
  CHECK(!strcmp(last(), "42"), "f(21) -> %s", last());
  CHECK(seen("... ", 6), "continuation lines echo with ...");

  type("2 ** 100\n");
  CHECK(!strcmp(last(), "1267650600228229401496703205376"), "2**100 -> %s", last());
  type("1 / 4\n");
  CHECK(!strcmp(last(), "0.25"), "1/4 -> %s", last());
  type("'%s-%d' % ('cardos', 3)\n");
  CHECK(!strcmp(last(), "'cardos-3'"), "%% -> %s", last());
  type("[c for c in 'jar' if c != 'a']\n");
  CHECK(!strcmp(last(), "['j', 'r']"), "list comp -> %s", last());

  type("1 / 0\n");
  CHECK(seen("ZeroDivisionError", 2) == T_ERR + 1, "an error, in red: %s", last());

  type("for i in range(3):\n\tprint('row', i)\n\n");
  CHECK(!strcmp(last(), "row 2") && seen("row 0", 3), "a loop prints rows: %s", last());

  /* garbage: far more than the heap, made and dropped */
  type("t = 0\nfor i in range(3000):\n\tt += len([i] * 50)\n\nt\n");
  CHECK(!strcmp(last(), "150000"), "a collecting loop -> %s", last());

  /* a runaway loop, stopped by a key held down once it is running */
  A.key_pending = held_later;
  pending_calls = 0;
  pending_after = 5;
  type("while True:\n\tpass\n\n");
  CHECK(seen("KeyboardInterrupt", 2), "a key stops it: %s", last());
  A.key_pending = never;

  /* tab: a unique name completes, an attribute too, several are listed;
   * at the start of a line it is still an indent */
  type("cardos_value = 7\ncardos_v\t");
  CHECK(!strcmp(in, "cardos_value"), "completed: '%s'", in);
  type("\n");
  CHECK(!strcmp(last(), "7"), "and runs -> %s", last());
  type("word = 'x'\nword.upp\t");
  CHECK(!strcmp(in, "word.upper"), "an attribute: '%s'", in);
  in_n = 0; in[0] = 0;
  type("pri\t");
  CHECK(!strcmp(in, "print"), "a builtin: '%s'", in);
  in_n = 0; in[0] = 0;
  type("whi\t");
  CHECK(!strcmp(in, "while"), "a keyword: '%s'", in);
  in_n = 0; in[0] = 0;
  type("is\t");
  CHECK(!strcmp(in, "is"), "two candidates leave the line: '%s'", in);
  CHECK(seen("isinstance", 2) && seen("issubclass", 2), "and are listed: %s", last());
  in_n = 0; in[0] = 0;
  type("\t");
  CHECK(!strcmp(in, "    "), "tab at the start indents: '%s'", in);
  in_n = 0; in[0] = 0;

  /* history: up walks back, down comes forward to what was being typed */
  type("sum([1, 2])\nlen('abc')\nhalf");
  app_key(0, CAPP_KEY_UP);
  CHECK(!strcmp(in, "len('abc')"), "up: '%s'", in);
  app_key(0, CAPP_KEY_UP);
  CHECK(!strcmp(in, "sum([1, 2])"), "up again: '%s'", in);
  app_key(0, CAPP_KEY_DOWN);
  app_key(0, CAPP_KEY_DOWN);
  CHECK(!strcmp(in, "half"), "down to the draft: '%s'", in);
  in_n = 0; in[0] = 0;
  app_key(0, CAPP_KEY_UP);
  type("\n");
  CHECK(!strcmp(last(), "3"), "a line from history runs -> %s", last());
  app_key(0, CTRL_U);
  CHECK(L.scroll == 6, "ctrl-u scrolls the log: %d", L.scroll);
  app_key(0, CTRL_D);
  CHECK(L.scroll == 0, "ctrl-d back: %d", L.scroll);

  /* and the REPL goes on */
  type("open('x')\n");
  CHECK(seen("OSError", 2), "no files yet: %s", last());
  type("sum(range(10))\n");
  CHECK(!strcmp(last(), "45"), "still running -> %s", last());

  printf("%s: %d lines in the log, %d failure(s)\n", fails ? "FAILED" : "ok", L.n, fails);
  return fails != 0;
}
