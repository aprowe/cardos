/* The real Wordloom app (apps/wordloom/wordloom.c) against a fake of the
 * server's friends and rooms (server/friends.py, server/rooms.py), painted
 * into a framebuffer with the real fonts (test/fakescreen.h).
 *
 * Two people, alex and sam, on one fake server: alex starts a game and
 * weaves, then the same app on sam's device (a blank card: everything comes
 * from replaying the room) sees the move, answers it, and so on. Every
 * paint must write every pixel.
 *
 * WORDLOOM_DUMP=dir writes the screens there (wordloom_*.ppm), to look at. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"
#include "fakescreen.h"

#define capp_info wordloom_capp_info
#define capp_main wordloom_capp_main
#include "apps/wordloom/wordloom.c"
#undef capp_info
#undef capp_main

/* ---- the fake server ------------------------------------------------------- */

#define FS_ROOMS 4
#define FS_MOVES 64
static struct {
  int  id;
  char members[64], state[64], title[64];
  int  n;
  char from[FS_MOVES][16];
  char body[FS_MOVES][64];
} srv_room[FS_ROOMS];
static int  srv_nrooms;
static char srv_user[16];
static char srv_reply[8192];
static int  srv_pending, srv_rc, srv_requests, srv_says;
static char srv_last_say[64];

static const char *q_arg(const char *url, const char *key, char *out, int n) {
  const char *p = strchr(url, '?');
  size_t k = strlen(key);
  out[0] = 0;
  while (p) {
    p++;
    if (!strncmp(p, key, k) && p[k] == '=') {
      int i = 0;
      p += k + 1;
      while (*p && *p != '&' && i < n - 1) out[i++] = *p++;
      out[i] = 0;
      return out;
    }
    p = strchr(p, '&');
  }
  return out;
}

static int s_start(const char *m, const char *url, const char *body, const char *ct, const char *br,
                   int ms) {
  const char *path = url + strlen("http://srv");
  char a[64];
  int at = 0, i;
  (void)ct; (void)br; (void)ms; (void)m;
  if (srv_pending) return -1;
  srv_pending = 1;
  srv_rc = 0;
  srv_requests++;
  srv_reply[0] = 0;
  if (!strncmp(path, "/friends", 8)) {
    at = sprintf(srv_reply, "%s\t%s\tfriend\t0\n", !strcmp(srv_user, "alex") ? "sam" : "alex",
                 !strcmp(srv_user, "alex") ? "Sam" : "Alex");
  } else if (!strncmp(path, "/room/new", 9)) {
    i = srv_nrooms++;
    srv_room[i].id = 40 + i;
    sprintf(srv_room[i].members, "%s,%s", srv_user, q_arg(url, "with", a, sizeof a));
    strcpy(srv_room[i].state, body);
    q_arg(url, "title", srv_room[i].title, sizeof srv_room[i].title);
    srv_room[i].n = 0;
    at = sprintf(srv_reply, "ok %d\n", srv_room[i].id);
  } else if (!strncmp(path, "/room/list", 10)) {
    for (i = srv_nrooms - 1; i >= 0; i--)
      if (strstr(srv_room[i].members, srv_user))
        at += sprintf(srv_reply + at, "%d\t%s\t%s\t%d\t0\talex\n", srv_room[i].id, srv_room[i].title,
                      srv_room[i].members, srv_room[i].n);
  } else {
    int id = atoi(q_arg(url, "id", a, sizeof a));
    for (i = 0; i < srv_nrooms && srv_room[i].id != id; i++) {}
    if (i == srv_nrooms) { srv_rc = -404; return 0; }
    if (!strncmp(path, "/room/get", 9)) {
      at = sprintf(srv_reply, "%d\t1\t%d\t%s\n%s", id, srv_room[i].n, srv_room[i].members,
                   srv_room[i].state);
    } else if (!strncmp(path, "/room/move", 10) && strncmp(path, "/room/moves", 11)) {
      int k = srv_room[i].n++;
      strcpy(srv_room[i].from[k], srv_user);
      strcpy(srv_room[i].body[k], body);
      q_arg(url, "say", srv_last_say, sizeof srv_last_say);
      if (srv_last_say[0]) srv_says++;
      at = sprintf(srv_reply, "ok %d\n", k + 1);
    } else if (!strncmp(path, "/room/moves", 11)) {
      int after = atoi(q_arg(url, "after", a, sizeof a)), k;
      for (k = after; k < srv_room[i].n; k++)
        at += sprintf(srv_reply + at, "%d\t%s\t0\t%d\n%s\n", k + 1, srv_room[i].from[k],
                      (int)strlen(srv_room[i].body[k]), srv_room[i].body[k]);
    } else {
      srv_rc = -404;
    }
  }
  (void)at;
  return 0;
}

