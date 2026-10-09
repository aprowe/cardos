/* WiFi station. See wifi.h. */

#include "kernel/net/wifi.h"
#include "kernel/sys/conf.h"
#include "kernel/net/wifilist.h"
#include "kernel/app/capp.h"   /* CAPP_CONFIG */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "wifi";

#define NVS_NS   "cardos"
#define NVS_SSID "wifi_ssid"
#define NVS_PASS "wifi_pass"

#define BIT_GOT_IP   BIT0
#define BIT_FAILED   BIT1

static WifiState s_state;
static int       s_started;   /* the radio is running */
static int       s_inited;    /* the driver, netif and handlers exist */
static char      s_detail[64];
static char      s_ip[16] = "0.0.0.0";
static char      s_ssid[WIFI_SSID_MAX];
static uint32_t  s_heap_cost;
static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_on_wifi, s_on_ip;

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
/* What the radio itself needs, plus a little. Measured: the driver costs 49792
 * bytes. The old figure here was 85 KB -- the radio's cost plus room for a TLS
 * handshake afterwards -- which meant a device with 79 KB free refused to
 * bring WiFi up at all rather than bringing it up and letting the caller find
 * out whether there was room to talk securely. Two questions, two answers:
 * http.c checks the TLS headroom separately, and says so in those words.
 *
 * But "a little" has to be enough for everything else running. 56 KB against
 * a 52 KB driver let WiFi build itself again in the middle of a print --
 * Bluetooth up, the print fonts loaded -- and the heap's low water went to
 * 1964 bytes (2026-10-02). 72 KB leaves 20 for the rest of the machine;
 * below it the radio waits for the print to end. */
#define WIFI_MIN_HEAP (72 * 1024)

int wifi_start(void) {
  size_t heap_before;

  if (s_started) return 0;
  heap_before = esp_get_free_heap_size();

  /* Stopped and starting again: the driver is still there, only the radio
   * went. Building it twice registered the handlers twice; and until stop
   * cleared s_started, this returned early with the radio off and every
   * connect waited twenty seconds for a driver that was not listening. */
  if (s_inited) {
    if (esp_wifi_start() != ESP_OK) {
      snprintf(s_detail, sizeof s_detail, "radio would not restart");
      s_state = WIFI_FAILED;
      return -1;
    }
    s_started = 1;
    s_state = WIFI_OFF;
    return 0;
  }

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

  /* Each of these survives wifi_release, so a second build must not make a
   * second one. */
  if (!s_events) s_events = xEventGroupCreate();
  if (!s_events) return -1;

  if (esp_netif_init() != ESP_OK) return -1;
  /* Already created if something else brought the loop up first, which is not
   * an error. */
  esp_event_loop_create_default();
  s_netif = esp_netif_create_default_wifi_sta();

  {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
      snprintf(s_detail, sizeof s_detail, "radio init failed");
      s_state = WIFI_FAILED;
      return -1;
    }
  }

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, &s_on_wifi);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, &s_on_ip);

  /* FLASH, not RAM. The driver then keeps its own copy of the last successful
   * join, which is what connect_driver_config falls back to -- and on a board
   * that has run other firmware, that copy may be the only credentials there
   * are. With RAM storage the fallback always found an empty config, so a
   * device that had never been joined through CardOS could not get online at
   * all and said only "no network". */
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  esp_wifi_set_mode(WIFI_MODE_STA);
  if (esp_wifi_start() != ESP_OK) {
    snprintf(s_detail, sizeof s_detail, "radio would not start");
    s_state = WIFI_FAILED;
    return -1;
  }

  s_started = 1;
  s_inited = 1;
  s_heap_cost = (uint32_t)(heap_before - esp_get_free_heap_size());
  ESP_LOGI(TAG, "radio up, %u bytes of heap", (unsigned)s_heap_cost);
  return 0;
}

/* ---- who is using it, and one join at a time ----
 *
 * wifi_release used to be guarded only by "is httpq running", which a
 * print's task checked -- and then tore the driver down under the shell's
 * blocking requests, a Music stream, a voice upload or the time sync, with
 * their sockets open. Now everything that needs the radio up says so:
 * http.c around every transfer, link.c while ESP-NOW is open. The count is
 * under a spinlock and never blocks; s_releasing makes a use that arrives
 * mid-teardown wait the few milliseconds until it is over, and then find
 * the radio down and build it again, rather than talk through a netif being
 * destroyed.
 *
 * Joins are serialised by s_join (recursive: connect_saved calls connect).
 * Two tasks used to run wifi_start and wifi_scan at once and clear each
 * other's event bits -- the boot time sync and the first app sync, for one.
 * A second caller now waits for the first join and then finds it done. */
