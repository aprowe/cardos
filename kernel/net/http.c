/* HTTP(S) requests, blocking. See http.h.
 *
 * Five public transfers -- a request into a buffer, a file posted, a file
 * exchanged for a file, a download, a stream -- and one way of doing each
 * step they share. They used to be five copies of the same setup and
 * teardown, and had drifted: -4 meant "not enough memory" in four of them
 * and "not 2xx" in the stream, and the reason string was reset by two and
 * left stale by three, so net_status reported an earlier request's failure.
 * Now every transfer is
 *
 *   req_open        the network, the memory, the client, the handshake
 *   send_*          the body, from memory or a file
 *   req_headers     the status line and headers
 *   read_*          the reply, into a buffer, a file or a sink
 *   req_status      2xx or -status
 *   req_close
 *
 * and the codes are pinned in http.h: -1 no network, -2 bad arguments,
 * -3 transport (s_why always says which step), -4 out of memory, -NNN the
 * HTTP status. */

#include "kernel/net/http.h"
#include "kernel/sys/busy.h"
#include "kernel/net/wifi.h"
#include "kernel/fs/fs.h"

#include <string.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

static const char *TAG = "http";

/* A kilobyte of scratch for moving a file through the socket, one per
 * transfer, from the heap for as long as the transfer runs.
 *
 * It used to be one static shared by every transfer, on the grounds that
 * all of them were blocking calls on one task. They are not: the agent's
 * exchange runs on httpq's task while voice, screenshots and
 * api->http_upload post files from the shell, and a G0 dictation during a
 * Claude turn had both writing it at once. Not on the stack either: the
 * shell's is 8 KB with an app's frames on it, and httpq's 6 KB has to hold a
 * TLS handshake (it logs its high-water mark; see httpq.c). */
#define CHUNK 1024

/* The `auth` string, applied. A bare token is a bearer; anything with a
 * colon in it is header lines, "Name: value" separated by newlines, sent as
 * they are. One parameter covers OAuth, an API key and a version header
 * without the signature growing a field per kind. */
static void set_auth(esp_http_client_handle_t cli, const char *auth) {
  char hdr[600];
  const char *p;
  if (!auth || !*auth) return;
  if (!strchr(auth, ':')) {
    snprintf(hdr, sizeof hdr, "Bearer %s", auth);
    esp_http_client_set_header(cli, "Authorization", hdr);
    return;
  }
  p = auth;
  while (*p) {
    const char *end = p, *colon;
    while (*end && *end != '\n' && *end != '\r') end++;
    colon = memchr(p, ':', (size_t)(end - p));
    if (colon && end - p < (int)sizeof hdr) {
      char name[64], *v;
      size_t nl_ = (size_t)(colon - p);
      if (nl_ < sizeof name) {
        memcpy(name, p, nl_); name[nl_] = 0;
        memcpy(hdr, colon + 1, (size_t)(end - colon - 1)); hdr[end - colon - 1] = 0;
        for (v = hdr; *v == ' '; v++) {}
        esp_http_client_set_header(cli, name, v);
      }
    }
    p = end;
    while (*p == '\n' || *p == '\r') p++;
  }
}

/* What a TLS handshake needs on top of whatever the caller is holding: the
 * record buffers, the peer certificate while it is being verified, and the
 * socket. Measured by watching the free heap either side of a request, not
 * taken from a datasheet, and deliberately a little pessimistic -- a request
 * that fails halfway through leaves the app with a torn reply, while one
 * refused up front leaves it able to say why. */
#define TLS_HEADROOM 34000

static char s_why[96];

const char *http_last_error(void) { return s_why[0] ? s_why : "no error"; }

/* Why, in words, with the heap beside it. Not logged to the card from
 * here: this runs on httpq's task too, and the caller logs it. */
static void why_failed(const char *what, esp_err_t e) {
  unsigned kb = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024);
  if (e != ESP_OK) snprintf(s_why, sizeof s_why, "%s: %s, %u KB free", what, esp_err_to_name(e), kb);
  else snprintf(s_why, sizeof s_why, "%s, %u KB free", what, kb);
  ESP_LOGW(TAG, "%s", s_why);
}

