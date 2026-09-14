/* Calendar -- Google Calendar on the Cardputer.
 *
 * Offline first, like Todo and for the same reason: a calendar you carry is
 * useless if it will not open a diary without a radio. Events are cached to
 * /calendar.cache and drawn before the network is asked about anything. An
 * event added with no signal is queued with no id and pushed on the next
 * sync.
 *
 * Two views over the same list. The agenda is the default because it answers
 * "what is next", which is the question a small screen is actually good for;
 * the month grid is a date picker, because at 240x135 a month cell is about
 * thirty pixels wide and can hold a number and a dot and nothing else.
 *
 * THE MEMORY PROBLEM, AND THE ONE TRICK THAT SOLVES IT. A Google event is
 * about 700 bytes of JSON -- attendees, reminders, conferencing, html links,
 * two timezone names -- so forty of them is 28 KB of reply for a device with
 * a 6 KB buffer. Google supports partial responses, so this asks for
 * `fields=items(id,summary,start,end)` and gets about 120 bytes an event.
 * That is the difference between this app existing and not. `singleEvents`
 * makes Google expand recurring events server-side, so there is no recurrence
 * engine here either -- another few kilobytes that stayed on the server.
 *
 * The JSON is scanned for field names rather than parsed, as in Todo: a real
 * parser costs more than the four strings per event it would extract.
 *
 * The sync is automatic and does not block. It runs on the OS's request task
 * (CardApi.http_start) and is collected from tick, so the screen keeps
 * drawing and the keyboard keeps working while it is in the air -- the shell
 * shows a spinner of its own, so there is none here. It runs when the app
 * opens, because that is why anybody opens a calendar, and every ten minutes
 * after that.
 *
 * Dates are done here, in integer arithmetic, because an app links against no
 * libc. api->now() gives local time and api->epoch() gives UTC, and the
 * difference between them is the zone offset -- which is how local times get
 * displayed without this app knowing anything about zones. That offset is
 * sampled now, so an event on the far side of a DST change can be shown an
 * hour out. Storing the offset per event would fix it and cost more than the
 * error is worth on a device whose clock is lost at every power cut.
 */

#include "kernel/app/capp.h"
#include "apps/toolbar.h"

#define MAX_EVENTS  40
#define SUMMARY_MAX 34
#define ID_MAX      56
#define REPLY_MAX   6000
#define URL_MAX     320
#define CACHE_PATH  "/calendar.cache"

#define ROW_H       11
#define SCREEN_W    240
#define SCREEN_H    135
#define BAR_H       10

#define DAY_SECS    86400u
#define WINDOW_DAYS 60          /* how far ahead a sync looks */

#define EVENTS_URL "https://www.googleapis.com/calendar/v3/calendars/primary/events"

#define CLR_BG      CAPP_RGB(20, 22, 28)
#define CLR_ROW     CAPP_RGB(28, 31, 38)
#define CLR_SEL     CAPP_RGB(48, 74, 110)
#define CLR_TEXT    CAPP_RGB(222, 228, 238)
#define CLR_DIM     CAPP_RGB(126, 136, 152)
#define CLR_HEAD    CAPP_RGB(150, 170, 255)
#define CLR_BAR     CAPP_RGB(38, 62, 98)
#define CLR_BARFG   CAPP_RGB(232, 238, 248)
#define CLR_TODAY   CAPP_RGB(112, 208, 140)
#define CLR_WARN    CAPP_RGB(240, 176, 80)
#define CLR_PEND    CAPP_RGB(150, 170, 255)
#define CLR_GRID    CAPP_RGB(44, 48, 58)

typedef enum { VIEW_AGENDA = 0, VIEW_MONTH, VIEW_ADD } View;
typedef enum { FIELD_TITLE = 0, FIELD_DATE, FIELD_TIME, FIELD_COUNT } Field;

typedef struct {
  char     id[ID_MAX];              /* empty means it has never reached Google */
  char     summary[SUMMARY_MAX + 1];
  uint32_t start;                   /* UTC seconds since the epoch */
  uint32_t end;
  uint8_t  all_day;
  uint8_t  dirty;                   /* made here, not yet pushed */
  uint8_t  deleted;                 /* gone here, not yet gone at Google */
} Event;

static const CardApi *api;

static struct {
  View  view;
  Event ev[MAX_EVENTS];
  int   n;
  int   sel;                        /* index into the agenda */
  int   top;                        /* first agenda line drawn */
  int   shown_rows;
  int   shown_top;

  int32_t offset;                   /* local time minus UTC, in seconds */
  int   cur_y, cur_m, cur_d;        /* the day the month grid is sitting on */
  int   today_y, today_m, today_d;
  int   have_clock;

  Field field;
  char  draft[SUMMARY_MAX + 1];
  int   draft_len;
  int   draft_hour, draft_min;
  int32_t draft_day;                /* days since the epoch, local */

  char  status[64];
  char  reply[REPLY_MAX];

  /* The sync, which no longer blocks. See CardApi.http_start: the request
   * runs on a task of its own and this polls it from tick, so the screen
   * keeps drawing and the keyboard keeps working while it is in the air. The
   * shell draws the spinner; this app draws none. */
  int      stage;                   /* SYNC_* below */
  int      pushing;                 /* index of the event being pushed */
  uint32_t next_auto;               /* when to sync again, unprompted */
  int      tried_once;
} C;

enum { SYNC_IDLE = 0, SYNC_PUSH, SYNC_FETCH };

