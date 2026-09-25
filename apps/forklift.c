/* Forklift -- a warehouse you program. A demake of The Farmer Was Replaced.
 *
 * Orders come in ("red, red, blue"); a forklift robot fetches the kinds from
 * the bays down the left and ships them from the dock on the right. It does
 * whatever your program says: a few lines of Forklang (apps/forklang.h), a
 * small pure language, where `bot` is called every step and answers with an
 * action. Shipping what the order wants earns credits; shipping what it does
 * not costs a little. Credits buy forks that carry more, a faster motor, a
 * second and third robot running the same program, and a bigger warehouse
 * with racks to path round, more kinds, bigger orders and better pay. It
 * saves to the card and never ends.
 *
 * Four views. The warehouse (e code, s shop, r reference, space pause, f
 * fast). The code: a small editor made for this keyboard -- Tab completes
 * names, brackets close themselves, Enter indents, the program is checked as
 * you type and the first error is on the bottom line; Escape runs it if it
 * parses and keeps the old one if it does not. The shop. The reference:
 * every built-in, one line each, so nothing has to be remembered.
 *
 * A runtime error pauses the warehouse and says where; `e` goes straight to
 * the line. Design: docs/superpowers/specs/2026-09-25-forklift-design.md.
 */

#include "kernel/app/capp.h"
#include "apps/forklang.h"
#include "apps/safefile.h"

#define SCREEN_W  240
#define SCREEN_H  135
#define TOP_H     10
#define MAP_Y0    (TOP_H + 1)
#define MAP_Y1    111            /* the map lives above this */
#define BOTTOM_Y  112

#define MAX_W     20
#define MAX_H     10
#define MAX_BOTS  3

#define ED_LINES  48
#define ED_COLS   60
#define SRC_MAX   (ED_LINES * (ED_COLS + 1) + 1)
#define ROWH      9
#define CHARW     6
#define GUTTER    15

#define DIR        CAPP_HOME "/forklift"
#define STATE_PATH DIR "/state.txt"
#define CODE_PATH  DIR "/bot.fl"
#define SAVE_EVERY_MS 60000u

#define C_BG       CAPP_RGB(22, 24, 30)
#define C_BAR      CAPP_RGB(40, 62, 96)
#define C_BAR_FG   CAPP_RGB(234, 238, 246)
#define C_FLOOR    CAPP_RGB(58, 60, 66)
#define C_FLOOR2   CAPP_RGB(64, 66, 72)
#define C_RACK     CAPP_RGB(120, 84, 52)
#define C_RACK_HI  CAPP_RGB(156, 112, 70)
#define C_SHIP     CAPP_RGB(46, 120, 70)
#define C_SHIP_HI  CAPP_RGB(110, 200, 130)
#define C_TEXT     CAPP_RGB(220, 224, 232)
#define C_DIM      CAPP_RGB(130, 138, 152)
#define C_ERR      CAPP_RGB(255, 110, 100)
#define C_GOOD     CAPP_RGB(130, 220, 140)
#define C_SEL      CAPP_RGB(50, 78, 116)
#define C_ED_BG    CAPP_RGB(24, 26, 32)
#define C_ED_GUT   CAPP_RGB(34, 37, 45)
#define C_ED_NUM   CAPP_RGB(92, 102, 120)
#define C_ED_CUR   CAPP_RGB(120, 200, 255)
#define C_SYN_NUM  CAPP_RGB(240, 180, 90)
#define C_SYN_OP   CAPP_RGB(120, 210, 220)
#define C_SYN_PRIM CAPP_RGB(150, 170, 255)
#define C_SYN_CMT  CAPP_RGB(110, 120, 136)

static const uint16_t KIND_C[FL_KINDS] = {
  CAPP_RGB(230, 70, 60), CAPP_RGB(70, 130, 235), CAPP_RGB(80, 200, 100),
  CAPP_RGB(240, 210, 60), CAPP_RGB(235, 235, 240),
};
static const uint16_t BOT_C[MAX_BOTS] = {
  CAPP_RGB(250, 150, 40), CAPP_RGB(60, 210, 220), CAPP_RGB(240, 110, 200),
};

enum { TL_FLOOR = 0, TL_RACK, TL_BAY, TL_SHIP };
enum { VIEW_MAP = 0, VIEW_CODE, VIEW_SHOP, VIEW_REF };

typedef struct { uint8_t t, kind; } Tile;

typedef struct {
  int x, y;
  int held[FL_MAX_HOLD], nheld;
} Bot;

static const CardApi *api;

/* ---- the warehouse: everything the host tests drive ----------------------- */

static struct {
  int   w, h;
  Tile  tile[MAX_H][MAX_W];
  int   ship_x, ship_y;
  int   src_x[FL_KINDS], src_y[FL_KINDS];
  int   kinds;
  Bot   bot[MAX_BOTS];
  int   nbots;
  int   order[FL_MAX_ORDER], norder, order_size;

  int32_t credits;
  int   orders, shipped, mistakes;
  int   cap_lv, motor_lv, size_lv;        /* robots is nbots */
  uint32_t seed;

  FlProg prog;
  FlRun  run;
  int   running;                          /* the program parsed */
  int   paused;
  char  err[FL_ERR + 12];                 /* a runtime error, with its line */
  int   err_line;
  char  note[48];
  uint32_t note_until;
  int   dirty;                            /* worth saving */
} G;

static uint32_t rnd(void) {
  uint32_t x = G.seed ? G.seed : 1u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  G.seed = x;
  return x;
}

static int cap_now(void)   { return 1 + G.cap_lv; }
static int steps_per_s(void) { return 2 << G.motor_lv; }

/* Three warehouses, each bigger: more kinds, racks to path round, bigger
 * orders and better pay. Bays down the left, the dock on the right. */
