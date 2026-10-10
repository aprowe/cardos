/* Jar Factory on the card: what apps/jar.c (the jar) and its companions,
 * apps/jarshop.c (the shop, the garden, the shelf) and apps/jarpost.c
 * (friends, gifts, mail), all read and write. The jar saves before it opens
 * a companion and starts afresh from the save when it comes back, so no two
 * of them hold the jar at once.
 *
 *   /var/jar/jar.txt        the jar: apps/jarsim.h's js_save text
 *   /var/jar/items/ID.itm   one record per owned item (apps/jaritem.h)
 *   /var/jar/stock.txt      the day's stock: date, tags, ids, prices, sold
 *   /var/jar/stock/G_K.itm  its records, as the server signed them
 *   /var/jar/mail.txt       gifts: who, the note, opened, thanked
 *   /var/jar/mail/QID.itm   a parcel not yet opened
 *   /var/jar/friends.txt    the last GET /jar/me reply, for offline
 *   /var/jar/server.pub     the server's key, pinned the first time it is
 *                           fetched (130 hex digits)
 *
 * A record that came from the server is kept byte for byte as it arrived,
 * so its signature still holds when it is sent on as a gift. Every write
 * goes through apps/safefile.h. Header-only, static helpers taking the API,
 * host-tested in test/test_jarstore.c.
 */
#ifndef CARDOS_JARSTORE_H
#define CARDOS_JARSTORE_H

#include "kernel/app/capp.h"
#include "apps/safefile.h"
#include "apps/jarsim.h"
#include "apps/jar_art.h"

#if defined(__GNUC__)
#define JST_OPT __attribute__((unused))
#else
#define JST_OPT
#endif

#define JST_DIR       CAPP_VAR "/jar"
#define JST_ITEMS     CAPP_VAR "/jar/items"
#define JST_SAVE      CAPP_VAR "/jar/jar.txt"
#define JST_STOCK     CAPP_VAR "/jar/stock.txt"
#define JST_STOCKDIR  CAPP_VAR "/jar/stock"
#define JST_MAIL      CAPP_VAR "/jar/mail.txt"
#define JST_MAILDIR   CAPP_VAR "/jar/mail"
#define JST_FRIENDS   CAPP_VAR "/jar/friends.txt"
#define JST_PUB       CAPP_VAR "/jar/server.pub"
#define JST_SAY       CAPP_VAR "/jar/say.txt"    /* Tibbs's line of the day */
#define JST_SEEDS     CAPP_VAR "/jar/seeds.txt"  /* seed packets: a count a plant */

#define JST_POSTAGE   10
#define JST_NOTE      24
#define JST_DNAME     8

/* A record's bytes and what they decode to: about 2 KB, so an app keeps one. */
typedef struct { uint8_t raw[JI_MAX]; JItem it; } JIo;

static JST_OPT int jst_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static JST_OPT void jst_dirs(const CardApi *api) {
  api->mkdir(CAPP_VAR);
  api->mkdir(JST_DIR);
  api->mkdir(JST_ITEMS);
  api->mkdir(JST_STOCKDIR);
  api->mkdir(JST_MAILDIR);
}

/* ---- whole files ----------------------------------------------------------- */

/* `n` bytes to `path`, safely. 0 or -1. */
static JST_OPT int jst_put(const CardApi *api, const char *path, const void *d, int n) {
  SafeFile f;
  if (safe_begin(&f, api, path)) return -1;
  safe_write(&f, (const char *)d, (size_t)n);
  return safe_commit(&f);
}

/* `path` into `buf`, at most cap - 1 bytes and a NUL after. The bytes, or -1
 * if there is no such file. */
static JST_OPT int jst_get(const CardApi *api, const char *path, void *buf, int cap) {
  int fd = safe_open_read(api, path), n;
  if (fd < 0) return -1;
  n = api->read(fd, buf, (size_t)(cap - 1));
  api->close(fd);
  if (n < 0) n = 0;
  ((char *)buf)[n] = 0;
  return n;
}

/* ---- items ------------------------------------------------------------------ */

static JST_OPT void jst_item_path(const CardApi *api, uint32_t id, char *p, int n) {
  api->fmt(p, (size_t)n, JST_ITEMS "/%u.itm", (unsigned)id);
}

/* io->it encoded and written as owned item io->it.id. For records made here
 * (the built-ins); a server's record is written as it came, with jst_put. */
static JST_OPT int jst_item_write(const CardApi *api, JIo *io) {
  char p[48];
  int n = jitem_encode(&io->it, io->raw, JI_MAX);
  if (n < 0) return -1;
  jst_item_path(api, io->it.id, p, sizeof p);
  return jst_put(api, p, io->raw, n);
}