/* Ten minutes. Often enough that a diary edited on a laptop turns up without
 * being asked for, rare enough that it is not what drains the battery. */
#define AUTO_EVERY_MS (10u * 60u * 1000u)

/* When a sync could not even be attempted -- no radio yet, no token yet --
 * try again soon rather than in ten minutes. Opening the app during the few
 * seconds it takes WiFi to come up is the common case, not the rare one. */
#define RETRY_MS      (15u * 1000u)

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *s) { api->fmt(C.status, sizeof C.status, "%s", s); }

static const char *WDAY[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---- the calendar itself -------------------------------------------------
 *
 * Howard Hinnant's civil-date algorithms, which are the short exact way to do
 * this in integers. Day 0 is 1970-01-01. Everything else here -- the grid, the
 * agenda's day headers, the RFC 3339 in and out -- is these two functions and
 * some formatting. They are also the part most likely to be wrong, which is
 * why test/test_calendar.c hammers them across leap years and century
 * boundaries rather than trusting the transcription. */

static int is_leap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static int days_in_month(int y, int m) {
  static const int LEN[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (m == 2 && is_leap(y)) return 29;
  if (m < 1 || m > 12) return 30;
  return LEN[m];
}

static int32_t days_from_civil(int y, int m, int d) {
  int era;
  unsigned yoe, doy, doe;
  y -= (m <= 2);
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int32_t)era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int *yy, int *mm, int *dd) {
  int era, y;
  unsigned doe, yoe, doy, mp, d, m;
  z += 719468;
  era = (int)((z >= 0 ? z : z - 146096) / 146097);
  doe = (unsigned)(z - (int32_t)era * 146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : (unsigned)-9);
  *yy = y + (m <= 2);
  *mm = (int)m;
  *dd = (int)d;
}

/* 0 = Sunday. Day 0 was a Thursday, hence the 4. */
static int weekday_of_day(int32_t z) {
  int32_t w = (z + 4) % 7;
  return (int)(w < 0 ? w + 7 : w);
}

/* ---- RFC 3339 ------------------------------------------------------------
 *
 * Only the two shapes Google emits: "2026-09-13" for an all-day event and
 * "2026-09-13T09:30:00+01:00" (or "...Z") for a timed one. */

static int digits(const char *p, int n) {
  int v = 0, i;
  for (i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') return -1;
    v = v * 10 + (p[i] - '0');
  }
  return v;
}

static int rfc3339_parse(const char *s, uint32_t *out, int *all_day) {
  int y, m, d, hh = 0, mi = 0, ss = 0;
  int32_t off = 0, secs;

  if (!s || !out) return -1;
  y = digits(s, 4);
  m = digits(s + 5, 2);
  d = digits(s + 8, 2);
  if (y < 0 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
  if (s[4] != '-' || s[7] != '-') return -1;

  if (s[10] != 'T') {
    /* A bare date: an all-day event, which Google states with no zone at all.
     * Treated as midnight UTC so it sorts and groups with everything else. */
    if (all_day) *all_day = 1;
    secs = days_from_civil(y, m, d) * (int32_t)DAY_SECS;
    *out = (uint32_t)secs;
    return 0;
  }

  if (all_day) *all_day = 0;
  hh = digits(s + 11, 2);
  mi = digits(s + 14, 2);
  ss = digits(s + 17, 2);
  if (hh < 0 || mi < 0 || ss < 0) return -1;

  if (s[19] == '+' || s[19] == '-') {
    int oh = digits(s + 20, 2), om = digits(s + 23, 2);
    if (oh < 0 || om < 0) return -1;
    off = (int32_t)oh * 3600 + (int32_t)om * 60;
    if (s[19] == '-') off = -off;
  }

  secs = days_from_civil(y, m, d) * (int32_t)DAY_SECS
       + (int32_t)hh * 3600 + mi * 60 + ss - off;
  if (secs < 0) return -1;
  *out = (uint32_t)secs;
  return 0;
}

/* UTC, which is the only form worth sending: it needs no zone database at
 * either end. */
static void rfc3339_utc(uint32_t t, char *out, int n) {
  int y, m, d;
  uint32_t rem = t % DAY_SECS;
  civil_from_days((int32_t)(t / DAY_SECS), &y, &m, &d);
  api->fmt(out, (size_t)n, "%04d-%02d-%02dT%02d:%02d:%02dZ",
           y, m, d, (int)(rem / 3600u), (int)(rem / 60u % 60u), (int)(rem % 60u));
}

/* ---- local time ---------------------------------------------------------- */

/* Local minus UTC. api->now is local and api->epoch is not, so subtracting
 * one from the other is the zone offset without a zone database. */
static void refresh_clock(void) {
  CappTime t;
  uint32_t utc = api->epoch();
  int32_t local;

  api->now(&t);
  C.have_clock = t.synced && utc;
  if (!C.have_clock) { C.offset = 0; return; }

  local = days_from_civil(t.year, t.month, t.day) * (int32_t)DAY_SECS
        + (int32_t)t.hour * 3600 + t.min * 60 + t.sec;
  C.offset = local - (int32_t)utc;
  C.today_y = t.year;
  C.today_m = t.month;
  C.today_d = t.day;
}

/* The local day an instant falls in, as days since the epoch. */
static int32_t local_day(uint32_t utc) {
  int32_t l = (int32_t)utc + C.offset;
  return l >= 0 ? l / (int32_t)DAY_SECS : -(((-l) + (int32_t)DAY_SECS - 1) / (int32_t)DAY_SECS);
}

static void local_hm(uint32_t utc, int *h, int *m) {
  int32_t l = (int32_t)utc + C.offset;
  int32_t rem = l - local_day(utc) * (int32_t)DAY_SECS;
  *h = (int)(rem / 3600);
  *m = (int)(rem / 60 % 60);
}

static int32_t today_day(void) {
  if (!C.have_clock) return 0;
  return days_from_civil(C.today_y, C.today_m, C.today_d);
}

/* ---- JSON ---------------------------------------------------------------- */

static char *find_pat(char *from, const char *pat) {
  int plen = (int)api->str_len(pat), j;
  char *q = from;
  if (!from) return 0;
  while (*q) {
    for (j = 0; j < plen && q[j] == pat[j]; j++) { }
    if (j == plen) return q;
    q++;
  }
  return 0;
}

/* The string value of `name`, if it is the next thing that looks like one. */
static int json_str(char *from, const char *name, char *out, int n) {
  char pat[24];
  char *p;
  int i = 0;

  out[0] = 0;
  api->fmt(pat, sizeof pat, "\"%s\"", name);
  p = find_pat(from, pat);
  if (!p) return 0;
  p += api->str_len(pat);
  while (*p == ' ' || *p == ':') p++;
  if (*p != '"') return 0;
  p++;
  while (*p && *p != '"' && i + 1 < n) {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'n' || *p == 't') { out[i++] = ' '; p++; continue; }
      if (*p == 'u') { out[i++] = '?'; p += 5; continue; }
    }
    out[i++] = *p++;
  }
  out[i] = 0;
  return i > 0;
}

