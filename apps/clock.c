/* Clock: the time, big, and the alarms.
 *
 * THE FACE. Hours and minutes in clock56 (Space Mono Bold, fonts/fonts.txt)
 * with the seconds smaller beside them, the date, and the next alarm and how
 * long until it. k keeps the screen on -- a clock on a nightstand is watched,
 * not touched (API 32) -- until k again or the app closes.
 *
 * THE ALARMS (a, or Enter). A list with a switch each: space turns one on or
 * off, Enter edits, n makes a new one, d deletes after asking. The editor has
 * the time (up and down, or type the digits), the seven days (space on a day;
 * none at all means once), and a label.
 *
 * THIS APP DOES NOT RING THEM. The kernel does (kernel/sys/alarm.c), from
 * /config/alarms.txt, whatever is open -- an alarm that needed its app open
 * would be a timer. So the file is the truth, and it changes under this app:
 * the kernel switches a `once` off after it rings and adds and removes
 * snoozes. The list is read again before every change and the change applied
 * to the alarm it names (by time, kind and label, not by position), so a
 * save here never undoes one the kernel made. Saved through apps/safefile.h.
 * The line format and the rules are kernel/sys/alarmfmt.h, shared with the
 * kernel so the two cannot read a line differently.
 *
 * Commands: add "7:30 wake up" (once), list, next, off TIME.
 */

#include "kernel/app/capp.h"
#include "kernel/sys/alarmfmt.h"
#include "apps/toolbar.h"
#include "apps/safefile.h"

static const CardApi *api;

#define ROW_H   22
#define FOOT_H  11

#define CLR_BG      CAPP_RGB(12, 14, 20)
#define CLR_TEXT    CAPP_RGB(238, 241, 247)
#define CLR_DIM     CAPP_RGB(128, 136, 152)
#define CLR_FAINT   CAPP_RGB(62, 68, 84)
#define CLR_ACCENT  CAPP_RGB(255, 176, 76)
#define CLR_SEL     CAPP_RGB(28, 36, 54)
#define CLR_ON      CAPP_RGB(76, 196, 128)
#define CLR_OFF     CAPP_RGB(54, 60, 74)
#define CLR_FIELD   CAPP_RGB(40, 64, 100)
#define CLR_FOOT    CAPP_RGB(28, 32, 42)
#define CLR_WARN    CAPP_RGB(236, 104, 84)

enum { VIEW_FACE = 0, VIEW_LIST, VIEW_EDIT };
enum { F_HOUR = 0, F_MIN, F_DAY0, F_LABEL = F_DAY0 + 7 };

static struct {
  Alarm   list[ALARM_MAX];
  int     n;
  int     view;
  int     sel, top, rows;
  int     ask_delete;

  Alarm   edit;                 /* the one in the editor */
  Alarm   was;                  /* what it was when opened, to find it again */
  int     editing_new;
  int     field;
  int     typed;                /* digits typed into the hour or minute so far */

  int     hold;                 /* k: the screen held on */
  int     last_sec, last_min;
  int     f_big, f_mid, f_ui, f_uib;
  CRect   content;
} K;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

/* ---- the file ------------------------------------------------------------- */

static void load(void) {
  char buf[256], line[96];
  int fd, n, i, len = 0;
  K.n = 0;
  fd = safe_open_read(api, ALARM_FILE);
  if (fd < 0) return;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      if (buf[i] != '\n') { if (len < (int)sizeof line - 1) line[len++] = buf[i]; continue; }
      line[len] = 0;
      len = 0;
      if (K.n < ALARM_MAX && alarm_parse(line, &K.list[K.n])) K.n++;
    }
  }
  if (len && K.n < ALARM_MAX) { line[len] = 0; if (alarm_parse(line, &K.list[K.n])) K.n++; }
  api->close(fd);
}

