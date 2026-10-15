/* Jar Factory: what the lane's deals do to the jar (server/jar/town.py,
 * server/jar/shopkeep.py).
 *
 * A deal struck in talk -- with Tibbs, Pip, Bramble or Wren -- reaches the
 * device as "tx ID OP ARG" lines, in the talk reply and from GET
 * /jar/tx?after=N, which the scene asks after with the post. Each is done
 * once, in order; J.txseen (in the save) is the last done.
 *
 *   pay N        coins to them          get N        coins from them
 *   lose ITEM    a thing handed over    bedup K      bed K (from 0) a level up
 *   use<res> N   spores, nectar or twigs spent
 *   gain<res> N  ... given back (a job that fell through)
 *   remake ITEM  the item was made again -- fetch its new record
 *   give ITEM    a new item of yours -- fetch it
 *
 * The last two need the network: jtx_apply answers JTX_FETCH and leaves
 * txseen where it was, the caller fetches GET /jar/item?id=ITEM, checks
 * it against the server's key and calls jtx_install; a fetch that fails is
 * tried again with the next poll. Needs `api` and `J` (apps/jar/jarui.h, or
 * apps/jar/jar.c) and apps/jar/jarstore.h.
 */
#ifndef CARDOS_JARTX_H
#define CARDOS_JARTX_H

#include "apps/str.h"
#include "apps/jar/jarstore.h"

#if defined(__GNUC__)
#define JTX_OPT __attribute__((unused))
#else
#define JTX_OPT
#endif

enum { JTX_OLD = 0, JTX_DONE, JTX_FETCH, JTX_BAD };

typedef struct { uint32_t id, arg; char op[12]; } JTx;

/* "tx ID OP ARG" (p at the "tx") into t. 0, or -1 if it is not one. */
static JTX_OPT int jtx_parse(const char *p, JTx *t) {
  const char *q;
  int k = 0;
  if (!str_starts(p, "tx ")) return -1;
  q = p + 3;
  while (*q == ' ') q++;
  t->id = str_uint(&q);
  while (*q == ' ') q++;
  while (*q && *q != ' ' && *q != '\n' && k < (int)sizeof t->op - 1) t->op[k++] = *q++;
  t->op[k] = 0;
  while (*q == ' ') q++;
  t->arg = str_uint(&q);
  return t->id && k ? 0 : -1;
}

/* One deal's effect, unless it was done: JTX_DONE (and a line for the
 * player in msg), JTX_OLD, JTX_FETCH (remake or give: t->arg is the item
 * to fetch; nothing is done yet), or JTX_BAD (unknown: skipped). */
static JTX_OPT int jtx_apply(const JTx *t, char *msg, int n) {
  const char *op = t->op;
  uint32_t a = t->arg;
  int r;
  if (t->id <= J.txseen) return JTX_OLD;
  msg[0] = 0;
  if (str_same(op, "remake") || str_same(op, "give")) return JTX_FETCH;
  if (str_same(op, "pay")) {
    J.coins = J.coins > a ? J.coins - a : 0;
    api->fmt(msg, (size_t)n, "You paid %u coins", (unsigned)a);
  } else if (str_same(op, "get")) {
    J.coins += a;
    api->fmt(msg, (size_t)n, "You were paid %u coins", (unsigned)a);
  } else if (str_same(op, "lose")) {
    if (js_owns(&J, a)) {
      char path[48];
      js_disown(&J, a);
      jst_item_path(api, a, path, sizeof path);
      api->remove(path);
      api->fmt(msg, (size_t)n, "Handed over");
    }
  } else if (str_same(op, "bedup")) {
    if (a < JS_MAX_BEDS && js_bed_lv(&J.bed[a]) < JS_BED_LV_MAX) {
      J.bed[a].lv = (uint8_t)(js_bed_lv(&J.bed[a]) + 1);
      api->fmt(msg, (size_t)n, "Bed %u is level %d now", (unsigned)a + 1, J.bed[a].lv);
    }
  } else if (str_starts(op, "use") && (r = js_res_of(op + 3)) >= 0) {
    if (js_res_use(&J, r, a) != 0) J.res[r] = 0;      /* spent elsewhere meanwhile: all of it */
    api->fmt(msg, (size_t)n, "%u %s spent", (unsigned)a, JS_RES_NAME[r]);
  } else if (str_starts(op, "gain") && (r = js_res_of(op + 4)) >= 0) {
    js_res_gain(&J, r, a);
    api->fmt(msg, (size_t)n, "%u %s back", (unsigned)a, JS_RES_NAME[r]);
  } else {
    J.txseen = t->id;
    return JTX_BAD;
  }
  J.txseen = t->id;
  return JTX_DONE;
}

