/* One-shot HTTP GET. See http.h. */

#include "kernel/net/http.h"
#include "kernel/sys/busy.h"
#include "kernel/net/wifi.h"
#include "kernel/fs/fs.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

static const char *TAG = "http";

/* One kilobyte of scratch, shared by the upload and the download.
 *
 * Static rather than on the stack because task stacks here are 1 KB, and one
 * copy rather than two because both users are blocking calls on the same task
 * -- a download cannot be halfway through while an upload runs. Two of these
 * was two kilobytes of permanently spent RAM on a machine with a hundred and
 * twenty free. */
static uint8_t s_chunk[1024];

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

/* Refuse early, and say what is actually wrong. "Not enough memory: 21 KB
 * free, a secure request needs about 34" is something the owner of the device
 * can act on -- turn Bluetooth off, close an app. "Network error" is not. */
static int enough_memory(const char *url) {
  size_t largest, free_now;
  if (!url || strncmp(url, "https:", 6) != 0) return 1;
  free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (free_now >= TLS_HEADROOM && largest >= 17000) return 1;
  snprintf(s_why, sizeof s_why,
           "not enough memory: %u KB free, TLS needs about %u",
           (unsigned)(free_now / 1024), (unsigned)(TLS_HEADROOM / 1024));
  ESP_LOGW(TAG, "%s (largest block %u)", s_why, (unsigned)largest);
  return 0;
}

/* The indicator wraps the two calls that block for seconds. http_stream is
 * deliberately not among them: it runs for as long as a viewer is open and
 * paints the screen itself, so a badge over it would be fighting the very
 * thing it is meant to reassure you about. */
int http_request_quiet(const char *method, const char *url,
                 const char *body, const char *content_type,
                 const char *bearer,
                 char *out, size_t out_size, int timeout_ms) {
  esp_http_client_config_t cfg;
  esp_http_client_handle_t cli;
  int status, got = 0;

  int truncated = 0;

  if (!url || !out || out_size < 2) return -2;
  out[0] = 0;

  /* Bring the network up rather than refusing. Something that wants a URL
   * wants the network, and making it say so separately only moves the same
   * call into every caller. The radio's cost is still only paid when something
   * actually asks. */
  if (!wifi_is_connected() && wifi_connect_saved(20000) != 0) return -1;
  if (!enough_memory(url)) return -4;

  memset(&cfg, 0, sizeof cfg);
  cfg.url = url;
  cfg.timeout_ms = timeout_ms;
  /* The bundle is what makes https work without shipping a certificate per
   * site. About 60 KB of flash, and nothing at runtime until a handshake
   * actually happens. */
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.disable_auto_redirect = false;
  cfg.buffer_size = 1024;

  if (!method || !strcmp(method, "GET"))        cfg.method = HTTP_METHOD_GET;
  else if (!strcmp(method, "POST"))             cfg.method = HTTP_METHOD_POST;
  else if (!strcmp(method, "PATCH"))            cfg.method = HTTP_METHOD_PATCH;
  else if (!strcmp(method, "PUT"))              cfg.method = HTTP_METHOD_PUT;
  else if (!strcmp(method, "DELETE"))           cfg.method = HTTP_METHOD_DELETE;
  else return -2;

  cli = esp_http_client_init(&cfg);
  if (!cli) return -2;

  set_auth(cli, bearer);
  if (content_type && *content_type)
    esp_http_client_set_header(cli, "Content-Type", content_type);

  {
    int blen = body ? (int)strlen(body) : 0;
    if (esp_http_client_open(cli, blen) != ESP_OK) {
      esp_http_client_cleanup(cli);
      return -3;
    }
    if (blen > 0 && esp_http_client_write(cli, body, blen) != blen) {
      esp_http_client_close(cli);
      esp_http_client_cleanup(cli);
      return -3;
    }
  }

  if (esp_http_client_fetch_headers(cli) < 0) {
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return -3;
  }

  /* Read the body whatever the status: an error from an API is a JSON document
   * explaining itself, and throwing it away is what makes a failure a mystery.
   * Reading stops at the buffer rather than draining -- whatever is past the
   * end was not going to be looked at. */
  while ((size_t)got < out_size - 1) {
    int n = esp_http_client_read(cli, out + got, (int)(out_size - 1 - (size_t)got));
    if (n < 0) { truncated = 1; break; }
    if (n == 0) break;
    got += n;
  }
  out[got] = 0;
  /* A connection that dropped mid-body used to come back as a shorter
   * document and a success. Only when the buffer had room for more: a body
   * the buffer cut short is the caller's choice, and was never complete. */
  if ((size_t)got < out_size - 1 && !esp_http_client_is_complete_data_received(cli))
    truncated = 1;

  status = esp_http_client_get_status_code(cli);
  esp_http_client_close(cli);
  esp_http_client_cleanup(cli);

  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "%s %s -> %d", method ? method : "GET", url, status);
    return (status > 0 && status < 1000) ? -status : -4;
  }
  if (truncated) return -3;
  return got;
}

