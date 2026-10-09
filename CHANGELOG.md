# Changelog

Releases are git tags, `vMAJOR.MINOR.PATCH`. The firmware reports its
version from `git describe` (ESP-IDF stamps it into the image): the boot
banner, `bootinfo` and About show `v0.9.0` on a tagged build and
`v0.9.0-3-gabc1234` between tags (`-dirty` if built with uncommitted
changes). `CAPP_API_VERSION` in `kernel/app/capp.h` is a separate number:
the app table, which every `.capp` must match.

To release: add a section here, commit, `git tag -a vX.Y.Z -m "..."`, then
`bash tools/deploy_droplet.sh sync`, which pushes the tags to GitHub and the
droplet so its builds carry the same version.

## v0.10.0 -- 2026-10-09

The cleanup: a multi-agent audit (seven reviewers, 78 findings, the eight
most severe re-checked against the code) and five workstreams fixing it.
API 43, unchanged: apps built for v0.9.0 still load.

**Flicker.** Full app repaints are composed off the panel a 16-row strip at a
time and sent whole (`draw_offscreen`, `kernel/ui/apphost.c`), so nothing
filled-then-drawn reaches the screen; apps that draw their own strips opt out
with `CAPP_PAINT_DIRECT`. The launcher, banner, picker, desktop and alarm use
the same strip; the busy badge waits 350 ms and repaints only its corner (it
repainted the whole shell on every request -- every ~1.2 s in Build). Tetris,
Pet, the toolbar and Quoridor's lobby stopped redrawing what had not changed.

**Memory.** About 32 KB more heap at boot (static RAM 172 KB -> 140.5 KB):
buffers held for the uptime now live only while used, the old memory
manager/swapper/scheduler and device-side Google sign-in are out of the
firmware, colour icons share 16 slots instead of a malloc each, the notify
poll no longer takes 8 KB every 30 s. A failed app load says how much it
needed and the largest block there was; the Memory app shows `mem`. The build
fails an app over its size budget; Notes, MIDI and Forklift were slimmed.

**Bugs fixed.**
- Stocks, Chat and Hub wrote past their reply buffers (`http_poll` now
  returns what it copied).
- Dashboard Link's uploads were cut at 511 bytes.
- Voice could take the mic pin while audio held it; G0 during music now stops
  the music first.
- WiFi could be torn down under a running request.
- Music and Photos could delete files from the card when the server's list
  did not fit their buffer.
- Web cut URLs at `&`; Explorer took Enter as yes on delete; Files opened
  pictures in Edit.
- Voice "open notes" closed the app it had just opened; a notification due
  under the lock screen was dropped.
- Server: a Google re-sign-in kept the old token (Todo 403s) for an hour;
  saving a Toggl token wiped the targets; one person's new Build
  conversation could stop another's.

**Structure.** One HTTP layer with fixed error codes, one owner for the audio
pins, one shell state and one key path, release hooks registered per module,
shared app headers (`str.h`, `confirm.h`, `syncset.h`, `dirmodel.h`,
`termlog.h`, `datetime.h`) and a shared test fake, a shared server store
(0600 files, a lock per file), route auth kinds, job expiry. Removed: dead
routes and commands, `google pull`.

Tests: host 19740 checks (was 19532), server 242 tests (was 196).

## v0.9.0 -- 2026-10-09

The first tagged release: everything up to here, as a baseline before the
cleanup. CardOS on the Cardputer and Cardputer ADV, API 43.

- Shells: the launcher (animated carousel, folders, search, favourites,
  hotkeys), the desktop, the console.
- Apps as native `.capp` files loaded from the card; built-ins Memory, About
  and Settings.
- The server: updates (debug and release firmware, apps), Build on the
  droplet (planned and stepped, `--effort xhigh`), fenced Build for people
  who are not the owner, accounts, the dashboard, Google, Toggl, notes,
  photos, music, MIDI, chat, rendering for Web, the Claude app's messages.
- Notifications (banner, centre, scheduled, polled), the lock screen (dim
  clock or black), voice on G0, the printer, audio, USB disk mode, ESP-NOW
  local multiplayer (Quoridor), Hub (M5Launcher's firmware catalog).
- New Update app: a list with a mark per item and real progress bars.
