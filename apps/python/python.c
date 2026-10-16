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

#include "py/builtin.h"
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

#define CTRL_U 0x15                /* a ctrl chord is the letter's low five bits */
#define CTRL_D 0x04

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
/* ---- history: the lines typed, newest last; up and down walk it ----------------- */

#define HIST 24
static char hist[HIST][IN_MAX + 1];
static int hist_n;                 /* lines kept */
static int hist_at = -1;           /* the one shown, or -1: the line being typed */
static char draft[IN_MAX + 1];     /* the line being typed, while walking */

static void hist_add(void) {
  int i;
  if (!in_n || (hist_n && !strcmp(hist[hist_n - 1], in))) return;   /* no blanks, no repeats */
  if (hist_n == HIST) {
    for (i = 1; i < HIST; i++) memcpy(hist[i - 1], hist[i], IN_MAX + 1);
    hist_n--;
  }
  memcpy(hist[hist_n++], in, (size_t)in_n + 1);
}

static void set_line(const char *s) {
  in_n = (int)strlen(s);
  memcpy(in, s, (size_t)in_n + 1);
}

static void hist_walk(int back) {
  if (!hist_n) return;
  if (hist_at < 0) {
    if (!back) return;
    memcpy(draft, in, (size_t)in_n + 1);
    hist_at = hist_n;
  }
  hist_at += back ? -1 : 1;
  if (hist_at < 0) hist_at = 0;
  if (hist_at >= hist_n) {                /* past the newest: what was being typed */
    hist_at = -1;
    set_line(draft);
  } else {
    set_line(hist[hist_at]);
  }
}

/* ---- tab: a name completed, or the names it could be --------------------------- */

static char comp[1024];            /* the candidates, as print_completions prints them */
static int comp_n;

static void comp_strn(void *env, const char *s, size_t n) {
  (void)env;
  while (n-- && comp_n < (int)sizeof comp - 1) comp[comp_n++] = *s++;
}

/* The candidates again, re-flowed to the screen: print_completions lays
 * them out for 64 columns, three of thirteen here. */
static void show_candidates(void) {
  char row[TL_COLS + 1];
  int i = 0, col = 0;
  comp[comp_n] = 0;
  while (comp[i]) {
    int s, w;
    while (comp[i] == ' ' || comp[i] == '\n') i++;
    if (!comp[i]) break;
    s = i;
    while (comp[i] && comp[i] != ' ' && comp[i] != '\n') i++;
    w = i - s;
    if (w > TL_COLS) w = TL_COLS;
    if (col && col + 13 > TL_COLS) {
      row[col] = 0;
      say(row, T_OUT);
      col = 0;
    }
    if (col) while (col % 13) row[col++] = ' ';
    memcpy(row + col, comp + s, (size_t)w);
    col += w > 13 ? (w + 12) / 13 * 13 : w;
    if (col > TL_COLS) col = TL_COLS;
  }
  if (col) {
    row[col] = 0;
    say(row, T_OUT);
  }
}

static const char *const KEYWORDS[] = {
  "and", "break", "class", "continue", "def", "elif", "else", "except", "False", "finally",
  "for", "from", "global", "import", "lambda", "None", "nonlocal", "not", "pass", "raise",
  "return", "True", "try", "while", "with", "yield",
};

static int is_name_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* One candidate for a plain word: the first sets *add, the rest shorten
 * *n to the prefix they share; every one is written to comp. */
static void consider(const char *cand, const char *word, size_t len, const char **add,
                     size_t *n, int *count) {
  size_t cl = strlen(cand), j;
  if (cl < len || strncmp(cand, word, len) || (!len && cand[0] == '_')) return;
  if (!*count) {
    *add = cand + len;
    *n = cl - len;
  } else {
    for (j = 0; j < *n && j < cl - len && (*add)[j] == cand[len + j]; j++) {
    }
    *n = j;
  }
  (*count)++;
  comp_strn(0, cand, cl);
  comp_strn(0, " ", 1);
}

/* A plain word: the names defined here, the builtins and the keywords --
 * mp_repl_autocomplete looks only in __main__, so `pri` found nothing. */
static size_t complete_word(const char *word, size_t len, const char **add) {
  mp_map_t *maps[2];
  size_t n = 0, i, m;
  int count = 0;
  maps[0] = &mp_globals_get()->map;
  maps[1] = &((mp_obj_dict_t *)MP_OBJ_TO_PTR(mp_module_builtins.globals))->map;
  for (m = 0; m < 2; m++)
    for (i = 0; i < maps[m]->alloc; i++)
      if (mp_map_slot_is_filled(maps[m], i) && mp_obj_is_qstr(maps[m]->table[i].key))
        consider(qstr_str(MP_OBJ_QSTR_VALUE(maps[m]->table[i].key)), word, len, add, &n,
                 &count);
  for (i = 0; i < sizeof KEYWORDS / sizeof KEYWORDS[0]; i++)
    consider(KEYWORDS[i], word, len, add, &n, &count);
  if (count > 1 && n == 0) return (size_t)-1;
  return count ? n : 0;
}

static void complete(void) {
  const char *add = 0;
  size_t n = 0;
  int start = in_n, dotted = 0;
  nlr_buf_t nlr;
  volatile int top = 0;
  mp_print_t to_comp = { 0, comp_strn };
  mp_stack_set_top((void *)&top);
  mp_stack_set_limit(STACK_LIMIT);
  comp_n = 0;
  while (start > 0 && (is_name_char(in[start - 1]) || in[start - 1] == '.')) {
    start--;
    if (in[start] == '.') dotted = 1;
  }
  if (nlr_push(&nlr) == 0) {
    if (dotted)
      n = mp_repl_autocomplete(in, (size_t)in_n, &to_comp, &add);
    else
      n = complete_word(in + start, (size_t)(in_n - start), &add);
    nlr_pop();
  } else {
    n = 0;
  }
  if (n == (size_t)-1) {                  /* several: show them under the line */
    char echo[IN_MAX + 8];
    api->fmt(echo, sizeof echo, "%s %s", src_n ? "..." : ">>>", in);
    say(echo, T_IN);
    show_candidates();
  } else if (n && add) {
    if (in_n + (int)n > IN_MAX) n = (size_t)(IN_MAX - in_n);
    memcpy(in + in_n, add, n);
    in_n += (int)n;
    in[in_n] = 0;
  }
  (void)top;
}

static void submit(void) {
  char echo[IN_MAX + 8];
  api->fmt(echo, sizeof echo, "%s %s", src_n ? "..." : ">>>", in);
  say(echo, T_IN);
  hist_add();
  hist_at = -1;
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
    hist_walk(k == CAPP_KEY_UP);
    L.scroll = 0;
    return 1;
  }
  if (k == CTRL_U || k == CTRL_D) {       /* the log, half a screen at a time */
    L.scroll += k == CTRL_U ? 6 : -6;
    if (L.scroll < 0) L.scroll = 0;
    if (L.scroll > L.n) L.scroll = L.n;
    return 1;
  }
  if (k == '\t') {
    /* After a name: complete it. At the start or after a space: an
     * indent, to the next four. */
    if (ready && in_n && in[in_n - 1] != ' ') {
      complete();
      return 1;
    }
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
  "tab\tcomplete a name; at the start, indent\n"
  "up/down\tthe lines typed before\nctrl-u ctrl-d\tscroll the log\n"
  "esc\tdrop the block, clear the line\nany key\tstop a running program\n",
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