/* start and end are objects holding either "dateTime" or "date". Whichever
 * appears first in the slice is the one meant -- the quotes in the pattern
 * keep "date" from matching the front of "dateTime". */
static int json_when(char *from, uint32_t *out, int *all_day) {
  char buf[40];
  char *dt = find_pat(from, "\"dateTime\"");
  char *da = find_pat(from, "\"date\"");
  char *use;

  if (!dt && !da) return 0;
  if (!dt) use = da;
  else if (!da) use = dt;
  else use = (dt < da) ? dt : da;

  if (!json_str(use, (use == dt) ? "dateTime" : "date", buf, sizeof buf)) return 0;
  return rfc3339_parse(buf, out, all_day) == 0;
}

/* ---- the list ------------------------------------------------------------ */

static void sort_events(void) {
  int i, j;
  /* Insertion sort: forty items that arrive very nearly sorted already. */
  for (i = 1; i < C.n; i++) {
    Event tmp;
    api->mem_cpy(&tmp, &C.ev[i], sizeof tmp);
    for (j = i; j > 0 && C.ev[j - 1].start > tmp.start; j--)
      api->mem_cpy(&C.ev[j], &C.ev[j - 1], sizeof tmp);
    api->mem_cpy(&C.ev[j], &tmp, sizeof tmp);
  }
}

static int day_has_event(int32_t day) {
  int i;
  for (i = 0; i < C.n; i++) {
    if (C.ev[i].deleted) continue;
    if (local_day(C.ev[i].start) == day) return 1;
  }
  return 0;
}

/* ---- the cache -----------------------------------------------------------
 *
 * One line an event: flags, the two instants, the id, then the summary last
 * because it is the only field that can hold a space. */
static void cache_save(void) {
  char line[ID_MAX + SUMMARY_MAX + 48];
  int fd, i;

  fd = api->open(CACHE_PATH, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  for (i = 0; i < C.n; i++) {
    int n = api->fmt(line, sizeof line, "%d %d %d %lu %lu %s %s\n",
                     C.ev[i].all_day, C.ev[i].dirty, C.ev[i].deleted,
                     (unsigned long)C.ev[i].start, (unsigned long)C.ev[i].end,
                     C.ev[i].id[0] ? C.ev[i].id : "-", C.ev[i].summary);
    api->write(fd, line, (size_t)n);
  }
  api->close(fd);
}

static unsigned long scan_ul(const char *s, int *pos) {
  unsigned long v = 0;
  int p = *pos;
  while (s[p] == ' ') p++;
  while (s[p] >= '0' && s[p] <= '9') v = v * 10ul + (unsigned long)(s[p++] - '0');
  *pos = p;
  return v;
}

static void cache_load(void) {
  char buf[512], line[ID_MAX + SUMMARY_MAX + 48];
  int fd, n, i, len = 0;

  C.n = 0;
  fd = api->open(CACHE_PATH, CAPP_O_READ);
  if (fd < 0) return;

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n && C.n < MAX_EVENTS; i++) {
      char c = buf[i];
      Event *e;
      int p = 0, q = 0;
      if (c != '\n') {
        if (len < (int)sizeof line - 1) line[len++] = c;
        continue;
      }
      line[len] = 0;
      len = 0;
      if (line[0] < '0' || line[0] > '1') continue;

      e = &C.ev[C.n];
      api->mem_set(e, 0, sizeof *e);
      e->all_day = (uint8_t)(line[0] - '0');
      e->dirty   = (uint8_t)(line[2] == '1');
      e->deleted = (uint8_t)(line[4] == '1');
      p = 5;
      e->start = (uint32_t)scan_ul(line, &p);
      e->end   = (uint32_t)scan_ul(line, &p);
      while (line[p] == ' ') p++;
      while (line[p] && line[p] != ' ' && q < ID_MAX - 1) e->id[q++] = line[p++];
      e->id[q] = 0;
      if (e->id[0] == '-' && !e->id[1]) e->id[0] = 0;
      while (line[p] == ' ') p++;
      api->fmt(e->summary, sizeof e->summary, "%s", line + p);
      if (e->summary[0]) C.n++;
    }
  }
  api->close(fd);
  sort_events();
}

