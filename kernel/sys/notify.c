/* Notifications. See notify.h; the list and the rules are notifyq.c. */
#include "kernel/sys/notify.h"
#include "kernel/sys/notifyq.h"
#include "kernel/sys/blip.h"
#include "kernel/sys/prefs.h"
#include "kernel/sys/clock.h"
#include "kernel/fs/fs.h"
#include "kernel/net/httpq.h"
#include "kernel/net/update.h"
#include "kernel/net/wifi.h"
#include "kernel/drv/display.h"
#include "kernel/ui/draw.h"
#include "kernel/ui/shell.h"
#include "kernel/ui/app.h"
#include "kernel/drv/keyboard.h"
#include "kernel/sys/power.h"
#include "kernel/sys/bg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "notify";

#define BANNER_H     24
#define BANNER_MS    4500
#define CHAT_EVERY   30000
#define WIFI_EVERY   600000               /* the poll's own rejoin, at most this often */
#define WIFI_FIRST   45000                /* after boot's clock sync has had its go */
#define CHAT_SEEN    "/cache/chat.seen"   /* also written by apps/chat.c */
#define CHAT_NAME    "/config/chat.txt"
#define NOTE_SEEN    "/cache/note.seen"
#define SCHED_FILE   "/cache/notify.sched"
#define SCHED_MAX    32
#define RING_MS      60000
#define RING_EVERY   1600

#define C_BG     RGB565(24, 28, 40)
#define C_EDGE   RGB565(70, 84, 120)
#define C_FG     RGB565(236, 240, 248)
#define C_DIM    RGB565(150, 160, 180)
#define C_SEL    RGB565(44, 54, 84)

static Nq       s_q;
static void   (*s_repaint)(void);
static int      s_banner;             /* showing the newest */
static uint32_t s_banner_until, s_banner_drawn, s_now;
static int      s_center, s_sel;

/* ---- settings ------------------------------------------------------------ */

static int s_chat = -1;
int  notify_chat_on(void) { if (s_chat < 0) s_chat = prefs_get_u16("n_chat", 1) != 0; return s_chat; }
void notify_set_chat(int on) { s_chat = on != 0; prefs_set_u16("n_chat", s_chat); }

/* ---- the colour of an app's notifications -------------------------------- */

static uint16_t accent(const char *app) {
  if (!strcmp(app, "Chat")) return RGB565(110, 196, 128);
  if (!strcmp(app, "Calendar")) return RGB565(228, 86, 76);
  if (!strcmp(app, "Clock")) return RGB565(255, 204, 82);
  return RGB565(96, 176, 232);
}

/* ---- the banner ------------------------------------------------------------ */

/* "Chat  Arty", or "Chat  Arty  +2" for one standing for three. */
static void head_text(char *out, size_t n, const NqItem *it) {
  if (it->count > 1) snprintf(out, n, "%s  %s  +%d", it->app, it->title, it->count - 1);
  else snprintf(out, n, "%s  %s", it->app, it->title);
}