int http_post_file(const char *url, const char *path, const char *content_type,
                   char *out, size_t out_size, int timeout_ms) {
  return http_post_file_progress(url, path, content_type, out, out_size,
                                 timeout_ms, NULL);
}

int http_post_file_progress(const char *url, const char *path,
                            const char *content_type,
                            char *out, size_t out_size, int timeout_ms,
                            void (*progress)(int sent, int total)) {
  esp_http_client_config_t cfg;
  esp_http_client_handle_t cli;
  int fd, size, sent = 0, got = 0, status, truncated = 0;

  if (!url || !path || !out || out_size < 2) return -2;
  out[0] = 0;

  if (!wifi_is_connected() && wifi_connect_saved(20000) != 0) return -1;
  if (!enough_memory(url)) return -4;

  fd = fs_open(path, FS_O_READ);
  if (fd < 0) return -2;
  size = fs_seek(fd, 0, FS_SEEK_END);
  if (size <= 0 || fs_seek(fd, 0, FS_SEEK_SET) < 0) { fs_close(fd); return -2; }

  memset(&cfg, 0, sizeof cfg);
  cfg.url = url;
  cfg.timeout_ms = timeout_ms;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.method = HTTP_METHOD_POST;
  cfg.buffer_size = 1024;

  cli = esp_http_client_init(&cfg);
  if (!cli) { fs_close(fd); return -2; }
  if (content_type && *content_type)
    esp_http_client_set_header(cli, "Content-Type", content_type);

  /* Opened with the length up front, then streamed a kilobyte at a time. The
   * whole point of this function is that the body never exists in memory: a
   * fifteen-second recording is 480 KB and there are about 120 KB to play
   * with. */
  if (esp_http_client_open(cli, size) != ESP_OK) {
    fs_close(fd);
    esp_http_client_cleanup(cli);
    return -3;
  }
  while (sent < size) {
    int n = fs_read(fd, s_chunk, sizeof s_chunk);
    if (n <= 0) break;
    if (esp_http_client_write(cli, (const char *)s_chunk, n) != n) break;
    sent += n;
    if (progress) progress(sent, size);
  }
  fs_close(fd);
  if (sent != size) {
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return -3;
  }

  if (esp_http_client_fetch_headers(cli) < 0) {
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return -3;
  }
  while ((size_t)got < out_size - 1) {
    int n = esp_http_client_read(cli, out + got, (int)(out_size - 1 - (size_t)got));
    if (n < 0) { truncated = 1; break; }
    if (n == 0) break;
    got += n;
  }
  out[got] = 0;
  /* A connection that dropped mid-body used to come back as a shorter
   * document and a success. Only when the buffer had room for more: a body
   * the buffer cut short is the caller's choice, and was never complete. */
  if ((size_t)got < out_size - 1 && !esp_http_client_is_complete_data_received(cli))
    truncated = 1;

  status = esp_http_client_get_status_code(cli);
  esp_http_client_close(cli);
  esp_http_client_cleanup(cli);
  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "POST %s -> %d", url, status);
    return (status > 0 && status < 1000) ? -status : -4;
  }
  if (truncated) return -3;
  return got;
}

int http_exchange_files(const char *url, const char *body_path,
                        const char *content_type, const char *auth,
                        const char *reply_path, int timeout_ms) {
  esp_http_client_config_t cfg;
  esp_http_client_handle_t cli;
  int fd, size, sent = 0, got = 0, status, rc;

  if (!url || !body_path || !reply_path) return -2;
  if (!wifi_is_connected() && wifi_connect_saved(20000) != 0) return -1;
  if (!enough_memory(url)) return -4;

  fd = fs_open(body_path, FS_O_READ);
  if (fd < 0) return -2;
  size = fs_seek(fd, 0, FS_SEEK_END);
  if (size <= 0 || fs_seek(fd, 0, FS_SEEK_SET) < 0) { fs_close(fd); return -2; }

  memset(&cfg, 0, sizeof cfg);
  cfg.url = url;
  cfg.timeout_ms = timeout_ms;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.method = HTTP_METHOD_POST;
  cfg.buffer_size = 1024;

  cli = esp_http_client_init(&cfg);
  if (!cli) { fs_close(fd); return -2; }
  set_auth(cli, auth);
  if (content_type && *content_type)
    esp_http_client_set_header(cli, "Content-Type", content_type);

  if (esp_http_client_open(cli, size) != ESP_OK) {
    fs_close(fd);
    esp_http_client_cleanup(cli);
    return -3;
  }
  while (sent < size) {
    int n = fs_read(fd, s_chunk, sizeof s_chunk);
    if (n <= 0) break;
    if (esp_http_client_write(cli, (const char *)s_chunk, n) != n) break;
    sent += n;
  }
  fs_close(fd);
  if (sent != size || esp_http_client_fetch_headers(cli) < 0) {
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return -3;
  }

  /* The reply is written whatever the status, as http_request reads it
   * whatever the status: an API's error is a document that explains itself,
   * and the caller can only show it if it was kept. */
  fd = fs_open(reply_path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) {
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return -2;
  }
  for (;;) {
    int n = esp_http_client_read(cli, (char *)s_chunk, sizeof s_chunk);
    int put = 0;
    if (n < 0) { got = -3; break; }
    if (n == 0) break;
    while (put < n) {
      int w = fs_write(fd, s_chunk + put, (size_t)(n - put));
      if (w <= 0) { n = -1; break; }
      put += w;
    }
    if (n < 0) { got = -3; break; }
    got += n;
  }
  fs_close(fd);
  /* Half a reply is not a reply: a connection dropped mid-body used to leave
   * a file that scanned cleanly as a shorter answer, and was believed. */
  if (got >= 0 && !esp_http_client_is_complete_data_received(cli)) got = -3;

  status = esp_http_client_get_status_code(cli);
  esp_http_client_close(cli);
  esp_http_client_cleanup(cli);
  if (got < 0) return -3;
  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "POST %s -> %d", url, status);
    rc = (status > 0 && status < 1000) ? -status : -4;
    return rc;
  }
  return got;
}

