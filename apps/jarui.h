/* What Jar Factory's two companion apps share: the look, the item card, the
 * jar's save, and talking to the server.
 *
 * Jar Shop (apps/jarshop.c: the shop, My Stuff, upgrades, garden, shelf, the
 * daily stock) and Jar Post (apps/jarpost.c: friends, gifts, mail) are the
 * screens that used to be inside the jar. One app holding them and the scene
 * was a 44.5 KB load, against a heap whose biggest free piece is often
 * 27 KB, so the jar (apps/jar.c) keeps the scene and Decorate and opens these
 * with api->run(APP, SCREEN) after saving; each loads the save, works on it,
 * saves it, and goes back to the jar (Esc at its top, or fn-`), which starts
 * afresh from the save. Neither runs the factory: JS_KEEP_ONLY leaves out
 * the room for placed items.
 *
 * Painted with the API's own calls -- the OS composes a repaint off the
 * panel -- since nothing here animates. Each app includes this once; it
 * defines the app's globals (api, J, IO, TEXT, NET) and static helpers.
 */
#ifndef CARDOS_JARUI_H
#define CARDOS_JARUI_H

#ifndef JAR_ONE_APP
#define JS_KEEP_ONLY
#endif

#include "kernel/app/capp.h"
#include "apps/str.h"
#include "apps/confirm.h"
#include "apps/datetime.h"
#include "apps/b64.h"
#include "apps/jarstore.h"

#if defined(__GNUC__)
#define JU_OPT __attribute__((unused))
#else
#define JU_OPT
#endif

#define SW 240
#define SHT 135
#define BAR 13
#define BODY (SHT - 2 * BAR)

/* The interface palette (spec, "Screens and look"). */
#define C_BG     CAPP_RGB(0x17, 0x13, 0x2a)
#define C_PANEL  CAPP_RGB(0x2a, 0x24, 0x47)
#define C_TEXT   CAPP_RGB(0xf2, 0xec, 0xdc)
#define C_DIM    CAPP_RGB(0xa5, 0x9f, 0xc4)
#define C_GOLD   CAPP_RGB(0xff, 0xd1, 0x66)
#define C_TRAIT  CAPP_RGB(0x7f, 0xd6, 0xa6)
#define C_WARN   CAPP_RGB(0xff, 0x6b, 0x5a)
#define C_PINK   CAPP_RGB(0xff, 0x8f, 0xab)

/* Requests both apps make; each numbers its own from N_APP. */
enum { N_IDLE = 0, N_PUB, N_APP };

/* In Jar Factory itself (JAR_ONE_APP) the world and these buffers are the
 * jar's: apps/jar.c defines api and J and maps IO, TEXT, NET and PB into
 * the memory its scene draws in, which is idle while a screen is up. */
#ifndef JAR_ONE_APP
static const CardApi *api;
static Jar J;
static JIo IO;
static char TEXT[2048];                 /* the save and other files */
static char NET[2048];                  /* a request's body, its reply, a signed message */
#endif
#define PB_N (32 * 8)                   /* a picture, eight rows at a time */

/* Another screen or app: within Jar Factory a screen of it (jar_open in
 * apps/jar.c), otherwise the app by name. */
static JU_OPT int ui_run(const char *app, const char *args) {
#ifdef JAR_ONE_APP
  return jar_open(app, args);
#else
  return api->run(app, args);
#endif
}

/* What a grid tile or the item card shows of an item. */
typedef struct {
  uint32_t id;
  uint16_t price, pal[8];
  uint8_t kind, move, flags, in_jar, shelved, sold, held, seed, ok;
  uint8_t frame[JI_FRAME_BYTES];
} Tile;

typedef struct {
  Tile card;
  char name[JI_NAME + 1], line[JI_LINE + 1], maker[JI_WHO + 1], tags[JI_TAGS + 1], gifted[JI_WHO + 1];
  uint32_t made;
  char msg[44];
  uint32_t msg_until;
  int dirty, net;
  char status[40];
  uint8_t pub[65];
  int has_pub;
  uint32_t now;                         /* the clock as paint sees it: read in tick */
} UiState;

