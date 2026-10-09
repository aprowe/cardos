/* Toggl -- the running timer, and starting and stopping one.
 *
 * Toggl's API is HTTPS and JSON and an hourly quota; the server
 * (server/toggl.py) holds the token, does all of that, and answers in lines.
 * This asks once when it opens and once after anything it does, and counts
 * the running timer itself from the start time it was given -- a request a
 * second would be the quota gone in a minute.
 *
 * The list is what to start from: a new entry, then every project, then the
 * descriptions and projects of the last two weeks, newest first, each once.
 * Enter starts the highlighted one (whatever was running stops) -- a project
 * starts with no description, which e adds while it runs. Space stops the
 * running one, or with nothing running starts the highlighted one, as space
 * is start/stop in every app; n types a new one, r asks again. (d described
 * and s stopped, until the keys were made the same everywhere: d is delete
 * and s is sync.)
 *
 * Targets: hours a project should get, weekly or in total since a date --
 * set on the dashboard or with `target`. g shows them as bars filling up in
 * each project's colour, and `targets` gives Today the same as %bar lines.
 *
 * A running timer has the screen to itself: its project, in the project's
 * own colour from Toggl, the time in clock56, the description, and the keys
 * in the bar. l goes to the list and back; starting anything comes back to
 * the timer.
 *
 * Commands: `status`, `start TEXT` (a recent entry's project if the text
 * names one), `project NAME` (no description), `describe TEXT` (the running
 * one), `stop`, and `today` -- what was tracked today and the total,
 * which is what Today's page wants from it.
 */

#include "kernel/app/capp.h"
#include "apps/datetime.h"
#include "apps/str.h"
#include "apps/footer.h"

static const CardApi *api;

#define MAX_ROWS   30                   /* the server sends 20 projects, 10 recent */
#define DESC_MAX   40
#define PROJ_MAX   24
#define REPLY_MAX  3200                   /* 30 rows of status, about 2.2 KB */
#define ROW_H      15
#define TOP_H      18
#define TIMER_H    34

#define CLR_BG     CAPP_RGB(16, 18, 24)
#define CLR_TEXT   CAPP_RGB(230, 234, 242)
#define CLR_DIM    CAPP_RGB(126, 136, 152)
#define CLR_RUN    CAPP_RGB(232, 120, 210)      /* Toggl's pink, near enough */
#define CLR_SEL    CAPP_RGB(42, 48, 64)
#define CLR_BAD    CAPP_RGB(240, 110, 96)

enum { ST_IDLE = 0, ST_STATUS, ST_START, ST_STOP, ST_DESCRIBE, ST_TARGETS };

#define MAX_TARGETS 8

typedef struct {
  char     proj[PROJ_MAX];
  uint16_t colour;
  char     week;                  /* 1 weekly, 0 total since `since` */
  uint32_t done, want;            /* seconds */
  char     since[12];
} Target;

typedef struct {
  char kind;                      /* 'p' a project, 'r' a recent entry */
  char proj_id[12];
  char desc[DESC_MAX];
  char proj[PROJ_MAX];
  uint16_t colour;                /* the project's, or 0 for none */
} Recent;

static struct {
  int      running;
  uint32_t start;                 /* UTC epoch seconds */
  char     desc[DESC_MAX];
  char     proj[PROJ_MAX];
  uint16_t colour;                /* the running project's, or 0 */
  int      list;                  /* the list, not the timer, while one runs */
  int      goals;                 /* the targets view is up (g) */
  Target   tg[MAX_TARGETS];
  int      ntg;

  Recent   rec[MAX_ROWS];
  int      nrec;
  int      sel;                   /* 0 is "new entry", then rec[sel - 1] */

  int      stage;
  int      bad;                   /* the status line is an error */
  char     status[48];
  uint32_t shown_sec;             /* the second the timer last painted */
  /* Where it was drawn, so the next second can mark only its own digits. */
  int16_t  t_x, t_y, t_w, t_h, t_pre;   /* t_pre: width of "H:MM:" */
  char     t_str[16];
  uint32_t elapsed0;              /* seconds run when the server answered */
  uint32_t got_ms;                /* our uptime then: the count goes on from it */

  int      typing;                /* 1 a new entry, 2 describing the running one */
  char     draft[DESC_MAX];
  int      dlen;

  int      f_big, f_num, f_ui, f_uib;
  CRect    content;
  char     body[DESC_MAX + 32];
  char     reply[REPLY_MAX];
} G;

static long to_long(const char *s) {
  long v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v;
}

/* ---- what the server says ------------------------------------------------------ */

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* "#rrggbb" as a colour for this screen, or 0. A dark one is lifted halfway
 * to white: Toggl's palette is made for a white page. */
static uint16_t parse_colour(const char *s) {
  int v[6], i, r, g, b;
  if (s[0] != '#') return 0;
  for (i = 0; i < 6; i++) if ((v[i] = hexval(s[i + 1])) < 0) return 0;
  r = v[0] * 16 + v[1]; g = v[2] * 16 + v[3]; b = v[4] * 16 + v[5];
  if (r * 3 + g * 6 + b < 1100) { r = (r + 255) / 2; g = (g + 255) / 2; b = (b + 255) / 2; }
  return CAPP_RGB(r, g, b) ? CAPP_RGB(r, g, b) : 1;
}

