/* Build -- talk to the machine that builds this one.
 *
 * The model does not run here and never could. What runs here is a terminal:
 * it posts what you typed to the CardOS server (server/), which hands it to
 * Claude Code running *in the CardOS repository*, and prints what comes back.
 * So "make the flippers stronger" is not a chat message, it is an edit to
 * apps/pinball.c on the other end of the wire.
 *
 * The thing that shapes this file is that the shell is one cooperative loop.
 * A reply takes anywhere from five seconds to two minutes, and a blocking
 * request that long is a frozen machine -- no clock, no escape key, nothing.
 * So nothing here blocks for long:
 *
 *   Enter          posts the message, gets an id back, and returns
 *   every second   tick() asks whether that id is done yet
 *   when it is     the answer is wrapped and added to the log
 *
 * Each of those is a short request. The screen stays alive between them, and
 * escape still works while Claude is thinking.
 *
 *     build                   the server the OS uses (env PROXY)
 *     build <base-url>        somewhere else, e.g. http://10.0.0.5:8080
 *
 * This used to be called Claude. It was renamed when a Claude of its own
 * arrived -- apps/claude.c talks to the model directly and can drive the
 * device; this one talks to Claude Code and can change it.
 *
 * Type /new to forget the conversation, /url <base> to move the server, and
 * /update (or ctrl-u) to install whatever the PC has built since -- which,
 * after "make the flippers stronger", is the point.
 */

#include "kernel/app/capp.h"

#define COLS        40            /* 240 pixels at six a character */
#define LINES      140            /* about eight screens of scrollback */
#define ROW_H        9
#define BAR_H       10
#define IN_H        11
#define INPUT_MAX  200
#define QUEUE_MAX    4           /* messages typed while one is running */
#define REPLY_MAX 4000

#define POLL_MS   1200

#define CLR_BG     CAPP_RGB(18, 20, 26)
#define CLR_BAR    CAPP_RGB(48, 40, 96)
#define CLR_FG     CAPP_RGB(226, 230, 240)
#define CLR_DIM    CAPP_RGB(132, 140, 158)
#define CLR_YOU    CAPP_RGB(140, 210, 150)
#define CLR_CLAUDE CAPP_RGB(226, 176, 110)
#define CLR_ERR    CAPP_RGB(232, 110, 100)
#define CLR_IN     CAPP_RGB(28, 32, 42)

static const CardApi *api;

/* Who said a line, which is all the colour it needs. */
enum { WHO_YOU = 0, WHO_CLAUDE, WHO_NOTE, WHO_ERR };

static struct {
  char  line[LINES][COLS + 1];
  unsigned char who[LINES];
  int   nlines;
  int   scroll;                   /* lines from the bottom */

  char  input[INPUT_MAX + 1];
  int   in_len;

  char  base[96];                 /* http://host:port */
  char  token[64];                /* from /config/claude.token, if there is one */

  int   job;                      /* the id being waited on, 0 for none */
  int   check_update;             /* an answer just landed: ask what is new */
  int   sending;                  /* a post is due on the next tick */
  char  pending[INPUT_MAX + 1];   /* what to post */
  char  queued[QUEUE_MAX][INPUT_MAX + 1];  /* sent one at a time, in order */
  int   nqueued;
  uint32_t next_poll;
  uint32_t started;
  int   dots;
  char  progress[31];             /* "step 2/3: writing timer.c"; the bar is
                                     40 columns, less "Build  " and dots */
  int   log_seen;                 /* the server's log lines already shown */

  char  reply[REPLY_MAX];
  char  status[40];

  CRect at;                       /* where we last painted */
  int   have_at;
  int   marked;                   /* something was marked since tick began */
} C;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

/* What the next paint has to redraw, said to the shell. A typed character is
 * the input line and nothing else; without this every keystroke redrew the
 * whole window, log and all, which on a 40x12 terminal is visible as a
 * flicker. These were three flags and an expect_paint guess; api->damage
 * says it outright, as apps/claude.c does. Before the first paint there is
 * nowhere to mark, and the shell paints everything anyway. */
static void mark_bar(void) { C.marked = 1; if (C.have_at) api->damage(rect(C.at.x, C.at.y, C.at.w, BAR_H)); }
static void mark_log(void) { C.marked = 1; if (C.have_at) api->damage(rect(C.at.x, C.at.y + BAR_H, C.at.w, C.at.h - BAR_H - IN_H)); }
static void mark_in(void)  { C.marked = 1; if (C.have_at) api->damage(rect(C.at.x, C.at.y + C.at.h - IN_H, C.at.w, IN_H)); }

