/* Roku -- a remote for a Roku TV (a TCL one, here), over the network.
 *
 * Not infrared: a Roku TV answers Roku's External Control Protocol on port
 * 8060 of the LAN, and a button is a POST with no body --
 * http://TV:8060/keypress/VolumeUp. That reaches everything the remote has
 * and one thing it does not: typing. Lit_x types x into whatever text box is
 * on the TV, so Tab here makes this keyboard the TV's.
 *
 * There is no discovery. Roku's is SSDP, which is UDP multicast, and an app
 * has HTTP only; so the TV's address is asked for once (the TV shows it under
 * Settings > Network > About) and kept in /config/roku.txt. Opening asks the
 * TV for /query/device-info, which says it is there and what it is called.
 *
 * Keys go through a short queue: one request is out at a time, and a held +
 * repeats faster than a press can go and come back. A failure drops the rest
 * of the queue -- a TV that refused one volume step will refuse ten.
 *
 * Two things the TV decides. A 403 means Settings > System > Advanced system
 * settings > Control by mobile apps is off. And PowerOn only reaches a TV
 * whose network stays up in standby (Settings > Power > Fast TV start).
 *
 * Commands: `power on|off|toggle`, `volume up|down`, `mute`,
 * `input hdmi1..hdmi4|tuner|av`, `key NAME` (any ECP key: Home, Play, ...),
 * `address IP`.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/safefile.h"
#include "apps/footer.h"

static const CardApi *api;

#define CONF_FILE  CAPP_CONFIG "/roku.txt"
#define IP_MAX     16
#define TVNAME_MAX   32
#define KEY_MAX    16                 /* "InputHDMI4", "Lit_%2F" */
#define QUEUE_MAX  8
#define DRAFT_MAX  28
#define REPLY_MAX  3072               /* device-info runs to about 2.5 KB */
#define TOP_H      18
#define ROW_H      15
#define TIMEOUT_MS 3000               /* the TV is on the LAN or it is not */

#define CLR_BG     CAPP_RGB(16, 14, 24)
#define CLR_TEXT   CAPP_RGB(232, 230, 242)
#define CLR_DIM    CAPP_RGB(128, 122, 150)
#define CLR_ROKU   CAPP_RGB(170, 110, 230)    /* Roku's purple, lifted for a dark screen */
#define CLR_CHIP   CAPP_RGB(44, 36, 64)
#define CLR_BAD    CAPP_RGB(240, 110, 96)

enum { ST_IDLE = 0, ST_INFO, ST_KEY };
enum { M_REMOTE = 0, M_TYPING, M_ADDRESS };

static struct {
  char  ip[IP_MAX];
  char  name[TVNAME_MAX];
  int   mode;
  int   stage;
  int   bad;                          /* the status line is an error */
  char  status[56];
  char  last[24];                     /* the last button sent, as a person says it */

  char  q[QUEUE_MAX][KEY_MAX];
  int   qhead, qlen;

  char  draft[DRAFT_MAX];             /* typing: what went to the TV; address: the field */
  int   dlen;

  int   f_ui, f_uib;
  char  reply[REPLY_MAX];

  /* Where the last paint put the lines that change, so a change can mark
   * just its line: the rect paint was given (w 0 until the first paint),
   * the last button's line and the draft's. */
  CRect at;
  int   last_y, draft_y;
} G;

/* ---- the buttons ----------------------------------------------------------------- */

typedef struct { uint8_t key; const char *ecp; const char *label; } Button;

static const Button BUTTONS[] = {
  { 'p', "Power", "Power" },          { 'P', "Power", "Power" },
  { '+', "VolumeUp", "Volume up" },   { '=', "VolumeUp", "Volume up" },
  { '-', "VolumeDown", "Volume down" }, { '_', "VolumeDown", "Volume down" },
  { 'm', "VolumeMute", "Mute" },      { 'M', "VolumeMute", "Mute" },
  { '1', "InputHDMI1", "HDMI 1" },    { '2', "InputHDMI2", "HDMI 2" },
  { '3', "InputHDMI3", "HDMI 3" },    { '4', "InputHDMI4", "HDMI 4" },
  { 't', "InputTuner", "TV tuner" },  { 'a', "InputAV1", "AV" },
  { CAPP_KEY_UP, "Up", "Up" },        { CAPP_KEY_DOWN, "Down", "Down" },
  { CAPP_KEY_LEFT, "Left", "Left" },  { CAPP_KEY_RIGHT, "Right", "Right" },
  { CAPP_KEY_ENTER, "Select", "OK" }, { CAPP_KEY_BACK, "Back", "Back" },
  { 'h', "Home", "Home" },            { ' ', "Play", "Play/pause" },
  { 'r', "Rev", "Rewind" },           { 'f', "Fwd", "Fast forward" },
  { '*', "Info", "Options" },
};
#define NBUTTONS ((int)(sizeof BUTTONS / sizeof BUTTONS[0]))

