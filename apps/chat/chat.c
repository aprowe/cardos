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
 * Tab picks a dog emoji -- Arty or Marcy, eight moods (apps/chat/emoji.h,
 * drawn in apps/chat/tools/make_emoji.py); Enter puts it in what is being typed, as
 * the text ":arty-happy:", and the message goes when that is sent. Here
 * each one is shown as the picture, four log rows tall, among the words:
 * each row is a log line marked EMO_MARK, which paint_row draws instead.
 *
 * The requests are http_start/http_poll, so typing never waits on the
 * network. Drawn the way apps/claude/claude.c is: every line written over the one
 * before, padded to the width, nothing cleared first.
 *
 * @app folder Net
 * @app routes /msg /share
 * @app release core
 */
#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/safefile.h"
#include "apps/chat/emoji.h"

#define CONF       "/config/chat.txt"
#define TL_LINES   90
#include "apps/termlog.h"

#define BAR_H      TL_BAR_H
#define IN_H       TL_IN_H
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
#define CLR_SEL    CAPP_RGB(60, 92, 140)

#define EMO_MARK   '\x01'         /* a log line that is a slice of a picture */
#define EMO_SCALE  2              /* a sprite pixel is 2x2 here */
#define EMO_ROWS   (EMO_H * EMO_SCALE / TL_ROW_H)
#define PANEL_H    32             /* the picker, over the bottom of the log */
#define SLOT       29             /* a picker sprite and the gap after it */
#define KEY_TAB    0x09

/* Everyone else gets a colour from their name, the same on every device. */
static const uint16_t WHO[6] = {
  CAPP_RGB(120, 180, 255), CAPP_RGB(250, 190, 90), CAPP_RGB(220, 140, 250),
  CAPP_RGB(110, 220, 220), CAPP_RGB(255, 140, 150), CAPP_RGB(230, 230, 120),
};

static const CardApi *api;

enum { REQ_NONE, REQ_GET, REQ_POST };

static struct {
  TermLog  log;                    /* the lines, each one's colour, the scroll */
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
  int      dirty;                  /* something was marked since the last tick */
  CRect    at;
  int      have_at;
  int      picking;                /* the emoji picker is up */
  int      dog;                    /* which dog it shows */
  int      mood;                   /* the one chosen */
} C;

static char reply[REPLY_MAX];
static uint16_t pic[EMO_W * EMO_SCALE * TL_ROW_H];   /* a slice; a picker sprite fits too */

/* Marked, and remembered: tick says so, or a message that arrives while no
 * key is pressed waits for one to be drawn (2026-10-06). */
static void damage_in(void)  { C.dirty = 1; if (C.have_at) api->damage(capp_rect(C.at.x, C.at.y + C.at.h - IN_H, C.at.w, IN_H)); }
static void damage_log(void) { C.dirty = 1; if (C.have_at) api->damage(capp_rect(C.at.x, C.at.y + BAR_H, C.at.w, C.at.h - BAR_H - IN_H)); }
static void damage_bar(void) { C.dirty = 1; if (C.have_at) api->damage(capp_rect(C.at.x, C.at.y, C.at.w, BAR_H)); }

/* ---- the log ----------------------------------------------------------------- */

/* Word-wrapped at forty columns, continuation lines indented two
 * (apps/termlog.h). An empty message is no line at all. */
static void push_wrapped(const char *text, uint16_t colour) {
  if (*text) tl_push_text(api, &C.log, text, colour, 2);
}

/* The log's tag is the colour itself. */
static uint16_t colour_is(int tag) { return (uint16_t)tag; }

static uint16_t colour_of(const char *name) {
  uint32_t h = 2166136261u;
  if (str_same(name, C.name)) return CLR_ME;
  while (*name) h = (h ^ (uint8_t)*name++) * 16777619u;
  return WHO[h % 6];
}

static void say(const char *s) { push_wrapped(s, CLR_DIM); damage_log(); }

/* ---- the name ------------------------------------------------------------------ */

