/* Vim's core (apps/vim/vimcore.h) driven a key at a time, as the app does:
 * the buffer, the motions, the operators, undo and the dot, search, the ex
 * commands, the syntax states and completion. Files go to fakeapi's
 * in-memory card. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"
#include "apps/vim/vimcore.h"
#include "apps/vim/vimtree.h"

static Vim V;
static char T[16384], U[4096], R[2048];
static CardApi A;
static char OUT[16384];

static void setup_ex(const char *text, int tcap, int ucap) {
  fakeapi_init(&A);
  fakefs_mem(&A);
  vapi = &A;
  memset(&V, 0, sizeof V);
  vi_init(&V, T, tcap, U, ucap, R, sizeof R);
  vb_insert(&V.t, 0, text, (int)strlen(text));
  vi_layout(&V);
}
static void setup(const char *text) { setup_ex(text, sizeof T, sizeof U); }

static const char *txt(void) {
  int n = vb_len(&V.t);
  vb_get(&V.t, 0, n, OUT);
  OUT[n] = 0;
  return OUT;
}
static int line(void) { return vi_line(&V); }
static int col(void) { return vi_col(&V); }
static void k(const char *s) { vi_keys(&V, s); }
static void key(int c) { vi_key(&V, c); }
#define TEXT_IS(s) CHECK(strcmp(txt(), s) == 0)
#define AT(l, c) do { CHECK_EQ(line(), l); CHECK_EQ(col(), c); } while (0)

/* ------------------------------------------------------------- buffer --- */

void test_vim_buf_insert_delete_and_lines(void) {
  static char mem[64];
  VBuf b;
  vb_init(&b, mem, sizeof mem);
  CHECK_EQ(vb_insert(&b, 0, "abc\ndef\n\nxyz", 12), 0);
  CHECK_EQ(vb_len(&b), 12);
  CHECK_EQ(vb_lines(&b), 4);
  CHECK_EQ(vb_line_start(&b, 1), 4);
  CHECK_EQ(vb_line_start(&b, 3), 9);
  CHECK_EQ(vb_line_end(&b, 2), 8);
  CHECK_EQ(vb_line_len(&b, 3), 3);
  CHECK_EQ(vb_line_of(&b, 10), 3);
  CHECK_EQ(vb_line_of(&b, 0), 0);
  vb_delete(&b, 3, 1);                     /* join the first two lines */
  CHECK_EQ(vb_lines(&b), 3);
  CHECK_EQ(vb_line_start(&b, 2), 8);
  CHECK_EQ(vb_insert(&b, 0, mem, 60), -1); /* no room: nothing changes */
  CHECK_EQ(vb_len(&b), 11);
}

/* The anchor stays true through edits before and after it: every answer is
 * checked against a count from the top. */
void test_vim_buf_anchor_matches_a_plain_count(void) {
  static char mem[4096];
  VBuf b;
  int i, round;
  unsigned seed = 7;
  vb_init(&b, mem, sizeof mem);
  for (round = 0; round < 400; round++) {
    int len = vb_len(&b), p, L, q, n = 0, s = 0;
    seed = seed * 1103515245u + 12345u;
    p = len ? (int)(seed >> 8) % (len + 1) : 0;
    if ((seed >> 3) % 3 && len < 3000) vb_insert(&b, p, (seed >> 5) & 1 ? "ab\ncd" : "\n", (seed >> 5) & 1 ? 5 : 1);
    else vb_delete(&b, p, (int)(seed >> 12) % 4);
    len = vb_len(&b);
    q = len ? (int)(seed >> 16) % len : 0;
    for (i = 0; i < q; i++) if (vb_at(&b, i) == '\n') { n++; s = i + 1; }
    CHECK_EQ(vb_line_of(&b, q), n);
    CHECK_EQ(vb_line_start(&b, n), s);
    L = (int)(seed >> 20) % (b.nl + 1);
    for (i = 0, n = 0, s = 0; i < len && n < L; i++) if (vb_at(&b, i) == '\n') { n++; s = i + 1; }
    CHECK_EQ(vb_line_start(&b, L), s);
  }
}

/* ------------------------------------------------------------ motions --- */

void test_vim_motion_hjkl_and_arrows_keep_the_column(void) {
  setup("hello world\nab\nthe third line");
  k("llll");
  AT(0, 4);
  k("j");
  AT(1, 1);                          /* the line is short: its last char */
  k("j");
  AT(2, 4);                          /* and the column comes back */
  k("kk");
  AT(0, 4);
  key(VK_DOWN);
  AT(1, 1);
  k("h");
  AT(1, 0);
  k("h");
  AT(1, 0);                          /* cannot go past the start */
  k("$");
  AT(1, 1);
  k("j");
  AT(2, 13);                         /* $ sticks to the end */
  k("0");
  AT(2, 0);
  k("10l");
  AT(2, 10);
  k("100l");
  AT(2, 13);
}

void test_vim_motion_words(void) {
  setup("int foo(bar, baz);\n\n  next_line x");
  k("w");  AT(0, 4);
  k("w");  AT(0, 7);                 /* ( is a word of punctuation */
  k("w");  AT(0, 8);
  k("2w"); AT(0, 13);
  k("w");  AT(0, 16);
  k("w");  AT(1, 0);                 /* an empty line is a word */
  k("w");  AT(2, 2);
  k("e");  AT(2, 10);
  k("b");  AT(2, 2);
  k("b");  AT(1, 0);
  k("b");  AT(0, 16);
  k("gg");
  k("W");  AT(0, 4);
  k("W");  AT(0, 13);                /* foo(bar, is one WORD */
  k("E");  AT(0, 17);
  k("B");  AT(0, 13);
  k("$b"); AT(0, 16);
}

