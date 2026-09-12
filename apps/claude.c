/* Claude -- talk to the machine that builds this one.
 *
 * The model does not run here and never could. What runs here is a terminal:
 * it posts what you typed to tools/webproxy.py on a PC, which hands it to
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
 *     claude                  the proxy compiled in
 *     claude <base-url>       somewhere else, e.g. http://10.0.0.5:8080
 *
 * Type /new to forget the conversation, /url <base> to move the server, and
 * /update to install whatever the PC has built since -- which, after "make
 * the flippers stronger", is the point.
 */

#include "kernel/app/capp.h"

#define COLS        40            /* 240 pixels at six a character */
#define LINES      140            /* about eight screens of scrollback */
#define ROW_H        9
#define BAR_H       10
#define IN_H        11
#define INPUT_MAX  200
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
  char  token[64];                /* from /claude.token, if there is one */

  int   job;                      /* the id being waited on, 0 for none */
  int   check_update;             /* an answer just landed: ask what is new */
  int   sending;                  /* a post is due on the next tick */
  char  pending[INPUT_MAX + 1];   /* what to post */
  uint32_t next_poll;
  uint32_t started;
  int   dots;

  char  reply[REPLY_MAX];
  char  status[40];

  /* What the next paint has to redraw. A typed character is the input line
   * and nothing else; without this every keystroke redrew the whole window,
   * log and all, which on a 40x12 terminal is visible as a flicker. */
  int   dirty_bar, dirty_log, dirty_in;
  int   expect_paint;             /* this paint answers something we did */
  int   full;                     /* draw everything regardless */
  CRect at;                       /* where we last painted */
  int   have_at;
} C;

static void mark_bar(void) { C.dirty_bar = 1; }
static void mark_log(void) { C.dirty_log = 1; }
static void mark_in(void)  { C.dirty_in = 1; }

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

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
  int fd = api->open("/claude.token", CAPP_O_READ);
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
    push_wrapped("could not reach the server -- is webproxy.py running?",
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
  C.started = api->ticks_ms();
  C.next_poll = C.started + POLL_MS;
}

/* Ask whether it is done. Also short -- the server replies "pending" straight
 * away rather than holding the connection open, which is what keeps this
 * machine answering its keyboard while Claude works. */
