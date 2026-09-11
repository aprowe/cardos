/* One-shot HTTP GET. See http.h. */

#include "kernel/net/http.h"
#include "kernel/net/wifi.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "http";

int http_request(const char *method, const char *url,
                 const char *body, const char *content_type,
                 const char *bearer,
                 char *out, size_t out_size, int timeout_ms) {
  esp_http_client_config_t cfg;
  esp_http_client_handle_t cli;
  int status, got = 0;

  if (!url || !out || out_size < 2) return -2;
  out[0] = 0;

  /* Bring the network up rather than refusing. Something that wants a URL
   * wants the network, and making it say so separately only moves the same
   * call into every caller. The radio's cost is still only paid when something
   * actually asks. */
  if (!wifi_is_connected() && wifi_connect_saved(20000) != 0) return -1;

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

  if (bearer && *bearer) {
    char hdr[600];
    snprintf(hdr, sizeof hdr, "Bearer %s", bearer);
    esp_http_client_set_header(cli, "Authorization", hdr);
  }
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
    if (n <= 0) break;
    got += n;
  }
  out[got] = 0;

  status = esp_http_client_get_status_code(cli);
  esp_http_client_close(cli);
  esp_http_client_cleanup(cli);

  if (status < 200 || status >= 300) {
    ESP_LOGW(TAG, "%s %s -> %d", method ? method : "GET", url, status);
    return (status > 0 && status < 1000) ? -status : -4;
  }
  return got;
}

int http_get(const char *url, char *buf, size_t size, int timeout_ms) {
  return http_request("GET", url, NULL, NULL, NULL, buf, size, timeout_ms);
}
