/* Iron -- press the wrinkles out of a tea towel, a shirt and a bedsheet before
 * the clock runs out, without scorching them.
 *
 * The game itself -- the garment, the iron, the press, the scoring and the
 * levels -- is apps/iron_game.h: portable, no API, host-testable. This file is
 * the app around it: the screens, the keys, the clock, the drawing and the
 * feel.
 *
 * SCREENS. A run goes title -> level card -> garment -> result card -> the
 * next level card, until a garment is not flat enough in time (or burns
 * through) and the run is over. The garment is the only screen the clock runs
 * on; the iron warms up on the level card, so the heat can be chosen there.
 *
 * KEYS. The arrow keys move the iron, Space holds it down, and H cycles the
 * heat dial, with - and = for cooler and hotter; M turns the sounds off and
 * on. The heat is not on , and . because the shell turns ; . , / into arrows
 * before an app sees them (unless the app is taking text), and those are the
 * arrows this game is played with. The keyboard has no key-up and only the
 * newest key repeats, so "hold" is a window that key events top up (see
 * iron_press_key and iron_move_key in the game): Space then an arrow irons a
 * stroke, and letting go of the arrow lifts the iron a moment later.
 *
 * Esc is back one level, as everywhere: from a garment or the pause screen it
 * asks whether to leave the run (the clock stops while it asks); from the
 * level card of the first garment it goes back to the title, and on the title
 * it is not taken, so the app keeps it. fn-` is the OS's way out.
 *
 * THE GARMENT is drawn by this file, a pixel at a time, into a strip buffer
 * that is sent to the panel whole (api->pixels) -- so the cloth, its shadow,
 * the iron on top of it and everything that floats above it are each pixel
 * written once and nothing is ever seen half drawn. The cloth is the game's
 * wrinkle field smoothed between cell centres, then lit from the top left: a
 * crease is a ridge with a bright side and a dark side and a little shading
 * of its own, and as it is pressed the ridge sinks and the shading fades with
 * it, to flat cloth. Scorch is smoothed the same way and goes from a singe to
 * a char. The iron is a procedural sprite (a pointed sole in the colour of
 * its heat with the cloth showing through, a teal housing on top of it, a
 * dial that glows with the heat) that sits lower, with a tighter shadow,
 * while it is pressing.
 *
 * THE FEEL is a handful of small things the game's events set off, each
 * drawn into the same strips:
 *   - steam: puffs leave the nose while it hisses and drift up and fade where
 *     they are, so the iron leaves a trail of them; scorching sends up dark
 *     smoke instead;
 *   - a crease that goes flat glints: a teal (gold, once the combo is high)
 *     sheen sweeps across its cell, and every so often a star pops on it, so
 *     a stroke leaves a sweep of light behind it;
 *   - a tier of the combo going up, the pass mark being reached and a scorch
 *     float a word up from the iron, and the combo in the HUD flashes;
 *   - sounds, which the API has no tone generator for: short WAV files are
 *     synthesised into /cache once (as Timer's beep is) and played with
 *     api->audio(), only when the speaker is idle or playing one of our own
 *     less important ones, so music is never cut off;
 *   - the garment done: it is pressed, a sheen runs across it, it folds in
 *     half and in half again, hops onto the pile of the ones before and is
 *     rated, with the numbers counting up. Enter skips to the end.
 *
 * REDRAWING. Only what changed is repainted. Each sample (FRAME_MS) the tick
 * moves the effects and compares the iron's look and each HUD element's value
 * with what was last drawn, and marks api->damage for the rectangles that
 * differ: the iron where it was and where it is, the cells under it while it
 * presses, every effect that moved, and the HUD rows that changed. The OS
 * unions those into one rectangle, so the paint works out what really changed
 * itself: when the clip is exactly what was marked it is our own damage
 * coming back, and only the rectangles on our list and the HUD elements that
 * differ are drawn; any other clip -- the whole screen, after a card or the
 * help panel -- draws everything inside it. Paint reads the game and does not
 * change it. The garment screen asks to be painted direct (api->paint_direct),
 * since it composes its own strips; the cards are composed off the panel by
 * the OS as usual.
 *
 * THE BEST SCORE AND THE HIGHEST LEVEL are kept in /config/iron.txt, two
 * lines (best=NNN, level=N), written through apps/safefile.h -- to a .tmp and
 * renamed over, so a device pulled from a pocket mid-save keeps the old file --
 * whenever a garment or a run ends with either one beaten, and read once when
 * the app opens, before the title shows them. A missing file is a first run,
 * and a file that is not exactly those two lines (a damaged card, a bad edit)
 * is thrown away: the scores start again from nothing, the title says so, and
 * the next save writes a clean file. A card that cannot be written says so too.
 *
 * THE FOOTER (apps/footer.h) is the one bar every app has, along the bottom
 * 11 rows, saying what the keys do on the screen it is on; the garment's also
 * says how to change the heat when the iron is too cool or too hot for the
 * cloth. What does not fit there is in the help (fn-h): capp_info.help, a key
 * and its meaning a line, at most eight lines, with the keys short enough for
 * the panel's key column.
 */

#include "kernel/app/capp.h"
#include "kernel/console/font6x8.h"
#include "apps/str.h"
#include "apps/confirm.h"
#include "apps/safefile.h"
#include "apps/footer.h"
#include "apps/iron_game.h"

static const CardApi *api;
static const CappAudio *au;

#define SCORE_PATH CAPP_CONFIG "/iron.txt"

/* What reading the scores file found. */
enum { FILE_NONE, FILE_OK, FILE_BAD };

static const char *foot_text(void);

/* ---- layout and colours ---------------------------------------------------- */

/* The screen is 240x135: a canvas on the left (the board, 160x120, inset by
 * PAD from the top and left of its table), the HUD column on the right, and
 * a band along the bottom the footer will use. Everything is a disjoint
 * rectangle, so a full paint writes each pixel once. */
#define CELL       5                      /* pixels to a board cell */
#define BOARD_W    (IR_W * CELL)          /* 160 */
#define BOARD_H    (IR_H * CELL)          /* 120 */
#define PAD        4
#define CANVAS_W   (BOARD_W + 2 * PAD)    /* 168 */
#define CANVAS_H   (BOARD_H + PAD)        /* 124 */
#define HUD_W      66
#define STRIP_H    15                     /* rows composed at a time */
#define FRAME_MS   33u                    /* at most 30 samples a second */
#define NOTE_MS    2000u
#define NOFIELD    65535                  /* "not cloth" in the wrinkle field */

#define C_BG      CAPP_RGB(24, 28, 36)
#define C_TEXT    CAPP_RGB(232, 235, 242)
#define C_DIM     CAPP_RGB(128, 136, 152)
#define C_ACCENT  CAPP_RGB(88, 208, 196)
#define C_GOOD    CAPP_RGB(120, 214, 120)
#define C_WARN    CAPP_RGB(244, 132, 72)
#define C_COOL    CAPP_RGB(110, 160, 255)
#define C_GOLD    CAPP_RGB(250, 206, 84)
#define C_BAR     CAPP_RGB(14, 16, 22)
#define C_WINDOW  CAPP_RGB(40, 92, 64)
#define C_STEAM   CAPP_RGB(190, 220, 240)
#define C_TABLE_D CAPP_RGB(58, 68, 92)
#define C_TABLE_L CAPP_RGB(86, 98, 126)

/* The feel. */
#define NPUFF      20                     /* steam and smoke, at most */
#define NSPK       12                     /* stars */
#define NPOP       4                      /* words floating up */
#define BD_MAX     8                      /* rectangles to compose, at most */
#define GLINT_LIFE 9                      /* samples a flattened cell glints for */
#define SPK_LIFE   8
#define POP_LIFE   26

static const char *const ZONE_TEXT[3] = { "too cool", "just right", "too hot" };
static const char *const ZONE_WORD[3] = { "cool", "good", "HOT" };

/* The cloth of each garment, before wrinkles. */
static const uint8_t CLOTH_RGB[IR_KINDS][3] = {
  { 244, 238, 222 },                      /* tea towel: cream */
  { 148, 192, 238 },                      /* shirt: light blue */
  { 232, 226, 244 },                      /* bedsheet: lavender white */
};

/* What each cell is, for the drawing: cloth, and which sides are bare table. */
#define M_CLOTH   1
#define M_OPEN_W  2
#define M_OPEN_E  4
#define M_OPEN_N  8
#define M_OPEN_S  16

/* Half the height of the iron's sole at each column from -12 to +12: a
 * rounded back and a pointed nose. (The game presses a 5x3 rectangle; this
 * is how it looks.) */
static const int8_t SOLE_H[25] = {
  4, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 5, 5, 4, 3, 2, 1,
};

/* ---- sounds ------------------------------------------------------------------ */

/* A sound is notes in a row, each a tone (or noise, or a rest) that dies
 * away over its length. They are synthesised into 8 kHz mono WAV files in
 * /cache on the first run and played from there, the way Timer's beep is: the
 * API has api->audio()->play and nothing that makes a tone. */
#define SND_RATE 8000u

enum { W_TRI, W_SQR, W_NOISE };
typedef struct { uint16_t hz, ms; uint8_t vol, wave; } Note;

enum { SND_THUMP, SND_HISS, SND_TING, SND_TIER, SND_STROKE, SND_BURN, SND_GOAL,
       SND_LOW, SND_FOLD, SND_CRISP, SND_NICE, SND_OVER, SND_COUNT };

static const Note N_THUMP[]  = { { 110, 70, 90, W_TRI } };                       /* the iron comes down */
static const Note N_HISS[]   = { { 0, 130, 30, W_NOISE } };                      /* steam */
static const Note N_TING[]   = { { 2093, 36, 55, W_TRI } };                      /* a crease goes flat */
static const Note N_TIER[]   = { { 784, 50, 60, W_TRI }, { 1175, 100, 60, W_TRI } };
static const Note N_STROKE[] = { { 1568, 26, 40, W_TRI } };                      /* a clean stroke */
static const Note N_BURN[]   = { { 150, 220, 50, W_SQR } };                      /* a scorch */
static const Note N_GOAL[]   = { { 523, 70, 60, W_TRI }, { 659, 70, 60, W_TRI }, { 784, 120, 60, W_TRI } };
static const Note N_LOW[]    = { { 988, 90, 55, W_SQR } };                       /* the clock */
static const Note N_FOLD[]   = { { 392, 50, 45, W_TRI } };
static const Note N_CRISP[]  = { { 659, 80, 60, W_TRI }, { 784, 80, 60, W_TRI },
                                 { 988, 80, 60, W_TRI }, { 1319, 180, 65, W_TRI } };
static const Note N_NICE[]   = { { 523, 100, 55, W_TRI }, { 784, 160, 55, W_TRI } };
static const Note N_OVER[]   = { { 392, 150, 55, W_TRI }, { 330, 150, 55, W_TRI }, { 262, 300, 55, W_TRI } };

/* prio: a more important sound cuts off one of ours that is still playing. */
typedef struct { const char *name; const Note *n; uint8_t count, prio; } Snd;
#define NN(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))
static const Snd SND[SND_COUNT] = {
  { "thump",  N_THUMP,  NN(N_THUMP),  1 },
  { "hiss",   N_HISS,   NN(N_HISS),   1 },
  { "ting",   N_TING,   NN(N_TING),   1 },
  { "tier",   N_TIER,   NN(N_TIER),   3 },
  { "stroke", N_STROKE, NN(N_STROKE), 1 },
  { "burn",   N_BURN,   NN(N_BURN),   5 },
  { "goal",   N_GOAL,   NN(N_GOAL),   3 },
  { "low",    N_LOW,    NN(N_LOW),    2 },
  { "fold",   N_FOLD,   NN(N_FOLD),   2 },
  { "crisp",  N_CRISP,  NN(N_CRISP),  4 },
  { "nice",   N_NICE,   NN(N_NICE),   4 },
  { "over",   N_OVER,   NN(N_OVER),   5 },
};

/* ---- state ------------------------------------------------------------------- */

enum { SC_TITLE, SC_INTRO, SC_PLAY, SC_PAUSE, SC_LEAVE, SC_DONE, SC_OVER };

/* The HUD, as elements: each is a rectangle drawn whole, so a change repaints
 * that and nothing else. */
enum { E_LABEL, E_TIME, E_FLAT, E_FBAR, E_SCORE, E_COMBO, E_METER, E_HEAT,
       E_HBAR, E_STEAM, E_FOOT, E_COUNT };