static int save(void) {
  SafeFile f;
  char line[96];
  int i;
  api->mkdir(CAPP_CONFIG);
  if (safe_begin(&f, api, ALARM_FILE) != 0) return -1;
  safe_line(&f, "# alarms: on|off HH:MM SMTWTFS|once|snooze label -- see kernel/sys/alarmfmt.h\n");
  for (i = 0; i < K.n; i++) {
    alarm_format(&K.list[i], line, sizeof line - 1);
    safe_line(&f, line);
    safe_line(&f, "\n");
  }
  return safe_commit(&f);
}

static int same_str(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static int same_alarm(const Alarm *a, const Alarm *b) {
  return a->hour == b->hour && a->min == b->min && a->kind == b->kind &&
         a->days == b->days && same_str(a->label, b->label);
}

/* The list as the file has it now, and where `a` is in it, or -1. */
static int reload_find(const Alarm *a) {
  int i;
  load();
  for (i = 0; i < K.n; i++) if (same_alarm(&K.list[i], a)) return i;
  return -1;
}

static void clamp_sel(void) {
  if (K.sel >= K.n) K.sel = K.n - 1;
  if (K.sel < 0) K.sel = 0;
  if (K.rows < 1) K.rows = 1;
  if (K.sel < K.top) K.top = K.sel;
  if (K.sel >= K.top + K.rows) K.top = K.sel - K.rows + 1;
  if (K.top < 0) K.top = 0;
}

/* ---- the time --------------------------------------------------------------- */

static const char *const WDAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday" };
static const char *const MON[12] = { "January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November",
                                     "December" };

/* "07:00 Work, in 9 h 12 min", or "no alarms set". */
static void next_text(const CappTime *t, char *out, size_t n) {
  int i, best = -1, bi = -1;
  for (i = 0; i < K.n; i++) {
    int m = alarm_minutes_until(&K.list[i], t->wday, t->hour, t->min);
    if (m >= 0 && (best < 0 || m < best)) { best = m; bi = i; }
  }
  if (bi < 0) { api->fmt(out, n, K.n ? "no alarms on" : "no alarms set"); return; }
  if (best >= 1440)
    api->fmt(out, n, "%02u:%02u %s, in %d d %d h", K.list[bi].hour, K.list[bi].min,
             K.list[bi].label, best / 1440, (best % 1440) / 60);
  else if (best >= 60)
    api->fmt(out, n, "%02u:%02u %s, in %d h %d min", K.list[bi].hour, K.list[bi].min,
             K.list[bi].label, best / 60, best % 60);
  else
    api->fmt(out, n, "%02u:%02u %s, in %d min", K.list[bi].hour, K.list[bi].min,
             K.list[bi].label, best);
}

/* ---- painting ------------------------------------------------------------------ */

static int font_y(int f, int y, int h) { return y + (h - api->font_height(f)) / 2; }

static void paint_foot(const char *s, uint16_t fg) {
  CRect f = rect(K.content.x, K.content.y + K.content.h - FOOT_H, K.content.w, FOOT_H);
  api->fill(f, CLR_FOOT);
  api->text((int16_t)(f.x + 4), (int16_t)(f.y + 2), s, fg, CLR_FOOT);
}

/* The hours and minutes, and the seconds after them, as one line centred:
 * the part that changes every second, so tick damages only this. */
static CRect time_rect(void) {
  return rect(K.content.x, K.content.y + 10, K.content.w, api->font_height(K.f_big));
}

static void paint_time(const CappTime *t) {
  CRect r = time_rect();
  char hm[8], ss[4];
  int wb, ws, x, sy;
  api->fmt(hm, sizeof hm, "%02u:%02u", t->hour, t->min);
  api->fmt(ss, sizeof ss, "%02u", t->sec);
  wb = api->text_width(K.f_big, hm);
  ws = api->text_width(K.f_mid, ss);
  x = r.x + (r.w - (wb + 6 + ws)) / 2;
  sy = r.y + r.h - api->font_height(K.f_mid);           /* on the baseline */
  api->fill(rect(r.x, r.y, x - r.x, r.h), CLR_BG);
  api->text_font(K.f_big, (int16_t)x, (int16_t)r.y, hm, CLR_TEXT, CLR_BG);
  api->fill(rect(x + wb, r.y, 6, r.h), CLR_BG);
  api->fill(rect(x + wb + 6, r.y, ws, sy - r.y), CLR_BG);
  api->text_font(K.f_mid, (int16_t)(x + wb + 6), (int16_t)sy, ss, CLR_ACCENT, CLR_BG);
  api->fill(rect(x + wb + 6 + ws, r.y, r.x + r.w - (x + wb + 6 + ws), r.h), CLR_BG);
}

static void paint_face(void) {
  CappTime t;
  CRect c = K.content;
  char s[64];
  int y, w;
  api->now(&t);
  api->fill(rect(c.x, c.y, c.w, 10), CLR_BG);
  if (!t.synced) {
    api->fill(rect(c.x, c.y + 10, c.w, c.h - 10 - FOOT_H), CLR_BG);
    api->text_font(K.f_uib, (int16_t)(c.x + 12), (int16_t)(c.y + 30), "The clock is not set",
                   CLR_TEXT, CLR_BG);
    api->text_font(K.f_ui, (int16_t)(c.x + 12), (int16_t)(c.y + 50),
                   "it sets itself on WiFi; alarms wait for it", CLR_DIM, CLR_BG);
  } else {
    paint_time(&t);
    y = time_rect().y + time_rect().h + 6;
    api->fill(rect(c.x, time_rect().y + time_rect().h, c.w, c.y + c.h - FOOT_H -
                   (time_rect().y + time_rect().h)), CLR_BG);
    api->fmt(s, sizeof s, "%s %u %s", WDAY[t.wday % 7], t.day, MON[(t.month + 11) % 12]);
    w = api->text_width(K.f_uib, s);
    api->text_font(K.f_uib, (int16_t)(c.x + (c.w - w) / 2), (int16_t)y, s, CLR_TEXT, CLR_BG);
    y += api->font_height(K.f_uib) + 6;
    /* Only when there is one: an empty line says nothing worth reading. */
    next_text(&t, s, sizeof s);
    if (s[0] != 'n') {
      w = api->text_width(K.f_ui, s);
      api->text_font(K.f_ui, (int16_t)(c.x + (c.w - w) / 2), (int16_t)y, s, CLR_ACCENT, CLR_BG);
    }
  }
  paint_foot(K.hold ? "a alarms   k lets the screen sleep" : "a alarms   k keeps the screen on",
             K.hold ? CLR_ACCENT : CLR_DIM);
}

static void paint_switch(int x, int y, int on, uint16_t bg) {
  api->fill(rect(x, y, 22, 12), on ? CLR_ON : CLR_OFF);
  api->fill(rect(x, y, 1, 1), bg); api->fill(rect(x + 21, y, 1, 1), bg);
  api->fill(rect(x, y + 11, 1, 1), bg); api->fill(rect(x + 21, y + 11, 1, 1), bg);
  api->fill(rect(on ? x + 12 : x + 2, y + 2, 8, 8), CLR_TEXT);
}

static void paint_list(void) {
  CRect c = K.content;
  int i, y;
  char t[8], d[32];
  K.rows = (c.h - FOOT_H) / ROW_H;
  clamp_sel();
  y = c.y;
  if (!K.n) {
    api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_BG);
    api->text_font(K.f_uib, (int16_t)(c.x + 12), (int16_t)(c.y + 12), "No alarms", CLR_TEXT, CLR_BG);
    api->text_font(K.f_ui, (int16_t)(c.x + 12), (int16_t)(c.y + 32), "n makes one", CLR_DIM, CLR_BG);
  } else {
    for (i = K.top; i < K.n && i < K.top + K.rows; i++, y += ROW_H) {
      const Alarm *a = &K.list[i];
      uint16_t bg = i == K.sel ? CLR_SEL : CLR_BG;
      int x;
      api->fill(rect(c.x, y, c.w, ROW_H), bg);
      api->fmt(t, sizeof t, "%02u:%02u", a->hour, a->min);
      api->text_font(K.f_uib, (int16_t)(c.x + 10), (int16_t)font_y(K.f_uib, y, ROW_H), t,
                     a->on ? CLR_TEXT : CLR_DIM, bg);
      alarm_days_text(a, d, sizeof d);
      x = c.x + 10 + api->text_width(K.f_uib, "00:00") + 10;
      api->text_font(K.f_ui, (int16_t)x, (int16_t)font_y(K.f_ui, y, ROW_H), d, CLR_DIM, bg);
      x += api->text_width(K.f_ui, d) + 8;
      if (a->label[0])
        api->text_font(K.f_ui, (int16_t)x, (int16_t)font_y(K.f_ui, y, ROW_H), a->label,
                       a->on ? CLR_ACCENT : CLR_FAINT, bg);
      paint_switch(c.x + c.w - 32, y + (ROW_H - 12) / 2, a->on, bg);
    }
    if (y < c.y + c.h - FOOT_H) api->fill(rect(c.x, y, c.w, c.y + c.h - FOOT_H - y), CLR_BG);
  }
  if (K.ask_delete) {
    char q[48];
    api->fmt(q, sizeof q, "delete %02u:%02u %s?  y/n", K.list[K.sel].hour, K.list[K.sel].min,
             K.list[K.sel].label);
    paint_foot(q, CLR_WARN);
  } else {
    paint_foot("spc on/off  enter edit  n new  d del", CLR_DIM);
  }
}

