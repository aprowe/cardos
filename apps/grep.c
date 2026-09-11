/* grep -- print lines matching a pattern.
 *
 * A command, not an app: it has a capp_main and installs no user interface, so
 * it runs, writes to stdout, and returns. Nothing takes the screen.
 *
 *     grep PATTERN            filter stdin
 *     grep PATTERN FILE...    search those files
 *     cat notes | grep TODO   the same thing, the other way round
 *     grep -i todo notes.txt  case-insensitive
 *     grep -n TODO notes.txt  number the lines
 *     grep -v TODO notes.txt  invert
 *
 * Reads stdin when given no files, which is what makes it work in a pipeline
 * without knowing it is in one. That is the whole point of the paradigm: this
 * program has no idea whether its output is going to the screen, a file, or
 * another program.
 *
 * The matcher is plain substring, not a regular expression. A regex engine is
 * several kilobytes to answer a question that, on a machine with this much
 * text on it, "does this line contain these characters" answers just as well.
 */

#include "kernel/app/capp.h"

#define LINE_MAX 200

static const CardApi *api;

static char fold(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Naive, and right to be: the haystack is one line and the needle a few
 * characters, so the clever algorithms spend more time on setup than this
 * spends searching. */
static int contains(const char *hay, const char *needle, int fold_case) {
  int i, j;
  int hlen = (int)api->str_len(hay), nlen = (int)api->str_len(needle);

  if (nlen == 0) return 1;
  if (nlen > hlen) return 0;
  for (i = 0; i <= hlen - nlen; i++) {
    for (j = 0; j < nlen; j++) {
      char a = hay[i + j], b = needle[j];
      if (fold_case) { a = fold(a); b = fold(b); }
      if (a != b) break;
    }
    if (j == nlen) return 1;
  }
  return 0;
}

typedef struct {
  const char *pattern;
  int fold_case, invert, numbered, with_name;
  int hits;
} Grep;

static void emit(Grep *g, const char *name, int lineno, const char *line) {
  char out[LINE_MAX + 48];

  if (g->with_name && g->numbered)
    api->fmt(out, sizeof out, "%s:%d:%s", name, lineno, line);
  else if (g->with_name)
    api->fmt(out, sizeof out, "%s:%s", name, line);
  else if (g->numbered)
    api->fmt(out, sizeof out, "%d:%s", lineno, line);
  else
    api->fmt(out, sizeof out, "%s", line);

  api->out_line(out);
  g->hits++;
}

static void test_line(Grep *g, const char *name, int lineno, const char *line) {
  int match = contains(line, g->pattern, g->fold_case);
  if (match != g->invert) emit(g, name, lineno, line);
}

static void grep_stdin(Grep *g) {
  char line[LINE_MAX];
  int lineno = 1;
  while (api->in_line(line, sizeof line) >= 0)
    test_line(g, "-", lineno++, line);
}

static void grep_file(Grep *g, const char *path) {
  char buf[256], line[LINE_MAX];
  int fd, n, i, len = 0, lineno = 1;

  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) {
    char msg[96];
    api->fmt(msg, sizeof msg, "grep: %s: cannot open", path);
    api->out_line(msg);
    return;
  }

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c == 13) continue;
      if (c != 10) {
        /* A control byte means this is not text. Stop rather than matching
         * against whatever a binary happens to contain. */
        if (c < 32 || (unsigned char)c > 126) { api->close(fd); return; }
        if (len < LINE_MAX - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      test_line(g, path, lineno++, line);
      len = 0;
    }
  }
  if (len > 0) {
    line[len] = 0;
    test_line(g, path, lineno, line);
  }
  api->close(fd);
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_CLI,
  "grep",
  /* 16x16: a magnifier. Unused while it is a command, and there in case it is
   * ever given a face. */
  { 0x00, 0x00, 0x0F, 0x80, 0x18, 0xC0, 0x30, 0x60,
    0x30, 0x60, 0x30, 0x60, 0x18, 0xC0, 0x0F, 0x80,
    0x03, 0xC0, 0x01, 0xE0, 0x00, 0xF0, 0x00, 0x78,
    0x00, 0x3C, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00 },
  NULL,
};

int capp_main(const CardApi *a, int argc, char **argv) {
  Grep g;
  int i, nfiles = 0;
  int first_file = argc;

  api = a;
  api->mem_set(&g, 0, sizeof g);

  for (i = 1; i < argc; i++) {
    const char *s = argv[i];
    if (s[0] == '-' && s[1] && !g.pattern) {
      int j;
      for (j = 1; s[j]; j++) {
        if (s[j] == 'i') g.fold_case = 1;
        else if (s[j] == 'v') g.invert = 1;
        else if (s[j] == 'n') g.numbered = 1;
        else {
          api->out_line("grep: options are -i -v -n");
          return 2;
        }
      }
      continue;
    }
    if (!g.pattern) { g.pattern = s; continue; }
    if (first_file == argc) first_file = i;
    nfiles++;
  }

  if (!g.pattern) {
    api->out_line("usage: grep [-inv] PATTERN [FILE...]");
    return 2;
  }

  /* The file name is prefixed only when there is more than one, which is what
   * makes "grep x one.txt" readable and "grep x *.txt" navigable. */
  g.with_name = (nfiles > 1);

  if (nfiles == 0) {
    if (!api->has_input()) {
      api->out_line("grep: no input -- give a file, or pipe something in");
      return 2;
    }
    grep_stdin(&g);
  } else {
    for (i = first_file; i < argc; i++) {
      if (argv[i][0] == '-' && argv[i][1]) continue;
      if (argv[i] == g.pattern) continue;
      grep_file(&g, argv[i]);
    }
  }

  /* Nothing found is exit status 1, as grep has always done, so a shell could
   * one day branch on it. */
  return g.hits ? 0 : 1;
}
