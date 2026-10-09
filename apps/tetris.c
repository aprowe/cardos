/* Tetris. The pieces, their rotations and kicks, and the scoring and speed
 * tables are apps/tetris_data.h; this is the game around them -- the board,
 * gravity, the title, pause and game-over screens, and the best score.
 *
 * No hidden rows above the board: a piece spawns with its box's top row at
 * board row 0, and if that collides the game is over, which is the classic
 * (NES-era) behaviour the speed table is borrowed from. No lock delay
 * either, for the same reason -- a piece locks the moment gravity finds it
 * cannot fall, not some milliseconds later. Both are the simplest rule that
 * plays correctly, and this is one small screen, not a tournament client.
 *
 * Damage is coarse on purpose: the board is 60x120 pixels, so redrawing all
 * of it on every fall or keypress costs nothing worth tracking more finely,
 * unlike Mines' much busier cell-by-cell bookkeeping.
 */

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/tetris_data.h"
#include "apps/safefile.h"

static const CardApi *api;

#define CELL           6
#define BOARD_W_CELLS  TT_BOARD_W_STD        /* 10 */
#define BOARD_H_CELLS  20
#define BOARD_PX_W     (BOARD_W_CELLS * CELL)
#define BOARD_PX_H     (BOARD_H_CELLS * CELL)
#define GAP            8
#define PANEL_W        92

#define CLR_BG        CAPP_RGB(12, 14, 20)
#define CLR_BOARD_BG  CAPP_RGB(22, 25, 34)
#define CLR_TEXT      CAPP_RGB(232, 235, 242)
#define CLR_DIM       CAPP_RGB(120, 128, 144)
#define CLR_ACCENT    CAPP_RGB(96, 214, 198)

static const uint16_t PIECE_COLOUR[TT_PIECES] = {
  CAPP_RGB(0, 210, 210),    /* I cyan */
  CAPP_RGB(226, 198, 0),    /* O yellow */
  CAPP_RGB(160, 32, 200),   /* T purple */
  CAPP_RGB(0, 190, 64),     /* S green */
  CAPP_RGB(214, 32, 48),    /* Z red */
  CAPP_RGB(32, 96, 224),    /* J blue */
  CAPP_RGB(230, 140, 24),   /* L orange */
};

#define BEST_DIR  CAPP_VAR "/tetris"
#define BEST_PATH BEST_DIR "/best.txt"

enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_OVER };

static struct {
  uint8_t  board[BOARD_H_CELLS][BOARD_W_CELLS];   /* 0 empty, else type + 1 */
  int      state;

  int      type, rot, col, row;    /* the falling piece */
  int      next_type;
  uint8_t  bag[TT_PIECES];
  int      bag_pos;
  unsigned seed;

  uint32_t score;
  int      lines;
  uint32_t best;
  int      new_best;

  uint32_t acc_ms, last_ms;        /* gravity's accumulator and last tick */

  CRect    area, board_r, panel_r;
  int      have_area;
} G;

static void mark_board(void) { if (G.have_area) api->damage(G.board_r); }
static void mark_panel(void) { if (G.have_area) api->damage(G.panel_r); }
static void mark_all(void)   { if (G.have_area) api->damage(G.area); }

/* ---- the random bag: each of the seven pieces once before any repeats --- */

static unsigned rnd(void) {
  G.seed ^= G.seed << 13;
  G.seed ^= G.seed >> 17;
  G.seed ^= G.seed << 5;
  return G.seed;
}

static void bag_refill(void) {
  int i, j;
  uint8_t t;
  for (i = 0; i < TT_PIECES; i++) G.bag[i] = (uint8_t)i;
  for (i = TT_PIECES - 1; i > 0; i--) {
    j = (int)(rnd() % (unsigned)(i + 1));
    t = G.bag[i]; G.bag[i] = G.bag[j]; G.bag[j] = t;
  }
  G.bag_pos = 0;
}

static int bag_next(void) {
  if (G.bag_pos >= TT_PIECES) bag_refill();
  return G.bag[G.bag_pos++];
}

/* ---- the best score, saved through apps/safefile.h -------------------- */

