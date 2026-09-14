/* IDE -- write assembly on the machine, and run it on the machine.
 *
 * The editor is apps/edit.c's, kept as it was: the same buffer, cursor, file
 * browser and save path, because a second editor would be a second set of
 * bugs. Its markdown renderer is not here -- it is 220 lines that an assembly
 * file has no use for.
 *
 * What is here instead is the other half of the loop. ctrl-b assembles the
 * buffer and interprets it; ctrl-l compiles it to real Xtensa and jumps to
 * it; ctrl-x does both and compares, which is the only place the compiler
 * can actually be checked, because the host that runs the test suite is x86
 * and cannot execute what the compiler emits.
 *
 * Errors put the cursor on the offending line. An assembler that reports
 * "syntax error" without saying where is worse than no assembler at all.
 *
 * The machine, the assembler and the code generator are in apps/asmvm.h,
 * with no UI in them, so they run under the host tests. Only the screen is
 * here. See docs/superpowers/specs/2026-09-13-asm-vm-and-native-compiler-design.md.
 */
#include "kernel/app/capp.h"
#include "apps/toolbar.h"
#include "apps/asmvm.h"


#define MAXLINES  96
#define MAXCOL    64
#define ROWH      9
#define GUTTER    18      /* three digits and a separator */
#define CHARW     6

#define DIRMAX    28
#define NAMELEN   32

/* A dark palette that keeps syntax-free text readable at this size. Comment
 * green is used for the gutter, not for comments -- there is no parser here,
 * and pretending otherwise would highlight the wrong things. */
#define CLR_BG      CAPP_RGB(24, 26, 32)
#define CLR_GUTTER  CAPP_RGB(34, 37, 45)
#define CLR_LINENO  CAPP_RGB(92, 102, 120)
#define CLR_TEXT    CAPP_RGB(214, 220, 232)
#define CLR_CUR_BG  CAPP_RGB(38, 42, 52)
#define CLR_CARET   CAPP_RGB(120, 200, 255)
#define CLR_BAR     CAPP_RGB(48, 82, 128)
#define CLR_BAR_FG  CAPP_RGB(232, 238, 248)
#define CLR_DIRTY   CAPP_RGB(255, 190, 90)
#define CLR_SEL     CAPP_RGB(52, 80, 116)
#define CLR_DIM     CAPP_RGB(130, 140, 158)

/* Preview: a page, not a terminal. Lighter ground and darker ink, because
 * prose is read rather than scanned, and every markdown renderer anyone has
 * seen looks like paper. */
#define CLR_PG      CAPP_RGB(238, 238, 232)
#define CLR_PG_TX   CAPP_RGB(32, 34, 40)
#define CLR_PG_H    CAPP_RGB(16, 40, 96)
#define CLR_PG_DIM  CAPP_RGB(110, 116, 128)
#define CLR_PG_RULE CAPP_RGB(180, 182, 176)
#define CLR_PG_CODE CAPP_RGB(216, 218, 210)
#define CLR_PG_LINK CAPP_RGB(24, 82, 170)
#define CLR_PG_QUOT CAPP_RGB(150, 154, 160)

typedef enum { VIEW_BROWSE = 0, VIEW_EDIT, VIEW_NAME } View;

/* What the filename prompt is for. One view serves both, because "what shall
 * it be called" is the same question either way -- only what happens after the
 * answer differs. */
typedef enum { NAME_NEW = 0, NAME_SAVE_AS } NameFor;

static const CardApi *api;

static struct {
  View view;

  /* browser */
  char dir[64];
  char names[DIRMAX][NAMELEN];
  int  ndir;
  int  bsel;

  /* buffer */
  char line[MAXLINES][MAXCOL + 1];
  short len[MAXLINES];
  int   nlines;
  int   cx, cy;
  int   top, leftcol;
  int   dirty;
  int   truncated;
  char  path[96];
  char  status[40];


  /* the filename prompt */
  NameFor name_for;
  char    name[40];
  int     name_len;
} E;

static CRect rect(int x, int y, int w, int h) {
  CRect r;
  r.x = (short)x; r.y = (short)y; r.w = (short)w; r.h = (short)h;
  return r;
}

static void say(const char *m) { api->fmt(E.status, sizeof E.status, "%s", m); }

/* ------------------------------------------------------------- buffer ---- */

static void blank(void) {
  api->mem_set(E.line, 0, sizeof E.line);
  api->mem_set(E.len, 0, sizeof E.len);
  E.nlines = 1;
  E.cx = E.cy = E.top = E.leftcol = 0;
  E.dirty = 0;
  E.truncated = 0;
}