static void paint_edit(void) {
  static const char *const L[7] = { "S", "M", "T", "W", "T", "F", "S" };
  CRect c = K.content;
  char hh[4], mm[4], shown[ALARM_LABEL_MAX + 2];
  int hb = api->font_height(K.f_big), wd = api->text_width(K.f_big, "00");
  int wc = api->text_width(K.f_big, ":"), x0 = c.x + (c.w - (2 * wd + wc)) / 2;
  int y = c.y + 6, i, dx, bw = 26;

  api->fill(rect(c.x, c.y, c.w, c.h - FOOT_H), CLR_BG);
  api->fmt(hh, sizeof hh, "%02u", K.edit.hour);
  api->fmt(mm, sizeof mm, "%02u", K.edit.min);
  if (K.field == F_HOUR) api->fill(rect(x0 - 3, y - 2, wd + 6, hb + 4), CLR_FIELD);
  if (K.field == F_MIN) api->fill(rect(x0 + wd + wc - 3, y - 2, wd + 6, hb + 4), CLR_FIELD);
  api->text_font(K.f_big, (int16_t)x0, (int16_t)y, hh, CLR_TEXT,
                 K.field == F_HOUR ? CLR_FIELD : CLR_BG);
  api->text_font(K.f_big, (int16_t)(x0 + wd), (int16_t)y, ":", CLR_DIM, CLR_BG);
  api->text_font(K.f_big, (int16_t)(x0 + wd + wc), (int16_t)y, mm, CLR_TEXT,
                 K.field == F_MIN ? CLR_FIELD : CLR_BG);
  y += hb + 8;

  dx = c.x + (c.w - 7 * bw) / 2;
  for (i = 0; i < 7; i++) {
    int on = (K.edit.days >> i) & 1, sel = K.field == F_DAY0 + i;
    uint16_t bg = on ? CLR_ON : CLR_OFF;
    api->fill(rect(dx + i * bw + 1, y, bw - 3, 18), bg);
    if (sel) api->frame(rect(dx + i * bw, y - 1, bw - 1, 20), CLR_TEXT);
    api->text_font(K.f_uib, (int16_t)(dx + i * bw + (bw - 3 - api->text_width(K.f_uib, L[i])) / 2 + 1),
                   (int16_t)font_y(K.f_uib, y, 18), L[i], CLR_TEXT, bg);
  }
  y += 24;

  api->fill(rect(c.x + 8, y, c.w - 16, 20), K.field == F_LABEL ? CLR_FIELD : CLR_SEL);
  api->fmt(shown, sizeof shown, "%s%s", K.edit.label, K.field == F_LABEL ? "_" : "");
  api->text_font(K.f_ui, (int16_t)(c.x + 14), (int16_t)font_y(K.f_ui, y, 20),
                 shown[0] ? shown : "label", shown[0] ? CLR_TEXT : CLR_FAINT,
                 K.field == F_LABEL ? CLR_FIELD : CLR_SEL);
  paint_foot(K.edit.days ? "</> field  up/dn  spc day  enter saves"
                         : "no days: rings once   enter saves", CLR_DIM);
}

