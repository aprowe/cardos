/* The commands a voice can give.
 *
 * "Carlos, turn the brightness down" goes to a Claude session on the PC, which
 * answers with one line of this vocabulary, which the device executes.
 *
 * The vocabulary is small on purpose, and this file is where that decision
 * lives. An LLM working out that "turn it down" means the backlight is useful;
 * an LLM handing a device an arbitrary string to run is not. The gap between
 * those two is a fixed list of verbs, parsed here and validated against that
 * list before anything happens -- so the worst a confused model can do is pick
 * the wrong verb from a set that contains nothing dangerous.
 *
 * One line, one command:
 *
 *     open NAME        launch an app by name
 *     shell WHICH      launcher | desktop | console
 *     bright N         0..100
 *     wifi on|off
 *     say TEXT         put TEXT into the focused app, as if typed
 *     key NAME         escape enter up down left right
 *     action ID        run one entry of the focused app's action table
 *     none REASON      nothing matched; REASON is shown to the user
 */
#ifndef CARDOS_RPC_H
#define CARDOS_RPC_H

typedef enum {
  RPC_BAD = 0,        /* not one of the verbs, or malformed */
  RPC_OPEN,
  RPC_SHELL,
  RPC_BRIGHT,
  RPC_WIFI,
  RPC_SAY,
  RPC_KEY,
  RPC_ACTION,
  RPC_NONE,           /* the model declined, and said why */
  RPC_DO              /* do APP COMMAND ARGS: an app's command, open or not */
} RpcVerb;

#define RPC_ARG_MAX 160

typedef struct {
  RpcVerb verb;
  char    arg[RPC_ARG_MAX];   /* the rest of the line, trimmed */
  int     num;                /* bright: 0..100. wifi: 1 on, 0 off. */
} RpcCmd;

/* Parse one line. Returns 1 if it is a command this device will execute, 0 if
 * not -- in which case cmd->verb is RPC_BAD and cmd->arg holds the line, so a
 * caller can show what was rejected. */
int rpc_parse(const char *line, RpcCmd *cmd);

/* Text for `say` is one line. A line break would be delivered as enter, and
 * with the console listening that turns a string into a command that runs;
 * this replaces every break and tab with a space, in place. rpc_parse does
 * it for `say`; the agent does it for its `type` tool, whose argument comes
 * from a tool call rather than a line. */
void rpc_one_line(char *s);

/* The wake word.
 *
 * Returns a pointer into `text` just past the wake word and any punctuation
 * after it, or NULL if the text does not start with it. Case-insensitive, and
 * tolerant of what recognition does to a name it does not know: "Carlos",
 * "carlos,", "Carlos:" and "Karlos" all count. Recognisers hear a name they
 * have no word for and pick something close, and a wake word that only works
 * when spelled correctly is a wake word that mostly does not work. */
const char *rpc_wake(const char *text);

#endif /* CARDOS_RPC_H */