/* ---- the log -------------------------------------------------------------- */

static void push(const char *text, int who) {
  int i;
  if (C.nlines == LINES) {
    /* Oldest out. A ring buffer would save the copying and cost a modulo in
     * every reader; at 140 lines of 41 bytes this is a memmove of 5 KB, once
     * per line, on a machine that is otherwise waiting for a network. */
    for (i = 1; i < LINES; i++) {
      api->mem_cpy(C.line[i - 1], C.line[i], COLS + 1);
      C.who[i - 1] = C.who[i];
    }
    C.nlines--;
  }
  api->fmt(C.line[C.nlines], COLS + 1, "%s", text);
  C.who[C.nlines] = (unsigned char)who;
  C.nlines++;
  mark_log();
}

/* Word wrap, because forty columns is narrow enough that breaking mid-word
 * makes prose genuinely hard to read. A word longer than a line is broken --
 * there is nothing else to do with a URL. */
static void push_wrapped(const char *text, int who) {
  char out[COLS + 1];
  int n = 0;
  size_t i = 0, len = api->str_len(text);

  while (i <= len) {
    char c = text[i];

    if (c == '\n' || c == 0) {
      out[n] = 0;
      push(out, who);
      n = 0;
      if (c == 0) return;
      i++;
      continue;
    }
    if (c == '\r') { i++; continue; }
    if (c == '\t') c = ' ';

    if (n == COLS) {
      /* Back up to the last space, if there is one worth backing up to. */
      int brk = n;
      while (brk > 0 && out[brk - 1] != ' ') brk--;
      if (brk > COLS / 3) {
        int keep = n - brk;
        char tail[COLS + 1];
        int k;
        for (k = 0; k < keep; k++) tail[k] = out[brk + k];
        out[brk ? brk - 1 : 0] = 0;
        push(out, who);
        for (k = 0; k < keep; k++) out[k] = tail[k];
        n = keep;
      } else {
        out[n] = 0;
        push(out, who);
        n = 0;
      }
    }
    out[n++] = c;
    i++;
  }
}

static void note(const char *s) { push_wrapped(s, WHO_NOTE); }

/* ---- talking to the server ------------------------------------------------ */

static void read_token(void) {
  int fd = api->open(CAPP_CONFIG "/claude.token", CAPP_O_READ);
  int n;
  if (fd < 0) return;
  n = api->read(fd, C.token, sizeof C.token - 1);
  api->close(fd);
  if (n < 0) n = 0;
  C.token[n] = 0;
  /* A file written by a text editor has a newline on the end; a token with a
   * newline in it matches nothing and the failure looks like a wrong secret
   * rather than a stray byte. */
  while (n > 0 && (C.token[n - 1] == '\n' || C.token[n - 1] == '\r' ||
                   C.token[n - 1] == ' '))
    C.token[--n] = 0;
}

static int online(void) {
  if (api->net_ready()) return 1;
  api->fmt(C.status, sizeof C.status, "wifi...");
  if (api->net_connect(20000) == 0) return 1;
  push_wrapped(api->net_status(), WHO_ERR);
  return 0;
}

/* Post the message and keep the id. Short: the server answers as soon as it
 * has queued the work, not when it has finished it. */
static void send_now(void) {
  char url[160];
  int n;

  C.sending = 0;
  mark_bar();                     /* status, then "thinking" */
  if (!online()) { C.status[0] = 0; return; }

  api->fmt(url, sizeof url, "%s/chat", C.base);
  n = api->http("POST", url, C.pending, "text/plain",
                C.token[0] ? C.token : (const char *)0,
                C.reply, sizeof C.reply, 20000);
  if (n < 0) {
    api->fmt(C.status, sizeof C.status, "send failed (%d)", n);
    push_wrapped("could not reach the server -- is it running?",
                 WHO_ERR);
    return;
  }
  if (C.reply[0] == 'i' && C.reply[1] == 'd') {
    C.job = 0;
    {
      const char *p = C.reply + 2;
      while (*p == ' ') p++;
      while (*p >= '0' && *p <= '9') { C.job = C.job * 10 + (*p - '0'); p++; }
    }
  }
  if (C.job <= 0) {
    push_wrapped(C.reply, WHO_ERR);
    C.status[0] = 0;
    return;
  }
  C.progress[0] = 0;              /* nothing said yet about this one */
  C.log_seen = 0;
  C.started = api->ticks_ms();
  C.next_poll = C.started + POLL_MS;
}

