/* Forklift, the warehouse you program, on the host: the default program
 * earns money, robots path round racks, mistakes cost, a broken program
 * pauses rather than crashes, the shop, the editor's keys, and a save that
 * comes back. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info forklift_capp_info
#define capp_main forklift_capp_main
#include "apps/forklift.c"
#undef capp_info
#undef capp_main

/* ---- a CardApi with a pretend card ---------------------------------------- */

static CardApi FAKE;
static uint32_t NOW;

#define NFILES 6
static struct { char path[96]; char data[4096]; int len, used; } FILES[NFILES];
static struct { int file, pos, write; } FDS[4];

static int k_fmt(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  int r;
  va_start(ap, fmt);
  r = vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}
static void *k_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *k_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static size_t k_strlen(const char *s) { return strlen(s); }
static uint32_t k_ticks(void) { return NOW; }
static void k_fill(CRect r, uint16_t c) { (void)r; (void)c; }
static void k_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  (void)x; (void)y; (void)s; (void)fg; (void)bg;
}
static void k_damage(CRect r) { (void)r; }

static int find(const char *path) {
  int i;
  for (i = 0; i < NFILES; i++) if (FILES[i].used && !strcmp(FILES[i].path, path)) return i;
  return -1;
}
static int k_open(const char *path, int flags) {
  int f = find(path), d;
  if (flags & CAPP_O_WRITE) {
    if (f < 0) {
      for (f = 0; f < NFILES && FILES[f].used; f++) { }
      if (f == NFILES) return -1;
      FILES[f].used = 1;
      snprintf(FILES[f].path, sizeof FILES[f].path, "%s", path);
    }
    FILES[f].len = 0;
  } else if (f < 0) return -1;
  for (d = 0; d < 4 && FDS[d].file >= 0; d++) { }
  if (d == 4) return -1;
  FDS[d].file = f; FDS[d].pos = 0; FDS[d].write = (flags & CAPP_O_WRITE) != 0;
  return d;
}
static int k_read(int fd, void *buf, size_t n) {
  int f = FDS[fd].file, left = FILES[f].len - FDS[fd].pos;
  if ((int)n > left) n = (size_t)left;
  memcpy(buf, FILES[f].data + FDS[fd].pos, n);
  FDS[fd].pos += (int)n;
  return (int)n;
}
static int k_write(int fd, const void *buf, size_t n) {
  int f = FDS[fd].file;
  memcpy(FILES[f].data + FILES[f].len, buf, n);
  FILES[f].len += (int)n;
  return (int)n;
}
static void k_close(int fd) { FDS[fd].file = -1; }
static int k_remove(const char *path) { int f = find(path); if (f < 0) return -1; FILES[f].used = 0; return 0; }
static int k_rename(const char *a, const char *b) {
  int f = find(a);
  if (f < 0 || find(b) >= 0) return -1;
  snprintf(FILES[f].path, sizeof FILES[f].path, "%s", b);
  return 0;
}
static int k_stat(const char *path, CappStat *st) {
  int f = find(path);
  if (f < 0) return -1;
  st->size = (uint32_t)FILES[f].len;
  st->is_dir = 0;
  return 0;
}
static int k_mkdir(const char *p) { (void)p; return 0; }
static int k_ui_called;
static void k_ui(const CappUi *ui) { (void)ui; k_ui_called = 1; }

static void fresh_card(void) {
  int i;
  memset(FILES, 0, sizeof FILES);
  for (i = 0; i < 4; i++) FDS[i].file = -1;
}

static void boot(void) {
  memset(&FAKE, 0, sizeof FAKE);
  FAKE.fmt = k_fmt;
  FAKE.mem_set = k_memset;
  FAKE.mem_cpy = k_memcpy;
  FAKE.str_len = k_strlen;
  FAKE.ticks_ms = k_ticks;
  FAKE.fill = k_fill;
  FAKE.frame = k_fill;
  FAKE.text = k_text;
  FAKE.damage = k_damage;
  FAKE.open = k_open;
  FAKE.read = k_read;
  FAKE.write = k_write;
  FAKE.close = k_close;
  FAKE.remove = k_remove;
  FAKE.rename = k_rename;
  FAKE.stat = k_stat;
  FAKE.mkdir = k_mkdir;
  FAKE.ui = k_ui;
  NOW = 1000;
  forklift_capp_main(&FAKE, 0, 0);
  G.seed = 777;                  /* the same orders every run */
  new_order();
}

