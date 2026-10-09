/* Notes on the host: the sync, against a card and a server that are both
 * in memory. Each case is one row of the table at the top of apps/notes.c. */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"

#define capp_info notes_capp_info
#define capp_main notes_capp_main
#include "apps/notes.c"
#undef capp_info
#undef capp_main

/* ---- a card -------------------------------------------------------------------- */

#define FS_FILES 24
static struct { char path[96]; char data[4096]; int len, used; } F[FS_FILES];
static struct { int f, pos, write; } FD[4];

static int fs_find(const char *p) {
  int i;
  for (i = 0; i < FS_FILES; i++) if (F[i].used && !strcmp(F[i].path, p)) return i;
  return -1;
}
static int n_open(const char *p, int flags) {
  int i = fs_find(p), d;
  if (i < 0) {
    if (!(flags & CAPP_O_WRITE)) return -1;
    for (i = 0; i < FS_FILES && F[i].used; i++) {}
    if (i == FS_FILES) return -1;
    F[i].used = 1; F[i].len = 0; snprintf(F[i].path, sizeof F[i].path, "%s", p);
  }
  if (flags & CAPP_O_TRUNC) F[i].len = 0;
  for (d = 0; d < 4 && FD[d].f >= 0; d++) {}
  FD[d].f = i; FD[d].pos = 0; FD[d].write = (flags & CAPP_O_WRITE) != 0;
  return d;
}
static int n_read(int d, void *b, size_t n) {
  int f = FD[d].f, left = F[f].len - FD[d].pos;
  if ((int)n > left) n = (size_t)left;
  memcpy(b, F[f].data + FD[d].pos, n);
  FD[d].pos += (int)n;
  return (int)n;
}
static int n_write(int d, const void *b, size_t n) {
  int f = FD[d].f;
  memcpy(F[f].data + FD[d].pos, b, n);
  FD[d].pos += (int)n;
  if (FD[d].pos > F[f].len) F[f].len = FD[d].pos;
  return (int)n;
}
static void n_close(int d) { FD[d].f = -1; }
static int n_remove(const char *p) { int i = fs_find(p); if (i < 0) return -1; F[i].used = 0; return 0; }
static int n_rename(const char *a, const char *b) {
  int i = fs_find(a);
  if (i < 0 || fs_find(b) >= 0) return -1;
  snprintf(F[i].path, sizeof F[i].path, "%s", b);
  return 0;
}
static int n_stat(const char *p, CappStat *st) {
  int i = fs_find(p);
  if (i < 0) return -1;
  if (st) { st->size = (uint32_t)F[i].len; st->is_dir = 0; }
  return 0;
}
static int n_mkdir(const char *p) { (void)p; return 0; }
static int n_list(const char *dir, CappEntry *out, int max) {
  int i, n = 0;
  size_t dl = strlen(dir);
  for (i = 0; i < FS_FILES && n < max; i++) {
    const char *rest;
    if (!F[i].used || strncmp(F[i].path, dir, dl) || F[i].path[dl] != '/') continue;
    rest = F[i].path + dl + 1;
    if (strchr(rest, '/')) continue;
    memset(&out[n], 0, sizeof out[n]);
    snprintf(out[n].name, sizeof out[n].name, "%s", rest);
    out[n].size = (uint32_t)F[i].len;
    n++;
  }
  return n;
}

static void put(const char *path, const char *text) {
  int d = n_open(path, CAPP_O_WRITE | CAPP_O_TRUNC);
  n_write(d, text, strlen(text));
  n_close(d);
}
static const char *get(const char *path) {
  int i = fs_find(path);
  if (i < 0) return NULL;
  F[i].data[F[i].len] = 0;
  return F[i].data;
}

/* ---- a server -------------------------------------------------------------------- */

#define SRV_NOTES 16
static struct { char id[12]; char text[2048]; int used; } S[SRV_NOTES];
static int s_next = 1, s_calls;

