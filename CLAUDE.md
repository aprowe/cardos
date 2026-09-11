# CardOS — project context

A tiny custom OS for the M5Stack Cardputer. **Read
`docs/specs/2026-09-10-kernel-core-design.md` first** — it is the approved
design for the current sub-project (the kernel core) and records every
decision already made and why.

CardOS is standalone: no PC dependency at runtime, no Lua, no Arduino layer.
Apps will be written in a custom language compiled **on the device** to native
Xtensa machine code (sub-project 3). The current sub-project is the kernel
core, and its MVP is a working shell.

## Where things stand

The kernel core works on hardware. Three shells over it, all switchable:

- **launcher** (`launch`) -- fullscreen icon grid, apps run fullscreen, Escape
  leaves. Boots into this, because it is the one that is actually useful.
- **desktop** (`desk`) -- windows, taskbar, Start menu, pointer. Proof the
  window system works more than a daily driver.
- **console** -- the development shell. `help` lists it.

Apps come in two kinds. Built-ins (Files, Memory, About, Settings) are compiled
in. Loadable apps are `.capp` files: ELF, linked complete, relocated at load.
They live in `/desktop` on the card, carry their own name and 16x16 icon, and
are embedded in the firmware so first boot writes them out. See
`kernel/app/elfload.h` for why the loader is short, and `apps/capp.ld` for the
one hardware fact that shapes all of it.

The console runs apps like a shell would: `grep TODO /desktop` finds
grep.capp on PATH, `./grep` runs one by path, `run NAME args` is the explicit
form, and tab completes commands and paths. PATH and a few other variables live
in NVS -- see `env` and `set`. The launcher's app list is searched last, after
PATH, so `edit` works even when nothing on PATH is called that.

There is a web browser, of a sort. The device has no HTML parser and no layout
engine; `tools/webproxy.py` renders a page with headless Chrome at a **240px
viewport** -- so sites serve their narrowest mobile layout and render text at
that size rather than being shrunk into mush -- and ships RLE'd RGB565 rows.
The device decodes one row at a time and blits it, holding 480 bytes rather
than a page. See `apps/web.c`.

Also working: BLE mouse and keyboard (two links at once), WiFi with an HTTPS
client apps can call, SD card, and chain-booting third-party firmware with a
one-shot rollback home.

**Measured memory, with both radios up: 83 KB of heap free.** See
`tools/mapsize.py` and the `mem` command. Both numbers moved a lot during
bring-up -- do not trust any figure here that you have not re-measured.

## Hardware facts — measured on the actual device, not from a datasheet

M5Stack Cardputer v1.1, ESP32-S3FN8 (Xtensa LX7 dual-core, 240 MHz).

| | |
|---|---|
| Flash | 8 MB |
| SRAM | 512 KB, **no PSRAM** |
| Free heap after chip + display init | **322 KB** |
| Full-screen 240×135 16-bit canvas | **65 KB** |
| WiFi stack, once started | **53 KB** |
| RTC chip | **none** — time comes from NTP or the user |

Pin map (from M5Stack docs, confirmed working):

| | |
|---|---|
| Display ST7789V2 240×135 | BL 38, RST 33, RS 34, DAT 35, SCK 36, CS 37 |
| microSD (SPI) | CS 12, MOSI 14, MISO 39, SCK 40 |
| Keyboard | rows G7/G6/G5/G4/G3/G15/G13, columns via a 74HC138 decoder, 4×14 matrix |
| Mic SPM1423 (PDM) | DAT 46, CLK 43 |
| Speaker NS4168 (I²S) | BCLK 41, SDATA 42, LRCLK 43 |
| IR TX | 44 |

**G43 is shared** between the mic clock and the speaker's LRCLK — recording
and playback are mutually exclusive. Any audio API must say so rather than
failing silently.

The `` ` `` key (labelled ESC) is the universal escape. Arrow keys are the
`; , . /` keys.

## Toolchain

- **`python tools/build_apps.py` before the firmware build**, whenever anything
  in `apps/` or `kernel/app/capp.h` changed. It compiles each app, refuses any
  binary the on-device loader could not load, and regenerates
  `kernel/app/capp_blobs.h`, which is gitignored precisely so a stale copy
  cannot ship. Forgetting it means the firmware embeds apps built against a
  different API version, and the loader refuses them at boot with
  "built for a different API version".
- `python tools/make_font.py` regenerates the 6x8 font from pictures.
  `--show` renders the table back out; edit the pictures, never the hex.
- `python tools/mapsize.py` says where the flash and RAM went, per component.
- PlatformIO is installed under the user Python: **`python -m platformio`**
  (there is no `pio` on PATH). `python -m platformio run -t upload --upload-port COM3`.
- For CardOS use `framework = espidf`, **not** Arduino.
- The device enumerates as **COM3** (USB VID:PID 303A:1001).

## Hard-won lessons from the sibling project

`~/Projects/cardputer` (cardlet) is a separate, working project on the same
hardware — a Lua app runtime driven by Claude. Worth reading for anything
hardware-specific. What it cost to learn:

- **The USB-CDC serial port drops out constantly.** It disappears mid-session,
  and after a flash. Do not build a debugging strategy that depends on it.
  Cardlet ended up POSTing diagnostics over WiFi instead. For CardOS, get
  logging onto something durable early — the display, the SD card, or both.
- **`Serial` does not reach USB by default** on this board under Arduino:
  the board definition routes it to UART0 pins. Under ESP-IDF, check where
  the console is configured to go before concluding the firmware is dead.
- **Measure memory, never assume it.** Cardlet shipped a hardcoded Lua budget
  that was wrong in both directions and cost a long debugging session. Every
  memory number in the spec above was measured on this device. CardOS then made
  the same mistake in miniature: the memory manager's arena was fixed at 128 KB
  before either radio existed, and with WiFi and Bluetooth both up that left
  1156 bytes free and the WiFi driver failing buffer allocations in a loop. It
  is 48 KB now. Run `mem` after adding anything that allocates.
- **A firmware that seems dead is usually a serial problem**, not a crash —
  check whether the device is still doing its job by some other channel first.

## Working agreements

- Test-first for anything with logic. The memory manager, swap allocator and
  scheduler queue are deliberately written as portable C so they run under a
  host test suite on the PC — use it, because there is no debugger on the
  device and eviction bugs only appear under pressure.
- Measure before optimising, and put the measurement in the commit message.
- When something cannot work on this hardware, say so plainly and early
  rather than building a version that pretends.
- Watch for name collisions with ESP-IDF and its vendored stacks. Three so far:
  `console_write` (esp_stdio), `LINE_MAX` (picolibc), and `mem_init`/`mem_free`
  (lwIP, once WiFi was linked). The CardOS memory manager is `kmem_*` for that
  reason. A collision surfaces as a "multiple definition" that names neither
  module usefully.