static void load(const char *path) {
  char buf[512];
  int fd, n, i, col = 0;

  blank();
  api->fmt(E.path, sizeof E.path, "%s", path);

  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) { say("new file"); return; }

  while ((n = api->read(fd, buf, sizeof buf)) > 0) {
    for (i = 0; i < n; i++) {
      char c = buf[i];
      if (c == 13) continue;
      if (c == 10) {
        E.len[E.nlines - 1] = (short)col;
        col = 0;
        if (E.nlines >= MAXLINES) { E.truncated = 1; goto out; }
        E.nlines++;
        continue;
      }
      if (c == 9) {                     /* tabs become two spaces */
        int t;
        for (t = 0; t < 2 && col < MAXCOL; t++) E.line[E.nlines - 1][col++] = ' ';
        continue;
      }
      /* A control byte means this is not text. Say so rather than drawing
       * 14 KB of ELF as characters. */
      if (c < 32 || (unsigned char)c > 126) { E.truncated = 2; goto out; }
      if (col < MAXCOL) E.line[E.nlines - 1][col++] = c;
      else E.truncated = 1;
    }
  }
  E.len[E.nlines - 1] = (short)col;
out:
  api->close(fd);
  if (E.truncated == 2) {
    blank();
    say("not a text file");
  } else if (E.truncated) {
    say("opened, truncated to fit");
  } else {
    api->fmt(E.status, sizeof E.status, "%d lines", E.nlines);
  }
}

static void begin_name(NameFor why, const char *initial) {
  E.name_for = why;
  api->fmt(E.name, sizeof E.name, "%s", initial ? initial : "");
  E.name_len = (int)api->str_len(E.name);
  E.view = VIEW_NAME;
}

static void save(void) {
  int fd, i;
  char nl = 10;

  /* Empty rather than pre-filled: a buffer with no name came from a pipe or a
   * new file, and the first thing typed should be the name rather than the
   * end of a name you have to delete first. */
  if (!E.path[0]) { begin_name(NAME_SAVE_AS, ""); return; }
  fd = api->open(E.path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) { say("cannot write"); return; }
  for (i = 0; i < E.nlines; i++) {
    if (E.len[i]) api->write(fd, E.line[i], (size_t)E.len[i]);
    if (i + 1 < E.nlines) api->write(fd, &nl, 1);
  }
  api->close(fd);
  E.dirty = 0;
  say("saved");
}

/* ------------------------------------------------------------ browser ---- */

static void rescan(void) {
  static char raw[DIRMAX][NAMELEN];
  int n, i;

  E.ndir = 0;
  E.bsel = 0;
  n = api->list(E.dir, &raw[0][0], DIRMAX, NAMELEN);
  if (n < 0) { say("cannot read folder"); return; }
  for (i = 0; i < n && E.ndir < DIRMAX; i++) {
    api->fmt(E.names[E.ndir], NAMELEN, "%s", raw[i]);
    E.ndir++;
  }
  api->fmt(E.status, sizeof E.status, "%s  %d items", E.dir, E.ndir);
}

static void go_up(void) {
  int i, cut = 0;
  for (i = 0; E.dir[i]; i++) if (E.dir[i] == '/') cut = i;
  E.dir[cut ? cut : 1] = 0;
  rescan();
}

static void open_selected(void) {
  char path[96];
  int fd;

  if (E.bsel < 0 || E.bsel >= E.ndir) return;

  api->fmt(path, sizeof path, "%s%s%s", E.dir,
           E.dir[1] == 0 ? "" : "/", E.names[E.bsel]);

  /* A folder cannot be opened for reading, which is how this tells the two
   * apart -- there is no is_dir in the API an app is given. */
  fd = api->open(path, CAPP_O_READ);
  if (fd < 0) {
    api->fmt(E.dir, sizeof E.dir, "%s", path);
    rescan();
    return;
  }
  api->close(fd);

  load(path);
  E.view = VIEW_EDIT;
}

/* ------------------------------------------------------------- editing --- */

static void scroll_to_cursor(int rows, int cols) {
  if (E.cy < E.top) E.top = E.cy;
  if (E.cy >= E.top + rows) E.top = E.cy - rows + 1;
  if (E.top < 0) E.top = 0;

  if (E.cx < E.leftcol) E.leftcol = E.cx;
  if (E.cx >= E.leftcol + cols) E.leftcol = E.cx - cols + 1;
  if (E.leftcol < 0) E.leftcol = 0;
}

static void insert_char(char c) {
  int i, n = E.len[E.cy];
  if (n >= MAXCOL) return;
  for (i = n; i > E.cx; i--) E.line[E.cy][i] = E.line[E.cy][i - 1];
  E.line[E.cy][E.cx] = c;
  E.len[E.cy] = (short)(n + 1);
  E.cx++;
  E.dirty = 1;
}

