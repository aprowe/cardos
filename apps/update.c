/* Update: the console's `update` command, with a screen.
 *
 * api->update_check() and api->update_apply_progress() (API 14, 41) are the
 * same manifest-check and download/apply logic the console's `update`
 * command calls (kernel/net/update.h, through cardapi.c) -- this app does
 * not duplicate any of it, only shows it.
 *
 * Open to a check (blocking, so "checking..." is shown first and the
 * reply repaints over it); r asks again. With something to install,
 * left/right choose apps, os or all and Enter runs it -- also blocking,
 * so each progress line is drawn the moment it arrives rather than
 * waited on, the same way the console's lines appear one at a time. apps
 * always goes in (the shared api has no apps-only/os-only split below
 * "install the firmware too or not"), so os and all both do both; the
 * console still has the finer three-way choice if that distinction
 * matters. Esc, once a run has finished, clears the result and goes back
 * to asking again.
 */
#include "kernel/app/capp.h"
#include "apps/footer.h"

static const CardApi *api;

#define TOP_H    20
#define ROW_H    15
#define MAX_ROWS 16
#define ROW_CHARS 40

#define CLR_BG    CAPP_RGB(16, 18, 24)
#define CLR_TEXT  CAPP_RGB(230, 234, 242)
#define CLR_DIM   CAPP_RGB(126, 136, 152)
#define CLR_BAD   CAPP_RGB(240, 110, 96)
#define CLR_GOOD  CAPP_RGB(120, 232, 140)

static const char *const CHOICE_NAME[3] = { "apps", "os", "all" };

static struct {
  char     diff[512];            /* "pinball, claude, firmware", or the reason it failed */
  int      n;                    /* count from update_check; negative is an error */
  int      checked;              /* a check has completed at least once */
  char     status[48];
  char     rows[MAX_ROWS][ROW_CHARS];
  int      nrows;
  int      choice;                /* into CHOICE_NAME: what Enter installs */
  int      done;                  /* a run just finished; U.result is why */
  int      result_bad;
  char     result[96];
  int      f_ui, f_uib;
  CRect    content;
} U;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  return r;
}

static void draw(int f, int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  if (f >= 0) api->text_font(f, (int16_t)x, (int16_t)y, s, fg, bg);
  else api->text((int16_t)x, (int16_t)y, s, fg, bg);
}

static int width(int f, const char *s) {
  return f >= 0 ? api->text_width(f, s) : (int)api->str_len(s) * 6;
}

static int height(int f) { return f >= 0 ? api->font_height(f) : 8; }

/* U.diff, "a, b, c", one name a row. */
static void split_rows(void) {
  const char *p = U.diff;
  U.nrows = 0;
  while (*p && U.nrows < MAX_ROWS) {
    int i = 0;
    while (*p && *p != ',' && i < ROW_CHARS - 1) U.rows[U.nrows][i++] = *p++;
    U.rows[U.nrows][i] = 0;
    U.nrows++;
    while (*p == ',' || *p == ' ') p++;
  }
}

static void run_check(void) {
  U.n = api->update_check(U.diff, sizeof U.diff);
  U.checked = 1;
  U.status[0] = 0;
  if (U.n < 0) { U.nrows = 0; return; }
  if (U.n == 0) { api->fmt(U.status, sizeof U.status, "current"); U.nrows = 0; return; }
  api->fmt(U.status, sizeof U.status, "%d to update", U.n);
  split_rows();
}

static void paint_top(CRect c) {
  api->fill(rect(c.x, c.y, c.w, TOP_H), CLR_BG);
  draw(U.f_uib, c.x + 8, c.y + (TOP_H - height(U.f_uib)) / 2, "Update", CLR_TEXT, CLR_BG);
  if (!U.checked) {
    draw(U.f_ui, c.x + 8 + width(U.f_uib, "Update") + 10, c.y + (TOP_H - height(U.f_ui)) / 2,
         "checking...", CLR_DIM, CLR_BG);
  } else if (U.status[0]) {
    int w = width(U.f_ui, U.status);
    draw(U.f_ui, c.x + c.w - 8 - w, c.y + (TOP_H - height(U.f_ui)) / 2, U.status,
         U.n == 0 ? CLR_GOOD : CLR_DIM, CLR_BG);
  }
}

static void paint_picker(CRect c, int y) {
  int i, x = c.x + 8;
  for (i = 0; i < 3; i++) {
    uint16_t fg = i == U.choice ? CLR_GOOD : CLR_DIM;
    draw(U.f_ui, x, y, CHOICE_NAME[i], fg, CLR_BG);
    x += width(U.f_ui, CHOICE_NAME[i]) + 14;
  }
}

