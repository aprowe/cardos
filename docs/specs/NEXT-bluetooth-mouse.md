# Sub-project 5: Bluetooth mouse — direction note

Not a full spec. Records the constraints while they are cheap to act on,
because one of them affects which mouse is worth owning.

## The constraint that matters most

**The ESP32-S3 has Bluetooth LE only. There is no Bluetooth Classic.**

The original ESP32 has both BR/EDR (Classic) and LE. The S3 dropped Classic. So
a mouse that speaks only Bluetooth Classic HID **cannot** be paired with the
Cardputer at all — not with more work, not with a different stack. It is a
radio capability the chip does not have.

What this means in practice:

- A mouse advertised as **Bluetooth Low Energy / BLE / Bluetooth Smart** will
  work.
- A mouse advertised as **Bluetooth 4.0+ / 5.x** usually means LE, but not
  always — some are dual-mode and some are Classic with a modern version
  number.
- A mouse whose only pairing mode is Classic HID will not work.
- Mice with a **USB dongle** (2.4 GHz proprietary, e.g. Logitech Unifying) are
  not Bluetooth at all and cannot work either — there is no USB host port.

Worth confirming against a specific mouse before assuming. If a candidate is
already to hand, the cheapest test is whether a phone sees it as a BLE device.

## Role: HID host, which is the less common direction

Almost every ESP32 Bluetooth-HID example is the *device* role — the ESP32
pretending to be a keyboard or mouse for a PC. This is the opposite: CardOS is
the **host** (BLE central), consuming reports from a mouse.

ESP-IDF supports it. The `esp_hid` component provides `esp_hidh`, a HID host
that speaks HID-over-GATT (HOGP) over BLE. That is the path, rather than
writing GATT client code by hand.

## Decisions to carry into the spec

| Decision | Choice | Why |
|---|---|---|
| Stack | **NimBLE**, not Bluedroid | Roughly 30 KB lighter, and CardOS has 322 KB total with no PSRAM. Bluedroid's extra features are all Classic-era. |
| Protocol | **Boot protocol** first | A boot-protocol mouse report is 3–4 fixed bytes. Report protocol means parsing an arbitrary HID report descriptor, which is a real parser for a marginal gain. Fall back to it only if a mouse refuses boot mode. |
| When the radio starts | **On demand, not at boot** | The stack costs tens of KB for the whole uptime. Measured WiFi on this board is 53 KB; BLE will be the same order. A `mouse on` command or a desktop setting, so the RAM is only spent when a mouse is actually wanted. |
| Bonding | Persist in NVS | Pair once. The `nvs` partition already exists. |

## Memory, which is the whole reason for care

The budget from the kernel spec: 322 KB free after chip and display init. A BLE
stack of roughly 40–50 KB is affordable on its own, but **WiFi plus BLE plus
the handle heap plus the window system is not**, and the sum needs measuring
rather than estimating — CLAUDE.md is emphatic on exactly this point, and the
sibling project was burned by a guessed budget.

The on-demand rule above is what keeps this tractable: nothing pays for the
radio until someone asks for a pointer.

## What is host-testable, and is already done

The plumbing needs hardware, but the logic does not, and the logic is where the
fiddly bugs are:

- **Decoding a boot-protocol report** — a buttons bitmap and *signed* 8-bit
  deltas. Getting the sign wrong gives a mouse that only moves down and right,
  which is the classic version of this bug.
- **Cursor tracking** — accumulating deltas and clamping to 240x135 without
  the cursor escaping or sticking at an edge.
- **Button edge detection** — click and release as events rather than levels,
  which is what a window system's dispatch actually needs.

That is `kernel/input/mouse.c`, written and tested now. When the radio work
happens, it only has to deliver bytes.

## Where this got to on hardware (2026-09-10)

Both a Logitech Pebble M350 and an MX Master 3S **pair, encrypt and bond**
successfully -- `ENC_CHANGE ... bonded=1` -- and CardOS reads their report
descriptors. Neither ever delivers an input report.

The failure is in ESP-IDF's `esp_hid` component, not in CardOS, and it is
identical on both devices. `attach_report_listeners` in `nimble_hidh.c` takes
`LOCK_OPS()`, then calls `write_char_descr`, which issues a GATT write and
blocks in `WAIT_CB()`. On both mice the log shows the write to the battery
CCCD being issued and then no further GATT activity at all -- it hangs on the
first descriptor write, before it ever reaches the HID report
characteristics. A completion callback that needs the same lock would do
exactly this.

There is a second, independent problem in the same function: it subscribes
only to reports tagged `ESP_HID_PROTOCOL_MODE_REPORT`. A Boot Mouse Input
characteristic (`0x2A33`) is tagged `PROTOCOL_MODE_BOOT` at discovery and
skipped, so a boot-protocol mouse would never be subscribed even without the
first bug.

Neither is fixable from outside the component. The way forward is a minimal
HOGP client written directly against NimBLE: connect, initiate security,
discover service `0x1812`, find the Report characteristics that support
notify, write their `0x2902` descriptors, and handle
`BLE_GAP_EVENT_NOTIFY_RX`. Perhaps 300 lines, entirely under our control, and
it feeds the decode and cursor logic that is already built and tested.

Worth recording that everything *around* the gap works: scanning finds both
mice by HID appearance (`0x03c2`), bonding persists, and the radio costs a
measured 70 KB.

## Open questions for the real spec

- Cursor rendering: the compositor must save the pixels under the cursor and
  restore them on move. At 240x135 with no back buffer, a cursor is a damage
  rectangle like everything else — see the window system note.
- Pointer acceleration: probably none. The screen is 240 pixels wide; a mouse
  crosses it in a couple of centimetres already.
- What happens on disconnect — does the cursor vanish, or freeze?
- Does the keyboard keep driving focus when a mouse is connected, or defer?