int http_stream(const char *url, HttpSink on_data, void *ctx, int timeout_ms) {
  esp_http_client_config_t cfg;
  esp_http_client_handle_t cli;
  int total = 0, status;

  if (!url || !on_data) return -2;
  if (!wifi_is_connected() && wifi_connect_saved(20000) != 0) return -1;

  memset(&cfg, 0, sizeof cfg);
  cfg.url = url;
  cfg.timeout_ms = timeout_ms;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;

  cli = esp_http_client_init(&cfg);
  if (!cli) return -2;
  if (esp_http_client_open(cli, 0) != ESP_OK) {
    esp_http_client_cleanup(cli);
    return -3;
  }
  if (esp_http_client_fetch_headers(cli) < 0) {
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return -3;
  }
  status = esp_http_client_get_status_code(cli);
  if (status < 200 || status >= 300) {
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return -4;
  }

  /* Until the caller says stop, the server stops, or the socket does. The
   * shared scratch buffer is fine here for the same reason it is fine
   * elsewhere: these are all blocking calls on one task. */
  for (;;) {
    int n = esp_http_client_read(cli, (char *)s_chunk, sizeof s_chunk);
    if (n <= 0) break;
    total += n;
    if (on_data(ctx, s_chunk, n)) break;
  }

  esp_http_client_close(cli);
  esp_http_client_cleanup(cli);
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
  esp_http_client_config_t cfg;
  esp_http_client_handle_t cli;
  int status, fd, total = 0, rc = -3;
  int64_t expected;

  if (!url || !path) return -2;
  if (!wifi_is_connected() && wifi_connect_saved(20000) != 0) return -1;
  if (!enough_memory(url)) return -4;

  memset(&cfg, 0, sizeof cfg);
  cfg.url = url;
  cfg.timeout_ms = timeout_ms;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 1024;

  cli = esp_http_client_init(&cfg);
  if (!cli) return -2;

  set_auth(cli, bearer);

  if (esp_http_client_open(cli, 0) != ESP_OK) {
    esp_http_client_cleanup(cli);
    return -3;
  }
  expected = esp_http_client_fetch_headers(cli);
  if (expected < 0) goto done;

  status = esp_http_client_get_status_code(cli);
  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "%s -> %d", url, status);
    rc = (status > 0 && status < 1000) ? -status : -4;
    goto done;
  }

  fd = fs_open(path, FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
  if (fd < 0) { rc = -2; goto done; }

  for (;;) {
    int n = esp_http_client_read(cli, (char *)s_chunk, sizeof s_chunk);
    int put = 0;
    if (n < 0) { fs_close(fd); rc = -3; goto done; }
    if (n == 0) break;
    /* Written in full or not at all: fs_write returns -1 on a short write and
     * leaves the partial bytes behind, which is how a truncated file gets
     * written and believed. */
    while (put < n) {
      int w = fs_write(fd, s_chunk + put, (size_t)(n - put));
      if (w <= 0) { fs_close(fd); rc = -3; goto done; }
      put += w;
    }
    total += n;
    if (progress) progress(ctx, (uint32_t)total, (uint32_t)(expected > 0 ? expected : 0));
  }
  fs_close(fd);
  /* Fewer bytes than promised is a failure here, not a smaller file: the
   * firmware and app installers hash what they get, but a page or a photo
   * cut short by a dropped connection was being kept and shown. */
  if (!esp_http_client_is_complete_data_received(cli) ||
      (expected > 0 && total != expected)) {
    ESP_LOGW(TAG, "%s: got %d of %d bytes", url, total, (int)expected);
    rc = -3;
    goto done;
  }
  rc = total;

done:
  esp_http_client_close(cli);
  esp_http_client_cleanup(cli);
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
  busy_begin(bearer ? "signing in" : "fetching");
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
