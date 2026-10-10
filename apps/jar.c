/* Jar Factory -- a terrarium with a tiny factory inside, running unattended.
 *
 * Mosslings carry berries from the garden beds to a thimble vat; a matchbox
 * press, a cotton spool and a twig crane, joined by a bottle-cap belt, make
 * them into jam; a snail carries the jars out through the cork door, and the
 * coins land as it leaves the screen. Coins buy upgrades, plants and items
 * from the shop, which go in the jar and do small things of their own, or to
 * a friend. Nothing needs attention: leave it on.
 *
 * The design is docs/superpowers/specs/2026-10-09-jar-factory-design.md.
 * This app is the scene and Decorate; everything with a menu is in two
 * companion apps it opens after saving -- Jar Shop (apps/jarshop.c: the
 * shop, My Stuff, upgrades, garden, shelf) and Jar Post (apps/jarpost.c:
 * friends, gifts, mail) -- because the three together were 44.5 KB to load
 * against a heap whose biggest piece is often 27 KB. Quitting either comes
 * back here, and the jar starts again from its save, paid for the time it
 * was away. The world is apps/jarsim.h, the item record apps/jaritem.h, the
 * card apps/jarstore.h, the pictures apps/jar_art.h (tools/make_jar_art.py
 * from tools/jar_art.txt). Host tests: test/test_jarsim.c,
 * test/test_jaritem.c, test/test_jarstore.c, test/test_jar.c (JAR_DUMP=dir
 * writes frames).
 *
 * GIFTS ARRIVE HERE. While online (and once at the start) the jar asks the
 * server how many parcels wait (GET /q/len?q=jar.gifts), and for a new one
 * who sent it (GET /q/peek?q=jar.gifts&after=LAST&max=1): it floats down on
 * a parachute, the critters look up, a banner names the sender, and it waits
 * on the dock; Enter opens Jar Post's mail, which collects it. A thank-you
 * (q=jar.thanks, "FROM\tID") is hearts over the dock and a banner, then
 * acknowledged. Through http_start/http_poll, never blocking, every three
 * minutes while the network is up and not at all while it is down.
 *
 * DRAWING. The scene moves every frame, so the whole screen is composed here
 * a strip at a time into one buffer and blitted whole (CAPP_PAINT_DIRECT, as
 * Kart and Noodle), so nothing is ever filled and then drawn over on the
 * panel. A strip that came out the same as last time is not sent.
 *
 * ON THE CARD. /var/jar/jar.txt is the jar (js_save), saved on every
 * placement, before opening a companion and every five minutes;
 * /var/jar/items/ holds one record per owned item (apps/jarstore.h).
 */

#include "kernel/app/capp.h"
#include "kernel/console/font6x8.h"
#include "apps/str.h"
#include "apps/datetime.h"
#include "apps/jarstore.h"

#define SW 240
#define SHT 135
#define SH 8                            /* rows a strip: 3.75 KB */
#define NSTRIP ((SHT + SH - 1) / SH)
#define BAR 13
#define FRAME_MS 66                     /* the scene at 15 frames a second */
#define BARS_IDLE_MS 10000
#define SAVE_MS (5u * 60u * 1000u)
#define NET_EVERY_MS (3u * 60u * 1000u) /* how often to ask after the post */
#define NET_FIRST_MS 3000               /* ... and the first time, once drawn */
#define PARCEL_X 211                    /* parcels wait on the top crate */
#define PARCEL_Y (JS_SOIL - 29)

/* The interface palette (spec, "Screens and look"). */
#define C_BG     CAPP_RGB(0x17, 0x13, 0x2a)
#define C_PANEL  CAPP_RGB(0x2a, 0x24, 0x47)
#define C_TEXT   CAPP_RGB(0xf2, 0xec, 0xdc)
#define C_DIM    CAPP_RGB(0xa5, 0x9f, 0xc4)
#define C_GOLD   CAPP_RGB(0xff, 0xd1, 0x66)
#define C_TRAIT  CAPP_RGB(0x7f, 0xd6, 0xa6)
#define C_WARN   CAPP_RGB(0xff, 0x6b, 0x5a)
#define C_PINK   CAPP_RGB(0xff, 0x8f, 0xab)

