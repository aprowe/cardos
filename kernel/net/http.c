/* One-shot HTTP GET. See http.h. */

#include "kernel/net/http.h"
#include "kernel/net/wifi.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "http";

int http_get(const char *url, char *buf, size_t size, int timeout_ms) {
  esp_http_client_config_t cfg;
  esp_http_client_handle_t cli;
  int status, got = 0;

  if (!url || !buf || size < 2) return -2;
  buf[0] = 0;
  if (!wifi_is_connected()) return -1;

  memset(&cfg, 0, sizeof cfg);
  cfg.url = url;
  cfg.timeout_ms = timeout_ms;
  /* The bundle is what makes https work without shipping a certificate per
   * site. It costs about 60 KB of flash and nothing at runtime until a TLS
   * handshake actually happens. */
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.disable_auto_redirect = false;
  cfg.buffer_size = 1024;

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
    ESP_LOGW(TAG, "%s -> %d", url, status);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return (status > 0 && status < 1000) ? -status : -4;
  }

  /* Read until the buffer is full and then stop, rather than draining the
   * body. Whatever is past the end of a buffer this size was not going to be
   * looked at. */
  while ((size_t)got < size - 1) {
    int n = esp_http_client_read(cli, buf + got, (int)(size - 1 - (size_t)got));
    if (n <= 0) break;
    got += n;
  }
  buf[got] = 0;

  esp_http_client_close(cli);
  esp_http_client_cleanup(cli);
  return got;
}
