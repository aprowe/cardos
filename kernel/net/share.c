/* WebDAV on a socket. See share.h; the protocol is in dav.c. */

#include "kernel/net/share.h"
#include "kernel/net/dav.h"
#include "kernel/net/wifi.h"
#include "kernel/fs/fs.h"
#include "kernel/fs/path.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "share";

#define PORT         80
#define BACKLOG      4
#define HDR_MAX      2048       /* request line and headers */
#define IO_MAX       4096       /* one transfer buffer, both directions */
#define HEAD_MAX     512        /* a response head */
#define XML_MAX      1024       /* one PROPFIND entry */
#define IDLE_MS      5000       /* a connection that says nothing */
#define TICK_MS      1000       /* how long a recv blocks before the stop flag is looked at */
#define SEND_MS      5000       /* a client that stops reading */
#define STOP_WAIT_MS 10000      /* how long share_stop waits for the task */
#define MIN_FREE     (40 * 1024)
#define LOG_LINES    8
#define LOG_MAX      64
#define MAX_DEPTH    8          /* recursive delete: levels of folder */

/* Below the shell, so serving never competes with drawing; core 1 with the
 * other background work. 8 KB of stack: a recursive delete keeps a directory
 * handle and a path per level, and lwIP's own calls are not frugal. */
#define S_PRIORITY   3
#define S_STACK      8192
#define S_CORE       1

typedef struct {
  char hdr[HDR_MAX];
  char io[IO_MAX];
  char head[HEAD_MAX];
  char xml[XML_MAX];
  /* Body bytes that arrived in the same packet as the headers. curl and
   * Python's http.client both do this for a small PUT, and dropping them
   * corrupts the file. They sit in hdr past the blank line; body_recv hands
   * them out before it asks the socket for more. */
  const char *pending;
  size_t      pending_n;
} Bufs;

static TaskHandle_t  s_task;
static QueueHandle_t s_log;
static Bufs         *s_b;
static volatile int  s_stop, s_done, s_running;
static int           s_owned_by_app;
static int           s_last_status;     /* what send_head last put on the wire */
static char          s_url[48];
static char          s_error[96];
static char          s_logline[LOG_MAX];

/* ---- state the shell reads ------------------------------------------- */

/* A task that outlived share_stop's patience (see there) finishes on its
 * own; whoever asks next collects it. */
static void reap(void) {
  if (s_running && s_stop && s_done) {
    s_task = NULL;
    free(s_b);
    s_b = NULL;
    s_running = 0;
  }
}

int  share_running(void) { reap(); return s_running; }
const char *share_url(void) { return share_running() ? s_url : ""; }
const char *share_error(void) { return s_error; }

const char *share_take_log(void) {
  if (!s_log || xQueueReceive(s_log, s_logline, 0) != pdTRUE) return NULL;
  return s_logline;
}

/* One line for the shell to show: "PUT /desktop/x.capp 201". Never blocks:
 * a full queue means nobody is looking, and dropping news is better than
 * stalling a transfer. A long path loses its tail rather than the status. */
static void note(const char *method, const char *path, int status) {
  char line[LOG_MAX];
  int room = LOG_MAX - 1 - (int)strlen(method) - 1 - 4;
  if (room < 8) room = 8;
  snprintf(line, sizeof line, "%s %.*s %d", method, room, path, status);
  xQueueSend(s_log, line, 0);
}

/* ---- the wire --------------------------------------------------------- */

/* recv, in ticks. The socket's receive timeout is one tick, not the idle
 * limit, so that a stop asked for mid-transfer is seen within a second
 * rather than after however long the client takes; the idle limit is
 * counted here. Returns bytes, 0 if the peer closed, -1 for an error, the
 * stop flag, or a client that has said nothing for IDLE_MS. */
static int recv_some(int fd, char *dst, size_t max) {
  int idle = 0;
  while (!s_stop) {
    int r = recv(fd, dst, max, 0);
    if (r > 0) return r;
    if (r == 0) return 0;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    idle += TICK_MS;
    if (idle >= IDLE_MS) return -1;
  }
  return -1;
}

