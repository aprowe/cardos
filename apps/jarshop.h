/* Jar Shop -- Jar Factory's shop, My Stuff, upgrades and garden.
 *
 * A screen of Jar Factory (apps/jar.c), opened with shop_open(SCREEN): shop,
 * stuff, decor (My Stuff, picking something to put in the jar), up, garden.
 * Esc at a top screen, or fn-`, goes back to the jar. What it shares
 * with Jar Post (apps/jarpost.c) is apps/jarui.h; the files are
 * apps/jarstore.h's.
 *
 * THE DAILY STOCK (spec step 5; server/jar.py). When the shop opens and its
 * stock is not today's (UTC), it asks in the background, one request at a
 * time, never blocking:
 *   POST /jar/day   "garden mushroom=N,berry=N,fern=N,flower=N,cactus=N"
 *                   (grown beds), "shelf TAG,..." (the shelved items'
 *                   tags), "owned NAME,..." (up to 40), "tz ZONE"
 *   GET  /jar/day   again every 3 s while it says "pending", until
 *                   "ok DATE\ntags ...\nitems N" ("error WHY" gives up);
 *                   a "more" line after it says the server, which makes
 *                   them one at a time, has more coming: ask again in 4 s
 *   GET  /jar/item?i=K  for K in 0..N-1: one line of base64, a signed
 *                   record, checked against the key pinned in
 *                   /var/jar/server.pub (GET /jar/pubkey the first time)
 * and each item goes on show as it comes, the new batch replacing the old
 * one from its first item. Offline, or on
 * any failure, yesterday's stays; before any batch there is the hand-made
 * one built into the app. A record carries no price: jst_price works one
 * out from the item, the same everywhere.
 *
 * THE GARDEN (step 4). Six beds, five plants; a plant takes three real days
 * (api->epoch; with no clock it waits) and only grown beds make jam and
 * steer the stock. Replacing a grown plant asks first.
 *
 * Host tests: test/test_jarshop.c (JAR_DUMP=dir writes its screens).
 */

#ifndef CARDOS_JARSHOP_H
#define CARDOS_JARSHOP_H

#include "apps/jarui.h"

#define C_SOIL CAPP_RGB(0x4a, 0x34, 0x24)

enum { V_STOCK = 0, V_STUFF, V_CARD, V_UP, V_GARDEN, V_PLANT };
enum { P_BROWSE = 0, P_DECOR };                          /* what My Stuff is picking for */
enum { N_DAY_POST = N_APP, N_DAY_POLL, N_ITEM };

typedef struct {
  JStock s;                                               /* the stock on show */
  JStock ns;                                              /* the day's batch, arriving */
  /* Tibbs's line of the day (server/shopkeep.py), and seeds in hand. Talking
   * to him is Jar Post's ("talk"); t opens it. */
  char say[144];
  int seeds[JPL_KINDS];
  struct {
  int view, back, pick, slot, sel, top, ntile;
  int up_sel, bed, plant, asking;
  Tile tile[8];
  int k, nitems, polls, tried, again;
  /* The server makes a stock one item at a time and says "more" until it is
   * done: `more` is that, `live` that this batch is already on show. */
  int more, live, got, quiet, dropped;
  uint32_t wait_until;
  } g;
} ShopMem;

/* In Jar Factory, in the scene's item memory while the shop is up (jarui.h). */
#ifdef JAR_ONE_APP
static ShopMem *SHOP_P;
#define SHOP_MEM (*SHOP_P)
#else
static ShopMem SHOP_MEM;
#endif
#define S     SHOP_MEM.s
#define NS    SHOP_MEM.ns
#define SAY   SHOP_MEM.say
#define SEEDS SHOP_MEM.seeds
#define G     SHOP_MEM.g

static const char *const UP_NAME[JU_KINDS] = { "Another mossling", "Another machine", "Faster belt", "Another bed" };
static const int PLANT_SPR[JPL_KINDS] = { SPR_BUSH, SPR_FERN, SPR_SHROOM, SPR_FLOWER, SPR_CACTUS };
static const char *const SHORT[JPL_KINDS] = { "berry", "fern", "shroom", "flower", "cactus" };

/* ---- the grids ------------------------------------------------------------------- */

static int stuff_mode(void) { return G.view == V_STUFF; }
static int page_count(void) { return stuff_mode() ? J.nowned : S.n + (S.seed ? 1 : 0); }

/* The seed packet is the stock's last tile. */
static void seed_tile(Tile *t) {
  api->mem_set(t, 0, sizeof *t);
  t->seed = S.seed;
  t->price = S.seed_price;
  t->sold = S.seed_sold;
  t->ok = !S.seed_sold;
}

/* What the grid shows, and the selected one's words. Card I/O: from key
 * handlers and tick, never from paint. */
