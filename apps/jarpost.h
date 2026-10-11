/* Jar Post -- Jar Factory's friends, gifts and mail.
 *
 * A screen of Jar Factory (apps/jar.c), opened with post_open(SCREEN):
 * friends, mail, or "gift ID" (from Jar Shop's item card). Esc at a top
 * screen, or fn-`, goes back to the jar. What it shares with Jar Shop
 * (apps/jarshop.c) is apps/jarui.h; the files are apps/jarstore.h's.
 *
 * THE SERVER (spec step 6; server/jar.py), one request at a time, never
 * blocking, the device's own token on each:
 *   GET  /jar/me                  "code CODE", "name NAME", and a
 *                                 "friend NAME\tDISPLAY\tLAST_SEEN\tmutual|waiting"
 *                                 a line; kept in /var/jar/friends.txt
 *   POST /jar/friend?code=CODE    "ok NAME mutual|waiting"
 *   POST /people/name             the display name (8 at most) as the body
 *   POST /jar/gift?to=NAME        "NOTE\nBASE64RECORD\n" -> "ok". Postage
 *                                 is taken when it goes and given back if
 *                                 it does not; the item leaves My Stuff only
 *                                 on "ok". Built-ins cannot travel (the
 *                                 server relays only what it signed).
 *   GET  /q/peek?q=jar.gifts&max=1  one parcel: "ID\tFROM\tAT\tSIZE\n",
 *                                 then "FROM\tNOTE\tBASE64RECORD", then
 *                                 "\n". Checked against the pinned key,
 *                                 kept in /var/jar/mail, and only then
 *                                 POST /q/ack?q=jar.gifts&upto=ID. A record
 *                                 whose signature fails is dropped and
 *                                 logged; one that cannot be checked is
 *                                 left on the server.
 *   POST /jar/thanks?to=NAME&id=ID  T on a gift: a heart on their jar.
 * Offline, sending and collecting say so and nothing changes.
 *
 * Host tests: test/test_jarpost.c (JAR_DUMP=dir writes its screens).
 */

#ifndef CARDOS_JARPOST_H
#define CARDOS_JARPOST_H

#include "apps/jarui.h"

enum { V_FRIENDS = 0, V_GIFT, V_MAIL, V_CARD, V_PICK, V_TALK };
#define PK_MAX 64
enum { IN_NONE = 0, IN_CODE, IN_NAME, IN_NOTE, IN_TALK };
enum { N_ME = N_APP + 8, N_FRIEND, N_NAME, N_GIFT, N_PEEK, N_ACK, N_THANKS, N_SAY, N_HEAR };

/* Talking to Tibbs, the shopkeeper (server/shopkeep.py): the conversation,
 * wrapped into rows when it arrives (never in paint). */
#define TK_ROWS 28
#define TK_COLS 38
#define TK_SAY  120
static int tk_parse(void);
static void start_input(int mode);
static int key_input(int k, int max);
static int TK_N, TK_PENDING;
static uint32_t TK_AGAIN;

#define F_MAX 16

typedef struct { char name[JI_WHO + 1], disp[JI_WHO + 1]; uint32_t seen; uint8_t mutual; } Friend;

typedef struct {
  JMail m[JST_MAIL_MAX];
  char tk[TK_ROWS][TK_COLS + 1];
  uint8_t tk_who[TK_ROWS];                                /* 0 him, 1 you, 2 his day */
  struct {
  int view, in, inlen;
  char input[TK_SAY + 1];
  char note[JST_NOTE + 1];
  /* friends, from /jar/me */
  char code[16], me[JI_WHO + 1];
  Friend fr[F_MAX];
  int nfr, me_ok;
  /* a gift: 0 choose a friend, 1 the note, 2 sure?, 3 posting */
  int gift_step, gift_sel, gift_back;
  uint32_t gift_id;
  char gift_name[JI_NAME + 1];
  /* mail */
  int mail_n, mail_sel, card_mail, tried_mail;
  /* sending from the friends list: who, then which of your things */
  int fsel, npk, psel, ptop;
  uint32_t pk[PK_MAX];
  char pkname[PK_MAX][JI_NAME + 1];
  } g;
} PostMem;

/* In Jar Factory, in the scene's item memory while the post is up (jarui.h). */
#ifdef JAR_ONE_APP
static PostMem *POST_P;
#define POST_MEM (*POST_P)
#else
static PostMem POST_MEM;
#endif
#define M      POST_MEM.m
#define TK     POST_MEM.tk
#define TK_WHO POST_MEM.tk_who
#define G      POST_MEM.g

static int unopened(void) {
  int i, n = 0;
  for (i = 0; i < G.mail_n; i++) if (!M[i].opened) n++;
  return n;
}

/* ---- friends ----------------------------------------------------------------------- */

