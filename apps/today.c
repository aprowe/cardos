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
 * r gathers again, p or fn-p prints (in the print fonts: fonts/fonts.txt), e
 * opens the sections in Edit -- with a comment line naming the todo lists,
 * so choosing one is typing its name into the To do line. The page gathers
 * again by itself when the file changes. `do today print` is
 * the whole thing without the screen -- "Carlos, print my day".
 *
 * t turns the page to tomorrow and back: a section whose command is `today`
 * (Calendar's, Habits') is asked `tomorrow` instead, and the rest as they
 * are. `do today tomorrow` prints it, for the night before.
 *
 * Two sections come from the server rather than an app: `daily focus` (one
 * thing to be mindful of today) and `daily fact` (a fun fact), the same all
 * day (server/daily.py). Lines written `%bar NN label` -- Toggl's targets --
 * are drawn as bars here and printed as bars.
 *
 * Gathering is one command a tick: a network one (stocks) takes a second or
 * two, and one at a time the screen says which it is waiting on rather than
 * freezing on the lot. What was gathered is what prints: nothing is fetched
 * twice.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/toolbar.h"
#include "apps/safefile.h"
#include "apps/footer.h"

static const CardApi *api;

#define CONFIG      CAPP_CONFIG "/today.txt"
#define MAX_SECT    12
#define HEAD_MAX    24
#define CMD_MAX     64
#define ANSWER_MAX  1024
#define PAGE_MAX    4096
#define ROW_H       16
#define HEAD_H      20

#define CLR_BG      CAPP_RGB(14, 16, 22)
#define CLR_TEXT    CAPP_RGB(232, 236, 244)
#define CLR_DIM     CAPP_RGB(128, 136, 152)
#define CLR_ACCENT  CAPP_RGB(255, 176, 76)
#define CLR_BOX     CAPP_RGB(150, 160, 180)
#define CLR_DONE    CAPP_RGB(76, 196, 128)
#define CLR_WARN    CAPP_RGB(236, 104, 84)

typedef struct {
  char head[HEAD_MAX];
  char app[16];
  char line[CMD_MAX];         /* the command and its arguments */
} Section;


static struct {
  Section sect[MAX_SECT];
  int     nsect;

  char    page[PAGE_MAX];     /* the print markup, and what the screen shows */
  int     len;
  int     next;               /* the section gathering next, or -1 when done */
  int     ahead;              /* 1: tomorrow's page */
  int     announced;          /* the status says which, before it blocks */
  char    status[48];
  int     printing;

  int     top, rows, nlines;
  int32_t cfg_size;           /* the sections file as last read, to see edits */
  uint32_t cfg_check;         /* when it was last looked at */

  int     f_ui, f_uib;
  CRect   content;
} D;

/* What an app answered. One buffer for every question: they are asked one
 * at a time, and three of them were 2 KB this app's data did not need while
 * it loads the apps it asks (2026-09-29). */
static char s_answer[ANSWER_MAX];

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

/* "# todo lists: Home, Chores (open in Todo)" from Todo's answer, where the
 * open one is starred. A line in the script rather than a picker of its
 * own: the To do line is chosen by typing a name, beside the names. */
static int lists_line(char *out, int n) {
  char *answer = s_answer;
  const char *p = answer;
  int o, first = 1;
  if (api->run_command("todo", "lists", answer, ANSWER_MAX) != 0) return -1;
  o = api->fmt(out, (size_t)n, "# todo lists:");
  while (*p && o < n - 1) {
    const char *e = p;
    int star = p[0] == '*' && p[1] == ' ';
    char name[40];
    while (*e && *e != '\n') e++;
    trim_copy(name, sizeof name, p + (star ? 2 : 0), (int)(e - p) - (star ? 2 : 0));
    if (name[0])
      o += api->fmt(out + o, (size_t)(n - o), "%s %s%s", first ? "" : ",", name,
                    star ? " (open in Todo)" : "");
    if (name[0]) first = 0;
    p = *e ? e + 1 : e;
  }
  if (o < n - 1) o += api->fmt(out + o, (size_t)(n - o), "\n");
  return o < n - 1 ? 0 : -1;
}