static void load_page(void) {
  int i, n = page_count();
  /* whole tiles, not just .ok: a seed tile's mark once stayed on the item after it */
  api->mem_set(G.tile, 0, sizeof G.tile);
  G.ntile = 0;
  for (i = 0; i < 8 && G.top * 4 + i < n; i++) {
    int k = G.top * 4 + i, ok;
    if (!stuff_mode() && k == S.n) { seed_tile(&G.tile[i]); G.ntile = i + 1; continue; }
    ok = stuff_mode() ? jst_item_read(api, J.owned[k], &IO) >= 0 : jst_stock_read(api, &S, k, &IO) >= 0;
    if (ok) {
      tile_from(&G.tile[i], &IO.it);
      if (!stuff_mode()) {
        G.tile[i].price = S.price[k];
        G.tile[i].sold = (uint8_t)jst_sold(&S, &J, k);
        G.tile[i].held = (uint8_t)(S.held >> k & 1);
        G.tile[i].ok = !G.tile[i].sold;
      }
    }
    G.ntile = i + 1;
  }
  U.card.ok = 0;
  i = G.sel - G.top * 4;
  if (i >= 0 && i < G.ntile && G.tile[i].ok && G.tile[i].seed) {
    ji_copy(&U.card, &G.tile[i], (int)sizeof U.card);
    api->fmt(U.name, sizeof U.name, "%s seeds", JS_PLANT_NAME[G.tile[i].seed - 1]);
    api->fmt(U.line, sizeof U.line, "Plant them in the Garden");
  } else if (i >= 0 && i < G.ntile && G.tile[i].ok) {
    ji_copy(&U.card, &G.tile[i], (int)sizeof U.card);
    if (stuff_mode()) jst_item_read(api, G.tile[i].id, &IO);
    else jst_stock_read(api, &S, G.sel, &IO);
    words_from(&IO.it);
  }
  U.dirty = 1;
}

static void go_stuff(int pick) {
  G.view = V_STUFF;
  G.pick = pick;
  G.sel = G.top = 0;
  load_page();
}

static void start_day(void);

static void go_stock(void) {
  G.view = V_STOCK;
  G.sel = G.top = 0;
  jst_stock_load(api, &S, TEXT, sizeof TEXT);
  load_page();
  start_day();
}

static void open_card(uint32_t id, int back) {
  if (card_load(id)) return;
  G.back = back;
  G.view = V_CARD;
  U.dirty = 1;
}

/* ---- the daily stock ---------------------------------------------------------------- */

static uint32_t today(void) { return api->epoch() / 86400u; }
static int today_gen(void) { return 2 + (int)(today() % 250u); }

/* "TZ=..." from the console's settings, for the server's weather; "" if
 * none is set (the server then goes without). */
static void tz_of(char *out, int n) {
  const char *p;
  out[0] = 0;
  if (api->shell("env", NET, sizeof NET) < 0) return;
  for (p = NET; *p; p = tsv_next_line(p))
    if (str_starts(p, "TZ=")) {
      int i = 0;
      p += 3;
      while (*p && *p != '\n' && *p != '\r' && i < n - 1) out[i++] = *p++;
      out[i] = 0;
      return;
    }
}

/* The day's request into NET: what is in the jar (Tibbs knows it), what is
 * owned (not to be made again), tz, and what was bought and held. Its length. */
static int day_body(void) {
  char tz[48];
  int k, i, first;
  tz_of(tz, sizeof tz);
  k = put(NET, 0, sizeof NET, "jar ");
  for (i = 0, first = 1; i < J.nwant && i < 16 && k < (int)sizeof NET - 400; i++)
    if (jst_item_read(api, J.want[i].id, &IO) >= 0) {
      char *c;
      for (c = IO.it.name; *c; c++) if (*c == ',') *c = ' ';
      if (!first) k = put(NET, k, sizeof NET, ",");
      k = put(NET, k, sizeof NET, IO.it.name);
      first = 0;
    }
  k = put(NET, k, sizeof NET, "\nowned ");
  for (i = 0, first = 1; i < J.nowned && i < 40 && k < (int)sizeof NET - 80; i++)
    if (jst_item_read(api, J.owned[i], &IO) >= 0) {
      char *c;
      for (c = IO.it.name; *c; c++) if (*c == ',') *c = ' ';
      if (!first) k = put(NET, k, sizeof NET, ",");
      k = put(NET, k, sizeof NET, IO.it.name);
      first = 0;
    }
  k = put(NET, k, sizeof NET, "\n");
  if (tz[0]) { k = put(NET, k, sizeof NET, "tz "); k = put(NET, k, sizeof NET, tz); k = put(NET, k, sizeof NET, "\n"); }
  /* The stock on show: what was bought from it is ours now, what is held
   * stays in the next one; the server puts the rest back in the pool. */
  if (S.date[0]) {
    int pass;
    for (pass = 0; pass < 2; pass++) {
      first = 1;
      for (i = 0; i < S.n; i++)
        if ((pass ? S.held : S.sold) >> i & 1) {
          k += api->fmt(NET + k, sizeof NET - (size_t)k, "%s%u", first ? (pass ? "held " : "bought ") : ",",
                        (unsigned)S.id[i]);
          first = 0;
        }
      if (!first) k = put(NET, k, sizeof NET, "\n");
    }
  }
  return k;
}

/* Ask for today's stock, once a session. When the stock here is already
 * today's, only ask how it stands (a GET): the rest of a stock that was still
 * being made, or a new one, is picked up then -- quietly, saying nothing if
 * nothing changed or the server cannot be reached. Without a clock there is
 * no "today", so nothing. */
static int s_fresh;                               /* r: a new batch now, not tomorrow */