static void layout(void) {
  int x, y, k;
  static const int W[3] = { 10, 14, 20 }, H[3] = { 6, 8, 10 }, K[3] = { 3, 4, 5 };
  G.w = W[G.size_lv];
  G.h = H[G.size_lv];
  G.kinds = K[G.size_lv];
  for (y = 0; y < MAX_H; y++)
    for (x = 0; x < MAX_W; x++) { G.tile[y][x].t = TL_FLOOR; G.tile[y][x].kind = 0; }

  /* Racks in columns, with an aisle through the middle and round the ends. */
  if (G.size_lv >= 1) {
    int step = G.size_lv == 1 ? 5 : 4;
    for (x = 4; x < G.w - 2; x += step)
      for (y = 1; y < G.h - 1; y++)
        if (y != G.h / 2 && y != G.h / 2 - 1) G.tile[y][x].t = TL_RACK;
  }
  for (k = 0; k < FL_KINDS; k++) { G.src_x[k] = -1; G.src_y[k] = -1; }
  for (k = 0; k < G.kinds; k++) {
    int by = 1 + 2 * k;
    if (by >= G.h) by = G.h - 1;
    G.tile[by][0].t = TL_BAY;
    G.tile[by][0].kind = (uint8_t)k;
    G.src_x[k] = 0;
    G.src_y[k] = by;
  }
  G.ship_x = G.w - 1;
  G.ship_y = G.h / 2;
  G.tile[G.ship_y][G.ship_x].t = TL_SHIP;
}

static void place_bots(void) {
  int i;
  for (i = 0; i < G.nbots; i++) {
    G.bot[i].x = 2;
    G.bot[i].y = (G.h / 2 + i) % G.h;
    G.bot[i].nheld = 0;
  }
}

static void new_order(void) {
  int i;
  G.order_size = 1 + (int)(rnd() % (unsigned)(2 + G.size_lv));
  if (G.order_size > FL_MAX_ORDER) G.order_size = FL_MAX_ORDER;
  for (i = 0; i < G.order_size; i++) G.order[i] = (int)(rnd() % (unsigned)G.kinds);
  G.norder = G.order_size;
}

static void note(const char *s) {
  api->fmt(G.note, sizeof G.note, "%s", s);
  G.note_until = api->ticks_ms() + 2500;
}

static int walkable(int x, int y) {
  return x >= 0 && y >= 0 && x < G.w && y < G.h && G.tile[y][x].t != TL_RACK;
}

/* One step from (x, y) towards (tx, ty) along a shortest path round the
 * racks: a breadth-first fill out from the target, then the neighbour that
 * is nearer. Stays put if there is no way there. */
static void step_towards(int x, int y, int tx, int ty, int *nx, int *ny) {
  static int16_t dist[MAX_H][MAX_W];
  static uint8_t qx[MAX_W * MAX_H], qy[MAX_W * MAX_H];
  static const int DX[4] = { 1, -1, 0, 0 }, DY[4] = { 0, 0, 1, -1 };
  int head = 0, tail = 0, i, d, best = -1;

  *nx = x;
  *ny = y;
  if (!walkable(tx, ty) || (x == tx && y == ty)) return;
  for (i = 0; i < MAX_H * MAX_W; i++) dist[i / MAX_W][i % MAX_W] = -1;
  dist[ty][tx] = 0;
  qx[tail] = (uint8_t)tx; qy[tail] = (uint8_t)ty; tail++;
  while (head < tail) {
    int cx = qx[head], cy = qy[head];
    head++;
    for (d = 0; d < 4; d++) {
      int ax = cx + DX[d], ay = cy + DY[d];
      if (!walkable(ax, ay) || dist[ay][ax] >= 0) continue;
      dist[ay][ax] = (int16_t)(dist[cy][cx] + 1);
      qx[tail] = (uint8_t)ax; qy[tail] = (uint8_t)ay; tail++;
    }
  }
  for (d = 0; d < 4; d++) {
    int ax = x + DX[d], ay = y + DY[d];
    if (!walkable(ax, ay) || dist[ay][ax] < 0) continue;
    if (best < 0 || dist[ay][ax] < best) { best = dist[ay][ax]; *nx = ax; *ny = ay; }
  }
}

static void world_for(int b, FlWorld *w) {
  int k;
  const Bot *r = &G.bot[b];
  w->me = b;
  w->x = r->x;
  w->y = r->y;
  w->cap = cap_now();
  w->nheld = r->nheld;
  for (k = 0; k < r->nheld; k++) w->held[k] = r->held[k];
  w->norder = G.norder;
  for (k = 0; k < G.norder; k++) w->order[k] = G.order[k];
  w->ship_x = G.ship_x;
  w->ship_y = G.ship_y;
  for (k = 0; k < FL_KINDS; k++) { w->src_x[k] = G.src_x[k]; w->src_y[k] = G.src_y[k]; }
}

/* Ship what the robot holds. What the order wants comes off it and pays;
 * anything else was a mistake and costs. A finished order pays a bonus and
 * the next one arrives. */
static void deliver(Bot *r) {
  int i, j, pay = 3 * (1 + G.size_lv), right = 0, wrong = 0;
  char m[48];
  for (i = 0; i < r->nheld; i++) {
    for (j = 0; j < G.norder; j++) if (G.order[j] == r->held[i]) break;
    if (j < G.norder) {
      G.order[j] = G.order[--G.norder];
      G.credits += pay;
      G.shipped++;
      right++;
    } else {
      G.credits -= 2;
      G.mistakes++;
      wrong++;
    }
  }
  r->nheld = 0;
  if (G.credits < 0) G.credits = 0;
  if (!G.norder) {
    int bonus = 2 * G.order_size * (1 + G.size_lv);
    G.credits += bonus;
    G.orders++;
    api->fmt(m, sizeof m, "order done! +%d", right * pay + bonus);
    note(m);
    new_order();
  } else if (wrong) {
    api->fmt(m, sizeof m, "not in the order: -%d", 2 * wrong);
    note(m);
  }
  G.dirty = 1;
}

