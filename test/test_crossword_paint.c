/* The real Crossword app (apps/crossword/crossword.c) driven by keys, ticks
 * and a scripted server, and painted into a framebuffer with the real fonts
 * (test/fakescreen.h).
 *
 * Three things are checked on every screen the app has -- the list, the grid,
 * the results, the statistics:
 *
 *   - a whole paint writes every pixel (there is no framebuffer on the
 *     device: a pixel nobody writes is whatever was there before);
 *   - after each handler that said it changed something, painting only what
 *     the app marked as damaged (api->damage) leaves exactly the screen a
 *     whole paint makes -- a square or a row the app forgot to mark is a stale
 *     one on the panel, and this finds it. The shell paints a whole repaint
 *     one 16-row strip at a time and a damaged one in a piece, and so do we,
 *     which also shows a paint that changes state or reads the clock;
 *   - the things the screens are for: a weekday coloured, a cursor square, a
 *     number, a ring, a wrong letter in red, a check where a puzzle is done.
 *
 * The server is scripted (XN below): a list reply, a puzzle for a download, or
 * a failure. The card is fakeapi's in-memory one, so a puzzle's file, the
 * card's index and the progress files are looked at as they are written.
 *
 * CROSSWORD_DUMP=dir writes the screens there (crossword_*.ppm), to look at. */
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakescreen.h"

#define capp_info crossword_capp_info
#define capp_main crossword_capp_main
#include "apps/crossword/crossword.c"
#undef capp_info
#undef capp_main

static CardApi XA;
static const CRect X_ALL = { 0, 0, FS_W, FS_H };

#define X_PUZ  PUZ_DIR "/1993-11-21.xw"
#define X_SAV  SAVE_DIR "/1993-11-21.sav"

/* ---- the fixtures -------------------------------------------------------------- */

/* The library as the server lists it, newest first (and as the card's index
 * is written). 1993-11-21 was a Sunday. */
static const char X_LIST[] =
  "1993-11-21\t1993-11-21\tSun\t5x5\tMini Heart\n"
  "1993-11-20\t1993-11-20\tSat\t15x15\tSaturday Stumper\n"
  "1993-11-19\t1993-11-19\tFri\t15x15\tFriday Crossing\n"
  "1993-11-18\t1993-11-18\tThu\t15x15\n"
  "1993-11-17\t1993-11-17\tWed\t15x15\tMidweek Mix\n"
  "1993-11-16\t1993-11-16\tTue\t15x15\tA Very Long Title That Will Have To Be Cut Short\n"
  "1993-11-15\t1993-11-15\tMon\t15x15\tMonday Warm-up\n"
  "1993-11-14\t1993-11-14\tSun\t21x21\tSunday Best\n"
  "1993-11-13\t1993-11-13\tSat\t15x15\tWeekend Opener\n";

static const char X_MINI_LINE[] = "1993-11-21\t1993-11-21\tSun\t5x5\tMini Heart\n";

/* A 5x5 word square with a black square in two corners and a circled U:
 *
 *      0 1 2 3 4         across: 1 HEAR  5 EMBER  7 ABUSE  8 RESIN  9 REND
 *   0  H E A R .         down:   1 HEAR  2 EMBER  3 ABUSE  4 RESIN  6 REND
 *   1  E M B E R
 *   2  A B u S E
 *   3  R E S I N
 *   4  . R E N D                                                          */
#define X_MINI_TEXT \
  "xw 1\ntitle Mini Heart\nby A. Setter\ndate 1993-11-21\nnote Circled letter: U\n" \
  "size 5 5\nrow HEAR.\nrow EMBER\nrow ABuSE\nrow RESIN\nrow .REND\n" \
  "A 1 Catch with the ears\nA 5 Smoldering bit\nA 7 Mistreat\nA 8 Pine product\nA 9 Tear, as a seam\n" \
  "D 1 Listen up\nD 2 Fire remnant\nD 3 Misuse\nD 4 Sticky stuff\nD 6 Rip apart\n" \
  "end\n"
static const char X_MINI[] = X_MINI_TEXT;

#define X_LONG_CLUE \
  "A rather long clue that goes on and on well past the width of the little clue bar at the foot of the grid"

/* A 21x21 grid with no black square, every clue long: 21 across, 21 down,
 * numbered 1-21 along the top and 22-41 down the left side. */
static char X_BIG[12000];
/* An n by n grid of A's, every clue the long one. */
static void x_build_sq(int n) {
  int r, c, at = 0;
  at += sprintf(X_BIG + at, "xw 1\ntitle Big Sunday\ndate 1993-11-14\nsize %d %d\n", n, n);
  for (r = 0; r < n; r++) {
    at += sprintf(X_BIG + at, "row ");
    for (c = 0; c < n; c++) X_BIG[at++] = 'A';
    X_BIG[at++] = '\n';
  }
  for (r = 0; r < n; r++) at += sprintf(X_BIG + at, "A %d %s\n", r ? n + r : 1, X_LONG_CLUE);
  for (c = 0; c < n; c++) at += sprintf(X_BIG + at, "D %d %s\n", c + 1, X_LONG_CLUE);
  at += sprintf(X_BIG + at, "end\n");
  X_BIG[at] = 0;
}

static void x_build_big(void) {
  x_build_sq(21);
}

/* A full page of the list: forty puzzles, December 28 back to November 17
 * (the days are made up to be in order, not to be a calendar). */
static char X_PAGE[4000];
static void x_build_page(void) {
  int i, at = 0;
  for (i = 0; i < 40; i++)
    at += sprintf(X_PAGE + at, "1993-%02d-%02d\t1993-%02d-%02d\tMon\t15x15\tPuzzle %d\n",
                  12 - i / 28, 28 - i % 28, 12 - i / 28, 28 - i % 28, i);
}

/* ---- the server, scripted ------------------------------------------------------ */

static struct {
  const char *list;          /* what a list request answers; NULL fails it with `fail` */
  int         fail;
  int         wait;          /* polls that say "pending" before the answer */
  int         busy;          /* a request is out */
  int         starts;
  char        url[300];      /* the last list request */
  const char *puzzle;        /* what a download writes; NULL answers 404 */
  int         dl_fail;       /* a download that fails with this */
  int         downloads;
  char        dl_url[300];
} XN;

static int xn_start(const char *m, const char *u, const char *b, const char *ct, const char *br, int ms) {
  (void)m; (void)b; (void)ct; (void)br; (void)ms;
  if (XN.busy) return -1;
  XN.busy = 1;
  XN.starts++;
  snprintf(XN.url, sizeof XN.url, "%s", u);
  return 0;
}

static int xn_poll(char *out, size_t n) {
  size_t k;
  if (n) out[0] = 0;
  if (!XN.busy || !n) return -1;
  if (XN.wait > 0) { XN.wait--; return CAPP_HTTP_PENDING; }
  XN.busy = 0;
  if (!XN.list) return XN.fail ? XN.fail : -3;
  k = strlen(XN.list);
  if (k > n - 1) k = n - 1;
  memcpy(out, XN.list, k);
  out[k] = 0;
  return (int)k;
}

static int xn_download(const char *url, const char *path, int ms) {
  (void)ms;
  XN.downloads++;
  snprintf(XN.dl_url, sizeof XN.dl_url, "%s", url);
  if (XN.dl_fail) return XN.dl_fail;
  if (!XN.puzzle) return -404;
  fakefs_put(path, XN.puzzle);
  return (int)strlen(XN.puzzle);
}

static int xn_caps_none(void) { return 0; }
static int xn_zero(void) { return 0; }
static int xn_connect_fails(int ms) { (void)ms; return -1; }

/* ---- the screen, and what the app marked ----------------------------------------- */

static CRect X_DMG;
static int   X_NDMG;
static int   X_BAD;          /* repaints of the damage that differed from a whole paint */
static int   X_UNSET;        /* whole paints that left a pixel unwritten */

static void x_damage(CRect r) {
  if (!X_NDMG) {
    X_DMG = r;
  } else {
    int x0 = r.x < X_DMG.x ? r.x : X_DMG.x, y0 = r.y < X_DMG.y ? r.y : X_DMG.y;
    int x1 = r.x + r.w > X_DMG.x + X_DMG.w ? r.x + r.w : X_DMG.x + X_DMG.w;
    int y1 = r.y + r.h > X_DMG.y + X_DMG.h ? r.y + r.h : X_DMG.y + X_DMG.h;
    X_DMG = capp_rect(x0, y0, x1 - x0, y1 - y0);
  }
  X_NDMG++;
}

