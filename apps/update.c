/* Update: what the server has that this device does not, and installing it.
 *
 * The check and the install are the kernel's (api->update_check and
 * api->update_apply_progress, kernel/net/update.c through cardapi.c) --
 * the same code the console's `update` runs. This app is the screen.
 *
 * Open: a check. Up to date says so with a tick; otherwise the list of what
 * differs -- each app, and the firmware -- and one or two buttons: Install
 * all (apps and the firmware) and, when the firmware is among them, Apps
 * only. Installing walks the list with a mark per item (waiting, going,
 * done, failed) and a bar: the apps one by one, then the firmware's
 * download and its write, each with its percentage. A firmware that
 * installs restarts the device into itself.
 *
 * Both calls block, so the screen is drawn from inside the install's
 * progress callback, a row and the bar at a time, each drawn over itself
 * with its own background -- nothing is cleared first, so nothing blinks.
 *
 * What the install says, and what it means here (kernel/net/update.c):
 *   "NAME.capp 1234 bytes"   NAME is in
 *   "NAME: ..."              NAME failed, and why
 *   "downloading 40%"        the firmware, coming down
 *   "N KB on the card, ..."  the firmware is down, being checked
 *   "writing 70%"            the firmware, going into its slot
 */
#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/footer.h"

static const CardApi *api;

#define HEAD_H    18
#define ROW_H     13
#define MAX_ITEMS 24
#define NAME_MAX  24
#define BTN_H     17

#define CLR_BG     CAPP_RGB(14, 16, 22)
#define CLR_HEAD   CAPP_RGB(22, 26, 36)
#define CLR_LINE   CAPP_RGB(40, 46, 60)
#define CLR_TEXT   CAPP_RGB(236, 240, 248)
#define CLR_DIM    CAPP_RGB(128, 136, 152)
#define CLR_FAINT  CAPP_RGB(70, 76, 92)
#define CLR_ACCENT CAPP_RGB(86, 158, 232)
#define CLR_GOOD   CAPP_RGB(110, 204, 128)
#define CLR_BAD    CAPP_RGB(232, 96, 86)
#define CLR_FW     CAPP_RGB(255, 196, 82)
#define CLR_TRACK  CAPP_RGB(34, 40, 54)

enum { S_CHECKING = 0, S_ERROR, S_CURRENT, S_READY, S_INSTALLING, S_DONE };
enum { I_WAIT = 0, I_GOING, I_DONE, I_FAILED };
enum { PH_APPS = 0, PH_DOWNLOAD, PH_VERIFY, PH_WRITE, PH_RESTART };

typedef struct {
  char name[NAME_MAX];
  int  firmware;
  int  state;
} Item;

static struct {
  int   state;
  Item  it[MAX_ITEMS];
  int   n, napps, has_fw;
  int   button;                  /* 0 install all, 1 apps only */
  int   with_fw;                 /* this run includes the firmware */
  int   phase, pct;              /* the firmware's part of a run */
  int   ok, failed;
  char  msg[96];                 /* an error, or a failed item's reason */
  char  host[40];
  int   top;                     /* the list's first row on screen */
  int   f_ui, f_uib;
  CRect c;
  char  diff[512];
  /* versions, from the console's `update version`: this firmware's, the one
   * the server offers, the server's own */
  char  run_v[40], off_v[40], srv_v[40];
} U;

static void draw(int f, int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  if (f >= 0) api->text_font(f, (int16_t)x, (int16_t)y, s, fg, bg);
  else api->text((int16_t)x, (int16_t)y, s, fg, bg);
}
static int width(int f, const char *s) { return f >= 0 ? api->text_width(f, s) : (int)api->str_len(s) * 6; }
static int height(int f) { return f >= 0 ? api->font_height(f) : 8; }

static int num(const char *s) {
  int v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v;
}

/* ---- little pictures, drawn as runs of pixels ------------------------------ */

