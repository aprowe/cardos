/* Today: your day on one page, to read here or print.
 *
 * The page is sections, and every section is some other app's command --
 * the same `do APP COMMAND` the console, voice and Claude use. So this app
 * knows nothing about calendars or stocks: it asks, and puts the answers
 * under headings. The sections are /config/today.txt, one a line:
 *
 *     Calendar | calendar today
 *     To do    | todo show Chores
 *     Habits   | habits today
 *     Stocks   | stocks portfolio
 *
 * Anything an app can answer can be a section -- `do` in the console lists
 * every command there is -- and a new app with a command is a new section
 * without this file changing. Answers written as `[ ] task` print as boxes to
 * tick with a pen; Todo's `show` and Habits' `today` answer that way for this.
 *
 * r gathers again, fn-p prints (in the print fonts: fonts/fonts.txt), l
 * picks which todo list, e opens the sections in Edit. `do today print` is
 * the whole thing without the screen -- "Carlos, print my day".
 *
 * Gathering is one command a tick: a network one (stocks) takes a second or
 * two, and one at a time the screen says which it is waiting on rather than
 * freezing on the lot. What was gathered is what prints: nothing is fetched
 * twice.
 */

#include "kernel/app/capp.h"
#include "apps/toolbar.h"
#include "apps/safefile.h"

static const CardApi *api;

#define CONFIG      CAPP_CONFIG "/today.txt"
#define MAX_SECT    12
#define HEAD_MAX    24
#define CMD_MAX     64
#define ANSWER_MAX  1024
#define PAGE_MAX    4096
#define ROW_H       16
#define HEAD_H      20
#define FOOT_H      11
#define MAX_LISTS   12

#define CLR_BG      CAPP_RGB(14, 16, 22)
#define CLR_TEXT    CAPP_RGB(232, 236, 244)
#define CLR_DIM     CAPP_RGB(128, 136, 152)
#define CLR_ACCENT  CAPP_RGB(255, 176, 76)
#define CLR_BOX     CAPP_RGB(150, 160, 180)
#define CLR_DONE    CAPP_RGB(76, 196, 128)
#define CLR_SEL     CAPP_RGB(30, 40, 58)
#define CLR_FOOT    CAPP_RGB(30, 34, 44)
#define CLR_WARN    CAPP_RGB(236, 104, 84)

typedef struct {
  char head[HEAD_MAX];
  char app[16];
  char line[CMD_MAX];         /* the command and its arguments */
} Section;

enum { VIEW_PAGE = 0, VIEW_LISTS };

static struct {
  Section sect[MAX_SECT];
  int     nsect;

  char    page[PAGE_MAX];     /* the print markup, and what the screen shows */
  int     len;
  int     next;               /* the section gathering next, or -1 when done */
  int     announced;          /* the status says which, before it blocks */
  char    status[48];
  int     printing;

  int     view;
  int     top, rows, nlines;
  char    lists[MAX_LISTS][32];
  int     nlists, lsel;

  int     f_ui, f_uib;
  CRect   content;
} D;

/* What an app answered. One buffer for every question: they are asked one
 * at a time, and three of them were 2 KB this app's data did not need while
 * it loads the apps it asks (2026-09-29). */
static char s_answer[ANSWER_MAX];

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static int starts(const char *s, const char *p) {
  while (*p) { if (*s != *p) return 0; s++; p++; }
  return 1;
}

/* ---- the sections ------------------------------------------------------- */

static void trim_copy(char *out, int n, const char *s, int len) {
  int i = 0;
  while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
  while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) len--;
  for (; i < len && i < n - 1; i++) out[i] = s[i];
  out[i] = 0;
}

