/* Lamp: a remote for LED lamps and strips, out of the IR LED (API 50).
 *
 * For the lamp whose remote went missing. The buttons are drawn as the
 * remote's are, a model at a time: the two remotes nearly every cheap RGB
 * strip, bulb and floor lamp comes with are built in (apps/lamp/lampmodel.h),
 * and more can be written into /config/lamps.txt in the same format. The
 * model chosen is kept in /config/lamp.txt.
 *
 * Not knowing which remote it was is the usual case, so `f` finds it: each
 * model in turn sends on (or power) and red, and asks whether the lamp did
 * anything. A lamp can answer a remote's address with its buttons in
 * other places -- a colour that is a mode -- so then `c` (or Enter after a
 * find) maps it: each code is sent in turn and the person picks what the
 * lamp did, colours and switches, and every pick is written at once to a
 * model of their own, "My lamp", in /config/lamps.txt.
 *
 * The Cardputer has an IR LED and no receiver: Lamp can send but not learn.
 * A remote that worked without being pointed at the lamp was radio (RF or
 * Bluetooth), not infrared, and nothing here will reach that lamp.
 *
 *   arrows  choose a button     enter  press it
 *   space   on / off            + -    brighter, dimmer
 *   m       models              f      find my remote
 *   r       re-read /config/lamps.txt
 *   c       map this lamp's buttons by hand
 *
 * @app folder Make
 * @app release extra
 */
#include "kernel/app/capp.h"
#include "apps/footer.h"
#include "apps/safefile.h"
#include "apps/str.h"
#include "apps/lamp/lampmodel.h"

static const CardApi *api;

#define MAX_MODELS  8
#define USER_FILE   "/config/lamps.txt"
#define PICK_FILE   "/config/lamp.txt"
#define MY_NAME     "My lamp"
#define GRID_COLS   7
#define TOP_H       15
#define ROW_H       18
#define BTN_GAP     3

#define CLR_BG      CAPP_RGB(18, 20, 26)
#define CLR_BAR     CAPP_RGB(30, 34, 44)
#define CLR_BTN     CAPP_RGB(52, 58, 72)
#define CLR_EDGE    CAPP_RGB(80, 88, 104)
#define CLR_SEL     CAPP_RGB(255, 255, 255)
#define CLR_TEXT    CAPP_RGB(232, 236, 244)
#define CLR_DIM     CAPP_RGB(128, 136, 152)
#define CLR_SENT    CAPP_RGB(120, 232, 140)
#define CLR_ERR     CAPP_RGB(255, 120, 110)
#define CLR_HI      CAPP_RGB(40, 90, 160)

enum { V_REMOTE, V_MODELS, V_FIND, V_FOUND, V_MAP };

static struct {
  LampModel model[MAX_MODELS];
  int       nmodels, nbuiltin;
  int       cur;                 /* the model in use */
  int       sel, top;            /* button chosen, first row shown */
  int       view;
  int       pick;                /* the models list's row */
  int       find;                /* the model being tried */
  int       on;                  /* what space last sent, for on/off pairs */
  int       bad_line;            /* in /config/lamps.txt, 0 if none */
  int       shown;               /* button rows that fit, as last painted */
  int       f_title;
  char      status[40];
  uint16_t  status_clr;
  char      buf[4096];
  /* Mapping by hand. */
  uint16_t  map_addr;            /* the address the picks are for */
  int       map_code;            /* the code sent last */
  int       map_opt;             /* LM_OPTS index under the cursor */
  uint8_t   map_pick[256];       /* what each code was picked as, + 1; 0 untried */
  char      map_block[1536];
  char      map_out[4096];
} L;

/* ---- models ------------------------------------------------------------------- */

static void load_models(void) {
  int fd, n = 0;
  L.nmodels = 0;
  lm_parse(LM_BUILTIN, (int)sizeof LM_BUILTIN - 1, L.model, &L.nmodels, MAX_MODELS, &L.bad_line);
  L.nbuiltin = L.nmodels;
  L.bad_line = 0;
  fd = safe_open_read(api, USER_FILE);
  if (fd >= 0) {
    n = api->read(fd, L.buf, sizeof L.buf - 1);
    api->close(fd);
    if (n > 0) lm_parse(L.buf, n, L.model, &L.nmodels, MAX_MODELS, &L.bad_line);
  }
}