/* One step: every robot runs the program once and does what it says.
 * Returns 1 if anything changed. A program error pauses everything. */
static int sim_step(void) {
  int b;
  if (!G.running || G.paused) return 0;
  for (b = 0; b < G.nbots; b++) {
    FlWorld w;
    FlAction a;
    Bot *r = &G.bot[b];
    world_for(b, &w);
    if (fl_run(&G.run, &G.prog, &w, &a) != 0) {
      api->fmt(G.err, sizeof G.err, "line %d: %s", G.run.err_line, G.run.err);
      G.err_line = G.run.err_line;
      G.paused = 1;
      return 1;
    }
    switch (a.act) {
    case FL_ACT_GO: {
      int nx, ny;
      step_towards(r->x, r->y, a.x, a.y, &nx, &ny);
      r->x = nx;
      r->y = ny;
      break;
    }
    case FL_ACT_TAKE:
      if (G.tile[r->y][r->x].t != TL_BAY || G.tile[r->y][r->x].kind != a.kind) {
        char m[48];
        api->fmt(m, sizeof m, "robot %d: no %s bay here", b + 1, FL_KIND_NAME[a.kind]);
        note(m);
      } else if (r->nheld >= cap_now()) {
        note("the forks are full");
      } else {
        r->held[r->nheld++] = a.kind;
      }
      break;
    case FL_ACT_DROP:
      if (G.tile[r->y][r->x].t == TL_SHIP) deliver(r);
      else note("drop only ships at the dock");
      break;
    default:
      break;
    }
  }
  return 1;
}

/* ---- the shop -------------------------------------------------------------- */

typedef struct { const char *name, *about; int max; int cost[3]; } Upgrade;

static const Upgrade SHOP[] = {
  { "Forks",     "carry one more crate",             3, { 20, 60, 150 } },
  { "Motor",     "twice as many steps a second",     3, { 15, 50, 140 } },
  { "Robot",     "another robot, same program; me",  2, { 120, 400, 0 } },
  { "Warehouse", "bigger: racks, kinds, better pay", 2, { 80, 300, 0 } },
};
#define NSHOP ((int)(sizeof SHOP / sizeof SHOP[0]))

static int shop_level(int i) {
  switch (i) {
  case 0: return G.cap_lv;
  case 1: return G.motor_lv;
  case 2: return G.nbots - 1;
  default: return G.size_lv;
  }
}

/* Buy upgrade i. 0, or -1 (maxed, or not enough credits). */
static int buy(int i) {
  int lv = shop_level(i);
  if (lv >= SHOP[i].max || G.credits < SHOP[i].cost[lv]) return -1;
  G.credits -= SHOP[i].cost[lv];
  switch (i) {
  case 0: G.cap_lv++; break;
  case 1: G.motor_lv++; break;
  case 2:
    G.bot[G.nbots].x = 2;
    G.bot[G.nbots].y = (G.h / 2 + G.nbots) % G.h;
    G.bot[G.nbots].nheld = 0;
    G.nbots++;
    break;
  default:
    G.size_lv++;
    layout();
    place_bots();
    new_order();
    break;
  }
  G.dirty = 1;
  return 0;
}

/* ---- the program's text --------------------------------------------------------- */

static const char DEFAULT_PROGRAM[] =
  "# bot runs every step and gives an action:\n"
  "#   go p, take k, drop, wait\n"
  "# r shows everything built in\n"
  "want = first order\n"
  "bot = empty holding\n"
  "  ? (at (src want) ? take want : go (src want))\n"
  "  : (at ship ? drop : go ship)\n";

static struct {
  char line[ED_LINES][ED_COLS + 1];
  int  nlines;
  int  cx, cy, top, left;
  char src[SRC_MAX];              /* the lines joined, for the parser */
  char good[SRC_MAX];             /* the last program that parsed */
  char status[64];
  int  status_err;
  int  changed;
} E;

static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static void ed_from_text(const char *s) {
  int col = 0;
  api->mem_set(E.line, 0, sizeof E.line);
  E.nlines = 1;
  for (; *s; s++) {
    if (*s == '\n') {
      if (E.nlines >= ED_LINES) break;
      E.nlines++;
      col = 0;
      continue;
    }
    if (*s == '\r') continue;
    if (*s == '\t') { if (col < ED_COLS - 1) { E.line[E.nlines - 1][col++] = ' '; E.line[E.nlines - 1][col++] = ' '; } continue; }
    if (col < ED_COLS) E.line[E.nlines - 1][col++] = *s;
  }
  while (E.nlines > 1 && !E.line[E.nlines - 1][0]) E.nlines--;
  E.cx = E.cy = E.top = E.left = 0;
}

static void ed_to_text(char *out) {
  int i, o = 0, n;
  for (i = 0; i < E.nlines; i++) {
    n = str_len(E.line[i]);
    if (o + n + 2 >= SRC_MAX) break;
    api->mem_cpy(out + o, E.line[i], (size_t)n);
    o += n;
    out[o++] = '\n';
  }
  out[o] = 0;
}

/* Parse the editor's text as the program. 0, or -1 with the error in the
 * status line. The warehouse is paused while the editor is up, so the
 * program it runs can be replaced as you type and put back on Escape. */
static int ed_check(void) {
  ed_to_text(E.src);
  if (fl_parse(&G.prog, E.src) != 0) {
    api->fmt(E.status, sizeof E.status, "line %d: %s", G.prog.err_line, G.prog.err);
    E.status_err = 1;
    return -1;
  }
  api->fmt(E.status, sizeof E.status, "ok -- %d definition%s. esc runs it",
           G.prog.ndef, G.prog.ndef == 1 ? "" : "s");
  E.status_err = 0;
  return 0;
}