/* The same for a failure that is not about the heap. Returns `rc`. */
static int fail(int rc, const char *what) {
  snprintf(s_why, sizeof s_why, "%s", what);
  ESP_LOGW(TAG, "%s (%d)", s_why, rc);
  return rc;
}

/* Refuse early, and say what is actually wrong. "Not enough memory: 21 KB
 * free, a secure request needs about 34" is something the owner of the device
 * can act on -- turn Bluetooth off, close an app. "Network error" is not. */
static int enough_memory(const char *url) {
  size_t largest, free_now;
  if (!url || strncmp(url, "https:", 6) != 0) return 1;
  free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (free_now >= TLS_HEADROOM && largest >= 17000) return 1;
  /* Both numbers, because either can be the one that failed: 45 KB free in
   * pieces no bigger than 12 is a different problem from 20 KB free. */
  snprintf(s_why, sizeof s_why,
           "not enough memory: %u KB free (largest %u), TLS needs %u",
           (unsigned)(free_now / 1024), (unsigned)(largest / 1024),
           (unsigned)(TLS_HEADROOM / 1024));
  ESP_LOGW(TAG, "%s (largest block %u)", s_why, (unsigned)largest);
  return 0;
}


/* ---- opening, past a certificate check that fails for no reason ----
 *
 * Measured 2026-10-08 on the ADV: after every boot the first two or three
 * HTTPS handshakes failed in ESP-IDF's certificate bundle -- for this
 * server, google.com and github.com alike, whichever came first -- and every
 * one after them passed. The bundle's lookup (esp_crt_bundle.c) matched a
 * root by name and then got it wrong: an RSA-4096 key for an all-ECDSA
 * chain, then "no matching root", then the right one, for the same chain a
 * second apart. Not time (a minute's wait changed nothing), not memory, not
 * the hash (it succeeds), not MPI acceleration (off, the same). Until that
 * is found in ESP-IDF, a handshake that fails on the certificate -- and
 * only that: a network that is down is not asked again -- is tried again,
 * up to three more times. Every boot measured succeeded by the fourth. */
#define CERT_RETRIES 3

static int cert_failure(esp_http_client_handle_t cli) {
  int code = 0, flags = 0;
  esp_http_client_get_and_clear_last_tls_error(cli, &code, &flags);
  if (code < 0) code = -code;                /* esp-tls hands mbedTLS's code back positive */
  /* X509_FATAL_ERROR (what the bundle's failure surfaces as) or CERT_VERIFY_FAILED */
  return code == 0x3000 || code == 0x2700 || flags != 0;
}

static esp_err_t open_tls(esp_http_client_handle_t cli, int len) {
  esp_err_t e = esp_http_client_open(cli, len);
  int tries = 0;
  while (e != ESP_OK && tries < CERT_RETRIES && cert_failure(cli)) {
    tries++;
    ESP_LOGW(TAG, "certificate check failed; trying again (%d)", tries);
    esp_http_client_close(cli);
    e = esp_http_client_open(cli, len);
  }
  return e;
}

/* ---- the steps -------------------------------------------------------------- */

typedef struct {
  esp_http_client_handle_t cli;
} Req;

/* Everything up to a connection ready for the body: the network (brought
 * up rather than refused -- something that wants a URL wants the network,
 * and the radio's cost is still only paid when something asks), the
 * memory a handshake needs, the client, the headers, and the handshake.
 * `body_len` is the Content-Length to send, 0 for none. 0, or the code;
 * on failure there is nothing to close. */