static void save_pick(void) {
  SafeFile f;
  if (safe_begin(&f, api, PICK_FILE) != 0) return;
  safe_line(&f, L.model[L.cur].name);
  safe_commit(&f);
}

static void load_pick(void) {
  int fd = safe_open_read(api, PICK_FILE), n, i;
  L.cur = 0;
  if (fd < 0) return;
  n = api->read(fd, L.buf, LM_NAME);
  api->close(fd);
  if (n <= 0) return;
  L.buf[n] = 0;
  for (i = 0; i < n; i++)
    if (L.buf[i] == '\n' || L.buf[i] == '\r') { L.buf[i] = 0; break; }
  for (i = 0; i < L.nmodels; i++)
    if (str_same(L.model[i].name, L.buf)) { L.cur = i; return; }
}

static void use_model(int m) {
  L.cur = m;
  L.sel = 0;
  L.top = 0;
  L.on = 0;
}

/* ---- sending ------------------------------------------------------------------- */

static void say(const char *s, uint16_t clr) {
  api->fmt(L.status, sizeof L.status, "%s", s);
  L.status_clr = clr;
}

static const char *key_name(const LampKey *k) {
  return k->label[0] ? k->label : "colour";
}

/* Press button i of the model in use. */
static int press(int i) {
  const LampModel *m = &L.model[L.cur];
  int rc;
  char line[40];
  if (i < 0 || i >= m->n) return -1;
  rc = api->ir_send(m->proto, m->addr, m->key[i].cmd, 0);
  if (rc == 0) {
    api->fmt(line, sizeof line, "sent %s", key_name(&m->key[i]));
    say(line, CLR_SENT);
  } else {
    say(rc == -2 ? "that code cannot be sent" : "the IR LED failed", CLR_ERR);
  }
  return rc;
}

static int press_role(int role) {
  int i = lm_find_role(&L.model[L.cur], role);
  if (i < 0) { say("this remote has no such button", CLR_DIM); return -1; }
  return press(i);
}

/* Space: a power toggle if the remote has one, else on and off in turn. */
static void toggle(void) {
  const LampModel *m = &L.model[L.cur];
  if (lm_find_role(m, LM_ROLE_POWER) >= 0) { press_role(LM_ROLE_POWER); return; }
  if (press_role(L.on ? LM_ROLE_OFF : LM_ROLE_ON) == 0) L.on = !L.on;
}

/* The finder's try: on (or power), then a red swatch if there is one, so a
 * lamp that was already on still visibly changes. */
static void try_model(int mi) {
  const LampModel *m = &L.model[mi];
  int i, wake = lm_find_role(m, LM_ROLE_ON);
  if (wake < 0) wake = lm_find_role(m, LM_ROLE_POWER);
  if (wake >= 0) api->ir_send(m->proto, m->addr, m->key[wake].cmd, 0);
  for (i = 0; i < m->n; i++)
    if (m->key[i].swatch && m->key[i].r > 200 && m->key[i].g < 40 && m->key[i].b < 40) {
      api->ir_send(m->proto, m->addr, m->key[i].cmd, 0);
      break;
    }
}

/* ---- paint ------------------------------------------------------------------------ */

static int rows_shown(CRect c) { return (c.h - TOP_H - FOOT_H) / ROW_H; }

static void paint_top(CRect c, const char *title) {
  api->fill(capp_rect(c.x, c.y, c.w, TOP_H), CLR_BAR);
  api->text_font(L.f_title, (int16_t)(c.x + 4), (int16_t)(c.y + 1), title, CLR_TEXT, CLR_BAR);
}

static void paint_button(const LampKey *k, CRect r, int sel) {
  uint16_t face = k->swatch ? CAPP_RGB(k->r, k->g, k->b) : CLR_BTN;
  uint16_t ink = k->swatch && (k->r * 3 + k->g * 6 + k->b) / 10 > 140 ? CAPP_RGB(0, 0, 0) : CLR_TEXT;
  int w = (int)api->str_len(k->label) * 6;
  api->fill(r, face);
  api->frame(r, sel ? CLR_SEL : CLR_EDGE);
  if (sel) api->frame(capp_rect(r.x + 1, r.y + 1, r.w - 2, r.h - 2), CLR_SEL);
  if (k->label[0])
    api->text((int16_t)(r.x + (r.w - w) / 2), (int16_t)(r.y + (r.h - 8) / 2), k->label, ink, face);
}