/* /config/chat.txt: the name, then the dog the picker opens on. */
static void name_load(void) {
  char b[NAME_MAX + 16], *dog = 0;
  int fd = safe_open_read(api, CONF), n, i;
  C.name[0] = 0;
  if (fd < 0) return;
  n = api->read(fd, b, sizeof b - 1);
  api->close(fd);
  if (n < 0) n = 0;
  b[n] = 0;
  for (i = 0; b[i]; i++)
    if (b[i] == '\n' || b[i] == '\r') {
      b[i] = 0;
      if (!dog) dog = b + i + 1;
    }
  api->fmt(C.name, sizeof C.name, "%s", b);
  for (i = 0; dog && i < EMO_DOGS; i++) if (str_same(dog, EMO_DOG[i])) C.dog = i;
}

static void name_save(void) {
  SafeFile f;
  if (safe_begin(&f, api, CONF) != 0) return;
  safe_write(&f, C.name, api->str_len(C.name));
  safe_write(&f, "\n", 1);
  safe_write(&f, EMO_DOG[C.dog], api->str_len(EMO_DOG[C.dog]));
  safe_write(&f, "\n", 1);
  safe_commit(&f);
}

static void ask_name(void) {
  C.naming = 1;
  say("What should the others call you? Type a name and press enter.");
}

/* ---- the server ------------------------------------------------------------------ */

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

/* A picture, EMO_ROWS log lines: the mark, the code, which row, and the
 * label beside its first row -- "name:", or a space for one that follows
 * words, so it sits under them as a wrapped line does. */
static void push_picture(int code, const char *label, uint16_t colour) {
  char line[NAME_MAX + 8];
  int r;
  for (r = 0; r < EMO_ROWS; r++) {
    api->fmt(line, sizeof line, "%c%c%c%s", EMO_MARK, 'A' + code, '0' + r, label);
    tl_push(api, &C.log, line, colour);
  }
}

/* A message: its words wrapped, each emoji among them a picture. The name
 * leads whichever comes first. */
static void push_message(const char *name, const char *text) {
  static char seg[TL_COLS * 12];
  uint16_t colour = colour_of(name);
  int first = 1;
  while (*text) {
    const char *q = text;
    int code = -1, n = 0, k;
    while (*q && (code = emo_at(q, &n)) < 0) q++;
    while (text < q && *text == ' ') text++;
    k = api->fmt(seg, sizeof seg, first ? "%s: " : "  ", name);
    while (text < q && k < (int)sizeof seg - 1) seg[k++] = *text++;
    while (k > 0 && seg[k - 1] == ' ') k--;
    seg[k] = 0;
    text = q;
    if (k > (first ? (int)api->str_len(name) + 1 : 0)) {
      push_wrapped(seg, colour);
      first = 0;
    }
    if (code >= 0) {
      char label[NAME_MAX + 2];
      api->fmt(label, sizeof label, first ? "%s:" : " ", name);
      push_picture(code, label, colour);
      first = 0;
      text = q + n;
    }
  }
  if (first) {                                   /* nothing at all: the name */
    api->fmt(seg, sizeof seg, "%s:", name);
    push_wrapped(seg, colour);
  }
}

/* id \t time \t name \t text, a line each. */
static int take_messages(void) {
  char *p = reply, name[NAME_MAX + 1];
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
    push_message(name, f[3]);
    got++;
  }
  return got;
}

/* The last message shown, for the OS's notification watcher
 * (kernel/sys/notify.c): what has been read here is not announced. */
static void seen_save(void) {
  char b[16];
  int fd = api->open("/cache/chat.seen", CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC), n;
  if (fd < 0) return;
  n = api->fmt(b, sizeof b, "%d\n", C.last_id);
  api->write(fd, b, (size_t)n);
  api->close(fd);
}