static void steps(int n) { while (n-- > 0) sim_step(); }

static void set_program(const char *src) {
  ed_from_text(src);
  apply_program();
}

/* ---- the warehouse ---------------------------------------------------------- */

void test_forklift_the_default_program_earns_money(void) {
  fresh_card();
  boot();
  CHECK_EQ(1, G.running);
  CHECK_EQ(0, G.err[0]);
  steps(600);
  CHECK_EQ(0, G.err[0]);
  CHECK(G.orders >= 3);
  CHECK(G.credits > 20);
  CHECK_EQ(0, G.mistakes);
}

void test_forklift_robots_path_round_racks(void) {
  int b, x, y, n = 0, hit_rack = 0;
  fresh_card();
  boot();
  G.size_lv = 2;
  layout();
  place_bots();
  b = 0;
  G.bot[b].x = 0; G.bot[b].y = 1;              /* at the red bay */
  set_program("bot = go ship");
  while ((G.bot[b].x != G.ship_x || G.bot[b].y != G.ship_y) && n < 200) {
    sim_step();
    x = G.bot[b].x; y = G.bot[b].y;
    if (G.tile[y][x].t == TL_RACK) hit_rack = 1;
    n++;
  }
  CHECK_EQ(0, hit_rack);
  CHECK(n < 60);                               /* got there, and directly */
  CHECK_EQ(G.ship_x, G.bot[b].x);
}

void test_forklift_the_warehouses_have_what_they_say(void) {
  int s, k, bays;
  fresh_card();
  boot();
  for (s = 0; s <= 2; s++) {
    int x, y;
    G.size_lv = s;
    layout();
    for (bays = 0, y = 0; y < G.h; y++)
      for (x = 0; x < G.w; x++) if (G.tile[y][x].t == TL_BAY) bays++;
    CHECK_EQ(G.kinds, bays);
    for (k = 0; k < G.kinds; k++) CHECK(G.tile[G.src_y[k]][G.src_x[k]].t == TL_BAY);
    CHECK(G.tile[G.ship_y][G.ship_x].t == TL_SHIP);
  }
}

void test_forklift_shipping_the_wrong_thing_costs(void) {
  fresh_card();
  boot();
  G.norder = 1; G.order[0] = 1; G.order_size = 1;       /* wants blue */
  G.credits = 10;
  G.bot[0].x = G.ship_x; G.bot[0].y = G.ship_y;
  G.bot[0].held[0] = 0; G.bot[0].nheld = 1;             /* holds red */
  set_program("bot = drop");
  sim_step();
  CHECK_EQ(1, G.mistakes);
  CHECK_EQ(8, G.credits);
  CHECK_EQ(0, G.bot[0].nheld);
}

void test_forklift_a_finished_order_pays_a_bonus_and_brings_the_next(void) {
  int before;
  fresh_card();
  boot();
  G.norder = 1; G.order[0] = 2; G.order_size = 1;
  G.credits = 0;
  G.bot[0].x = G.ship_x; G.bot[0].y = G.ship_y;
  G.bot[0].held[0] = 2; G.bot[0].nheld = 1;
  before = G.orders;
  set_program("bot = drop");
  sim_step();
  CHECK_EQ(before + 1, G.orders);
  CHECK_EQ(3 + 2, G.credits);                  /* the item, and the bonus */
  CHECK(G.norder >= 1);                        /* the next order is in */
}

void test_forklift_a_runtime_error_pauses_and_says_where(void) {
  fresh_card();
  boot();
  set_program("want = first []\nbot = go (src want)");
  CHECK_EQ(1, G.running);
  sim_step();
  CHECK_EQ(1, G.paused);
  CHECK(strstr(G.err, "line 1"));
  CHECK(strstr(G.err, "empty list"));
  CHECK_EQ(1, G.err_line);
  CHECK_EQ(0, sim_step());                     /* paused: nothing moves */
}