static int req_open(Req *r, esp_http_client_method_t method, const char *url,
                    const char *auth, const char *content_type, int body_len,
                    int timeout_ms, int buffer_size) {
  esp_http_client_config_t cfg;
  esp_err_t e;

  r->cli = NULL;
  if (!url || !*url) return fail(-2, "no URL");
  if (!wifi_is_connected() && wifi_connect_saved(20000) != 0) {
    char what[sizeof s_why];
    snprintf(what, sizeof what, "no network: %s", wifi_status());
    return fail(-1, what);
  }
  if (!enough_memory(url)) return -4;

  memset(&cfg, 0, sizeof cfg);
  cfg.url = url;
  cfg.method = method;
  cfg.timeout_ms = timeout_ms;
  /* The bundle is what makes https work without shipping a certificate per
   * site. About 60 KB of flash, and nothing at runtime until a handshake
   * actually happens. */
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.disable_auto_redirect = false;
  cfg.buffer_size = buffer_size;

  r->cli = esp_http_client_init(&cfg);
  if (!r->cli) return fail(-2, "the client would not start: is the URL right?");
  set_auth(r->cli, auth);
  if (content_type && *content_type)
    esp_http_client_set_header(r->cli, "Content-Type", content_type);

  e = open_tls(r->cli, body_len);
  if (e != ESP_OK) {
    why_failed("could not connect", e);
    esp_http_client_cleanup(r->cli);
    r->cli = NULL;
    return -3;
  }
  return 0;
}

static void req_close(Req *r) {
  if (!r->cli) return;
  esp_http_client_close(r->cli);
  esp_http_client_cleanup(r->cli);
  r->cli = NULL;
}

static int send_buf(Req *r, const char *body, int len) {
  if (len > 0 && esp_http_client_write(r->cli, body, len) != len) {
    why_failed("the request was cut off sending", ESP_OK);
    return -3;
  }
  return 0;
}

/* `size` bytes of `fd`, a chunk at a time: the body never exists in memory
 * (a fifteen-second recording is 480 KB, and there are about 120 to play
 * with). */
static int send_from_fd(Req *r, int fd, int size, uint8_t *chunk,
                        void (*progress)(int sent, int total)) {
  int sent = 0;
  while (sent < size) {
    int n = fs_read(fd, chunk, CHUNK);
    if (n <= 0) break;
    if (esp_http_client_write(r->cli, (const char *)chunk, n) != n) break;
    sent += n;
    if (progress) progress(sent, size);
  }
  if (sent != size) {
    why_failed("the request was cut off sending", ESP_OK);
    return -3;
  }
  return 0;
}

/* The status line and headers. Content-Length (or 0 when the server did not
 * say), or -3. */
static int64_t req_headers(Req *r) {
  int64_t len = esp_http_client_fetch_headers(r->cli);
  if (len < 0) { why_failed("no answer (timed out?)", ESP_OK); return -3; }
  return len;
}

/* 0 for a 2xx, else the status negated -- -404 is a 404. A response with
 * no status a number can carry is a transport failure. */
static int req_status(Req *r, const char *url) {
  int status = esp_http_client_get_status_code(r->cli);
  if (status >= 200 && status < 300) return 0;
  ESP_LOGW(TAG, "%s -> %d", url, status);
  if (status > 0 && status < 1000) return -status;
  return fail(-3, "no HTTP status in the answer");
}

/* Into `out`, up to out_size - 1 and a NUL, whatever the status: an error
 * from an API is a document explaining itself, and throwing it away is what
 * makes a failure a mystery. Reading stops at the buffer rather than
 * draining -- whatever is past the end was not going to be looked at. Bytes
 * read; *cut is set when the connection dropped with room still left. */
static int read_to_buf(Req *r, char *out, size_t out_size, int *cut) {
  int got = 0;
  *cut = 0;
  while ((size_t)got < out_size - 1) {
    int n = esp_http_client_read(r->cli, out + got, (int)(out_size - 1 - (size_t)got));
    if (n < 0) { *cut = 1; break; }
    if (n == 0) break;
    got += n;
  }
  out[got] = 0;
  /* A connection that dropped mid-body used to come back as a shorter
   * document and a success. Only when the buffer had room for more: a body
   * the buffer cut short is the caller's choice, and was never complete. */
  if ((size_t)got < out_size - 1 && !esp_http_client_is_complete_data_received(r->cli))
    *cut = 1;
  return got;
}

/* As much as `cap` holds, or what is left: 0 at the end, <0 on an error
 * with nothing read. */