static void split_line(void) {
  int i, tail;
  if (E.nlines >= MAXLINES) { say("too many lines"); return; }
  for (i = E.nlines; i > E.cy + 1; i--) {
    api->mem_cpy(E.line[i], E.line[i - 1], MAXCOL + 1);
    E.len[i] = E.len[i - 1];
  }
  E.nlines++;
  tail = E.len[E.cy] - E.cx;
  api->mem_cpy(E.line[E.cy + 1], E.line[E.cy] + E.cx, (size_t)tail);
  E.len[E.cy + 1] = (short)tail;
  E.len[E.cy] = (short)E.cx;

  /* Keep the new line's indent. An editor that drops back to column zero on
   * every return is an editor you fight. */
  {
    int ind = 0;
    while (ind < E.len[E.cy] && E.line[E.cy][ind] == ' ') ind++;
    if (ind > 0 && E.len[E.cy + 1] + ind <= MAXCOL) {
      for (i = E.len[E.cy + 1]; i >= 0; i--)
        E.line[E.cy + 1][i + ind] = E.line[E.cy + 1][i];
      for (i = 0; i < ind; i++) E.line[E.cy + 1][i] = ' ';
      E.len[E.cy + 1] = (short)(E.len[E.cy + 1] + ind);
      E.cx = ind;
    } else {
      E.cx = 0;
    }
  }
  E.cy++;
  E.dirty = 1;
}

static void join_prev(void) {
  int i, prev;
  if (E.cy == 0) return;
  prev = E.len[E.cy - 1];
  if (prev + E.len[E.cy] > MAXCOL) { say("line would be too long"); return; }
  api->mem_cpy(E.line[E.cy - 1] + prev, E.line[E.cy], (size_t)E.len[E.cy]);
  E.len[E.cy - 1] = (short)(prev + E.len[E.cy]);
  for (i = E.cy; i + 1 < E.nlines; i++) {
    api->mem_cpy(E.line[i], E.line[i + 1], MAXCOL + 1);
    E.len[i] = E.len[i + 1];
  }
  E.nlines--;
  E.cy--;
  E.cx = prev;
  E.dirty = 1;
}

static void backspace(void) {
  int i;
  if (E.cx == 0) { join_prev(); return; }
  for (i = E.cx - 1; i + 1 < E.len[E.cy]; i++) E.line[E.cy][i] = E.line[E.cy][i + 1];
  E.len[E.cy]--;
  E.cx--;
  E.dirty = 1;
}

/* The name is joined to the folder being browsed, so "notes.txt" lands where
 * you were looking rather than at the root. */
static void finish_name(void) {
  char path[96];

  if (E.name_len == 0) { E.view = VIEW_EDIT; return; }
  api->fmt(path, sizeof path, "%s%s%s", E.dir, E.dir[1] == 0 ? "" : "/", E.name);

  if (E.name_for == NAME_NEW) {
    blank();
    api->fmt(E.path, sizeof E.path, "%s", path);
    say("new file, ctrl-s saves");
  } else {
    api->fmt(E.path, sizeof E.path, "%s", path);
    E.view = VIEW_EDIT;
    save();
    return;
  }
  E.view = VIEW_EDIT;
}

/* ------------------------------------------------------------ painting --- */

static void paint_browse(CRect c) {
  int rows = (c.h - ROWH) / ROWH;
  int top = 0, i;

  api->fill(c, CLR_BG);
  if (E.bsel >= rows) top = E.bsel - rows + 1;

  for (i = 0; i < rows && top + i < E.ndir; i++) {
    int idx = top + i;
    short y = (short)(c.y + i * ROWH);
    int sel = (idx == E.bsel);
    api->fill(rect(c.x, y, c.w, ROWH), sel ? CLR_SEL : CLR_BG);
    api->text((short)(c.x + 3), y, E.names[idx],
              sel ? CLR_BAR_FG : CLR_TEXT, sel ? CLR_SEL : CLR_BG);
  }
  if (E.ndir == 0)
    api->text((short)(c.x + 3), c.y, "empty", CLR_DIM, CLR_BG);

  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  api->text((short)(c.x + 2), (short)(c.y + c.h - ROWH + 1), E.status,
            CLR_BAR_FG, CLR_BAR);
}

/* ------------------------------------------------------- markdown ---- */

/* A preview, not a parser.
 *
 * Markdown is a large specification and almost none of it earns its place on
 * a 240-pixel screen. What is here is what people actually write in notes:
 * headings, lists, quotes, code, rules, and bold or `code` inside a line.
 * Anything unrecognised is drawn as the text it is, which is markdown's whole
 * premise and a good fallback.
 *
 * It renders from the edit buffer rather than the file, so a preview shows
 * what you have typed and not what you last saved.
 *
 * There is no font but the 6x8 one and no way to scale it -- the API draws
 * text one way. Weight is therefore faked by drawing a glyph twice, one pixel
 * apart, which at this size reads convincingly as bold; headings get that
 * plus colour plus a rule under the big ones. Italics have no honest
 * equivalent, so emphasis is shown in colour instead of being invented.
 */