static const Button *button_for_key(uint8_t k) {
  int i;
  for (i = 0; i < NBUTTONS; i++) if (BUTTONS[i].key == k) return &BUTTONS[i];
  return 0;
}

static const char *ecp_for_key(uint8_t k) {
  const Button *b = button_for_key(k);
  return b ? b->ecp : 0;
}

static const char *label_for_ecp(const char *ecp) {
  int i;
  for (i = 0; i < NBUTTONS; i++) if (str_same(BUTTONS[i].ecp, ecp)) return BUTTONS[i].label;
  return ecp;
}

/* A typed character as the key that types it: letters and digits as they
 * are, everything else %XX, since it lands in a URL path. */
static void lit_key(char c, char *out, size_t n) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
    api->fmt(out, n, "Lit_%c", c);
  else
    api->fmt(out, n, "Lit_%%%02X", (unsigned)(unsigned char)c);
}

/* ---- the queue ------------------------------------------------------------------- */

static int q_push(const char *key) {
  if (G.qlen >= QUEUE_MAX) return -1;
  api->fmt(G.q[(G.qhead + G.qlen) % QUEUE_MAX], KEY_MAX, "%s", key);
  G.qlen++;
  return 0;
}

static const char *q_front(void) { return G.qlen ? G.q[G.qhead] : 0; }

static void q_pop(void) {
  if (!G.qlen) return;
  G.qhead = (G.qhead + 1) % QUEUE_MAX;
  G.qlen--;
}

static void q_clear(void) { G.qhead = G.qlen = 0; }

/* ---- the address ----------------------------------------------------------------- */

/* Four numbers 0..255 with dots between, and nothing else. */
static int valid_ip(const char *s) {
  int part, digits, v;
  for (part = 0; part < 4; part++) {
    digits = 0; v = 0;
    while (*s >= '0' && *s <= '9') {
      v = v * 10 + (*s++ - '0');
      if (++digits > 3) return 0;
    }
    if (!digits || v > 255) return 0;
    if (part < 3 && *s++ != '.') return 0;
  }
  return *s == 0;
}

/* The address out of a line: no spaces, no line end. */
static void take_ip(const char *line, char *out, size_t n) {
  size_t k = 0;
  while (*line == ' ' || *line == '\t') line++;
  while (*line && *line != ' ' && *line != '\t' && *line != '\r' && *line != '\n' && k + 1 < n)
    out[k++] = *line++;
  out[k] = 0;
}

static void load_ip(void) {
  char buf[40];
  int fd = safe_open_read(api, CONF_FILE), n;
  G.ip[0] = 0;
  if (fd < 0) return;
  n = api->read(fd, buf, sizeof buf - 1);
  api->close(fd);
  if (n <= 0) return;
  buf[n] = 0;
  take_ip(buf, G.ip, sizeof G.ip);
  if (!valid_ip(G.ip)) G.ip[0] = 0;
}

static int save_ip(const char *ip) {
  SafeFile f;
  char line[IP_MAX + 2];
  if (safe_begin(&f, api, CONF_FILE) != 0) return -1;
  api->fmt(line, sizeof line, "%s\n", ip);
  safe_line(&f, line);
  return safe_commit(&f);
}

/* ---- what the TV says ------------------------------------------------------------- */

