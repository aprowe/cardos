/* Claude -- a terminal to the model, and through it to the machine.
 *
 * Nothing about the conversation lives here. The kernel's agent
 * (kernel/sys/agent.h) keeps it -- on the card, and posts it to
 * api.anthropic.com over the device's own HTTPS with no PC in the path --
 * and this is a window onto it: the transcript, a prompt line, a status
 * word. That is why "open Todo" works from here: the agent runs from the
 * shell's loop whichever app has the screen, so when Todo takes over this
 * app simply stops being shown, and the conversation carries on without it.
 * Come back and the transcript is where you left it.
 *
 * The model can act on the device through a fixed list of tools -- open an
 * app, run one of its actions, type, press a key, switch shell, brightness,
 * WiFi -- each of them something a keyboard could have done, and each shown
 * here as "-> what it did" as it happens.
 *
 * Type /new to forget the conversation. The key lives in /config/claude.key
 * on the card and is sent to Anthropic and nowhere else.
 *
 * Opened with a file -- `claude PATH`, or `claude -k song PATH` -- it is
 * something else: a conversation about that document, which another app
 * handed over (Edit's ctrl-k, MIDI's c). The document goes to the CardOS
 * server (server/talk.py), Claude answers there with no tools, and a
 * revised document waits on the server until ctrl-s saves it over the
 * file; ctrl-z puts the file back. fn-` returns to the app that asked,
 * which opens the file again and finds it changed.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/safefile.h"

#define TL_LINES    80            /* the agent keeps ~2 KB; this is its wrap */
#include "apps/termlog.h"

#define BAR_H      TL_BAR_H
#define IN_H       TL_IN_H
#define INPUT_MAX  200

#define CLR_BG     CAPP_RGB(18, 20, 26)
#define CLR_BAR    CAPP_RGB(96, 56, 40)
#define CLR_FG     CAPP_RGB(226, 230, 240)
#define CLR_DIM    CAPP_RGB(132, 140, 158)
#define CLR_YOU    CAPP_RGB(140, 210, 150)
#define CLR_CLAUDE CAPP_RGB(226, 176, 110)
#define CLR_ERR    CAPP_RGB(232, 110, 100)
#define CLR_IN     CAPP_RGB(28, 32, 42)

static const CardApi  *api;
static const CappAgent *agent;

enum { WHO_YOU = 0, WHO_CLAUDE, WHO_TOOL, WHO_ERR };

static struct {
  TermLog log;                    /* the lines, who said each, the scroll */

  char  input[INPUT_MAX + 1];
  int   in_len;

  unsigned seen_gen;              /* the transcript generation we drew */
  int   no_key;
  int   dots;
  uint32_t dot_at;

  CRect at;
  int   have_at;

  /* about a document */
  int   talk;                     /* opened with a file */
  char  path[96], name[48], kind[8];
  char  sid[16];                  /* the server's conversation */
  int   started, failed, waiting, have_rev, can_undo;
  uint32_t next_poll;
} C;

#define TALK_REV   "/cache/talk-rev.txt"
#define TALK_UNDO  "/cache/talk-undo.txt"
#define REPLY_MAX  3072

static char reply[REPLY_MAX];

/* ---- the log, rebuilt from the agent's transcript ------------------------ */

static void push(const char *text, int who) { tl_push(api, &C.log, text, who); }

/* One line of `text`, to a newline or the end, word-wrapped (apps/termlog.h);
 * what follows it, the end of the text at the end. */
static const char *push_wrapped(const char *text, int who) {
  const char *next = tl_wrap_line(api, &C.log, text, who, 0);
  return next ? next : text + api->str_len(text);
}

static void rebuild(void) {
  const char *t = agent->transcript();
  C.log.n = 0;
  if (C.no_key) {
    push_wrapped("No key. Put an Anthropic API key in /config/claude.key on the card and open this again.", WHO_ERR);
    return;
  }
  while (*t) {
    int who = WHO_CLAUDE;
    if (t[0] == '>' && t[1] == ' ') { who = WHO_YOU; t += 2; }
    else if (t[0] == '-' && t[1] == '>' && t[2] == ' ') { who = WHO_TOOL; t += 3; }
    t = push_wrapped(t, who);
  }
  C.seen_gen = agent->generation();
}

/* ---- painting ------------------------------------------------------------- */

static uint16_t colour_of(int who) {
  return who == WHO_YOU ? CLR_YOU
       : who == WHO_CLAUDE ? CLR_CLAUDE
       : who == WHO_ERR ? CLR_ERR : CLR_DIM;
}

/* Written over itself (apps/termlog.h): the dots move every 400 ms, and
 * filling the bar before writing it blinked it each time. */