/* /jar/me's reply (or the copy kept for offline) into the list. */
static void parse_me(const char *p) {
  G.nfr = 0;
  for (; *p; p = tsv_next_line(p)) {
    if (str_starts(p, "code ")) tsv_field(p + 5, 0, G.code, sizeof G.code);
    else if (str_starts(p, "name ")) tsv_field(p + 5, 0, G.me, sizeof G.me);
    else if (str_starts(p, "friend ") && G.nfr < F_MAX) {
      Friend *f = &G.fr[G.nfr];
      char b[16];
      const char *q = b;
      tsv_field(p + 7, 0, f->name, sizeof f->name);
      tsv_field(p + 7, 1, f->disp, sizeof f->disp);
      tsv_field(p + 7, 2, b, sizeof b);
      f->seen = str_uint(&q);
      tsv_field(p + 7, 3, b, sizeof b);
      f->mutual = (uint8_t)str_same(b, "mutual");
      if (f->name[0]) G.nfr++;
    }
  }
}

static void fetch_me(void) {
  if (U.net) return;
  if (send(N_ME, "GET", "/jar/me", 0) == 0) net_status("asking after your friends");
}

static int mutual_n(void) {
  int i, n = 0;
  for (i = 0; i < G.nfr; i++) n += G.fr[i].mutual;
  return n;
}

static Friend *mutual_at(int k) {
  int i;
  for (i = 0; i < G.nfr; i++) if (G.fr[i].mutual && k-- == 0) return &G.fr[i];
  return 0;
}

static const char *who(const Friend *f) { return f ? (f->disp[0] ? f->disp : f->name) : "a friend"; }

/* Friend i's place among the mutual ones (what the gift is addressed by). */
static int mutual_index(int i) {
  int k, n = 0;
  for (k = 0; k < i && k < G.nfr; k++) n += G.fr[k].mutual;
  return n;
}

/* ---- gifts ---------------------------------------------------------------------------- */

static void go_gift(uint32_t id, int back) {
  if (jst_item_read(api, id, &IO) < 0) { say("That item is missing"); return; }
  if (IO.it.flags & JIF_BUILTIN) { say("Hand-made things stay home"); return; }
  if (IO.raw[204] == 0) { say("Only the shop's things can travel"); return; }
  G.gift_id = id;
  ji_copy(G.gift_name, IO.it.name, (int)sizeof G.gift_name);
  tile_from(&U.card, &IO.it);
  G.gift_step = 0;
  G.gift_sel = 0;
  G.gift_back = back;
  G.view = V_GIFT;
  if (!G.me_ok) fetch_me();
  U.dirty = 1;
}

static void send_gift(void) {
  Friend *f = mutual_at(G.gift_sel);
  char path[80], to[40];
  int n, k;
  if (!f) return;
  if (U.net) { say("Busy: try again"); return; }
  if (J.coins < JST_POSTAGE) { say("Postage is 10 coins"); return; }
  n = jst_item_read(api, G.gift_id, &IO);
  if (n < 0) { say("That item is missing"); return; }
  k = put(NET, 0, sizeof NET, G.note);
  k = put(NET, k, sizeof NET, "\n");
  if (b64_encode(IO.raw, n, NET + k, (int)sizeof NET - k - 2) < 0) { say("Too big to post"); return; }
  k += slen(NET + k);
  put(NET, k, sizeof NET, "\n");
  url_enc(to, sizeof to, f->name);
  api->fmt(path, sizeof path, "/jar/gift?to=%s", to);
  if (send(N_GIFT, "POST", path, NET) != 0) { say("Busy: try again"); return; }
  J.coins -= JST_POSTAGE;                 /* in hand, not saved, until the server says */
  G.gift_step = 3;
  net_status("posting");
}

static void gift_done(int ok) {
  char p[48];
  if (!ok) {
    J.coins += JST_POSTAGE;               /* the postage back */
    if (!U.msg[0]) say("Not sent: coins back");
    G.gift_step = 2;
    return;
  }
  js_disown(&J, G.gift_id);
  jst_item_path(api, G.gift_id, p, sizeof p);
  api->remove(p);
  save();
  api->fmt(U.msg, sizeof U.msg, "Sent to %s", who(mutual_at(G.gift_sel)));
  U.msg_until = api->ticks_ms() + 4000;
  G.view = V_FRIENDS;
}

/* ---- mail ------------------------------------------------------------------------------- */

static void start_mail(void) {
  if (G.tried_mail || U.net) return;
  if (need_pub()) return;                   /* the key first; then here again */
  G.tried_mail = 1;
  if (send(N_PEEK, "GET", "/q/peek?q=jar.gifts&max=1", 0) == 0) net_status("collecting the post");
  else G.tried_mail = 0;
}

static void go_mail(void) {
  G.view = V_MAIL;
  G.mail_sel = 0;
  U.dirty = 1;
  start_mail();
}

/* Mail index for row r: newest first. */
static int mail_row(int r) { return G.mail_n - 1 - r; }

