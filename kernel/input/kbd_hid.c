/* HID boot-keyboard decoding. See kbd_hid.h. */

#include "kernel/input/kbd_hid.h"

/* Usage codes 0x04..0x38, which is every printable key on a boot keyboard, in
 * order. Two tables because shift is not a transformation of ASCII: shift-2 is
 * '@' on this layout and '"' on another, and there is no arithmetic that gets
 * from one to the other. US layout, which is what the codes assume. */
static const char PLAIN[] =
  "abcdefghijklmnopqrstuvwxyz"   /* 0x04..0x1D */
  "1234567890"                   /* 0x1E..0x27 */
  "\0\0\0\0"                     /* enter, escape, backspace, tab */
  " -=[]\\\0;'`,./";             /* 0x2C..0x38 */

static const char SHIFTED[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "!@#$%^&*()"
  "\0\0\0\0"
  " _+{}|\0:\"~<>?";

#define USAGE_FIRST 0x04
#define USAGE_LAST  0x38

void kbd_hid_init(KbdHid *k) {
  int i;
  if (!k) return;
  for (i = 0; i < KBD_HID_MAX_KEYS; i++) k->held[i] = 0;
  k->nheld = 0;
  k->repeat_code = 0;
  k->repeat_mods = 0;
  k->repeat_at_ms = 0;
}

uint8_t kbd_hid_translate(uint8_t usage, uint8_t mods) {
  int shift = (mods & (KBD_MOD_LSHIFT | KBD_MOD_RSHIFT)) != 0;
  int ctrl = (mods & (KBD_MOD_LCTRL | KBD_MOD_RCTRL)) != 0;
  /* Alt stands in for the Cardputer's Opt key: a Bluetooth keyboard has no
   * key by that name, and Alt is where a hand goes looking for one. */
  int opt = (mods & (KBD_MOD_LALT | KBD_MOD_RALT)) != 0;
  int fn  = (mods & (KBD_MOD_LGUI | KBD_MOD_RGUI)) != 0;

  /* Before the plain meanings below: Escape with the window modifier is
   * "leave the app", and the switch would otherwise have answered Escape
   * and never reached the fn block at all. */
  if (fn && usage == 0x29) return KBD_KEY_QUIT;

  switch (usage) {
  case 0x28: return 0x0D;        /* enter */
  case 0x29: return 0x1B;        /* escape */
  case 0x2A: return 0x08;        /* backspace */
  case 0x2B: return 0x09;        /* tab */
  case 0x4F: return 0x83;        /* right */
  case 0x50: return 0x82;        /* left */
  case 0x51: return 0x81;        /* down */
  case 0x52: return 0x80;        /* up */
  case 0x4C: return 0x7F;        /* delete */
  default: break;
  }

  /* The keypad, which a compact Bluetooth keyboard usually lacks but a
   * full-size one does not, and whose codes are nowhere near the main
   * block. */
  if (usage >= 0x59 && usage <= 0x61) return (uint8_t)('1' + (usage - 0x59));
  if (usage == 0x62) return '0';
  if (usage == 0x63) return '.';
  if (usage == 0x58) return 0x0D;          /* keypad enter */

  /* The global shortcuts, before anything else claims the key. */
  if (opt) {
    if (usage >= 0x04 && usage <= 0x1D) return KBD_KEY_OPT_LETTER('a' + (usage - 0x04));
    if (usage >= 0x1E && usage <= 0x26) return KBD_KEY_OPT_DIGIT(1 + (usage - 0x1E));
    if (usage == 0x27) return KBD_KEY_OPT_DIGIT(0);
    return 0;
  }

  /* The window modifier, before the app sees a letter. Same reasoning as opt
   * above: fn-w has to close a window while an editor is swallowing text. */
  if (fn) {
    if (usage >= 0x04 && usage <= 0x1D)
      return KBD_KEY_FN_LETTER('a' + (usage - 0x04));
    return 0;
  }

  if (usage < USAGE_FIRST || usage > USAGE_LAST) return 0;

  {
    char c = (shift ? SHIFTED : PLAIN)[usage - USAGE_FIRST];
    if (c == 0) return 0;
    /* Control characters, the same way the built-in keyboard makes them.
     * Ctrl belongs entirely to the app now, and an app that wants ctrl-S has
     * to receive the same byte from either keyboard. */
    if (ctrl) {
      /* No special case for ctrl-h any more. It used to become KBD_KEY_HELP
       * because ctrl-h and Backspace are both 0x08 -- but help is a window
       * operation and moved to fn-h, so ctrl-h is the app's like every other
       * ctrl chord, and the tie stopped needing to be broken. */
      if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 1);
      if (c >= 'A' && c <= 'Z') return (uint8_t)(c - 'A' + 1);
      return 0;
    }
    return (uint8_t)c;
  }
}