static void load_best(void) {
  char buf[64], line[32];
  int fd, n, i, len = 0;
  G.best = 0;
  fd = safe_open_read(api, BEST_PATH);
  if (fd < 0) return;
  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      if (buf[i] == '\r') continue;
      if (buf[i] != '\n') { if (len < (int)sizeof line - 1) line[len++] = buf[i]; continue; }
      line[len] = 0;
      len = 0;
      if (line[0] == 'b' && line[1] == 'e' && line[2] == 's' && line[3] == 't' && line[4] == '=') {
        uint32_t v = 0;
        const char *p = line + 5;
        while (*p >= '0' && *p <= '9') v = v * 10u + (uint32_t)(*p++ - '0');
        G.best = v;
      }
    }
  }
  api->close(fd);
}

static void save_best(void) {
  SafeFile f;
  char line[32];
  api->mkdir(BEST_DIR);
  if (safe_begin(&f, api, BEST_PATH) != 0) return;
  api->fmt(line, sizeof line, "best=%u\n", (unsigned)G.best);
  safe_line(&f, line);
  safe_commit(&f);
}

/* ---- the board ---------------------------------------------------------- */

static int collides(int type, int rot, int col, int row) {
  uint16_t mask = TT_SHAPE[type][rot];
  int r, c;
  for (r = 0; r < TT_BOX; r++)
    for (c = 0; c < TT_BOX; c++)
      if (mask & (uint16_t)(1u << (r * 4 + c))) {
        int br = row + r, bc = col + c;
        if (bc < 0 || bc >= BOARD_W_CELLS || br < 0 || br >= BOARD_H_CELLS) return 1;
        if (G.board[br][bc]) return 1;
      }
  return 0;
}

/* Full rows are dropped by copying every row that survives down onto a
 * write pointer that starts at the bottom and only moves when a row is
 * kept -- which leaves the empty space at the top, where falling fills it
 * in again. */
static int clear_lines(void) {
  int dst = BOARD_H_CELLS - 1, src, c, full, cleared = 0;
  for (src = BOARD_H_CELLS - 1; src >= 0; src--) {
    full = 1;
    for (c = 0; c < BOARD_W_CELLS; c++) if (!G.board[src][c]) { full = 0; break; }
    if (full) { cleared++; continue; }
    if (dst != src) api->mem_cpy(G.board[dst], G.board[src], sizeof G.board[src]);
    dst--;
  }
  for (; dst >= 0; dst--) api->mem_set(G.board[dst], 0, sizeof G.board[dst]);
  return cleared;
}

static void spawn_piece(void) {
  G.type = G.next_type;
  G.next_type = bag_next();
  G.rot = TT_SPAWN;
  G.col = TT_SPAWN_COL;
  G.row = TT_SPAWN_ROW;
  if (collides(G.type, G.rot, G.col, G.row)) {
    G.state = ST_OVER;
    if (G.score > G.best) { G.best = G.score; G.new_best = 1; save_best(); }
  }
}

static void lock_piece(void) {
  uint16_t mask = TT_SHAPE[G.type][G.rot];
  int r, c, cleared, level_before;
  for (r = 0; r < TT_BOX; r++)
    for (c = 0; c < TT_BOX; c++)
      if (mask & (uint16_t)(1u << (r * 4 + c))) {
        int br = G.row + r, bc = G.col + c;
        if (br >= 0 && br < BOARD_H_CELLS && bc >= 0 && bc < BOARD_W_CELLS)
          G.board[br][bc] = (uint8_t)(G.type + 1);
      }
  cleared = clear_lines();
  if (cleared > 0) {
    level_before = 1 + G.lines / TT_LINES_PER_LEVEL;
    G.score += (uint32_t)TT_SCORE_LINES[cleared - 1] * (uint32_t)level_before;
    G.lines += cleared;
  }
  spawn_piece();
  mark_all();
}

static void reset_game(void) {
  int r;
  for (r = 0; r < BOARD_H_CELLS; r++) api->mem_set(G.board[r], 0, sizeof G.board[r]);
  G.score = 0;
  G.lines = 0;
  G.new_best = 0;
  G.seed = api->ticks_ms() | 1u;
  bag_refill();
  G.type = bag_next();
  G.next_type = bag_next();
  G.rot = TT_SPAWN;
  G.col = TT_SPAWN_COL;
  G.row = TT_SPAWN_ROW;
  G.acc_ms = 0;
  G.last_ms = api->ticks_ms();
  G.state = ST_PLAY;
}