static void poll_now(void) {
  char url[160];
  int n;

  mark_bar();                     /* the dots, the time, or an error */
  api->fmt(url, sizeof url, "%s/chat?id=%d", C.base, C.job);
  n = api->http("GET", url, (const char *)0, (const char *)0,
                C.token[0] ? C.token : (const char *)0,
                C.reply, sizeof C.reply, 20000);
  if (n < 0) {
    api->fmt(C.status, sizeof C.status, "poll failed (%d)", n);
    C.next_poll = api->ticks_ms() + 3000;      /* it may just be a hiccup */
    return;
  }

  if (C.reply[0] == 'p' && C.reply[1] == 'e') {      /* "pending" */
    C.next_poll = api->ticks_ms() + POLL_MS;
    C.dots = (C.dots + 1) & 3;
    return;
  }

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

static void paint_bar(CRect c) {
  char bar[64];
  api->fill(rect(c.x, c.y, c.w, BAR_H), CLR_BAR);
  if (C.job)
    api->fmt(bar, sizeof bar, "Claude  thinking%s",
             C.dots == 0 ? "" : C.dots == 1 ? "." : C.dots == 2 ? ".." : "...");
  else if (C.status[0])
    api->fmt(bar, sizeof bar, "Claude  %s", C.status);
  else
    api->fmt(bar, sizeof bar, "Claude  %s", C.base);
  api->text((short)(c.x + 3), (short)(c.y + 1), bar, CLR_FG, CLR_BAR);
}

/* The newest line sits just above the input box; scroll moves the window
 * back through the log. */
static void paint_log(CRect c) {
  int rows = (c.h - BAR_H - IN_H) / ROW_H;
  int first, r;

  api->fill(rect(c.x, c.y + BAR_H, c.w, c.h - BAR_H - IN_H), CLR_BG);
  first = C.nlines - rows - C.scroll;
  if (first < 0) first = 0;
  for (r = 0; r < rows; r++) {
    int i = first + r;
    if (i >= C.nlines) break;
    api->text((short)(c.x + 2), (short)(c.y + BAR_H + r * ROW_H),
              C.line[i], colour_of(C.who[i]), CLR_BG);
  }
}

/* The input line, showing the tail of what has been typed. */
static void paint_input(CRect c) {
  int y = c.y + c.h - IN_H;
  int vis = (c.w - 12) / 6;
  int from = C.in_len > vis ? C.in_len - vis : 0;
  api->fill(rect(c.x, y, c.w, IN_H), CLR_IN);
  api->text((short)(c.x + 2), (short)(y + 2), ">", CLR_DIM, CLR_IN);
  api->text((short)(c.x + 10), (short)(y + 2), C.input + from, CLR_FG, CLR_IN);
  api->fill(rect(c.x + 10 + (C.in_len - from) * 6, y + 2, 5, 8), CLR_FG);
}

/* A paint we did not ask for -- the window moved, the help overlay closed,
 * the launcher cleared the screen -- has to be the whole thing, because only
 * the shell knows what was drawn over us and it does not say. One that
 * answers our own key or tick redraws the regions that changed. */
static void app_paint(void *st, CRect c) {
  (void)st;

  if (!C.have_at || c.x != C.at.x || c.y != C.at.y || c.w != C.at.w ||
      c.h != C.at.h)
    C.full = 1;
  C.at = c;
  C.have_at = 1;
  if (!C.expect_paint) C.full = 1;
  C.expect_paint = 0;

  if (C.full) {
    C.full = 0;
    C.dirty_bar = C.dirty_log = C.dirty_in = 0;
    paint_bar(c);
    paint_log(c);
    paint_input(c);
    return;
  }
  if (C.dirty_bar) { C.dirty_bar = 0; paint_bar(c); }
  if (C.dirty_log) { C.dirty_log = 0; paint_log(c); }
  if (C.dirty_in)  { C.dirty_in = 0;  paint_input(c); }
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
    const char *p = C.input + 2;
    while (*p == ' ' || (*p >= 'a' && *p <= 'z')) p++;   /* skip "/url " */
    if (*p) {
      api->fmt(C.base, sizeof C.base, "%s", p);
      note(C.base);
      mark_bar();                 /* the bar shows the address */
    }
    C.in_len = 0;
    C.input[0] = 0;
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
  C.expect_paint = 1;

  if (k == CAPP_KEY_ENTER) { submit(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (C.in_len) C.input[--C.in_len] = 0;
    mark_in();
    return 1;
  }
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
  C.expect_paint = 0;
  return 0;
}

static int app_tick(void *st, uint32_t now) {
  (void)st;

  if (C.sending) { send_now(); C.expect_paint = 1; return 1; }
  if (C.check_update) { check_update_now(); C.expect_paint = 1; return 1; }
  if (C.job && (int32_t)(now - C.next_poll) >= 0) {
    poll_now();
    C.expect_paint = 1;
    return 1;
  }
  return 0;
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
  C.expect_paint = 1;
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_NEEDS_PROXY,   /* a window; the agent it talks to lives on the PC */
  "Claude",
  /* 16x16: a speech bubble with a tail. */
  { 0x0F, 0xF0, 0x38, 0x1C, 0x60, 0x06, 0x40, 0x02,
    0x8F, 0xF1, 0x80, 0x01, 0x9F, 0xF9, 0x80, 0x01,
    0x8F, 0xF1, 0x80, 0x01, 0x40, 0x02, 0x60, 0x06,
    0x38, 0x1C, 0x1C, 0xF0, 0x07, 0x00, 0x03, 0x00 },
  "enter\tsend\narrows\tscrollback, when nothing is typed\n"
  "/new\tforget the conversation\n/url X\tpoint at another server\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  api->mem_set(&C, 0, sizeof C);

  api->fmt(C.base, sizeof C.base, "%s", CAPP_PROXY_DEFAULT);
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