static void hex(const char *t, char out[9]) { fnv_hex(t, (int)strlen(t), out); }
static int s_find(const char *id) {
  int i;
  for (i = 0; i < SRV_NOTES; i++) if (S[i].used && !strcmp(S[i].id, id)) return i;
  return -1;
}
static int s_add(const char *text) {
  int i;
  for (i = 0; i < SRV_NOTES && S[i].used; i++) {}
  S[i].used = 1;
  snprintf(S[i].id, sizeof S[i].id, "n%d", s_next++);
  snprintf(S[i].text, sizeof S[i].text, "%s", text);
  return i;
}

static int n_http(const char *m, const char *url, const char *body, const char *ct,
                  const char *bearer, char *out, size_t n, int ms) {
  const char *rel = strstr(url, "/notes"), *q = strstr(url, "id=");
  char h[9];
  int i;
  (void)ct; (void)bearer; (void)ms;
  s_calls++;
  out[0] = 0;
  if (!strcmp(m, "GET") && !strcmp(rel, "/notes")) {
    size_t o = 0;
    for (i = 0; i < SRV_NOTES; i++) if (S[i].used) {
      char title[48];
      int k = 0;
      while (S[i].text[k] && S[i].text[k] != '\n' && k < 40) { title[k] = S[i].text[k]; k++; }
      title[k] = 0;
      hex(S[i].text, h);
      o += (size_t)snprintf(out + o, n - o, "%s\t%s\t1\t%s\n", S[i].id, h, title);
    }
    return (int)o;
  }
  if (!strcmp(m, "POST") && !q) {
    i = s_add(body);
    hex(S[i].text, h);
    return snprintf(out, n, "%s\t%s\n", S[i].id, h);
  }
  i = q ? s_find(q + 3) : -1;
  if (i < 0) return -404;
  if (!strcmp(m, "GET")) return snprintf(out, n, "%s", S[i].text);
  if (!strcmp(m, "POST")) {
    snprintf(S[i].text, sizeof S[i].text, "%s", body);
    hex(S[i].text, h);
    return snprintf(out, n, "%s\t%s\n", S[i].id, h);
  }
  if (!strcmp(m, "DELETE")) { S[i].used = 0; return snprintf(out, n, "ok\n"); }
  return -400;
}

/* ---- the app over both ----------------------------------------------------------- */

static int n_fmt(char *b, size_t n, const char *f, ...) {
  va_list ap; int r;
  va_start(ap, f); r = vsnprintf(b, n, f, ap); va_end(ap);
  return r;
}
static void *n_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static void *n_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *n_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static size_t n_strlen(const char *s) { return strlen(s); }
static int n_ready(void) { return 1; }
static const char *n_proxy(void) { return "http://srv"; }

static CardApi NF;

static void fresh(void) {
  int i;
  memset(F, 0, sizeof F);
  memset(S, 0, sizeof S);
  for (i = 0; i < 4; i++) FD[i].f = -1;
  s_next = 1;
  memset(&NF, 0, sizeof NF);
  NF.fmt = n_fmt; NF.mem_set = n_memset; NF.mem_cpy = n_memcpy; NF.mem_move = n_memmove;
  NF.str_len = n_strlen; NF.open = n_open; NF.read = n_read; NF.write = n_write;
  NF.close = n_close; NF.remove = n_remove; NF.rename = n_rename; NF.stat = n_stat;
  NF.mkdir = n_mkdir; NF.list_ex = n_list; NF.http = n_http; NF.net_ready = n_ready;
  NF.proxy = n_proxy;
  api = &NF;
  memset(&N, 0, sizeof N);
}

static void sync_all(void) {
  int guard = 0;
  sync_begin();
  while (N.syncing && guard++ < 200) sync_tick();
}