static void start_day(void) {
  if (G.tried || U.net || !api->epoch()) return;
  if (need_pub()) return;                        /* the key first; then here again */
  G.tried = 1;
  G.quiet = 0;
  if (!s_fresh && S.date[0] && S.gen == today_gen()) {
    G.quiet = 1;
    if (send(N_DAY_POLL, "GET", "/jar/day", 0) != 0) G.tried = 0;
    return;
  }
  day_body();
  if (send(N_DAY_POST, "POST", s_fresh ? "/jar/day?fresh=1" : "/jar/day", NET) == 0)
    net_status(s_fresh ? "asking for a new stock" : "asking for today's stock");
  else G.tried = 0;
  s_fresh = 0;
}

/* The batch so far goes on show: it replaces the old stock, whose records
 * go. What was bought from it stays bought (its sold bits are the shop's). */
static void show_batch(void) {
  char p[48];
  int i, j, old = S.gen;
  NS.sold = G.live ? S.sold : 0;
  if (G.live && NS.seed == S.seed) NS.seed_sold = S.seed_sold;
  if (G.live) NS.held = S.held;
  else                                            /* a new stock: held ones, by id */
    for (NS.held = 0, i = 0; i < NS.n; i++)
      for (j = 0; j < S.n; j++)
        if (S.date[0] && NS.id[i] == S.id[j] && (S.held >> j & 1)) NS.held = (uint8_t)(NS.held | 1u << i);
  ji_copy(&S, &NS, (int)sizeof S);
  jst_stock_save(api, &S, TEXT, sizeof TEXT);
  if (old != S.gen)
    for (i = 0; i < JST_STOCK_N; i++) { jst_stock_path(api, old, i, p, sizeof p); api->remove(p); }
  G.live = 1;
  if (G.sel >= S.n) G.sel = G.top = 0;
  if (G.view == V_STOCK) load_page();
}

static void next_item(void) {
  char p[40];
  if (G.k >= G.nitems || G.k >= JST_STOCK_N) {
    if (G.more && G.k < JST_STOCK_N) {                   /* the rest is being made */
      api->fmt(p, sizeof p, "%d here, more being made", S.n);
      net_status(p);
      G.wait_until = api->ticks_ms() + 4000;
      G.again = 1;
      return;
    }
    net_status("");
    if (!G.got && G.live) { G.quiet = 0; return; }   /* asked how it stands: as it was */
    show_batch();
    if (G.dropped) {                               /* never quietly: it looks like no stock */
      api->fmt(p, sizeof p, "%d failed the check (log)", G.dropped);
      say(p);
    } else say(S.n ? "Today's stock is in" : "No stock today");
    G.quiet = 0;
    return;
  }
  api->fmt(p, sizeof p, "/jar/item?i=%d", G.k);
  if (send(N_ITEM, "GET", p, 0) != 0) G.wait_until = api->ticks_ms() + 500;
}

/* "ok DATE\ntags ...\nitems N\nbatch B[\nmore]": 1 if that is what NET
 * holds. "more" says the server is still making the rest: the N so far are
 * fetched and shown, and it is asked again. The batch number says whether
 * it is the stock on show (carry on from what is here) or a new one. */
static int day_reply(void) {
  const char *p = NET;
  char date[12], tags[sizeof NS.tags], *c;
  uint32_t batch = 0;
  int was_more, seed = 0, seed_price = 0;
  if (!str_starts(p, "ok")) return 0;
  tsv_field(p + (p[2] ? 3 : 2), 0, date, sizeof date);
  for (c = date; *c; c++) if (*c == ' ') *c = 0;
  was_more = G.more;                              /* this session is mid-batch */
  G.nitems = -1;
  G.more = 0;
  tags[0] = 0;
  for (p = tsv_next_line(p); *p; p = tsv_next_line(p)) {
    if (str_starts(p, "tags ")) tsv_field(p + 5, 0, tags, sizeof tags);
    else if (str_starts(p, "items ")) { const char *q = p + 6; G.nitems = (int)str_uint(&q); }
    else if (str_starts(p, "batch ")) { const char *q = p + 6; batch = str_uint(&q); }
    else if (str_starts(p, "say ")) {                    /* Tibbs's line: kept as it comes */
      int i = 0;
      const char *q = p + 4;
      while (*q && *q != '\n' && i < (int)sizeof SAY - 1) SAY[i++] = *q++;
      SAY[i] = 0;
      jst_clean(SAY);
      jst_put(api, JST_SAY, SAY, i);
    } else if (str_starts(p, "seed ")) {
      char name[16];
      const char *q;
      int j;
      tsv_field(p + 5, 0, name, sizeof name);
      for (q = name; *q && *q != ' '; q++) {}
      seed = 0;
      for (j = 0; j < JPL_KINDS; j++)
        if (str_starts(name, JS_PLANT_NAME[j]) && (name[slen(JS_PLANT_NAME[j])] == ' ')) seed = j + 1;
      if (*q == ' ') { q++; seed_price = (int)str_uint(&q); }
    }
    else if (str_starts(p, "more")) G.more = 1;
  }
  /* No count is not a count of none: an older server answered a POST for a
   * stock it had already made with "ok DATE" alone, and that replaced the
   * stock with nothing. Ask for the whole answer instead. */
  if (G.nitems < 0) { G.nitems = 0; return 0; }
  G.got = 0;
  if (was_more && NS.batch == batch && str_same(NS.date, date)) {
    /* the batch this session is fetching, grown: on from where it got to,
     * even past items that were refused (they are not fetched again) */
  } else if (S.date[0] && str_same(S.date, date) && S.batch == batch) {
    G.dropped = 0;
    /* the stock on show: fetch only what is not here yet */
    ji_copy(&NS, &S, (int)sizeof NS);
    G.k = S.next > S.n ? S.next : S.n;
    G.live = 1;
  } else {
    G.dropped = 0;
    ji_zero(&NS, (int)sizeof NS);
    ji_copy(NS.date, date, (int)sizeof NS.date);
    NS.batch = batch;
    /* The batch's records are kept under a number that names the day, so a
     * stock file only ever points at records of its own day. */
    NS.gen = (uint8_t)today_gen();
    G.k = 0;
    G.live = 0;
  }
  ji_copy(NS.tags, tags, (int)sizeof NS.tags);
  if (seed != NS.seed) NS.seed_sold = 0;
  NS.seed = (uint8_t)seed;
  NS.seed_price = (uint16_t)seed_price;
  return 1;
}

