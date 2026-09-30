/* The saved networks: several, not one. Portable, so the host suite pins it.
 *
 * A device carried between home, work and a phone's hotspot needs all three,
 * and saving the newest over the only one meant going home to a device that
 * had forgotten home. Up to WIFILIST_MAX, most recently joined first; joining
 * one moves it to the front, and a new one past the limit pushes out the one
 * joined longest ago.
 *
 * The text form is /config/wifi.txt and the NVS copy: SSID and password on
 * alternate lines. One pair is the file every earlier version wrote, so an
 * old file reads as a list of one.
 */
#ifndef CARDOS_WIFILIST_H
#define CARDOS_WIFILIST_H

#include <stddef.h>
#include <stdint.h>

#define WIFILIST_MAX  8
#define WIFILIST_SSID 33
#define WIFILIST_PASS 65

typedef struct {
  char ssid[WIFILIST_SSID];
  char pass[WIFILIST_PASS];
} WifiSaved;

typedef struct {
  WifiSaved net[WIFILIST_MAX];
  int       n;
} WifiList;

/* Pairs of lines into the list (CR and trailing blanks forgiven; a blank
 * SSID line ends nothing, it is just skipped with its password). */
void wifilist_parse(WifiList *l, const char *text);

/* The list as pairs of lines, NUL-terminated. Its length, or -1 if `out`
 * is too small (nothing useful written). */
int  wifilist_format(const WifiList *l, char *out, size_t size);

/* Joined just now: to the front, with this password, in or out of the list. */
void wifilist_remember(WifiList *l, const char *ssid, const char *pass);

/* 1 if it was there and is not now. */
int  wifilist_forget(WifiList *l, const char *ssid);

/* Which saved networks to try, in order: the ones a scan saw, strongest
 * first; if the scan saw none of them (or there was no scan), all of them,
 * most recent first -- a hidden network is in no scan. `seen`/`rssi` are the
 * scan, `nseen` long. Writes list indexes into `order`; returns how many. */
int  wifilist_order(const WifiList *l, const char *const *seen, const int8_t *rssi,
                    int nseen, int *order);

#endif /* CARDOS_WIFILIST_H */
