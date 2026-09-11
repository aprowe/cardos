/* Bluetooth LE mouse, HID host role. See btmouse.h.
 *
 * A minimal HOGP client written straight against NimBLE, rather than ESP-IDF's
 * esp_hidh. That component pairs and bonds correctly but never subscribes to
 * the report characteristics: attach_report_listeners takes its ops lock and
 * then blocks in WAIT_CB() on a GATT write, and hangs on the very first
 * descriptor write. Verified identically on a Logitech Pebble M350 and an MX
 * Master 3S -- both reach bonded=1, write the battery CCCD, and then stop
 * dead. It also only subscribes to reports tagged PROTOCOL_MODE_REPORT, so a
 * boot-protocol mouse would be skipped even without that.
 *
 * Everything here runs from GATT callbacks and nothing blocks waiting for a
 * reply, so there is no lock to deadlock on.
 *
 * Low Energy only: the ESP32-S3 dropped the Classic radio the original ESP32
 * carried, so a Classic-only mouse cannot be made to work at all.
 */

#include "kernel/drv/btmouse.h"
#include "kernel/drv/vendor/esp_hid_gap.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

/* Provided by NimBLE's store_config, which has no public header. */
void ble_store_config_init(void);

static const char *TAG = "btmouse";

#define UUID_HID_SVC        0x1812
#define UUID_CHR_REPORT     0x2A4D   /* Report, report protocol mode */
#define UUID_CHR_BOOT_MOUSE 0x2A33   /* Boot Mouse Input Report */
#define UUID_CHR_PROTO_MODE 0x2A4E   /* 0 = boot, 1 = report */
#define UUID_DSC_CCC        0x2902

#define MAX_CHRS    16
#define MAX_REPORTS 8

typedef struct {
  uint16_t def_handle;
  uint16_t val_handle;
  uint16_t uuid16;
  uint8_t  properties;
} Chr;

static BtMouseState s_state;
static char         s_detail[48];
static uint32_t     s_heap_cost;
static uint32_t     s_reports;
static int          s_inited;

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_hid_start, s_hid_end;
static uint8_t  s_own_addr_type;

static Chr s_chr[MAX_CHRS];
static int s_nchr;

static struct { uint16_t val_handle, ccc_handle; } s_rep[MAX_REPORTS];
static int s_nrep;
static int s_dsc_idx;      /* report whose CCCD we are hunting for */
static int s_sub_idx;      /* report we are subscribing to */
static uint16_t s_proto_handle;   /* Protocol Mode characteristic */

/* Decoded reports. Filled from the NimBLE host task and drained by the main
 * loop, which is the only reason this is a ring rather than a variable. */
#define RING 16
static MouseReport s_ring[RING];
static volatile uint8_t s_head, s_tail;

static void ring_push(const MouseReport *r) {
  uint8_t next = (uint8_t)((s_head + 1) % RING);
  if (next == s_tail) return;            /* full: drop rather than block */
  s_ring[s_head] = *r;
  s_head = next;
}

int btmouse_poll(MouseReport *out) {
  if (s_tail == s_head) return 0;
  *out = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) % RING);
  return 1;
}

/* ---------------------------------------------------------- discovery -- */

static void subscribe_next(void);
static void find_ccc_for(int idx);

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (error && error->status != 0)
    ESP_LOGW(TAG, "subscribe %d failed status=%d", s_sub_idx, error->status);
  s_sub_idx++;
  subscribe_next();
  return 0;
}

static void subscribe_next(void) {
  static const uint8_t on[2] = { 0x01, 0x00 };   /* notifications enabled */

  while (s_sub_idx < s_nrep && s_rep[s_sub_idx].ccc_handle == 0) s_sub_idx++;

  if (s_sub_idx >= s_nrep) {
    int live = 0, i;
    for (i = 0; i < s_nrep; i++) if (s_rep[i].ccc_handle) live++;
    if (live == 0) {
      /* Reporting success here would be a lie: a mouse that is connected and
       * subscribed to nothing looks identical to a working one. */
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "no CCCD found on any report");
      ESP_LOGE(TAG, "connected but subscribed to nothing");
      return;
    }
    /* Ask for Boot Protocol. In report mode this mouse sends seven bytes of
     * Logitech's own packing; in boot mode every mouse sends the same fixed
     * buttons/dx/dy/wheel, which is what mouse_decode_boot already handles and
     * what the design note chose for exactly this reason. Write-without-
     * response, so there is nothing to wait for. */
    if (s_proto_handle) {
      static const uint8_t boot = 0x00;
      ESP_LOGI(TAG, "requesting boot protocol via handle %u", s_proto_handle);
      ble_gattc_write_no_rsp_flat(s_conn, s_proto_handle, &boot, 1);
    }

    s_state = BTM_CONNECTED;
    snprintf(s_detail, sizeof s_detail, "%d report%s live", live,
             live == 1 ? "" : "s");
    ESP_LOGI(TAG, "ready: %s", s_detail);
    return;
  }

  ESP_LOGI(TAG, "subscribing report %d ccc=%u", s_sub_idx,
           s_rep[s_sub_idx].ccc_handle);
  if (ble_gattc_write_flat(s_conn, s_rep[s_sub_idx].ccc_handle,
                           on, sizeof on, on_subscribed, NULL) != 0) {
    s_sub_idx++;
    subscribe_next();
  }
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                  void *arg) {
  (void)conn; (void)chr_val_handle; (void)arg;

  if (error && error->status != 0) {     /* BLE_HS_EDONE included */
    find_ccc_for(s_dsc_idx + 1);
    return 0;
  }
  if (dsc) {
    uint16_t u = ble_uuid_u16(&dsc->uuid.u);
    ESP_LOGI(TAG, "  dsc handle=%u uuid=0x%04x (report %d)", dsc->handle, u,
             s_dsc_idx);
    if (u == UUID_DSC_CCC && s_dsc_idx < s_nrep &&
        s_rep[s_dsc_idx].ccc_handle == 0) {
      s_rep[s_dsc_idx].ccc_handle = dsc->handle;
    }
  }
  return 0;
}

