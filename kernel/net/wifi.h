/* WiFi, station role. Device-only.
 *
 * Started on demand, never at boot. The radio costs tens of KB of heap for as
 * long as it is up -- measured, not assumed, and reported by wifi_heap_cost --
 * and on a board with 512 KB and no PSRAM nothing should pay that until
 * someone asks to be online.
 *
 * Credentials live in NVS so a reconnect after a reboot needs no typing. They
 * are stored in the clear, as they must be for the device to reconnect by
 * itself; the flash is not encrypted, so anyone holding the board can read
 * them. That is the honest position, and worth knowing before typing a
 * password you use elsewhere.
 */
#ifndef CARDOS_WIFI_H
#define CARDOS_WIFI_H

#include <stdint.h>

#define WIFI_SSID_MAX 33
#define WIFI_PASS_MAX 65
#define WIFI_MAX_SCAN 12

typedef enum {
  WIFI_OFF = 0,
  WIFI_CONNECTING,
  WIFI_CONNECTED,
  WIFI_FAILED
} WifiState;

typedef struct {
  char ssid[WIFI_SSID_MAX];
  int8_t rssi;
  int  open;          /* no passphrase needed */
} WifiAp;

/* Bring the radio up if it is not already. Costs heap; see wifi_heap_cost. */
int wifi_start(void);
void wifi_stop(void);

/* Blocking, up to `timeout_ms`. Saves the credentials on success. */
int wifi_connect(const char *ssid, const char *pass, int timeout_ms);

/* Connect using whatever was saved. Returns -1 if nothing was. */
int wifi_connect_saved(int timeout_ms);

/* When NVS has no networks but /config/wifi.txt (SSID, then password, on
 * alternate lines -- one pair is the old single-network file) does, take the
 * file's. Called once at boot after the card mounts. Returns how many. */
int wifi_restore_from_card(void);

/* Blocking scan. Fills `out` with up to `max` networks, strongest first, and
 * returns how many. */
int wifi_scan(WifiAp *out, int max);

WifiState   wifi_state(void);
int         wifi_is_connected(void);
const char *wifi_status(void);        /* human-readable, one line */
const char *wifi_ip(void);            /* "0.0.0.0" when not connected */
/* The saved networks (kernel/net/wifilist.h): up to eight, newest first.
 * wifi_connect adds or refreshes one; wifi_connect_saved joins whichever is
 * in range. */
const char *wifi_saved_ssid(void);    /* the newest, "" if nothing is saved */
int         wifi_saved_count(void);
const char *wifi_saved_name(int i);   /* 0 is the newest; "" past the end */
int         wifi_forget_one(const char *ssid);   /* 1 if it was saved */
void        wifi_forget(void);        /* every one */

uint32_t wifi_heap_cost(void);

#endif /* CARDOS_WIFI_H */
