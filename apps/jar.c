/* Jar Factory -- a terrarium with a tiny factory inside, running unattended.
 *
 * Mosslings carry berries from the garden beds to a thimble vat; a matchbox
 * press, a cotton spool and a twig crane, joined by a bottle-cap belt, make
 * them into jam; a snail carries the jars out through the cork door, and the
 * coins land as it leaves the screen. Coins buy upgrades (another mossling,
 * machine, bed, a faster belt) and items from the shop, which go in the jar
 * and do small things of their own. Nothing needs attention: leave it on.
 *
 * The design is docs/superpowers/specs/2026-10-09-jar-factory-design.md;
 * this is its steps 1 to 3, offline. The world is apps/jarsim.h, the item
 * record apps/jaritem.h, the pictures apps/jar_art.h (tools/make_jar_art.py
 * from tools/jar_art.txt). Host tests: test/test_jarsim.c,
 * test/test_jaritem.c, test/test_jar.c (JAR_DUMP=dir writes frames).
 *
 * DRAWING. The scene moves every frame, so the whole screen -- scene, bars
 * and menus alike -- is composed here a strip at a time into one buffer and
 * blitted whole (CAPP_PAINT_DIRECT, as Kart and Noodle), so nothing is ever
 * filled and then drawn over on the panel. A strip that came out the same as
 * last time is not sent: the soil and a still menu cost nothing. The strip is
 * 8 rows, not 16: at 16 the app was 3.7 KB over the 44 KB budget, and the
 * strip was the one piece of data that could shrink without the game losing
 * anything.
 *
 * ON THE CARD. /var/jar/jar.txt is the jar (apps/jarsim.h, js_save), saved
 * on every purchase and placement and every five minutes; /var/jar/items/
 * holds one record per owned item. Both go through apps/safefile.h.
 */

#include "kernel/app/capp.h"
#include "kernel/console/font6x8.h"
#include "apps/str.h"
#include "apps/safefile.h"
#include "apps/datetime.h"
#include "apps/jarsim.h"
#include "apps/jar_art.h"

#define SW 240
#define SHT 135
#define SH 8                            /* rows a strip: 3.75 KB, see below */
#define NSTRIP ((SHT + SH - 1) / SH)
#define BAR 13
#define FRAME_MS 66                     /* the scene at 15 frames a second */
#define BARS_IDLE_MS 10000
#define SAVE_MS (5u * 60u * 1000u)

#define DIR_JAR   CAPP_VAR "/jar"
#define DIR_ITEMS CAPP_VAR "/jar/items"
#define SAVE_PATH CAPP_VAR "/jar/jar.txt"

/* The interface palette (spec, "Screens and look"). */
#define C_BG     CAPP_RGB(0x17, 0x13, 0x2a)
#define C_PANEL  CAPP_RGB(0x2a, 0x24, 0x47)
#define C_TEXT   CAPP_RGB(0xf2, 0xec, 0xdc)
#define C_DIM    CAPP_RGB(0xa5, 0x9f, 0xc4)
#define C_GOLD   CAPP_RGB(0xff, 0xd1, 0x66)
#define C_TRAIT  CAPP_RGB(0x7f, 0xd6, 0xa6)
#define C_WARN   CAPP_RGB(0xff, 0x6b, 0x5a)
#define C_PINK   CAPP_RGB(0xff, 0x8f, 0xab)

enum { V_JAR = 0, V_DECOR, V_UP, V_SHOP, V_CARD };

static const CardApi *api;
static Jar J;

/* The strip, and the same memory for card I/O, which never happens while a
 * strip is being drawn. */
static union {
  uint16_t strip[SW * SH];
  struct { uint8_t raw[JI_MAX]; JItem it; } io;
  char text[2048];
} SCR;

/* What a shop tile or a detail panel shows of an item. */
typedef struct {
  uint32_t id;
  uint16_t price, pal[8];
  uint8_t kind, move, flags, placed, sold, ok;
  uint8_t frame[JI_FRAME_BYTES];
} Tile;

typedef struct {
  char name[JI_NAME + 1], line[JI_LINE + 1], maker[JI_WHO + 1], tags[JI_TAGS + 1], gifted[JI_WHO + 1];
  uint32_t made;
} Words;

static struct {
  int view, tab, sel, top;              /* shop: tab 0 stock, 1 my stuff */
  int pick;                             /* my stuff opened from decorate */
  int up_sel;
  int dsel, dmove, dnew;                /* decorate: selection, moving, a new one */
  int16_t dx0, dy0;
  int card_from;
  Tile tile[8];
  int ntile;
  Tile card;                            /* the item card's picture, and the detail panel's */
  Words w;                              /* its words */
  char dname[JI_NAME + 1];              /* the item selected in decorate */
  char dline[JI_LINE + 1];
  char msg[40];
  uint32_t msg_until;
  uint32_t last_ms, acc, last_key, last_paint, last_save, last_min, frames, bar_ms;
  int bars, menu_dirty, asked;
  int sky[6];                           /* top and bottom of the background, r g b */
  uint32_t hash[NSTRIP];
} G;

/* ---- small helpers ------------------------------------------------------- */

static CRect rc(int x, int y, int w, int h) { return capp_rect(x, y, w, h); }

static uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* A panel colour from r g b. */
#define rgb(r, g, b) CAPP_RGB(r, g, b)

/* Mix colour `c` (panel order) towards r g b by a/256. */
static uint16_t mix(uint16_t c, int r, int g, int b, int a) {
  uint16_t v = swap16(c);
  int cr = (v >> 8) & 0xF8, cg = (v >> 3) & 0xFC, cb = (v << 3) & 0xF8;
  return rgb(cr + (r - cr) * a / 256, cg + (g - cg) * a / 256, cb + (b - cb) * a / 256);
}

static void say(const char *s) {
  api->fmt(G.msg, sizeof G.msg, "%s", s);
  G.msg_until = api->ticks_ms() + 4000;
  G.menu_dirty = 1;
}

static int slen(const char *s) { return (int)api->str_len(s); }

/* ---- drawing into the strip ---------------------------------------------- */

static int SY0, SY1;                    /* the rows the strip holds */

static void px(int x, int y, uint16_t c) {
  if ((unsigned)x < SW && y >= SY0 && y < SY1) SCR.strip[(y - SY0) * SW + x] = c;
}

static void fill(int x, int y, int w, int h, uint16_t c) {
  int yy, xx, x1 = x + w, y1 = y + h;
  if (x < 0) x = 0;
  if (x1 > SW) x1 = SW;
  if (y < SY0) y = SY0;
  if (y1 > SY1) y1 = SY1;
  for (yy = y; yy < y1; yy++) {
    uint16_t *r = SCR.strip + (yy - SY0) * SW;
    for (xx = x; xx < x1; xx++) r[xx] = c;
  }
}

static void frame(int x, int y, int w, int h, uint16_t c) {
  fill(x, y, w, 1, c);
  fill(x, y + h - 1, w, 1, c);
  fill(x, y, 1, h, c);
  fill(x + w - 1, y, 1, h, c);
}

/* Lighten towards r g b, a/256, within radius rr of (cx, cy), fading out. */
static void glow(int cx, int cy, int rr, int r, int g, int b, int a) {
  int y, x;
  for (y = cy - rr; y <= cy + rr; y++) {
    uint16_t *row;
    if (y < SY0 || y >= SY1) continue;
    row = SCR.strip + (y - SY0) * SW;
    for (x = cx - rr; x <= cx + rr; x++) {
      int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
      if ((unsigned)x >= SW || d > rr * rr) continue;
      row[x] = mix(row[x], r, g, b, a - a * d / (rr * rr + 1));
    }
  }
}