static void app_paint(void *st, CRect full) {
  (void)st;
  if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
  if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
  toolbar_paint_bar(full);
  K.content = toolbar_rest(full);
  if (K.view == VIEW_LIST) paint_list();
  else if (K.view == VIEW_EDIT) paint_edit();
  else paint_face();
  toolbar_paint_menu(full);
}

/* ---- changing alarms --------------------------------------------------------------- */

static void open_editor(int index) {
  CappTime t;
  if (index >= 0) {
    api->mem_cpy(&K.edit, &K.list[index], sizeof(Alarm));
    api->mem_cpy(&K.was, &K.list[index], sizeof(Alarm));
    K.editing_new = 0;
  } else {
    api->mem_set(&K.edit, 0, sizeof K.edit);
    api->now(&t);
    K.edit.on = 1;
    K.edit.hour = t.synced ? t.hour : 7;
    K.edit.kind = ALARM_ONCE;
    K.editing_new = 1;
  }
  K.field = F_HOUR;
  K.typed = 0;
  K.view = VIEW_EDIT;
}

/* Save what the editor holds: a new line, or the one it came from -- found
 * again in the file as it is now, in case the kernel changed it meanwhile. */
static void save_editor(void) {
  int i;
  K.edit.kind = K.edit.days ? ALARM_REPEAT : ALARM_ONCE;
  K.edit.on = 1;
  if (K.editing_new) {
    load();
    if (K.n >= ALARM_MAX) { K.view = VIEW_LIST; return; }
    api->mem_cpy(&K.list[K.n++], &K.edit, sizeof(Alarm));
    K.sel = K.n - 1;
  } else if ((i = reload_find(&K.was)) >= 0) {
    api->mem_cpy(&K.list[i], &K.edit, sizeof(Alarm));
    K.sel = i;
  } else if (K.n < ALARM_MAX) {
    api->mem_cpy(&K.list[K.n++], &K.edit, sizeof(Alarm));
    K.sel = K.n - 1;
  }
  save();
  K.view = VIEW_LIST;
}