/* ---- Google -------------------------------------------------------------- */

/* ---- syncing, without stopping the world ---------------------------------
 *
 * Three states. Anything made here that Google has not seen is pushed first,
 * one request at a time, then everything is fetched back -- so a queued event
 * returns with an id and the queue empties itself. Each step starts a request
 * and returns; sync_tick collects it on a later pass.
 */

/* The next unpushed event, or -1. */
static int next_dirty(void) {
  int i;
  for (i = 0; i < C.n; i++)
    if (C.ev[i].dirty && !C.ev[i].id[0] && !C.ev[i].deleted) return i;
  return -1;
}

static int start_push(const char *tok, int i) {
  char body[SUMMARY_MAX + 160], s[24], e[24];
  rfc3339_utc(C.ev[i].start, s, sizeof s);
  rfc3339_utc(C.ev[i].end, e, sizeof e);
  api->fmt(body, sizeof body,
           "{\"summary\":\"%s\",\"start\":{\"dateTime\":\"%s\"},"
           "\"end\":{\"dateTime\":\"%s\"}}",
           C.ev[i].summary, s, e);
  return api->http_start("POST", EVENTS_URL, body, "application/json", tok,
                         15000);
}

static int start_fetch(const char *tok) {
  char url[URL_MAX], tmin[24], tmax[24];
  int32_t base = C.have_clock ? today_day() : 0;

  rfc3339_utc((uint32_t)(base * (int32_t)DAY_SECS), tmin, sizeof tmin);
  rfc3339_utc((uint32_t)((base + WINDOW_DAYS) * (int32_t)DAY_SECS), tmax,
              sizeof tmax);

  /* fields= is the whole reason this fits in memory. See the file header. */
  api->fmt(url, sizeof url,
           "%s?singleEvents=true&orderBy=startTime&maxResults=%d"
           "&timeMin=%s&timeMax=%s&fields=items(id,summary,start,end)",
           EVENTS_URL, MAX_EVENTS, tmin, tmax);
  return api->http_start("GET", url, 0, 0, tok, 20000);
}

/* Rebuild the list from a reply. Anything still queued survives, so a
 * deletion made in a browser disappears here too. */
static void absorb(void) {
  char *p;
  int got = 0;
  int i, keep = 0;

  for (i = 0; i < C.n; i++) {
    if (!C.ev[i].dirty || C.ev[i].id[0]) continue;
    if (keep != i) api->mem_cpy(&C.ev[keep], &C.ev[i], sizeof C.ev[0]);
    keep++;
  }
  C.n = keep;

  p = find_pat(C.reply, "\"id\"");
  while (p && C.n < MAX_EVENTS) {
    char *next = find_pat(p + 4, "\"id\"");
    char saved = 0;
    Event *e = &C.ev[C.n];
    char *sp, *ep;

    if (next) { saved = *next; *next = 0; }

    api->mem_set(e, 0, sizeof *e);
    json_str(p, "id", e->id, ID_MAX);
    if (!json_str(p, "summary", e->summary, SUMMARY_MAX + 1))
      api->fmt(e->summary, sizeof e->summary, "%s", "(no title)");

    sp = find_pat(p, "\"start\"");
    ep = find_pat(p, "\"end\"");
    if (sp) {
      char keep2 = 0;
      if (ep && ep > sp) { keep2 = *ep; *ep = 0; }
      if (json_when(sp, &e->start, &got)) e->all_day = (uint8_t)got;
      if (ep && ep > sp) *ep = keep2;
    }
    if (ep) json_when(ep, &e->end, &got);
    if (!e->end) e->end = e->start + 3600u;

    if (e->id[0] && e->start) C.n++;
    if (next) *next = saved;
    p = next;
  }

  sort_events();
  cache_save();
  if (C.sel >= C.n) C.sel = C.n ? C.n - 1 : 0;
}

static void sync_failed(int n) {
  /* The code matters here more than usual: -403 with a good token means the
   * Calendar API is not enabled on the project, which is a different switch
   * from the OAuth scope and a different thing to go and fix. */
  if (n == -403)      say("403: enable the Calendar API");
  else if (n == -401) say("401: sign in again on the PC");
  else                api->fmt(C.status, sizeof C.status, "sync failed (%d)", n);
  C.stage = SYNC_IDLE;
}

/* Kick one off. Returns without waiting for anything. */
static void sync_begin(void) {
  const char *tok;
  int i;

  if (C.stage != SYNC_IDLE) return;
  C.next_auto = api->ticks_ms() + AUTO_EVERY_MS;
  C.tried_once = 1;

  /* A sync that could not start is not a sync that succeeded. Both paths
   * below leave the next attempt ten minutes away otherwise, so opening the
   * app one second before the radio came up meant it said "offline" and then
   * sat there saying it for ten minutes with WiFi plainly connected. */

  if (!api->net_ready()) {
    /* Not worth a blocking twenty-second join from here; the OS brings the
     * radio up for its own clock sync and this will catch the next round. */
    say("offline -- showing the cache");
    C.next_auto = api->ticks_ms() + RETRY_MS;
    return;
  }
  tok = api->google_token();
  if (!tok || !tok[0]) { say(api->google_status()); C.next_auto = api->ticks_ms() + RETRY_MS; return; }

  i = next_dirty();
  if (i >= 0) {
    if (start_push(tok, i) != 0) return;
    C.pushing = i;
    C.stage = SYNC_PUSH;
    say("sending...");
  } else {
    if (start_fetch(tok) != 0) return;
    C.stage = SYNC_FETCH;
    say("syncing...");
  }
}