static int try_move(int dx, int dy) {
  if (collides(G.type, G.rot, G.col + dx, G.row + dy)) return 0;
  G.col += dx;
  G.row += dy;
  return 1;
}

static int try_rotate(int dir) {
  int new_rot = (G.rot + (dir == TT_CW ? 1 : 3)) & 3;
  const int8_t (*kicks)[2] = (G.type == TT_I) ? TT_KICK_I[G.rot][dir] : TT_KICK_JLSTZ[G.rot][dir];
  int i;
  for (i = 0; i < TT_KICK_TESTS; i++) {
    int nc = G.col + kicks[i][0], nr = G.row + kicks[i][1];
    if (!collides(G.type, new_rot, nc, nr)) {
      G.col = nc; G.row = nr; G.rot = new_rot;
      return 1;
    }
  }
  return 0;
}

static void hard_drop(void) {
  int rows = 0;
  while (!collides(G.type, G.rot, G.col, G.row + 1)) { G.row++; rows++; }
  G.score += (uint32_t)TT_SCORE_HARD * (uint32_t)rows;
  lock_piece();
}

static uint32_t drop_ms(void) {
  int level = 1 + G.lines / TT_LINES_PER_LEVEL;
  int idx = level - 1;
  if (idx > TT_LEVEL_MAX) idx = TT_LEVEL_MAX;
  if (idx < 0) idx = 0;
  return TT_LEVEL_MS[idx];
}

/* ---- drawing -------------------------------------------------------------- */

static void draw_block(int x, int y, uint16_t colour) {
  api->fill(capp_rect(x, y, CELL, CELL), colour);
  api->fill(capp_rect(x, y, CELL, 1), CAPP_WHITE);
  api->fill(capp_rect(x, y, 1, CELL), CAPP_WHITE);
  api->fill(capp_rect(x, y + CELL - 1, CELL, 1), CAPP_BLACK);
  api->fill(capp_rect(x + CELL - 1, y, 1, CELL), CAPP_BLACK);
}

static void draw_shape_px(int type, int rot, int px, int py, int ghost) {
  uint16_t mask = TT_SHAPE[type][rot];
  int r, c;
  for (r = 0; r < TT_BOX; r++)
    for (c = 0; c < TT_BOX; c++)
      if (mask & (uint16_t)(1u << (r * 4 + c))) {
        int x = px + c * CELL, y = py + r * CELL;
        if (ghost) api->frame(capp_rect(x, y, CELL, CELL), CLR_DIM);
        else draw_block(x, y, PIECE_COLOUR[type]);
      }
}

static void layout(CRect c) {
  int total_w = BOARD_PX_W + GAP + PANEL_W;
  int ox = c.x + (c.w > total_w ? (c.w - total_w) / 2 : 0);
  int oy = c.y + (c.h > BOARD_PX_H ? (c.h - BOARD_PX_H) / 2 : 0);
  G.area = c;
  G.board_r = capp_rect(ox, oy, BOARD_PX_W, BOARD_PX_H);
  G.panel_r = capp_rect(ox + BOARD_PX_W + GAP, oy, PANEL_W, BOARD_PX_H);
  G.have_area = 1;
}

static void paint_board(void) {
  int r, c, gr;
  for (r = 0; r < BOARD_H_CELLS; r++)
    for (c = 0; c < BOARD_W_CELLS; c++) {
      int x = G.board_r.x + c * CELL, y = G.board_r.y + r * CELL;
      if (G.board[r][c]) draw_block(x, y, PIECE_COLOUR[G.board[r][c] - 1]);
      else api->fill(capp_rect(x, y, CELL, CELL), CLR_BOARD_BG);
    }
  gr = G.row;
  while (!collides(G.type, G.rot, G.col, gr + 1)) gr++;
  if (gr > G.row)
    draw_shape_px(G.type, G.rot, G.board_r.x + G.col * CELL, G.board_r.y + gr * CELL, 1);
  draw_shape_px(G.type, G.rot, G.board_r.x + G.col * CELL, G.board_r.y + G.row * CELL, 0);
}

