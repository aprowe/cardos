/* The built-in apps. See app.h. */

#include "kernel/ui/app.h"
#include "kernel/ui/draw.h"
#include "kernel/fs/fs.h"
#include "kernel/mem/mem.h"
#include "esp_system.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#define LINE_H 9

static void line(Rect c, int n, const char *s) {
  draw_text(c.x, (int16_t)(c.y + n * LINE_H), s, C_TEXT, C_WHITE);
}

/* ------------------------------------------------------------- About ---- */

static void about_paint(void *state, Rect c) {
  char buf[40];
  (void)state;
  line(c, 0, "CardOS 0.1");
  line(c, 1, "M5Stack Cardputer");
  snprintf(buf, sizeof buf, "heap %uK free",
           (unsigned)(esp_get_free_heap_size() / 1024));
  line(c, 2, buf);
  line(c, 3, "no PSRAM, no MMU");
}

/* ------------------------------------------------------------- Files ---- */

/* The listing is cached rather than read in paint. Reading the card inside a
 * paint callback meant every cursor movement across this window did SD SPI
 * I/O -- tens of milliseconds each -- which is what made the mouse lag. A
 * paint callback has to be cheap, because damage is what calls it. */
#define FILES_MAX 24
#define FILES_NAME 26
typedef struct {
  int  top;
  int  count;
  char name[FILES_MAX][FILES_NAME];
} FilesState;
static FilesState s_files;

static void files_reload(FilesState *st) {
  FsDir d;
  FsEntry e;
  st->count = 0;
  if (fs_opendir("/", &d) != 0) return;
  while (st->count < FILES_MAX && fs_readdir(&d, &e) == 1) {
    snprintf(st->name[st->count], FILES_NAME, "%.24s%s", e.name,
             e.is_dir ? "/" : "");
    st->count++;
  }
  fs_closedir(&d);
}

static void files_paint(void *state, Rect c) {
  FilesState *st = (FilesState *)state;
  int rows = c.h / LINE_H, i;

  if (st->count == 0) { line(c, 0, "no card"); return; }
  for (i = 0; i < rows && st->top + i < st->count; i++)
    line(c, i, st->name[st->top + i]);
}

static int files_key(void *state, uint8_t k) {
  FilesState *st = (FilesState *)state;
  if ((k == 'j' || k == 0x81) && st->top + 1 < st->count) { st->top++; return 1; }
  if ((k == 'k' || k == 0x80) && st->top > 0) { st->top--; return 1; }
  if (k == 'r' || k == 'R') { files_reload(st); return 1; }
  return 0;
}

static void files_open(void *state) {
  FilesState *st = (FilesState *)state;
  st->top = 0;
  files_reload(st);
}

/* ------------------------------------------------------------ Memory ---- */

static void mem_paint(void *state, Rect c) {
  MemStats st;
  char buf[40];
  (void)state;
  mem_stats(&st);
  snprintf(buf, sizeof buf, "heap   %uK", (unsigned)(st.heap_size / 1024));
  line(c, 0, buf);
  snprintf(buf, sizeof buf, "used   %u", (unsigned)(st.movable_used + st.fixed_used));
  line(c, 1, buf);
  snprintf(buf, sizeof buf, "handles %u/%u", (unsigned)st.handles_used,
           (unsigned)MEM_MAX_HANDLES);
  line(c, 2, buf);
  snprintf(buf, sizeof buf, "compact %u", (unsigned)st.compactions);
  line(c, 3, buf);
  snprintf(buf, sizeof buf, "evict   %u", (unsigned)st.evictions);
  line(c, 4, buf);
}

/* -------------------------------------------------------------- Edit ---- */
/* A small text editor. One flat buffer with embedded newlines: enough for a
 * 40x16 screen, and it avoids keeping a line table in step with the text. */

#define EDIT_MAX  512
#define EDIT_PATH "/home/edit.txt"

typedef struct {
  char buf[EDIT_MAX + 1];
  int  len;
  int  cur;          /* insertion point */
  int  dirty;
  char note[24];     /* transient message: loaded, saved, full */
} EditState;
static EditState s_edit;

static void edit_load(EditState *st) {
  int fd = fs_open(EDIT_PATH, FS_O_READ);
  int n = 0;
  st->len = 0;
  st->dirty = 0;
  if (fd >= 0) {
    n = fs_read(fd, st->buf, EDIT_MAX);
    fs_close(fd);
  }
  if (n > 0) st->len = n;
  st->buf[st->len] = 0;
  st->cur = st->len;
  snprintf(st->note, sizeof st->note, "%s", n > 0 ? "loaded" : "new file");
}

static void edit_save(EditState *st) {
  int fd = fs_open(EDIT_PATH, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) { snprintf(st->note, sizeof st->note, "cannot save"); return; }
  fs_write(fd, st->buf, (size_t)st->len);
  fs_close(fd);
  st->dirty = 0;
  snprintf(st->note, sizeof st->note, "saved %d", st->len);
}

static void edit_open(void *state) { edit_load((EditState *)state); }