static portMUX_TYPE      s_use_mux = portMUX_INITIALIZER_UNLOCKED;
static int               s_users;
static volatile int      s_releasing;
static SemaphoreHandle_t s_join;
static StaticSemaphore_t s_join_buf;

static SemaphoreHandle_t join_lock(void) {
  portENTER_CRITICAL(&s_use_mux);
  if (!s_join) s_join = xSemaphoreCreateRecursiveMutexStatic(&s_join_buf);
  portEXIT_CRITICAL(&s_use_mux);
  return s_join;
}

static int join_take(int wait_ms) {
  return xSemaphoreTakeRecursive(join_lock(), wait_ms < 0 ? portMAX_DELAY
                                 : pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}
static void join_give(void) { xSemaphoreGiveRecursive(join_lock()); }

void wifi_use(void) {
  for (;;) {
    int ok = 0;
    portENTER_CRITICAL(&s_use_mux);
    if (!s_releasing) { s_users++; ok = 1; }
    portEXIT_CRITICAL(&s_use_mux);
    if (ok) return;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void wifi_unuse(void) {
  portENTER_CRITICAL(&s_use_mux);
  if (s_users > 0) s_users--;
  portEXIT_CRITICAL(&s_use_mux);
}

int wifi_in_use(void) { return s_users > 0; }

/* The whole driver, not just the radio: wifi_stop gives back 3 KB of the
 * 50 the driver holds (measured 2026-10-02), and that was the difference
 * between Bluetooth starting and not with Today open. Everything wifi_start
 * built goes, so the next start builds it again from nothing. */
int wifi_release(void) {
  int busy;
  if (!s_inited) return 0;
  /* Not mid-join: the joiner would find the driver gone under it. */
  if (!join_take(0)) return -1;
  portENTER_CRITICAL(&s_use_mux);
  busy = s_users > 0;
  if (!busy) s_releasing = 1;
  portEXIT_CRITICAL(&s_use_mux);
  /* Not under a transfer, and not under ESP-NOW (kernel/net/link.c):
   * deinit with it running fails, and a game would lose its partner to
   * make room for a print. */
  if (busy) {
    join_give();
    ESP_LOGI(TAG, "not released: %d using it", s_users);
    return -1;
  }
  wifi_stop();
  if (s_on_wifi) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_on_wifi);
  if (s_on_ip) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_on_ip);
  s_on_wifi = s_on_ip = NULL;
  esp_wifi_deinit();
  if (s_netif) esp_netif_destroy_default_wifi(s_netif);
  s_netif = NULL;
  s_inited = 0;
  snprintf(s_detail, sizeof s_detail, "off, to make room");
  s_releasing = 0;
  join_give();
  ESP_LOGI(TAG, "released, %u bytes of heap free", (unsigned)esp_get_free_heap_size());
  return 0;
}

void wifi_stop(void) {
  if (!s_started) return;
  esp_wifi_disconnect();
  esp_wifi_stop();
  s_started = 0;
  s_state = WIFI_OFF;
  strcpy(s_ip, "0.0.0.0");
  snprintf(s_detail, sizeof s_detail, "off");
}

/* The saved networks (kernel/net/wifilist.h): a list in NVS for the runtime,
 * and the same text -- SSID and password on alternate lines -- in
 * /config/wifi.txt, so a flash that wipes NVS does not cost the networks.
 * The old single-network keys still hold the newest one, for anything that
 * reads them and for a firmware from before the list. */
#define WIFI_CONF CAPP_CONFIG "/wifi.txt"
#define NVS_LIST  "wifi_list"

static WifiList s_list;          /* scratch: one list at a time, not on a stack */

/* The text form's buffer is the heap's for the moment it is needed: the list
 * is read when joining, not held -- 800 bytes a copy was 4 KB of .bss. */
#define LIST_TEXT (WIFILIST_MAX * (WIFILIST_SSID + WIFILIST_PASS + 2) + 1)

static void load_list(WifiList *l) {
  char *text = (char *)malloc(LIST_TEXT);
  nvs_handle_t h;
  size_t n = LIST_TEXT;
  memset(l, 0, sizeof *l);
  if (!text) return;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) { free(text); return; }
  if (nvs_get_str(h, NVS_LIST, text, &n) == ESP_OK) {
    wifilist_parse(l, text);
  } else {
    /* From before the list: the one network, as a list of one. */
    size_t ns = sizeof l->net[0].ssid, np = sizeof l->net[0].pass;
    if (nvs_get_str(h, NVS_SSID, l->net[0].ssid, &ns) == ESP_OK && l->net[0].ssid[0]) {
      if (nvs_get_str(h, NVS_PASS, l->net[0].pass, &np) != ESP_OK) l->net[0].pass[0] = 0;
      l->n = 1;
    }
  }
  nvs_close(h);
  free(text);
}

