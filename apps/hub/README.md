# Hub

_Moved from CLAUDE.md on 2026-10-27._

**Hub: M5Launcher's firmware catalog** (2026-10-08, `apps/hub/hub.c`, System).
LauncherHub (api.launcherhub.net, ~700 Cardputer firmwares) through
`apps/hub/server/__init__.py`, which does the HTTPS and JSON and pages it 40 at a time
(the kernel's HTTP buffer is 8 KB). The files are whole-flash images; each
version gives its app's offset and size (`ao`, `as`), and the server fetches
only that range from m5burner-cdn, so the device gets a bootable app image
(490 KB of a 4 MB file) into /firmware/NAME.bin and, if asked, boots it
through `api->firmware_boot` (API 42) -- the launcher's chain-boot, reset
comes home. A version that also wants a SPIFFS/FAT partition is marked
"+data": its app boots without that data.