void test_forklift_a_broken_edit_keeps_the_old_program(void) {
  fresh_card();
  boot();
  set_program("bot = go ship");
  set_program("bot = go (ship");
  CHECK_EQ(1, G.running);
  CHECK(strstr(G.err, "kept the old program"));
  G.bot[0].x = 2; G.bot[0].y = 1;
  G.paused = 0;
  sim_step();
  CHECK(G.bot[0].x == 3 || G.bot[0].y == 2);   /* still heading for the dock */
}

void test_forklift_every_robot_runs_the_program_with_its_own_me(void) {
  fresh_card();
  boot();
  G.credits = 1000;
  CHECK_EQ(0, buy(2));
  CHECK_EQ(2, G.nbots);
  G.bot[0].x = 5; G.bot[0].y = 3;
  G.bot[1].x = 5; G.bot[1].y = 3;
  set_program("bot = me == 0 ? go ship : go (src red)");
  sim_step();
  CHECK_EQ(6, G.bot[0].x);                     /* towards the dock */
  CHECK_EQ(4, G.bot[1].x);                     /* towards the bays */
}

/* ---- the shop ----------------------------------------------------------------- */

void test_forklift_the_shop_takes_credits_and_gives_upgrades(void) {
  fresh_card();
  boot();
  G.credits = 10;
  CHECK_EQ(-1, buy(0));                        /* forks are 20 */
  G.credits = 25;
  CHECK_EQ(0, buy(0));
  CHECK_EQ(2, cap_now());
  CHECK_EQ(5, G.credits);
  G.credits = 100000;
  CHECK_EQ(0, buy(0));
  CHECK_EQ(0, buy(0));
  CHECK_EQ(-1, buy(0));                        /* four is the most */
  CHECK_EQ(4, cap_now());
  CHECK_EQ(0, buy(3));                         /* a bigger warehouse */
  CHECK_EQ(14, G.w);
  CHECK_EQ(4, G.kinds);
}

/* ---- the editor ----------------------------------------------------------------- */

static void type(const char *s) { while (*s) ed_key((unsigned char)*s++); }

void test_forklift_brackets_close_themselves_and_go_as_a_pair(void) {
  fresh_card();
  boot();
  ed_from_text("");
  type("go (");
  CHECK(!strcmp(E.line[0], "go ()"));
  CHECK_EQ(4, E.cx);
  type("ship)");
  CHECK(!strcmp(E.line[0], "go (ship)"));      /* ) typed over the one there */
  ed_from_text("");
  type("(");
  ed_key(CAPP_KEY_BACK);
  CHECK(!strcmp(E.line[0], ""));
}

void test_forklift_tab_completes_names(void) {
  fresh_card();
  boot();
  ed_from_text("bot = hol");
  E.cy = 0; E.cx = 9;
  ed_key('\t');
  CHECK(!strcmp(E.line[0], "bot = holding "));
  /* Several: as far as they agree, and the list on the status line. */
  ed_from_text("bot = f");
  E.cx = 7;
  ed_key('\t');
  CHECK(strstr(E.status, "first") && strstr(E.status, "filter"));
  /* Nothing to finish: an indent. */
  ed_from_text("");
  ed_key('\t');
  CHECK(!strcmp(E.line[0], "  "));
}

void test_forklift_enter_indents_after_something_that_wants_more(void) {
  fresh_card();
  boot();
  ed_from_text("bot = empty holding ?");
  E.cx = (int)strlen(E.line[0]);
  ed_key(CAPP_KEY_ENTER);
  CHECK_EQ(2, E.nlines);
  CHECK(!strcmp(E.line[1], "  "));
  CHECK_EQ(2, E.cx);
  /* A plain line keeps its indent and no more. */
  ed_from_text("  go ship");
  E.cx = (int)strlen(E.line[0]);
  ed_key(CAPP_KEY_ENTER);
  CHECK(!strcmp(E.line[1], "  "));
}

void test_forklift_the_editor_checks_as_you_type(void) {
  fresh_card();
  boot();
  ed_from_text("");
  type("bot = go shp");
  CHECK_EQ(1, E.status_err);
  CHECK(strstr(E.status, "no such name: shp"));
  ed_key(CAPP_KEY_BACK);
  type("ip");
  CHECK_EQ(0, E.status_err);
}