/* ------------------------------------------------------- running it ---- */

#define CON_LINES 6
#define CON_COLS  38
#define EXEC_MAX  4096          /* generated code; a long program is ~2 KB */

static struct {
  int   console;                /* the pane is showing */
  char  out[CON_LINES][CON_COLS + 1];
  int   nout;
  int   native;                 /* the last run was compiled */
  AsmProgram prog;
  AsmState   st;
} G;

static int console_h(void) { return G.console ? CON_LINES * 8 + 2 : 0; }

/* The console scrolls rather than wraps: six lines is not enough to justify
 * reflowing, and a truncated line is easier to read than a lost one. */
static void con_line(const char *s) {
  int i;
  if (G.nout >= CON_LINES) {
    for (i = 1; i < CON_LINES; i++)
      api->mem_cpy(G.out[i - 1], G.out[i], sizeof G.out[0]);
    G.nout = CON_LINES - 1;
  }
  api->fmt(G.out[G.nout], sizeof G.out[0], "%s", s);
  G.nout++;
  G.console = 1;
}

/* Characters arrive one at a time from SYS_PUTC, so they accumulate on the
 * last line rather than each becoming one. */
static void con_char(char c) {
  int n;
  if (c == '\n') { con_line(""); return; }
  if (!G.nout) con_line("");
  n = (int)api->str_len(G.out[G.nout - 1]);
  if (n + 1 < CON_COLS) {
    G.out[G.nout - 1][n] = c;
    G.out[G.nout - 1][n + 1] = 0;
  }
  G.console = 1;
}

static void con_clear(void) {
  api->mem_set(G.out, 0, sizeof G.out);
  G.nout = 0;
}

/* The one place both back ends meet. The interpreter calls this directly and
 * compiled code calls it through a pointer in its literal pool, so a program
 * cannot behave differently depending on how it was run. */
static void ide_sys(void *ctx, int call, AsmState *st) {
  char buf[16];
  (void)ctx;
  switch (call) {
  case SYS_PUTC:  con_char((char)st->r[0]); break;
  case SYS_PUTI:  api->fmt(buf, sizeof buf, "%d", (int)st->r[0]);
                  { int i; for (i = 0; buf[i]; i++) con_char(buf[i]); }
                  break;
  case SYS_TICKS: st->r[0] = (int32_t)api->ticks_ms(); break;
  case SYS_KEY:   st->r[0] = api->key_pending(); break;
  default: break;
  }
}

/* The buffer as one string, which is what the assembler wants. Built on the
 * stack rather than kept, because it is only alive for the length of an
 * assemble. */
static void gather(char *out, int max) {
  int i, n = 0;
  for (i = 0; i < E.nlines && n < max - 2; i++) {
    int len = E.len[i];
    if (n + len + 1 >= max) len = max - n - 2;
    if (len > 0) { api->mem_cpy(out + n, E.line[i], (size_t)len); n += len; }
    out[n++] = '\n';
  }
  out[n] = 0;
}

/* An assembly error goes to the line it is on, because that is the whole
 * difference between an error message and a scavenger hunt. */
static int assemble_buffer(void) {
  static char src[MAXLINES * (MAXCOL + 1)];
  gather(src, (int)sizeof src);
  if (asm_assemble(&G.prog, src)) return 1;
  if (G.prog.err_line >= 1 && G.prog.err_line <= E.nlines) {
    E.cy = G.prog.err_line - 1;
    E.cx = 0;
  }
  api->fmt(E.status, sizeof E.status, "line %d: %s", G.prog.err_line, G.prog.err);
  con_line(E.status);
  return 0;
}

static const char *stop_name(AsmStop s) {
  switch (s) {
  case RUN_HALT:   return "halted";
  case RUN_FAULT:  return "faulted";
  case RUN_STEPS:  return "ran too long";
  case RUN_NOCODE: return "nothing to run";
  default:         return "stopped";
  }
}

static void report(AsmStop stop, uint32_t ms) {
  char buf[CON_COLS + 1];
  if (stop == RUN_FAULT)
    api->fmt(buf, sizeof buf, "%s at %lu", stop_name(stop),
             (unsigned long)G.st.fault_addr);
  else
    api->fmt(buf, sizeof buf, "%s, r0=%d in %lums", stop_name(stop),
             (int)G.st.r[0], (unsigned long)ms);
  con_line(buf);
  api->fmt(E.status, sizeof E.status, "%s", buf);
}