/* The text of <tag>...</tag> in doc, or "" if it is not there. */
static void xml_tag(const char *doc, const char *tag, char *out, size_t n) {
  const char *p;
  size_t tl = api->str_len(tag), k = 0;
  out[0] = 0;
  for (p = doc; *p; p++) {
    size_t i;
    if (*p != '<') continue;
    for (i = 0; i < tl && p[1 + i] == tag[i]; i++) {}
    if (i < tl || p[1 + tl] != '>') continue;
    p += tl + 2;
    while (*p && *p != '<' && k + 1 < n) out[k++] = *p++;
    out[k] = 0;
    return;
  }
}

/* What to call the TV: the name its owner gave it, Roku's name for it, the
 * model, or just "Roku". */
static void tv_name(const char *doc, char *out, size_t n) {
  xml_tag(doc, "user-device-name", out, n);
  if (!out[0]) xml_tag(doc, "friendly-device-name", out, n);
  if (!out[0]) xml_tag(doc, "model-name", out, n);
  if (!out[0]) api->fmt(out, n, "Roku");
}

/* A failed request as a sentence a person can act on. */
static void why(int r, char *out, size_t n) {
  if (r == -403)
    api->fmt(out, n, "TV refuses: allow Control by mobile apps");
  else if (r <= -100)
    api->fmt(out, n, "the TV said %d", -r);
  else
    api->fmt(out, n, "no answer from %s", G.ip);
}

/* ---- asking ----------------------------------------------------------------------- */

static void url_for(const char *rel, char *out, size_t n) {
  api->fmt(out, n, "http://%s:8060%s", G.ip, rel);
}

static void ask_info(void) {
  char url[64];
  if (!G.ip[0] || G.stage != ST_IDLE) return;
  url_for("/query/device-info", url, sizeof url);
  if (api->http_start("GET", url, 0, 0, 0, TIMEOUT_MS) != 0) return;
  G.stage = ST_INFO;
  G.bad = 0;
  api->fmt(G.status, sizeof G.status, "looking for the TV...");
}

/* The next queued key, if nothing is out. */
static int send_next(void) {
  char rel[32], url[64];
  const char *k = q_front();
  if (!k || G.stage != ST_IDLE || !G.ip[0]) return 0;
  api->fmt(rel, sizeof rel, "/keypress/%s", k);
  url_for(rel, url, sizeof url);
  if (api->http_start("POST", url, 0, 0, 0, TIMEOUT_MS) != 0) return 0;   /* next tick */
  G.stage = ST_KEY;
  return 1;
}

static int poll(void) {
  int r;
  if (G.stage == ST_IDLE) return 0;
  r = api->http_poll(G.reply, sizeof G.reply);
  if (r == CAPP_HTTP_PENDING) return 0;
  if (r < 0) {
    G.bad = 1;
    why(r, G.status, sizeof G.status);
    q_clear();
  } else if (G.stage == ST_INFO) {
    tv_name(G.reply, G.name, sizeof G.name);
    G.bad = 0;
    G.status[0] = 0;
  } else {
    const char *k = q_front();
    if (k && !(k[0] == 'L' && k[1] == 'i' && k[2] == 't' && k[3] == '_'))
      api->fmt(G.last, sizeof G.last, "%s", label_for_ecp(k));
    q_pop();
    if (G.bad) { G.bad = 0; G.status[0] = 0; }
  }
  G.stage = ST_IDLE;
  return 1;
}

static void press(const char *ecp) {
  if (!G.ip[0]) return;
  if (q_push(ecp) != 0) return;       /* a full queue is a held key outrunning the TV */
}

/* ---- the screen ------------------------------------------------------------------- */

static void draw(int f, int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  if (f >= 0) api->text_font(f, (int16_t)x, (int16_t)y, s, fg, bg);
  else api->text((int16_t)x, (int16_t)y, s, fg, bg);
}

static int width(int f, const char *s) {
  return f >= 0 ? api->text_width(f, s) : (int)api->str_len(s) * 6;
}

static int height(int f) { return f >= 0 ? api->font_height(f) : 8; }

/* r less the part `hole` covers, in up to four fills. The hole is where
 * text is about to go, and text paints its own background: filling under it
 * first blinks it, on a panel with no framebuffer. */
