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

/* A clock that moves: time.sleep must end. */
static uint32_t clock_ms;
static uint32_t moving_ticks(void) { return clock_ms += 3; }

/* The app opened as `python PATH`, the way Vim's :run opens it. */
static void start_with(CardApi *A, const char *path) {
  char *argv[2];
  argv[0] = (char *)"python";
  argv[1] = (char *)path;
  capp_main(A, path ? 2 : 1, argv);
  if (path) {                    /* the REPL first; the file a moment later, from tick */
    CHECK(pending[0] && !seen("-- running", 3), "not run at start: %s", last());
    CHECK(!app_tick(0, A->ticks_ms()), "nor before its moment");
    CHECK(app_tick(0, A->ticks_ms() + RUN_DELAY_MS + 1), "then it runs");
    CHECK(!pending[0], "once");
  }
}

static int open_files(void) {
  int i, n = 0;
  for (i = 0; i < FILES_MAX; i++) n += files[i] >= 0;
  return n;
}

static void files_on_the_card(CardApi *A) {
  const char *got;
  fakefs_mem(A);
  fakefs_put("/home/demo.py",
             "import helper\n"
             "from tool import twice\n"
             "total = helper.add(2, 3)\n"
             "print('total', total)\n"
             "print(twice(21))\n");
  fakefs_put("/home/helper.py", "def add(a, b):\n    return a + b\n");
  fakefs_put("/lib/tool.py", "def twice(n):\n    return n * 2\n");

  /* python FILE: it runs, imports from its folder and /lib, says so */
  start_with(A, "/home/demo.py");
  CHECK(seen("total 5", 6) == T_OUT + 1, "the script's output: %s", last());
  CHECK(seen("42", 6), "a module from /lib: %s", last());
  CHECK(seen("-- ran demo.py in", 1) == T_NOTE + 1, "what ran and how long: %s", last());
  CHECK(!seen("MicroPython", 1) || L.n > 1, "the banner first");
  /* and stays, with the script's globals */
  type("total * 10\n");
  CHECK(!strcmp(last(), "50"), "globals after the script -> %s", last());
  type("__file__\n");
  CHECK(!strcmp(last(), "'/home/demo.py'"), "__file__ -> %s", last());
  type("import os, sys\nos.getcwd()\n");
  CHECK(!strcmp(last(), "'/home'"), "cwd is the script's folder -> %s", last());
  type("sys.argv\n");
  CHECK(!strcmp(last(), "['/home/demo.py']"), "sys.argv -> %s", last());
  type("sys.path\n");
  CHECK(!strcmp(last(), "['', '/lib']"), "sys.path -> %s", last());

  /* open(): w, with, print(file=), a, r, readline(s), seek, tell, iteration, rb */
  type("with open('out.txt', 'w') as f:\n\tf.write('one\\n')\n\tprint('two', file=f)\n\n");
  got = fakefs_get("/home/out.txt");
  CHECK(got && !strcmp(got, "one\ntwo\n"), "written: '%s'", got ? got : "(none)");
  CHECK(open_files() == 0, "with closes it: %d open", open_files());
  type("f = open('out.txt', 'a')\nf.write('three\\n')\nf.close()\n");
  got = fakefs_get("/home/out.txt");
  CHECK(got && !strcmp(got, "one\ntwo\nthree\n"), "appended: '%s'", got ? got : "(none)");
  type("open('new.txt', 'a').write('x')\n");
  got = fakefs_get("/home/new.txt");
  CHECK(got && !strcmp(got, "x"), "append makes a file: '%s'", got ? got : "(none)");
  type("open('out.txt').read()\n");
  CHECK(!strcmp(last(), "'one\\ntwo\\nthree\\n'"), "read() -> %s", last());
  type("f = open('/home/out.txt')\nf.readline()\n");
  CHECK(!strcmp(last(), "'one\\n'"), "readline() -> %s", last());
  type("f.readlines()\n");
  CHECK(!strcmp(last(), "['two\\n', 'three\\n']"), "readlines() -> %s", last());
  type("f.seek(4)\nf.read(3)\n");
  CHECK(!strcmp(last(), "'two'"), "seek and read(n) -> %s", last());
  type("f.tell()\n");
  CHECK(!strcmp(last(), "7"), "tell() -> %s", last());
  type("f.close()\n[l.strip() for l in open('out.txt')]\n");
  CHECK(!strcmp(last(), "['one', 'two', 'three']"), "iteration -> %s", last());
  type("open('out.txt', 'rb').read(3)\n");
  CHECK(!strcmp(last(), "b'one'"), "rb -> %s", last());
  type("g = open('out.txt', 'r+')\ng.write('ONE')\ng.seek(0)\ng.readline()\n");
  CHECK(!strcmp(last(), "'ONE\\n'"), "r+ -> %s", last());
  type("g.close()\nopen('nope.txt')\n");
  CHECK(seen("OSError", 2) == T_ERR + 1, "a missing file: %s", last());
  type("f.read()\n");
  CHECK(seen("OSError", 2), "reading a closed file: %s", last());

  /* files nothing refers to are closed when they run out */
  type("for i in range(12):\n\tn = len(open('helper.py').read())\n\nn\n");
  CHECK(!strcmp(last(), "32"), "twelve opens, never closed -> %s", last());

  /* os */
  type("'out.txt' in os.listdir(), 'helper.py' in os.listdir('/home')\n");
  CHECK(!strcmp(last(), "(True, True)"), "listdir -> %s", last());
  type("os.stat('out.txt')[6], os.stat('/lib')[0] & 0x4000\n");
  CHECK(!strcmp(last(), "(14, 16384)"), "stat -> %s", last());
  type("os.mkdir('sub')\nos.rename('new.txt', 'sub/new.txt')\nos.listdir('sub')\n");
  CHECK(!strcmp(last(), "['new.txt']"), "mkdir, rename -> %s", last());
  type("os.chdir('sub')\nopen('../out.txt').readline()\n");
  CHECK(!strcmp(last(), "'ONE\\n'"), "chdir and .. -> %s", last());
  type("os.remove('new.txt')\nos.listdir()\n");
  CHECK(!strcmp(last(), "[]") && !fakefs_exists("/home/sub/new.txt"), "remove -> %s", last());

  /* time: a sleep ends, and a held key would stop it */
  A->ticks_ms = moving_ticks;
  type("import time\ntime.sleep_ms(30)\ntime.ticks_diff(5, 3)\n");
  CHECK(!strcmp(last(), "2"), "time -> %s", last());

  /* exit() leaves, closing what is open; ctrl-d on an empty line too */
  fakeapi_left = 0;
  type("import gc\ngc.collect()\n");
  CHECK(open_files() == 0, "a collection closes the forgotten: %d open", open_files());
  type("h = open('/home/out.txt')\n");
  CHECK(open_files() == 1, "one open: %d", open_files());
  type("exit()\n");
  CHECK(fakeapi_left == 1, "exit() leaves: %d", fakeapi_left);
  CHECK(open_files() == 0, "and closes the file: %d", open_files());
  app_key(0, CTRL_D);
  CHECK(fakeapi_left == 2, "ctrl-d on an empty line leaves: %d", fakeapi_left);
  type("1");
  app_key(0, CTRL_D);
  CHECK(fakeapi_left == 2, "not with something typed: %d", fakeapi_left);
  in_n = 0; in[0] = 0;
  type("import sys\nsys.exit(0)\n");
  CHECK(fakeapi_left == 3, "sys.exit() leaves: %d", fakeapi_left);
  type("raise SystemExit\n");
  CHECK(fakeapi_left == 4, "SystemExit leaves: %d", fakeapi_left);

  /* a traceback names the file and the line; the globals are still there */
  fakefs_put("/home/bad.py", "x = 1\ny = 2\nz = x / 0\n");
  start_with(A, "/home/bad.py");
  CHECK(seen("File \"/home/bad.py\", line 3", 6), "the file and line: %s", last());
  CHECK(seen("ZeroDivisionError", 3) == T_ERR + 1, "the error: %s", last());
  CHECK(seen("with an error", 1) == T_NOTE + 1, "said: %s", last());
  type("y\n");
  CHECK(!strcmp(last(), "2"), "what ran before the error -> %s", last());

  /* SystemExit inside a script ends the script, not the app */
  fakeapi_left = 0;
  fakefs_put("/home/quits.py", "import sys\nprint('before')\nsys.exit(3)\nprint('later')\n");
  start_with(A, "/home/quits.py");
  CHECK(seen("before", 4) && !seen("later", 4), "the script stops: %s", last());
  CHECK(seen("quits.py exited (3)", 1) == T_NOTE + 1, "and says so: %s", last());
  CHECK(fakeapi_left == 0, "the app stays: %d", fakeapi_left);
  type("1 + 1\n");
  CHECK(!strcmp(last(), "2"), "the prompt after -> %s", last());

  /* a key held down stops a script, and lands at the prompt */
  fakefs_put("/home/spin.py", "n = 0\nwhile True:\n    n += 1\n");
  A->key_pending = held_later;
  pending_calls = 0;
  pending_after = 5;
  start_with(A, "/home/spin.py");
  A->key_pending = never;
  CHECK(seen("KeyboardInterrupt", 3) == T_ERR + 1, "stopped: %s", last());
  CHECK(seen("spin.py\", line", 4), "where: %s", last());
  type("n > 0\n");
  CHECK(!strcmp(last(), "True"), "its globals -> %s", last());
  CHECK(fakeapi_left == 0, "still here: %d", fakeapi_left);

  /* a file that is not there */
  start_with(A, "/home/none.py");
  CHECK(seen("no such file: /home/none.py", 1) == T_ERR + 1, "missing: %s", last());

  /* a module that does not import */
  fakefs_put("/home/imp.py", "import nothing_here\n");
  start_with(A, "/home/imp.py");
  CHECK(seen("ImportError", 3), "no module: %s", last());

  /* input(): typed keys from key_wait; the prompt and the answer stay in
   * the log as one line, and the script carries on with it */
  A->ticks_ms = moving_ticks;
  fakefs_put("/home/ask.py", "name = input('Name? ')\nprint('hi', name.upper())\n");
  fakeapi_keys = "bp\bob\r";                     /* a slip, a backspace, Enter */
  start_with(A, "/home/ask.py");
  CHECK(seen("Name? bob", 3), "prompt and answer: %s", last());
  CHECK(seen("hi BOB", 3), "the script had it: %s", last());
  CHECK(!inputting, "input is over");
  type("name\n");
  CHECK(!strcmp(last(), "'bob'"), "and it is kept -> %s", last());
  /* at the prompt too; Esc is KeyboardInterrupt, ctrl-d on nothing EOFError */
  fakeapi_keys = "\x1b";
  type("input('? ')\n");
  CHECK(seen("KeyboardInterrupt", 2), "esc: %s", last());
  fakeapi_keys = "\x04";
  type("input()\n");
  CHECK(seen("EOFError", 2), "ctrl-d: %s", last());
  fakeapi_keys = "x\r";
  type("input() * 2\n");
  CHECK(!strcmp(last(), "'xx'"), "an expression -> %s", last());

  /* live: a script's prints are drawn as it goes, not only at its end */
  fakefs_put("/home/slow.py", "import time\nfor i in range(4):\n    print('tick', i)\n"
             "    time.sleep_ms(150)\n");
  fakeapi_keys = 0;
  fakeapi_repaints = 0;
  start_with(A, "/home/slow.py");
  CHECK(seen("tick 3", 3), "it ran: %s", last());
  CHECK(fakeapi_repaints >= 2, "drawn while it ran: %d", fakeapi_repaints);
  fakeapi_repaints = 0;
  type("1 + 1\n");                               /* a quick line draws nothing extra */
  CHECK(fakeapi_repaints == 0, "no live draws for a quick line: %d", fakeapi_repaints);
  /* a key typed during a sleep stops it */
  fakeapi_keys = "k";
  type("import time\ntime.sleep(5)\n");
  CHECK(seen("KeyboardInterrupt", 2), "a key during sleep: %s", last());
  fakeapi_keys = 0;
}

