/* HID boot-protocol keyboard decoding. Portable -- no ESP-IDF, host-tested.
 *
 * A boot keyboard report is eight bytes:
 *
 *     [0] modifier bitmap   [1] reserved   [2..7] up to six usage codes
 *
 * It describes which keys are *held*, not which were pressed, so turning it
 * into a stream of characters means diffing against the previous report. That
 * diff is the whole reason this is a module with a test suite rather than four
 * lines in the radio driver: it is where auto-repeat, stuck keys and ghosted
 * characters come from, and none of those are debuggable on the device.
 *
 * Output is the same byte alphabet kernel/drv/keyboard.c produces, so a
 * Bluetooth keyboard and the built-in one are indistinguishable upstream.
 */
#ifndef CARDOS_KBD_HID_H
#define CARDOS_KBD_HID_H

#include <stdint.h>
#include <stddef.h>

#define KBD_HID_REPORT_LEN 8
#define KBD_HID_MAX_KEYS   6

/* Modifier bits, as the HID spec orders them. */
#define KBD_MOD_LCTRL  0x01
#define KBD_MOD_LSHIFT 0x02
#define KBD_MOD_LALT   0x04
#define KBD_MOD_LGUI   0x08
#define KBD_MOD_RCTRL  0x10
#define KBD_MOD_RSHIFT 0x20
#define KBD_MOD_RALT   0x40
#define KBD_MOD_RGUI   0x80

typedef struct {
  uint8_t  held[KBD_HID_MAX_KEYS];   /* usage codes down at the last report */
  uint8_t  nheld;

  /* Auto-repeat is generated here rather than by the keyboard: a boot-protocol
   * keyboard just keeps saying the key is still down. */
  uint8_t  repeat_code;
  uint8_t  repeat_mods;
  uint32_t repeat_at_ms;
} KbdHid;

void kbd_hid_init(KbdHid *k);

/* Decode one report. Newly pressed keys are written to `out` as CardOS key
 * bytes; returns how many, or -1 if the report is not a boot keyboard report.
 * `now_ms` seeds auto-repeat.
 *
 * A report where every slot is 0x01 is a rollover error -- more keys down than
 * the device can report -- and yields nothing rather than six garbage keys. */
int kbd_hid_decode(KbdHid *k, const uint8_t *report, size_t len,
                   uint32_t now_ms, uint8_t *out, int max_out);

/* Characters due from holding a key down. Call once per main-loop pass. */
int kbd_hid_repeat(KbdHid *k, uint32_t now_ms, uint8_t *out, int max_out);

/* One usage code to a CardOS key byte, or 0 for keys with no character. */
uint8_t kbd_hid_translate(uint8_t usage, uint8_t mods);

#define KBD_REPEAT_DELAY_MS 400
#define KBD_REPEAT_RATE_MS  60

#endif /* CARDOS_KBD_HID_H */
