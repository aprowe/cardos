/* Bluetooth LE HID host. See bthid.h.
 *
 * A minimal HOGP client written straight against NimBLE, rather than ESP-IDF's
 * esp_hidh. That component pairs and bonds correctly but never subscribes to
 * the report characteristics: attach_report_listeners takes its ops lock and
 * then blocks in WAIT_CB() on a GATT write, and hangs on the very first
 * descriptor write. Verified identically on a Logitech Pebble M350 and an MX
 * Master 3S -- both reach bonded=1, write the battery CCCD, and then stop
 * dead. It also only subscribes to reports tagged PROTOCOL_MODE_REPORT, so a
 * boot-protocol device would be skipped even without that.
 *
 * Everything here runs from GATT callbacks and nothing blocks waiting for a
 * reply, so there is no lock to deadlock on.
 *
 * Two links are supported at once. All the discovery state that used to be
 * file-scope globals now lives in a Link, found by connection handle, because
 * a mouse and a keyboard discover their services concurrently and one set of
 * globals would have them overwrite each other's handles.
 */

#include "kernel/drv/bthid.h"
#include "kernel/drv/vendor/esp_hid_gap.h"
#include "kernel/input/kbd_hid.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
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

static const char *TAG = "bthid";

#define UUID_HID_SVC        0x1812
#define UUID_CHR_REPORT     0x2A4D   /* Report, report protocol mode */
#define UUID_CHR_BOOT_KBD   0x2A22   /* Boot Keyboard Input Report */
#define UUID_CHR_BOOT_MOUSE 0x2A33   /* Boot Mouse Input Report */
#define UUID_CHR_PROTO_MODE 0x2A4E   /* 0 = boot, 1 = report */
#define UUID_DSC_CCC        0x2902

#define MAX_CHRS    16
#define MAX_REPORTS 8
#define MAX_LINKS   2

typedef struct {
  uint16_t def_handle;
  uint16_t val_handle;
  uint16_t uuid16;
  uint8_t  properties;
} Chr;

typedef struct {
  int        used;
  BtHidKind  kind;
  BtHidState state;
  char       detail[48];
  uint32_t   reports;

  uint16_t conn;
  uint16_t hid_start, hid_end;

  Chr chr[MAX_CHRS];
  int nchr;

  struct { uint16_t val_handle, ccc_handle; } rep[MAX_REPORTS];
  int nrep;
  int dsc_idx;                /* report whose CCCD we are hunting for */
  int sub_idx;                /* report we are subscribing to */
  uint16_t proto_handle;      /* Protocol Mode characteristic */
} Link;

static Link     s_link[MAX_LINKS];
static uint32_t s_heap_cost;
static int      s_inited;
static uint8_t  s_own_addr_type;

/* What the scan is currently looking for, so the connect callback knows which
 * kind of link it is opening. */
static BtHidKind s_want = BTHID_MOUSE;

/* ------------------------------------------------------------- links ----- */

static Link *link_for_kind(BtHidKind k) {
  int i;
  for (i = 0; i < MAX_LINKS; i++)
    if (s_link[i].used && s_link[i].kind == k) return &s_link[i];
  return NULL;
}

static Link *link_for_conn(uint16_t conn) {
  int i;
  for (i = 0; i < MAX_LINKS; i++)
    if (s_link[i].used && s_link[i].conn == conn) return &s_link[i];
  return NULL;
}

static Link *link_claim(BtHidKind k) {
  Link *l = link_for_kind(k);
  int i;
  if (!l) {
    for (i = 0; i < MAX_LINKS; i++) if (!s_link[i].used) { l = &s_link[i]; break; }
  }
  if (!l) return NULL;
  memset(l, 0, sizeof *l);
  l->used = 1;
  l->kind = k;
  l->conn = BLE_HS_CONN_HANDLE_NONE;
  return l;
}

/* The link whose discovery is in flight. There is only ever one connect
 * attempt outstanding, because bthid_start blocks for the scan. */
static Link *s_pending;

/* --------------------------------------------------------------- queues -- */

/* Decoded reports. Filled from the NimBLE host task and drained by the main
 * loop, which is the only reason these are rings rather than variables. */
#define RING 16
static MouseReport s_mring[RING];
static volatile uint8_t s_mhead, s_mtail;

#define KRING 32
static uint8_t s_kring[KRING];
static volatile uint8_t s_khead, s_ktail;