static void fill_round(CRect r, CRect hole, uint16_t colour) {
  int x0 = hole.x > r.x ? hole.x : r.x;
  int y0 = hole.y > r.y ? hole.y : r.y;
  int x1 = hole.x + hole.w < r.x + r.w ? hole.x + hole.w : r.x + r.w;
  int y1 = hole.y + hole.h < r.y + r.h ? hole.y + hole.h : r.y + r.h;
  if (r.w <= 0 || r.h <= 0) return;
  if (x0 >= x1 || y0 >= y1) { api->fill(r, colour); return; }
  if (y0 > r.y) api->fill(capp_rect(r.x, r.y, r.w, y0 - r.y), colour);
  if (y1 < r.y + r.h) api->fill(capp_rect(r.x, y1, r.w, r.y + r.h - y1), colour);
  if (x0 > r.x) api->fill(capp_rect(r.x, y0, x0 - r.x, y1 - y0), colour);
  if (x1 < r.x + r.w) api->fill(capp_rect(x1, y0, r.x + r.w - x1, y1 - y0), colour);
}

/* One line of text at x in font f, and the rest of the row [c.x, c.x+c.w)
 * in the background: a line that got shorter is covered, nothing blinks. */
static void line_at(CRect c, int f, int x, int y, const char *s, uint16_t fg) {
  fill_round(capp_rect(c.x, y, c.w, height(f)), capp_rect(x, y, s[0] ? width(f, s) : 0, height(f)), CLR_BG);
  if (s[0]) draw(f, x, y, s, fg, CLR_BG);
}

/* Marks r for the next paint -- once there has been one, so the rects
 * mean something. Returns 1, for a handler to return. */
static int mark(CRect r) {
  if (G.at.w > 0) api->damage(r);
  return 1;
}

static int top_changed(void) { return mark(capp_rect(G.at.x, G.at.y, G.at.w, TOP_H)); }

/* A line in the big font at y. Its height is not asked before there has
 * been a paint: before one, nothing is marked anyway. */
static int line_changed(int y) {
  return G.at.w > 0 ? mark(capp_rect(G.at.x, y, G.at.w, height(G.f_uib))) : 1;
}

/* The draft line: a typed key changes it and nothing else. */
static int draft_changed(void) {
  top_changed();
  return line_changed(G.draft_y);
}

/* A key in a box, then what it does; returns the x after it. */
static int hint(int x, int y, const char *key, const char *what) {
  int kw = width(-1, key) + 6, ty = y + (ROW_H - 8) / 2;
  fill_round(capp_rect(x, y + 1, kw, ROW_H - 2), capp_rect(x + 3, ty, width(-1, key), 8), CLR_CHIP);
  api->text((int16_t)(x + 3), (int16_t)ty, key, CLR_TEXT, CLR_CHIP);
  x += kw + 4;
  draw(G.f_ui, x, y + (ROW_H - height(G.f_ui)) / 2, what, CLR_DIM, CLR_BG);
  return x + width(G.f_ui, what) + 10;
}

/* "Roku" on the left, the status or the TV's name on the right, and the
 * background round them -- split between the two, so a status that got
 * shorter is covered by the fill and neither text is filled under. */
static void paint_top(CRect c) {
  const char *right = G.status[0] ? G.status : G.name;
  int tw = width(G.f_uib, "Roku"), th = height(G.f_uib);
  int rw = right[0] ? width(G.f_ui, right) : 0, rh = height(G.f_ui);
  int tx = c.x + 8, rx = c.x + c.w - 8 - rw, mid = (tx + tw + rx) / 2;
  int ty = c.y + (TOP_H - th) / 2, ry = c.y + (TOP_H - rh) / 2;
  if (mid < c.x) mid = c.x;
  if (mid > c.x + c.w) mid = c.x + c.w;
  fill_round(capp_rect(c.x, c.y, mid - c.x, TOP_H), capp_rect(tx, ty, tw, th), CLR_BG);
  fill_round(capp_rect(mid, c.y, c.x + c.w - mid, TOP_H), capp_rect(rx, ry, rw, rh), CLR_BG);
  draw(G.f_uib, tx, ty, "Roku", CLR_ROKU, CLR_BG);
  if (right[0]) draw(G.f_ui, rx, ry, right, G.bad ? CLR_BAD : CLR_DIM, CLR_BG);
}

static void paint_foot(CRect c, const char *keys) { footer_paint(api, c, keys); }

