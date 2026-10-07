/* Chat: talk between the devices.
 *
 * They cannot reach each other, but both reach the CardOS server, so that
 * is the room (server/msg.py): a line typed here is posted there, and
 * while this is open it asks every few seconds for anything newer than the
 * last message it has. One conversation, everyone in it, under a name each
 * device picks the first time (kept in /config/chat.txt; /name changes it).
 *
 * Hold the button on top and talk, and what you said is sent.
 *
 * The requests are http_start/http_poll, so typing never waits on the
 * network. Drawn the way apps/claude.c is: every line written over the one
 * before, padded to the width, nothing cleared first.
 */
#include "kernel/app/capp.h"
#include "apps/safefile.h"

#define CONF       "/config/chat.txt"
#define COLS       40
#define LINES      90
#define ROW_H      9
#define BAR_H      10
#define IN_H       11
#define INPUT_MAX  200
#define NAME_MAX   16
#define REPLY_MAX  6144
#define POLL_MS    3000
#define BATCH      12

#define CLR_BG     CAPP_RGB(18, 20, 26)
#define CLR_BAR    CAPP_RGB(40, 70, 110)
#define CLR_FG     CAPP_RGB(226, 230, 240)
#define CLR_DIM    CAPP_RGB(132, 140, 158)
#define CLR_ME     CAPP_RGB(140, 210, 150)
#define CLR_ERR    CAPP_RGB(232, 110, 100)
#define CLR_IN     CAPP_RGB(28, 32, 42)

/* Everyone else gets a colour from their name, the same on every device. */
static const uint16_t WHO[6] = {
  CAPP_RGB(120, 180, 255), CAPP_RGB(250, 190, 90), CAPP_RGB(220, 140, 250),
  CAPP_RGB(110, 220, 220), CAPP_RGB(255, 140, 150), CAPP_RGB(230, 230, 120),
};

static const CardApi *api;

enum { REQ_NONE, REQ_GET, REQ_POST };

static struct {
  char     line[LINES][COLS + 1];
  uint16_t colour[LINES];
  int      nlines, scroll;
  char     input[INPUT_MAX + 1];
  int      in_len;
  char     name[NAME_MAX + 1];
  int      naming;                 /* the input is the name, not a message */
  int      last_id;
  int      req;                    /* what is in flight */
  char     outbox[INPUT_MAX + 1];  /* a message waiting for its turn */
  uint32_t next_poll;
  char     status[24];
  int      offline;
  CRect    at;
  int      have_at;
} C;

static char reply[REPLY_MAX];

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static void damage_in(void)  { if (C.have_at) api->damage(rect(C.at.x, C.at.y + C.at.h - IN_H, C.at.w, IN_H)); }
static void damage_log(void) { if (C.have_at) api->damage(rect(C.at.x, C.at.y + BAR_H, C.at.w, C.at.h - BAR_H - IN_H)); }
static void damage_bar(void) { if (C.have_at) api->damage(rect(C.at.x, C.at.y, C.at.w, BAR_H)); }

