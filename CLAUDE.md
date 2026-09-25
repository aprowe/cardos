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
  Subdirectories of `/apps` are folders (one level); Enter goes in, Escape
  comes out. `k` then a letter binds `Opt+letter` to the highlighted app;
  `hotkey` in the console does the same. The table is `/config/hotkeys.txt`
  on the card, one `t=Todo` line per binding, hand-editable; it starts empty
  (`kernel/sys/hotkeys.c`, host-tested).
- **desktop** (`desk`) -- windows, taskbar, Start menu, pointer. Proof the
  window system works more than a daily driver.
- **console** -- the development shell. `help` lists it.

Apps come in two kinds. Built-ins are now only **Memory, About and Settings** —
what cannot sensibly be loaded, being reports on the kernel that would load
them. Everything else, Files included, is a `.capp`: ELF, linked complete,
relocated at load.
They live in `/apps` on the card, carry their own name and 16x16 icon, and
are embedded in the firmware so first boot writes them out. See
`kernel/app/elfload.h` for why the loader is short, and `apps/capp.ld` for the
one hardware fact that shapes all of it.

The console runs apps like a shell would: `grep TODO /apps` finds
grep.capp on PATH, `./grep` runs one by path, `run NAME args` is the explicit
form, and tab completes commands and paths. PATH and a few other variables live
in NVS -- see `env` and `set`. The launcher's app list is searched last, after
PATH, so `edit` works even when nothing on PATH is called that.

**Claude runs on the device**, in the only sense it can: `apps/claude.c` is a
terminal, and the CardOS server — one process, one port, the same one that
renders web pages — hands what you type to Claude Code running **in this
repository, with permission to edit it**. Asking the device to change an app
changes the source on the PC. Three short calls rather than one long one
(`POST /chat` → id, `GET /chat?id=N` → pending or the answer, `GET /chat/new`),
because the shell is a single cooperative loop and a two-minute request is a
frozen machine; the device polls from its tick handler and stays alive
throughout. Conversation state is one resumed Claude Code session.

The obvious warning applies and the server prints it: anything that can reach
that port can edit this folder. `--token SECRET` requires a shared string,
which the device reads from `/config/claude.token` on its card and sends as a bearer
token. `python -m server.tests.test_chat` exercises the protocol against a
stubbed agent.

**The server is `server/`, not a tool** (2026-09-22). It was
`tools/webproxy.py`, which had stopped being a proxy long ago: one handler
class held the routes for six services and the renderer's pixel pipeline.
`python -m server` runs it. `server/app.py` is only the HTTP layer --
dispatch, the token, body limits; each service module declares its own
`ROUTES` (`chat.py`, `updates.py`, `voice.py`, `shots.py`, `screen.py`,
`render/render.py`), and `/` lists them. `server/build.py` is the droplet's
build-after-turn (`--build --store`); tests are `server/tests/`. `tools/` is
dev-time scripts only. A Build request is **planned, then run a step at a
time** (`server/chat.py`): a quick no-tools call splits it into at most six
steps, each is its own resumed turn, and each is streamed, so a poll answers
`pending` plus "step 2/3: writing timer.c" and a turn is stopped when it goes
quiet for 240 s rather than at a wall clock -- the old 300 s limit killed a
new-app request that was still thinking. The device still calls it the proxy (`env PROXY`,
`CAPP_PROXY_DEFAULT`), because renaming a setting stored on every device is
not worth a word.

There is a web browser, of a sort. The device has no HTML parser and no layout
engine; the server's `/render` (`server/render/`) drives headless Chrome over
the DevTools protocol at a **240px viewport**, re-typesets the page in
**CardOS's own 6x8 font** (built into a TTF by `server/render/pixelfont.py`
from the same table the console draws from,
so text lands on the pixel grid with nothing to antialias), and ships RLE'd
RGB565 rows. Rendering wide and scaling down was tried first and is still there
behind `?px=0`; it turns body text into grey mush, which is the whole reason for
the font. The device decodes one row at a time and blits it, holding 480 bytes
rather than a page. See `apps/web.c`.

Also working: BLE mouse and keyboard (two links at once), WiFi with an HTTPS
client apps can call, SD card, and chain-booting third-party firmware with a
one-shot rollback home.