static void poll_server(uint32_t now) {
  int r;
  if (C.req == REQ_NONE) {
    if ((int32_t)(now - C.next_poll) < 0) return;      /* a failed send waits too */
    if (C.outbox[0] && C.name[0]) start_post();
    else start_get();
    return;
  }
  r = api->http_poll(reply, sizeof reply - 1);
  if (r == CAPP_HTTP_PENDING) return;
  if (r < 0) {
    set_status(r == -403 ? "not signed in" : "offline", 1);
    if (C.req == REQ_POST) say("(not sent yet: the server did not answer; trying again)");
    C.req = REQ_NONE;
    C.next_poll = now + POLL_MS * 2;
    return;
  }
  reply[r] = 0;
  if (C.offline) set_status("", 0);
  if (C.req == REQ_POST) {
    C.outbox[0] = 0;
    set_status("", 0);
    C.req = REQ_NONE;
    C.next_poll = now;                       /* fetch it back, and anything else */
    return;
  }
  C.req = REQ_NONE;
  {
    int got = take_messages();
    if (got) {
      C.log.scroll = 0;
      damage_log();
      seen_save();                            /* the OS does not announce these */
    }
    /* A full batch means there may be more: ask again at once. */
    C.next_poll = now + (got >= BATCH ? 200 : POLL_MS);
  }
}

/* ---- painting ------------------------------------------------------------------- */

static void paint_bar(CRect c) {
  char bar[64];
  api->fmt(bar, sizeof bar, "Chat  %s%s%s", C.name[0] ? C.name : "",
           C.status[0] ? "  " : "", C.status);
  tl_paint_bar(api, c, bar, C.offline ? CLR_ERR : CLR_FG, CLR_BAR);
}

static void fill(int x, int y, int w, int h, uint16_t c) {
  if (w > 0 && h > 0) api->fill(capp_rect(x, y, w, h), c);
}

/* A slice of a picture: the name on the first, then the picture's rows. */
static int paint_row(const char *line, int tag, int x, int y, int w) {
  int code, part, nw, i;
  if (line[0] != EMO_MARK) return 0;
  code = line[1] - 'A';
  part = line[2] - '0';
  if (code < 0 || code >= EMO_DOGS * EMO_MOODS || part < 0 || part >= EMO_ROWS) return 0;
  nw = (int)api->str_len(line + 3) * 6 + 6;
  if (part == 0) {
    tl_text_cols(api, x, y, line + 3, nw / 6, (uint16_t)tag, CLR_BG);
    fill(x, y + 8, nw, TL_ROW_H - 8, CLR_BG);
  } else fill(x, y, nw, TL_ROW_H, CLR_BG);
  for (i = 0; i < TL_ROW_H; i++)
    emo_row(code, part * TL_ROW_H + i, EMO_SCALE, CLR_BG, pic + i * EMO_W * EMO_SCALE);
  api->pixels(capp_rect(x + nw, y, EMO_W * EMO_SCALE, TL_ROW_H), pic);
  fill(x + nw + EMO_W * EMO_SCALE, y, w - nw - EMO_W * EMO_SCALE, TL_ROW_H, CLR_BG);
  return 1;
}

/* The picker: which dog and mood, then the eight at their own size. Every
 * pixel of the panel written once. */
static void paint_picker(CRect c) {
  int y = c.y + c.h - IN_H - PANEL_H, i, x = c.x + 3;
  char label[48];
  api->fmt(label, sizeof label, "%c%s %s  < > enter add  tab %s",
           EMO_DOG[C.dog][0] - 32, EMO_DOG[C.dog] + 1, EMO_MOOD[C.mood], EMO_DOG[(C.dog + 1) % EMO_DOGS]);
  fill(c.x, y, c.w, 2, CLR_IN);
  fill(c.x, y + 2, 3, 8, CLR_IN);
  tl_text_cols(api, c.x + 3, y + 2, label, 39, CLR_DIM, CLR_IN);
  fill(c.x + 3 + 39 * 6, y + 2, c.w - 3 - 39 * 6, 8, CLR_IN);
  fill(c.x, y + 10, c.w, 2, CLR_IN);
  fill(c.x, y + 12, 3, EMO_H, CLR_IN);
  for (i = 0; i < EMO_MOODS; i++) {
    int code = C.dog * EMO_MOODS + i, r;
    uint16_t bg = i == C.mood ? CLR_SEL : CLR_IN;
    fill(x, y + 12, 2, EMO_H, bg);
    for (r = 0; r < EMO_H; r++) emo_row(code, r, 1, bg, pic + r * EMO_W);
    api->pixels(capp_rect(x + 2, y + 12, EMO_W, EMO_H), pic);
    fill(x + 2 + EMO_W, y + 12, 2, EMO_H, bg);
    fill(x + 4 + EMO_W, y + 12, SLOT - 4 - EMO_W, EMO_H, CLR_IN);
    x += SLOT;
  }
  fill(x, y + 12, c.x + c.w - x, EMO_H, CLR_IN);
  fill(c.x, y + 12 + EMO_H, c.w, PANEL_H - 12 - EMO_H, CLR_IN);
}