static int same(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

/* ---- the log ----------------------------------------------------------------- */

static void push(const char *text, uint16_t colour) {
  int i;
  if (C.nlines == LINES) {
    for (i = 1; i < LINES; i++) {
      api->mem_cpy(C.line[i - 1], C.line[i], COLS + 1);
      C.colour[i - 1] = C.colour[i];
    }
    C.nlines--;
  }
  api->fmt(C.line[C.nlines], COLS + 1, "%s", text);
  C.colour[C.nlines++] = colour;
}

/* Word-wrapped at forty columns, continuation lines indented two. */
static void push_wrapped(const char *text, uint16_t colour) {
  char out[COLS + 1];
  int n = 0, first = 1;
  for (;;) {
    char c = *text;
    int width = first ? COLS : COLS - 2;
    if (!c) {
      out[n] = 0;
      if (n) push(out, colour);
      return;
    }
    if (n == width) {
      int brk = n, keep, k;
      char tail[COLS + 1];
      while (brk > 0 && out[brk - 1] != ' ') brk--;
      if (brk < width / 3) brk = n;
      keep = n - brk;
      for (k = 0; k < keep; k++) tail[k] = out[brk + k];
      out[brk && brk < n ? brk - 1 : brk] = 0;
      push(out, colour);
      out[0] = ' '; out[1] = ' ';
      for (k = 0; k < keep; k++) out[2 + k] = tail[k];
      n = 2 + keep;
      first = 0;
    }
    out[n++] = c;
    text++;
  }
}

static uint16_t colour_of(const char *name) {
  uint32_t h = 2166136261u;
  if (same(name, C.name)) return CLR_ME;
  while (*name) h = (h ^ (uint8_t)*name++) * 16777619u;
  return WHO[h % 6];
}

static void say(const char *s) { push_wrapped(s, CLR_DIM); damage_log(); }

/* ---- the name ------------------------------------------------------------------ */

static void name_load(void) {
  int fd = safe_open_read(api, CONF), n, i;
  C.name[0] = 0;
  if (fd < 0) return;
  n = api->read(fd, C.name, NAME_MAX);
  api->close(fd);
  if (n < 0) n = 0;
  C.name[n] = 0;
  for (i = 0; C.name[i]; i++) if (C.name[i] == '\n' || C.name[i] == '\r') { C.name[i] = 0; break; }
}

static void name_save(void) {
  SafeFile f;
  if (safe_begin(&f, api, CONF) != 0) return;
  safe_write(&f, C.name, api->str_len(C.name));
  safe_write(&f, "\n", 1);
  safe_commit(&f);
}

static void ask_name(void) {
  C.naming = 1;
  say("What should the others call you? Type a name and press enter.");
}

/* ---- the server ------------------------------------------------------------------ */

static void url_enc(char *out, int n, const char *s) {
  static const char HEX[] = "0123456789ABCDEF";
  int k = 0;
  for (; *s && k < n - 4; s++) {
    unsigned char ch = (unsigned char)*s;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
      out[k++] = (char)ch;
    else { out[k++] = '%'; out[k++] = HEX[ch >> 4]; out[k++] = HEX[ch & 15]; }
  }
  out[k] = 0;
}

static void start_get(void) {
  char url[160];
  api->fmt(url, sizeof url, "%s/msg?since=%d&max=%d", api->proxy(), C.last_id, BATCH);
  if (api->http_start("GET", url, 0, 0, "", 15000) == 0) C.req = REQ_GET;
}

static void start_post(void) {
  char url[200], nm[64];
  url_enc(nm, sizeof nm, C.name);
  api->fmt(url, sizeof url, "%s/msg?name=%s", api->proxy(), nm);
  if (api->http_start("POST", url, C.outbox, "text/plain", "", 15000) == 0) C.req = REQ_POST;
}

static void set_status(const char *s, int bad) {
  api->fmt(C.status, sizeof C.status, "%s", s);
  C.offline = bad;
  damage_bar();
}

/* id \t time \t name \t text, a line each. */
static int take_messages(void) {
  char *p = reply, name[NAME_MAX + 1], line[COLS * 12];
  int got = 0;
  while (*p) {
    char *f[4];
    int k = 0, id = 0;
    f[0] = p;
    while (*p && *p != '\n') {
      if (*p == '\t' && k < 3) { *p = 0; f[++k] = p + 1; }
      p++;
    }
    if (*p) *p++ = 0;
    if (k < 3) continue;
    for (; *f[0] >= '0' && *f[0] <= '9'; f[0]++) id = id * 10 + (*f[0] - '0');
    if (id <= C.last_id) continue;
    C.last_id = id;
    api->fmt(name, sizeof name, "%s", f[2]);
    api->fmt(line, sizeof line, "%s: %s", name, f[3]);
    push_wrapped(line, colour_of(name));
    got++;
  }
  return got;
}

static void poll_server(uint32_t now) {
  int r;
  if (C.req == REQ_NONE) {
    if (C.outbox[0] && C.name[0]) { start_post(); return; }
    if ((int32_t)(now - C.next_poll) >= 0) start_get();
    return;
  }
  r = api->http_poll(reply, sizeof reply - 1);
  if (r == CAPP_HTTP_PENDING) return;
  if (r < 0) {
    set_status(r == -403 ? "not signed in" : "offline", 1);
    if (C.req == REQ_POST) say("(not sent: the server did not answer. enter sends it again)");
    C.req = REQ_NONE;
    C.next_poll = now + POLL_MS * 2;
    return;
  }
  reply[r] = 0;
  if (C.offline) set_status("", 0);
  if (C.req == REQ_POST) {
    C.outbox[0] = 0;
    C.req = REQ_NONE;
    C.next_poll = now;                       /* fetch it back, and anything else */
    return;
  }
  C.req = REQ_NONE;
  {
    int got = take_messages();
    if (got) { C.scroll = 0; damage_log(); }
    /* A full batch means there may be more: ask again at once. */
    C.next_poll = now + (got >= BATCH ? 200 : POLL_MS);
  }
}

/* ---- painting ------------------------------------------------------------------- */

static void fill_if(int x, int y, int w, int h, uint16_t c) {
  if (w > 0 && h > 0) api->fill(rect(x, y, w, h), c);
}

static void text_cols(int x, int y, const char *s, int cols, uint16_t fg, uint16_t bg) {
  char b[64];
  int n = 0;
  if (cols > (int)sizeof b - 1) cols = (int)sizeof b - 1;
  while (s[n] && n < cols) { b[n] = s[n]; n++; }
  while (n < cols) b[n++] = ' ';
  b[n] = 0;
  api->text((int16_t)x, (int16_t)y, b, fg, bg);
}

static void paint_bar(CRect c) {
  char bar[64];
  int cols = (c.w - 3 + 5) / 6;
  if (cols > 63) cols = 63;
  api->fmt(bar, sizeof bar, "Chat  %s%s%s", C.name[0] ? C.name : "",
           C.status[0] ? "  " : "", C.status);
  fill_if(c.x, c.y, c.w, 1, CLR_BAR);
  fill_if(c.x, c.y + 9, c.w, BAR_H - 9, CLR_BAR);
  fill_if(c.x, c.y + 1, 3, 8, CLR_BAR);
  fill_if(c.x + 3 + cols * 6, c.y + 1, c.w - 3 - cols * 6, 8, CLR_BAR);
  text_cols(c.x + 3, c.y + 1, bar, cols, C.offline ? CLR_ERR : CLR_FG, CLR_BAR);
}

static void paint_log(CRect c, CRect clip) {
  int rows = (c.h - BAR_H - IN_H) / ROW_H;
  int top = c.y + BAR_H, bottom = c.y + c.h - IN_H;
  int cols = (c.w - 2 + 5) / 6, right, first, r, y = top;
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
      text_cols(c.x + 2, y, C.line[i], cols, C.colour[i], CLR_BG);
      fill_if(c.x + 2, y + 8, cols * 6, ROW_H - 8, CLR_BG);
    }
    y += ROW_H;
  }
  fill_if(c.x + 2, y, cols * 6, bottom - y, CLR_BG);
}

