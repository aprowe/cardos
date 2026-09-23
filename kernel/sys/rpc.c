/* Parsing the small command vocabulary. See rpc.h. */

#include "kernel/sys/rpc.h"

#include <stddef.h>

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static const char *skip_space(const char *p) {
  while (*p && is_space(*p)) p++;
  return p;
}

/* Compare a word at `p` against `word`, case-insensitively. Returns the
 * position after it if it matched and ended at a space or end of string, or
 * NULL. */
static const char *word_is(const char *p, const char *word) {
  while (*word) {
    if (lower(*p) != lower(*word)) return NULL;
    p++; word++;
  }
  if (*p && !is_space(*p)) return NULL;
  return p;
}

static void copy_arg(RpcCmd *cmd, const char *p) {
  int i = 0;
  p = skip_space(p);
  while (*p && i < RPC_ARG_MAX - 1) cmd->arg[i++] = *p++;
  /* Trailing whitespace and the full stop a recogniser adds to everything. */
  while (i > 0 && (is_space(cmd->arg[i - 1]) || cmd->arg[i - 1] == '.')) i--;
  cmd->arg[i] = 0;
}

static int parse_num(const char *s, int *out) {
  int v = 0, any = 0;
  s = skip_space(s);
  while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
  if (!any) return 0;
  *out = v;
  return 1;
}

void rpc_one_line(char *s) {
  for (; *s; s++)
    if (*s == '\n' || *s == '\r' || *s == '\t') *s = ' ';
}

int rpc_parse(const char *line, RpcCmd *cmd) {
  const char *p;
  int i;

  if (!line || !cmd) return 0;
  cmd->verb = RPC_BAD;
  cmd->arg[0] = 0;
  cmd->num = 0;

  p = skip_space(line);
  /* Keep the whole line for the caller to show if this turns out to be junk. */
  for (i = 0; p[i] && i < RPC_ARG_MAX - 1; i++) cmd->arg[i] = p[i];
  cmd->arg[i] = 0;
  while (i > 0 && is_space(cmd->arg[i - 1])) cmd->arg[--i] = 0;

  {
    const char *rest;

    if ((rest = word_is(p, "open")) != NULL) {
      cmd->verb = RPC_OPEN;
      copy_arg(cmd, rest);
      return cmd->arg[0] ? 1 : (cmd->verb = RPC_BAD, 0);
    }
    if ((rest = word_is(p, "shell")) != NULL) {
      copy_arg(cmd, rest);
      if (word_is(cmd->arg, "launcher") || word_is(cmd->arg, "desktop") ||
          word_is(cmd->arg, "console")) {
        cmd->verb = RPC_SHELL;
        return 1;
      }
      cmd->verb = RPC_BAD;
      return 0;
    }
    if ((rest = word_is(p, "bright")) != NULL) {
      if (!parse_num(rest, &cmd->num)) return 0;
      /* Clamped rather than rejected: "as bright as possible" coming back as
       * 200 is a reasonable thing for a model to say, and the device has an
       * opinion about the range anyway. */
      if (cmd->num > 100) cmd->num = 100;
      if (cmd->num < 0) cmd->num = 0;
      cmd->verb = RPC_BRIGHT;
      return 1;
    }
    if ((rest = word_is(p, "wifi")) != NULL) {
      copy_arg(cmd, rest);
      if (word_is(cmd->arg, "on"))       { cmd->num = 1; cmd->verb = RPC_WIFI; return 1; }
      if (word_is(cmd->arg, "off"))      { cmd->num = 0; cmd->verb = RPC_WIFI; return 1; }
      cmd->verb = RPC_BAD;
      return 0;
    }
    if ((rest = word_is(p, "say")) != NULL) {
      cmd->verb = RPC_SAY;
      copy_arg(cmd, rest);
      rpc_one_line(cmd->arg);
      /* An empty `say` is not a command, it is a model with nothing to add. */
      return cmd->arg[0] ? 1 : (cmd->verb = RPC_BAD, 0);
    }
    if ((rest = word_is(p, "key")) != NULL) {
      copy_arg(cmd, rest);
      if (word_is(cmd->arg, "escape") || word_is(cmd->arg, "enter") ||
          word_is(cmd->arg, "up") || word_is(cmd->arg, "down") ||
          word_is(cmd->arg, "left") || word_is(cmd->arg, "right")) {
        cmd->verb = RPC_KEY;
        return 1;
      }
      cmd->verb = RPC_BAD;
      return 0;
    }
    if ((rest = word_is(p, "action")) != NULL) {
      /* The id is validated by the app that owns it, not here: this list
       * cannot know what Todo calls its verbs, and an unknown one is
       * refused with the app's own list in the answer. */
      cmd->verb = RPC_ACTION;
      copy_arg(cmd, rest);
      return cmd->arg[0] ? 1 : (cmd->verb = RPC_BAD, 0);
    }
    if ((rest = word_is(p, "do")) != NULL) {
      /* APP COMMAND ARGS, kept whole: kernel/app/cmdline.c splits it and
       * checks it against the app's declared table, which is the check
       * that matters. Here only: at least an app and a command. */
      const char *q;
      cmd->verb = RPC_DO;
      copy_arg(cmd, rest);
      q = cmd->arg;
      while (*q && !is_space(*q)) q++;
      while (*q && is_space(*q)) q++;
      return *q ? 1 : (cmd->verb = RPC_BAD, 0);
    }
    if ((rest = word_is(p, "none")) != NULL) {
      cmd->verb = RPC_NONE;
      copy_arg(cmd, rest);
      return 1;
    }
  }
  return 0;
}

const char *rpc_wake(const char *text) {
  static const char *NAMES[] = { "carlos", "karlos", "carlus", "carlo" };
  size_t n;
  const char *p;

  if (!text) return NULL;
  p = skip_space(text);

  for (n = 0; n < sizeof NAMES / sizeof NAMES[0]; n++) {
    const char *w = NAMES[n];
    const char *q = p;
    while (*w && lower(*q) == *w) { q++; w++; }
    if (*w) continue;
    /* The name has to end here -- "Carlsberg" is not the wake word. */
    if (*q && !is_space(*q) && *q != ',' && *q != '.' && *q != ':' &&
        *q != '!' && *q != '?')
      continue;
    while (*q == ',' || *q == '.' || *q == ':' || *q == '!' || *q == '?') q++;
    return skip_space(q);
  }
  return NULL;
}