/* Called every pass. Does nothing until there is something to collect. */
static void sync_tick(void) {
  int n;

  if (C.stage == SYNC_IDLE) return;
  n = api->http_poll(C.reply, sizeof C.reply);
  if (n == CAPP_HTTP_PENDING) return;

  if (n < 0) { sync_failed(n); return; }

  if (C.stage == SYNC_PUSH) {
    const char *tok = api->google_token();
    int i;
    /* It exists at Google now, so it is no longer queued. The id comes back
     * on the next fetch rather than being read out of the reply -- one place
     * that parses an event is better than two. */
    if (C.pushing >= 0 && C.pushing < C.n) C.ev[C.pushing].dirty = 0;
    cache_save();
    i = next_dirty();
    if (i >= 0 && tok && start_push(tok, i) == 0) { C.pushing = i; return; }
    if (tok && start_fetch(tok) == 0) { C.stage = SYNC_FETCH; return; }
    C.stage = SYNC_IDLE;
    return;
  }

  absorb();
  C.stage = SYNC_IDLE;
  api->fmt(C.status, sizeof C.status, "%d event%s", C.n, C.n == 1 ? "" : "s");
}

/* ---- adding --------------------------------------------------------------- */

static void begin_add(void) {
  C.draft[0] = 0;
  C.draft_len = 0;
  C.field = FIELD_TITLE;
  C.draft_day = C.have_clock ? days_from_civil(C.cur_y, C.cur_m, C.cur_d) : 0;
  C.draft_hour = 9;
  C.draft_min = 0;
  C.view = VIEW_ADD;
}

static void commit_add(void) {
  Event *e;
  int32_t local;

  if (!C.draft_len) { C.view = VIEW_AGENDA; return; }
  if (C.n >= MAX_EVENTS) { say("the list is full"); C.view = VIEW_AGENDA; return; }

  e = &C.ev[C.n];
  api->mem_set(e, 0, sizeof *e);
  api->fmt(e->summary, sizeof e->summary, "%s", C.draft);
  local = C.draft_day * (int32_t)DAY_SECS + (int32_t)C.draft_hour * 3600
        + C.draft_min * 60;
  e->start = (uint32_t)(local - C.offset);
  e->end = e->start + 3600u;
  e->dirty = 1;
  C.n++;

  sort_events();
  cache_save();
  C.view = VIEW_AGENDA;
  say("added -- press s to sync");
}

static void delete_selected(void) {
  if (C.sel < 0 || C.sel >= C.n) return;
  /* Only what has never been pushed can be dropped outright. Anything Google
   * knows about would come straight back on the next fetch, so rather than
   * pretend, this says what it can and cannot do. */
  if (C.ev[C.sel].id[0]) { say("delete it in Google Calendar"); return; }
  {
    int i;
    for (i = C.sel; i + 1 < C.n; i++)
      api->mem_cpy(&C.ev[i], &C.ev[i + 1], sizeof C.ev[0]);
    C.n--;
    if (C.sel >= C.n) C.sel = C.n ? C.n - 1 : 0;
  }
  cache_save();
}

/* ---- painting -------------------------------------------------------------- */

static void day_label(int32_t day, char *out, int n) {
  int y, m, d;
  int32_t t = today_day();
  civil_from_days(day, &y, &m, &d);
  if (C.have_clock && day == t)     { api->fmt(out, (size_t)n, "Today"); return; }
  if (C.have_clock && day == t + 1) { api->fmt(out, (size_t)n, "Tomorrow"); return; }
  api->fmt(out, (size_t)n, "%s %d %s", WDAY[weekday_of_day(day)], d, MON[m - 1]);
}

static void paint_bar(CRect c) {
  char bar[64];
  api->fill(rect(c.x, c.y + c.h - BAR_H, c.w, BAR_H), CLR_BAR);
  api->fmt(bar, sizeof bar, "%s", C.status);
  api->text((short)(c.x + 2), (short)(c.y + c.h - BAR_H + 1), bar,
            C.have_clock ? CLR_BARFG : CLR_WARN, CLR_BAR);
}

static void paint_agenda(CRect c) {
  int y = c.y + 1, i;
  int32_t last = -999999;
  int rows = 0;

  api->fill(rect(c.x, c.y, c.w, c.h - BAR_H), CLR_BG);

  if (!C.n) {
    api->text((short)(c.x + 8), (short)(c.y + 20), "Nothing in the diary.",
              CLR_DIM, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 34), "a  add an event",
              CLR_DIM, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 46), "s  sync with Google",
              CLR_DIM, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 58), "m  month view",
              CLR_DIM, CLR_BG);
    paint_bar(c);
    return;
  }

  C.shown_top = C.top;
  for (i = C.top; i < C.n && y + ROW_H <= c.y + c.h - BAR_H; i++) {
    Event *e = &C.ev[i];
    int32_t d = local_day(e->start);
    char line[48], when[8];
    int hh, mm;

    if (e->deleted) continue;

    if (d != last) {
      char head[24];
      if (y + ROW_H + 10 > c.y + c.h - BAR_H) break;
      day_label(d, head, sizeof head);
      api->text((short)(c.x + 2), (short)(y + 1), head, CLR_HEAD, CLR_BG);
      y += 10;
      last = d;
    }

    api->fill(rect(c.x, y, c.w, ROW_H - 1), i == C.sel ? CLR_SEL : CLR_ROW);
    if (e->all_day) api->fmt(when, sizeof when, "%s", "all");
    else {
      local_hm(e->start, &hh, &mm);
      api->fmt(when, sizeof when, "%02d:%02d", hh, mm);
    }
    api->fmt(line, sizeof line, "%s", e->summary);
    api->text((short)(c.x + 3), (short)(y + 2), when,
              e->dirty ? CLR_PEND : CLR_DIM, i == C.sel ? CLR_SEL : CLR_ROW);
    api->text((short)(c.x + 38), (short)(y + 2), line, CLR_TEXT,
              i == C.sel ? CLR_SEL : CLR_ROW);
    y += ROW_H;
    rows++;
  }
  C.shown_rows = rows;
  paint_bar(c);
}

