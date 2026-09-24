/* Counter: a tally you can read across a room, with laps.
 *
 * Space adds one -- hold it to count fast -- and backspace takes one off, so
 * a tap too many is not a reason to start over. Enter records a lap: the
 * count at that moment, how far it moved since the lap before, and the time.
 * Del resets the count and the laps, after asking.
 *
 * The number is set in clock56, Space Mono Bold at 56 px (fonts/fonts.txt),
 * up to six digits, and in num30 past that; it flashes teal for a moment on
 * every change. Laps and captions are Atkinson Hyperlegible at 13 px. If a
 * font will not load the same calls draw in the 6x8 font, which is small
 * but still right.
 *
 * The state is /var/counter/state.txt, plain text a card reader or Files can
 * read:
 *
 *     count=47
 *     lap=47 d=12 14:32:07      newest first; d= is the change since the
 *     lap=35 d=35 14:20:51      lap below it (older files without d= get it
 *                               worked out on load)
 *
 * Saved whole through apps/safefile.h, so a power cut mid-save leaves the old
 * file or the new one, never half of one. A single press saves at once; a
 * held key saves once, when it has been still for a moment, rather than
 * every 60 ms of the repeat.
 *
 * Commands (the same functions as the keys): add, sub, set N, lap, show.
 */

#include "kernel/app/capp.h"
#include "apps/toolbar.h"
#include "apps/safefile.h"

static const CardApi *api;

#define STATE_DIR   CAPP_VAR "/counter"
#define STATE_PATH  STATE_DIR "/state.txt"

#define MAX_LAPS      99
#define ROW_H         17
#define FOOT_H        11
#define FLASH_MS      180      /* the teal after a change */
#define SAVE_IDLE_MS  700      /* a held key saves once it has been still this long */
#define BIG_DIGITS    6        /* clock56 fits six across 240 px */

#define CLR_BG      CAPP_RGB(14, 16, 22)
#define CLR_TEXT    CAPP_RGB(238, 241, 247)
#define CLR_DIM     CAPP_RGB(128, 136, 150)
#define CLR_FAINT   CAPP_RGB(70, 76, 90)
#define CLR_ACCENT  CAPP_RGB(96, 214, 198)
#define CLR_LAP     CAPP_RGB(255, 198, 96)
#define CLR_ROW     CAPP_RGB(20, 23, 31)
#define CLR_ROW_ALT CAPP_RGB(25, 29, 38)
#define CLR_FOOT    CAPP_RGB(32, 36, 46)
#define CLR_WARN    CAPP_RGB(236, 104, 84)

typedef struct {
  uint32_t count;
  int32_t  delta;          /* since the lap before this one */
  char     stamp[10];      /* "14:32:07" with a clock, "+3:41" without */
} Lap;

enum { ASK_NONE = 0, ASK_RESET };

static struct {
  uint32_t count;
  Lap      laps[MAX_LAPS];     /* newest first */
  int      nlaps;
  int      top, rows;          /* the lap list's scroll */
  int      ask;

  int      dirty;              /* changed, not yet on the card */
  uint32_t changed_at;
  uint32_t flash_at;
  int      flashing;
  uint32_t start_ms;

  int      f_big, f_mid, f_ui, f_uib;   /* font handles; -1 is the 6x8 font */
  CRect    hero, list, foot;
} C;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* ---- the state ------------------------------------------------------------ */

static int starts_with(const char *s, const char *p) {
  while (*p) { if (*s != *p) return 0; s++; p++; }
  return 1;
}

static const char *read_u32(const char *p, uint32_t *out) {
  uint32_t v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10u + (uint32_t)(*p++ - '0');
  *out = v;
  return p;
}

/* A lap's line: "lap=47 d=12 14:32:07", or the older "lap=47 14:32:07". A
 * delta that was not written is marked by has_d = 0 and filled in once
 * every lap is read (fix_deltas). */
static int s_has_d[MAX_LAPS];