/* A record from `path` into io (raw and decoded). Its length, or -1. */
static JST_OPT int jst_read_rec(const CardApi *api, const char *path, JIo *io) {
  int fd = safe_open_read(api, path), n;
  if (fd < 0) return -1;
  n = api->read(fd, io->raw, JI_MAX);
  api->close(fd);
  if (n <= 0 || jitem_decode(&io->it, io->raw, n) != 0) return -1;
  return n;
}

/* Owned item `id` into io. Its length, or -1. */
static JST_OPT int jst_item_read(const CardApi *api, uint32_t id, JIo *io) {
  char p[48];
  jst_item_path(api, id, p, sizeof p);
  return jst_read_rec(api, p, io);
}

/* A built-in, as the record it would be. */
static JST_OPT void jst_from_builtin(const JBuiltin *b, JItem *it) {
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
    s += jst_len(s) + 1;
  }
  for (i = 0; i < b->nframes; i++) ji_copy(it->frames[i], b->frames + i * JI_FRAME_BYTES, JI_FRAME_BYTES);
}

static JST_OPT const JBuiltin *jst_builtin(uint32_t id) {
  int i;
  for (i = 0; i < JB_COUNT; i++) if (BUILTINS[i].id == id) return &BUILTINS[i];
  return 0;
}

/* What a shop asks for an item the server made: the record carries no
 * price, so it is worked out from the item -- the same on every device, 45
 * to 200 coins (the spec's range), dearer for a critter and for each frame
 * of animation. */
static JST_OPT int jst_price(const JItem *it) {
  uint32_t h = it->id * 2654435761u;
  if (it->price5) return it->price5 * 5;          /* the server chose it */
  int p = 45 + (int)((h >> 24) % 70u);
  if (it->kind == JK_CRITTER) p += 45;
  p += (it->nframes > 1 ? it->nframes - 1 : 0) * 12;
  if (p > 200) p = 200;
  return p - p % 5;
}

/* ---- the jar ------------------------------------------------------------------ */

/* The saved jar into `j`, which the caller has js_init'ed. 0, or -1 when
 * there is no save (a first run). `text` is the caller's 2 KB. */
static JST_OPT int jst_load(const CardApi *api, Jar *j, char *text, int cap) {
  int n = jst_get(api, JST_SAVE, text, cap);
  if (n <= 0) return -1;
  return js_load(j, text);
}

static JST_OPT int jst_save(const CardApi *api, Jar *j, char *text, int cap) {
  uint32_t now = api->epoch();
  int n;
  if (now) j->seen = now;
  n = js_save(j, text, cap);
  return jst_put(api, JST_SAVE, text, n);
}

/* ---- the day's stock ---------------------------------------------------------- */

#define JST_STOCK_N 8

typedef struct {
  char     date[12];                   /* the server's day; "" the built-in batch */
  char     tags[48];                   /* the Today card */
  uint8_t  n, gen, sold;               /* sold: a bit per slot */
  uint8_t  held;                       /* a bit per slot: kept into the next stock */
  uint8_t  next;                       /* the server's items fetched (some may be refused) */
  uint32_t batch;                      /* the server's number for this stock */
  uint8_t  seed, seed_sold;            /* a seed packet: plant + 1, 0 none */
  uint16_t seed_price;
  uint32_t id[JST_STOCK_N];
  uint16_t price[JST_STOCK_N];
} JStock;

static JST_OPT void jst_stock_path(const CardApi *api, int gen, int k, char *p, int n) {
  api->fmt(p, (size_t)n, JST_STOCKDIR "/%d_%d.itm", gen, k);
}

/* The built-in batch: the stock before the server has given one. */
static JST_OPT void jst_stock_builtin(JStock *s) {
  int i;
  ji_zero(s, (int)sizeof *s);
  for (i = 0; i < JB_COUNT && s->n < JST_STOCK_N; i++)
    if (BUILTINS[i].price) {
      s->id[s->n] = BUILTINS[i].id;
      s->price[s->n] = BUILTINS[i].price;
      s->n++;
    }
  ji_copy(s->tags, "cosy, garden, glow", 19);
}

static JST_OPT void jst_word(const char **pp, char *out, int n) {
  const char *p = *pp;
  int i = 0;
  while (*p == ' ') p++;
  while (*p && *p != '\n' && i < n - 1) out[i++] = *p++;
  out[i] = 0;
  while (*p && *p != '\n') p++;
  *pp = p;
}

