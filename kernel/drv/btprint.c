/* BLE client for the cat-printer family. See btprint.h. */
#include "kernel/drv/btprint.h"
#include "kernel/drv/bthid.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

static const char *TAG = "btprint";

#define UUID_SVC_PRINT   0xAE30
#define UUID_SVC_ADV     0xAF30   /* what the X6h advertises; the service is AE30 */
#define UUID_CHR_TX      0xAE01
#define UUID_CHR_RX      0xAE02
#define UUID_DSC_CCC     0x2902

enum { ST_OFF, ST_CONNECTING, ST_DISCOVERING, ST_READY, ST_FAILED };

static struct {
  int      state;
  uint16_t conn;
  uint16_t svc_start, svc_end;
  uint16_t tx_handle;             /* AE01 value handle */
  uint16_t rx_handle;             /* AE02 value handle */
  uint16_t rx_end;                /* where AE02's descriptors stop */
  uint16_t ccc_handle;
  uint16_t mtu;
  char     error[48];
  uint8_t  reply[32];
  int      reply_len;
} s;

/* s.conn starts at 0, which is a valid handle; the `st != ST_OFF` guard in
 * btprint_disconnect is what stops a never-connected client from
 * terminating somebody else's link 0. */
static BtPrintSeen s_seen[BTPRINT_SCAN_MAX];
static int         s_nseen;
static uint8_t     s_own_addr_type;

static void fail(const char *why) {
  s.state = ST_FAILED;
  snprintf(s.error, sizeof s.error, "%s", why);
  ESP_LOGW(TAG, "%s", why);
}

/* ---------------------------------------------------------- discovery -- */

static int on_ccc_written(uint16_t conn, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
  (void)conn; (void)attr; (void)arg;
  if (error->status != 0) ESP_LOGW(TAG, "notify subscribe failed (%d)", error->status);
  /* Subscribed or not, the printer takes commands. Status is a nicety. */
  s.state = ST_READY;
  return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                  void *arg) {
  (void)conn; (void)chr_val_handle; (void)arg;
  if (error->status == 0 && dsc) {
    if (ble_uuid_u16(&dsc->uuid.u) == UUID_DSC_CCC) s.ccc_handle = dsc->handle;
    return 0;
  }
  if (s.ccc_handle) {
    uint8_t on[2] = { 0x01, 0x00 };
    if (ble_gattc_write_flat(s.conn, s.ccc_handle, on, 2, on_ccc_written, NULL) == 0)
      return 0;
  }
  s.state = ST_READY;                 /* no CCCD: still printable */
  return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg) {
  (void)conn; (void)arg;
  if (error->status == 0 && chr) {
    uint16_t u = ble_uuid_u16(&chr->uuid.u);
    /* The characteristic after AE02 bounds its descriptor range. */
    if (s.rx_handle && !s.rx_end && chr->def_handle > s.rx_handle)
      s.rx_end = (uint16_t)(chr->def_handle - 1);
    if (u == UUID_CHR_TX) s.tx_handle = chr->val_handle;
    if (u == UUID_CHR_RX) s.rx_handle = chr->val_handle;
    return 0;
  }
  if (!s.tx_handle) { fail("no AE01 characteristic"); return 0; }
  if (!s.rx_handle) { s.state = ST_READY; return 0; }
  if (!s.rx_end) s.rx_end = s.svc_end;
  if (ble_gattc_disc_all_dscs(s.conn, s.rx_handle, s.rx_end, on_dsc, NULL) != 0)
    s.state = ST_READY;
  return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg) {
  (void)conn; (void)arg;
  if (error->status == 0 && service) {
    s.svc_start = service->start_handle;
    s.svc_end = service->end_handle;
    return 0;
  }
  if (!s.svc_start) { fail("no AE30 service"); return 0; }
  if (ble_gattc_disc_all_chrs(s.conn, s.svc_start, s.svc_end, on_chr, NULL) != 0)
    fail("characteristic discovery refused");
  return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg) {
  (void)conn; (void)arg;
  /* Called with no error from gap_event when the exchange could not even
   * start: the link is at the default 23 and discovery goes ahead. */
  if (error && error->status == 0) s.mtu = mtu;
  {
    static const ble_uuid16_t svc = BLE_UUID16_INIT(UUID_SVC_PRINT);
    s.state = ST_DISCOVERING;
    if (ble_gattc_disc_svc_by_uuid(s.conn, &svc.u, on_svc, NULL) != 0)
      fail("service discovery refused");
  }
  return 0;
}