/* ---- saving ------------------------------------------------------------------------ */

static void save_all(void) {
  SafeFile f;
  char line[48];
  if (!api->mkdir || !api->open) return;
  api->mkdir(DIR);
  if (safe_begin(&f, api, STATE_PATH) == 0) {
    api->fmt(line, sizeof line, "credits=%ld\n", (long)G.credits);      safe_line(&f, line);
    api->fmt(line, sizeof line, "orders=%d\n", G.orders);               safe_line(&f, line);
    api->fmt(line, sizeof line, "shipped=%d\n", G.shipped);             safe_line(&f, line);
    api->fmt(line, sizeof line, "mistakes=%d\n", G.mistakes);           safe_line(&f, line);
    api->fmt(line, sizeof line, "forks=%d\n", G.cap_lv);                safe_line(&f, line);
    api->fmt(line, sizeof line, "motor=%d\n", G.motor_lv);              safe_line(&f, line);
    api->fmt(line, sizeof line, "robots=%d\n", G.nbots);                safe_line(&f, line);
    api->fmt(line, sizeof line, "warehouse=%d\n", G.size_lv);           safe_line(&f, line);
    safe_commit(&f);
  }
  if (safe_begin(&f, api, CODE_PATH) == 0) {
    safe_line(&f, E.good);
    safe_commit(&f);
  }
  G.dirty = 0;
}

static int read_file(const char *path, char *buf, int size) {
  int fd = safe_open_read(api, path), n, got = 0;
  if (fd < 0) return -1;
  while (got < size - 1 && (n = api->read(fd, buf + got, (size_t)(size - 1 - got))) > 0) got += n;
  api->close(fd);
  buf[got] = 0;
  return got;
}

static int starts(const char *s, const char *w) {
  while (*w) if (*s++ != *w++) return 0;
  return 1;
}

static int num_after(const char *s) {
  int v = 0, neg = 0;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return neg ? -v : v;
}

static void load_all(void) {
  static char buf[256];
  const char *p;
  G.nbots = 1;
  if (read_file(STATE_PATH, buf, sizeof buf) > 0) {
    for (p = buf; *p; ) {
      if (starts(p, "credits="))   G.credits = num_after(p + 8);
      if (starts(p, "orders="))    G.orders = num_after(p + 7);
      if (starts(p, "shipped="))   G.shipped = num_after(p + 8);
      if (starts(p, "mistakes="))  G.mistakes = num_after(p + 9);
      if (starts(p, "forks="))     G.cap_lv = num_after(p + 6);
      if (starts(p, "motor="))     G.motor_lv = num_after(p + 6);
      if (starts(p, "robots="))    G.nbots = num_after(p + 7);
      if (starts(p, "warehouse=")) G.size_lv = num_after(p + 10);
      while (*p && *p != '\n') p++;
      if (*p) p++;
    }
  }
  /* A hand-edited file cannot put the game out of its own range. */
  if (G.cap_lv < 0 || G.cap_lv > 3) G.cap_lv = 0;
  if (G.motor_lv < 0 || G.motor_lv > 3) G.motor_lv = 0;
  if (G.nbots < 1 || G.nbots > MAX_BOTS) G.nbots = 1;
  if (G.size_lv < 0 || G.size_lv > 2) G.size_lv = 0;
  if (G.credits < 0) G.credits = 0;

  if (read_file(CODE_PATH, E.src, SRC_MAX) <= 0)
    api->fmt(E.src, SRC_MAX, "%s", DEFAULT_PROGRAM);
  ed_from_text(E.src);
}

/* Start running what is in the editor, or say why not. */
static void apply_program(void) {
  if (ed_check() == 0) {
    ed_to_text(E.good);
    G.running = 1;
    G.err[0] = 0;
    G.err_line = 0;
    G.paused = 0;
    G.dirty = 1;
  } else {
    /* Keep the one that worked, if there was one. */
    char why[64];
    api->fmt(why, sizeof why, "%s", E.status);
    if (E.good[0] && fl_parse(&G.prog, E.good) == 0) {
      G.running = 1;
      api->fmt(G.err, sizeof G.err, "kept the old program. %s", why);
    } else {
      G.running = 0;
      api->fmt(G.err, sizeof G.err, "%s", why);
    }
    G.err_line = G.prog.err_line;
  }
}

/* ---- the editor's keys -------------------------------------------------------------- */

static int is_word(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '_' || c == '\'';
}

static void ed_insert(const char *s) {
  char *l = E.line[E.cy];
  int n = str_len(l), k = str_len(s), i;
  if (n + k > ED_COLS) return;
  for (i = n; i >= E.cx; i--) l[i + k] = l[i];
  for (i = 0; i < k; i++) l[E.cx + i] = s[i];
  E.cx += k;
  E.changed = 1;
}

static void ed_newline(void) {
  char *l = E.line[E.cy];
  int indent = 0, i, n = str_len(l);
  char tail[ED_COLS + 1];
  if (E.nlines >= ED_LINES) return;
  while (l[indent] == ' ') indent++;
  /* After something that wants more, indent further. */
  {
    int e = E.cx;
    while (e > 0 && l[e - 1] == ' ') e--;
    if (e > 0 && (l[e - 1] == '?' || l[e - 1] == ':' || l[e - 1] == '(' ||
                  l[e - 1] == '=' || (e > 1 && l[e - 2] == '-' && l[e - 1] == '>')))
      indent += 2;
  }
  if (indent > ED_COLS / 2) indent = ED_COLS / 2;
  api->fmt(tail, sizeof tail, "%s", l + E.cx);
  l[E.cx] = 0;
  (void)n;
  for (i = E.nlines; i > E.cy + 1; i--) api->mem_cpy(E.line[i], E.line[i - 1], ED_COLS + 1);
  E.nlines++;
  E.cy++;
  api->mem_set(E.line[E.cy], 0, ED_COLS + 1);
  for (i = 0; i < indent; i++) E.line[E.cy][i] = ' ';
  {
    char *t = tail;
    while (*t == ' ' && indent) t++;
    if (str_len(t) + indent <= ED_COLS) api->mem_cpy(E.line[E.cy] + indent, t, (size_t)str_len(t) + 1);
  }
  E.cx = indent;
  E.changed = 1;
}