static void paint_remote(CRect c) {
  int x, y = c.y + TOP_H + 4;
  x = hint(c.x + 8, y, "p", "power");
  x = hint(x, y, "+ -", "volume");
  hint(x, y, "m", "mute");
  y += ROW_H + 3;
  x = hint(c.x + 8, y, "1-4", "HDMI");
  x = hint(x, y, "t", "tuner");
  hint(x, y, "a", "AV");
  y += ROW_H + 3;
  x = hint(c.x + 8, y, "h", "home");
  x = hint(x, y, "ok", "enter");
  hint(x, y, "back", "del");
  y += ROW_H + 8;
  G.last_y = y;
  line_at(c, G.f_uib, c.x + (c.w - width(G.f_uib, G.last)) / 2, y, G.last, CLR_ROKU);
  paint_foot(c, G.ip[0] ? "space play  tab type  i address"
                        : "i address");
}

static void paint_typing(CRect c) {
  char line[DRAFT_MAX + 2];
  int y = c.y + TOP_H + 12;
  draw(G.f_ui, c.x + 8, y, "Typing to the TV", CLR_DIM, CLR_BG);
  y += ROW_H + 6;
  api->fmt(line, sizeof line, "%s_", G.draft);
  G.draft_y = y;
  line_at(c, G.f_uib, c.x + 8, y, line, CLR_TEXT);
  paint_foot(c, "enter enter  del erase  esc done");
}

static void paint_address(CRect c) {
  char line[DRAFT_MAX + 2];
  int y = c.y + TOP_H + 8;
  draw(G.f_ui, c.x + 8, y, "The TV's address:", CLR_DIM, CLR_BG);
  y += ROW_H + 4;
  api->fmt(line, sizeof line, "%s_", G.draft);
  G.draft_y = y;
  line_at(c, G.f_uib, c.x + 8, y, line, CLR_TEXT);
  y += ROW_H + 8;
  draw(G.f_ui, c.x + 8, y, "On the TV: Settings > Network", CLR_DIM, CLR_BG);
  draw(G.f_ui, c.x + 8, y + ROW_H, "> About shows it.", CLR_DIM, CLR_BG);
  paint_foot(c, "enter save  esc cancel");
}

static void app_paint(void *st, CRect c) {
  CRect a = api->paint_area();
  (void)st;
  G.at = c;
  /* The whole background only when the whole screen is being repaired: a
   * mode change, or something of the shell's that was over it. A key or a
   * reply marks only the lines it changed, and those draw over themselves --
   * this fill on every key and every reply blinked the screen. */
  if (a.x <= c.x && a.y <= c.y && a.x + a.w >= c.x + c.w && a.y + a.h >= c.y + c.h)
    api->fill(c, CLR_BG);
  paint_top(c);
  if (G.mode == M_ADDRESS) paint_address(c);
  else if (G.mode == M_TYPING) paint_typing(c);
  else paint_remote(c);
}

/* ---- keys ------------------------------------------------------------------------- */

static void begin_address(void) {
  G.mode = M_ADDRESS;
  api->fmt(G.draft, sizeof G.draft, "%s", G.ip);
  G.dlen = (int)api->str_len(G.draft);
}

static int key_address(uint8_t k) {
  /* Escape always backs out, even before there is an address: the remote
   * then says it has none and how to give it one. Keeping the user in a
   * field they did not want was the one view in the app with no way back. */
  if (k == CAPP_KEY_ESC) {
    G.mode = M_REMOTE;
    if (!G.ip[0]) { G.bad = 1; api->fmt(G.status, sizeof G.status, "no TV address: i sets it"); }
    return 1;
  }
  if (k == CAPP_KEY_BACK) { if (G.dlen) G.draft[--G.dlen] = 0; return draft_changed(); }
  if (k == CAPP_KEY_ENTER) {
    if (!valid_ip(G.draft)) {
      G.bad = 1;
      api->fmt(G.status, sizeof G.status, "four numbers, like 192.168.1.50");
      return top_changed();
    }
    api->fmt(G.ip, sizeof G.ip, "%s", G.draft);
    G.name[0] = 0;
    G.mode = M_REMOTE;
    if (save_ip(G.ip) != 0) {
      G.bad = 1;
      api->fmt(G.status, sizeof G.status, "could not save it to the card");
    }
    q_clear();
    ask_info();
    return 1;
  }
  if (((k >= '0' && k <= '9') || k == '.') && G.dlen < IP_MAX - 1) {
    G.draft[G.dlen++] = (char)k;
    G.draft[G.dlen] = 0;
    return draft_changed();
  }
  return top_changed();
}