static KbdHid s_kbd;

/* Merge consecutive movement into the pending entry rather than queueing it.
 *
 * A mouse reports far faster than a 240x135 panel can be repainted, so a plain
 * queue means the cursor keeps replaying stale movement after the hand has
 * stopped -- it feels like the movements are backing up, because they are.
 * Summing deltas loses nothing: two moves of +3 are one move of +6.
 *
 * A report that changes the buttons starts a new entry, so a click is never
 * merged into a drag or lost. */
static void mouse_push(const MouseReport *r) {
  uint8_t next = (uint8_t)((s_mhead + 1) % RING);

  if (s_mhead != s_mtail) {
    uint8_t last = (uint8_t)((s_mhead + RING - 1) % RING);
    MouseReport *m = &s_mring[last];
    if (m->buttons == r->buttons) {
      int dx = m->dx + r->dx, dy = m->dy + r->dy, w = m->wheel + r->wheel;
      if (dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127 &&
          w >= -128 && w <= 127) {
        m->dx = (int8_t)dx;
        m->dy = (int8_t)dy;
        m->wheel = (int8_t)w;
        return;
      }
    }
  }

  if (next == s_mtail) {
    /* Full anyway: drop the oldest, not the newest. A stale delta is worth
     * less than a fresh one. */
    s_mtail = (uint8_t)((s_mtail + 1) % RING);
  }
  s_mring[s_mhead] = *r;
  s_mhead = next;
}

/* Keystrokes are the opposite case: they must not be merged and must not be
 * dropped from the front, because the order is the whole content. A full queue
 * drops the newest, which is the one the user has not seen yet and will notice
 * least. */
static void key_push(uint8_t c) {
  uint8_t next = (uint8_t)((s_khead + 1) % KRING);
  if (next == s_ktail) return;
  s_kring[s_khead] = c;
  s_khead = next;
}

int bthid_poll_mouse(MouseReport *out) {
  if (s_mtail == s_mhead) return 0;
  *out = s_mring[s_mtail];
  s_mtail = (uint8_t)((s_mtail + 1) % RING);
  return 1;
}

int bthid_poll_key(uint8_t *out) {
  if (s_ktail == s_khead) return 0;
  *out = s_kring[s_ktail];
  s_ktail = (uint8_t)((s_ktail + 1) % KRING);
  return 1;
}

void bthid_tick(uint32_t now_ms) {
  uint8_t c;
  while (kbd_hid_repeat(&s_kbd, now_ms, &c, 1) == 1) key_push(c);
}

/* ---------------------------------------------------------- discovery -- */

static void subscribe_next(Link *l);
static void find_ccc_for(Link *l, int idx);

static int on_subscribed(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
  Link *l = (Link *)arg;
  (void)conn; (void)attr;
  if (!l || !l->used) return 0;
  if (error && error->status != 0)
    ESP_LOGW(TAG, "subscribe %d failed status=%d", l->sub_idx, error->status);
  l->sub_idx++;
  subscribe_next(l);
  return 0;
}

static void subscribe_next(Link *l) {
  static const uint8_t on[2] = { 0x01, 0x00 };   /* notifications enabled */

  while (l->sub_idx < l->nrep && l->rep[l->sub_idx].ccc_handle == 0) l->sub_idx++;

  if (l->sub_idx >= l->nrep) {
    int live = 0, i;
    for (i = 0; i < l->nrep; i++) if (l->rep[i].ccc_handle) live++;
    if (live == 0) {
      /* Reporting success here would be a lie: a device that is connected and
       * subscribed to nothing looks identical to a working one. */
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "no CCCD found on any report");
      ESP_LOGE(TAG, "connected but subscribed to nothing");
      return;
    }
    /* Ask for Boot Protocol. In report mode a Logitech mouse sends seven bytes
     * of its own packing; in boot mode every device sends the same fixed
     * shape, which is what the decoders already handle and what the design
     * note chose for exactly this reason. Write-without-response, so there is
     * nothing to wait for. */
    if (l->proto_handle) {
      static const uint8_t boot = 0x00;
      ESP_LOGI(TAG, "requesting boot protocol via handle %u", l->proto_handle);
      ble_gattc_write_no_rsp_flat(l->conn, l->proto_handle, &boot, 1);
    }

    l->state = BTH_CONNECTED;
    snprintf(l->detail, sizeof l->detail, "%d report%s live", live,
             live == 1 ? "" : "s");
    ESP_LOGI(TAG, "ready: %s", l->detail);
    return;
  }

  ESP_LOGI(TAG, "subscribing report %d ccc=%u", l->sub_idx,
           l->rep[l->sub_idx].ccc_handle);
  if (ble_gattc_write_flat(l->conn, l->rep[l->sub_idx].ccc_handle,
                           on, sizeof on, on_subscribed, l) != 0) {
    l->sub_idx++;
    subscribe_next(l);
  }
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                  void *arg) {
  Link *l = (Link *)arg;
  (void)conn; (void)chr_val_handle;

  if (!l || !l->used) return 0;
  if (error && error->status != 0) {     /* BLE_HS_EDONE included */
    find_ccc_for(l, l->dsc_idx + 1);
    return 0;
  }
  if (dsc) {
    uint16_t u = ble_uuid_u16(&dsc->uuid.u);
    if (u == UUID_DSC_CCC && l->dsc_idx < l->nrep &&
        l->rep[l->dsc_idx].ccc_handle == 0) {
      l->rep[l->dsc_idx].ccc_handle = dsc->handle;
    }
  }
  return 0;
}