/* A fetched record (raw, n bytes, decoded into it and checked by the
 * caller) for deal t: written over the old one or as a new thing of
 * yours, and the deal done. 0, or -1 if it is not the item asked for or
 * the card refused it. */
static JTX_OPT int jtx_install(const JTx *t, const uint8_t *raw, int n, const JItem *it,
                               char *msg, int mn) {
  char path[48];
  if (it->id != t->arg) return -1;
  jst_dirs(api);
  jst_item_path(api, it->id, path, sizeof path);
  if (jst_put(api, path, raw, n) != 0) return -1;
  if (str_same(t->op, "give")) {
    /* My Stuff full: the record stays on the card (the jar takes it into My
     * Stuff at its next start, js_reown) and the deal waits -- before
     * 2026-10-15 it was marked done and the thing was never seen */
    if (js_own(&J, it->id) != 0) {
      api->fmt(msg, (size_t)mn, "My Stuff is full: %s waits", it->name);
      return -1;
    }
    api->fmt(msg, (size_t)mn, "%s is yours: My Stuff", it->name);
  } else {
    /* what it learned about its neighbours was for the old script */
    api->fmt(path, sizeof path, CAPP_VAR "/jar/patches/%u.pat", (unsigned)it->id);
    api->remove(path);
    api->fmt(msg, (size_t)mn, "%s is back, changed", it->name);
  }
  J.txseen = t->id;
  return 0;
}

/* A duration for people: "45 min", "2h", "2h 10m". Static: one at a time. */
static JTX_OPT const char *jtx_dur(uint32_t s) {
  static char b[16];
  if (s < 3600) api->fmt(b, sizeof b, "%u min", (unsigned)((s + 59) / 60));
  else if (s % 3600 < 60) api->fmt(b, sizeof b, "%uh", (unsigned)(s / 3600));
  else api->fmt(b, sizeof b, "%uh %um", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
  return b;
}

/* What the jar has, for talk: the lines before the words. First what was
 * bought from the shop on show, which the server otherwise hears of only
 * with the next stock -- until then Pip could not find a thing bought an
 * hour ago (2026-10-14). */
static JTX_OPT int jtx_have(char *b, int n) {
  JStock s;
  char t[384];
  int i, k = 0;
  jst_stock_load(api, &s, t, (int)sizeof t);
  for (i = 0; s.date[0] && i < s.n && k < n - 24; i++)
    if (s.sold >> i & 1)
      k += api->fmt(b + k, (size_t)(n - k), "%s%u", k ? "," : "bought ", (unsigned)s.id[i]);
  if (k) b[k++] = '\n';
  k += api->fmt(b + k, (size_t)(n - k), "coins %u\nhave spores=%u nectar=%u twigs=%u\nbeds ",
                (unsigned)J.coins, (unsigned)J.res[JRS_SPORES], (unsigned)J.res[JRS_NECTAR],
                (unsigned)J.res[JRS_TWIGS]);
  if (k >= n) k = n - 1;
  for (i = 0; i < J.nbeds && k < n - 12; i++) {
    k += api->fmt(b + k, (size_t)(n - k), "%s%s%d", i ? "," : "",
                  JS_PLANT_SHORT[J.bed[i].type % JPL_KINDS], js_bed_lv(&J.bed[i]));
    if (k >= n) k = n - 1;
  }
  if (k < n - 1) b[k++] = '\n';
  b[k] = 0;
  return k;
}

#endif /* CARDOS_JARTX_H */
