/* Mouse input: report decoding and cursor tracking.
 *
 * Portable C -- no Bluetooth, no ESP-IDF. The BLE HID plumbing has to wait for
 * hardware, but the logic does not, and the logic is where the bugs are:
 * signed deltas, edge-clamping, and turning button levels into press/release
 * events. When the radio work happens it only has to deliver bytes.
 *
 * See docs/specs/NEXT-bluetooth-mouse.md -- in particular, the ESP32-S3 has
 * Bluetooth LE only, so a Classic-only mouse can never work.
 */
#ifndef CARDOS_MOUSE_H
#define CARDOS_MOUSE_H

#include <stddef.h>
#include <stdint.h>

#define MOUSE_LEFT   0x01
#define MOUSE_RIGHT  0x02
#define MOUSE_MIDDLE 0x04

typedef struct {
  uint8_t buttons;
  int8_t  dx;
  int8_t  dy;
  int8_t  wheel;
} MouseReport;

/* Decode a HID boot-protocol mouse report: buttons, signed dx, signed dy, and
 * an optional wheel. A leading report ID is skipped when present. Returns 0 on
 * success, -1 if the report is too short to be one. */
int mouse_decode_boot(const uint8_t *report, size_t len, MouseReport *out);

/* Cursor state, clamped to a screen of w x h. */
void mouse_init(int16_t w, int16_t h);
void mouse_apply(const MouseReport *r);

int16_t mouse_x(void);
int16_t mouse_y(void);

/* Levels. */
int mouse_down(uint8_t button);

/* Events: each returns 1 once, then 0 until it happens again. A window system
 * dispatches clicks, not button levels. */
int mouse_pressed(uint8_t button);
int mouse_released(uint8_t button);

/* Accumulated wheel movement since the last call. */
int mouse_take_wheel(void);

/* 1 if the cursor actually changed position since the last call -- the
 * compositor's cue to repaint. Running into an edge is not movement. */
int mouse_take_moved(void);

#endif /* CARDOS_MOUSE_H */