**The device updates itself from the PC.** `update` in the console asks
the server for its manifest (`/update`: the firmware's ELF SHA and an FNV
hash per `.capp`), says what differs, and `update apps|os|all` installs it.
The Claude terminal checks after every answer and offers `/update`. Apps land
in `/desktop`; the firmware goes to the card and then through the same
`launcher.c` path guests use, into whichever OTA slot is not running.
`factory` is USB-only and is where a bad update rolls back to; if a USB flash
puts a newer build in `factory`, boot notices and switches to it. Design in
`docs/superpowers/specs/2026-09-11-remote-update-design.md`.

**There are two firmware builds, debug and release** (2026-09-25). The
`cardputer` env is debug (-Og, INFO logs); it is `default_envs`, so a plain
`platformio run` or a USB upload is still debug. `-e release` is -O2 with
WARN logs: `sdkconfig.release.defaults` layered on `sdkconfig.defaults`,
generated into `sdkconfig.release`, and `-DCARDOS_RELEASE`. At -O2,
`format-truncation` and `stringop-truncation` stay warnings, not errors,
because every bounded `snprintf` in the tree trips them. The server offers
both (`/update?flavor=debug|release`; the store's debug image keeps the name
`firmware.bin`, the release one is `firmware-release.bin`). A device updates
to the flavor it is running (`update_flavor()`; `bootinfo` and the boot
banner say which), and `update os|all debug` or `... release` switches.
Firmware older than this asks for no flavor and is given release. Build on
the droplet builds and publishes both. A laptop server offers release only
after `-e release` has been built there. The partition
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

**The card is a network drive when you ask.** `share` in the console, or the
Share app, runs a WebDAV server on port 80 (`kernel/net/share.c` is the
socket and task; `kernel/net/dav.c` is the protocol and is host-tested).
Windows maps `http://IP/` as a drive letter, Finder connects to it. No
password: the boundary is the LAN, so it is a thing you turn on. Design in
`docs/superpowers/specs/2026-09-14-webdav-share-design.md`.

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

**Boot does not load the apps any more** (2026-09-25). The icon scan used to
load every `.capp` (read it, relocate it, free it) to learn each app's name,
icon, flags and commands, and it ran twice on a boot into the launcher, since
`main.c` and `launchui_init` both scanned. Now only the shell scans, and
`/cache/apps.idx` (`kernel/app/appidx.c`, host-tested) remembers what each
file said, keyed by path, size and date. The whole file is keyed by the API
version and `CAPP_BLOB_STAMP`, which `build_apps.py` works out so boot no
longer hashes the blobs. **Any change under `/apps` deletes the index**:
`fs_on_change` in `fs_fat.c` tells `capprun`. That only covers writes that
go through `fs_*`, so anything that writes the card any other way must delete
it too. Seeding lists each directory once instead of opening every file. The
safe-mode check waits 60 ms, not 400. The CPU runs at 240 MHz rather than 160;
power management is off, so that is also the idle clock.

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

**Voice memos, and a speaker** (2026-09-20). `kernel/drv/speaker.c` is the
first speaker driver: I2S standard mode to the NS4168 on BCLK 41 / DATA 42 /
LRCLK 43, streaming a PCM WAV (16-bit, mono or stereo, 8-48 kHz, chunk
walk rather than a fixed header) from the card a block at a time, with a
volume in 8.8 fixed point. It closes the mic before taking the pins -- G43
is both the mic's clock and LRCLK, which mic.h always said. `kernel/sys/audio.c`
runs a recording or a playback on a task and an app polls state, level and
position from tick; `api->audio()` (API 28) is that behind a table. The
mic's ceiling is ten minutes now (`MIC_HARD_MAX_MS`); the voice button still
asks for fifteen seconds. `apps/memo.c` records into `/home/memos` (named by
the clock, `0920-1142.wav`, or by count when there is no clock), lists with
lengths, plays back with a bar, deletes after asking. The strip with the
meter is drawn in place and repainted only when it would look different, at
most fifteen times a second -- clearing and redrawing it every tick was a
visible flicker.