void test_forklift_backspace_at_the_start_joins_lines(void) {
  fresh_card();
  boot();
  ed_from_text("bot = go\n ship");
  E.cy = 1; E.cx = 0;
  ed_key(CAPP_KEY_BACK);
  CHECK_EQ(1, E.nlines);
  CHECK(!strcmp(E.line[0], "bot = go ship"));
}

/* ---- saving ------------------------------------------------------------------------ */

void test_forklift_progress_and_program_survive_a_restart(void) {
  fresh_card();
  boot();
  G.credits = 1234;
  G.orders = 17;
  G.cap_lv = 2;
  G.size_lv = 1;
  set_program("bot = go ship  # mine");
  save_all();
  boot();                                      /* the same card */
  CHECK_EQ(1234, G.credits);
  CHECK_EQ(17, G.orders);
  CHECK_EQ(3, cap_now());
  CHECK_EQ(14, G.w);
  CHECK(strstr(E.line[0], "# mine"));
}

void test_forklift_a_fresh_card_starts_with_a_program_that_works(void) {
  fresh_card();
  boot();
  CHECK(strstr(E.good, "bot = "));
  CHECK_EQ(1, G.running);
  CHECK_EQ(0, G.credits);
}

/* ---- FORKLIFT_DUMP=dir: the screens as pictures, to look at ----------------- */

#include <stdlib.h>

static uint16_t FB[135][240];

static void d_fill(CRect r, uint16_t c) {
  int x, y;
  for (y = r.y; y < r.y + r.h; y++)
    for (x = r.x; x < r.x + r.w; x++)
      if (x >= 0 && y >= 0 && x < 240 && y < 135) FB[y][x] = c;
}
static void d_frame(CRect r, uint16_t c) {
  d_fill(rect(r.x, r.y, r.w, 1), c);
  d_fill(rect(r.x, r.y + r.h - 1, r.w, 1), c);
  d_fill(rect(r.x, r.y, 1, r.h), c);
  d_fill(rect(r.x + r.w - 1, r.y, 1, r.h), c);
}
/* Not a font: each character a block, so layout and colour can be seen. */
static void d_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  for (; *s; s++, x += 6) {
    d_fill(rect(x, y, 6, 8), bg);
    if (*s != ' ') d_fill(rect(x + 1, y + 1, 4, 6), fg);
  }
}