static void paint_bar(CRect c) {
  char bar[64];
  const char *s = agent->status();
  if (C.talk)
    api->fmt(bar, sizeof bar, "Claude  %s%s%s", C.waiting ? "thinking" : C.name,
             !C.waiting || C.dots == 0 ? "" : C.dots == 1 ? "." : C.dots == 2 ? ".." : "...",
             !C.waiting && C.have_rev ? "  ctrl-s saves" : "");
  else if (s[0])
    api->fmt(bar, sizeof bar, "Claude  %s%s", s,
             C.dots == 0 ? "" : C.dots == 1 ? "." : C.dots == 2 ? ".." : "...");
  else
    api->fmt(bar, sizeof bar, "Claude  api.anthropic.com");
  tl_paint_bar(api, c, bar, CLR_FG, CLR_BAR);
}

/* Painted to the clip the shell hands back: our own damage marks come back
 * as the region they named, and anything else is the whole window. */
static void app_paint(void *st, CRect c) {
  CRect clip = api->paint_area();
  (void)st;
  C.at = c;
  C.have_at = 1;
  if (clip.y < c.y + BAR_H) paint_bar(c);
  if (clip.y < c.y + c.h - IN_H && clip.y + clip.h > c.y + BAR_H)
    tl_paint_log(api, &C.log, c, clip, colour_of, CLR_BG);
  if (clip.y + clip.h > c.y + c.h - IN_H)
    tl_paint_input(api, c, ">", C.input, C.in_len, CLR_FG, CLR_DIM, CLR_IN);
}

static void damage_in(void)  { if (C.have_at) api->damage(capp_rect(C.at.x, C.at.y + C.at.h - IN_H, C.at.w, IN_H)); }
static void damage_log(void) { if (C.have_at) api->damage(capp_rect(C.at.x, C.at.y + BAR_H, C.at.w, C.at.h - BAR_H - IN_H)); }
static void damage_bar(void) { if (C.have_at) api->damage(capp_rect(C.at.x, C.at.y, C.at.w, BAR_H)); }


/* ---- about a document ---------------------------------------------------------- */

static void say_line(const char *s, int who) {
  while (*s) s = push_wrapped(s, who);
  damage_log();
}