static int read_full(esp_http_client_handle_t cli, uint8_t *b, int cap) {
  int got = 0;
  while (got < cap) {
    int n = esp_http_client_read(cli, (char *)b + got, cap - got);
    if (n < 0) return got ? got : n;
    if (n == 0) break;
    got += n;
  }
  return got;
}

/* The whole reply into `fd` through `buf`. Bytes written, or -3 for a
 * socket or card that failed, or a reply cut short -- `expected` (0 when
 * the server did not say) is checked too. Written in full or not at all:
 * fs_write returns -1 on a short write and leaves the partial bytes behind,
 * which is how a truncated file gets written and believed. */
static int read_to_fd(Req *r, int fd, uint8_t *buf, int cap, int64_t expected,
                      HttpProgress progress, void *ctx) {
  int total = 0;
  for (;;) {
    int n = read_full(r->cli, buf, cap), put = 0;
    if (n < 0) { why_failed("the answer was cut off", ESP_OK); return -3; }
    if (n == 0) break;
    while (put < n) {
      int w = fs_write(fd, buf + put, (size_t)(n - put));
      if (w <= 0) return fail(-3, "the card would not take the answer");
      put += w;
    }
    total += n;
    if (progress) progress(ctx, (uint32_t)total, (uint32_t)(expected > 0 ? expected : 0));
  }
  /* Fewer bytes than promised is a failure, not a smaller file: the
   * installers hash what they get, but a page, a photo or the agent's reply
   * cut short by a dropped connection was being kept and believed. */
  if (!esp_http_client_is_complete_data_received(r->cli) ||
      (expected > 0 && total != expected)) {
    char what[64];
    snprintf(what, sizeof what, "the answer was cut off: %d of %d bytes", total, (int)expected);
    why_failed(what, ESP_OK);
    return -3;
  }
  return total;
}

/* ---- big transfers ----
 *
 * A download used to come off the socket 1 KB at a time and go to the card
 * 1 KB at a time: two sectors a write, each one a single-block SD command,
 * with the radio dozing between beacons. Now the radio stays awake for the
 * transfer (wifi_fast), and the reads fill an 8 KB buffer before anything
 * is written, so the card sees multi-block writes. 8 KB is borrowed for the
 * transfer only; without it, 1 KB still works, and without that, nothing
 * does (NULL: the caller says -4). */
#define BIG_CHUNK 8192

static uint8_t *big_begin(int *cap) {
  uint8_t *b = (uint8_t *)malloc(BIG_CHUNK);
  *cap = BIG_CHUNK;
  if (!b) { b = (uint8_t *)malloc(CHUNK); *cap = CHUNK; }
  if (b) wifi_fast(1);
  return b;
}

static void big_end(uint8_t *b) {
  free(b);
  wifi_fast(0);
}

/* A file to send: open, and its size. 0 or -2. */
static int open_body(const char *path, int *fd, int *size) {
  *fd = fs_open(path, FS_O_READ);
  if (*fd < 0) return fail(-2, "cannot read the file to send");
  *size = fs_seek(*fd, 0, FS_SEEK_END);
  if (*size <= 0 || fs_seek(*fd, 0, FS_SEEK_SET) < 0) {
    fs_close(*fd);
    return fail(-2, "the file to send is empty");
  }
  return 0;
}

/* ---- the transfers ------------------------------------------------------------ */

/* The indicator wraps the two calls that block for seconds. http_stream is
 * deliberately not among them: it runs for as long as a viewer is open and
 * paints the screen itself, so a badge over it would be fighting the very
 * thing it is meant to reassure you about. */