/* The whole screen, in one paint. */
static void x_full(void) {
  fakeapi_screen = X_ALL;
  fakescreen_paint();
  if (fakescreen_unset()) X_UNSET++;
}

/* Paint `clip` over the screen as it is, as the shell does: cut into strips
 * `step` rows high (all of it in one when step is 0), the app painted whole
 * for each with paint_area saying which, and only the strip's pixels taken. */
static void x_paint_clip(CRect clip, int step) {
  static uint16_t acc[FS_H][FS_W];
  int y;
  memcpy(acc, fakescreen_fb, sizeof acc);
  for (y = clip.y; y < clip.y + clip.h; y += step ? step : clip.h) {
    int hh = step ? step : clip.h, px, py;
    CRect s;
    if (y + hh > clip.y + clip.h) hh = clip.y + clip.h - y;
    s = capp_rect(clip.x, y, clip.w, hh);
    memcpy(fakescreen_fb, acc, sizeof acc);
    fakeapi_screen = s;
    fakescreen_ui.paint(fakescreen_ui.state, X_ALL);
    for (py = s.y; py < s.y + s.h; py++)
      for (px = s.x; px < s.x + s.w; px++)
        if (px >= 0 && py >= 0 && px < FS_W && py < FS_H) acc[py][px] = fakescreen_fb[py][px];
  }
  fakeapi_screen = X_ALL;
  memcpy(fakescreen_fb, acc, sizeof acc);
}

/* What the shell does after a handler that said it changed something: paint
 * what was marked (all of it, if nothing was) over the last screen, and set a
 * whole paint beside it. They must be the same screen. The screen is left as
 * the whole paint made it, so one mistake is not counted again and again. */
static void x_repaint(void) {
  static uint16_t got[FS_H][FS_W];
  CRect clip = X_NDMG ? X_DMG : X_ALL;
  int whole = clip.x <= 0 && clip.y <= 0 && clip.x + clip.w >= FS_W && clip.y + clip.h >= FS_H;
  int x, y, bad = 0, bx = 0, by = 0;
  x_paint_clip(clip, whole ? 16 : 0);
  memcpy(got, fakescreen_fb, sizeof got);
  x_full();
  for (y = 0; y < FS_H; y++)
    for (x = 0; x < FS_W; x++)
      if (!bad && got[y][x] != fakescreen_fb[y][x]) { bad = 1; bx = x; by = y; }
  if (bad) {
    if (!X_BAD)
      printf("  crossword: a repaint of %d,%d %dx%d differs from a whole paint, first at %d,%d\n",
             clip.x, clip.y, clip.w, clip.h, bx, by);
    X_BAD++;
  }
  X_NDMG = 0;
}

static void x_init(void) {
  fakeapi_init(&XA);
  fakefs_mem(&XA);
  fakescreen_install(&XA);
  memset(&XN, 0, sizeof XN);
  XA.http_start = xn_start;
  XA.http_poll = xn_poll;
  XA.http_download = xn_download;
  XA.damage = x_damage;
  fakeapi_screen = X_ALL;
  X_NDMG = X_BAD = X_UNSET = 0;
  fakefs_put(VIEW_PATH, "big\n");          /* these tests measure the big view; the side view has its own */
}

/* The app opened, and painted once. Files and the script are put in place first. */
static void x_start(void) {
  crossword_capp_main(&XA, 0, 0);
  X_NDMG = 0;
  x_full();
}

static int x_key(int k) {
  int handled;
  X_NDMG = 0;
  handled = fakescreen_ui.key(fakescreen_ui.state, (uint8_t)k);
  if (handled) x_repaint();
  return handled;
}

static void x_keys(const char *s) {
  for (; *s; s++) x_key((uint8_t)*s);
}

/* An action, as its chord or the palette would run it. */
static int x_act(int a) {
  int r;
  X_NDMG = 0;
  r = fakescreen_ui.action(fakescreen_ui.state, a);
  if (r) x_repaint();
  return r;
}

/* Time passes: ms on the clock, then one pass of the shell's loop. */
static int x_tick(unsigned ms) {
  int r;
  fakeapi_ticks += ms;
  X_NDMG = 0;
  r = fakescreen_ui.tick(fakescreen_ui.state, fakeapi_ticks);
  if (r) x_repaint();
  return r;
}

static void x_ticks(int n, unsigned ms) {
  while (n-- > 0) x_tick(ms);
}

/* Every repaint left what a whole paint would, and a whole paint left no
 * pixel unwritten. */
static void x_clean(void) {
  CHECK_EQ(X_BAD, 0);
  CHECK_EQ(X_UNSET, 0);
}

/* A card with the Mini Heart puzzle on it and no server, opened on its grid. */
static void x_open_mini(void) {
  x_init();
  XA.caps_ok = xn_caps_none;
  fakefs_put(INDEX_PATH, X_MINI_LINE);
  fakefs_put(X_PUZ, X_MINI);
  x_start();
  x_ticks(3, 10);
  x_key('\r');
}

/* The square at row r, column c of the grid, where it is on screen now. */
static CRect x_cell(int r, int c) {
  Geo g;
  geometry(&g);
  return capp_rect(g.gx0 + (c - G.ox) * g.cs, g.gy0 + (r - G.oy) * g.cs, g.cs, g.cs);
}

/* A file of the repository, found beside this file's folder. */
static FILE *x_repo_file(const char *rel) {
  char path[600];
  const char *f = __FILE__, *cut = f, *p;
  for (p = f; *p; p++) if (*p == '/' || *p == '\\') cut = p;
  if (cut == f) snprintf(path, sizeof path, "%s", rel);
  else snprintf(path, sizeof path, "%.*s/../%s", (int)(cut - f), f, rel);
  return fopen(path, "r");
}

/* ---- registered ---------------------------------------------------------------------- */

void test_crossword_paint_is_registered_with_an_icon_a_folder_and_the_proxy_flag(void) {
  static const char INK[] = "KWGDyobngermptPR.";   /* tools/make_color_icons.py's INK */
  char line[256], a[64], b[64];
  FILE *f;
  int i, any = 0, rows = 0, bad = 0, drawn = 0, found = 0, first = 1;

  CHECK(crossword_capp_info.flags & CAPP_NEEDS_PROXY);
  CHECK(crossword_capp_info.flags & CAPP_FULLSCREEN);
  CHECK_EQ(crossword_capp_info.api_version, CAPP_API_VERSION);
  CHECK(!strcmp(crossword_capp_info.name, "Crossword"));
  CHECK_EQ((int)sizeof crossword_capp_info.icon, 32);
  for (i = 0; i < (int)sizeof crossword_capp_info.icon; i++) any |= crossword_capp_info.icon[i];
  CHECK(any);                                   /* build_apps refuses a blank one */
  CHECK(crossword_capp_info.commands == ACTIONS);
  CHECK_EQ(crossword_capp_info.ncommands, NACT);

  /* A line in apps/folders.txt: Games. */
  f = x_repo_file("apps/folders.txt");
  CHECK(f != NULL);
  if (f) {
    while (fgets(line, sizeof line, f))
      if (sscanf(line, "%63s %63s", a, b) == 2 && !strcmp(a, "crossword") && !strcmp(b, "Games")) found++;
    fclose(f);
  }
  CHECK_EQ(found, 1);

  /* The colour icon: the app's name, then sixteen rows of sixteen INK letters. */
  f = x_repo_file("apps/icons/crossword.txt");
  CHECK(f != NULL);
  if (f) {
    while (fgets(line, sizeof line, f)) {
      int len = (int)strlen(line);
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ')) line[--len] = 0;
      if (!len) continue;
      if (first) {
        CHECK(!strcmp(line, crossword_capp_info.name));
        first = 0;
      } else {
        if (len != 16) bad++;
        for (i = 0; i < len; i++) {
          if (!strchr(INK, line[i])) bad++;
          if (line[i] != '.') drawn++;
        }
        rows++;
      }
    }
    fclose(f);
  }
  CHECK_EQ(rows, 16);
  CHECK_EQ(bad, 0);
  CHECK(drawn > 100);
}