/* "Heading | app command args" into a section; 0 for a comment or junk. */
static int parse_section(const char *line, Section *s) {
  const char *bar = line, *p;
  int k;
  while (*bar && *bar != '|') bar++;
  if (!*bar || line[0] == '#') return 0;
  trim_copy(s->head, sizeof s->head, line, (int)(bar - line));
  p = bar + 1;
  while (*p == ' ') p++;
  for (k = 0; p[k] && p[k] != ' ' && k < (int)sizeof s->app - 1; k++) s->app[k] = p[k];
  s->app[k] = 0;
  p += k;
  trim_copy(s->line, sizeof s->line, p, (int)api->str_len(p));
  return s->head[0] && s->app[0] && s->line[0];
}

static void add_section(const char *head, const char *app, const char *line) {
  Section *s;
  if (D.nsect >= MAX_SECT) return;
  s = &D.sect[D.nsect++];
  api->fmt(s->head, sizeof s->head, "%s", head);
  api->fmt(s->app, sizeof s->app, "%s", app);
  api->fmt(s->line, sizeof s->line, "%s", line);
}

static int save_sections(void) {
  SafeFile f;
  char line[HEAD_MAX + CMD_MAX + 24];
  int i;
  if (safe_begin(&f, api, CONFIG) != 0) return -1;
  safe_line(&f, "# The Today app's page: one section a line, \"Heading | app command\".\n"
                "# Any app's command can be a section; `do` in the console lists them.\n");
  for (i = 0; i < D.nsect; i++) {
    api->fmt(line, sizeof line, "%s | %s %s\n", D.sect[i].head, D.sect[i].app, D.sect[i].line);
    safe_line(&f, line);
  }
  return safe_commit(&f);
}

/* The todo list Todo has open: the line `todo lists` marks with a star. */
static int current_list(char *out, int n) {
  char *answer = s_answer;
  const char *p = answer;
  if (api->run_command("todo", "lists", answer, ANSWER_MAX) != 0) return -1;
  while (*p) {
    const char *e = p;
    while (*e && *e != '\n') e++;
    if (p[0] == '*' && p[1] == ' ') { trim_copy(out, n, p + 2, (int)(e - p - 2)); return 0; }
    p = *e ? e + 1 : e;
  }
  return -1;
}

/* The page as it is on the card, or -- the first time -- the four sections
 * this was asked for, written out so they can be changed. */
static void load_sections(void) {
  char buf[256], line[HEAD_MAX + CMD_MAX + 24], list[40];
  int fd, n, i, len = 0;
  D.nsect = 0;
  fd = safe_open_read(api, CONFIG);
  if (fd >= 0) {
    while ((n = api->read(fd, buf, sizeof buf)) > 0)
      for (i = 0; i < n; i++) {
        if (buf[i] != '\n') { if (len < (int)sizeof line - 1) line[len++] = buf[i]; continue; }
        line[len] = 0;
        len = 0;
        if (D.nsect < MAX_SECT && parse_section(line, &D.sect[D.nsect])) D.nsect++;
      }
    if (len && D.nsect < MAX_SECT) { line[len] = 0; if (parse_section(line, &D.sect[D.nsect])) D.nsect++; }
    api->close(fd);
    return;
  }
  add_section("Calendar", "calendar", "today");
  if (current_list(list, sizeof list) == 0) {
    char cmd[CMD_MAX];
    api->fmt(cmd, sizeof cmd, "show %s", list);
    add_section("To do", "todo", cmd);
  } else {
    add_section("To do", "todo", "list");
  }
  add_section("Habits", "habits", "today");
  add_section("Stocks", "stocks", "portfolio");
  save_sections();
}

/* ---- the page ------------------------------------------------------------- */

static const char *const WDAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday" };
static const char *const MON[12] = { "January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November",
                                     "December" };

static void put(const char *s) {
  int n = (int)api->str_len(s);
  if (D.len + n >= PAGE_MAX - 1) n = PAGE_MAX - 1 - D.len;
  if (n <= 0) return;
  api->mem_cpy(D.page + D.len, s, (size_t)n);
  D.len += n;
  D.page[D.len] = 0;
}