/* The screens' state. In Jar Factory itself it lives in the scene's item
 * memory, idle while a screen is up (apps/jar.c, screen_bind), because the
 * three of them do not fit one app's 28 KB otherwise; standing alone (the
 * host tests) it is ordinary statics. */
#ifdef JAR_ONE_APP
static UiState *UI_P;
static uint16_t *PB_P;
#define U  (*UI_P)
#define PB PB_P
#else
static UiState U;
static uint16_t PB[PB_N];
#endif

static JU_OPT const char *const KIND_NAME[JK_KINDS] = { "floor decor", "hanging decor", "critter" };
static JU_OPT const char *const MOVE_NAME[JM_KINDS] = { "sits", "hops", "wanders", "sways", "floats" };

/* ---- small things ------------------------------------------------------------- */

static JU_OPT CRect rc(int x, int y, int w, int h) { return capp_rect(x, y, w, h); }
static JU_OPT int slen(const char *s) { return (int)api->str_len(s); }
static JU_OPT uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static JU_OPT void say(const char *s) {
  api->fmt(U.msg, sizeof U.msg, "%s", s);
  U.msg_until = api->ticks_ms() + 4000;
  U.dirty = 1;
}

/* The jar as it was saved, with this app's changes. Not stamped with the
 * time: the factory went on running while the shop was open, and the jar
 * pays for that when it comes back. */
static JU_OPT void save(void) { jst_put(api, JST_SAVE, TEXT, js_save(&J, TEXT, sizeof TEXT)); }

static JU_OPT int put(char *b, int k, int cap, const char *s) {
  while (*s && k < cap - 1) b[k++] = *s++;
  b[k] = 0;
  return k;
}

/* Back to the jar, which starts again from the save. */
static JU_OPT void to_jar(void) {
  save();
  if (ui_run("Jar Factory", 0) != 0) say("No Jar Factory app");
}

/* Load the jar for an app that keeps it. 0, or -1 when there is none yet --
 * then the jar is opened instead, which makes one. */
static JU_OPT int ui_load(void) {
#ifndef JAR_ONE_APP                        /* in Jar Factory the world is already up */
  api->mem_set(&J, 0, sizeof J);
  js_init(&J, api->ticks_ms() | 1u);
  jst_dirs(api);
  if (jst_load(api, &J, TEXT, sizeof TEXT) != 0) { api->run("Jar Factory", 0); return -1; }
#endif
  U.now = api->epoch();
  js_settle_beds(&J, U.now);
  {
    char b[140];
    U.has_pub = jst_get(api, JST_PUB, b, sizeof b) > 0 && jst_pub_parse(b, U.pub) == 0;
  }
  return 0;
}

/* From tick: the clock for paint, which runs once a strip and so must not
 * read it itself; 1 when the minute changed (a countdown on screen). */
static JU_OPT int ui_clock(void) {
  uint32_t t = api->epoch(), was = U.now;
  U.now = t;
  return t / 60u != was / 60u;
}

/* ---- drawing -------------------------------------------------------------------- */

static JU_OPT void text(int x, int y, const char *s, uint16_t fg, uint16_t bg) {
  (api->text)((int16_t)x, (int16_t)y, s, fg, bg);
}

static JU_OPT void textn(int x, int y, const char *s, int cols, uint16_t fg, uint16_t bg) {
  char b[41];
  int i;
  for (i = 0; s[i] && i < cols && i < 40; i++) b[i] = s[i];
  b[i] = 0;
  text(x, y, b, fg, bg);
}

static JU_OPT void wrapped(int x, int y, const char *s, int cols, int lines, uint16_t fg, uint16_t bg) {
  while (*s && lines-- > 0) {
    int n = slen(s), cut = n;
    if (n > cols) {
      for (cut = cols; cut > 0 && s[cut] != ' '; cut--) {}
      if (cut == 0) cut = cols;
    }
    textn(x, y, s, cut, fg, bg);
    s += cut;
    while (*s == ' ') s++;
    y += 9;
  }
}

static JU_OPT void box(int x, int y, int w, int h, uint16_t c) { api->fill(rc(x, y, w, h), c); }
static JU_OPT void outline(int x, int y, int w, int h, uint16_t c) { api->frame(rc(x, y, w, h), c); }