static void paint_month(CRect c) {
  int col, row, i;
  int first = weekday_of_day(days_from_civil(C.cur_y, C.cur_m, 1));
  int len = days_in_month(C.cur_y, C.cur_m);
  int cw = c.w / 7;
  int top = c.y + 22;
  int ch = (c.h - BAR_H - 22) / 6;
  char head[24];

  api->fill(rect(c.x, c.y, c.w, c.h - BAR_H), CLR_BG);
  api->fmt(head, sizeof head, "%s %d", MON[C.cur_m - 1], C.cur_y);
  api->text((short)(c.x + 2), (short)(c.y + 1), head, CLR_HEAD, CLR_BG);

  for (i = 0; i < 7; i++)
    api->text((short)(c.x + i * cw + 2), (short)(c.y + 12), WDAY[i], CLR_DIM, CLR_BG);

  for (i = 1; i <= len; i++) {
    int idx = first + i - 1;
    int32_t day = days_from_civil(C.cur_y, C.cur_m, i);
    int x, yy;
    char num[4];
    uint16_t bg = CLR_BG, fg = CLR_TEXT;

    col = idx % 7;
    row = idx / 7;
    if (row > 5) break;
    x = c.x + col * cw;
    yy = top + row * ch;

    if (i == C.cur_d) bg = CLR_SEL;
    if (C.have_clock && day == today_day()) fg = CLR_TODAY;

    api->fill(rect(x, yy, cw - 1, ch - 1), bg);
    api->fmt(num, sizeof num, "%d", i);
    api->text((short)(x + 2), (short)(yy + 1), num, fg, bg);
    if (day_has_event(day))
      api->fill(rect(x + cw - 6, yy + ch - 5, 3, 2), CLR_PEND);
  }
  paint_bar(c);
}

static void paint_add(CRect c) {
  char line[64];
  int y, m, d;
  int i;
  static const char *LABEL[FIELD_COUNT] = { "what", "when", "time" };

  civil_from_days(C.draft_day, &y, &m, &d);
  api->fill(rect(c.x, c.y, c.w, c.h - BAR_H), CLR_BG);
  api->text((short)(c.x + 4), (short)(c.y + 3), "New event", CLR_HEAD, CLR_BG);

  for (i = 0; i < FIELD_COUNT; i++) {
    int yy = c.y + 20 + i * 18;
    uint16_t bg = (i == (int)C.field) ? CLR_SEL : CLR_ROW;
    api->fill(rect(c.x + 4, yy, c.w - 8, 14), bg);
    api->text((short)(c.x + 7), (short)(yy + 3), LABEL[i], CLR_DIM, bg);
    if (i == FIELD_TITLE)
      api->fmt(line, sizeof line, "%s%s", C.draft,
               C.field == FIELD_TITLE ? "_" : "");
    else if (i == FIELD_DATE)
      api->fmt(line, sizeof line, "%s %d %s %d",
               WDAY[weekday_of_day(C.draft_day)], d, MON[m - 1], y);
    else
      api->fmt(line, sizeof line, "%02d:%02d", C.draft_hour, C.draft_min);
    api->text((short)(c.x + 40), (short)(yy + 3), line, CLR_TEXT, bg);
  }

  api->text((short)(c.x + 4), (short)(c.y + 76),
            "tab field  arrows adjust", CLR_DIM, CLR_BG);
  api->text((short)(c.x + 4), (short)(c.y + 88),
            "enter save  backspace back", CLR_DIM, CLR_BG);
  paint_bar(c);
}

/* Everything this app can be asked to do, stated once: the ctrl chords, the
 * menu bar, the help panel and the names a script or a model would use all
 * come out of this table. See CappUi.actions. */
enum { ACT_ADD = 1, ACT_DELETE, ACT_SYNC, ACT_AGENDA, ACT_MONTH,
       ACT_SAVE, ACT_CANCEL };

static const CappAction MAIN_ACTIONS[] = {
  { "add",    "Add",      "Event", 0x01, ACT_ADD },     /* ctrl-a */
  { "delete", "Delete",   "Event", 0x04, ACT_DELETE },  /* ctrl-d */
  { "agenda", "Agenda",   "View",  0x07, ACT_AGENDA },  /* ctrl-g */
  { "month",  "Month",    "View",  0x0D, ACT_MONTH },   /* ctrl-m */
  { "sync",   "Sync now", "View",  0x13, ACT_SYNC },    /* ctrl-s */
};