static void ed_backspace(void) {
  char *l = E.line[E.cy];
  int i, n = str_len(l);
  if (E.cx > 0) {
    /* A pair typed as a pair goes as a pair. */
    int pair = (l[E.cx - 1] == '(' && l[E.cx] == ')') || (l[E.cx - 1] == '[' && l[E.cx] == ']');
    for (i = E.cx - 1; i < n; i++) l[i] = l[i + 1];
    E.cx--;
    if (pair) for (i = E.cx; i < n; i++) l[i] = l[i + 1];
  } else if (E.cy > 0) {
    char *p = E.line[E.cy - 1];
    int pn = str_len(p);
    if (pn + n > ED_COLS) return;
    api->mem_cpy(p + pn, l, (size_t)n + 1);
    for (i = E.cy; i + 1 < E.nlines; i++) api->mem_cpy(E.line[i], E.line[i + 1], ED_COLS + 1);
    E.nlines--;
    E.cy--;
    E.cx = pn;
  }
  E.changed = 1;
}

/* Tab: finish the name under the cursor. One match finishes it; several
 * finish as far as they agree and are listed; no name to finish indents. */
static void ed_complete(void) {
  char *l = E.line[E.cy];
  int s = E.cx, plen, i, n = 0, common = 0;
  const char *name, *first = 0;
  char pre[FL_SYM_LEN + 1], list[64];
  int lo = 0;

  while (s > 0 && is_word(l[s - 1])) s--;
  plen = E.cx - s;
  if (plen == 0) { ed_insert("  "); return; }
  if (plen > FL_SYM_LEN) return;
  api->mem_cpy(pre, l + s, (size_t)plen);
  pre[plen] = 0;

  list[0] = 0;
  for (i = 0; (name = fl_name_at(&G.prog, i)) != 0; i++) {
    int k;
    if (!starts(name, pre)) continue;
    if (!first) { first = name; common = str_len(name); }
    else {
      for (k = 0; k < common && name[k] == first[k]; k++) { }
      common = k;
    }
    n++;
    if (lo + str_len(name) + 2 < (int)sizeof list) {
      lo += api->fmt(list + lo, sizeof list - (size_t)lo, "%s ", name);
    }
  }
  if (!n) {
    api->fmt(E.status, sizeof E.status, "nothing starts with %s", pre);
    E.status_err = 0;
    return;
  }
  if (common > plen) {
    char add[FL_SYM_LEN + 2];
    int k;
    for (k = 0; k < common - plen && k < FL_SYM_LEN; k++) add[k] = first[plen + k];
    add[k] = 0;
    if (n == 1 && k < FL_SYM_LEN) { add[k] = ' '; add[k + 1] = 0; }
    ed_insert(add);
  }
  if (n > 1) {
    api->fmt(E.status, sizeof E.status, "%s", list);
    E.status_err = 0;
  }
}

static int ed_key(unsigned char k) {
  char *l = E.line[E.cy];
  int n = str_len(l);
  E.changed = 0;
  switch (k) {
  case CAPP_KEY_UP:    if (E.cy > 0) E.cy--; break;
  case CAPP_KEY_DOWN:  if (E.cy + 1 < E.nlines) E.cy++; break;
  case CAPP_KEY_LEFT:
    if (E.cx > 0) E.cx--;
    else if (E.cy > 0) { E.cy--; E.cx = str_len(E.line[E.cy]); }
    break;
  case CAPP_KEY_RIGHT:
    if (E.cx < n) E.cx++;
    else if (E.cy + 1 < E.nlines) { E.cy++; E.cx = 0; }
    break;
  case CAPP_KEY_ENTER: ed_newline(); break;
  case CAPP_KEY_BACK:  ed_backspace(); break;
  case '\t':           ed_complete(); break;
  case 0x01:           E.cx = 0; break;                  /* ctrl-a */
  case 0x05:           E.cx = n; break;                  /* ctrl-e */
  case '(':            ed_insert("()"); E.cx--; break;
  case '[':            ed_insert("[]"); E.cx--; break;
  case ')': case ']':
    if (l[E.cx] == (char)k) { E.cx++; break; }        /* over the one already there */
    { char s[2] = { (char)k, 0 }; ed_insert(s); }
    break;
  default:
    if (k >= 32 && k < 127) { char s[2] = { (char)k, 0 }; ed_insert(s); }
    else return 0;
  }
  if (E.cx > str_len(E.line[E.cy])) E.cx = str_len(E.line[E.cy]);
  if (E.changed) ed_check();
  return 1;
}

/* ---- painting ----------------------------------------------------------------------- */

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static struct {
  int view;
  CRect c;
  int shop_sel, ref_top;
  int fast;
  uint32_t last_ms, acc_ms, last_save;
  int prev_x[MAX_BOTS], prev_y[MAX_BOTS];
  int32_t shown_credits;
  int shown_norder, shown_orders;
} U;

static int tile_px(void) {
  int t = SCREEN_W / G.w, t2 = (MAP_Y1 - MAP_Y0) / G.h;
  return t < t2 ? t : t2;
}
static int map_x0(void) { return (SCREEN_W - G.w * tile_px()) / 2; }
static int map_y0(void) { return MAP_Y0 + ((MAP_Y1 - MAP_Y0) - G.h * tile_px()) / 2; }

static CRect tile_rect(int x, int y) {
  int t = tile_px();
  return rect(U.c.x + map_x0() + x * t, U.c.y + map_y0() + y * t, t, t);
}