static void edit_paint(void *state, Rect c) {
  EditState *st = (EditState *)state;
  int cols = c.w / 6, rows = (c.h / LINE_H) - 1;   /* last row is status */
  int i = 0, row = 0, col, curx = 0, cury = 0;
  char ln[44];

  if (cols <= 1 || rows < 1) return;
  if (cols > (int)sizeof ln - 1) cols = (int)sizeof ln - 1;

  /* Lay the buffer out, wrapping at the window width and breaking on
   * newlines, noting where the cursor falls on the way past. */
  while (row < rows) {
    col = 0;
    while (col < cols && i < st->len && st->buf[i] != '\n') {
      if (i == st->cur) { curx = col; cury = row; }
      ln[col++] = st->buf[i++];
    }
    ln[col] = 0;
    if (i == st->cur) { curx = col; cury = row; }
    draw_text(c.x, (int16_t)(c.y + row * LINE_H), ln, C_TEXT, C_WHITE);
    if (i < st->len && st->buf[i] == '\n') i++;
    else if (i >= st->len) { row++; break; }
    row++;
  }

  if (cury < rows) {
    Rect cur;
    cur.x = (int16_t)(c.x + curx * 6);
    cur.y = (int16_t)(c.y + cury * LINE_H);
    cur.w = 6; cur.h = 8;
    draw_rect(cur, C_TEXT);
  }

  snprintf(ln, sizeof ln, "^S save %s%s", st->dirty ? "* " : "", st->note);
  draw_text(c.x, (int16_t)(c.y + rows * LINE_H), ln, C_SHADOW, C_WHITE);
}

static int edit_key(void *state, uint8_t k) {
  EditState *st = (EditState *)state;

  if (k == 0x13) { edit_save(st); return 1; }            /* ctrl-S */
  if (k == 0x08) {                                       /* backspace */
    if (st->cur == 0) return 0;
    memmove(st->buf + st->cur - 1, st->buf + st->cur,
            (size_t)(st->len - st->cur));
    st->cur--; st->len--; st->buf[st->len] = 0;
    st->dirty = 1;
    return 1;
  }
  if (k == 0x82) { if (st->cur > 0) st->cur--; return 1; }        /* left */
  if (k == 0x83) { if (st->cur < st->len) st->cur++; return 1; }  /* right */

  if (k == 0x0D) k = '\n';
  if (k != '\n' && (k < 0x20 || k > 0x7E)) return 0;
  if (st->len >= EDIT_MAX) {
    snprintf(st->note, sizeof st->note, "full");
    return 1;
  }
  memmove(st->buf + st->cur + 1, st->buf + st->cur,
          (size_t)(st->len - st->cur));
  st->buf[st->cur++] = (char)k;
  st->len++;
  st->buf[st->len] = 0;
  st->dirty = 1;
  return 1;
}

/* ------------------------------------------------------------- Mines ---- */
/* Minesweeper on a 10x8 grid of 9-pixel cells, which is what fits the default
 * window's content area. Playable with the mouse or the keyboard. */

#define MINE_W 10
#define MINE_H 8
#define MINE_N 10
#define CELL   9

typedef struct {
  uint8_t mine[MINE_H][MINE_W];
  uint8_t shown[MINE_H][MINE_W];
  uint8_t flag[MINE_H][MINE_W];
  int cx, cy;
  int dead, won, started;
  uint32_t seed;
} MinesState;
static MinesState s_mines;

static uint32_t mines_rand(MinesState *st) {
  st->seed ^= st->seed << 13;
  st->seed ^= st->seed >> 17;
  st->seed ^= st->seed << 5;
  return st->seed;
}

static void mines_open(void *state) {
  MinesState *st = (MinesState *)state;
  memset(st->mine, 0, sizeof st->mine);
  memset(st->shown, 0, sizeof st->shown);
  memset(st->flag, 0, sizeof st->flag);
  st->cx = st->cy = 0;
  st->dead = st->won = st->started = 0;
  /* There is no RTC, but the uptime at the moment a window opens is
   * unpredictable enough to differ between games. */
  st->seed = (uint32_t)esp_timer_get_time() | 1u;
}

static int mines_count(MinesState *st, int x, int y) {
  int dx, dy, n = 0;
  for (dy = -1; dy <= 1; dy++)
    for (dx = -1; dx <= 1; dx++) {
      int nx = x + dx, ny = y + dy;
      if (nx < 0 || ny < 0 || nx >= MINE_W || ny >= MINE_H) continue;
      if (st->mine[ny][nx]) n++;
    }
  return n;
}

/* Mines are laid after the first dig, so the opening move is never fatal. */
static void mines_lay(MinesState *st, int safe_x, int safe_y) {
  int placed = 0;
  while (placed < MINE_N) {
    int x = (int)(mines_rand(st) % MINE_W);
    int y = (int)(mines_rand(st) % MINE_H);
    if (st->mine[y][x]) continue;
    if (x == safe_x && y == safe_y) continue;
    st->mine[y][x] = 1;
    placed++;
  }
  st->started = 1;
}