/* ---- the list, from the server ------------------------------------------------------------ */

void test_crossword_paint_the_list_comes_from_the_server_and_fills_the_screen(void) {
  int i;
  x_init();
  XN.list = X_LIST;
  XN.wait = 2;
  x_start();
  CHECK_EQ(XN.starts, 1);
  CHECK(!strcmp(XN.url, "http://srv/crossword/list?limit=40&kind=full"));
  CHECK(G.online);
  CHECK_EQ(G.net, NET_LIST);
  CHECK_EQ(G.npage, 0);
  CHECK(!strcmp(G.status, "loading..."));
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H, FS_W, ROW_H), L_TEXT) > 0);     /* "loading the list..." */
  fakescreen_dump("CROSSWORD_DUMP", "crossword_loading");

  /* Pending twice, then the reply. */
  CHECK_EQ(x_tick(10), 0);
  CHECK_EQ(x_tick(10), 0);
  CHECK_EQ(G.npage, 0);
  CHECK_EQ(x_tick(10), 1);
  CHECK_EQ(G.net, NET_IDLE);
  x_ticks(10, 10);                                 /* and each row looked up on the card */
  CHECK_EQ(G.npage, 9);
  CHECK_EQ(G.look, 9);
  CHECK_EQ(G.sel, 0);
  CHECK_EQ(G.top, 0);
  CHECK(!G.local);
  CHECK_EQ(G.more_older, 0);
  CHECK_EQ(G.more_newer, 0);
  CHECK_EQ(G.status[0], 0);
  CHECK(!strcmp(G.page[0].id, "1993-11-21"));
  CHECK(!strcmp(G.page[0].date, "1993-11-21"));
  CHECK(!strcmp(G.page[0].title, "Mini Heart"));
  CHECK_EQ(G.page[0].wd, 6);
  CHECK_EQ(G.page[1].wd, 5);
  CHECK_EQ(G.page[6].wd, 0);
  CHECK_EQ(G.page[0].w, 5);
  CHECK_EQ(G.page[1].w, 15);
  CHECK_EQ(G.page[7].h, 21);
  CHECK_EQ(G.page[3].title[0], 0);                  /* the Thursday has no title */
  CHECK_EQ((int)strlen(G.page[5].title), TITLE_KEEP - 1);   /* a long one is cut to fit its field */

  /* The selected row is tinted, the next is not; each weekday has its colour. */
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H, FS_W, ROW_H), L_SEL) > 2000);
  CHECK(fakescreen_count(capp_rect(0, TOP_H + ROW_H, FS_W, ROW_H), L_BG) > 2000);
  CHECK_EQ(fakescreen_count(capp_rect(0, TOP_H + ROW_H, FS_W, ROW_H), L_SEL), 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H, 34, ROW_H), WD_COLOUR[6]) > 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H + ROW_H, 34, ROW_H), WD_COLOUR[5]) > 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H + 6 * ROW_H, 34, ROW_H), WD_COLOUR[0]) > 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_list");

  /* Down past what fits scrolls the window; at the end there is nothing to ask for. */
  for (i = 0; i < 8; i++) CHECK_EQ(x_key(CAPP_KEY_DOWN), 1);
  CHECK_EQ(G.sel, 8);
  CHECK_EQ(G.top, 2);
  CHECK_EQ(x_key(CAPP_KEY_DOWN), 0);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_list_end");
  CHECK_EQ(x_key(CAPP_KEY_LEFT), 1);                /* a screenful up */
  CHECK_EQ(G.sel, 1);
  CHECK_EQ(G.top, 1);
  CHECK_EQ(x_key(CAPP_KEY_UP), 1);
  CHECK_EQ(G.sel, 0);
  x_clean();
}

void test_crossword_paint_the_list_filters_by_weekday_and_date_through_the_server(void) {
  x_init();
  XN.list = X_LIST;
  x_start();
  x_ticks(12, 10);
  CHECK_EQ(G.npage, 9);

  /* w: Mondays only, asked of the server. */
  XN.list = "1993-11-15\t1993-11-15\tMon\t15x15\tMonday Warm-up\n"
            "1993-11-08\t1993-11-08\tMon\t15x15\tAnother Monday\n";
  CHECK_EQ(x_key('w'), 1);
  CHECK_EQ(G.qwd, 0);
  CHECK_EQ(XN.starts, 2);
  CHECK(!strcmp(XN.url, "http://srv/crossword/list?limit=40&kind=full&weekday=0"));
  CHECK_EQ(G.npage, 0);
  CHECK_EQ(fakescreen_unset(), 0);                  /* loading, with nothing to list */
  x_tick(10);
  CHECK_EQ(G.npage, 2);
  CHECK(!strcmp(G.page[1].id, "1993-11-08"));
  CHECK_EQ(G.sel, 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_monday");

  /* g: a date to go back to, typed into the footer. */
  CHECK_EQ(x_key('g'), 1);
  CHECK(G.prompting);
  CHECK(fakescreen_ui.wants_text(fakescreen_ui.state));
  x_keys("1993-11");
  CHECK(!strcmp(G.pbuf, "1993-11"));
  fakescreen_dump("CROSSWORD_DUMP", "crossword_prompt");
  XN.list = "1993-11-15\t1993-11-15\tMon\t15x15\tMonday Warm-up\n";
  x_key('\r');
  CHECK(!G.prompting);
  CHECK(!strcmp(G.qto, "1993-11-30"));              /* a month is up to its last day */
  CHECK(!strcmp(XN.url, "http://srv/crossword/list?limit=40&kind=full&weekday=0&to=1993-11-30"));
  x_tick(10);
  CHECK_EQ(G.npage, 1);
  CHECK(!fakescreen_ui.wants_text(fakescreen_ui.state));

  /* A date that is not one is said so and stays in the box; Esc leaves it. */
  x_key('g');
  x_keys("19");
  x_key('\r');
  CHECK(G.prompting);
  CHECK(G.bad);
  CHECK(strstr(G.status, "a date") != NULL);
  CHECK_EQ(XN.starts, 3);                            /* the first list, Mondays, Mondays to a date: no more */
  x_key(CAPP_KEY_ESC);
  CHECK(!G.prompting);

  /* a: everything again. */
  XN.list = X_LIST;
  x_key('a');
  CHECK_EQ(G.qwd, 7);
  CHECK_EQ(G.qto[0], 0);
  CHECK(!strcmp(XN.url, "http://srv/crossword/list?limit=40&kind=full"));
  x_tick(10);
  CHECK_EQ(G.npage, 9);
  x_clean();
}

void test_crossword_paint_the_list_pages_past_either_end_of_its_window(void) {
  int i;
  x_init();
  x_build_page();
  XN.list = X_PAGE;
  x_start();
  x_tick(10);
  CHECK_EQ(G.npage, 40);
  CHECK_EQ(G.more_older, 1);
  CHECK_EQ(G.more_newer, 0);
  for (i = 0; i < 39; i++) x_key(CAPP_KEY_DOWN);
  CHECK_EQ(G.sel, 39);
  CHECK_EQ(G.top, 33);
  CHECK_EQ(fakescreen_unset(), 0);

  /* Past the last row: the next page, by the last id. */
  XN.list = "1993-11-16\t1993-11-16\tTue\t15x15\tOlder A\n"
            "1993-11-15\t1993-11-15\tMon\t15x15\tOlder B\n"
            "1993-11-14\t1993-11-14\tSun\t15x15\tOlder C\n";
  CHECK_EQ(x_key(CAPP_KEY_DOWN), 1);
  CHECK(strstr(XN.url, "&after=1993-11-17") != NULL);
  CHECK_EQ(G.net, NET_LIST);
  x_tick(10);
  CHECK_EQ(G.npage, 3);
  CHECK(!strcmp(G.page[0].id, "1993-11-16"));
  CHECK_EQ(G.sel, 0);
  CHECK_EQ(G.more_newer, 1);
  CHECK_EQ(G.more_older, 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_older");

  /* Above the first row: the page before, asked oldest-first and turned round. */
  XN.list = "1993-11-17\t1993-11-17\tWed\t15x15\tNearer\n"
            "1993-11-18\t1993-11-18\tThu\t15x15\tNewer still\n";
  CHECK_EQ(x_key(CAPP_KEY_UP), 1);
  CHECK(strstr(XN.url, "&order=old&after=1993-11-16") != NULL);
  x_tick(10);
  CHECK_EQ(G.npage, 2);
  CHECK(!strcmp(G.page[0].id, "1993-11-18"));
  CHECK(!strcmp(G.page[1].id, "1993-11-17"));
  CHECK_EQ(G.sel, 1);
  CHECK_EQ(G.more_older, 1);
  CHECK_EQ(G.more_newer, 0);

  /* An answer with nothing in it leaves the window and says why. */
  XN.list = "";
  CHECK_EQ(x_key(CAPP_KEY_DOWN), 1);
  x_tick(10);
  CHECK_EQ(G.npage, 2);
  CHECK(strstr(G.status, "oldest") != NULL);
  x_clean();
}

/* ---- the list, from the card ----------------------------------------------------------------- */

void test_crossword_paint_a_server_that_fails_leaves_the_card_to_answer(void) {
  x_init();
  fakefs_put(INDEX_PATH, X_LIST);
  XN.list = NULL;
  XN.fail = -503;
  x_start();
  CHECK(G.online);
  x_tick(10);
  CHECK_EQ(G.net, NET_IDLE);
  CHECK(!G.online);                                  /* the card's list until `r` asks again */
  CHECK(G.local);
  CHECK_EQ(G.npage, 9);
  CHECK(strstr(G.status, "offline") != NULL);
  CHECK(G.bad);
  CHECK(!strcmp(G.page[8].id, "1993-11-13"));
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_offline");

  /* r: the server again, now answering. */
  XN.list = X_LIST;
  x_key('r');
  CHECK(G.online);
  x_tick(10);
  CHECK(!G.local);
  CHECK_EQ(G.npage, 9);
  CHECK_EQ(XN.starts, 2);

  /* o: the card's, on purpose -- no request. */
  x_key('o');
  CHECK(!G.online);
  CHECK(G.local);
  CHECK_EQ(G.npage, 9);
  CHECK_EQ(XN.starts, 2);

  /* The card answers the questions the server does. */
  x_key('w');
  CHECK_EQ(G.qwd, 0);
  CHECK_EQ(G.npage, 1);
  CHECK(!strcmp(G.page[0].id, "1993-11-15"));
  x_key('W');
  CHECK_EQ(G.qwd, 7);
  CHECK_EQ(G.npage, 9);
  x_key('g');
  x_keys("1993-11-17");
  x_key('\r');
  CHECK(!strcmp(G.qto, "1993-11-17"));
  CHECK_EQ(G.npage, 5);
  CHECK(!strcmp(G.page[0].id, "1993-11-17"));
  CHECK(!strcmp(G.page[4].id, "1993-11-13"));
  fakescreen_dump("CROSSWORD_DUMP", "crossword_card_to");
  x_key('a');
  CHECK_EQ(G.npage, 9);
  CHECK_EQ(XN.starts, 2);
  x_clean();
}

void test_crossword_paint_with_no_server_an_empty_card_says_what_to_do(void) {
  x_init();
  XA.caps_ok = xn_caps_none;
  x_start();
  CHECK(!G.online);
  CHECK_EQ(XN.starts, 0);
  CHECK(G.local);
  CHECK_EQ(G.npage, 0);
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H, FS_W, ROW_H), L_TEXT) > 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_empty");
  CHECK_EQ(x_key('\r'), 1);                          /* Enter on nothing opens nothing */
  CHECK_EQ(G.view, V_LIST);
  CHECK_EQ(x_key(CAPP_KEY_DOWN), 0);
  x_clean();
}