static void begin_page(void) {
  CappTime t;
  char s[64];
  D.len = 0;
  D.page[0] = 0;
  api->now(&t);
  if (t.synced) api->fmt(s, sizeof s, "# %s %u %s\n", WDAY[t.wday % 7], t.day, MON[(t.month + 11) % 12]);
  else api->fmt(s, sizeof s, "# Today\n");
  put(s);
  D.next = 0;
  D.announced = 0;
}

/* One section: its heading, and the app's answer line by line. A section
 * whose app could not answer says so, in its own place, and the rest go on. */
static void gather_one(int i) {
  char *answer = s_answer;
  char head[HEAD_MAX + 8];
  int rc;
  answer[0] = 0;
  rc = api->run_command(D.sect[i].app, D.sect[i].line, answer, ANSWER_MAX);
  api->fmt(head, sizeof head, "\n## %s\n", D.sect[i].head);
  put(head);
  if (rc != 0) { put("("); put(answer[0] ? answer : "no answer"); put(")\n"); return; }
  put(answer);
  if (D.len && D.page[D.len - 1] != '\n') put("\n");
}

static void end_page(void) {
  CappTime t;
  char s[48];
  api->now(&t);
  put("\n---\n");
  if (t.synced) {
    api->fmt(s, sizeof s, "printed %u %.3s %02u:%02u\n", t.day, MON[(t.month + 11) % 12],
             t.hour, t.min);
    put(s);
  }
  D.next = -1;
}

/* All of it at once, for a command: no screen to show progress on. */
static void gather_all(void) {
  int i;
  begin_page();
  for (i = 0; i < D.nsect; i++) gather_one(i);
  end_page();
}

/* ---- painting ------------------------------------------------------------------ */

static int font_y(int f, int y, int h) { return y + (h - api->font_height(f)) / 2; }

/* One line of the page as the screen shows it: headings, boxes, text. */
static void paint_line(const char *s, int len, int y, int x, int w) {
  char buf[128];
  int n = len < (int)sizeof buf - 1 ? len : (int)sizeof buf - 1;
  api->mem_cpy(buf, s, (size_t)n);
  buf[n] = 0;
  api->fill(rect(x, y, w, ROW_H), CLR_BG);
  if (starts(buf, "## ")) {
    api->text_font(D.f_uib, (int16_t)(x + 6), (int16_t)font_y(D.f_uib, y, ROW_H), buf + 3,
                   CLR_ACCENT, CLR_BG);
  } else if (starts(buf, "[ ] ") || starts(buf, "[x] ")) {
    int done = buf[1] == 'x';
    int by = y + (ROW_H - 9) / 2;
    api->fill(rect(x + 8, by, 9, 9), done ? CLR_DONE : CLR_BOX);
    if (!done) api->fill(rect(x + 9, by + 1, 7, 7), CLR_BG);
    api->text_font(D.f_ui, (int16_t)(x + 22), (int16_t)font_y(D.f_ui, y, ROW_H), buf + 4,
                   done ? CLR_DIM : CLR_TEXT, CLR_BG);
  } else if (starts(buf, "---")) {
    api->fill(rect(x + 8, y + ROW_H / 2, w - 16, 1), CLR_DIM);
  } else if (buf[0] == '(') {
    api->text_font(D.f_ui, (int16_t)(x + 8), (int16_t)font_y(D.f_ui, y, ROW_H), buf, CLR_WARN, CLR_BG);
  } else {
    api->text_font(D.f_ui, (int16_t)(x + 8), (int16_t)font_y(D.f_ui, y, ROW_H), buf, CLR_TEXT, CLR_BG);
  }
}

static void paint_foot(const char *s, uint16_t fg) {
  CRect f = rect(D.content.x, D.content.y + D.content.h - FOOT_H, D.content.w, FOOT_H);
  api->fill(f, CLR_FOOT);
  api->text((int16_t)(f.x + 4), (int16_t)(f.y + 2), s, fg, CLR_FOOT);
}