static Rect R(int x, int y, int w, int h) {
  Rect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static void banner_body(void) {
  const NqItem *it = &s_q.it[0];
  char head[NQ_APP + NQ_TITLE + 12];
  uint16_t a = accent(it->app);
  draw_rect(R(0, 0, DISPLAY_W, BANNER_H), C_BG);
  draw_rect(R(0, 0, 3, BANNER_H), a);
  draw_rect(R(0, BANNER_H - 1, DISPLAY_W, 1), C_EDGE);
  head_text(head, sizeof head, it);
  draw_text_ellipsis(8, 3, DISPLAY_W - 12, head, a, C_BG);
  draw_text_ellipsis(8, 13, DISPLAY_W - 12, it->text, C_FG, C_BG);
}

/* Off the panel and sent whole, through draw_offscreen's strip -- it used to
 * malloc 11.5 KB of its own every time it was drawn. */
static void banner_strip(void *ctx) { (void)ctx; banner_body(); }

static void banner_paint(void) {
  Rect was = draw_clip();
  if (!s_banner || !s_q.n) return;
  draw_reserve_top(0);                         /* the one thing allowed there */
  draw_set_clip(R(0, 0, DISPLAY_W, BANNER_H));
  draw_offscreen(R(0, 0, DISPLAY_W, BANNER_H), C_BG, banner_strip, NULL);
  draw_reserve_top(BANNER_H);                  /* and nothing else */
  draw_set_clip(was);
  s_banner_drawn = s_now;
}

static void center_paint(void);

/* After a shell has painted -- a screenshot repaints everything -- what is
 * over it goes back: the centre if it is open, else the banner. */
void notify_paint_over(void) {
  if (s_center) center_paint();
  else banner_paint();
}

/* ---- posting ------------------------------------------------------------------ */

static uint32_t stamp(void) {
  uint32_t t = clock_epoch();
  return t ? t : s_now / 1000;
}

void notify_log(const char *app, const char *title, const char *text) {
  nq_push(&s_q, app, title, text, stamp());
  ESP_LOGI(TAG, "%s: %s: %s", app, title, text);
}

/* The banner and the chime for whatever is newest in the list. */
static void show_newest(void) {
  if (s_center) return;                     /* it is in the list in front of them */
  if (power_asleep()) {
    /* No banner on a sleeping screen: the clock lists it, and only fn-o,
     * fn-c or opt-backspace wakes the screen (src/main.c). */
    blip(BLIP_NOTIFY);
    power_notice();
    return;
  }
  s_banner = 1;
  s_banner_until = s_now + BANNER_MS;
  banner_paint();
  blip(BLIP_NOTIFY);
  if (s_repaint && ui_shell() == UI_LAUNCHER && !shell_running_app()) s_repaint();  /* the bar's dot */
}

void notify_post(const char *app, const char *title, const char *text) {
  notify_log(app, title, text);
  show_newest();
}

void notify_opened(const char *app) {
  if (app) nq_read_app(&s_q, app);
}

int notify_unread(void) { return nq_unread(&s_q); }

int notify_unread_at(int i, const char **app, const char **title, const char **text) {
  int k;
  for (k = 0; k < s_q.n; k++) {
    const NqItem *it = &s_q.it[k];
    if (it->read) continue;
    if (i-- == 0) { *app = it->app; *title = it->title; *text = it->text; return 1; }
  }
  return 0;
}

int notify_covers(void) { return s_banner && !s_center ? BANNER_H : 0; }

/* ---- Chat: one request every half minute, while WiFi is up -------------------- */

static int      s_chat_seen = -2;       /* -2 not read yet; -1 never asked */
static uint32_t s_chat_next, s_wifi_next = WIFI_FIRST;
static int      s_chat_busy;
static char     s_reply[1024];
static const char s_owner = 0;          /* our name in the request slot */

static int read_small(const char *path, char *out, int n) {
  int fd = fs_open(path, FS_O_READ), r;
  if (fd < 0) { out[0] = 0; return -1; }
  r = fs_read(fd, out, (size_t)(n - 1));
  fs_close(fd);
  if (r < 0) r = 0;
  out[r] = 0;
  return r;
}

static void chat_seen_load(void) {
  char b[16];
  s_chat_seen = read_small(CHAT_SEEN, b, sizeof b) > 0 ? atoi(b) : -1;
}

static void chat_seen_save(int id) {
  char b[16];
  int fd = fs_open(CHAT_SEEN, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC), n;
  if (fd < 0) return;
  n = snprintf(b, sizeof b, "%d\n", id);
  fs_write(fd, b, (size_t)n);
  fs_close(fd);
}

/* `app` is what an awake screen shows: it is telling them itself. Whichever
 * shell has it -- shell_running_app is the launcher's app or the desktop's
 * focused window -- and not while the screen sleeps, when nothing is shown
 * and the lock screen's list is where it should land. One test for the
 * Chat poll and for a scheduled notification falling due. */
static int on_screen(const char *app) {
  const AppDef *a = shell_running_app();
  return a && a->name && !strcmp(a->name, app) && !power_asleep();
}
static int chat_on_screen(void) { return on_screen("Chat"); }

static int s_note_seen = -2;            /* -2 not read yet; -1 never asked */

static void note_seen_save(int id) {
  char b[16];
  int fd = fs_open(NOTE_SEEN, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC), n;
  if (fd < 0) return;
  n = snprintf(b, sizeof b, "%d\n", id);
  fs_write(fd, b, (size_t)n);
  fs_close(fd);
}

/* What a poll brings is stacked, one per app however many came -- a device
 * back on after a day is told "Chat +4", not five banners -- and announced
 * once, after the whole reply is read. */
static int s_fresh;

static void stack(const char *app, const char *title, const char *text, int quiet) {
  nq_stack(&s_q, app, title, text, stamp());
  ESP_LOGI(TAG, "%s: %s: %s", app, title, text);
  if (!quiet) s_fresh = 1;
}

static void each_note(void *ctx, NqStr app, NqStr title, NqStr text) {
  char a[NQ_APP], t[NQ_TITLE], x[NQ_TEXT];
  (void)ctx;
  snprintf(a, sizeof a, "%.*s", (int)app.n, app.s);
  snprintf(t, sizeof t, "%.*s", (int)title.n, title.s);
  snprintf(x, sizeof x, "%.*s", (int)text.n, text.s);
  stack(a, t, x, on_screen(a));                /* on screen: the app showed it already */
}

static void each_message(void *ctx, const char *name, size_t nl, const char *text, size_t tl) {
  char who[NQ_TITLE], what[NQ_TEXT];
  (void)ctx;
  if (chat_on_screen()) return;                /* the app is showing it */
  snprintf(who, sizeof who, "%.*s", (int)nl, name);
  snprintf(what, sizeof what, "%.*s", (int)tl, text);
  stack("Chat", who, what, 0);
}

static void url_enc(char *out, size_t n, const char *s) {
  static const char HEX[] = "0123456789ABCDEF";
  size_t k = 0;
  for (; *s && k + 4 < n; s++) {
    unsigned char ch = (unsigned char)*s;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
      out[k++] = (char)ch;
    else if (ch == '\n' || ch == '\r') break;
    else { out[k++] = '%'; out[k++] = HEX[ch >> 4]; out[k++] = HEX[ch & 15]; }
  }
  out[k] = 0;
}

static void chat_tick(uint32_t now) {
  if (s_chat_busy) {
    int r = httpq_poll_as(&s_owner, s_reply, sizeof s_reply);
    int last;
    if (r == HTTPQ_PENDING) return;
    s_chat_busy = 0;
    if (r < 0) return;                          /* offline, signed out: try later */
    {
      int nl;
      /* a first ask (-1) is told only where things are: not old news */
      last = nq_parse_poll(s_reply, notify_chat_on() && s_chat_seen >= 0 ? each_message : NULL,
                           s_note_seen >= 0 ? each_note : NULL, &nl, NULL);
      if (last >= 0 && last != s_chat_seen && notify_chat_on()) { s_chat_seen = last; chat_seen_save(last); }
      if (nl >= 0 && nl != s_note_seen) { s_note_seen = nl; note_seen_save(nl); }
      if (s_fresh) { s_fresh = 0; show_newest(); }
    }
    return;
  }
  if ((int32_t)(now - s_chat_next) < 0) return;
  s_chat_next = now + CHAT_EVERY;
  /* Not only for Chat: the server's own notes (a Build done) come this way. */
  if (!wifi_is_connected()) {
    /* Nothing else brings the radio back once an app has let it go, and a
     * device switched on goes no further than the boot's clock sync -- so
     * what came while it was off waited for the next `update` or Chat. From
     * the launcher or the clock only: an app on screen may have let it go
     * for the memory. Quietly, and seldom, for a device with no network. */
    if (!shell_running_app() && update_base()[0] && (int32_t)(now - s_wifi_next) >= 0) {
      s_wifi_next = now + WIFI_EVERY;
      bg_submit(BG_WIFI_QUIET);
      s_chat_next = now + 15000;               /* look again once it has joined */
    }
    return;
  }
  if (httpq_active() || !fs_mounted()) return;
  {
    char url[256], name[24], me[64], b[16];
    const char *base = update_base(), *tok = update_token();
    if (!base[0]) return;
    /* The Chat app writes the last message it showed: read since then. */
    if (read_small(CHAT_SEEN, b, sizeof b) > 0 && atoi(b) > s_chat_seen) s_chat_seen = atoi(b);
    if (s_chat_seen == -2) chat_seen_load();
    if (s_note_seen == -2) s_note_seen = read_small(NOTE_SEEN, b, sizeof b) > 0 ? atoi(b) : -1;
    read_small(CHAT_NAME, name, sizeof name);
    url_enc(me, sizeof me, name);
    snprintf(url, sizeof url, "%s/notify/poll?chat=%d&me=%s&note=%d", base,
             notify_chat_on() ? s_chat_seen : -1, me, s_note_seen);
    /* Into s_reply itself: every 30 s, so not 8 KB allocated and freed
     * each time in a heap that breaks up (s_reply is a static, so it
     * outlives any request, as httpq_start_into requires). */
    if (httpq_start_into(&s_owner, s_reply, sizeof s_reply, "GET", url, NULL, NULL,
                         tok[0] ? tok : NULL, 10000) == 0)
      s_chat_busy = 1;
  }
}

/* ---- the centre ------------------------------------------------------------------- */

static void center_paint(void) {
  int i, y, rows = (DISPLAY_H - 16) / 20, top;
  draw_set_clip(R(0, 0, DISPLAY_W, DISPLAY_H));
  draw_rect(R(0, 0, DISPLAY_W, DISPLAY_H), C_BG);
  draw_rect(R(0, 0, DISPLAY_W, 13), C_EDGE);
  draw_text(4, 3, "Notifications", C_FG, C_EDGE);
  draw_text(DISPLAY_W - 6 * 19 - 4, 3, "enter open  d clear", C_DIM, C_EDGE);
  if (!s_q.n) {
    draw_text(12, 40, "Nothing yet.", C_DIM, C_BG);
    return;
  }
  top = s_sel >= rows ? s_sel - rows + 1 : 0;
  for (i = top, y = 15; i < s_q.n && i - top < rows; i++, y += 20) {
    const NqItem *it = &s_q.it[i];
    char head[NQ_APP + NQ_TITLE + 12];
    uint16_t bg = i == s_sel ? C_SEL : C_BG;
    draw_rect(R(0, y, DISPLAY_W, 20), bg);
    draw_rect(R(0, y, 3, 20), accent(it->app));
    head_text(head, sizeof head, it);
    draw_text_ellipsis(8, y + 2, DISPLAY_W - 12, head, accent(it->app), bg);
    draw_text_ellipsis(8, y + 11, DISPLAY_W - 12, it->text, it->read ? C_DIM : C_FG, bg);
  }
}

void notify_center_open(void) {
  s_center = 1;
  s_sel = 0;
  s_banner = 0;
  draw_reserve_top(0);
  center_paint();
}

int notify_center_active(void) { return s_center; }

static void center_close(void) {
  s_center = 0;
  nq_read_all(&s_q);
  if (s_repaint) s_repaint();
}

void notify_center_key(uint8_t k) {
  switch (k) {
  case KEY_UP: if (s_sel > 0) s_sel--; break;
  case KEY_DOWN: if (s_sel < s_q.n - 1) s_sel++; break;
  case 'd': case 'D': case 0x7F: case KEY_BACKSPACE:
    nq_remove(&s_q, s_sel);
    if (s_sel >= s_q.n && s_sel) s_sel--;
    break;
  case KEY_ENTER:
    if (s_q.n) {
      char app[NQ_APP];
      snprintf(app, sizeof app, "%s", s_q.it[s_sel].app);
      center_close();
      shell_open_app(app);                 /* from whichever shell is up */
      return;
    }
    center_close();
    return;
  default:
    center_close();
    return;
  }
  center_paint();
}

/* ---- later: scheduled notifications ---------------------------------------------- */

typedef struct {
  uint8_t  used, ring;
  char     app[NQ_APP], key[16];
  uint32_t due_epoch;                   /* wall clock, when there was one */
  uint32_t due_ms;                      /* uptime, otherwise */
  char     title[NQ_TITLE], text[NQ_TEXT];
} Sched;

static Sched    s_sched[SCHED_MAX];
static int      s_ring;
static uint32_t s_ring_until, s_ring_next;
static const char s_ring_owner = 0;

static void sched_save(void) {
  int fd, i;
  char line[NQ_APP + 16 + NQ_TITLE + NQ_TEXT + 40];
  if (!fs_mounted()) return;
  fd = fs_open(SCHED_FILE, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) return;
  for (i = 0; i < SCHED_MAX; i++) {
    const Sched *s = &s_sched[i];
    int n;
    if (!s->used || !s->due_epoch) continue;    /* uptime ones do not outlive a restart */
    n = snprintf(line, sizeof line, "%s\t%s\t%lu\t%d\t%s\t%s\n", s->app, s->key,
                 (unsigned long)s->due_epoch, s->ring, s->title, s->text);
    fs_write(fd, line, (size_t)n);
  }
  fs_close(fd);
}

static void sched_load(void) {
  /* Once, at boot: on the heap for the reading, not 4.6 KB of .bss for the
   * uptime. */
  enum { SCHED_READ = SCHED_MAX * 144 };
  char *buf, *p, *f[6];
  int r, i = 0;
  if ((buf = malloc(SCHED_READ)) == NULL) {
    ESP_LOGW(TAG, "no %d bytes to read %s: scheduled notifications not restored",
             SCHED_READ, SCHED_FILE);
    return;
  }
  if (read_small(SCHED_FILE, buf, SCHED_READ) <= 0) { free(buf); return; }
  for (p = buf; *p && i < SCHED_MAX; ) {
    char *end = strchr(p, '\n');
    int k = 0;
    if (end) *end = 0;
    f[0] = p;
    for (r = 0; p[r] && k < 5; r++) if (p[r] == '\t') { p[r] = 0; f[++k] = p + r + 1; }
    if (k == 5) {
      Sched *s = &s_sched[i++];
      s->used = 1;
      snprintf(s->app, sizeof s->app, "%s", f[0]);
      snprintf(s->key, sizeof s->key, "%s", f[1]);
      s->due_epoch = (uint32_t)strtoul(f[2], NULL, 10);
      s->ring = (uint8_t)atoi(f[3]);
      snprintf(s->title, sizeof s->title, "%s", f[4]);
      snprintf(s->text, sizeof s->text, "%s", f[5]);
    }
    if (!end) break;
    p = end + 1;
  }
  free(buf);
}

static Sched *sched_find(const char *app, const char *key) {
  int i;
  for (i = 0; i < SCHED_MAX; i++)
    if (s_sched[i].used && !strcmp(s_sched[i].app, app) && !strcmp(s_sched[i].key, key))
      return &s_sched[i];
  return NULL;
}

int notify_at(const char *app, const char *key, uint32_t seconds,
              const char *title, const char *text, int ring) {
  Sched *s = sched_find(app, key);
  int i;
  uint32_t epoch = clock_epoch();
  for (i = 0; !s && i < SCHED_MAX; i++) if (!s_sched[i].used) s = &s_sched[i];
  if (!s) return -1;
  memset(s, 0, sizeof *s);
  s->used = 1;
  s->ring = ring != 0;
  snprintf(s->app, sizeof s->app, "%s", app);
  snprintf(s->key, sizeof s->key, "%s", key);
  snprintf(s->title, sizeof s->title, "%s", title);
  snprintf(s->text, sizeof s->text, "%s", text);
  s->due_ms = s_now + seconds * 1000u;
  s->due_epoch = epoch ? epoch + seconds : 0;
  sched_save();
  return 0;
}

void notify_cancel(const char *app, const char *key) {
  int i, any = 0;
  if (!strcmp(key, "*")) {                     /* everything this app set */
    for (i = 0; i < SCHED_MAX; i++)
      if (s_sched[i].used && !strcmp(s_sched[i].app, app)) { s_sched[i].used = 0; any = 1; }
  } else {
    Sched *s = sched_find(app, key);
    if (s) { s->used = 0; any = 1; }
  }
  if (any) sched_save();
}

static void sched_tick(uint32_t now) {
  uint32_t epoch = clock_epoch();
  int i, changed = 0;
  for (i = 0; i < SCHED_MAX; i++) {
    Sched *s = &s_sched[i];
    int due;
    if (!s->used) continue;
    due = s->due_epoch && epoch ? epoch >= s->due_epoch : (int32_t)(now - s->due_ms) >= 0;
    if (!due) continue;
    s->used = 0;
    changed = 1;
    if (on_screen(s->app)) continue;           /* the app is showing it itself */
    notify_post(s->app, s->title, s->text);
    if (s->ring) {
      s_ring = 1;
      s_ring_until = now + RING_MS;
      s_ring_next = now + RING_EVERY;
      s_banner_until = s_ring_until;           /* the banner stays while it rings */
      power_wake_now();
      power_hold(&s_ring_owner, 1);
    }
  }
  if (changed) sched_save();
}

int notify_ringing(void) { return s_ring; }

void notify_dismiss(void) {
  if (!s_ring) return;
  s_ring = 0;
  power_hold(&s_ring_owner, 0);
  s_banner_until = s_now;                      /* down at the next tick */
}

static void ring_tick(uint32_t now) {
  if (!s_ring) return;
  if ((int32_t)(now - s_ring_until) >= 0) { notify_dismiss(); return; }
  if ((int32_t)(now - s_ring_next) >= 0) {
    s_ring_next = now + RING_EVERY;
    blip(BLIP_NOTIFY);
  }
}

/* ---- time ------------------------------------------------------------------------- */

void notify_init(void (*repaint)(void)) {
  s_repaint = repaint;
  nq_init(&s_q);
  s_chat_next = 15000;                          /* the first look, a little after boot */
  if (fs_mounted()) sched_load();
}

void notify_tick(uint32_t now) {
  s_now = now;
  if (s_banner) {
    if ((int32_t)(now - s_banner_until) >= 0) {
      s_banner = 0;
      draw_reserve_top(0);
      if (s_repaint && !s_center) s_repaint();  /* what the banner covered */
    } else if (ui_shell() == UI_NONE && (int32_t)(now - s_banner_drawn) >= 150) {
      /* Only over the console, which writes its cells to the panel without
       * a clip. Everything the shells draw keeps out of the reserved rows,
       * so over them the banner stays put once drawn; a repaint of
       * everything (a screenshot) puts it back through notify_paint_over.
       * Redrawing it every 150 ms regardless was 11.5 KB of malloc a time. */
      banner_paint();
    }
  }
  chat_tick(now);
  sched_tick(now);
  ring_tick(now);
}