/* A 9x9 picture: '#' is `fg`, anything else the background. */
static void glyph(int x, int y, const char *const rows[9], uint16_t fg, uint16_t bg) {
  int r, c;
  for (r = 0; r < 9; r++) {
    c = 0;
    while (c < 9) {
      int on = rows[r][c] == '#', s = c;
      while (c < 9 && (rows[r][c] == '#') == on) c++;
      api->fill(capp_rect(x + s, y + r, c - s, 1), on ? fg : bg);
    }
  }
}

static const char *const G_WAIT[9] = {
  ".........", "...###...", "..#...#..", ".#.....#.", ".#.....#.",
  ".#.....#.", "..#...#..", "...###...", ".........",
};
static const char *const G_GOING[9] = {
  ".........", "...###...", "..#####..", ".#######.", ".#######.",
  ".#######.", "..#####..", "...###...", ".........",
};
static const char *const G_DONE[9] = {
  ".........", ".......##", "......##.", ".....##..", "##..##...",
  ".####....", "..##.....", ".........", ".........",
};
static const char *const G_FAILED[9] = {
  ".........", ".##...##.", "..##.##..", "...###...", "...###...",
  "..##.##..", ".##...##.", ".........", ".........",
};

/* A badge for the big states: a filled 26x26 rounded square with a mark. */
static void badge(int x, int y, uint16_t colour, int kind) {
  int i;
  api->fill(capp_rect(x + 3, y, 20, 26), colour);
  api->fill(capp_rect(x, y + 3, 26, 20), colour);
  api->fill(capp_rect(x + 1, y + 1, 24, 24), colour);
  if (kind == 0) {                           /* a tick */
    for (i = 0; i < 5; i++) api->fill(capp_rect(x + 6 + i, y + 12 + i, 3, 3), CLR_BG);
    for (i = 0; i < 9; i++) api->fill(capp_rect(x + 11 + i, y + 16 - i, 3, 3), CLR_BG);
  } else if (kind == 1) {                    /* a ! */
    api->fill(capp_rect(x + 11, y + 5, 4, 11), CLR_BG);
    api->fill(capp_rect(x + 11, y + 18, 4, 3), CLR_BG);
  } else {                                   /* an arrow down: an update */
    api->fill(capp_rect(x + 11, y + 4, 4, 12), CLR_BG);
    for (i = 0; i < 6; i++) api->fill(capp_rect(x + 7 + i, y + 13 + i, 12 - 2 * i, 1), CLR_BG);
    api->fill(capp_rect(x + 6, y + 21, 14, 2), CLR_BG);
  }
}

/* ---- the check ------------------------------------------------------------------- */

static void parse_diff(void) {
  const char *p = U.diff;
  U.n = U.napps = U.has_fw = 0;
  while (*p && U.n < MAX_ITEMS) {
    Item *it = &U.it[U.n];
    int k = 0;
    while (*p && *p != ',' && k < NAME_MAX - 1) it->name[k++] = *p++;
    it->name[k] = 0;
    while (*p && *p != ',') p++;             /* a name too long: cut */
    while (*p == ',' || *p == ' ') p++;
    if (!k) continue;
    it->firmware = str_starts(it->name, "firmware") && !it->name[8];
    it->state = I_WAIT;
    if (it->firmware) U.has_fw = 1; else U.napps++;
    U.n++;
  }
  /* The firmware last: it is installed last, and restarts the device. */
}

/* "running   V (flavor)" / "offered   V (flavor)..." / "server    V":
 * the word after the label, into out. */