void test_vim_motion_lines_and_screen(void) {
  int i;
  char big[400];
  big[0] = 0;
  for (i = 0; i < 40; i++) strcat(big, i % 2 ? "  odd\n" : "even\n");
  strcat(big, "last");
  setup(big);
  k("G");   AT(40, 0);
  k("gg");  AT(0, 0);
  k("5G");  AT(4, 0);
  k("3gg"); AT(2, 0);
  k("j^");  AT(3, 2);
  k("j");   AT(4, 2);                /* want is the column from ^ */
  k("gg");
  V.rows = 10;
  k("L");   AT(9, 2);
  k("H");   AT(0, 0);
  k("M");   AT(4, 0);
  key(VCTRL('d'));
  CHECK_EQ(line(), 9);
  CHECK_EQ(V.top, 5);
  key(VCTRL('u'));
  CHECK_EQ(line(), 4);
  CHECK_EQ(V.top, 0);
  k("+");   AT(5, 2);
  k("1-");  AT(4, 0);                /* - alone is the file tree */
  k("50%"); CHECK_EQ(line(), 20);
}

void test_vim_motion_find_till_and_repeat(void) {
  setup("a,b,c,d(e)f");
  k("f,");  AT(0, 1);
  k(";");   AT(0, 3);
  k(",");   AT(0, 1);
  k("2f,"); AT(0, 5);
  k("F,");  AT(0, 3);
  k("0t,"); AT(0, 0);
  k(";");   AT(0, 2);                /* t; steps past the one it stopped at */
  k("$T,"); AT(0, 6);
  k("fz");  AT(0, 6);                /* not there: stays */
  k("f(%"); AT(0, 9);
  k("%");   AT(0, 7);
}

void test_vim_motion_paragraphs(void) {
  setup("a\nb\n\nc\nd\n\ne");
  k("}");   AT(2, 0);
  k("}");   AT(5, 0);
  k("}");   AT(6, 0);
  k("{");   AT(5, 0);
  k("{{");  AT(0, 0);
}

/* ---------------------------------------------------------- operators --- */

void test_vim_op_delete_words_and_lines(void) {
  setup("one two three\nfour five\nsix");
  k("dw");      TEXT_IS("two three\nfour five\nsix");
  k("w");
  k("dw");      TEXT_IS("two \nfour five\nsix");      /* not the newline */
  k("0d$");     TEXT_IS("\nfour five\nsix");
  k("j");
  k("dd");      TEXT_IS("\nsix");
  AT(1, 0);
  k("dd");      TEXT_IS("");                          /* the last line goes */
  setup("a\nb\nc\nd\ne");
  k("j2dd");    TEXT_IS("a\nd\ne");
  AT(1, 0);
  k("dj");      TEXT_IS("a");
  setup("a\nb\nc\nd\ne");
  k("jdG");     TEXT_IS("a");
  setup("a\nb\nc\nd\ne");
  k("Gdgg");    TEXT_IS("");
  setup("a b c d e f");
  k("d3w");     TEXT_IS("d e f");
  k("2d2w");    TEXT_IS("");
}

void test_vim_op_x_r_tilde_and_join(void) {
  setup("abcdef\n   ghi\nj");
  k("x");       TEXT_IS("bcdef\n   ghi\nj");
  k("3x");      TEXT_IS("ef\n   ghi\nj");
  k("$X");      TEXT_IS("f\n   ghi\nj");
  k("rz");      TEXT_IS("z\n   ghi\nj");
  k("J");       TEXT_IS("z ghi\nj");
  AT(0, 1);
  k("J");       TEXT_IS("z ghi j");
  k("0~~");     TEXT_IS("Z ghi j");
  k("0w3rx");   TEXT_IS("Z xxx j");
  AT(0, 4);
  k("9rq");     TEXT_IS("Z xxx j");                   /* too few: nothing */
}

void test_vim_op_change(void) {
  setup("foo bar baz\nqux");
  k("cwFOO\x1b");      TEXT_IS("FOO bar baz\nqux");
  AT(0, 2);
  k("wcwx\x1b");       TEXT_IS("FOO x baz\nqux");
  k("C!\x1b");         TEXT_IS("FOO !\nqux");
  k("jccnew\x1b");     TEXT_IS("FOO !\nnew");
  k("0sN\x1b");        TEXT_IS("FOO !\nNew");
  k("S\x1b");          TEXT_IS("FOO !\n");
  setup("  indented line");
  k("ccx\x1b");        TEXT_IS("  x");               /* cc keeps the indent */
  setup("a.b");
  k("cwX\x1b");        TEXT_IS("X.b");               /* cw on the last char of a word */
}

void test_vim_op_yank_and_put(void) {
  setup("one\ntwo\nthree");
  k("yyp");      TEXT_IS("one\none\ntwo\nthree");
  AT(1, 0);
  k("jyyP");     TEXT_IS("one\none\ntwo\ntwo\nthree");
  k("G");
  k("yyp");      TEXT_IS("one\none\ntwo\ntwo\nthree\nthree");
  setup("abc def");
  k("ywP");      TEXT_IS("abc abc def");
  AT(0, 3);
  k("$p");       TEXT_IS("abc abc defabc ");
  k("0xp");      TEXT_IS("bac abc defabc ");
  setup("x");
  k("yy3p");     TEXT_IS("x\nx\nx\nx");
  setup("ab");
  k("yl3p");     TEXT_IS("aaaab");
}

void test_vim_op_indent(void) {
  setup("a\n\nb\nc");
  k(">>");       TEXT_IS("  a\n\nb\nc");
  k(">2j");      TEXT_IS("    a\n\n  b\nc");          /* empty lines stay empty */
  k("<<");       TEXT_IS("  a\n\n  b\nc");
  k("3<<");      TEXT_IS("a\n\nb\nc");
  V.sw = 4;
  k("G>>");      TEXT_IS("a\n\nb\n    c");
}

