/* Share -- the card as a network drive, while this is open.
 *
 * The server is the kernel's (kernel/net/share.c); this is the switch and
 * the readout. It starts sharing when it opens and the kernel stops sharing
 * when it is closed, which is the whole design: a thing you can see is on.
 * The console's `share` command outlives the command instead, for the
 * development loop.
 */

#include "kernel/app/capp.h"

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

static void app_paint(void *st, CRect c) {
  int i, y;
  (void)st;
  api->fill(c, CLR_BG);
  api->text((short)(c.x + 8), (short)(c.y + 6), "Share", CLR_FG, CLR_BG);
  if (S.on) {
    api->text((short)(c.x + 8), (short)(c.y + 20), "the card is on the network at",
              CLR_DIM, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 32), S.status, CLR_URL, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 46),
              "map it as a drive on the pc. esc stops", CLR_DIM, CLR_BG);
  } else {
    api->text((short)(c.x + 8), (short)(c.y + 20), "not sharing:", CLR_DIM, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 32), S.status, CLR_BAD, CLR_BG);
    api->text((short)(c.x + 8), (short)(c.y + 46), "enter to try again", CLR_DIM, CLR_BG);
  }
  y = c.y + 62;
  for (i = 0; i < S.nlog; i++, y += 10)
    api->text((short)(c.x + 8), (short)y, S.log[i], CLR_FG, CLR_BG);
  {
    char bar[48];
    api->fmt(bar, sizeof bar, "%lu requests", (unsigned long)S.count);
    api->text((short)(c.x + 8), (short)(c.y + c.h - 10), bar, CLR_DIM, CLR_BG);
  }
}

static int app_tick(void *st, uint32_t now) {
  const char *l;
  int changed = 0;
  (void)st; (void)now;
  while ((l = api->share_take_log()) != NULL) {
    if (S.nlog == LINES) {
      api->mem_move(S.log[0], S.log[1], sizeof S.log - sizeof S.log[0]);
      S.nlog--;
    }
    api->fmt(S.log[S.nlog++], LINE_MAX, "%s", l);
    S.count++;
    changed = 1;
  }
  return changed;
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if ((k == CAPP_KEY_ENTER || k == ' ') && !S.on) { start(); return 1; }
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
  "fn-`\tstop sharing and leave\n"
  "enter\ttry again after a failure\n"
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
