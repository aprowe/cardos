/* Dashboard Link -- the device, from the dashboard: its card and its console.
 *
 * Was Remote Files. While this is open the dashboard (server/dashboard.html)
 * can browse and change the card, and run console lines here and read what
 * they print -- `do calendar sync`, `update apps`, `mem`, anything typed at
 * the console, captured instead of drawn (api->shell, API 34).
 *
 * The server cannot reach this device; it is behind somebody's router. So
 * while this is open, it asks the server for work, over and over: one POST
 * to /files/poll hands in the answer to the last job and takes the next one
 * (server/files.py has the protocol). The server holds an idle poll open for
 * a second and a half, so an open app costs a request every 1.5 s and a
 * transfer is one round trip a 3 KB chunk.
 *
 * Nothing on the card is reachable while this is closed: a thing you can
 * see is on, as with Share. Base64 both ways, because the HTTP layer apps
 * have carries text. Paths with .. in them are refused here as well as on
 * the server. An upload is written to NAME.part and only becomes NAME when
 * the last chunk is in, so a dropped connection never leaves half a file
 * under the real name.
 */

#include "kernel/app/capp.h"

static const CardApi *api;

#define CHUNK     3072                  /* bytes a job carries */
#define SH_MAX    4096                  /* what a console line may print back */
#define B64_MAX   (CHUNK / 3 * 4 + 8)
#define JOB_MAX   (B64_MAX + 400)       /* the job line, then base64 */
#define ANS_MAX   (B64_MAX + 64)        /* also holds SH_MAX of console output */
#define PATH_MAX_ 160
#define MAX_ENT   48
#define LINES     6
#define LINE_W    38
#define RETRY_MS  3000u

#define CLR_BG    CAPP_RGB(8, 10, 14)
#define CLR_FG    CAPP_RGB(226, 232, 242)
#define CLR_DIM   CAPP_RGB(130, 140, 158)
#define CLR_OK    CAPP_RGB(120, 220, 140)
#define CLR_BAD   CAPP_RGB(240, 120, 100)

static struct {
  int      inflight;
  int      last_id;                 /* the job `answer` belongs to; 0 none */
  uint32_t next_try;
  int      connected;               /* the last poll was answered */
  int      err;                     /* the last failure, negated HTTP code */
  uint32_t jobs, bytes_in, bytes_out;
  char     log[LINES][LINE_W];
  int      nlog;
  int      dirty;
  CRect    at;                      /* the last paint, for damage */
  int      have_at;
  char     job[JOB_MAX];
  char     answer[ANS_MAX];
  union {
    unsigned char raw[CHUNK];
    CappEntry     ent[MAX_ENT];
    char          sh[SH_MAX];
  } u;
} R;

/* ---- base64 ------------------------------------------------------------------ */