int http_request_quiet(const char *method, const char *url,
                 const char *body, const char *content_type,
                 const char *bearer,
                 char *out, size_t out_size, int timeout_ms) {
  esp_http_client_method_t m;
  Req r;
  int rc, got, cut, blen = body ? (int)strlen(body) : 0;

  s_why[0] = 0;
  if (!out || out_size < 2) return fail(-2, "no buffer for the answer");
  out[0] = 0;

  if (!method || !strcmp(method, "GET"))        m = HTTP_METHOD_GET;
  else if (!strcmp(method, "POST"))             m = HTTP_METHOD_POST;
  else if (!strcmp(method, "PATCH"))            m = HTTP_METHOD_PATCH;
  else if (!strcmp(method, "PUT"))              m = HTTP_METHOD_PUT;
  else if (!strcmp(method, "DELETE"))           m = HTTP_METHOD_DELETE;
  else return fail(-2, "not a method this client speaks");

  if ((rc = req_open(&r, m, url, bearer, content_type, blen, timeout_ms, 1024)) != 0) return rc;
  if ((rc = send_buf(&r, body, blen)) != 0 || (rc = (int)req_headers(&r)) < 0) {
    req_close(&r);
    return rc;
  }
  got = read_to_buf(&r, out, out_size, &cut);
  rc = req_status(&r, url);
  req_close(&r);
  if (rc < 0) return rc;
  if (cut) { why_failed("the answer was cut off", ESP_OK); return -3; }
  return got;
}

int http_post_file(const char *url, const char *path, const char *content_type,
                   char *out, size_t out_size, int timeout_ms) {
  return http_post_file_progress(url, path, content_type, NULL, out, out_size,
                                 timeout_ms, NULL);
}

int http_post_file_progress(const char *url, const char *path,
                            const char *content_type, const char *bearer,
                            char *out, size_t out_size, int timeout_ms,
                            void (*progress)(int sent, int total)) {
  Req r;
  uint8_t *chunk;
  int fd, size, rc, got, cut;

  s_why[0] = 0;
  if (!path || !out || out_size < 2) return fail(-2, "no file, or no buffer for the answer");
  out[0] = 0;
  if ((rc = open_body(path, &fd, &size)) != 0) return rc;
  if ((chunk = (uint8_t *)malloc(CHUNK)) == NULL) {
    fs_close(fd);
    return fail(-4, "no memory for the transfer");
  }
  /* Opened with the length up front, then streamed a kilobyte at a time. */
  rc = req_open(&r, HTTP_METHOD_POST, url, bearer, content_type, size, timeout_ms, 1024);
  if (rc == 0) rc = send_from_fd(&r, fd, size, chunk, progress);
  free(chunk);
  fs_close(fd);
  if (rc == 0) rc = (int)req_headers(&r);
  if (rc < 0) { req_close(&r); return rc; }

  got = read_to_buf(&r, out, out_size, &cut);
  rc = req_status(&r, url);
  req_close(&r);
  if (rc < 0) return rc;
  if (cut) { why_failed("the answer was cut off", ESP_OK); return -3; }
  return got;
}

int http_exchange_files(const char *url, const char *body_path,
                        const char *content_type, const char *auth,
                        const char *reply_path, int timeout_ms) {
  Req r;
  uint8_t *chunk;
  int fd, size, rc, got;

  s_why[0] = 0;
  if (!body_path || !reply_path) return fail(-2, "no file to send, or none for the answer");
  if ((rc = open_body(body_path, &fd, &size)) != 0) return rc;
  if ((chunk = (uint8_t *)malloc(CHUNK)) == NULL) {
    fs_close(fd);
    return fail(-4, "no memory for the transfer");
  }
  rc = req_open(&r, HTTP_METHOD_POST, url, auth, content_type, size, timeout_ms, 1024);
  if (rc == 0) rc = send_from_fd(&r, fd, size, chunk, NULL);
  fs_close(fd);
  if (rc == 0) rc = (int)req_headers(&r);
  if (rc < 0) { free(chunk); req_close(&r); return rc; }

  /* The reply is written whatever the status, as http_request reads it
   * whatever the status: an API's error is a document that explains itself,
   * and the caller can only show it if it was kept. */
  fd = fs_open(reply_path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) {
    free(chunk);
    req_close(&r);
    return fail(-2, "cannot write the answer to the card");
  }
  /* Half a reply is not a reply: a connection dropped mid-body used to
   * leave a file that scanned cleanly as a shorter answer. */
  got = read_to_fd(&r, fd, chunk, CHUNK, 0, NULL, NULL);
  fs_close(fd);
  free(chunk);
  rc = req_status(&r, url);
  req_close(&r);
  if (rc < 0) return rc;
  return got;
}