static void version_of(const char *text, const char *label, char *out, int n) {
  const char *p = text;
  int k = 0;
  out[0] = 0;
  while (*p) {
    if (str_starts(p, label)) {
      p += api->str_len(label);
      while (*p == ' ') p++;
      while (*p && *p != ' ' && *p != '\n' && k < n - 1) out[k++] = *p++;
      out[k] = 0;
      return;
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
}

static void fetch_versions(void) {
  static char out[256];
  out[0] = 0;
  if (api->shell("update version", out, sizeof out) < 0) out[0] = 0;
  version_of(out, "running", U.run_v, sizeof U.run_v);
  version_of(out, "offered", U.off_v, sizeof U.off_v);
  version_of(out, "server", U.srv_v, sizeof U.srv_v);
}

/* Both versions, along the bottom of the body: what this is, and what the
 * server has. */
static void paint_versions(void) {
  char b[48];
  int y = U.c.y + U.c.h - FOOT_H - 22;
  if (!U.run_v[0]) return;
  api->fmt(b, sizeof b, "this device  %s", U.run_v);
  draw(-1, U.c.x + 8, y, b, CLR_FAINT, CLR_BG);
  api->fmt(b, sizeof b, "the server   %s", U.off_v[0] ? U.off_v : (U.srv_v[0] ? U.srv_v : "?"));
  draw(-1, U.c.x + 8, y + 10, b, CLR_FAINT, CLR_BG);
}

static void run_check(void) {
  int n = api->update_check(U.diff, sizeof U.diff);
  U.top = 0;
  U.ok = U.failed = 0;
  if (n < 0) {
    U.state = S_ERROR;
    api->fmt(U.msg, sizeof U.msg, "%s", U.diff);
    return;
  }
  fetch_versions();
  if (n == 0) { U.state = S_CURRENT; U.n = 0; return; }
  parse_diff();
  U.state = S_READY;
  U.button = 0;
}

/* ---- painting ------------------------------------------------------------------- */

static int list_y(void) { return U.c.y + HEAD_H + 22; }
static int list_bottom(void) {
  int b = U.c.y + U.c.h - FOOT_H - 4;
  return U.state == S_READY ? b - BTN_H - 4 : U.state == S_INSTALLING ? b - 22 : b;
}
static int list_rows(void) { int r = (list_bottom() - list_y()) / ROW_H; return r > 0 ? r : 0; }

static void paint_head(void) {
  CRect c = U.c;
  api->fill(capp_rect(c.x, c.y, c.w, HEAD_H - 1), CLR_HEAD);
  api->fill(capp_rect(c.x, c.y + HEAD_H - 1, c.w, 1), CLR_LINE);
  draw(U.f_uib, c.x + 8, c.y + (HEAD_H - 1 - height(U.f_uib)) / 2, "Update", CLR_TEXT, CLR_HEAD);
  draw(U.f_ui, c.x + c.w - 8 - width(U.f_ui, U.host), c.y + (HEAD_H - 1 - height(U.f_ui)) / 2,
       U.host, CLR_FAINT, CLR_HEAD);
}

static void paint_row(int i) {
  const Item *it = &U.it[i];
  int y = list_y() + (i - U.top) * ROW_H, x = U.c.x + 10;
  const char *const *g = it->state == I_DONE ? G_DONE : it->state == I_FAILED ? G_FAILED :
                         it->state == I_GOING ? G_GOING : G_WAIT;
  uint16_t gc = it->state == I_DONE ? CLR_GOOD : it->state == I_FAILED ? CLR_BAD :
                it->state == I_GOING ? CLR_ACCENT : CLR_FAINT;
  char label[48];
  int ty = y + (ROW_H - height(U.f_ui)) / 2, w;
  if (i < U.top || i >= U.top + list_rows()) return;
  api->fill(capp_rect(U.c.x, y, 10, ROW_H), CLR_BG);
  glyph(x, y + (ROW_H - 9) / 2, g, gc, CLR_BG);
  api->fill(capp_rect(x + 9, y, 6, ROW_H), CLR_BG);
  if (it->firmware) api->fmt(label, sizeof label, U.off_v[0] ? "CardOS %s" : "CardOS firmware",
                              U.off_v);
  else api->fmt(label, sizeof label, "%s", it->name);
  draw(U.f_ui, x + 15, ty, label, it->state == I_WAIT ? CLR_DIM : CLR_TEXT, CLR_BG);
  w = width(U.f_ui, label);
  /* the rest of the row, and a tag on the right */
  {
    const char *tag = it->firmware ? "system" : "app";
    int tw = width(U.f_ui, tag), tx = U.c.x + U.c.w - 10 - tw;
    api->fill(capp_rect(x + 15 + w, y, tx - (x + 15 + w), ROW_H), CLR_BG);
    draw(U.f_ui, tx, ty, tag, it->firmware ? CLR_FW : CLR_FAINT, CLR_BG);
    api->fill(capp_rect(tx + tw, y, U.c.x + U.c.w - (tx + tw), ROW_H), CLR_BG);
  }
  /* a hair under each row but the last */
  if (i < U.n - 1) api->fill(capp_rect(x + 15, y + ROW_H - 1, U.c.w - 35, 1), CLR_HEAD);
}

static void paint_list(void) {
  int i, rows = list_rows(), y;
  for (i = U.top; i < U.n && i < U.top + rows; i++) paint_row(i);
  y = list_y() + (i - U.top) * ROW_H;
  if (y < list_bottom()) api->fill(capp_rect(U.c.x, y, U.c.w, list_bottom() - y), CLR_BG);
  if (U.n > U.top + rows) {
    char more[24];
    api->fmt(more, sizeof more, "+%d more", U.n - U.top - rows);
    draw(U.f_ui, U.c.x + U.c.w - 10 - width(U.f_ui, more), list_bottom() - height(U.f_ui), more,
         CLR_FAINT, CLR_BG);
  }
}

static void paint_summary(const char *title, const char *sub, uint16_t sub_colour) {
  int y = U.c.y + HEAD_H + 4, x = U.c.x + 10;
  api->fill(capp_rect(U.c.x, U.c.y + HEAD_H, U.c.w, 22), CLR_BG);
  draw(U.f_uib, x, y, title, CLR_TEXT, CLR_BG);
  if (sub) draw(U.f_ui, x + width(U.f_uib, title) + 8, y + height(U.f_uib) - height(U.f_ui), sub,
                sub_colour, CLR_BG);
}

static void paint_button(int x, int y, int w, const char *label, int selected) {
  uint16_t fill = selected ? CLR_ACCENT : CLR_BG, edge = selected ? CLR_ACCENT : CLR_LINE;
  int tw = width(U.f_uib, label);
  api->fill(capp_rect(x + 2, y, w - 4, 1), edge);
  api->fill(capp_rect(x + 2, y + BTN_H - 1, w - 4, 1), edge);
  api->fill(capp_rect(x, y + 2, 1, BTN_H - 4), edge);
  api->fill(capp_rect(x + w - 1, y + 2, 1, BTN_H - 4), edge);
  api->fill(capp_rect(x + 1, y + 1, 1, 1), edge);
  api->fill(capp_rect(x + w - 2, y + 1, 1, 1), edge);
  api->fill(capp_rect(x + 1, y + BTN_H - 2, 1, 1), edge);
  api->fill(capp_rect(x + w - 2, y + BTN_H - 2, 1, 1), edge);
  api->fill(capp_rect(x + 1, y + 1, (w - tw) / 2 - 1, BTN_H - 2), fill);
  api->fill(capp_rect(x + (w + tw) / 2, y + 1, w - 1 - (w + tw) / 2, BTN_H - 2), fill);
  api->fill(capp_rect(x + (w - tw) / 2, y + 1, tw, (BTN_H - height(U.f_uib)) / 2 - 1), fill);
  draw(U.f_uib, x + (w - tw) / 2, y + (BTN_H - height(U.f_uib)) / 2, label,
       selected ? CLR_BG : CLR_DIM, fill);
  api->fill(capp_rect(x + (w - tw) / 2, y + (BTN_H + height(U.f_uib)) / 2, tw,
                      BTN_H - 1 - (BTN_H + height(U.f_uib)) / 2), fill);
}

static void paint_buttons(void) {
  int y = U.c.y + U.c.h - FOOT_H - 4 - BTN_H, x = U.c.x + 10, w = U.c.w - 20;
  api->fill(capp_rect(U.c.x, y - 4, U.c.w, BTN_H + 8), CLR_BG);
  if (!U.has_fw || !U.napps) {
    paint_button(x, y, w, U.has_fw ? "Install firmware" : "Install", 1);
    return;
  }
  paint_button(x, y, (w - 6) / 2, "Install all", U.button == 0);
  paint_button(x + (w - 6) / 2 + 6, y, (w - 6) / 2, "Apps only", U.button == 1);
}

/* The bar under the list while it runs: what is happening, and how far. */
static void paint_bar(void) {
  int y = U.c.y + U.c.h - FOOT_H - 22, x = U.c.x + 10, w = U.c.w - 20, fill, total;
  char what[48], right[16];
  if (U.phase == PH_APPS) {
    total = U.napps ? U.napps : 1;
    fill = (U.ok + U.failed) * w / total;
    api->fmt(what, sizeof what, "Installing apps");
    api->fmt(right, sizeof right, "%d of %d", U.ok + U.failed, U.napps);
  } else {
    fill = U.pct * w / 100;
    api->fmt(what, sizeof what, "%s", U.phase == PH_DOWNLOAD ? "Downloading firmware" :
             U.phase == PH_VERIFY ? "Checking firmware" :
             U.phase == PH_WRITE ? "Writing firmware" : "Restarting into the new firmware");
    api->fmt(right, sizeof right, U.phase == PH_RESTART ? "" : "%d%%", U.pct);
  }
  if (fill > w) fill = w;
  api->fill(capp_rect(U.c.x, y - 2, U.c.w, 22), CLR_BG);
  draw(U.f_ui, x, y, what, CLR_TEXT, CLR_BG);
  draw(U.f_ui, x + w - width(U.f_ui, right), y, right, CLR_DIM, CLR_BG);
  api->fill(capp_rect(x, y + 13, fill, 5), U.phase == PH_APPS ? CLR_ACCENT : CLR_FW);
  api->fill(capp_rect(x + fill, y + 13, w - fill, 5), CLR_TRACK);
}

static void paint_center(int kind, uint16_t colour, const char *title, const char *sub) {
  int y = U.c.y + HEAD_H + 22, x = U.c.x + (U.c.w - 26) / 2;
  api->fill(capp_rect(U.c.x, U.c.y + HEAD_H, U.c.w, U.c.h - HEAD_H - FOOT_H), CLR_BG);
  if (kind >= 0) badge(x, y, colour, kind);
  y += 34;
  draw(U.f_uib, U.c.x + (U.c.w - width(U.f_uib, title)) / 2, y, title, CLR_TEXT, CLR_BG);
  if (sub) {
    /* one or two lines, broken at a space */
    char a[64], b[64];
    int len = (int)api->str_len(sub), cut = len, k;
    if (width(U.f_ui, sub) > U.c.w - 16)
      for (k = len / 2; k > 0; k--) if (sub[k] == ' ') { cut = k; break; }
    api->fmt(a, sizeof a, "%.*s", cut, sub);
    api->fmt(b, sizeof b, "%s", cut < len ? sub + cut + 1 : "");
    draw(U.f_ui, U.c.x + (U.c.w - width(U.f_ui, a)) / 2, y + 16, a, CLR_DIM, CLR_BG);
    if (b[0]) draw(U.f_ui, U.c.x + (U.c.w - width(U.f_ui, b)) / 2, y + 29, b, CLR_DIM, CLR_BG);
  }
}

static void app_paint(void *st, CRect c) {
  char sub[48];
  (void)st;
  U.c = c;
  paint_head();
  switch (U.state) {
  case S_CHECKING:
    paint_center(2, CLR_ACCENT, "Checking for updates", "asking the server what it has");
    footer_paint(api, c, 0);
    return;
  case S_ERROR:
    paint_center(1, CLR_BAD, "Could not check", U.msg);
    footer_paint(api, c, "r try again");
    return;
  case S_CURRENT:
    paint_center(0, CLR_GOOD, "Up to date", "the firmware and every app match the server");
    paint_versions();
    footer_paint(api, c, "r check again");
    return;
  case S_READY:
    api->fmt(sub, sizeof sub, "%d app%s%s", U.napps, U.napps == 1 ? "" : "s",
             U.has_fw ? " and the firmware" : "");
    if (!U.napps) api->fmt(sub, sizeof sub, "the firmware");
    {
      char t[16];
      api->fmt(t, sizeof t, U.n == 1 ? "1 update" : "%d updates", U.n);
      paint_summary(t, sub, CLR_DIM);
    }
    paint_list();
    paint_buttons();
    footer_paint(api, c, U.has_fw && U.napps ? "enter install  < > choose  r recheck"
                                             : "enter install  r recheck");
    return;
  case S_INSTALLING:
    paint_summary("Installing", U.with_fw ? "don't switch off" : 0, CLR_FW);
    paint_list();
    paint_bar();
    footer_paint(api, c, 0);
    return;
  case S_DONE:
    if (U.failed) api->fmt(sub, sizeof sub, "%d failed", U.failed);
    paint_summary(U.failed ? (U.ok ? "Partly installed" : "Not installed") : "Installed",
                  U.failed ? sub : 0, CLR_BAD);
    paint_list();
    if (U.failed && U.msg[0]) {
      int y = U.c.y + U.c.h - FOOT_H - 12;
      api->fill(capp_rect(U.c.x, y - 2, U.c.w, 12), CLR_BG);
      draw(U.f_ui, U.c.x + 10, y, U.msg, CLR_BAD, CLR_BG);
    }
    footer_paint(api, c, "r check again");
    return;
  }
}

/* ---- installing -------------------------------------------------------------- */

static int find_item(const char *name, size_t len) {
  int i;
  for (i = 0; i < U.n; i++)
    if (!U.it[i].firmware && api->str_len(U.it[i].name) == len) {
      size_t k;
      for (k = 0; k < len && U.it[i].name[k] == name[k]; k++) {}
      if (k == len) return i;
    }
  return -1;
}

/* The next app waiting is the one going now: the install walks the list in
 * the order the check gave it. Its row is drawn, and the list scrolls to it. */
static void mark_next_going(void) {
  int i;
  for (i = 0; i < U.n; i++)
    if (!U.it[i].firmware && U.it[i].state == I_WAIT) {
      U.it[i].state = I_GOING;
      if (i >= U.top + list_rows()) { U.top = i - list_rows() + 1; paint_list(); }
      else paint_row(i);
      return;
    }
}

static int fw_index(void) {
  int i;
  for (i = 0; i < U.n; i++) if (U.it[i].firmware) return i;
  return -1;
}

static void fw_state(int s) {
  int f = fw_index();
  if (f < 0) return;
  U.it[f].state = s;
  if (f >= U.top + list_rows()) { U.top = f - list_rows() + 1; paint_list(); }
  else paint_row(f);
}

static void on_line(void *ctx, const char *line) {
  const char *colon = 0, *p;
  (void)ctx;
  if (str_starts(line, "downloading ")) {
    if (U.phase == PH_APPS) { U.phase = PH_DOWNLOAD; fw_state(I_GOING); }
    U.pct = num(line + 12);
  } else if (line[0] >= '0' && line[0] <= '9') {
    U.phase = PH_VERIFY;                     /* "N KB on the card, flashing" */
    U.pct = 100;
  } else if (str_starts(line, "writing ")) {
    U.phase = PH_WRITE;
    U.pct = num(line + 8);
    if (U.pct >= 100) { U.phase = PH_RESTART; fw_state(I_DONE); }
  } else {
    for (p = line; *p; p++) if (*p == ':' || *p == ' ') { colon = p; break; }
    if (colon && *colon == ':') {            /* "NAME: why" */
      int i = find_item(line, (size_t)(colon - line));
      if (i >= 0) { U.it[i].state = I_FAILED; paint_row(i); U.failed++; }
      api->fmt(U.msg, sizeof U.msg, "%s", line);
      mark_next_going();
    } else {
      /* "NAME.capp 1234 bytes" */
      for (p = line; *p && *p != '.' && *p != ' '; p++) {}
      if (*p == '.') {
        int i = find_item(line, (size_t)(p - line));
        if (i >= 0) { U.it[i].state = I_DONE; paint_row(i); U.ok++; }
        mark_next_going();
      }
    }
  }
  paint_bar();
}

static void run_install(void) {
  char out[96];
  int n;
  U.with_fw = U.has_fw && (U.button == 0 || !U.napps);
  U.state = S_INSTALLING;
  U.phase = PH_APPS;
  U.pct = 0;
  U.ok = U.failed = 0;
  U.msg[0] = 0;
  U.top = 0;
  /* Drawn now: there is no paint until the install returns. */
  app_paint(0, U.c);
  mark_next_going();
  n = api->update_apply_progress(U.with_fw, on_line, 0, out, sizeof out);
  /* Back only without a firmware, or when it failed: a firmware that went
   * in has restarted the device. */
  if (n < 0) {
    if (U.with_fw) fw_state(I_FAILED);
    U.failed++;
    api->fmt(U.msg, sizeof U.msg, "%s", out[0] ? out : "the install failed");
  }
  U.state = S_DONE;
}

/* ---- keys ------------------------------------------------------------------------- */

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (U.state == S_CHECKING || U.state == S_INSTALLING) return 1;
  if (k == 'r' || k == 'R') { U.state = S_CHECKING; return 1; }
  if (U.state != S_READY) return 0;
  switch (k) {
  case CAPP_KEY_LEFT: case CAPP_KEY_RIGHT: case '\t':
    /* Only what changed is drawn again: the buttons, or the list. */
    if (U.has_fw && U.napps) U.button ^= 1;
    api->damage(capp_rect(U.c.x, U.c.y + U.c.h - FOOT_H - 8 - BTN_H, U.c.w, BTN_H + 8));
    return 1;
  case CAPP_KEY_UP:
    if (U.top > 0) U.top--;
    api->damage(capp_rect(U.c.x, list_y(), U.c.w, list_bottom() - list_y()));
    return 1;
  case CAPP_KEY_DOWN:
    if (U.top + list_rows() < U.n) U.top++;
    api->damage(capp_rect(U.c.x, list_y(), U.c.w, list_bottom() - list_y()));
    return 1;
  case CAPP_KEY_ENTER: case ' ':
    run_install();
    return 1;
  default:
    return 0;
  }
}

/* The check blocks, so it runs on the tick after "Checking" has been drawn. */
static int app_tick(void *st, uint32_t now) {
  static int painted;
  (void)st; (void)now;
  if (U.state != S_CHECKING) { painted = 0; return 0; }
  if (!painted) { painted = 1; return 1; }   /* draw "Checking" first */
  painted = 0;
  run_check();
  return 1;
}

static void set_host(void) {
  const char *p = api->proxy();
  int k = 0;
  if (str_starts(p, "http://")) p += 7;
  else if (str_starts(p, "https://")) p += 8;
  while (*p && *p != '/' && k < (int)sizeof U.host - 1) U.host[k++] = *p++;
  U.host[k] = 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Update",
  /* 16x16: a rounded badge with an arrow down into a tray. */
  { 0x00, 0x00, 0x3F, 0xFC, 0x40, 0x02, 0x41, 0x82,
    0x41, 0x82, 0x41, 0x82, 0x47, 0xE2, 0x43, 0xC2,
    0x41, 0x82, 0x48, 0x12, 0x4F, 0xF2, 0x40, 0x02,
    0x40, 0x02, 0x3F, 0xFC, 0x00, 0x00, 0x00, 0x00 },
  "enter\tinstall\nleft/right\tinstall all, or apps only\nup/down\tscroll the list\n"
  "r\tcheck again\n"
  "\n"
  "the same check and install as the console's `update`.\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&U, 0, sizeof U);
  U.f_ui = api->font_load("ui13");
  U.f_uib = api->font_load("ui13b");
  set_host();
  U.state = S_CHECKING;
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  api->ui(&UI);
  return 0;
}