void test_vim_insert_commands(void) {
  setup("bc\n  x");
  k("ia\x1b");   TEXT_IS("abc\n  x");
  k("a!\x1b");   TEXT_IS("a!bc\n  x");
  k("A;\x1b");   TEXT_IS("a!bc;\n  x");
  k("jI-\x1b");  TEXT_IS("a!bc;\n  -x");
  k("onext\x1b"); TEXT_IS("a!bc;\n  -x\n  next");      /* o keeps the indent */
  k("ggOtop\x1b"); TEXT_IS("top\na!bc;\n  -x\n  next");
  k("3ia\x1b");  TEXT_IS("toaaap\na!bc;\n  -x\n  next");
  setup("x");
  k("2oy\x1b");  TEXT_IS("x\ny\ny");
  setup("ab");
  k("A\x08\x08\x08Z\x1b"); TEXT_IS("Z");
}

void test_vim_insert_newline_indents(void) {
  setup("int f(void) {");
  V.lang = LANG_C;
  k("A\rreturn 0;\r}\x1b");
  TEXT_IS("int f(void) {\n  return 0;\n}");
  setup("def f():");
  V.lang = LANG_PY;
  k("A\rpass\x1b");
  TEXT_IS("def f():\n  pass");
}

/* ------------------------------------------------------- undo and dot --- */

void test_vim_undo_and_redo(void) {
  setup("abc def");
  k("x");        TEXT_IS("bc def");
  CHECK(vi_modified(&V));
  k("u");        TEXT_IS("abc def");
  CHECK(!vi_modified(&V));
  key(VCTRL('r')); TEXT_IS("bc def");
  k("u");
  k("Ahello world\x1b"); TEXT_IS("abc defhello world");
  k("u");        TEXT_IS("abc def");                  /* one insert, one step */
  AT(0, 6);                                           /* where the A began */
  k("0dwdw");    TEXT_IS("");
  k("u");        TEXT_IS("def");
  k("u");        TEXT_IS("abc def");
  k("u");        TEXT_IS("abc def");
  CHECK(strstr(V.msg, "oldest") != NULL);
  k("2\x12");    TEXT_IS("");
  k("x");                                             /* ends the redo */
  key(VCTRL('r'));
  CHECK(strstr(V.msg, "newest") != NULL);
}

void test_vim_undo_cursor_and_lines(void) {
  setup("one\ntwo\nthree");
  k("jdd");      TEXT_IS("one\nthree");
  k("u");        TEXT_IS("one\ntwo\nthree");
  AT(1, 0);
  k("ggcwONE\x1b"); TEXT_IS("ONE\ntwo\nthree");
  k("u");        TEXT_IS("one\ntwo\nthree");
  AT(0, 0);
}

/* A small log: the oldest steps go to make room; a change bigger than the
 * whole log empties it and says so, and the buffer is still right. */
void test_vim_undo_log_overflow(void) {
  int i;
  setup_ex("0123456789012345678901234567890123456789", sizeof T, 128);
  for (i = 0; i < 10; i++) k("x");
  TEXT_IS("012345678901234567890123456789");
  k("100u");
  CHECK(strcmp(txt(), "0123456789012345678901234567890123456789") != 0);
  CHECK(strncmp(txt() + strlen(txt()) - 30, "012345678901234567890123456789", 30) == 0);
  CHECK(vi_modified(&V));
  setup_ex("0123456789012345678901234567890123456789\n"
           "0123456789012345678901234567890123456789\n"
           "0123456789012345678901234567890123456789\n"
           "0123456789012345678901234567890123456789", sizeof T, 128);
  k("dG");
  TEXT_IS("");
  CHECK(strstr(V.msg, "too big to undo") != NULL);
  k("u");
  TEXT_IS("");
  CHECK(vi_modified(&V));
}

void test_vim_dot_repeats_the_last_change(void) {
  setup("a b c d e f");
  k("dw.");      TEXT_IS("c d e f");
  k("2.");       TEXT_IS("e f");
  setup("x\nx\nx\nx");
  k("A!\x1bj.j.");  TEXT_IS("x!\nx!\nx!\nx");
  k("u");        TEXT_IS("x!\nx!\nx\nx");
  setup("one\ntwo\nthree");
  k("cwX\x1bj0.");  TEXT_IS("X\nX\nthree");
  k("j.");       TEXT_IS("X\nX\nX");
  setup("abcdef");
  k("x..");      TEXT_IS("def");
  k("lw");                                            /* motions do not replace it */
  k("0.");       TEXT_IS("ef");
}

/* ------------------------------------------------------------- visual --- */

void test_vim_visual_modes(void) {
  setup("hello world\nsecond\nthird");
  k("vlld");     TEXT_IS("lo world\nsecond\nthird");
  CHECK_EQ(V.mode, VM_NORMAL);
  k("Vjd");      TEXT_IS("third");
  setup("abc\ndef");
  k("vjy");      CHECK_EQ(V.rlen, 5);
  CHECK(memcmp(V.r, "abc\nd", 5) == 0);
  setup("a\nb\nc");
  k("Vj>");      TEXT_IS("  a\n  b\nc");
  setup("abcd");
  k("lvlrx");    TEXT_IS("axxd");
  setup("abcd");
  k("vl~");      TEXT_IS("ABcd");
  setup("a\nb\nc");
  k("VjJ");      TEXT_IS("a b\nc");
  setup("abc");
  k("vlcX\x1b"); TEXT_IS("Xc");
}

/* ------------------------------------------------------------- search --- */

void test_vim_search(void) {
  setup("foo bar\nbaz foo\nfoo");
  k("/foo\r");   AT(1, 4);
  k("n");        AT(2, 0);
  k("n");        AT(0, 0);
  CHECK(strstr(V.msg, "BOTTOM") != NULL);
  k("N");        AT(2, 0);
  k("?bar\r");   AT(0, 4);
  k("n");        AT(0, 4);                           /* the only one: wraps to itself */
  k("/nope\r");  AT(0, 4);
  CHECK(strstr(V.msg, "not found") != NULL);
  setup("foo foobar foo");
  k("*");        AT(0, 11);                          /* whole word: not foobar */
  k("#");        AT(0, 0);
  setup("a x b x c");
  k("/x\r");     AT(0, 2);
  k("0dn");      TEXT_IS("x b x c");
}

