/* Text console on the ST7789.
 *
 * 240x135 at 6x8 per cell is exactly 40x16 characters with 7 pixels spare at
 * the bottom, which is left black.
 *
 * Drawing is per cell. A full-screen back buffer would be 65 KB -- a fifth of
 * the heap on a board with 322 KB and no PSRAM -- so the console keeps a 40x16
 * character grid (640 bytes) and repaints only the cells that change. The one
 * exception is scrolling, which repaints every row.
 */

#include "kernel/console/console.h"
#include "kernel/console/font6x8.h"
#include "kernel/drv/display.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h" 

static char     s_grid[CON_ROWS][CON_COLS];
/* The colour each cell was written in. Without it a scroll or a repaint
 * redrew sixteen rows in whatever colour was current, and a screenshot of
 * the console was a screenshot of one colour. 1280 bytes. */
static uint16_t s_fgs[CON_ROWS][CON_COLS];
static int      s_cx, s_cy;
static uint16_t s_fg = COLOR_GREEN;      /* a terminal, obviously */
static uint16_t s_bg = COLOR_BLACK;
static int      s_cursor_on;
static int      s_serial;

/* One character cell, expanded to pixels on the way out. */
static uint16_t s_cell[FONT_W * FONT_H];

static void draw_cell(int col, int row, char ch, int invert) {
  int x, y;
  const uint8_t *glyph = NULL;
  uint16_t fg = s_fgs[row][col];

  if ((unsigned char)ch >= FONT_FIRST && (unsigned char)ch <= FONT_LAST)
    glyph = font6x8[(unsigned char)ch - FONT_FIRST];

  for (x = 0; x < FONT_W; x++) {
    uint8_t bits = glyph ? glyph[x] : 0;
    for (y = 0; y < FONT_H; y++) {
      int on = (bits >> y) & 1;
      if (invert) on = !on;
      s_cell[y * FONT_W + x] = on ? fg : s_bg;
    }
  }
  display_blit(col * FONT_W, row * FONT_H, FONT_W, FONT_H, s_cell);
}

static void repaint_all(void) {
  int r, c;
  for (r = 0; r < CON_ROWS; r++)
    for (c = 0; c < CON_COLS; c++)
      draw_cell(c, r, s_grid[r][c], 0);
}

static void scroll(void) {
  int c;
  memmove(&s_grid[0][0], &s_grid[1][0], (CON_ROWS - 1) * CON_COLS);
  memset(&s_grid[CON_ROWS - 1][0], ' ', CON_COLS);
  memmove(&s_fgs[0][0], &s_fgs[1][0], (CON_ROWS - 1) * CON_COLS * sizeof s_fgs[0][0]);
  for (c = 0; c < CON_COLS; c++) s_fgs[CON_ROWS - 1][c] = s_fg;
  s_cy = CON_ROWS - 1;
  repaint_all();
}

static void advance(void) {
  if (++s_cx >= CON_COLS) {
    s_cx = 0;
    if (++s_cy >= CON_ROWS) scroll();
  }
}

int con_init(void) {
  con_clear();
  return 0;
}

void con_clear(void) {
  int r, c;
  memset(s_grid, ' ', sizeof s_grid);
  for (r = 0; r < CON_ROWS; r++)
    for (c = 0; c < CON_COLS; c++) s_fgs[r][c] = s_fg;
  s_cx = s_cy = 0;
  display_fill(s_bg);
}

/* Every cell again, from the grid that was always being kept. Used to put the
 * screen back after the voice overlay covered it -- the panel cannot read what
 * it drew over, so whatever owns the screen has to redraw itself, and for the
 * console that means this. */
void con_repaint(void) {
  int r, c;
  display_fill(s_bg);
  for (r = 0; r < CON_ROWS; r++)
    for (c = 0; c < CON_COLS; c++)
      if (s_grid[r][c] != ' ') draw_cell(c, r, s_grid[r][c], 0);
}

void con_set_serial(int on) {
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();

  s_serial = 0;
  if (!on) return;

  /* Read through the driver rather than fgetc(stdin). The console VFS ignores
   * O_NONBLOCK, so fgetc blocks forever with nothing to read -- which wedged
   * the whole main loop, keyboard included, on the first attempt.
   * usb_serial_jtag_read_bytes with a zero timeout genuinely does not block. */
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
  usb_serial_jtag_vfs_use_driver();
  setvbuf(stdout, NULL, _IONBF, 0);
  s_serial = 1;
}

/* One byte read ahead by con_serial_pending, handed out by the next
 * con_serial_key. The driver cannot peek, and a key an app was only asking
 * about must not be lost. */
static int s_serial_held;

int con_serial_key(void) {
  uint8_t c;
  if (!s_serial) return 0;
  if (s_serial_held) { int k = s_serial_held; s_serial_held = 0; return k; }
  if (usb_serial_jtag_read_bytes(&c, 1, 0) != 1) return 0;
  return (int)c;
}

int con_serial_pending(void) {
  if (!s_serial_held) s_serial_held = con_serial_key();
  return s_serial_held != 0;
}

void con_putc(char c) {
  if (s_serial) fputc(c, stdout);
  if (s_cursor_on) { draw_cell(s_cx, s_cy, s_grid[s_cy][s_cx], 0); s_cursor_on = 0; }

  switch (c) {
  case '\n':
    s_cx = 0;
    if (++s_cy >= CON_ROWS) scroll();
    return;
  case '\r':
    s_cx = 0;
    return;
  case '\b':
    if (s_cx > 0) s_cx--;
    else if (s_cy > 0) { s_cy--; s_cx = CON_COLS - 1; }
    s_grid[s_cy][s_cx] = ' ';
    draw_cell(s_cx, s_cy, ' ', 0);
    return;
  case '\t': {
    int n = 4 - (s_cx % 4);
    while (n--) con_putc(' ');
    return;
  }
  default:
    break;
  }

  if ((unsigned char)c < 0x20) return;      /* ignore other control codes */

  s_grid[s_cy][s_cx] = c;
  s_fgs[s_cy][s_cx] = s_fg;
  draw_cell(s_cx, s_cy, c, 0);
  advance();
}

void con_write(const char *s) {
  while (*s) con_putc(*s++);
}

void con_printf(const char *fmt, ...) {
  char buf[CON_COLS * 4];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  con_write(buf);
}

void con_cursor(int visible) {
  if (visible == s_cursor_on) return;
  s_cursor_on = visible;
  /* The cell under the cursor is drawn in the colour being typed in. */
  s_fgs[s_cy][s_cx] = s_fg;
  draw_cell(s_cx, s_cy, s_grid[s_cy][s_cx], visible);
}

void con_set_color(uint16_t fg) { s_fg = fg; }