static void net_reply(int n) {
  int was = U.net;
  U.net = N_IDLE;
  U.dirty = 1;
  if (n >= 0) NET[n] = 0;
  switch (was) {
  case N_PUB:
    if (pub_reply(n)) start_day();
    return;
  case N_DAY_POST:
  case N_DAY_POLL:
    if (G.quiet && (n < 0 || str_starts(NET, "error"))) { net_status(""); G.quiet = 0; return; }
    if (n < 0) { net_status(""); failed("Today's stock", n); return; }
    if (refused("Today's stock")) { net_status(""); return; }
    if (day_reply()) {
      if (G.k < G.nitems) net_status("fetching today's things");
      next_item();
      return;
    }
    if (str_starts(NET, "ok")) {                          /* made, but no count: GET it */
      G.wait_until = api->ticks_ms();
      G.again = 1;
      return;
    }
    /* 15 minutes with nothing new: the server's turns are slow, not dead */
    if (++G.polls > 300) { net_status(""); say("The stock is late today"); return; }
    G.wait_until = api->ticks_ms() + 3000;                /* "pending": ask again */
    G.again = 1;
    net_status("today's stock is being made");
    return;
  case N_ITEM:
    if (n < 0) { net_status(""); failed("Today's things", n); return; }
    G.k++;
    if (take_record(NET, "stock") <= 0) G.dropped++;
    else if (NS.n < JST_STOCK_N) {
      char p[48];
      jst_stock_path(api, NS.gen, NS.n, p, sizeof p);
      if (jst_put(api, p, IO.raw, (int)ji_get16(IO.raw + 2)) == 0) {
        NS.id[NS.n] = IO.it.id;
        NS.price[NS.n] = (uint16_t)jst_price(&IO.it);
        NS.n++;
        NS.next = (uint8_t)G.k;
        G.got++;
        show_batch();                                      /* on show as it comes */
        G.polls = 0;
      }
    }
    next_item();
    return;
  }
}

static void net_tick(uint32_t now) {
  if (U.net) {
    int n = api->http_poll(NET, sizeof NET);
    if (n != CAPP_HTTP_PENDING) net_reply(n);
    return;
  }
  if (G.wait_until && (int32_t)(now - G.wait_until) >= 0) {
    G.wait_until = 0;
    if (G.again) {
      if (send(N_DAY_POLL, "GET", "/jar/day", 0) == 0) G.again = 0;
      else G.wait_until = now + 1000;
    } else if (G.k < G.nitems) next_item();
  }
}

/* ---- painting -------------------------------------------------------------------------- */

static void detail(int x) {
  const Tile *t = &U.card;
  char b[32];
  box(x - 2, BAR + 2, SW - x, BODY - 4, C_PANEL);
  if (!t->ok) {
    wrapped(x + 2, BAR + 8, stuff_mode() ? "Nothing here yet. The shop has more." :
            "Sold. More with the next batch.", 17, 4, C_DIM, C_PANEL);
    return;
  }
  if (t->seed) spr(PLANT_SPR[t->seed - 1], x + 38, BAR + 5, 2, C_PANEL);
  else pic(t->frame, t->pal, 1, 16, 16, x + 36, BAR + 5, 2, C_PANEL);
  textn(x + 2, BAR + 41, U.name, 17, C_TEXT, C_PANEL);
  wrapped(x + 2, BAR + 51, U.line, 17, 2, C_DIM, C_PANEL);
  if (t->seed) api->fmt(b, sizeof b, "you have %d", SEEDS[t->seed - 1]);
  else api->fmt(b, sizeof b, "%s, %s", t->kind == JK_CRITTER ? "critter" : t->kind == JK_HANGING ? "hanging" : "floor",
                MOVE_NAME[t->move % JM_KINDS]);
  textn(x + 2, BAR + 71, b, 17, C_TRAIT, C_PANEL);
  if (stuff_mode())
    text(x + 2, BAR + 84, t->in_jar ? "in the jar" : "in My Stuff", t->in_jar ? C_GOLD : C_DIM, C_PANEL);
  else {
    price(x + 2, BAR + 84, t->price, t->price > J.coins, C_PANEL);
    if (t->held) text(SW - 6 - 4 * 6, BAR + 84, "held", C_TRAIT, C_PANEL);
  }
}