/* The next queued message into the send slot, if there is one. The send
 * itself happens on the next tick, as it does for a message typed now. */
static void send_next_queued(void) {
  int i;
  if (!C.nqueued) return;
  api->fmt(C.pending, sizeof C.pending, "%s", C.queued[0]);
  for (i = 1; i < C.nqueued; i++)
    api->fmt(C.queued[i - 1], INPUT_MAX + 1, "%s", C.queued[i]);
  C.nqueued--;
  C.sending = 1;
  api->fmt(C.status, sizeof C.status, "sending");
  mark_bar();
}

/* Ask whether it is done. Also short -- the server replies "pending" straight
 * away rather than holding the connection open, which is what keeps this
 * machine answering its keyboard while Claude works. */
static void poll_now(void) {
  char url[160];
  int n;

  api->fmt(url, sizeof url, "%s/chat?id=%d&from=%d", C.base, C.job, C.log_seen);
  n = api->http("GET", url, (const char *)0, (const char *)0,
                C.token[0] ? C.token : (const char *)0,
                C.reply, sizeof C.reply, 20000);
  if (n < 0) {
    api->fmt(C.status, sizeof C.status, "poll failed (%d)", n);
    C.next_poll = api->ticks_ms() + 3000;      /* it may just be a hiccup */
    return;
  }

  if (C.reply[0] == 'p' && C.reply[1] == 'e') {      /* "pending" */
    /* The second line, if the server sent one, is what it is doing: a
     * two-minute wait with nothing but dots looks the same as a hang. */
    const char *s = C.reply;
    int i = 0;
    while (*s && *s != '\n') s++;
    if (*s == '\n') s++;
    while (s[i] && s[i] != '\n' && i < (int)sizeof C.progress - 1) {
      C.progress[i] = s[i];
      i++;
    }
    C.progress[i] = 0;
    /* Then the log lines past the ones already shown, one per line: what
     * Claude read, searched, wrote and said. Dim, above where the answer
     * will land, so the wait is something to watch rather than dots. */
    while (*s && *s != '\n') s++;
    while (*s == '\n') {
      char line[COLS * 2 + 1];
      s++;
      for (i = 0; s[i] && s[i] != '\n' && i < (int)sizeof line - 1; i++) line[i] = s[i];
      line[i] = 0;
      while (*s && *s != '\n') s++;
      if (!line[0]) continue;
      push_wrapped(line, WHO_NOTE);
      C.log_seen++;
    }
    C.next_poll = api->ticks_ms() + POLL_MS;
    C.dots = (C.dots + 1) & 3;
    return;
  }
  C.progress[0] = 0;

  /* First line is the outcome, the rest is the answer. */
  {
    char *body = C.reply;
    int err = (C.reply[0] == 'e');
    while (*body && *body != '\n') body++;
    if (*body == '\n') body++;
    push_wrapped(body, err ? WHO_ERR : WHO_CLAUDE);
  }
  C.job = 0;
  C.scroll = 0;
  api->fmt(C.status, sizeof C.status, "%lus",
           (unsigned long)((api->ticks_ms() - C.started) / 1000u));
  C.check_update = 1;               /* on the next tick, not in this one */
  send_next_queued();
}

/* An answer may have ended with a build. Ask the proxy what it has that is
 * newer than what is running, and say so -- installing is a decision, and
 * a restart mid-thought is not one to take for the user. */
static void check_update_now(void) {
  char what[96], line[120];
  int n;
  C.check_update = 0;
  n = api->update_check(what, sizeof what);
  if (n <= 0) return;               /* nothing new, or no proxy: stay quiet */
  api->fmt(line, sizeof line, "new: %s -- /update installs", what);
  note(line);
}

static void install_update(void) {
  char what[96];
  int n;
  note("updating...");
  n = api->update_apply(1, what, sizeof what);
  if (n < 0) push_wrapped(what, WHO_ERR);
  else {
    char line[120];
    api->fmt(line, sizeof line, "%d app%s installed%s%s", n, n == 1 ? "" : "s",
             what[0] ? ": " : "", what);
    note(line);
  }
}

