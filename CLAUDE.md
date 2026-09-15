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
  Subdirectories of `/desktop` are folders (one level); Enter goes in, Escape
  comes out. `k` then a letter binds `Opt+letter` to the highlighted app;
  `hotkey` in the console does the same, and the table is in NVS
  (`kernel/sys/hotkeys.c`, host-tested).
- **desktop** (`desk`) -- windows, taskbar, Start menu, pointer. Proof the
  window system works more than a daily driver.
- **console** -- the development shell. `help` lists it.

Apps come in two kinds. Built-ins are now only **Memory, About and Settings** —
what cannot sensibly be loaded, being reports on the kernel that would load
them. Everything else, Files included, is a `.capp`: ELF, linked complete,
relocated at load.
They live in `/desktop` on the card, carry their own name and 16x16 icon, and
are embedded in the firmware so first boot writes them out. See
`kernel/app/elfload.h` for why the loader is short, and `apps/capp.ld` for the
one hardware fact that shapes all of it.

The console runs apps like a shell would: `grep TODO /desktop` finds
grep.capp on PATH, `./grep` runs one by path, `run NAME args` is the explicit
form, and tab completes commands and paths. PATH and a few other variables live
in NVS -- see `env` and `set`. The launcher's app list is searched last, after
PATH, so `edit` works even when nothing on PATH is called that.

**Claude runs on the device**, in the only sense it can: `apps/claude.c` is a
terminal, and `tools/webproxy.py` — one process, one port, the same one that
renders web pages — hands what you type to Claude Code running **in this
repository, with permission to edit it**. Asking the device to change an app
changes the source on the PC. Three short calls rather than one long one
(`POST /chat` → id, `GET /chat?id=N` → pending or the answer, `GET /chat/new`),
because the shell is a single cooperative loop and a two-minute request is a
frozen machine; the device polls from its tick handler and stays alive
throughout. Conversation state is one resumed Claude Code session.

The obvious warning applies and the server prints it: anything that can reach
that port can edit this folder. `--token SECRET` requires a shared string,
which the device reads from `/claude.token` on its card and sends as a bearer
token. `python tools/test_chat.py` exercises the protocol against a stubbed
agent.

There is a web browser, of a sort. The device has no HTML parser and no layout
engine; `tools/webproxy.py` drives headless Chrome over the DevTools protocol at
a **240px viewport**, re-typesets the page in **CardOS's own 6x8 font** (built
into a TTF by `tools/pixelfont.py` from the same table the console draws from,
so text lands on the pixel grid with nothing to antialias), and ships RLE'd
RGB565 rows. Rendering wide and scaling down was tried first and is still there
behind `?px=0`; it turns body text into grey mush, which is the whole reason for
the font. The device decodes one row at a time and blits it, holding 480 bytes
rather than a page. See `apps/web.c`.

Also working: BLE mouse and keyboard (two links at once), WiFi with an HTTPS
client apps can call, SD card, and chain-booting third-party firmware with a
one-shot rollback home.

**The device updates itself from the PC.** `update` in the console asks
`webproxy.py` for its manifest (`/update`: the firmware's ELF SHA and an FNV
hash per `.capp`), says what differs, and `update apps|os|all` installs it.
The Claude terminal checks after every answer and offers `/update`. Apps land
in `/desktop`; the firmware goes to the card and then through the same
`launcher.c` path guests use, into whichever OTA slot is not running.
`factory` is USB-only and is where a bad update rolls back to; if a USB flash
puts a newer build in `factory`, boot notices and switches to it. Design in
`docs/superpowers/specs/2026-09-11-remote-update-design.md`. The partition
table changed for this (two OTA slots; `factory` is 2.75 MB and `spiffs`
700 KB since 2026-09-12, see the comments in `partitions.csv`), so the
first flash after it needs the whole table: `python -m platformio run -t
upload` does that, but old NVS contents (WiFi credentials, PATH) are gone.

**Voice, and where text comes from.** Hold the button on the top edge (G0) and
talk: the mic records to `/cache/voice.wav` (16 kHz mono, PDM on DAT 46 / CLK
43 — what whisper wants, so nothing resamples), posts it to the proxy, and
whisper.cpp turns it into words. If the words start with **"Carlos"** they are
a command: a second, stateless Claude session with a fixed prompt turns English
into one line of a seven-verb vocabulary (`open`, `shell`, `bright`, `wifi`,
`say`, `key`, `none`), which `kernel/sys/rpc.c` parses and validates before the
device does anything. An LLM choosing between seven verbs is useful; an LLM
handing a device a string to run is not, and that list is the difference.