/* One parcel off the queue: kept, and then acknowledged. */
static void mail_reply(int n) {
  const char *p = NET, *body, *b64;
  uint32_t qid;
  char from[JI_WHO + 1], note[JST_NOTE + 1], q[64];
  int t = 0, got;
  if (n <= 0 || !NET[0]) { net_status(""); return; }      /* nothing waiting */
  if (CAPP_HTTP_FILLED(n, sizeof NET)) {
    api->log("jar: a parcel too big to collect");
    net_status("");
    return;
  }
  qid = str_uint(&p);
  body = tsv_next_line(NET);
  tsv_field(body, 0, from, sizeof from);
  tsv_field(body, 1, note, sizeof note);
  jst_clean(note);
  for (b64 = body; *b64 && *b64 != '\n' && t < 2; ) if (*b64++ == '\t') t++;
  got = take_record(b64, "a gift");
  if (got == -2) { net_status(""); say("Cannot check parcels yet"); return; }
  if (got > 0) {
    char path[48];
    JMail *e;
    if (jst_mail_room(M, &G.mail_n) != 0) { net_status(""); say("Open some parcels first"); return; }
    jst_parcel_path(api, qid, path, sizeof path);
    if (jst_put(api, path, IO.raw, got) != 0) { net_status(""); return; }
    e = &M[G.mail_n++];
    ji_zero(e, (int)sizeof *e);
    e->qid = qid;
    e->item = IO.it.id;
    e->at = api->epoch();
    ji_copy(e->from, from, (int)sizeof e->from);
    ji_copy(e->note, note, (int)sizeof e->note);
    jst_mail_save(api, M, G.mail_n, TEXT, sizeof TEXT);
    J.parcels = (uint16_t)unopened();
    save();
    api->fmt(U.msg, sizeof U.msg, "A parcel from %s!", from);
    U.msg_until = api->ticks_ms() + 4000;
  }
  api->fmt(q, sizeof q, "/q/ack?q=jar.gifts&upto=%u", (unsigned)qid);
  if (send(N_ACK, "POST", q, 0) != 0) net_status("");
}

/* A parcel opened: its record becomes an owned item, and the card shows it
 * with the note. */
static void open_mail(int i) {
  JMail *e = &M[i];
  if (!e->opened) {
    char path[48], ip[48];
    int n;
    jst_parcel_path(api, e->qid, path, sizeof path);
    n = jst_read_rec(api, path, &IO);
    if (n < 0) { say("That parcel is missing"); return; }
    if (!js_owns(&J, e->item)) {
      if (js_own(&J, e->item)) { say("My Stuff is full: 64"); return; }
      jst_item_path(api, e->item, ip, sizeof ip);
      if (jst_put(api, ip, IO.raw, n) != 0) { J.nowned--; say("Could not write to the card"); return; }
    }
    api->remove(path);
    e->opened = 1;
    jst_mail_save(api, M, G.mail_n, TEXT, sizeof TEXT);
    J.parcels = (uint16_t)unopened();
    save();
  }
  if (card_load(e->item)) return;
  G.card_mail = i;
  G.view = V_CARD;
  U.dirty = 1;
}

static void thank(void) {
  JMail *e;
  char path[96], to[40];
  if (G.card_mail < 0) return;
  e = &M[G.card_mail];
  if (e->thanked) { say("Already thanked"); return; }
  url_enc(to, sizeof to, e->from);
  api->fmt(path, sizeof path, "/jar/thanks?to=%s&id=%u", to, (unsigned)e->item);
  if (send(N_THANKS, "POST", path, 0) == 0) net_status("sending a heart");
  else say("Busy: try again");
}

/* ---- replies ------------------------------------------------------------------------------ */

static void net_reply(int n) {
  int was = U.net;
  U.net = N_IDLE;
  U.dirty = 1;
  if (n >= 0) NET[n] = 0;
  switch (was) {
  case N_PUB:
    if (pub_reply(n)) start_mail();
    return;
  case N_SAY:                                       /* "pending": ask again shortly */
    net_status("");
    if (n < 0) { failed("Tibbs", n); TK_PENDING = 0; return; }
    if (refused("Tibbs")) { TK_PENDING = 0; return; }
    if (str_starts(NET, "busy")) say("He is still talking");
    TK_AGAIN = api->ticks_ms() + 3000;
    return;
  case N_HEAR:
    net_status("");
    if (n < 0) { failed("Tibbs", n); TK_PENDING = 0; return; }
    TK_PENDING = tk_parse();
    if (TK_PENDING) TK_AGAIN = api->ticks_ms() + 3000;
    return;
  case N_ME:
    net_status("");
    if (n < 0) { failed("Friends", n); return; }
    if (refused("Friends")) return;
    parse_me(NET);
    G.me_ok = 1;
    jst_put(api, JST_FRIENDS, NET, n);
    return;
  case N_FRIEND: {
    char name[JI_WHO + 1], *c;
    net_status("");
    if (n < 0) { failed("Adding", n); return; }
    if (refused("Adding")) return;
    tsv_field(NET + (NET[2] ? 3 : 2), 0, name, sizeof name);
    for (c = name; *c; c++) if (*c == ' ') { *c = 0; break; }
    api->fmt(U.msg, sizeof U.msg, "%s: %s", name, str_contains(NET, "mutual", 0) ? "friends now" : "waiting for them");
    U.msg_until = api->ticks_ms() + 5000;
    fetch_me();
    return;
  }
  case N_NAME:
    net_status("");
    if (n < 0) { failed("Name", n); return; }
    if (refused("Name")) return;
    say("Name changed");
    fetch_me();
    return;
  case N_GIFT:
    net_status("");
    if (n < 0) { failed("Not sent", n); gift_done(0); return; }
    if (refused("Not sent")) { gift_done(0); return; }
    gift_done(str_starts(NET, "ok"));
    return;
  case N_PEEK:
    if (n < 0) { net_status(""); failed("Post", n); return; }
    mail_reply(n);
    return;
  case N_ACK:
    if (n < 0) { net_status(""); return; }
    if (send(N_PEEK, "GET", "/q/peek?q=jar.gifts&max=1", 0) != 0) net_status("");
    return;
  case N_THANKS:
    net_status("");
    if (n < 0) { failed("Thank-you", n); return; }
    if (refused("Thank-you")) return;
    if (G.card_mail >= 0) {
      M[G.card_mail].thanked = 1;
      jst_mail_save(api, M, G.mail_n, TEXT, sizeof TEXT);
    }
    say("A heart is on its way");
    return;
  }
}