/* A report's descriptors sit between its value handle and the next
 * characteristic declaration. */
static void find_ccc_for(Link *l, int idx) {
  uint16_t start, end;
  int i;

  l->dsc_idx = idx;
  if (idx >= l->nrep) {
    l->sub_idx = 0;
    subscribe_next(l);
    return;
  }

  /* NimBLE wants the characteristic's *value* handle as the start -- the
   * parameter is called chr_val_handle -- and searches from value+1 itself.
   * Passing value+1 shifts the whole range by one, which lands past the CCCD
   * and finds only the Report Reference descriptor after it. That is exactly
   * how this failed: descriptors 42 and 46 were reported, both 0x2908, while
   * the CCCDs at 41 and 45 were never looked at. */
  start = l->rep[idx].val_handle;
  end = l->hid_end;
  for (i = 0; i < l->nchr; i++) {
    if (l->chr[i].def_handle > l->rep[idx].val_handle &&
        (uint16_t)(l->chr[i].def_handle - 1) < end) {
      end = (uint16_t)(l->chr[i].def_handle - 1);
    }
  }
  if (end < start) {
    ESP_LOGW(TAG, "report %d: no descriptor range (%u..%u)", idx, start, end);
    find_ccc_for(l, idx + 1);
    return;
  }
  if (ble_gattc_disc_all_dscs(l->conn, start, end, on_dsc, l) != 0)
    find_ccc_for(l, idx + 1);
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg) {
  Link *l = (Link *)arg;
  (void)conn;

  if (!l || !l->used) return 0;

  if (error && error->status != 0) {
    int i;
    if (error->status != BLE_HS_EDONE) {
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "characteristic discovery failed");
      return 0;
    }
    /* Take Report *and* the boot input characteristics that can notify.
     * esp_hidh takes only the former, which is why a boot-protocol device goes
     * silent there. */
    l->proto_handle = 0;
    for (i = 0; i < l->nchr; i++)
      if (l->chr[i].uuid16 == UUID_CHR_PROTO_MODE)
        l->proto_handle = l->chr[i].val_handle;

    for (i = 0; i < l->nchr && l->nrep < MAX_REPORTS; i++) {
      uint16_t u = l->chr[i].uuid16;
      if (u != UUID_CHR_REPORT && u != UUID_CHR_BOOT_MOUSE && u != UUID_CHR_BOOT_KBD)
        continue;
      if (!(l->chr[i].properties & BLE_GATT_CHR_PROP_NOTIFY)) continue;
      l->rep[l->nrep].val_handle = l->chr[i].val_handle;
      l->rep[l->nrep].ccc_handle = 0;
      l->nrep++;
    }
    ESP_LOGI(TAG, "%d characteristics, %d notifiable reports", l->nchr, l->nrep);
    if (l->nrep == 0) {
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "no notifiable reports");
      return 0;
    }
    find_ccc_for(l, 0);
    return 0;
  }
  if (chr && l->nchr < MAX_CHRS) {
    l->chr[l->nchr].def_handle = chr->def_handle;
    l->chr[l->nchr].val_handle = chr->val_handle;
    l->chr[l->nchr].uuid16 = ble_uuid_u16(&chr->uuid.u);
    l->chr[l->nchr].properties = chr->properties;
    l->nchr++;
  }
  return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg) {
  Link *l = (Link *)arg;
  (void)conn;

  if (!l || !l->used) return 0;

  if (error && error->status != 0) {
    if (l->hid_start == 0) {
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "no HID service");
      return 0;
    }
    l->nchr = 0;
    ble_gattc_disc_all_chrs(l->conn, l->hid_start, l->hid_end, on_chr, l);
    return 0;
  }
  if (service) {
    l->hid_start = service->start_handle;
    l->hid_end = service->end_handle;
    ESP_LOGI(TAG, "HID service handles %u..%u", l->hid_start, l->hid_end);
  }
  return 0;
}