/* The file with `line` in place of any lists line it had, put after the
 * comments it starts with. Everything else stays where it was. Length, or
 * -1 if it does not fit. */
static int with_lists_line(const char *file, const char *line, char *out, int n) {
  const char *p = file;
  int o = 0, put_in = 0, k;
  while (1) {
    const char *e = p;
    int is_lists, is_comment;
    while (*e && *e != '\n') e++;
    is_lists = str_starts(p, "# todo lists:");
    is_comment = p[0] == '#';
    if (!put_in && (!is_comment || !*p) && !is_lists) {
      for (k = 0; line[k]; k++) { if (o >= n - 1) return -1; out[o++] = line[k]; }
      put_in = 1;
    }
    if (!*p) break;
    if (!is_lists) {
      for (k = 0; p + k < e; k++) { if (o >= n - 2) return -1; out[o++] = p[k]; }
      out[o++] = '\n';
    }
    p = *e ? e + 1 : e;
  }
  out[o] = 0;
  return o;
}

/* Before Edit opens: the file, with the lists as they are now. The page
 * buffer is borrowed, so 1 says the page is gone and the caller gathers it
 * again. */
static int refresh_lists_in_file(void) {
  char line[200];
  int fd, n, len = 0;
  SafeFile f;
  if (lists_line(line, sizeof line) != 0) return 0;
  fd = safe_open_read(api, CONFIG);
  if (fd < 0) return 0;
  while (len < PAGE_MAX / 2 - 1 && (n = api->read(fd, D.page + len, (size_t)(PAGE_MAX / 2 - 1 - len))) > 0)
    len += n;
  api->close(fd);
  D.page[len] = 0;
  D.len = 0;
  if (with_lists_line(D.page, line, D.page + PAGE_MAX / 2, PAGE_MAX / 2) >= 0 &&
      safe_begin(&f, api, CONFIG) == 0) {
    safe_line(&f, D.page + PAGE_MAX / 2);
    safe_commit(&f);
  }
  D.page[0] = 0;
  return 1;
}

static int32_t config_size(void) {
  CappStat st;
  return api->stat && api->stat(CONFIG, &st) == 0 ? (int32_t)st.size : -1;
}

/* The page as it is on the card, or -- the first time -- the four sections
 * this was asked for, written out so they can be changed. */