/* The request body, from wherever the next byte of it is. */
static int body_recv(int fd, char *dst, size_t max) {
  if (s_b->pending_n) {
    size_t n = s_b->pending_n < max ? s_b->pending_n : max;
    memcpy(dst, s_b->pending, n);
    s_b->pending += n;
    s_b->pending_n -= n;
    return (int)n;
  }
  return recv_some(fd, dst, max);
}

static int send_all(int fd, const void *p, size_t n) {
  const char *c = p;
  while (n) {
    int w = send(fd, c, n, 0);
    if (w <= 0) return -1;
    c += w; n -= (size_t)w;
  }
  return 0;
}

static int send_head(int fd, int status, int32_t len, const char *type,
                     const char *extra, int keep) {
  int n = dav_response_head(status, len, type, extra, keep, s_b->head, HEAD_MAX);
  if (n < 0) return -1;
  s_last_status = status;
  return send_all(fd, s_b->head, (size_t)n);
}

static int send_chunk(int fd, const char *p, size_t n) {
  char sz[16];
  int k = snprintf(sz, sizeof sz, "%x\r\n", (unsigned)n);
  if (send_all(fd, sz, (size_t)k)) return -1;
  if (n && send_all(fd, p, n)) return -1;
  return send_all(fd, "\r\n", 2);
}

static int send_last_chunk(int fd) { return send_all(fd, "0\r\n\r\n", 5); }

/* Every method handler returns 0 when the connection may go on to the next
 * request (the reply honoured the client's keep-alive) and -1 when it must
 * close: the wire broke, or the reply said "Connection: close" because the
 * request's body was never read and the next bytes would be that body. */

/* A status with no body. */
static int reply(int fd, int status, const char *extra, int keep) {
  return send_head(fd, status, 0, NULL, extra, keep);
}

/* A status that ends the connection. */
static int refuse(int fd, int status) {
  reply(fd, status, NULL, 0);
  return -1;
}

/* Read and discard a request body we do not want. */
static int drain(int fd, uint32_t n) {
  while (n) {
    int r = body_recv(fd, s_b->io, n < IO_MAX ? n : IO_MAX);
    if (r <= 0) return -1;
    n -= (uint32_t)r;
  }
  return 0;
}

/* ---- files ------------------------------------------------------------ */

static int join(const char *dir, const char *name, char *out, size_t n) {
  int k = strcmp(dir, "/") == 0 ? snprintf(out, n, "/%s", name)
                                : snprintf(out, n, "%s/%s", dir, name);
  return (k > 0 && (size_t)k < n) ? 0 : -1;
}

static int parent_exists(const char *path) {
  char dir[FS_PATH_MAX];
  FsStat st;
  if (path_dirname(path, dir, sizeof dir) != 0) return 0;
  return fs_stat(dir, &st) == 0 && st.is_dir;
}

static int remove_tree(const char *path, int level) {
  FsDir d;
  FsEntry e;
  char child[FS_PATH_MAX];
  FsStat st;

  if (fs_stat(path, &st) != 0) return -1;
  if (!st.is_dir) return fs_remove(path);
  if (level >= MAX_DEPTH) return -1;

  /* FatFs cannot remove entries from a directory being iterated, so this
   * takes one child per pass and reopens. Slow, but a delete is rare. */
  for (;;) {
    int found = 0;
    if (fs_opendir(path, &d) != 0) return -1;
    while (fs_readdir(&d, &e) == 1) {
      if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
      found = 1;
      break;
    }
    fs_closedir(&d);
    if (!found) break;
    if (join(path, e.name, child, sizeof child) != 0) return -1;
    if (remove_tree(child, level + 1) != 0) return -1;
  }
  return fs_remove(path);
}