static const CappAction ADD_ACTIONS[] = {
  { "save",   "Save",   "Edit", 0, ACT_SAVE },
  { "cancel", "Cancel", "Edit", 0, ACT_CANCEL },
};

static const TbIcon MAIN_ICONS[] = {
  { "S", ACT_SYNC },
  { "+", ACT_ADD },
};

#define NMAIN ((int)(sizeof MAIN_ACTIONS / sizeof MAIN_ACTIONS[0]))
#define NADD  ((int)(sizeof ADD_ACTIONS / sizeof ADD_ACTIONS[0]))

static void use_main_menus(void) { toolbar_set(MAIN_ACTIONS, NMAIN, MAIN_ICONS, 2); }
static void use_add_menus(void)  { toolbar_set(ADD_ACTIONS, NADD, 0, 0); }

/* The month grid opens on today, or -- with no clock to say what today is --
 * on the first event, rather than on 1970. */
static void goto_month(void) {
  if (!C.have_clock && C.n)
    civil_from_days(local_day(C.ev[0].start), &C.cur_y, &C.cur_m, &C.cur_d);
  C.view = VIEW_MONTH;
}

/* The one place that knows what anything does. */
static int do_action(int a) {
  switch (a) {
  case ACT_ADD:    begin_add(); use_add_menus(); return 1;
  case ACT_DELETE: delete_selected(); return 1;
  case ACT_SYNC:   sync_begin(); return 1;
  case ACT_AGENDA: C.view = VIEW_AGENDA; return 1;
  case ACT_MONTH:  goto_month(); return 1;
  case ACT_SAVE:   commit_add(); use_main_menus(); return 1;
  case ACT_CANCEL: C.view = VIEW_AGENDA; use_main_menus(); return 1;
  default: return 0;
  }
}

static void app_paint(void *st, CRect c) {
  (void)st;
  {
    CRect full = c;
    /* Only the dropdown moved: draw it and nothing else. Repainting the
     * content underneath first is what made the menu flicker. */
    if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
    /* Only the bar changed -- the busy dots ticking over. Leave the content. */
    if (toolbar_only_bar()) { toolbar_paint_bar(full); return; }
    toolbar_paint_bar(full);
    c = toolbar_rest(full);
    if (C.view == VIEW_MONTH)    paint_month(c);
    else if (C.view == VIEW_ADD) paint_add(c);
    else                         paint_agenda(c);
    /* Last: a dropdown is drawn over the content it covers. */
    toolbar_paint_menu(full);
  }
}

/* ---- input ----------------------------------------------------------------- */

static void month_step(int days) {
  int32_t d = days_from_civil(C.cur_y, C.cur_m, C.cur_d) + days;
  civil_from_days(d, &C.cur_y, &C.cur_m, &C.cur_d);
}

static void scroll_to_sel(void) {
  if (C.sel < C.top) C.top = C.sel;
  if (C.shown_rows > 0 && C.sel >= C.top + C.shown_rows)
    C.top = C.sel - C.shown_rows + 1;
  if (C.top < 0) C.top = 0;
}

static int key_agenda(unsigned char k) {
  switch (k) {
  case CAPP_KEY_UP:   if (C.sel > 0) { C.sel--; scroll_to_sel(); } return 1;
  case CAPP_KEY_DOWN: if (C.sel + 1 < C.n) { C.sel++; scroll_to_sel(); } return 1;
  case 'm': case 'M': return do_action(ACT_MONTH);
  case 'a': case 'A': return do_action(ACT_ADD);
  case 's': case 'S': return do_action(ACT_SYNC);
  case 'd': case 'D': case 0x7F: return do_action(ACT_DELETE);
  default: return 0;
  }
}

static int key_month(unsigned char k) {
  switch (k) {
  case CAPP_KEY_LEFT:  month_step(-1); return 1;
  case CAPP_KEY_RIGHT: month_step(1);  return 1;
  case CAPP_KEY_UP:    month_step(-7); return 1;
  case CAPP_KEY_DOWN:  month_step(7);  return 1;
  case 'm': case 'M':  return do_action(ACT_AGENDA);
  case 'a': case 'A':  return do_action(ACT_ADD);
  case 's': case 'S':  return do_action(ACT_SYNC);
  case CAPP_KEY_ENTER: {
    /* Into the agenda, at the first event on this day -- or the next one
     * after it, so an empty day still lands somewhere useful. */
    int32_t want = days_from_civil(C.cur_y, C.cur_m, C.cur_d);
    int i;
    for (i = 0; i < C.n; i++)
      if (local_day(C.ev[i].start) >= want) break;
    C.sel = (i < C.n) ? i : (C.n ? C.n - 1 : 0);
    C.top = C.sel;
    C.view = VIEW_AGENDA;
    return 1;
  }
  default: return 0;
  }
}