/* `from` over `to`, through a .tmp so a power cut leaves one or the other. */
static int copy_file(const char *from, const char *to) {
  static char buf[512];
  SafeFile f;
  int fd = safe_open_read(api, from), r, ok = 0;
  if (fd < 0) return -1;
  if (api->str_len(to) < SF_PATH_MAX) {
    if (safe_begin(&f, api, to) != 0) { api->close(fd); return -1; }
    while ((r = api->read(fd, buf, sizeof buf)) > 0) safe_write(&f, buf, (size_t)r);
    api->close(fd);
    return safe_commit(&f);
  }
  {
    int out = api->open(to, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
    if (out < 0) { api->close(fd); return -1; }
    while ((r = api->read(fd, buf, sizeof buf)) > 0) if (api->write(out, buf, (size_t)r) != r) ok = -1;
    api->close(out);
  }
  api->close(fd);
  return ok;
}

/* The document to the server, once there is a screen to say so on. */
static void talk_start(void) {
  char url[200], nm[100];
  int r, i;
  C.started = 1;
  if (!api->net_ready() && api->net_connect(15000) != 0) {
    C.failed = 1;
    say_line("Offline: talking about a document needs the CardOS server.", WHO_ERR);
    return;
  }
  url_enc(nm, sizeof nm, C.name);
  api->fmt(url, sizeof url, "%s/talk/start?name=%s&kind=%s", api->proxy(), nm, C.kind);
  r = api->http_upload(url, C.path, "text/plain", reply, sizeof reply, 20000);
  if (r <= 0 || reply[0] == 'e' || reply[0] == '<') {
    C.failed = 1;
    say_line(r == -413 ? "The document is too long to send." : "The server did not take the document.", WHO_ERR);
    return;
  }
  for (i = 0; reply[i] && reply[i] != '\n' && i < (int)sizeof C.sid - 1; i++) C.sid[i] = reply[i];
  C.sid[i] = 0;
}

static void talk_say(void) {
  char url[160];
  int r;
  if (C.failed || !C.sid[0]) { push("(not connected: fn-` and try again)", WHO_ERR); return; }
  if (C.waiting) { push("(still thinking about the last one)", WHO_ERR); return; }
  api->fmt(url, sizeof url, "%s/talk/say?s=%s", api->proxy(), C.sid);
  r = api->http("POST", url, C.input, "text/plain", "", reply, 64, 15000);
  push_wrapped(C.input, WHO_YOU);
  if (r < 0 || reply[0] != 'o') {
    say_line(r == -404 ? "That conversation is over: fn-` and open it again." : "Could not send it.", WHO_ERR);
    return;
  }
  C.waiting = 1;
  C.next_poll = api->ticks_ms() + 1500;
}

/* The screen keeps drawing while it is dark (fn-o, fn-c, the idle
 * timeout), same as it does for the main agent's own conversation --
 * kernel/sys/agent.c's notify_done -- which this has no equivalent to:
 * a document conversation is this app's own state (C.sid and all),
 * gone the moment the app closes, so unlike the main agent it cannot
 * notify once closed, only while open and ticking (foreground in the
 * launcher, or any window at all on the desktop). The title says which
 * document and the text is the reply itself, cut to what the banner can
 * hold. */
static void notify_doc_reply(const char *text) {
  char title[32];
  api->fmt(title, sizeof title, "on %s", C.name);
  api->notify(title, text);
}

static void talk_poll(void) {
  char url[160];
  const char *body;
  int r, rev;
  api->fmt(url, sizeof url, "%s/talk/poll?s=%s", api->proxy(), C.sid);
  r = api->http("GET", url, 0, 0, "", reply, sizeof reply - 1, 15000);
  if (r < 0) { C.next_poll = api->ticks_ms() + 3000; return; }
  reply[r < (int)sizeof reply ? r : (int)sizeof reply - 1] = 0;
  if (reply[0] == 'p') { C.next_poll = api->ticks_ms() + 1500; return; }
  C.waiting = 0;
  damage_bar();
  if (reply[0] == 'e') {
    body = reply[5] ? reply + 6 : "Claude did not answer.";
    say_line(body, WHO_ERR);
    notify_doc_reply(body);
    return;
  }
  if (reply[0] != 'r') return;
  rev = reply[5] == ' ';
  body = reply;
  while (*body && *body != '\n') body++;
  if (*body) body++;
  say_line(body, WHO_CLAUDE);
  notify_doc_reply(body[0] ? body : "a revision is ready: ctrl-s saves it");
  if (rev) {
    C.have_rev = 1;
    say_line("[a revision is ready: ctrl-s saves it]", WHO_TOOL);
  }
}

/* ctrl-s: the revision over the file, the file kept for ctrl-z first. */
static void talk_save(void) {
  char url[160], line[80];
  int r;
  if (!C.have_rev) { say_line("[nothing to save yet: ask for a change]", WHO_TOOL); return; }
  api->fmt(url, sizeof url, "%s/talk/doc?s=%s", api->proxy(), C.sid);
  r = api->http_download(url, TALK_REV, 20000);
  if (r <= 0) { say_line("Could not fetch the revision.", WHO_ERR); return; }
  if (copy_file(C.path, TALK_UNDO) != 0) { say_line("Could not keep a copy; not saved.", WHO_ERR); return; }
  if (copy_file(TALK_REV, C.path) != 0) { say_line("Could not write the file.", WHO_ERR); return; }
  C.have_rev = 0;
  C.can_undo = 1;
  api->fmt(line, sizeof line, "[saved to %s; ctrl-z puts it back]", C.name);
  say_line(line, WHO_TOOL);
  damage_bar();
}

static void talk_undo(void) {
  if (!C.can_undo) { say_line("[nothing to undo]", WHO_TOOL); return; }
  if (copy_file(TALK_UNDO, C.path) != 0) { say_line("Could not put it back.", WHO_ERR); return; }
  C.can_undo = 0;
  say_line("[put back as it was before]", WHO_TOOL);
}

/* `claude [-k KIND] PATH...`: the rest of the words are the path, spaces
 * and all, since a note's name may have them. */
static void talk_args(int argc, char **argv) {
  int i = 1, k = 0;
  const char *base;
  api->fmt(C.kind, sizeof C.kind, "text");
  if (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'k' && !argv[1][2]) {
    api->fmt(C.kind, sizeof C.kind, "%s", argv[2]);
    i = 3;
  }
  for (; i < argc; i++) {
    if (k) C.path[k++] = ' ';
    k += api->fmt(C.path + k, sizeof C.path - (size_t)k, "%s", argv[i]);
    if (k >= (int)sizeof C.path - 1) break;
  }
  if (!C.path[0]) return;
  C.talk = 1;
  for (base = C.path, i = 0; C.path[i]; i++) if (C.path[i] == '/') base = C.path + i + 1;
  api->fmt(C.name, sizeof C.name, "%s", base);
  {
    char line[96];
    api->fmt(line, sizeof line, "About %s. Ask a question, or ask for a change.", C.name);
    push_wrapped(line, WHO_TOOL);
    push_wrapped("ctrl-s saves Claude's revision, ctrl-z undoes it. fn-` goes back.", WHO_TOOL);
  }
}

/* ---- input ------------------------------------------------------------------ */

static void submit(void) {
  int rc;
  if (!C.in_len) return;
  damage_in();

  if (C.talk) {
    C.log.scroll = 0;
    talk_say();
    C.in_len = 0; C.input[0] = 0;
    damage_log(); damage_bar();
    return;
  }

  if (C.input[0] == '/' && C.input[1] == 'n') {
    agent->reset();
    C.in_len = 0; C.input[0] = 0;
    rebuild(); damage_log();
    return;
  }

  rc = agent->ask(C.input);
  C.in_len = 0; C.input[0] = 0;
  C.log.scroll = 0;
  if (rc == -1) push("(still thinking about the last one)", WHO_ERR);
  else if (rc == -2) { C.no_key = 1; rebuild(); }
  else if (rc == -3) push("no card", WHO_ERR);
  else if (rc < 0) push("could not send", WHO_ERR);
  damage_log();
  damage_bar();
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (k == CAPP_KEY_ENTER) { submit(); return 1; }
  if (C.talk && k == 0x13) { talk_save(); return 1; }     /* ctrl-s */
  if (C.talk && k == 0x1A) { talk_undo(); return 1; }     /* ctrl-z */
  if (k == CAPP_KEY_BACK) {
    if (C.in_len) C.input[--C.in_len] = 0;
    damage_in();
    return 1;
  }
  if (!C.in_len) {
    int rows = 12;
    if (k == CAPP_KEY_UP)    { C.log.scroll += 3; damage_log(); return 1; }
    if (k == CAPP_KEY_DOWN)  { C.log.scroll -= 3; if (C.log.scroll < 0) C.log.scroll = 0; damage_log(); return 1; }
    if (k == CAPP_KEY_LEFT)  { C.log.scroll += rows; damage_log(); return 1; }
    if (k == CAPP_KEY_RIGHT) { C.log.scroll -= rows; if (C.log.scroll < 0) C.log.scroll = 0; damage_log(); return 1; }
  }
  if (k >= ' ' && k < 0x7F && C.in_len < INPUT_MAX) {
    C.input[C.in_len++] = (char)k;
    C.input[C.in_len] = 0;
    damage_in();
    return 1;
  }
  return 0;
}

static int app_tick(void *st, uint32_t now) {
  int changed = 0;
  (void)st;
  if (C.talk) {
    if (!C.started && C.have_at) { talk_start(); return 1; }
    if (C.waiting && (int32_t)(now - C.next_poll) >= 0) { talk_poll(); return 1; }
    if (C.waiting && (int32_t)(now - C.dot_at) >= 400) {
      C.dot_at = now;
      C.dots = (C.dots + 1) & 3;
      damage_bar();
      return 1;
    }
    return 0;
  }
  agent->seen();
  if (agent->generation() != C.seen_gen) {
    rebuild();
    C.log.scroll = 0;
    damage_log(); damage_bar();
    changed = 1;
  }
  if (agent->busy() && (int32_t)(now - C.dot_at) >= 400) {
    C.dot_at = now;
    C.dots = (C.dots + 1) & 3;
    damage_bar();
    changed = 1;
  }
  return changed;
}

/* Always taking text, so a spoken sentence reaches an empty prompt. */
static int app_wants_text(void *st) { (void)st; return 1; }

static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  (void)st; (void)x; (void)y; (void)buttons;
  if (!wheel) return 0;
  C.log.scroll += wheel > 0 ? 3 : -3;
  if (C.log.scroll < 0) C.log.scroll = 0;
  damage_log();
  return 1;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_NEEDS_NET,
  "Claude",
  /* 16x16: a speech bubble with a tail. */
  { 0x0F, 0xF0, 0x38, 0x1C, 0x60, 0x06, 0x40, 0x02,
    0x8F, 0xF1, 0x80, 0x01, 0x9F, 0xF9, 0x80, 0x01,
    0x8F, 0xF1, 0x80, 0x01, 0x40, 0x02, 0x60, 0x06,
    0x38, 0x1C, 0x1C, 0xF0, 0x07, 0x00, 0x03, 0x00 },
  "enter\tsend\narrows\tscrollback, when nothing is typed\n"
  "/new\tforget the conversation\n"
  "\nAbout a document (from Edit's ctrl-k, MIDI's c)\n"
  "ctrl-s\tsave Claude's revision over the file\nctrl-z\tput the file back\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  agent = api->agent();
  api->mem_set(&C, 0, sizeof C);
  talk_args(argc, argv);
  if (!C.talk) {
    C.no_key = !agent->has_key();
    C.seen_gen = (unsigned)-1;
    rebuild();
  }

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
