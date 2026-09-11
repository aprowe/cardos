/* cat -- copy files, or standard input, to standard output.
 *
 * There is a cat built into the console already. This one exists because the
 * built-in writes straight to the screen and therefore cannot be the left-hand
 * side of a pipeline: it has no stdout to redirect. This one has nothing but
 * stdout, which is what makes `cat notes | grep TODO > hits` work.
 *
 *     cat FILE...     write those files out
 *     cat             copy stdin to stdout
 *     cat -n FILE     number the lines
 */

#include "kernel/app/capp.h"

#define LINE_MAX 200

static const CardApi *api;

static int s_numbered;
static int s_lineno = 1;

static void put(const char *line) {
  if (!s_numbered) { api->out_line(line); return; }
  {
    char buf[LINE_MAX + 16];
    api->fmt(buf, sizeof buf, "%6d  %s", s_lineno++, line);
    api->out_line(buf);
  }
}

static int cat_file(const char *path) {
  char buf[256], line[LINE_MAX];
  int fd, n, i, len = 0;

  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) {
    char msg[96];
    api->fmt(msg, sizeof msg, "cat: %s: cannot open", path);
    api->out_line(msg);
    return 1;
  }

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c == 13) continue;
      if (c != 10) {
        if (len < LINE_MAX - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      put(line);
      len = 0;
    }
  }
  if (len > 0) { line[len] = 0; put(line); }

  api->close(fd);
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_CLI,
  "cat",
  { 0x00, 0x00, 0x1F, 0xF0, 0x10, 0x18, 0x10, 0x14,
    0x10, 0x12, 0x10, 0x1F, 0x13, 0xC1, 0x10, 0x01,
    0x13, 0xE1, 0x10, 0x01, 0x13, 0xC1, 0x10, 0x01,
    0x11, 0xE1, 0x10, 0x01, 0x1F, 0xFF, 0x00, 0x00 },
  NULL,
};

int capp_main(const CardApi *a, int argc, char **argv) {
  int i, files = 0, bad = 0;

  api = a;
  s_numbered = 0;
  s_lineno = 1;

  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1] == 'n' && !argv[i][2]) {
      s_numbered = 1;
      continue;
    }
    files++;
    if (cat_file(argv[i]) != 0) bad = 1;
  }

  if (files == 0) {
    char line[LINE_MAX];
    if (!api->has_input()) {
      api->out_line("usage: cat [-n] FILE...");
      return 2;
    }
    while (api->in_line(line, sizeof line) >= 0) put(line);
  }

  return bad;
}
