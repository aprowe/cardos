/* Bluetooth LE mouse. Device-only.
 *
 * CardOS is the HID *host* here -- the less common direction. Nearly every
 * ESP32 Bluetooth-HID example is the device role, an ESP32 pretending to be a
 * keyboard for a PC. This consumes reports from a real mouse instead, over
 * HID-over-GATT.
 *
 * Low Energy only, because that is all the ESP32-S3 has: it dropped the
 * Classic radio the original ESP32 carried. A Classic-only mouse cannot be
 * made to work by any amount of code.
 *
 * The radio starts on demand rather than at boot. It costs tens of KB for the
 * whole uptime, and nothing should pay for that until someone asks for a
 * pointer. See docs/specs/NEXT-bluetooth-mouse.md.
 */
#ifndef CARDOS_BTMOUSE_H
#define CARDOS_BTMOUSE_H

#include <stdint.h>

#include "kernel/input/mouse.h"

typedef enum {
  BTM_OFF = 0,
  BTM_SCANNING,
  BTM_CONNECTING,
  BTM_CONNECTED,
  BTM_FAILED
} BtMouseState;

/* Bring up the radio and look for a mouse. Blocking for the scan window --
 * seconds, not milliseconds -- so the caller should say what it is doing
 * first. Returns 0 if a HID device was found and opened. */
int btmouse_start(int scan_seconds);

void         btmouse_stop(void);
BtMouseState btmouse_state(void);
const char  *btmouse_status(void);   /* human-readable, for the shell */

/* Next report from the mouse, or 0 if none is waiting. Never blocks. */
int btmouse_poll(MouseReport *out);

/* Heap cost of the radio, measured across btmouse_start. 0 until started --
 * the spec insists these numbers are measured rather than assumed. */
uint32_t btmouse_heap_cost(void);

#endif /* CARDOS_BTMOUSE_H */