/* Compile into executable RAM and jump to it.
 *
 * Two pointers to the same memory: `exec` is the one to call, `writable` the
 * byte-addressable alias to build in. Writing through `exec` faults -- that
 * window only permits aligned 32-bit access. See CardApi.exec_alloc. */
static AsmStop run_native(uint32_t *ms) {
  void *exec = api->exec_alloc(EXEC_MAX);
  uint8_t *w;
  AsmEmit e;
  uint32_t t0;
  int (*entry)(AsmState *);

  if (!exec) { con_line("no executable RAM"); return RUN_NOCODE; }
  w = (uint8_t *)api->exec_writable(exec);

  api->mem_set(&e, 0, sizeof e);
  e.buf = w;
  e.cap = EXEC_MAX;
  if (!asm_compile(&G.prog, &e, (uint32_t)(size_t)ide_sys)) {
    api->exec_free(exec);
    con_line("the program is too big to compile");
    return RUN_NOCODE;
  }

  /* Entry is the first instruction after the literal pool, through the
   * executable window rather than the writable one. */
  entry = (int (*)(AsmState *))(void *)((uint8_t *)exec + e.code0);
  t0 = api->ticks_ms();
  entry(&G.st);
  *ms = api->ticks_ms() - t0;
  api->exec_free(exec);
  return RUN_HALT;
}

static void build_and_run(int native) {
  uint32_t ms = 0, t0;
  AsmStop stop;

  con_clear();
  if (!assemble_buffer()) return;

  api->mem_set(&G.st, 0, sizeof G.st);
  G.native = native;

  if (native) {
    con_line("compiled");
    stop = run_native(&ms);
  } else {
    t0 = api->ticks_ms();
    stop = asm_run(&G.prog, &G.st, ASM_STEPS, ide_sys, 0);
    ms = api->ticks_ms() - t0;
  }
  report(stop, ms);
}

/* Run both and compare.
 *
 * The host tests can check every instruction template against the bytes the
 * real assembler produces, but they cannot execute the result -- the machine
 * that runs them is x86. This is the only place the compiler is actually
 * tested against the interpreter, so it lives in the app rather than in the
 * suite. */
static void verify_both(void) {
  AsmState want;
  uint32_t ms = 0;
  int i, bad = -1;
  char buf[CON_COLS + 1];

  con_clear();
  if (!assemble_buffer()) return;

  api->mem_set(&G.st, 0, sizeof G.st);
  asm_run(&G.prog, &G.st, ASM_STEPS, ide_sys, 0);
  api->mem_cpy(&want, &G.st, sizeof want);

  api->mem_set(&G.st, 0, sizeof G.st);
  if (run_native(&ms) == RUN_NOCODE) return;

  for (i = 0; i < ASM_REGS; i++)
    if (G.st.r[i] != want.r[i]) { bad = i; break; }

  if (bad >= 0) {
    api->fmt(buf, sizeof buf, "r%d: vm %d, native %d",
             bad, (int)want.r[bad], (int)G.st.r[bad]);
    con_line("MISMATCH");
    con_line(buf);
  } else {
    int mem_bad = 0;
    for (i = 0; i < ASM_DATA; i++)
      if (G.st.mem[i] != want.mem[i]) { mem_bad = 1; break; }
    api->fmt(buf, sizeof buf, mem_bad ? "registers agree, memory does not"
                                      : "both agree, %lums native",
             (unsigned long)ms);
    con_line(buf);
  }
  api->fmt(E.status, sizeof E.status, "%s", G.out[G.nout - 1]);
}

static void paint_console(CRect c) {
  int h = console_h(), i;
  short y0;
  if (!h) return;
  y0 = (short)(c.y + c.h - ROWH - h);
  api->fill(rect(c.x, y0, c.w, h), CLR_GUTTER);
  api->fill(rect(c.x, y0, c.w, 1), CLR_SEL);
  for (i = 0; i < G.nout && i < CON_LINES; i++)
    api->text((short)(c.x + 2), (short)(y0 + 2 + i * 8), G.out[i],
              CLR_TEXT, CLR_GUTTER);
}