static int was_held(const KbdHid *k, uint8_t usage) {
  int i;
  for (i = 0; i < k->nheld; i++) if (k->held[i] == usage) return 1;
  return 0;
}

int kbd_hid_decode(KbdHid *k, const uint8_t *report, size_t len,
                   uint32_t now_ms, uint8_t *out, int max_out) {
  uint8_t mods;
  uint8_t now_held[KBD_HID_MAX_KEYS];
  int n_now = 0, produced = 0, i;
  const uint8_t *keys;

  if (!k || !report || !out || max_out <= 0) return -1;

  /* Some keyboards prefix a report ID. A boot report is eight bytes; nine
   * means the first is an ID, and the rest is the same shape. */
  if (len == KBD_HID_REPORT_LEN + 1) { report++; len--; }
  if (len < KBD_HID_REPORT_LEN) return -1;

  mods = report[0];
  keys = report + 2;

  /* Every slot 0x01 is ErrorRollOver: more keys are down than the device can
   * report, and none of the six are real. Emitting them would put six 'a's on
   * screen for leaning on the keyboard. */
  {
    int rollover = 1;
    for (i = 0; i < KBD_HID_MAX_KEYS; i++)
      if (keys[i] != 0x01) { rollover = 0; break; }
    if (rollover) return 0;
  }

  for (i = 0; i < KBD_HID_MAX_KEYS; i++) {
    uint8_t u = keys[i];
    if (u == 0 || u == 0x01) continue;
    now_held[n_now++] = u;

    if (was_held(k, u)) continue;            /* still down, not a new press */
    {
      uint8_t c = kbd_hid_translate(u, mods);
      if (c && produced < max_out) {
        out[produced++] = c;
        /* The most recent press is the one that repeats, which is what every
         * keyboard does and what makes holding a key while another is down
         * behave sensibly. */
        k->repeat_code = u;
        k->repeat_mods = mods;
        k->repeat_at_ms = now_ms + KBD_REPEAT_DELAY_MS;
      }
    }
  }

  /* The repeating key lifting cancels the repeat. Without this a report that
   * arrives late keeps a lifted key repeating forever. */
  if (k->repeat_code) {
    int still = 0;
    for (i = 0; i < n_now; i++) if (now_held[i] == k->repeat_code) still = 1;
    if (!still) k->repeat_code = 0;
  }

  for (i = 0; i < n_now; i++) k->held[i] = now_held[i];
  k->nheld = (uint8_t)n_now;
  return produced;
}

int kbd_hid_repeat(KbdHid *k, uint32_t now_ms, uint8_t *out, int max_out) {
  uint8_t c;

  if (!k || !out || max_out <= 0) return 0;
  if (!k->repeat_code) return 0;
  /* Signed difference, so the comparison survives the 32-bit millisecond
   * counter wrapping -- the same reason the scheduler compares deadlines this
   * way. */
  if ((int32_t)(now_ms - k->repeat_at_ms) < 0) return 0;

  c = kbd_hid_translate(k->repeat_code, k->repeat_mods);
  if (!c) { k->repeat_code = 0; return 0; }

  k->repeat_at_ms = now_ms + KBD_REPEAT_RATE_MS;
  out[0] = c;
  return 1;
}