/* A packed 3bpp picture at 1x or 2x, its clear pixels `bg`; at most 32 wide
 * once scaled. A palette `plain` is an item's (RGB565 as written), otherwise
 * the scene's (panel order). */
static JU_OPT void pic(const uint8_t *data, const uint16_t *pal, int plain, int w, int h, int x, int y,
                       int sc, uint16_t bg) {
  int W = w * sc, H = h * sc, rows, r0, r, c;
  if (W > 32 || W <= 0) return;
  rows = PB_N / W;
  for (r0 = 0; r0 < H; r0 += rows) {
    int n = H - r0 < rows ? H - r0 : rows;
    for (r = 0; r < n; r++)
      for (c = 0; c < W; c++) {
        int v = ji_px(data, ((r0 + r) / sc) * w + c / sc);
        PB[r * W + c] = v ? (plain ? swap16(pal[v]) : pal[v]) : bg;
      }
    api->pixels(rc(x, y + r0, W, n), PB);
  }
}

static JU_OPT void spr(int id, int x, int y, int sc, uint16_t bg) {
  const JSprite *s = &SPRITES[id];
  pic(s->px, s->pal, 0, s->w, s->h, x, y, sc, bg);
}

static JU_OPT void price(int x, int y, int cost, int warn, uint16_t bg) {
  char b[12];
  spr(SPR_COIN, x, y, 1, bg);
  api->fmt(b, sizeof b, "%d", cost);
  text(x + 9, y, b, warn ? C_WARN : C_GOLD, bg);
}

/* Coins, what the server is being asked, parcels waiting, where this is. */
static JU_OPT void top_bar(const char *where) {
  char b[24];
  int x, sx;
  box(0, 0, SW, BAR, C_BG);
  spr(SPR_COIN, 3, 3, 1, C_BG);
  api->fmt(b, sizeof b, "%u", (unsigned)J.coins);
  text(13, 3, b, C_GOLD, C_BG);
  sx = 13 + slen(b) * 6 + 8;
  x = SW - 4 - slen(where) * 6;
  text(x, 3, where, C_DIM, C_BG);
  if (J.parcels) {
    api->fmt(b, sizeof b, "%u", (unsigned)J.parcels);
    x -= slen(b) * 6 + 18;
    spr(SPR_PARCEL, x, 3, 1, C_BG);
    text(x + 11, 3, b, C_PINK, C_BG);
  }
  if (U.status[0] && x - sx > 10) textn(sx, 3, U.status, (x - sx - 4) / 6, C_DIM, C_BG);
}

/* Key hints: a light key cap, then a word (spec, "Screens and look"). */
static JU_OPT void hints(const char *const *h) {
  int x = 4, y = SHT - BAR;
  box(0, y, SW, BAR, C_BG);
  for (; *h; h += 2) {
    int w = slen(h[0]) * 6 + 3;
    box(x, y + 2, w, 9, C_TEXT);
    text(x + 2, y + 3, h[0], C_BG, C_TEXT);
    x += w + 3;
    text(x, y + 3, h[1], C_DIM, C_BG);
    x += slen(h[1]) * 6 + 7;
  }
}

static JU_OPT void paint_note(void) {
  int w;
  if (!U.msg[0]) return;
  w = slen(U.msg) * 6 + 8;
  box((SW - w) / 2, BAR + 3, w, 12, C_PANEL);
  outline((SW - w) / 2, BAR + 3, w, 12, C_GOLD);
  text((SW - w) / 2 + 4, BAR + 5, U.msg, C_TEXT, C_PANEL);
}

/* A box to type in, over the body. */
static JU_OPT void input_box(const char *label, const char *s) {
  box(10, BAR + 34, SW - 20, 34, C_BG);
  outline(10, BAR + 34, SW - 20, 34, C_GOLD);
  text(16, BAR + 38, label, C_DIM, C_BG);
  box(16, BAR + 50, SW - 32, 12, C_PANEL);
  text(18, BAR + 52, s, C_TEXT, C_PANEL);
  box(18 + slen(s) * 6, BAR + 52, 5, 8, C_GOLD);
}