static int s_poll(char *o, size_t n) {
  size_t k;
  if (!srv_pending) return -1;
  srv_pending = 0;
  if (srv_rc < 0) return srv_rc;
  k = strlen(srv_reply);
  if (k > n - 1) k = n - 1;
  memcpy(o, srv_reply, k);
  o[k] = 0;
  return (int)k;
}

/* ---- driving the app ------------------------------------------------------- */

static CardApi WA;

/* The app opened on `user`'s device, with a blank card. */
static void boot(const char *user) {
  strcpy(srv_user, user);
  srv_pending = 0;
  fakeapi_init(&WA);
  fakefs_mem(&WA);
  fakescreen_install(&WA);
  WA.http_start = s_start;
  WA.http_poll = s_poll;
  fakeapi_ticks = 1000;
  fakeapi_epoch = 1792141200u;
  wordloom_capp_main(&WA, 1, 0);
  fakescreen_paint();
}

/* `ms` of ticks, a tick each 50 ms, painting when asked. */
static void run(int ms) {
  for (; ms > 0; ms -= 50) {
    fakeapi_ticks += 50;
    if (fakescreen_ui.tick(fakescreen_ui.state, fakeapi_ticks)) fakescreen_paint();
  }
}

static void key(uint8_t k) {
  if (fakescreen_ui.key(fakescreen_ui.state, k)) fakescreen_paint();
}

static void keys(const char *s) { for (; *s; s++) key((uint8_t)*s); }

static int idle(void) { return G.net == NET_NONE && !srv_pending; }

/* ---- the tests --------------------------------------------------------------- */

void test_wordloom_paint_no_games_yet(void) {
  srv_nrooms = 0;
  boot("alex");
  CHECK(!G.listed);
  CHECK_EQ(fakescreen_unset(), 0);
  run(500);
  CHECK(G.listed);
  CHECK_EQ(G.nrooms, 0);
  CHECK_EQ(G.nfriends, 1);
  CHECK(idle());
  CHECK_EQ(fakescreen_unset(), 0);
  CHECK(!fakescreen_ui.wants_text(fakescreen_ui.state));
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_empty");
}