/* ---- opening a puzzle ------------------------------------------------------------------------------ */

void test_crossword_paint_opening_a_puzzle_downloads_it_once_and_keeps_it(void) {
  const char *idx;
  x_init();
  XN.list = X_LIST;
  XN.puzzle = X_MINI;
  x_start();
  x_ticks(12, 10);
  CHECK_EQ(G.npage, 9);
  CHECK(!fakefs_exists(X_PUZ));

  /* Enter: the screen says so first, the download is the next tick's. */
  x_key('\r');
  CHECK(G.dl);
  CHECK(strstr(G.status, "downloading 1993-11-21") != NULL);
  CHECK_EQ(XN.downloads, 0);
  CHECK_EQ(G.view, V_LIST);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_downloading");
  CHECK_EQ(x_tick(10), 1);
  CHECK_EQ(XN.downloads, 1);
  CHECK(!strcmp(XN.dl_url, "http://srv/crossword/puzzle?id=1993-11-21"));
  CHECK_EQ(G.view, V_GRID);
  CHECK(G.have_pz);
  CHECK_EQ(G.pz.w, 5);
  CHECK(!strcmp(G.pz.title, "Mini Heart"));
  CHECK(!strcmp(G.cur_id, "1993-11-21"));
  CHECK(!G.dl);

  /* Kept as it came, and in the card's index, newest first, in the server's line format. */
  CHECK(fakefs_exists(X_PUZ));
  CHECK(!fakefs_exists(X_PUZ ".part"));
  CHECK(!strcmp(fakefs_get(X_PUZ), X_MINI));
  idx = fakefs_get(INDEX_PATH);
  CHECK(idx != NULL);
  if (idx) CHECK(!strcmp(idx, X_MINI_LINE));
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_first_grid");

  /* Back to the list: the row shows the puzzle is on the card. Opened again, no download. */
  x_key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_LIST);
  x_ticks(10, 10);
  CHECK_EQ(G.page[0].mark, M_CARD);
  CHECK(fakescreen_count(capp_rect(FS_W - ROW_H, TOP_H, ROW_H, ROW_H), K_CARD) > 5);
  x_key('\r');
  CHECK_EQ(G.view, V_GRID);
  CHECK_EQ(XN.downloads, 1);
  x_clean();
}

void test_crossword_paint_a_download_that_goes_wrong_says_why_and_keeps_nothing(void) {
  int before;
  x_init();
  XN.list = X_LIST;
  XN.puzzle = NULL;                                  /* the server has no such puzzle */
  x_start();
  x_ticks(12, 10);

  x_key('\r');
  x_tick(10);
  CHECK_EQ(XN.downloads, 1);
  CHECK_EQ(G.view, V_LIST);
  CHECK(!strcmp(G.status, "the server has no such puzzle"));
  CHECK(G.bad);
  CHECK(!fakefs_exists(X_PUZ));
  CHECK(!fakefs_exists(X_PUZ ".part"));
  fakescreen_dump("CROSSWORD_DUMP", "crossword_failed");

  XN.dl_fail = -3;
  x_key('\r');
  x_tick(10);
  CHECK(!strcmp(G.status, "download failed (-3)"));
  CHECK(!fakefs_exists(X_PUZ ".part"));

  /* A puzzle that comes cut short is refused whole, not kept. */
  XN.dl_fail = 0;
  XN.puzzle = "xw 1\ntitle Cut Short\nsize 5 5\nrow HEAR.\n";
  x_key('\r');
  x_tick(10);
  CHECK(strstr(G.status, "a bad puzzle") == G.status);
  CHECK_EQ(G.view, V_LIST);
  CHECK(!fakefs_exists(X_PUZ));
  CHECK(!fakefs_exists(X_PUZ ".part"));
  CHECK(!fakefs_exists(INDEX_PATH));

  /* With no network at all it is the card's puzzles or none, and it says so. */
  XA.net_ready = xn_zero;
  XA.net_connect = xn_connect_fails;
  before = XN.downloads;
  x_key('\r');
  x_tick(10);
  CHECK_EQ(XN.downloads, before);
  CHECK(strstr(G.status, "offline") != NULL);
  CHECK(G.bad);
  CHECK_EQ(G.view, V_LIST);
  x_clean();
}

void test_crossword_paint_a_damaged_puzzle_on_the_card_is_fetched_again(void) {
  x_init();
  XN.list = X_LIST;
  XN.puzzle = X_MINI;
  fakefs_put(X_PUZ, "this is not a puzzle\n");
  x_start();
  x_ticks(12, 10);
  x_key('\r');
  CHECK(G.dl);
  CHECK(strstr(G.status, "damaged") != NULL);
  CHECK(G.bad);
  CHECK_EQ(G.view, V_LIST);
  x_tick(10);
  CHECK_EQ(XN.downloads, 1);
  CHECK_EQ(G.view, V_GRID);
  CHECK(!strcmp(fakefs_get(X_PUZ), X_MINI));
  x_clean();
}