/* ----------------------------------------------------------------- ex --- */

void test_vim_ex_goto_set_and_errors(void) {
  setup("a\nb\nc\nd");
  k(":3\r");     AT(2, 0);
  k(":$\r");     AT(3, 0);
  k(":1\r");     AT(0, 0);
  k(":set nonu\r");
  CHECK_EQ(V.nu, 0);
  CHECK_EQ(vi_gutter(&V), 0);
  k(":set nu sw=4 noet\r");
  CHECK_EQ(V.nu, 1);
  CHECK_EQ(V.sw, 4);
  CHECK_EQ(V.et, 0);
  k(":frob\r");
  CHECK(strstr(V.msg, "E492") != NULL);
  k(":set bogus\r");
  CHECK(strstr(V.msg, "E518") != NULL);
  k(":2,3d\r");  TEXT_IS("a\nd");
}

void test_vim_ex_substitute(void) {
  setup("a a a\nb a\na/b");
  k(":s/a/X/\r");      TEXT_IS("X a a\nb a\na/b");
  k(":s/a/Y/g\r");     TEXT_IS("X Y Y\nb a\na/b");
  k("u");              TEXT_IS("X a a\nb a\na/b");    /* one step */
  k(":%s/a/Z/g\r");    TEXT_IS("X Z Z\nb Z\nZ/b");
  CHECK(strstr(V.msg, "4 substitutions on 3 lines") != NULL);
  k(":%s#Z/#q#\r");    TEXT_IS("X Z Z\nb Z\nqb");
  k(":%s/\\//-/\r");   TEXT_IS("X Z Z\nb Z\nqb");      /* nothing to do */
  CHECK(strstr(V.msg, "E486") != NULL);
  k(":1,2s/Z//g\r");   TEXT_IS("X  \nb \nqb");
  setup("Foo foo");
  k(":s/foo/x/gi\r");  TEXT_IS("x x");
}

void test_vim_ex_files(void) {
  setup("");
  fakefs_put("/home/a.c", "int x;\r\nint y;\r\n");
  vi_load(&V, "/home/a.c");
  TEXT_IS("int x;\nint y;");
  CHECK_EQ(V.dos, 1);
  CHECK_EQ(V.lang, LANG_C);
  k("x:w\r");
  CHECK(strcmp(fakefs_get("/home/a.c"), "nt x;\r\nint y;\r\n") == 0);
  CHECK(!vi_modified(&V));
  k("x:q\r");
  CHECK(strstr(V.msg, "E37") != NULL);
  CHECK_EQ(V.req, VREQ_NONE);
  k(":q!\r");
  CHECK_EQ(V.req, VREQ_QUIT);
  V.req = 0;
  k(":w b.txt\r");                                   /* relative to the file */
  CHECK(fakefs_get("/home/b.txt") != NULL);
  CHECK(strcmp(V.path, "/home/a.c") == 0);
  k(":e! b.txt\r");
  CHECK(strcmp(V.path, "/home/b.txt") == 0);
  TEXT_IS("t x;\nint y;");
  k(":wq\r");
  CHECK_EQ(V.req, VREQ_QUIT);
  V.req = 0;
  /* No name: :w asks for one (the app opens the picker). */
  setup("hi");
  k(":w\r");
  CHECK_EQ(V.req, VREQ_SAVEAS);
  /* A file with no newline at its end is written back without one. */
  fakefs_put("/home/n.txt", "x\ny");
  vi_load(&V, "/home/n.txt");
  k("GA!\x1b:w\r");
  CHECK(strcmp(fakefs_get("/home/n.txt"), "x\ny!") == 0);
  /* New file. */
  vi_load(&V, "/home/new.md");
  CHECK(strstr(V.msg, "[New]") != NULL);
  CHECK_EQ(V.lang, LANG_MD);
}

void test_vim_load_refuses_what_it_cannot_hold(void) {
  static char big[3000];
  memset(big, 'x', sizeof big - 1);
  setup_ex("", 2048, 1024);
  fakefs_put("/home/big.txt", big);
  CHECK_EQ(vi_load(&V, "/home/big.txt"), -1);
  CHECK_EQ(vb_len(&V.t), 0);
  CHECK_EQ(V.path[0], 0);                            /* :w cannot clobber it */
  CHECK(strstr(V.msg, "too big") != NULL);
  fakefs_put_bytes("/home/bin", "a\0b", 3);
  CHECK_EQ(vi_load(&V, "/home/bin"), -1);
  CHECK(strstr(V.msg, "not text") != NULL);
}

/* ------------------------------------------------------------- syntax --- */

void test_vim_hl_c_tokens(void) {
  uint8_t c[64];
  const char *s = "int x = 42; /* hi */ return \"s\"; // end";
  int st = hl_line(LANG_C, HS_NONE, s, (int)strlen(s), c);
  CHECK_EQ(st, HS_NONE);
  CHECK_EQ(c[0], HL_TYPE);
  CHECK_EQ(c[4], HL_N);
  CHECK_EQ(c[8], HL_NUM);
  CHECK_EQ(c[12], HL_COM);
  CHECK_EQ(c[21], HL_KW);
  CHECK_EQ(c[29], HL_STR);
  CHECK_EQ(c[35], HL_COM);
  s = "#include <stdio.h> /* open";
  st = hl_line(LANG_C, HS_NONE, s, (int)strlen(s), c);
  CHECK_EQ(st, HS_CCOM);
  CHECK_EQ(c[1], HL_PRE);
  CHECK_EQ(c[10], HL_STR);
  s = "still */ int";
  st = hl_line(LANG_C, HS_CCOM, s, (int)strlen(s), c);
  CHECK_EQ(st, HS_NONE);
  CHECK_EQ(c[0], HL_COM);
  CHECK_EQ(c[9], HL_TYPE);
}