static void save_list(const WifiList *l) {
  char *text = (char *)malloc(LIST_TEXT);
  const char *lines[2 * WIFILIST_MAX];
  nvs_handle_t h;
  int i;
  if (!text) return;
  if (wifilist_format(l, text, LIST_TEXT) < 0) { free(text); return; }
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, NVS_LIST, text);
    if (l->n) {
      nvs_set_str(h, NVS_SSID, l->net[0].ssid);
      nvs_set_str(h, NVS_PASS, l->net[0].pass);
    } else {
      nvs_erase_key(h, NVS_SSID);
      nvs_erase_key(h, NVS_PASS);
    }
    nvs_commit(h);
    nvs_close(h);
  }
  for (i = 0; i < l->n; i++) { lines[2 * i] = l->net[i].ssid; lines[2 * i + 1] = l->net[i].pass; }
  if (l->n) conf_write(WIFI_CONF, lines, 2 * l->n);
  else conf_remove(WIFI_CONF);
  free(text);
}

/* Joined: to the front of the list, keeping the rest. */
static void save(const char *ssid, const char *pass) {
  load_list(&s_list);
  wifilist_remember(&s_list, ssid, pass);
  save_list(&s_list);
}

int wifi_restore_from_card(void) {
  char (*lines)[WIFI_PASS_MAX];
  int n, i;
  load_list(&s_list);
  if (s_list.n) return 0;                        /* NVS has some: it wins */
  lines = (char (*)[WIFI_PASS_MAX])malloc(2 * WIFILIST_MAX * WIFI_PASS_MAX);
  if (!lines) return 0;
  n = conf_read(WIFI_CONF, &lines[0][0], 2 * WIFILIST_MAX, WIFI_PASS_MAX);
  if (n < 1 || !lines[0][0]) { free(lines); return 0; }
  memset(&s_list, 0, sizeof s_list);
  for (i = 0; i + 1 <= n && s_list.n < WIFILIST_MAX; i += 2) {
    if (!lines[i][0]) continue;
    snprintf(s_list.net[s_list.n].ssid, sizeof s_list.net[0].ssid, "%.32s", lines[i]);
    snprintf(s_list.net[s_list.n].pass, sizeof s_list.net[0].pass, "%s",
             i + 1 < n ? lines[i + 1] : "");
    s_list.n++;
  }
  free(lines);
  if (!s_list.n) return 0;
  save_list(&s_list);
  return s_list.n;
}

int wifi_saved_count(void) { load_list(&s_list); return s_list.n; }

const char *wifi_saved_name(int i) {
  static char name[WIFI_SSID_MAX];
  load_list(&s_list);
  name[0] = 0;
  if (i >= 0 && i < s_list.n) snprintf(name, sizeof name, "%s", s_list.net[i].ssid);
  return name;
}

int wifi_forget_one(const char *ssid) {
  int gone;
  load_list(&s_list);
  gone = wifilist_forget(&s_list, ssid);
  if (gone) save_list(&s_list);
  return gone;
}

static int connect_one(const char *ssid, const char *pass, int timeout_ms);
static int scan(WifiAp *out, int max);

int wifi_connect(const char *ssid, const char *pass, int timeout_ms) {
  int rc;
  if (!join_take(timeout_ms)) return -1;
  rc = connect_one(ssid, pass, timeout_ms);
  join_give();
  return rc;
}