/* ---- painting -------------------------------------------------------------- */

static uint16_t colour_of(int who) {
  return who == WHO_YOU ? CLR_YOU
       : who == WHO_CLAUDE ? CLR_CLAUDE
       : who == WHO_ERR ? CLR_ERR : CLR_DIM;
}

static void fill_if(int x, int y, int w, int h, uint16_t c) {
  if (w > 0 && h > 0) api->fill(rect(x, y, w, h), c);
}

/* `s` padded with spaces to `cols` characters, so a line writes over the
 * one it replaces instead of the row being cleared first: there is no
 * framebuffer, and a fill followed by text is a blink on the panel. */
static void text_cols(int x, int y, const char *s, int cols, uint16_t fg, uint16_t bg) {
  char b[64];
  int n = 0;
  if (cols > (int)sizeof b - 1) cols = (int)sizeof b - 1;
  while (s[n] && n < (int)sizeof b - 1) { b[n] = s[n]; n++; }
  while (n < cols) b[n++] = ' ';
  b[n] = 0;
  api->text((short)x, (short)y, b, fg, bg);
}

/* What the bar says, worked out apart from drawing it so a tick can tell
 * whether a poll changed it. */
static void bar_text(char *bar, size_t n) {
  if (C.job && C.nqueued)
    api->fmt(bar, n, "Build  %.24s +%d",
             C.progress[0] ? C.progress : "thinking", C.nqueued);
  else if (C.job)
    api->fmt(bar, n, "Build  %s%s",
             C.progress[0] ? C.progress : "thinking",
             C.dots == 0 ? "" : C.dots == 1 ? "." : C.dots == 2 ? ".." : "...");
  else if (C.status[0])
    api->fmt(bar, n, "Build  %s", C.status);
  else
    api->fmt(bar, n, "Build  %s", C.base);
}

/* The bar is written over itself, padded to its width, with only the pixel
 * rows above and below the text and its margins filled. It used to be
 * filled and then written, and the dots move every poll. */
static void paint_bar(CRect c) {
  char bar[64];
  int cols = (c.w - 3 + 5) / 6;          /* the last, cut by the edge, still padded */
  if (cols > 63) cols = 63;
  bar_text(bar, sizeof bar);
  fill_if(c.x, c.y, c.w, 1, CLR_BAR);
  fill_if(c.x, c.y + 9, c.w, BAR_H - 9, CLR_BAR);
  fill_if(c.x, c.y + 1, 3, 8, CLR_BAR);
  fill_if(c.x + 3 + cols * 6, c.y + 1, c.w - 3 - cols * 6, 8, CLR_BAR);
  text_cols(c.x + 3, c.y + 1, bar, cols, CLR_FG, CLR_BAR);
}

/* The newest line sits just above the input box; scroll moves the window
 * back through the log. Each line is padded to the width and written over
 * the one before it; only the margins, the pixel row under each line and
 * the rows below the last are filled -- clearing the whole log first
 * blinked it on every answer and every log line a poll brought. */
static void paint_log(CRect c, CRect clip) {
  int rows = (c.h - BAR_H - IN_H) / ROW_H;
  int top = c.y + BAR_H, bottom = c.y + c.h - IN_H;
  int cols = (c.w - 2 + 5) / 6, right;   /* to the edge: a 40-column line reaches it */
  int first, r, y = top;

  if (cols > 63) cols = 63;
  right = c.x + 2 + cols * 6;
  first = C.nlines - rows - C.scroll;
  if (first < 0) first = 0;
  fill_if(c.x, top, 2, bottom - top, CLR_BG);
  fill_if(right, top, c.x + c.w - right, bottom - top, CLR_BG);
  for (r = 0; r < rows; r++) {
    int i = first + r;
    if (i >= C.nlines) break;
    y = top + r * ROW_H;
    if (y < clip.y + clip.h && y + ROW_H > clip.y) {
      text_cols(c.x + 2, y, C.line[i], cols, colour_of(C.who[i]), CLR_BG);
      fill_if(c.x + 2, y + 8, cols * 6, ROW_H - 8, CLR_BG);
    }
    y += ROW_H;
  }
  fill_if(c.x + 2, y, cols * 6, bottom - y, CLR_BG);
}