static void paint_page(void) {
  CRect c = D.content;
  const char *p = D.page, *title = "Today";
  char head[48];
  int y = c.y + HEAD_H, row = 0, w;

  /* The title line is the header here; the rest scrolls under it. */
  if (starts(p, "# ")) {
    const char *e = p;
    while (*e && *e != '\n') e++;
    api->fmt(head, sizeof head, "%.*s", (int)(e - p - 2), p + 2);
    title = head;
    p = *e ? e + 1 : e;
  }
  api->fill(rect(c.x, c.y, c.w, HEAD_H), CLR_BG);
  api->text_font(D.f_uib, (int16_t)(c.x + 8), (int16_t)font_y(D.f_uib, c.y, HEAD_H), title,
                 CLR_TEXT, CLR_BG);
  w = api->text_width(D.f_ui, D.status);
  api->text_font(D.f_ui, (int16_t)(c.x + c.w - 8 - w), (int16_t)font_y(D.f_ui, c.y, HEAD_H),
                 D.status, CLR_DIM, CLR_BG);

  D.rows = (c.h - HEAD_H - FOOT_H) / ROW_H;
  D.nlines = 0;
  while (*p) {
    const char *e = p;
    while (*e && *e != '\n') e++;
    if (e > p) {                          /* blank lines are the printer's */
      if (D.nlines >= D.top && row < D.rows) {
        paint_line(p, (int)(e - p), y, c.x, c.w);
        y += ROW_H;
        row++;
      }
      D.nlines++;
    }
    p = *e ? e + 1 : e;
  }
  if (y < c.y + c.h - FOOT_H) api->fill(rect(c.x, y, c.w, c.y + c.h - FOOT_H - y), CLR_BG);
  paint_foot("fn-p print  r again  l list  e sections", CLR_DIM);
}

static void paint_lists(void) {
  CRect c = D.content;
  int i, y = c.y + HEAD_H;
  api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_BG);
  api->text_font(D.f_uib, (int16_t)(c.x + 8), (int16_t)font_y(D.f_uib, c.y, HEAD_H),
                 "Which todo list?", CLR_TEXT, CLR_BG);
  if (!D.nlists)
    api->text_font(D.f_ui, (int16_t)(c.x + 8), (int16_t)y, "no lists: open Todo once",
                   CLR_DIM, CLR_BG);
  for (i = 0; i < D.nlists && y + ROW_H <= c.y + c.h - FOOT_H; i++, y += ROW_H) {
    uint16_t bg = i == D.lsel ? CLR_SEL : CLR_BG;
    api->fill(rect(c.x, y, c.w, ROW_H), bg);
    api->text_font(D.f_ui, (int16_t)(c.x + 8), (int16_t)font_y(D.f_ui, y, ROW_H), D.lists[i],
                   CLR_TEXT, bg);
  }
  paint_foot("enter uses it  esc back", CLR_DIM);
}

static void app_paint(void *st, CRect full) {
  (void)st;
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  D.content = toolbar_rest(full);
  if (D.view == VIEW_LISTS) paint_lists(); else paint_page();
  toolbar_paint_menu(full);
}

/* ---- doing things ------------------------------------------------------------------ */

static int print_page(void) {
  int rc = api->print_fonts(D.page, "print24", "print24b", "print34b");
  if (rc == 0) { D.printing = 1; api->fmt(D.status, sizeof D.status, "printing"); }
  else if (rc == -1) api->fmt(D.status, sizeof D.status, "printer busy");
  else if (rc == -2) api->fmt(D.status, sizeof D.status, "no printer set");
  else api->fmt(D.status, sizeof D.status, "no memory to print");
  return rc;
}