static void disc(int cx, int cy, int r, uint16_t c) {
  int y, x;
  for (y = -r; y <= r; y++)
    for (x = -r; x <= r; x++)
      if (x * x + y * y <= r * r + r) px(cx + x, cy + y, c);
}

/* A packed 3bpp picture, w x h, palette in panel colours or (plain) not. */
static void pic(const uint8_t *data, const uint16_t *pal, int plain, int w, int h,
                int x, int y, int flip, int scale) {
  int row, col, s;
  if (y + h * scale <= SY0 || y >= SY1) return;
  for (row = 0; row < h; row++) {
    int yy = y + row * scale;
    if (yy + scale <= SY0 || yy >= SY1) continue;
    for (col = 0; col < w; col++) {
      int v = ji_px(data, row * w + (flip ? w - 1 - col : col));
      uint16_t c;
      if (!v) continue;
      c = plain ? swap16(pal[v]) : pal[v];
      if (scale == 1) px(x + col, yy, c);
      else for (s = 0; s < scale; s++) { px(x + col * 2, yy + s, c); px(x + col * 2 + 1, yy + s, c); }
    }
  }
}

static void spr(int id, int f, int x, int y, int flip) {
  const JSprite *s = &SPRITES[id];
  pic(s->px + (f % s->nframes) * s->stride, s->pal, 0, s->w, s->h, x, y, flip, 1);
}

/* 6x8 text, no background. */
static void text(int x, int y, const char *s, uint16_t c) {
  int r0 = SY0 - y, r1 = SY1 - y, col, r;
  if (r1 <= 0 || r0 >= 8) return;
  if (r0 < 0) r0 = 0;
  if (r1 > 8) r1 = 8;
  for (; *s; s++, x += 6) {
    const uint8_t *g;
    if ((unsigned char)*s < FONT_FIRST || (unsigned char)*s > FONT_LAST) continue;
    g = font6x8[(unsigned char)*s - FONT_FIRST];
    for (col = 0; col < 5; col++)
      for (r = r0; r < r1; r++)
        if (g[col] >> r & 1) px(x + col, y + r, c);
  }
}

/* Text cut to `cols` characters. */
static void textn(int x, int y, const char *s, int cols, uint16_t c) {
  char b[41];
  int i;
  for (i = 0; s[i] && i < cols && i < 40; i++) b[i] = s[i];
  b[i] = 0;
  text(x, y, b, c);
}

/* Text wrapped at spaces into `cols` columns, at most `lines` lines. */
static void wrapped(int x, int y, const char *s, int cols, int lines, uint16_t c) {
  while (*s && lines-- > 0) {
    int n = slen(s), cut = n;
    if (n > cols) {
      for (cut = cols; cut > 0 && s[cut] != ' '; cut--) {}
      if (cut == 0) cut = cols;
    }
    textn(x, y, s, cut, c);
    s += cut;
    while (*s == ' ') s++;
    y += 9;
  }
}

/* A speech bubble pointing down at (cx, by). */
static void bubble(int cx, int by, const char *s) {
  int w = slen(s) * 6 + 5, x = cx - w / 2, y = by - 13;
  if (x < 3) x = 3;
  if (x + w > SW - 3) x = SW - 3 - w;
  if (y < 1) y = 1;
  fill(x + 1, y, w - 2, 11, C_TEXT);
  fill(x, y + 1, w, 9, C_TEXT);
  px(cx, y + 11, C_TEXT);
  px(cx - 1, y + 11, C_TEXT);
  px(cx, y + 12, C_TEXT);
  text(x + 3, y + 2, s, C_BG);
}

/* ---- the scene ----------------------------------------------------------- */

/* The back of the jar, by the hour: keyframes (minute, top rgb, bottom rgb). */
static const int16_t SKY[][7] = {
  {    0, 0x0b, 0x10, 0x20, 0x16, 0x24, 0x36 },
  {  300, 0x0b, 0x10, 0x20, 0x16, 0x24, 0x36 },
  {  390, 0x3a, 0x2a, 0x4a, 0x6a, 0x4a, 0x5a },
  {  540, 0x1f, 0x33, 0x40, 0x34, 0x58, 0x60 },
  {  990, 0x1f, 0x33, 0x40, 0x34, 0x58, 0x60 },
  { 1110, 0x2a, 0x1f, 0x3a, 0x6a, 0x3a, 0x3a },
  { 1230, 0x0b, 0x10, 0x20, 0x16, 0x24, 0x36 },
  { 1440, 0x0b, 0x10, 0x20, 0x16, 0x24, 0x36 },
};

static void set_sky(int minute) {
  int k = 0, i;
  if (minute < 0) minute = 700;          /* no clock: always day */
  while (k < 6 && SKY[k + 1][0] <= minute) k++;
  for (i = 0; i < 6; i++) {
    int a = SKY[k][i + 1], b = SKY[k + 1][i + 1];
    int span = SKY[k + 1][0] - SKY[k][0];
    G.sky[i] = a + (b - a) * (minute - SKY[k][0]) / (span ? span : 1);
  }
}

