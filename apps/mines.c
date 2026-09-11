/* Minesweeper, as a loadable CardOS app.
 *
 * Dressed as the Windows 3.1/95 original, because that game's look *is* its
 * interface: a raised cell is unopened, a flat one is opened, the digit
 * colours say how close the danger is without being read as text, and the face
 * is the only status indicator anyone ever needed.
 *
 * Links against nothing: everything it does goes through the CardApi table it
 * is handed at registration.
 */

#include "kernel/app/capp.h"

#define W 9
#define H 9
#define MINES 10

#define CELL   10
#define HEAD   16          /* counter / face / timer strip */
#define BOARD_W (W * CELL)
#define BOARD_H (H * CELL)

/* The palette of the original, as closely as RGB565 allows. */
#define CLR_FACE   CAPP_RGB(192, 192, 192)
#define CLR_LIGHT  CAPP_RGB(255, 255, 255)
#define CLR_DARK   CAPP_RGB(128, 128, 128)
#define CLR_BLACK  CAPP_RGB(0, 0, 0)
#define CLR_RED    CAPP_RGB(255, 0, 0)
#define CLR_YELLOW CAPP_RGB(255, 255, 0)

static const uint16_t NUM_COLOUR[9] = {
  0,
  CAPP_RGB(0, 0, 255),        /* 1 blue */
  CAPP_RGB(0, 128, 0),        /* 2 green */
  CAPP_RGB(255, 0, 0),        /* 3 red */
  CAPP_RGB(0, 0, 128),        /* 4 navy */
  CAPP_RGB(128, 0, 0),        /* 5 maroon */
  CAPP_RGB(0, 128, 128),      /* 6 teal */
  CAPP_RGB(0, 0, 0),          /* 7 black */
  CAPP_RGB(128, 128, 128),    /* 8 grey */
};

static const CardApi *api;

static struct {
  unsigned char mine[H][W];
  unsigned char shown[H][W];
  unsigned char flag[H][W];
  int cx, cy;
  int dead, won, started;
  int flags;
  uint32_t start_ms, end_ms;
  unsigned int seed;
} S;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static unsigned int rnd(void) {
  S.seed ^= S.seed << 13;
  S.seed ^= S.seed >> 17;
  S.seed ^= S.seed << 5;
  return S.seed;
}

static void app_open(void *st) {
  (void)st;
  api->mem_set(S.mine, 0, sizeof S.mine);
  api->mem_set(S.shown, 0, sizeof S.shown);
  api->mem_set(S.flag, 0, sizeof S.flag);
  S.cx = S.cy = 0;
  S.dead = S.won = S.started = 0;
  S.flags = 0;
  S.start_ms = S.end_ms = 0;
  S.seed = api->ticks_ms() | 1u;
}

static int neighbours(int x, int y) {
  int dx, dy, n = 0;
  for (dy = -1; dy <= 1; dy++)
    for (dx = -1; dx <= 1; dx++) {
      int nx = x + dx, ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
      if (S.mine[ny][nx]) n++;
    }
  return n;
}

/* Mines are laid after the first dig, so the opening move is never fatal --
 * which is how the original behaves, and why its first click always opens a
 * region rather than sometimes ending the game. */
static void lay(int sx, int sy) {
  int placed = 0;
  while (placed < MINES) {
    int x = (int)(rnd() % W), y = (int)(rnd() % H);
    if (S.mine[y][x] || (x == sx && y == sy)) continue;
    S.mine[y][x] = 1;
    placed++;
  }
  S.started = 1;
  S.start_ms = api->ticks_ms();
}

static void reveal(int x, int y) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  if (S.shown[y][x] || S.flag[y][x]) return;
  S.shown[y][x] = 1;
  if (S.mine[y][x]) { S.dead = 1; S.end_ms = api->ticks_ms(); return; }
  if (neighbours(x, y) == 0) {
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
      for (dx = -1; dx <= 1; dx++)
        if (dx || dy) reveal(x + dx, y + dy);
  }
}

static void check_won(void) {
  int x, y, hidden = 0;
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      if (!S.shown[y][x]) hidden++;
  if (hidden == MINES) { S.won = 1; S.end_ms = api->ticks_ms(); }
}

static void dig(int x, int y) {
  if (S.dead || S.won) return;
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  if (S.flag[y][x]) return;
  if (!S.started) lay(x, y);
  reveal(x, y);
  if (!S.dead) check_won();
}

static void toggle_flag(int x, int y) {
  if (S.dead || S.won) return;
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  if (S.shown[y][x]) return;
  S.flag[y][x] ^= 1;
  S.flags += S.flag[y][x] ? 1 : -1;
}

/* ------------------------------------------------------------ drawing ---- */

/* The original's cells carry a two-pixel bevel; at ten pixels a side one pixel
 * reads the same and leaves room for the digit. */