static void paint_tile(int x, int y) {
  CRect r = tile_rect(x, y);
  const Tile *t = &G.tile[y][x];
  int s = r.w, b;
  switch (t->t) {
  case TL_RACK:
    api->fill(r, C_RACK);
    api->fill(rect(r.x, r.y + s / 3, s, 1), C_RACK_HI);
    api->fill(rect(r.x, r.y + 2 * s / 3, s, 1), C_RACK_HI);
    break;
  case TL_BAY:
    api->fill(r, C_FLOOR);
    api->frame(r, KIND_C[t->kind]);
    api->fill(rect(r.x + s / 4, r.y + s / 4, s - s / 2, s - s / 2), KIND_C[t->kind]);
    break;
  case TL_SHIP:
    api->fill(r, C_SHIP);
    api->frame(r, C_SHIP_HI);
    if (s >= 10) api->text((short)(r.x + (s - 6) / 2), (short)(r.y + (s - 8) / 2), "S", C_SHIP_HI, C_SHIP);
    break;
  default:
    api->fill(r, ((x + y) & 1) ? C_FLOOR : C_FLOOR2);
  }
  /* Any robot standing here, over the tile. */
  for (b = 0; b < G.nbots; b++) {
    const Bot *bt = &G.bot[b];
    int i, in = s > 10 ? 2 : 1, cs;
    if (bt->x != x || bt->y != y) continue;
    api->fill(rect(r.x + in, r.y + in, s - 2 * in, s - 2 * in), BOT_C[b]);
    cs = (s - 2 * in - 2) / 2;
    if (cs < 2) cs = 2;
    for (i = 0; i < bt->nheld; i++)
      api->fill(rect(r.x + in + 1 + (i & 1) * cs, r.y + in + 1 + (i >> 1) * cs,
                     cs - 1, cs - 1), KIND_C[bt->held[i]]);
  }
}

static void paint_top(void) {
  char s[48];
  api->fill(rect(U.c.x, U.c.y, SCREEN_W, TOP_H), C_BAR);
  api->fmt(s, sizeof s, "$%ld  orders %d  %d/s%s", (long)G.credits, G.orders,
           steps_per_s() * (U.fast ? 4 : 1), U.fast ? " fast" : "");
  api->text((short)(U.c.x + 2), (short)(U.c.y + 1), s, C_BAR_FG, C_BAR);
  api->text((short)(U.c.x + SCREEN_W - 38), (short)(U.c.y + 1),
            G.paused ? "paused" : G.running ? "" : "no prog", C_BAR_FG, C_BAR);
  U.shown_credits = G.credits;
  U.shown_orders = G.orders;
}

static void paint_bottom(void) {
  int i, x;
  const char *msg;
  uint16_t fg = C_DIM;
  api->fill(rect(U.c.x, U.c.y + BOTTOM_Y, SCREEN_W, SCREEN_H - BOTTOM_Y), C_BG);
  api->text((short)(U.c.x + 2), (short)(U.c.y + BOTTOM_Y + 2), "order", C_DIM, C_BG);
  x = U.c.x + 36;
  for (i = 0; i < G.norder; i++) {
    api->fill(rect(x, U.c.y + BOTTOM_Y + 2, 8, 8), KIND_C[G.order[i]]);
    x += 10;
  }
  for (i = G.norder; i < G.order_size; i++) {
    api->frame(rect(x, U.c.y + BOTTOM_Y + 2, 8, 8), C_DIM);    /* done */
    x += 10;
  }
  if (G.err[0]) { msg = G.err; fg = C_ERR; }
  else if (G.note[0] && (int32_t)(G.note_until - api->ticks_ms()) > 0) { msg = G.note; fg = C_GOOD; }
  else msg = "e code s shop r help spc pause f fast";
  api->text((short)(U.c.x + 2), (short)(U.c.y + BOTTOM_Y + 13), msg, fg, C_BG);
  U.shown_norder = G.norder;
}

static void paint_map(void) {
  int x, y;
  api->fill(rect(U.c.x, U.c.y + TOP_H, SCREEN_W, BOTTOM_Y - TOP_H), C_BG);
  paint_top();
  for (y = 0; y < G.h; y++)
    for (x = 0; x < G.w; x++) paint_tile(x, y);
  paint_bottom();
}

static uint16_t syn_colour(const char *l, int i, int *in_comment) {
  char c = l[i];
  if (*in_comment || c == '#') { *in_comment = 1; return C_SYN_CMT; }
  if (c >= '0' && c <= '9' && (i == 0 || !is_word(l[i - 1]) || (l[i - 1] >= '0' && l[i - 1] <= '9')))
    return C_SYN_NUM;
  if (c == '?' || c == ':' || c == '-' || c == '>' || c == '=' || c == '<' || c == '!' ||
      c == '&' || c == '|' || c == '+' || c == '*' || c == '/' || c == '%')
    return C_SYN_OP;
  return C_TEXT;
}