/* -------------------------------------------------------------- events -- */

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    if (event->connect.status != 0) {
      char why[48];
      snprintf(why, sizeof why, "connect failed (%d)", event->connect.status);
      fail(why);
      return 0;
    }
    s.conn = event->connect.conn_handle;
    s.mtu = 23;
    if (ble_gattc_exchange_mtu(s.conn, on_mtu, NULL) != 0) on_mtu(s.conn, NULL, 23, NULL);
    return 0;

  case BLE_GAP_EVENT_MTU:
    s.mtu = event->mtu.value;
    return 0;

  case BLE_GAP_EVENT_NOTIFY_RX: {
    uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
    if (len > sizeof s.reply) len = sizeof s.reply;
    ble_hs_mbuf_to_flat(event->notify_rx.om, s.reply, len, NULL);
    s.reply_len = len;
    return 0;
  }

  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
    s.conn = BLE_HS_CONN_HANDLE_NONE;
    if (s.state != ST_OFF) {
      char why[48];
      snprintf(why, sizeof why, "disconnected (%d)", event->disconnect.reason);
      fail(why);
    }
    return 0;

  default:
    return 0;
  }
}

/* ---------------------------------------------------------------- scan -- */

static int looks_like_printer(const struct ble_hs_adv_fields *f) {
  int i;
  for (i = 0; i < f->num_uuids16; i++) {
    uint16_t u = ble_uuid_u16(&f->uuids16[i].u);
    if (u == UUID_SVC_PRINT || u == UUID_SVC_ADV) return 1;
  }
  if (f->name_len >= 3) {
    static const char *const prefixes[] = { "X6h", "GB0", "GT01", "MX0", "XW0", "YT0" };
    size_t p;
    for (p = 0; p < sizeof prefixes / sizeof prefixes[0]; p++) {
      size_t n = strlen(prefixes[p]);
      if (f->name_len >= n && memcmp(f->name, prefixes[p], n) == 0) return 1;
    }
  }
  return 0;
}

static int scan_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  if (event->type != BLE_GAP_EVENT_DISC) return 0;
  {
    struct ble_hs_adv_fields f;
    int i;
    if (ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0)
      return 0;
    if (!looks_like_printer(&f)) return 0;
    for (i = 0; i < s_nseen; i++)
      if (memcmp(s_seen[i].addr, event->disc.addr.val, 6) == 0) {
        /* seen already: a scan response may bring the name the advert lacked */
        if (f.name_len && !s_seen[i].name[0]) {
          size_t n = f.name_len < sizeof s_seen[i].name - 1 ? f.name_len : sizeof s_seen[i].name - 1;
          memcpy(s_seen[i].name, f.name, n);
          s_seen[i].name[n] = 0;
        }
        return 0;
      }
    if (s_nseen >= BTPRINT_SCAN_MAX) return 0;
    memset(&s_seen[s_nseen], 0, sizeof s_seen[0]);
    memcpy(s_seen[s_nseen].addr, event->disc.addr.val, 6);
    s_seen[s_nseen].addr_type = event->disc.addr.type;
    s_seen[s_nseen].rssi = event->disc.rssi;
    if (f.name_len) {
      size_t n = f.name_len < sizeof s_seen[0].name - 1 ? f.name_len : sizeof s_seen[0].name - 1;
      memcpy(s_seen[s_nseen].name, f.name, n);
      s_seen[s_nseen].name[n] = 0;
    }
    s_nseen++;
  }
  return 0;
}

int btprint_scan(int seconds) {
  struct ble_gap_disc_params p;
  int waited = 0;
  if (bt_radio_up() != 0) return -1;
  ble_hs_id_infer_auto(0, &s_own_addr_type);
  s_nseen = 0;
  memset(&p, 0, sizeof p);
  p.passive = 0;                 /* active: the X6h puts its name in the scan response */
  p.filter_duplicates = 0;
  if (ble_gap_disc(s_own_addr_type, seconds * 1000, &p, scan_event, NULL) != 0)
    return -1;
  while (ble_gap_disc_active() && waited < seconds * 1000 + 500) {
    vTaskDelay(pdMS_TO_TICKS(50));
    waited += 50;
  }
  return s_nseen;
}

int btprint_scan_count(void) { return s_nseen; }

const BtPrintSeen *btprint_scan_result(int i) {
  return (i >= 0 && i < s_nseen) ? &s_seen[i] : NULL;
}

/* ------------------------------------------------------------- connect -- */