static void raised(CRect r) {
  api->fill(r, CLR_FACE);
  api->fill(rect(r.x, r.y, r.w, 1), CLR_LIGHT);
  api->fill(rect(r.x, r.y, 1, r.h), CLR_LIGHT);
  api->fill(rect(r.x, r.y + r.h - 1, r.w, 1), CLR_DARK);
  api->fill(rect(r.x + r.w - 1, r.y, 1, r.h), CLR_DARK);
}

static void sunken(CRect r) {
  api->fill(r, CLR_FACE);
  api->fill(rect(r.x, r.y, r.w, 1), CLR_DARK);
  api->fill(rect(r.x, r.y, 1, r.h), CLR_DARK);
  api->fill(rect(r.x, r.y + r.h - 1, r.w, 1), CLR_LIGHT);
  api->fill(rect(r.x + r.w - 1, r.y, 1, r.h), CLR_LIGHT);
}

/* An opened cell is flat, with the grid line above and to its left -- which is
 * what gives the original its graph-paper field. */
static void opened(CRect r) {
  api->fill(r, CLR_FACE);
  api->fill(rect(r.x, r.y, r.w, 1), CLR_DARK);
  api->fill(rect(r.x, r.y, 1, r.h), CLR_DARK);
}

static void draw_mine(CRect c) {
  short cx = (short)(c.x + c.w / 2), cy = (short)(c.y + c.h / 2);
  api->fill(rect(cx - 2, cy - 1, 5, 3), CLR_BLACK);
  api->fill(rect(cx - 1, cy - 2, 3, 5), CLR_BLACK);
  api->fill(rect(cx - 3, cy, 7, 1), CLR_BLACK);      /* spikes */
  api->fill(rect(cx, cy - 3, 1, 7), CLR_BLACK);
  api->fill(rect(cx - 1, cy - 1, 1, 1), CLR_LIGHT);  /* the highlight */
}

static void draw_flag(CRect c) {
  short x = (short)(c.x + 2), y = (short)(c.y + 2);
  api->fill(rect(x + 3, y, 3, 3), CLR_RED);          /* the pennant */
  api->fill(rect(x + 2, y + 1, 1, 2), CLR_RED);
  api->fill(rect(x + 3, y + 3, 1, 3), CLR_BLACK);    /* the pole */
  api->fill(rect(x + 1, y + 6, 5, 1), CLR_BLACK);    /* the base */
}

/* Red on black, as on the original's counters. */
static void draw_counter(CRect box, int value) {
  char buf[8];
  if (value < 0) value = 0;
  if (value > 999) value = 999;
  api->fill(box, CLR_BLACK);
  api->fmt(buf, sizeof buf, "%03d", value);
  api->text((short)(box.x + 2), (short)(box.y + 1), buf, CLR_RED, CLR_BLACK);
}

static void draw_face(CRect box) {
  short cx = (short)(box.x + box.w / 2), cy = (short)(box.y + box.h / 2);
  raised(box);
  api->fill(rect(box.x + 2, box.y + 2, box.w - 4, box.h - 4), CLR_YELLOW);
  api->fill(rect(box.x + 3, box.y + 1, box.w - 6, 1), CLR_BLACK);
  api->fill(rect(box.x + 3, box.y + box.h - 2, box.w - 6, 1), CLR_BLACK);
  api->fill(rect(box.x + 1, box.y + 3, 1, box.h - 6), CLR_BLACK);
  api->fill(rect(box.x + box.w - 2, box.y + 3, 1, box.h - 6), CLR_BLACK);

  if (S.dead) {
    api->fill(rect(cx - 3, cy - 2, 3, 1), CLR_BLACK);   /* crosses for eyes */
    api->fill(rect(cx + 1, cy - 2, 3, 1), CLR_BLACK);
    api->fill(rect(cx - 2, cy - 3, 1, 3), CLR_BLACK);
    api->fill(rect(cx + 2, cy - 3, 1, 3), CLR_BLACK);
    api->fill(rect(cx - 2, cy + 2, 5, 1), CLR_BLACK);   /* a flat mouth */
  } else {
    api->fill(rect(cx - 3, cy - 2, 1, 2), CLR_BLACK);
    api->fill(rect(cx + 2, cy - 2, 1, 2), CLR_BLACK);
    if (S.won) {
      api->fill(rect(cx - 4, cy - 3, 3, 1), CLR_BLACK); /* sunglasses */
      api->fill(rect(cx + 1, cy - 3, 3, 1), CLR_BLACK);
    }
    api->fill(rect(cx - 2, cy + 2, 5, 1), CLR_BLACK);   /* a smile */
    api->fill(rect(cx - 3, cy + 1, 1, 1), CLR_BLACK);
    api->fill(rect(cx + 3, cy + 1, 1, 1), CLR_BLACK);
  }
}

/* Content-relative, so the same helper serves painting and hit testing. */
static CRect face_box(short ox, short oy) {
  return rect(ox + BOARD_W / 2 - 7, oy + 2, 14, 12);
}

static int elapsed(void) {
  uint32_t end;
  if (!S.started) return 0;
  end = (S.dead || S.won) ? S.end_ms : api->ticks_ms();
  return (int)((end - S.start_ms) / 1000u);
}