void test_notes_a_server_note_arrives_as_a_file_named_by_its_title(void) {
  fresh();
  s_add("Groceries\n- milk\n");
  sync_all();
  CHECK(!strcmp(get(DIR "/Groceries.md"), "Groceries\n- milk\n"));
  CHECK_EQ(N.nidx, 1);
  /* Nothing changed: the next sync moves nothing. */
  s_calls = 0;
  sync_all();
  CHECK_EQ(s_calls, 1);                                 /* the list, and only that */
}

void test_notes_a_change_here_goes_up_and_one_there_comes_down(void) {
  fresh();
  s_add("Plan\nv1\n");
  sync_all();
  put(DIR "/Plan.md", "Plan\nv2 here\n");
  sync_all();
  CHECK(!strcmp(S[0].text, "Plan\nv2 here\n"));
  snprintf(S[0].text, sizeof S[0].text, "Plan\nv3 there\n");
  sync_all();
  CHECK(!strcmp(get(DIR "/Plan.md"), "Plan\nv3 there\n"));
}

void test_notes_a_change_on_both_keeps_both(void) {
  fresh();
  s_add("Plan\nv1\n");
  sync_all();
  put(DIR "/Plan.md", "Plan\nmine\n");
  snprintf(S[0].text, sizeof S[0].text, "Plan\ntheirs\n");
  sync_all();
  CHECK(!strcmp(get(DIR "/Plan.md"), "Plan\ntheirs\n"));
  CHECK(!strcmp(get(DIR "/Plan (conflict).md"), "Plan\nmine\n"));
  CHECK(s_find("n2") >= 0 && !strcmp(S[s_find("n2")].text, "Plan\nmine\n"));
}

void test_notes_a_new_file_here_goes_up(void) {
  fresh();
  put(DIR "/Ideas.md", "Ideas\n- a\n");
  sync_all();
  CHECK(s_find("n1") >= 0 && !strcmp(S[s_find("n1")].text, "Ideas\n- a\n"));
  CHECK_EQ(N.nidx, 1);
}

/* The plan names a new file by its row in the list, not by a copy of its
 * name: each goes up once, whatever order the list sorts them in. */
void test_notes_several_new_files_go_up_once_each(void) {
  int i, n = 0;
  fresh();
  put(DIR "/Zebra.md", "Zebra\n");
  put(DIR "/Apple.md", "Apple\n");
  put(DIR "/Mango.md", "Mango\n");
  sync_all();
  for (i = 0; i < SRV_NOTES; i++) if (S[i].used) n++;
  CHECK_EQ(n, 3);
  CHECK_EQ(N.nidx, 3);
  s_calls = 0;
  sync_all();                         /* nothing changed: nothing goes up again */
  for (i = 0, n = 0; i < SRV_NOTES; i++) if (S[i].used) n++;
  CHECK_EQ(n, 3);
}

void test_notes_deleting_here_deletes_there_and_the_other_way(void) {
  fresh();
  s_add("One\n");
  s_add("Two\n");
  sync_all();
  n_remove(DIR "/One.md");
  sync_all();
  CHECK(s_find("n1") < 0);                              /* gone from the server */
  S[s_find("n2")].used = 0;                             /* deleted on the dashboard */
  sync_all();
  CHECK(get(DIR "/Two.md") == NULL);
  CHECK_EQ(N.nidx, 0);
}

void test_notes_a_note_deleted_there_but_changed_here_comes_back(void) {
  fresh();
  s_add("Keep\nold\n");
  sync_all();
  put(DIR "/Keep.md", "Keep\nedited\n");
  S[0].used = 0;
  sync_all();
  CHECK(get(DIR "/Keep.md") != NULL);
  CHECK(s_find("n2") >= 0 && !strcmp(S[s_find("n2")].text, "Keep\nedited\n"));
}

void test_notes_the_hash_is_the_servers(void) {
  char h[9];
  fnv_hex("", 0, h);
  CHECK(!strcmp(h, "811c9dc5"));
  fnv_hex("a", 1, h);
  CHECK(!strcmp(h, "e40c292c"));
}
