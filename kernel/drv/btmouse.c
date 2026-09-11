/* Bluetooth LE mouse, HID host role. See btmouse.h. */

#include "kernel/drv/btmouse.h"
#include "kernel/drv/vendor/esp_hid_gap.h"

#include <string.h>

#include "esp_err.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

/* Provided by NimBLE's store_config, which has no public header. */
void ble_store_config_init(void);

/* NimBLE runs its host in its own task; nothing works until it does. */
static void nimble_host_task(void *param) {
  (void)param;
  nimble_port_run();                 /* returns only on nimble_port_stop */
  nimble_port_freertos_deinit();
}

static const char *TAG = "btmouse";

static BtMouseState s_state;
static char         s_detail[48];
static uint32_t     s_heap_cost;
static int          s_inited;
static uint32_t     s_reports;

/* The bonded peer, remembered across reboots. Once a mouse has bonded it
 * stops advertising to be discovered -- it expects the host to come back to
 * it -- so scanning only ever works for the very first pairing. Every
 * reconnection afterwards has to be a direct open. */
#define NVS_NS   "cardos"
#define NVS_PEER "mousepeer"
typedef struct { uint8_t bda[6]; uint8_t addr_type; uint8_t valid; } SavedPeer;
static SavedPeer s_peer;

static void peer_load(void) __attribute__((unused));
static void peer_load(void) {
  nvs_handle_t h;
  size_t len = sizeof s_peer;
  s_peer.valid = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  if (nvs_get_blob(h, NVS_PEER, &s_peer, &len) != ESP_OK || len != sizeof s_peer)
    s_peer.valid = 0;
  nvs_close(h);
}

static void peer_save(const uint8_t *bda, uint8_t addr_type) {
  nvs_handle_t h;
  memcpy(s_peer.bda, bda, 6);
  s_peer.addr_type = addr_type;
  s_peer.valid = 1;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_blob(h, NVS_PEER, &s_peer, sizeof s_peer);
  nvs_commit(h);
  nvs_close(h);
}

/* A tiny ring of decoded reports. The HID callback runs on the Bluetooth
 * task, so it must not touch the display or the window manager -- it drops
 * reports here and the main loop drains them. */
#define RING 16
static MouseReport s_ring[RING];
static volatile uint8_t s_head, s_tail;

static void ring_push(const MouseReport *r) {
  uint8_t next = (uint8_t)((s_head + 1) % RING);
  if (next == s_tail) return;      /* full: drop the oldest movement, not the
                                      newest -- a stale delta is worthless */
  s_ring[s_head] = *r;
  s_head = next;
}

int btmouse_poll(MouseReport *out) {
  if (s_tail == s_head) return 0;
  *out = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) % RING);
  return 1;
}

/* ------------------------------------------------------------- events -- */

static void hidh_event(void *handler_args, esp_event_base_t base,
                       int32_t id, void *event_data) {
  esp_hidh_event_t event = (esp_hidh_event_t)id;
  esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)event_data;
  (void)handler_args;
  (void)base;

  switch (event) {
  case ESP_HIDH_OPEN_EVENT:
    if (p->open.status == ESP_OK) {
      s_state = BTM_CONNECTED;
      snprintf(s_detail, sizeof s_detail, "%s",
               esp_hidh_dev_name_get(p->open.dev) ?
                 esp_hidh_dev_name_get(p->open.dev) : "mouse");
      ESP_LOGI(TAG, "open: %s", s_detail);
    } else {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "open failed");
    }
    break;

  case ESP_HIDH_INPUT_EVENT: {
    MouseReport r;
    s_reports++;
    /* Log the first few raw reports. Whether a mouse speaks boot protocol or
     * report protocol cannot be assumed, and the difference is silent: a
     * report-protocol mouse with 16-bit deltas decodes as nonsense rather
     * than as nothing. */
    if (s_reports <= 5) {
      ESP_LOGI(TAG, "report len=%d: %02x %02x %02x %02x %02x %02x",
               (int)p->input.length,
               p->input.length > 0 ? p->input.data[0] : 0,
               p->input.length > 1 ? p->input.data[1] : 0,
               p->input.length > 2 ? p->input.data[2] : 0,
               p->input.length > 3 ? p->input.data[3] : 0,
               p->input.length > 4 ? p->input.data[4] : 0,
               p->input.length > 5 ? p->input.data[5] : 0);
    }
    /* Boot-protocol layout: buttons, then signed dx and dy, then wheel.
     * Decoding lives in the portable module so it stays host-tested; this
     * only has to hand over the bytes. */
    if (mouse_decode_boot(p->input.data, p->input.length, &r) == 0)
      ring_push(&r);
    break;
  }

  case ESP_HIDH_CLOSE_EVENT:
    s_state = BTM_FAILED;
    snprintf(s_detail, sizeof s_detail, "disconnected");
    ESP_LOGI(TAG, "closed");
    break;

  default:
    break;
  }
}

/* -------------------------------------------------------------- start -- */