static void paint_edit(CRect c) {
  int rows = (c.h - ROWH - console_h()) / ROWH;
  int cols = (c.w - GUTTER) / CHARW;
  char buf[MAXCOL + 8];
  int r;

  if (rows < 1) rows = 1;
  if (cols < 1) cols = 1;
  scroll_to_cursor(rows, cols);

  /* No full-screen clear. Each row paints its own background as it goes, so a
   * keystroke redraws rows rather than wiping 240x135 to one colour and
   * drawing over it -- which at 40MHz is 12ms of flat background on every
   * character typed, and reads as a flash. */
  for (r = 0; r < rows; r++) {
    int i = E.top + r;
    short y = (short)(c.y + r * ROWH);
    int on_cursor = (i == E.cy);
    uint16_t bg = on_cursor ? CLR_CUR_BG : CLR_BG;
    int n;

    api->fill(rect(c.x, y, GUTTER, ROWH), CLR_GUTTER);
    api->fill(rect(c.x + GUTTER, y, c.w - GUTTER, ROWH), bg);

    if (i >= E.nlines) continue;      /* cleared, so deleted lines disappear */

    api->fmt(buf, sizeof buf, "%3d", i + 1);
    api->text((short)(c.x + 1), y, buf,
              on_cursor ? CLR_TEXT : CLR_LINENO, CLR_GUTTER);

    n = E.len[i] - E.leftcol;
    if (n > cols) n = cols;
    if (n > 0) {
      api->mem_cpy(buf, E.line[i] + E.leftcol, (size_t)n);
      buf[n] = 0;
      api->text((short)(c.x + GUTTER), y, buf, CLR_TEXT, bg);
    }

    if (on_cursor) {
      short cxp = (short)(c.x + GUTTER + (E.cx - E.leftcol) * CHARW);
      api->fill(rect(cxp, y, 1, 8), CLR_CARET);
    }
  }

  /* Status bar: the two things you look down for are which file and whether it
   * is saved. The dot is the unsaved marker, coloured rather than lettered so
   * it reads without being parsed. */
  paint_console(c);
  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  if (E.dirty) api->fill(rect(c.x + 2, c.y + c.h - ROWH + 3, 3, 3), CLR_DIRTY);
  api->fmt(buf, sizeof buf, "%s  %d:%d  %s", E.path, E.cy + 1, E.cx + 1, E.status);
  api->text((short)(c.x + 7), (short)(c.y + c.h - ROWH + 1), buf,
            CLR_BAR_FG, CLR_BAR);
}

static void paint_name(CRect c) {
  char shown[sizeof E.name + 2];
  short y = (short)(c.y + c.h / 2 - 18);

  api->fill(c, CLR_BG);
  api->text((short)(c.x + 8), y,
            E.name_for == NAME_NEW ? "New file" : "Save as", CLR_BAR_FG, CLR_BG);
  api->text((short)(c.x + 8), (short)(y + 11), E.dir, CLR_DIM, CLR_BG);

  api->fill(rect(c.x + 6, y + 24, c.w - 12, 13), CLR_CUR_BG);
  api->fill(rect(c.x + 6, y + 24, c.w - 12, 1), CLR_SEL);
  api->mem_cpy(shown, E.name, (size_t)E.name_len);
  shown[E.name_len] = '_';
  shown[E.name_len + 1] = 0;
  api->text((short)(c.x + 9), (short)(y + 27), shown, CLR_TEXT, CLR_CUR_BG);

  api->fill(rect(c.x, c.y + c.h - ROWH, c.w, ROWH), CLR_BAR);
  api->text((short)(c.x + 3), (short)(c.y + c.h - ROWH + 1),
            "enter confirms   backspace cancels when empty", CLR_BAR_FG, CLR_BAR);
}

/* What this editor can be asked to do. Keys map onto these and so do menu
 * items; see apps/toolbar.h for why a menu item is never a keystroke. */
enum { ACT_NEW = 1, ACT_SAVE, ACT_SAVEAS, ACT_OPEN, ACT_VM, ACT_NATIVE,
       ACT_VERIFY, ACT_CONSOLE };

static const CappAction EDIT_ACTIONS[] = {
  { "new",     "New",         "File", 0x0E, ACT_NEW },     /* ctrl-n */
  { "save",    "Save",        "File", 0x13, ACT_SAVE },    /* ctrl-s */
  { "saveas",  "Save as",     "File", 0x12, ACT_SAVEAS },  /* ctrl-r */
  { "close",   "Close",       "File", 0x0F, ACT_OPEN },    /* ctrl-o */
  { "run.vm",  "On the VM",   "Run",  0x02, ACT_VM },      /* ctrl-b */
  { "run.native", "Compile+run", "Run", 0x0C, ACT_NATIVE },/* ctrl-l */
  { "verify",  "Verify both", "Run",  0x18, ACT_VERIFY },  /* ctrl-x */
  { "console", "Console",     "View", 0x10, ACT_CONSOLE }, /* ctrl-p */
};
static const TbIcon EDIT_ICONS[] = { { "R", ACT_VM } };

#define NEDIT ((int)(sizeof EDIT_ACTIONS / sizeof EDIT_ACTIONS[0]))

