/* Dashboard Link on the host: the jobs the server sends, answered against
 * files on the PC.
 *
 * What must hold: base64 both ways for every length, an upload in chunks
 * that comes back byte for byte only after its commit, a chunk out of order
 * refused rather than written into a hole, and .. refused before the card is
 * touched. server/tests/test_files.py is the other half, the protocol.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "tinytest.h"
#include "fakeapi.h"

#define capp_info dashlink_capp_info
#define capp_main dashlink_capp_main
#include "apps/dashlink.c"
#undef capp_info
#undef capp_main

static void rn(const char *path, char *out, size_t n) {
  size_t j = 0, i;
  const char *pre = "dashlink_test";
  for (i = 0; pre[i] && j + 1 < n; i++) out[j++] = pre[i];
  for (i = 0; path[i] && j + 1 < n; i++) out[j++] = path[i] == '/' ? '_' : path[i];
  out[j] = 0;
}
static FILE *s_f;
static int r_open(const char *p, int flags) {
  char nm[200];
  rn(p, nm, sizeof nm);
  if (flags & CAPP_O_TRUNC) s_f = fopen(nm, "wb");
  else if (flags & CAPP_O_WRITE) s_f = fopen(nm, "r+b");
  else s_f = fopen(nm, "rb");
  return s_f ? 3 : -1;
}
static int r_read(int fd, void *b, size_t n) { (void)fd; return (int)fread(b, 1, n, s_f); }
static int r_write(int fd, const void *b, size_t n) { (void)fd; return (int)fwrite(b, 1, n, s_f); }
static int r_seek(int fd, int32_t off, int whence) {
  (void)fd;
  if (fseek(s_f, off, whence == 2 ? SEEK_END : SEEK_SET) != 0) return -1;
  return (int)ftell(s_f);
}
static void r_close(int fd) { (void)fd; fclose(s_f); s_f = NULL; }
static int r_remove(const char *p) { char nm[200]; rn(p, nm, sizeof nm); return remove(nm); }
static int r_stat(const char *p, CappStat *st) {
  char nm[200];
  FILE *t;
  rn(p, nm, sizeof nm);
  if ((t = fopen(nm, "rb")) == NULL) return -1;
  fseek(t, 0, SEEK_END);
  st->size = (uint32_t)ftell(t);
  st->is_dir = 0;
  fclose(t);
  return 0;
}
static int r_rename(const char *a, const char *b) {
  char x[200], y[200];
  CappStat st;
  if (r_stat(b, &st) == 0) return -1;            /* the card's rename does not replace */
  rn(a, x, sizeof x);
  rn(b, y, sizeof y);
  return rename(x, y) == 0 ? 0 : -1;
}
static int r_list(const char *dir, CappEntry *out, int max) {
  (void)max;
  if (strcmp(dir, "/apps") != 0) return -1;
  memset(out, 0, 2 * sizeof *out);
  strcpy(out[0].name, "Tools"); out[0].is_dir = 1;
  strcpy(out[1].name, "cat.capp"); out[1].size = 544;
  return 2;
}

static CardApi RF;

static void ropen(void) {
  fakeapi_init(&RF);
  RF.open = r_open; RF.read = r_read; RF.write = r_write; RF.seek = r_seek;
  RF.close = r_close; RF.remove = r_remove; RF.rename = r_rename; RF.stat = r_stat;
  RF.list_ex = r_list;
  api = &RF;
  memset(&R, 0, sizeof R);
}

static int job(const char *text) {
  snprintf(R.job, sizeof R.job, "%s", text);
  return handle_job();
}

void test_dashlink_base64_round_trips_every_length(void) {
  unsigned char in[10], back[10];
  char enc[32];
  int n, i;
  for (i = 0; i < 10; i++) in[i] = (unsigned char)(i * 37 + 200);
  for (n = 0; n <= 10; n++) {
    CHECK(b64_encode(in, n, enc, sizeof enc) == (n + 2) / 3 * 4);
    CHECK_EQ(b64_decode(enc, back, sizeof back), n);
    CHECK(memcmp(in, back, (size_t)n) == 0);
  }
  CHECK(!strcmp((b64_encode((const unsigned char *)"hi!", 3, enc, sizeof enc), enc), "aGkh"));
  CHECK_EQ(b64_decode("not*base64", back, sizeof back), -1);
}