int btmouse_start(int scan_seconds) {
  esp_hid_scan_result_t *results = NULL, *r = NULL, *best = NULL;
  size_t count = 0;
  size_t heap_before = esp_get_free_heap_size();

  if (s_state == BTM_CONNECTED) return 0;

  if (!s_inited) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      nvs_flash_init();
    }
    /* BLE only. The S3 has no Classic radio, so asking for it would fail. */
    if (esp_hid_gap_init(ESP_BT_MODE_BLE) != ESP_OK) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "radio init failed");
      return -1;
    }
    {
      esp_hidh_config_t cfg = {
        .callback = hidh_event,
        .event_stack_size = 4096,
        .callback_arg = NULL,
      };
      if (esp_hidh_init(&cfg) != ESP_OK) {
        s_state = BTM_FAILED;
        snprintf(s_detail, sizeof s_detail, "hid host init failed");
        return -1;
      }
    }
    /* Start the host and wait for it to sync with the controller. Skipping
     * this is what crashed the first attempt: esp_hid_scan calls
     * ble_hs_id_infer_auto, and until the host has synced there is no identity
     * address to infer from -- it dereferences null and panics with
     * LoadProhibited at EXCVADDR 0x0b, which points nowhere near the cause.
     *
     * The IDF example waits a fixed 200 ticks here. Polling the actual sync
     * flag is the same thing without the race. */
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Bonding, which nothing in the IDF example sets up -- it pairs with
     * Espressif's own HID demo, which does not insist on it. A real mouse
     * does: the link encrypted with bonded=0 and the mouse then sent no
     * reports at all, which looks exactly like a working connection.
     *
     * No input and no output on this device, so Just Works pairing; the keys
     * are distributed and stored so the next reconnection needs no pairing
     * mode. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    nimble_port_freertos_init(nimble_host_task);

    {
      int waited = 0;
      while (!ble_hs_synced() && waited < 3000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
      }
      if (!ble_hs_synced()) {
        s_state = BTM_FAILED;
        snprintf(s_detail, sizeof s_detail, "radio did not sync");
        return -1;
      }
      ESP_LOGI(TAG, "host synced after %d ms", waited);
    }

    s_inited = 1;
    s_heap_cost = (uint32_t)(heap_before - esp_get_free_heap_size());
  }

  /* Always scan, even for a mouse already bonded.
   *
   * The first version remembered the peer address and reopened it directly,
   * on the reasoning that a bonded device stops advertising. That is wrong
   * for this hardware: the Pebble uses a resolvable private address that
   * rotates on every advertisement -- observed as ...9c:a5, then a7, then a8
   * within minutes -- so a saved address is stale immediately. Reconnecting
   * to it blocked for thirty seconds and then failed with status 13.
   *
   * Scanning finds it under whatever address it is wearing today, which is
   * what address resolution is for and what actually works. */
  s_state = BTM_SCANNING;
  snprintf(s_detail, sizeof s_detail, "scanning %ds", scan_seconds);

  if (esp_hid_scan((uint32_t)scan_seconds, &count, &results) != ESP_OK) {
    s_state = BTM_FAILED;
    snprintf(s_detail, sizeof s_detail, "scan failed");
    return -1;
  }

  /* Prefer something that says it is a mouse; fall back to any HID device.
   * A keyboard advertising HID would otherwise be grabbed by accident. */
  for (r = results; r; r = r->next) {
    if (r->transport != ESP_HID_TRANSPORT_BLE) continue;
    if (r->ble.appearance == ESP_HID_APPEARANCE_MOUSE) { best = r; break; }
    if (!best) best = r;
  }

  if (!best) {
    esp_hid_scan_results_free(results);
    s_state = BTM_FAILED;
    snprintf(s_detail, sizeof s_detail, "no BLE HID device found");
    return -1;
  }

  s_state = BTM_CONNECTING;
  snprintf(s_detail, sizeof s_detail, "%s",
           best->name ? best->name : "unnamed device");
  ESP_LOGI(TAG, "opening %s", s_detail);

  peer_save(best->bda, best->ble.addr_type);
  esp_hidh_dev_open(best->bda, ESP_HID_TRANSPORT_BLE, best->ble.addr_type);
  esp_hid_scan_results_free(results);
  return 0;
}

void btmouse_stop(void) {
  /* Deliberately does not tear the radio down. NimBLE deinit while a
   * connection is live is a good way to hang, and the memory is already
   * spent -- switching the pointer off is a desktop concern, not a radio
   * one. */
  s_state = s_inited ? BTM_FAILED : BTM_OFF;
  snprintf(s_detail, sizeof s_detail, "stopped");
}

BtMouseState btmouse_state(void) { return s_state; }
uint32_t     btmouse_heap_cost(void) { return s_heap_cost; }

const char *btmouse_status(void) {
  static char buf[80];
  const char *name =
      s_state == BTM_OFF        ? "off" :
      s_state == BTM_SCANNING   ? "scanning" :
      s_state == BTM_CONNECTING ? "connecting" :
      s_state == BTM_CONNECTED  ? "connected" : "failed";
  snprintf(buf, sizeof buf, "%s (%s) %u reports", name, s_detail,
           (unsigned)s_reports);
  return buf;
}
