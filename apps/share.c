/* Share -- the card as a network drive, while this is open.
 *
 * The server is the kernel's (kernel/net/share.c); this is the switch and
 * the readout. It starts sharing when it opens and the kernel stops sharing
 * when it is closed, which is the whole design: a thing you can see is on.
 * The console's `share` command outlives the command instead, for the
 * development loop.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"

#define LINES     7
#define LINE_MAX  36

#define CLR_BG   CAPP_RGB(8, 10, 14)
#define CLR_FG   CAPP_RGB(226, 232, 242)
#define CLR_DIM  CAPP_RGB(130, 140, 158)
#define CLR_URL  CAPP_RGB(120, 220, 140)
#define CLR_BAD  CAPP_RGB(240, 120, 100)

static const CardApi *api;

static struct {
  int  on;
  char status[96];
  char log[LINES][LINE_MAX];
  int  nlog;                 /* lines used; the newest is at nlog - 1 */
  uint32_t count;
} S;

static void start(void) {
  S.on = api->share_start() == 0;
  api->fmt(S.status, sizeof S.status, "%s", api->share_status());
}

/* Space is the switch, as it is in every app: the drive goes away without
 * leaving, and comes back on the same key. */
static void stop(void) {
  api->share_stop();
  S.on = 0;
  api->fmt(S.status, sizeof S.status, "stopped");
}

/* The panel has no framebuffer: filling a line and writing over it is a
 * blink you can see, and Explorer sends requests in bursts, so the whole
 * screen used to flash with every one. text paints its own background, so
 * every line is written padded with spaces to the full width -- it covers
 * what was there -- and only what no text covers is filled: the margins and
 * the gaps between lines, background over background. */
#define TOP_LOG   62               /* y of the first log line */
#define PITCH     10

static CRect C;                    /* the rect the last paint was given */

/* Log lines the screen has room for, ending clear of the request count.
 * Seven were asked for and six fit on 135 rows: the seventh was written
 * over the count. */
static int log_rows(CRect c) {
  int room = (c.h - 10 - 2) - TOP_LOG - 8, n;
  if (room < 0) return 0;
  n = room / PITCH + 1;
  return n > LINES ? LINES : n;
}

/* One line at y, from the left margin to the right edge; `*cur` is how far
 * down the screen is already drawn, and the gap above this line is filled. */
static void line(CRect c, int *cur, int y, const char *s, uint16_t fg) {
  char buf[64];
  int cols = (c.w - 8) / 6, i = 0;
  if (cols > (int)sizeof buf - 1) cols = (int)sizeof buf - 1;
  if (cols < 0) cols = 0;
  if (y > *cur) api->fill(capp_rect(c.x, *cur, c.w, y - *cur), CLR_BG);
  for (; s && s[i] && i < cols; i++) buf[i] = s[i];
  for (; i < cols; i++) buf[i] = ' ';
  buf[cols] = 0;
  api->fill(capp_rect(c.x, y, 8, 8), CLR_BG);
  api->text((int16_t)(c.x + 8), (int16_t)y, buf, fg, CLR_BG);
  api->fill(capp_rect(c.x + 8 + cols * 6, y, c.w - 8 - cols * 6, 8), CLR_BG);
  *cur = y + 8;
}

static void app_paint(void *st, CRect c) {
  int i, cur = c.y, rows = log_rows(c), first;
  char bar[48];
  (void)st;
  C = c;
  line(c, &cur, c.y + 6, "Share", CLR_FG);
  if (S.on) {
    line(c, &cur, c.y + 20, "the card is on the network at", CLR_DIM);
    line(c, &cur, c.y + 32, S.status, CLR_URL);
    line(c, &cur, c.y + 46, "map it as a drive. space stops", CLR_DIM);
  } else {
    line(c, &cur, c.y + 20, "not sharing:", CLR_DIM);
    line(c, &cur, c.y + 32, S.status, CLR_BAD);
    line(c, &cur, c.y + 46, "enter or space starts it again", CLR_DIM);
  }
  /* The newest `rows` lines; rows not yet used are blank lines, written. */
  first = S.nlog > rows ? S.nlog - rows : 0;
  for (i = 0; i < rows; i++)
    line(c, &cur, c.y + TOP_LOG + i * PITCH,
         first + i < S.nlog ? S.log[first + i] : "", CLR_FG);
  api->fmt(bar, sizeof bar, "%lu requests", (unsigned long)S.count);
  line(c, &cur, c.y + c.h - 10, bar, CLR_DIM);
  if (c.y + c.h > cur) api->fill(capp_rect(c.x, cur, c.w, c.y + c.h - cur), CLR_BG);
}