static void paint_body(CRect c) {
  int y = c.y + TOP_H, bottom = c.y + c.h - FOOT_H, i;
  api->fill(rect(c.x, y, c.w, bottom - y), CLR_BG);
  if (U.done) {
    draw(U.f_ui, c.x + 8, y + 6, U.result, U.result_bad ? CLR_BAD : CLR_GOOD, CLR_BG);
    return;
  }
  if (!U.checked) return;
  if (U.n < 0) {
    draw(U.f_ui, c.x + 8, y + 6, U.diff, CLR_BAD, CLR_BG);
    return;
  }
  if (U.n == 0) {
    draw(U.f_ui, c.x + 8, y + 6, "everything is current", CLR_DIM, CLR_BG);
    return;
  }
  paint_picker(c, y + 6);
  y += ROW_H;
  for (i = 0; i < U.nrows && y + ROW_H <= bottom; i++, y += ROW_H)
    draw(U.f_ui, c.x + 8, y + (ROW_H - height(U.f_ui)) / 2, U.rows[i], CLR_TEXT, CLR_BG);
}

static const char *footer_hint(void) {
  if (U.done) return "esc back  r check again";
  if (U.checked && U.n > 0) return "enter run  left/right choose  r check again";
  return "r check again";
}

static void app_paint(void *st, CRect c) {
  (void)st;
  U.content = c;
  paint_top(c);
  paint_body(c);
  footer_paint(api, c, footer_hint());
}

static int app_tick(void *st, uint32_t now) {
  (void)st; (void)now;
  if (!U.checked) { run_check(); return 1; }
  return 0;
}

/* The body, showing one progress line -- drawn directly rather than left
 * for the next paint, because there is no next paint until the whole
 * install returns: update_apply_progress blocks for as long as it runs,
 * calling this from inside itself for every line, the same way the
 * console's own lines appear one at a time while it blocks too. */
static void paint_progress(const char *line) {
  CRect c = U.content;
  int y = c.y + TOP_H, bottom = c.y + c.h - FOOT_H;
  api->fill(rect(c.x, y, c.w, bottom - y), CLR_BG);
  draw(U.f_ui, c.x + 8, y + 6, line, CLR_TEXT, CLR_BG);
}

static void on_progress(void *ctx, const char *line) {
  (void)ctx;
  paint_progress(line);
}

static void run_update(void) {
  char out[96];
  int os = U.choice != 0;         /* apps=0; os and all both include apps too */
  int n;
  paint_progress("starting...");
  n = api->update_apply_progress(os, on_progress, NULL, out, sizeof out);
  /* Reached only when there was nothing to flash, apps alone was picked,
   * or it failed -- a firmware install that succeeds restarts the chip
   * instead of returning. */
  U.done = 1;
  U.result_bad = n < 0;
  if (n < 0) api->fmt(U.result, sizeof U.result, "%s", out);
  else api->fmt(U.result, sizeof U.result, "%d app%s installed%s%s", n, n == 1 ? "" : "s",
               out[0] ? ": " : "", out);
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (U.done) {
    if (k == CAPP_KEY_ESC || k == CAPP_KEY_BACK) { U.done = 0; U.checked = 0; return 1; }
    return 0;
  }
  switch (k) {
  case 'r': case 'R':
    U.checked = 0;              /* the next tick redoes it, "checking..." shown first */
    return 1;
  case CAPP_KEY_LEFT:
    if (!U.checked || U.n <= 0) return 0;
    U.choice = (U.choice + 2) % 3;
    return 1;
  case CAPP_KEY_RIGHT:
    if (!U.checked || U.n <= 0) return 0;
    U.choice = (U.choice + 1) % 3;
    return 1;
  case CAPP_KEY_ENTER:
    if (!U.checked || U.n <= 0) return 0;
    run_update();
    return 1;
  default:
    return 0;
  }
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Update",
  /* 16x16: an arrow down into a tray. */
  { 0x00, 0x00, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80,
    0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x07, 0xC0,
    0x0F, 0xE0, 0x1F, 0xF8, 0x0F, 0xE0, 0x07, 0xC0,
    0x03, 0x80, 0x01, 0x00, 0x00, 0x00, 0x7F, 0xFE },
  "enter\trun the highlighted install\n"
  "left/right\tchoose apps, os or all\n"
  "r\task the server again\n"
  "esc\tonce a run has finished, back to asking again\n"
  "\n"
  "the same check and install the console's `update` command runs.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&U, 0, sizeof U);
  U.choice = 2;                  /* all, the common case */
  U.f_ui = api->font_load("ui13");
  U.f_uib = api->font_load("ui13b");
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  api->ui(&UI);
  return 0;
}