static void app_paint(void *st, CRect c) {
  CRect clip = api->paint_area(), lc = c;
  (void)st;
  C.at = c;
  C.have_at = 1;
  if (C.picking) lc.h = (int16_t)(lc.h - PANEL_H);    /* the log ends above the picker */
  if (clip.y < c.y + BAR_H) paint_bar(c);
  if (clip.y < lc.y + lc.h - IN_H && clip.y + clip.h > c.y + BAR_H)
    tl_paint_log_ex(api, &C.log, lc, clip, colour_is, CLR_BG, paint_row);
  if (C.picking && clip.y < c.y + c.h - IN_H && clip.y + clip.h > c.y + c.h - IN_H - PANEL_H)
    paint_picker(c);
  if (clip.y + clip.h > c.y + c.h - IN_H)
    tl_paint_input(api, c, C.naming ? "?" : ">", C.input, C.in_len, CLR_FG, CLR_DIM, CLR_IN);
}

/* ---- input ------------------------------------------------------------------------ */

static void send(const char *text) {
  if (!text[0]) return;
  if (C.outbox[0]) { say("(still sending the last one)"); return; }
  api->fmt(C.outbox, sizeof C.outbox, "%s", text);
  C.next_poll = api->ticks_ms();
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
  C.log.scroll = 0;
  damage_in();
  damage_log();
}

static void pick(int on) {
  C.picking = on;
  damage_log();
}

static int picker_key(uint8_t k) {
  if (k == CAPP_KEY_LEFT)  { C.mood = (C.mood + EMO_MOODS - 1) % EMO_MOODS; damage_log(); return 1; }
  if (k == CAPP_KEY_RIGHT) { C.mood = (C.mood + 1) % EMO_MOODS; damage_log(); return 1; }
  if (k == KEY_TAB || k == CAPP_KEY_UP || k == CAPP_KEY_DOWN) {
    C.dog = (C.dog + 1) % EMO_DOGS;
    name_save();
    damage_log();
    return 1;
  }
  if (k == CAPP_KEY_ENTER) {        /* into what is being typed; enter there sends */
    char t[24];
    int n = emo_text(t, sizeof t, C.dog * EMO_MOODS + C.mood);
    int sp = C.in_len && C.input[C.in_len - 1] != ' ';
    if (n && C.in_len + sp + n <= INPUT_MAX) {
      if (sp) C.input[C.in_len++] = ' ';
      api->mem_cpy(C.input + C.in_len, t, (size_t)n + 1);
      C.in_len += n;
      damage_in();
    }
    pick(0);
    return 1;
  }
  if (k == CAPP_KEY_ESC) { pick(0); return 1; }
  pick(0);
  return 0;                          /* anything else: away, and typed */
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (C.picking && picker_key(k)) return 1;
  if (k == KEY_TAB && !C.naming) { pick(1); return 1; }
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
    if (k == CAPP_KEY_UP)    { C.log.scroll += 3; damage_log(); return 1; }
    if (k == CAPP_KEY_DOWN)  { C.log.scroll -= 3; if (C.log.scroll < 0) C.log.scroll = 0; damage_log(); return 1; }
    if (k == CAPP_KEY_LEFT)  { C.log.scroll += 10; damage_log(); return 1; }
    if (k == CAPP_KEY_RIGHT) { C.log.scroll -= 10; if (C.log.scroll < 0) C.log.scroll = 0; damage_log(); return 1; }
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
  int d;
  (void)st;
  if (!C.naming) poll_server(now);
  d = C.dirty;
  C.dirty = 0;
  return d;
}

/* Taking text, so a spoken sentence reaches the prompt -- except while
 * the picker is up, when ; , . / are the arrows without fn. */
static int app_wants_text(void *st) { (void)st; return !C.picking; }

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
  "tab\tdog emojis: arrows choose, enter puts it in, tab the other dog\n"
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