static void load_fonts(void) {
  if (D.f_ui < 0) D.f_ui = api->font_load("ui13");
  if (D.f_uib < 0) D.f_uib = api->font_load("ui13b");
}

/* Gathering loads Calendar and Todo -- 30 KB each with their data -- beside
 * this app, and they need one unbroken block for their code. The fonts wait
 * until they have been and gone; the screen says "asking..." in 6x8 until
 * then, and nothing is lost by that. */
static void unload_fonts(void) {
  if (D.f_ui >= 0) api->font_free(D.f_ui);
  if (D.f_uib >= 0) api->font_free(D.f_uib);
  D.f_ui = D.f_uib = -1;
}

static void regather(void) {
  unload_fonts();
  begin_page();
  D.top = 0;
  api->fmt(D.status, sizeof D.status, "gathering");
}

/* The todo lists, from Todo, to pick one. */
static void open_lists(void) {
  char *answer = s_answer;
  const char *p = answer;
  D.nlists = 0;
  D.lsel = 0;
  if (api->run_command("todo", "lists", answer, ANSWER_MAX) == 0)
    while (*p && D.nlists < MAX_LISTS) {
      const char *e = p;
      int star = p[0] == '*' && p[1] == ' ';
      while (*e && *e != '\n') e++;
      trim_copy(D.lists[D.nlists], sizeof D.lists[0], p + (star ? 2 : 0),
                (int)(e - p) - (star ? 2 : 0));
      if (star) D.lsel = D.nlists;
      if (D.lists[D.nlists][0]) D.nlists++;
      p = *e ? e + 1 : e;
    }
  D.view = VIEW_LISTS;
}

/* Point the todo section at the chosen list -- or add one if the page has
 * none -- and save, so it is the list next time too. */
static void use_list(int i) {
  char cmd[CMD_MAX];
  int k, found = 0;
  if (i < 0 || i >= D.nlists) return;
  api->fmt(cmd, sizeof cmd, "show %s", D.lists[i]);
  for (k = 0; k < D.nsect; k++)
    if (starts(D.sect[k].app, "todo") && !D.sect[k].app[4]) {
      api->fmt(D.sect[k].line, sizeof D.sect[k].line, "%s", cmd);
      found = 1;
    }
  if (!found) add_section("To do", "todo", cmd);
  save_sections();
  D.view = VIEW_PAGE;
  regather();
}

enum { ACT_PRINT = 1, ACT_AGAIN, ACT_LISTS, ACT_EDIT, ACT_SHOW };