static void new_stock_in(char *b, int n) {
  uint32_t now = U.now, left;
  if (U.net >= N_DAY_POST || G.again) { api->fmt(b, (size_t)n, "New stock arriving..."); return; }
  if (!now) { api->fmt(b, (size_t)n, "No clock: no new stock"); return; }
  left = 86400u - now % 86400u;
  api->fmt(b, (size_t)n, "New stock in %uh %02um", (unsigned)(left / 3600u), (unsigned)(left / 60u % 60u));
}

static void paint_grid(void) {
  int i, stuff = stuff_mode();
  char b[40];
  box(0, BAR, SW, BODY, C_BG);
  if (stuff) api->fmt(b, sizeof b, "%u things", (unsigned)J.nowned);
  else new_stock_in(b, sizeof b);
  textn(4, BAR + 3, b, 20, C_DIM, C_BG);
  for (i = 0; i < 8; i++) {
    const Tile *t = &G.tile[i];
    int x = 4 + (i % 4) * 30, y = BAR + 13 + (i / 4) * 30;
    box(x, y, 28, 28, C_PANEL);
    if (i < G.ntile && t->ok) {
      if (t->seed) spr(PLANT_SPR[t->seed - 1], x + 8, y + 2, 1, C_PANEL);
      else pic(t->frame, t->pal, 1, 16, 16, x + 6, y + 2, 1, C_PANEL);
      if (stuff) {
        if (t->in_jar) box(x + 23, y + 2, 3, 3, C_GOLD);
      } else {
        api->fmt(b, sizeof b, "%u", (unsigned)t->price);
        text(x + 14 - slen(b) * 3, y + 19, b, t->price > J.coins ? C_WARN : C_GOLD, C_PANEL);
        if (t->held) { box(x + 22, y + 2, 4, 4, C_TRAIT); box(x + 23, y + 3, 2, 2, C_PANEL); }
      }
    }
    if (i == G.sel - G.top * 4) outline(x - 1, y - 1, 30, 30, C_GOLD);
  }
  if (!stuff) {
    box(4, BAR + 76, 118, 31, C_PANEL);
    if (SAY[0]) {                                         /* Tibbs, as far as it fits: t for all */
      text(7, BAR + 78, "Tibbs:", C_GOLD, C_PANEL);
      wrapped(7, BAR + 87, SAY, 19, 2, C_DIM, C_PANEL);
    } else {
      text(7, BAR + 78, S.date[0] ? "Tibbs's shop" : "The hand-made stock", C_TRAIT, C_PANEL);
      wrapped(7, BAR + 87, "t to talk to him", 19, 2, C_DIM, C_PANEL);
    }
  } else if (J.nowned > 8) {
    api->fmt(b, sizeof b, "%d/%d", G.top + 1, (J.nowned + 3) / 4 - 1);
    text(122 - slen(b) * 6, BAR + 3, b, C_DIM, C_BG);
  }
  detail(128);
}

static void paint_up(void) {
  int k;
  char b[40], r1[16], r2[16];
  box(0, BAR, SW, BODY, C_BG);
  for (k = 0; k < JU_KINDS; k++) {
    int y = BAR + 3 + k * 26, cost = js_up_cost(&J, k), lv = js_up_level(&J, k);
    box(4, y, SW - 8, 24, C_PANEL);
    if (k == G.up_sel) outline(4, y, SW - 8, 24, C_GOLD);
    switch (k) {
    case JU_MOSS: spr(SPR_MOSS, 9, y + 7, 1, C_PANEL); break;
    case JU_MACH: spr(SPR_SPOOL, 8, y + 4, 1, C_PANEL); break;
    case JU_BELT: box(7, y + 10, 6, 3, CAPP_RGB(0xc8, 0x42, 0x3a)); box(14, y + 10, 6, 3, CAPP_RGB(0x4a, 0x7a, 0xc8)); break;
    default:      spr(SPR_BUSH, 8, y + 5, 1, C_PANEL); break;
    }
    text(26, y + 4, UP_NAME[k], C_TEXT, C_PANEL);
    if (cost < 0) {
      api->fmt(b, sizeof b, "all %d", js_up_max(k) + (k == JU_BELT));
      text(26, y + 14, b, C_DIM, C_PANEL);
      text(SW - 34, y + 9, "max", C_DIM, C_PANEL);
      continue;
    }
    {
      int32_t r = js_rate_ph(&J) * 10 / 60, n = js_rate_after(&J, k) * 10 / 60;
      api->fmt(r1, sizeof r1, "%d.%d", (int)(r / 10), (int)(r % 10));
      api->fmt(r2, sizeof r2, "%d.%d", (int)(n / 10), (int)(n % 10));
    }
    if (k == JU_MACH) api->fmt(b, sizeof b, "%s  %s->%s/min", lv == 2 ? "press" : "spool", r1, r2);
    else api->fmt(b, sizeof b, "%d of %d  %s->%s/min", lv + (k == JU_BELT), js_up_max(k) + (k == JU_BELT), r1, r2);
    text(26, y + 14, b, C_DIM, C_PANEL);
    price(SW - 46, y + 9, cost, (uint32_t)cost > J.coins, C_PANEL);
  }
}