**The mic can listen without a file** (2026-09-25). `record(NULL, ms)` runs
the mic with `level()` moving and nothing written, where a file would be
32 KB a second of card wear for a meter or a toy. Older firmware refuses a
NULL path with -2, and an app should fall back to a file. Noodle
(`apps/noodle.c`, Games) is the first user. It is the car-lot tube man: blow
on the mic and he inflates and flails, stop and he folds over. He is drawn in
15-row strips into a buffer and blitted, redrawing only where he moved.
`NOODLE_DUMP=dir build\cardos_tests.exe` writes frames to look at, which is
how the physics was tuned. Apps are not told when they are closed, so it
records in 8 s pieces restarted from tick: leaving the app leaves the mic on
for at most that long.

**Edit's preview is set in Atkinson Hyperlegible and word-wraps** (2026-09-25),
using `ui13`/`ui13b`, with code still in 6x8. It used to cut a long line at the
screen edge and never draw the rest. Rows break at the last space that fits, a
word too long for a row breaks inside, and list items and quotes hang under
their text. `test/test_edit_preview.c` checks it against a font of uneven
widths.

**There is an OS file picker** (2026-09-20). `api->pick(&req)` puts a
dialog over the app -- open a file, save (a name field, asks before
replacing), or choose a folder (a "use this folder" row) -- and the app
polls `api->pick_poll` from tick, the shape `http_start`/`http_poll` has and
for the same reason. In every mode ctrl-n makes a folder, ctrl-r renames,
delete deletes (asks; refuses a full folder), ctrl-a shows dotfiles, typing
jumps to a name, backspace goes up, escape cancels. The behaviour is
`kernel/ui/pickmodel.c` (portable, 23 host tests over a fake card);
`kernel/ui/picker.c` paints it and gives it the card; both shells route
keys, clicks and paint to it while it is up and repaint themselves when it
closes. Edit was the first user: its own browser and name prompt (about 150
lines) are gone, ctrl-o and ctrl-r go through the picker, and an Edit with
no file opens the picker at once. API 27.

**Held keys repeat** (2026-09-20). Both keyboards: 400 ms, then every
60 ms, for letters, digits, punctuation, space, backspace, delete, tab and
the arrows -- never enter, escape, fn-` or any opt/fn chord. The policy and
the timer are `kernel/input/keyrepeat.c` (host-tested); the matrix driver
re-translates the held position on each repeat so a modifier pressed
mid-hold takes effect, and the Bluetooth decoder, which already repeated,
now asks the same policy (it used to repeat fn-w). The shell records
whether the key it is delivering is a repeat (`input_set_repeat`), and two
things read that: `capprun` drops repeats for an app whose flags include
`CAPP_NO_REPEAT` (Pinball -- a flicked flipper must not become a held one),
and `api->key_repeat()` (API 26) answers inside a key handler, which is how
Todo lets a held arrow keep moving while a held space toggles once.

**There is a printer, and fn-p prints** (2026-09-20). A Bluetooth thermal
printer (an X6h "cat printer", the TinyPrint one) is driven straight from
the device: `kernel/drv/btprint.c` is the NimBLE client beside the HID one,
`kernel/sys/printdoc.c` is the wire format and a renderer for a tiny
markdown-shaped markup (`# heading`, `[ ] task`, `---`, text; nothing under
2x of the 6x8 font, since 1x is unreadable on paper), and
`kernel/sys/printq.c` runs one job at a time on its own task. `api->print(doc)`
and `api->print_status()` are API 25. **fn-p is the print chord in every app
that can**: an action with key `CAPP_KEY_PRINT` in the table is all it takes,
and the menu and help show it. Todo and Edit print; the console has `print
scan`, `print use N`, `print test`, `print FILE`. The printer's address is in
`/config/printer.txt`. The desktop's keyboard-mouse toggle moved to fn-k. The
job takes a row source, so a pre-rendered bitmap is the next step, not a
rewrite. Design and the measured numbers in
`docs/specs/2026-09-20-print-design.md`; the protocol notes and the PC probe
that verified it are in `~/Projects/printer`.

**Pictures go to paper as `%%` lines** (2026-09-24). A print document may
carry pixel rows: `%%N*runs=base64` -- N identical rows, run lengths
alternating white/black from x = 0, then optionally raw pixels, six to a
base64 digit (`kernel/sys/printdoc.h`). No API change: it is still
`api->print`. A writer picks runs or raw per row, whichever is shorter --
runs alone put one graph of `4sin(5x)` at 20 KB, and the kernel copies the
whole document. A line that only starts with `%%` is text, as before.
Needs the firmware that knows it: on older firmware the lines print as text.