/* The input line, showing the tail of what has been typed: the prompt, the
 * text, the cursor, then spaces to the end -- each drawn over the last, so
 * a keystroke does not blank the line. */
static void paint_input(CRect c) {
  int y = c.y + c.h - IN_H;
  int vis = (c.w - 12) / 6;
  int from = C.in_len > vis ? C.in_len - vis : 0;
  int n = C.in_len - from, cx = c.x + 10 + n * 6, end;
  fill_if(c.x, y, c.w, 2, CLR_IN);
  fill_if(c.x, y + 10, c.w, IN_H - 10, CLR_IN);
  fill_if(c.x, y + 2, 2, 8, CLR_IN);
  api->text((short)(c.x + 2), (short)(y + 2), ">", CLR_DIM, CLR_IN);
  fill_if(c.x + 8, y + 2, 2, 8, CLR_IN);
  api->text((short)(c.x + 10), (short)(y + 2), C.input + from, CLR_FG, CLR_IN);
  api->fill(rect(cx, y + 2, 5, 8), CLR_FG);
  text_cols(cx + 5, y + 2, "", vis - n, CLR_FG, CLR_IN);
  end = cx + 5 + (vis > n ? vis - n : 0) * 6;
  fill_if(end, y + 2, c.x + c.w - end, 8, CLR_IN);
}

/* Painted to the clip the shell hands back: our own marks come back as the
 * regions they named, and a paint we did not ask for -- the window moved,
 * the help overlay closed -- comes with the whole window. */
static void app_paint(void *st, CRect c) {
  CRect clip = api->paint_area();
  (void)st;
  C.at = c;
  C.have_at = 1;
  if (clip.y < c.y + BAR_H) paint_bar(c);
  if (clip.y < c.y + c.h - IN_H && clip.y + clip.h > c.y + BAR_H) paint_log(c, clip);
  if (clip.y + clip.h > c.y + c.h - IN_H) paint_input(c);
}

/* ---- input ----------------------------------------------------------------- */

static void submit(void) {
  if (!C.in_len) return;
  mark_in();                      /* the line is cleared whichever way this goes */

  /* Two client-side commands. Everything else is for Claude. */
  if (C.input[0] == '/' && C.input[1] == 'n') {
    char url[160];
    api->fmt(url, sizeof url, "%s/chat/new", C.base);
    api->http("GET", url, (const char *)0, (const char *)0,
              C.token[0] ? C.token : (const char *)0,
              C.reply, sizeof C.reply, 15000);
    note("-- new conversation --");
    C.nqueued = 0;                  /* nothing typed for the old one carries over */
    mark_bar();
    C.in_len = 0;
    C.input[0] = 0;
    return;
  }
  if (C.input[0] == '/' && C.input[1] == 'u' && C.input[2] == 'p') {
    C.in_len = 0;
    C.input[0] = 0;
    install_update();               /* may not return: a firmware restarts */
    return;
  }
  if (C.input[0] == '/' && C.input[1] == 'u') {
    const char *p = C.input + 1;
    while (*p >= 'a' && *p <= 'z') p++;   /* the word "url" -- and only it: */
    while (*p == ' ') p++;                /* eating letters ate "http" too */
    if (*p) {
      api->fmt(C.base, sizeof C.base, "%s", p);
      note(C.base);
      mark_bar();                 /* the bar shows the address */
    }
    C.in_len = 0;
    C.input[0] = 0;
    return;
  }

  /* A request is running: hold this one rather than send it. Sending it
   * started a second job and polled that one instead, so the first answer
   * -- "published: timer" and all -- was never collected. */
  if (C.job || C.sending) {
    if (C.nqueued >= QUEUE_MAX) {
      push_wrapped("the queue is full -- wait for an answer", WHO_ERR);
      return;                       /* the text stays in the input line */
    }
    api->fmt(C.queued[C.nqueued++], INPUT_MAX + 1, "%s", C.input);
    push_wrapped(C.input, WHO_YOU);
    note(C.nqueued == 1 ? "queued: sent when this answer lands"
                        : "queued behind the others");
    C.in_len = 0;
    C.input[0] = 0;
    C.scroll = 0;
    mark_bar();
    return;
  }

  push_wrapped(C.input, WHO_YOU);
  api->fmt(C.pending, sizeof C.pending, "%s", C.input);
  C.in_len = 0;
  C.input[0] = 0;
  C.scroll = 0;
  /* Not sent here: sending blocks for a moment, and doing it on the next tick
   * means the screen has already shown the message and said "thinking". */
  C.sending = 1;
  api->fmt(C.status, sizeof C.status, "sending");
  mark_bar();
}

