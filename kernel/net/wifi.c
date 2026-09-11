/* WiFi station. See wifi.h. */

#include "kernel/net/wifi.h"

#include <string.h>
#include <stdio.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wifi";

#define NVS_NS   "cardos"
#define NVS_SSID "wifi_ssid"
#define NVS_PASS "wifi_pass"

#define BIT_GOT_IP   BIT0
#define BIT_FAILED   BIT1

static WifiState s_state;
static int       s_started;
static char      s_detail[64];
static char      s_ip[16] = "0.0.0.0";
static char      s_ssid[WIFI_SSID_MAX];
static uint32_t  s_heap_cost;
static EventGroupHandle_t s_events;

/* Retries are bounded and counted here rather than left to the driver: an
 * unbounded retry loop makes a wrong password indistinguishable from a weak
 * signal, and the user needs to be told which. */
#define MAX_RETRY 4
static int s_retries;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    /* Deliberately does not connect. Bringing the radio up and joining a
     * network are separate decisions -- a scan needs the first without the
     * second, and the driver refuses to scan while a join is in flight. */
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
    if (s_retries < MAX_RETRY) {
      s_retries++;
      esp_wifi_connect();
      return;
    }
    /* Reason 15 is a four-way-handshake timeout, which in practice always
     * means the passphrase is wrong; 201 means the network was not found. */
    snprintf(s_detail, sizeof s_detail,
             d->reason == 15 ? "wrong password" :
             d->reason == 201 ? "network not found" : "disconnected (%d)",
             d->reason);
    s_state = WIFI_FAILED;
    strcpy(s_ip, "0.0.0.0");
    xEventGroupSetBits(s_events, BIT_FAILED);
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
    snprintf(s_detail, sizeof s_detail, "%s", s_ip);
    s_state = WIFI_CONNECTED;
    s_retries = 0;
    xEventGroupSetBits(s_events, BIT_GOT_IP);
  }
}

/* Measured, not guessed: the radio itself takes ~49 KB and the TLS handshake
 * for one HTTPS request peaks another ~30 on top. Starting it with less than
 * this leaves the driver failing buffer allocations in a loop, which looks
 * like a hang rather than like running out of memory. */
#define WIFI_MIN_HEAP (85 * 1024)

int wifi_start(void) {
  size_t heap_before;

  if (s_started) return 0;
  heap_before = esp_get_free_heap_size();

  if (heap_before < WIFI_MIN_HEAP) {
    snprintf(s_detail, sizeof s_detail, "only %u KB free, needs %u",
             (unsigned)(heap_before / 1024), (unsigned)(WIFI_MIN_HEAP / 1024));
    s_state = WIFI_FAILED;
    ESP_LOGE(TAG, "%s", s_detail);
    return -1;
  }

  {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      nvs_flash_init();
    }
  }

  s_events = xEventGroupCreate();
  if (!s_events) return -1;

  if (esp_netif_init() != ESP_OK) return -1;
  /* Already created if something else brought the loop up first, which is not
   * an error. */
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();

  {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
      snprintf(s_detail, sizeof s_detail, "radio init failed");
      s_state = WIFI_FAILED;
      return -1;
    }
  }

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);

  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_STA);
  if (esp_wifi_start() != ESP_OK) {
    snprintf(s_detail, sizeof s_detail, "radio would not start");
    s_state = WIFI_FAILED;
    return -1;
  }

  s_started = 1;
  s_heap_cost = (uint32_t)(heap_before - esp_get_free_heap_size());
  ESP_LOGI(TAG, "radio up, %u bytes of heap", (unsigned)s_heap_cost);
  return 0;
}

void wifi_stop(void) {
  if (!s_started) return;
  esp_wifi_disconnect();
  esp_wifi_stop();
  s_state = WIFI_OFF;
  strcpy(s_ip, "0.0.0.0");
  snprintf(s_detail, sizeof s_detail, "off");
}

static void save(const char *ssid, const char *pass) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, NVS_SSID, ssid);
  nvs_set_str(h, NVS_PASS, pass);
  nvs_commit(h);
  nvs_close(h);
}

int wifi_connect(const char *ssid, const char *pass, int timeout_ms) {
  wifi_config_t cfg;
  EventBits_t bits;

  if (!ssid || !ssid[0]) return -1;
  if (wifi_start() != 0) return -1;

  memset(&cfg, 0, sizeof cfg);
  snprintf((char *)cfg.sta.ssid, sizeof cfg.sta.ssid, "%s", ssid);
  snprintf((char *)cfg.sta.password, sizeof cfg.sta.password, "%s", pass ? pass : "");
  /* WPA2 minimum, but only when a passphrase was given: insisting on it for an
   * open network makes the join fail for no reason. */
  cfg.sta.threshold.authmode =
      (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
  snprintf(s_detail, sizeof s_detail, "joining %s", ssid);
  s_state = WIFI_CONNECTING;
  s_retries = 0;

  xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  esp_wifi_disconnect();
  esp_wifi_connect();

  bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                             pdMS_TO_TICKS(timeout_ms));
  if (bits & BIT_GOT_IP) {
    save(ssid, pass ? pass : "");
    return 0;
  }
  if (!(bits & BIT_FAILED)) {
    s_state = WIFI_FAILED;
    snprintf(s_detail, sizeof s_detail, "timed out");
  }
  return -1;
}

