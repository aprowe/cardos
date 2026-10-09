/* Music's sync on the host, against an in-memory card and a server. The
 * point is what it deletes: only what the server's whole list leaves out. */

#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define capp_info music_capp_info
#define capp_main music_capp_main
#include "apps/music.c"
#undef capp_info
#undef capp_main

static CardApi MA;

#define SRV_MAX 80
static struct { char id[12]; char title[200]; } SV[SRV_MAX];
static int s_nsv;

static int mu_http(const char *m, const char *url, const char *body, const char *ct,
                   const char *bearer, char *out, size_t n, int ms) {
  size_t o = 0;
  int i;
  (void)m; (void)body; (void)ct; (void)bearer; (void)ms;
  if (!strstr(url, "/music")) return -404;
  /* As http.c does: fill the buffer and stop, and say how much fitted. */
  for (i = 0; i < s_nsv; i++) {
    char line[260];
    int k = snprintf(line, sizeof line, "%s\t%s\t1000\t4\n", SV[i].id, SV[i].title);
    if (o + (size_t)k > n - 1) { memcpy(out + o, line, n - 1 - o); o = n - 1; break; }
    memcpy(out + o, line, (size_t)k);
    o += (size_t)k;
  }
  out[o] = 0;
  return (int)o;
}
static int mu_stream(const char *url, int (*cb)(void *, const uint8_t *, int), void *ctx, int ms) {
  (void)url; (void)ms;
  return cb(ctx, (const uint8_t *)"RIFF", 4) ? -1 : 4;
}

static void serve(int n, int long_titles) {
  int i;
  s_nsv = n;
  for (i = 0; i < n; i++) {
    snprintf(SV[i].id, sizeof SV[i].id, "t%02d", i);
    if (long_titles) {
      memset(SV[i].title, 'x', 160);
      SV[i].title[160] = 0;
    } else {
      snprintf(SV[i].title, sizeof SV[i].title, "Track %d", i);
    }
  }
}

static void fresh(void) {
  fakeapi_init(&MA);
  fakefs_mem(&MA);
  MA.http = mu_http;
  MA.http_stream = mu_stream;
  api = &MA;
  memset(&M, 0, sizeof M);
  M.playing = -1;
  fakefs_put(DIR "/old1.wav", "RIFF");
  fakefs_put(INDEX, "old1\tOld one\t1000\n");
  index_load();
}

void test_music_a_whole_list_deletes_what_the_server_dropped(void) {
  fresh();
  serve(2, 0);
  sync_run();
  CHECK(!fakefs_exists(DIR "/old1.wav"));
  CHECK(fakefs_exists(DIR "/t00.wav"));
  CHECK(fakefs_exists(DIR "/t01.wav"));
  CHECK_EQ(M.n, 2);
}

void test_music_a_list_that_filled_the_reply_deletes_nothing(void) {
  fresh();
  serve(40, 1);                    /* ~170 bytes a line: 4.6 KB holds 27 */
  sync_run();
  CHECK(fakefs_exists(DIR "/old1.wav"));
  /* What did come is fetched; the cut line is not taken for a track. */
  CHECK(fakefs_exists(DIR "/t00.wav"));
  CHECK(!fakefs_exists(DIR "/t39.wav"));
  /* The track kept is still listed, and survives into the index. */
  {
    int i, found = 0;
    for (i = 0; i < M.n; i++) if (!strcmp(M.id[i], "old1")) found = 1;
    CHECK(found);
    CHECK(strstr(fakefs_get(INDEX), "old1\t") != NULL);
  }
}

void test_music_a_list_that_ran_to_the_maximum_deletes_nothing(void) {
  fresh();
  serve(MAXT + 5, 0);
  sync_run();
  CHECK(fakefs_exists(DIR "/old1.wav"));
  CHECK_EQ(M.n, MAXT);
}

void test_music_a_track_put_on_the_card_by_hand_is_never_deleted(void) {
  fresh();
  fakefs_put(DIR "/mine.wav", "RIFF");
  serve(1, 0);
  sync_run();
  CHECK(fakefs_exists(DIR "/mine.wav"));
  CHECK(!fakefs_exists(DIR "/old1.wav"));
}