void test_dashlink_an_upload_in_chunks_lands_only_at_commit(void) {
  char line[64];
  CappStat st;
  ropen();
  r_remove("/up.txt");
  r_remove("/up.txt.part");
  CHECK_EQ(job("7\twrite\t/up.txt\t0\naGVsbG8g"), 7);           /* "hello " */
  CHECK(!strcmp(R.answer, "ok"));
  CHECK_EQ(job("8\twrite\t/up.txt\t6\nd29ybGQ="), 8);           /* "world" */
  CHECK(!strcmp(R.answer, "ok"));
  CHECK(r_stat("/up.txt", &st) != 0);                          /* not yet */
  job("9\tcommit\t/up.txt\t\n");
  CHECK(!strcmp(R.answer, "ok"));
  CHECK(r_stat("/up.txt", &st) == 0 && st.size == 11);
  CHECK(r_stat("/up.txt.part", &st) != 0);
  /* And back, as the download reads it. */
  job("10\tread\t/up.txt\t0,3072\n");
  snprintf(line, sizeof line, "%s", R.answer);
  CHECK(!strcmp(line, "ok\naGVsbG8gd29ybGQ="));
  job("11\tstat\t/up.txt\t\n");
  CHECK(!strcmp(R.answer, "ok f 11"));
  /* Sending it again replaces it: the card's rename would not. */
  job("12\twrite\t/up.txt\t0\neA==");
  job("13\tcommit\t/up.txt\t\n");
  CHECK(!strcmp(R.answer, "ok"));
  CHECK(r_stat("/up.txt", &st) == 0 && st.size == 1);
  r_remove("/up.txt");
}

void test_dashlink_a_chunk_out_of_order_is_refused(void) {
  ropen();
  r_remove("/gap.part");
  job("1\twrite\t/gap\t0\naGVsbG8g");
  job("2\twrite\t/gap\t100\nd29ybGQ=");
  CHECK(!strncmp(R.answer, "error out of order", 18));
  r_remove("/gap.part");
}

void test_dashlink_dotdot_is_refused_before_the_card_is_touched(void) {
  ropen();
  RF.open = NULL;                                  /* a touch would crash */
  CHECK_EQ(job("3\tread\t/apps/../config/google.txt\t0,10\n"), 3);
  CHECK(!strcmp(R.answer, "error bad path"));
  job("4\tmv\t/a\t/b/../../c\n");
  CHECK(!strcmp(R.answer, "error bad path"));
  CHECK(path_ok("/notes..txt"));                   /* dots in a name are fine */
}

void test_dashlink_a_folder_lists_its_entries(void) {
  ropen();
  job("5\tlist\t/apps\t\n");
  CHECK(!strcmp(R.answer, "ok\nd\t0\tTools\nf\t544\tcat.capp\n"));
  job("6\tlist\t/nope\t\n");
  CHECK(!strcmp(R.answer, "error no such folder"));
}

/* A console line: run through api->shell, its output the answer. */
static char s_ran[128];
static int r_shell(const char *line, char *out, size_t n) {
  snprintf(s_ran, sizeof s_ran, "%s", line);
  if (!strncmp(line, "launch", 6)) { snprintf(out, n, "launch takes over\n"); return -1; }
  snprintf(out, n, "heap free 120000\n");
  return 0;
}

void test_dashlink_runs_a_console_line(void) {
  ropen();
  RF.shell = r_shell;
  CHECK_EQ(job("9\tsh\t/\t\nmem\n"), 9);
  CHECK(!strcmp(s_ran, "mem"));
  CHECK(!strcmp(R.answer, "ok\nheap free 120000\n"));
  job("10\tsh\t/\t\nlaunch\n");
  CHECK(!strcmp(R.answer, "ok refused\nlaunch takes over\n"));
}

void test_dashlink_idle_is_no_job(void) {
  ropen();
  CHECK_EQ(job("idle\n"), 0);
  CHECK_EQ(R.answer[0], 0);
  CHECK_EQ(R.jobs, 0);
}