**Calc is a graphing calculator** (`apps/calc.c`, 2026-09-24). A roll of
sums (`ans`, `a=5`, implicit multiplication, sin..round, `n!`), and any
line that is `y=...` or mentions x becomes one of four graphs: Tab shows
them, arrows pan, + - zoom, f fits, t traces. fn-p prints the view on
screen, the graph as a `%%` picture. Single-precision float, shown to seven
digits, because that is the hardware -- and an app links no libgcc, so
Calc carries its own `__divsf3` (`a / b` compiles to a call) and its own
libm. Struct assignment compiles to `memcpy`, which is not there either;
use `api->mem_cpy`. `test/test_calc.c` checks the maths against the
host's libm.

**The toolbar is a keyboard menu too** (2026-09-18). `fn-b` shows the bar and
puts the keyboard in it: left/right walk the menu names, down opens one,
up/down walk its items, Enter runs the highlighted one as an action, Escape
steps out a level at a time and `fn-b` hides it again. It still appears on its
own only when a mouse moves. While the bar has the keyboard `toolbar_key`
answers for *every* key and `toolbar_has_keys()` makes `wants_text` say no --
a key that fell through typed into the app underneath an open menu. An app
wires it with one call at the top of its key handler (`menu_key` in
`apps/todo.c` is the pattern); the menus still come from the same CappAction
table as the chords and the help panel, so they cannot disagree.

**The clock is UTC until `TZ` is set, and every app reads it** (2026-09-18).
NTP hands over UTC, `kernel/sys/clock.c` applies `env TZ` (default `UTC0`),
and `api->now()` minus `api->epoch()` is how an app gets a local offset
without a zone database. With TZ unset the offset is zero and everything --
the clock, the calendar, the taskbar -- is UTC, which looks exactly like a
clock that is simply wrong: an event at 17:30 read as 00:30 the next day.
`set TZ=PST8PDT,M3.2.0,M11.1.0` fixes it and persists in NVS. Two things made
it hard to find, both now fixed: the zone was applied only at boot and at each
NTP sync, so `set TZ=...` listed the new value and changed nothing until a
reboot; and `time` printed an hour with no indication of which zone it was in.
`time` now names the zone and says when there is none.

**Escape never leaves anything; fn-` does** (2026-09-20). Escape is
interior: it goes to the focused app first (a subview goes back a level), and
outside an app it closes an open folder or menu. That is all. An app that
declines it at its top level keeps it -- the 2026-09-18 version left the app
when it declined, which is the same surprise one level down: back out of a
subview once too often and the app is gone. `fn` + the ` key (`KEY_QUIT`,
`KBD_KEY_QUIT` over Bluetooth), or **opt-backspace** (alt-backspace over
Bluetooth), which is the same code from the other corner of the keyboard, is
the one way out: of an app in the launcher,
of a fullscreen app or a focused window on the desktop, and of the launcher to
the console. `CAPP_KEY_ESC` in `capp.h` is the constant an app matches.

**The help panel is fn-h, and for a while it was nothing at all.** Help moved
off ctrl-h because ctrl-h is 0x08, the byte Backspace sends. The header and
the shells were updated; the key code was not, so both drivers emitted the
fn-letter code while every shell waited for the old dedicated 0x86 and no key
on either keyboard opened help. `KEY_HELP` is now defined as the fn-h chord
itself. If a chord ever stops working, check that the code the driver emits
is the code the shell matches -- a test in `test_kbd_hid.c` pins both.

**Apps log to the card.** `api->log()` reaches `/cache/app.log` (tagged with
the calling app, stamped with uptime, rotated at 32 KB to `app.log.1`) as well
as the serial port, because the USB port is not attached when the device is in
a pocket and that is when the intermittent fault happens. `log`, `log N` and
`log clear` in the console read it. The format and the rotation rule are in
`kernel/sys/logring.c` so the host suite can reach them; `kernel/sys/applog.c`
is the device glue. Log a handful of lines per sync, never one per tick -- it
is an SD card.