static const char B64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const unsigned char *in, int n, char *out, int cap) {
  int i, o = 0;
  for (i = 0; i < n; i += 3) {
    unsigned v = (unsigned)in[i] << 16;
    if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    if (o + 4 >= cap) return -1;
    out[o++] = B64[(v >> 18) & 63];
    out[o++] = B64[(v >> 12) & 63];
    out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? B64[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

static int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* Decoded length, or -1. Whitespace is skipped; '=' ends it. */
static int b64_decode(const char *in, unsigned char *out, int cap) {
  unsigned v = 0;
  int bits = 0, o = 0, d;
  for (; *in && *in != '='; in++) {
    if (*in == '\n' || *in == '\r' || *in == ' ') continue;
    if ((d = b64_val(*in)) < 0) return -1;
    v = (v << 6) | (unsigned)d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (unsigned char)(v >> bits);
    }
  }
  return o;
}

/* ---- the jobs -------------------------------------------------------------------- */

static int path_ok(const char *p) {
  int i;
  if (p[0] != '/') return 0;
  for (i = 0; p[i]; i++) {
    if (i >= PATH_MAX_ - 8) return 0;
    if (p[i] == '.' && p[i + 1] == '.' && (i == 0 || p[i - 1] == '/') &&
        (p[i + 2] == '/' || p[i + 2] == 0))
      return 0;
  }
  return 1;
}

static int same(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return *a == *b;
}

static int to_int(const char *s) {
  int v = 0;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v;
}

static void note(const char *op, const char *path, int n) {
  if (R.nlog == LINES) {
    api->mem_move(R.log[0], R.log[1], sizeof R.log - sizeof R.log[0]);
    R.nlog--;
  }
  if (n >= 0) api->fmt(R.log[R.nlog++], LINE_W, "%s %s %d", op, path, n);
  else api->fmt(R.log[R.nlog++], LINE_W, "%s %s", op, path);
  R.dirty = 1;
}

static int fail(const char *why) {
  api->fmt(R.answer, ANS_MAX, "error %s", why);
  return -1;
}

static int do_list(const char *path) {
  int n, i, o;
  n = api->list_ex(path, R.u.ent, MAX_ENT);
  if (n < 0) return fail("no such folder");
  o = api->fmt(R.answer, ANS_MAX, "ok\n");
  for (i = 0; i < n && o < ANS_MAX - CAPP_NAME_MAX - 24; i++)
    o += api->fmt(R.answer + o, (size_t)(ANS_MAX - o), "%c\t%lu\t%s\n",
                  R.u.ent[i].is_dir ? 'd' : 'f', (unsigned long)R.u.ent[i].size,
                  R.u.ent[i].name);
  note("list", path, n);
  return 0;
}

static int do_read(const char *path, const char *arg) {
  int off = to_int(arg), len = CHUNK, fd, n, o;
  const char *c = arg;
  while (*c && *c != ',') c++;
  if (*c == ',') len = to_int(c + 1);
  if (len > CHUNK) len = CHUNK;
  if ((fd = api->open(path, CAPP_O_READ)) < 0) return fail("cannot open");
  if (off && api->seek(fd, off, 0) < 0) { api->close(fd); return fail("cannot seek"); }
  n = api->read(fd, R.u.raw, (size_t)len);
  api->close(fd);
  if (n < 0) return fail("cannot read");
  o = api->fmt(R.answer, ANS_MAX, "ok\n");
  if (b64_encode(R.u.raw, n, R.answer + o, ANS_MAX - o) < 0) return fail("too long");
  R.bytes_out += (uint32_t)n;
  if (off == 0) note("send", path, -1);
  return 0;
}

static int do_write(const char *path, const char *arg, const char *data) {
  char part[PATH_MAX_ + 8];
  int off = to_int(arg), n, fd, w;
  n = b64_decode(data, R.u.raw, CHUNK);
  if (n < 0) return fail("bad data");
  api->fmt(part, sizeof part, "%s.part", path);
  if (off == 0) {
    fd = api->open(part, CAPP_O_WRITE | CAPP_O_CREATE | CAPP_O_TRUNC);
  } else {
    fd = api->open(part, CAPP_O_WRITE);
    /* Every chunk lands at the end of what is there, and that must be where
     * the server thinks it is: a chunk lost or sent twice is an error, not a
     * file with a hole or a repeat in it. */
    if (fd >= 0 && api->seek(fd, 0, 2) != off) { api->close(fd); return fail("out of order"); }
  }
  if (fd < 0) return fail("cannot write there");
  w = n ? api->write(fd, R.u.raw, (size_t)n) : 0;
  api->close(fd);
  if (w != n) return fail("card full?");
  R.bytes_in += (uint32_t)n;
  if (off == 0) note("receive", path, -1);
  api->fmt(R.answer, ANS_MAX, "ok");
  return 0;
}

static int do_commit(const char *path) {
  char part[PATH_MAX_ + 8];
  CappStat st;
  api->fmt(part, sizeof part, "%s.part", path);
  if (api->stat(part, &st) != 0) return fail("nothing was sent");
  /* The card's rename will not replace a file. */
  if (api->stat(path, &st) == 0 && api->remove(path) != 0) return fail("cannot replace it");
  if (api->rename(part, path) != 0) return fail("cannot rename");
  api->fmt(R.answer, ANS_MAX, "ok");
  return 0;
}

/* One job, `R.job`, into an answer in `R.answer`. The id, or 0 for none. */
static int handle_job(void) {
  char *f[4], *p = R.job, *data;
  int i;
  CappStat st;
  if (p[0] == 'i' && p[1] == 'd' && p[2] == 'l' && p[3] == 'e') { R.answer[0] = 0; return 0; }
  for (i = 0; i < 4; i++) {
    f[i] = p;
    while (*p && *p != '\t' && *p != '\n') p++;
    if (i < 3 && *p != '\t') { R.answer[0] = 0; return 0; }    /* not a job */
    if (*p) *p++ = 0;
  }
  data = p;
  R.jobs++;
  R.dirty = 1;
  if (!path_ok(f[2])) { fail("bad path"); return to_int(f[0]); }

  if (same(f[1], "list")) do_list(f[2]);
  else if (same(f[1], "stat")) {
    if (api->stat(f[2], &st) != 0) fail("no such file");
    else api->fmt(R.answer, ANS_MAX, "ok %c %lu", st.is_dir ? 'd' : 'f', (unsigned long)st.size);
  }
  else if (same(f[1], "read")) do_read(f[2], f[3]);
  else if (same(f[1], "write")) do_write(f[2], f[3], data);
  else if (same(f[1], "commit")) do_commit(f[2]);
  else if (same(f[1], "sh")) {
    /* A console line: its text is the job's data, one line. What it printed
     * comes back whole -- an `update` that runs for minutes answers when it
     * is done, which the server waits for. */
    int k;
    for (k = 0; data[k] && data[k] != '\n'; k++) {}
    data[k] = 0;
    note("$", data, -1);
    R.u.sh[0] = 0;
    if (!api->shell) fail("this firmware has no console for apps: update os");
    else {
      int rc = api->shell(data, R.u.sh, SH_MAX);
      api->fmt(R.answer, ANS_MAX, "%s\n%s", rc == 0 ? "ok" : "ok refused", R.u.sh);
    }
  }
  else if (same(f[1], "mkdir")) {
    if (api->mkdir(f[2]) != 0) fail("cannot make it");
    else { api->fmt(R.answer, ANS_MAX, "ok"); note("mkdir", f[2], -1); }
  }
  else if (same(f[1], "rm")) {
    if (api->remove(f[2]) != 0) fail("cannot delete (a folder must be empty)");
    else { api->fmt(R.answer, ANS_MAX, "ok"); note("delete", f[2], -1); }
  }
  else if (same(f[1], "mv")) {
    if (!path_ok(f[3])) fail("bad path");
    else if (api->rename(f[2], f[3]) != 0) fail("cannot rename (is the name taken?)");
    else { api->fmt(R.answer, ANS_MAX, "ok"); note("rename", f[3], -1); }
  }
  else fail("unknown job");
  return to_int(f[0]);
}

/* ---- the loop ------------------------------------------------------------------------ */

static void poll_start(uint32_t now) {
  char url[160];
  api->fmt(url, sizeof url, "%s/files/poll?id=%d", api->proxy(), R.last_id);
  /* "" bearer: the kernel signs a request to its own server. */
  if (api->http_start("POST", url, R.answer, "text/plain", "", 15000) == 0) R.inflight = 1;
  else R.next_try = now + RETRY_MS;                 /* busy: someone else's request */
}

static int app_tick(void *st, uint32_t now) {
  int n, was = R.connected;
  (void)st;
  if (!R.inflight) {
    if ((int32_t)(now - R.next_try) >= 0) poll_start(now);
  } else {
    n = api->http_poll(R.job, sizeof R.job);
    if (n != CAPP_HTTP_PENDING) {
      R.inflight = 0;
      if (n < 0) {
        R.connected = 0;
        R.err = n;
        R.last_id = 0;                      /* the answer is lost with it */
        R.answer[0] = 0;
        R.next_try = now + RETRY_MS;
      } else {
        R.connected = 1;
        R.last_id = handle_job();
        R.next_try = now;                   /* straight back for the next */
      }
    }
  }
  if (was != R.connected) R.dirty = 1;
  n = R.dirty;
  R.dirty = 0;
  /* Only what moves: the link line, the log and the counts. The name and
   * the address above them never change, and an app that returns 1 without
   * saying what changed has its whole screen redrawn -- once a poll, every
   * second and a half, for as long as it is open. */
  if (n && R.have_at) {
    CRect d;
    d.x = R.at.x; d.y = (short)(R.at.y + 34);
    d.w = R.at.w; d.h = (short)(R.at.h - 34);
    api->damage(d);
  }
  return n;
}

static void app_paint(void *st, CRect c) {
  int i, y;
  char line[48];
  (void)st;
  R.at = c;
  R.have_at = 1;
  api->fill(c, CLR_BG);                     /* clipped to the damage, if any */
  api->text((short)(c.x + 8), (short)(c.y + 6), "Dashboard Link", CLR_FG, CLR_BG);
  api->text((short)(c.x + 8), (short)(c.y + 20), "files and console at /dash", CLR_DIM, CLR_BG);
  if (R.connected)
    api->text((short)(c.x + 8), (short)(c.y + 34), "linked: the dashboard can reach it", CLR_OK, CLR_BG);
  else {
    api->fmt(line, sizeof line, "cannot reach the server (%d)", R.err);
    api->text((short)(c.x + 8), (short)(c.y + 34), R.err ? line : "connecting...",
              R.err ? CLR_BAD : CLR_DIM, CLR_BG);
  }
  y = c.y + 50;
  for (i = 0; i < R.nlog; i++, y += 10)
    api->text((short)(c.x + 8), (short)y, R.log[i], CLR_FG, CLR_BG);
  api->fmt(line, sizeof line, "%lu jobs  in %lu KB  out %lu KB", (unsigned long)R.jobs,
           (unsigned long)(R.bytes_in / 1024), (unsigned long)(R.bytes_out / 1024));
  api->text((short)(c.x + 8), (short)(c.y + c.h - 10), line, CLR_DIM, CLR_BG);
}

const CappInfo capp_info = {
  CAPP_API_VERSION,
  CAPP_FULLSCREEN | CAPP_NEEDS_PROXY,
  "Dashboard Link",
  /* 16x16: a card with an arrow going each way. */
  { 0x00, 0x00, 0x3F, 0xC0, 0x20, 0x60, 0x20, 0x50,
    0x20, 0x78, 0x24, 0x08, 0x2E, 0x08, 0x24, 0x08,
    0x24, 0x48, 0x20, 0x48, 0x20, 0xE8, 0x20, 0x48,
    0x3F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
  "while this is open, the dashboard (/dash on\n"
  "the server) can browse and change the card\n"
  "and run console commands here. leaving unlinks it.\n",
  0,
  0,
};

static CappUi UI;

int capp_main(const CardApi *a, int argc, char **argv) {
  (void)argc; (void)argv;
  api = a;
  api->mem_set(&R, 0, sizeof R);
  if (api->keep_awake) api->keep_awake(1);    /* it is working while it is open */
  R.next_try = api->ticks_ms();
  UI.paint = app_paint;
  UI.tick = app_tick;
  api->ui(&UI);
  return 0;
}