static uint32_t hash2(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

static void background(void) {
  int y, x;
  for (y = SY0; y < SY1; y++) {
    uint16_t *row = SCR.strip + (y - SY0) * SW, c;
    if (y < 9) {                                       /* the lid */
      c = y == 8 ? rgb(0x4a, 0x40, 0x30) : (y % 3 == 1) ? rgb(0xd8, 0xc0, 0x80) : rgb(0xa8, 0x90, 0x60);
      for (x = 0; x < SW; x++) row[x] = c;
      continue;
    }
    if (y >= JS_SOIL) {                                /* soil */
      int d = y - JS_SOIL;
      c = d == 0 ? rgb(0x3a, 0x5a, 0x2e) : d < 3 ? rgb(0x4a, 0x34, 0x24) : rgb(0x30, 0x22, 0x18);
      for (x = 0; x < SW; x++) {
        uint32_t h = hash2(x, y);
        row[x] = (d > 0 && h % 19 == 0) ? rgb(0x6a, 0x54, 0x40) : (d > 1 && h % 23 == 1) ? rgb(0x1e, 0x16, 0x10) : c;
      }
    } else {
      int f = (y - 9) * 256 / (JS_SOIL - 9);
      c = rgb(G.sky[0] + (G.sky[3] - G.sky[0]) * f / 256, G.sky[1] + (G.sky[4] - G.sky[1]) * f / 256,
              G.sky[2] + (G.sky[5] - G.sky[2]) * f / 256);
      for (x = 0; x < SW; x++) row[x] = c;
      if (y < 13) for (x = 0; x < SW; x++) row[x] = mix(row[x], 0, 0, 0, 120);   /* the neck */
      /* light on the glass: a soft streak, and the far wall's edge */
      for (x = 14 + y / 3; x < 18 + y / 3; x++) row[x] = mix(row[x], 255, 255, 255, 18);
      row[9] = mix(row[9], 255, 255, 255, 24);
      /* grass at the back, darker than what is in front */
      if (y > JS_SOIL - 13)
        for (x = 3; x < SW - 3; x++) {
          uint32_t h = hash2(x, 7);
          if ((h & 3) == 0 && y > JS_SOIL - 4 - (int)((h >> 8) % 9))
            row[x] = mix(row[x], 0x18, 0x30, 0x20, 110);
        }
    }
    /* the glass walls, and the shoulders up to the neck: outside is dark */
    {
      int in = y < 9 ? 14 : y < 26 ? (26 - y) * (26 - y) * 14 / 289 : 0;
      for (x = 0; x < in; x++) row[x] = row[SW - 1 - x] = C_BG;
      row[in] = row[SW - 1 - in] = rgb(0x0e, 0x1a, 0x20);
      if (y >= 9) {
        row[in + 1] = row[SW - 2 - in] = rgb(0x6a, 0x8a, 0x94);
        row[in + 2] = row[SW - 3 - in] = rgb(0x2a, 0x40, 0x48);
      }
    }
  }
}

static const int PLANT[5] = { SPR_BUSH, SPR_FERN, SPR_SHROOM, SPR_FLOWER, SPR_CACTUS };

static void garden(void) {
  int i;
  for (i = 0; i < J.nbeds; i++) {
    int bx = JS_BED_X(i);
    fill(bx - 5, JS_SOIL - 1, 11, 2, rgb(0x5a, 0x3e, 0x2a));
    fill(bx - 4, JS_SOIL - 2, 9, 1, rgb(0x6a, 0x4a, 0x32));
    spr(PLANT[J.bed[i].type % 5], (int)((J.steps / 48 + (uint32_t)i) & 1), bx - 6, JS_SOIL - 13, 0);
    if (J.bed[i].ready) spr(SPR_BERRY, 0, bx - 2, JS_SOIL - 15 + (int)((J.steps / 20 + (uint32_t)i) & 1), 0);
    else if (J.bed[i].grow > JS_GROW_MS * 2 / 3) px(bx, JS_SOIL - 13, rgb(0xd2, 0x80, 0x90));
  }
  /* the pond: a bottle cap of water */
  {
    int x0 = JS_POND_X - 8, y;
    for (y = JS_SOIL - 5; y < JS_SOIL; y++) fill(x0, y, 17, 1, y == JS_SOIL - 5 ? rgb(0xd0, 0xd4, 0xdc) : rgb(0x9a, 0xa0, 0xaa));
    fill(x0 + 1, JS_SOIL - 4, 15, 2, rgb(0x3a, 0x8a, 0xc8));
    px(x0 + 3 + (int)((J.steps / 12) % 10), JS_SOIL - 4, rgb(0xc8, 0xe8, 0xff));
    for (y = 0; y < 17; y += 3) px(x0 + y, JS_SOIL - 1, rgb(0x6a, 0x70, 0x7a));
  }
}

static void works(void) {
  int i, x, working;
  static const uint16_t CAP[4] = { CAPP_RGB(0x9a, 0xa0, 0xaa), CAPP_RGB(0xc8, 0x42, 0x3a),
                                   CAPP_RGB(0x4a, 0x7a, 0xc8), CAPP_RGB(0xff, 0xd1, 0x66) };
  uint16_t cap = CAP[J.belt], capd = mix(cap, 0, 0, 0, 90);
  int off = (int)(J.belt_pos / JS_FX) % 7;
  /* the belt: bottle caps on edge, turning */
  for (x = 112 - 7 + off; x < 166; x += 7) {
    if (x < 112) continue;
    fill(x, JS_SOIL - 4, 6, 1, mix(cap, 255, 255, 255, 80));
    fill(x, JS_SOIL - 3, 6, 2, cap);
    fill(x, JS_SOIL - 1, 6, 1, capd);
  }
  fill(112, JS_SOIL - 5, 54, 1, rgb(0x5a, 0x3e, 0x2a));    /* the twig rail */
  for (i = 0; i < JS_UNITS; i++)
    if (J.unit[i].to >= 0) spr(SPR_JAR, 0, J.unit[i].x / JS_FX - 3, JS_SOIL - 13, 0);
  if (J.jammed && (J.steps / 10) & 1) {
    fill(J.jam_x - 1, JS_SOIL - 17, 2, 6, C_WARN);
    fill(J.jam_x - 1, JS_SOIL - 9, 2, 2, C_WARN);
  }
  /* the vat, the press and the spool */
  working = J.mach[0].st == 1;
  spr(SPR_VAT, working ? (int)((J.steps / 8) & 1) : 0, JS_MACH_X[0] - 8, JS_SOIL - 16, 0);
  if (js_mach_active(&J, 1))
    spr(SPR_PRESS, J.mach[1].st == 1 ? (int)((J.steps / 12) & 1) : 0, JS_MACH_X[1] - 10, JS_SOIL - 14, 0);
  if (js_mach_active(&J, 2))
    spr(SPR_SPOOL, J.mach[2].st == 1 ? (int)((J.steps / 6) & 1) : 0, JS_MACH_X[2] - 6, JS_SOIL - 16, 0);
  /* the twig crane: a mast, an arm out over the dock, a hook */
  {
    uint16_t tw = rgb(0x8a, 0x5a, 0x32), tl = rgb(0xb0, 0x7c, 0x48);
    int top = JS_SOIL - 44, mx = JS_MACH_X[3], hx, hy, prog = 0;
    fill(mx - 1, top, 2, 44, tw);
    fill(mx - 1, top, 1, 44, tl);
    fill(mx - 4, JS_SOIL - 2, 8, 2, tw);
    fill(mx - 6, top, 30, 2, tw);
    fill(mx - 6, top, 30, 1, tl);
    if (J.mach[3].st == 1) {
      int stage = js_stage_steps(J.nmach, J.belt);
      prog = 256 - J.mach[3].t * 256 / (stage ? stage : 1);
    }
    hx = mx + 4 + (JS_PILE_X - mx - 4) * prog / 256;
    hy = JS_SOIL - 28 + (prog < 128 ? 6 - prog * 6 / 128 : 0);
    fill(hx, top + 2, 1, hy - top - 2, rgb(0xe8, 0xdc, 0xc0));
    fill(hx - 2, hy, 5, 1, rgb(0x9a, 0xa0, 0xaa));
    if (J.mach[3].st == 1) spr(SPR_JAR, 0, hx - 3, hy + 1, 0);
  }
}

static void dock(void) {
  int n = (int)(J.dock > 6 ? 6 : J.dock), i, open;
  static const int8_t PX[6] = { -11, -3, 5, -7, 1, -3 }, PY[6] = { 8, 8, 8, 16, 16, 24 };
  spr(SPR_CRATE, 0, 198, JS_SOIL - 11, 0);
  spr(SPR_CRATE, 0, 212, JS_SOIL - 11, 0);
  spr(SPR_CRATE, 0, 205, JS_SOIL - 22, 0);
  for (i = 0; i < n; i++) spr(SPR_JAR, 0, JS_PILE_X + PX[i] - 1, JS_SOIL - PY[i], 0);
  if (J.dock > 6) {
    char b[12];
    api->fmt(b, sizeof b, "x%u", (unsigned)J.dock);
    text(JS_PILE_X - slen(b) * 3, JS_SOIL - 34, b, C_GOLD);
  }
  /* the cork door, pulled up while the snail goes through */
  open = J.snail.x / JS_FX > JS_DOOR_X - 14;
  if (open) fill(JS_DOOR_X, JS_SOIL - 20, 8, 20, rgb(0x0b, 0x0f, 0x18));
  spr(SPR_CORK, 0, JS_DOOR_X, JS_SOIL - 22 - (open ? 16 : 0), 0);
}

/* A placed item: where it is drawn. */
static void item_at(int i, int *x, int *y) {
  const JPlaced *p = &J.placed[i];
  int bob = 0, ph = p->phase & 255;
  if (p->move == JM_FLOATS || p->move == JM_SWAYS) bob = (ph < 128 ? ph : 255 - ph) * 7 / 128 - 3;
  *x = p->x / JS_FX - 8;
  if (p->kind == JK_HANGING) { *y = JS_LID + p->home_y; *x += bob / 2; }
  else if (p->kind == JK_CRITTER) *y = JS_SOIL - 16 - p->home_y - p->yoff / 16 - (p->move == JM_FLOATS ? bob : 0);
  else *y = JS_SOIL - 16 - p->yoff / 16;
}

static void items(void) {
  int i, x, y;
  for (i = 0; i < J.nplaced; i++) {
    const JPlaced *p = &J.placed[i];
    int f = p->slot[p->cur % (p->nframes ? p->nframes : 1)];
    item_at(i, &x, &y);
    if (p->kind == JK_HANGING) fill(x + 7, 9, 1, y - 8, rgb(0xc8, 0xc0, 0xb0));
    if (p->glow) glow(x + 8, y + 8, 14, 255, 220, 140, 110);
    if (f < JS_POOL) pic(J.pool[f], p->pal, 1, 16, 16, x, y, p->flip, 1);
  }
}

static void workers(void) {
  int k;
  for (k = 0; k < J.nmoss; k++) {
    const JMoss *m = &J.moss[k];
    int x = m->x / JS_FX - 5, y = JS_SOIL - 9 - m->yoff;
    spr(SPR_MOSS, m->frame, x, y, m->face);
    if (m->carry) spr(SPR_BERRY, 0, x + 3, y - 4, 0);
  }
  if (J.snail.st != S_AWAY) {
    const JSnail *s = &J.snail;
    int x = s->x / JS_FX - 8, y = JS_SOIL - 12;
    spr(SPR_SNAIL, s->frame, x, y, s->face);
    if (s->load > 3) spr(SPR_PARCEL, 0, x + (s->face ? 6 : 1), y - 6, 0);
    else for (k = 0; k < (int)s->load; k++) spr(SPR_JAR, 0, x + (s->face ? 6 : 2) + k * 2 - 2, y - 7 - k * 3, 0);
  }
}

static void air(void) {
  int i;
  for (i = 0; i < JS_PARTS; i++) {
    const JPart *p = &J.part[i];
    int x = p->x / 16, y = p->y / 16;
    if (!p->life) continue;
    switch (p->type) {
    case JP_PUFF: disc(x, y, 1 + (50 - p->life) / 14, mix(rgb(0xe8, 0xec, 0xf0), 0x1f, 0x33, 0x40, (50 - p->life) * 3)); break;
    case JP_ZZZ: text(x, y, "z", C_TEXT); break;
    case JP_HEART: spr(SPR_HEART, 0, x - 2, y, 0); break;
    case JP_NOTE: spr(SPR_NOTE, 0, x - 2, y, 0); break;
    default: spr(SPR_SPARK, 0, x - 2, y, 0); break;
    }
  }
  if (J.phase == PH_NIGHT || J.phase == PH_DUSK)
    for (i = 0; i < JS_FLIES; i++) {
      int x = J.fly_x[i] / 16, y = J.fly_y[i] / 16;
      if ((J.steps + (uint32_t)i * 37) % 90 > 60) continue;
      glow(x, y, 3, 200, 255, 120, 120);
      px(x, y, rgb(0xe8, 0xff, 0x90));
    }
}

static void talk(void) {
  int i;
  for (i = 0; i < J.nmoss; i++)
    if (J.moss[i].say_t > 0 && J.moss[i].say >= 0)
      bubble(J.moss[i].x / JS_FX, JS_SOIL - 10 - J.moss[i].yoff, JS_SAYINGS[(int)J.moss[i].say]);
  if (J.snail.say_t > 0 && J.snail.say >= 0 && J.snail.st != S_AWAY)
    bubble(J.snail.x / JS_FX + 4, JS_SOIL - 13, JS_SAYINGS[(int)J.snail.say]);
  for (i = 0; i < J.nplaced; i++) {
    const JPlaced *p = &J.placed[i];
    int x, y;
    if (p->say_t <= 0 || p->say < 0) continue;
    item_at(i, &x, &y);
    bubble(x + 8, y, p->bub[(int)p->say]);
  }
  for (i = 0; i < JS_FLOATS; i++)
    if (J.fl[i].life) {
      char b[12];
      api->fmt(b, sizeof b, "+%u", (unsigned)J.fl[i].amount);
      text(J.fl[i].x - slen(b) * 6 + 1, J.fl[i].y + 1, b, C_BG);
      text(J.fl[i].x - slen(b) * 6, J.fl[i].y, b, C_GOLD);
    }
}

static void scene(void) {
  background();
  garden();
  works();
  dock();
  items();
  workers();
  air();
  talk();
}

/* ---- the bars ------------------------------------------------------------ */

/* Key hints: a light key cap, then a word (spec, "Screens and look"). */
static void hints(int y, const char *const *h) {
  int x = 4;
  for (; *h; h += 2) {
    int w = slen(h[0]) * 6 + 3;
    fill(x, y + 2, w, 9, C_TEXT);
    text(x + 2, y + 3, h[0], C_BG);
    x += w + 3;
    text(x, y + 3, h[1], C_DIM);
    x += slen(h[1]) * 6 + 7;
  }
}

static void rate_text(char *b, int n) {
  int32_t r = js_rate_ph(&J) * 10 / 60;
  api->fmt(b, (size_t)n, "%d.%d/min", (int)(r / 10), (int)(r % 10));
}

static void bars(void) {
  static const char *const H_JAR[] = { "S", "shop", "D", "decorate", "U", "upgrades", 0 };
  static const char *const H_DSEL[] = { "<>", "pick", "Ent", "move", "N", "add", "X", "out", "Esc", "done", 0 };
  static const char *const H_DMOVE[] = { "<>", "move", "^v", "string", "Ent", "put", "Esc", "cancel", 0 };
  static const char *const H_DMOVE2[] = { "<>", "move", "Ent", "put", "Esc", "cancel", 0 };
  static const char *const H_UP[] = { "^v", "pick", "Ent", "buy", "Esc", "back", 0 };
  static const char *const H_STOCK[] = { "<>^v", "pick", "Ent", "buy", "Tab", "my stuff", "Esc", "back", 0 };
  static const char *const H_STUFF[] = { "<>^v", "pick", "Ent", "open", "Tab", "stock", "Esc", "back", 0 };
  static const char *const H_PICK[] = { "<>^v", "pick", "Ent", "place", "Esc", "back", 0 };
  static const char *const H_CARD[] = { "Ent", "place in jar", "Esc", "keep", 0 };
  static const char *const H_CARD2[] = { "Ent", "find in jar", "X", "take out", "Esc", "back", 0 };
  const char *const *h = H_JAR;
  const char *where = "The Jar";
  char b[24];
  int ty = -G.bars, by = SHT - BAR + G.bars;
  if (ty + BAR > SY0 && ty < SY1) {
    fill(0, ty, SW, BAR, C_BG);
    spr(SPR_COIN, 0, 3, ty + 3, 0);
    api->fmt(b, sizeof b, "%u", (unsigned)J.coins);
    text(13, ty + 3, b, C_GOLD);
    if (G.view == V_JAR || G.view == V_UP) {
      int x = 13 + slen(b) * 6 + 6;
      rate_text(b, sizeof b);
      text(x, ty + 3, b, C_DIM);
    }
    switch (G.view) {
    case V_DECOR: api->fmt(b, sizeof b, "Decorate %u/%d", (unsigned)J.nplaced, JS_MAX_PLACED); where = b; break;
    case V_UP:    where = "Upgrades"; break;
    case V_SHOP:  where = G.pick ? "Add to the jar" : G.tab ? "My Stuff" : "Shop"; break;
    case V_CARD:  where = "Item"; break;
    }
    text(SW - 4 - slen(where) * 6, ty + 3, where, C_DIM);
  }
  if (by + BAR > SY0 && by < SY1) {
    switch (G.view) {
    case V_DECOR:
      h = !G.dmove ? H_DSEL : J.placed[G.dsel].kind == JK_HANGING ? H_DMOVE : H_DMOVE2;
      break;
    case V_UP:   h = H_UP; break;
    case V_SHOP: h = G.pick ? H_PICK : G.tab ? H_STUFF : H_STOCK; break;
    case V_CARD: h = G.card.placed ? H_CARD2 : H_CARD; break;
    }
    fill(0, by, SW, BAR, C_BG);
    hints(by, h);
  }
}

/* A note across the top of the body: "not enough coins", away earnings. */
static void note(void) {
  int w;
  if (!G.msg[0] || (int32_t)(api->ticks_ms() - G.msg_until) > 0) return;
  w = slen(G.msg) * 6 + 8;
  fill((SW - w) / 2, 16, w, 12, C_PANEL);
  frame((SW - w) / 2, 16, w, 12, C_GOLD);
  text((SW - w) / 2 + 4, 18, G.msg, C_TEXT);
}

/* ---- decorate ------------------------------------------------------------ */

static void decor_overlay(void) {
  int x, y;
  fill(0, BAR, SW, 11, C_PANEL);
  if (!J.nplaced) {
    text(4, BAR + 2, "Nothing in the jar. N adds an item.", C_DIM);
    return;
  }
  item_at(G.dsel, &x, &y);
  frame(x - 1, y - 1, 18, 18, C_GOLD);
  text(4, BAR + 2, G.dname, C_TEXT);
  textn(4 + (slen(G.dname) + 1) * 6, BAR + 2, G.dline, 38 - slen(G.dname), C_DIM);
}

/* ---- menus --------------------------------------------------------------- */

static const char *const UP_NAME[JU_KINDS] = { "Another mossling", "Another machine", "Faster belt", "Another bed" };
static const char *const KIND_NAME[JK_KINDS] = { "floor decor", "hanging decor", "critter" };
static const char *const MOVE_NAME[JM_KINDS] = { "sits", "hops", "wanders", "sways", "floats" };

static void price(int x, int y, int cost, int warn) {
  char b[12];
  spr(SPR_COIN, 0, x, y, 0);
  api->fmt(b, sizeof b, "%d", cost);
  text(x + 9, y, b, warn ? C_WARN : C_GOLD);
}

static void upgrades(void) {
  int k;
  char b[40], r1[16], r2[16];
  fill(0, BAR, SW, SHT - 2 * BAR, C_BG);
  for (k = 0; k < JU_KINDS; k++) {
    int y = BAR + 3 + k * 23, cost = js_up_cost(&J, k), lv = js_up_level(&J, k);
    fill(4, y, SW - 8, 21, C_PANEL);
    if (k == G.up_sel) frame(4, y, SW - 8, 21, C_GOLD);
    switch (k) {
    case JU_MOSS: spr(SPR_MOSS, 0, 9, y + 6, 0); break;
    case JU_MACH: spr(SPR_SPOOL, 0, 8, y + 3, 0); break;
    case JU_BELT: fill(7, y + 9, 6, 3, rgb(0xc8, 0x42, 0x3a)); fill(14, y + 9, 6, 3, rgb(0x4a, 0x7a, 0xc8)); break;
    default:      spr(SPR_BUSH, 0, 8, y + 4, 0); break;
    }
    text(26, y + 3, UP_NAME[k], C_TEXT);
    if (cost < 0) {
      api->fmt(b, sizeof b, "all %d", js_up_max(k) + (k == JU_BELT));
      text(26, y + 12, b, C_DIM);
      text(SW - 34, y + 7, "max", C_DIM);
      continue;
    }
    {
      int32_t r = js_rate_ph(&J) * 10 / 60, n = js_rate_after(&J, k) * 10 / 60;
      api->fmt(r1, sizeof r1, "%d.%d", (int)(r / 10), (int)(r % 10));
      api->fmt(r2, sizeof r2, "%d.%d", (int)(n / 10), (int)(n % 10));
    }
    if (k == JU_MACH) api->fmt(b, sizeof b, "%s  %s->%s/min", lv == 2 ? "press" : "spool", r1, r2);
    else api->fmt(b, sizeof b, "%d of %d  %s->%s/min", lv + (k == JU_BELT), js_up_max(k) + (k == JU_BELT), r1, r2);
    text(26, y + 12, b, C_DIM);
    price(SW - 46, y + 7, cost, (uint32_t)cost > J.coins);
  }
}

/* The detail panel, right of a shop grid. */
static void detail(void) {
  const Tile *t = &G.card;
  int x = 128;
  char b[32];
  fill(x - 2, BAR + 2, SW - x, SHT - 2 * BAR - 4, C_PANEL);
  if (!t->ok) {
    wrapped(x + 2, BAR + 8, G.tab || G.pick ? "Nothing here yet. The shop has more." : "Sold. More with the next batch.", 17, 4, C_DIM);
    return;
  }
  pic(t->frame, t->pal, 1, 16, 16, x + 36, BAR + 5, 0, 2);
  textn(x + 2, BAR + 41, G.w.name, 17, C_TEXT);
  wrapped(x + 2, BAR + 51, G.w.line, 17, 2, C_DIM);
  api->fmt(b, sizeof b, "%s, %s", t->kind == JK_CRITTER ? "critter" : t->kind == JK_HANGING ? "hanging" : "floor",
           MOVE_NAME[t->move % JM_KINDS]);
  textn(x + 2, BAR + 71, b, 17, C_TRAIT);
  if (G.tab || G.pick) text(x + 2, BAR + 84, t->placed ? "in the jar" : "in My Stuff", t->placed ? C_GOLD : C_DIM);
  else price(x + 2, BAR + 84, t->price, t->price > J.coins);
}

static void grid(void) {
  int i, rows = 2;
  char b[32];
  fill(0, BAR, SW, SHT - 2 * BAR, C_BG);
  if (G.tab || G.pick) api->fmt(b, sizeof b, "%u things", (unsigned)J.nowned);
  else api->fmt(b, sizeof b, "No new stock offline");
  text(4, BAR + 3, b, C_DIM);
  for (i = 0; i < rows * 4; i++) {
    const Tile *t = &G.tile[i];
    int x = 4 + (i % 4) * 30, y = BAR + 13 + (i / 4) * 30;
    fill(x, y, 28, 28, C_PANEL);
    if (i < G.ntile && t->ok) {
      pic(t->frame, t->pal, 1, 16, 16, x + 6, y + 2, 0, 1);
      if (G.tab || G.pick) { if (t->placed) fill(x + 23, y + 2, 3, 3, C_GOLD); }
      else {
        api->fmt(b, sizeof b, "%u", (unsigned)t->price);
        text(x + 14 - slen(b) * 3, y + 19, b, t->price > J.coins ? C_WARN : C_GOLD);
      }
    }
    if (i == G.sel - G.top * 4) frame(x - 1, y - 1, 30, 30, C_GOLD);
  }
  if (!G.tab && !G.pick) {
    fill(4, BAR + 76, 118, 30, C_PANEL);
    text(8, BAR + 79, "Today", C_TRAIT);
    wrapped(8, BAR + 88, "hand-made: cosy, garden, glow", 18, 2, C_DIM);
  } else if (J.nowned > 8) {
    api->fmt(b, sizeof b, "%d/%d", G.top + 1, (J.nowned + 3) / 4 - 1);
    text(122 - slen(b) * 6, BAR + 3, b, C_DIM);
  }
  detail();
}

static void card(void) {
  const Tile *t = &G.card;
  char b[40];
  fill(0, BAR, SW, SHT - 2 * BAR, C_BG);
  fill(4, BAR + 3, SW - 8, SHT - 2 * BAR - 6, C_PANEL);
  pic(t->frame, t->pal, 1, 16, 16, 12, BAR + 10, 0, 2);
  jitem_number(t->id, b);
  text(SW - 9 - slen(b) * 6, BAR + 9, b, C_DIM);
  text(54, BAR + 9, G.w.name, C_TEXT);
  wrapped(54, BAR + 21, G.w.line, 30, 2, C_DIM);
  api->fmt(b, sizeof b, "%s, %s", KIND_NAME[t->kind % JK_KINDS], MOVE_NAME[t->move % JM_KINDS]);
  text(54, BAR + 42, b, C_TRAIT);
  if (G.w.tags[0]) { api->fmt(b, sizeof b, "tags: %s", G.w.tags); textn(54, BAR + 52, b, 30, C_TRAIT); }
  {
    static const char *const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    int y = 0, m = 1, d = 1;
    if (G.w.made) dt_civil_from_days((int32_t)(G.w.made / 86400u), &y, &m, &d);
    if (G.w.made) api->fmt(b, sizeof b, "made by %s, %d %s %d", G.w.maker[0] ? G.w.maker : "someone", d, MON[(m - 1) % 12], y);
    else api->fmt(b, sizeof b, "made by %s", G.w.maker[0] ? G.w.maker : "someone");
    textn(12, BAR + 66, b, 37, C_DIM);
  }
  if (G.w.gifted[0]) { api->fmt(b, sizeof b, "a gift from %s", G.w.gifted); text(12, BAR + 76, b, C_PINK); }
  else text(12, BAR + 76, (t->flags & JIF_BUILTIN) ? "hand-made, built in" : "from the shop", C_DIM);
  text(12, BAR + 88, t->placed ? "In your jar." : "In My Stuff.", t->placed ? C_GOLD : C_TEXT);
}

/* ---- painting ------------------------------------------------------------ */

static void render(void) {
  if (G.view == V_JAR || G.view == V_DECOR) {
    scene();
    if (G.view == V_DECOR) decor_overlay();
  } else if (G.view == V_UP) upgrades();
  else if (G.view == V_SHOP) grid();
  else card();
  bars();
  note();
}

static uint32_t strip_hash(int n) {
  uint32_t h = 2166136261u;
  int i;
  for (i = 0; i < n; i++) h = (h ^ SCR.strip[i]) * 16777619u;
  return h;
}

static void app_paint(void *st, CRect c) {
  int k, full = !G.asked || (G.frames % 30) == 0;
  (void)st;
  G.asked = 0;
  G.frames++;
  for (k = 0; k < NSTRIP; k++) {
    uint32_t h;
    SY0 = k * SH;
    SY1 = SY0 + SH > SHT ? SHT : SY0 + SH;
    render();
    h = strip_hash((SY1 - SY0) * SW);
    if (!full && h == G.hash[k]) continue;
    G.hash[k] = h;
    api->pixels(rc(c.x, c.y + SY0, SW, SY1 - SY0), SCR.strip);
  }
}

/* ---- items on the card ------------------------------------------------------- */

static void item_path(uint32_t id, char *p, int n) { api->fmt(p, (size_t)n, DIR_ITEMS "/%u.itm", (unsigned)id); }

/* A built-in, as the record it would be. */
static void from_builtin(const JBuiltin *b, JItem *it) {
  const char *s = b->text;
  static const uint8_t LIM[4] = { JI_NAME, JI_LINE, JI_TAGS, JI_WHO };
  char *fld[4];
  int i, k;
  ji_zero(it, (int)sizeof *it);
  it->version = JI_VERSION;
  it->id = b->id;
  it->kind = b->kind;
  it->nframes = b->nframes;
  it->flags = JIF_BUILTIN;
  it->move = b->move;
  it->speed = b->speed;
  it->zone = b->zone;
  it->nhab = b->nhab;
  it->nbub = b->nbub;
  it->made = JB_MADE;
  for (i = 0; i < 3; i++) {
    it->hab[i].event = b->hab[i][0];
    it->hab[i].earg = b->hab[i][1];
    it->hab[i].action = b->hab[i][2];
    it->hab[i].aarg = b->hab[i][3];
  }
  for (i = 0; i < 8; i++) it->pal[i] = b->pal[i];
  fld[0] = it->name; fld[1] = it->line; fld[2] = it->tags; fld[3] = it->maker;
  for (i = 0; i < 4 + b->nbub; i++) {
    char *d = i < 4 ? fld[i] : it->bub[i - 4];
    int n = i < 4 ? LIM[i] : JI_BUB;
    for (k = 0; s[k] && k < n; k++) d[k] = s[k];
    d[k] = 0;
    s += slen(s) + 1;
  }
  for (i = 0; i < b->nframes; i++) ji_copy(it->frames[i], b->frames + i * JI_FRAME_BYTES, JI_FRAME_BYTES);
}

static int item_write(const JItem *it) {
  SafeFile f;
  char p[48];
  int n = jitem_encode(it, SCR.io.raw, JI_MAX);
  if (n < 0) return -1;
  item_path(it->id, p, sizeof p);
  if (safe_begin(&f, api, p)) return -1;
  safe_write(&f, (const char *)SCR.io.raw, (size_t)n);
  return safe_commit(&f);
}

/* Item `id` from the card into SCR.io.it. 0, or -1. */
static int item_read(uint32_t id) {
  char p[48];
  int fd, n;
  item_path(id, p, sizeof p);
  fd = safe_open_read(api, p);
  if (fd < 0) return -1;
  n = api->read(fd, SCR.io.raw, JI_MAX);
  api->close(fd);
  if (n <= 0) return -1;
  return jitem_decode(&SCR.io.it, SCR.io.raw, n);
}

static void tile_from(Tile *t, const JItem *it) {
  int i;
  t->id = it->id;
  t->kind = it->kind;
  t->move = it->move;
  t->flags = it->flags;
  for (i = 0; i < 8; i++) t->pal[i] = it->pal[i];
  ji_copy(t->frame, it->frames[0], JI_FRAME_BYTES);
  t->placed = (uint8_t)(js_find(&J, it->id) >= 0);
  t->ok = 1;
}

static void words_from(const JItem *it) {
  ji_copy(G.w.name, it->name, sizeof G.w.name);
  ji_copy(G.w.line, it->line, sizeof G.w.line);
  ji_copy(G.w.maker, it->maker, sizeof G.w.maker);
  ji_copy(G.w.tags, it->tags, sizeof G.w.tags);
  ji_copy(G.w.gifted, it->gifted, sizeof G.w.gifted);
  G.w.made = it->made;
}

static void save(void) {
  SafeFile f;
  int n;
  uint32_t now = api->epoch();
  if (now) J.seen = now;
  n = js_save(&J, SCR.text, sizeof SCR.text);
  if (safe_begin(&f, api, SAVE_PATH) == 0) {
    safe_write(&f, SCR.text, (size_t)n);
    safe_commit(&f);
  }
  G.last_save = api->ticks_ms();
}

/* ---- the shop's pages ------------------------------------------------------ */

static int stock_n(void) {
  int i, n = 0;
  for (i = 0; i < JB_COUNT; i++) if (BUILTINS[i].price) n++;
  return n;
}

static const JBuiltin *stock_at(int k) {
  int i;
  for (i = 0; i < JB_COUNT; i++) if (BUILTINS[i].price && k-- == 0) return &BUILTINS[i];
  return 0;
}

/* Load what the grid shows, and the selected one's words. Card I/O: from
 * key handlers, never from paint. */
static void load_page(void) {
  int i, n;
  for (i = 0; i < 8; i++) G.tile[i].ok = 0;
  if (!G.tab && !G.pick) {
    n = stock_n();
    for (i = 0; i < n && i < 8; i++) {
      const JBuiltin *b = stock_at(i);
      from_builtin(b, &SCR.io.it);
      tile_from(&G.tile[i], &SCR.io.it);
      G.tile[i].price = b->price;
      G.tile[i].ok = !(J.sold & (1u << b->id));
    }
    G.ntile = n;
  } else {
    G.ntile = 0;
    for (i = 0; i < 8 && G.top * 4 + i < J.nowned; i++) {
      if (item_read(J.owned[G.top * 4 + i]) == 0) tile_from(&G.tile[i], &SCR.io.it);
      G.ntile = i + 1;
    }
  }
  G.card.ok = 0;
  i = G.sel - G.top * 4;
  if (i >= 0 && i < G.ntile && G.tile[i].ok) {
    ji_copy(&G.card, &G.tile[i], (int)sizeof G.card);
    if (!G.tab && !G.pick) from_builtin(stock_at(G.sel), &SCR.io.it);
    else item_read(G.tile[i].id);
    words_from(&SCR.io.it);
  }
  G.menu_dirty = 1;
}

static void open_shop(int tab, int pick) {
  G.view = V_SHOP;
  G.tab = tab;
  G.pick = pick;
  G.sel = 0;
  G.top = 0;
  load_page();
}

/* The item card for `id`, which is owned. */
static void open_card(uint32_t id, int from) {
  if (item_read(id) != 0) { say("That item is missing"); return; }
  tile_from(&G.card, &SCR.io.it);
  words_from(&SCR.io.it);
  G.card_from = from;
  G.view = V_CARD;
  G.menu_dirty = 1;
}

/* ---- decorate ------------------------------------------------------------- */

static void decor_select(int i) {
  if (!J.nplaced) return;
  G.dsel = (i + J.nplaced) % J.nplaced;
  G.dline[0] = G.dname[0] = 0;
  if (item_read(J.placed[G.dsel].id) == 0) {
    ji_copy(G.dline, SCR.io.it.line, sizeof G.dline);
    ji_copy(G.dname, SCR.io.it.name, sizeof G.dname);
  }
  js_item_event(&J, G.dsel, JE_POKE, 0);        /* the player picked it */
}

/* Put item `id` in the jar and start moving it. */
static void place_new(uint32_t id) {
  int i;
  if (J.nplaced >= JS_MAX_PLACED) { say("The jar is full: 24 things"); return; }
  if (item_read(id) != 0) { say("That item is missing"); return; }
  i = js_place(&J, &SCR.io.it, 120, 24);
  if (i < 0) { say("No room for its pictures"); return; }
  G.view = V_DECOR;
  G.dsel = i;
  ji_copy(G.dline, SCR.io.it.line, sizeof G.dline);
  ji_copy(G.dname, SCR.io.it.name, sizeof G.dname);
  G.dmove = 1;
  G.dnew = 1;
  G.dx0 = J.placed[i].home_x;
  G.dy0 = J.placed[i].home_y;
}

static int key_decor(int k) {
  JPlaced *p = J.nplaced ? &J.placed[G.dsel] : 0;
  if (G.dmove && p) {
    int x = p->home_x, y = p->home_y, step = api->key_repeat && api->key_repeat() ? 3 : 1;
    switch (k) {
    case CAPP_KEY_LEFT:  js_move_to(&J, G.dsel, x - step, y); return 1;
    case CAPP_KEY_RIGHT: js_move_to(&J, G.dsel, x + step, y); return 1;
    case CAPP_KEY_UP:    if (p->kind == JK_HANGING) js_move_to(&J, G.dsel, x, y - 2); return 1;
    case CAPP_KEY_DOWN:  if (p->kind == JK_HANGING) js_move_to(&J, G.dsel, x, y + 2); return 1;
    case CAPP_KEY_ENTER: G.dmove = 0; G.dnew = 0; save(); return 1;
    case CAPP_KEY_ESC:
      if (G.dnew) { js_unplace(&J, G.dsel); G.dsel = 0; decor_select(0); }
      else js_move_to(&J, G.dsel, G.dx0, G.dy0);
      G.dmove = 0;
      G.dnew = 0;
      return 1;
    }
    return 1;
  }
  switch (k) {
  case CAPP_KEY_LEFT: case CAPP_KEY_UP:    decor_select(G.dsel - 1); return 1;
  case CAPP_KEY_RIGHT: case CAPP_KEY_DOWN: decor_select(G.dsel + 1); return 1;
  case CAPP_KEY_ENTER:
    if (!p) return 1;
    G.dmove = 1;
    G.dnew = 0;
    G.dx0 = p->home_x;
    G.dy0 = p->home_y;
    return 1;
  case 'n': case 'N': open_shop(1, 1); return 1;
  case 'x': case 'X':
    if (!p) return 1;
    say("Put away in My Stuff");
    js_unplace(&J, G.dsel);
    save();
    decor_select(G.dsel);
    return 1;
  case CAPP_KEY_ESC: G.view = V_JAR; return 1;
  }
  return 1;
}

/* ---- keys ------------------------------------------------------------------- */

static void shop_move(int d) {
  int n = (G.tab || G.pick) ? J.nowned : stock_n(), rows = 2;
  int s = G.sel + d;
  if (s < 0 || s >= n) return;
  G.sel = s;
  while (G.sel < G.top * 4) G.top--;
  while (G.sel >= (G.top + rows) * 4) G.top++;
  load_page();
}

static int key_shop(int k) {
  switch (k) {
  case CAPP_KEY_LEFT:  shop_move(-1); return 1;
  case CAPP_KEY_RIGHT: shop_move(1); return 1;
  case CAPP_KEY_UP:    shop_move(-4); return 1;
  case CAPP_KEY_DOWN:  shop_move(4); return 1;
  case 0x09:
    if (!G.pick) open_shop(!G.tab, 0);
    return 1;
  case CAPP_KEY_ESC:
    if (G.pick) { G.view = V_DECOR; G.pick = 0; } else G.view = V_JAR;
    return 1;
  case CAPP_KEY_ENTER:
    if (!G.card.ok) return 1;
    if (G.pick) {
      G.pick = 0;
      if (G.card.placed) { G.view = V_DECOR; decor_select(js_find(&J, G.card.id)); }
      else place_new(G.card.id);
      return 1;
    }
    if (G.tab) { open_card(G.card.id, V_SHOP); return 1; }
    {
      const JBuiltin *b = stock_at(G.sel);
      if (!b || (J.sold & (1u << b->id))) return 1;
      if (J.coins < b->price) return 1;              /* the price is in red already */
      if (js_own(&J, b->id)) { say("My Stuff is full"); return 1; }
      from_builtin(b, &SCR.io.it);
      if (item_write(&SCR.io.it)) { J.nowned--; say("Could not write to the card"); return 1; }
      js_spend(&J, b->price);
      J.sold |= 1u << b->id;
      save();
      load_page();
      open_card(b->id, V_SHOP);
    }
    return 1;
  }
  return 1;
}

static int key_card(int k) {
  switch (k) {
  case CAPP_KEY_ENTER:
    if (G.card.placed) { G.view = V_DECOR; G.dmove = 0; decor_select(js_find(&J, G.card.id)); }
    else place_new(G.card.id);
    return 1;
  case 'x': case 'X':
    if (G.card.placed) {
      js_unplace(&J, js_find(&J, G.card.id));
      save();
      G.card.placed = 0;
      say("Put away in My Stuff");
    }
    return 1;
  case CAPP_KEY_ESC:
    G.view = G.card_from;
    if (G.view == V_SHOP) load_page();
    return 1;
  }
  return 1;
}

static int key_up(int k) {
  switch (k) {
  case CAPP_KEY_UP:   if (G.up_sel > 0) G.up_sel--; return 1;
  case CAPP_KEY_DOWN: if (G.up_sel < JU_KINDS - 1) G.up_sel++; return 1;
  case CAPP_KEY_ENTER: {
    int r = js_buy(&J, G.up_sel);
    if (r == 0) { save(); say("Bought: look in the jar"); }
    else if (r == -2) say("Not enough coins yet");
    return 1;
  }
  case CAPP_KEY_ESC: G.view = V_JAR; return 1;
  }
  return 1;
}

static int app_key(void *st, unsigned char k) {
  uint32_t now = api->ticks_ms();
  (void)st;
  G.menu_dirty = 1;
  /* Bars hidden: a key brings them back and does nothing else. */
  if (G.view == V_JAR && G.bars > 0) { G.last_key = now; return 1; }
  G.last_key = now;
  switch (G.view) {
  case V_DECOR: return key_decor(k);
  case V_UP:    return key_up(k);
  case V_SHOP:  return key_shop(k);
  case V_CARD:  return key_card(k);
  }
  switch (k) {
  case 's': case 'S': open_shop(0, 0); return 1;
  case 'd': case 'D': G.view = V_DECOR; G.dmove = 0; decor_select(G.dsel); return 1;
  case 'u': case 'U': G.view = V_UP; return 1;
  }
  return 0;
}

/* ---- time -------------------------------------------------------------------- */

static void clock_minute(void) {
  CappTime t;
  api->now(&t);
  J.epoch = api->epoch();
  js_set_minute(&J, t.synced ? t.hour * 60 + t.min : -1);
  set_sky(J.minute);
}

static int app_tick(void *st, uint32_t now) {
  uint32_t dt;
  int steps = 0, want;
  (void)st;
  if (!G.last_ms) G.last_ms = now;
  dt = now - G.last_ms;
  G.last_ms = now;
  if (dt > 250) dt = 250;
  G.acc += dt;
  while (G.acc >= JS_STEP_MS && steps < 10) { js_step(&J); G.acc -= JS_STEP_MS; steps++; }
  if (now - G.last_min > 1000) {
    G.last_min = now;
    clock_minute();
    if (!J.seen && J.epoch) J.seen = J.epoch;     /* the clock arrived: count from now */
  }
  /* The bars slide away on the jar after a while without a key. */
  want = G.view == V_JAR && now - G.last_key > BARS_IDLE_MS ? BAR : 0;
  if (G.bars != want && now - G.bar_ms >= 20) { G.bar_ms = now; G.bars += G.bars < want ? 1 : -1; }
  if (now - G.last_save > SAVE_MS) save();
  if (G.msg[0] && (int32_t)(now - G.msg_until) > 0) { G.msg[0] = 0; G.menu_dirty = 1; }
  if (!G.menu_dirty && (G.view != V_JAR && G.view != V_DECOR)) return 0;
  if (!G.menu_dirty && now - G.last_paint < FRAME_MS) return 0;
  G.last_paint = now;
  G.menu_dirty = 0;
  G.asked = 1;
  return 1;
}

/* ---- starting --------------------------------------------------------------- */

/* A new jar: the four starting items written to the card, three put in. */
static void first_run(void) {
  static const int16_t AT[4][2] = { { 58, 0 }, { 150, 18 }, { 40, 0 }, { 0, 0 } };
  int i, k = 0;
  for (i = 0; i < JB_COUNT; i++) {
    if (BUILTINS[i].price) continue;
    from_builtin(&BUILTINS[i], &SCR.io.it);
    if (item_write(&SCR.io.it) == 0) {
      js_own(&J, BUILTINS[i].id);
      if (k < 3) js_place(&J, &SCR.io.it, AT[k][0], AT[k][1]);
    }
    k++;
  }
  J.seen = api->epoch();
  save();
}

static void load(void) {
  int fd, n = 0, i;
  api->mkdir(CAPP_VAR);
  api->mkdir(DIR_JAR);
  api->mkdir(DIR_ITEMS);
  fd = safe_open_read(api, SAVE_PATH);
  if (fd >= 0) {
    n = api->read(fd, SCR.text, sizeof SCR.text - 1);
    api->close(fd);
  }
  if (n <= 0 || (SCR.text[n] = 0, js_load(&J, SCR.text)) != 0) { first_run(); return; }
  for (i = 0; i < J.nwant; i++) {
    int x = J.want[i].x, y = J.want[i].y;
    if (item_read(J.want[i].id) == 0) js_place(&J, &SCR.io.it, x, y);
  }
  {
    uint32_t jars = js_away(&J, api->epoch());
    if (jars) {
      api->fmt(G.msg, sizeof G.msg, "While you were away: %u jars", (unsigned)jars);
      G.msg_until = api->ticks_ms() + 7000;
    }
  }
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_PAINT_DIRECT,   /* composes its own strips */
  "Jar Factory",
  /* 16x16: a jar with a lid, a sprout and a berry bush inside. */
  { 0x3F, 0xFC, 0x20, 0x04, 0x3F, 0xFC, 0x10, 0x08,
    0x20, 0x04, 0x40, 0x02, 0x40, 0x02, 0x44, 0x02,
    0x4E, 0x02, 0x44, 0x22, 0x44, 0x72, 0x44, 0x22,
    0x7F, 0xFE, 0x40, 0x02, 0x40, 0x02, 0x3F, 0xFC },
  "S\tthe shop (Tab: My Stuff)\n"
  "D\tdecorate: move, add, take out\n"
  "U\tupgrades\n"
  "any key\tbrings the bars back\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&J, 0, sizeof J);
  api->mem_set(&G, 0, sizeof G);
  js_init(&J, api->ticks_ms() ^ api->epoch());
  G.last_key = G.last_save = api->ticks_ms();
  G.up_sel = 0;
  load();
  clock_minute();
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.pref_w = SW;
  UI.pref_h = SHT;
  api->ui(&UI);
  return 0;
}