enum { V_JAR = 0, V_DECOR };
enum { Q_IDLE = 0, Q_LEN, Q_PEEK, Q_THANKS, Q_ACK };   /* asking after the post */

static const CardApi *api;
static Jar J;

/* The strip, and the same memory for the card and the network, which never
 * happen while a strip is being drawn. */
static union {
  uint16_t strip[SW * SH];
  JIo io;
  char text[2048];
} SCR;

static struct {
  int view;
  int dsel, dmove, dnew;                /* decorate: selection, moving, a new one */
  int16_t dx0, dy0;
  char dname[JI_NAME + 1];              /* the item selected in decorate */
  char dline[JI_LINE + 1];
  char msg[40];
  uint32_t msg_until;
  uint32_t last_ms, acc, last_key, last_paint, last_save, last_min, frames, bar_ms;
  int bars, menu_dirty, asked;
  int sky[6];                           /* top and bottom of the background, r g b */
  uint32_t hash[NSTRIP];
  /* the post */
  int q, waiting, failed, asked_post;
  uint32_t next_q;
  int32_t para_y;                       /* a parcel on its way down, 1/16 px; 0 none */
  int16_t para_x;
  int hearts;                           /* hearts still to rise from the dock */
  /* zoom: 2x, every scene pixel drawn as 2x2 -- the art is pixel art */
  int zoom;
  int zx, zy;                           /* the view's top left, in scene pixels */
  int follow;                           /* 0..moss-1 a mossling, JS_MAX_MOSS the snail, -1 free */
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
static uint16_t *SB = SCR.strip;        /* where row SY0 starts (zoom: the buffer's second half) */

static void px(int x, int y, uint16_t c) {
  if ((unsigned)x < SW && y >= SY0 && y < SY1) SB[(y - SY0) * SW + x] = c;
}

static void fill(int x, int y, int w, int h, uint16_t c) {
  int yy, xx, x1 = x + w, y1 = y + h;
  if (x < 0) x = 0;
  if (x1 > SW) x1 = SW;
  if (y < SY0) y = SY0;
  if (y1 > SY1) y1 = SY1;
  for (yy = y; yy < y1; yy++) {
    uint16_t *r = SB + (yy - SY0) * SW;
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
    row = SB + (y - SY0) * SW;
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
    uint16_t *row = SB + (y - SY0) * SW, c;
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
    if (J.bed[i].young) {                     /* newly planted: a shoot, no berries */
      uint16_t g = rgb(0x5a, 0xa8, 0x48);
      int sway = (int)((J.steps / 48 + (uint32_t)i) & 1);
      fill(bx, JS_SOIL - 7, 1, 5, g);
      fill(bx - 2 + sway, JS_SOIL - 7, 2, 1, g);
      fill(bx + 1 + sway, JS_SOIL - 5, 2, 1, g);
      continue;
    }
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
  /* parcels from friends, waiting to be opened (one may still be falling) */
  n = J.parcels + G.waiting - (G.para_y ? 1 : 0);
  for (i = 0; i < n && i < 3; i++) spr(SPR_PARCEL, 0, PARCEL_X - 4 + (i & 1) * 2, PARCEL_Y - i * 7, 0);
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

/* A parcel from a friend, swinging under a striped parachute. */
static void parachute(void) {
  int y = G.para_y / 16, t = (int)((J.steps / 5) % 16u), sway = t < 8 ? t - 4 : 12 - t, x, dx, dy;
  if (!G.para_y) return;
  x = G.para_x + sway / 2;
  for (dy = -8; dy <= 0; dy++)                       /* the canopy: a striped dome */
    for (dx = -9; dx <= 9; dx++)
      if (dx * dx + dy * dy * 2 <= 90) px(x + dx, y - 13 + dy, ((dx + 9) / 3) & 1 ? C_PINK : C_TEXT);
  for (dy = 0; dy < 12; dy++) {                       /* the strings */
    px(x - 8 + dy * 5 / 12, y - 12 + dy, C_DIM);
    px(x + 8 - dy * 5 / 12, y - 12 + dy, C_DIM);
  }
  spr(SPR_PARCEL, 0, x - 4, y, 0);
}

static void scene(void) {
  background();
  garden();
  works();
  dock();
  items();
  workers();
  parachute();
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

/* The jar's seven keys, each a word with its key as a light cap inside it:
 * seven caps and seven words side by side do not fit in 240 pixels. */
static void jar_keys(int y) {
  static const char *const W[] = { "Shop", "Deco", "Plant", "sHelf", "Friends", "Mail", "Up", "Zoom", 0 };
  int x = 3, i, k;
  char c[2];
  c[1] = 0;
  for (i = 0; W[i]; i++, x += 3)
    for (k = 0; W[i][k]; k++, x += 6) {
      c[0] = W[i][k];
      if (c[0] >= 'A' && c[0] <= 'Z') { fill(x - 1, y + 2, 7, 9, C_TEXT); text(x, y + 3, c, C_BG); }
      else text(x, y + 3, c, C_DIM);
    }
}

static void rate_text(char *b, int n) {
  int32_t r = js_rate_ph(&J) * 10 / 60;
  api->fmt(b, (size_t)n, "%d.%d/min", (int)(r / 10), (int)(r % 10));
}

/* Parcels on the dock or on their way: what Jar Post has collected and not
 * opened, and what the server still holds. */
static int parcels(void) { return J.parcels + G.waiting; }

static void bars(void) {
  static const char *const H_DSEL[] = { "<>", "pick", "Ent", "move", "N", "add", "X", "out", "Esc", "done", 0 };
  static const char *const H_DMOVE[] = { "<>", "move", "^v", "string", "Ent", "put", "Esc", "cancel", 0 };
  static const char *const H_DMOVE2[] = { "<>", "move", "Ent", "put", "Esc", "cancel", 0 };
  const char *where = "The Jar";
  char b[24];
  int ty = -G.bars, by = SHT - BAR + G.bars;
  if (ty + BAR > SY0 && ty < SY1) {
    int x;
    fill(0, ty, SW, BAR, C_BG);
    spr(SPR_COIN, 0, 3, ty + 3, 0);
    api->fmt(b, sizeof b, "%u", (unsigned)J.coins);
    text(13, ty + 3, b, C_GOLD);
    if (G.view == V_JAR) {
      x = 13 + slen(b) * 6 + 6;
      rate_text(b, sizeof b);
      text(x, ty + 3, b, C_DIM);
    } else {
      api->fmt(b, sizeof b, "Decorate %u/%d", (unsigned)J.nplaced, JS_MAX_PLACED);
      where = b;
    }
    x = SW - 4 - slen(where) * 6;
    text(x, ty + 3, where, C_DIM);
    if (parcels()) {                                   /* the mail indicator */
      char m[8];
      api->fmt(m, sizeof m, "%d", parcels());
      x -= slen(m) * 6 + 17;
      spr(SPR_PARCEL, 0, x, ty + 3, 0);
      text(x + 11, ty + 3, m, C_PINK);
    }
  }
  if (by + BAR > SY0 && by < SY1) {
    fill(0, by, SW, BAR, C_BG);
    if (G.view == V_JAR) jar_keys(by);
    else hints(by, !G.dmove ? H_DSEL : J.placed[G.dsel].kind == JK_HANGING ? H_DMOVE : H_DMOVE2);
  }
}

/* A note across the top of the body: away earnings, a parcel, a heart. */
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

/* ---- painting ------------------------------------------------------------ */

static void render(void) {
  scene();
  if (G.view == V_DECOR) decor_overlay();
  bars();
  note();
}

static uint32_t strip_hash(int n) {
  uint32_t h = 2166136261u;
  int i;
  for (i = 0; i < n; i++) h = (h ^ SCR.strip[i]) * 16777619u;
  return h;
}

/* Zoomed: the output strip's 8 rows are 4 scene rows, each drawn twice and
 * each pixel twice across. The scene draws those 4 rows into the second half
 * of the strip buffer, and they are spread over the whole of it from the top:
 * output rows 2k and 2k+1 come from scene row k, so a scene row is always
 * read before an output row lands on it -- except the last, which is why a
 * row is copied out first. No second buffer: memory is the budget here. */
static void render_zoomed(int k) {
  uint16_t row[SW / 2];
  int half = SH / 2, r, x, o0 = k * SH;
  SY0 = G.zy + o0 / 2;
  SY1 = SY0 + half;
  SB = SCR.strip + half * SW;
  scene();
  SB = SCR.strip;
  for (r = 0; r < half; r++) {
    const uint16_t *src = SCR.strip + (half + r) * SW + G.zx;
    uint16_t *a = SCR.strip + (2 * r) * SW, *b = a + SW;
    for (x = 0; x < SW / 2; x++) row[x] = src[x];
    for (x = 0; x < SW / 2; x++) { a[2 * x] = a[2 * x + 1] = row[x]; }
    for (x = 0; x < SW; x++) b[x] = a[x];
  }
  SY0 = o0;
  SY1 = o0 + SH > SHT ? SHT : o0 + SH;
  /* on top, at full size: a banner (a parcel, a heart, away earnings) and
   * the mark that this is the zoomed view */
  note();
  if (SY0 < 12) {
    fill(SW - 19, 2, 17, 9, C_BG);
    text(SW - 17, 3, "2x", C_GOLD);
  }
}

/* Where the view wants to be: on whoever it follows, kept inside the jar. */
static void zoom_aim(void) {
  int tx, ty;
  if (G.follow < 0) return;
  if (G.follow < J.nmoss) { tx = (int)(J.moss[G.follow].x / JS_FX); ty = JS_SOIL - 20; }
  else { tx = (int)(J.snail.x / JS_FX); ty = JS_SOIL - 20; }
  tx -= SW / 4;
  ty -= SHT / 4;
  /* glide, two pixels a frame, rather than jump */
  G.zx += tx > G.zx + 1 ? 2 : tx < G.zx - 1 ? -2 : 0;
  G.zy += ty > G.zy + 1 ? 1 : ty < G.zy - 1 ? -1 : 0;
}

static void zoom_clamp(void) {
  if (G.zx < 0) G.zx = 0;
  if (G.zx > SW / 2) G.zx = SW / 2;
  if (G.zy < 0) G.zy = 0;
  if (G.zy > SHT - (SHT + 1) / 2) G.zy = SHT - (SHT + 1) / 2;
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
    if (G.zoom) render_zoomed(k);
    else render();
    h = strip_hash((SY1 - SY0) * SW);
    if (!full && h == G.hash[k]) continue;
    G.hash[k] = h;
    api->pixels(rc(c.x, c.y + SY0, SW, SY1 - SY0), SCR.strip);
  }
}

/* ---- the card ------------------------------------------------------------- */

/* Owned item `id` into SCR.io. 0, or -1. */
static int item_read(uint32_t id) { return jst_item_read(api, id, &SCR.io) >= 0 ? 0 : -1; }

/* Scripts' memory, back into the items' records on the card. Only the 16
 * bytes at 136 are touched -- the rest stays byte for byte as the server
 * signed it, and the signature does not cover memory, so a gift of this
 * item still verifies. */
static void save_memory(void) {
  int i, k, n;
  char p[48];
  for (i = 0; i < J.nplaced; i++) {
    if (!J.placed[i].mem_dirty) continue;
    n = jst_item_read(api, J.placed[i].id, &SCR.io);
    if (n < 0 || !js_mem_sync(&J, i, &SCR.io.it)) { J.placed[i].mem_dirty = 0; continue; }
    for (k = 0; k < 8; k++) {
      SCR.io.raw[136 + 2 * k] = (uint8_t)SCR.io.it.mem[k];
      SCR.io.raw[137 + 2 * k] = (uint8_t)((uint16_t)SCR.io.it.mem[k] >> 8);
    }
    jst_item_path(api, J.placed[i].id, p, sizeof p);
    jst_put(api, p, SCR.io.raw, n);
  }
}

static void save(void) {
  save_memory();
  jst_save(api, &J, SCR.text, sizeof SCR.text);
  G.last_save = api->ticks_ms();
}

/* Save, and open a companion at one of its screens. Quitting it comes back
 * to a fresh jar, loaded from this save. */
static void open_app(const char *app, const char *screen) {
  save();
  if (api->run(app, screen) != 0) {
    api->fmt(G.msg, sizeof G.msg, "%s is not on the card", app);
    G.msg_until = api->ticks_ms() + 4000;
  }
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
      if (G.dnew) { js_unplace(&J, G.dsel); G.dsel = 0; decor_select(0); save(); }
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
  case 'n': case 'N': open_app("Jar Shop", "decor"); return 1;
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

static int app_key(void *st, unsigned char k) {
  uint32_t now = api->ticks_ms();
  (void)st;
  G.menu_dirty = 1;
  /* Zoomed: the view's keys. Z or Esc goes back; arrows pan (and stop
   * following); Tab follows the next critter. */
  if (G.zoom) {
    G.last_key = now;
    switch (k) {
    case 'z': case 'Z': case CAPP_KEY_ESC: G.zoom = 0; return 1;
    case CAPP_KEY_LEFT:  G.follow = -1; G.zx -= 12; zoom_clamp(); return 1;
    case CAPP_KEY_RIGHT: G.follow = -1; G.zx += 12; zoom_clamp(); return 1;
    case CAPP_KEY_UP:    G.follow = -1; G.zy -= 8; zoom_clamp(); return 1;
    case CAPP_KEY_DOWN:  G.follow = -1; G.zy += 8; zoom_clamp(); return 1;
    case '\t': G.follow = G.follow < 0 ? 0 : G.follow >= J.nmoss ? 0 : G.follow + 1;
      if (G.follow == J.nmoss) G.follow = JS_MAX_MOSS;   /* after the mosslings, the snail */
      return 1;
    default: return 1;
    }
  }
  /* Bars hidden: a key brings them back and does nothing else. */
  if (G.view == V_JAR && G.bars > 0) { G.last_key = now; return 1; }
  G.last_key = now;
  if (G.view == V_DECOR) return key_decor(k);
  switch (k) {
  case 's': case 'S': open_app("Jar Shop", "shop"); return 1;
  case 'd': case 'D': G.view = V_DECOR; G.dmove = 0; decor_select(G.dsel); return 1;
  case 'p': case 'P': open_app("Jar Shop", "garden"); return 1;
  case 'h': case 'H': open_app("Jar Shop", "shelf"); return 1;
  case 'u': case 'U': open_app("Jar Shop", "up"); return 1;
  case 'z': case 'Z':
    G.zoom = 1;
    zoom_aim();
    zoom_clamp();
    return 1;
  case 'f': case 'F': open_app("Jar Post", "friends"); return 1;
  case 'm': case 'M': open_app("Jar Post", "mail"); return 1;
  case CAPP_KEY_ENTER:
    if (parcels()) open_app("Jar Post", "mail");     /* the parcel on the dock */
    return 1;
  }
  return 0;
}

/* ---- the post ------------------------------------------------------------------ */

static void q_start(int what, const char *method, const char *path) {
  char u[160];
  api->fmt(u, sizeof u, "%s%s", api->proxy(), path);
  if (api->http_start(method, u, 0, 0, 0, 15000) == 0) G.q = what;
}

/* A parcel from `from` floats down to the dock; the critters look up. */
static void parcel_arrives(const char *from) {
  int k;
  G.para_x = PARCEL_X;
  G.para_y = 16 * 16;
  for (k = 0; k < J.nmoss; k++) if (k % 2 == 0) js_say(&J, k, 3);     /* "ooh" */
  if (J.snail.st != S_AWAY) js_say(&J, JW_SNAIL, 3);
  js_event(&J, JE_GIFT, 0);
  api->fmt(G.msg, sizeof G.msg, "Parcel from %s! Enter opens it", from[0] ? from : "a friend");
  G.msg_until = api->ticks_ms() + 7000;
}

/* Thank-you messages, "ID\tFROM\tAT\tSIZE\n" + "FROM\tID" + "\n" each: hearts
 * for each, and the last id to acknowledge. */
static uint32_t thanks(int n) {
  const char *p = SCR.text, *end = SCR.text + n;
  uint32_t last = 0;
  char from[JI_WHO + 1], sz[12];
  int count = 0;
  from[0] = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    const char *h = p, *body, *q = sz;
    uint32_t id = str_uint(&p), size;
    tsv_field(h, 3, sz, sizeof sz);
    size = str_uint(&q);
    body = tsv_next_line(h);
    if (body + size > end) break;                      /* cut short: the rest next time */
    tsv_field(body, 0, from, sizeof from);
    last = id;
    count++;
    p = body + size;
    if (*p == '\n') p++;
  }
  if (count) {
    G.hearts += 3 * count;
    /* A parcel's banner stays up: the hearts are enough on their own. */
    if (G.msg[0] && (int32_t)(api->ticks_ms() - G.msg_until) < 0) return last;
    if (count == 1) api->fmt(G.msg, sizeof G.msg, "%s sends a heart", from);
    else api->fmt(G.msg, sizeof G.msg, "%d hearts from friends", count);
    G.msg_until = api->ticks_ms() + 6000;
  }
  return last;
}

static void post_reply(int n) {
  int was = G.q;
  char path[64];
  G.q = Q_IDLE;
  if (n < 0) { G.failed = 1; return; }
  SCR.text[n] = 0;
  G.failed = 0;
  switch (was) {
  case Q_LEN: {
    const char *p = SCR.text;
    G.waiting = (int)str_uint(&p);
    if (G.waiting > 0) {
      api->fmt(path, sizeof path, "/q/peek?q=jar.gifts&after=%u&max=1", (unsigned)J.gseen);
      q_start(Q_PEEK, "GET", path);
    } else q_start(Q_THANKS, "GET", "/q/peek?q=jar.thanks&max=8");
    return;
  }
  case Q_PEEK:
    if (SCR.text[0] >= '0' && SCR.text[0] <= '9') {
      const char *p = SCR.text;
      uint32_t id = str_uint(&p);
      char from[JI_WHO + 1];
      tsv_field(tsv_next_line(SCR.text), 0, from, sizeof from);
      if (id > J.gseen) {
        J.gseen = id;
        parcel_arrives(from);
        save();
      }
    }
    q_start(Q_THANKS, "GET", "/q/peek?q=jar.thanks&max=8");
    return;
  case Q_THANKS: {
    uint32_t last = thanks(n);
    if (last) {
      api->fmt(path, sizeof path, "/q/ack?q=jar.thanks&upto=%u", (unsigned)last);
      q_start(Q_ACK, "POST", path);
    }
    return;
  }
  }
}

/* Asking after the post: at the start, then every few minutes while the
 * network is up (never waking it), and less often after a failure. */
static void post_tick(uint32_t now) {
  if (G.q) {
    int n = api->http_poll(SCR.text, sizeof SCR.text);
    if (n != CAPP_HTTP_PENDING) post_reply(n);
    return;
  }
  if ((int32_t)(now - G.next_q) < 0) return;
  G.next_q = now + (G.failed ? 5 * NET_EVERY_MS : NET_EVERY_MS);
  /* The first ask may bring the network up; after that, only while it is. */
  if (G.asked_post && !api->net_ready()) return;
  G.asked_post = 1;
  q_start(Q_LEN, "GET", "/q/len?q=jar.gifts");
}

/* The parcel on its parachute, and hearts rising, a jar step at a time. */
static void post_step(void) {
  if (G.para_y) {
    G.para_y += 8;
    if (G.para_y >= PARCEL_Y * 16) G.para_y = 0;   /* landed: on the crates with the rest */
  }
  if (G.hearts > 0 && J.steps % 12 == 0) {
    js_particle(&J, JP_HEART, 200 + (int)(J.steps % 24u), JS_SOIL - 18);
    G.hearts--;
  }
}

/* ---- time -------------------------------------------------------------------- */

static void clock_minute(void) {
  CappTime t;
  api->now(&t);
  J.epoch = api->epoch();
  js_set_minute(&J, t.synced ? t.hour * 60 + t.min : -1);
  js_settle_beds(&J, J.epoch);              /* a plant comes of age while you watch */
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
  while (G.acc >= JS_STEP_MS && steps < 10) { js_step(&J); post_step(); G.acc -= JS_STEP_MS; steps++; }
  if (now - G.last_min > 1000) {
    G.last_min = now;
    clock_minute();
    if (!J.seen && J.epoch) J.seen = J.epoch;     /* the clock arrived: count from now */
  }
  post_tick(now);
  if (G.zoom) { zoom_aim(); zoom_clamp(); }
  /* The bars slide away on the jar after a while without a key. */
  want = G.view == V_JAR && now - G.last_key > BARS_IDLE_MS ? BAR : 0;
  if (G.bars != want && now - G.bar_ms >= 20) { G.bar_ms = now; G.bars += G.bars < want ? 1 : -1; }
  if (now - G.last_save > SAVE_MS) save();
  if (G.msg[0] && (int32_t)(now - G.msg_until) > 0) { G.msg[0] = 0; G.menu_dirty = 1; }
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
  for (i = 0; i < JB_NSTART; i++) {
    jst_from_builtin(&JB_STARTERS[i], &SCR.io.it);
    if (jst_item_write(api, &SCR.io) == 0) {
      js_own(&J, JB_STARTERS[i].id);
      if (k < 3) js_place(&J, &SCR.io.it, AT[k][0], AT[k][1]);
    }
    k++;
  }
  J.seen = api->epoch();
  save();
}

static void load(void) {
  int i;
  uint32_t now = api->epoch(), gone, jars;
  jst_dirs(api);
  if (jst_load(api, &J, SCR.text, sizeof SCR.text) != 0) { first_run(); return; }
  js_settle_beds(&J, now);
  for (i = 0; i < J.nwant; i++) {
    int x = J.want[i].x, y = J.want[i].y;
    if (item_read(J.want[i].id) == 0) js_place(&J, &SCR.io.it, x, y);
  }
  J.nwant = 0;                                     /* placed, or gone from the card */
  gone = now && J.seen && now > J.seen ? now - J.seen : 0;
  jars = js_away(&J, now);
  if (jars && gone >= 600) {                       /* not for a trip to the shop */
    api->fmt(G.msg, sizeof G.msg, "While you were away: %u jars", (unsigned)jars);
    G.msg_until = api->ticks_ms() + 7000;
  }
  /* Jar Shop or Jar Post asked for an item to be put in the jar. */
  if (J.decor) {
    uint32_t id = J.decor;
    J.decor = 0;
    if (js_find(&J, id) >= 0) { G.view = V_DECOR; decor_select(js_find(&J, id)); }
    else if (js_owns(&J, id)) place_new(id);
    save();
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
  "P\tthe garden: plant the beds\n"
  "H\tthe shelf: what steers the shop\n"
  "F\tfriends\n"
  "M\tmail; Enter opens a parcel on the dock\n"
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
  G.next_q = api->ticks_ms() + NET_FIRST_MS;
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