static void paint_garden(void) {
  int i, x;
  char b[40];
  box(0, BAR, SW, BODY, C_BG);
  for (i = 0; i < JS_MAX_BEDS; i++) {
    int bx = 4 + i * 39, y = BAR + 4, has = i < J.nbeds, d = js_bed_days(&J.bed[i], U.now);
    const JBed *bd = &J.bed[i];
    box(bx, y, 36, 62, C_PANEL);
    box(bx + 2, y + 42, 32, 3, has ? C_SOIL : C_BG);
    if (has) {
      if (bd->young) {                                  /* a shoot, taller by the day */
        int h = d < 0 ? 4 : 4 + (3 - (d > 3 ? 3 : d)) * 4;
        box(bx + 17, y + 42 - h, 2, h, CAPP_RGB(0x5a, 0xa8, 0x48));
        box(bx + 14, y + 42 - h, 3, 2, CAPP_RGB(0x5a, 0xa8, 0x48));
        box(bx + 19, y + 44 - h, 3, 2, CAPP_RGB(0x5a, 0xa8, 0x48));
      } else spr(PLANT_SPR[bd->type % JPL_KINDS], bx + 6, y + 14, 2, C_PANEL);
      text(bx + 18 - slen(SHORT[bd->type % JPL_KINDS]) * 3, y + 46, SHORT[bd->type % JPL_KINDS], C_TEXT, C_PANEL);
      if (!bd->young) api->fmt(b, sizeof b, "grown");
      else if (d < 0) api->fmt(b, sizeof b, "paused");
      else api->fmt(b, sizeof b, "%d day%s", d, d == 1 ? "" : "s");
      text(bx + 18 - slen(b) * 3, y + 54, b, bd->young ? C_DIM : C_TRAIT, C_PANEL);
    } else text(bx + 4, y + 26, "U:buy", C_DIM, C_PANEL);
    if (i == G.bed) outline(bx - 1, y - 1, 38, 64, C_GOLD);
  }
  /* Seeds in hand: the shop sells packets now and then. */
  text(6, BAR + 71, "Seeds:", C_DIM, C_BG);
  for (i = 0, x = 6 + 7 * 6; i < JPL_KINDS; i++) {
    char b[8];
    spr(PLANT_SPR[i], x, BAR + 68, 1, C_BG);
    api->fmt(b, sizeof b, "%d", SEEDS[i]);
    text(x + 13, BAR + 71, b, SEEDS[i] ? C_GOLD : C_DIM, C_BG);
    x += 36;
  }
  text(6, BAR + 91, "Plants take 3 real days; grown", C_DIM, C_BG);
  text(6, BAR + 99, "ones make jam. Seeds: the shop.", C_DIM, C_BG);
}

static void paint_plant(void) {
  int i;
  char b[48];
  box(0, BAR, SW, BODY, C_BG);
  api->fmt(b, sizeof b, "Bed %d: plant what?", G.bed + 1);
  text(6, BAR + 3, b, C_TEXT, C_BG);
  for (i = 0; i < JPL_KINDS; i++) {
    int y = BAR + 13 + i * 18;
    box(4, y, SW - 8, 17, C_PANEL);
    if (i == G.plant) outline(4, y, SW - 8, 17, C_GOLD);
    pic(SPRITES[PLANT_SPR[i]].px, SPRITES[PLANT_SPR[i]].pal, 0, 12, 14, 7, y + 1, 1, C_PANEL);
    text(24, y + 1, JS_PLANT_NAME[i], C_TEXT, C_PANEL);
    api->fmt(b, sizeof b, "%d seed%s", SEEDS[i], SEEDS[i] == 1 ? "" : "s");
    text(SW - 10 - slen(b) * 6, y + 5, b, SEEDS[i] ? C_GOLD : C_DIM, C_PANEL);
  }
  if (G.asking) {
    box(20, BAR + 40, SW - 40, 30, C_BG);
    outline(20, BAR + 40, SW - 40, 30, C_GOLD);
    api->fmt(b, sizeof b, "Pull up the grown %s?", JS_PLANT_NAME[J.bed[G.bed].type % JPL_KINDS]);
    text(28, BAR + 45, b, C_TEXT, C_BG);
    text(28, BAR + 57, "y yes   n no", C_DIM, C_BG);
  }
}

static void shop_paint(void *st, CRect c) {
  static const char *const H_STOCK[] = { "Ent", "buy", "Spc", "hold", "T", "Tibbs", "Tab", "stuff", 0 };
  static const char *const H_STUFF[] = { "Ent", "open", "G", "gift", "Tab", "shop", "Esc", "jar", 0 };
  static const char *const H_PICK[] = { "Ent", "choose", "Esc", "back", 0 };
  static const char *const H_CARD[] = { "Ent", "in jar", "G", "gift", "Esc", "back", 0 };
  static const char *const H_UP[] = { "^v", "pick", "Ent", "buy", "Esc", "jar", 0 };
  static const char *const H_GARDEN[] = { "<>", "bed", "Ent", "plant", "Esc", "jar", 0 };
  static const char *const H_PLANT[] = { "^v", "pick", "Ent", "plant", "Esc", "back", 0 };
  const char *const *h = H_STOCK;
  const char *where = "Shop";
  (void)st; (void)c;
  switch (G.view) {
  case V_STOCK:  paint_grid(); break;
  case V_STUFF:  paint_grid(); h = G.pick ? H_PICK : H_STUFF;
                 where = G.pick == P_DECOR ? "Add to the jar" : "My Stuff"; break;
  case V_CARD:   paint_card(0, 0); h = H_CARD; where = "Item"; break;
  case V_UP:     paint_up(); h = H_UP; where = "Upgrades"; break;
  case V_GARDEN: paint_garden(); h = H_GARDEN; where = "Garden"; break;
  case V_PLANT:  paint_plant(); h = H_PLANT; where = "Garden"; break;
  }
  top_bar(where);
  hints(h);
  paint_note();
}

