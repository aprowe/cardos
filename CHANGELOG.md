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
