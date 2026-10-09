/* A scrolling terminal: a bar, a log of wrapped lines, an input line.
 *
 * Build, Claude and Chat are the same screen with different things behind
 * it, and each carried its own copy of it -- push (oldest out), word wrap
 * at forty columns, the padded text that writes over itself, the three
 * paints -- whose wrap rules had already drifted: Chat indented its
 * continuation lines and dropped empty ones, Claude stopped at each newline,
 * Build walked the whole text. This is that screen once; the wrap is pinned
 * by test/test_termlog.c.
 *
 * Nothing here clears a row and then writes it: there is no framebuffer, and
 * a fill followed by text is a blink on the panel. Lines are padded with
 * spaces to the width and written over the one before; only the pixel rows
 * and margins text does not cover are filled.
 *
 * The app defines TL_LINES (how much scrollback) before including this.
 * Header-only, static helpers, the same arrangement as apps/safefile.h.
 */
#ifndef CARDOS_TERMLOG_H
#define CARDOS_TERMLOG_H

#include "kernel/app/capp.h"

#if defined(__GNUC__)
#define TL_OPT __attribute__((unused))
#else
#define TL_OPT
#endif

#ifndef TL_LINES
#define TL_LINES 100
#endif
#define TL_COLS   40          /* 240 pixels at six a character */
#define TL_ROW_H   9
#define TL_BAR_H  10
#define TL_IN_H   11

typedef struct {
  char     line[TL_LINES][TL_COLS + 1];
  uint16_t tag[TL_LINES];     /* the app's: who said it, or its colour */
  int      n;
  int      scroll;            /* lines from the bottom */
} TermLog;

/* ---- the log ------------------------------------------------------------------ */

/* One line, the oldest falling out when full. A ring would save the copying
 * and cost a modulo in every reader; at a hundred lines of 41 bytes this is
 * a few KB moved once per line, on a machine waiting for a network. */
static TL_OPT void tl_push(const CardApi *api, TermLog *L, const char *text, int tag) {
  int i;
  if (L->n == TL_LINES) {
    for (i = 1; i < TL_LINES; i++) {
      api->mem_cpy(L->line[i - 1], L->line[i], TL_COLS + 1);
      L->tag[i - 1] = L->tag[i];
    }
    L->n--;
  }
  api->fmt(L->line[L->n], TL_COLS + 1, "%s", text);
  L->tag[L->n] = (uint16_t)tag;
  L->n++;
}

/* One line of `text` -- up to a newline or the end -- word-wrapped at
 * TL_COLS: broken at the last space when that is more than a third of the
 * way along, else mid-word (there is nothing else to do with a URL), with
 * `indent` spaces before each continuation. An empty line is a line.
 * Returns what follows the newline, or NULL at the end of the text. */
static TL_OPT const char *tl_wrap_line(const CardApi *api, TermLog *L, const char *text,
                                       int tag, int indent) {
  char out[TL_COLS + 1];
  int n = 0, k;
  if (indent > TL_COLS / 3) indent = TL_COLS / 3;
  for (;;) {
    char c = *text;
    if (c == '\n' || c == 0) {
      out[n] = 0;
      tl_push(api, L, out, tag);
      return c ? text + 1 : 0;
    }
    if (c == '\r') { text++; continue; }
    if (c == '\t') c = ' ';
    if (n == TL_COLS) {
      int brk = n, keep, cut;
      char tail[TL_COLS + 1];
      while (brk > 0 && out[brk - 1] != ' ') brk--;
      if (brk > TL_COLS / 3) cut = brk - 1;   /* at the space, which goes */
      else cut = brk = n;                     /* no space worth breaking at */
      keep = n - brk;
      for (k = 0; k < keep; k++) tail[k] = out[brk + k];
      out[cut] = 0;
      tl_push(api, L, out, tag);
      for (n = 0; n < indent; n++) out[n] = ' ';
      for (k = 0; k < keep; k++) out[n++] = tail[k];
    }
    out[n++] = c;
    text++;
  }
}

/* All of `text`, a line per newline. */
static TL_OPT void tl_push_text(const CardApi *api, TermLog *L, const char *text, int tag,
                                int indent) {
  while (text) text = tl_wrap_line(api, L, text, tag, indent);
}

/* ---- where things are ---------------------------------------------------------- */

static TL_OPT CRect tl_bar_rect(CRect c) { CRect r = c; r.h = TL_BAR_H; return r; }
static TL_OPT CRect tl_log_rect(CRect c) {
  CRect r = c;
  r.y = (int16_t)(c.y + TL_BAR_H);
  r.h = (int16_t)(c.h - TL_BAR_H - TL_IN_H);
  return r;
}
static TL_OPT CRect tl_in_rect(CRect c) {
  CRect r = c;
  r.y = (int16_t)(c.y + c.h - TL_IN_H);
  r.h = TL_IN_H;
  return r;
}
static TL_OPT int tl_rows(CRect c) { return (c.h - TL_BAR_H - TL_IN_H) / TL_ROW_H; }

