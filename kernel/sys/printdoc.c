/* The print document renderer and the printer's wire format. See printdoc.h.
 *
 * Portable: no allocation, no I/O, and the font comes from the same table
 * the console draws with. */
#include "kernel/sys/printdoc.h"
#include "kernel/console/font6x8.h"

#include <string.h>

/* ---------------------------------------------------------- wire format -- */

uint8_t printdoc_crc8(const uint8_t *data, size_t n) {
  uint8_t c = 0;
  size_t i;
  int b;
  for (i = 0; i < n; i++) {
    c ^= data[i];
    for (b = 0; b < 8; b++) c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
  }
  return c;
}

int printdoc_packet(uint8_t cmd, const uint8_t *data, int len,
                    uint8_t *out, size_t size) {
  if (len < 0 || len > 255 || size < (size_t)len + 8) return -1;
  out[0] = 0x51; out[1] = 0x78; out[2] = cmd; out[3] = 0x00;
  out[4] = (uint8_t)len; out[5] = 0x00;
  if (len) memcpy(out + 6, data, (size_t)len);
  out[6 + len] = printdoc_crc8(data, (size_t)len);
  out[7 + len] = 0xFF;
  return len + 8;
}

/* Appends one packet to a buffer being filled; -1 sticks once it has
 * happened, so a sequence can be written straight through and checked once. */
static int put(uint8_t *out, size_t size, int at, uint8_t cmd,
               const uint8_t *data, int len) {
  int n;
  if (at < 0) return -1;
  n = printdoc_packet(cmd, data, len, out + at, size - (size_t)at);
  return n < 0 ? -1 : at + n;
}

/* The values TinyPrint sends for its default darkness and the ones that
 * printed cleanly on 2026-09-20: quality 0x32, energy 12000, speed 0x20. */
int printdoc_prologue(uint8_t *out, size_t size) {
  static const uint8_t status[]  = { 0x00 };
  static const uint8_t quality[] = { 0x32 };
  static const uint8_t lattice[] = { 0xAA, 0x55, 0x17, 0x38, 0x44, 0x5F,
                                     0x5F, 0x5F, 0x44, 0x38, 0x2C };
  static const uint8_t energy[]  = { 0xB0, 0x2E };
  static const uint8_t image[]   = { 0x00 };
  static const uint8_t speed[]   = { 0x20 };
  static const uint8_t feed[]    = { 0x10, 0x00 };
  int at = 0;
  at = put(out, size, at, 0xA3, status, 1);
  at = put(out, size, at, 0xA4, quality, 1);
  at = put(out, size, at, 0xA6, lattice, (int)sizeof lattice);
  at = put(out, size, at, 0xAF, energy, 2);
  at = put(out, size, at, 0xBE, image, 1);
  at = put(out, size, at, 0xBD, speed, 1);
  at = put(out, size, at, 0xA1, feed, 2);
  return at;
}

int printdoc_epilogue(uint8_t *out, size_t size) {
  static const uint8_t feed[]    = { 0x70, 0x00 };   /* clear of the tear bar */
  static const uint8_t lattice[] = { 0xAA, 0x55, 0x17, 0x00, 0x00, 0x00,
                                     0x00, 0x00, 0x00, 0x00, 0x17 };
  static const uint8_t status[]  = { 0x00 };
  int at = 0;
  at = put(out, size, at, 0xA1, feed, 2);
  at = put(out, size, at, 0xA6, lattice, (int)sizeof lattice);
  at = put(out, size, at, 0xA3, status, 1);
  return at;
}

int printdoc_row_packet(const uint8_t row[PRINT_ROW_BYTES],
                        uint8_t *out, size_t size) {
  return printdoc_packet(0xA2, row, PRINT_ROW_BYTES, out, size);
}

const char *printdoc_status_text(const uint8_t *reply, size_t n) {
  uint8_t s;
  if (!reply || n < 7) return NULL;
  if (reply[0] != 0x51 || reply[1] != 0x78 || reply[2] != 0xA3 || reply[3] != 0x01)
    return NULL;
  s = reply[6];
  if (s == 0) return "ok";
  if (s & 0x01) return "out of paper";
  if (s & 0x04) return "too hot";
  if (s & 0x08) return "battery low";
  return "printer error";
}