/* ---- keys ------------------------------------------------------------------------------- */

static void grid_move(int d) {
  int s = G.sel + d;
  if (s < 0 || s >= page_count()) return;
  G.sel = s;
  while (G.sel < G.top * 4) G.top--;
  while (G.sel >= (G.top + 2) * 4) G.top++;
  load_page();
}

/* Buy stock slot G.sel: its record (as it came) becomes an owned item. Its
 * id, or 0 when it was not bought. */
static uint32_t buy(void) {
  int k = G.sel, n;
  uint32_t id;
  char p[48];
  if (k == S.n && S.seed && !S.seed_sold) {              /* the seed packet: seeds, not a thing */
    if (J.coins < S.seed_price) return 0;
    js_spend(&J, S.seed_price);
    SEEDS[S.seed - 1]++;
    jst_seeds_save(api, SEEDS, TEXT, sizeof TEXT);
    S.seed_sold = 1;
    jst_stock_save(api, &S, TEXT, sizeof TEXT);
    save();
    api->fmt(p, sizeof p, "%s seeds: plant them in the Garden", JS_PLANT_NAME[S.seed - 1]);
    say(p);
    load_page();
    return 0;
  }
  if (k >= S.n || jst_sold(&S, &J, k)) return 0;
  if (J.coins < S.price[k]) return 0;             /* the price is in red already */
  n = jst_stock_read(api, &S, k, &IO);
  if (n < 0) { say("That item is missing"); return 0; }
  id = IO.it.id;
  if (js_owns(&J, id)) { say("You have that one"); return 0; }
  if (js_own(&J, id)) { say("My Stuff is full: 64"); return 0; }
  jst_item_path(api, id, p, sizeof p);
  if (jst_put(api, p, IO.raw, n) != 0) { J.nowned--; say("Could not write to the card"); return 0; }
  js_spend(&J, S.price[k]);
  if (!S.date[0]) J.sold |= 1u << (id & 31);
  else {
    S.sold = (uint8_t)(S.sold | (1u << k));
    S.held = (uint8_t)(S.held & ~(1u << k));          /* bought: no longer held */
    jst_stock_save(api, &S, TEXT, sizeof TEXT);
  }
  save();
  load_page();
  return id;
}

/* A gift is Jar Post's: it opens there, on this item. */
static void gift(uint32_t id) {
  char a[24];
  save();
  api->fmt(a, sizeof a, "gift %u", (unsigned)id);
  if (ui_run("Jar Post", a) != 0) say("No Jar Post app");
}

static int key_grid(int k) {
  switch (k) {
  case CAPP_KEY_LEFT:  grid_move(-1); return 1;
  case CAPP_KEY_RIGHT: grid_move(1); return 1;
  case CAPP_KEY_UP:    grid_move(-4); return 1;
  case CAPP_KEY_DOWN:  grid_move(4); return 1;
  case 0x09:
    if (G.view == V_STOCK) go_stuff(P_BROWSE);
    else if (!G.pick) go_stock();
    return 1;
  case CAPP_KEY_ESC:
    to_jar();
    return 1;
  case ' ':
    /* Hold: it stays in the shop, through new stocks, until let go or bought.
     * Only the server's stock: the hand-made one never changes anyway. */
    if (G.view == V_STOCK && U.card.ok && S.date[0] && G.sel < S.n) {
      int k = G.sel, n = 0, i;
      for (i = 0; i < S.n; i++) n += S.held >> i & 1;
      if (!(S.held >> k & 1) && n >= 4) { say("You can hold 4"); return 1; }
      S.held = (uint8_t)(S.held ^ (1u << k));
      jst_stock_save(api, &S, TEXT, sizeof TEXT);
      say(S.held >> k & 1 ? "Held: it stays till you let go" : "Let go: it may move on");
      load_page();
    }
    return 1;
  case 'g': case 'G':
    if (G.view == V_STOCK) { uint32_t id = U.card.ok ? buy() : 0; if (id) gift(id); }
    else if (U.card.ok && !G.pick) gift(U.card.id);
    return 1;
  case CAPP_KEY_ENTER:
    if (!U.card.ok) return 1;
    if (G.view == V_STOCK) { uint32_t id = buy(); if (id) open_card(id, V_STOCK); return 1; }
    if (G.pick == P_DECOR) { J.decor = U.card.id; to_jar(); return 1; }
    open_card(U.card.id, V_STUFF);
    return 1;
  }
  return 1;
}

static int key_card(int k) {
  if (card_key(k)) return 1;
  if (k == 'g' || k == 'G') { gift(U.card.id); return 1; }
  if (k == CAPP_KEY_ESC) {
    G.view = G.back;
    load_page();
  }
  return 1;
}