/* ---- the item card --------------------------------------------------------------- */

static JU_OPT void tile_from(Tile *t, const JItem *it) {
  int i;
  t->id = it->id;
  t->kind = it->kind;
  t->move = it->move;
  t->flags = it->flags;
  for (i = 0; i < 8; i++) t->pal[i] = it->pal[i];
  ji_copy(t->frame, it->frames[0], JI_FRAME_BYTES);
  t->in_jar = (uint8_t)js_in_jar(&J, it->id);
  t->shelved = (uint8_t)(js_on_shelf(&J, it->id) >= 0);
  t->ok = 1;
}

static JU_OPT void words_from(const JItem *it) {
  ji_copy(U.name, it->name, sizeof U.name);
  ji_copy(U.line, it->line, sizeof U.line);
  ji_copy(U.maker, it->maker, sizeof U.maker);
  ji_copy(U.tags, it->tags, sizeof U.tags);
  ji_copy(U.gifted, it->gifted, sizeof U.gifted);
  U.made = it->made;
}

/* Owned item `id` as the card. 0, or -1 (and said) when it is missing. */
static JU_OPT int card_load(uint32_t id) {
  if (jst_item_read(api, id, &IO) < 0) { say("That item is missing"); return -1; }
  tile_from(&U.card, &IO.it);
  words_from(&IO.it);
  return 0;
}

/* The card: picture, number, name, line, kind, provenance, where it is. A
 * gift's sender and note come from the mail when the record has no sender. */