void test_wordloom_paint_a_whole_exchange_between_two_devices(void) {
  char first[3], mv[32];
  int a_score, i, sam_moves;
  WlGame check;

  /* alex: n, the picker, Sam, and a game. */
  srv_nrooms = 0;
  srv_says = 0;
  boot("alex");
  run(500);
  key('n');
  CHECK(G.pick.open);
  run(200);
  CHECK_EQ(G.pick.n, 1);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_picker");
  key(CAPP_KEY_ENTER);
  CHECK(!G.pick.open);
  run(1000);
  CHECK_EQ(srv_nrooms, 1);
  CHECK(!strcmp(srv_room[0].members, "alex,sam"));
  CHECK(!strncmp(srv_room[0].state, "wordloom 1\nseed ", 16));
  CHECK_EQ(G.nrooms, 1);
  CHECK(!strcmp(G.me, "alex"));                 /* the member who is not the friend */
  CHECK(fakefs_get(ME_PATH) != 0);
  CHECK(G.rooms[0].known);
  CHECK_EQ(G.rooms[0].turn, 0);
  CHECK(idle());
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_list");

  /* Open it: alex's turn. Lay the first two tiles through the centre. */
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.view, V_GAME);
  run(300);
  CHECK_EQ(G.me_idx, 0);
  CHECK(my_turn());
  CHECK_EQ(fakescreen_ui.wants_text(fakescreen_ui.state), CAPP_TEXT_ARROWS);
  first[0] = G.g.rack[0][0]; first[1] = G.g.rack[0][1]; first[2] = 0;
  keys(first);
  CHECK_EQ(G.nt, 2);
  CHECK_EQ(G.tc[1], WL_MID + 1);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_typing");
  /* Backspace takes one back; typing it again puts it back. */
  key(CAPP_KEY_BACK);
  CHECK_EQ(G.nt, 1);
  key((uint8_t)first[1]);
  CHECK_EQ(G.nt, 2);
  /* A letter not in the rack is refused. */
  {
    char none = 'a';
    while (strchr(G.g.rack[0], none)) none++;
    key((uint8_t)none);
    CHECK_EQ(G.nt, 2);
    CHECK(!strcmp(G.msg, "not in your rack"));
  }
  key(CAPP_KEY_ENTER);
  run(300);
  CHECK_EQ(srv_room[0].n, 1);
  wl_fmt_play(mv, sizeof mv, WL_MID, WL_MID, WL_ACROSS, first);
  CHECK(!strcmp(srv_room[0].body[0], mv));
  CHECK_EQ(srv_says, 1);
  CHECK(!strcmp(srv_last_say, "your%20move"));
  run(3500);                                    /* the poll reads it back */
  CHECK_EQ(G.rooms[0].have, 1);
  CHECK(G.g.score[0] > 0);
  CHECK_EQ(G.g.turn, 1);
  CHECK(!my_turn());
  CHECK_EQ(G.nt, 0);
  CHECK(!fakescreen_ui.wants_text(fakescreen_ui.state));
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_sent");
  a_score = G.g.score[0];
  /* Out of turn, letters do nothing and send nothing. */
  keys("ab");
  CHECK_EQ(G.nt, 0);
  CHECK_EQ(srv_room[0].n, 1);

  /* sam, on a blank card: the room, replayed. */
  boot("sam");
  run(1000);
  CHECK_EQ(G.nrooms, 1);
  CHECK(!strcmp(G.me, "sam"));
  CHECK_EQ(G.rooms[0].have, 1);
  CHECK_EQ(G.rooms[0].score[0], a_score);
  CHECK_EQ(G.rooms[0].turn, 1);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_list_sam");
  key(CAPP_KEY_ENTER);
  run(300);
  CHECK_EQ(G.me_idx, 1);
  CHECK(my_turn());
  CHECK_EQ(G.g.board[WL_MID][WL_MID], first[0]);
  {
    uint32_t seed;
    wl_state_parse(srv_room[0].state, &seed);
    wl_new(&check, seed);
    wl_play(&check, 0, WL_MID, WL_MID, WL_ACROSS, first, 0, 0);
    CHECK(!strcmp(G.g.rack[1], check.rack[1]));
    CHECK(!strcmp(G.g.rack[0], check.rack[0]));
  }
  /* One tile under the centre: a word down. */
  key(CAPP_KEY_DOWN);
  key((uint8_t)G.g.rack[1][0]);
  CHECK_EQ(G.nt, 1);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_sam_typing");
  key(CAPP_KEY_ENTER);
  run(3500);
  CHECK_EQ(srv_room[0].n, 2);
  CHECK(!strncmp(srv_room[0].body[1], "play 6 5 d ", 11));
  CHECK_EQ(G.g.turn, 0);
  CHECK_EQ(G.g.last_kind, WL_K_PLAY);

  /* The swap screen: Tab, Swap, a tile marked, Esc. Not sam's turn now. */
  key('\t');
  CHECK_EQ(G.mode, M_MENU);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_menu");
  key(CAPP_KEY_ENTER);
  CHECK_EQ(G.mode, M_PLACE);                    /* not your turn */
  sam_moves = srv_room[0].n;

  /* alex again (a blank card once more): swap two, then sam passes, then
   * alex resigns from the menu. */
  boot("alex");
  run(1000);
  key(CAPP_KEY_ENTER);
  run(300);
  CHECK(my_turn());
  key('\t');
  key(CAPP_KEY_ENTER);                          /* Swap tiles */
  CHECK_EQ(G.mode, M_SWAP);
  key((uint8_t)G.g.rack[0][0]);
  key((uint8_t)G.g.rack[0][1]);
  CHECK(G.swap_mark[0] && G.swap_mark[1]);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_swap");
  key(CAPP_KEY_ENTER);
  run(3500);
  CHECK_EQ(srv_room[0].n, sam_moves + 1);
  CHECK(!strncmp(srv_room[0].body[sam_moves], "swap ", 5));
  CHECK_EQ(G.g.last_kind, WL_K_SWAP);
  CHECK_EQ(G.g.turn, 1);

  boot("sam");
  run(1000);
  key(CAPP_KEY_ENTER);
  run(300);
  key('\t');
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);                          /* Pass */
  CHECK_EQ(G.mode, M_ASK_PASS);
  key(CAPP_KEY_ENTER);                          /* not an answer */
  CHECK_EQ(G.mode, M_ASK_PASS);
  key('y');
  run(3500);
  CHECK(!strcmp(srv_room[0].body[srv_room[0].n - 1], "pass"));
  CHECK_EQ(G.g.last_kind, WL_K_PASS);

  boot("alex");
  run(1000);
  key(CAPP_KEY_ENTER);
  run(300);
  key('\t');
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_DOWN);
  key(CAPP_KEY_ENTER);                          /* Resign */
  CHECK_EQ(G.mode, M_ASK_RESIGN);
  key('y');
  run(3500);
  CHECK(!strcmp(srv_room[0].body[srv_room[0].n - 1], "resign"));
  CHECK(!strcmp(srv_last_say, "resigned"));
  CHECK_EQ(G.g.over, WL_RESIGNED);
  CHECK_EQ(G.g.winner, 1);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_over");
  key(CAPP_KEY_ESC);
  run(500);
  CHECK_EQ(G.view, V_LIST);
  CHECK(G.rooms[0].over);
  CHECK_EQ(fakescreen_unset(), 0);
  fakescreen_dump("WORDLOOM_DUMP", "wordloom_list_over");

  /* Every device agrees with a replay of the server's log. */
  {
    char log[2048];
    int at = 0;
    uint32_t seed;
    for (i = 0; i < srv_room[0].n; i++)
      at += sprintf(log + at, "%d\t%s\t%s\n", i + 1, srv_room[0].from[i], srv_room[0].body[i]);
    wl_state_parse(srv_room[0].state, &seed);
    CHECK_EQ(wl_replay(&check, seed, "alex", "sam", log), 0);
    CHECK_EQ(check.score[0], G.rooms[0].score[0]);
    CHECK_EQ(check.score[1], G.rooms[0].score[1]);
  }
}

void test_wordloom_paint_the_cache_answers_while_the_server_is_away(void) {
  /* After the last test the card holds alex's game; the server goes. */
  int score0 = G.rooms[0].score[0];
  srv_user[0] = 0;
  {
    int n = srv_nrooms;
    memset(&G, 0, sizeof G);
    srv_nrooms = 0;
    wordloom_capp_main(&WA, 1, 0);
    run(500);
    CHECK_EQ(G.nrooms, 0);                     /* nobody is in anything */
    srv_nrooms = n;
  }
  /* The room file is there, and replays to the same score. */
  {
    char path[48];
    load_me();
    CHECK(!strcmp(G.me, "alex"));
    room_path(path, sizeof path, srv_room[0].id);
    CHECK(fakefs_get(path) != 0);
    G.nrooms = 1;
    G.rooms[0].id = srv_room[0].id;
    strcpy(G.rooms[0].p[0], "alex");
    strcpy(G.rooms[0].p[1], "sam");
    load_room(0);
    CHECK(G.rooms[0].known);
    CHECK_EQ(G.rooms[0].have, srv_room[0].n);
    replay(0);
    CHECK_EQ(G.rooms[0].score[0], score0);
  }
}