int http_stream(const char *url, HttpSink on_data, void *ctx, int timeout_ms) {
  return http_stream_ex(url, NULL, on_data, ctx, timeout_ms);
}

int http_stream_ex(const char *url, const char *bearer, HttpSink on_data, void *ctx,
                   int timeout_ms) {
  Req r;
  uint8_t *b;
  int rc, cap, total = 0;

  s_why[0] = 0;
  if (!on_data) return fail(-2, "nowhere for the stream to go");
  if ((rc = req_open(&r, HTTP_METHOD_GET, url, bearer, NULL, 0, timeout_ms, 2048)) != 0) return rc;
  if ((rc = (int)req_headers(&r)) < 0 || (rc = req_status(&r, url)) < 0) {
    req_close(&r);
    return rc;
  }
  if ((b = big_begin(&cap)) == NULL) {
    req_close(&r);
    return fail(-4, "no memory for the transfer");
  }
  /* Until the caller says stop, the server stops, or the socket does: a
   * stream has no end to fall short of, so how it ended is not an error. */
  for (;;) {
    int n = read_full(r.cli, b, cap);
    if (n <= 0) break;
    total += n;
    if (on_data(ctx, b, n)) break;
  }
  big_end(b);
  req_close(&r);
  return total;
}

int http_get(const char *url, char *buf, size_t size, int timeout_ms) {
  return http_request("GET", url, NULL, NULL, NULL, buf, size, timeout_ms);
}

int http_download(const char *url, const char *path, int timeout_ms) {
  return http_download_ex(url, path, NULL, NULL, NULL, timeout_ms);
}

static int http_download_ex_do(const char *url, const char *path, const char *bearer,
                               HttpProgress progress, void *ctx, int timeout_ms) {
  Req r;
  int64_t expected;
  uint8_t *big;
  int fd, rc, cap;

  s_why[0] = 0;
  if (!path) return fail(-2, "nowhere to put the download");
  if ((rc = req_open(&r, HTTP_METHOD_GET, url, bearer, NULL, 0, timeout_ms, 1024)) != 0) return rc;
  /* Not written at all on a failing status: a 404's page is not the file. */
  if ((expected = req_headers(&r)) < 0 || (rc = req_status(&r, url)) < 0) {
    req_close(&r);
    return expected < 0 ? (int)expected : rc;
  }
  fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) { req_close(&r); return fail(-2, "cannot write the download to the card"); }
  if ((big = big_begin(&cap)) == NULL) {
    fs_close(fd);
    req_close(&r);
    return fail(-4, "no memory for the transfer");
  }
  rc = read_to_fd(&r, fd, big, cap, expected, progress, ctx);
  fs_close(fd);
  big_end(big);
  if (rc < 0) ESP_LOGW(TAG, "%s: %s", url, s_why);
  req_close(&r);
  return rc;
}

/* ---- the busy indicator ----------------------------------------------------
 *
 * Wrappers rather than a begin/end pair threaded through the bodies above:
 * both functions have a dozen error returns, and one that forgot to clear the
 * indicator would leave it on the screen for good. A wrapper cannot forget. */

int http_request(const char *method, const char *url,
                 const char *body, const char *content_type,
                 const char *bearer,
                 char *out, size_t out_size, int timeout_ms) {
  int n;
  /* Not "signing in" when there is a bearer: a bearer is a token already
   * held, and Build's poll carries one every 1.2 s, so the badge said
   * "signing in" for as long as an answer took. The one real sign-in,
   * gauth's refresh, sends none and labels itself. */
  busy_begin("fetching");
  n = http_request_quiet(method, url, body, content_type, bearer,
                         out, out_size, timeout_ms);
  busy_end();
  return n;
}

int http_download_ex(const char *url, const char *path, const char *bearer,
                     HttpProgress progress, void *ctx, int timeout_ms) {
  int n;
  busy_begin("downloading");
  n = http_download_ex_do(url, path, bearer, progress, ctx, timeout_ms);
  busy_end();
  return n;
}