static void paint_panel(void) {
  CRect p = G.panel_r;
  int y = p.y;
  char buf[24];
  api->fill(p, CLR_BG);
  api->text((int16_t)p.x, (int16_t)y, "TETRIS", CLR_ACCENT, CLR_BG); y += 10;
  api->text((int16_t)p.x, (int16_t)y, "next", CLR_DIM, CLR_BG); y += 9;
  api->fill(capp_rect(p.x, y, TT_BOX * CELL, TT_BOX * CELL), CLR_BOARD_BG);
  draw_shape_px(G.next_type, TT_SPAWN, p.x, y, 0);
  y += TT_BOX * CELL + 3;
  api->text((int16_t)p.x, (int16_t)y, "score", CLR_DIM, CLR_BG); y += 9;
  api->fmt(buf, sizeof buf, "%06u", (unsigned)G.score);
  api->text((int16_t)p.x, (int16_t)y, buf, CLR_TEXT, CLR_BG); y += 10;
  api->text((int16_t)p.x, (int16_t)y, "level", CLR_DIM, CLR_BG); y += 9;
  api->fmt(buf, sizeof buf, "%d", 1 + G.lines / TT_LINES_PER_LEVEL);
  api->text((int16_t)p.x, (int16_t)y, buf, CLR_TEXT, CLR_BG); y += 10;
  api->text((int16_t)p.x, (int16_t)y, "lines", CLR_DIM, CLR_BG); y += 9;
  api->fmt(buf, sizeof buf, "%d", G.lines);
  api->text((int16_t)p.x, (int16_t)y, buf, CLR_TEXT, CLR_BG); y += 10;
  api->text((int16_t)p.x, (int16_t)y, "best", CLR_DIM, CLR_BG); y += 9;
  api->fmt(buf, sizeof buf, "%06u", (unsigned)G.best);
  api->text((int16_t)p.x, (int16_t)y, buf, CLR_TEXT, CLR_BG);
}

static void paint_play(void) {
  CRect b = G.board_r, p = G.panel_r, a = G.area;
  if (b.x > a.x) api->fill(capp_rect(a.x, a.y, b.x - a.x, a.h), CLR_BG);
  if (b.y > a.y) api->fill(capp_rect(a.x, a.y, a.w, b.y - a.y), CLR_BG);
  if (b.y + b.h < a.y + a.h)
    api->fill(capp_rect(a.x, b.y + b.h, a.w, a.y + a.h - (b.y + b.h)), CLR_BG);
  if (p.x + p.w < a.x + a.w)
    api->fill(capp_rect(p.x + p.w, b.y, a.x + a.w - (p.x + p.w), b.h), CLR_BG);
  paint_board();
  paint_panel();
}

static void draw_centered(int y, const char *s, uint16_t fg) {
  int w = (int)api->str_len(s) * 6;
  int x = G.area.x + (G.area.w - w) / 2;
  if (x < G.area.x) x = G.area.x;
  api->text((int16_t)x, (int16_t)y, s, fg, CLR_BG);
}

static void paint_title(void) {
  char buf[32];
  api->fill(G.area, CLR_BG);
  draw_centered(G.area.y + 24, "TETRIS", CLR_ACCENT);
  api->fmt(buf, sizeof buf, "best %06u", (unsigned)G.best);
  draw_centered(G.area.y + 44, buf, CLR_DIM);
  draw_centered(G.area.y + 62, "enter: start", CLR_TEXT);
  draw_centered(G.area.y + 78, "arrows move, down drops", CLR_DIM);
  draw_centered(G.area.y + 90, "x/z rotate, space slams", CLR_DIM);
  draw_centered(G.area.y + 104, "p: pause", CLR_DIM);
}

static void paint_pause(void) {
  api->fill(G.area, CLR_BG);
  draw_centered(G.area.y + 50, "PAUSED", CLR_ACCENT);
  draw_centered(G.area.y + 68, "p / esc: resume", CLR_TEXT);
  draw_centered(G.area.y + 82, "n: new game", CLR_DIM);
}

static void paint_over(void) {
  char buf[32];
  api->fill(G.area, CLR_BG);
  draw_centered(G.area.y + 20, "GAME OVER", CLR_ACCENT);
  api->fmt(buf, sizeof buf, "score %06u", (unsigned)G.score);
  draw_centered(G.area.y + 40, buf, CLR_TEXT);
  if (G.new_best) {
    draw_centered(G.area.y + 54, "new best!", CLR_ACCENT);
  } else {
    api->fmt(buf, sizeof buf, "best %06u", (unsigned)G.best);
    draw_centered(G.area.y + 54, buf, CLR_DIM);
  }
  draw_centered(G.area.y + 74, "enter: play again", CLR_TEXT);
  draw_centered(G.area.y + 86, "esc: title", CLR_DIM);
}

