/* Python: MicroPython on the device, a REPL to play with (2026-10-16).
 *
 * The interpreter is MicroPython's embed port, generated into apps/python/mp
 * from apps/python/mpconfigport.h (tools/make_micropython.sh) and built into
 * this one app (tools/build_apps.py, MULTI): the only app linked against the
 * toolchain's libc, libm and libgcc, and still checked for anything they
 * leave unresolved. Its code runs from flash like any app's.
 *
 * Memory: the app's own state is in the arena as usual; the Python heap is
 * big-mode blocks (CAPP_BIG), several of them as one heap (MicroPython's
 * split heap). It declares no network, so entering lets go of WiFi too.
 *
 * It runs on the shell's stack, so Python calls are stackless (frames on
 * the heap) and the C stack is checked. Nothing else runs while Python
 * does: a key held down stops it (KeyboardInterrupt, cardpy_poll).
 */
#include <setjmp.h>
#include <string.h>

#include "kernel/app/capp.h"
#define TL_LINES 120
#include "apps/termlog.h"

#include "py/compile.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "py/stackctrl.h"

#define HEAP_WANT   (112 * 1024)   /* as much as big mode will give, up to this */
#define HEAP_BLOCKS 6
#ifndef STACK_LIMIT
#define STACK_LIMIT (3 * 1024)     /* of the shell's 8 KB, for the compiler and the VM */
#endif
#define SRC_MAX     2048
#define IN_MAX      200

#define BG    0x0000
#define FG    0xFFFF
#define DIM   0x8410
#define BAR   0x2124
#define C_IN  0x07E0               /* what was typed: green */
#define C_ERR 0xF9A6

enum { T_OUT, T_IN, T_ERR };

static const CardApi *api;
static TermLog L;
static char in[IN_MAX + 1];
static int in_n;
static char src[SRC_MAX + 1];      /* a block being typed, lines joined by \n */
static int src_n;
static char outl[TL_COLS * 4 + 1]; /* stdout not yet ended by a newline */
static int out_n;
static int out_tag;
static size_t heap_total;
static int ready;
static int armed;                  /* the key that started a run has been let go */

/* ---- stdout ------------------------------------------------------------------ */

static void flush_out(void) {
  if (!out_n) return;
  outl[out_n] = 0;
  tl_push_text(api, &L, outl, out_tag, 0);
  out_n = 0;
}

void mp_hal_stdout_tx_strn_cooked(const char *s, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] == '\n') {
      outl[out_n] = 0;
      tl_push_text(api, &L, outl, out_tag, 0);
      out_n = 0;
    } else if (s[i] != '\r') {
      if (out_n == (int)sizeof outl - 1) flush_out();
      outl[out_n++] = s[i];
    }
  }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *s, size_t n) {
  mp_hal_stdout_tx_strn_cooked(s, n);
  return n;
}

void mp_hal_stdout_tx_str(const char *s) { mp_hal_stdout_tx_strn_cooked(s, strlen(s)); }

/* ---- what MicroPython asks of its port ----------------------------------------- */

/* newlib's maths functions set errno through its reentrancy layer, which an
 * app does not have; one errno of its own is all they need. */
int *__errno(void) {
  static int e;
  return &e;
}

/* The registers into memory, then the stack from here to where the run
 * began: setjmp spills every live register window (a syscall the kernel's
 * vectors handle), so a pointer held only in a register is on the stack. */
void gc_collect(void) {
  jmp_buf regs;
  gc_collect_start();
  setjmp(regs);
  gc_collect_root((void **)(void *)&regs,
                  ((uintptr_t)MP_STATE_THREAD(stack_top) - (uintptr_t)&regs) / sizeof(uintptr_t));
  gc_collect_end();
}

/* An exception with nowhere to go: there is no recovering the interpreter,
 * but the machine is not stopped for it. */
void nlr_jump_fail(void *val) {
  (void)val;
  api->log("python: nlr_jump_fail");
  for (;;) {
  }
}

/* open() is the port's to give; files on the card come later. */
static mp_obj_t cardpy_open(size_t n_args, const mp_obj_t *args, mp_map_t *kw) {
  (void)n_args;
  (void)args;
  (void)kw;
  mp_raise_OSError(MP_EPERM);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, cardpy_open);

void cardpy_poll(void) {
  if (!armed) {
    if (!api->key_pending()) armed = 1;
  } else if (api->key_pending()) {
    mp_sched_keyboard_interrupt();
    armed = 0;
  }
}

/* ---- running ------------------------------------------------------------------- */

static void run(const char *text) {
  nlr_buf_t nlr;
  volatile int top = 0;
  mp_stack_set_top((void *)&top);
  mp_stack_set_limit(STACK_LIMIT);
  armed = 0;
  out_tag = T_OUT;
  if (nlr_push(&nlr) == 0) {
    mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, text, strlen(text), 0);
    qstr name = lex->source_name;
    mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_SINGLE_INPUT);
    mp_obj_t fun = mp_compile(&tree, name, true);
    mp_call_function_0(fun);
    nlr_pop();
  } else {
    flush_out();
    out_tag = T_ERR;
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
  }
  flush_out();
  out_tag = T_OUT;
  (void)top;
}

static void say(const char *s, int tag) { tl_push_text(api, &L, s, tag, 0); }