/* ------------------------------------------------------------- renderer -- */

enum { PD_TEXT, PD_CHECK, PD_CHECKED, PD_SUB, PD_HEAD, PD_RULE, PD_BLANK };

#define USABLE     (PRINT_WIDTH - 2 * PRINT_MARGIN)   /* 352 */
#define BOX        16                                  /* the checkbox, square */
#define BOX_GAP    8
#define CHECK_X    (PRINT_MARGIN + BOX + BOX_GAP)      /* where its text starts */

/* Block heights: the ink plus the gap under it. */
#define H_TEXT   (FONT_H * 2 + 4)      /* 20 */
#define H_SUB    (FONT_H * 2 + 6)      /* 22 */
#define H_HEAD   (FONT_H * 3 + 4 + 2 + 8)  /* 38: glyphs, gap, rule, gap */
#define H_RULE   (8 + 2 + 8)
#define H_BLANK  8

static void set_px(uint8_t (*block)[PRINT_ROW_BYTES], int rows, int x, int y) {
  if (x < 0 || x >= PRINT_WIDTH || y < 0 || y >= rows) return;
  block[y][x >> 3] |= (uint8_t)(1u << (x & 7));
}

static void fill(uint8_t (*block)[PRINT_ROW_BYTES], int rows,
                 int x, int y, int w, int h) {
  int i, j;
  for (j = 0; j < h; j++)
    for (i = 0; i < w; i++) set_px(block, rows, x + i, y + j);
}

void printdoc_glyph(uint8_t (*block)[PRINT_ROW_BYTES], int rows, int x, int y,
                    char c, int s, int bold) {
  const uint8_t *g;
  int col, bit;
  if ((unsigned char)c < FONT_FIRST || (unsigned char)c > FONT_LAST) c = '?';
  g = font6x8[(unsigned char)c - FONT_FIRST];
  for (col = 0; col < FONT_W; col++)
    for (bit = 0; bit < FONT_H; bit++)
      if (g[col] & (1 << bit))
        fill(block, rows, x + col * s, y + bit * s, s + (bold ? 1 : 0), s);
}

static void text_at(uint8_t (*block)[PRINT_ROW_BYTES], int rows, int x, int y,
                    const char *s, int len, int scale, int bold) {
  int i;
  for (i = 0; i < len; i++) printdoc_glyph(block, rows, x + i * FONT_W * scale, y,
                                           s[i], scale, bold);
}

static void rule(uint8_t (*block)[PRINT_ROW_BYTES], int rows, int y) {
  fill(block, rows, PRINT_MARGIN, y, USABLE, 2);
}

static void checkbox(uint8_t (*block)[PRINT_ROW_BYTES], int rows, int ticked) {
  int x = PRINT_MARGIN, i;
  fill(block, rows, x, 0, BOX, 2);
  fill(block, rows, x, BOX - 2, BOX, 2);
  fill(block, rows, x, 0, 2, BOX);
  fill(block, rows, x + BOX - 2, 0, 2, BOX);
  if (!ticked) return;
  /* a cross, two pixels thick, inside the border */
  for (i = 3; i < BOX - 3; i++) {
    fill(block, rows, x + i, i, 2, 2);
    fill(block, rows, x + BOX - 2 - i, i, 2, 2);
  }
}

/* How the line is drawn: pixel x where text starts, and the scale. */
static int style_scale(int style) { return style == PD_HEAD ? 3 : 2; }
static int style_x(int style, int cont) {
  if (style == PD_CHECK || style == PD_CHECKED) return CHECK_X;
  (void)cont;
  return PRINT_MARGIN;
}
static int style_cols(int style, int cont) {
  return (PRINT_WIDTH - PRINT_MARGIN - style_x(style, cont)) / (FONT_W * style_scale(style));
}
static int style_height(int style) {
  switch (style) {
  case PD_HEAD:  return H_HEAD;
  case PD_SUB:   return H_SUB;
  case PD_RULE:  return H_RULE;
  case PD_BLANK: return H_BLANK;
  default:       return H_TEXT;
  }
}