static int key_up(int k) {
  switch (k) {
  case CAPP_KEY_UP:   if (G.up_sel > 0) G.up_sel--; return 1;
  case CAPP_KEY_DOWN: if (G.up_sel < JU_KINDS - 1) G.up_sel++; return 1;
  case CAPP_KEY_ENTER: {
    int r = js_buy(&J, G.up_sel);
    if (r == 0) { save(); say("Bought: it is in the jar"); }
    else if (r == -2) say("Not enough coins yet");
    return 1;
  }
  case CAPP_KEY_ESC: to_jar(); return 1;
  }
  return 1;
}

static void do_plant(void) {
  int t = G.plant;
  if (SEEDS[t] <= 0) { say("No seeds: the shop has them now and then"); return; }
  SEEDS[t]--;
  jst_seeds_save(api, SEEDS, TEXT, sizeof TEXT);
  js_plant(&J, G.bed, t, api->epoch());
  save();
  say(api->epoch() ? "Planted: grown in 3 days" : "Planted: it grows once there is a clock");
  G.view = V_GARDEN;
}

static int key_garden(int k) {
  if (G.view == V_PLANT) {
    if (G.asking) {
      int a = confirm_key(api, (uint8_t)k);
      if (a == CONFIRM_YES) do_plant();
      if (a != CONFIRM_WAIT) G.asking = 0;
      return 1;
    }
    switch (k) {
    case CAPP_KEY_UP:   if (G.plant > 0) G.plant--; return 1;
    case CAPP_KEY_DOWN: if (G.plant < JPL_KINDS - 1) G.plant++; return 1;
    case CAPP_KEY_ESC:  G.view = V_GARDEN; return 1;
    case CAPP_KEY_ENTER:
      if (SEEDS[G.plant] <= 0) { say("No seeds: the shop has them now and then"); return 1; }
      if (!J.bed[G.bed].young) G.asking = 1;     /* a grown plant: ask first */
      else do_plant();
      return 1;
    }
    return 1;
  }
  switch (k) {
  case CAPP_KEY_LEFT:  if (G.bed > 0) G.bed--; return 1;
  case CAPP_KEY_RIGHT: if (G.bed < JS_MAX_BEDS - 1) G.bed++; return 1;
  case CAPP_KEY_ENTER:
    if (G.bed >= J.nbeds) { say("More beds are in Upgrades"); return 1; }
    G.view = V_PLANT;
    G.plant = J.bed[G.bed].type % JPL_KINDS;
    return 1;
  case CAPP_KEY_ESC: to_jar(); return 1;
  }
  return 1;
}

static int shop_key(void *st, uint8_t k) {
  (void)st;
  U.dirty = 1;
  if (U.msg[0]) U.msg[0] = 0;
  if ((k == 't' || k == 'T') && (G.view == V_STOCK || G.view == V_STUFF)) {
    save();                                       /* Tibbs is Jar Post's */
    if (ui_run("Jar Post", "talk") != 0) say("No Jar Post app");
    return 1;
  }
  /* For trying things out: r asks the server for a new stock now (it makes
   * one even though today's is done), $ is 1000 coins. */
  if (k == 'r' && (G.view == V_STOCK || G.view == V_STUFF)) {
    if (U.net) { say("Already asking the server"); return 1; }
    if (!api->epoch()) { say("No clock yet: no stock to refresh"); return 1; }
    s_fresh = 1;
    G.tried = 0;
    start_day();
    return 1;
  }
  if (k == '$') {
    J.coins += 1000;
    save();
    say("+1000 coins (for testing)");
    return 1;
  }
  switch (G.view) {
  case V_STOCK: case V_STUFF: return key_grid(k);
  case V_CARD:    return key_card(k);
  case V_UP:      return key_up(k);
  case V_GARDEN: case V_PLANT: return key_garden(k);
  }
  return 0;
}

static int shop_tick(void *st, uint32_t now) {
  (void)st;
  net_tick(now);
  if (ui_clock() && (G.view == V_STOCK || G.view == V_GARDEN)) U.dirty = 1;
  if (U.msg[0] && (int32_t)(now - U.msg_until) > 0) { U.msg[0] = 0; U.dirty = 1; }
  if (!U.dirty) return 0;
  U.dirty = 0;
  return 1;
}

/* ---- opening ------------------------------------------------------------------------------ */

/* The shop on SCREEN: shop (the default), stuff, decor, up, garden ("shelf",
 * which is gone, is My Stuff). A screen of Jar Factory (apps/jar.c), which
 * routes its paint, keys and ticks here while it is on. */
static void shop_open(const char *s) {
  if (!s) s = "";
  api->mem_set(&G, 0, sizeof G);
  api->mem_set(&U, 0, sizeof U);
  if (ui_load()) return;
  if (jst_get(api, JST_SAY, SAY, sizeof SAY) < 0) SAY[0] = 0;
  jst_clean(SAY);
  jst_seeds_load(api, SEEDS, TEXT, sizeof TEXT);
  if (str_same(s, "stuff")) go_stuff(P_BROWSE);
  else if (str_same(s, "decor")) go_stuff(P_DECOR);
  else if (str_same(s, "up")) G.view = V_UP;
  else if (str_same(s, "garden")) G.view = V_GARDEN;
  else if (str_same(s, "shelf")) go_stuff(P_BROWSE);   /* the shelf is gone */
  else go_stock();
}

#endif /* CARDOS_JARSHOP_H */