static void apply_line(const char *line) {
  uint32_t v;
  const char *p;
  if (starts_with(line, "count=")) {
    read_u32(line + 6, &v);
    C.count = v;
  } else if (starts_with(line, "lap=") && C.nlaps < MAX_LAPS) {
    Lap *l = &C.laps[C.nlaps];
    p = read_u32(line + 4, &v);
    l->count = v;
    l->delta = 0;
    s_has_d[C.nlaps] = 0;
    while (*p == ' ') p++;
    if (starts_with(p, "d=")) {
      int neg = 0;
      p += 2;
      if (*p == '-') { neg = 1; p++; }
      else if (*p == '+') p++;
      p = read_u32(p, &v);
      l->delta = neg ? -(int32_t)v : (int32_t)v;
      s_has_d[C.nlaps] = 1;
      while (*p == ' ') p++;
    }
    api->fmt(l->stamp, sizeof l->stamp, "%s", p);
    C.nlaps++;
  }
}

static void fix_deltas(void) {
  int i;
  for (i = 0; i < C.nlaps; i++)
    if (!s_has_d[i])
      C.laps[i].delta = (int32_t)C.laps[i].count -
                        (int32_t)(i + 1 < C.nlaps ? C.laps[i + 1].count : 0);
}

static void state_load(void) {
  char buf[256], line[64];
  int fd, n, i, len = 0;
  C.count = 0;
  C.nlaps = 0;
  fd = safe_open_read(api, STATE_PATH);
  if (fd < 0) return;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      if (buf[i] == '\r') continue;
      if (buf[i] != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = buf[i];
        continue;
      }
      line[len] = 0;
      len = 0;
      apply_line(line);
    }
  }
  if (len) { line[len] = 0; apply_line(line); }   /* no newline at the end */
  api->close(fd);
  fix_deltas();
}

static int state_save(void) {
  SafeFile f;
  char line[64];
  int i;
  api->mkdir(STATE_DIR);
  if (safe_begin(&f, api, STATE_PATH) != 0) return -1;
  api->fmt(line, sizeof line, "count=%u\n", (unsigned)C.count);
  safe_line(&f, line);
  for (i = 0; i < C.nlaps; i++) {
    api->fmt(line, sizeof line, "lap=%u d=%d %s\n", (unsigned)C.laps[i].count,
             (int)C.laps[i].delta, C.laps[i].stamp);
    safe_line(&f, line);
  }
  if (safe_commit(&f) != 0) return -1;
  C.dirty = 0;
  return 0;
}

/* ---- what the keys and the commands do -------------------------------------
 *
 * `now` is the save policy: 1 writes the card at once, 0 leaves it to tick
 * once the key has been still for SAVE_IDLE_MS. */

static void changed(int now) {
  C.dirty = 1;
  C.changed_at = api->ticks_ms();
  C.flash_at = C.changed_at;
  C.flashing = 1;
  if (now) state_save();
}

static void count_add(int now) {
  if (C.count < 0xFFFFFFFEu) C.count++;
  changed(now);
}

/* Not below zero: a tally counts things that happened. */
static int count_sub(int now) {
  if (!C.count) return 0;
  C.count--;
  changed(now);
  return 1;
}

static void count_set(uint32_t v) {
  C.count = v;
  changed(1);
}