static void absorb_running(const char *line) {
  char num[16];
  G.running = 1;
  tsv_field(line, 2, num, sizeof num);
  G.start = (uint32_t)to_long(num);
  tsv_field(line, 3, G.desc, DESC_MAX);
  tsv_field(line, 4, G.proj, PROJ_MAX);
  tsv_field(line, 5, num, sizeof num);
  G.colour = parse_colour(num);
  /* How long it has run, by the server's clock, and when we heard -- not the
   * device's clock, which can be minutes behind after a reboot. A server
   * from before this field: the device's clock, as before. */
  tsv_field(line, 6, num, sizeof num);
  G.got_ms = api->ticks_ms();
  if (num[0]) G.elapsed0 = (uint32_t)to_long(num);
  else {
    uint32_t now = api->epoch();
    G.elapsed0 = now > G.start ? now - G.start : 0;
  }
}

static uint32_t elapsed(void) {
  return G.elapsed0 + (api->ticks_ms() - G.got_ms) / 1000u;
}

static void absorb_status(void) {
  const char *p;
  int pass;
  G.running = 0;
  G.nrec = 0;
  /* Projects first -- starting one with no description is the quick way --
   * then what was done lately. */
  for (pass = 0; pass < 2; pass++)
    for (p = G.reply; *p; ) {
      if (pass == 0 && str_starts(p, "running\t")) absorb_running(p);
      else if (G.nrec < MAX_ROWS &&
               ((pass == 0 && str_starts(p, "project\t")) || (pass == 1 && str_starts(p, "recent\t")))) {
        Recent *r = &G.rec[G.nrec++];
        r->kind = pass == 0 ? 'p' : 'r';
        tsv_field(p, 1, r->proj_id, sizeof r->proj_id);
        char hex[12];
        if (pass == 0) {
          r->desc[0] = 0;
          tsv_field(p, 2, r->proj, PROJ_MAX);
          tsv_field(p, 3, hex, sizeof hex);
        } else {
          tsv_field(p, 2, r->desc, DESC_MAX);
          tsv_field(p, 3, r->proj, PROJ_MAX);
          tsv_field(p, 4, hex, sizeof hex);
        }
        r->colour = parse_colour(hex);
      }
      while (*p && *p != '\n') p++;
      if (*p) p++;
    }
  if (G.sel > G.nrec) G.sel = G.nrec;
}

static int ask(const char *method, const char *rel, const char *body, int stage);