**Todo syncs once, at open** (2026-09-18). One sweep: the lists, then the
list on screen (pending edits pushed first, then pulled), then every other
list straight into its own cache file. Then it stops -- `s` asks for another.
It used to re-pull the current list every ten minutes, which rearranged the
screen while you were reading it and still never fetched the lists you were
not looking at. A list off screen is absorbed into its file rather than into
the item array, because the array is what is on display. `o` is the overview:
every list's open tasks under its own heading, read from those files. `d`
toggles a pending delete rather than only setting it.

**`fields=` is not an optimisation, it is why the reply fits.** Todo asked
Google for whole task objects -- etag, selfLink, position, updated, three
hundred bytes each -- against a 6 KB app buffer and the kernel's 8 KB. The
HTTP layer fills the buffer, cannot tell a body that ended from one that was
cut off, and the app parses as far as the cut: a long list came back short
with nothing saying so. Both Todo URLs now carry a field mask, as
`apps/calendar.c` always did. Any new Google call needs one.

**Todo has lists** (2026-09-17). All of the account's task lists, one at a
time: only the current list's tasks are in memory, each list caches to
`/todo/<id>.cache`, and `/todo/lists` remembers the names and the choice so
Left/Right work offline. `l` is the picker. Switching mid-pull drops that
reply; switching mid-push waits, because a dropped push reply means the item
is sent twice. Lists are made and named elsewhere, on purpose.

**One HTTP request at a time, and it has an owner.** `kernel/net/httpq.c`
runs a single request off the shell's loop; `api->http_start` refuses a
second. Every request is owned by the app slot whose handler started it, and
`capprun.c` disowns it when that slot is released, so a reply that lands after
the user has left the app is dropped. Before that, it sat in the slot forever:
leave Todo or Calendar within the two seconds its sync takes and neither could
sync again until reboot, with nothing on screen to say why (2026-09-17,
`test/test_httpslot.c` pins the sequence). The second half of that bug: the
trampolines in `capprun.c` nest. A tick that fetches a Google token blocks in
`http_request`, whose busy badge repaints the shell on the way out, and that
paint used to clear the active slot for the rest of the tick — so `damage()`
marks were dropped and the request had no owner. Apps now say "busy" when a
start is refused instead of returning silently.

**API version 32** (`CAPP_API_VERSION` in `capp.h` is the truth; this
paragraph is history). It moved six times in one day — 11 to 17 — and has
kept moving since; each move means every `.capp` must be rebuilt, because the
loader refuses a binary built against a different table. `python
tools/build_apps.py` before every firmware build; the symptom of forgetting is
"built for a different API version" at boot. What arrived: `tick` and `mouse`
(12, 13), `update_check`/`update_apply` (14), `caps_ok` (15), the file
operations `list_ex`, `stat`, `mkdir`, `remove`, `rename` and `run` that the
file manager needed (16), `damage`/`paint_area` (17), then actions,
`http_stream`, `key_pending`, `now`, the `exec_*` memory the IDE compiles into
and the `http_start`/`http_poll` pair (18 to 22), the agent table (23), and
`share_start`/`share_stop`/`share_status`/`share_take_log` (24 — the share
branch and master both called themselves 23, so the merge bumped it), and
`print`/`print_status` (25), `key_repeat` (26), `pick`/`pick_poll` (27), `audio` (28), and `proxy` (29) -- the address the kernel resolved, because Build, Web and Screen had been talking to the compiled-in laptop while the OS talked to `env PROXY`. Then commands (30): `CappParam`, and `about`/`params`/`cmd` at the end of `CappAction`, `commands` on `CappInfo`, `command` on `CappUi`, `headless`/`command_done` -- see `docs/superpowers/specs/2026-09-23-app-commands-design.md`. Then fonts (31): `font_load`, `font_free`, `text_font`, `text_width`, `font_height`, `print_fonts`. Then the screen (32): `keep_awake`, `wake`.