static void stamp_now(char *out, size_t n) {
  CappTime t;
  api->now(&t);
  if (t.synced) {
    api->fmt(out, n, "%02u:%02u:%02u", t.hour, t.min, t.sec);
  } else {
    uint32_t s = (api->ticks_ms() - C.start_ms) / 1000u;
    api->fmt(out, n, "+%u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
  }
}

/* Newest first; the oldest falls off the end at MAX_LAPS. */
static void add_lap(void) {
  int i;
  int32_t prev = C.nlaps ? (int32_t)C.laps[0].count : 0;
  if (C.nlaps < MAX_LAPS) C.nlaps++;
  for (i = C.nlaps - 1; i > 0; i--) api->mem_cpy(&C.laps[i], &C.laps[i - 1], sizeof(Lap));
  C.laps[0].count = C.count;
  C.laps[0].delta = (int32_t)C.count - prev;
  stamp_now(C.laps[0].stamp, sizeof C.laps[0].stamp);
  C.top = 0;
  changed(1);
}

static void reset_all(void) {
  C.count = 0;
  C.nlaps = 0;
  C.top = 0;
  changed(1);
}

/* ---- painting --------------------------------------------------------------- */

static int digits_of(uint32_t v) {
  int n = 1;
  while (v >= 10) { v /= 10; n++; }
  return n;
}

static int number_font(void) {
  return digits_of(C.count) <= BIG_DIGITS ? C.f_big : C.f_mid;
}

static void layout(CRect c) {
  int big = api->font_height(C.f_big);
  int hero_h = 6 + big + 2 + api->font_height(C.f_ui) + 4;
  C.hero = rect(c.x, c.y, c.w, hero_h);
  C.foot = rect(c.x, c.y + c.h - FOOT_H, c.w, FOOT_H);
  C.list = rect(c.x, C.hero.y + C.hero.h, c.w, C.foot.y - (C.hero.y + C.hero.h));
  C.rows = C.list.h / ROW_H;
  if (C.rows < 0) C.rows = 0;
}

/* The line the number sits on, the height of the big font whichever font is
 * drawing -- so a count passing a million does not move the caption. */
static CRect number_rect(void) {
  return rect(C.hero.x, C.hero.y + 6, C.hero.w, api->font_height(C.f_big));
}

/* The number, and the strips either side of it in the background: the font
 * fills behind its own glyphs, so nothing is cleared first and a held space
 * does not flash the digits off and on. */
static void paint_number(void) {
  char s[12];
  CRect r = number_rect();
  int f = number_font(), w, h = api->font_height(f), x, y;
  api->fmt(s, sizeof s, "%u", (unsigned)C.count);
  w = api->text_width(f, s);
  x = r.x + (r.w - w) / 2;
  y = r.y + (r.h - h) / 2;
  if (x > r.x) api->fill(rect(r.x, r.y, x - r.x, r.h), CLR_BG);
  if (x + w < r.x + r.w) api->fill(rect(x + w, r.y, r.x + r.w - (x + w), r.h), CLR_BG);
  if (h < r.h) {
    api->fill(rect(x, r.y, w, y - r.y), CLR_BG);
    api->fill(rect(x, y + h, w, r.y + r.h - (y + h)), CLR_BG);
  }
  api->text_font(f, (int16_t)x, (int16_t)y, s, C.flashing ? CLR_ACCENT : CLR_TEXT, CLR_BG);
}

static void paint_caption(void) {
  char s[48];
  int y = C.hero.y + 6 + api->font_height(C.f_big) + 2, w;
  int h = api->font_height(C.f_ui);
  if (!C.nlaps) api->fmt(s, sizeof s, "enter records a lap");
  else api->fmt(s, sizeof s, "%d lap%s   last %+d", C.nlaps, C.nlaps == 1 ? "" : "s",
                (int)C.laps[0].delta);
  w = api->text_width(C.f_ui, s);
  /* from the bottom of the number's line: the two rows between it and the
   * caption belong to nobody else, and were left showing what was there */
  api->fill(rect(C.hero.x, y - 2, C.hero.w, C.hero.y + C.hero.h - (y - 2)), CLR_BG);
  api->text_font(C.f_ui, (int16_t)(C.hero.x + (C.hero.w - w) / 2), (int16_t)y, s,
                 CLR_DIM, CLR_BG);
  (void)h;
}

static void paint_hero(void) {
  api->fill(rect(C.hero.x, C.hero.y, C.hero.w, 6), CLR_BG);
  paint_number();
  paint_caption();
}

static void paint_list(void) {
  int i, y = C.list.y;
  for (i = C.top; i < C.nlaps && i < C.top + C.rows; i++, y += ROW_H) {
    const Lap *l = &C.laps[i];
    uint16_t bg = (i & 1) ? CLR_ROW_ALT : CLR_ROW;
    char num[8], cnt[12], d[12];
    int ty = y + (ROW_H - api->font_height(C.f_ui)) / 2, sw;
    api->fill(rect(C.list.x, y, C.list.w, ROW_H), bg);
    api->fmt(num, sizeof num, "#%d", C.nlaps - i);
    api->fmt(cnt, sizeof cnt, "%u", (unsigned)l->count);
    api->fmt(d, sizeof d, "%+d", (int)l->delta);
    api->text_font(C.f_ui, (int16_t)(C.list.x + 8), (int16_t)ty, num, CLR_FAINT, bg);
    api->text_font(C.f_uib, (int16_t)(C.list.x + 44), (int16_t)ty, cnt, CLR_TEXT, bg);
    api->text_font(C.f_ui, (int16_t)(C.list.x + 108), (int16_t)ty, d, CLR_LAP, bg);
    sw = api->text_width(C.f_ui, l->stamp);
    api->text_font(C.f_ui, (int16_t)(C.list.x + C.list.w - 8 - sw), (int16_t)ty,
                   l->stamp, CLR_DIM, bg);
  }
  if (y < C.list.y + C.list.h)
    api->fill(rect(C.list.x, y, C.list.w, C.list.y + C.list.h - y), CLR_BG);
}

static void paint_foot(void) {
  char s[48];
  api->fill(C.foot, CLR_FOOT);
  if (C.ask == ASK_RESET) {
    api->fmt(s, sizeof s, "reset to 0 and clear %d lap%s?  y/n", C.nlaps,
             C.nlaps == 1 ? "" : "s");
    api->text((int16_t)(C.foot.x + 4), (int16_t)(C.foot.y + 2), s, CLR_WARN, CLR_FOOT);
  } else {
    api->text((int16_t)(C.foot.x + 4), (int16_t)(C.foot.y + 2),
              "spc +1  bksp -1  enter lap  del reset", CLR_DIM, CLR_FOOT);
  }
}

static void app_paint(void *st, CRect full) {
  CRect c;
  (void)st;
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  c = toolbar_rest(full);
  layout(c);
  paint_hero();
  paint_list();
  paint_foot();
  toolbar_paint_menu(full);
}

/* ---- actions and keys --------------------------------------------------------- */

enum { ACT_ADD = 1, ACT_SUB, ACT_LAP, ACT_RESET, ACT_SET, ACT_SHOW };

static const CappParam P_N[] = { { "n", CAPP_ARG_INT, "the number to set it to" } };

static const CappAction ACTIONS[] = {
  { "add",   "+1",          "Count", 0, ACT_ADD,
    "add one to the count", 0, 0, CAPP_CMD_YES },
  { "sub",   "-1",          "Count", 0, ACT_SUB,
    "take one off the count", 0, 0, CAPP_CMD_YES },
  { "lap",   "Record lap",  "Count", 0, ACT_LAP,
    "record a lap: the count now, the change since the last, the time", 0, 0, CAPP_CMD_YES },
  { "reset", "Reset...",    "Count", 0, ACT_RESET },
  { "set",   "Set",         0,       0, ACT_SET,
    "set the count to a number", P_N, 1, CAPP_CMD_YES },
  { "show",  "Show",        0,       0, ACT_SHOW,
    "the count and the last few laps", 0, 0, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

/* What each change repaints: the number alone for a count, and the caption
 * and the list too for a lap. Nothing marked means everything. */
static void damage_number(void) {
  if (number_font() == C.f_big) api->damage(number_rect());
}
static void damage_laps(void) {
  api->damage(C.hero);
  api->damage(C.list);
}

static int do_action(int a) {
  switch (a) {
  case ACT_ADD:   count_add(1); damage_number(); return 1;
  case ACT_SUB:   if (count_sub(1)) damage_number(); return 1;
  case ACT_LAP:   add_lap(); damage_laps(); return 1;
  case ACT_RESET: if (C.count || C.nlaps) C.ask = ASK_RESET; return 1;
  default:        return 0;
  }
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  size_t o;
  int i;
  (void)st;
  switch (action) {
  case ACT_ADD: count_add(1); break;
  case ACT_SUB:
    if (!count_sub(1)) { api->fmt(out, n, "the count is already 0"); return -1; }
    break;
  case ACT_LAP: add_lap(); break;
  case ACT_SET: {
    uint32_t v;
    const char *p = argc > 0 ? argv[0] : "";
    if (*p < '0' || *p > '9' || *read_u32(p, &v)) {
      api->fmt(out, n, "set takes a whole number, 0 or more");
      return -1;
    }
    count_set(v);
    break;
  }
  case ACT_SHOW:
    o = (size_t)api->fmt(out, n, "count %u", (unsigned)C.count);
    for (i = 0; i < C.nlaps && i < 5 && o + 32 < n; i++)
      o += (size_t)api->fmt(out + o, n - o, "\nlap %d: %u (%+d) at %s", C.nlaps - i,
                            (unsigned)C.laps[i].count, (int)C.laps[i].delta, C.laps[i].stamp);
    return 0;
  default:
    api->fmt(out, n, "counter has no command %d", action);
    return -1;
  }
  if (action == ACT_LAP)
    api->fmt(out, n, "lap %d: %u (%+d)", C.nlaps, (unsigned)C.count, (int)C.laps[0].delta);
  else
    api->fmt(out, n, "count %u", (unsigned)C.count);
  return 0;
}

static void clamp_top(void) {
  int max = C.nlaps - C.rows;
  if (max < 0) max = 0;
  if (C.top > max) C.top = max;
  if (C.top < 0) C.top = 0;
}

static int app_key(void *st, uint8_t k) {
  int a = toolbar_key(k);
  (void)st;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);

  if (C.ask == ASK_RESET) {
    if (api->key_repeat()) return 1;
    if (k == 'y' || k == 'Y' || k == CAPP_KEY_ENTER) { C.ask = ASK_NONE; reset_all(); }
    else if (k == 'n' || k == 'N' || k == CAPP_KEY_ESC || k == CAPP_KEY_BACK) C.ask = ASK_NONE;
    else return 1;
    return 1;
  }

  /* Space and backspace are for holding down; so is scrolling. Nothing else
   * here should fire twice because a finger rested on it. */
  if (api->key_repeat() && k != ' ' && k != CAPP_KEY_BACK &&
      k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 1;

  switch (k) {
  case ' ':
    count_add(!api->key_repeat());
    damage_number();
    return 1;
  case CAPP_KEY_BACK:
    if (count_sub(!api->key_repeat())) damage_number();
    return 1;
  case CAPP_KEY_ENTER: return do_action(ACT_LAP);
  case 0x7F:           return do_action(ACT_RESET);
  case CAPP_KEY_UP:    C.top--; clamp_top(); api->damage(C.list); return 1;
  case CAPP_KEY_DOWN:  C.top++; clamp_top(); api->damage(C.list); return 1;
  default:             return 0;
  }
}

static int app_click(void *st, int16_t x, int16_t y, int button) {
  int a;
  (void)st; (void)button;
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  y = (int16_t)(y - toolbar_h());
  if (C.ask != ASK_NONE) return 0;
  if (y >= 0 && y < C.hero.h) { count_add(1); damage_number(); return 1; }
  return 0;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int ch;
  (void)st; (void)buttons;
  ch = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) ch = 1;
  if (wheel) { C.top += wheel > 0 ? -1 : 1; clamp_top(); ch = 1; }
  return ch;
}

static int app_wants_text(void *st) { (void)st; return 0; }

/* The flash fading back to white, and the save a held key put off. */
static int app_tick(void *st, uint32_t now) {
  int redraw = 0;
  (void)st;
  if (C.flashing && now - C.flash_at >= FLASH_MS) {
    C.flashing = 0;
    damage_number();
    redraw = 1;
  }
  if (C.dirty && now - C.changed_at >= SAVE_IDLE_MS) state_save();
  return redraw;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Counter",
  /* 16x16: a rounded badge with a plus in it. */
  { 0x00, 0x00, 0x00, 0x00, 0x3F, 0xFC, 0x20, 0x04,
    0x20, 0x04, 0x21, 0x84, 0x21, 0x84, 0x27, 0xE4,
    0x27, 0xE4, 0x21, 0x84, 0x21, 0x84, 0x20, 0x04,
    0x20, 0x04, 0x3F, 0xFC, 0x00, 0x00, 0x00, 0x00 },
  "space\t+1, hold to count fast\nbackspace\t-1\nenter\trecord a lap\n"
  "del\treset, asks first\nup/down\tscroll the laps\nclick\t+1\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&C, 0, sizeof C);
  C.start_ms = api->ticks_ms();
  C.f_big = C.f_mid = C.f_ui = C.f_uib = -1;
  state_load();
  /* A command needs the state and nothing to draw with. */
  if (!api->headless()) {
    C.f_big = api->font_load("clock56");
    C.f_mid = api->font_load("num30");
    C.f_ui  = api->font_load("ui13");
    C.f_uib = api->font_load("ui13b");
    toolbar_init(api, ACTIONS, NACT, 0, 0);
  }

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
