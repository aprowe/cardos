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
 * Type /new to forget the conversation. The key lives in /claude.key on the
 * card and is sent to Anthropic and nowhere else.
 */

#include "kernel/app/capp.h"

#define COLS        40
#define LINES       80            /* the agent keeps ~2 KB; this is its wrap */
#define ROW_H        9
#define BAR_H       10
#define IN_H        11
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
  char  line[LINES][COLS + 1];
  unsigned char who[LINES];
  int   nlines;
  int   scroll;

  char  input[INPUT_MAX + 1];
  int   in_len;

  unsigned seen_gen;              /* the transcript generation we drew */
  int   no_key;
  int   dots;
  uint32_t dot_at;

  CRect at;
  int   have_at;
} C;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

/* ---- the log, rebuilt from the agent's transcript ------------------------ */

static void push(const char *text, int who) {
  int i;
  if (C.nlines == LINES) {
    for (i = 1; i < LINES; i++) {
      api->mem_cpy(C.line[i - 1], C.line[i], COLS + 1);
      C.who[i - 1] = C.who[i];
    }
    C.nlines--;
  }
  api->fmt(C.line[C.nlines], COLS + 1, "%s", text);
  C.who[C.nlines] = (unsigned char)who;
  C.nlines++;
}

/* Word wrap: forty columns is narrow enough that breaking mid-word makes
 * prose hard to read. `text` runs to a newline or the end. */
static const char *push_wrapped(const char *text, int who) {
  char out[COLS + 1];
  int n = 0;

  for (;;) {
    char c = *text;
    if (c == '\n' || c == 0) {
      out[n] = 0;
      push(out, who);
      return c ? text + 1 : text;
    }
    if (c == '\r') { text++; continue; }
    if (c == '\t') c = ' ';
    if (n == COLS) {
      int brk = n;
      while (brk > 0 && out[brk - 1] != ' ') brk--;
      if (brk > COLS / 3) {
        int keep = n - brk, k;
        char tail[COLS + 1];
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
    text++;
  }
}

static void rebuild(void) {
  const char *t = agent->transcript();
  C.nlines = 0;
  if (C.no_key) {
    push_wrapped("No key. Put an Anthropic API key in /claude.key on the card and open this again.", WHO_ERR);
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

static void paint_bar(CRect c) {
  char bar[64];
  const char *s = agent->status();
  api->fill(rect(c.x, c.y, c.w, BAR_H), CLR_BAR);
  if (s[0])
    api->fmt(bar, sizeof bar, "Claude  %s%s", s,
             C.dots == 0 ? "" : C.dots == 1 ? "." : C.dots == 2 ? ".." : "...");
  else
    api->fmt(bar, sizeof bar, "Claude  api.anthropic.com");
  api->text((short)(c.x + 3), (short)(c.y + 1), bar, CLR_FG, CLR_BAR);
}

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

static void paint_input(CRect c) {
  int y = c.y + c.h - IN_H;
  int vis = (c.w - 12) / 6;
  int from = C.in_len > vis ? C.in_len - vis : 0;
  api->fill(rect(c.x, y, c.w, IN_H), CLR_IN);
  api->text((short)(c.x + 2), (short)(y + 2), ">", CLR_DIM, CLR_IN);
  api->text((short)(c.x + 10), (short)(y + 2), C.input + from, CLR_FG, CLR_IN);
  api->fill(rect(c.x + 10 + (C.in_len - from) * 6, y + 2, 5, 8), CLR_FG);
}

/* Painted to the clip the shell hands back: our own damage marks come back
 * as the region they named, and anything else is the whole window. */
static void app_paint(void *st, CRect c) {
  CRect clip = api->paint_area();
  (void)st;
  C.at = c;
  C.have_at = 1;
  if (clip.y < c.y + BAR_H) paint_bar(c);
  if (clip.y < c.y + c.h - IN_H && clip.y + clip.h > c.y + BAR_H) paint_log(c);
  if (clip.y + clip.h > c.y + c.h - IN_H) paint_input(c);
}

static void damage_in(void)  { if (C.have_at) api->damage(rect(C.at.x, C.at.y + C.at.h - IN_H, C.at.w, IN_H)); }
static void damage_log(void) { if (C.have_at) api->damage(rect(C.at.x, C.at.y + BAR_H, C.at.w, C.at.h - BAR_H - IN_H)); }
static void damage_bar(void) { if (C.have_at) api->damage(rect(C.at.x, C.at.y, C.at.w, BAR_H)); }

/* ---- input ------------------------------------------------------------------ */

static void submit(void) {
  int rc;
  if (!C.in_len) return;
  damage_in();

  if (C.input[0] == '/' && C.input[1] == 'n') {
    agent->reset();
    C.in_len = 0; C.input[0] = 0;
    rebuild(); damage_log();
    return;
  }

  rc = agent->ask(C.input);
  C.in_len = 0; C.input[0] = 0;
  C.scroll = 0;
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
  if (k == CAPP_KEY_BACK) {
    if (C.in_len) C.input[--C.in_len] = 0;
    damage_in();
    return 1;
  }
  if (!C.in_len) {
    int rows = 12;
    if (k == CAPP_KEY_UP)    { C.scroll += 3; damage_log(); return 1; }
    if (k == CAPP_KEY_DOWN)  { C.scroll -= 3; if (C.scroll < 0) C.scroll = 0; damage_log(); return 1; }
    if (k == CAPP_KEY_LEFT)  { C.scroll += rows; damage_log(); return 1; }
    if (k == CAPP_KEY_RIGHT) { C.scroll -= rows; if (C.scroll < 0) C.scroll = 0; damage_log(); return 1; }
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
  agent->seen();
  if (agent->generation() != C.seen_gen) {
    rebuild();
    C.scroll = 0;
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
  C.scroll += wheel > 0 ? 3 : -3;
  if (C.scroll < 0) C.scroll = 0;
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
  "/new\tforget the conversation\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  agent = api->agent();
  api->mem_set(&C, 0, sizeof C);
  C.no_key = !agent->has_key();
  C.seen_gen = (unsigned)-1;
  rebuild();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