void test_vim_hl_python_and_markdown(void) {
  uint8_t c[64];
  const char *s = "def f(): \"\"\"doc";
  int st = hl_line(LANG_PY, HS_NONE, s, (int)strlen(s), c);
  CHECK_EQ(st, HS_PY3D);
  CHECK_EQ(c[0], HL_KW);
  CHECK_EQ(c[12], HL_STR);
  st = hl_line(LANG_PY, st, "x\"\"\" # c", 8, c);
  CHECK_EQ(st, HS_NONE);
  CHECK_EQ(c[0], HL_STR);
  CHECK_EQ(c[7], HL_COM);
  st = hl_line(LANG_MD, HS_NONE, "```c", 4, c);
  CHECK_EQ(st, HS_MDFENCE);
  st = hl_line(LANG_MD, st, "# not", 5, c);
  CHECK_EQ(c[0], HL_CODE);
  st = hl_line(LANG_MD, st, "```", 3, c);
  CHECK_EQ(st, HS_NONE);
  st = hl_line(LANG_MD, st, "# head", 6, c);
  CHECK_EQ(c[3], HL_HEAD);
  s = "- a `b` **c**";
  hl_line(LANG_MD, HS_NONE, s, (int)strlen(s), c);
  CHECK_EQ(c[0], HL_KW);
  CHECK_EQ(c[4], HL_CODE);
  CHECK_EQ(c[10], HL_EMPH);
  CHECK_EQ(hl_lang_for("/home/x.PY"), LANG_PY);
  CHECK_EQ(hl_lang_for("/home/x.h"), LANG_C);
  CHECK_EQ(hl_lang_for("/home.d/readme"), LANG_PLAIN);
}

/* The state at the top of the screen comes from a checkpoint, and an edit
 * above the screen is seen. */
void test_vim_hl_state_across_the_file(void) {
  static char big[20000];
  int i, scanned;
  strcpy(big, "/* open\n");
  for (i = 0; i < 300; i++) strcat(big, "x = 1;\n");
  strcat(big, "y");
  setup(big);
  V.lang = LANG_C;
  V.hlnv = 1;
  k("G");
  CHECK_EQ(V.row_st[0], HS_CCOM);                    /* still inside the comment */
  CHECK(V.hlnv >= 300 / HL_STEP);
  k("ggA */\x1b");                                   /* closed at the top */
  CHECK(V.hlnv <= 1);
  k("G");
  CHECK_EQ(V.row_st[0], HS_NONE);
  /* Moving a screen down scans at most a checkpoint's worth of lines. */
  scanned = V.hlnv;
  k("k");
  CHECK_EQ(V.hlnv, scanned);
}

/* --------------------------------------------------------- completion --- */

void test_vim_complete_from_the_buffer(void) {
  setup("counter count_up\ncou");
  k("GA");
  key(VCTRL('n'));
  CHECK_EQ(V.ncand, 2);
  CHECK(V.cpop);
  TEXT_IS("counter count_up\ncount_up");             /* nearest first */
  key(VCTRL('n'));
  TEXT_IS("counter count_up\ncounter");
  key(VCTRL('n'));
  TEXT_IS("counter count_up\ncount_up");             /* round again */
  key(VCTRL('p'));
  TEXT_IS("counter count_up\ncounter");
  key(VK_ENTER);                                     /* accept: no newline */
  CHECK(!V.cpop);
  k("s\x1b");
  TEXT_IS("counter count_up\ncounters");
  k("u");
  TEXT_IS("counter count_up\ncou");
}

void test_vim_complete_keywords_tab_and_escape(void) {
  setup("x\nretu");
  V.lang = LANG_C;
  k("GA\t");
  TEXT_IS("x\nreturn");                               /* one: no list */
  CHECK(!V.cpop);
  k("\x1b");
  setup("unsign");
  V.lang = LANG_C;
  k("A\t");
  TEXT_IS("unsigned");
  setup("a");
  k("A \t");                                          /* not after a word: indent */
  TEXT_IS("a   ");
  setup("foo1 foo2 fo");
  k("A");
  key(VCTRL('n'));
  CHECK(V.cpop);
  key(VCTRL('e'));                                    /* back to what was typed */
  TEXT_IS("foo1 foo2 fo");
  key(VCTRL('n'));
  k("\x1b");                                          /* esc accepts, leaves insert */
  TEXT_IS("foo1 foo2 foo2");
  CHECK_EQ(V.mode, VM_NORMAL);
  setup("zz");
  k("A");
  key(VCTRL('n'));
  CHECK(strstr(V.msg, "no match") != NULL);
}

/* ---------------------------------------------------------- the view --- */

void test_vim_view_follows_the_cursor(void) {
  int i;
  static char big[2000];
  big[0] = 0;
  for (i = 0; i < 30; i++) strcat(big, "line\n");
  strcat(big, "0123456789012345678901234567890123456789012345678901234567890123456789");
  setup(big);
  V.rows = 13;
  V.width = 40;
  k("G");
  CHECK_EQ(V.top, 30 - 12);
  CHECK_EQ(vi_gutter(&V), 4);
  k("$");
  CHECK(V.left > 0);
  CHECK(69 >= V.left && 69 < V.left + vi_cols(&V));
  k("0");
  CHECK_EQ(V.left, 0);
  k("gg");
  CHECK_EQ(V.top, 0);
  k("zz");
  CHECK_EQ(V.top, 0);
  k("20Gzt");
  CHECK_EQ(V.top, 19);
}

/* ---------------------------------------------------------- the tree --- */

static VTree TT;
static char TMEM[16384];