/* ---- painting -------------------------------------------------------------------------------- */

static void ago(uint32_t seen, char *b, int n) {
  uint32_t now = U.now, d;
  if (!seen) { api->fmt(b, (size_t)n, "never"); return; }
  if (!now || now < seen) { api->fmt(b, (size_t)n, "-"); return; }
  d = now - seen;
  if (d < 120) api->fmt(b, (size_t)n, "now");
  else if (d < 7200) api->fmt(b, (size_t)n, "%um ago", (unsigned)(d / 60u));
  else if (d < 172800u) api->fmt(b, (size_t)n, "%uh ago", (unsigned)(d / 3600u));
  else api->fmt(b, (size_t)n, "%ud ago", (unsigned)(d / 86400u));
}

static void paint_friends(void) {
  int i;
  char b[48], s[16];
  box(0, BAR, SW, BODY, C_BG);
  box(4, BAR + 3, SW - 8, 20, C_PANEL);
  api->fmt(b, sizeof b, "You: %s", G.me[0] ? G.me : "(no name yet)");
  text(8, BAR + 5, b, C_TEXT, C_PANEL);
  api->fmt(b, sizeof b, "your friend code: %s", G.code[0] ? G.code : "?");
  text(8, BAR + 14, b, C_GOLD, C_PANEL);
  if (!G.nfr)
    wrapped(8, BAR + 30, G.code[0] ? "No friends yet. Give them your code; A adds theirs. Gifts flow once both have."
                                   : "Not heard from the server yet.", 37, 4, C_DIM, C_BG);
  for (i = 0; i < G.nfr && i < 9; i++) {
    const Friend *f = &G.fr[i];
    int y = BAR + 27 + i * 9, on = i == G.fsel;
    uint16_t bg = on ? C_PANEL : C_BG;
    if (on) box(4, y - 1, SW - 8, 9, C_PANEL);
    textn(8, y, who(f), 12, on ? C_GOLD : C_TEXT, bg);
    text(86, y, f->mutual ? "friends" : "waiting", f->mutual ? C_TRAIT : C_DIM, bg);
    ago(f->seen, s, sizeof s);
    text(SW - 6 - slen(s) * 6, y, s, C_DIM, bg);
  }
  if (G.in == IN_CODE) input_box("Their friend code:", G.input);
  else if (G.in == IN_NAME) input_box("Your name, up to 8:", G.input);
}

/* Which of your things to send: the shop's, signed (hand-made ones stay home). */
static void paint_pick(void) {
  int i;
  char b[48];
  box(0, BAR, SW, BODY, C_BG);
  api->fmt(b, sizeof b, "Send to %s:", who(&G.fr[G.fsel]));
  text(6, BAR + 3, b, C_DIM, C_BG);
  for (i = 0; i < 9 && G.ptop + i < G.npk; i++) {
    int k = G.ptop + i, y = BAR + 14 + i * 10, on = k == G.psel;
    if (on) box(4, y - 1, 132, 10, C_PANEL);
    text(8, y, G.pkname[k], on ? C_GOLD : C_TEXT, on ? C_PANEL : C_BG);
  }
  box(140, BAR + 14, 96, 92, C_PANEL);
  if (U.card.ok) {
    pic(U.card.frame, U.card.pal, 1, 16, 16, 172, BAR + 22, 2, C_PANEL);
    wrapped(144, BAR + 58, U.line, 15, 4, C_DIM, C_PANEL);
  }
}

/* Your giftable things into the list: shop items, signed, not hand-made. */
static void load_pick(void) {
  int i;
  G.npk = G.psel = G.ptop = 0;
  for (i = 0; i < J.nowned && G.npk < PK_MAX; i++) {
    if (jst_item_read(api, J.owned[i], &IO) < 0) continue;
    if ((IO.it.flags & JIF_BUILTIN) || IO.raw[204] == 0) continue;
    G.pk[G.npk] = IO.it.id;
    ji_copy(G.pkname[G.npk], IO.it.name, JI_NAME + 1);
    G.npk++;
  }
}