static void mines_reveal(MinesState *st, int x, int y) {
  if (x < 0 || y < 0 || x >= MINE_W || y >= MINE_H) return;
  if (st->shown[y][x] || st->flag[y][x]) return;
  st->shown[y][x] = 1;
  if (st->mine[y][x]) { st->dead = 1; return; }
  if (mines_count(st, x, y) == 0) {            /* flood the empty region */
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
      for (dx = -1; dx <= 1; dx++)
        if (dx || dy) mines_reveal(st, x + dx, y + dy);
  }
}

static void mines_check_won(MinesState *st) {
  int x, y, hidden = 0;
  for (y = 0; y < MINE_H; y++)
    for (x = 0; x < MINE_W; x++)
      if (!st->shown[y][x]) hidden++;
  if (hidden == MINE_N) st->won = 1;
}

static void mines_paint(void *state, Rect c) {
  MinesState *st = (MinesState *)state;
  int x, y;
  char buf[28];

  for (y = 0; y < MINE_H; y++) {
    for (x = 0; x < MINE_W; x++) {
      Rect cell;
      cell.x = (int16_t)(c.x + x * CELL);
      cell.y = (int16_t)(c.y + y * CELL);
      cell.w = CELL - 1; cell.h = CELL - 1;
      if (cell.x + cell.w > c.x + c.w || cell.y + cell.h > c.y + c.h) continue;

      if (!st->shown[y][x]) {
        draw_bevel(cell, C_FACE, C_LIGHT, C_SHADOW);
        if (st->flag[y][x])
          draw_text((int16_t)(cell.x + 1), (int16_t)cell.y, "F", C_TITLE, C_FACE);
      } else if (st->mine[y][x]) {
        draw_rect(cell, C_TITLE_UN);
        draw_text((int16_t)(cell.x + 1), (int16_t)cell.y, "*", C_DARK, C_TITLE_UN);
      } else {
        int n = mines_count(st, x, y);
        draw_rect(cell, C_WHITE);
        if (n) {
          char d[2];
          d[0] = (char)('0' + n); d[1] = 0;
          draw_text((int16_t)(cell.x + 1), (int16_t)cell.y, d, C_TEXT, C_WHITE);
        }
      }
      if (x == st->cx && y == st->cy) draw_frame(cell, C_RED);
    }
  }

  snprintf(buf, sizeof buf, "%s",
           st->dead ? "boom - o restarts" :
           st->won  ? "swept!" : "space dig  f flag");
  draw_text(c.x, (int16_t)(c.y + MINE_H * CELL + 1), buf, C_SHADOW, C_WHITE);
}

static int mines_dig(MinesState *st, int x, int y) {
  if (st->dead || st->won) return 0;
  if (x < 0 || y < 0 || x >= MINE_W || y >= MINE_H) return 0;
  if (!st->started) mines_lay(st, x, y);
  mines_reveal(st, x, y);
  if (!st->dead) mines_check_won(st);
  return 1;
}

static int mines_click(void *state, int16_t x, int16_t y, int button) {
  MinesState *st = (MinesState *)state;
  int cx = x / CELL, cy = y / CELL;
  (void)button;
  if (cx < 0 || cy < 0 || cx >= MINE_W || cy >= MINE_H) return 0;
  st->cx = cx;
  st->cy = cy;
  mines_dig(st, cx, cy);
  return 1;
}

static int mines_key(void *state, uint8_t k) {
  MinesState *st = (MinesState *)state;
  switch (k) {
  case 0x82: if (st->cx > 0) st->cx--; return 1;
  case 0x83: if (st->cx < MINE_W - 1) st->cx++; return 1;
  case 0x80: if (st->cy > 0) st->cy--; return 1;
  case 0x81: if (st->cy < MINE_H - 1) st->cy++; return 1;
  case ' ':
  case 0x0D: mines_dig(st, st->cx, st->cy); return 1;
  case 'f':
  case 'F':
    if (!st->shown[st->cy][st->cx]) st->flag[st->cy][st->cx] ^= 1;
    return 1;
  case 'o':
  case 'O': mines_open(st); return 1;
  default: return 0;
  }
}

/* ---------------------------------------------------------- registry ---- */

static const AppDef APPS[] = {
  /*  name      paint        key         click        open        state */
  { "Files",  files_paint, files_key,  NULL,        files_open, &s_files },
  { "Edit",   edit_paint,  edit_key,   NULL,        edit_open,  &s_edit  },
  { "Mines",  mines_paint, mines_key,  mines_click, mines_open, &s_mines },
  { "Memory", mem_paint,   NULL,       NULL,        NULL,       NULL     },
  { "About",  about_paint, NULL,       NULL,        NULL,       NULL     },
};

int app_count(void) { return (int)(sizeof APPS / sizeof APPS[0]); }

const AppDef *app_at(int i) {
  if (i < 0 || i >= app_count()) return &APPS[0];
  return &APPS[i];
}

int app_index_by_name(const char *name) {
  int i;
  for (i = 0; i < app_count(); i++)
    if (strcmp(APPS[i].name, name) == 0) return i;
  return -1;
}
