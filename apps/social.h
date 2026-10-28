/* Friends, from an app: the server's friends, pub/ data, rooms and sharing
 * (server/friends.py, server/kv.py, server/rooms.py, apps/chat/server), as
 * calls and parsers, so no app builds one of those URLs or reads one of
 * those replies by hand. And a friend picker, drawn over the app.
 *
 * The rule the owner set: not social media. Nothing here posts by itself.
 * An app shows a friend's data where it belongs (Wordle's result screen,
 * under yours), and sends only what its person chose to send.
 *
 * Every request is the one an app may have in flight (api->http_start):
 * each soc_* call starts one and returns what http_start returned (0 is
 * started, -1 busy, -2 no memory, -3 URL too long); the app polls
 * api->http_poll from tick, as it does for anything else, and hands the
 * reply to the parser. The server is the device's own (api->proxy), which
 * the kernel signs with the device's token.
 *
 *   soc_friends(api)                         GET /friends
 *   soc_pub_put(api, "wordle", "stats", v)   your pub/wordle value of stats
 *   soc_pub_each(api, "wordle", "stats")     yours and each friend's
 *   soc_share(api, "Wordle", "Wordle 300 3/6", chat_name)   a line in Chat
 *   soc_room_new(api, app, "sam", title, state)
 *   soc_room_list(api, app)  soc_room_get(api, id)
 *   soc_room_state(api, id, if_version, state)
 *   soc_room_move(api, id, body, say)   soc_room_moves(api, id, after)
 *
 * Replies: soc_friend_parse for /friends; soc_block_next for the two that
 * carry bytes (/kv/each and /room/moves: a header line of tab fields whose
 * last is a size, then that many bytes, then a newline); tsv_field
 * (apps/str.h) for the rest, which are plain lines.
 *
 * Portable C, no libc beyond what the app has: host-tested in
 * test/test_social.c. */
#ifndef CARDOS_SOCIAL_H
#define CARDOS_SOCIAL_H

#include <stdint.h>
#include "kernel/app/capp.h"
#include "apps/str.h"

#ifdef __GNUC__
#define SOC_OPT __attribute__((unused))
#else
#define SOC_OPT
#endif

#define SOC_NAME 25          /* an account name, 1-24 */
#define SOC_DISPLAY 17       /* the name a person chose, up to 16 */
#define SOC_FRIENDS 8
#define SOC_TIMEOUT 15000

typedef struct {
  char name[SOC_NAME];
  char display[SOC_DISPLAY];
} SocFriend;

/* ---- requests ------------------------------------------------------------ */

static SOC_OPT int soc_start(const CardApi *api, const char *method, const char *url,
                             const char *body) {
  return api->http_start(method, url, body, body ? "application/octet-stream" : 0, 0, SOC_TIMEOUT);
}

static SOC_OPT int soc_friends(const CardApi *api) {
  char url[160];
  api->fmt(url, sizeof url, "%s/friends", api->proxy());
  return soc_start(api, "GET", url, 0);
}

static SOC_OPT int soc_pub_put(const CardApi *api, const char *app, const char *key,
                               const char *value) {
  char url[256], k[96];
  url_enc(k, (int)sizeof k, key);
  api->fmt(url, sizeof url, "%s/kv/put?ns=pub/%s&key=%s", api->proxy(), app, k);
  return soc_start(api, "POST", url, value);
}

static SOC_OPT int soc_pub_each(const CardApi *api, const char *app, const char *key) {
  char url[256], k[96];
  url_enc(k, (int)sizeof k, key);
  api->fmt(url, sizeof url, "%s/kv/each?ns=pub/%s&key=%s", api->proxy(), app, k);
  return soc_start(api, "GET", url, 0);
}

/* `chat_name`: the name the person uses in Chat (/config/chat.txt's first
 * line), or 0 for the name their friends see. */
static SOC_OPT int soc_share(const CardApi *api, const char *app, const char *line,
                             const char *chat_name) {
  char url[256], a[48], n[48];
  url_enc(a, (int)sizeof a, app);
  if (chat_name && *chat_name) {
    url_enc(n, (int)sizeof n, chat_name);
    api->fmt(url, sizeof url, "%s/share?app=%s&name=%s", api->proxy(), a, n);
  } else {
    api->fmt(url, sizeof url, "%s/share?app=%s", api->proxy(), a);
  }
  return soc_start(api, "POST", url, line);
}