/* ------------------------------------------------------------- events -- */

/* Reports are told apart by length, not by which link they arrived on. A boot
 * mouse report is three or four bytes; a boot keyboard report is eight, or
 * nine with a report ID. That also makes a combined device work without being
 * special-cased, and stops a mis-guessed link kind from silently discarding
 * everything the device sends. */
static void dispatch_report(const uint8_t *buf, uint16_t len) {
  if (len == 8 || len == 9) {
    uint8_t out[KBD_HID_MAX_KEYS];
    int n = kbd_hid_decode(&s_kbd, buf, len,
                           (uint32_t)(esp_timer_get_time() / 1000), out,
                           (int)sizeof out);
    int i;
    for (i = 0; i < n; i++) key_push(out[i]);
    return;
  }
  {
    MouseReport r;
    if (mouse_decode_boot(buf, len, &r) == 0) mouse_push(&r);
  }
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;

  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT: {
    Link *l = s_pending;
    if (!l) return 0;
    if (event->connect.status != 0) {
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "connect failed (%d)",
               event->connect.status);
      s_pending = NULL;
      return 0;
    }
    l->conn = event->connect.conn_handle;
    ESP_LOGI(TAG, "connected; starting encryption");
    /* HOGP will not talk over an unencrypted link. */
    if (ble_gap_security_initiate(l->conn) != 0) {
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "could not start encryption");
    }
    return 0;
  }

  case BLE_GAP_EVENT_ENC_CHANGE: {
    Link *l = link_for_conn(event->enc_change.conn_handle);
    if (!l) return 0;
    ESP_LOGI(TAG, "encryption status=%d", event->enc_change.status);
    if (event->enc_change.status != 0) {
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "encryption failed");
      return 0;
    }
    l->hid_start = l->hid_end = 0;
    l->nrep = 0;
    {
      static const ble_uuid16_t hid = BLE_UUID16_INIT(UUID_HID_SVC);
      ble_gattc_disc_svc_by_uuid(l->conn, &hid.u, on_svc, l);
    }
    return 0;
  }

  case BLE_GAP_EVENT_NOTIFY_RX: {
    Link *l = link_for_conn(event->notify_rx.conn_handle);
    uint8_t buf[10];
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len > sizeof buf) len = sizeof buf;
    ble_hs_mbuf_to_flat(event->notify_rx.om, buf, len, NULL);
    if (l) l->reports++;
    dispatch_report(buf, len);
    return 0;
  }

  case BLE_GAP_EVENT_DISCONNECT: {
    Link *l = link_for_conn(event->disconnect.conn.conn_handle);
    ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
    if (l) {
      l->conn = BLE_HS_CONN_HANDLE_NONE;
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "disconnected");
      /* Keys held when the link dropped are not held any more, and a repeat
       * left running would type forever. */
      if (l->kind == BTHID_KEYBOARD) kbd_hid_init(&s_kbd);
    }
    return 0;
  }

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

/* Measured: the NimBLE stack and controller take about 67 KB. Below this the
 * allocations that fail are inside the controller, where the failure surfaces
 * as a link that never completes rather than as an error. */
#define BT_MIN_HEAP (80 * 1024)

static int radio_up(void) {
  size_t heap_before = esp_get_free_heap_size();

  if (s_inited) return 0;
  if (heap_before < BT_MIN_HEAP) {
    ESP_LOGE(TAG, "only %u KB free, bluetooth needs %u",
             (unsigned)(heap_before / 1024), (unsigned)(BT_MIN_HEAP / 1024));
    return -1;
  }

  {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      nvs_flash_init();
    }
  }
  if (esp_hid_gap_init(ESP_BT_MODE_BLE) != ESP_OK) return -1;

  ble_store_config_init();
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  /* Just Works pairing: this device has no input and no output. Both key
   * distributions, so the bond is actually stored -- without bonding the link
   * encrypts and the device then says nothing at all, which looks exactly like
   * success. */
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
    if (!ble_hs_synced()) return -1;
  }
  ble_hs_id_infer_auto(0, &s_own_addr_type);
  s_inited = 1;
  s_heap_cost = (uint32_t)(heap_before - esp_get_free_heap_size());
  kbd_hid_init(&s_kbd);
  return 0;
}