int main(void) {
  static CardApi A;
  fakeapi_init(&A);
  fakeapi_big_room = 96 * 1024;
  capp_main(&A, 1, 0);

  CHECK(seen("MicroPython 1.24.1", 3), "banner: %s", L.line[0]);
  CHECK(seen("MicroPython", 1), "opened plain, the banner is all: %s", last());
  CHECK(!app_tick(0, 99999), "and no file waits");

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
  /* ctrl-b / ctrl-f scroll; scrolling to the bottom never leaves (ctrl-d
   * used to do both); ctrl-u clears the line */
  {
    int left = fakeapi_left;
    app_key(0, CTRL_B);
    CHECK(L.scroll == 6, "ctrl-b scrolls the log back: %d", L.scroll);
    app_key(0, CTRL_F);
    app_key(0, CTRL_F);
    CHECK(L.scroll == 0, "ctrl-f forward, and stops at the bottom: %d", L.scroll);
    CHECK(fakeapi_left == left, "scrolling never leaves: %d", fakeapi_left);
    type("half a line");
    app_key(0, CTRL_U);
    CHECK(in_n == 0, "ctrl-u clears the line: '%s'", in);
    CHECK(fakeapi_left == left, "nor does clearing: %d", fakeapi_left);
  }

  /* and the REPL goes on */
  type("open('x')\n");
  CHECK(seen("OSError", 2), "no card, no file: %s", last());
  type("sum(range(10))\n");
  CHECK(!strcmp(last(), "45"), "still running -> %s", last());

  files_on_the_card(&A);

  printf("%s: %d lines in the log, %d failure(s)\n", fails ? "FAILED" : "ok", L.n, fails);
  return fails != 0;
}