/* ---- the grid --------------------------------------------------------------------------------------- */

void test_crossword_paint_the_grid_draws_the_word_the_cursor_numbers_and_the_ring(void) {
  Geo g;
  int c;
  x_open_mini();
  CHECK_EQ(G.view, V_GRID);
  CHECK_EQ(G.pz.cur, 0);
  CHECK_EQ(G.pz.dir, XW_ACROSS);
  CHECK(!strcmp(G.cl_label, "1-Across"));
  CHECK(!strcmp(G.cl_text, "Catch with the ears"));
  CHECK_EQ(G.cl_cut, 0);
  CHECK_EQ(G.cl_len[1], 0);                          /* one line is enough for it */
  CHECK_EQ(fakescreen_ui.wants_text(fakescreen_ui.state), CAPP_TEXT_ARROWS);
  CHECK_EQ(fakescreen_unset(), 0);

  geometry(&g);
  CHECK_EQ(g.nc, 5);
  CHECK_EQ(g.nr, 5);
  CHECK(g.gy0 > 0);
  /* The cursor's square, and the rest of its word tinted. */
  CHECK(fakescreen_count(x_cell(0, 0), G_CUR) > 140);
  for (c = 1; c < 4; c++) CHECK(fakescreen_count(x_cell(0, c), G_WORD) > 140);
  CHECK_EQ(fakescreen_count(x_cell(0, 1), G_CUR), 0);
  /* A square out of the word is white, and a black one all black. */
  CHECK(fakescreen_count(x_cell(1, 1), G_WHITE) > 160);
  CHECK_EQ(fakescreen_count(x_cell(1, 1), G_WORD), 0);
  CHECK_EQ(fakescreen_count(x_cell(0, 4), G_BLOCK), CELL * CELL);
  CHECK_EQ(fakescreen_count(x_cell(4, 0), G_BLOCK), CELL * CELL);
  /* 1 is numbered; the circled square has its ring and its neighbours none. */
  CHECK_EQ(fakescreen_count(x_cell(0, 0), G_NUM), 8);
  CHECK(fakescreen_count(x_cell(2, 2), G_RING) > 30);
  CHECK_EQ(fakescreen_count(x_cell(0, 0), G_RING), 0);
  CHECK_EQ(fakescreen_count(x_cell(2, 3), G_RING), 0);
  /* The margin round the grid, and the grid closed on its right and bottom. */
  CHECK_EQ(fakescreen_count(capp_rect(0, 0, FS_W, 1), G_MARGIN), FS_W);
  CHECK_EQ(fakescreen_count(capp_rect(g.gx0 + 5 * CELL, g.gy0, 1, 5 * CELL + 1), G_LINE), 5 * CELL + 1);
  CHECK_EQ(fakescreen_count(capp_rect(g.gx0, g.gy0 + 5 * CELL, 5 * CELL + 1, 1), G_LINE), 5 * CELL + 1);
  /* The clue bar: its label in blue, its words in black, on its own ground. */
  CHECK(fakescreen_count(capp_rect(0, g.bar_y, FS_W, g.bar_h), B_LABEL) > 0);
  CHECK(fakescreen_count(capp_rect(0, g.bar_y, FS_W, g.bar_h), B_TEXT) > 0);
  CHECK(fakescreen_count(capp_rect(0, g.bar_y, FS_W, g.bar_h), B_BG) > 3000);
  /* The footer's hints and the time. */
  CHECK(fakescreen_count(capp_rect(0, FS_H - FOOT_H, FS_W, FOOT_H), FOOT_FG) > 40);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_grid");
  x_clean();
}