static void toggle_selected(void) {
  Alarm a;
  int i;
  if (K.sel < 0 || K.sel >= K.n) return;
  api->mem_cpy(&a, &K.list[K.sel], sizeof a);
  if ((i = reload_find(&a)) < 0) return;
  K.list[i].on = !K.list[i].on;
  K.sel = i;
  save();
}

static void delete_selected(void) {
  Alarm a;
  int i, j;
  if (K.sel < 0 || K.sel >= K.n) return;
  api->mem_cpy(&a, &K.list[K.sel], sizeof a);
  if ((i = reload_find(&a)) < 0) return;
  for (j = i; j + 1 < K.n; j++) api->mem_cpy(&K.list[j], &K.list[j + 1], sizeof(Alarm));
  K.n--;
  save();
  clamp_sel();
}

/* ---- actions and commands ---------------------------------------------------------- */

enum { ACT_ALARMS = 1, ACT_FACE, ACT_NEW, ACT_HOLD, ACT_ADD, ACT_LIST, ACT_NEXT, ACT_OFF };

static const CappParam P_WHEN[] = {
  { "when", CAPP_ARG_TEXT, "a time and an optional label: 7:30 wake up, 6pm, 19:00 gym" } };
static const CappParam P_TIME[] = { { "time", CAPP_ARG_TEXT, "the alarm's time, like 7:30" } };

