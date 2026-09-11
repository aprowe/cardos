/* grep, as a loadable CardOS app.
 *
 * Deliberately not a kernel command. Searching files is not something an
 * operating system has to be able to do -- it is something a program does --
 * and putting it on the card rather than in the image means it can be replaced
 * without reflashing, and costs nothing when it is not running.
 *
 *     run grep TODO            search the current folder
 *     run grep TODO /desktop   search somewhere else
 *     run grep -i todo /notes  case-insensitive
 *
 * Results are a list you can walk rather than a wall of text that has already
 * scrolled past, which is the one thing a 240x135 screen makes obviously
 * better than a terminal.
 *
 * The matcher is plain substring, not a regular expression. A regex engine is
 * several kilobytes to answer a question that, on a machine with this much
 * text on it, "does this line contain these characters" answers just as well.
 */

#include "kernel/app/capp.h"

#define MAX_HITS   64
#define LINE_MAX   96
#define SHOW_MAX   44      /* of the matching line, after the location */
#define MAX_FILES  32
#define NAMELEN    32
#define ROW_H      9

#define CLR_BG    CAPP_RGB(24, 26, 32)
#define CLR_TEXT  CAPP_RGB(214, 220, 232)
#define CLR_FILE  CAPP_RGB(120, 200, 255)
#define CLR_HIT   CAPP_RGB(255, 200, 90)
#define CLR_DIM   CAPP_RGB(130, 140, 158)
#define CLR_BAR   CAPP_RGB(48, 82, 128)
#define CLR_BARFG CAPP_RGB(232, 238, 248)
#define CLR_SEL   CAPP_RGB(52, 80, 116)

static const CardApi *api;

typedef struct {
  char file[NAMELEN];
  int  line;
  char text[SHOW_MAX + 1];
} Hit;

static struct {
  char pattern[40];
  char dir[64];
  int  fold_case;

  Hit  hit[MAX_HITS];
  int  nhits;
  int  sel;
  int  scanned;        /* files looked at */
  int  searched;       /* have we run yet */
  int  full;           /* stopped at MAX_HITS */
  char note[56];
} G;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static char fold(char c) {
  if (c >= 'A' && c <= 'Z') return (char)(c + 32);
  return c;
}

/* Substring search. Naive, and right to be: the haystack is one line and the
 * needle is a few characters, so the clever algorithms spend more time on
 * setup than this spends searching. */
static int contains(const char *hay, int hlen, const char *needle, int nlen,
                    int fold_case) {
  int i, j;
  if (nlen == 0) return 1;
  if (nlen > hlen) return 0;
  for (i = 0; i <= hlen - nlen; i++) {
    for (j = 0; j < nlen; j++) {
      char a = hay[i + j], b = needle[j];
      if (fold_case) { a = fold(a); b = fold(b); }
      if (a != b) break;
    }
    if (j == nlen) return i;
  }
  return -1;
}

static void add_hit(const char *file, int line, const char *text, int len, int at) {
  Hit *h;
  int start = 0, n;

  if (G.nhits >= MAX_HITS) { G.full = 1; return; }
  h = &G.hit[G.nhits++];
  api->fmt(h->file, sizeof h->file, "%s", file);
  h->line = line;

  /* Window the line around the match, so a hit at column 200 is still visible
   * rather than scrolled off the right of a 40-column screen. */
  if (at > 8) start = at - 8;
  n = len - start;
  if (n > SHOW_MAX) n = SHOW_MAX;
  if (n < 0) n = 0;
  api->mem_cpy(h->text, text + start, (size_t)n);
  h->text[n] = 0;
}

static void search_file(const char *dir, const char *name) {
  char path[128], buf[256], line[LINE_MAX];
  int fd, n, i, len = 0, lineno = 1;
  int plen = (int)api->str_len(G.pattern);

  api->fmt(path, sizeof path, "%s%s%s", dir, dir[1] == 0 ? "" : "/", name);
  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) return;                   /* a folder, or unreadable */
  G.scanned++;

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];

      if (c != 10) {
        if (c == 13) continue;
        /* A control byte means this is not text. Stop rather than matching
         * against whatever a binary happens to contain. */
        if (c < 32 || (unsigned char)c > 126) { api->close(fd); return; }
        if (len < LINE_MAX - 1) line[len++] = c;
        continue;
      }

      line[len] = 0;
      {
        int at = contains(line, len, G.pattern, plen, G.fold_case);
        if (at >= 0) add_hit(name, lineno, line, len, at);
      }
      if (G.nhits >= MAX_HITS) { api->close(fd); return; }
      len = 0;
      lineno++;
    }
  }

  /* The last line, if the file does not end with a newline. */
  if (len > 0) {
    line[len] = 0;
    {
      int at = contains(line, len, G.pattern, plen, G.fold_case);
      if (at >= 0) add_hit(name, lineno, line, len, at);
    }
  }
  api->close(fd);
}

static void run_search(void) {
  static char names[MAX_FILES][NAMELEN];
  int n, i;

  G.nhits = 0;
  G.sel = 0;
  G.scanned = 0;
  G.full = 0;
  G.searched = 1;

  if (!G.pattern[0]) { api->fmt(G.note, sizeof G.note, "%s", "no pattern"); return; }

  n = api->list(G.dir, &names[0][0], MAX_FILES, NAMELEN);
  if (n < 0) {
    api->fmt(G.note, sizeof G.note, "cannot read %s", G.dir);
    return;
  }
  for (i = 0; i < n && G.nhits < MAX_HITS; i++) search_file(G.dir, names[i]);

  api->fmt(G.note, sizeof G.note, "%d hit%s in %d file%s%s",
           G.nhits, G.nhits == 1 ? "" : "s",
           G.scanned, G.scanned == 1 ? "" : "s",
           G.full ? " (stopped early)" : "");

  /* Also to the log, because the screen is the only other place this appears
   * and the screen is not always where you are when you run it. */
  api->log(G.note);
}