/* How the iron looks, as drawn: when this differs from the last one drawn the
 * iron is redrawn. cx, cy are canvas pixels. */
typedef struct {
  int cx, cy, hv, left, down, steam, danger, dq, heat;
} Spr;

/* A puff of steam (kind 0) or smoke (kind 1) in canvas pixels x16; life 0 is
 * a free slot. */
typedef struct { int x, y, vx, vy, age, life, kind, r0; } Puff;
typedef struct { int x, y, age, life; } Spk;
typedef struct { int x, y, age, life; uint8_t cr, cg, cb; char text[8]; } Pop;

/* The colours of the words, as r, g, b: they are mixed into the picture a
 * pixel at a time, not drawn as a colour of the panel's. */
#define PC_GOLD  250, 206, 84
#define PC_WARN  244, 132, 72
#define PC_GOOD  120, 214, 120

/* A fold: where a coordinate goes on screen. x = fx + (xl - fxo) * kx / 1024,
 * and the same for y; kx = -1024 is the half flipped over. */
typedef struct { int fx, fxo, kx, fy, fyo, ky; } Map;

static Iron game;

/* Handles of the fonts in /fonts, or -1 for the 6x8 console font. */
static struct { int ui, uib, num, big; } F;

static struct {
  int      screen;
  int      leave_back;             /* the screen "no" goes back to */
  uint32_t last_ms;                /* the clock the game was last advanced to */
  uint32_t frame_ms;               /* when the display was last sampled */
  uint32_t best;
  int      best_level;
  int      new_best;               /* this run beat the best */
  int      file;                   /* FILE_*: what the scores file was when it was read */
  int      save_failed;            /* the last save did not take */
  char     note[24];               /* a line in the HUD for a moment */
  uint32_t note_until;
  int      note_serial;
  /* damage */
  CRect    area;                   /* the content rectangle, from the last paint */
  CRect    mark;                   /* the union of what was marked since the last paint */
  CRect    bd[BD_MAX];             /* the rectangles of the canvas to compose */
  CRect    cells_prev;             /* where the cells under the iron were last sample */
  int      nbd;
  int      have_area, has_mark, need_full, marked;
  /* the garment screen */
  int      hv;                     /* how high the iron is held: 0 down .. 3 up */
  int      left;                   /* it faces left */
  int      was_pressing;
  int      last_sumw;
  int      spr_valid;
  Spr      spr;                    /* the iron as it was last drawn */
  int      hud[E_COUNT];           /* each HUD element's value as last drawn */
  int      i_heat, i_dial;         /* the level card, as last drawn */
  /* the feel */
  uint32_t rng;
  int      sample_no;
  int      nglint;                 /* cells that are glinting */
  int      flash, flash_kind;      /* the combo flashing: samples left, 0 gold 1 red */
  int      was_steam, burn_seen, goal, marks_seen, last_secs;
  uint32_t ting_ms;
  /* sound */
  int      sound;
  uint32_t snd_have;               /* bit per sound whose file is on the card */
  uint32_t snd_end;                /* when the one we started will have finished */
  int      snd_prio;
  /* the garment done */
  int      anim;                   /* ms since the result came up */
  int      fired;                  /* which of its sounds have gone */
} S;

static struct {
  CRect c, canvas, gap, right, bottom, pad_top, pad_bot;
  int hx, hw;
} L;

static uint16_t OUT[STRIP_H * CANVAS_W];                   /* a strip, ready to send */
static uint16_t WF[(STRIP_H + 2) * (CANVAS_W + 2)];        /* wrinkle, smoothed, with a border */
static uint8_t  SF[(STRIP_H + 2) * (CANVAS_W + 2)];        /* scorch, the same */
static uint8_t  MASK[IR_CELLS];                            /* M_* for each cell */
static uint8_t  GL[IR_CELLS];                              /* samples of glint left, per cell */
static Puff     PF[NPUFF];
static Spk      SK[NSPK];
static Pop      PO[NPOP];

/* The garment as rows of bits (bit u of FM0[v] is the cloth at column u of
 * row v), as it is folded once and twice. */
static uint32_t FM0[22], FM1[22], FM2[11];
static int      FW, FH;

/* ---- small things ------------------------------------------------------------- */