static const CappAction ACTIONS[] = {
  { "alarms",   "Alarms",           "Clock", 0, ACT_ALARMS },
  { "face",     "Clock",            "Clock", 0, ACT_FACE },
  { "new",      "New alarm",        "Clock", 0, ACT_NEW },
  { "hold",     "Keep screen on",   "Clock", 0, ACT_HOLD },
  { "add",      "Add",              0,       0, ACT_ADD,
    "set a one-time alarm; it rings even with Clock closed", P_WHEN, 1, CAPP_CMD_YES },
  { "list",     "List",             0,       0, ACT_LIST,
    "every alarm, on or off, and its days", 0, 0, CAPP_CMD_YES },
  { "next",     "Next",             0,       0, ACT_NEXT,
    "the next alarm to ring and how long until it", 0, 0, CAPP_CMD_YES },
  { "off",      "Off",              0,       0, ACT_OFF,
    "switch off the alarm set for a time", P_TIME, 1, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int do_action(int a) {
  switch (a) {
  case ACT_ALARMS: load(); K.view = VIEW_LIST; K.ask_delete = 0; return 1;
  case ACT_FACE:   K.view = VIEW_FACE; return 1;
  case ACT_NEW:    open_editor(-1); return 1;
  case ACT_HOLD:
    K.hold = !K.hold;
    api->keep_awake(K.hold);
    return 1;
  default:         return 0;
  }
}

static int app_action(void *st, int a) { (void)st; return do_action(a); }

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  CappTime t;
  char word[16], d[32];
  const char *p;
  int i, h, m, k, hits = 0;
  size_t o = 0;
  (void)st; (void)argc;
  load();
  api->now(&t);
  switch (action) {
  case ACT_ADD: {
    Alarm a;
    p = argv[0];
    while (*p == ' ') p++;
    for (k = 0; p[k] && p[k] != ' ' && k < (int)sizeof word - 1; k++) word[k] = p[k];
    word[k] = 0;
    p += k;
    /* "7 30 pm" and "7:30 pm" are how people say it: an am/pm word after the
     * time belongs to it. */
    while (*p == ' ') p++;
    if ((p[0] == 'a' || p[0] == 'p' || p[0] == 'A' || p[0] == 'P') &&
        (p[1] == 'm' || p[1] == 'M') && (p[2] == 0 || p[2] == ' ') &&
        k + 2 < (int)sizeof word) {
      word[k++] = p[0]; word[k++] = p[1]; word[k] = 0;
      p += 2;
    }
    if (!alarm_parse_time(word, &h, &m)) {
      api->fmt(out, n, "not a time: \"%s\" (try 7:30, 6pm, 19:00)", word);
      return -1;
    }
    if (K.n >= ALARM_MAX) { api->fmt(out, n, "already %d alarms", ALARM_MAX); return -1; }
    api->mem_set(&a, 0, sizeof a);
    a.on = 1; a.hour = (uint8_t)h; a.min = (uint8_t)m; a.kind = ALARM_ONCE;
    while (*p == ' ') p++;
    for (k = 0; p[k] && k < ALARM_LABEL_MAX - 1; k++) a.label[k] = p[k];
    a.label[k] = 0;
    api->mem_cpy(&K.list[K.n++], &a, sizeof a);
    if (save() != 0) { api->fmt(out, n, "could not write to the card"); return -1; }
    if (t.synced) {
      int mins = alarm_minutes_until(&a, t.wday, t.hour, t.min);
      api->fmt(out, n, "alarm set for %02d:%02d%s%s, in %d h %d min", h, m,
               a.label[0] ? " " : "", a.label, mins / 60, mins % 60);
    } else {
      api->fmt(out, n, "alarm set for %02d:%02d (the clock is not set yet)", h, m);
    }
    return 0;
  }
  case ACT_LIST:
    if (!K.n) { api->fmt(out, n, "no alarms"); return 0; }
    for (i = 0; i < K.n && o + 16 < n; i++) {
      alarm_days_text(&K.list[i], d, sizeof d);
      o += (size_t)api->fmt(out + o, n - o, "%s%02u:%02u %s %s%s%s", i ? "\n" : "",
                            K.list[i].hour, K.list[i].min, d, K.list[i].on ? "on" : "off",
                            K.list[i].label[0] ? " " : "", K.list[i].label);
    }
    return 0;
  case ACT_NEXT:
    if (!t.synced) { api->fmt(out, n, "the clock is not set"); return -1; }
    next_text(&t, out, n);
    return 0;
  case ACT_OFF:
    if (!alarm_parse_time(argv[0], &h, &m)) { api->fmt(out, n, "not a time: %s", argv[0]); return -1; }
    for (i = 0; i < K.n; i++)
      if (K.list[i].hour == h && K.list[i].min == m && K.list[i].on) { K.list[i].on = 0; hits++; }
    if (!hits) { api->fmt(out, n, "no alarm on at %02d:%02d", h, m); return -1; }
    save();
    api->fmt(out, n, "%d alarm%s at %02d:%02d switched off", hits, hits == 1 ? "" : "s", h, m);
    return 0;
  }
  api->fmt(out, n, "clock has no command %d", action);
  return -1;
}

/* ---- keys ---------------------------------------------------------------------------- */

static int key_face(uint8_t k) {
  switch (k) {
  case 'a': case 'A': case CAPP_KEY_ENTER: return do_action(ACT_ALARMS);
  case 'n': case 'N': return do_action(ACT_NEW);
  case 'k': case 'K': return do_action(ACT_HOLD);
  default: return 0;
  }
}

static int key_list(uint8_t k) {
  if (K.ask_delete) {
    if (k == 'y' || k == 'Y' || k == CAPP_KEY_ENTER) delete_selected();
    else if (!(k == 'n' || k == 'N' || k == CAPP_KEY_ESC || k == CAPP_KEY_BACK)) return 1;
    K.ask_delete = 0;
    return 1;
  }
  switch (k) {
  case CAPP_KEY_UP:   K.sel--; clamp_sel(); return 1;
  case CAPP_KEY_DOWN: K.sel++; clamp_sel(); return 1;
  case ' ':           toggle_selected(); return 1;
  case CAPP_KEY_ENTER: if (K.n) open_editor(K.sel); return 1;
  case 'n': case 'N': return do_action(ACT_NEW);
  case 'd': case 'D': case 0x7F: if (K.n) K.ask_delete = 1; return 1;
  case CAPP_KEY_ESC: case CAPP_KEY_BACK: K.view = VIEW_FACE; return 1;
  default: return 0;
  }
}

static int key_edit(uint8_t k) {
  if (k == CAPP_KEY_ESC) { K.view = VIEW_LIST; return 1; }
  if (k == CAPP_KEY_ENTER) { save_editor(); return 1; }
  if (k == CAPP_KEY_LEFT || k == CAPP_KEY_RIGHT) {
    K.field += k == CAPP_KEY_RIGHT ? 1 : -1;
    if (K.field < F_HOUR) K.field = F_LABEL;
    if (K.field > F_LABEL) K.field = F_HOUR;
    K.typed = 0;
    return 1;
  }
  if (K.field == F_LABEL) {
    int len = (int)api->str_len(K.edit.label);
    if (k == CAPP_KEY_BACK) { if (len) K.edit.label[len - 1] = 0; return 1; }
    if (k >= 32 && k < 127 && len < ALARM_LABEL_MAX - 1) {
      K.edit.label[len] = (char)k;
      K.edit.label[len + 1] = 0;
    }
    return 1;
  }
  if (K.field >= F_DAY0) {
    if (k == ' ' || k == CAPP_KEY_UP || k == CAPP_KEY_DOWN)
      K.edit.days ^= (uint8_t)(1u << (K.field - F_DAY0));
    return 1;
  }
  {
    int max = K.field == F_HOUR ? 23 : 59;
    int v = K.field == F_HOUR ? K.edit.hour : K.edit.min;
    if (k == CAPP_KEY_UP) v = (v + 1) % (max + 1);
    else if (k == CAPP_KEY_DOWN) v = (v + max) % (max + 1);
    else if (k >= '0' && k <= '9') {
      /* Two digits and on to the next field, as a clock's buttons do. */
      v = K.typed ? (v % 10) * 10 + (k - '0') : (k - '0');
      if (v > max) v = k - '0';
      K.typed++;
    } else {
      return 1;
    }
    if (K.field == F_HOUR) K.edit.hour = (uint8_t)v;
    else K.edit.min = (uint8_t)v;
    if (K.typed == 2) { K.field++; K.typed = 0; }
    return 1;
  }
}

static int app_key(void *st, uint8_t k) {
  int a = toolbar_key(k);
  (void)st;
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  if (K.view == VIEW_EDIT) return key_edit(k);
  if (api->key_repeat() && k != CAPP_KEY_UP && k != CAPP_KEY_DOWN) return 1;
  if (K.view == VIEW_LIST) return key_list(k);
  return key_face(k);
}

static int app_click(void *st, int16_t x, int16_t y, int button) {
  int a, row;
  (void)st; (void)button;
  a = toolbar_click(x, y);
  if (a == TB_CONSUMED) return 1;
  if (a != TB_NONE) return do_action(a);
  if (K.view != VIEW_LIST) return 0;
  y = (int16_t)(y - toolbar_h());
  row = K.top + y / ROW_H;
  if (y < 0 || row >= K.n) return 0;
  K.sel = row;
  if (x > K.content.w - 40) toggle_selected();        /* on the switch */
  return 1;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int ch;
  (void)st; (void)buttons; (void)wheel;
  ch = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) ch = 1;
  return ch;
}