/* ---- arguments ----------------------------------------------------------
 *
 * "[-i] PATTERN [PATH]". Parsed here rather than by the shell, because the
 * shell has no business knowing what -i means to this program. */
static void app_set_args(void *st, const char *args) {
  char word[80];
  int i = 0, w;
  (void)st;

  G.pattern[0] = 0;
  G.fold_case = 0;

  for (;;) {
    while (args[i] == ' ') i++;
    if (!args[i]) break;

    w = 0;
    while (args[i] && args[i] != ' ' && w < (int)sizeof word - 1)
      word[w++] = args[i++];
    word[w] = 0;

    if (word[0] == '-' && (word[1] == 'i' || word[1] == 'I') && !word[2]) {
      G.fold_case = 1;
    } else if (!G.pattern[0]) {
      api->fmt(G.pattern, sizeof G.pattern, "%s", word);
    } else {
      api->fmt(G.dir, sizeof G.dir, "%s", word);
    }
  }

  run_search();
}

/* ------------------------------------------------------------- painting -- */

static void app_paint(void *st, CRect c) {
  int rows = (c.h - ROW_H) / ROW_H;
  int top = 0, r;
  char buf[96];
  (void)st;

  if (G.sel >= rows) top = G.sel - rows + 1;

  for (r = 0; r < rows; r++) {
    int i = top + r;
    short y = (short)(c.y + r * ROW_H);
    int sel = (i == G.sel);
    uint16_t bg = sel ? CLR_SEL : CLR_BG;

    api->fill(rect(c.x, y, c.w, ROW_H), bg);
    if (i >= G.nhits) continue;

    /* The location in its own colour, so the eye can skip it to read the
     * matches or follow it to find them. */
    api->fmt(buf, sizeof buf, "%s:%d", G.hit[i].file, G.hit[i].line);
    api->text((short)(c.x + 2), y, buf, CLR_FILE, bg);
    {
      short x = (short)(c.x + 2 + (short)api->str_len(buf) * 6 + 4);
      api->text(x, y, G.hit[i].text, sel ? CLR_BARFG : CLR_TEXT, bg);
    }
  }

  if (G.nhits == 0) {
    api->fill(rect(c.x, c.y, c.w, c.h - ROW_H), CLR_BG);
    if (!G.searched) {
      api->text((short)(c.x + 4), (short)(c.y + 4),
                "run grep PATTERN [PATH]", CLR_TEXT, CLR_BG);
      api->text((short)(c.x + 4), (short)(c.y + 16),
                "-i for case-insensitive", CLR_DIM, CLR_BG);
    } else {
      api->text((short)(c.x + 4), (short)(c.y + 4), "no matches", CLR_DIM, CLR_BG);
    }
  }

  api->fill(rect(c.x, c.y + c.h - ROW_H, c.w, ROW_H), CLR_BAR);
  api->fmt(buf, sizeof buf, "%s%s  %s  %s",
           G.fold_case ? "-i " : "", G.pattern[0] ? G.pattern : "-",
           G.dir, G.note);
  api->text((short)(c.x + 2), (short)(c.y + c.h - ROW_H + 1), buf,
            CLR_BARFG, CLR_BAR);
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  switch (k) {
  case CAPP_KEY_UP:   if (G.sel > 0) G.sel--; return 1;
  case CAPP_KEY_DOWN: if (G.sel + 1 < G.nhits) G.sel++; return 1;
  case 'r': case 'R': run_search(); return 1;
  case 'i': case 'I': G.fold_case = !G.fold_case; run_search(); return 1;
  default: return 0;
  }
}

static int app_click(void *st, short x, short y, int button) {
  int i = y / ROW_H;
  (void)st; (void)x; (void)button;
  if (i < 0 || i >= G.nhits) return 0;
  G.sel = i;
  return 1;
}

static void app_open(void *st) {
  (void)st;
  if (!G.dir[0]) api->fmt(G.dir, sizeof G.dir, "%s", "/");
  G.searched = 0;
  G.nhits = 0;
  G.sel = 0;
  api->fmt(G.note, sizeof G.note, "%s", "waiting for a pattern");
}

/* 16x16: a magnifier over ruled lines. */
static const unsigned char ICON[CAPP_ICON_BYTES] = {
  0x00, 0x00, 0x0F, 0x80, 0x18, 0xC0, 0x30, 0x60,
  0x30, 0x60, 0x30, 0x60, 0x18, 0xC0, 0x0F, 0x80,
  0x03, 0xC0, 0x01, 0xE0, 0x00, 0xF0, 0x00, 0x78,
  0x00, 0x3C, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00,
};

static CappApp APP;

const CappApp *capp_register(const CardApi *a) {
  api = a;
  APP.api_version = CAPP_API_VERSION;
  api->mem_cpy(APP.name, "Grep", 5);
  api->mem_cpy(APP.icon, ICON, CAPP_ICON_BYTES);
  APP.fullscreen = 1;      /* results want the width */
  APP.paint = app_paint;
  APP.key = app_key;
  APP.click = app_click;
  APP.open = app_open;
  APP.set_args = app_set_args;
  APP.height = 0;
  APP.pref_w = 0;
  APP.pref_h = 0;
  APP.wants_text = 0;
  APP.help = "arrows\tmove through the hits\nr\tsearch again\ni\tcase on and off\n";
  APP.state = 0;
  return &APP;
}