static int do_action(int a) {
  switch (a) {
  case ACT_NEW:     begin_name(NAME_NEW, ""); return 1;
  case ACT_SAVE:    save(); return 1;
  case ACT_SAVEAS:  begin_name(NAME_SAVE_AS, E.path[0] ? E.path : "untitled.s"); return 1;
  case ACT_OPEN:    E.view = VIEW_BROWSE; rescan(); return 1;
  case ACT_VM:      build_and_run(0); return 1;
  case ACT_NATIVE:  build_and_run(1); return 1;
  case ACT_VERIFY:  verify_both(); return 1;
  case ACT_CONSOLE: G.console = !G.console; return 1;
  default: return 0;
  }
}

static void app_paint(void *st, CRect c) {
  (void)st;
  /* The bar belongs to the editor. The browser and the name prompt are
   * whole screens of their own and have nothing to put on it. */
  if (E.view == VIEW_BROWSE) { paint_browse(c); return; }
  if (E.view == VIEW_NAME) { paint_name(c); return; }
  {
    CRect full = c;
    /* Only the dropdown moved: draw it and nothing else, or the content
     * under the menu is repainted on every mouse move and flickers. */
    if (toolbar_only_menu()) { toolbar_paint_menu(full); return; }
    toolbar_paint_bar(full);
    paint_edit(toolbar_rest(full));
    toolbar_paint_menu(full);
  }
}


/* --------------------------------------------------------------- input --- */

static int key_browse(unsigned char k) {
  switch (k) {
  case CAPP_KEY_UP:   if (E.bsel > 0) E.bsel--; return 1;
  case CAPP_KEY_DOWN: if (E.bsel + 1 < E.ndir) E.bsel++; return 1;
  case CAPP_KEY_LEFT:
  case CAPP_KEY_BACK: go_up(); return 1;
  case CAPP_KEY_RIGHT:
  case CAPP_KEY_ENTER: open_selected(); return 1;
  case 'n': case 'N':
    begin_name(NAME_NEW, "");
    return 1;
  case 'r': case 'R': rescan(); return 1;
  default: return 0;
  }
}

static int key_edit(unsigned char k) {
  switch (k) {
  case CAPP_KEY_LEFT:
    if (E.cx > 0) E.cx--;
    else if (E.cy > 0) { E.cy--; E.cx = E.len[E.cy]; }
    return 1;
  case CAPP_KEY_RIGHT:
    if (E.cx < E.len[E.cy]) E.cx++;
    else if (E.cy + 1 < E.nlines) { E.cy++; E.cx = 0; }
    return 1;
  case CAPP_KEY_UP:
    if (E.cy > 0) { E.cy--; if (E.cx > E.len[E.cy]) E.cx = E.len[E.cy]; }
    return 1;
  case CAPP_KEY_DOWN:
    if (E.cy + 1 < E.nlines) { E.cy++; if (E.cx > E.len[E.cy]) E.cx = E.len[E.cy]; }
    return 1;

  case CAPP_KEY_BACK:  backspace(); return 1;
  case CAPP_KEY_ENTER: split_line(); return 1;

  case 0x01: E.cx = 0; return 1;                  /* ctrl-a */
  case 0x05: E.cx = E.len[E.cy]; return 1;        /* ctrl-e */

  default:
    if (k >= 32 && k < 127) { insert_char((char)k); return 1; }
    return 0;
  }
}

static int key_name(unsigned char k) {
  if (k == CAPP_KEY_ENTER) { finish_name(); return 1; }
  if (k == CAPP_KEY_BACK) {
    if (E.name_len > 0) E.name[--E.name_len] = 0;
    else E.view = (E.name_for == NAME_NEW) ? VIEW_BROWSE : VIEW_EDIT;
    return 1;
  }
  /* No slashes: this names a file in the folder being browsed, and a path
   * typed here would silently land somewhere else. */
  if (k >= 32 && k < 127 && k != '/' && E.name_len < (int)sizeof E.name - 1) {
    E.name[E.name_len++] = (char)k;
    E.name[E.name_len] = 0;
    return 1;
  }
  return 0;
}

/* What the shell calls for a chord out of the table, and what the menu bar
 * and any script reach through. */
static int app_action(void *st, int a) {
  (void)st;
  return do_action(a);
}

static int app_key(void *st, unsigned char k) {
  (void)st;
  if (E.view == VIEW_BROWSE) return key_browse(k);
  if (E.view == VIEW_NAME) return key_name(k);
  return key_edit(k);
}

static int app_click(void *st, short x, short y, int button) {
  (void)st; (void)button;

  if (E.view == VIEW_BROWSE) {
    int i = y / ROWH;
    if (i < 0 || i >= E.ndir) return 0;
    if (i == E.bsel) open_selected();
    else E.bsel = i;
    return 1;
  }

  {
    int a = toolbar_click(x, y);
    if (a == TB_CONSUMED) return 1;            /* opened or closed a menu */
    if (a != TB_NONE) return do_action(a);
  }

  {
    int r, i;
    y = (short)(y - toolbar_h());
    r = y / ROWH;
    i = E.top + r;
    if (i < 0 || i >= E.nlines) return 0;
    E.cy = i;
    E.cx = E.leftcol + (x - GUTTER) / CHARW;
    if (E.cx < 0) E.cx = 0;
    if (E.cx > E.len[i]) E.cx = E.len[i];
    return 1;
  }
}