Otherwise the words are typed. `kernel/sys/input.c` delivers them **as
keystrokes** down the path keys already take, so every app that handles typing
handles voice without containing the word: the matrix keyboard, a Bluetooth
keyboard and a spoken sentence are indistinguishable by the time an app sees
them. `wants_text()` — which already decided whether `; . , /` are arrows — is
what says whether anything is listening. `listen` in the console does the same
thing without the button. Design in
`docs/specs/2026-09-12-voice-and-capabilities.md`.

**Apps declare what they need**: `CAPP_NEEDS_NET`, `CAPP_NEEDS_PROXY`, or
nothing (Mines and Pinball work on a device that has never seen a network).
The OS joins WiFi and checks the proxy answers *before* `capp_main` runs, so
twenty seconds of joining belongs to "starting Web" rather than to "Web is
broken". It never refuses to start an app; `api->caps_ok()` says what was
found and the app explains itself in its own words.

Apps get five callbacks, not two: `paint`, `key`, `click`, plus `tick` (every
pass of the shell's loop, ~5 ms, return 1 to repaint — the only way anything
moves on its own) and `mouse` (position, held buttons, wheel; the pointer is
drawn over fullscreen apps by repainting the square it left).

**Redundant redrawing is an OS problem, not an app problem.** The shell used
to hand every app its whole rectangle as the clip on every event, so a
keypress redrew a screen — and three apps grew their own `expect_paint`
dirty-tracking to dodge it, each slightly different. `api->damage(rect)` marks
what actually changed; `capprun.c` unions the marks per app and both shells
clip the next paint to them (a windowed app's damage goes through `wm_damage`,
so anything stacked above still wins). `api->paint_area()` returns the clip, so
an app can skip expensive work outside it — and comparing it with the rect
paint was handed answers what `expect_paint` was guessing at. **An app that
marks nothing gets its whole rectangle exactly as before**, so this cost the
existing apps nothing; `apps/files.c` shows the pattern, and Mines, Claude and
Pinball can drop their hand-rolled versions whenever someone is in there.

**API version 23** (`CAPP_API_VERSION` in `capp.h` is the truth; this
paragraph is history). It moved six times in one day — 11 to 17 — and each
move means every `.capp` must be rebuilt, because the loader refuses a binary
built against a different table. `python tools/build_apps.py` before every
firmware build; the symptom of forgetting is "built for a different API
version" at boot. What arrived: `tick` and `mouse` (12, 13),
`update_check`/`update_apply` (14), `caps_ok` (15), the file operations
`list_ex`, `stat`, `mkdir`, `remove`, `rename` and `run` that the file manager
needed (16), `damage`/`paint_area` (17), then actions, `http_stream`,
`key_pending`, `now`, the `exec_*` memory the IDE compiles into and the
`http_start`/`http_poll` pair (18 to 22), and the agent table (23). **The
`worktree-webdav-share` branch also calls itself 23** with `share_*` in the
slots master gave to `agent`; whichever lands second must bump to 24.

**`CAPP_PROXY_DEFAULT` in `capp.h` is the one place the PC's address is
written.** Kernel and apps both include that header; the kernel prefers
`env PROXY` over it. It used to be spelled out in five files, which is a bug
waiting for the laptop's address to change.

**Flash is the constraint now, not RAM.** `factory` grew to 2.75 MB on
2026-09-12 after the image hit 93% of 1.75 MB; it is at about 66% now.
Half the image is radio and TLS (net80211 158 KB,
mbedTLS + PSA crypto 228 KB, Bluetooth 177 KB, lwIP 103 KB), which is not
shrinkable by writing tighter kernel code — CardOS's own code is about 178 KB.
The lever that is available: **204 KB of embedded `.capp` blobs**, which are
copies of files that also live on the card and can be fetched with `update
apps`. Keeping two or three seeded and dropping the rest returns ~150 KB. Run
`python tools/mapsize.py` before deciding anything about size.

**Measured memory, with both radios up: 120 KB of heap free**, low water 95 KB.
It was 79 KB until the memory manager's arena came down from 48 KB to 16 —
grep says nothing outside `kernel/mem` ever allocated from it, and holding a
third of the free heap for that was why WiFi and TLS could not both fit while
Bluetooth was connected. See `tools/mapsize.py` and the `mem` command. Every
number here moved during bring-up — do not trust one you have not re-measured.

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