void test_crossword_paint_typing_checking_and_starting_over_repaint_what_changed(void) {
  const char *sav;
  x_open_mini();

  /* Letters go into the squares and the cursor goes on. */
  x_keys("HEA");
  CHECK_EQ(G.pz.cell[0].ent, 'H');
  CHECK_EQ(G.pz.cell[1].ent, 'E');
  CHECK_EQ(G.pz.cell[2].ent, 'A');
  CHECK_EQ(G.pz.cur, 3);
  CHECK(G.dirty);
  CHECK_EQ(fakescreen_count(x_cell(0, 0), G_INK), 17);       /* an H */
  CHECK_EQ(fakescreen_count(x_cell(0, 1), G_INK), 18);       /* an E */
  CHECK(fakescreen_count(x_cell(0, 3), G_CUR) > 140);
  CHECK_EQ(fakescreen_count(x_cell(0, 0), G_CUR), 0);
  CHECK(fakescreen_count(x_cell(0, 0), G_WORD) > 100);

  /* Backspace on an empty square steps back and clears the one before. */
  x_key(CAPP_KEY_BACK);
  CHECK_EQ(G.pz.cur, 2);
  CHECK_EQ(G.pz.cell[2].ent, 0);
  CHECK_EQ(fakescreen_count(x_cell(0, 2), G_INK), 0);
  x_key('a');                                                /* either case */
  CHECK_EQ(G.pz.cell[2].ent, 'A');
  CHECK_EQ(G.pz.cur, 3);

  /* Space turns across to down and back; the clue bar follows. */
  x_key(' ');
  CHECK_EQ(G.pz.dir, XW_DOWN);
  CHECK(!strcmp(G.cl_label, "4-Down"));
  CHECK(!strcmp(G.cl_text, "Sticky stuff"));
  x_key(' ');
  CHECK_EQ(G.pz.dir, XW_ACROSS);
  CHECK(!strcmp(G.cl_label, "1-Across"));

  /* A wrong letter (R is the answer), and ctrl-c asks how much to check. */
  x_key('x');
  CHECK_EQ(G.pz.cell[3].ent, 'X');
  CHECK_EQ(G.pz.cur, 3);
  CHECK_EQ(fakescreen_ui.wants_text(fakescreen_ui.state), CAPP_TEXT_ARROWS);
  CHECK_EQ(x_act(ACT_CHECK), 1);
  CHECK_EQ(G.ask, ASK_CHECK);
  CHECK_EQ(fakescreen_ui.wants_text(fakescreen_ui.state), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_ask");
  x_key(CAPP_KEY_ESC);                                       /* Esc leaves the question, not the puzzle */
  CHECK_EQ(G.ask, ASK_NONE);
  CHECK_EQ(G.view, V_GRID);
  x_act(ACT_CHECK);
  x_key('w');
  CHECK_EQ(G.ask, ASK_NONE);
  CHECK(G.pz.cell[3].fl & XW_F_WRONG);
  CHECK(G.msg_bad);
  CHECK(!strcmp(G.msg, "1 wrong in this word"));
  CHECK_EQ(fakescreen_count(x_cell(0, 3), G_WRONG), 13);     /* the X, in red */
  CHECK_EQ(fakescreen_count(x_cell(0, 2), G_WRONG), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_wrong");

  /* Changing it takes the red off. */
  x_key('r');
  CHECK_EQ(G.pz.cell[3].ent, 'R');
  CHECK(!(G.pz.cell[3].fl & XW_F_WRONG));
  CHECK_EQ(fakescreen_count(x_cell(0, 3), G_WRONG), 0);

  /* The message goes after its few seconds; a changed grid is written every 15. */
  x_tick(MSG_MS + 100);
  CHECK_EQ(G.msg[0], 0);
  CHECK(!fakefs_exists(X_SAV));
  x_tick(SAVE_MS);
  CHECK(!G.dirty);
  sav = fakefs_get(X_SAV);
  CHECK(sav != NULL);
  if (sav) {
    CHECK(strstr(sav, "state P") != NULL);
    CHECK(strstr(sav, "fill 4 23") != NULL);
  }

  /* Start over asks first. */
  x_act(ACT_RESET);
  CHECK_EQ(G.ask, ASK_RESET);
  x_key('n');
  CHECK_EQ(G.ask, ASK_NONE);
  CHECK_EQ(G.pz.cell[0].ent, 'H');
  x_act(ACT_RESET);
  x_key('y');
  CHECK_EQ(G.pz.cell[0].ent, 0);
  CHECK_EQ(G.pz.cur, 0);
  CHECK_EQ(G.view, V_GRID);
  CHECK(!fakefs_exists(X_SAV));
  CHECK_EQ(fakescreen_count(x_cell(0, 0), G_INK), 0);
  x_clean();
}

void test_crossword_paint_the_clock_counts_in_the_grid_and_stops_when_nobody_is_there(void) {
  unsigned t0;
  x_open_mini();
  t0 = fakeapi_ticks;
  CHECK(G.pz.running);
  CHECK_EQ(G.pz.t0, t0);
  CHECK_EQ(G.shown_secs, 0);

  /* The second turning over is marked, in the footer's corner only. */
  CHECK_EQ(x_tick(1000), 1);
  CHECK_EQ(G.shown_secs, 1);
  CHECK_EQ(x_tick(500), 0);

  /* No key for three minutes: the clock stops as of the last key, and says so. */
  CHECK_EQ(x_tick(IDLE_MS), 1);
  CHECK(G.paused);
  CHECK(!G.pz.running);
  CHECK_EQ(G.pz.ms, 0);                              /* the idle time is not counted */
  CHECK_EQ(G.shown_secs, 0);
  CHECK_EQ(x_tick(60000), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_paused");

  /* Any key is someone there. */
  x_key(CAPP_KEY_RIGHT);
  CHECK(!G.paused);
  CHECK(G.pz.running);
  CHECK_EQ(G.pz.t0, fakeapi_ticks);
  CHECK_EQ(G.pz.cur, 1);
  x_tick(3000);
  CHECK_EQ(G.shown_secs, 3);

  /* What the idle save wrote is an empty grid, and the list does not call it begun. */
  x_key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_LIST);
  x_ticks(3, 10);
  CHECK(fakefs_exists(X_SAV));
  CHECK_EQ(G.page[0].mark, M_CARD);
  x_clean();
}

void test_crossword_paint_a_grid_bigger_than_the_screen_scrolls_after_the_cursor(void) {
  Geo g;
  int i;
  x_init();
  XA.caps_ok = xn_caps_none;
  x_build_big();
  fakefs_put(INDEX_PATH, "1993-11-14\t1993-11-14\tSun\t21x21\tBig Sunday\n");
  fakefs_put(PUZ_DIR "/1993-11-14.xw", X_BIG);
  x_start();
  x_ticks(3, 10);
  x_key('\r');
  CHECK_EQ(G.view, V_GRID);
  CHECK_EQ(G.pz.w, 21);
  CHECK_EQ(G.pz.h, 21);
  geometry(&g);
  CHECK(g.vcols < 21);
  CHECK(g.vrows < 21);
  CHECK_EQ(g.nc, g.vcols);
  CHECK_EQ(g.nr, g.vrows);
  CHECK_EQ(G.cl_cut, 1);                             /* two lines of it fit; the rest is "..", not drawn */
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_big");

  /* Right along the top row: the view follows, and stops at the grid's edge. */
  for (i = 0; i < 20; i++) x_key(CAPP_KEY_RIGHT);
  CHECK_EQ(G.pz.cur % 21, 20);
  CHECK_EQ(G.ox, 21 - g.vcols);
  CHECK_EQ(G.oy, 0);
  CHECK(fakescreen_count(x_cell(0, 20), G_CUR) > 120);        /* a two-digit number takes some of it */
  /* Space turns to the down word; arrows only move, so the rest walk it. */
  x_key(' ');
  for (i = 0; i < 21; i++) x_key(CAPP_KEY_DOWN);
  CHECK_EQ(G.pz.cur / 21, 20);
  CHECK_EQ(G.pz.dir, XW_DOWN);
  CHECK_EQ(G.oy, 21 - g.vrows);
  CHECK(!strcmp(G.cl_label, "21-Down"));
  CHECK(fakescreen_count(x_cell(20, 20), G_CUR) > 140);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_big_corner");
  x_clean();
}

/* A 15x15 daily, opened in the view the card says. */
static void x_open_daily(const char *view) {
  x_init();
  fakefs_put(VIEW_PATH, view);
  XA.caps_ok = xn_caps_none;
  x_build_sq(15);
  fakefs_put(INDEX_PATH, "1993-11-15\t1993-11-15\tMon\t15x15\tA Daily\n");
  fakefs_put(PUZ_DIR "/1993-11-15.xw", X_BIG);
  x_start();
  x_ticks(3, 10);
  x_key('\r');
}

void test_crossword_paint_the_side_view_shows_a_daily_whole_and_its_clues_beside_it(void) {
  Geo g;
  int i;
  x_open_daily("side\n");
  CHECK_EQ(G.view, V_GRID);
  CHECK_EQ(G.lay, LAY_SIDE);
  geometry(&g);
  CHECK_EQ(g.cs, CELL_MIN);                           /* 15 eights and a line: 121 of 124 */
  CHECK_EQ(g.nc, 15);
  CHECK_EQ(g.nr, 15);
  CHECK_EQ(g.gx0, 0);                                 /* against the left edge: no gutter */
  CHECK_EQ(g.col.x, 15 * CELL_MIN + 1);
  CHECK_EQ(g.col.w, FS_W - g.col.x);
  CHECK_EQ(fakescreen_count(capp_rect(0, g.gy0, g.col.x, 15 * CELL_MIN + 1), G_MARGIN), 0);
  /* The clue, long enough here to fill the column; the crossing clue gets
   * what room is left, which for this one is none. */
  CHECK(fakescreen_count(g.col, B_LABEL) > 0);
  CHECK(fakescreen_count(g.col, B_TEXT) > 100);
  CHECK(fakescreen_count(x_cell(0, 0), G_CUR) > 30);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_side");

  /* Arrows move and never turn; the column's crossing clue follows the cursor. */
  for (i = 0; i < 3; i++) CHECK_EQ(x_key(CAPP_KEY_RIGHT), 1);
  CHECK_EQ(G.pz.dir, XW_ACROSS);
  CHECK_EQ(x_key(CAPP_KEY_DOWN), 1);
  CHECK_EQ(G.pz.dir, XW_ACROSS);
  CHECK_EQ(G.pz.cur, 15 + 3);
  CHECK_EQ(G.cross_ci, xw_clue_at(&G.pz, G.pz.cur, XW_DOWN));
  /* Tab turns, as space does; Enter is the next word. */
  x_key('\t');
  CHECK_EQ(G.pz.dir, XW_DOWN);
  x_key(' ');
  CHECK_EQ(G.pz.dir, XW_ACROSS);
  x_key('\r');
  CHECK_EQ(G.pz.cur / 15, 2);
  x_clean();
}

void test_crossword_paint_the_side_column_shows_the_crossing_clue_dimmed(void) {
  Geo g;
  x_init();
  fakefs_put(VIEW_PATH, "side\n");
  XA.caps_ok = xn_caps_none;
  fakefs_put(INDEX_PATH, X_MINI_LINE);
  fakefs_put(X_PUZ, X_MINI);
  x_start();
  x_ticks(3, 10);
  x_key('\r');
  geometry(&g);
  CHECK_EQ(g.cs, CELL_SIDE);                          /* a mini in big squares */
  CHECK_EQ(g.gx0, 0);
  CHECK(g.col.w >= COL_MIN);
  CHECK(fakescreen_count(g.col, B_TEXT) > 30);       /* 1-Across: Catch with the ears */
  CHECK(fakescreen_count(g.col, L_DIM) > 30);        /* 1-Down: Listen up */
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_side_mini");
  /* Along the word the crossing clue changes, and only the column and the
   * squares are repainted -- and match a whole paint (x_clean). */
  x_key(CAPP_KEY_RIGHT);
  CHECK_EQ(G.cross_ci, xw_clue_at(&G.pz, 1, XW_DOWN));
  x_clean();
}

void test_crossword_paint_tab_switches_full_and_midi_on_the_server_and_the_card(void) {
  const char *v;
  /* Online: the tab is the server's kind=. */
  x_init();
  XN.list = X_MINI_LINE;
  x_start();
  x_ticks(3, 10);
  CHECK_EQ(G.kind, KIND_FULL);
  CHECK(strstr(XN.url, "&kind=full") != NULL);
  XN.list = "2026-10-27-midi\t2026-10-27\tTue\t10x9\tMidi: License Plate Game\n";
  CHECK_EQ(x_key('\t'), 1);
  x_ticks(3, 10);
  CHECK_EQ(G.kind, KIND_MIDI);
  CHECK(strstr(XN.url, "&kind=midi") != NULL);
  CHECK_EQ(G.npage, 1);
  CHECK(!strcmp(G.page[0].id, "2026-10-27-midi"));
  CHECK(fakescreen_count(capp_rect(0, TOP_H - 3, 80, 2), K_PLAY) > 20);   /* the tab's blue line */
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_tab_midi");
  v = fakefs_get(VIEW_PATH);
  CHECK(v && !strcmp(v, "big\nmidi\n"));             /* x_init chose the big view */
  x_clean();

  /* Offline: the card's index is split the same way -- a name that is not a
   * date is Themed -- and the tab is kept. */
  x_init();
  fakefs_put(VIEW_PATH, "side\nmidi\n");
  XA.caps_ok = xn_caps_none;
  fakefs_put(INDEX_PATH, "traitors\t\t\t15x15\tThe Traitors\n"
                         "2026-10-27-midi\t2026-10-27\tTue\t10x9\tMidi: A\n"
                         "1993-11-21\t1993-11-21\tSun\t5x5\tMini Heart\n");
  x_start();
  x_ticks(3, 10);
  CHECK_EQ(G.kind, KIND_MIDI);
  CHECK_EQ(G.npage, 1);
  CHECK(!strcmp(G.page[0].id, "2026-10-27-midi"));
  x_key('\t');
  x_ticks(3, 10);
  CHECK_EQ(G.kind, KIND_THEMED);
  CHECK_EQ(G.npage, 1);
  CHECK(!strcmp(G.page[0].id, "traitors"));
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_tab_themed");
  x_key('\t');
  x_ticks(3, 10);
  CHECK_EQ(G.kind, KIND_FULL);
  CHECK_EQ(G.npage, 1);
  CHECK(!strcmp(G.page[0].id, "1993-11-21"));
  x_clean();
}

void test_crossword_paint_ctrl_l_goes_side_big_zoom_and_is_kept(void) {
  Geo g;
  const char *v;
  x_open_daily("side\n");
  CHECK_EQ(x_act(ACT_LAYOUT), 1);
  CHECK_EQ(G.lay, LAY_BIG);
  v = fakefs_get(VIEW_PATH);
  CHECK(v && !strcmp(v, "big\nfull\n"));
  geometry(&g);
  CHECK_EQ(g.cs, CELL);
  CHECK(g.nr < 15);                                   /* the big view scrolls a daily */
  CHECK_EQ(fakescreen_unset(), 0);

  /* Zoom: sixteens, edge to edge, scrolling up and down. */
  CHECK_EQ(x_act(ACT_LAYOUT), 1);
  CHECK_EQ(G.lay, LAY_ZOOM);
  geometry(&g);
  CHECK_EQ(g.cs, 16);
  CHECK_EQ(g.nc, 15);
  CHECK_EQ(g.gx0, 0);
  CHECK(g.nr < 15);
  CHECK_EQ(fakescreen_count(capp_rect(0, g.gy0, FS_W, g.nr * 16), G_MARGIN), 0);
  CHECK(fakescreen_count(capp_rect(0, g.bar_y, FS_W, g.bar_h), B_LABEL) > 0);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_zoom");
  x_key(CAPP_KEY_DOWN);
  x_key(CAPP_KEY_DOWN);
  CHECK_EQ(G.pz.cur, 30);
  v = fakefs_get(VIEW_PATH);
  CHECK(v && !strcmp(v, "zoom\nfull\n"));

  /* And round to the side view again; leaving and coming back keeps it. */
  CHECK_EQ(x_act(ACT_LAYOUT), 1);
  CHECK_EQ(G.lay, LAY_SIDE);
  x_key(CAPP_KEY_ESC);
  x_key('\r');
  CHECK_EQ(G.lay, LAY_SIDE);
  x_clean();
}

void test_crossword_paint_the_zoom_view_draws_a_mini_in_big_letters(void) {
  Geo g;
  x_init();
  fakefs_put(VIEW_PATH, "zoom\n");
  XA.caps_ok = xn_caps_none;
  fakefs_put(INDEX_PATH, X_MINI_LINE);
  fakefs_put(X_PUZ, X_MINI);
  x_start();
  x_ticks(3, 10);
  x_key('\r');
  geometry(&g);
  CHECK_EQ(g.cs, CELL_ZOOM);
  x_key('h');
  CHECK(fakescreen_count(x_cell(0, 0), G_INK) > 40);  /* an H at twice the size */
  CHECK(fakescreen_count(x_cell(2, 2), G_RING) > 30);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_zoom_mini");
  x_clean();
}

/* ---- finishing -------------------------------------------------------------------------------------- */

void test_crossword_paint_solving_shows_the_results_and_counts_it_once(void) {
  const char *s;
  x_open_mini();
  x_keys("HEAR\rEMBER\rABUSE\rRESIN\rREN");
  CHECK_EQ(G.view, V_GRID);
  CHECK(!G.pz.solved);
  fakeapi_ticks += 95000;                            /* a minute and thirty-five seconds on the clock */
  x_key('D');
  CHECK_EQ(G.view, V_DONE);
  CHECK(G.pz.solved);
  CHECK_EQ(G.pz.ms, 95000u);
  CHECK(!G.pz.running);
  CHECK_EQ(G.done_new, XWS_FIRST);
  CHECK_EQ(G.st.solved[6], 1);                       /* a Sunday */
  CHECK_EQ(G.st.best[6], 95u);
  CHECK_EQ(G.st.total[6], 95u);
  CHECK_EQ(G.st.helped, 0);
  CHECK_EQ(G.st.streak, 1);
  CHECK_EQ(fakescreen_ui.wants_text(fakescreen_ui.state), 0);

  /* Counted in the file, and the puzzle's progress says solved. */
  s = fakefs_get(STATS_PATH);
  CHECK(s != NULL);
  if (s) CHECK(strstr(s, "wd 6 1 95 95\n") != NULL);
  s = fakefs_get(X_SAV);
  CHECK(s != NULL);
  if (s) {
    CHECK(strstr(s, "state S") != NULL);
    CHECK(strstr(s, "time 95") != NULL);
  }

  /* The results: the time large, in the colour of a clean solve. */
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H + 6, FS_W, G.lhn + 6), K_DONE) > 20);
  CHECK(fakescreen_count(capp_rect(0, 0, FS_W, TOP_H), L_BAR) > 1000);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_done");

  /* Tab is the statistics, Esc comes back; Enter is the finished grid, locked. */
  x_key('\t');
  CHECK_EQ(G.view, V_STATS);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_stats_after");
  x_key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_DONE);
  x_key('\r');
  CHECK_EQ(G.view, V_GRID);
  CHECK_EQ(fakescreen_ui.wants_text(fakescreen_ui.state), 0);
  CHECK_EQ(x_key('z'), 1);                           /* taken, changing nothing */
  CHECK_EQ(G.pz.cell[0].ent, 'H');
  x_key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_LIST);

  /* Opened again it is already done, and is not counted a second time. */
  x_ticks(3, 10);
  CHECK_EQ(G.page[0].mark, M_DONE);
  CHECK_EQ(G.page[0].secs, 95u);
  CHECK(fakescreen_count(capp_rect(FS_W - ROW_H, TOP_H, ROW_H, ROW_H), K_DONE) > 10);        /* the check */
  CHECK(fakescreen_count(capp_rect(FS_W - ROW_H - TIME_W, TOP_H, TIME_W, ROW_H), K_DONE) > 5);  /* 1:35: thin digits, few solid pixels */
  fakescreen_dump("CROSSWORD_DUMP", "crossword_list_done");
  x_key('\r');
  CHECK_EQ(G.view, V_GRID);
  CHECK(G.pz.solved);
  CHECK(!G.pz.running);
  x_key(CAPP_KEY_ESC);
  CHECK_EQ(G.st.solved[6], 1);
  x_clean();
}

