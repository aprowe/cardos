/* Printing: the document model and the wire format for the "cat printer"
 * family of BLE thermal printers (GB01/GT01/MX06/X6h -- the TinyPrint one).
 *
 * Two halves, both portable so the host suite can pin them:
 *
 *  - The packet layer: `51 78 CMD 00 LEN 00 DATA.. CRC8 FF`, verified
 *    against a real X6h on 2026-09-20 (see docs/specs/2026-09-20-print-design.md).
 *    A row is 384 pixels = 48 bytes, bit set = black, and the LSB of each
 *    byte is the LEFTMOST pixel -- the opposite of the 16x16 icons.
 *
 *  - The renderer: a line-oriented markup, deliberately markdown-shaped,
 *    turned into rows one at a time. Nothing holds the page: the largest
 *    buffer is one wrapped line of the tallest style.
 *
 *      # Heading          3x font, bold, ruled under
 *      ## Subheading      2x font, bold
 *      [ ] task           drawn checkbox, 2x text
 *      [x] task           ticked checkbox
 *      ---                horizontal rule
 *      text               2x text, word-wrapped; a blank line is a gap
 *
 * The screen font is 6x8; at 203 dpi that is unreadable at 1x, so nothing
 * here prints smaller than 2x (12x16 -- 29 columns between the margins).
 *
 * The job runner (printq.c) takes a row source rather than a document, so a
 * pre-rendered bitmap can be streamed through the same path later. */
#ifndef CARDOS_PRINTDOC_H
#define CARDOS_PRINTDOC_H

#include <stddef.h>
#include <stdint.h>

#define PRINT_WIDTH     384
#define PRINT_ROW_BYTES (PRINT_WIDTH / 8)
#define PRINT_MARGIN    16

/* One row of pixels, or 0 when the source is exhausted. */
typedef int (*PrintRowFn)(void *ctx, uint8_t row[PRINT_ROW_BYTES]);

/* ---- wire format ---- */

uint8_t printdoc_crc8(const uint8_t *data, size_t n);

/* Frame one command. Returns the packet length, or -1 if `out` is too small. */
int printdoc_packet(uint8_t cmd, const uint8_t *data, int len,
                    uint8_t *out, size_t size);

/* The commands that bracket a job: quality, lattice start, energy, draw mode,
 * speed, a short feed -- and the feed and lattice end that finish it. Each
 * returns bytes written, or -1. 192 bytes is enough for either. */
int printdoc_prologue(uint8_t *out, size_t size);
int printdoc_epilogue(uint8_t *out, size_t size);

/* A bitmap row as a packet: 48 data bytes, 56 on the wire. */
int printdoc_row_packet(const uint8_t row[PRINT_ROW_BYTES],
                        uint8_t *out, size_t size);

/* The printer's reply to a status request, one line of English, or NULL if
 * the bytes are not a status reply. Bit meanings are the community's, not
 * the vendor's. */
const char *printdoc_status_text(const uint8_t *reply, size_t n);

/* ---- renderer ---- */

#define PRINTDOC_LINE_H_MAX 40    /* the tallest block: a 3x heading and its rule */

typedef struct {
  const char *text;              /* the document; must outlive the struct */
  const char *pos;               /* start of the next source line */
  uint8_t  block[PRINTDOC_LINE_H_MAX][PRINT_ROW_BYTES];
  int      block_rows;           /* how many rows block[] holds */
  int      row;                  /* the next one to hand out */
  /* A source line that wrapped: what is left of it, and how to draw it. */
  const char *rest;
  int      rest_len;
  int      style;                /* PD_* */
  int      cont;                 /* a continuation (wrapped) line */
  int      done;
} PrintDoc;

void printdoc_begin(PrintDoc *d, const char *text);

/* Fills `row` and returns 1, or returns 0 when the document has ended.
 * Has the PrintRowFn shape, so &printdoc_next_row is a row source. */
int  printdoc_next_row(void *d, uint8_t row[PRINT_ROW_BYTES]);

/* Rows the whole document will produce. Runs the renderer over a copy, so it
 * costs what printing costs minus the radio; for a progress figure. */
int  printdoc_count_rows(const char *text);

/* Draw one glyph into a row block at scale `s`, `bold` smears one pixel
 * right. Exposed for the tests; x is a pixel column, y a row in the block. */
void printdoc_glyph(uint8_t (*block)[PRINT_ROW_BYTES], int rows, int x, int y,
                    char c, int s, int bold);

#endif /* CARDOS_PRINTDOC_H */