static void app_paint(void *st, CRect c) {
  (void)st;
  layout(c);
  switch (G.state) {
  case ST_TITLE: paint_title(); break;
  case ST_PAUSE: paint_pause(); break;
  case ST_OVER:  paint_over(); break;
  default:       paint_play(); break;
  }
}

/* ---- keys and the clock --------------------------------------------------- */

static int app_key(void *st, uint8_t k) {
  int repeat;
  (void)st;
  repeat = api->key_repeat();

  if (G.state == ST_TITLE) {
    if (repeat) return 1;
    if (k == CAPP_KEY_ENTER || k == 'n' || k == 'N') { reset_game(); mark_all(); return 1; }
    return 0;
  }
  if (G.state == ST_OVER) {
    if (repeat) return 1;
    if (k == CAPP_KEY_ENTER || k == 'n' || k == 'N') { reset_game(); mark_all(); return 1; }
    if (k == CAPP_KEY_ESC) { G.state = ST_TITLE; mark_all(); return 1; }
    return 0;
  }
  if (G.state == ST_PAUSE) {
    if (repeat) return 1;
    if (k == 'p' || k == 'P' || k == CAPP_KEY_ESC) {
      G.state = ST_PLAY;
      G.last_ms = api->ticks_ms();
      mark_all();
      return 1;
    }
    if (k == 'n' || k == 'N') { reset_game(); mark_all(); return 1; }
    return 0;
  }

  /* ST_PLAY */
  switch (k) {
  case CAPP_KEY_LEFT:  try_move(-1, 0); mark_board(); return 1;
  case CAPP_KEY_RIGHT: try_move(1, 0);  mark_board(); return 1;
  case CAPP_KEY_DOWN:
    if (try_move(0, 1)) {
      G.score += TT_SCORE_SOFT;
      G.acc_ms = 0;
      mark_board();
      mark_panel();
    }
    return 1;
  case CAPP_KEY_UP: case 'x': case 'X':
    if (repeat) return 1;
    try_rotate(TT_CW);
    mark_board();
    return 1;
  case 'z': case 'Z':
    if (repeat) return 1;
    try_rotate(TT_CCW);
    mark_board();
    return 1;
  case ' ':
    if (repeat) return 1;
    hard_drop();
    return 1;
  case 'p': case 'P': case CAPP_KEY_ESC:
    if (repeat) return 1;
    G.state = ST_PAUSE;
    mark_all();
    return 1;
  case 'n': case 'N':
    if (repeat) return 1;
    reset_game();
    mark_all();
    return 1;
  default:
    return 0;
  }
}

static int app_tick(void *st, uint32_t now) {
  uint32_t dt, interval;
  int moved = 0;
  (void)st;
  if (G.state != ST_PLAY) { G.last_ms = now; return 0; }
  dt = now - G.last_ms;
  G.last_ms = now;
  G.acc_ms += dt;
  interval = drop_ms();
  while (G.acc_ms >= interval) {
    G.acc_ms -= interval;
    if (!collides(G.type, G.rot, G.col, G.row + 1)) {
      G.row++;
      moved = 1;
    } else {
      lock_piece();
      G.acc_ms = 0;
      moved = 1;
      break;
    }
    interval = drop_ms();
  }
  if (moved) { mark_board(); return 1; }
  return 0;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Tetris",
  /* 16x16: four blocks, a square of squares. */
  { 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F,
    0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0x00, 0x00,
    0x00, 0x00, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F,
    0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F },
  "left/right\tmove\ndown\tsoft drop\nup, x\trotate clockwise\n"
  "z\trotate counter-clockwise\nspace\thard drop\np, esc\tpause / resume\n"
  "enter\tstart, or play again\nn\tnew game\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&G, 0, sizeof G);
  G.state = ST_TITLE;
  G.seed = api->ticks_ms() | 1u;
  load_best();

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.pref_w = BOARD_PX_W + GAP + PANEL_W;
  UI.pref_h = BOARD_PX_H;
  api->ui(&UI);
  return 0;
}