static void absorb_targets(void) {
  const char *p;
  char num[16];
  G.ntg = 0;
  for (p = G.reply; *p && G.ntg < MAX_TARGETS; ) {
    if (str_starts(p, "target\t")) {
      Target *g = &G.tg[G.ntg++];
      tsv_field(p, 1, g->proj, PROJ_MAX);
      tsv_field(p, 2, num, sizeof num);
      g->colour = parse_colour(num);
      tsv_field(p, 3, num, sizeof num);
      g->week = num[0] == 'w';
      tsv_field(p, 4, num, sizeof num);
      g->done = (uint32_t)to_long(num);
      tsv_field(p, 5, num, sizeof num);
      g->want = (uint32_t)to_long(num);
      tsv_field(p, 6, g->since, sizeof g->since);
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
}

static int pct_of(const Target *g) {
  uint32_t p = g->want ? g->done * 100u / g->want : 0;
  return p > 999 ? 999 : (int)p;
}

/* "6:30 of 10:00 this week", "12:00 of 20:00 since 1 Oct". */
static void target_words(const Target *g, char *out, int n) {
  static const char *const MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char a[12], b[12];
  api->fmt(a, sizeof a, "%lu:%02lu", (unsigned long)(g->done / 3600), (unsigned long)(g->done / 60 % 60));
  api->fmt(b, sizeof b, "%lu:%02lu", (unsigned long)(g->want / 3600), (unsigned long)(g->want / 60 % 60));
  if (g->week) api->fmt(out, (size_t)n, "%s of %s this week", a, b);
  else {
    int m = (int)to_long(g->since + 5), d = (int)to_long(g->since + 8);
    if (m >= 1 && m <= 12) api->fmt(out, (size_t)n, "%s of %s since %d %s", a, b, d, MON[m - 1]);
    else api->fmt(out, (size_t)n, "%s of %s in total", a, b);
  }
}

static long utc_offset(void);

static void ask_targets(void) {
  char rel[48];
  api->fmt(rel, sizeof rel, "/toggl/targets?off=%ld", utc_offset());
  ask("GET", rel, 0, ST_TARGETS);
}

/* "error why" from the server, or a status code, as a line for the screen. */
static void say_failure(int n) {
  G.bad = 1;
  if (str_starts(G.reply, "error ")) api->fmt(G.status, sizeof G.status, "%s", G.reply + 6);
  else if (n == -401) api->fmt(G.status, sizeof G.status, "connect Toggl at /dash");
  else api->fmt(G.status, sizeof G.status, "cannot reach the server (%d)", n);
  {
    int i;                        /* one line */
    for (i = 0; G.status[i]; i++) if (G.status[i] == '\n') { G.status[i] = 0; break; }
  }
}

/* ---- asking -------------------------------------------------------------------- */

static int ask(const char *method, const char *rel, const char *body, int stage) {
  char url[96];
  /* One request at a time. A key pressed while one is out used to do
   * nothing at all, which reads as a key that did not register; it says so. */
  if (G.stage != ST_IDLE) {
    G.bad = 1;
    api->fmt(G.status, sizeof G.status, "busy: waiting on Toggl");
    return -1;
  }
  api->fmt(url, sizeof url, "%s%s", api->proxy(), rel);
  if (api->http_start(method, url, body, body ? "text/plain" : 0, "", 15000) != 0) {
    G.bad = 1;
    api->fmt(G.status, sizeof G.status, "busy: another request is out");
    return -1;
  }
  G.stage = stage;
  G.bad = 0;
  api->fmt(G.status, sizeof G.status, "%s", stage == ST_STATUS ? "asking..." :
           stage == ST_START ? "starting..." :
           stage == ST_DESCRIBE ? "saving..." :
           stage == ST_TARGETS ? "adding up..." : "stopping...");
  return 0;
}

static void refresh(void) { ask("GET", "/toggl/status", 0, ST_STATUS); }

static void start_entry(const char *desc, const char *proj_id) {
  api->fmt(G.body, sizeof G.body, "description=%s\nproject=%s", desc, proj_id);
  ask("POST", "/toggl/start", G.body, ST_START);
}

static void describe_entry(const char *desc) {
  api->fmt(G.body, sizeof G.body, "description=%s", desc);
  ask("POST", "/toggl/describe", G.body, ST_DESCRIBE);   /* answers with the running line */
}

static void stop_entry(void) {
  if (!G.running) { api->fmt(G.status, sizeof G.status, "nothing is running"); return; }
  ask("POST", "/toggl/stop", "", ST_STOP);
}

static int poll(void) {
  int n;
  if (G.stage == ST_IDLE) return 0;
  n = api->http_poll(G.reply, sizeof G.reply);
  if (n == CAPP_HTTP_PENDING) return 0;
  if (n < 0) {
    G.stage = ST_IDLE;
    say_failure(n);
    return 1;
  }
  if (G.stage == ST_TARGETS) {
    absorb_targets();
    G.status[0] = 0;
    G.stage = ST_IDLE;
  } else if (G.stage == ST_STATUS) {
    absorb_status();
    G.status[0] = 0;
    G.stage = ST_IDLE;
  } else {
    /* A start answers with its running line, a stop with what stopped;
     * either way the list may have changed, so ask again. */
    if ((G.stage == ST_START || G.stage == ST_DESCRIBE) && str_starts(G.reply, "running\t")) {
      absorb_running(G.reply);
      G.list = 0;
    }
    if (G.stage == ST_STOP) G.running = 0;
    G.stage = ST_IDLE;
    refresh();
  }
  return 1;
}

/* ---- the screen -------------------------------------------------------------------- */

static void draw(int f, int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  if (f >= 0) api->text_font(f, (int16_t)x, (int16_t)y, s, fg, bg);
  else api->text((int16_t)x, (int16_t)y, s, fg, bg);
}

static int width(int f, const char *s) {
  return f >= 0 ? api->text_width(f, s) : (int)api->str_len(s) * 6;
}

static int height(int f) { return f >= 0 ? api->font_height(f) : 8; }

static int timer_screen(void) { return G.running && !G.list && G.typing != 1; }

static uint16_t proj_colour(uint16_t c) { return c ? c : CLR_TEXT; }

/* Where the time is drawn: the middle of the screen on the timer screen, a
 * strip under the title on the list. Its own rect, so a second's tick
 * repaints only it. */
static CRect timer_rect(void) {
  CRect c = G.content;
  if (timer_screen()) {
    int h = height(G.f_big >= 0 ? G.f_big : G.f_num) + 4;
    return capp_rect(c.x, c.y + TOP_H + ROW_H + 2, c.w, h);
  }
  return capp_rect(c.x, c.y + TOP_H, c.w, TIMER_H);
}

/* The time, drawn over itself. It used to clear its strip and then draw:
 * a second of blank clock each second, the whole clock blinking when only
 * the seconds had changed. text_font paints its own background, so the
 * digits are never cleared -- only the margins round them, which were
 * background already. And app_tick marks only the seconds when only they
 * changed, so the shell clips this paint to them. */
/* The second a paint shows, read once a pass in app_tick rather than in
 * paint: the OS paints a full repaint a strip at a time, calling paint once
 * per strip, and a second turning over between two strips drew the top of
 * the digits at one time and the bottom at the next -- and the time saved
 * from the last strip then told the next tick the top was right. */
static uint32_t s_draw_sec;
static int      s_draw_set;

static void paint_timer(void) {
  CRect r = timer_rect();
  char t[16], pre[16];
  uint32_t secs = s_draw_set ? s_draw_sec : elapsed();
  int big = timer_screen() && G.f_big >= 0, f = big ? G.f_big : G.f_num;
  int x, y, w, h, k;
  if (!G.running) {
    api->fill(r, CLR_BG);
    draw(G.f_ui, r.x + 8, r.y + (r.h - height(G.f_ui)) / 2, "not running", CLR_DIM, CLR_BG);
    G.t_str[0] = 0;
    return;
  }
  dt_hms(api, secs, t, sizeof t);
  w = width(f, t);
  h = height(f);
  x = timer_screen() ? r.x + (r.w - w) / 2 : r.x + 8;
  y = r.y + (r.h - h) / 2;
  if (x > r.x) api->fill(capp_rect(r.x, r.y, x - r.x, r.h), CLR_BG);
  if (x + w < r.x + r.w) api->fill(capp_rect(x + w, r.y, r.x + r.w - x - w, r.h), CLR_BG);
  if (y > r.y) api->fill(capp_rect(x, r.y, w, y - r.y), CLR_BG);
  if (y + h < r.y + r.h) api->fill(capp_rect(x, y + h, w, r.y + r.h - y - h), CLR_BG);
  draw(f, x, y, t, proj_colour(G.colour), CLR_BG);
  G.shown_sec = secs;
  G.t_x = (int16_t)x; G.t_y = (int16_t)y; G.t_w = (int16_t)w; G.t_h = (int16_t)h;
  api->fmt(G.t_str, sizeof G.t_str, "%s", t);
  k = (int)api->str_len(t) - 2;              /* "H:MM:" is all but the last two */
  api->mem_cpy(pre, t, (size_t)k);
  pre[k] = 0;
  G.t_pre = (int16_t)width(f, pre);
}

/* What changed since the timer was drawn: only its seconds, when the rest
 * reads the same and nothing moved -- else the whole strip. */
static CRect timer_damage(uint32_t secs) {
  char t[16];
  int n;
  dt_hms(api, secs, t, sizeof t);
  n = (int)api->str_len(t);
  if (G.t_str[0] && n == (int)api->str_len(G.t_str) && n > 2) {
    int i;
    for (i = 0; i < n - 2 && t[i] == G.t_str[i]; i++) ;
    if (i == n - 2) return capp_rect(G.t_x + G.t_pre, G.t_y, G.t_w - G.t_pre, G.t_h);
  }
  return timer_rect();
}

static void paint_row(int i, int y) {
  CRect c = G.content;
  uint16_t bg = i == G.sel ? CLR_SEL : CLR_BG;
  char line[80];
  api->fill(capp_rect(c.x, y, c.w, ROW_H), bg);
  if (i == 0) {
    if (G.typing == 1) api->fmt(line, sizeof line, "new: %s_", G.draft);
    else api->fmt(line, sizeof line, "+ new entry");
    draw(G.f_ui, c.x + 8, y + (ROW_H - height(G.f_ui)) / 2, line,
         G.typing == 1 ? CLR_TEXT : CLR_DIM, bg);
    return;
  }
  if (G.rec[i - 1].kind == 'p') {
    Recent *r = &G.rec[i - 1];
    api->fill(capp_rect(c.x + 8, y + ROW_H / 2 - 3, 6, 6), proj_colour(r->colour));
    draw(G.f_uib, c.x + 20, y + (ROW_H - height(G.f_uib)) / 2, r->proj,
         proj_colour(r->colour), bg);
    return;
  }
  {
    Recent *r = &G.rec[i - 1];
    int x = c.x + 8;
    const char *d = r->desc[0] ? r->desc : "(no description)";
    draw(G.f_ui, x, y + (ROW_H - height(G.f_ui)) / 2, d, r->desc[0] ? CLR_TEXT : CLR_DIM, bg);
    if (r->proj[0]) {
      x += width(G.f_ui, d) + 8;
      if (x < c.x + c.w - 30)
        draw(G.f_ui, x, y + (ROW_H - height(G.f_ui)) / 2, r->proj, proj_colour(r->colour), bg);
    }
  }
}

static void paint_top(void) {
  CRect c = G.content;
  int w;
  api->fill(capp_rect(c.x, c.y, c.w, TOP_H), CLR_BG);
  draw(G.f_uib, c.x + 8, c.y + (TOP_H - height(G.f_uib)) / 2, "Toggl", CLR_DIM, CLR_BG);
  if (G.status[0]) {
    w = width(G.f_ui, G.status);
    draw(G.f_ui, c.x + c.w - 8 - w, c.y + (TOP_H - height(G.f_ui)) / 2, G.status,
         G.bad ? CLR_BAD : CLR_DIM, CLR_BG);
  }
}

/* The shared hint bar, apps/footer.h. */
static void paint_foot(const char *keys) {
  footer_paint(api, G.content, keys);
}

/* The timer, alone: project, time, description. */
static void paint_running(void) {
  CRect c = G.content, tr;
  const char *proj = G.proj[0] ? G.proj : "no project";
  char line[72];
  int y = c.y + TOP_H;
  api->fill(capp_rect(c.x, y, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  draw(G.f_uib, c.x + (c.w - width(G.f_uib, proj)) / 2, y + (ROW_H - height(G.f_uib)) / 2,
       proj, G.proj[0] ? proj_colour(G.colour) : CLR_DIM, CLR_BG);
  paint_timer();
  tr = timer_rect();
  y = tr.y + tr.h + 2;
  if (G.typing == 2) api->fmt(line, sizeof line, "%s_", G.draft);
  else api->fmt(line, sizeof line, "%s", G.desc[0] ? G.desc : "no description");
  draw(G.f_ui, c.x + (c.w - width(G.f_ui, line)) / 2, y, line,
       G.typing == 2 || G.desc[0] ? CLR_TEXT : CLR_DIM, CLR_BG);
  paint_foot(G.typing == 2 ? "enter save  esc cancel" : "space stop  e desc  l list  g targets");
}

static void paint_list(void) {
  CRect c = G.content;
  int y, i, rows, top = 0;
  paint_timer();
  y = c.y + TOP_H + TIMER_H;
  api->fill(capp_rect(c.x, y, c.w, 2), CLR_BG);
  y += 2;
  rows = (c.y + c.h - FOOT_H - y) / ROW_H;
  if (G.sel >= rows) top = G.sel - rows + 1;
  for (i = top; i <= G.nrec && i - top < rows; i++, y += ROW_H) paint_row(i, y);
  if (y < c.y + c.h - FOOT_H) api->fill(capp_rect(c.x, y, c.w, c.y + c.h - FOOT_H - y), CLR_BG);
  paint_foot(G.typing == 1 ? "enter start  esc cancel" :
             G.running ? "enter start  space stop  l timer" :
                         "enter start  n new  g targets");
}

/* Each target: its project in its colour, the hours, and a bar filling up. */
static void paint_goals(void) {
  CRect c = G.content;
  int y = c.y + TOP_H, i, rowh = 30;
  char words[48];
  api->fill(capp_rect(c.x, y, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  if (!G.ntg)
    draw(G.f_ui, c.x + 8, y + 6, G.stage == ST_TARGETS ? "adding up..." :
         "no targets: set them on the dashboard", CLR_DIM, CLR_BG);
  for (i = 0; i < G.ntg && y + rowh <= c.y + c.h - FOOT_H; i++, y += rowh) {
    const Target *g = &G.tg[i];
    uint16_t col = proj_colour(g->colour);
    int bw = c.w - 16, fill = pct_of(g) >= 100 ? bw - 2 : (bw - 2) * pct_of(g) / 100;
    target_words(g, words, sizeof words);
    draw(G.f_uib, c.x + 8, y + 1, g->proj, col, CLR_BG);
    draw(G.f_ui, c.x + c.w - 8 - width(G.f_ui, words), y + 1, words, CLR_DIM, CLR_BG);
    api->fill(capp_rect(c.x + 8, y + 18, bw, 8), CLR_SEL);
    if (fill > 0) api->fill(capp_rect(c.x + 9, y + 19, fill, 6), col);
  }
  paint_foot("esc back  r refresh");
}

static void app_paint(void *st, CRect c) {
  CRect a = api->paint_area(), tr;
  (void)st;
  G.content = c;
  /* A tick's repaint is the timer and nothing else: the rest of the screen
   * is not cleared and drawn again under a clip that would hide it anyway. */
  tr = timer_rect();
  if (!G.goals && a.x >= tr.x && a.y >= tr.y && a.x + a.w <= tr.x + tr.w &&
      a.y + a.h <= tr.y + tr.h) {
    paint_timer();
    return;
  }
  paint_top();
  if (G.goals) paint_goals();
  else if (timer_screen()) paint_running();
  else paint_list();
}

/* ---- keys ---------------------------------------------------------------------------- */

static int key_typing(uint8_t k) {
  if (k == CAPP_KEY_ENTER) {
    int was = G.typing;
    G.typing = 0;
    if (was == 2) describe_entry(G.draft);          /* empty clears it */
    else if (G.dlen) start_entry(G.draft, "");
    return 1;
  }
  if (k == CAPP_KEY_ESC) { G.typing = 0; return 1; }
  if (k == CAPP_KEY_BACK) { if (G.dlen) G.draft[--G.dlen] = 0; else G.typing = 0; return 1; }
  if (k >= 32 && k < 127 && k != '\t' && G.dlen < DESC_MAX - 1) {
    G.draft[G.dlen++] = (char)k;
    G.draft[G.dlen] = 0;
    return 1;
  }
  return 1;
}

static void begin_typing(void) {
  G.typing = 1;
  G.sel = 0;
  G.dlen = 0;
  G.draft[0] = 0;
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (G.typing) return key_typing(k);
  if (G.goals) {
    if (k == 'g' || k == 'G' || k == CAPP_KEY_ESC || k == CAPP_KEY_BACK) { G.goals = 0; return 1; }
    if (k == 'r' || k == 'R') { ask_targets(); return 1; }
    return 1;
  }
  if (k == 'g' || k == 'G') { G.goals = 1; ask_targets(); return 1; }
  if (timer_screen()) {
    switch (k) {
    case 'l': case 'L': case CAPP_KEY_DOWN: G.list = 1; return 1;
    case 'e': case 'E': case ' ': case 'n': case 'N': case 'r': case 'R':
      break;                                 /* as on the list, below */
    default: return 0;
    }
  }
  switch (k) {
  case 'l': case 'L': if (G.running) { G.list = 0; return 1; } return 0;
  /* The list over a running timer is a level down from it: Escape goes back
   * up. With nothing running the list is the top, and declines it. */
  case CAPP_KEY_ESC:  if (G.running) { G.list = 0; return 1; } return 0;
  case CAPP_KEY_UP:   if (G.sel > 0) G.sel--; return 1;
  case CAPP_KEY_DOWN: if (G.sel < G.nrec) G.sel++; return 1;
  case CAPP_KEY_ENTER:
    if (G.sel == 0) begin_typing();
    else start_entry(G.rec[G.sel - 1].desc, G.rec[G.sel - 1].proj_id);
    return 1;
  case 'n': case 'N': begin_typing(); return 1;
  case 'e': case 'E':
    if (!G.running) { api->fmt(G.status, sizeof G.status, "nothing is running"); return 1; }
    G.typing = 2;
    api->fmt(G.draft, sizeof G.draft, "%s", G.desc);   /* to change, not retype */
    G.dlen = (int)api->str_len(G.draft);
    return 1;
  case ' ':
    /* Start/stop: stop what runs; with nothing running, what Enter would. */
    if (G.running) stop_entry();
    else if (G.sel == 0) begin_typing();
    else start_entry(G.rec[G.sel - 1].desc, G.rec[G.sel - 1].proj_id);
    return 1;
  case 'r': case 'R': refresh(); return 1;
  default: return 0;
  }
}

static int app_wants_text(void *st) { (void)st; return G.typing != 0; }

static int app_tick(void *st, uint32_t now_ms) {
  int changed;
  (void)st; (void)now_ms;
  changed = poll();
  s_draw_sec = elapsed();
  s_draw_set = 1;
  /* The timer is the only thing that moves: a second's change repaints
   * just its strip. Not under the targets, which draw no timer: there
   * shown_sec never caught up, so every tick asked for a repaint. */
  if (!changed && G.running && !G.goals && s_draw_sec != G.shown_sec) {
    api->damage(timer_damage(s_draw_sec));
    return 1;
  }
  return changed;
}

/* ---- commands -------------------------------------------------------------------------- */

enum { ACT_STATUS = 1, ACT_START, ACT_STOP, ACT_TODAY, ACT_TOMORROW, ACT_PROJECT, ACT_DESCRIBE,
       ACT_TARGETS, ACT_TARGET };

static const CappParam P_START[] = {
  { "what", CAPP_ARG_TEXT, "the description; a recent entry's project comes with it" },
};

static const CappParam P_PROJECT[] = {
  { "project", CAPP_ARG_TEXT, "the project, or the start of its name" },
};
static const CappParam P_TARGET[] = {
  { "project", CAPP_ARG_TEXT, "the project, or the start of its name" },
  { "hours", CAPP_ARG_INT, "hours wanted; 0 removes the target" },
  { "kind", CAPP_ARG_CHOICE, "week|total" },
};
static const CappParam P_DESCRIBE[] = {
  { "what", CAPP_ARG_TEXT, "the running entry's description" },
};

static const CappAction ACTIONS[] = {
  { "status", "Status", 0, 0, ACT_STATUS, "what Toggl is timing, and for how long", 0, 0,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "start",  "Start",  0, 0, ACT_START, "start a Toggl timer (stops the running one)",
    P_START, 1, CAPP_CMD_YES | CAPP_CMD_NET },
  { "stop",   "Stop",   0, 0, ACT_STOP, "stop the running Toggl timer", 0, 0,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "project", "Start project", 0, 0, ACT_PROJECT,
    "start a timer for a project, with no description yet", P_PROJECT, 1,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "targets", "Targets", 0, 0, ACT_TARGETS,
    "each project's hours against its target, as bars (for Today)", 0, 0,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "target", "Set target", 0, 0, ACT_TARGET,
    "hours a project should get each week, or in total from today", P_TARGET, 3,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "describe", "Describe", 0, 0, ACT_DESCRIBE, "give the running Toggl entry a description",
    P_DESCRIBE, 1, CAPP_CMD_YES | CAPP_CMD_NET },
  { "today",  "Today",  0, 0, ACT_TODAY, "what was tracked today, and the total", 0, 0,
    CAPP_CMD_YES | CAPP_CMD_NET },
  { "tomorrow", "Tomorrow", 0, 0, ACT_TOMORROW, "nothing yet: for Today's tomorrow page", 0, 0,
    CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

/* A blocking request for a command: the console and voice wait anyway. */
static int fetch(const char *method, const char *rel, const char *body) {
  char url[160];
  if (!api->net_ready() && api->net_connect(20000) != 0) return -2;
  api->fmt(url, sizeof url, "%s%s", api->proxy(), rel);
  return api->http(method, url, body, body ? "text/plain" : 0, "", G.reply, sizeof G.reply,
                   15000);
}

static int failed(int n, char *out, size_t sz) {
  if (n == -2) api->fmt(out, sz, "%s", api->net_status());
  else if (str_starts(G.reply, "error ")) api->fmt(out, sz, "%s", G.reply + 6);
  else api->fmt(out, sz, "cannot reach the server (%d)", n);
  return -1;
}

static void rfc3339(uint32_t t, char *out, int n) {
  int y, m, d;
  dt_civil_from_days((int32_t)(t / 86400), &y, &m, &d);
  api->fmt(out, (size_t)n, "%04d-%02d-%02dT%02lu:%02lu:%02luZ", y, m, d,
           (unsigned long)(t / 3600 % 24), (unsigned long)(t / 60 % 60), (unsigned long)(t % 60));
}

/* Local seconds minus UTC seconds, from the kernel's two clocks. */
static long utc_offset(void) {
  int32_t off;
  dt_utc_offset(api, &off);
  return (long)off;
}

static int cmd_today(char *out, size_t n) {
  char rel[96], from[24], to[24], tbuf[16], line[96];
  uint32_t now = api->epoch(), midnight;
  long off;
  const char *p;
  size_t o = 0;
  int r, any = 0;
  if (!now) { api->fmt(out, n, "the clock is not set"); return -1; }
  off = utc_offset();
  midnight = (uint32_t)((long)now - (((long)now + off) % 86400));
  rfc3339(midnight, from, sizeof from);
  rfc3339(midnight + 86400u, to, sizeof to);
  api->fmt(rel, sizeof rel, "/toggl/today?from=%s&to=%s", from, to);
  if ((r = fetch("GET", rel, 0)) < 0) return failed(r, out, n);
  out[0] = 0;
  for (p = G.reply; *p; ) {
    char a[16], b[16], desc[DESC_MAX], proj[PROJ_MAX];
    tsv_field(p, 0, a, sizeof a);
    tsv_field(p, 1, b, sizeof b);
    if (str_starts(p, "total\t")) {
      dt_hm(api, (uint32_t)to_long(b), tbuf, sizeof tbuf);
      o += (size_t)api->fmt(out + o, n - o, "%stotal %s", any ? "" : "nothing tracked\n", tbuf);
    } else if (a[0] >= '0' && a[0] <= '9' && o + 100 < n) {
      long ls = (long)to_long(a) + off;
      tsv_field(p, 2, desc, sizeof desc);
      tsv_field(p, 3, proj, sizeof proj);
      dt_hm(api, (uint32_t)to_long(b), tbuf, sizeof tbuf);
      api->fmt(line, sizeof line, "%02ld:%02ld %s%s%s%s %s\n", ls / 3600 % 24, ls / 60 % 60,
               desc[0] ? desc : "(no description)", proj[0] ? " (" : "", proj,
               proj[0] ? ")" : "", tbuf);
      o += (size_t)api->fmt(out + o, n - o, "%s", line);
      any = 1;
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
  return 0;
}

/* The recent entry whose description starts with `what`, for its project. */
static const char *project_for(const char *what) {
  int i, k;
  for (i = 0; i < G.nrec; i++) {
    if (G.rec[i].kind != 'r') continue;
    for (k = 0; what[k] && str_lower(what[k]) == str_lower(G.rec[i].desc[k]); k++) {}
    if (!what[k] && k > 0) return G.rec[i].proj_id;
  }
  return "";
}

/* The project whose name starts with `name`, ignoring case; -1 if none. */
static int find_project(const char *name) {
  int i, k;
  for (i = 0; i < G.nrec; i++) {
    if (G.rec[i].kind != 'p') continue;
    for (k = 0; name[k] && str_lower(name[k]) == str_lower(G.rec[i].proj[k]); k++) {}
    if (!name[k] && k > 0) return i;
  }
  return -1;
}

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  char t[16];
  int r;
  uint32_t now;
  (void)st; (void)argc;
  switch (action) {
  case ACT_STATUS:
    if ((r = fetch("GET", "/toggl/status", 0)) < 0) return failed(r, out, n);
    absorb_status();
    now = elapsed();
    if (!G.running) { api->fmt(out, n, "not running"); return 0; }
    dt_hm(api, now, t, sizeof t);
    api->fmt(out, n, "running: %s%s%s%s, %s", G.desc[0] ? G.desc : "(no description)",
             G.proj[0] ? " (" : "", G.proj, G.proj[0] ? ")" : "", t);
    return 0;
  case ACT_START:
    if ((r = fetch("GET", "/toggl/status", 0)) >= 0) absorb_status();   /* for the project */
    api->fmt(G.body, sizeof G.body, "description=%s\nproject=%s", argv[0], project_for(argv[0]));
    if ((r = fetch("POST", "/toggl/start", G.body)) < 0) return failed(r, out, n);
    absorb_running(G.reply);
    api->fmt(out, n, "started %s%s%s%s", G.desc, G.proj[0] ? " (" : "", G.proj,
             G.proj[0] ? ")" : "");
    return 0;
  case ACT_STOP:
    if ((r = fetch("POST", "/toggl/stop", "")) < 0) return failed(r, out, n);
    if (str_starts(G.reply, "idle")) { api->fmt(out, n, "nothing was running"); return 0; }
    {
      char secs[16], desc[DESC_MAX];
      tsv_field(G.reply, 1, secs, sizeof secs);
      tsv_field(G.reply, 2, desc, sizeof desc);
      dt_hm(api, (uint32_t)to_long(secs), t, sizeof t);
      api->fmt(out, n, "stopped %s after %s", desc[0] ? desc : "(no description)", t);
    }
    return 0;
  case ACT_PROJECT:
    if ((r = fetch("GET", "/toggl/status", 0)) < 0) return failed(r, out, n);
    absorb_status();
    if ((r = find_project(argv[0])) < 0) { api->fmt(out, n, "no project called %s", argv[0]); return -1; }
    api->fmt(G.body, sizeof G.body, "description=\nproject=%s", G.rec[r].proj_id);
    if ((r = fetch("POST", "/toggl/start", G.body)) < 0) return failed(r, out, n);
    absorb_running(G.reply);
    api->fmt(out, n, "started %s", G.proj);
    return 0;
  case ACT_DESCRIBE:
    api->fmt(G.body, sizeof G.body, "description=%s", argv[0]);
    if ((r = fetch("POST", "/toggl/describe", G.body)) < 0) return failed(r, out, n);
    if (str_starts(G.reply, "idle")) { api->fmt(out, n, "nothing is running"); return -1; }
    absorb_running(G.reply);
    api->fmt(out, n, "%s%s%s", G.proj, G.proj[0] ? ": " : "", G.desc);
    return 0;
  case ACT_TARGETS: {
    char rel[48], words[48];
    size_t o = 0;
    int i;
    api->fmt(rel, sizeof rel, "/toggl/targets?off=%ld", utc_offset());
    if ((r = fetch("GET", rel, 0)) < 0) return failed(r, out, n);
    absorb_targets();
    if (!G.ntg) { api->fmt(out, n, "no targets set"); return 0; }
    for (i = 0; i < G.ntg && o + 80 < n; i++) {
      target_words(&G.tg[i], words, sizeof words);
      o += (size_t)api->fmt(out + o, n - o, "%s%%bar %d %s  %s", i ? "\n" : "",
                            pct_of(&G.tg[i]), G.tg[i].proj, words);
    }
    return 0;
  }
  case ACT_TARGET:
    api->fmt(G.body, sizeof G.body, "project=%s\nhours=%s\nkind=%s", argv[0], argv[1], argv[2]);
    if ((r = fetch("POST", "/toggl/target", G.body)) < 0) return failed(r, out, n);
    api->fmt(out, n, to_long(argv[1]) ? "%s: %s hours a %s" : "%s: target removed",
             argv[0], argv[1], argv[2][0] == 'w' ? "week" : "contract");
    return 0;
  case ACT_TODAY:
    return cmd_today(out, n);
  case ACT_TOMORROW:
    api->fmt(out, n, "nothing tracked yet");
    return 0;
  }
  api->fmt(out, n, "toggl has no command %d", action);
  return -1;
}

static int app_action(void *st, int a) { (void)st; (void)a; return 0; }

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Toggl",
  /* 16x16: a stopwatch. */
  { 0x03, 0xC0, 0x01, 0x80, 0x07, 0xE0, 0x18, 0x18,
    0x21, 0x84, 0x41, 0x82, 0x41, 0x82, 0x81, 0x81,
    0x81, 0xF1, 0x80, 0x01, 0x40, 0x02, 0x40, 0x02,
    0x20, 0x04, 0x18, 0x18, 0x07, 0xE0, 0x00, 0x00 },
  "enter\tstart the highlighted project or entry\n"
  "space\tstop the running timer; with none, start the highlighted one\n"
  "n\ta new entry: type its description\n"
  "e\tdescribe the running entry\n"
  "l\tthe list, and back to the timer\n"
  "g\ttargets: hours against each project's target\n"
  "r\task Toggl again\n"
  "up/down\tchoose\n"
  "esc\tback: from the targets, or the list to the timer\n"
  "\n"
  "connect Toggl on the dashboard (/dash) first.\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  G.f_big = G.f_num = G.f_ui = G.f_uib = -1;
  if (!api->headless()) {
    G.f_big = api->font_load("clock56");
    G.f_num = api->font_load("num30");
    G.f_ui = api->font_load("ui13");
    G.f_uib = api->font_load("ui13b");
    refresh();
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