static void pick_card(void) {
  U.card.ok = 0;
  if (G.psel < G.npk && jst_item_read(api, G.pk[G.psel], &IO) >= 0) {
    tile_from(&U.card, &IO.it);
    U.card.ok = 1;
    words_from(&IO.it);
  }
}

/* One said thing into rows, wrapped at spaces; the oldest rows go first. */
static void tk_add(const char *s, int n, int who) {
  while (n > 0) {
    int cut = n, i;
    if (n > TK_COLS) {
      for (cut = TK_COLS; cut > 0 && s[cut] != ' '; cut--) {}
      if (cut == 0) cut = TK_COLS;
    }
    if (TK_N == TK_ROWS) {                                /* full: drop the oldest */
      for (i = 1; i < TK_ROWS; i++) { api->mem_cpy(TK[i - 1], TK[i], TK_COLS + 1); TK_WHO[i - 1] = TK_WHO[i]; }
      TK_N--;
    }
    api->mem_cpy(TK[TK_N], s, (size_t)cut);
    TK[TK_N][cut] = 0;
    TK_WHO[TK_N++] = (uint8_t)who;
    s += cut;
    n -= cut;
    while (n > 0 && *s == ' ') { s++; n--; }
  }
}

/* A deal's effect, once: "tx ID pay N" (coins to him), "get N" (coins from
 * him), "lose ITEM" (a thing handed over). Done in order, each id once --
 * J.txseen is in the save. */
static void tk_tx(const char *p) {
  const char *q = p + 3;
  uint32_t id = str_uint(&q), n;
  char what[8];
  if (id <= J.txseen) return;
  while (*q == ' ') q++;
  tsv_field(q, 0, what, sizeof what);
  {
    char *c;
    for (c = what; *c; c++) if (*c == ' ') *c = 0;
  }
  while (*q && *q != ' ') q++;
  while (*q == ' ') q++;
  n = str_uint(&q);
  if (str_same(what, "pay")) {
    J.coins = J.coins > n ? J.coins - n : 0;
    api->fmt(U.msg, sizeof U.msg, "You paid Tibbs %u", (unsigned)n);
  } else if (str_same(what, "get")) {
    J.coins += n;
    api->fmt(U.msg, sizeof U.msg, "Tibbs paid you %u", (unsigned)n);
  } else if (str_same(what, "lose") && js_owns(&J, n)) {
    char path[48];
    js_disown(&J, n);
    jst_item_path(api, n, path, sizeof path);
    api->remove(path);
    api->fmt(U.msg, sizeof U.msg, "Handed over to Tibbs");
  }
  U.msg_until = api->ticks_ms() + 4000;
  J.txseen = id;
  save();
}

/* GET /jar/talk: "pending" | "ok" | "error WHY", then "day\tTEXT", "me\tTEXT"
 * and "him\tTEXT" lines, then the deals' "tx" lines. 1 while he is still
 * answering. */
static int tk_parse(void) {
  const char *p = NET, *e;
  char who[8];
  int pending = str_starts(p, "pending");
  if (str_starts(p, "error")) say(p[5] ? p + 6 : "He did not hear you");
  TK_N = 0;
  for (p = tsv_next_line(p); *p; p = tsv_next_line(p)) {
    if (str_starts(p, "tx ")) { tk_tx(p); continue; }
    tsv_field(p, 0, who, sizeof who);
    e = p;
    while (*e && *e != '\t' && *e != '\n') e++;
    if (*e != '\t') continue;
    e++;
    {
      const char *end = e;
      while (*end && *end != '\n') end++;
      if (str_same(who, "day")) { tk_add(e, (int)(end - e), 2); tk_add("", 0, 2); }
      else if (str_same(who, "me")) tk_add(e, (int)(end - e), 1);
      else if (str_same(who, "him")) tk_add(e, (int)(end - e), 0);
    }
  }
  return pending;
}

static int hear(void) {
  char path[40];
  api->fmt(path, sizeof path, "/jar/talk?tx=%u", (unsigned)J.txseen);
  return send(N_HEAR, "GET", path, 0);
}

static void go_talk(void) {
  G.view = V_TALK;
  start_input(IN_TALK);
  if (!U.net && hear() == 0) net_status("knocking");
}

static void paint_talk(void) {
  int rows = 9, first = TK_N - rows, i, y;
  box(0, BAR, SW, BODY, C_BG);
  if (!TK_N && !TK_PENDING)
    wrapped(8, BAR + 8, "Tibbs is behind the counter. Say something: type, then Enter.", 37, 3, C_DIM, C_BG);
  if (TK_PENDING) first++;
  if (first < 0) first = 0;
  for (i = first, y = BAR + 2; i < TK_N; i++, y += 9) {
    uint16_t c = TK_WHO[i] == 1 ? C_TEXT : TK_WHO[i] == 2 ? C_DIM : C_GOLD;
    if (TK_WHO[i] == 1) text(SW - 4 - slen(TK[i]) * 6, y, TK[i], c, C_BG);   /* you, on the right */
    else text(4, y, TK[i], c, C_BG);
  }
  if (TK_PENDING) text(4, y, "Tibbs: ...", C_DIM, C_BG);
  box(2, SHT - BAR - 13, SW - 4, 12, C_PANEL);
  {
    int n = slen(G.input), from = n > 37 ? n - 37 : 0;   /* the end of what is typed */
    text(5, SHT - BAR - 11, G.input + from, C_TEXT, C_PANEL);
    box(5 + (n - from) * 6, SHT - BAR - 11, 5, 8, C_GOLD);
  }
}

