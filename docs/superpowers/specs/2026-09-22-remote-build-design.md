# Remote build: the droplet builds

2026-09-22. Decided in conversation; this is the record.

## What it is for

From anywhere the device has a network, ask the Build app for a change
("make the flippers stronger") and install the result with `/update`, with
no PC switched on anywhere.

## What was there

`tools/webproxy.py` ran on the droplet (`cardos-proxy.service`, port 8081,
nginx forwarding plain HTTP from :8080). Its `/chat` ran Claude Code with
`Bash` allowed, against `/opt/cardos` -- a loose copy of the tree, not a git
repository, with no toolchain. So a Build request could edit files that
nothing would ever build, and `/update` served a firmware and apps copied up
by hand on 09-20.

## What changes

**The droplet holds a git clone.** `~cardos/cardos.git` is a bare repository;
`~cardos/cardos` is a clone with branch `remote` checked out, and the service
runs from it. The laptop has the bare repository as a remote named `droplet`
(ssh as `cardos`) and pushes to it and pulls `remote` from it. The droplet has
no credentials to anything else.

**The toolchain is installed there**: PlatformIO in the service's venv,
`espressif32 @ 7.1.2` -- pinned in `platformio.ini`, the version the laptop
builds with, rather than whatever is newest on the day the droplet installs it.
`tools/build_apps.py` no longer assumes `.exe`.

**A Build turn ends in a build** (`tools/buildstep.py`, `webproxy.py --build
--store DIR`). Around each turn the content of every changed file is hashed
before and after; the difference is what the turn did.

- Nothing changed: the answer, as before.
- Only docs, tools, tests or Markdown changed: committed, nothing built.
- Code changed: `build_apps.py`, and `platformio run` if anything outside
  `apps/` changed. Firmware is not rebuilt for an app-only change -- it embeds
  every app as a seed blob, so every app edit would otherwise look like an OS
  update too.
- Built: whatever differs from the store is published into it, each file
  written beside its destination and renamed over it, so the device never
  downloads half of one. Committed. The answer ends "published: pinball --
  /update installs".
- Did not build: committed with "(does not build)" in the subject, nothing
  published, and the answer carries the compiler's error lines.

`/update` serves the store, never the build tree, which is half-written while
a build runs.

**The droplet's agent has no Bash.** The server runs the build itself, so
Claude needs only Read, Edit, Write, Glob and Grep; `Bash`, `WebFetch` and
`WebSearch` are disallowed. Before this, the device token -- which crosses the
internet in plain HTTP -- was a shell on the host that holds arowe.net's TLS
keys. Now it is a way to propose code.

## What is still exposed, said plainly

Anyone holding the device token can have code built and offered to the device.
The token travels in the clear on :8080. The fix is HTTPS for the device's
Build and update traffic, which needs a kept-alive connection first: one TLS
handshake per 1.2 s poll is ~32 KB of heap on a device that Calendar has just
shown can run out. Until then, the device still asks before installing -- an
update is never applied without `update apps|os|all` or `/update`.

## Rejected

- **A relay with the laptop as the builder**, repo never on the public host.
  Built and tested first, then dropped: it needs the laptop awake.
- **Committing to master or pushing anywhere from the droplet.** Remote work
  lands on `remote` and waits to be pulled.

## Tests

`tools/test_buildstep.py`: change detection, what gets built for which paths,
publish only-what-differs and atomic, garbage refused, a turn that builds /
changes nothing / only changes docs / fails to build, no Bash in the tool
list, `/update` served from the store. End to end on hardware: a visible
change through Build, then `/update`.