/* Classify one source line, leaving `*text` at its content. */
static int classify(const char *line, int len, const char **text, int *tlen) {
  *text = line; *tlen = len;
  if (len == 0) return PD_BLANK;
  if (len >= 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') return PD_RULE;
  if (len >= 2 && line[0] == '#' && line[1] == '#') {
    *text = line + 2; *tlen = len - 2;
    while (*tlen && **text == ' ') { (*text)++; (*tlen)--; }
    return PD_SUB;
  }
  if (line[0] == '#') {
    *text = line + 1; *tlen = len - 1;
    while (*tlen && **text == ' ') { (*text)++; (*tlen)--; }
    return PD_HEAD;
  }
  if (len >= 3 && line[0] == '[' && line[2] == ']') {
    int ticked = line[1] == 'x' || line[1] == 'X';
    *text = line + 3; *tlen = len - 3;
    while (*tlen && **text == ' ') { (*text)++; (*tlen)--; }
    return ticked ? PD_CHECKED : PD_CHECK;
  }
  return PD_TEXT;
}

/* The longest prefix of `s` that fits `cols`, breaking at a space when
 * there is one to break at. Returns the length to draw; *skip is how much
 * to advance past it (the same, or one more for the space). */
static int wrap_len(const char *s, int len, int cols, int *skip) {
  int i;
  if (len <= cols) { *skip = len; return len; }
  for (i = cols; i > 0; i--)
    if (s[i] == ' ') { *skip = i + 1; return i; }
  *skip = cols;
  return cols;
}

/* Take the next source line into rest/style; 0 at the end of the text. */
static int next_source_line(PrintDoc *d) {
  const char *p = d->pos, *nl;
  int len;
  if (!p || !*p) return 0;
  nl = strchr(p, '\n');
  len = nl ? (int)(nl - p) : (int)strlen(p);
  d->pos = nl ? nl + 1 : p + len;
  if (len && p[len - 1] == '\r') len--;
  d->style = classify(p, len, &d->rest, &d->rest_len);
  d->cont = 0;
  return 1;
}

/* Render the next block: the next wrapped line of the current source line,
 * or the first of a new one. 0 when there is nothing left. */
static int next_block(PrintDoc *d) {
  int scale, cols, n, skip, x, bold;
  if (d->rest == NULL && !next_source_line(d)) return 0;

  memset(d->block, 0, sizeof d->block);
  d->row = 0;
  d->block_rows = style_height(d->style);

  switch (d->style) {
  case PD_BLANK:
    d->rest = NULL;
    return 1;
  case PD_RULE:
    rule(d->block, d->block_rows, 8);
    d->rest = NULL;
    return 1;
  default:
    break;
  }

  scale = style_scale(d->style);
  cols  = style_cols(d->style, d->cont);
  x     = style_x(d->style, d->cont);
  bold  = d->style == PD_HEAD || d->style == PD_SUB;
  n = wrap_len(d->rest, d->rest_len, cols, &skip);

  if (!d->cont && (d->style == PD_CHECK || d->style == PD_CHECKED))
    checkbox(d->block, d->block_rows, d->style == PD_CHECKED);
  text_at(d->block, d->block_rows, x, 0, d->rest, n, scale, bold);
  if (d->style == PD_HEAD && d->rest_len - skip <= 0)
    rule(d->block, d->block_rows, FONT_H * 3 + 4);

  d->rest += skip;
  d->rest_len -= skip;
  if (d->rest_len <= 0) d->rest = NULL;
  else d->cont = 1;
  return 1;
}

void printdoc_begin(PrintDoc *d, const char *text) {
  memset(d, 0, sizeof *d);
  d->text = text;
  d->pos = text;
  d->rest = NULL;
  d->done = (text == NULL || *text == 0);
}

int printdoc_next_row(void *ctx, uint8_t row[PRINT_ROW_BYTES]) {
  PrintDoc *d = (PrintDoc *)ctx;
  if (d->done) return 0;
  while (d->row >= d->block_rows) {
    if (!next_block(d)) { d->done = 1; return 0; }
  }
  memcpy(row, d->block[d->row++], PRINT_ROW_BYTES);
  return 1;
}

int printdoc_count_rows(const char *text) {
  PrintDoc d;
  uint8_t row[PRINT_ROW_BYTES];
  int n = 0;
  printdoc_begin(&d, text);
  while (printdoc_next_row(&d, row)) n++;
  return n;
}