**Fonts are files an app asks for** (2026-09-23). The 6x8 console font is
still compiled in and still the default; anything nicer is a `.cfnt` in
`/fonts` on the card, which an app loads by name (`api->font_load("clock56")`)
and draws with `api->text_font`. A `.cfnt` is a bitmap font rendered on the PC
at one size -- the device has no TrueType rasteriser and should not grow one
-- by `tools/make_cfnt.py` from a TTF in `fonts/src` (open-licence only; the
OFL texts are beside them). **Fonts are made on request:** an app that wants a
face or a size adds a line to `fonts/fonts.txt` and runs `python
tools/make_cfnt.py --all`, which writes `fonts/NAME.cfnt` and
`kernel/ui/font_blobs.h`; the firmware seeds `/fonts` from that like the
colour icons. Screen fonts are 4-bit coverage, blended against the `bg` the
app passes (so `text_font` fills its line and needs no clear first); print
fonts are 1-bit. `kernel/ui/cfont.c` reads the format and is host-tested,
including every way a file can lie about where its bitmaps are;
`kernel/ui/fontres.c` loads them, owned by the app (freed when capprun
releases it) or by a print job. `api->print_fonts(doc, body, bold, head)` sets
a print in fonts -- Edit prints in Atkinson Hyperlegible -- and
`printdoc.c`'s output with no fonts is pinned row for row to the 6x8 one.
Timer's digits are `clock56` (Space Mono Bold, digits only, every digit the
same width). No shared libraries were needed: a font is data, and the one
renderer is in the kernel behind the API table.

**Every setting a person chose survives a reflash** (2026-09-24). WiFi,
Google and `env` have their own mirrors (below); everything else small --
brightness, volume, the boot shell, Bluetooth at boot, desktop autostart,
launcher pins, the screen timeouts -- is mirrored to `/config/settings.txt`
by `kernel/sys/prefs.c`, one `key=value` a line, rewritten from NVS after
each change and restored at boot for any key NVS has lost (NVS wins where
both have one). **A new NVS setting goes in the table in `prefs.c` and its
setter calls `prefs_mirror()`**; only a true cache (the last known time)
stays out. Values escape newlines (`kernel/sys/kvtext.c`, host-tested) --
the pins are names one a line, and unescaped they restored as one pin.
`defaults` deletes the file too, or the next boot would undo it.

**The screen's timeouts are a setting, and apps can hold it on** (2026-09-24).
Settings > Display: "Dim after" (15 s .. 5 min, never) and "Screen off"
(1 .. 30 min, never), `dim_s`/`off_s` in prefs. `api->keep_awake(1)` holds
the backlight on while it matters -- Timer while it counts and rings,
Clock's `k` for a nightstand -- and is let go when the app closes
(`power_release_owner` in capprun); `api->wake()` lights it now, as a key
would, without being a key. `kernel/sys/power.c`.

**Alarms ring whatever is open** (2026-09-24). `/config/alarms.txt`, one
alarm a line (`on 07:00 -MTWTF- Work`, `once`, `snooze`; the format and the
rules are `kernel/sys/alarmfmt.h`, header-only and libc-free so the kernel
and the Clock app share it, host-tested). `kernel/sys/alarm.c` reads it when
the minute changes and rings the due one as a panel over the current app --
which is not closed or told -- beeping, holding the screen on, until a key:
`s` snoozes nine minutes, anything else stops it; unanswered it stops after
five. A `once` switches itself off after ringing, a snooze deletes itself.
The Clock app (`apps/clock.c`) is the face and the editor; it re-reads the
file before every change and finds its alarm by content, never position,
so it cannot undo what the kernel changed. Commands: `do clock add 7:30
wake up`, `list`, `next`, `off 7:30`. The big numbers everywhere are Inter
Bold now (`clock56`, `num30`); `make_cfnt.py --style Bold` picks a variable
font's weight.

**Credentials survive a reflash** (2026-09-19). WiFi and Google credentials
live in NVS at runtime, and a full-table flash wipes NVS -- one day it cost the
network, the hotkeys and the Google login in a row. Each is now mirrored to a
file on the card, written whenever it is set: `/config/wifi.txt` (SSID, then
password) and `/config/google.txt` (client id, secret, refresh token), one
value per line. At boot, after the card mounts, a file is read back only when
NVS has nothing, so NVS wins where both exist and `wifi forget` / `google
forget` delete the file too. A hand-written `/config/wifi.txt` is also the
way to give a fresh device its network without a keyboard. `kernel/sys/conf.c`
holds the line format (host-tested); `conf_file.c` is the fs glue.

