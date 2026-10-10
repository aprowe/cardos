/* Jar Post -- Jar Factory's friends, gifts and mail.
 *
 * Opened from the jar (apps/jar.c) with api->run("Jar Post", SCREEN):
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

#include "apps/jarui.h"

enum { V_FRIENDS = 0, V_GIFT, V_MAIL, V_CARD };
enum { IN_NONE = 0, IN_CODE, IN_NAME, IN_NOTE };
enum { N_ME = N_APP, N_FRIEND, N_NAME, N_GIFT, N_PEEK, N_ACK, N_THANKS };

#define F_MAX 16

typedef struct { char name[JI_WHO + 1], disp[JI_WHO + 1]; uint32_t seen; uint8_t mutual; } Friend;

static JMail M[JST_MAIL_MAX];

static struct {
  int view, in, inlen;
  char input[JST_NOTE + 1];
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
} G;

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
    int y = BAR + 27 + i * 9;
    textn(8, y, who(f), 12, C_TEXT, C_BG);
    text(86, y, f->mutual ? "friends" : "waiting", f->mutual ? C_TRAIT : C_DIM, C_BG);
    ago(f->seen, s, sizeof s);
    text(SW - 6 - slen(s) * 6, y, s, C_DIM, C_BG);
  }
  if (G.in == IN_CODE) input_box("Their friend code:", G.input);
  else if (G.in == IN_NAME) input_box("Your name, up to 8:", G.input);
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

static void app_paint(void *st, CRect c) {
  static const char *const H_FRIENDS[] = { "A", "add", "N", "name", "R", "refresh", "Esc", "jar", 0 };
  static const char *const H_INPUT[] = { "Ent", "ok", "Esc", "cancel", 0 };
  static const char *const H_GIFT[] = { "^v", "pick", "Ent", "next", "Esc", "back", 0 };
  static const char *const H_SURE[] = { "y", "send", "n", "the note", 0 };
  static const char *const H_POSTING[] = { 0 };
  static const char *const H_MAIL[] = { "^v", "pick", "Ent", "open", "R", "check", "Esc", "jar", 0 };
  static const char *const H_CARD[] = { "Ent", "in jar", "T", "thanks", "H", "shelf", "Esc", "back", 0 };
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
  case 'a': case 'A': start_input(IN_CODE); return 1;
  case 'n': case 'N': case 'e': case 'E': start_input(IN_NAME); return 1;
  case 'r': case 'R': fetch_me(); return 1;
  case 'm': case 'M': go_mail(); return 1;
  case CAPP_KEY_ESC: to_jar(); return 1;
  }
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

static int app_key(void *st, uint8_t k) {
  (void)st;
  U.dirty = 1;
  if (U.msg[0]) U.msg[0] = 0;
  switch (G.view) {
  case V_FRIENDS: return key_friends(k);
  case V_GIFT:    return key_gift(k);
  case V_MAIL:    return key_mail(k);
  case V_CARD:    return key_card(k);
  }
  return 0;
}

static int app_wants_text(void *st) {
  (void)st;
  return G.in != IN_NONE;
}

static int app_tick(void *st, uint32_t now) {
  (void)st;
  if (ui_clock() && G.view == V_FRIENDS) U.dirty = 1;
  if (U.net) {
    int n = api->http_poll(NET, sizeof NET);
    if (n != CAPP_HTTP_PENDING) net_reply(n);
  }
  if (U.msg[0] && (int32_t)(now - U.msg_until) > 0) { U.msg[0] = 0; U.dirty = 1; }
  if (!U.dirty) return 0;
  U.dirty = 0;
  return 1;
}

/* ---- starting ---------------------------------------------------------------------------------- */

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN,
  "Jar Post",
  /* 16x16: a parcel tied with string, a heart on its label. */
  { 0x03, 0x60, 0x04, 0x90, 0x03, 0xE0, 0x7F, 0xFE,
    0x40, 0x82, 0x40, 0x82, 0x7F, 0xFE, 0x40, 0x82,
    0x5B, 0x82, 0x5F, 0x82, 0x4E, 0x82, 0x44, 0x82,
    0x40, 0x82, 0x40, 0x82, 0x7F, 0xFE, 0x00, 0x00 },
  "Jar Factory's friends and post\n"
  "A\tadd a friend by their code\n"
  "N\tyour name, up to 8\n"
  "M\tmail: parcels from friends\n"
  "T\tsay thank you for a gift\n"
  "Esc\tback to the jar\n",
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  const char *s = argc > 1 ? argv[1] : "";
  api = a;
  api->mem_set(&G, 0, sizeof G);
  api->mem_set(&U, 0, sizeof U);
  G.card_mail = -1;
  if (ui_load()) return 0;
  G.mail_n = jst_mail_load(api, M, TEXT, sizeof TEXT);
  J.parcels = (uint16_t)unopened();
  if (jst_get(api, JST_FRIENDS, NET, sizeof NET) > 0) parse_me(NET);
  if (str_same(s, "mail")) go_mail();
  else if (str_starts(s, "gift ")) {
    const char *p = s + 5;
    G.view = V_FRIENDS;
    go_gift(str_uint(&p), -1);
  } else { G.view = V_FRIENDS; fetch_me(); }
  UI.paint = app_paint;
  UI.key = app_key;
  UI.tick = app_tick;
  UI.wants_text = app_wants_text;
  UI.pref_w = SW;
  UI.pref_h = SHT;
  api->ui(&UI);
  return 0;
}