static void paint_input(CRect c) {
  int y = c.y + c.h - IN_H;
  int vis = (c.w - 12) / 6;
  int from = C.in_len > vis ? C.in_len - vis : 0;
  int n = C.in_len - from, cx = c.x + 10 + n * 6, end;
  fill_if(c.x, y, c.w, 2, CLR_IN);
  fill_if(c.x, y + 10, c.w, IN_H - 10, CLR_IN);
  fill_if(c.x, y + 2, 2, 8, CLR_IN);
  api->text((int16_t)(c.x + 2), (int16_t)(y + 2), C.naming ? "?" : ">", CLR_DIM, CLR_IN);
  fill_if(c.x + 8, y + 2, 2, 8, CLR_IN);
  api->text((int16_t)(c.x + 10), (int16_t)(y + 2), C.input + from, CLR_FG, CLR_IN);
  api->fill(rect(cx, y + 2, 5, 8), CLR_FG);
  text_cols(cx + 5, y + 2, "", vis - n, CLR_FG, CLR_IN);
  end = cx + 5 + (vis > n ? vis - n : 0) * 6;
  fill_if(end, y + 2, c.x + c.w - end, 8, CLR_IN);
}

static void app_paint(void *st, CRect c) {
  CRect clip = api->paint_area();
  (void)st;
  C.at = c;
  C.have_at = 1;
  if (clip.y < c.y + BAR_H) paint_bar(c);
  if (clip.y < c.y + c.h - IN_H && clip.y + clip.h > c.y + BAR_H) paint_log(c, clip);
  if (clip.y + clip.h > c.y + c.h - IN_H) paint_input(c);
}