**Google sign-in is on the dashboard now** (2026-09-24).
`https://cardos.arowe.net/dash` (`server/dash.py`; the password is
`DASH_PASSWORD` in `/etc/cardos/env`, and the server still needs `--token`) signs in with a Google *Web* client, and the device runs
`google pull`, which fetches client id, secret and refresh token from
`/google/creds` over HTTPS with its bearer, `env DASH` overriding the address.
Only `/dash*` and `/google/creds` are exposed on that name;
`tools/deploy_droplet.sh dash` sets up nginx, the certificate and the client.
`tools/google_auth.py` remains the offline fallback. Refresh tokens expiring
after 7 days is the consent screen being in Testing, not a bug here -- publish
it. Design in `docs/superpowers/specs/2026-09-24-dashboard-google-design.md`.

**"Google: not configured" came back over and over** (2026-09-20), and the
cause was the push, not the device. `tools/google_auth.py` typed the three
`google` commands with a fixed 1.5 s wait after each -- but the console takes
one character per pass of its loop, so the hundred-character refresh token
is still being read when the wait ends, and the tool declared the push
failed, printed nothing useful, and was run again, and again. (It also
reached the console by Escape, which was never reliable and now does
nothing.) The tool now checks for a prompt before typing, waits for the
prompt after each command, sends opt-3 rather than Escape, and on failure
prints what the device said with the values redacted. Two more guards on the
device: boot writes `/config/google.txt` whenever NVS has credentials and
the card does not (a login older than the mirror had no file, so the day NVS
went it was gone for good), and a boot that erases NVS
(`ESP_ERR_NVS_NO_FREE_PAGES`, or a version change) says so in red and puts a
line in `log`. `mem` shows NVS entries used; 142 of 630 after a WiFi join.
Opening the port with pyserial does not reset this board (checked), so that
was not it.

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
- `python tools/make_cfnt.py --all` rebuilds the loadable fonts from
  `fonts/fonts.txt`; `--show out.png --text 12:34` renders a `.cfnt` back to
  check it.
- `python tools/make_font.py` regenerates the 6x8 font from pictures.
  `--show` renders the table back out; edit the pictures, never the hex.
- `python tools/mapsize.py` says where the flash and RAM went, per component.
- PlatformIO is installed under the user Python: **`python -m platformio`**
  (there is no `pio` on PATH). `python -m platformio run -t upload --upload-port COM3`
  flashes the debug build; add `-e release` for the release one.
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

- **A new app needs two things besides its code**, or `tools/build_apps.py`
  refuses it: a line in `apps/folders.txt` (`name Tools`, `Games`, `Net`, or
  `-` for a CLI app at the top level), and a non-blank 16x16 icon in its
  `capp_info` (32 bytes, 1bpp, two bytes a row, bit 7 leftmost; `apps/timer.c`
  has one). And a colour icon under the app's name in
  `tools/make_color_icons.py` (then run it): the launcher shows that one, and
  without it the app is a blank page. The folder also reaches the `/update` manifest, so a first install
  lands in it rather than at the top level, which is where every app Build
  made used to end up.

- **An app's own files are saved through `apps/safefile.h`**: write
  `NAME.tmp`, remove `NAME`, rename (the card's rename will not replace a
  file), and `safe_open_read` puts back a `.tmp` a power cut stranded. A
  device switched off by being pulled from a pocket is the normal case, and
  rewriting in place loses the file in that window. Counter and Habits use
  it; their host tests (`test/test_counter.c`, `test/test_habits.c`) show
  how to test an app's storage against files on the PC.

- **Nothing that talks HTTPS runs on `cardos-bg`.** Its stack is 4 KB
  (`BG_STACK`, `kernel/sys/bg.c`) and a TLS handshake does not fit. A Build
  turn (2026-09-23) added a timezone lookup to `https://ipapi.co` there, right
  after NTP; the device then overflowed that stack ~15 s after every boot --
  a restart loop that also made `update os` impossible, since a 2 MB download
  outlasts 15 s. Network requests go through `kernel/net/httpq.c` (its own
  6 KB task) or run on the shell with the busy badge. Anything new on
  `cardos-bg` should be measured with `uxTaskGetStackHighWaterMark` first.
- **A kernel change can take the device off the air**, and then only USB
  brings it back. Build on the droplet can make one; before asking for
  `update os` after a Build turn touched `kernel/` or `src/`, run the
  firmware somewhere it can be watched.

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