static void dump(const char *name) {
  const char *dir = getenv("FORKLIFT_DUMP");
  char path[256];
  FILE *f;
  int x, y;
  CRect all = { 0, 0, 240, 135 };
  if (!dir) return;
  FAKE.fill = d_fill;
  FAKE.frame = d_frame;
  FAKE.text = d_text;
  FAKE.paint_area = 0;
  U.full = 1;                    /* a view switch paints everything */
  app_paint(0, all);
  snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
  f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P6\n240 135\n255\n");
  for (y = 0; y < 135; y++)
    for (x = 0; x < 240; x++) {
      uint16_t v = FB[y][x], c = (uint16_t)((v >> 8) | (v << 8));
      unsigned char rgb[3];
      rgb[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
      rgb[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
      rgb[2] = (unsigned char)((c & 31) * 255 / 31);
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

void test_forklift_dump_screens(void) {
  fresh_card();
  boot();
  steps(7);
  U.view = VIEW_MAP; dump("map0");
  G.credits = 100000; buy(3); buy(0); buy(2); steps(11);
  dump("map1");
  buy(3); buy(2); steps(13);
  dump("map2");
  U.view = VIEW_CODE; ed_check(); dump("code");
  U.view = VIEW_SHOP; dump("shop");
  U.view = VIEW_REF; dump("ref");
  U.view = VIEW_MAP;
  CHECK(1);
}

/* ---- flicker: no pixel drawn twice in one paint ------------------------------
 *
 * There is no back buffer, so a pixel cleared and then drawn over is a flash
 * on the panel. The first version cleared the whole map every step. These
 * count writes per pixel through a paint and want one at most. */

static uint8_t WRITES[135][240];
static int OVERDRAWN;

static void w_px(int x, int y) {
  if (x < 0 || y < 0 || x >= 240 || y >= 135) return;
  if (++WRITES[y][x] == 2) OVERDRAWN++;
}
static void w_fill(CRect r, uint16_t c) {
  int x, y;
  (void)c;
  for (y = r.y; y < r.y + r.h; y++) for (x = r.x; x < r.x + r.w; x++) w_px(x, y);
}
static void w_frame(CRect r, uint16_t c) {
  int x, y;
  (void)c;
  for (x = r.x; x < r.x + r.w; x++) { w_px(x, r.y); w_px(x, r.y + r.h - 1); }
  for (y = r.y + 1; y < r.y + r.h - 1; y++) { w_px(r.x, y); w_px(r.x + r.w - 1, y); }
}
static void w_text(int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg) {
  (void)fg; (void)bg;
  for (; *s; s++, x += 6) w_fill(rect(x, y, 6, 8), 0);
}
static CRect PAINT_AREA;
static CRect w_area(void) { return PAINT_AREA; }
static CRect DAMAGE;
static int NDAMAGE;
static void w_damage(CRect r) {
  if (!NDAMAGE++) { DAMAGE = r; return; }
  {
    int x0 = DAMAGE.x < r.x ? DAMAGE.x : r.x, y0 = DAMAGE.y < r.y ? DAMAGE.y : r.y;
    int x1 = DAMAGE.x + DAMAGE.w > r.x + r.w ? DAMAGE.x + DAMAGE.w : r.x + r.w;
    int y1 = DAMAGE.y + DAMAGE.h > r.y + r.h ? DAMAGE.y + DAMAGE.h : r.y + r.h;
    DAMAGE = rect(x0, y0, x1 - x0, y1 - y0);
  }
}

/* One tick and the paint the shell would do for it: clipped to the union of
 * the damage, the way capprun unions it. Returns how many pixels were
 * written twice. */
static int tick_and_paint(void) {
  CRect all = { 0, 0, 240, 135 };
  memset(WRITES, 0, sizeof WRITES);
  OVERDRAWN = 0;
  NDAMAGE = 0;
  NOW += 600;
  if (app_tick(0, NOW) || NDAMAGE) {
    PAINT_AREA = NDAMAGE ? DAMAGE : all;
    app_paint(0, all);
  }
  return OVERDRAWN;
}

static void watch_writes(void) {
  FAKE.fill = w_fill;
  FAKE.frame = w_frame;
  FAKE.text = w_text;
  FAKE.damage = w_damage;
  FAKE.paint_area = w_area;
}

void test_forklift_the_map_never_draws_a_pixel_twice_in_a_step(void) {
  int i, worst = 0;
  CRect all = { 0, 0, 240, 135 };
  fresh_card();
  boot();
  watch_writes();
  PAINT_AREA = all;
  app_paint(0, all);                              /* the first, full paint */
  for (i = 0; i < 200; i++) {
    int o = tick_and_paint();
    if (o > worst) worst = o;
  }
  CHECK_EQ(0, worst);
  CHECK(G.orders >= 1);                           /* and it was doing things */
}

void test_forklift_a_bigger_warehouse_with_three_robots_does_not_flicker(void) {
  int i, worst = 0;
  CRect all = { 0, 0, 240, 135 };
  fresh_card();
  boot();
  G.credits = 100000;
  buy(3); buy(3); buy(2); buy(2); buy(0); buy(1);
  watch_writes();
  PAINT_AREA = all;
  U.full = 1;
  app_paint(0, all);
  for (i = 0; i < 200; i++) {
    int o = tick_and_paint();
    if (o > worst) worst = o;
  }
  CHECK_EQ(0, worst);
}

void test_forklift_typing_redraws_the_editor_without_clearing_it(void) {
  CRect all = { 0, 0, 240, 135 };
  fresh_card();
  boot();
  go_view(VIEW_CODE);
  watch_writes();
  PAINT_AREA = all;
  app_paint(0, all);                              /* entering the view */
  memset(WRITES, 0, sizeof WRITES);
  OVERDRAWN = 0;
  ed_key('x');
  app_paint(0, all);                              /* a key: a repaint, no damage */
  /* Only the cursor's one-pixel bar lands on a character already drawn. */
  CHECK(OVERDRAWN <= 8);
}