static void paint_remote(CRect c) {
  const LampModel *m = &L.model[L.cur];
  int cols = m->cols ? m->cols : 4, rows = (m->n + cols - 1) / cols;
  int bw = (c.w - BTN_GAP) / cols, shown = rows_shown(c), i;
  L.shown = shown;
  char title[48];
  api->fmt(title, sizeof title, "%s", m->name);
  paint_top(c, title);
  if (L.status[0]) {
    int w = (int)api->str_len(L.status) * 6;
    api->text((int16_t)(c.x + c.w - w - 4), (int16_t)(c.y + 4), L.status, L.status_clr, CLR_BAR);
  }
  api->fill(capp_rect(c.x, c.y + TOP_H, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  for (i = 0; i < m->n; i++) {
    int row = i / cols - L.top, col = i % cols;
    CRect r;
    if (row < 0 || row >= shown) continue;
    r = capp_rect(c.x + BTN_GAP + col * bw, c.y + TOP_H + 2 + row * ROW_H, bw - BTN_GAP, ROW_H - BTN_GAP);
    paint_button(&m->key[i], r, i == L.sel);
  }
  if (rows > shown) {          /* a sliver of scrollbar on the right */
    int h = c.h - TOP_H - FOOT_H, bh = h * shown / rows, by = h * L.top / rows;
    api->fill(capp_rect(c.x + c.w - 2, c.y + TOP_H + by, 2, bh), CLR_EDGE);
  }
  footer_paint(api, c, "spc on/off  +- bright  m model  f find");
}

static void paint_models(CRect c) {
  int i, y = c.y + TOP_H + 2;
  paint_top(c, "Which remote?");
  api->fill(capp_rect(c.x, c.y + TOP_H, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  for (i = 0; i < L.nmodels && y + 14 <= c.y + c.h - FOOT_H; i++, y += 14) {
    uint16_t bg = i == L.pick ? CLR_HI : CLR_BG;
    char line[48];
    api->fill(capp_rect(c.x, y, c.w, 14), bg);
    api->fmt(line, sizeof line, "%s %s%s", i == L.cur ? "*" : " ", L.model[i].name,
             i >= L.nbuiltin ? "  (yours)" : "");
    api->text((int16_t)(c.x + 4), (int16_t)(y + 3), line, CLR_TEXT, bg);
  }
  if (L.bad_line && y + 12 <= c.y + c.h - FOOT_H) {
    char line[48];
    api->fmt(line, sizeof line, "lamps.txt line %d not read", L.bad_line);
    api->text((int16_t)(c.x + 4), (int16_t)(y + 4), line, CLR_ERR, CLR_BG);
  }
  footer_paint(api, c, "enter use  f find  esc back");
}

static void paint_find(CRect c) {
  int x = c.x + 8, y = c.y + TOP_H + 8;
  char line[48];
  paint_top(c, "Find my remote");
  api->fill(capp_rect(c.x, c.y + TOP_H, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  if (L.find < L.nmodels) {
    api->fmt(line, sizeof line, "Tried %d of %d:", L.find + 1, L.nmodels);
    api->text((int16_t)x, (int16_t)y, line, CLR_DIM, CLR_BG);
    api->text_font(L.f_title, (int16_t)x, (int16_t)(y + 12), L.model[L.find].name, CLR_TEXT, CLR_BG);
    api->text((int16_t)x, (int16_t)(y + 34), "Point the top edge at the lamp.", CLR_DIM, CLR_BG);
    api->text((int16_t)x, (int16_t)(y + 48), "Did it turn on or change colour?", CLR_TEXT, CLR_BG);
    footer_paint(api, c, "y yes  n next  enter again  esc stop");
  } else {
    api->text((int16_t)x, (int16_t)y, "None of them reached it.", CLR_TEXT, CLR_BG);
    api->text((int16_t)x, (int16_t)(y + 16), "Closer, and aimed at the sensor", CLR_DIM, CLR_BG);
    api->text((int16_t)x, (int16_t)(y + 28), "(a little bump on the cord box)?", CLR_DIM, CLR_BG);
    api->text((int16_t)x, (int16_t)(y + 46), "If the remote worked without", CLR_DIM, CLR_BG);
    api->text((int16_t)x, (int16_t)(y + 58), "pointing, it was radio, not IR.", CLR_DIM, CLR_BG);
    api->text((int16_t)x, (int16_t)(y + 76), "Codes can go in " USER_FILE, CLR_DIM, CLR_BG);
    footer_paint(api, c, "enter start over  esc back");
  }
}

static void paint_found(CRect c) {
  int x = c.x + 8, y = c.y + TOP_H + 8;
  paint_top(c, "Found it");
  api->fill(capp_rect(c.x, c.y + TOP_H, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  api->text((int16_t)x, (int16_t)y, "The lamp answers the", CLR_DIM, CLR_BG);
  api->text_font(L.f_title, (int16_t)x, (int16_t)(y + 12), L.model[L.cur].name, CLR_TEXT, CLR_BG);
  api->text((int16_t)x, (int16_t)(y + 34), "If its colours come out wrong,", CLR_DIM, CLR_BG);
  api->text((int16_t)x, (int16_t)(y + 46), "map them: each code is sent and", CLR_DIM, CLR_BG);
  api->text((int16_t)x, (int16_t)(y + 58), "you pick what the lamp did.", CLR_DIM, CLR_BG);
  footer_paint(api, c, "enter map buttons  esc use as is");
}

static int map_named(void) {
  int i, n = 0;
  for (i = 0; i < 256; i++)
    if (L.map_pick[i] && L.map_pick[i] != LM_OPT_NONE + 1) n++;
  return n;
}

static void paint_map(CRect c) {
  int x = c.x + 4, y = c.y + TOP_H + 4, i;
  int cw = (c.w - BTN_GAP) / GRID_COLS, gy = c.y + TOP_H + 52;
  int was = L.map_pick[L.map_code];
  char line[48];
  api->fmt(line, sizeof line, "Map: code %02X of FF", L.map_code);
  paint_top(c, line);
  api->fmt(line, sizeof line, "%d named", map_named());
  api->text((int16_t)(c.x + c.w - (int)api->str_len(line) * 6 - 4), (int16_t)(c.y + 4), line, CLR_DIM, CLR_BAR);
  api->fill(capp_rect(c.x, c.y + TOP_H, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  api->text((int16_t)x, (int16_t)y, "Sent. What did the lamp do?", CLR_DIM, CLR_BG);
  api->text_font(L.f_title, (int16_t)x, (int16_t)(y + 14), LM_OPTS[L.map_opt].name, CLR_TEXT, CLR_BG);
  if (was) {
    api->fmt(line, sizeof line, "picked before: %s", LM_OPTS[was - 1].name);
    api->text((int16_t)x, (int16_t)(y + 34), line, CLR_SENT, CLR_BG);
  }
  for (i = 0; i < LM_NOPTS; i++) {
    const LampOpt *q = &LM_OPTS[i];
    LampKey k;
    CRect r = capp_rect(c.x + BTN_GAP + (i % GRID_COLS) * cw, gy + (i / GRID_COLS) * ROW_H,
                        cw - BTN_GAP, ROW_H - BTN_GAP);
    api->mem_set(&k, 0, sizeof k);
    k.r = q->r; k.g = q->g; k.b = q->b; k.swatch = q->swatch;
    api->fmt(k.label, sizeof k.label, "%s", i == LM_OPT_NONE ? "--" : q->swatch ? "" : q->label);
    paint_button(&k, r, i == L.map_opt);
  }
  footer_paint(api, c, "enter pick n nothing b back esc done");
}

static void app_paint(void *st, CRect c) {
  (void)st;
  if (L.view == V_FOUND) { paint_found(c); return; }
  if (L.view == V_MAP) { paint_map(c); return; }
  if (L.view == V_MODELS) paint_models(c);
  else if (L.view == V_FIND) paint_find(c);
  else paint_remote(c);
}

/* ---- keys ------------------------------------------------------------------------- */

static void move(int d) {
  const LampModel *m = &L.model[L.cur];
  int cols = m->cols ? m->cols : 4, s = L.sel + d, shown = L.shown > 0 ? L.shown : 1, row;
  if (s < 0 || s >= m->n) return;
  L.sel = s;
  row = s / cols;
  if (row < L.top) L.top = row;
  if (row >= L.top + shown) L.top = row - shown + 1;
}

static void map_send(void) {
  int rc = api->ir_send(LM_NEC, L.map_addr, (uint32_t)L.map_code, 0);
  if (rc) say("the IR LED failed", CLR_ERR);
}

/* The picks so far as "My lamp" in /config/lamps.txt, replacing the last
 * one written. On every pick, so leaving any way loses nothing. */
static int map_save(void) {
  SafeFile f;
  int fd, n = 0, k;
  if (lm_map_text(MY_NAME, L.map_addr, L.map_pick, L.map_block, sizeof L.map_block) < 0) return -1;
  fd = safe_open_read(api, USER_FILE);
  if (fd >= 0) {
    n = api->read(fd, L.buf, sizeof L.buf - 1);
    api->close(fd);
    if (n < 0) n = 0;
  }
  k = lm_replace(L.buf, n, MY_NAME, L.map_block, L.map_out, sizeof L.map_out);
  if (k < 0 || safe_begin(&f, api, USER_FILE) != 0) return -1;
  safe_write(&f, L.map_out, (size_t)k);
  return safe_commit(&f);
}

static void map_start(void) {
  const LampModel *m = &L.model[L.cur];
  if (m->proto != LM_NEC) { say("only a model with a nec address", CLR_ERR); return; }
  if (L.map_addr != m->addr) {
    api->mem_set(L.map_pick, 0, sizeof L.map_pick);
    L.map_addr = m->addr;
    L.map_code = 0;
  }
  L.map_opt = 0;
  L.view = V_MAP;
  L.status[0] = 0;
  map_send();
}

/* Back to the remote, on "My lamp" if anything was named. */
static void map_done(void) {
  int i;
  L.view = V_REMOTE;
  if (!map_named()) return;
  load_models();
  for (i = 0; i < L.nmodels; i++)
    if (str_same(L.model[i].name, MY_NAME)) {
      use_model(i);
      save_pick();
      say("mapped: " MY_NAME, CLR_SENT);
      return;
    }
  say("could not read lamps.txt back", CLR_ERR);
}

static void map_step(int d) {
  int c = L.map_code + d;
  if (c < 0 || c > 255) { say("no more codes that way", CLR_DIM); return; }
  L.map_code = c;
  map_send();
}

static int key_map(uint8_t k) {
  switch (k) {
  case CAPP_KEY_LEFT:  if (L.map_opt > 0) L.map_opt--; break;
  case CAPP_KEY_RIGHT: if (L.map_opt < LM_NOPTS - 1) L.map_opt++; break;
  case CAPP_KEY_UP:    if (L.map_opt >= GRID_COLS) L.map_opt -= GRID_COLS; break;
  case CAPP_KEY_DOWN:  if (L.map_opt + GRID_COLS < LM_NOPTS) L.map_opt += GRID_COLS; break;
  case CAPP_KEY_ENTER:
    L.map_pick[L.map_code] = (uint8_t)(L.map_opt + 1);
    if (map_save() != 0) say("could not write lamps.txt", CLR_ERR);
    map_step(1);
    break;
  case 'n': case 'N':
    if (!L.map_pick[L.map_code]) L.map_pick[L.map_code] = LM_OPT_NONE + 1;
    map_step(1);
    break;
  case 'b': case 'B': case CAPP_KEY_BACK: map_step(-1); break;
  case ' ': case 'r': case 'R': map_send(); break;
  case 'd': case 'D':
    L.map_pick[L.map_code] = 0;
    map_save();
    break;
  case CAPP_KEY_ESC: map_done(); break;
  default: return 0;
  }
  return 1;
}

static void find_start(void) {
  L.view = V_FIND;
  L.find = 0;
  try_model(0);
}

static int key_find(uint8_t k) {
  if (k == CAPP_KEY_ESC) { L.view = V_REMOTE; return 1; }
  if (L.find >= L.nmodels) {
    if (k == CAPP_KEY_ENTER) find_start();
    return 1;
  }
  if (k == 'y' || k == 'Y') {
    use_model(L.find);
    save_pick();
    L.view = V_FOUND;
  } else if (k == 'n' || k == 'N' || k == ' ') {
    if (++L.find < L.nmodels) try_model(L.find);
  } else if (k == CAPP_KEY_ENTER) {
    try_model(L.find);
  }
  return 1;
}

static int key_models(uint8_t k) {
  switch (k) {
  case CAPP_KEY_UP:    if (L.pick > 0) L.pick--; break;
  case CAPP_KEY_DOWN:  if (L.pick < L.nmodels - 1) L.pick++; break;
  case CAPP_KEY_ENTER:
    use_model(L.pick);
    save_pick();
    L.view = V_REMOTE;
    L.status[0] = 0;
    break;
  case 'f': case 'F':  find_start(); break;
  case CAPP_KEY_ESC:   L.view = V_REMOTE; break;
  default: return 0;
  }
  return 1;
}

static int app_key(void *st, uint8_t k) {
  const LampModel *m = &L.model[L.cur];
  int cols = m->cols ? m->cols : 4;
  (void)st;
  if (L.view == V_FIND) return key_find(k);
  if (L.view == V_MAP) return key_map(k);
  if (L.view == V_FOUND) {
    if (k == CAPP_KEY_ENTER) map_start();
    else if (k == CAPP_KEY_ESC) { L.view = V_REMOTE; say("found it", CLR_SENT); }
    return 1;
  }
  if (L.view == V_MODELS) return key_models(k);
  switch (k) {
  case CAPP_KEY_LEFT:  move(-1); break;
  case CAPP_KEY_RIGHT: move(1); break;
  case CAPP_KEY_UP:    move(-cols); break;
  case CAPP_KEY_DOWN:  move(cols); break;
  case CAPP_KEY_ENTER: press(L.sel); break;
  case ' ':            toggle(); break;
  case '+': case '=':  press_role(LM_ROLE_UP); break;
  case '-': case '_':  press_role(LM_ROLE_DOWN); break;
  case 'm': case 'M':  L.view = V_MODELS; L.pick = L.cur; break;
  case 'f': case 'F':  find_start(); break;
  case 'c': case 'C':  map_start(); break;
  case 'r': case 'R': {
    char name[LM_NAME];
    int i;
    api->fmt(name, sizeof name, "%s", m->name);
    load_models();
    L.cur = 0;
    for (i = 0; i < L.nmodels; i++)
      if (str_same(L.model[i].name, name)) L.cur = i;
    if (L.sel >= L.model[L.cur].n) L.sel = L.top = 0;
    say(L.bad_line ? "lamps.txt has a bad line" : "models re-read",
        L.bad_line ? CLR_ERR : CLR_DIM);
    break;
  }
  default: return 0;
  }
  return 1;
}

static int app_wants_text(void *st) { (void)st; return 0; }

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Lamp",
  /* 16x16: a bulb with rays. */
  { 0x01, 0x80, 0x21, 0x84, 0x10, 0x08, 0x03, 0xC0,
    0x0C, 0x30, 0xC8, 0x13, 0x08, 0x10, 0x08, 0x10,
    0x04, 0x20, 0x02, 0x40, 0x03, 0xC0, 0x03, 0xC0,
    0x03, 0xC0, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00 },
  "arrows\tchoose a button\n"
  "enter\tpress it\n"
  "space\ton / off\n"
  "+ -\tbrighter, dimmer\n"
  "m\tchoose the remote model\n"
  "f\tfind my remote: tries each model\n"
  "r\tre-read /config/lamps.txt\n"
  "c\tmap this lamp: each code is sent,\n"
  "\tyou pick what the lamp did\n"
  "\n"
  "mapping: arrows choose, enter picks\n"
  "and sends the next code; n nothing\n"
  "happened; b back; space again; d clear;\n"
  "esc done. Saved as My lamp as you go.\n"
  "\n"
  "Point the top edge at the lamp.\n"
  "Infrared only: a remote that worked\n"
  "without pointing was radio.\n"
  "\n"
  "More remotes go in /config/lamps.txt:\n"
  "  model NAME\n"
  "  nec 0xEF00   (or necmsb)\n"
  "  cols 4\n"
  "  key 03 On on\n"
  "  key 04 Red #FF0000\n"
  "roles: on off power up down\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  L.f_title = api->font_load("ui13b");
  load_models();
  load_pick();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