/* ---- input ------------------------------------------------------------------------ */

static void send(const char *text) {
  if (!text[0]) return;
  if (C.outbox[0]) { say("(still sending the last one)"); return; }
  api->fmt(C.outbox, sizeof C.outbox, "%s", text);
  set_status("sending", 0);
}

static void submit(void) {
  if (!C.in_len) return;
  if (C.naming) {
    int i, n = 0;
    for (i = 0; C.input[i] && n < NAME_MAX; i++)
      if (C.input[i] != '\t') C.name[n++] = C.input[i];
    C.name[n] = 0;
    if (!n) return;
    name_save();
    C.naming = 0;
    {
      char l[48];
      api->fmt(l, sizeof l, "You are %s.", C.name);
      say(l);
    }
    damage_bar();
  } else if (C.input[0] == '/' && C.input[1] == 'n' && C.input[2] == 'a') {
    ask_name();                                     /* /name */
  } else send(C.input);
  C.in_len = 0;
  C.input[0] = 0;
  C.scroll = 0;
  damage_in();
  damage_log();
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (k == CAPP_KEY_ENTER) { submit(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (C.in_len) C.input[--C.in_len] = 0;
    damage_in();
    return 1;
  }
  if (k == CAPP_KEY_ESC) {
    if (C.in_len) { C.in_len = 0; C.input[0] = 0; damage_in(); return 1; }
    return 0;
  }
  if (!C.in_len) {
    if (k == CAPP_KEY_UP)    { C.scroll += 3; damage_log(); return 1; }
    if (k == CAPP_KEY_DOWN)  { C.scroll -= 3; if (C.scroll < 0) C.scroll = 0; damage_log(); return 1; }
    if (k == CAPP_KEY_LEFT)  { C.scroll += 10; damage_log(); return 1; }
    if (k == CAPP_KEY_RIGHT) { C.scroll -= 10; if (C.scroll < 0) C.scroll = 0; damage_log(); return 1; }
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
  (void)st;
  if (!C.naming) poll_server(now);
  return 0;
}

/* Always taking text, so a spoken sentence reaches the prompt. */
static int app_wants_text(void *st) { (void)st; return 1; }

/* Hold the button and talk: the words are sent as a message. */
static int app_button(void *st, int event, const char *text) {
  (void)st;
  if (event == CAPP_G0_ASK) return C.naming ? CAPP_G0_NONE : CAPP_G0_WORDS;
  if (event == CAPP_G0_HEARD && text && text[0]) { send(text); damage_log(); return 1; }
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_NEEDS_PROXY,
  "Chat",
  /* 16x16: two speech bubbles. */
  { 0x7F, 0x00, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80,
    0x7F, 0x00, 0x18, 0xFE, 0x11, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0xFE, 0x00, 0x18, 0x00, 0x10,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "Talk between the devices, through the CardOS server.\n"
  "enter\tsend\nG0\thold and talk: what you say is sent\n"
  "arrows\tscroll back, when nothing is typed\nesc\tclear what is typed\n"
  "/name\tchange the name the others see\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&C, 0, sizeof C);
  name_load();
  if (!C.name[0]) ask_name();
  else {
    char l[48];
    api->fmt(l, sizeof l, "You are %s. Messages appear as they arrive.", C.name);
    say(l);
  }
  C.next_poll = api->ticks_ms();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.button = app_button;
  api->ui(&UI);
  return 0;
}