/* Is this scan result already one of our open links? Reconnecting to the mouse
 * while looking for a keyboard would take the second and last slot for a
 * duplicate. */
static int already_connected(const esp_hid_scan_result_t *r) {
  int i;
  for (i = 0; i < MAX_LINKS; i++) {
    struct ble_gap_conn_desc desc;
    if (!s_link[i].used || s_link[i].conn == BLE_HS_CONN_HANDLE_NONE) continue;
    if (s_link[i].state != BTH_CONNECTED) continue;
    if (ble_gap_conn_find(s_link[i].conn, &desc) != 0) continue;
    if (memcmp(desc.peer_id_addr.val, r->bda, 6) == 0) return 1;
    if (memcmp(desc.peer_ota_addr.val, r->bda, 6) == 0) return 1;
  }
  return 0;
}

int bthid_start(int scan_seconds, BtHidKind want) {
  esp_hid_scan_result_t *results = NULL, *r = NULL, *best = NULL;
  size_t count = 0;
  Link *l;

  l = link_for_kind(want);
  if (l && l->state == BTH_CONNECTED) return 0;

  if (radio_up() != 0) {
    l = link_claim(want);
    if (l) {
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "radio would not start");
    }
    return -1;
  }

  l = link_claim(want);
  if (!l) return -1;

  /* Always scan. A bonded device still advertises, and it wears a resolvable
   * private address that rotates every time, so remembering one is useless --
   * observed rotating through four addresses in as many minutes. */
  l->state = BTH_SCANNING;
  snprintf(l->detail, sizeof l->detail, "scanning %ds", scan_seconds);
  s_want = want;

  if (esp_hid_scan((uint32_t)scan_seconds, &count, &results) != ESP_OK) {
    l->state = BTH_FAILED;
    snprintf(l->detail, sizeof l->detail, "scan failed");
    return -1;
  }

  /* Prefer a device whose advertised appearance matches what was asked for,
   * and fall back to any HID device -- a bonded peripheral often advertises
   * with neither a name nor an appearance, so insisting on the match would
   * make an already-paired keyboard invisible. */
  {
    uint16_t wanted = (want == BTHID_KEYBOARD) ? ESP_HID_APPEARANCE_KEYBOARD
                                               : ESP_HID_APPEARANCE_MOUSE;
    uint16_t other  = (want == BTHID_KEYBOARD) ? ESP_HID_APPEARANCE_MOUSE
                                               : ESP_HID_APPEARANCE_KEYBOARD;
    for (r = results; r; r = r->next) {
      if (r->transport != ESP_HID_TRANSPORT_BLE) continue;
      if (already_connected(r)) continue;
      if (r->ble.appearance == wanted) { best = r; break; }
      if (r->ble.appearance == other) continue;   /* the wrong kind, plainly */
      if (!best) best = r;
    }
  }

  if (!best) {
    esp_hid_scan_results_free(results);
    l->state = BTH_FAILED;
    snprintf(l->detail, sizeof l->detail, "no BLE HID device found");
    return -1;
  }

  l->state = BTH_CONNECTING;
  snprintf(l->detail, sizeof l->detail, "%s",
           best->name ? best->name : (want == BTHID_KEYBOARD ? "keyboard" : "mouse"));
  ESP_LOGI(TAG, "connecting to %s", l->detail);

  {
    ble_addr_t addr;
    addr.type = best->ble.addr_type;
    memcpy(addr.val, best->bda, 6);
    esp_hid_scan_results_free(results);

    s_pending = l;
    if (ble_gap_connect(s_own_addr_type, &addr, 10000, NULL, gap_event, NULL) != 0) {
      l->state = BTH_FAILED;
      snprintf(l->detail, sizeof l->detail, "connect refused");
      s_pending = NULL;
      return -1;
    }
  }
  return 0;
}

/* Wait for a link that is mid-handshake to settle. Discovery runs entirely
 * from GATT callbacks, so this is a poll rather than a wait on anything --
 * there is no lock to block on, which is the whole reason this client works
 * where esp_hidh deadlocked. */