/* `with`: one friend's name, or several with commas. */
static SOC_OPT int soc_room_new(const CardApi *api, const char *app, const char *with,
                                const char *title, const char *state) {
  char url[320], w[96], t[128];
  url_enc(w, (int)sizeof w, with);
  url_enc(t, (int)sizeof t, title ? title : "");
  api->fmt(url, sizeof url, "%s/room/new?app=%s&with=%s&title=%s", api->proxy(), app, w, t);
  return soc_start(api, "POST", url, state ? state : "");
}

static SOC_OPT int soc_room_list(const CardApi *api, const char *app) {
  char url[200];
  api->fmt(url, sizeof url, "%s/room/list?app=%s", api->proxy(), app);
  return soc_start(api, "GET", url, 0);
}

static SOC_OPT int soc_room_get(const CardApi *api, int id) {
  char url[200];
  api->fmt(url, sizeof url, "%s/room/get?id=%d", api->proxy(), id);
  return soc_start(api, "GET", url, 0);
}

/* if_version < 0: replace whatever is there. Otherwise only if nobody
 * changed it since that version: a reply of -409 says someone did, and
 * the app reads the room again and retries. */
static SOC_OPT int soc_room_state(const CardApi *api, int id, int if_version, const char *state) {
  char url[200];
  if (if_version >= 0)
    api->fmt(url, sizeof url, "%s/room/state?id=%d&if=%d", api->proxy(), id, if_version);
  else
    api->fmt(url, sizeof url, "%s/room/state?id=%d", api->proxy(), id);
  return soc_start(api, "POST", url, state);
}

/* `say`: tells the other members (a turn game's "your move"); 0 tells
 * nobody, which is right for anything sent often. */
static SOC_OPT int soc_room_move(const CardApi *api, int id, const char *body, const char *say) {
  char url[256], s[96];
  if (say && *say) {
    url_enc(s, (int)sizeof s, say);
    api->fmt(url, sizeof url, "%s/room/move?id=%d&say=%s", api->proxy(), id, s);
  } else {
    api->fmt(url, sizeof url, "%s/room/move?id=%d", api->proxy(), id);
  }
  return soc_start(api, "POST", url, body);
}

static SOC_OPT int soc_room_moves(const CardApi *api, int id, int after) {
  char url[200];
  api->fmt(url, sizeof url, "%s/room/moves?id=%d&after=%d", api->proxy(), id, after);
  return soc_start(api, "GET", url, 0);
}

/* ---- replies ------------------------------------------------------------- */

/* /friends: the people who are friends (not asks), at most `max`; how many. */
static SOC_OPT int soc_friend_parse(const char *reply, SocFriend *out, int max) {
  const char *p = reply;
  int n = 0;
  char state[12];
  while (*p && n < max) {
    tsv_field(p, 2, state, sizeof state);
    if (str_same(state, "friend")) {
      tsv_field(p, 0, out[n].name, SOC_NAME);
      tsv_field(p, 1, out[n].display, SOC_DISPLAY);
      if (out[n].name[0]) n++;
    }
    p = tsv_next_line(p);
  }
  return n;
}

/* One block of a reply that carries bytes: *p is where the next header
 * line starts, `end` is the reply's end. On a whole block, `head` points
 * at its header line (read its fields with tsv_field; the last is the
 * size), *body at its bytes and *len their count, *p moves past it, and 1
 * is returned. 0 at the end, or at a block the reply cut short -- a reply
 * that filled the buffer (CAPP_HTTP_FILLED) is read up to its last whole
 * block, and the app asks again from there. */
static SOC_OPT int soc_block_next(const char **p, const char *end, const char **head,
                                  const char **body, int *len) {
  const char *h = *p, *q = *p, *last;
  uint32_t size;
  if (h >= end) return 0;
  while (q < end && *q != '\n') q++;
  if (q >= end) return 0;
  last = q;                                  /* the size: the last field */
  while (last > h && last[-1] != '\t') last--;
  {
    const char *s = last;
    size = str_uint(&s);
    if (s != q) return 0;                    /* not a number up to the newline */
  }
  q++;                                       /* past the header's newline */
  if ((uint32_t)(end - q) < size + 1 || q[size] != '\n') return 0;
  *head = h;
  *body = q;
  *len = (int)size;
  *p = q + size + 1;
  return 1;
}

/* ---- the friend picker ----------------------------------------------------
 *
 *   SocPick pk;
 *   soc_pick_open(api, &pk, "Play with");   on a key; it asks the server
 *   tick:  if (pk.open) repaint |= soc_pick_tick(api, &pk);
 *   key:   if (pk.open) { soc_pick_key(&pk, k); return; }   it takes them all
 *   paint: if (pk.open) soc_pick_paint(api, &pk, area);     last, over the app
 *   ... once pk.done is 1, pk.chosen is the friend; -1, cancelled.
 *
 * It holds the request while it loads, so start nothing else meanwhile. */