static void tree_card(void) {
  fakeapi_init(&A);
  fakefs_mem(&A);
  vapi = &A;
  fakefs_put("/home/b.py", "print(1)\n");
  fakefs_put("/home/a.c", "int x;\n");
  fakefs_put("/home/Zed/x.md", "# x\n");
  fakefs_put("/home/.hidden", "h");
  fakefs_put("/apps/vim/vim.c", "v");
  fakefs_put("/readme.md", "r");
  memset(&TT, 0, sizeof TT);
  vt_init(&TT);
  TT.vis = 5;
  vt_attach(&TT, TMEM, sizeof TMEM);
  vt_build(&TT);
}

static void tk(const char *s) { while (*s) vt_key(&TT, (uint8_t)*s++); }
static const char *sel_path(void) {
  static char p[VT_PATH];
  vt_path(&TT, TT.sel, p, sizeof p);
  return p;
}
#define ROW_IS(i, s) CHECK(strcmp(vt_name(&TT, i), s) == 0)

void test_vim_tree_lists_folders_first_and_hides_dotfiles(void) {
  tree_card();
  CHECK_EQ(TT.nrows, 4);                          /* / apps home readme.md */
  ROW_IS(0, "/");
  ROW_IS(1, "apps");
  ROW_IS(2, "home");
  ROW_IS(3, "readme.md");
  CHECK(vt_is_dir(&TT, 1) && !vt_is_dir(&TT, 3));
  tk("jj");                                       /* home */
  CHECK(strcmp(sel_path(), "/home") == 0);
  vt_key(&TT, VK_ENTER);
  CHECK_EQ(TT.nrows, 7);
  ROW_IS(3, "Zed");                               /* folders first ... */
  ROW_IS(4, "a.c");                               /* ... then A-Z */
  ROW_IS(5, "b.py");
  CHECK_EQ(TT.rows[4].depth, 2);
  CHECK(vt_is_open(&TT, "/home"));
  tk(".");                                        /* the dotfile shows */
  CHECK_EQ(TT.nrows, 8);
  ROW_IS(4, ".hidden");
  CHECK(strcmp(sel_path(), "/home") == 0);        /* the selection stays */
  tk("gh");
  CHECK_EQ(TT.nrows, 7);
}

void test_vim_tree_folds_and_moves(void) {
  tree_card();
  tk("jjl");                                      /* l unfolds home */
  CHECK_EQ(TT.nrows, 7);
  tk("l");                                        /* again: into it */
  CHECK(strcmp(sel_path(), "/home/Zed") == 0);
  tk("l");                                        /* Zed unfolds */
  CHECK_EQ(TT.nrows, 8);
  tk("j");
  CHECK(strcmp(sel_path(), "/home/Zed/x.md") == 0);
  tk("h");                                        /* on a file: to its folder */
  CHECK(strcmp(sel_path(), "/home/Zed") == 0);
  tk("h");                                        /* on an open folder: folds */
  CHECK_EQ(TT.nrows, 7);
  CHECK(!vt_is_open(&TT, "/home/Zed"));
  tk("ll");                                       /* Zed unfolds; into it */
  tk("-");                                        /* -: up a folder, folded */
  CHECK(strcmp(sel_path(), "/home/Zed") == 0);
  CHECK_EQ(TT.nrows, 7);
  tk("G");
  CHECK(strcmp(sel_path(), "/readme.md") == 0);
  tk("gg");
  CHECK_EQ(TT.sel, 0);
  tk("3j");
  CHECK_EQ(TT.sel, 3);
  /* closing home forgets Zed inside it too */
  tk("ll");
  CHECK(vt_is_open(&TT, "/home/Zed"));
  tk("gg");
  tk("jj");
  vt_key(&TT, VK_ENTER);
  CHECK(!vt_is_open(&TT, "/home"));
  CHECK(!vt_is_open(&TT, "/home/Zed"));
  CHECK_EQ(TT.nrows, 4);
  CHECK(strcmp(sel_path(), "/home") == 0);
}

/* Open a file, the rows' memory goes to the editor, and coming back finds
 * the same folders open, the same row selected and the same scroll. */
void test_vim_tree_comes_back_at_the_same_spot(void) {
  int sel, top, n;
  tree_card();
  TT.vis = 3;
  tk("jjl");
  tk("jjj");                                      /* b.py */
  CHECK(strcmp(sel_path(), "/home/b.py") == 0);
  sel = TT.sel;
  top = TT.top;
  CHECK(top > 0);
  n = TT.nrows;
  CHECK_EQ(vt_key(&TT, VK_ENTER), VT_OPEN);
  CHECK(strcmp(TT.out, "/home/b.py") == 0);
  vt_remember(&TT);
  memset(TMEM, 'x', sizeof TMEM);                 /* the editor had it */
  vt_attach(&TT, TMEM, sizeof TMEM);
  vt_build(&TT);
  CHECK_EQ(TT.nrows, n);
  CHECK_EQ(TT.sel, sel);
  CHECK_EQ(TT.top, top);
  /* gone from the card meanwhile: the nearest folder above it */
  vt_remember(&TT);
  A.remove("/home/b.py");
  vt_attach(&TT, TMEM, sizeof TMEM);
  vt_build(&TT);
  CHECK(strcmp(sel_path(), "/home") == 0);
  /* - from a file: every folder on the way opened, the file selected */
  vt_init(&TT);
  TT.vis = 5;
  vt_reveal(&TT, "/apps/vim/vim.c");
  vt_attach(&TT, TMEM, sizeof TMEM);
  vt_build(&TT);
  CHECK(strcmp(sel_path(), "/apps/vim/vim.c") == 0);
  CHECK(vt_is_open(&TT, "/apps") && vt_is_open(&TT, "/apps/vim"));
  CHECK(TT.sel >= TT.top && TT.sel < TT.top + TT.vis);
}