static const CappAction ACTIONS[] = {
  { "print",    "Print",          "Today", CAPP_KEY_PRINT, ACT_PRINT,     /* fn-p */
    "print today's page -- calendar, todo list, habits, stocks: the sections in "
    "/config/today.txt", 0, 0, CAPP_CMD_YES },
  { "again",    "Gather again",   "Today", 0, ACT_AGAIN },
  { "lists",    "Todo list...",   "Today", 0, ACT_LISTS },
  { "sections", "Edit sections",  "Today", 0, ACT_EDIT },
  { "show",     "Show",           0,       0, ACT_SHOW,
    "today's page as text, without printing it", 0, 0, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int do_action(int a) {
  switch (a) {
  case ACT_PRINT:
    if (D.next >= 0) { api->fmt(D.status, sizeof D.status, "still gathering"); return 1; }
    print_page();
    return 1;
  case ACT_AGAIN: regather(); return 1;
  case ACT_LISTS: open_lists(); return 1;
  case ACT_EDIT:  api->run("edit", CONFIG); return 1;
  default:        return 0;
  }
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  (void)st; (void)argc; (void)argv;
  gather_all();
  if (action == ACT_SHOW) { api->fmt(out, n, "%s", D.page); return 0; }
  if (action == ACT_PRINT) {
    if (print_page() != 0) { api->fmt(out, n, "%s", D.status); return -1; }
    api->fmt(out, n, "printing today's page, %d sections", D.nsect);
    return 0;
  }
  api->fmt(out, n, "today has no command %d", action);
  return -1;
}

static int app_key(void *st, uint8_t k) {
  int a = toolbar_key(k);
  (void)st;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  if (D.view == VIEW_LISTS) {
    switch (k) {
    case CAPP_KEY_UP:   if (D.lsel > 0) D.lsel--; return 1;
    case CAPP_KEY_DOWN: if (D.lsel + 1 < D.nlists) D.lsel++; return 1;
    case CAPP_KEY_ENTER: use_list(D.lsel); return 1;
    case CAPP_KEY_ESC: case CAPP_KEY_BACK: D.view = VIEW_PAGE; return 1;
    default: return 1;
    }
  }
  switch (k) {
  case CAPP_KEY_UP:   if (D.top > 0) D.top--; return 1;
  case CAPP_KEY_DOWN: if (D.top + D.rows < D.nlines) D.top++; return 1;
  case 'r': case 'R': return do_action(ACT_AGAIN);
  case 'l': case 'L': return do_action(ACT_LISTS);
  case 'e': case 'E': return do_action(ACT_EDIT);
  case 'p': case 'P': return do_action(ACT_PRINT);
  default: return 0;
  }
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int ch;
  (void)st; (void)buttons;
  ch = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) ch = 1;
  if (wheel) {
    D.top += wheel > 0 ? -1 : 1;
    if (D.top < 0) D.top = 0;
    if (D.top + D.rows > D.nlines) D.top = D.nlines > D.rows ? D.nlines - D.rows : 0;
    ch = 1;
  }
  return ch;
}

static int app_click(void *st, int16_t x, int16_t y, int button) {
  int a;
  (void)st; (void)button;
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;
  return a != TB_NONE ? do_action(a) : 0;
}

/* One section a tick: say which, repaint, then ask for it -- so the screen
 * shows what it is waiting on while a network command takes its seconds. */
static int app_tick(void *st, uint32_t now) {
  (void)st; (void)now;
  if (D.next >= 0) {
    if (D.next >= D.nsect) {
      end_page();
      load_fonts();
      api->fmt(D.status, sizeof D.status, "fn-p prints");
      return 1;
    }
    if (!D.announced) {
      api->fmt(D.status, sizeof D.status, "asking %s...", D.sect[D.next].app);
      D.announced = 1;
      return 1;
    }
    gather_one(D.next++);
    D.announced = 0;
    return 1;
  }
  if (D.printing) {
    const char *ps = api->print_status();
    if (ps[0] && !starts(ps, "printing") && !starts(ps, "connecting") &&
        !starts(ps, "starting") && !starts(ps, "waiting"))
      D.printing = 0;
    if (!starts(D.status, ps)) { api->fmt(D.status, sizeof D.status, "%s", ps); return 1; }
  }
  return 0;
}

static int app_wants_text(void *st) { (void)st; return 0; }

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Today",
  /* 16x16: a page with a sun rising over it. */
  { 0x01, 0x80, 0x11, 0x88, 0x08, 0x10, 0x03, 0xC0,
    0x37, 0xEC, 0x07, 0xE0, 0x7F, 0xFE, 0x40, 0x02,
    0x5E, 0x02, 0x40, 0x02, 0x5F, 0xE2, 0x40, 0x02,
    0x5C, 0x02, 0x40, 0x02, 0x7F, 0xFE, 0x00, 0x00 },
  "fn-p\tprint the page\nr\tgather again\nl\twhich todo list\n"
  "e\tedit the sections (/config/today.txt)\nup/down\tscroll\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&D, 0, sizeof D);
  D.f_ui = D.f_uib = -1;
  D.next = -1;
  load_sections();
  if (!api->headless()) {
    toolbar_init(api, ACTIONS, NACT, 0, 0);
    regather();                 /* in tick, one section at a time; fonts after */
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