static int copy_file(const char *from, const char *to) {
  int in = fs_open(from, FS_O_READ), out, n, rc = 0;
  if (in < 0) return -1;
  out = fs_open(to, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (out < 0) { fs_close(in); return -1; }
  while ((n = fs_read(in, s_b->io, IO_MAX)) > 0)
    if (fs_write(out, s_b->io, (size_t)n) != n) { rc = -1; break; }
  if (n < 0) rc = -1;
  fs_close(in);
  fs_close(out);
  if (rc) fs_remove(to);
  return rc;
}

/* ---- methods ---------------------------------------------------------- */

static int do_options(int fd, const DavRequest *q) {
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  return reply(fd, 200,
    "DAV: 1,2\r\nMS-Author-Via: DAV\r\nAllow: " DAV_ALLOW "\r\n", q->keep_alive);
}

static int propfind_one(int fd, const char *path, const FsStat *st) {
  DavEntry e = { path, st->size, st->is_dir, st->mtime };
  int n = dav_propfind_entry(&e, s_b->xml, XML_MAX);
  if (n < 0) return -1;
  return send_chunk(fd, s_b->xml, (size_t)n);
}

static int do_propfind(int fd, const DavRequest *q) {
  FsStat st;
  FsDir d;
  FsEntry e;
  char child[FS_PATH_MAX];

  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (q->depth == DAV_DEPTH_INFINITY) return reply(fd, 403, NULL, q->keep_alive);
  if (fs_stat(q->path, &st) != 0) return reply(fd, 404, NULL, q->keep_alive);

  if (send_head(fd, 207, -1, "text/xml; charset=\"utf-8\"", NULL, q->keep_alive)) return -1;
  if (send_chunk(fd, DAV_MULTISTATUS_HEAD, strlen(DAV_MULTISTATUS_HEAD))) return -1;
  if (propfind_one(fd, q->path, &st)) return -1;

  if (q->depth == 1 && st.is_dir && fs_opendir(q->path, &d) == 0) {
    while (fs_readdir(&d, &e) == 1) {
      FsStat cs;
      if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;
      if (join(q->path, e.name, child, sizeof child) != 0) continue;
      cs.size = e.size; cs.is_dir = e.is_dir; cs.mtime = e.mtime;
      if (propfind_one(fd, child, &cs)) { fs_closedir(&d); return -1; }
    }
    fs_closedir(&d);
  }
  if (send_chunk(fd, DAV_MULTISTATUS_TAIL, strlen(DAV_MULTISTATUS_TAIL))) return -1;
  return send_last_chunk(fd);
}

static int do_get(int fd, const DavRequest *q, int head_only) {
  FsStat st;
  char extra[96];
  int in = -1, n;

  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (fs_stat(q->path, &st) != 0) return reply(fd, 404, NULL, q->keep_alive);
  if (st.is_dir) return reply(fd, 403, NULL, q->keep_alive);
  /* Open before promising a body: a head that says Content-Length and then
   * nothing is a truncated download, where a 500 is an honest answer. */
  if (!head_only) {
    in = fs_open(q->path, FS_O_READ);
    if (in < 0) return reply(fd, 500, NULL, q->keep_alive);
  }

  {
    char date[40];
    dav_http_date(st.mtime, date, sizeof date);
    snprintf(extra, sizeof extra, "Last-Modified: %s\r\nAccept-Ranges: none\r\n", date);
  }
  if (send_head(fd, 200, (int32_t)st.size, dav_content_type(q->path), extra, q->keep_alive)) {
    if (in >= 0) fs_close(in);
    return -1;
  }
  if (head_only) return 0;

  while ((n = fs_read(in, s_b->io, IO_MAX)) > 0)
    if (send_all(fd, s_b->io, (size_t)n)) { fs_close(in); return -1; }
  fs_close(in);
  return n < 0 ? -1 : 0;
}

/* PUT's refusals close the connection: the body is on its way and reading
 * it all just to say no is the only alternative. */
static int do_put(int fd, const DavRequest *q) {
  FsStat st;
  int existed, out;
  uint32_t left;

  if (q->chunked || !q->has_content_length) return refuse(fd, 411);
  existed = fs_stat(q->path, &st) == 0;
  if (existed && st.is_dir) return refuse(fd, 405);
  if (!parent_exists(q->path)) return refuse(fd, 409);

  out = fs_open(q->path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (out < 0) return refuse(fd, 507);

  if (q->expect_continue && send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25)) {
    fs_close(out); fs_remove(q->path); return -1;
  }
  left = q->content_length;
  while (left) {
    int r = body_recv(fd, s_b->io, left < IO_MAX ? left : IO_MAX);
    if (r <= 0) { fs_close(out); fs_remove(q->path); return -1; }
    if (fs_write(out, s_b->io, (size_t)r) != r) {
      fs_close(out); fs_remove(q->path);
      return refuse(fd, 507);
    }
    left -= (uint32_t)r;
  }
  fs_close(out);
  return reply(fd, existed ? 204 : 201, NULL, q->keep_alive);
}

static int do_delete(int fd, const DavRequest *q) {
  FsStat st;
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (fs_stat(q->path, &st) != 0) return reply(fd, 404, NULL, q->keep_alive);
  if (strcmp(q->path, "/") == 0) return reply(fd, 403, NULL, q->keep_alive);
  if (remove_tree(q->path, 0) != 0) return reply(fd, 403, NULL, q->keep_alive);
  return reply(fd, 204, NULL, q->keep_alive);
}

static int do_mkcol(int fd, const DavRequest *q) {
  FsStat st;
  if (q->chunked) return refuse(fd, 415);
  if (q->has_content_length && q->content_length) {
    if (drain(fd, q->content_length)) return -1;
    return reply(fd, 415, NULL, q->keep_alive);
  }
  if (fs_stat(q->path, &st) == 0) return reply(fd, 405, NULL, q->keep_alive);
  if (!parent_exists(q->path)) return reply(fd, 409, NULL, q->keep_alive);
  if (fs_mkdir(q->path) != 0) return reply(fd, 507, NULL, q->keep_alive);
  return reply(fd, 201, NULL, q->keep_alive);
}

/* MOVE and COPY share their preamble: a Destination, a source that exists,
 * a parent for the destination, and the Overwrite rule. Returns 0 to go on,
 * 1 when a refusal was already sent, -1 when the connection is gone. */
static int move_copy_check(int fd, const DavRequest *q, FsStat *src, int *dest_existed) {
  FsStat dst;
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (!q->dest[0]) return reply(fd, 400, NULL, q->keep_alive) ? -1 : 1;
  if (fs_stat(q->path, src) != 0) return reply(fd, 404, NULL, q->keep_alive) ? -1 : 1;
  if (strcmp(q->path, q->dest) == 0) return reply(fd, 403, NULL, q->keep_alive) ? -1 : 1;
  if (!parent_exists(q->dest)) return reply(fd, 409, NULL, q->keep_alive) ? -1 : 1;
  *dest_existed = fs_stat(q->dest, &dst) == 0;
  if (*dest_existed) {
    if (!q->overwrite) return reply(fd, 412, NULL, q->keep_alive) ? -1 : 1;
    if (remove_tree(q->dest, 0) != 0) return reply(fd, 403, NULL, q->keep_alive) ? -1 : 1;
  }
  return 0;
}

static int do_move(int fd, const DavRequest *q) {
  FsStat src;
  int existed = 0, rc = move_copy_check(fd, q, &src, &existed);
  if (rc) return rc < 0 ? -1 : 0;
  if (fs_rename(q->path, q->dest) != 0) return reply(fd, 403, NULL, q->keep_alive);
  return reply(fd, existed ? 204 : 201, NULL, q->keep_alive);
}

static int do_copy(int fd, const DavRequest *q) {
  FsStat src;
  int existed = 0, rc = move_copy_check(fd, q, &src, &existed);
  if (rc) return rc < 0 ? -1 : 0;
  if (src.is_dir) return reply(fd, 403, NULL, q->keep_alive);   /* the documented limit */
  if (copy_file(q->path, q->dest) != 0) return reply(fd, 507, NULL, q->keep_alive);
  return reply(fd, existed ? 204 : 201, NULL, q->keep_alive);
}

static int do_proppatch(int fd, const DavRequest *q) {
  FsStat st;
  int n;
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  if (fs_stat(q->path, &st) != 0) return reply(fd, 404, NULL, q->keep_alive);
  n = dav_proppatch_body(q->path, s_b->xml, XML_MAX);
  if (n < 0) return refuse(fd, 500);
  if (send_head(fd, 207, n, "text/xml; charset=\"utf-8\"", NULL, q->keep_alive)) return -1;
  return send_all(fd, s_b->xml, (size_t)n);
}

static int do_lock(int fd, const DavRequest *q) {
  int n;
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  n = dav_lock_body(q->path, s_b->xml, XML_MAX);
  if (n < 0) return refuse(fd, 500);
  if (send_head(fd, 200, n, "text/xml; charset=\"utf-8\"",
                "Lock-Token: <" DAV_LOCK_TOKEN ">\r\n", q->keep_alive)) return -1;
  return send_all(fd, s_b->xml, (size_t)n);
}

static int do_unlock(int fd, const DavRequest *q) {
  if (q->has_content_length && drain(fd, q->content_length)) return -1;
  return reply(fd, 204, NULL, q->keep_alive);
}

/* One request on an open connection. Returns 1 to keep the connection, 0 to
 * close it. */
static int serve_one(int fd) {
  DavRequest q;
  int have = 0, end = -1, rc;

  s_b->pending = NULL;
  s_b->pending_n = 0;
  s_last_status = 0;

  /* Headers first. A slow client sends them in pieces; a hostile one never
   * sends the blank line, and the size cap is what ends that. */
  while (end < 0) {
    int r;
    if (have >= HDR_MAX - 1) { reply(fd, 431, NULL, 0); return 0; }
    r = recv_some(fd, s_b->hdr + have, (size_t)(HDR_MAX - 1 - have));
    if (r <= 0) return 0;                   /* idle, stopping, or gone */
    have += r;
    end = dav_headers_end(s_b->hdr, (size_t)have);
  }
  if (dav_parse(s_b->hdr, (size_t)end, &q) != 0) {
    reply(fd, q.bad, NULL, 0);
    note("refused", q.bad == 403 ? "(path)" : "(bad request)", q.bad);
    return 0;
  }
  /* Whatever followed the blank line is the body's first bytes. A chunked
   * body is refused before anything reads it, so this is never a chunk. */
  if (have > end) {
    s_b->pending = s_b->hdr + end;
    s_b->pending_n = (size_t)(have - end);
  }

  switch (q.method) {
  case DAV_OPTIONS:   rc = do_options(fd, &q); break;
  case DAV_PROPFIND:  rc = do_propfind(fd, &q); break;
  case DAV_GET:       rc = do_get(fd, &q, 0); break;
  case DAV_HEAD:      rc = do_get(fd, &q, 1); break;
  case DAV_PUT:       rc = do_put(fd, &q); break;
  case DAV_DELETE:    rc = do_delete(fd, &q); break;
  case DAV_MKCOL:     rc = do_mkcol(fd, &q); break;
  case DAV_MOVE:      rc = do_move(fd, &q); break;
  case DAV_COPY:      rc = do_copy(fd, &q); break;
  case DAV_PROPPATCH: rc = do_proppatch(fd, &q); break;
  case DAV_LOCK:      rc = do_lock(fd, &q); break;
  case DAV_UNLOCK:    rc = do_unlock(fd, &q); break;
  default:
    if (q.chunked || (q.has_content_length && drain(fd, q.content_length))) {
      rc = refuse(fd, 405);
    } else {
      rc = reply(fd, 405, "Allow: " DAV_ALLOW "\r\n", q.keep_alive);
    }
    break;
  }
  note(dav_method_name(q.method), q.path, s_last_status);
  return rc == 0 && q.keep_alive;
}

/* ---- the task --------------------------------------------------------- */

static void share_task(void *arg) {
  int lfd, cfd = -1;
  struct sockaddr_in addr;
  int one = 1;
  struct timeval rcv = { TICK_MS / 1000, (TICK_MS % 1000) * 1000 };
  struct timeval snd = { SEND_MS / 1000, (SEND_MS % 1000) * 1000 };
  (void)arg;

  lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (lfd < 0) { ESP_LOGE(TAG, "no socket"); goto out; }
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) { ESP_LOGE(TAG, "bind failed"); goto out; }
  if (listen(lfd, BACKLOG) < 0) { ESP_LOGE(TAG, "listen failed"); goto out; }

  while (!s_stop) {
    fd_set rf;
    struct timeval wait = { 0, 200 * 1000 };
    FD_ZERO(&rf);
    FD_SET(lfd, &rf);
    if (select(lfd + 1, &rf, NULL, NULL, &wait) <= 0) continue;

    cfd = accept(lfd, NULL, NULL);
    if (cfd < 0) continue;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    while (!s_stop && serve_one(cfd)) { }
    close(cfd);
    cfd = -1;
  }

out:
  if (cfd >= 0) close(cfd);
  if (lfd >= 0) close(lfd);
  s_done = 1;
  vTaskDelete(NULL);
}