void test_crossword_paint_a_solve_with_some_letters_given_is_helped_and_sets_no_best(void) {
  const char *s;
  x_open_mini();
  CHECK_EQ(x_act(ACT_REVEAL_WORD), 1);               /* 1-Across, given */
  CHECK_EQ(G.pz.cell[0].ent, 'H');
  CHECK(G.pz.cell[0].fl & XW_F_REVEALED);
  CHECK(!strcmp(G.msg, "revealed: no best time now"));
  CHECK_EQ(fakescreen_count(x_cell(0, 0), G_GIVEN), 17);     /* a given letter is blue */
  CHECK_EQ(fakescreen_count(x_cell(0, 0), G_INK), 0);
  x_keys("\rEMBER\rABUSE\rRESIN\rREND");
  CHECK_EQ(G.view, V_DONE);
  CHECK_EQ(G.done_new, XWS_NOT_BEST);
  CHECK_EQ(G.st.helped, 1);
  CHECK_EQ(G.st.solved[6], 0);
  CHECK_EQ(G.st.best[6], 0u);
  s = fakefs_get(STATS_PATH);
  CHECK(s != NULL);
  if (s) CHECK(strstr(s, "helped 1\n") != NULL);
  s = fakefs_get(X_SAV);
  CHECK(s != NULL);
  if (s) CHECK(strstr(s, "state H") != NULL);
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H + 6, FS_W, G.lhn + 6), K_HELP) > 20);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_helped");

  x_key(CAPP_KEY_ESC);
  x_ticks(3, 10);
  CHECK_EQ(G.page[0].mark, M_HELP);
  CHECK(fakescreen_count(capp_rect(FS_W - ROW_H, TOP_H, ROW_H, ROW_H), K_HELP) > 10);       /* an amber check */
  x_clean();
}

