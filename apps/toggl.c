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
 * starts with no description, which d adds while it runs. s stops, n types a
 * new one, r asks again.
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

static const CardApi *api;

#define MAX_ROWS   30                   /* the server sends 20 projects, 10 recent */
#define DESC_MAX   40
#define PROJ_MAX   24
#define REPLY_MAX  3200                   /* 30 rows of status, about 2.2 KB */
#define ROW_H      15
#define TOP_H      18
#define TIMER_H    34
#define FOOT_H     11

#define CLR_BG     CAPP_RGB(16, 18, 24)
#define CLR_TEXT   CAPP_RGB(230, 234, 242)
#define CLR_DIM    CAPP_RGB(126, 136, 152)
#define CLR_RUN    CAPP_RGB(232, 120, 210)      /* Toggl's pink, near enough */
#define CLR_SEL    CAPP_RGB(42, 48, 64)
#define CLR_FOOT   CAPP_RGB(28, 32, 42)
#define CLR_BAD    CAPP_RGB(240, 110, 96)

enum { ST_IDLE = 0, ST_STATUS, ST_START, ST_STOP, ST_DESCRIBE };

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

  Recent   rec[MAX_ROWS];
  int      nrec;
  int      sel;                   /* 0 is "new entry", then rec[sel - 1] */

  int      stage;
  int      bad;                   /* the status line is an error */
  char     status[48];
  uint32_t shown_sec;             /* the second the timer last painted */

  int      typing;                /* 1 a new entry, 2 describing the running one */
  char     draft[DESC_MAX];
  int      dlen;

  int      f_big, f_num, f_ui, f_uib;
  CRect    content;
  char     body[DESC_MAX + 32];
  char     reply[REPLY_MAX];
} G;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int starts(const char *s, const char *p) {
  while (*p) { if (*s != *p) return 0; s++; p++; }
  return 1;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static long to_long(const char *s) {
  long v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v;
}

/* Field `i` of a tab-separated line into out. */
static void field(const char *line, int i, char *out, int n) {
  int k = 0;
  while (i > 0 && *line && *line != '\n') { if (*line++ == '\t') i--; }
  while (*line && *line != '\t' && *line != '\n' && k < n - 1) out[k++] = *line++;
  out[k] = 0;
}

static void hms(uint32_t secs, char *out, int n) {
  api->fmt(out, (size_t)n, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600),
           (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
}

static void hm(uint32_t secs, char *out, int n) {
  api->fmt(out, (size_t)n, "%lu:%02lu", (unsigned long)(secs / 3600),
           (unsigned long)(secs / 60 % 60));
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
  field(line, 2, num, sizeof num);
  G.start = (uint32_t)to_long(num);
  field(line, 3, G.desc, DESC_MAX);
  field(line, 4, G.proj, PROJ_MAX);
  field(line, 5, num, sizeof num);
  G.colour = parse_colour(num);
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
      if (pass == 0 && starts(p, "running\t")) absorb_running(p);
      else if (G.nrec < MAX_ROWS &&
               ((pass == 0 && starts(p, "project\t")) || (pass == 1 && starts(p, "recent\t")))) {
        Recent *r = &G.rec[G.nrec++];
        r->kind = pass == 0 ? 'p' : 'r';
        field(p, 1, r->proj_id, sizeof r->proj_id);
        char hex[12];
        if (pass == 0) {
          r->desc[0] = 0;
          field(p, 2, r->proj, PROJ_MAX);
          field(p, 3, hex, sizeof hex);
        } else {
          field(p, 2, r->desc, DESC_MAX);
          field(p, 3, r->proj, PROJ_MAX);
          field(p, 4, hex, sizeof hex);
        }
        r->colour = parse_colour(hex);
      }
      while (*p && *p != '\n') p++;
      if (*p) p++;
    }
  if (G.sel > G.nrec) G.sel = G.nrec;
}

/* "error why" from the server, or a status code, as a line for the screen. */
static void say_failure(int n) {
  G.bad = 1;
  if (starts(G.reply, "error ")) api->fmt(G.status, sizeof G.status, "%s", G.reply + 6);
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
  if (G.stage != ST_IDLE) return -1;
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
           stage == ST_DESCRIBE ? "saving..." : "stopping...");
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
  if (G.stage == ST_STATUS) {
    absorb_status();
    G.status[0] = 0;
    G.stage = ST_IDLE;
  } else {
    /* A start answers with its running line, a stop with what stopped;
     * either way the list may have changed, so ask again. */
    if ((G.stage == ST_START || G.stage == ST_DESCRIBE) && starts(G.reply, "running\t")) {
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
    return rect(c.x, c.y + TOP_H + ROW_H + 2, c.w, h);
  }
  return rect(c.x, c.y + TOP_H, c.w, TIMER_H);
}

static void paint_timer(void) {
  CRect r = timer_rect();
  char t[16];
  uint32_t now = api->epoch();
  int big = timer_screen() && G.f_big >= 0, f = big ? G.f_big : G.f_num;
  api->fill(r, CLR_BG);
  if (!G.running) {
    draw(G.f_ui, r.x + 8, r.y + (r.h - height(G.f_ui)) / 2, "not running", CLR_DIM, CLR_BG);
    return;
  }
  if (now && now >= G.start) hms(now - G.start, t, sizeof t);
  else api->fmt(t, sizeof t, "0:00:00");
  if (timer_screen())
    draw(f, r.x + (r.w - width(f, t)) / 2, r.y + (r.h - height(f)) / 2, t,
         proj_colour(G.colour), CLR_BG);
  else
    draw(f, r.x + 8, r.y + (r.h - height(f)) / 2, t, proj_colour(G.colour), CLR_BG);
  G.shown_sec = now;
}

static void paint_row(int i, int y) {
  CRect c = G.content;
  uint16_t bg = i == G.sel ? CLR_SEL : CLR_BG;
  char line[80];
  api->fill(rect(c.x, y, c.w, ROW_H), bg);
  if (i == 0) {
    if (G.typing == 1) api->fmt(line, sizeof line, "new: %s_", G.draft);
    else api->fmt(line, sizeof line, "+ new entry");
    draw(G.f_ui, c.x + 8, y + (ROW_H - height(G.f_ui)) / 2, line,
         G.typing == 1 ? CLR_TEXT : CLR_DIM, bg);
    return;
  }
  if (G.rec[i - 1].kind == 'p') {
    Recent *r = &G.rec[i - 1];
    api->fill(rect(c.x + 8, y + ROW_H / 2 - 3, 6, 6), proj_colour(r->colour));
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
  api->fill(rect(c.x, c.y, c.w, TOP_H), CLR_BG);
  draw(G.f_uib, c.x + 8, c.y + (TOP_H - height(G.f_uib)) / 2, "Toggl", CLR_DIM, CLR_BG);
  if (G.status[0]) {
    w = width(G.f_ui, G.status);
    draw(G.f_ui, c.x + c.w - 8 - w, c.y + (TOP_H - height(G.f_ui)) / 2, G.status,
         G.bad ? CLR_BAD : CLR_DIM, CLR_BG);
  }
}

static void paint_foot(const char *keys) {
  CRect c = G.content;
  api->fill(rect(c.x, c.y + c.h - FOOT_H, c.w, FOOT_H), CLR_FOOT);
  api->text((int16_t)(c.x + 4), (int16_t)(c.y + c.h - FOOT_H + 2), keys, CLR_DIM, CLR_FOOT);
}

/* The timer, alone: project, time, description. */
static void paint_running(void) {
  CRect c = G.content, tr;
  const char *proj = G.proj[0] ? G.proj : "no project";
  char line[72];
  int y = c.y + TOP_H;
  api->fill(rect(c.x, y, c.w, c.h - TOP_H - FOOT_H), CLR_BG);
  draw(G.f_uib, c.x + (c.w - width(G.f_uib, proj)) / 2, y + (ROW_H - height(G.f_uib)) / 2,
       proj, G.proj[0] ? proj_colour(G.colour) : CLR_DIM, CLR_BG);
  paint_timer();
  tr = timer_rect();
  y = tr.y + tr.h + 2;
  if (G.typing == 2) api->fmt(line, sizeof line, "%s_", G.draft);
  else api->fmt(line, sizeof line, "%s", G.desc[0] ? G.desc : "no description");
  draw(G.f_ui, c.x + (c.w - width(G.f_ui, line)) / 2, y, line,
       G.typing == 2 || G.desc[0] ? CLR_TEXT : CLR_DIM, CLR_BG);
  paint_foot(G.typing == 2 ? "enter save  esc cancel" : "d describe  s stop  l list  n new");
}

static void paint_list(void) {
  CRect c = G.content;
  int y, i, rows, top = 0;
  paint_timer();
  y = c.y + TOP_H + TIMER_H;
  api->fill(rect(c.x, y, c.w, 2), CLR_BG);
  y += 2;
  rows = (c.y + c.h - FOOT_H - y) / ROW_H;
  if (G.sel >= rows) top = G.sel - rows + 1;
  for (i = top; i <= G.nrec && i - top < rows; i++, y += ROW_H) paint_row(i, y);
  if (y < c.y + c.h - FOOT_H) api->fill(rect(c.x, y, c.w, c.y + c.h - FOOT_H - y), CLR_BG);
  paint_foot(G.typing == 1 ? "enter start  esc cancel" :
             G.running ? "enter start  l timer  s stop  n new" :
                         "enter start  n new  r refresh");
}

static void app_paint(void *st, CRect c) {
  (void)st;
  G.content = c;
  paint_top();
  if (timer_screen()) paint_running();
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
  if (timer_screen()) {
    switch (k) {
    case 'l': case 'L': case CAPP_KEY_DOWN: G.list = 1; return 1;
    case 'd': case 'D': case 's': case 'S': case ' ': case 'n': case 'N': case 'r': case 'R':
      break;                                 /* as on the list, below */
    default: return 0;
    }
  }
  switch (k) {
  case 'l': case 'L': if (G.running) { G.list = 0; return 1; } return 0;
  case CAPP_KEY_UP:   if (G.sel > 0) G.sel--; return 1;
  case CAPP_KEY_DOWN: if (G.sel < G.nrec) G.sel++; return 1;
  case CAPP_KEY_ENTER:
    if (G.sel == 0) begin_typing();
    else start_entry(G.rec[G.sel - 1].desc, G.rec[G.sel - 1].proj_id);
    return 1;
  case 'n': case 'N': begin_typing(); return 1;
  case 'd': case 'D':
    if (!G.running) { api->fmt(G.status, sizeof G.status, "nothing is running"); return 1; }
    G.typing = 2;
    api->fmt(G.draft, sizeof G.draft, "%s", G.desc);   /* to change, not retype */
    G.dlen = (int)api->str_len(G.draft);
    return 1;
  case 's': case 'S': case ' ': stop_entry(); return 1;
  case 'r': case 'R': refresh(); return 1;
  default: return 0;
  }
}

static int app_wants_text(void *st) { (void)st; return G.typing != 0; }

static int app_tick(void *st, uint32_t now_ms) {
  int changed;
  (void)st; (void)now_ms;
  changed = poll();
  /* The timer is the only thing that moves: a second's change repaints
   * just its strip. */
  if (!changed && G.running && api->epoch() != G.shown_sec) {
    api->damage(timer_rect());
    return 1;
  }
  return changed;
}

/* ---- commands -------------------------------------------------------------------------- */

enum { ACT_STATUS = 1, ACT_START, ACT_STOP, ACT_TODAY, ACT_TOMORROW, ACT_PROJECT, ACT_DESCRIBE };

static const CappParam P_START[] = {
  { "what", CAPP_ARG_TEXT, "the description; a recent entry's project comes with it" },
};

static const CappParam P_PROJECT[] = {
  { "project", CAPP_ARG_TEXT, "the project, or the start of its name" },
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
  else if (starts(G.reply, "error ")) api->fmt(out, sz, "%s", G.reply + 6);
  else api->fmt(out, sz, "cannot reach the server (%d)", n);
  return -1;
}

/* Days since 1970 to a date: Howard Hinnant's civil_from_days. */
static void civil(long z, int *y, int *m, int *d) {
  long era, yoe, doy, mp;
  z += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  yoe = z - era * 146097;
  yoe = (yoe - yoe / 1460 + yoe / 36524 - yoe / 146096) / 365;
  doy = z - era * 146097 - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2));
}