typedef struct {
  int open;                  /* drawn and taking keys */
  int done;                  /* 0 still choosing, 1 chose, -1 cancelled */
  int loading;               /* the list is on its way */
  int failed;                /* the server could not be asked (or said no) */
  int n, sel;
  const char *title;
  SocFriend f[SOC_FRIENDS];
  SocFriend chosen;
} SocPick;

static SOC_OPT void soc_pick_open(const CardApi *api, SocPick *pk, const char *title) {
  api->mem_set(pk, 0, sizeof *pk);
  pk->open = 1;
  pk->title = title ? title : "Choose a friend";
  pk->loading = soc_friends(api) == 0;
  pk->failed = !pk->loading;
}

/* 1 when the picker changed and wants a repaint. */
static SOC_OPT int soc_pick_tick(const CardApi *api, SocPick *pk) {
  char buf[1024];
  int r;
  if (!pk->open || !pk->loading) return 0;
  r = api->http_poll(buf, sizeof buf);
  if (r == CAPP_HTTP_PENDING) return 0;
  pk->loading = 0;
  if (r < 0) { pk->failed = 1; return 1; }
  buf[r] = 0;
  pk->n = soc_friend_parse(buf, pk->f, SOC_FRIENDS);
  return 1;
}

/* Every key while it is open: arrows choose, Enter takes, Esc cancels. */
static SOC_OPT void soc_pick_key(SocPick *pk, int key) {
  if (!pk->open) return;
  if (key == CAPP_KEY_ESC) { pk->open = 0; pk->done = -1; return; }
  if (pk->loading) return;
  if (key == CAPP_KEY_UP && pk->sel > 0) pk->sel--;
  else if (key == CAPP_KEY_DOWN && pk->sel < pk->n - 1) pk->sel++;
  else if (key == CAPP_KEY_ENTER) {
    if (pk->n > 0) {
      /* Field by field: a struct assignment compiles to memcpy, which an
       * app does not link (build_apps.py, -fno-builtin). */
      int i;
      for (i = 0; i < SOC_NAME; i++) pk->chosen.name[i] = pk->f[pk->sel].name[i];
      for (i = 0; i < SOC_DISPLAY; i++) pk->chosen.display[i] = pk->f[pk->sel].display[i];
      pk->done = 1;
    }
    else pk->done = -1;
    pk->open = 0;
  }
}

#define SOC_BG   CAPP_RGB(250, 250, 250)
#define SOC_INK  CAPP_RGB(24, 26, 32)
#define SOC_DIM  CAPP_RGB(110, 116, 128)
#define SOC_SEL  CAPP_RGB(44, 92, 158)
#define SOC_EDGE CAPP_RGB(92, 100, 112)

/* A box in the middle of `area`: the title, then the friends (or why
 * there are none), in the 6x8 font. */
static SOC_OPT void soc_pick_paint(const CardApi *api, const SocPick *pk, CRect area) {
  int rows = pk->n > 0 ? pk->n : 1, i;
  int w = 160, h = 22 + rows * 12 + 14;
  CRect box = capp_rect(area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h);
  const char *why;
  if (!pk->open) return;
  api->fill(box, SOC_EDGE);
  api->fill(capp_rect(box.x + 1, box.y + 1, box.w - 2, box.h - 2), SOC_BG);
  api->text(box.x + 6, box.y + 6, pk->title, SOC_INK, SOC_BG);
  if (pk->loading || pk->failed || pk->n == 0) {
    why = pk->loading ? "Asking the server..." : pk->failed ? "Could not reach the server." :
          "No friends yet: add one on the dashboard.";
    api->text(box.x + 6, box.y + 22, why, SOC_DIM, SOC_BG);
  }
  for (i = 0; i < pk->n; i++) {
    int y = box.y + 20 + i * 12;
    int on = i == pk->sel;
    if (on) api->fill(capp_rect(box.x + 3, y - 2, box.w - 6, 12), SOC_SEL);
    api->text(box.x + 8, y, pk->f[i].display[0] ? pk->f[i].display : pk->f[i].name,
              on ? SOC_BG : SOC_INK, on ? SOC_SEL : SOC_BG);
  }
  api->text(box.x + 6, box.y + box.h - 11, "enter choose  esc cancel", SOC_DIM, SOC_BG);
}

#endif /* CARDOS_SOCIAL_H */