static void paint_code(void) {
  int rows = (SCREEN_H - TOP_H - ROWH - 1) / ROWH, cols = (SCREEN_W - GUTTER) / CHARW, r;
  char s[64];

  if (E.cy < E.top) E.top = E.cy;
  if (E.cy >= E.top + rows) E.top = E.cy - rows + 1;
  if (E.cx < E.left) E.left = E.cx;
  if (E.cx >= E.left + cols) E.left = E.cx - cols + 1;

  api->fill(rect(U.c.x, U.c.y, SCREEN_W, TOP_H), C_BAR);
  api->fmt(s, sizeof s, "code  tab completes  esc runs");
  api->text((short)(U.c.x + 2), (short)(U.c.y + 1), s, C_BAR_FG, C_BAR);

  for (r = 0; r < rows; r++) {
    int ln = E.top + r, y = U.c.y + TOP_H + 1 + r * ROWH, i, cmt = 0, x;
    char num[4];
    api->fill(rect(U.c.x, y, GUTTER, ROWH), C_ED_GUT);
    api->fill(rect(U.c.x + GUTTER, y, SCREEN_W - GUTTER, ROWH), C_ED_BG);
    if (ln >= E.nlines) continue;
    api->fmt(num, sizeof num, "%2d", ln + 1);
    api->text((short)(U.c.x + 1), (short)(y + 1), num,
              (E.status_err && G.prog.err_line == ln + 1) ? C_ERR : C_ED_NUM, C_ED_GUT);
    /* Colour each character, then draw runs of one colour with one call:
     * a call a character made typing lag. */
    {
      const char *l = E.line[ln];
      uint16_t col[ED_COLS + 1];
      char run[ED_COLS + 1];
      int n = str_len(l), from, to, rl = 0, rx = 0;
      for (i = 0; i < n; i++) {
        col[i] = syn_colour(l, i, &cmt);
        if (!cmt && is_word(l[i]) && !(l[i] >= '0' && l[i] <= '9') &&
            (i == 0 || !is_word(l[i - 1]))) {
          char w[FL_SYM_LEN + 1];
          int k = 0, j;
          while (is_word(l[i + k]) && k < FL_SYM_LEN) { w[k] = l[i + k]; k++; }
          w[k] = 0;
          while (is_word(l[i + k])) k++;
          for (j = 0; j < k; j++)
            col[i + j] = fl_prim_index(w) >= 0 ? C_SYN_PRIM : C_TEXT;
          i += k - 1;
        }
      }
      from = E.left;
      to = n < E.left + cols ? n : E.left + cols;
      for (i = from; i <= to; i++) {
        if (rl && (i == to || col[i] != col[i - 1])) {
          run[rl] = 0;
          api->text((short)(U.c.x + GUTTER + rx * CHARW), (short)(y + 1), run,
                    col[i - 1], C_ED_BG);
          rl = 0;
        }
        if (i == to) break;
        if (!rl) rx = i - E.left;
        run[rl++] = l[i];
      }
      (void)x;
    }
    if (ln == E.cy) {
      int cx = E.cx - E.left;
      api->fill(rect(U.c.x + GUTTER + cx * CHARW, y, 1, ROWH), C_ED_CUR);
    }
  }
  {
    int y = U.c.y + SCREEN_H - ROWH - 1;
    api->fill(rect(U.c.x, y, SCREEN_W, ROWH + 1), C_BG);
    api->text((short)(U.c.x + 2), (short)(y + 1), E.status, E.status_err ? C_ERR : C_GOOD, C_BG);
  }
}

static void paint_shop(void) {
  int i;
  char s[48];
  api->fill(rect(U.c.x, U.c.y, SCREEN_W, SCREEN_H), C_BG);
  api->fill(rect(U.c.x, U.c.y, SCREEN_W, TOP_H), C_BAR);
  api->fmt(s, sizeof s, "shop  $%ld  enter buys  esc back", (long)G.credits);
  api->text((short)(U.c.x + 2), (short)(U.c.y + 1), s, C_BAR_FG, C_BAR);
  for (i = 0; i < NSHOP; i++) {
    int y = U.c.y + TOP_H + 4 + i * 22, lv = shop_level(i), j;
    uint16_t bg = i == U.shop_sel ? C_SEL : C_BG;
    api->fill(rect(U.c.x + 2, y, SCREEN_W - 4, 20), bg);
    api->text((short)(U.c.x + 6), (short)(y + 2), SHOP[i].name, C_TEXT, bg);
    for (j = 0; j < SHOP[i].max; j++)
      api->fill(rect(U.c.x + 70 + j * 8, y + 3, 6, 6), j < lv ? C_GOOD : C_ED_NUM);
    if (lv >= SHOP[i].max) api->fmt(s, sizeof s, "max");
    else api->fmt(s, sizeof s, "$%d", SHOP[i].cost[lv]);
    api->text((short)(U.c.x + SCREEN_W - 44), (short)(y + 2), s,
              lv < SHOP[i].max && G.credits >= SHOP[i].cost[lv] ? C_GOOD : C_DIM, bg);
    api->text((short)(U.c.x + 6), (short)(y + 11), SHOP[i].about, C_DIM, bg);
  }
}

/* The reference: syntax, then every built-in with its line. */
static const char *const REF_HEAD[] = {
  "name args = expr   a definition",
  "  (a line starting with space goes on)",
  "c ? a : b   x -> e   f x y   (x, y)",
  "[a, b]   + - * / %   == != < > <= >=",
  "&& || !   # comment",
  "",
};
#define NREF_HEAD ((int)(sizeof REF_HEAD / sizeof REF_HEAD[0]))

static void paint_ref(void) {
  int rows = (SCREEN_H - TOP_H - 2) / ROWH, r, total = NREF_HEAD + P_COUNT_OF_PRIMS;
  if (U.ref_top > total - rows) U.ref_top = total - rows;
  if (U.ref_top < 0) U.ref_top = 0;
  api->fill(rect(U.c.x, U.c.y, SCREEN_W, SCREEN_H), C_BG);
  api->fill(rect(U.c.x, U.c.y, SCREEN_W, TOP_H), C_BAR);
  api->text((short)(U.c.x + 2), (short)(U.c.y + 1), "reference  up/down  esc back", C_BAR_FG, C_BAR);
  for (r = 0; r < rows; r++) {
    int i = U.ref_top + r, y = U.c.y + TOP_H + 2 + r * ROWH;
    if (i >= total) break;
    if (i < NREF_HEAD) {
      api->text((short)(U.c.x + 2), (short)y, REF_HEAD[i], C_SYN_OP, C_BG);
    } else {
      const FlPrim *p = &FL_PRIMS[i - NREF_HEAD];
      api->text((short)(U.c.x + 2), (short)y, p->name, C_SYN_PRIM, C_BG);
      api->text((short)(U.c.x + 50), (short)y, p->about, C_TEXT, C_BG);
    }
  }
}

