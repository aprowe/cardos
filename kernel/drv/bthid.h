/* Bluetooth LE HID host: mouse and keyboard. Device-only.
 *
 * CardOS is the HID *host* here -- the less common direction. Nearly every
 * ESP32 Bluetooth-HID example is the device role, an ESP32 pretending to be a
 * keyboard for a PC. This consumes reports from real peripherals instead, over
 * HID-over-GATT.
 *
 * Low Energy only, because that is all the ESP32-S3 has: it dropped the
 * Classic radio the original ESP32 carried. A Classic-only mouse or keyboard
 * cannot be made to work by any amount of code.
 *
 * Two links at once, so a mouse and a keyboard can both be connected. Reports
 * are told apart by length rather than by which link they arrived on -- a boot
 * mouse report is three or four bytes and a boot keyboard report is eight --
 * which also means a combined device works without being special-cased.
 *
 * The radio starts on demand rather than at boot. It costs tens of KB for the
 * whole uptime, and nothing should pay for that until someone asks.
 */
#ifndef CARDOS_BTHID_H
#define CARDOS_BTHID_H

#include <stdint.h>

#include "kernel/input/mouse.h"

typedef enum { BTHID_MOUSE = 0, BTHID_KEYBOARD } BtHidKind;

typedef enum {
  BTH_OFF = 0,
  BTH_SCANNING,
  BTH_CONNECTING,
  BTH_CONNECTED,
  BTH_FAILED
} BtHidState;

/* Bring up the radio and look for a device of this kind. Blocking for the scan
 * window -- seconds, not milliseconds -- so the caller should say what it is
 * doing first. Returns 0 if a device was found and opened. */
int bthid_start(int scan_seconds, BtHidKind want);

/* One scan, and connect to everything HID it finds, up to both slots. For
 * boot: a device that was bonded last time is usually still advertising, and a
 * short look costs a second where two separate scans cost two.
 *
 * The radio's ~67 KB is the real price, and it is paid for the whole uptime
 * from the moment this runs -- so it is a setting, not a default buried in the
 * boot path. Returns how many links reached CONNECTED. */
int bthid_autoconnect(int scan_seconds);

/* Whether to run that scan at boot. Remembered across reboots. Off by default:
 * the radio costs about 67 KB for the whole uptime, and a machine that is
 * being used from its own keyboard should not pay that without being asked. */
int  bthid_autostart(void);
void bthid_set_autostart(int on);

/* Disconnect one kind, or everything and the radio with it. */
void bthid_stop(BtHidKind kind);
void bthid_stop_all(void);

/* Is the Bluetooth radio up? Distinct from "nothing is connected": a link
 * that was never claimed reads as BTH_OFF, so this is what says whether
 * reconnecting is even possible. */
int         bthid_radio_on(void);

/* Every BLE device the scan sees, one line each, through `say`. A diagnostic:
 * when a mouse will not reconnect, this says whether it is advertising at
 * all, which is the fork in the road. */
void        bthid_scan_dump(int scan_seconds, void (*say)(const char *line));

BtHidState  bthid_state(BtHidKind kind);
const char *bthid_status(BtHidKind kind);   /* human-readable, one line */

/* Next report, or 0 if none is waiting. Never blocks. */
int bthid_poll_mouse(MouseReport *out);
int bthid_poll_key(uint8_t *out);

/* Drives key auto-repeat, which the keyboard does not generate itself. Call
 * once per pass of whatever loop is draining the queues. */
void bthid_tick(uint32_t now_ms);

/* Heap cost of the radio, measured across the first start. 0 until then --
 * the design notes insist these numbers are measured rather than assumed. */
uint32_t bthid_heap_cost(void);

#endif /* CARDOS_BTHID_H */