static void rfc3339(uint32_t t, char *out, int n) {
  int y, m, d;
  civil((long)(t / 86400), &y, &m, &d);
  api->fmt(out, (size_t)n, "%04d-%02d-%02dT%02lu:%02lu:%02luZ", y, m, d,
           (unsigned long)(t / 3600 % 24), (unsigned long)(t / 60 % 60), (unsigned long)(t % 60));
}

/* Local seconds minus UTC seconds, from the kernel's two clocks. */
static long utc_offset(void) {
  CappTime lt;
  long local, utc, off;
  api->now(&lt);
  local = lt.hour * 3600L + lt.min * 60L + lt.sec;
  utc = (long)(api->epoch() % 86400);
  off = local - utc;
  if (off > 14 * 3600L) off -= 86400;
  if (off < -12 * 3600L) off += 86400;
  return off;
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
    field(p, 0, a, sizeof a);
    field(p, 1, b, sizeof b);
    if (starts(p, "total\t")) {
      hm((uint32_t)to_long(b), tbuf, sizeof tbuf);
      o += (size_t)api->fmt(out + o, n - o, "%stotal %s", any ? "" : "nothing tracked\n", tbuf);
    } else if (a[0] >= '0' && a[0] <= '9' && o + 100 < n) {
      long ls = (long)to_long(a) + off;
      field(p, 2, desc, sizeof desc);
      field(p, 3, proj, sizeof proj);
      hm((uint32_t)to_long(b), tbuf, sizeof tbuf);
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
    for (k = 0; what[k] && lower(what[k]) == lower(G.rec[i].desc[k]); k++) {}
    if (!what[k] && k > 0) return G.rec[i].proj_id;
  }
  return "";
}