static int key_talk(int k) {
  if (k == CAPP_KEY_ESC) { to_jar(); return 1; }
  if (key_input(k, TK_SAY)) return 1;
  if (!G.inlen) return 1;
  if (TK_PENDING || U.net) { say("He is still talking"); return 1; }
  jst_clean(G.input);
  tk_add(G.input, G.inlen, 1);
  {
    /* what you have first: with it he can strike a deal (a bribe, a trade) */
    char body[TK_SAY + 24];
    api->fmt(body, sizeof body, "coins %u\n%s", (unsigned)J.coins, G.input);
    if (send(N_SAY, "POST", "/jar/talk", body) != 0) { say("Busy: try again"); return 1; }
  }
  TK_PENDING = 1;
  start_input(IN_TALK);
  return 1;
}

static void paint_gift(void) {
  int i, n = mutual_n();
  char b[64];
  box(0, BAR, SW, BODY, C_BG);
  box(4, BAR + 3, SW - 8, 22, C_PANEL);
  pic(U.card.frame, U.card.pal, 1, 16, 16, 8, BAR + 6, 1, C_PANEL);
  api->fmt(b, sizeof b, "Send %s", G.gift_name);
  text(28, BAR + 6, b, C_TEXT, C_PANEL);
  text(28, BAR + 15, "postage 10 coins", J.coins < JST_POSTAGE ? C_WARN : C_GOLD, C_PANEL);
  if (G.gift_step == 0) {
    text(8, BAR + 30, "To which friend?", C_DIM, C_BG);
    if (!n) wrapped(8, BAR + 42, U.net ? "Asking the server..." :
                    "No friends who have added you back yet: Esc, then F.", 37, 3, C_DIM, C_BG);
    for (i = 0; i < n && i < 7; i++) {
      int y = BAR + 40 + i * 10, on = i == G.gift_sel;
      if (on) box(6, y - 1, SW - 12, 10, C_PANEL);
      text(10, y, who(mutual_at(i)), on ? C_GOLD : C_TEXT, on ? C_PANEL : C_BG);
    }
    return;
  }
  api->fmt(b, sizeof b, "To %s", who(mutual_at(G.gift_sel)));
  text(8, BAR + 30, b, C_TEXT, C_BG);
  if (G.gift_step == 1) { input_box("A note, up to 24 (or none):", G.input); return; }
  if (G.note[0]) { api->fmt(b, sizeof b, "\"%s\"", G.note); text(8, BAR + 42, b, C_PINK, C_BG); }
  text(8, BAR + 60, G.gift_step == 3 ? "Posting..." : "Send it? Gifts are for keeps.", C_TEXT, C_BG);
  if (G.gift_step == 2) text(8, BAR + 72, "y send   n change the note", C_DIM, C_BG);
}

static void paint_mail(void) {
  int r;
  char b[48];
  box(0, BAR, SW, BODY, C_BG);
  if (!G.mail_n) {
    wrapped(8, BAR + 10, U.net ? "Looking for parcels..." :
            "No post yet. Gifts from friends land here, and on the jar's dock.", 37, 3, C_DIM, C_BG);
    return;
  }
  for (r = 0; r < G.mail_n && r < 9; r++) {
    const JMail *e = &M[mail_row(r)];
    int y = BAR + 3 + r * 12;
    uint16_t bg = r == G.mail_sel ? C_PANEL : C_BG;
    box(4, y - 1, SW - 8, 11, bg);
    if (!e->opened) spr(SPR_PARCEL, 7, y + 1, 1, bg);
    else if (e->thanked) spr(SPR_HEART, 9, y + 2, 1, bg);
    api->fmt(b, sizeof b, "%s %s", e->opened ? "Gift from" : "Parcel from", e->from);
    textn(20, y + 1, b, 22, e->opened ? C_DIM : C_PINK, bg);
    if (e->note[0]) textn(20 + 23 * 6, y + 1, e->note, 13, C_TEXT, bg);
  }
}