/* A report's descriptors sit between its value handle and the next
 * characteristic declaration. */
static void find_ccc_for(int idx) {
  uint16_t start, end;
  int i;

  s_dsc_idx = idx;
  if (idx >= s_nrep) {
    s_sub_idx = 0;
    subscribe_next();
    return;
  }

  /* NimBLE wants the characteristic's *value* handle as the start -- the
   * parameter is called chr_val_handle -- and searches from value+1 itself.
   * Passing value+1 shifts the whole range by one, which lands past the CCCD
   * and finds only the Report Reference descriptor after it. That is exactly
   * how this failed: descriptors 42 and 46 were reported, both 0x2908, while
   * the CCCDs at 41 and 45 were never looked at. */
  start = s_rep[idx].val_handle;
  end = s_hid_end;
  for (i = 0; i < s_nchr; i++) {
    if (s_chr[i].def_handle > s_rep[idx].val_handle &&
        (uint16_t)(s_chr[i].def_handle - 1) < end) {
      end = (uint16_t)(s_chr[i].def_handle - 1);
    }
  }
  if (end < start) {
    /* No gap after this characteristic, so its descriptors cannot be here. */
    ESP_LOGW(TAG, "report %d: no descriptor range (%u..%u)", idx, start, end);
    find_ccc_for(idx + 1);
    return;
  }
  ESP_LOGI(TAG, "report %d: searching descriptors %u..%u", idx, start, end);

  if (ble_gattc_disc_all_dscs(s_conn, start, end, on_dsc, NULL) != 0)
    find_ccc_for(idx + 1);
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg) {
  (void)conn; (void)arg;

  if (error && error->status != 0) {
    int i;
    if (error->status != BLE_HS_EDONE) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "characteristic discovery failed");
      return 0;
    }
    /* Take Report *and* Boot Mouse Input characteristics that can notify.
     * esp_hidh takes only the former, which is why a boot-protocol mouse
     * goes silent there. */
    s_proto_handle = 0;
    for (i = 0; i < s_nchr; i++)
      if (s_chr[i].uuid16 == UUID_CHR_PROTO_MODE) s_proto_handle = s_chr[i].val_handle;

    for (i = 0; i < s_nchr && s_nrep < MAX_REPORTS; i++) {
      uint16_t u = s_chr[i].uuid16;
      if (u != UUID_CHR_REPORT && u != UUID_CHR_BOOT_MOUSE) continue;
      if (!(s_chr[i].properties & BLE_GATT_CHR_PROP_NOTIFY)) continue;
      s_rep[s_nrep].val_handle = s_chr[i].val_handle;
      s_rep[s_nrep].ccc_handle = 0;
      s_nrep++;
    }
    ESP_LOGI(TAG, "%d characteristics, %d notifiable reports", s_nchr, s_nrep);
    if (s_nrep == 0) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "no notifiable reports");
      return 0;
    }
    find_ccc_for(0);
    return 0;
  }
  if (chr && s_nchr < MAX_CHRS) {
    ESP_LOGI(TAG, "  chr def=%u val=%u uuid=0x%04x props=0x%02x",
             chr->def_handle, chr->val_handle,
             ble_uuid_u16(&chr->uuid.u), chr->properties);
    s_chr[s_nchr].def_handle = chr->def_handle;
    s_chr[s_nchr].val_handle = chr->val_handle;
    s_chr[s_nchr].uuid16 = ble_uuid_u16(&chr->uuid.u);
    s_chr[s_nchr].properties = chr->properties;
    s_nchr++;
  }
  return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg) {
  (void)conn; (void)arg;

  if (error && error->status != 0) {
    if (s_hid_start == 0) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "no HID service");
      return 0;
    }
    s_nchr = 0;
    ble_gattc_disc_all_chrs(s_conn, s_hid_start, s_hid_end, on_chr, NULL);
    return 0;
  }
  if (service) {
    s_hid_start = service->start_handle;
    s_hid_end = service->end_handle;
    ESP_LOGI(TAG, "HID service handles %u..%u", s_hid_start, s_hid_end);
  }
  return 0;
}