/* ---- painting ------------------------------------------------------------------- */

static TL_OPT void tl_fill_if(const CardApi *api, int x, int y, int w, int h, uint16_t c) {
  CRect r;
  if (w <= 0 || h <= 0) return;
  r.x = (int16_t)x; r.y = (int16_t)y; r.w = (int16_t)w; r.h = (int16_t)h;
  api->fill(r, c);
}

/* `s` padded with spaces to `cols` characters, so a line writes over the
 * one it replaces instead of the row being cleared first. */
static TL_OPT void tl_text_cols(const CardApi *api, int x, int y, const char *s, int cols,
                                uint16_t fg, uint16_t bg) {
  char b[64];
  int n = 0;
  if (cols > (int)sizeof b - 1) cols = (int)sizeof b - 1;
  while (s[n] && n < cols) { b[n] = s[n]; n++; }
  while (n < cols) b[n++] = ' ';
  b[n] = 0;
  api->text((int16_t)x, (int16_t)y, b, fg, bg);
}

/* The bar: written over itself, padded to its width, with only the pixel
 * rows above and below the text and its margins filled. */
static TL_OPT void tl_paint_bar(const CardApi *api, CRect c, const char *text, uint16_t fg,
                                uint16_t bg) {
  int cols = (c.w - 3 + 5) / 6;          /* the last, cut by the edge, still padded */
  if (cols > 63) cols = 63;
  tl_fill_if(api, c.x, c.y, c.w, 1, bg);
  tl_fill_if(api, c.x, c.y + 9, c.w, TL_BAR_H - 9, bg);
  tl_fill_if(api, c.x, c.y + 1, 3, 8, bg);
  tl_fill_if(api, c.x + 3 + cols * 6, c.y + 1, c.w - 3 - cols * 6, 8, bg);
  tl_text_cols(api, c.x + 3, c.y + 1, text, cols, fg, bg);
}

/* The log, the newest line just above the input; scroll moves back through
 * it. Only the rows inside `clip` are written. */
static TL_OPT void tl_paint_log(const CardApi *api, const TermLog *L, CRect c, CRect clip,
                                uint16_t (*colour)(int tag), uint16_t bg) {
  int rows = tl_rows(c);
  int top = c.y + TL_BAR_H, bottom = c.y + c.h - TL_IN_H;
  int cols = (c.w - 2 + 5) / 6, right;   /* to the edge: a 40-column line reaches it */
  int first, r, y = top;
  if (cols > 63) cols = 63;
  right = c.x + 2 + cols * 6;
  first = L->n - rows - L->scroll;
  if (first < 0) first = 0;
  tl_fill_if(api, c.x, top, 2, bottom - top, bg);
  tl_fill_if(api, right, top, c.x + c.w - right, bottom - top, bg);
  for (r = 0; r < rows; r++) {
    int i = first + r;
    if (i >= L->n) break;
    y = top + r * TL_ROW_H;
    if (y < clip.y + clip.h && y + TL_ROW_H > clip.y) {
      tl_text_cols(api, c.x + 2, y, L->line[i], cols, colour(L->tag[i]), bg);
      tl_fill_if(api, c.x + 2, y + 8, cols * 6, TL_ROW_H - 8, bg);
    }
    y += TL_ROW_H;
  }
  tl_fill_if(api, c.x + 2, y, cols * 6, bottom - y, bg);
}

/* The input line: the prompt, the tail of what has been typed, the cursor,
 * then spaces to the end -- each drawn over the last, so a keystroke does
 * not blank the line. */
static TL_OPT void tl_paint_input(const CardApi *api, CRect c, const char *prompt,
                                  const char *input, int len, uint16_t fg, uint16_t dim,
                                  uint16_t bg) {
  int y = c.y + c.h - TL_IN_H;
  int vis = (c.w - 12) / 6;
  int from = len > vis ? len - vis : 0;
  int n = len - from, cx = c.x + 10 + n * 6, end;
  CRect cur;
  tl_fill_if(api, c.x, y, c.w, 2, bg);
  tl_fill_if(api, c.x, y + 10, c.w, TL_IN_H - 10, bg);
  tl_fill_if(api, c.x, y + 2, 2, 8, bg);
  api->text((int16_t)(c.x + 2), (int16_t)(y + 2), prompt, dim, bg);
  tl_fill_if(api, c.x + 8, y + 2, 2, 8, bg);
  api->text((int16_t)(c.x + 10), (int16_t)(y + 2), input + from, fg, bg);
  cur.x = (int16_t)cx; cur.y = (int16_t)(y + 2); cur.w = 5; cur.h = 8;
  api->fill(cur, fg);
  tl_text_cols(api, cx + 5, y + 2, "", vis - n, fg, bg);
  end = cx + 5 + (vis > n ? vis - n : 0) * 6;
  tl_fill_if(api, end, y + 2, c.x + c.w - end, 8, bg);
}

#endif /* CARDOS_TERMLOG_H */