static void load_sections(void) {
  char buf[256], line[HEAD_MAX + CMD_MAX + 24], list[40];
  int fd, n, i, len = 0;
  D.nsect = 0;
  D.cfg_size = config_size();
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
  add_section("Focus", "daily", "focus");
  add_section("Fun fact", "daily", "fact");
  save_sections();
  D.cfg_size = config_size();
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

static void next_day(int *y, int *m, int *d) {
  static const unsigned char LEN[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  int len = LEN[(*m + 11) % 12];
  if (*m == 2 && *y % 4 == 0 && (*y % 100 != 0 || *y % 400 == 0)) len = 29;
  if (++*d > len) { *d = 1; if (++*m > 12) { *m = 1; ++*y; } }
}

static void begin_page(void) {
  CappTime t;
  char s[64];
  D.len = 0;
  D.page[0] = 0;
  api->now(&t);
  if (t.synced) {
    int y = t.year, m = t.month, d = t.day, wd = t.wday % 7;
    if (D.ahead) { next_day(&y, &m, &d); wd = (wd + 1) % 7; }
    api->fmt(s, sizeof s, "# %s %d %s\n", WDAY[wd], d, MON[(m + 11) % 12]);
  } else {
    api->fmt(s, sizeof s, D.ahead ? "# Tomorrow\n" : "# Today\n");
  }
  put(s);
  D.next = 0;
  D.announced = 0;
}

/* One section: its heading, and the app's answer line by line. A section
 * whose app could not answer says so, in its own place, and the rest go on. */
/* `daily focus` / `daily fact`: the server's line for this page's date. */
static int daily_answer(const char *kind, char *out, int n) {
  CappTime t;
  char url[128];
  int y, m, d, r;
  api->now(&t);
  if (!t.synced) { api->fmt(out, (size_t)n, "the clock is not set"); return -1; }
  y = t.year; m = t.month; d = t.day;
  if (D.ahead) next_day(&y, &m, &d);
  api->fmt(url, sizeof url, "%s/daily?date=%04d-%02d-%02d&kind=%s", api->proxy(), y, m, d, kind);
  r = api->http("GET", url, 0, 0, "", out, (size_t)n, 120000);
  if (r < 0) { api->fmt(out, (size_t)n, "the server did not answer (%d)", r); return -1; }
  return 0;
}

static void gather_one(int i) {
  char *answer = s_answer;
  char head[HEAD_MAX + 8];
  int rc;
  const char *line = D.sect[i].line;
  char moved[CMD_MAX + 8];
  answer[0] = 0;
  /* Tomorrow's page: "today" -- the whole command or its first word -- is
   * asked as "tomorrow". */
  if (D.ahead && str_starts(line, "today") && (line[5] == 0 || line[5] == ' ')) {
    api->fmt(moved, sizeof moved, "tomorrow%s", line + 5);
    line = moved;
  }
  if (str_starts(D.sect[i].app, "daily") && !D.sect[i].app[5])
    rc = daily_answer(D.sect[i].line, answer, ANSWER_MAX);
  else
    rc = api->run_command(D.sect[i].app, line, answer, ANSWER_MAX);
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

/* r in the background, less the part `hole` covers: up to four fills round
 * it. The hole is where something is about to be drawn that paints all of
 * its own pixels -- text_font fills behind its glyphs -- and filling under
 * it first is a blink of it on every repaint. */
static void fill_round(CRect r, CRect hole) {
  int x0 = hole.x > r.x ? hole.x : r.x;
  int y0 = hole.y > r.y ? hole.y : r.y;
  int x1 = hole.x + hole.w < r.x + r.w ? hole.x + hole.w : r.x + r.w;
  int y1 = hole.y + hole.h < r.y + r.h ? hole.y + hole.h : r.y + r.h;
  if (r.w <= 0 || r.h <= 0) return;
  if (x0 >= x1 || y0 >= y1) { api->fill(r, CLR_BG); return; }
  if (y0 > r.y) api->fill(capp_rect(r.x, r.y, r.w, y0 - r.y), CLR_BG);
  if (y1 < r.y + r.h) api->fill(capp_rect(r.x, y1, r.w, r.y + r.h - y1), CLR_BG);
  if (x0 > r.x) api->fill(capp_rect(r.x, y0, x0 - r.x, y1 - y0), CLR_BG);
  if (x1 < r.x + r.w) api->fill(capp_rect(x1, y0, r.x + r.w - x1, y1 - y0), CLR_BG);
}

/* Text at tx in font f, centred in the row, and the background of
 * [x0, x1) round it -- but not under it. */
static void row_text(int f, int x0, int x1, int y, int tx, const char *s, uint16_t fg) {
  int fy = font_y(f, y, ROW_H);
  fill_round(capp_rect(x0, y, x1 - x0, ROW_H), capp_rect(tx, fy, api->text_width(f, s), api->font_height(f)));
  api->text_font(f, (int16_t)tx, (int16_t)fy, s, fg, CLR_BG);
}

/* One line of the page as the screen shows it: headings, boxes, text.
 * Nothing is drawn twice: each part of the row is filled or drawn once. */
static void paint_line(const char *s, int len, int y, int x, int w) {
  char buf[128];
  int n = len < (int)sizeof buf - 1 ? len : (int)sizeof buf - 1;
  api->mem_cpy(buf, s, (size_t)n);
  buf[n] = 0;
  if (str_starts(buf, "%bar ")) {
    /* "%bar NN label": the label, and a bar filled NN percent on the right. */
    const char *p = buf + 5;
    int pct = 0, bw = 70, bx = x + w - 8 - bw, by = y + ROW_H / 2 - 4, f;
    while (*p >= '0' && *p <= '9') pct = pct * 10 + (*p++ - '0');
    while (*p == ' ') p++;
    if (pct > 100) pct = 100;
    row_text(D.f_ui, x, bx, y, x + 8, p, CLR_TEXT);
    fill_round(capp_rect(bx, y, x + w - bx, ROW_H), capp_rect(bx, by, bw, 8));
    /* The bar's outline, its filled part and the rest, side by side rather
     * than the whole bar and the filled part over it. */
    f = (bw - 2) * pct / 100;
    api->frame(capp_rect(bx, by, bw, 8), CLR_BOX);
    if (f > 0) api->fill(capp_rect(bx + 1, by + 1, f, 6), CLR_DONE);
    if (f < bw - 2) api->fill(capp_rect(bx + 1 + f, by + 1, bw - 2 - f, 6), CLR_BOX);
    return;
  }
  if (str_starts(buf, "## ")) {
    row_text(D.f_uib, x, x + w, y, x + 6, buf + 3, CLR_ACCENT);
  } else if (str_starts(buf, "[ ] ") || str_starts(buf, "[x] ")) {
    int done = buf[1] == 'x';
    int by = y + (ROW_H - 9) / 2;
    fill_round(capp_rect(x, y, 22, ROW_H), capp_rect(x + 8, by, 9, 9));
    if (done) {
      api->fill(capp_rect(x + 8, by, 9, 9), CLR_DONE);
    } else {
      api->frame(capp_rect(x + 8, by, 9, 9), CLR_BOX);
      api->fill(capp_rect(x + 9, by + 1, 7, 7), CLR_BG);
    }
    row_text(D.f_ui, x + 22, x + w, y, x + 22, buf + 4, done ? CLR_DIM : CLR_TEXT);
  } else if (str_starts(buf, "---")) {
    fill_round(capp_rect(x, y, w, ROW_H), capp_rect(x + 8, y + ROW_H / 2, w - 16, 1));
    api->fill(capp_rect(x + 8, y + ROW_H / 2, w - 16, 1), CLR_DIM);
  } else if (buf[0] == '(') {
    row_text(D.f_ui, x, x + w, y, x + 8, buf, CLR_WARN);
  } else {
    row_text(D.f_ui, x, x + w, y, x + 8, buf, CLR_TEXT);
  }
}

/* The shared hint bar, apps/footer.h. */
static void paint_foot(const char *s) {
  footer_paint(api, D.content, s);
}

/* The header: the title on the left, the status on the right, and the
 * background round them -- split between the two, so a status that got
 * shorter is covered by the fill and neither text is filled under. */
static void paint_head(const char *title) {
  CRect c = D.content;
  int tw = api->text_width(D.f_uib, title), th = api->font_height(D.f_uib);
  int sw = api->text_width(D.f_ui, D.status), sh = api->font_height(D.f_ui);
  int tx = c.x + 8, sx = c.x + c.w - 8 - sw, mid;
  int ty = font_y(D.f_uib, c.y, HEAD_H), sy = font_y(D.f_ui, c.y, HEAD_H);
  mid = (tx + tw + sx) / 2;
  if (mid < c.x) mid = c.x;
  if (mid > c.x + c.w) mid = c.x + c.w;
  fill_round(capp_rect(c.x, c.y, mid - c.x, HEAD_H), capp_rect(tx, ty, tw, th));
  fill_round(capp_rect(mid, c.y, c.x + c.w - mid, HEAD_H), capp_rect(sx, sy, sw, sh));
  api->text_font(D.f_uib, (int16_t)tx, (int16_t)ty, title, CLR_TEXT, CLR_BG);
  api->text_font(D.f_ui, (int16_t)sx, (int16_t)sy, D.status, CLR_DIM, CLR_BG);
}

static CRect head_rect(void) {
  return capp_rect(D.content.x, D.content.y, D.content.w, HEAD_H);
}

/* Only the status changed: mark the header, and the shell clips the repaint
 * to it -- "asking calendar..." used to repaint the whole page, ten to
 * twenty times a gathering. */
static int status_changed(void) {
  if (D.content.w > 0) api->damage(head_rect());
  return 1;
}

static void paint_page(void) {
  CRect c = D.content, a = api->paint_area();
  const char *p = D.page, *title = "Today";
  char head[48];
  int y = c.y + HEAD_H, row = 0;

  /* The title line is the header here; the rest scrolls under it. */
  if (str_starts(p, "# ")) {
    const char *e = p;
    while (*e && *e != '\n') e++;
    api->fmt(head, sizeof head, "%.*s", (int)(e - p - 2), p + 2);
    title = head;
    p = *e ? e + 1 : e;
  }
  if (capp_overlaps(a, head_rect())) paint_head(title);

  D.rows = (c.h - HEAD_H - FOOT_H) / ROW_H;
  D.nlines = 0;
  while (*p) {
    const char *e = p;
    while (*e && *e != '\n') e++;
    if (e > p) {                          /* blank lines are the printer's */
      if (D.nlines >= D.top && row < D.rows) {
        if (capp_overlaps(a, capp_rect(c.x, y, c.w, ROW_H))) paint_line(p, (int)(e - p), y, c.x, c.w);
        y += ROW_H;
        row++;
      }
      D.nlines++;
    }
    p = *e ? e + 1 : e;
  }
  if (y < c.y + c.h - FOOT_H) api->fill(capp_rect(c.x, y, c.w, c.y + c.h - FOOT_H - y), CLR_BG);
  paint_foot(D.ahead ? "p print  t today  e edit sections"
                     : "p print  t tomorrow  e edit sections");
}

/* The page's lines as paint_page counts them: not the title, not blanks. */
static int body_lines(void) {
  const char *p = D.page;
  int n = 0;
  if (str_starts(p, "# ")) {
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
  while (*p) {
    const char *e = p;
    while (*e && *e != '\n') e++;
    if (e > p) n++;
    p = *e ? e + 1 : e;
  }
  return n;
}

/* A section landed: lines were added after the `before` there were. Mark
 * the rows from the first new one down, or nothing when they are below the
 * screen -- what is already there is not drawn again. */
static int page_grew(int before) {
  CRect c = D.content;
  int row = before - D.top;
  if (D.rows <= 0 || c.w <= 0) return 1;     /* never painted: all of it */
  if (row < 0) row = 0;
  if (row >= D.rows || body_lines() == before) return 0;
  api->damage(capp_rect(c.x, c.y + HEAD_H + row * ROW_H, c.w, (D.rows - row) * ROW_H));
  return 1;
}

static void app_paint(void *st, CRect full) {
  (void)st;
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  D.content = toolbar_rest(full);
  paint_page();
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

enum { ACT_PRINT = 1, ACT_AGAIN, ACT_EDIT, ACT_SHOW, ACT_DAY, ACT_TOMORROW,
       ACT_SHOW_TOMORROW };

static const CappAction ACTIONS[] = {
  { "print",    "Print",          "Today", CAPP_KEY_PRINT, ACT_PRINT,     /* fn-p */
    "print today's page -- calendar, todo list, habits, stocks: the sections in "
    "/config/today.txt", 0, 0, CAPP_CMD_YES },
  { "day",      "Today/tomorrow", "Today", 0, ACT_DAY },
  { "again",    "Gather again",   "Today", 0, ACT_AGAIN },
  { "sections", "Edit sections",  "Today", 0, ACT_EDIT },
  { "show",     "Show",           0,       0, ACT_SHOW,
    "today's page as text, without printing it", 0, 0, CAPP_CMD_YES },
  { "tomorrow", "Print tomorrow", 0,     0, ACT_TOMORROW,
    "print tomorrow's page: tomorrow's events, the todo list, habits to tick", 0, 0,
    CAPP_CMD_YES },
  { "show.tomorrow", "Show tomorrow", 0, 0, ACT_SHOW_TOMORROW,
    "tomorrow's page as text, without printing it", 0, 0, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int do_action(int a) {
  switch (a) {
  case ACT_PRINT:
    if (D.next >= 0) { api->fmt(D.status, sizeof D.status, "still gathering"); return 1; }
    print_page();
    return 1;
  case ACT_AGAIN: regather(); return 1;
  case ACT_DAY:   D.ahead = !D.ahead; regather(); return 1;
  case ACT_EDIT: {
    /* The lists line borrows the page's buffer. It used to be left blank on
     * the promise that the page would be gathered again when the file
     * changed -- and coming back from Edit without saving, nothing changed
     * and the page stayed empty. So a borrowed page is gathered again at
     * once, on screen as any other gathering is. In the launcher Today is
     * closed while Edit has the screen and this is moot; on the desktop
     * the two are side by side. */
    int lost = refresh_lists_in_file();
    D.cfg_size = config_size();
    api->run("edit", CONFIG);
    if (lost) regather();
    return 1;
  }
  default:        return 0;
  }
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  (void)st; (void)argc; (void)argv;
  D.ahead = action == ACT_TOMORROW || action == ACT_SHOW_TOMORROW;
  gather_all();
  if (action == ACT_SHOW || action == ACT_SHOW_TOMORROW) {
    api->fmt(out, n, "%s", D.page);
    return 0;
  }
  if (action == ACT_PRINT || action == ACT_TOMORROW) {
    if (print_page() != 0) { api->fmt(out, n, "%s", D.status); return -1; }
    api->fmt(out, n, "printing %s page, %d sections", D.ahead ? "tomorrow's" : "today's",
             D.nsect);
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
  switch (k) {
  case CAPP_KEY_UP:   if (D.top > 0) D.top--; return 1;
  case CAPP_KEY_DOWN: if (D.top + D.rows < D.nlines) D.top++; return 1;
  case 'r': case 'R': return do_action(ACT_AGAIN);
  case 't': case 'T': return do_action(ACT_DAY);
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
  (void)st;
  if (D.next >= 0) {
    if (D.next >= D.nsect) {
      end_page();
      load_fonts();
      D.status[0] = 0;
      return 1;
    }
    if (!D.announced) {
      api->fmt(D.status, sizeof D.status, "asking %s...", D.sect[D.next].app);
      D.announced = 1;
      return status_changed();
    }
    {
      int before = body_lines();
      gather_one(D.next++);
      D.announced = 0;
      return page_grew(before);
    }
  }
  if ((int32_t)(now - D.cfg_check) > 1000) {
    int32_t sz;
    D.cfg_check = now;
    sz = config_size();
    if (sz != D.cfg_size) {
      load_sections();
      regather();
      return 1;
    }
  }
  if (D.printing) {
    const char *ps = api->print_status();
    if (ps[0] && !str_starts(ps, "printing") && !str_starts(ps, "connecting") &&
        !str_starts(ps, "starting") && !str_starts(ps, "waiting"))
      D.printing = 0;
    if (!str_starts(D.status, ps)) {
      api->fmt(D.status, sizeof D.status, "%s", ps);
      return status_changed();
    }
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
  "p\tprint the page\nt\ttomorrow's page, and back\nr\tgather again\n"
  "e\tedit the sections; the todo lists are named in it\nup/down\tscroll\n",
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