/* The wheel scrolls the view without moving the cursor -- looking somewhere
 * else is not the same as typing there, and a wheel that dragged the caret
 * around would lose your place. */
static int app_mouse(void *st, short x, short y, int buttons, int wheel) {
  int changed;
  (void)st; (void)buttons;

  changed = toolbar_saw_mouse();
  if (toolbar_hover(x, y)) changed = 1;
  if (!wheel) return changed;

  if (E.view == VIEW_BROWSE) {
    E.bsel -= wheel * 2;
    if (E.bsel < 0) E.bsel = 0;
    if (E.bsel >= E.ndir) E.bsel = E.ndir - 1;
    return 1;
  }
  if (E.view != VIEW_EDIT) return 0;

  E.top -= wheel * 3;
  if (E.top > E.nlines - 1) E.top = E.nlines - 1;
  if (E.top < 0) E.top = 0;
  return 1;
}

/* Only while editing. In the browser the arrows move a selection, and a
 * machine whose arrow keys are ; . , / needs those back. */
static int app_wants_text(void *st) {
  (void)st;
  return E.view == VIEW_EDIT || E.view == VIEW_NAME;
}

static void app_open(void *st) {
  (void)st;
  if (!E.dir[0]) api->fmt(E.dir, sizeof E.dir, "%s", "/");
  E.view = VIEW_BROWSE;
  rescan();
}

static void app_set_args(void *st, const char *path) {
  (void)st;
  load(path);
  E.view = VIEW_EDIT;
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "IDE",
  /* 16x16: a terminal with a prompt and a caret. */
  { 0x00, 0x00, 0x7F, 0xFE, 0x40, 0x02, 0x40, 0x02,
    0x48, 0x02, 0x44, 0x02, 0x42, 0x02, 0x44, 0x02,
    0x48, 0x02, 0x40, 0x02, 0x43, 0x82, 0x40, 0x02,
    0x40, 0x02, 0x7F, 0xFE, 0x00, 0x00, 0x00, 0x00 },
  "ctrl-b\trun on the VM\nctrl-l\tcompile to Xtensa and run\nctrl-x\trun both and compare\nctrl-p\tshow or hide the console\nctrl-s\tsave\nctrl-n\tnew file\nctrl-r\tsave as\nctrl-o\tback to the file list\nctrl-a\tstart of line\nctrl-e\tend of line\n",
};

/* Static, not a local: the shell keeps calling into this long after
 * capp_main has returned. */
static CappUi UI;

/* Pulls whatever was piped in into the buffer. "cat notes | grep TODO | edit"
 * is the case that matters: the left-hand side produced text and this is where
 * you want to look at it. It stays an unnamed buffer until saved, which is
 * what an editor should do with something that arrived without a file. */
static void load_stdin(void) {
  char line[MAXCOL + 64];
  int n;

  blank();
  if (!E.dir[0]) api->fmt(E.dir, sizeof E.dir, "%s", "/");
  E.path[0] = 0;
  while (E.nlines < MAXLINES && (n = api->in_line(line, sizeof line)) >= 0) {
    if (n > MAXCOL) n = MAXCOL;
    api->mem_cpy(E.line[E.nlines - 1], line, (size_t)n);
    E.len[E.nlines - 1] = (short)n;
    E.nlines++;
  }
  if (E.nlines > 1) E.nlines--;        /* the last increment had no line */
  E.dirty = 1;                         /* nothing on the card holds this yet */
  E.view = VIEW_EDIT;
  api->fmt(E.status, sizeof E.status, "%d lines in, ctrl-s names it", E.nlines);
}

int capp_main(const CardApi *a, int argc, char **argv) {
  api = a;
  blank();
  /* Three ways in, in the order a shell would expect: something piped in, a
     named file, or nothing -- and nothing means the browser, because an editor
     with no file has nothing to do. */
  if (api->has_input()) load_stdin();
  else if (argc > 1) app_set_args(0, argv[1]);
  else app_open(0);
  toolbar_init(api, EDIT_ACTIONS, NEDIT, EDIT_ICONS, 1);

  UI.paint = app_paint;
  UI.key = app_key;
  UI.click = app_click;
  UI.mouse = app_mouse;
  UI.wants_text = app_wants_text;
  UI.actions = EDIT_ACTIONS;
  UI.nactions = NEDIT;
  UI.action = app_action;
  api->ui(&UI);
  return 0;
}
