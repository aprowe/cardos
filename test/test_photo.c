/* Photos' sync on the host, against an in-memory card and a server: a photo
 * is deleted from the card only when the server's whole list leaves it out. */

#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define capp_info photo_capp_info
#define capp_main photo_capp_main
#include "apps/photo.c"
#undef capp_info
#undef capp_main

static CardApi PA;
static int s_npics, s_long;

static int ph_http(const char *m, const char *url, const char *body, const char *ct,
                   const char *bearer, char *out, size_t n, int ms) {
  size_t o = 0;
  int i;
  (void)m; (void)body; (void)ct; (void)bearer; (void)ms;
  if (!strstr(url, "/photos")) return -404;
  for (i = 0; i < s_npics; i++) {
    char line[300];
    int k = snprintf(line, sizeof line, "p%02d\t%s%d\t240\t135\n", i,
                     s_long ? "a very long name for a photograph, long enough that the list"
                              " outgrows the buffer the app keeps for it, which is the point "
                            : "Photo ", i);
    if (o + (size_t)k > n - 1) { memcpy(out + o, line, n - 1 - o); o = n - 1; break; }
    memcpy(out + o, line, (size_t)k);
    o += (size_t)k;
  }
  out[o] = 0;
  return (int)o;
}
static int ph_download(const char *url, const char *path, int ms) {
  (void)url; (void)ms;
  fakefs_put(path, "CIMG");
  return 4;
}

static void fresh(void) {
  fakeapi_init(&PA);
  fakefs_mem(&PA);
  PA.http = ph_http;
  PA.http_download = ph_download;
  api = &PA;
  memset(&P, 0, sizeof P);
  api->fmt(P.dir, sizeof P.dir, "%s", DIR);
  fakefs_put(DIR "/old.img", "CIMG");
  fakefs_put(INDEX, "old\tOld photo\n");
  index_load();
  s_long = 0;
}

static void sync_all(void) {
  int guard = 0;
  P.sync = SYNC_LIST;
  sync_list();
  while (P.sync == SYNC_GET && guard++ < 200) sync_get();
}

void test_photo_a_whole_list_deletes_what_the_dashboard_dropped(void) {
  fresh();
  s_npics = 2;
  sync_all();
  CHECK(!fakefs_exists(DIR "/old.img"));
  CHECK(fakefs_exists(DIR "/p00.img"));
  CHECK(fakefs_exists(DIR "/p01.img"));
  CHECK_EQ(P.nsynced, 2);
}

void test_photo_a_list_that_filled_the_reply_deletes_nothing(void) {
  fresh();
  s_npics = 40;
  s_long = 1;
  sync_all();
  CHECK(fakefs_exists(DIR "/old.img"));
  CHECK(fakefs_exists(DIR "/p00.img"));
  CHECK(strstr(fakefs_get(INDEX), "old\tOld photo") != NULL);
}

void test_photo_a_list_that_ran_to_the_maximum_deletes_nothing(void) {
  fresh();
  s_npics = MAXPICS + 2;
  sync_all();
  CHECK(fakefs_exists(DIR "/old.img"));
  CHECK_EQ(P.nsynced, MAXPICS);
}