static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int cc(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static uint16_t rgb(int r, int g, int b) {
  r = cc(r); g = cc(g); b = cc(b);
  return CAPP_RGB(r, g, b);
}

static int crect_empty(CRect r) { return r.w <= 0 || r.h <= 0; }

static int crect_eq(CRect a, CRect b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

static int crect_touch(CRect a, CRect b) {
  return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}

static CRect crect_isect(CRect a, CRect b) {
  int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
  int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
  int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
  return capp_rect(x0, y0, x1 - x0, y1 - y0);
}

static CRect crect_union(CRect a, CRect b) {
  int x0, y0, x1, y1;
  if (crect_empty(a)) return b;
  if (crect_empty(b)) return a;
  x0 = a.x < b.x ? a.x : b.x;
  y0 = a.y < b.y ? a.y : b.y;
  x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
  y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
  return capp_rect(x0, y0, x1 - x0, y1 - y0);
}

/* Tell the OS this changed, and remember what was told: the paint compares
 * its clip with the union to know the damage is ours. */
static void mark(CRect r) {
  if (!S.have_area) return;
  r = crect_isect(r, S.area);
  if (crect_empty(r)) return;
  api->damage(r);
  S.mark = S.has_mark ? crect_union(S.mark, r) : r;
  S.has_mark = 1;
  S.marked++;
}

/* Part of the canvas that has to be composed again. A rectangle that touches
 * one already on the list is joined to it, so the list stays short and a
 * stroke's trail of small changes becomes a few rectangles, not a hundred. */
static void bd_add(CRect r) {
  int i, j;
  r = crect_isect(r, L.canvas);
  if (crect_empty(r)) return;
  for (i = 0; i < S.nbd; ) {
    if (crect_touch(r, S.bd[i])) {
      r = crect_union(r, S.bd[i]);
      S.nbd--;
      S.bd[i] = S.bd[S.nbd];
      i = 0;
    } else {
      i++;
    }
  }
  if (S.nbd >= BD_MAX) {
    for (j = 0; j < S.nbd; j++) r = crect_union(r, S.bd[j]);
    S.nbd = 0;
  }
  S.bd[S.nbd++] = r;
}

/* 0 .. n-1, for the effects: they need to look random and nothing more. */
static int frnd(int n) {
  uint32_t x = S.rng ? S.rng : 0x2545F491u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  S.rng = x;
  return n > 1 ? (int)(x % (uint32_t)n) : 0;
}

/* A repeatable scatter of 0 .. mod-1 from two small numbers. */
static int scatter(int i, int k, int mod) {
  uint32_t h = (uint32_t)(i * 7919 + k * 104729 + 13) * 2654435761u;
  h ^= h >> 15;
  return (int)(h % (uint32_t)mod);
}

/* Which cells are cloth and which edges of them are bare, for the garment
 * the game has laid out. */
static void build_mask(void) {
  int x, y;
  for (y = 0; y < IR_H; y++) {
    for (x = 0; x < IR_W; x++) {
      int m = 0;
      if (iron_cloth(&game, x, y)) {
        m = M_CLOTH;
        if (!iron_cloth(&game, x - 1, y)) m |= M_OPEN_W;
        if (!iron_cloth(&game, x + 1, y)) m |= M_OPEN_E;
        if (!iron_cloth(&game, x, y - 1)) m |= M_OPEN_N;
        if (!iron_cloth(&game, x, y + 1)) m |= M_OPEN_S;
      }
      MASK[y * IR_W + x] = (uint8_t)m;
    }
  }
}

/* The garment as rows of bits, and the same folded in half down the middle
 * and then folded in half across it: the left half turns over onto the right,
 * the top half down onto the bottom. */
static void build_fold(void) {
  int v, u, mid;
  FW = game.lv.w;
  FH = game.lv.h;
  mid = FW / 2;
  for (v = 0; v < FH; v++) {
    uint32_t m = 0, left, mir = 0;
    for (u = 0; u < FW; u++)
      if (iron_cloth(&game, game.lv.bx + u, game.lv.by + v)) m |= 1u << u;
    FM0[v] = m;
    left = m & ((1u << mid) - 1u);
    for (u = 0; u < mid; u++)
      if (left & (1u << u)) mir |= 1u << (mid - 1 - u);
    FM1[v] = (m >> mid) | mir;
  }
  for (v = 0; v < FH / 2; v++) FM2[v] = FM1[FH / 2 + v] | FM1[FH / 2 - 1 - v];
}

/* The middle of the iron's sole on the canvas, in pixels. */
static void iron_xy(int *cx, int *cy) {
  *cx = PAD + ((game.x * CELL) >> 8);
  *cy = PAD + ((game.y * CELL) >> 8);
}

/* ---- sounds, made and played ----------------------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static int note_samples(const Note *n) { return (int)(n->ms * SND_RATE / 1000u); }

static int snd_samples(int id) {
  int i, n = 0;
  for (i = 0; i < SND[id].count; i++) n += note_samples(&SND[id].n[i]);
  return n;
}

static int snd_ms(int id) {
  int i, n = 0;
  for (i = 0; i < SND[id].count; i++) n += SND[id].n[i].ms;
  return n;
}

static void snd_path(char *out, size_t n, int id) {
  api->fmt(out, n, CAPP_CACHE "/iron_%s.wav", SND[id].name);
}

/* Write sound `id` as a WAV: each note a triangle or square wave (a phase
 * that wraps, no trig) or noise, dying away over its length, with a few
 * samples of fade at each end so it does not click. */
static void make_snd(int id) {
  const Snd *s = &SND[id];
  uint8_t hdr[44];
  int16_t buf[128];
  char path[40];
  int fd, i, nb = 0;
  uint32_t seed = 0x1234567u, data_bytes = (uint32_t)snd_samples(id) * 2u;
  snd_path(path, sizeof path, id);
  fd = api->open(path, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  if (fd < 0) return;
  api->mem_set(hdr, 0, sizeof hdr);
  api->mem_cpy(hdr, "RIFF", 4);
  put_u32(hdr + 4, 36u + data_bytes);
  api->mem_cpy(hdr + 8, "WAVE", 4);
  api->mem_cpy(hdr + 12, "fmt ", 4);
  put_u32(hdr + 16, 16);
  put_u16(hdr + 20, 1);                 /* PCM */
  put_u16(hdr + 22, 1);                 /* mono */
  put_u32(hdr + 24, SND_RATE);
  put_u32(hdr + 28, SND_RATE * 2u);     /* byte rate */
  put_u16(hdr + 32, 2);                 /* block align */
  put_u16(hdr + 34, 16);                /* bits per sample */
  api->mem_cpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, data_bytes);
  api->write(fd, hdr, sizeof hdr);
  for (i = 0; i < s->count; i++) {
    const Note *nt = &s->n[i];
    int n = note_samples(nt), j;
    uint32_t phase = 0, step = (uint32_t)nt->hz * 65536u / SND_RATE;
    for (j = 0; j < n; j++) {
      int amp = nt->vol * 180, v = 0;
      amp = amp * (n - j) / n;
      if (j < 24) amp = amp * j / 24;
      if (n - j < 24) amp = amp * (n - j) / 24;
      if (nt->wave == W_NOISE) {
        seed = seed * 1664525u + 1013904223u;
        v = ((int)((seed >> 16) & 0x7FFFu) - 16384) * amp / 16384;
      } else if (nt->hz) {
        uint32_t p = phase & 0xFFFFu;
        if (nt->wave == W_SQR) v = p < 32768u ? amp : -amp;
        else v = ((p < 32768u ? (int)p - 16384 : 49152 - (int)p) * amp) / 16384;
        phase += step;
      }
      buf[nb++] = (int16_t)v;
      if (nb == 128) { api->write(fd, buf, sizeof buf); nb = 0; }
    }
  }
  if (nb) api->write(fd, buf, (size_t)nb * 2);
  api->close(fd);
}

/* Every sound there and the right size: a write cut short is made again. No
 * card, no sounds; the game does not care. */
static void ensure_sounds(void) {
  int i;
  char path[40];
  CappStat st;
  api->mkdir(CAPP_CACHE);
  for (i = 0; i < SND_COUNT; i++) {
    uint32_t want = 44u + (uint32_t)snd_samples(i) * 2u;
    snd_path(path, sizeof path, i);
    if (api->stat(path, &st) != 0 || st.is_dir || st.size != want) make_snd(i);
    if (api->stat(path, &st) == 0 && !st.is_dir && st.size == want) S.snd_have |= 1u << i;
  }
}

/* Play sound `id` if sound is on and the speaker is free -- or is playing one
 * of ours that matters less. Music, a recording, anyone else's sound is left
 * alone. */
static void snd(int id) {
  const Snd *s = &SND[id];
  char path[40];
  uint32_t now;
  if (!S.sound || !au || !(S.snd_have & (1u << id))) return;
  if (au->volume() <= 0) return;
  now = api->ticks_ms();
  if (au->state() != CAPP_AUDIO_IDLE) {
    if ((int32_t)(now - S.snd_end) >= 0 || (int)s->prio <= S.snd_prio) return;
    au->stop();
  }
  snd_path(path, sizeof path, id);
  if (au->play(path) == 0) {
    S.snd_end = now + (uint32_t)snd_ms(id) + 80u;
    S.snd_prio = s->prio;
  }
}

/* ---- the feel: steam, smoke, glints, stars, words ------------------------------------ */

static void puff_add(int x, int y, int vx, int vy, int life, int kind, int r0) {
  int i;
  for (i = 0; i < NPUFF; i++) {
    if (!PF[i].life) {
      Puff *p = &PF[i];
      p->x = x; p->y = y; p->vx = vx; p->vy = vy;
      p->age = 0; p->life = life; p->kind = kind; p->r0 = r0;
      return;
    }
  }
}

static void spk_add(int x, int y) {
  int i;
  for (i = 0; i < NSPK; i++) {
    if (!SK[i].life) {
      SK[i].x = x; SK[i].y = y; SK[i].age = 0; SK[i].life = SPK_LIFE;
      return;
    }
  }
}

/* Where each is, on screen, with room for its largest. */
static CRect puff_rect(const Puff *p) {
  int rad = p->r0 + 5;
  return capp_rect(L.canvas.x + (p->x >> 4) - rad, L.canvas.y + (p->y >> 4) - rad, 2 * rad + 1, 2 * rad + 1);
}

static CRect spk_rect(const Spk *k) {
  return capp_rect(L.canvas.x + k->x - 5, L.canvas.y + k->y - 5, 11, 11);
}

static CRect pop_rect(const Pop *p) {
  return capp_rect(L.canvas.x + p->x - 1, L.canvas.y + p->y - 1, (int)api->str_len(p->text) * 6 + 2, 10);
}

/* A word that floats up from (cx, cy) and goes. */
static void pop_add(const char *text, int cr, int cg, int cb, int cx, int cy) {
  int i, slot = 0, len = (int)api->str_len(text);
  Pop *p;
  for (i = 0; i < NPOP; i++) {
    if (!PO[i].life) { slot = i; break; }
  }
  p = &PO[slot];
  if (p->life) bd_add(pop_rect(p));
  api->fmt(p->text, sizeof p->text, "%s", text);
  p->x = iclamp(cx - len * 3, 2, CANVAS_W - len * 6 - 2);
  p->y = iclamp(cy - 22, 2, CANVAS_H - 12);
  p->age = 0;
  p->life = POP_LIFE;
  p->cr = (uint8_t)cr;
  p->cg = (uint8_t)cg;
  p->cb = (uint8_t)cb;
}

static void glint_add(int idx) {
  if (idx < 0 || idx >= IR_CELLS) return;
  if (!GL[idx]) S.nglint++;
  GL[idx] = GLINT_LIFE;
}

static void fx_reset(void) {
  api->mem_set(PF, 0, sizeof PF);
  api->mem_set(SK, 0, sizeof SK);
  api->mem_set(PO, 0, sizeof PO);
  api->mem_set(GL, 0, sizeof GL);
  S.nglint = 0;
  S.flash = 0;
  S.burn_seen = 0;
  S.was_steam = 0;
}

/* What the game just did, as the feel: called after every advance, since the
 * game's events say what happened in that call and no more. */
static void fx_events(void) {
  uint32_t ev = game.ev;
  int i, cx, cy, secs;
  iron_xy(&cx, &cy);
  for (i = 0; i < game.nspark; i++) {
    int idx = game.spark[i];
    glint_add(idx);
    if (i < 3 && ((i & 1) == 0 || iron_tier(&game) >= 3))
      spk_add(PAD + (idx % IR_W) * CELL + 1 + frnd(3), PAD + (idx / IR_W) * CELL + 1 + frnd(3));
  }
  if (game.nspark > 0 && (int32_t)(api->ticks_ms() - S.ting_ms) >= 90) {
    S.ting_ms = api->ticks_ms();
    snd(SND_TING);
  }
  if (ev & IR_EV_PRESS) snd(SND_THUMP);
  if (game.steaming && !S.was_steam) snd(SND_HISS);
  S.was_steam = game.steaming;
  if (ev & IR_EV_TIER_UP) {
    char t[8];
    int m = iron_mult_pct(&game);
    api->fmt(t, sizeof t, "x%d.%d", m / 100, m % 100 / 10);
    pop_add(t, PC_GOLD, cx, cy);
    S.flash = 12;
    S.flash_kind = 0;
    snd(SND_TIER);
  }
  if (ev & IR_EV_STROKE) snd(SND_STROKE);
  if (ev & IR_EV_SCORCH) {
    char t[8];
    int n = game.marks - S.marks_seen;
    api->fmt(t, sizeof t, "-%d", (n > 0 ? n : 1) * IR_PEN_MARK);
    pop_add(t, PC_WARN, cx, cy);
    S.flash = 14;
    S.flash_kind = 1;
    for (i = 0; i < 4; i++)
      puff_add((cx - 6 + frnd(13)) * 16, (cy - 4) * 16, frnd(9) - 4, -(10 + frnd(6)), 24 + frnd(8), 1, 2);
    snd(SND_BURN);
  }
  S.marks_seen = game.marks;
  if ((ev & IR_EV_COMBO_LOST) && S.flash == 0) { S.flash = 10; S.flash_kind = 1; }
  if (!S.goal && iron_pass_ok(&game)) {
    S.goal = 1;
    pop_add("goal!", PC_GOOD, cx, cy);
    snd(SND_GOAL);
  }
  if (ev & IR_EV_LOW_TIME) snd(SND_LOW);
  secs = iron_secs_left(&game);
  if (secs != S.last_secs) {
    S.last_secs = secs;
    if (secs <= 5 && secs > 0) snd(SND_LOW);
  }
  if (game.burn > 0) S.burn_seen = 1;
}

/* One sample of time for the effects: new steam and smoke, everything that
 * is alive moved on, and each rectangle it covered, before and after, put on
 * the list to compose again. */
static void fx_step(void) {
  int i, cx, cy;
  iron_xy(&cx, &cy);
  S.sample_no++;
  if (game.steaming && (S.sample_no & 1) == 0)
    puff_add((S.left ? cx - 8 : cx + 8) * 16, (cy - 1) * 16, frnd(9) - 4, -(16 + frnd(7)), 20 + frnd(6), 0, 2);
  if (S.burn_seen) {
    if ((S.sample_no & 1) == 0)
      puff_add((cx - 6 + frnd(13)) * 16, (cy - 4) * 16, frnd(7) - 3, -(10 + frnd(6)), 26 + frnd(8), 1, 2);
    S.burn_seen = 0;
  }
  for (i = 0; i < NPUFF; i++) {
    Puff *p = &PF[i];
    if (!p->life) continue;
    bd_add(puff_rect(p));
    p->age++;
    if (p->age >= p->life) { p->life = 0; continue; }
    p->x += p->vx;
    p->y += p->vy;
    p->vy = p->vy * 15 / 16;
    p->vx = iclamp(p->vx + frnd(3) - 1, -6, 6);
    bd_add(puff_rect(p));
  }
  for (i = 0; i < NSPK; i++) {
    Spk *k = &SK[i];
    if (!k->life) continue;
    bd_add(spk_rect(k));
    k->age++;
    if (k->age >= k->life) k->life = 0;
  }
  if (S.nglint > 0) {
    for (i = 0; i < IR_CELLS; i++) {
      if (!GL[i]) continue;
      bd_add(capp_rect(L.canvas.x + PAD + (i % IR_W) * CELL, L.canvas.y + PAD + (i / IR_W) * CELL, CELL, CELL));
      GL[i]--;
      if (!GL[i]) S.nglint--;
    }
  }
  for (i = 0; i < NPOP; i++) {
    Pop *p = &PO[i];
    if (!p->life) continue;
    bd_add(pop_rect(p));
    p->age++;
    p->y--;
    if (p->age >= p->life) { p->life = 0; continue; }
    bd_add(pop_rect(p));
  }
  if (S.flash > 0) S.flash--;
}

/* ---- screens, and moving between them ----------------------------------------------- */

/* Change screen. Everything on the new one is repainted, and the old one's
 * damage is dropped -- left standing, the OS would repaint only that. */
static void go(int screen, uint32_t now) {
  S.screen = screen;
  S.last_ms = now;
  S.need_full = 1;
  S.has_mark = 0;
  S.nbd = 0;
  if (screen == SC_INTRO) build_mask();
  if (screen == SC_PLAY) {
    S.hv = 3;
    S.was_pressing = 0;
    S.last_sumw = (int)game.sumw;
    S.spr_valid = 0;
    S.marks_seen = game.marks;
    S.goal = iron_pass_ok(&game);
    S.last_secs = iron_secs_left(&game);
    fx_reset();
  }
  if (screen == SC_DONE) {
    S.anim = 0;
    S.fired = 0;
    build_fold();
  }
  api->paint_direct(screen == SC_PLAY);
  mark(S.area);
}

static void note_say(const char *text) {
  api->fmt(S.note, sizeof S.note, "%s", text);
  S.note_until = api->ticks_ms() + NOTE_MS;
  S.note_serial++;
}

static void note_need(void) {
  char t[24];
  api->fmt(t, sizeof t, "need %d%%", iron_pass_pct(&game));
  note_say(t);
}

/* ---- the scores, on the card ---------------------------------------------------- */

/* One line of the file: "best=123" or "level=4", the digits and nothing else
 * (a trailing space or carriage return is let go). 1 if it was one of those,
 * well formed and not seen before; `seen` has a bit for each that has been. */
static int parse_line(const char *s, uint32_t *best, uint32_t *level, int *seen) {
  const char *p;
  uint32_t v = 0;
  int digits = 0, which;
  if (str_starts(s, "best=")) { which = 1; p = s + 5; }
  else if (str_starts(s, "level=")) { which = 2; p = s + 6; }
  else return 0;
  if (*seen & which) return 0;
  while (*p >= '0' && *p <= '9') {
    if (++digits > 9) return 0;
    v = v * 10u + (uint32_t)(*p - '0');
    p++;
  }
  if (!digits) return 0;
  while (*p == ' ' || *p == '\r') p++;
  if (*p) return 0;
  if (which == 1) {
    *best = v;
  } else {
    if (v > 9999u) return 0;
    *level = v;
  }
  *seen |= which;
  return 1;
}

/* Read the scores into S.best and S.best_level. Missing, empty, too long,
 * holding anything but the two lines, or with a number out of range: the
 * values stay 0 (the file is gone as far as the game is concerned) and
 * S.file says which it was. */
static void load_best(void) {
  char buf[96], line[32];
  uint32_t best = 0, level = 0;
  int fd, n = 0, got = 0, i, len = 0, seen = 0, bad = 0;
  S.best = 0;
  S.best_level = 0;
  S.file = FILE_NONE;
  fd = safe_open_read(api, SCORE_PATH);
  if (fd < 0) return;
  while (n < (int)sizeof buf - 1 && (got = api->read(fd, buf + n, sizeof buf - 1 - (size_t)n)) > 0) n += got;
  api->close(fd);
  buf[n] = 0;
  S.file = FILE_BAD;
  if (n == 0 || n >= (int)sizeof buf - 1) return;      /* empty, or too much to be ours */
  for (i = 0; i <= n; i++) {
    char ch = i < n ? buf[i] : '\n';
    if (ch != '\n') {
      if ((unsigned char)ch < 0x20 && ch != '\r') bad = 1;
      if (len < (int)sizeof line - 1) line[len++] = ch;
      else bad = 1;
      continue;
    }
    if (len > 0 && line[len - 1] == '\r') len--;           /* a file written on a PC */
    line[len] = 0;
    len = 0;
    if (line[0] && !parse_line(line, &best, &level, &seen)) bad = 1;
  }
  if (bad || seen != 3) return;
  S.best = best;
  S.best_level = (int)level;
  S.file = FILE_OK;
}

/* Write them. A card that is missing, full or read-only leaves the old file
 * as it was and sets save_failed, which the title and the run-over card say. */
static void save_best(void) {
  SafeFile f;
  char line[32];
  api->mkdir(CAPP_CONFIG);
  if (safe_begin(&f, api, SCORE_PATH) != 0) { S.save_failed = 1; return; }
  api->fmt(line, sizeof line, "best=%u\n", (unsigned)S.best);
  safe_line(&f, line);
  api->fmt(line, sizeof line, "level=%d\n", S.best_level);
  safe_line(&f, line);
  S.save_failed = safe_commit(&f) != 0;
  if (!S.save_failed) S.file = FILE_OK;
}

/* The best score and level, kept when a card shows the run's score -- and on
 * the card, if either was beaten. */
static void bank_best(void) {
  uint32_t score = (uint32_t)iron_score(&game);
  int changed = 0;
  if (score > S.best) { S.best = score; S.new_best = 1; changed = 1; }
  if (game.level > S.best_level) { S.best_level = game.level; changed = 1; }
  if (changed) save_best();
}

static void start_run(void) {
  uint32_t now = api->ticks_ms();
  iron_new_run(&game, now * 2654435761u + api->epoch());
  S.new_best = 0;
  S.note[0] = 0;
  go(SC_INTRO, now);
}

/* The garment ended in the game: show the card for how it went. Returns 1 if
 * the screen changed. */
static int after_play(void) {
  uint32_t now = api->ticks_ms();
  if (game.phase == IR_DONE) { bank_best(); go(SC_DONE, now); return 1; }
  if (game.phase == IR_OVER) { bank_best(); snd(SND_OVER); go(SC_OVER, now); return 1; }
  return 0;
}

static void ask_leave(int back) {
  iron_lift(&game);
  S.leave_back = back;
  go(SC_LEAVE, api->ticks_ms());
}

/* The run ends here, with whatever is in the score. */
static void leave_run(void) {
  if (game.phase == IR_PLAY || game.phase == IR_READY) iron_give_up(&game);
  bank_best();
  go(SC_OVER, api->ticks_ms());
}

static void lay(CRect c) {
  L.c = c;
  L.canvas = capp_rect(c.x, c.y, CANVAS_W, CANVAS_H);
  L.hx = c.x + CANVAS_W + 2;
  L.hw = HUD_W;
  L.gap = capp_rect(c.x + CANVAS_W, c.y, 2, CANVAS_H);
  L.right = capp_rect(L.hx + L.hw, c.y, c.x + c.w - (L.hx + L.hw), CANVAS_H);
  L.bottom = capp_rect(c.x, c.y + c.h - FOOT_H, c.w, FOOT_H);
  L.pad_top = capp_rect(L.hx, c.y, L.hw, 3);
  L.pad_bot = capp_rect(L.hx, c.y + 115, L.hw, CANVAS_H - 115);
}

/* ---- text and bars -------------------------------------------------------------- */

static int fits(int font, const char *s, int w) { return api->text_width(font, s) <= w; }

/* The big face for the title, or the bold one if /fonts has no such file. */
static int big_font(void) { return F.big >= 0 ? F.big : F.uib; }

static void fmt_time(char *buf, size_t n, int secs) {
  api->fmt(buf, n, "%d:%02d", secs / 60, secs % 60);
}

/* A line of text centred across `c` with its top at y, in `font`; returns
 * the y below it. For the cards, which are composed off the panel by the OS
 * over a fill, so text over a fill is fine there. */
static int line_c(CRect c, int y, int font, const char *s, uint16_t fg) {
  int w = api->text_width(font, s), x = c.x + (c.w - w) / 2;
  if (x < c.x) x = c.x;
  api->text_font(font, (int16_t)x, (int16_t)y, s, fg, C_BG);
  return y + api->font_height(font);
}

/* Text in a row of the HUD, the row's whole rectangle drawn once: the text
 * where `align` puts it (0 left, 1 centre, 2 right) and the background round
 * it. */
static void row_text(CRect row, int font, const char *s, uint16_t fg, int align) {
  int th = api->font_height(font), tw = api->text_width(font, s), ty, lx;
  if (tw > row.w) tw = row.w;
  ty = row.y + (row.h - th) / 2;
  if (ty < row.y) ty = row.y;
  lx = align == 0 ? row.x : align == 1 ? row.x + (row.w - tw) / 2 : row.x + row.w - tw;
  api->fill(capp_rect(row.x, row.y, row.w, ty - row.y), C_BG);
  api->fill(capp_rect(row.x, ty + th, row.w, row.y + row.h - (ty + th)), C_BG);
  api->fill(capp_rect(row.x, ty, lx - row.x, th), C_BG);
  api->fill(capp_rect(lx + tw, ty, row.x + row.w - (lx + tw), th), C_BG);
  api->text_font(font, (int16_t)lx, (int16_t)ty, s, fg, C_BG);
}

/* A bar is runs of colour across its width: the filled part, an optional
 * window behind the unfilled part (the heat gauge's good range), a one-pixel
 * marker (the pass mark), and the track. `edge` rows at the top and bottom
 * are drawn without the fill, so a gauge shows its window above and below it.
 * Each run is one fill, and no pixel is drawn twice. */
typedef struct { int f, lo, hi, mx, edge; uint16_t fill, track, window, mark; } BarSpec;

static void bar_set(BarSpec *b, int f, int lo, int hi, int mx, int edge,
                    uint16_t fill, uint16_t track, uint16_t window, uint16_t mark) {
  b->f = f; b->lo = lo; b->hi = hi; b->mx = mx; b->edge = edge;
  b->fill = fill; b->track = track; b->window = window; b->mark = mark;
}

static uint16_t bar_col(const BarSpec *b, int x, int filled) {
  if (x == b->mx) return b->mark;
  if (filled && x < b->f) return b->fill;
  if (x >= b->lo && x < b->hi) return b->window;
  return b->track;
}

static void bar_band(int x, int y, int w, int h, const BarSpec *b, int filled) {
  int i = 0;
  if (h <= 0) return;
  while (i < w) {
    uint16_t col = bar_col(b, i, filled);
    int j = i + 1;
    while (j < w && bar_col(b, j, filled) == col) j++;
    api->fill(capp_rect(x + i, y, j - i, h), col);
    i = j;
  }
}

static void bar_draw(CRect r, const BarSpec *b) {
  bar_band(r.x, r.y, r.w, b->edge, b, 0);
  bar_band(r.x, r.y + b->edge, r.w, r.h - 2 * b->edge, b, 1);
  bar_band(r.x, r.y + r.h - b->edge, r.w, b->edge, b, 0);
}

/* Heat as a colour: grey when off, blue, amber, red. */
static void heat_rgb(int heat, int *r, int *g, int *b) {
  int t;
  if (heat < 100) { *r = 120; *g = 126; *b = 140; return; }
  if (heat < 600) {
    t = (heat - 100) * 100 / 500;
    *r = 90 + (255 - 90) * t / 100;
    *g = 140 + (190 - 140) * t / 100;
    *b = 230 + (60 - 230) * t / 100;
  } else {
    t = (heat - 600) * 100 / 400;
    *r = 255;
    *g = 190 + (70 - 190) * t / 100;
    *b = 60 + (50 - 60) * t / 100;
  }
}

static uint16_t heat_colour(int heat) {
  int r, g, b;
  heat_rgb(heat, &r, &g, &b);
  return CAPP_RGB(r, g, b);
}

static uint16_t zone_col(int zone) {
  return zone == IR_Z_GOOD ? C_GOOD : zone == IR_Z_HOT ? C_WARN : C_COOL;
}

/* ---- the cloth ------------------------------------------------------------------ */

/* A neighbouring cell's value for the smoothing; a cell that is not cloth
 * stands in with the value of the cell the pixel is in, so the field does not
 * fade away at the hem. which: 0 wrinkle, 1 scorch. */
static inline int corner(int cx, int cy, int own, int which) {
  const IronCell *c;
  cx = iclamp(cx, 0, IR_W - 1);
  cy = iclamp(cy, 0, IR_H - 1);
  if (!(MASK[cy * IR_W + cx] & M_CLOTH)) return own;
  c = &game.cell[cy * IR_W + cx];
  return which ? c->scorch : c->wr;
}

/* The wrinkle and the scorch at board pixel (bu, bv), a cloth pixel: the
 * cells' values interpolated between their centres, so a crease is a soft
 * ridge and not a block. */
static void sample(int bu, int bv, int *wr, int *sc) {
  const IronCell *o = &game.cell[(bv / CELL) * IR_W + bu / CELL];
  int uu = bu - CELL / 2, vv = bv - CELL / 2;
  int cx0 = uu < 0 ? -1 : uu / CELL, cy0 = vv < 0 ? -1 : vv / CELL;
  int fx = uu - cx0 * CELL, fy = vv - cy0 * CELL;
  int ow = o->wr, os = o->scorch, top, bot;
  top = corner(cx0, cy0, ow, 0) * (CELL - fx) + corner(cx0 + 1, cy0, ow, 0) * fx;
  bot = corner(cx0, cy0 + 1, ow, 0) * (CELL - fx) + corner(cx0 + 1, cy0 + 1, ow, 0) * fx;
  *wr = (top * (CELL - fy) + bot * fy) / (CELL * CELL);
  top = corner(cx0, cy0, os, 1) * (CELL - fx) + corner(cx0 + 1, cy0, os, 1) * fx;
  bot = corner(cx0, cy0 + 1, os, 1) * (CELL - fx) + corner(cx0 + 1, cy0 + 1, os, 1) * fx;
  *sc = (top * (CELL - fy) + bot * fy) / (CELL * CELL);
}

static inline int nb(int v, int centre) { return v == NOFIELD ? centre : v; }

static inline int cloth_at(int bu, int bv) {
  if (bu < 0 || bu >= BOARD_W || bv < 0 || bv >= BOARD_H) return 0;
  return MASK[(bv / CELL) * IR_W + bu / CELL] & M_CLOTH;
}

/* One pixel of cloth: its colour, darker where it is creased, lit from the
 * top left by the slope of the wrinkle field (`sh`, positive on a side that
 * faces the light), a little weave, a darker hem on bare edges, the sheen
 * sweeping over a cell that has just gone flat (teal; gold when `gold`, a
 * high combo), and the scorch over it all. */
static void cloth_px(int kind, int mid, int bu, int bv, int wr, int sc, int sh, int gold,
                     int *r, int *g, int *b) {
  int ci = (bv / CELL) * IR_W + bu / CELL;
  int m = MASK[ci], gl = GL[ci], lx = bu % CELL, ly = bv % CELL, k;
  int rr = CLOTH_RGB[kind][0], gg = CLOTH_RGB[kind][1], bb = CLOTH_RGB[kind][2];
  if (kind == IR_TOWEL) {
    if (((bu / 10) ^ (bv / 10)) & 1) { rr -= 14; gg -= 10; }          /* gingham */
  } else if (kind == IR_SHIRT && (bu == mid - 1 || bu == mid)) {
    if ((bv - game.lv.by * CELL) % 15 >= 9 && (bv - game.lv.by * CELL) % 15 <= 10) {
      rr = 250; gg = 250; bb = 250;                                   /* a button */
    } else {
      rr -= 12; gg -= 12; bb -= 8;                                    /* the placket */
    }
  }
  k = 1024 - ((wr * 315) >> 10);
  rr = (rr * k) >> 10;
  gg = (gg * k) >> 10;
  bb = (bb * k) >> 10;
  sh = iclamp(sh, -45, 45);
  rr += sh; gg += sh; bb += sh;
  if ((((bu * 7 + bv * 13) >> 1) & 3) == 0) { rr -= 4; gg -= 4; bb -= 4; }
  if ((lx == 0 && (m & M_OPEN_W)) || (lx == CELL - 1 && (m & M_OPEN_E)) ||
      (ly == 0 && (m & M_OPEN_N)) || (ly == CELL - 1 && (m & M_OPEN_S))) {
    rr -= 34; gg -= 34; bb -= 30;
  }
  if (gl) {
    int d = lx + ly - (GLINT_LIFE - gl), a;
    int tr = gold ? 255 : 120, tg = gold ? 214 : 235, tb = gold ? 110 : 225;
    if (d < 0) d = -d;
    a = 130 - 46 * d;
    if (a > 0) {
      rr += ((tr - rr) * a) >> 8;
      gg += ((tg - gg) * a) >> 8;
      bb += ((tb - bb) * a) >> 8;
    }
    rr += gl; gg += gl; bb += gl;                                     /* still a touch fresher */
  }
  if (sc > 0) {
    int t;
    if (sc < 100) {                       /* a singe */
      t = sc * 255 / 100;
      rr += ((186 - rr) * t) >> 8;
      gg += ((140 - gg) * t) >> 8;
      bb += ((64 - bb) * t) >> 8;
    } else {                              /* then a char */
      t = (sc - 100) * 255 / 155;
      rr = 186 + (((58 - 186) * t) >> 8);
      gg = 140 + (((32 - 140) * t) >> 8);
      bb = 64 + (((14 - 64) * t) >> 8);
    }
  }
  *r = rr; *g = gg; *b = bb;
}

/* The ironing board's cover, quilted, and the cloth's shadow on it. */
static void table_px(int cu, int cv, int *r, int *g, int *b) {
  *r = 58; *g = 68; *b = 92;
  if ((cu + cv) % 14 == 0 || (cu - cv + 2 * CANVAS_H) % 14 == 0) { *r += 7; *g += 8; *b += 10; }
  if (cloth_at(cu - PAD - 3, cv - PAD - 3)) { *r -= 24; *g -= 26; *b -= 30; }
}

/* ---- the iron --------------------------------------------------------------------- */

static inline int in_sole(int mx, int dy) {
  int h;
  if (mx < -12 || mx > 12) return 0;
  h = SOLE_H[mx + 12];
  return dy >= -h && dy <= h;
}

/* The sole eroded by `rad` in the four directions: the inside of a shape
 * that follows its outline, nose and all. */
static inline int in_core(int mx, int dy, int rad) {
  return in_sole(mx, dy) && in_sole(mx - rad, dy) && in_sole(mx + rad, dy) &&
         in_sole(mx, dy - rad) && in_sole(mx, dy + rad);
}

/* The iron's pixel at (dx, dy) from the middle of its sole, over whatever
 * colour is there: its shadow, the sole (a rim in the colour of its heat,
 * the cloth showing through the plate, the steam vents), and the housing
 * with its grip and its dial. The steam is not part of it: it is left where
 * it was let out (see the puffs). */
static void spr_px(const Spr *sp, int dx, int dy, int *r, int *g, int *b) {
  int mx = sp->left ? -dx : dx, sd = 1 + sp->hv, hr, hg, hb, by, a;
  heat_rgb(sp->heat * 40, &hr, &hg, &hb);
  if (in_sole(sp->left ? -(dx - sd) : dx - sd, dy - sd)) {     /* further off the higher it is */
    *r = *r * 5 / 8; *g = *g * 5 / 8; *b = *b * 5 / 8;
  }
  if (in_sole(mx, dy)) {
    if (!in_core(mx, dy, 2)) {
      if (sp->dq) { hr = 255; hg = 96; hb = 56; }                /* about to scorch: it flickers */
      *r = hr; *g = hg; *b = hb;
    } else {
      a = sp->down ? 120 : 56;
      *r += ((hr - *r) * a) >> 8;
      *g += ((hg - *g) * a) >> 8;
      *b += ((hb - *b) * a) >> 8;
      if ((mx == 7 || mx == 9) && dy == 0) {
        if (sp->steam) { *r = 236; *g = 244; *b = 250; }
        else { *r = 30; *g = 40; *b = 52; }
      }
    }
  }
  by = dy + 3 + sp->hv;                                          /* the housing stands up off the sole */
  if (in_core(mx, by, 3)) {
    if (!in_core(mx, by, 4)) { *r = 16; *g = 74; *b = 80; }
    else if (mx >= -6 && mx <= -5 && by >= 0 && by <= 1) { *r = hr; *g = hg; *b = hb; }
    else if ((by == -3 || by == -2) && mx >= -6 && mx <= 2) { *r = 214; *g = 246; *b = 240; }
    else if (by <= -1) { *r = 128; *g = 232; *b = 222; }
    else if (by >= 2) { *r = 52; *g = 150; *b = 150; }
    else { *r = 88; *g = 208; *b = 196; }
  }
}

static void spr_now(Spr *s) {
  iron_xy(&s->cx, &s->cy);
  s->hv = S.hv;
  s->left = S.left;
  s->down = game.pressing;
  s->steam = game.steaming;
  s->danger = game.danger >= 160;
  s->dq = s->danger ? (int)((game.tick / 6u) & 1u) : 0;
  s->heat = game.heat / 40;
}

static int spr_eq(const Spr *a, const Spr *b) {
  return a->cx == b->cx && a->cy == b->cy && a->hv == b->hv && a->left == b->left &&
         a->down == b->down && a->steam == b->steam && a->danger == b->danger &&
         a->dq == b->dq && a->heat == b->heat;
}

/* Everything the iron itself can touch: its shadow and the housing. */
static CRect spr_rect(const Spr *s) {
  return capp_rect(L.canvas.x + s->cx - 18, L.canvas.y + s->cy - 14, 37, 28);
}

/* The pixels the cells under the iron can change: the footprint, one cell
 * more for the smoothing, one pixel more for the shading. */
static CRect cells_rect(void) {
  int ix = iron_ix(&game), iy = iron_iy(&game);
  return capp_rect(L.canvas.x + PAD + (ix - IR_FX - 1) * CELL - 1,
                   L.canvas.y + PAD + (iy - IR_FY - 1) * CELL - 1,
                   (2 * IR_FX + 3) * CELL + 2, (2 * IR_FY + 3) * CELL + 2);
}

/* ---- what floats over everything ------------------------------------------------------ */

/* A puff over the pixel (cu, cv): a soft disc that grows, thins as it ages
 * and is white-blue for steam, dark for smoke. */
static void puff_px(const Puff *p, int cu, int cv, int *r, int *g, int *b) {
  int dx = cu - (p->x >> 4), dy = cv - (p->y >> 4);
  int rad = p->r0 + p->age * 4 / p->life, rr = rad * rad + 1, d2 = dx * dx + dy * dy, a;
  if (d2 >= rr) return;
  a = (p->kind ? 170 : 150) * (p->life - p->age) / p->life;
  a = a * (rr - d2) / rr;
  if (p->kind) {
    *r += ((54 - *r) * a) >> 8;
    *g += ((48 - *g) * a) >> 8;
    *b += ((46 - *b) * a) >> 8;
  } else {
    *r += ((238 - *r) * a) >> 8;
    *g += ((246 - *g) * a) >> 8;
    *b += ((252 - *b) * a) >> 8;
  }
}

/* A star: a plus that grows to four pixels an arm and shrinks, with short
 * diagonals at its fullest. */
static void spk_px(const Spk *k, int cu, int cv, int *r, int *g, int *b) {
  int dx = cu - k->x, dy = cv - k->y, ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
  int len = k->age < SPK_LIFE / 2 ? k->age + 1 : SPK_LIFE - k->age;
  int m = ax > ay ? ax : ay, a;
  if (!((ax == 0 || ay == 0) && m <= len) && !(ax == ay && m <= len / 2)) return;
  a = 255 - m * 48;
  if (a < 70) a = 70;
  *r += ((255 - *r) * a) >> 8;
  *g += ((240 - *g) * a) >> 8;
  *b += ((140 - *b) * a) >> 8;
}

/* Is pixel (tx, ty) of `s` set in the 6x8 font? */
static int glyph_on(const char *s, int tx, int ty) {
  int ci, k;
  char ch;
  if (tx < 0 || ty < 0 || ty >= FONT_H) return 0;
  ci = tx / FONT_W;
  for (k = 0; k < ci; k++) {
    if (!s[k]) return 0;
  }
  ch = s[ci];
  if (ch < FONT_FIRST || ch > FONT_LAST) return 0;
  return (font6x8[ch - FONT_FIRST][tx % FONT_W] >> ty) & 1;
}

/* A word, in the 6x8 font, with a shadow, drawn over the pixel and not on a
 * box of its own. */
static void pop_px(const Pop *p, int cu, int cv, int *r, int *g, int *b) {
  int tx = cu - p->x, ty = cv - p->y;
  if (p->age > p->life - 6 && (p->age & 1)) return;                /* blinking out */
  if (glyph_on(p->text, tx, ty)) {
    *r = p->cr; *g = p->cg; *b = p->cb;
  } else if (glyph_on(p->text, tx - 1, ty - 1)) {
    *r = 18; *g = 18; *b = 26;
  }
}

/* ---- composing the canvas ------------------------------------------------------------ */

/* Rows y0 .. y0+h of the canvas, columns x0 .. x0+w, into OUT: the field a
 * pixel past the strip on every side (the shading looks at its neighbours),
 * then each pixel -- cloth or table, the iron over it, and over that the
 * steam, the stars and the words that reach this strip. */
static void strip(int x0, int y0, int w, int h, const Spr *sp) {
  int fw = w + 2, fx, fy, px, py, q, kind = game.lv.kind;
  int mid = (game.lv.bx + game.lv.w / 2) * CELL, gold = iron_tier(&game) >= 3;
  int pl[NPUFF], sl[NSPK], ol[NPOP], np = 0, ns = 0, no = 0;

  for (q = 0; q < NPUFF; q++) {
    if (PF[q].life) {
      int rad = PF[q].r0 + 5, qx = PF[q].x >> 4, qy = PF[q].y >> 4;
      if (qx + rad >= x0 && qx - rad < x0 + w && qy + rad >= y0 && qy - rad < y0 + h) pl[np++] = q;
    }
  }
  for (q = 0; q < NSPK; q++) {
    if (SK[q].life && SK[q].x + 5 >= x0 && SK[q].x - 5 < x0 + w && SK[q].y + 5 >= y0 && SK[q].y - 5 < y0 + h)
      sl[ns++] = q;
  }
  for (q = 0; q < NPOP; q++) {
    if (PO[q].life) {
      int tw = (int)api->str_len(PO[q].text) * FONT_W + 1;
      if (PO[q].x + tw >= x0 && PO[q].x - 1 < x0 + w && PO[q].y + FONT_H >= y0 && PO[q].y - 1 < y0 + h)
        ol[no++] = q;
    }
  }

  for (fy = -1; fy <= h; fy++) {
    int bv = iclamp(y0 + fy, 0, CANVAS_H - 1) - PAD;
    for (fx = -1; fx <= w; fx++) {
      int bu = iclamp(x0 + fx, 0, CANVAS_W - 1) - PAD, i = (fy + 1) * fw + (fx + 1);
      if (cloth_at(bu, bv)) {
        int wr, sc;
        sample(bu, bv, &wr, &sc);
        WF[i] = (uint16_t)wr;
        SF[i] = (uint8_t)sc;
      } else {
        WF[i] = (uint16_t)NOFIELD;
        SF[i] = 0;
      }
    }
  }
  for (py = 0; py < h; py++) {
    int cv = y0 + py;
    for (px = 0; px < w; px++) {
      int cu = x0 + px, fi = (py + 1) * fw + (px + 1), wr = WF[fi], r, g, b;
      if (wr != NOFIELD) {
        int sh = ((nb(WF[fi + 1], wr) - nb(WF[fi - 1], wr)) +
                  (nb(WF[fi + fw], wr) - nb(WF[fi - fw], wr))) / 10;
        cloth_px(kind, mid, cu - PAD, cv - PAD, wr, SF[fi], sh, gold, &r, &g, &b);
      } else {
        table_px(cu, cv, &r, &g, &b);
      }
      if (cu >= sp->cx - 18 && cu <= sp->cx + 18 && cv >= sp->cy - 14 && cv <= sp->cy + 13)
        spr_px(sp, cu - sp->cx, cv - sp->cy, &r, &g, &b);
      for (q = 0; q < np; q++) puff_px(&PF[pl[q]], cu, cv, &r, &g, &b);
      for (q = 0; q < ns; q++) spk_px(&SK[sl[q]], cu, cv, &r, &g, &b);
      for (q = 0; q < no; q++) pop_px(&PO[ol[q]], cu, cv, &r, &g, &b);
      OUT[py * w + px] = CAPP_RGB(cc(r), cc(g), cc(b));
    }
  }
}

/* `scr` (screen pixels, inside the canvas) composed and sent, a strip at a
 * time. */
static void compose(CRect scr, const Spr *sp) {
  int x0 = scr.x - L.canvas.x, y0 = scr.y - L.canvas.y, y;
  for (y = 0; y < scr.h; y += STRIP_H) {
    int h = scr.h - y < STRIP_H ? scr.h - y : STRIP_H;
    strip(x0, y0 + y, scr.w, h, sp);
    api->pixels(capp_rect(scr.x, scr.y + y, scr.w, h), OUT);
  }
}

/* ---- the HUD ---------------------------------------------------------------------------- */

static CRect hud_rect(int e) {
  int y = L.c.y;
  switch (e) {
  case E_LABEL: return capp_rect(L.hx, y + 3, L.hw, 9);
  case E_TIME:  return capp_rect(L.hx, y + 12, L.hw, 27);
  case E_FLAT:  return capp_rect(L.hx, y + 39, L.hw, 18);
  case E_FBAR:  return capp_rect(L.hx, y + 57, L.hw, 6);
  case E_SCORE: return capp_rect(L.hx, y + 63, L.hw, 19);
  case E_COMBO: return capp_rect(L.hx, y + 82, 32, 11);
  case E_METER: return capp_rect(L.hx + 32, y + 82, L.hw - 32, 11);
  case E_HEAT:  return capp_rect(L.hx, y + 93, L.hw, 9);
  case E_HBAR:  return capp_rect(L.hx, y + 102, L.hw, 8);
  case E_STEAM: return capp_rect(L.hx, y + 110, L.hw, 5);
  default:      return L.bottom;
  }
}

/* What an element shows, as one number: when it differs from what was drawn,
 * the element is redrawn. The combo's two carry the flash, so they are drawn
 * on every sample it lasts. */
static int hud_val(int e) {
  int s;
  switch (e) {
  case E_LABEL: return S.note[0] ? 100000 + S.note_serial : game.level;
  case E_TIME:
    s = iron_secs_left(&game);
    return s * 2 + (s <= IR_LOW_TIME_MS / 1000);
  case E_FLAT:  return iron_flat_pct(&game) * 2 + iron_pass_ok(&game);
  case E_FBAR:  return game.flat_pm * HUD_W / IR_PM * 2 + iron_pass_ok(&game);
  case E_SCORE: return (int)iron_score(&game);
  case E_COMBO: return iron_mult_pct(&game) * 16 + S.flash;
  case E_METER: return (game.meter * 32 / IR_METER_MAX * 8 + iron_tier(&game)) * 16 + S.flash;
  case E_HEAT:  return game.dial * 4 + iron_zone(&game);
  case E_HBAR:  return game.heat * HUD_W / IR_HEAT_MAX;
  case E_STEAM: return game.steam * HUD_W / IR_STEAM_MAX;
  default:      return iron_zone(&game) == IR_Z_GOOD;      /* the footer, which changes with it */
  }
}

static void hud_paint(int e) {
  CRect r = hud_rect(e);
  char buf[24], tm[12];
  int secs, mult, zone;
  uint16_t col;
  BarSpec b;
  switch (e) {
  case E_LABEL:
    if (S.note[0]) {
      row_text(r, -1, S.note, C_WARN, 0);
    } else {
      api->fmt(buf, sizeof buf, "L%d %s", game.level, iron_kind_name(game.lv.kind));
      if (!fits(-1, buf, r.w)) api->fmt(buf, sizeof buf, "level %d", game.level);
      row_text(r, -1, buf, C_ACCENT, 0);
    }
    break;
  case E_TIME:
    secs = iron_secs_left(&game);
    fmt_time(tm, sizeof tm, secs);
    row_text(r, fits(F.num, tm, r.w) ? F.num : F.uib, tm,
             secs <= IR_LOW_TIME_MS / 1000 ? C_WARN : C_TEXT, 1);
    break;
  case E_FLAT:
    api->fmt(buf, sizeof buf, "%d%%", iron_flat_pct(&game));
    row_text(capp_rect(r.x, r.y, r.w - 26, r.h), F.uib, buf,
             iron_pass_ok(&game) ? C_GOOD : C_TEXT, 0);
    row_text(capp_rect(r.x + r.w - 26, r.y, 26, r.h), -1, "flat", C_DIM, 2);
    break;
  case E_FBAR:
    bar_set(&b, game.flat_pm * r.w / IR_PM, 0, 0, game.lv.pass_pm * r.w / IR_PM, 0,
            iron_pass_ok(&game) ? C_GOOD : C_ACCENT, C_BAR, C_BAR, C_TEXT);
    bar_draw(r, &b);
    break;
  case E_SCORE:
    api->fmt(buf, sizeof buf, "%u", (unsigned)iron_score(&game));
    row_text(r, F.uib, buf, C_TEXT, 0);
    break;
  case E_COMBO:
    mult = iron_mult_pct(&game);
    api->fmt(buf, sizeof buf, "x%d.%02d", mult / 100, mult % 100);
    col = S.flash > 0 ? (S.flash_kind ? C_WARN : C_GOLD) : (mult > 100 ? C_ACCENT : C_DIM);
    row_text(r, -1, buf, col, 0);
    break;
  case E_METER:
    col = iron_tier(&game) >= 3 ? C_GOLD : C_ACCENT;
    if (S.flash > 0) col = S.flash_kind ? C_WARN : ((S.flash & 1) ? C_TEXT : C_GOLD);
    api->fill(capp_rect(r.x, r.y, r.w, 2), C_BG);
    api->fill(capp_rect(r.x, r.y + 2, 2, 6), C_BG);
    bar_set(&b, game.meter * (r.w - 2) / IR_METER_MAX, 0, 0, -1, 0, col, C_BAR, C_BAR, C_BAR);
    bar_draw(capp_rect(r.x + 2, r.y + 2, r.w - 2, 6), &b);
    api->fill(capp_rect(r.x, r.y + 8, r.w, 3), C_BG);
    break;
  case E_HEAT:
    zone = iron_zone(&game);
    api->fmt(buf, sizeof buf, "heat %d", game.dial);
    row_text(capp_rect(r.x, r.y, 42, r.h), -1, buf, C_DIM, 0);
    row_text(capp_rect(r.x + 42, r.y, r.w - 42, r.h), -1, ZONE_WORD[zone], zone_col(zone), 2);
    break;
  case E_HBAR:
    bar_set(&b, game.heat * r.w / IR_HEAT_MAX, game.lv.need * r.w / IR_HEAT_MAX,
            game.lv.burn * r.w / IR_HEAT_MAX, -1, 1, heat_colour(game.heat), C_BAR, C_WINDOW, C_BAR);
    bar_draw(r, &b);
    break;
  case E_STEAM:
    bar_set(&b, game.steam * r.w / IR_STEAM_MAX, 0, 0, -1, 0, C_STEAM, C_BAR, C_BAR, C_BAR);
    bar_draw(r, &b);
    break;
  default:
    footer_paint(api, L.c, foot_text());
    break;
  }
  S.hud[e] = hud_val(e);
}

/* ---- the garment done: fold it, stack it, name it ---------------------------------------- */

/* When things happen, in milliseconds from the result coming up. */
#define T_F1       400                    /* the sheen runs, then the first fold */
#define D_FOLD     400
#define T_F2       850                    /* the second fold */
#define T_SLIDE    1300                   /* it hops onto the pile */
#define D_SLIDE    500
#define T_LAND     1800
#define T_RATE     1800                   /* the rating pops in */
#define D_RATE     420
#define T_ROWS     2000                   /* then the numbers, a row at a time */
#define D_ROW      130
#define D_COUNT    500
#define ANIM_END   2900

/* 0 .. 1024 eased in and out. */
static int ease(int p) {
  p = iclamp(p, 0, 1024);
  return ((p * p) >> 10) * (3072 - 2 * p) >> 10;
}

static uint16_t cloth565(int kind, int pct) {
  return rgb(CLOTH_RGB[kind][0] * pct / 100, CLOTH_RGB[kind][1] * pct / 100, CLOTH_RGB[kind][2] * pct / 100);
}

static void map_id(Map *m, int ox, int oy) {
  m->fx = ox; m->fxo = 0; m->kx = 1024;
  m->fy = oy; m->fyo = 0; m->ky = 1024;
}

static int map_x(const Map *m, int xl) { return m->fx + (((xl - m->fxo) * m->kx) >> 10); }
static int map_y(const Map *m, int yl) { return m->fy + (((yl - m->fyo) * m->ky) >> 10); }

/* The runs of set bits in rows r0 .. r1 of `rows` (n rows tall in all, rh
 * pixels), between columns c0 and c1 (cw pixels each), as rectangles through
 * the map. A half of a garment that is being folded is the same call with a
 * map that squeezes it. */
static void mask_draw(const uint32_t *rows, int n, int r0, int r1, int c0, int c1, int cw, int rh,
                      const Map *m, uint16_t col) {
  int i, u;
  for (i = r0; i < r1; i++) {
    int ya = map_y(m, i * rh / n), yb = map_y(m, (i + 1) * rh / n);
    uint32_t bits = rows[i];
    if (ya > yb) { int t = ya; ya = yb; yb = t; }
    if (yb <= ya) continue;
    u = c0;
    while (u < c1) {
      int u1 = u, xa, xb;
      if (!(bits & (1u << u))) { u++; continue; }
      while (u1 < c1 && (bits & (1u << u1))) u1++;
      xa = map_x(m, u * cw);
      xb = map_x(m, u1 * cw);
      if (xa > xb) { int t = xa; xa = xb; xb = t; }
      if (xb > xa) api->fill(capp_rect(xa, ya, xb - xa, yb - ya), col);
      u = u1;
    }
  }
}

/* The left side of the card: a table, the pile of the garments already done,
 * and this one pressed, sheened, folded, folded, and put on top. */
static void paint_stage(CRect c) {
  int t = S.anim, kind = game.lv.kind, W = FW, H = FH, mid = W / 2, hh = H / 2;
  int cw = 3, rh = H * 3, ox = c.x + (112 - W * cw) / 2, oy = c.y + 28;
  int base = c.y + 100, nprev = game.level - 1, bw = mid * cw, bh = hh * 3, i;
  int ey, k, e;
  Map m;
  uint16_t col = cloth565(kind, 100);
  if (W <= 0 || H <= 0) return;
  if (nprev > 5) nprev = 5;
  ey = base - 6 * (nprev + 1);
  api->fill(capp_rect(c.x, base, 112, 22), C_TABLE_D);
  api->fill(capp_rect(c.x, base, 112, 1), C_TABLE_L);
  for (i = 0; i < nprev; i++) {
    int lvl = game.level - nprev + i, y = base - 6 * (i + 1);
    api->fill(capp_rect(c.x + 32, y, 48, 6), cloth565((lvl - 1) % 3, 100 - 10 * (i & 1)));
    api->fill(capp_rect(c.x + 32, y + 5, 48, 1), cloth565((lvl - 1) % 3, 62));
  }

  if (t < T_F1) {                                           /* pressed, with a sheen across it */
    uint32_t tmp[22];
    int uc = t * (W + 14) / T_F1 - 7;
    map_id(&m, ox, oy);
    mask_draw(FM0, H, 0, H, 0, W, cw, rh, &m, col);
    for (i = 0; i < H; i++) {
      uint32_t band = 0;
      int lo = uc - i / 2 - 2, j;
      for (j = 0; j < 5; j++)
        if (lo + j >= 0 && lo + j < W) band |= 1u << (lo + j);
      tmp[i] = FM0[i] & band;
    }
    mask_draw(tmp, H, 0, H, 0, W, cw, rh, &m, rgb(150, 240, 228));
  } else if (t < T_F1 + D_FOLD) {                           /* the left half turns over onto the right */
    e = ease((t - T_F1) * 1024 / D_FOLD);
    k = 1024 - 2 * e;
    map_id(&m, ox, oy);
    mask_draw(FM0, H, 0, H, mid, W, cw, rh, &m, col);
    m.fx = ox + mid * cw; m.fxo = mid * cw; m.kx = k;
    mask_draw(FM0, H, 0, H, 0, mid, cw, rh, &m, cloth565(kind, 70 + 30 * (k < 0 ? -k : k) / 1024));
  } else if (t < T_F2) {
    map_id(&m, ox + mid * cw, oy);
    mask_draw(FM1, H, 0, H, 0, mid, cw, rh, &m, col);
  } else if (t < T_F2 + D_FOLD) {                           /* and the top half down */
    e = ease((t - T_F2) * 1024 / D_FOLD);
    k = 1024 - 2 * e;
    map_id(&m, ox + mid * cw, oy);
    mask_draw(FM1, H, hh, H, 0, mid, cw, rh, &m, col);
    m.fy = oy + hh * 3; m.fyo = hh * 3; m.ky = k;
    mask_draw(FM1, H, 0, hh, 0, mid, cw, rh, &m, cloth565(kind, 70 + 30 * (k < 0 ? -k : k) / 1024));
  } else if (t < T_SLIDE) {
    map_id(&m, ox + mid * cw, oy + hh * 3);
    mask_draw(FM2, hh, 0, hh, 0, mid, cw, bh, &m, col);
  } else if (t < T_SLIDE + D_SLIDE) {                       /* hops to the pile and flattens onto it */
    int u = ease((t - T_SLIDE) * 1024 / D_SLIDE), q = iclamp((u - 600) * 1024 / 424, 0, 1024);
    int sx = ox + mid * cw, sy = oy + hh * 3, ex = c.x + 56 - bw / 2, ex_y = ey + 6 - bh;
    int x = sx + (((ex - sx) * u) >> 10);
    int y = sy + (((ex_y - sy) * u) >> 10) - ((56 * ((u * (1024 - u)) >> 10)) >> 10);
    if (q == 0) {
      map_id(&m, x, y);
      mask_draw(FM2, hh, 0, hh, 0, mid, cw, bh, &m, col);
    } else {
      int hnow = bh + (((6 - bh) * q) >> 10), wnow = bw + (((48 - bw) * q) >> 10);
      int xl = x + (((c.x + 32 - x) * q) >> 10);
      api->fill(capp_rect(xl, y + bh - hnow, wnow, hnow), col);
      api->fill(capp_rect(xl, y + bh - 1, wnow, 1), cloth565(kind, 62));
    }
  } else {                                                  /* on the pile */
    int dt = t - T_LAND, bounce = dt < 60 ? 2 : dt < 120 ? -1 : 0;
    api->fill(capp_rect(c.x + 32, ey + bounce, 48, 6), col);
    api->fill(capp_rect(c.x + 32, ey + bounce + 5, 48, 1), cloth565(kind, 62));
  }
}

/* A label on the left and a number on the right, for the result card. */
static void money_row(int x0, int w, int y, const char *label, int32_t v, int plus, uint16_t fg) {
  char buf[16];
  int tw;
  if (plus) api->fmt(buf, sizeof buf, v < 0 ? "-%d" : "+%d", (int)(v < 0 ? -v : v));
  else api->fmt(buf, sizeof buf, "%d", (int)v);
  tw = (int)api->str_len(buf) * 6;
  api->text((int16_t)x0, (int16_t)y, label, C_DIM, C_BG);
  api->text((int16_t)(x0 + w - tw), (int16_t)y, buf, fg, C_BG);
}

/* The right of the card: the rating pops up (stars round it, when it is
 * Crisp!), then the numbers count up a row at a time. */
static void paint_done_info(CRect c) {
  const IronResult *r = &game.res;
  const char *name = iron_rating_name(r->rating);
  int t = S.anim, x0 = c.x + 118, w = 118, i, n = 0, y;
  int fr = fits(big_font(), name, w) ? big_font() : F.uib;
  int fh = api->font_height(fr), ry = c.y + 3;
  int32_t taken = r->earned - (r->total - r->flat_bonus - r->time_bonus);
  const char *labels[5];
  int32_t vals[5];
  uint16_t cols[5];
  char buf[40];
  uint16_t rc = r->rating == IR_R_CRISP ? C_GOOD : r->rating == IR_R_SMOOTH ? C_ACCENT : C_TEXT;

  if (t >= T_RATE) {
    int p = iclamp((t - T_RATE) * 1024 / D_RATE, 0, 1024);
    int eo = p < 614 ? 1177 * p / 614 : 1177 - 153 * (p - 614) / 410;
    int tw = api->text_width(fr, name), yoff = 22 * (1024 - eo) / 1024;
    api->text_font(fr, (int16_t)(x0 + (w - tw) / 2), (int16_t)(ry + yoff), name, rc, C_BG);
    if (r->rating == IR_R_CRISP) {
      for (i = 0; i < 9; i++) {
        int tt = t - T_RATE - i * 45, sz, sx, sy;
        if (tt < 0 || tt > 300) continue;
        sz = (tt < 150 ? tt : 300 - tt) / 38 + 1;
        sx = x0 + scatter(i, 1, w);
        sy = ry + scatter(i, 2, fh > 0 ? fh : 1);
        api->fill(capp_rect(sx - sz, sy, 2 * sz + 1, 1), C_GOLD);
        api->fill(capp_rect(sx, sy - sz, 1, 2 * sz + 1), C_GOLD);
      }
    }
    api->fmt(buf, sizeof buf, "%s: %d%% flat", iron_kind_name(game.lv.kind), r->flat_pm / 10);
    api->text_font(F.ui, (int16_t)(x0 + (w - api->text_width(F.ui, buf)) / 2),
                   (int16_t)(ry + fh + 2), buf, C_TEXT, C_BG);
  }

  y = ry + fh + 2 + api->font_height(F.ui) + 4;
  labels[n] = "pressing";   vals[n] = r->earned;     cols[n++] = C_TEXT;
  if (taken > 0) { labels[n] = "scorch"; vals[n] = -taken; cols[n++] = C_WARN; }
  labels[n] = "flat bonus"; vals[n] = r->flat_bonus; cols[n++] = C_TEXT;
  labels[n] = "time bonus"; vals[n] = r->time_bonus; cols[n++] = C_TEXT;
  labels[n] = "score";      vals[n] = game.score;    cols[n++] = C_GOOD;
  for (i = 0; i < n; i++) {
    int t0 = T_ROWS + i * D_ROW, prog = iclamp(t - t0, 0, D_COUNT);
    int32_t v = vals[i];
    if (t < t0) continue;
    if (i == n - 1) v = game.score - r->total + r->total * prog / D_COUNT;      /* the score climbs */
    else v = v * prog / D_COUNT;
    money_row(x0, w, y + i * 10, labels[i], v, i != n - 1, cols[i]);
  }
}

/* What the keys do on the screen that is up: the footer's words, 38
 * characters at most. On the garment it also says how to change the heat when
 * the iron is not at the right one for the cloth, which is when it matters. */
static const char *foot_text(void) {
  switch (S.screen) {
  case SC_TITLE: return "enter start  m sound";
  case SC_INTRO: return "enter start  h - = heat  esc back";
  case SC_PLAY:  return iron_zone(&game) == IR_Z_GOOD ? "arrows move  space press  p pause"
                                                      : "arrows move  space press  h - = heat";
  case SC_PAUSE: return "p resume  esc leave the run";
  case SC_LEAVE: return "y leave  n keep going";
  case SC_DONE:  return S.anim >= ANIM_END ? "enter next  esc leave" : "enter skip";
  default:       return "enter again  esc title";
  }
}

/* Where a block of cards' text `h` tall starts, to sit in the middle of the
 * screen above the footer. */
static int card_y(CRect c, int h) {
  int y = c.y + (CANVAS_H - h) / 2;
  return y < c.y + 2 ? c.y + 2 : y;
}

static void paint_done(CRect c) {
  api->fill(c, C_BG);
  paint_stage(c);
  paint_done_info(c);
  footer_paint(api, c, foot_text());
}

/* ---- the other screens ------------------------------------------------------------------- */

static void fill_if(CRect r, CRect clip) {
  if (capp_overlaps(r, clip)) api->fill(r, C_BG);
}

/* The garment. Our own damage coming back is only the rectangles on the list
 * and the HUD elements whose value moved; any other clip is drawn whole. */
static void paint_play(void) {
  CRect clip = api->paint_area(), r;
  int own = !S.need_full && S.has_mark && crect_eq(clip, S.mark), e, j;
  Spr cur;
  spr_now(&cur);
  if (!own) {
    fill_if(L.gap, clip);
    fill_if(L.right, clip);
    fill_if(L.pad_top, clip);
    fill_if(L.pad_bot, clip);
    r = crect_isect(clip, L.canvas);
    if (!crect_empty(r)) compose(r, &cur);
  } else {
    for (j = 0; j < S.nbd; j++) {
      r = crect_isect(crect_isect(S.bd[j], L.canvas), clip);
      if (!crect_empty(r)) compose(r, &cur);
    }
  }
  for (e = 0; e < E_COUNT; e++) {
    if (own ? hud_val(e) != S.hud[e] : capp_overlaps(hud_rect(e), clip)) hud_paint(e);
  }
  spr_now(&S.spr);
  S.spr_valid = 1;
  S.has_mark = 0;
  S.nbd = 0;
  S.need_full = 0;
}

/* The title: the best score and level from the file, or what is wrong with
 * the file. */
static void paint_title(CRect c) {
  char buf[40];
  int fb = big_font(), hb = api->font_height(fb);
  int hu = api->font_height(F.ui), hs = api->font_height(F.uib);
  const char *warn = S.file == FILE_BAD ? "scores file unreadable - starting over"
                   : S.save_failed ? "could not save the best score" : 0;
  int total = hb + 2 + hu + 8 + hu + 8 + hs + (warn ? 8 + 8 : 0), y;
  if (S.best > 0) api->fmt(buf, sizeof buf, "best %u   level %d", (unsigned)S.best, S.best_level);
  else api->fmt(buf, sizeof buf, "no best yet");
  api->fill(c, C_BG);
  y = card_y(c, total);
  y = line_c(c, y, fb, "IRON", C_ACCENT) + 2;
  y = line_c(c, y, F.ui, "press the wrinkles out", C_DIM) + 8;
  y = line_c(c, y, F.ui, buf, C_TEXT) + 8;
  y = line_c(c, y, F.uib, "enter  start", C_TEXT);
  if (warn) line_c(c, y + 8, -1, warn, C_WARN);
  footer_paint(api, c, foot_text());
}

static void paint_intro(CRect c) {
  char buf[48], tm[12];
  BarSpec b;
  int hs = api->font_height(F.uib), hu = api->font_height(F.ui);
  int y = card_y(c, hs + 3 * hu + 2 + 4 + 11 + 8), z = iron_zone(&game);
  api->fill(c, C_BG);
  api->fmt(buf, sizeof buf, "LEVEL %d", game.level);
  y = line_c(c, y, F.uib, buf, C_ACCENT);
  api->fmt(buf, sizeof buf, "%s (%s)", iron_kind_name(game.lv.kind), iron_fabric_name(game.lv.kind));
  y = line_c(c, y, F.ui, buf, C_TEXT);
  fmt_time(tm, sizeof tm, (int)(game.lv.time_ms / 1000));
  api->fmt(buf, sizeof buf, "get it %d%% flat in %s", iron_pass_pct(&game), tm);
  y = line_c(c, y, F.ui, buf, C_DIM) + 2;
  api->fmt(buf, sizeof buf, "ideal heat %d    yours %d", iron_ideal_dial(&game), game.dial);
  y = line_c(c, y, F.ui, buf, C_TEXT) + 4;
  bar_set(&b, game.heat * 120 / IR_HEAT_MAX, game.lv.need * 120 / IR_HEAT_MAX,
          game.lv.burn * 120 / IR_HEAT_MAX, -1, 1, heat_colour(game.heat), C_BAR, C_WINDOW, C_BAR);
  bar_draw(capp_rect(c.x + (c.w - 120) / 2, y, 120, 8), &b);
  y += 11;
  line_c(c, y, -1, ZONE_TEXT[z], zone_col(z));
  footer_paint(api, c, foot_text());
}

static void paint_pause(CRect c) {
  char buf[32];
  int hs = api->font_height(F.uib), hu = api->font_height(F.ui);
  int y = card_y(c, hs + 8 + hu);
  api->fill(c, C_BG);
  y = line_c(c, y, F.uib, "PAUSED", C_ACCENT) + 8;
  api->fmt(buf, sizeof buf, "score %u", (unsigned)iron_score(&game));
  line_c(c, y, F.ui, buf, C_TEXT);
  footer_paint(api, c, foot_text());
}

static void paint_leave(CRect c) {
  char buf[32];
  int hs = api->font_height(F.uib), hu = api->font_height(F.ui);
  int y = card_y(c, hs + 6 + hu);
  api->fill(c, C_BG);
  y = line_c(c, y, F.uib, "Leave this run?", C_ACCENT) + 6;
  api->fmt(buf, sizeof buf, "score so far %u", (unsigned)iron_score(&game));
  line_c(c, y, F.ui, buf, C_TEXT);
  footer_paint(api, c, foot_text());
}

static const char *over_reason(void) {
  if (game.phase == IR_DONE) return "you cashed out";
  switch (game.why) {
  case IR_WHY_BURNT:  return "burnt right through";
  case IR_WHY_TIME:   return "out of time";
  case IR_WHY_GIVEUP: return "you left the run";
  default:            return "not flat enough";
  }
}

static void paint_over(CRect c) {
  char buf[40];
  int hs = api->font_height(F.uib), hu = api->font_height(F.ui);
  int timed = game.phase == IR_OVER && game.why == IR_WHY_TIME;
  int total = hs + 2 + hu + (timed ? 8 : 0) + 6 + hu + hu + 2 + (S.new_best ? hs : hu) + 8
              + (S.save_failed ? 4 + 8 : 0);
  int y = card_y(c, total);
  api->fill(c, C_BG);
  y = line_c(c, y, F.uib, "RUN OVER", C_ACCENT) + 2;
  y = line_c(c, y, F.ui, over_reason(), C_WARN);
  if (timed) {
    api->fmt(buf, sizeof buf, "%d%% flat, needed %d%%", game.res.flat_pm / 10, iron_pass_pct(&game));
    y = line_c(c, y, -1, buf, C_DIM);
  }
  y += 6;
  api->fmt(buf, sizeof buf, "reached level %d", game.level);
  y = line_c(c, y, F.ui, buf, C_TEXT);
  api->fmt(buf, sizeof buf, "score %u", (unsigned)iron_score(&game));
  y = line_c(c, y, F.ui, buf, C_TEXT) + 2;
  if (S.new_best) {
    y = line_c(c, y, F.uib, "new best!", C_GOOD);
  } else {
    api->fmt(buf, sizeof buf, "best %u", (unsigned)S.best);
    y = line_c(c, y, F.ui, buf, C_DIM);
  }
  api->fmt(buf, sizeof buf, "highest level %d", S.best_level);
  y = line_c(c, y, -1, buf, C_DIM);
  if (S.save_failed) line_c(c, y + 4, -1, "could not save the best score", C_WARN);
  footer_paint(api, c, foot_text());
}

static void app_paint(void *st, CRect c) {
  (void)st;
  S.area = c;
  S.have_area = 1;
  lay(c);
  switch (S.screen) {
  case SC_TITLE: paint_title(c); break;
  case SC_INTRO: paint_intro(c); break;
  case SC_PLAY:  paint_play(); break;
  case SC_PAUSE: paint_pause(c); break;
  case SC_LEAVE: paint_leave(c); break;
  case SC_DONE:  paint_done(c); break;
  default:       paint_over(c); break;
  }
}

/* ---- keys --------------------------------------------------------------------------------- */

static int arrow_dir(uint8_t k) {
  switch (k) {
  case CAPP_KEY_UP:    return IR_UP;
  case CAPP_KEY_DOWN:  return IR_DOWN;
  case CAPP_KEY_LEFT:  return IR_LEFT;
  case CAPP_KEY_RIGHT: return IR_RIGHT;
  default:             return -1;
  }
}

/* H cycles the dial round (off, then 1 to 5); - and = step it down and up. */
static int heat_key(uint8_t k) {
  switch (k) {
  case 'h': case 'H':
    iron_dial_set(&game, game.dial >= IR_DIAL_MAX ? 0 : game.dial + 1);
    return 1;
  case '-': case '_':
    iron_dial_adjust(&game, -1);
    return 1;
  case '=': case '+':
    iron_dial_adjust(&game, 1);
    return 1;
  default:
    return 0;
  }
}

/* The result card's sounds, each when the animation reaches it. */
static void done_sounds(void) {
  if (S.anim >= T_F1 && !(S.fired & 1)) { S.fired |= 1; snd(SND_FOLD); }
  if (S.anim >= T_F2 && !(S.fired & 2)) { S.fired |= 2; snd(SND_FOLD); }
  if (S.anim >= T_LAND && !(S.fired & 4)) { S.fired |= 4; snd(SND_THUMP); }
  if (S.anim >= T_RATE + 80 && !(S.fired & 8)) {
    S.fired |= 8;
    snd(game.res.rating == IR_R_CRISP ? SND_CRISP : SND_NICE);
  }
}

static int key_title(uint8_t k, int repeat) {
  if (repeat) return 1;
  if (k == CAPP_KEY_ENTER || k == ' ' || k == 'n' || k == 'N') { start_run(); return 1; }
  return 0;                                  /* Esc: the top level keeps it */
}

static int key_intro(uint8_t k, int repeat) {
  uint32_t now = api->ticks_ms();
  if (repeat) return 1;
  if (k == CAPP_KEY_ENTER || k == ' ') {
    iron_begin(&game);
    go(SC_PLAY, now);
    return 1;
  }
  if (heat_key(k)) return 1;
  if (k == CAPP_KEY_ESC) {
    if (game.level == 1) go(SC_TITLE, now);
    else ask_leave(SC_INTRO);
    return 1;
  }
  return 0;
}

/* On the garment a key changes nothing that shows until the next sample, so
 * it asks for no repaint (the tick marks what moved); a key that changes the
 * screen is the exception. */
static int key_play(uint8_t k, int repeat) {
  int dir = arrow_dir(k);
  uint32_t now = api->ticks_ms();
  if (dir >= 0) { iron_move_key(&game, dir, repeat); return 0; }
  if (k == ' ') { iron_press_key(&game, repeat); return 0; }
  if (repeat) return 0;
  if (heat_key(k)) return 0;
  if (k == CAPP_KEY_ENTER) {
    if (iron_finish_early(&game) != 0) note_need();
    return after_play();
  }
  if (k == 'p' || k == 'P') {
    iron_lift(&game);
    go(SC_PAUSE, now);
    return 1;
  }
  if (k == CAPP_KEY_ESC) { ask_leave(SC_PLAY); return 1; }
  return 0;
}

static int key_pause(uint8_t k, int repeat) {
  uint32_t now = api->ticks_ms();
  if (repeat) return 1;
  if (k == 'p' || k == 'P' || k == CAPP_KEY_ENTER || k == ' ') { go(SC_PLAY, now); return 1; }
  if (k == CAPP_KEY_ESC) { ask_leave(SC_PAUSE); return 1; }
  return 0;
}

static int key_leave(uint8_t k) {
  int a = confirm_key(api, k);
  if (a == CONFIRM_YES) leave_run();
  else if (a == CONFIRM_NO) go(S.leave_back, api->ticks_ms());
  return 1;                                  /* the question swallows every key */
}

static int key_done(uint8_t k, int repeat) {
  uint32_t now = api->ticks_ms();
  if (repeat) return 1;
  if (k == CAPP_KEY_ENTER || k == ' ') {
    if (S.anim < ANIM_END) {                 /* the first press skips to the end */
      S.anim = ANIM_END;
      S.fired |= 7;
      done_sounds();
      return 1;
    }
    iron_next_garment(&game);
    go(SC_INTRO, now);
    return 1;
  }
  if (k == CAPP_KEY_ESC) { ask_leave(SC_DONE); return 1; }
  return 0;
}

static int key_over(uint8_t k, int repeat) {
  if (repeat) return 1;
  if (k == CAPP_KEY_ENTER || k == ' ' || k == 'n' || k == 'N') { start_run(); return 1; }
  if (k == CAPP_KEY_ESC) { go(SC_TITLE, api->ticks_ms()); return 1; }
  return 0;
}

static int app_key(void *st, uint8_t k) {
  int repeat = api->key_repeat();
  (void)st;
  if ((k == 'm' || k == 'M') && S.screen != SC_LEAVE) {      /* sound on and off, anywhere */
    if (!repeat) {
      S.sound = !S.sound;
      note_say(S.sound ? "sound on" : "sound off");
      if (S.sound) snd(SND_TING);
    }
    return S.screen == SC_PLAY ? 0 : 1;
  }
  switch (S.screen) {
  case SC_TITLE: return key_title(k, repeat);
  case SC_INTRO: return key_intro(k, repeat);
  case SC_PLAY:  return key_play(k, repeat);
  case SC_PAUSE: return key_pause(k, repeat);
  case SC_LEAVE: return key_leave(k);
  case SC_DONE:  return key_done(k, repeat);
  default:       return key_over(k, repeat);
  }
}

/* ---- the clock ------------------------------------------------------------------------------ */

/* Move the effects on, compare what the garment would draw now with what was
 * drawn, and mark the difference. Returns 1 if anything was marked. */
static int play_dirty(void) {
  Spr cur;
  int e, j, pressing = game.pressing;
  S.marked = 0;
  if (!S.have_area || S.need_full) return 0;

  /* The iron comes down over three frames and lifts over three. */
  if (pressing && S.hv > 0) S.hv--;
  else if (!pressing && S.hv < 3) S.hv++;
  if (game.face == IR_LEFT) S.left = 1;
  else if (game.face == IR_RIGHT) S.left = 0;

  fx_step();
  spr_now(&cur);
  if (!S.spr_valid || !spr_eq(&cur, &S.spr)) {
    if (S.spr_valid) bd_add(spr_rect(&S.spr));
    bd_add(spr_rect(&cur));
  }
  /* The cells it presses, from where it was at the last sample to where it
   * is: it moves a cell or so between samples. */
  if (pressing || S.was_pressing || (int)game.sumw != S.last_sumw) {
    CRect cr = cells_rect();
    bd_add(S.was_pressing ? crect_union(cr, S.cells_prev) : cr);
    S.cells_prev = cr;
  }
  S.was_pressing = pressing;
  S.last_sumw = (int)game.sumw;

  if (S.note[0] && (int32_t)(api->ticks_ms() - S.note_until) >= 0) S.note[0] = 0;
  for (e = 0; e < E_COUNT; e++)
    if (hud_val(e) != S.hud[e]) mark(hud_rect(e));
  for (j = 0; j < S.nbd; j++) mark(S.bd[j]);
  return S.marked != 0;
}

/* The result card: the animation runs on the clock, a frame at a time, until
 * it is over, and then the card is still. */
static int done_tick(uint32_t now) {
  uint32_t dt = now - S.last_ms;
  S.last_ms = now;
  if (S.anim >= ANIM_END) return 0;
  S.anim += (int)(dt > 100u ? 100u : dt);
  if (S.anim > ANIM_END) S.anim = ANIM_END;
  done_sounds();
  if (now - S.frame_ms < FRAME_MS && S.anim < ANIM_END) return 0;
  S.frame_ms = now;
  return 1;
}

static int app_tick(void *st, uint32_t now) {
  (void)st;
  if (S.screen == SC_DONE) return done_tick(now);
  if (S.screen != SC_PLAY && S.screen != SC_INTRO) { S.last_ms = now; return 0; }

  iron_advance(&game, now - S.last_ms);
  S.last_ms = now;
  if (S.screen == SC_PLAY) {
    fx_events();
    if (game.phase != IR_PLAY) return after_play();
  }

  if (now - S.frame_ms < FRAME_MS) return 0;
  S.frame_ms = now;
  if (S.screen == SC_PLAY) return play_dirty();

  /* The level card: the iron is warming, and the gauge moves. */
  if (game.heat != S.i_heat || game.dial != S.i_dial) {
    S.i_heat = game.heat;
    S.i_dial = game.dial;
    return 1;
  }
  return 0;
}

/* ---- the app ------------------------------------------------------------------------------------ */

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Iron",
  /* 16x16: steam over an iron, handle, body and sole. */
  { 0x00, 0x00, 0x12, 0x40, 0x09, 0x20, 0x00, 0x00,
    0x07, 0xE0, 0x0C, 0x30, 0x08, 0x10, 0x3F, 0xF8,
    0x3F, 0xFE, 0x3F, 0xFF, 0x3F, 0xFF, 0x7F, 0xFF,
    0x7F, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  /* What the panel can show: eight lines under its heading, a key of seven
   * characters and a meaning of twenty-nine. */
  "arrows\tmove the iron\n"
  "space\thold to press, add arrows\n"
  "enter\tdone, once past the goal\n"
  "h\tcycle the heat dial\n"
  "- =\tcooler / hotter\n"
  "p\tpause\n"
  "m\tsound on / off\n"
  "esc\tleave the run (asks first)\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&S, 0, sizeof S);
  api->mem_set(&game, 0, sizeof game);
  S.screen = SC_TITLE;
  S.sound = 1;
  S.rng = api->ticks_ms() | 1u;
  F.ui = api->font_load("ui13");
  F.uib = api->font_load("ui13b");
  F.num = api->font_load("num30");
  F.big = api->font_load("print34b");
  au = api->audio ? api->audio() : 0;
  if (au) ensure_sounds();
  load_best();                               /* the title shows what it found */

  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.pref_w = 240;
  UI.pref_h = 135;
  api->ui(&UI);
  return 0;
}
