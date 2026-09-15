/* The voice command vocabulary, and the wake word.
 *
 * Both are parsers fed by a speech recogniser, which is to say fed by
 * something that will hand them almost anything. The tests are mostly about
 * what must be *rejected*: the device executes these verbs, and the only thing
 * standing between a misheard sentence and a surprise is this parser.
 */

#include <string.h>

#include "tinytest.h"
#include "kernel/sys/rpc.h"

static RpcCmd parse(const char *line, int *ok) {
  RpcCmd c;
  *ok = rpc_parse(line, &c);
  return c;
}

void test_rpc_the_verbs_parse(void) {
  int ok;
  RpcCmd c;

  c = parse("open notes", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_OPEN); CHECK(!strcmp(c.arg, "notes"));

  c = parse("shell launcher", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_SHELL); CHECK(!strcmp(c.arg, "launcher"));

  c = parse("bright 40", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_BRIGHT); CHECK_EQ(c.num, 40);

  c = parse("wifi off", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_WIFI); CHECK_EQ(c.num, 0);

  c = parse("say good morning", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_SAY); CHECK(!strcmp(c.arg, "good morning"));

  c = parse("key escape", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_KEY); CHECK(!strcmp(c.arg, "escape"));

  c = parse("action save", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_ACTION); CHECK(!strcmp(c.arg, "save"));

  c = parse("action", &ok);
  CHECK(!ok);

  c = parse("none I did not understand that", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_NONE);
  CHECK(!strcmp(c.arg, "I did not understand that"));
}

/* Whitespace and the full stop a recogniser puts on the end of everything. */
void test_rpc_tolerates_what_a_recogniser_produces(void) {
  int ok;
  RpcCmd c;

  c = parse("  open  pinball  ", &ok);
  CHECK(ok); CHECK(!strcmp(c.arg, "pinball"));

  c = parse("open notes.", &ok);
  CHECK(ok); CHECK(!strcmp(c.arg, "notes"));

  c = parse("OPEN Notes", &ok);
  CHECK(ok); CHECK_EQ(c.verb, RPC_OPEN);

  c = parse("bright   75\n", &ok);
  CHECK(ok); CHECK_EQ(c.num, 75);
}

/* The important half: everything that is not one of the seven verbs is
 * refused, and refused with the line kept so a person can see what happened. */
void test_rpc_refuses_anything_else(void) {
  int ok;
  RpcCmd c;

  c = parse("rm -rf /", &ok);
  CHECK(!ok); CHECK_EQ(c.verb, RPC_BAD); CHECK(!strcmp(c.arg, "rm -rf /"));

  c = parse("run grep", &ok);          CHECK(!ok);
  c = parse("openn notes", &ok);       CHECK(!ok);
  c = parse("shell bash", &ok);        CHECK(!ok);
  c = parse("wifi maybe", &ok);        CHECK(!ok);
  c = parse("key f7", &ok);            CHECK(!ok);
  c = parse("bright", &ok);            CHECK(!ok);
  c = parse("open", &ok);              CHECK(!ok);
  c = parse("say", &ok);               CHECK(!ok);
  c = parse("", &ok);                  CHECK(!ok);
  c = parse("   ", &ok);               CHECK(!ok);
  (void)c;
}

/* A number outside the range is clamped rather than refused: "as bright as it
 * goes" coming back as 200 is a reasonable thing for a model to say. */
void test_rpc_clamps_the_brightness(void) {
  int ok;
  RpcCmd c = parse("bright 400", &ok);
  CHECK(ok); CHECK_EQ(c.num, 100);
}

/* A very long argument must not run off the end of the buffer. */
void test_rpc_does_not_overflow_on_a_long_argument(void) {
  char line[600];
  int ok, i;
  RpcCmd c;

  memcpy(line, "say ", 4);
  for (i = 4; i < (int)sizeof line - 1; i++) line[i] = 'x';
  line[sizeof line - 1] = 0;

  c = parse(line, &ok);
  CHECK(ok);
  CHECK(strlen(c.arg) < RPC_ARG_MAX);
}

void test_rpc_wake_word(void) {
  CHECK(rpc_wake("carlos open notes") != NULL);
  CHECK(!strcmp(rpc_wake("carlos open notes"), "open notes"));
  CHECK(!strcmp(rpc_wake("Carlos, open notes"), "open notes"));
  CHECK(!strcmp(rpc_wake("  CARLOS: turn the brightness down"),
                "turn the brightness down"));

  /* Recognition hears a name it has no word for and picks something close, so
   * the near misses count too. */
  CHECK(rpc_wake("karlos open notes") != NULL);
  CHECK(rpc_wake("Carlo open notes") != NULL);

  /* And things that merely start with the same letters do not. */
  CHECK(rpc_wake("carlsberg is a lager") == NULL);
  CHECK(rpc_wake("the carlos I know") == NULL);
  CHECK(rpc_wake("hello there") == NULL);
  CHECK(rpc_wake("") == NULL);
  CHECK(rpc_wake(NULL) == NULL);
}

/* The wake word on its own is not a command -- it is someone starting to speak
 * and stopping. It must not come back as an empty instruction. */
void test_rpc_wake_word_alone_leaves_nothing(void) {
  const char *rest = rpc_wake("Carlos.");
  int ok;
  CHECK(rest != NULL);
  CHECK_EQ((int)strlen(rest), 0);
  parse(rest, &ok);
  CHECK(!ok);
}

/* `say` puts text into whatever is listening, and the console listens. A line
 * break in the text would be delivered as enter -- so "say ls\nrm /x" was a
 * command typed and run, which is exactly what the vocabulary exists to
 * prevent. The text is one line: breaks become spaces. */
void test_rpc_say_is_one_line(void) {
  int ok;
  RpcCmd c = parse("say hello\nrm /x\r\tthere", &ok);
  CHECK(ok);
  CHECK_EQ(c.verb, RPC_SAY);
  CHECK(!strcmp(c.arg, "hello rm /x  there"));
}

/* And the same guard as a function, for the agent's `type` tool, whose
 * argument does not come through rpc_parse. */
void test_rpc_one_line_flattens_breaks(void) {
  char s[] = "a\nb\r\nc";
  rpc_one_line(s);
  CHECK(!strcmp(s, "a b  c"));
}