static void app_paint(void *st, CRect c) {
  (void)st;
  U.c = c;
  switch (U.view) {
  case VIEW_CODE: paint_code(); break;
  case VIEW_SHOP: paint_shop(); break;
  case VIEW_REF:  paint_ref(); break;
  default:        paint_map(); break;
  }
}

/* ---- the shell's callbacks -------------------------------------------------------------- */

static void damage_tile(int x, int y) {
  CRect r = tile_rect(x, y);
  r.x = (short)(r.x - U.c.x);
  r.y = (short)(r.y - U.c.y);
  api->damage(r);
}

static void remember_bots(void) {
  int b;
  for (b = 0; b < G.nbots; b++) { U.prev_x[b] = G.bot[b].x; U.prev_y[b] = G.bot[b].y; }
}

static int app_tick(void *st, uint32_t now) {
  uint32_t dt, per;
  int steps = 0, b, changed = 0;
  (void)st;

  if (!U.last_ms) { U.last_ms = now; U.last_save = now; }
  dt = now - U.last_ms;
  U.last_ms = now;
  if (dt > 200) dt = 200;

  if (G.dirty && now - U.last_save > SAVE_EVERY_MS) { save_all(); U.last_save = now; }

  if (U.view != VIEW_MAP) return 0;
  U.acc_ms += dt;
  per = 1000u / (uint32_t)(steps_per_s() * (U.fast ? 4 : 1));
  if (per < 1) per = 1;
  remember_bots();
  while (U.acc_ms >= per && steps < 8) {
    U.acc_ms -= per;
    if (sim_step()) changed = 1;
    steps++;
  }
  if (U.acc_ms > per * 8) U.acc_ms = 0;          /* never run to catch up */

  if (changed) {
    for (b = 0; b < G.nbots; b++) {
      damage_tile(U.prev_x[b], U.prev_y[b]);
      damage_tile(G.bot[b].x, G.bot[b].y);
    }
  }
  if (G.credits != U.shown_credits || G.orders != U.shown_orders || changed)
    api->damage(rect(0, 0, SCREEN_W, TOP_H));
  if (G.norder != U.shown_norder || changed)
    api->damage(rect(0, BOTTOM_Y, SCREEN_W, SCREEN_H - BOTTOM_Y));
  return changed;
}

static void go_view(int v) {
  if (U.view == VIEW_CODE && v != VIEW_CODE) apply_program();
  U.view = v;
  if (v == VIEW_CODE) {
    ed_check();
    if (G.err_line > 0 && G.err_line <= E.nlines) { E.cy = G.err_line - 1; E.cx = 0; }
  }
  if (v != VIEW_MAP && G.dirty) save_all();
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (U.view == VIEW_CODE) {
    if (k == CAPP_KEY_ESC) { go_view(VIEW_MAP); return 1; }
    return ed_key(k);
  }
  if (U.view == VIEW_SHOP) {
    switch (k) {
    case CAPP_KEY_UP:   if (U.shop_sel > 0) U.shop_sel--; return 1;
    case CAPP_KEY_DOWN: if (U.shop_sel + 1 < NSHOP) U.shop_sel++; return 1;
    case CAPP_KEY_ENTER:
      if (buy(U.shop_sel) == 0) { note("bought!"); save_all(); }
      return 1;
    case CAPP_KEY_ESC: case 's': case 'S': go_view(VIEW_MAP); return 1;
    default: return 0;
    }
  }
  if (U.view == VIEW_REF) {
    switch (k) {
    case CAPP_KEY_UP:    U.ref_top--; return 1;
    case CAPP_KEY_DOWN:  U.ref_top++; return 1;
    case CAPP_KEY_LEFT:  U.ref_top -= 8; return 1;
    case CAPP_KEY_RIGHT: case ' ': U.ref_top += 8; return 1;
    case CAPP_KEY_ESC: case 'r': case 'R': go_view(VIEW_MAP); return 1;
    default: return 0;
    }
  }
  switch (k) {
  case 'e': case 'E': case CAPP_KEY_ENTER: go_view(VIEW_CODE); return 1;
  case 's': case 'S': go_view(VIEW_SHOP); return 1;
  case 'r': case 'R': case '?': go_view(VIEW_REF); return 1;
  case ' ':
    if (G.err[0] && G.running) { G.err[0] = 0; G.err_line = 0; }
    G.paused = !G.paused;
    return 1;
  case 'f': case 'F': U.fast = !U.fast; return 1;
  case CAPP_KEY_ESC: return 0;                  /* the top: the shell's */
  default: return 0;
  }
}

static int app_wants_text(void *st) {
  (void)st;
  return U.view == VIEW_CODE;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Forklift",
  /* 16x16: a forklift with a crate on its forks. */
  { 0x00, 0x00, 0x00, 0x00, 0x70, 0x00, 0x50, 0x00,
    0x50, 0x3C, 0x5F, 0x3C, 0x51, 0x3C, 0x7F, 0x3C,
    0x7F, 0x20, 0x7F, 0x20, 0x7F, 0x3F, 0x00, 0x00,
    0x36, 0x00, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "e\tedit the program (esc runs it)\ns\tshop\nr\treference: every built-in\n"
  "space\tpause / go on after an error\nf\tfast\n"
  "tab\t(in code) complete a name\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  api->mem_set(&E, 0, sizeof E);
  api->mem_set(&U, 0, sizeof U);
  G.seed = api->ticks_ms() | 1u;
  load_all();
  layout();
  place_bots();
  new_order();
  apply_program();
  U.shown_credits = -1;
  U.shown_norder = -1;

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.pref_w = SCREEN_W;
  UI.pref_h = SCREEN_H;
  api->ui(&UI);
  return 0;
}