int share_start(int owned_by_app) {
  reap();
  if (s_running) { snprintf(s_error, sizeof s_error, "already sharing"); return -1; }
  if (!fs_mounted()) { snprintf(s_error, sizeof s_error, "no card mounted"); return -1; }
  if (!wifi_is_connected()) { snprintf(s_error, sizeof s_error, "not on wifi"); return -1; }
  {
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeb < MIN_FREE) {
      snprintf(s_error, sizeof s_error, "not enough memory: %u KB free, sharing needs %u",
               (unsigned)(freeb / 1024), (unsigned)(MIN_FREE / 1024));
      return -1;
    }
  }
  if (!s_log) s_log = xQueueCreate(LOG_LINES, LOG_MAX);
  s_b = malloc(sizeof *s_b);
  if (!s_log || !s_b) {
    free(s_b); s_b = NULL;
    snprintf(s_error, sizeof s_error, "no memory for buffers");
    return -1;
  }
  s_b->pending = NULL;
  s_b->pending_n = 0;

  snprintf(s_url, sizeof s_url, "http://%s/", wifi_ip());
  s_stop = 0;
  s_done = 0;
  s_owned_by_app = owned_by_app;
  if (xTaskCreatePinnedToCore(share_task, "share", S_STACK, NULL, S_PRIORITY,
                              &s_task, S_CORE) != pdPASS) {
    free(s_b); s_b = NULL;
    snprintf(s_error, sizeof s_error, "no memory for the task");
    return -1;
  }
  s_running = 1;
  s_error[0] = 0;
  ESP_LOGI(TAG, "sharing at %s", s_url);
  return 0;
}

void share_stop(void) {
  int waited = 0;
  if (!s_running) return;
  s_stop = 1;
  /* The task looks at the flag every 200 ms between connections and every
   * second inside one, so a transfer in flight ends within about a second
   * plus one write. What could outlast this wait is the card itself, and a
   * task that is inside fs_write holds the open-table lock: deleting it
   * from here would leave that lock taken and the shell's next file
   * operation waiting forever. So it is left to finish, and reap() collects
   * it on the next call. */
  while (!s_done && waited < STOP_WAIT_MS) { vTaskDelay(pdMS_TO_TICKS(20)); waited += 20; }
  if (!s_done) { ESP_LOGW(TAG, "share task has not stopped yet"); return; }
  reap();
  ESP_LOGI(TAG, "stopped");
}

void share_app_closed(void) {
  if (s_running && s_owned_by_app) share_stop();
}