static void app_paint(void *st, CRect c) {
  int x, y;
  CRect board = rect(c.x, c.y + HEAD, BOARD_W, BOARD_H);
  (void)st;

  api->fill(rect(c.x, c.y, BOARD_W, HEAD), CLR_FACE);
  sunken(rect(c.x + 1, c.y + 2, 26, 12));
  draw_counter(rect(c.x + 2, c.y + 3, 24, 10), MINES - S.flags);
  sunken(rect(c.x + BOARD_W - 27, c.y + 2, 26, 12));
  draw_counter(rect(c.x + BOARD_W - 26, c.y + 3, 24, 10), elapsed());
  draw_face(face_box(c.x, c.y));

  for (y = 0; y < H; y++) {
    for (x = 0; x < W; x++) {
      CRect cell = rect(board.x + x * CELL, board.y + y * CELL, CELL, CELL);

      if (!S.shown[y][x]) {
        /* A lost game shows the mines that were missed and crosses out the
         * flags that were wrong -- the only feedback the original ever gives
         * about where the reasoning went astray. */
        if (S.dead && S.mine[y][x] && !S.flag[y][x]) {
          opened(cell);
          draw_mine(cell);
        } else {
          raised(cell);
          if (S.flag[y][x]) draw_flag(cell);
          if (S.dead && !S.mine[y][x] && S.flag[y][x]) {
            api->fill(rect(cell.x + 2, cell.y + 4, 6, 1), CLR_RED);
            api->fill(rect(cell.x + 4, cell.y + 2, 1, 6), CLR_RED);
          }
        }
      } else if (S.mine[y][x]) {
        api->fill(cell, CLR_RED);            /* the one that was stepped on */
        draw_mine(cell);
      } else {
        int n = neighbours(x, y);
        opened(cell);
        if (n) {
          char d[2];
          d[0] = (char)('0' + n);
          d[1] = 0;
          api->text((short)(cell.x + 3), (short)(cell.y + 1), d,
                    NUM_COLOUR[n], CLR_FACE);
        }
      }

      /* The keyboard cursor. A mouse player never sees it move, which is
       * right -- it exists only because this machine has arrow keys. */
      if (x == S.cx && y == S.cy && !S.dead && !S.won)
        api->frame(cell, CLR_BLACK);
    }
  }
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  switch (k) {
  case CAPP_KEY_LEFT:  if (S.cx > 0) S.cx--; return 1;
  case CAPP_KEY_RIGHT: if (S.cx < W - 1) S.cx++; return 1;
  case CAPP_KEY_UP:    if (S.cy > 0) S.cy--; return 1;
  case CAPP_KEY_DOWN:  if (S.cy < H - 1) S.cy++; return 1;
  case ' ':
  case CAPP_KEY_ENTER: dig(S.cx, S.cy); return 1;
  case 'f': case 'F':  toggle_flag(S.cx, S.cy); return 1;
  case 'n': case 'N':  app_open(0); return 1;
  default: return 0;
  }
}

static int app_click(void *st, short x, short y, int button) {
  CRect f = face_box(0, 0);
  int cx, cy;
  (void)st;

  /* The face restarts, exactly as it does in the original. */
  if (y >= f.y && y < f.y + f.h && x >= f.x && x < f.x + f.w) {
    app_open(0);
    return 1;
  }
  if (y < HEAD) return 0;

  cx = x / CELL;
  cy = (y - HEAD) / CELL;
  if (cx < 0 || cy < 0 || cx >= W || cy >= H) return 0;
  S.cx = cx;
  S.cy = cy;
  if (button == CAPP_BTN_RIGHT) toggle_flag(cx, cy);
  else dig(cx, cy);
  return 1;
}

/* 16x16, one bit per pixel: a mine with a fuse. */
static const unsigned char ICON[CAPP_ICON_BYTES] = {
  0x00, 0x00, 0x00, 0x60, 0x00, 0x90, 0x01, 0x10,
  0x03, 0xE0, 0x07, 0xC0, 0x1F, 0xF0, 0x3F, 0xF8,
  0x7F, 0xFC, 0x7F, 0xFC, 0x7F, 0xFC, 0x3F, 0xF8,
  0x1F, 0xF0, 0x07, 0xC0, 0x00, 0x00, 0x00, 0x00,
};

static CappApp APP;

const CappApp *capp_register(const CardApi *a) {
  api = a;
  APP.api_version = CAPP_API_VERSION;
  api->mem_cpy(APP.name, "Mines", 6);
  api->mem_cpy(APP.icon, ICON, CAPP_ICON_BYTES);
  APP.fullscreen = 0;
  APP.paint = app_paint;
  APP.key = app_key;
  APP.click = app_click;
  APP.open = app_open;
  APP.set_file = 0;
  APP.height = 0;
  APP.pref_w = BOARD_W;
  APP.pref_h = HEAD + BOARD_H;
  APP.state = 0;
  return &APP;
}