int btprint_connect(const uint8_t addr[6], uint8_t addr_type, int timeout_ms) {
  ble_addr_t a;
  int waited = 0;

  if (s.state == ST_READY) return 0;
  if (bt_radio_up() != 0) { fail("radio would not start"); return -1; }
  ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (ble_gap_disc_active()) ble_gap_disc_cancel();

  memset(&s, 0, sizeof s);
  s.conn = BLE_HS_CONN_HANDLE_NONE;
  s.state = ST_CONNECTING;
  a.type = addr_type;
  memcpy(a.val, addr, 6);

  {
    int rc = ble_gap_connect(s_own_addr_type, &a, timeout_ms, NULL, gap_event, NULL);
    if (rc != 0) {
      char why[48];
      snprintf(why, sizeof why, "connect refused (%d)", rc);   /* BLE_HS_E* */
      fail(why);
      return -1;
    }
  }
  while ((s.state == ST_CONNECTING || s.state == ST_DISCOVERING) &&
         waited < timeout_ms + 3000) {
    vTaskDelay(pdMS_TO_TICKS(50));
    waited += 50;
  }
  if (s.state != ST_READY) {
    if (s.state != ST_FAILED) fail("printer not found");
    btprint_disconnect();
    return -1;
  }
  ESP_LOGI(TAG, "ready: tx %u rx %u mtu %u", s.tx_handle, s.rx_handle, s.mtu);
  return 0;
}

void btprint_disconnect(void) {
  int st = s.state;
  s.state = ST_OFF;
  /* No radio, no link -- and nothing to ask NimBLE about. A connect that
   * failed because the radio would not start left the state FAILED and the
   * handle at its zero start, which reads as a real connection; the
   * terminate below then went into a host that was never initialised and
   * the device crashed (LoadProhibited in ble_hs_is_enabled, 2026-09-24:
   * printing from Todo with 48 KB free). */
  if (!bthid_radio_on()) {
    s.conn = BLE_HS_CONN_HANDLE_NONE;
    return;
  }
  /* Handle 0 is a real connection -- the first one NimBLE opens, when no
   * mouse or keyboard got there first. Treating it as "none" left the link
   * up, and every print after the first was refused as already connected. */
  if (st != ST_OFF && s.conn != BLE_HS_CONN_HANDLE_NONE) {
    int waited = 0;
    ble_gap_terminate(s.conn, BLE_ERR_REM_USER_CONN_TERM);
    while (s.conn != BLE_HS_CONN_HANDLE_NONE && waited < 1000) {
      vTaskDelay(pdMS_TO_TICKS(20));
      waited += 20;
    }
  }
  if (st == ST_CONNECTING) ble_gap_conn_cancel();
  s.conn = BLE_HS_CONN_HANDLE_NONE;
}

int btprint_connected(void) { return s.state == ST_READY; }

int btprint_write(const uint8_t *buf, size_t n) {
  size_t chunk_max = s.mtu > 3 ? (size_t)(s.mtu - 3) : 20;
  size_t paced = 0;
  if (chunk_max > 180) chunk_max = 180;
  while (n > 0) {
    size_t chunk = n < chunk_max ? n : chunk_max;
    int tries = 0, rc;
    if (s.state != ST_READY) return -1;
    rc = ble_gattc_write_no_rsp_flat(s.conn, s.tx_handle, buf, (uint16_t)chunk);
    if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
      /* out of mbufs: the controller has not drained yet. Wait, not drop. */
      if (++tries > 300) { fail("printer stopped taking data"); return -1; }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (rc != 0) {
      char why[48];
      snprintf(why, sizeof why, "write failed (%d)", rc);
      fail(why);
      return -1;
    }
    buf += chunk;
    n -= chunk;
    /* The printer's own buffer is small and it has no way to say stop. The
     * PC-side probe sent 200 bytes every 20 ms and printed cleanly, so this
     * keeps that rate -- 10 bytes per ms -- whatever the link's MTU makes a
     * chunk. The X6h only ever offers 23, so a chunk is 20 bytes and a 206-row
     * page is 620 writes; at a flat 20 ms each that was 14 seconds. */
    paced += chunk;
    if (paced >= 100) { vTaskDelay(pdMS_TO_TICKS(paced / 10)); paced = 0; }
  }
  return 0;
}

int btprint_last_reply(uint8_t *out, size_t size) {
  int n = s.reply_len;
  if (n <= 0) return 0;
  if ((size_t)n > size) n = (int)size;
  memcpy(out, s.reply, (size_t)n);
  return n;
}

const char *btprint_error(void) { return s.error; }

/* ---------------------------------------------------------- addresses -- */

int btprint_parse_addr(const char *text, uint8_t addr[6]) {
  unsigned b[6];
  if (!text) return -1;
  if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
    return -1;
  {
    int i;
    for (i = 0; i < 6; i++) addr[i] = (uint8_t)b[5 - i];
  }
  return 0;
}

void btprint_format_addr(const uint8_t addr[6], char *out, size_t size) {
  snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}