void test_crossword_paint_revealing_the_puzzle_asks_twice_and_counts_nothing(void) {
  const char *s;
  x_open_mini();
  CHECK_EQ(x_act(ACT_REVEAL_ALL), 1);
  CHECK_EQ(G.ask, ASK_REVEAL_ALL);
  x_key('x');                                        /* not an answer: the question stays */
  CHECK_EQ(G.ask, ASK_REVEAL_ALL);
  x_key('n');
  CHECK_EQ(G.ask, ASK_NONE);
  CHECK_EQ(xw_reveals(&G.pz), 0);
  CHECK_EQ(G.view, V_GRID);

  x_act(ACT_REVEAL);                                 /* ctrl-r asks how much; p is the puzzle ... */
  CHECK_EQ(G.ask, ASK_REVEAL);
  x_key('p');
  CHECK_EQ(G.ask, ASK_REVEAL_ALL);                   /* ... and that is asked again */
  fakescreen_dump("CROSSWORD_DUMP", "crossword_reveal_ask");
  x_key('y');
  CHECK_EQ(G.view, V_DONE);
  CHECK_EQ(xw_reveals(&G.pz), 23);
  CHECK_EQ(G.st.solved[6], 0);
  CHECK_EQ(G.st.helped, 0);
  CHECK(!fakefs_exists(STATS_PATH));                 /* nothing is counted */
  s = fakefs_get(X_SAV);
  CHECK(s != NULL);
  if (s) {
    CHECK(strstr(s, "state H") != NULL);
    CHECK(strstr(s, "reveals 23") != NULL);
  }
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H + 6, FS_W, G.lhn + 6), K_CARD) > 20);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_revealed");

  /* Start over from the results. */
  x_act(ACT_RESET);
  CHECK_EQ(G.view, V_GRID);
  CHECK_EQ(G.ask, ASK_RESET);
  x_key('y');
  CHECK(!G.pz.solved);
  CHECK_EQ(G.pz.cell[0].ent, 0);
  CHECK(G.pz.running);
  CHECK(!fakefs_exists(X_SAV));
  x_clean();
}

/* ---- progress and statistics ------------------------------------------------------------------------- */

void test_crossword_paint_progress_survives_leaving_and_can_be_forgotten(void) {
  const char *sav;
  x_open_mini();
  x_keys("HEAR");
  x_key(CAPP_KEY_ESC);                               /* a changed grid is written on the way out */
  CHECK_EQ(G.view, V_LIST);
  sav = fakefs_get(X_SAV);
  CHECK(sav != NULL);
  if (sav) {
    CHECK(strstr(sav, "state P") != NULL);
    CHECK(strstr(sav, "fill 4 23") != NULL);
    CHECK(strstr(sav, "cur 3 A") != NULL);
  }
  x_ticks(3, 10);
  CHECK_EQ(G.page[0].mark, M_PLAY);
  CHECK(fakescreen_count(capp_rect(FS_W - ROW_H, TOP_H, ROW_H, ROW_H), K_PLAY) > 30);    /* half a circle */
  fakescreen_dump("CROSSWORD_DUMP", "crossword_list_begun");

  /* Opened again it is as it was left. */
  x_key('\r');
  CHECK_EQ(G.view, V_GRID);
  CHECK_EQ(G.pz.cell[0].ent, 'H');
  CHECK_EQ(G.pz.cell[3].ent, 'R');
  CHECK_EQ(G.pz.cur, 3);
  CHECK(!G.dirty);
  CHECK_EQ(fakescreen_count(x_cell(0, 3), G_INK), 18);        /* an R */
  x_key(CAPP_KEY_ESC);

  /* d forgets what was done, after asking. */
  x_key('d');
  CHECK_EQ(G.ask, ASK_FORGET);
  x_key('x');
  CHECK_EQ(G.ask, ASK_FORGET);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_forget");
  x_key('n');
  CHECK_EQ(G.ask, ASK_NONE);
  CHECK(fakefs_exists(X_SAV));
  x_key('d');
  x_key('y');
  CHECK(!fakefs_exists(X_SAV));
  CHECK_EQ(G.page[0].mark, M_CARD);
  CHECK(!strcmp(G.status, "progress forgotten"));
  x_key('d');
  CHECK(!strcmp(G.status, "nothing saved for that puzzle"));
  x_clean();
}

void test_crossword_paint_the_statistics_are_read_back_and_drawn(void) {
  XwStats s;
  char buf[512];
  int32_t today = xw_days(2026, 10, 9);
  xws_init(&s);
  xws_solve(&s, 0, 245, today - 2);                  /* three Mondays on three days running */
  xws_solve(&s, 0, 300, today - 1);
  xws_solve(&s, 0, 255, today);
  xws_solve(&s, 2, 400, XWS_NO_DAY);
  xws_help(&s);
  CHECK(xws_format(&s, buf, sizeof buf) > 0);

  x_init();
  XA.caps_ok = xn_caps_none;
  fakefs_put(STATS_PATH, buf);
  x_start();
  CHECK_EQ(G.st.solved[0], 3);
  CHECK_EQ(G.st.best[0], 245u);
  CHECK_EQ(G.st.solved[2], 1);
  CHECK_EQ(G.st.helped, 1);
  CHECK_EQ(G.st.streak, 3);

  x_key('s');                                        /* s is the statistics; tab is the list's tabs */
  CHECK_EQ(G.view, V_STATS);
  CHECK_EQ(G.today, today);
  CHECK_EQ(xws_streak_now(&G.st, G.today), 3);
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(fakescreen_count(capp_rect(0, TOP_H, 44, 13), WD_COLOUR[0]) > 0);        /* Mon */
  CHECK(fakescreen_count(capp_rect(0, TOP_H + 13, 44, 13), WD_COLOUR[1]) > 0);   /* Tue */
  CHECK(fakescreen_count(capp_rect(0, 0, FS_W, TOP_H), L_BAR) > 1000);
  fakescreen_dump("CROSSWORD_DUMP", "crossword_stats");
  CHECK_EQ(x_key('q'), 0);                           /* nothing else is a key here */
  x_key(CAPP_KEY_ESC);
  CHECK_EQ(G.view, V_LIST);

  /* A day later the streak is over, though the file still says three. */
  fakeapi_now.day = 11;
  x_key('s');
  CHECK_EQ(G.today, today + 2);
  CHECK_EQ(xws_streak_now(&G.st, G.today), 0);
  CHECK_EQ(fakescreen_unset(), 0);
  x_key('\t');
  CHECK_EQ(G.view, V_LIST);

  /* A file that is not ours is a fresh start, not a wrong number. */
  fakefs_put(STATS_PATH, "this is not statistics\n");
  x_start();
  CHECK_EQ(G.st.solved[0], 0);
  CHECK_EQ(G.st.streak, 0);
  x_key('s');
  CHECK_EQ(fakescreen_unset(), 0);
  x_clean();
}