/* ------------------------------------------------------------- events -- */

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;

  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status != 0) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "connect failed (%d)",
               event->connect.status);
      return 0;
    }
    s_conn = event->connect.conn_handle;
    ESP_LOGI(TAG, "connected; starting encryption");
    /* HOGP will not talk over an unencrypted link. */
    if (ble_gap_security_initiate(s_conn) != 0) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "could not start encryption");
    }
    return 0;

  case BLE_GAP_EVENT_ENC_CHANGE:
    ESP_LOGI(TAG, "encryption status=%d", event->enc_change.status);
    if (event->enc_change.status != 0) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "encryption failed");
      return 0;
    }
    s_hid_start = s_hid_end = 0;
    s_nrep = 0;
    {
      static const ble_uuid16_t hid = BLE_UUID16_INIT(UUID_HID_SVC);
      ble_gattc_disc_svc_by_uuid(s_conn, &hid.u, on_svc, NULL);
    }
    return 0;

  case BLE_GAP_EVENT_NOTIFY_RX: {
    MouseReport r;
    uint8_t buf[8];
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len > sizeof buf) len = sizeof buf;
    ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, NULL);

    s_reports++;
    if (s_reports <= 8) {
      ESP_LOGI(TAG, "report h=%u len=%u: %02x %02x %02x %02x %02x %02x",
               event->notify_rx.attr_handle, (unsigned)len,
               len > 0 ? buf[0] : 0, len > 1 ? buf[1] : 0,
               len > 2 ? buf[2] : 0, len > 3 ? buf[3] : 0,
               len > 4 ? buf[4] : 0, len > 5 ? buf[5] : 0);
    }
    if (mouse_decode_boot(buf, len, &r) == 0) ring_push(&r);
    return 0;
  }

  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_state = BTM_FAILED;
    snprintf(s_detail, sizeof s_detail, "disconnected");
    return 0;

  case BLE_GAP_EVENT_REPEAT_PAIRING: {
    /* Bonded, but the device is offering fresh keys -- which is what happens
     * when it is put back into pairing mode. Drop the stale bond and accept,
     * or the re-pair is refused and nothing ever connects again. */
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
      ble_store_util_delete_peer(&desc.peer_id_addr);
    return BLE_GAP_REPEAT_PAIRING_RETRY;
  }

  default:
    return 0;
  }
}

/* -------------------------------------------------------------- start -- */

static void nimble_host_task(void *param) {
  (void)param;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

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
    if (esp_hid_gap_init(ESP_BT_MODE_BLE) != ESP_OK) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "radio init failed");
      return -1;
    }

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Just Works pairing: this device has no input and no output. Both key
     * distributions, so the bond is actually stored -- without bonding the
     * link encrypts and the mouse then says nothing at all, which looks
     * exactly like success. */
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
    }
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_inited = 1;
    s_heap_cost = (uint32_t)(heap_before - esp_get_free_heap_size());
  }

  /* Always scan. A bonded mouse still advertises, and it wears a resolvable
   * private address that rotates every time, so remembering one is useless --
   * observed rotating through four addresses in as many minutes. */
  s_state = BTM_SCANNING;
  snprintf(s_detail, sizeof s_detail, "scanning %ds", scan_seconds);

  if (esp_hid_scan((uint32_t)scan_seconds, &count, &results) != ESP_OK) {
    s_state = BTM_FAILED;
    snprintf(s_detail, sizeof s_detail, "scan failed");
    return -1;
  }

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
  snprintf(s_detail, sizeof s_detail, "%s", best->name ? best->name : "mouse");
  ESP_LOGI(TAG, "connecting to %s", s_detail);

  {
    ble_addr_t addr;
    addr.type = best->ble.addr_type;
    memcpy(addr.val, best->bda, 6);
    esp_hid_scan_results_free(results);

    if (ble_gap_connect(s_own_addr_type, &addr, 10000, NULL, gap_event, NULL) != 0) {
      s_state = BTM_FAILED;
      snprintf(s_detail, sizeof s_detail, "connect refused");
      return -1;
    }
  }
  return 0;
}

void btmouse_stop(void) {
  if (s_conn != BLE_HS_CONN_HANDLE_NONE)
    ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
  s_state = s_inited ? BTM_FAILED : BTM_OFF;
  snprintf(s_detail, sizeof s_detail, "stopped");
}

BtMouseState btmouse_state(void) { return s_state; }
uint32_t     btmouse_heap_cost(void) { return s_heap_cost; }

const char *btmouse_status(void) {
  static char buf[96];
  const char *name =
      s_state == BTM_OFF        ? "off" :
      s_state == BTM_SCANNING   ? "scanning" :
      s_state == BTM_CONNECTING ? "connecting" :
      s_state == BTM_CONNECTED  ? "connected" : "failed";
  snprintf(buf, sizeof buf, "%s (%s) %u reports", name, s_detail,
           (unsigned)s_reports);
  return buf;
}