static int key_add(unsigned char k) {
  if (k == CAPP_KEY_ENTER) return do_action(ACT_SAVE);
  if (k == '\t') { C.field = (Field)((C.field + 1) % FIELD_COUNT); return 1; }

  if (C.field == FIELD_DATE) {
    if (k == CAPP_KEY_LEFT)  { C.draft_day--; return 1; }
    if (k == CAPP_KEY_RIGHT) { C.draft_day++; return 1; }
    if (k == CAPP_KEY_UP)    { C.draft_day -= 7; return 1; }
    if (k == CAPP_KEY_DOWN)  { C.draft_day += 7; return 1; }
  }
  if (C.field == FIELD_TIME) {
    int mins = C.draft_hour * 60 + C.draft_min;
    if (k == CAPP_KEY_LEFT)  mins -= 15;
    else if (k == CAPP_KEY_RIGHT) mins += 15;
    else if (k == CAPP_KEY_UP)    mins += 60;
    else if (k == CAPP_KEY_DOWN)  mins -= 60;
    else mins = -1;
    if (mins >= 0 || k == CAPP_KEY_LEFT || k == CAPP_KEY_DOWN) {
      while (mins < 0) mins += 1440;
      mins %= 1440;
      C.draft_hour = mins / 60;
      C.draft_min = mins % 60;
      return 1;
    }
  }

  if (k == CAPP_KEY_BACK) {
    if (C.field == FIELD_TITLE && C.draft_len > 0) { C.draft[--C.draft_len] = 0; return 1; }
    return do_action(ACT_CANCEL);
  }
  /* No quote or backslash: the summary goes into a JSON body, and escaping
   * two characters properly costs more than refusing them. */
  if (C.field == FIELD_TITLE && k >= 32 && k < 127 && k != '"' && k != '\\' &&
      C.draft_len < SUMMARY_MAX) {
    C.draft[C.draft_len++] = (char)k;
    C.draft[C.draft_len] = 0;
    return 1;
  }
  return 0;
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (C.view == VIEW_ADD)   return key_add(k);
  if (C.view == VIEW_MONTH) return key_month(k);
  return key_agenda(k);
}

/* What the shell calls for a chord out of the table, and what the menu bar
 * and any script reach through. */
static int app_action(void *st, int a) {
  (void)st;
  return do_action(a);
}

static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)button; (void)x;
  {
    int a = toolbar_click(x, y);
    if (a == TB_CONSUMED) return 1;             /* opened or closed a menu */
    if (a != TB_NONE) return do_action(a);
  }
  y = (short)(y - toolbar_h());
  if (C.view != VIEW_AGENDA) return 0;
  {
    /* The agenda's rows are not a fixed pitch -- day headers sit between them
     * -- so the row under a click is found by walking the same layout paint
     * used, rather than by dividing. */
    int yy = 1, i;
    int32_t last = -999999;
    for (i = C.shown_top; i < C.n; i++) {
      int32_t d = local_day(C.ev[i].start);
      if (C.ev[i].deleted) continue;
      if (d != last) { yy += 10; last = d; }
      if (y >= yy && y < yy + ROW_H) { C.sel = i; return 1; }
      yy += ROW_H;
      if (yy > SCREEN_H - BAR_H) break;
    }
  }
  return 0;
}

/* Every pass of the shell's loop. Collects the request if one is in flight,
 * and starts one when it is due -- the sync is automatic, so a diary edited
 * on a laptop turns up without anybody pressing anything. Returns 1 to ask
 * for a repaint, which is only when something actually changed. */
static int app_tick(void *st, uint32_t now_ms) {
  int was = C.stage, n = C.n;
  (void)st;

  sync_tick();
  toolbar_busy(C.stage != SYNC_IDLE);
  /* Just the bar, so the dots can move without redrawing the window. */
  if (C.stage != SYNC_IDLE) toolbar_damage_bar();
  toolbar_busy(C.stage != SYNC_IDLE);

  /* The first sync is on open rather than on a timer: the reason to open a
   * calendar is to find out what is in it. */
  if (C.stage == SYNC_IDLE &&
      (!C.tried_once || (int32_t)(now_ms - C.next_auto) >= 0))
    sync_begin();

  /* Repaint while waiting, so the bar's dots animate. Otherwise only when
   * something actually changed. */
  return was != C.stage || n != C.n || C.stage != SYNC_IDLE;
}

static int app_mouse(void *st, int16_t x, int16_t y, int buttons, int wheel) {
  int changed;
  (void)st; (void)buttons; (void)wheel;
  changed = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) changed = 1;
  return changed;
}

static int app_wants_text(void *st) {
  (void)st;
  return C.view == VIEW_ADD && C.field == FIELD_TITLE;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_NEEDS_NET,
  "Calendar",
  /* 16x16: a wall calendar, two rings and a heading. */
  { 0x00, 0x00, 0x18, 0x18, 0x7F, 0xFE, 0x40, 0x02,
    0x7F, 0xFE, 0x40, 0x02, 0x55, 0x52, 0x40, 0x02,
    0x55, 0x52, 0x40, 0x02, 0x55, 0x52, 0x40, 0x02,
    0x7F, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "arrows\tmove\nm\tmonth view / agenda\nenter\topen the day\n"
  "a\tadd an event\nd\tdelete (unsynced only)\ns\tsync with Google\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&C, 0, sizeof C);

  refresh_clock();
  if (C.have_clock) {
    C.cur_y = C.today_y; C.cur_m = C.today_m; C.cur_d = C.today_d;
  } else {
    C.cur_y = 1970; C.cur_m = 1; C.cur_d = 1;
  }

  /* The cache first, and on screen, before anything is asked of the radio.
   * That is the whole difference between a diary and a web page. */
  cache_load();
  if (!C.have_clock)
    say("clock not set -- times may be wrong");
  else if (C.n)
    api->fmt(C.status, sizeof C.status, "%d event%s -- s to sync",
             C.n, C.n == 1 ? "" : "s");
  else
    say("press s to sync");

  toolbar_init(api, MAIN_ACTIONS, NMAIN, MAIN_ICONS, 2);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.tick = app_tick;
  UI.actions = MAIN_ACTIONS;
  UI.nactions = NMAIN;
  UI.action = app_action;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