void test_vim_tree_search_and_commands(void) {
  tree_card();
  tk("jjl");
  tk("/b.p");                                     /* as it is typed */
  CHECK(strcmp(sel_path(), "/home/b.py") == 0);
  vt_key(&TT, VK_ENTER);
  CHECK_EQ(TT.mode, VTM_LIST);
  tk("gg/ZZZ\r");
  CHECK(strstr(TT.msg, "E486") != NULL);
  CHECK_EQ(TT.sel, 0);
  tk("/a\r");                                     /* apps, then a.c, home ... */
  CHECK(strcmp(sel_path(), "/apps") == 0);
  tk("n");
  CHECK(strcmp(sel_path(), "/home/a.c") == 0);
  tk("N");
  CHECK(strcmp(sel_path(), "/apps") == 0);
  tk("/x");
  vt_key(&TT, VK_ESC);                            /* esc: back where it was */
  CHECK(strcmp(sel_path(), "/apps") == 0);
  /* :run and space r want a .py; :q and q leave; :e makes a file here */
  CHECK_EQ(vt_key(&TT, 'q'), VT_QUIT);
  tk(":run");
  CHECK_EQ(vt_key(&TT, VK_ENTER), VT_NONE);
  CHECK(strstr(TT.msg, "Python") != NULL);
  tk("/b.py\r ");
  CHECK_EQ(vt_key(&TT, 'r'), VT_RUN);
  CHECK(strcmp(TT.out, "/home/b.py") == 0);
  tk(":e new.py");
  CHECK_EQ(vt_key(&TT, VK_ENTER), VT_OPEN);
  CHECK(strcmp(TT.out, "/home/new.py") == 0);
  tk(":q");
  CHECK_EQ(vt_key(&TT, VK_ENTER), VT_QUIT);
  tk(":nonsense\r");
  CHECK(strstr(TT.msg, "E492") != NULL);
}

/* nvim-tree's keys: a, r and d fill in the command line, Enter does it, Esc
 * does not; :mkdir and a trailing / make folders. */
void test_vim_tree_adds_renames_and_deletes(void) {
  CappStat st;
  tree_card();
  tk("jjl");                                      /* home, open */
  CHECK(strcmp(sel_path(), "/home") == 0);
  tk("a");
  CHECK_EQ(TT.mode, VTM_CMD);
  CHECK(strcmp(TT.line, "add ") == 0);
  tk("new.py\r");                                 /* in the folder selected */
  CHECK(fakefs_exists("/home/new.py"));
  CHECK(strcmp(sel_path(), "/home/new.py") == 0);  /* shown and selected */
  CHECK(strstr(TT.msg, "made") != NULL);
  tk("anew.py\r");                                /* beside a file: same folder */
  CHECK(strstr(TT.msg, "already there") != NULL);
  tk("alib/\r");                                  /* a trailing /: a folder */
  CHECK(A.stat("/home/lib", &st) == 0 && st.is_dir);
  CHECK(strcmp(sel_path(), "/home/lib") == 0);
  tk(":mkdir sub\r");                             /* inside the folder selected */
  CHECK(A.stat("/home/lib/sub", &st) == 0 && st.is_dir);
  CHECK(strcmp(sel_path(), "/home/lib/sub") == 0);
  CHECK(vt_is_open(&TT, "/home/lib"));
  tk(":mkdir /top/\r");                           /* absolute, and a trailing / */
  CHECK(A.stat("/top", &st) == 0 && st.is_dir);
  tk(":mkdir\r");
  CHECK(strstr(TT.msg, "E471") != NULL);

  /* r: rename, the old name filled in to edit */
  tk("/new.py\r");
  tk("r");
  CHECK(strcmp(TT.line, "rename new.py") == 0);
  while (TT.linen) vt_key(&TT, VK_BS);
  tk("rename main.py\r");                         /* typed over */
  CHECK(!fakefs_exists("/home/new.py") && fakefs_exists("/home/main.py"));
  CHECK(strcmp(sel_path(), "/home/main.py") == 0);
  tk("ra.c");                                     /* esc: nothing happens */
  vt_key(&TT, VK_ESC);
  CHECK(fakefs_exists("/home/main.py"));
  tk(":rename a.c\r");                            /* not over another */
  CHECK(strstr(TT.msg, "already there") != NULL);

  /* d: delete, Enter to confirm; a folder must be empty */
  tk("d");
  CHECK(strcmp(TT.line, "delete main.py") == 0);
  vt_key(&TT, VK_ESC);
  CHECK(fakefs_exists("/home/main.py"));
  tk("d\r");
  CHECK(!fakefs_exists("/home/main.py"));
  CHECK(strstr(TT.msg, "deleted") != NULL);
  CHECK(strcmp(sel_path(), "/home/main.py") != 0);
  tk("/lib\rd\r");
  CHECK(strstr(TT.msg, "not empty") != NULL);
  CHECK(A.stat("/home/lib", &st) == 0);
  tk("gg");                                       /* nothing to do on / */
  tk("d");
  CHECK_EQ(TT.mode, VTM_LIST);
  tk("R");                                        /* R reads the card again */
  CHECK(strstr(TT.msg, "read again") != NULL);
}

/* A folder bigger than can be read at once is cut, and says so; rows that
 * do not fit are left out, and said. */
void test_vim_tree_caps_big_folders(void) {
  static char small[900];
  int i, cut = 0;
  tree_card();
  for (i = 0; i < 9; i++) {
    char p[32];
    snprintf(p, sizeof p, "/big/f%d.txt", i);
    fakefs_put(p, "x");
  }
  vt_init(&TT);
  TT.vis = 5;
  vt_attach(&TT, small, sizeof small);
  CHECK_EQ(TT.maxent, 6);
  vt_set_open(&TT, "/big", 1);
  vt_build(&TT);
  CHECK(!TT.full);
  for (i = 0; i < TT.nrows; i++) cut += (TT.rows[i].flags & VTF_CUT) != 0;
  CHECK_EQ(cut, 1);
  CHECK_EQ(TT.nrows, 1 + 4 + 6 + 1);              /* / apps big(6 ...) home readme */
  CHECK(TT.rows[2 + 7].flags & VTF_CUT);
  vt_key(&TT, VK_ENTER);                          /* on / : nothing happens */
  /* rows and names that run out: full, said, and the tree still works */
  vt_attach(&TT, small, sizeof small);
  TT.front = 40;
  vt_build(&TT);
  CHECK(TT.full);
  CHECK(strstr(TT.msg, "too many rows") != NULL);
  CHECK(TT.nrows >= 1);
  tk("jjjkG");
  CHECK_EQ(TT.sel, TT.nrows - 1);
}