/* The project whose name starts with `name`, ignoring case; -1 if none. */
static int find_project(const char *name) {
  int i, k;
  for (i = 0; i < G.nrec; i++) {
    if (G.rec[i].kind != 'p') continue;
    for (k = 0; name[k] && lower(name[k]) == lower(G.rec[i].proj[k]); k++) {}
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
    now = api->epoch();
    if (!G.running) { api->fmt(out, n, "not running"); return 0; }
    hm(now > G.start ? now - G.start : 0, t, sizeof t);
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
    if (starts(G.reply, "idle")) { api->fmt(out, n, "nothing was running"); return 0; }
    {
      char secs[16], desc[DESC_MAX];
      field(G.reply, 1, secs, sizeof secs);
      field(G.reply, 2, desc, sizeof desc);
      hm((uint32_t)to_long(secs), t, sizeof t);
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
    if (starts(G.reply, "idle")) { api->fmt(out, n, "nothing is running"); return -1; }
    absorb_running(G.reply);
    api->fmt(out, n, "%s%s%s", G.proj, G.proj[0] ? ": " : "", G.desc);
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
  "s\tstop the running timer\n"
  "n\ta new entry: type its description\n"
  "d\tdescribe the running entry\n"
  "l\tthe list, and back to the timer\n"
  "r\task Toggl again\n"
  "up/down\tchoose\n"
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