static int connect_one(const char *ssid, const char *pass, int timeout_ms) {
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

/* The saved networks, the one in range first. One saved: straight at it, as
 * before -- a scan costs two seconds for nothing. Several: a scan, then the
 * ones it saw, strongest first, each with a share of the time; if it saw none
 * (a hidden network, or away from all of them), every one in turn, most
 * recent first, until the time is up. */
static int connect_saved(int timeout_ms);

int wifi_connect_saved(int timeout_ms) {
  int rc;
  int64_t t0 = esp_timer_get_time() / 1000;
  if (!join_take(0)) {
    /* Someone else is joining: wait for theirs, which may do the work. */
    if (!join_take(timeout_ms)) return -1;
    if (wifi_is_connected()) { join_give(); return 0; }
  }
  timeout_ms -= (int)(esp_timer_get_time() / 1000 - t0);
  rc = timeout_ms > 0 ? connect_saved(timeout_ms) : -1;
  join_give();
  return rc;
}

static int connect_saved(int timeout_ms) {
  /* On the heap, not the stack: this is called from cardos-bg, whose 4 KB
   * stack a list and a scan (1.4 KB) overflowed -- a reboot loop on the
   * first join after boot (2026-09-29). See CLAUDE.md on cardos-bg. */
  typedef struct {
    WifiList l;
    WifiAp   aps[WIFI_MAX_SCAN];
    const char *seen[WIFI_MAX_SCAN];
    int8_t   rssi[WIFI_MAX_SCAN];
    int      order[WIFILIST_MAX];
  } Work;
  Work *w;
  int nseen, n, i, rc = -1, total;
  int64_t until;

  if (wifi_start() != 0) return -1;
  if ((w = (Work *)calloc(1, sizeof *w)) == NULL) return -1;
  load_list(&w->l);
  total = w->l.n;

  if (total == 0) {
    free(w);
    if (connect_driver_config(timeout_ms) == 0) return 0;
    /* Nothing saved anywhere. Say so as a sentence: "no network" on its own
     * sends the reader looking at the router. */
    snprintf(s_detail, sizeof s_detail, "%s", "no saved network -- join one in Settings");
    s_state = WIFI_FAILED;
    return -1;
  }

  until = esp_timer_get_time() / 1000 + timeout_ms;
  nseen = scan(w->aps, WIFI_MAX_SCAN);
  for (i = 0; i < nseen; i++) { w->seen[i] = w->aps[i].ssid; w->rssi[i] = w->aps[i].rssi; }
  n = wifilist_order(&w->l, w->seen, w->rssi, nseen, w->order);
  /* Only what the scan saw. Asked to, wifilist_order falls back to every
   * saved network, for a hidden one -- and out of the house that was each
   * of them in turn until the twenty seconds ran out, every time an app
   * that wants the network was opened (2026-10-02). Two seconds of scan
   * says nobody is home; a hidden network is joined by name instead
   * (`wifi SSID PASS`). One saved network is scanned for too: it used to be
   * tried blind, for the full timeout. */
  {
    int k, j, in_range = 0;
    for (k = 0; k < total && !in_range; k++)
      for (j = 0; j < nseen; j++)
        if (!strcmp(w->seen[j], w->l.net[k].ssid)) { in_range = 1; break; }
    if (!in_range) {
      free(w);
      snprintf(s_detail, sizeof s_detail, "no saved network in range");
      s_state = WIFI_FAILED;
      return -1;
    }
  }
  for (i = 0; i < n; i++) {
    int64_t left = until - esp_timer_get_time() / 1000;
    int share = (int)(left / (n - i));
    if (left < 2000) break;
    if (share < 8000) share = (int)(left < 8000 ? left : 8000);  /* a join needs a few seconds */
    if (connect_one(w->l.net[w->order[i]].ssid, w->l.net[w->order[i]].pass, share) == 0) {
      rc = 0;
      break;
    }
  }
  free(w);
  if (rc == 0) return 0;
  snprintf(s_detail, sizeof s_detail, "none of %d saved networks answered", total);
  s_state = WIFI_FAILED;
  return -1;
}

/* Under the join lock too: a scan mid-join used to end the join. */
int wifi_scan(WifiAp *out, int max) {
  int n;
  if (!join_take(25000)) return 0;
  n = scan(out, max);
  join_give();
  return n;
}

static int scan(WifiAp *out, int max) {
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

/* The newest saved network, or "". */
const char *wifi_saved_ssid(void) { return wifi_saved_name(0); }

/* All of them, NVS and the card. */
void wifi_forget(void) {
  memset(&s_list, 0, sizeof s_list);
  save_list(&s_list);
  {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
      nvs_erase_key(h, NVS_LIST);
      nvs_commit(h);
      nvs_close(h);
    }
  }
}

/* Power save off while something big comes down. The default modem sleep
 * dozes between beacons, and a download then trickles at the beacon rate;
 * on the rest of the time, since an idle radio awake costs battery. Counted,
 * so overlapping callers do not switch it back under each other. */
static int s_fast;
void wifi_fast(int on) {
  if (on) { if (s_fast++ == 0) esp_wifi_set_ps(WIFI_PS_NONE); }
  else if (s_fast > 0 && --s_fast == 0) esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}