/* Enter: the line joins the block, which runs once it is whole. */
static void submit(void) {
  char echo[IN_MAX + 8];
  api->fmt(echo, sizeof echo, "%s %s", src_n ? "..." : ">>>", in);
  say(echo, T_IN);
  if (src_n + in_n + 2 > SRC_MAX) {
    say("too long for one block", T_ERR);
    src_n = 0;
    in_n = 0;
    in[0] = 0;
    return;
  }
  if (src_n) src[src_n++] = '\n';
  memcpy(src + src_n, in, (size_t)in_n);
  src_n += in_n;
  src[src_n] = 0;
  in_n = 0;
  in[0] = 0;
  if (mp_repl_continue_with_input(src)) return;
  if (src_n) run(src);
  src_n = 0;
  src[0] = 0;
}

/* ---- the screen ------------------------------------------------------------------ */

static uint16_t colour(int tag) { return tag == T_IN ? C_IN : tag == T_ERR ? C_ERR : FG; }

static void app_paint(void *st, CRect c) {
  char bar[48];
  CRect clip = api->paint_area();
  (void)st;
  if (ready) {
    gc_info_t g;
    gc_info(&g);
    api->fmt(bar, sizeof bar, "Python  %u KB heap  %u free", (unsigned)(heap_total / 1024),
             (unsigned)(g.free / 1024));
  } else
    api->fmt(bar, sizeof bar, "Python  (no memory for a heap)");
  tl_paint_bar(api, tl_bar_rect(c), bar, FG, BAR);
  tl_paint_log(api, &L, c, clip, colour, BG);
  tl_paint_input(api, c, src_n ? "." : ">", in, in_n, C_IN, DIM, BG);
}

static int app_key(void *st, uint8_t k) {
  (void)st;
  if (k == CAPP_KEY_ENTER) {
    if (ready) submit();
    return 1;
  }
  if (k == CAPP_KEY_BACK) {
    if (in_n) in[--in_n] = 0;
    return 1;
  }
  if (k == CAPP_KEY_ESC) {               /* out of a block, else clear the line */
    if (src_n) say("(block dropped)", T_ERR);
    src_n = 0;
    src[0] = 0;
    in_n = 0;
    in[0] = 0;
    return 1;
  }
  if (k == CAPP_KEY_UP || k == CAPP_KEY_DOWN) {
    L.scroll += k == CAPP_KEY_UP ? 1 : -1;
    if (L.scroll < 0) L.scroll = 0;
    if (L.scroll > L.n) L.scroll = L.n;
    return 1;
  }
  if (k == '\t') {                        /* an indent, four spaces */
    while (in_n < IN_MAX && (in_n % 4 || k == '\t')) { in[in_n++] = ' '; k = 0; }
    in[in_n] = 0;
    return 1;
  }
  if (k >= 32 && k < 127 && in_n < IN_MAX) {
    in[in_n++] = (char)k;
    in[in_n] = 0;
    L.scroll = 0;
    return 1;
  }
  return 0;
}

static int app_wants_text(void *st) {
  (void)st;
  return 1;
}

static CappUi UI;

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_BIG,
  "Python",
  /* 16x16: two snakes, head to tail, after the logo. */
  { 0x0F, 0xC0, 0x18, 0x60, 0x12, 0x60, 0x1F, 0xE0,
    0x00, 0x60, 0x7F, 0xEE, 0xC0, 0x06, 0xC0, 0x06,
    0x60, 0x03, 0x77, 0xFE, 0x06, 0x00, 0x07, 0xF8,
    0x06, 0x48, 0x06, 0x18, 0x03, 0xF0, 0x00, 0x00 },
  "enter\trun the line (a block runs at a blank line)\n"
  "tab\tindent\nesc\tdrop the block, clear the line\n"
  "up/down\tscroll back\nany key\tstop a running program\n",
};

int capp_main(const CardApi *a, int argc, char **argv) {
  int nb = 0;
  volatile int top = 0;
  (void)argc;
  (void)argv;
  api = a;
  api->mem_set(&L, 0, sizeof L);
  heap_total = 0;
  while (nb < HEAP_BLOCKS && heap_total < HEAP_WANT) {
    size_t room = api->big_room(), n;
    uint8_t *b;
    if (room < 4096) break;
    n = room < HEAP_WANT - heap_total ? room : HEAP_WANT - heap_total;
    b = api->big_alloc(n);
    if (!b) break;
    if (nb == 0) gc_init(b, b + n);
    else gc_add(b, b + n);
    heap_total += n;
    nb++;
  }
  if (heap_total) {
    char hello[64];
    mp_stack_set_top((void *)&top);
    mp_stack_set_limit(STACK_LIMIT);
    mp_init();
    ready = 1;
    api->fmt(hello, sizeof hello, "MicroPython %s, %u KB in %d block%s", MICROPY_VERSION_STRING,
             (unsigned)(heap_total / 1024), nb, nb == 1 ? "" : "s");
    say(hello, T_OUT);
    run("print(1 + 1)");
  } else {
    say("Not enough memory for Python's heap.", T_ERR);
  }
  (void)top;
  UI.paint = app_paint;
  UI.key = app_key;
  UI.wants_text = app_wants_text;
  api->ui(&UI);
  return 0;
}