static int key_typing(uint8_t k) {
  char lk[KEY_MAX];
  if (k == CAPP_KEY_ESC || k == '\t') { G.mode = M_REMOTE; return 1; }
  /* Below, only the draft line changes: the screen stays as it is. */
  if (k == CAPP_KEY_ENTER) { press("Enter"); G.dlen = 0; G.draft[0] = 0; return draft_changed(); }
  if (k == CAPP_KEY_BACK) {
    press("Backspace");
    if (G.dlen) G.draft[--G.dlen] = 0;
    return draft_changed();
  }
  if (k >= 32 && k < 127) {
    lit_key((char)k, lk, sizeof lk);
    press(lk);
    if (G.dlen >= DRAFT_MAX - 1) {    /* show the end of it */
      int i;
      for (i = 0; i < G.dlen - 1; i++) G.draft[i] = G.draft[i + 1];
      G.dlen--;
    }
    G.draft[G.dlen++] = (char)k;
    G.draft[G.dlen] = 0;
    return draft_changed();
  }
  return top_changed();
}

static int app_key(void *st, uint8_t k) {
  const char *ecp;
  (void)st;
  if (G.mode == M_ADDRESS) return key_address(k);
  if (G.mode == M_TYPING) return key_typing(k);
  if (k == '\t') { G.mode = M_TYPING; G.dlen = 0; G.draft[0] = 0; return 1; }
  if (k == 'i' || k == 'I') { begin_address(); return 1; }
  /* Queued, not sent: nothing on screen changes until the TV answers. */
  if ((ecp = ecp_for_key(k)) != 0) { press(ecp); return top_changed(); }
  return 0;
}

static int app_wants_text(void *st) { (void)st; return G.mode != M_REMOTE; }

static int app_tick(void *st, uint32_t now_ms) {
  int changed;
  (void)st; (void)now_ms;
  changed = poll();
  send_next();
  /* A reply changes the status and, for a key, the last button's line. */
  if (changed) {
    top_changed();
    line_changed(G.last_y);
  }
  return changed;
}

/* ---- commands --------------------------------------------------------------------- */

enum { ACT_POWER = 1, ACT_VOLUME, ACT_MUTE, ACT_INPUT, ACT_KEY, ACT_ADDRESS };

static const CappParam P_POWER[]   = { { "state", CAPP_ARG_CHOICE, "on|off|toggle" } };
static const CappParam P_VOLUME[]  = { { "way", CAPP_ARG_CHOICE, "up|down" } };
static const CappParam P_INPUT[]   = {
  { "input", CAPP_ARG_CHOICE, "hdmi1|hdmi2|hdmi3|hdmi4|tuner|av" } };
static const CappParam P_KEY[]     = { { "key", CAPP_ARG_TEXT, "an ECP key: Home, Play, Up, ..." } };
static const CappParam P_ADDRESS[] = { { "ip", CAPP_ARG_TEXT, "the TV's address, 192.168.1.50" } };