static JU_OPT void paint_card(const char *from, const char *note) {
  static const char *const MON[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  const Tile *t = &U.card;
  char b[48];
  box(0, BAR, SW, BODY, C_BG);
  box(4, BAR + 3, SW - 8, BODY - 6, C_PANEL);
  pic(t->frame, t->pal, 1, 16, 16, 12, BAR + 10, 2, C_PANEL);
  jitem_number(t->id, b);
  text(SW - 9 - slen(b) * 6, BAR + 9, b, C_DIM, C_PANEL);
  text(54, BAR + 9, U.name, C_TEXT, C_PANEL);
  wrapped(54, BAR + 21, U.line, 30, 2, C_DIM, C_PANEL);
  api->fmt(b, sizeof b, "%s, %s", KIND_NAME[t->kind % JK_KINDS], MOVE_NAME[t->move % JM_KINDS]);
  text(54, BAR + 42, b, C_TRAIT, C_PANEL);
  /* Provenance: where it was made ("Jar Works", or someone's jar), when,
   * and the day's tags, "rainy, night". */
  {
    int y = 0, m = 1, d = 1, k = 0, i;
    const char *who = U.maker[0] ? U.maker : "someone";
    const char *own = str_contains(who, "jar", 1) || str_contains(who, "works", 1) ? "" : "'s jar";
    if (U.made) {
      dt_civil_from_days((int32_t)(U.made / 86400u), &y, &m, &d);
      api->fmt(b, sizeof b, "made in %s%s, %d %s %d", who, own, d, MON[(m - 1) % 12], y);
    } else api->fmt(b, sizeof b, "made in %s%s", who, own);
    textn(12, BAR + 54, b, 37, C_DIM, C_PANEL);
    for (i = 0; U.tags[i] && k < (int)sizeof b - 3; i++) {
      b[k++] = U.tags[i];
      if (U.tags[i] == ',') b[k++] = ' ';
    }
    b[k] = 0;
    if (k) textn(12, BAR + 63, b, 37, C_TRAIT, C_PANEL);
  }
  if (U.gifted[0] || (from && from[0])) {
    api->fmt(b, sizeof b, "a gift from %s", U.gifted[0] ? U.gifted : from);
    text(12, BAR + 73, b, C_PINK, C_PANEL);
  } else text(12, BAR + 73, (t->flags & JIF_BUILTIN) ? "hand-made, built in" : "from the shop", C_DIM, C_PANEL);
  if (note && note[0]) {
    api->fmt(b, sizeof b, "\"%s\"", note);
    textn(12, BAR + 85, b, 37, C_TEXT, C_PANEL);
  } else {
    text(12, BAR + 85, t->in_jar ? "In your jar." : t->shelved ? "On your shelf." : "In My Stuff.",
         t->in_jar ? C_GOLD : C_TEXT, C_PANEL);
  }
}

/* The card's own keys, the same in both apps: Enter places it in the jar,
 * H puts it on the shelf or takes it off. 1 if `k` was one. */
static JU_OPT int card_key(int k) {
  if (k == CAPP_KEY_ENTER) {
    J.decor = U.card.id;                          /* the jar places it, then decorates */
    to_jar();
    return 1;
  }
  return 0;
}

/* ---- the server --------------------------------------------------------------------- */

/* Start a request to the device's own server (the kernel adds its token).
 * 0, or -1 when one is already in flight. */
static JU_OPT int send(int what, const char *method, const char *path, const char *body) {
  char u[200];
  api->fmt(u, sizeof u, "%s%s", api->proxy(), path);
  if (api->http_start(method, u, body, body ? "text/plain" : 0, 0, 20000) != 0) return -1;
  U.net = what;
  U.dirty = 1;
  return 0;
}

static JU_OPT void net_status(const char *s) {
  api->fmt(U.status, sizeof U.status, "%s", s);
  U.dirty = 1;
}

/* A request that failed, in words. */
static JU_OPT void failed(const char *what, int n) {
  if (n == -1) api->fmt(U.msg, sizeof U.msg, "%s: offline", what);
  else if (n < -99) api->fmt(U.msg, sizeof U.msg, "%s: refused (%d)", what, -n);
  else api->fmt(U.msg, sizeof U.msg, "%s: no answer", what);
  U.msg_until = api->ticks_ms() + 5000;
  U.dirty = 1;
}

/* "error WHY" in a reply that came back 200: said, and 1. */
static JU_OPT int refused(const char *what) {
  char *c;
  if (!str_starts(NET, "error")) return 0;
  api->fmt(U.msg, sizeof U.msg, "%s: %s", what, NET + (NET[5] ? 6 : 5));
  for (c = U.msg; *c; c++) if (*c == '\n') *c = 0;
  U.msg_until = api->ticks_ms() + 5000;
  U.dirty = 1;
  return 1;
}

/* The server's key, fetched once and pinned (trust on first use). 1 if a
 * request for it went out -- the caller carries on when it answers. */
static JU_OPT int need_pub(void) {
  if (U.has_pub || U.net) return 0;
  if (send(N_PUB, "GET", "/jar/pubkey", 0) != 0) return 0;
  net_status("asking for the server's key");
  return 1;
}

/* GET /jar/pubkey answered (`n` as http_poll said). 1 if there is a key now. */
static JU_OPT int pub_reply(int n) {
  if (n > 0 && jst_pub_parse(NET, U.pub) == 0) {
    jst_put(api, JST_PUB, NET, 130);
    U.has_pub = 1;
    return 1;
  }
  net_status("");
  if (n < 0) failed("Server", n);
  return 0;
}

/* A record in base64 at `p`, from the server: into IO, checked against the
 * pinned key. Its length; -1 (and logged) if it does not decode or is not
 * the server's; -2 if it could not be checked at all (no key yet, or a
 * firmware that cannot). Uses NET for the signed message, so `p` must have
 * been read before. */
static JU_OPT int take_record(const char *p, const char *what) {
  int n = b64_decode(p, IO.raw, JI_MAX), ok;
  char b[72];
  if (n <= 0 || jitem_decode(&IO.it, IO.raw, n) != 0) {
    api->fmt(b, sizeof b, "jar: %s: not a record, dropped", what);
    api->log(b);
    return -1;
  }
  ok = U.has_pub ? jst_verify(api, U.pub, IO.raw, n, (uint8_t *)NET) : -1;
  if (ok != 1) {
    api->fmt(b, sizeof b, "jar: %s No. %u: signature %s", what, (unsigned)IO.it.id,
             ok < 0 ? "not checked, kept back" : "bad, dropped");
    api->log(b);
    return ok < 0 ? -2 : -1;
  }
  return n;
}

#endif /* CARDOS_JARUI_H */