static void post_paint(void *st, CRect c) {
  static const char *const H_FRIENDS[] = { "^v", "pick", "Ent", "send gift", "A", "add", "Esc", "jar", 0 };
  static const char *const H_PICK[] = { "^v", "pick", "Ent", "send this", "Esc", "back", 0 };
  static const char *const H_TALK[] = { "Ent", "say it", "Esc", "leave", 0 };
  static const char *const H_INPUT[] = { "Ent", "ok", "Esc", "cancel", 0 };
  static const char *const H_GIFT[] = { "^v", "pick", "Ent", "next", "Esc", "back", 0 };
  static const char *const H_SURE[] = { "y", "send", "n", "the note", 0 };
  static const char *const H_POSTING[] = { 0 };
  static const char *const H_MAIL[] = { "^v", "pick", "Ent", "open", "R", "check", "Esc", "jar", 0 };
  static const char *const H_CARD[] = { "Ent", "in jar", "T", "thanks", "Esc", "back", 0 };
  const char *const *h = H_FRIENDS;
  const char *where = "Friends";
  (void)st; (void)c;
  switch (G.view) {
  case V_FRIENDS: paint_friends(); h = G.in ? H_INPUT : H_FRIENDS; break;
  case V_GIFT:
    paint_gift();
    h = G.gift_step == 1 ? H_INPUT : G.gift_step == 2 ? H_SURE : G.gift_step == 3 ? H_POSTING : H_GIFT;
    where = "Send a Gift";
    break;
  case V_MAIL:    paint_mail(); h = H_MAIL; where = "Mail"; break;
  case V_PICK:    paint_pick(); h = H_PICK; where = "Send a Gift"; break;
  case V_TALK:    paint_talk(); h = H_TALK; where = "Tibbs"; break;
  case V_CARD:
    paint_card(M[G.card_mail].from, M[G.card_mail].note);
    h = H_CARD;
    where = M[G.card_mail].thanked ? "Gift (thanked)" : "Gift";
    break;
  }
  top_bar(where);
  hints(h);
  paint_note();
}

/* ---- keys ------------------------------------------------------------------------------------ */

static void start_input(int mode) {
  G.in = mode;
  G.input[0] = 0;
  G.inlen = 0;
}

/* Typing: 1 if the key was taken; 0 for Enter, which finishes. */
static int key_input(int k, int max) {
  if (k == CAPP_KEY_BACK) { if (G.inlen) G.input[--G.inlen] = 0; return 1; }
  if (k == CAPP_KEY_ENTER) return 0;
  if (k >= 32 && k < 127 && G.inlen < max) { G.input[G.inlen++] = (char)k; G.input[G.inlen] = 0; }
  return 1;
}

static int key_friends(int k) {
  if (G.in) {
    char path[64], q[40];
    if (k == CAPP_KEY_ESC) { G.in = IN_NONE; return 1; }
    if (key_input(k, G.in == IN_CODE ? 12 : JST_DNAME)) return 1;
    if (G.inlen && U.net) { say("Busy: try again"); return 1; }
    if (G.inlen && G.in == IN_CODE) {
      url_enc(q, sizeof q, G.input);
      api->fmt(path, sizeof path, "/jar/friend?code=%s", q);
      if (send(N_FRIEND, "POST", path, 0) == 0) net_status("adding");
    } else if (G.inlen) {
      jst_clean(G.input);
      if (send(N_NAME, "POST", "/people/name", G.input) == 0) net_status("renaming");
    }
    G.in = IN_NONE;
    return 1;
  }
  switch (k) {
  case CAPP_KEY_UP:   if (G.fsel > 0) G.fsel--; return 1;
  case CAPP_KEY_DOWN: if (G.fsel < G.nfr - 1) G.fsel++; return 1;
  case CAPP_KEY_ENTER: case 'g': case 'G':
    if (G.fsel >= G.nfr) { say("Add a friend first: A"); return 1; }
    if (!G.fr[G.fsel].mutual) { say("They have not added you back yet"); return 1; }
    load_pick();
    if (!G.npk) { say("Nothing to send: buy from the shop first"); return 1; }
    pick_card();
    G.view = V_PICK;
    return 1;
  case 'a': case 'A': start_input(IN_CODE); return 1;
  case 'n': case 'N': case 'e': case 'E': start_input(IN_NAME); return 1;
  case 'r': case 'R': fetch_me(); return 1;
  case 'm': case 'M': go_mail(); return 1;
  case 't': case 'T': go_talk(); return 1;
  case CAPP_KEY_ESC: to_jar(); return 1;
  }
  return 1;
}

static int key_pick(int k) {
  switch (k) {
  case CAPP_KEY_UP:   if (G.psel > 0) G.psel--; break;
  case CAPP_KEY_DOWN: if (G.psel < G.npk - 1) G.psel++; break;
  case CAPP_KEY_ENTER:
    go_gift(G.pk[G.psel], V_FRIENDS);
    if (G.view != V_GIFT) return 1;              /* it said why */
    G.gift_sel = mutual_index(G.fsel);         /* the friend is chosen: the note */
    G.gift_step = 1;
    start_input(IN_NOTE);
    return 1;
  case CAPP_KEY_ESC: G.view = V_FRIENDS; return 1;
  default: return 1;
  }
  if (G.psel < G.ptop) G.ptop = G.psel;
  if (G.psel >= G.ptop + 9) G.ptop = G.psel - 8;
  pick_card();
  return 1;
}