static int app_key(void *st, unsigned char k) {
  (void)st;

  if (k == CAPP_KEY_ENTER) { submit(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (C.in_len) C.input[--C.in_len] = 0;
    mark_in();
    return 1;
  }
  if (k == 0x15) { install_update(); return 1; }  /* ctrl-u: same as /update */
  /* With nothing typed the arrows are a scrollback, which is what they should
   * be on a screen this size. Mid-word they are characters, because ; . , /
   * are the arrow keys on this machine and an address needs full stops. */
  if (!C.in_len) {
    int rows = 12;
    if (k == CAPP_KEY_UP)   { C.scroll += 3; mark_log(); return 1; }
    if (k == CAPP_KEY_DOWN) { C.scroll -= 3; if (C.scroll < 0) C.scroll = 0; mark_log(); return 1; }
    if (k == CAPP_KEY_LEFT) { C.scroll += rows; mark_log(); return 1; }
    if (k == CAPP_KEY_RIGHT) {
      C.scroll -= rows;
      if (C.scroll < 0) C.scroll = 0;
      mark_log();
      return 1;
    }
  }
  if (k >= ' ' && k < 0x7F && C.in_len < INPUT_MAX) {
    C.input[C.in_len++] = (char)k;
    C.input[C.in_len] = 0;
    mark_in();
    return 1;
  }
  return 0;
}

/* A repaint only when something on screen changed. Every poll used to ask
 * for one, though a "pending" with nothing new in it changes nothing -- and
 * the bar was marked at the start of each poll whatever came back. Now the
 * bar is marked when its words differ, and anything that pushed a line or
 * marked otherwise says so through C.marked. */
static int app_tick(void *st, uint32_t now) {
  char before[64], after[64];
  int i;
  (void)st;

  if (!C.sending && !C.check_update &&
      !(C.job && (int32_t)(now - C.next_poll) >= 0))
    return 0;
  bar_text(before, sizeof before);
  C.marked = 0;
  if (C.sending) send_now();
  else if (C.check_update) check_update_now();
  else poll_now();
  bar_text(after, sizeof after);
  for (i = 0; before[i] && before[i] == after[i]; i++) ;
  if (before[i] != after[i]) mark_bar();
  return C.marked;
}

/* A terminal is always taking text: the prompt is open before the first
 * character is typed. This used to say "only once something is typed" so that
 * bare ; . , / could scroll the log -- but voice asks the same question, and
 * "no" here meant every spoken sentence was heard and then dropped. Fn plus
 * those keys still scrolls, and a prompt can now start with a semicolon. */
static int app_wants_text(void *st) {
  (void)st;
  return 1;
}

static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  (void)st; (void)x; (void)y; (void)buttons;
  if (!wheel) return 0;
  C.scroll += wheel * 3;
  if (C.scroll < 0) C.scroll = 0;
  mark_log();
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_NEEDS_PROXY,   /* a window; the agent it talks to lives on the PC */
  "Build",
  /* 16x16: a speech bubble with a hammer in it. */
  { 0x0F, 0xF0, 0x38, 0x1C, 0x60, 0x06, 0x47, 0xC2,
    0x87, 0xC1, 0x87, 0xC1, 0x81, 0x01, 0x81, 0x01,
    0x81, 0x01, 0x81, 0x01, 0x40, 0x02, 0x60, 0x06,
    0x38, 0x1C, 0x1C, 0xF0, 0x07, 0x00, 0x03, 0x00 },
  "enter\tsend\narrows\tscrollback, when nothing is typed\n"
  "/new\tforget the conversation\n/url X\tpoint at another server\n"
  "/update, ctrl-u\tinstall whatever the PC has built\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  api->mem_set(&C, 0, sizeof C);

  api->fmt(C.base, sizeof C.base, "%s", api->proxy());
  if (argc > 1 && argv[1][0]) api->fmt(C.base, sizeof C.base, "%s", argv[1]);
  read_token();

  note("Claude, through the server on your PC.");
  note("It can read and edit the CardOS folder.");
  note("Type and press enter.");

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