/* Whatever the driver is already carrying. The WiFi driver keeps its own copy
 * of the last successful join in NVS, and on a board that has run other
 * firmware that copy may be the only credentials there are. Using it means a
 * device provisioned elsewhere still gets online without being asked to type a
 * password it has already been told. */
static int connect_driver_config(int timeout_ms) {
  wifi_config_t cfg;
  EventBits_t bits;

  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return -1;
  if (!cfg.sta.ssid[0]) return -1;

  snprintf(s_ssid, sizeof s_ssid, "%s", (const char *)cfg.sta.ssid);
  snprintf(s_detail, sizeof s_detail, "joining %s", s_ssid);
  s_state = WIFI_CONNECTING;
  s_retries = 0;

  xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);
  esp_wifi_connect();
  bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                             pdMS_TO_TICKS(timeout_ms));
  return (bits & BIT_GOT_IP) ? 0 : -1;
}

int wifi_connect_saved(int timeout_ms) {
  nvs_handle_t h;
  char ssid[WIFI_SSID_MAX] = "", pass[WIFI_PASS_MAX] = "";
  size_t ns = sizeof ssid, np = sizeof pass;

  if (wifi_start() != 0) return -1;

  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    if (nvs_get_str(h, NVS_SSID, ssid, &ns) != ESP_OK) ssid[0] = 0;
    nvs_get_str(h, NVS_PASS, pass, &np);
    nvs_close(h);
  }

  if (ssid[0]) return wifi_connect(ssid, pass, timeout_ms);
  return connect_driver_config(timeout_ms);
}

int wifi_scan(WifiAp *out, int max) {
  wifi_scan_config_t cfg;
  uint16_t found = 0;
  wifi_ap_record_t *recs;
  int n = 0, i;

  if (!out || max <= 0) return 0;
  if (wifi_start() != 0) return 0;

  /* "STA is connecting, scan are not allowed" -- the driver refuses a scan
   * mid-join, and a join that is going to fail takes its full retry budget to
   * do so. Waiting a moment for it to settle turns the common case into a
   * short pause; a join still in flight after that is dropped, because the
   * user asked for a list and asking again would not help. */
  if (s_state == WIFI_CONNECTING) {
    int waited = 0;
    while (s_state == WIFI_CONNECTING && waited < 4000) {
      vTaskDelay(pdMS_TO_TICKS(100));
      waited += 100;
    }
    if (s_state == WIFI_CONNECTING) {
      esp_wifi_disconnect();
      vTaskDelay(pdMS_TO_TICKS(200));
    }
  }

  memset(&cfg, 0, sizeof cfg);
  cfg.show_hidden = false;
  if (esp_wifi_scan_start(&cfg, true) != ESP_OK) return 0;

  esp_wifi_scan_get_ap_num(&found);
  if (found == 0) return 0;
  if (found > WIFI_MAX_SCAN * 3) found = WIFI_MAX_SCAN * 3;

  recs = calloc(found, sizeof *recs);
  if (!recs) { esp_wifi_clear_ap_list(); return 0; }
  esp_wifi_scan_get_ap_records(&found, recs);

  /* The driver hands these back strongest first, and duplicates are common --
   * one SSID on several channels or several access points. Showing the same
   * name five times would push the rest off a 135-pixel screen. */
  for (i = 0; i < (int)found && n < max; i++) {
    int j, dup = 0;
    if (!recs[i].ssid[0]) continue;
    for (j = 0; j < n; j++)
      if (strcmp(out[j].ssid, (const char *)recs[i].ssid) == 0) { dup = 1; break; }
    if (dup) continue;
    snprintf(out[n].ssid, sizeof out[n].ssid, "%s", (const char *)recs[i].ssid);
    out[n].rssi = recs[i].rssi;
    out[n].open = (recs[i].authmode == WIFI_AUTH_OPEN);
    n++;
  }
  free(recs);
  esp_wifi_clear_ap_list();
  return n;
}

WifiState wifi_state(void) { return s_state; }
int wifi_is_connected(void) { return s_state == WIFI_CONNECTED; }
const char *wifi_ip(void) { return s_ip; }
uint32_t wifi_heap_cost(void) { return s_heap_cost; }

const char *wifi_status(void) {
  static char buf[80];
  const char *name =
      s_state == WIFI_OFF        ? "off" :
      s_state == WIFI_CONNECTING ? "joining" :
      s_state == WIFI_CONNECTED  ? "connected" : "failed";
  if (s_state == WIFI_CONNECTED)
    snprintf(buf, sizeof buf, "%s %s", s_ssid, s_ip);
  else
    snprintf(buf, sizeof buf, "%s (%s)", name, s_detail[0] ? s_detail : "-");
  return buf;
}

const char *wifi_saved_ssid(void) {
  static char ssid[WIFI_SSID_MAX];
  nvs_handle_t h;
  size_t n = sizeof ssid;
  ssid[0] = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return ssid;
  if (nvs_get_str(h, NVS_SSID, ssid, &n) != ESP_OK) ssid[0] = 0;
  nvs_close(h);
  return ssid;
}

void wifi_forget(void) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, NVS_SSID);
  nvs_erase_key(h, NVS_PASS);
  nvs_commit(h);
  nvs_close(h);
}