static int key_gift(int k) {
  if (G.gift_step == 3) return 1;                 /* posting: wait for the answer */
  if (G.gift_step == 1) {
    if (k == CAPP_KEY_ESC) { G.in = IN_NONE; G.gift_step = 0; return 1; }
    if (key_input(k, JST_NOTE)) return 1;
    jst_clean(G.input);
    ji_copy(G.note, G.input, (int)sizeof G.note);
    G.in = IN_NONE;
    G.gift_step = 2;
    return 1;
  }
  if (G.gift_step == 2) {
    int a = confirm_key(api, (uint8_t)k);
    if (a == CONFIRM_YES) send_gift();
    else if (a == CONFIRM_NO) {                   /* back to the note, as it was */
      G.gift_step = 1;
      G.in = IN_NOTE;
      ji_copy(G.input, G.note, (int)sizeof G.input);
      G.inlen = slen(G.input);
    }
    return 1;
  }
  switch (k) {
  case CAPP_KEY_UP:   if (G.gift_sel > 0) G.gift_sel--; return 1;
  case CAPP_KEY_DOWN: if (G.gift_sel < mutual_n() - 1) G.gift_sel++; return 1;
  case CAPP_KEY_ENTER:
    if (!mutual_n()) return 1;
    G.gift_step = 1;
    start_input(IN_NOTE);
    return 1;
  case CAPP_KEY_ESC:
    if (G.gift_back == V_CARD) G.view = V_CARD;
    else if (G.gift_back == V_FRIENDS) G.view = V_FRIENDS;
    else to_jar();
    return 1;
  }
  return 1;
}

static int key_mail(int k) {
  switch (k) {
  case CAPP_KEY_UP:   if (G.mail_sel > 0) G.mail_sel--; return 1;
  case CAPP_KEY_DOWN: if (G.mail_sel < G.mail_n - 1) G.mail_sel++; return 1;
  case CAPP_KEY_ENTER: if (G.mail_n) open_mail(mail_row(G.mail_sel)); return 1;
  case 'r': case 'R': G.tried_mail = 0; start_mail(); return 1;
  case 'f': case 'F': G.view = V_FRIENDS; fetch_me(); return 1;
  case CAPP_KEY_ESC: to_jar(); return 1;
  }
  return 1;
}

static int key_card(int k) {
  if (card_key(k)) return 1;
  switch (k) {
  case 't': case 'T': thank(); return 1;
  case 'g': case 'G': go_gift(U.card.id, V_CARD); return 1;
  case CAPP_KEY_ESC: G.view = V_MAIL; return 1;
  }
  return 1;
}

static int post_key(void *st, uint8_t k) {
  (void)st;
  U.dirty = 1;
  if (U.msg[0]) U.msg[0] = 0;
  switch (G.view) {
  case V_FRIENDS: return key_friends(k);
  case V_GIFT:    return key_gift(k);
  case V_PICK:    return key_pick(k);
  case V_TALK:    return key_talk(k);
  case V_MAIL:    return key_mail(k);
  case V_CARD:    return key_card(k);
  }
  return 0;
}

static int post_wants_text(void *st) {
  (void)st;
  return G.in != IN_NONE;
}

static int post_tick(void *st, uint32_t now) {
  (void)st;
  if (ui_clock() && G.view == V_FRIENDS) U.dirty = 1;
  if (U.net) {
    int n = api->http_poll(NET, sizeof NET);
    if (n != CAPP_HTTP_PENDING) net_reply(n);
  } else if (TK_AGAIN && (int32_t)(now - TK_AGAIN) >= 0) {
    TK_AGAIN = 0;                                   /* is he done talking? */
    if (G.view != V_TALK || hear() != 0) TK_PENDING = 0;
  }
  if (U.msg[0] && (int32_t)(now - U.msg_until) > 0) { U.msg[0] = 0; U.dirty = 1; }
  if (!U.dirty) return 0;
  U.dirty = 0;
  return 1;
}

/* ---- opening ---------------------------------------------------------------------------------- */

/* The post on SCREEN: friends (the default), mail, talk (Tibbs), "gift ID".
 * A screen of Jar Factory (apps/jar.c). */
static void post_open(const char *s) {
  if (!s) s = "";
  api->mem_set(&G, 0, sizeof G);
  api->mem_set(&U, 0, sizeof U);
  G.card_mail = -1;
  if (ui_load()) return;
  G.mail_n = jst_mail_load(api, M, TEXT, sizeof TEXT);
  J.parcels = (uint16_t)unopened();
  if (jst_get(api, JST_FRIENDS, NET, sizeof NET) > 0) parse_me(NET);
  TK_N = TK_PENDING = 0;
  TK_AGAIN = 0;
  if (str_same(s, "mail")) go_mail();
  else if (str_same(s, "talk")) go_talk();
  else if (str_starts(s, "gift ")) {
    const char *p = s + 5;
    G.view = V_FRIENDS;
    go_gift(str_uint(&p), -1);
  } else { G.view = V_FRIENDS; fetch_me(); }
}

#endif /* CARDOS_JARPOST_H */