static const CappAction ACTIONS[] = {
  { "power", "Power", 0, 0, ACT_POWER, "turn the TV on or off", P_POWER, 1,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "volume", "Volume", 0, 0, ACT_VOLUME, "the TV's volume one step up or down", P_VOLUME, 1,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "mute", "Mute", 0, 0, ACT_MUTE, "mute or unmute the TV", 0, 0,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "input", "Input", 0, 0, ACT_INPUT, "switch the TV's input", P_INPUT, 1,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "key", "Key", 0, 0, ACT_KEY, "press any Roku button by its ECP name", P_KEY, 1,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "address", "TV address", 0, 0, ACT_ADDRESS, "set the TV's address", P_ADDRESS, 1,
    CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

/* A blocking keypress for a command: the console and voice wait anyway. */
static int send_now(const char *ecp, const char *label, char *out, size_t n) {
  char rel[32], url[64];
  int r;
  if (!G.ip[0]) { api->fmt(out, n, "no TV address yet: do roku address IP"); return -1; }
  if (!api->net_ready() && api->net_connect(20000) != 0) {
    api->fmt(out, n, "%s", api->net_status());
    return -1;
  }
  api->fmt(rel, sizeof rel, "/keypress/%s", ecp);
  url_for(rel, url, sizeof url);
  r = api->http("POST", url, 0, 0, 0, G.reply, sizeof G.reply, TIMEOUT_MS);
  if (r < 0) { why(r, out, n); return -1; }
  api->fmt(out, n, "%s", label);
  return 0;
}

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  (void)st; (void)argc;
  switch (action) {
  case ACT_POWER:
    if (str_same(argv[0], "on")) return send_now("PowerOn", "TV on", out, n);
    if (str_same(argv[0], "off")) return send_now("PowerOff", "TV off", out, n);
    return send_now("Power", "power pressed", out, n);
  case ACT_VOLUME:
    return str_same(argv[0], "up") ? send_now("VolumeUp", "volume up", out, n)
                               : send_now("VolumeDown", "volume down", out, n);
  case ACT_MUTE:
    return send_now("VolumeMute", "mute pressed", out, n);
  case ACT_INPUT: {
    static const char *const IN[][3] = {
      { "hdmi1", "InputHDMI1", "HDMI 1" }, { "hdmi2", "InputHDMI2", "HDMI 2" },
      { "hdmi3", "InputHDMI3", "HDMI 3" }, { "hdmi4", "InputHDMI4", "HDMI 4" },
      { "tuner", "InputTuner", "TV tuner" }, { "av", "InputAV1", "AV" },
    };
    int i;
    for (i = 0; i < 6; i++)
      if (str_same(argv[0], IN[i][0])) return send_now(IN[i][1], IN[i][2], out, n);
    api->fmt(out, n, "no input %s", argv[0]);
    return -1;
  }
  case ACT_KEY:
    return send_now(argv[0], argv[0], out, n);
  case ACT_ADDRESS: {
    char ip[IP_MAX];
    take_ip(argv[0], ip, sizeof ip);
    if (!valid_ip(ip)) { api->fmt(out, n, "four numbers, like 192.168.1.50"); return -1; }
    api->fmt(G.ip, sizeof G.ip, "%s", ip);
    if (save_ip(ip) != 0) { api->fmt(out, n, "could not save it to the card"); return -1; }
    api->fmt(out, n, "the TV is %s", ip);
    return 0;
  }
  }
  api->fmt(out, n, "roku has no command %d", action);
  return -1;
}

static int app_action(void *st, int a) { (void)st; (void)a; return 0; }

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_NET,
  "Roku",
  /* 16x16: a remote, buttons and a d-pad. */
  { 0x0F, 0xF0, 0x10, 0x08, 0x13, 0xC8, 0x10, 0x08,
    0x11, 0x88, 0x13, 0xC8, 0x11, 0x88, 0x10, 0x08,
    0x15, 0x48, 0x10, 0x08, 0x15, 0x48, 0x10, 0x08,
    0x15, 0x48, 0x10, 0x08, 0x0F, 0xF0, 0x00, 0x00 },
  "p P\tpower\n"
  "+ - = _\tvolume (hold to keep going)\n"
  "m M\tmute\n"
  "1-4\tHDMI 1 to 4;  t tuner;  a AV\n"
  "arrows\tmove;  enter OK;  del back;  h home\n"
  "space\tplay/pause;  r rewind;  f fast forward;  * options\n"
  "tab\ttype into the TV's search box\n"
  "i\tthe TV's address\n"
  "escape\tleave typing or the address\n"
  "\n"
  "A 403 means the TV refuses: Settings > System > Advanced\n"
  "system settings > Control by mobile apps.\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  G.f_ui = G.f_uib = -1;
  load_ip();
  if (!api->headless()) {
    G.f_ui = api->font_load("ui13");
    G.f_uib = api->font_load("ui13b");
    if (!G.ip[0]) begin_address();
    else if (!(api->caps_ok() & CAPP_CAP_NET)) {
      G.bad = 1;
      api->fmt(G.status, sizeof G.status, "%s", api->net_status());
    } else ask_info();
  }
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