/* stock.txt into s; the built-in batch when there is none. */
static JST_OPT void jst_stock_load(const CardApi *api, JStock *s, char *text, int cap) {
  const char *p = text;
  if (jst_get(api, JST_STOCK, text, cap) <= 0) { jst_stock_builtin(s); return; }
  ji_zero(s, (int)sizeof *s);
  while (*p) {
    const char *w = p;
    while (*p && *p != ' ' && *p != '\n') p++;
    if (js_word_is(w, "date")) jst_word(&p, s->date, sizeof s->date);
    else if (js_word_is(w, "tags")) jst_word(&p, s->tags, sizeof s->tags);
    else if (js_word_is(w, "gen")) s->gen = (uint8_t)js_num(&p);
    else if (js_word_is(w, "sold")) s->sold = (uint8_t)js_num(&p);
    else if (js_word_is(w, "held")) s->held = (uint8_t)js_num(&p);
    else if (js_word_is(w, "next")) s->next = (uint8_t)js_num(&p);
    else if (js_word_is(w, "batch")) s->batch = js_num(&p);
    else if (js_word_is(w, "seed")) {
      s->seed = (uint8_t)js_num(&p);
      s->seed_price = (uint16_t)js_num(&p);
      s->seed_sold = (uint8_t)js_num(&p);
      if (s->seed > JPL_KINDS) s->seed = 0;
    }
    else if (js_word_is(w, "item") && s->n < JST_STOCK_N) {
      s->id[s->n] = js_num(&p);
      s->price[s->n] = (uint16_t)js_num(&p);
      s->n++;
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }
  if (!s->date[0] && !s->n) jst_stock_builtin(s);
}

static JST_OPT int jst_stock_save(const CardApi *api, const JStock *s, char *text, int cap) {
  int n, i;
  n = api->fmt(text, (size_t)cap, "date %s\ntags %s\ngen %u\nsold %u\nheld %u\nnext %u\nbatch %u\n",
               s->date, s->tags, (unsigned)s->gen, (unsigned)s->sold, (unsigned)s->held,
               (unsigned)s->next, (unsigned)s->batch);
  if (s->seed)
    n += api->fmt(text + n, (size_t)(cap - n), "seed %u %u %u\n", (unsigned)s->seed,
                  (unsigned)s->seed_price, (unsigned)s->seed_sold);
  for (i = 0; i < s->n && n < cap - 32; i++)
    n += api->fmt(text + n, (size_t)(cap - n), "item %u %u\n", (unsigned)s->id[i], (unsigned)s->price[i]);
  return jst_put(api, JST_STOCK, text, n);
}

/* Is slot k sold? The built-in batch keeps its sold bits in the jar, by id
 * (they were bought before there was a stock file). */
static JST_OPT int jst_sold(const JStock *s, const Jar *j, int k) {
  if (!s->date[0]) return (j->sold >> (s->id[k] & 31)) & 1;
  return (s->sold >> k) & 1;
}

/* Slot k's record into io: a built-in made afresh, or the server's as it
 * came. Its length, or -1. */
static JST_OPT int jst_stock_read(const CardApi *api, const JStock *s, int k, JIo *io) {
  char p[48];
  if (k < 0 || k >= s->n) return -1;
  if (!s->date[0]) {
    const JBuiltin *b = jst_builtin(s->id[k]);
    if (!b) return -1;
    jst_from_builtin(b, &io->it);
    return jitem_encode(&io->it, io->raw, JI_MAX);
  }
  jst_stock_path(api, s->gen, k, p, sizeof p);
  return jst_read_rec(api, p, io);
}

/* ---- seed packets ------------------------------------------------------------------ */

/* Seeds in hand, a count a plant, into s. A card that has never had the file
 * starts with one of each (the garden used to be planted for coins). */
static JST_OPT void jst_seeds_load(const CardApi *api, int *s, char *text, int cap) {
  const char *p = text;
  int i;
  if (jst_get(api, JST_SEEDS, text, cap) <= 0) {
    for (i = 0; i < JPL_KINDS; i++) s[i] = 1;
    return;
  }
  for (i = 0; i < JPL_KINDS; i++) s[i] = (int)js_num(&p);
}

static JST_OPT int jst_seeds_save(const CardApi *api, const int *s, char *text, int cap) {
  int n = 0, i;
  for (i = 0; i < JPL_KINDS; i++) n += api->fmt(text + n, (size_t)(cap - n), "%d ", s[i]);
  return jst_put(api, JST_SEEDS, text, n);
}

/* ---- mail ----------------------------------------------------------------------- */

#define JST_MAIL_MAX 16

typedef struct {
  uint32_t qid, item, at;
  uint8_t  opened, thanked;
  char     from[JI_WHO + 1];
  char     note[JST_NOTE + 1];
} JMail;

static JST_OPT void jst_parcel_path(const CardApi *api, uint32_t qid, char *p, int n) {
  api->fmt(p, (size_t)n, JST_MAILDIR "/%u.itm", (unsigned)qid);
}

static JST_OPT void jst_field(const char **pp, char *out, int n) {
  const char *p = *pp;
  int i = 0;
  while (*p && *p != '\t' && *p != '\n') { if (i < n - 1) out[i++] = *p; p++; }
  out[i] = 0;
  if (*p == '\t') p++;
  *pp = p;
}

static JST_OPT uint32_t jst_ufield(const char **pp) {
  char b[12];
  const char *q = b;
  jst_field(pp, b, sizeof b);
  return js_num(&q);
}

/* mail.txt into m (newest last); how many. */
static JST_OPT int jst_mail_load(const CardApi *api, JMail *m, char *text, int cap) {
  const char *p = text;
  int n = 0;
  if (jst_get(api, JST_MAIL, text, cap) <= 0) return 0;
  while (*p && n < JST_MAIL_MAX) {
    JMail *e = &m[n];
    if (*p == '\n') { p++; continue; }
    e->qid = jst_ufield(&p);
    e->item = jst_ufield(&p);
    e->at = jst_ufield(&p);
    e->opened = (uint8_t)jst_ufield(&p);
    e->thanked = (uint8_t)jst_ufield(&p);
    jst_field(&p, e->from, sizeof e->from);
    jst_field(&p, e->note, sizeof e->note);
    while (*p && *p != '\n') p++;
    if (e->item) n++;
  }
  return n;
}

static JST_OPT int jst_mail_save(const CardApi *api, const JMail *m, int n, char *text, int cap) {
  int k = 0, i;
  text[0] = 0;
  for (i = 0; i < n && k < cap - 80; i++)
    k += api->fmt(text + k, (size_t)(cap - k), "%u\t%u\t%u\t%u\t%u\t%s\t%s\n", (unsigned)m[i].qid,
                  (unsigned)m[i].item, (unsigned)m[i].at, (unsigned)m[i].opened,
                  (unsigned)m[i].thanked, m[i].from, m[i].note);
  return jst_put(api, JST_MAIL, text, k);
}

/* Room for one more: the oldest opened gift goes. -1 when all are unopened
 * parcels (they wait on the server meanwhile). */
static JST_OPT int jst_mail_room(JMail *m, int *n) {
  int i;
  if (*n < JST_MAIL_MAX) return 0;
  for (i = 0; i < *n; i++)
    if (m[i].opened) {
      for (; i + 1 < *n; i++) ji_copy(&m[i], &m[i + 1], (int)sizeof m[i]);
      (*n)--;
      return 0;
    }
  return -1;
}

/* A note as it may be kept and sent: printable, no tabs or newlines. */
static JST_OPT void jst_clean(char *s) {
  for (; *s; s++) if ((unsigned char)*s < 32 || (unsigned char)*s > 126) *s = ' ';
}

/* ---- the server's key --------------------------------------------------------- */

static JST_OPT int jst_hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* 130 hex digits into the 65-byte point (0x04, X, Y). 0, or -1. */
static JST_OPT int jst_pub_parse(const char *s, uint8_t *pub) {
  int i;
  while (*s == ' ' || *s == '\n' || *s == '\r') s++;
  for (i = 0; i < 65; i++) {
    int a = jst_hex(s[2 * i]), b = a < 0 ? -1 : jst_hex(s[2 * i + 1]);
    if (b < 0) return -1;
    pub[i] = (uint8_t)(a * 16 + b);
  }
  if (jst_hex(s[130]) >= 0) return -1;            /* longer than a key */
  return pub[0] == 4 ? 0 : -1;
}

/* Is the record in io->raw (n bytes) the server's? 1 yes; 0 no (unsigned,
 * or the signature fails); -1 it could not be checked. `msg` is the
 * caller's JI_MAX bytes of room. */
static JST_OPT int jst_verify(const CardApi *api, const uint8_t *pub, const uint8_t *raw, int n,
                              uint8_t *msg) {
  int m, hdr;
  if (n < JI_HDR || raw[204] != 64) return 0;
  hdr = raw[1];
  m = ji_signed_message(raw, n, msg, JI_MAX);
  if (m < 0) return 0;
  if (!api->sig_verify) return -1;
  return api->sig_verify(pub, msg, (size_t)m, raw + hdr);
}

#endif /* CARDOS_JARSTORE_H */