static int settle(Link *l, int ms) {
  int waited = 0;
  while (l->used && l->state == BTH_CONNECTING && waited < ms) {
    vTaskDelay(pdMS_TO_TICKS(50));
    waited += 50;
  }
  return l->used && l->state == BTH_CONNECTED;
}

int bthid_autoconnect(int scan_seconds) {
  esp_hid_scan_result_t *results = NULL, *r;
  size_t count = 0;
  int opened = 0;

  if (radio_up() != 0) return 0;
  if (esp_hid_scan((uint32_t)scan_seconds, &count, &results) != ESP_OK) return 0;

  for (r = results; r && opened < MAX_LINKS; r = r->next) {
    BtHidKind kind;
    Link *l;
    ble_addr_t addr;

    if (r->transport != ESP_HID_TRANSPORT_BLE) continue;
    if (already_connected(r)) continue;

    /* The appearance is the only thing that says which this is before the
     * services are read. A device that advertises neither is assumed to be a
     * mouse, because that is the one worth guessing: a keyboard that guesses
     * wrong still types, while a mouse that never connects is a dead
     * pointer. */
    kind = (r->ble.appearance == ESP_HID_APPEARANCE_KEYBOARD) ? BTHID_KEYBOARD
                                                              : BTHID_MOUSE;
    l = link_for_kind(kind);
    if (l && l->state == BTH_CONNECTED) continue;

    l = link_claim(kind);
    if (!l) continue;
    l->state = BTH_CONNECTING;
    snprintf(l->detail, sizeof l->detail, "%s",
             r->name ? r->name : (kind == BTHID_KEYBOARD ? "keyboard" : "mouse"));

    addr.type = r->ble.addr_type;
    memcpy(addr.val, r->bda, 6);
    s_pending = l;
    if (ble_gap_connect(s_own_addr_type, &addr, 8000, NULL, gap_event, NULL) != 0) {
      l->state = BTH_FAILED;
      continue;
    }
    /* One connect at a time: discovery is driven by s_pending, and starting a
     * second before the first has a handle would hand its events to the wrong
     * link. */
    if (settle(l, 6000)) opened++;
    s_pending = NULL;
  }

  esp_hid_scan_results_free(results);
  ESP_LOGI(TAG, "autoconnect: %d link%s", opened, opened == 1 ? "" : "s");
  return opened;
}

#define NVS_NS      "cardos"
#define NVS_BTBOOT  "btboot"

int bthid_autostart(void) {
  nvs_handle_t h;
  uint8_t v = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
  if (nvs_get_u8(h, NVS_BTBOOT, &v) != ESP_OK) v = 0;
  nvs_close(h);
  return v ? 1 : 0;
}

void bthid_set_autostart(int on) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, NVS_BTBOOT, (uint8_t)(on ? 1 : 0));
  nvs_commit(h);
  nvs_close(h);
}

void bthid_stop(BtHidKind kind) {
  Link *l = link_for_kind(kind);
  if (!l) return;
  if (l->conn != BLE_HS_CONN_HANDLE_NONE)
    ble_gap_terminate(l->conn, BLE_ERR_REM_USER_CONN_TERM);
  l->used = 0;
  if (kind == BTHID_KEYBOARD) kbd_hid_init(&s_kbd);
}

void bthid_stop_all(void) {
  bthid_stop(BTHID_MOUSE);
  bthid_stop(BTHID_KEYBOARD);
}

BtHidState bthid_state(BtHidKind kind) {
  Link *l = link_for_kind(kind);
  if (!l) return s_inited ? BTH_OFF : BTH_OFF;
  return l->state;
}

const char *bthid_status(BtHidKind kind) {
  static char buf[96];
  Link *l = link_for_kind(kind);
  const char *name;

  if (!l) { snprintf(buf, sizeof buf, "off"); return buf; }
  name = l->state == BTH_OFF        ? "off" :
         l->state == BTH_SCANNING   ? "scanning" :
         l->state == BTH_CONNECTING ? "connecting" :
         l->state == BTH_CONNECTED  ? "connected" : "failed";
  snprintf(buf, sizeof buf, "%s (%s) %u reports", name, l->detail,
           (unsigned)l->reports);
  return buf;
}

uint32_t bthid_heap_cost(void) { return s_heap_cost; }