void test_vim_tree_state_round_trip(void) {
  static char buf[CAPP_SAVE_MAX];
  static VTree T2;
  VtHead h, h2;
  int n, cut;
  tree_card();
  tk("jjl.jj");
  TT.top = 1;
  memset(&h, 0, sizeof h);
  h.mode = 'b';
  strcpy(h.path, "/home/a.c");
  h.line = 12; h.col = 4; h.top = 3;
  n = vt_state_write(&TT, &h, buf, sizeof buf);
  CHECK(n > 0 && n < (int)sizeof buf);
  CHECK(strncmp(buf, "vim1\n", 5) == 0);
  memset(&T2, 0, sizeof T2);
  vt_init(&T2);
  CHECK_EQ(vt_state_read(&T2, &h2, buf, n), 0);
  CHECK_EQ(h2.mode, 'b');
  CHECK(strcmp(h2.path, "/home/a.c") == 0);
  CHECK_EQ(h2.line, 12); CHECK_EQ(h2.col, 4); CHECK_EQ(h2.top, 3);
  CHECK(strcmp(T2.selpath, TT.selpath) == 0);
  CHECK_EQ(T2.top, 1);
  CHECK_EQ(T2.dots, 1);
  CHECK(vt_is_open(&T2, "/home"));
  CHECK_EQ(T2.openlen, TT.openlen);
  /* every cut-short copy is refused, and changes nothing */
  for (cut = 0; cut < n; cut++) {
    vt_init(&T2);
    vt_set_open(&T2, "/keep", 1);
    CHECK_EQ(vt_state_read(&T2, &h2, buf, cut), -1);
    CHECK(vt_is_open(&T2, "/keep") && T2.openlen == 6);
  }
  /* garbage, another version, a bad line */
  CHECK_EQ(vt_state_read(&T2, &h2, "\x01\x02garbage", 9), -1);
  CHECK_EQ(vt_state_read(&T2, &h2, "vim2\nm t\n.\n", 11), -1);
  CHECK_EQ(vt_state_read(&T2, &h2, "vim1\nm q\n.\n", 11), -1);
  CHECK_EQ(vt_state_read(&T2, &h2, "vim1\nm t\nz 1\n.\n", 15), -1);
  CHECK_EQ(vt_state_read(&T2, &h2, "vim1\nm t\nc 1 x 2\n.\n", 19), -1);
  CHECK_EQ(vt_state_read(&T2, &h2, "vim1\nm t\no rel\n.\n", 17), -1);
  CHECK_EQ(vt_state_read(&T2, &h2, "vim1\nm t\n.\nextra", 16), -1);
  CHECK_EQ(vt_state_read(&T2, &h2, "vim1\n.\n", 7), -1);          /* no mode */
  CHECK(vt_is_open(&T2, "/keep"));
  CHECK_EQ(vt_state_read(&T2, &h2, "vim1\nm t\n.\n", 11), 0);    /* the least */
  CHECK(!vt_is_open(&T2, "/keep"));
  /* too small a buffer: nothing, rather than half */
  CHECK_EQ(vt_state_write(&TT, &h, buf, 20), 0);
  /* the most there can be still fits the way back's 1 KB */
  {
    VtHead big;
    memset(&big, 0, sizeof big);
    big.mode = 'e';
    memset(big.path, 'p', VT_PATH - 1);
    big.path[0] = '/';
    vt_init(&T2);
    memset(T2.selpath, 's', VT_PATH - 1);
    T2.selpath[0] = '/';
    T2.selpath[VT_PATH - 1] = 0;
    while (T2.openlen + 40 <= VT_OPENSZ) {
      char p[40];
      snprintf(p, sizeof p, "/folder/number/%023d", T2.openlen);
      vt_set_open(&T2, p, 1);
    }
    CHECK(vt_state_write(&T2, &big, buf, CAPP_SAVE_MAX) > 0);
  }
}

/* The editor's side: - and :Ex, :run and space r. */
void test_vim_tree_and_run_from_a_file(void) {
  setup("");
  fakefs_put("/home/a.py", "x = 1\n");
  fakefs_put("/home/a.c", "int x;\n");
  vi_load(&V, "/home/a.py");
  k("-");
  CHECK_EQ(V.req, VREQ_TREE);
  V.req = 0;
  k("x-");
  CHECK_EQ(V.req, VREQ_NONE);
  CHECK(strstr(V.msg, "E37") != NULL);
  k(":Ex!\r");
  CHECK_EQ(V.req, VREQ_TREE);
  V.req = 0;
  /* :run writes first */
  k(":run\r");
  CHECK_EQ(V.req, VREQ_RUN);
  CHECK(strcmp(fakefs_get("/home/a.py"), " = 1\n") == 0);
  CHECK(!vi_modified(&V));
  V.req = 0;
  k(" r");
  CHECK_EQ(V.req, VREQ_RUN);
  V.req = 0;
  /* space is still l when r does not follow; 2- is still the motion */
  k("0 x");
  CHECK(strcmp(txt(), " 1") == 0 || strcmp(txt(), "  1") == 0);
  vi_load(&V, "/home/a.c");
  k(":py\r");
  CHECK_EQ(V.req, VREQ_NONE);
  CHECK(strstr(V.msg, "not a Python file") != NULL);
  setup("one\ntwo\nthree");
  k("G2-");
  CHECK_EQ(line(), 0);
  CHECK_EQ(V.req, VREQ_NONE);
}