static int app_wants_text(void *st) {
  (void)st;
  return !toolbar_has_keys() && K.view == VIEW_EDIT && K.field == F_LABEL;
}

/* The face moves every second; the list is re-read every minute, which is
 * when the kernel may have rung and changed it. */
static int app_tick(void *st, uint32_t now) {
  CappTime t;
  (void)st; (void)now;
  api->now(&t);
  if (t.min != K.last_min) {
    K.last_min = t.min;
    if (K.view != VIEW_EDIT) { load(); return 1; }
  }
  if (t.sec != K.last_sec) {
    K.last_sec = t.sec;
    if (K.view == VIEW_FACE && t.synced) { api->damage(time_rect()); return 1; }
  }
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Clock",
  /* 16x16: an alarm clock -- a round face, two bells, two feet. */
  { 0x30, 0x0C, 0x67, 0xE6, 0x48, 0x12, 0x10, 0x08,
    0x21, 0x04, 0x21, 0x04, 0x41, 0x02, 0x41, 0x82,
    0x40, 0x42, 0x40, 0x02, 0x20, 0x04, 0x20, 0x04,
    0x10, 0x08, 0x0F, 0xF0, 0x18, 0x18, 0x00, 0x00 },
  "a / enter\talarms\nk\tkeep the screen on\nn\tnew alarm\n"
  "space\t(alarms) on / off\nenter\t(alarms) edit\nd\t(alarms) delete\n"
  "left/right\t(editor) field\nup/down\t(editor) change it\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&K, 0, sizeof K);
  K.f_big = K.f_mid = K.f_ui = K.f_uib = -1;
  K.last_sec = K.last_min = -1;
  load();
  if (!api->headless()) {
    K.f_big = api->font_load("clock56");
    K.f_mid = api->font_load("num30");
    K.f_ui  = api->font_load("ui13");
    K.f_uib = api->font_load("ui13b");
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