static int app_tick(void *st, uint32_t now) {
  const char *l;
  int changed = 0, shown_before = S.nlog, rows = log_rows(C), from;
  (void)st; (void)now;
  while ((l = api->share_take_log()) != NULL) {
    if (S.nlog == LINES) {
      api->mem_move(S.log[0], S.log[1], sizeof S.log - sizeof S.log[0]);
      S.nlog--;
      shown_before = rows;              /* everything moved up */
    }
    api->fmt(S.log[S.nlog++], LINE_MAX, "%s", l);
    S.count++;
    changed = 1;
  }
  if (!changed) return 0;
  if (C.w == 0) return 1;               /* not painted yet: all of it */
  /* Only the log and the count changed. While the list is still filling,
   * the lines above the new ones stay where they are; once it scrolls,
   * every log line moved. */
  from = (shown_before >= rows || S.nlog > rows) ? 0 : shown_before;
  api->damage(capp_rect(C.x, C.y + TOP_LOG + from * PITCH, C.w,
                        C.h - TOP_LOG - from * PITCH));
  return 1;
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if ((k == CAPP_KEY_ENTER || k == ' ') && !S.on) { start(); return 1; }
  if (k == ' ' && S.on) { stop(); return 1; }
  return 0;
}

/* ---- commands ---------------------------------------------------------------
 *
 * Sharing is on while Share is on screen -- a thing you can see is on -- so
 * `start` opens it (CAPP_CMD_OPEN) rather than starting a server a headless
 * instance would stop again the moment the command ended. `status` is the
 * kernel's own answer, the same with Share closed. */
enum { ACT_START = 1, ACT_STATUS };

static const CappAction ACTIONS[] = {
  { "start",  "Start",  0, 0, ACT_START,
    "open Share: the card becomes a network drive while it is on screen", 0, 0,
    CAPP_CMD_YES | CAPP_CMD_OPEN },
  { "status", "Status", 0, 0, ACT_STATUS,
    "whether the card is shared, and the address to type on a PC", 0, 0, CAPP_CMD_YES },
};
#define NACT ((int)(sizeof ACTIONS / sizeof ACTIONS[0]))

static int app_action(void *st, int a) { (void)st; (void)a; return 0; }

static int app_command(void *st, int action, int argc, const char *const *argv,
                       char *out, size_t n) {
  (void)st;
  (void)argc;
  (void)argv;
  if (action != ACT_STATUS) { api->fmt(out, n, "no command %d", action); return -1; }
  {
    const char *st = api->share_status();
    api->fmt(out, n, "%s", st && st[0] ? st : "not shared -- share start opens it");
  }
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_NET,
  "Share",
  /* 16x16: a card with a wave leaving it. */
  { 0x00, 0x00, 0x3F, 0xC0, 0x20, 0x40, 0x2F, 0x40,
    0x20, 0x40, 0x2F, 0x40, 0x20, 0x40, 0x2F, 0x48,
    0x20, 0x44, 0x20, 0x52, 0x20, 0x4A, 0x20, 0x4A,
    0x3F, 0xD2, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00 },
  "space\tstop sharing, or start it again\n"
  "enter\tstart again after a failure or a stop\n"
  "leaving Share stops sharing too\n"
  "\n"
  "on the pc: map network drive to the url shown, no password.\n"
  "windows refuses files over 50 MB by default (WebClient\n"
  "FileSizeLimitInBytes in the registry). copying a folder\n"
  "within the drive is not supported; copy its files.\n",
  ACTIONS,
  sizeof ACTIONS / sizeof ACTIONS[0],
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&S, 0, sizeof S);
  /* Not for a command: sharing is on while Share is on screen, and a
   * headless instance is never on screen -- `status` only reads. */
  if (!api->headless()) start();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.actions = ACTIONS;
  UI.nactions = NACT;
  UI.action = app_action;
  UI.command = app_command;
  api->ui(&UI);
  return 0;
}
